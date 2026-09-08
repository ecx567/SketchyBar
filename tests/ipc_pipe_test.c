/**
 * Named-pipe IPC test suite (slice S4, task 4.5).
 *
 * Exercises the Windows mach.c transport end to end through real pipes with a
 * handler registered via mach_server_begin:
 *   1. frame-validation units (ipc_frame_valid grammar)
 *   2. client -> server -> handler -> response round trip
 *   3. timeout: a slow handler still yields an empty response within ~100ms
 *   4. oversized frames are rejected and later requests still get served
 *   5. malformed NUL separators are rejected on the wire
 *   6. session guard: ipc_sessions_match, DACL contents and the live
 *      same-session check
 *
 * Cross-session rejection cannot be fabricated from one process (it needs a
 * second Windows session), so that branch is pinned by the decision logic and
 * the DACL introspection instead; the residual gap is documented in
 * apply-progress.md.
 *
 * Built against real cmocka when available (CI) or tests/cmocka_shim.h
 * (offline machines) via the SKBAR_CMOCKA_SHIM define.
 */

#include "mach.h"
#include "../platform/win_ipc.h"

#ifdef SKBAR_CMOCKA_SHIM
#include "cmocka_shim.h"
#else
#include <setjmp.h>   /* cmocka 1.1.7 relies on it being included first */
#include <cmocka.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sddl.h>   /* ConvertSidToStringSidA (DACL introspection) */

/* Defined by the app (sketchybar.c) in production; here the suite owns it so
 * every test gets a unique pipe name and never collides with a running bar. */
char g_name[256];

static void unique_bar_name(void) {
  static int seq = 0;
  snprintf(g_name, sizeof(g_name), "ipc_test_%d", seq++);
}

/* --- handlers ---------------------------------------------------------------- */
static void echo_handler(struct mach_buffer* message) {
  char* request = (char*)message->message.descriptor.address;
  char response[256];
  snprintf(response, sizeof(response), "ok:%s", request);
  mach_send_message(message->message.header.msgh_remote_port,
                    response, (uint32_t)strlen(response) + 1, false);
}

static void slow_handler(struct mach_buffer* message) {
  Sleep(300);  /* longer than the client's 100ms deadline */
  char* request = (char*)message->message.descriptor.address;
  char response[64];
  snprintf(response, sizeof(response), "late:%s", request);
  mach_send_message(message->message.header.msgh_remote_port,
                    response, (uint32_t)strlen(response) + 1, false);
}

static bool start_server(struct mach_server* server, mach_handler handler) {
  unique_bar_name();
  return mach_server_begin(server, handler);
}

static void stop_server(struct mach_server* server) {
  mach_server_stop(server);
}

static void* wait_pipe(const char* name) {
  for (int i = 0; i < 30; i++) {
    HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) return h;
    Sleep(10);
  }
  return NULL;
}

/* Client call that mirrors sketchybar.c's client_send_message framing. */
static char* send_frame(const char* token1, const char* token2) {
  size_t len1 = strlen(token1);
  size_t len2 = strlen(token2);
  uint32_t frame_len = (uint32_t)(len1 + 1 + len2 + 2);  /* tok1\0tok2\0\0 */
  char* frame = malloc(frame_len);
  if (!frame) return NULL;
  memcpy(frame, token1, len1);
  frame[len1] = '\0';
  memcpy(frame + len1 + 1, token2, len2);
  frame[len1 + 1 + len2] = '\0';
  frame[frame_len - 1] = '\0';

  char bs_name[256];
  snprintf(bs_name, sizeof(bs_name), MACH_BS_NAME_FMT, g_name);
  char* rsp = mach_send_message(mach_get_bs_port(bs_name), frame, frame_len, true);
  free(frame);
  return rsp;
}

/* --- 1. frame validation ------------------------------------------------------ */
static void test_frame_validation(void** state) {
  (void)state;
  assert_true(ipc_frame_valid("a\0\0", 3));
  assert_true(ipc_frame_valid("a\0b\0\0", 5));
  assert_false(ipc_frame_valid("a\0\0b\0", 5));  /* empty interior token */
  assert_false(ipc_frame_valid("abc", 3));       /* no trailing NUL */
  assert_false(ipc_frame_valid("", 0));          /* empty */
  assert_false(ipc_frame_valid("\0\0", 2));      /* zero tokens */
  assert_false(ipc_frame_valid(NULL, 3));        /* null frame */

  char* huge = malloc(IPC_MAX_FRAME + 1);
  assert_non_null(huge);
  memset(huge, 'a', IPC_MAX_FRAME + 1);
  huge[IPC_MAX_FRAME] = '\0';
  assert_false(ipc_frame_valid(huge, IPC_MAX_FRAME + 1));  /* over budget */
  free(huge);
}

/* --- 2. round trip ------------------------------------------------------------ */
static void test_round_trip(void** state) {
  (void)state;
  struct mach_server server = { 0 };
  assert_true(start_server(&server, echo_handler));
  char* rsp = send_frame("--set", "test");
  assert_non_null(rsp);
  assert_string_equal(rsp, "ok:--set");
  free(rsp);
  stop_server(&server);
}

/* --- 3. timeout ---------------------------------------------------------------- */
static void test_timeout(void** state) {
  (void)state;
  struct mach_server server = { 0 };
  assert_true(start_server(&server, slow_handler));
  char* rsp = send_frame("--get", "time");
  assert_non_null(rsp);
  assert_int_equal(strlen(rsp), 0);  /* empty response, no deadlock */
  free(rsp);
  stop_server(&server);
}

/* --- 4. oversized frames ------------------------------------------------------- */
static void test_oversized_rejected(void** state) {
  (void)state;
  struct mach_server server = { 0 };
  assert_true(start_server(&server, echo_handler));

  char name[IPC_PIPE_NAME_MAX];
  ipc_pipe_name(g_name, name, sizeof(name));
  HANDLE h = wait_pipe(name);
  assert_non_null(h);

  char* big = malloc(100 * 1024);
  assert_non_null(big);
  memset(big, 'x', 100 * 1024);
  DWORD written = 0;
  WriteFile(h, big, 100 * 1024, &written, NULL);  /* may fail: rejected */
  free(big);
  CloseHandle(h);

  /* Canary: the server must still serve a valid request afterwards. */
  char* rsp = send_frame("--get", "after-oversized");
  assert_non_null(rsp);
  assert_string_equal(rsp, "ok:--get");
  free(rsp);
  stop_server(&server);
}

/* --- 5. malformed NUL separators on the wire ----------------------------------- */
static void test_malformed_wire_rejected(void** state) {
  (void)state;
  struct mach_server server = { 0 };
  assert_true(start_server(&server, echo_handler));

  char name[IPC_PIPE_NAME_MAX];
  ipc_pipe_name(g_name, name, sizeof(name));
  HANDLE h = wait_pipe(name);
  assert_non_null(h);

  const char* bad = "a\0\0b\0";  /* empty interior token, len 5 */
  DWORD written = 0;
  WriteFile(h, bad, 5, &written, NULL);
  CloseHandle(h);

  char* rsp = send_frame("--get", "after-malformed");
  assert_non_null(rsp);
  assert_string_equal(rsp, "ok:--get");
  free(rsp);
  stop_server(&server);
}

/* --- 6. session guard ----------------------------------------------------------- */
static void test_session_guard(void** state) {
  (void)state;
  assert_true(ipc_sessions_match(7, 7));
  assert_false(ipc_sessions_match(7, 8));

  /* DACL must carry SYSTEM (S-1-5-18) and the logon session (S-1-5-5-X-Y). */
  PSECURITY_DESCRIPTOR sd = ipc_create_security_descriptor();
  assert_non_null(sd);
  BOOL present = FALSE, dacl_defaulted = FALSE;
  PACL dacl = NULL;
  assert_true(GetSecurityDescriptorDacl(sd, &present, &dacl, &dacl_defaulted));
  assert_true(present);
  assert_non_null(dacl);

  bool has_system = false;
  bool has_logon = false;
  ACL_SIZE_INFORMATION acl_info = { 0 };
  GetAclInformation(dacl, &acl_info, sizeof(acl_info), AclSizeInformation);
  for (DWORD i = 0; i < acl_info.AceCount; i++) {
    LPVOID ace = NULL;
    GetAce(dacl, i, &ace);
    ACCESS_ALLOWED_ACE* allowed = (ACCESS_ALLOWED_ACE*)ace;
    PSID sid = (PSID)&allowed->SidStart;
    char* sid_str = NULL;
    if (ConvertSidToStringSidA(sid, &sid_str) && sid_str) {
      if (strstr(sid_str, "S-1-5-18")) has_system = true;
      if (strstr(sid_str, "S-1-5-5-")) has_logon = true;
      LocalFree(sid_str);
    }
  }
  assert_true(has_system);
  assert_true(has_logon);
  free(sd);

  /* A live client of the test server is same-session by construction. */
  struct mach_server server = { 0 };
  assert_true(start_server(&server, echo_handler));
  char name[IPC_PIPE_NAME_MAX];
  ipc_pipe_name(g_name, name, sizeof(name));
  HANDLE h = wait_pipe(name);
  assert_non_null(h);
  assert_true(ipc_client_is_same_session(h));
  CloseHandle(h);

  char* rsp = send_frame("--get", "session");
  assert_non_null(rsp);
  assert_string_equal(rsp, "ok:--get");
  free(rsp);
  stop_server(&server);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_frame_validation),
    cmocka_unit_test(test_round_trip),
    cmocka_unit_test(test_timeout),
    cmocka_unit_test(test_oversized_rejected),
    cmocka_unit_test(test_malformed_wire_rejected),
    cmocka_unit_test(test_session_guard),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}