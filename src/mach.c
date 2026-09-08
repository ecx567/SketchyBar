#include "mach.h"

#ifdef _WIN32
#include "../platform/win_ipc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Temporary diagnostic trace (removed before commit): SBAR_IPC_TRACE=1 */
static void ipc_trace(const char* fmt, ...) {
  if (!getenv("SBAR_IPC_TRACE")) return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[ipc] ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

/*
 * Windows named-pipe IPC (slice S4, tasks 4.1-4.3).
 *
 * The macOS transport registers a bootstrapped mach port and services it from
 * the main run loop; Windows has no bootstrap service, so the server creates a
 * single-instance named pipe (\\.\pipe\git.felix.<name>, task 4.1) and
 * services it from a dedicated thread that runs the registered handler
 * synchronously (mirroring the macOS dispatch semantics). The handler contract
 * is identical to macOS: `buffer->message.descriptor.address` is the
 * NUL-separated frame and `buffer->message.header.msgh_remote_port` is the
 * reply channel (the pipe HANDLE) that handler responses are written back
 * through (message.c's mach_message_handler uses exactly this shape).
 *
 * Frame lifetime: the frame is freed after the handler returns and the pipe
 * handle is closed afterwards, which is safe for S4/S5 where handlers consume
 * the frame synchronously; the macOS event-queue handoff lands with the S5
 * event pump (carryover noted in apply-progress.md).
 */

extern char g_name[256];

/* --- frame validation (grammar: tok1\0tok2\0...tokN\0\0) --------------------- */
bool ipc_frame_valid(const char* frame, size_t len) {
  if (!frame || len == 0 || len > IPC_MAX_FRAME) return false;
  if (frame[len - 1] != '\0') return false;

  size_t tokens = 0;
  size_t i = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && frame[i] != '\0') i++;
    if (i == start && i != len - 1) return false;  /* empty interior token  */
    if (i > start) tokens++;
    i++;
  }
  return tokens >= 1;
}

bool ipc_sessions_match(DWORD session_a, DWORD session_b) {
  return session_a == session_b;
}

/* --- security (task 4.3) ----------------------------------------------------- */
static PSID create_well_known_sid(WELL_KNOWN_SID_TYPE type) {
  DWORD size = SECURITY_MAX_SID_SIZE;
  PSID sid = malloc(size);
  if (!sid) return NULL;
  if (!CreateWellKnownSid(type, NULL, sid, &size)) {
    free(sid);
    return NULL;
  }
  return sid;
}

static PSID copy_sid_from_token(TOKEN_INFORMATION_CLASS info_class) {
  HANDLE token = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;

  DWORD size = 0;
  GetTokenInformation(token, info_class, NULL, 0, &size);
  if (size == 0) {
    CloseHandle(token);
    return NULL;
  }

  uint8_t* info = malloc(size);
  PSID result = NULL;
  if (info && GetTokenInformation(token, info_class, info, size, &size)) {
    PSID sid;
    if (info_class == TokenUser) {
      sid = ((TOKEN_USER*)info)->User.Sid;
    } else {
      /* TokenLogonSid: the group carrying SE_GROUP_LOGON_ID (S-1-5-5-X-Y). */
      TOKEN_GROUPS* groups = (TOKEN_GROUPS*)info;
      sid = NULL;
      for (DWORD i = 0; i < groups->GroupCount; i++) {
        if (groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) {
          sid = groups->Groups[i].Sid;
          break;
        }
      }
    }
    if (sid) {
      DWORD len = GetLengthSid(sid);
      result = malloc(len);
      if (result) memcpy(result, sid, len);
    }
  }
  free(info);
  CloseHandle(token);
  return result;
}

PSECURITY_DESCRIPTOR ipc_create_security_descriptor(void) {
  /* One allocation for descriptor + DACL so the caller frees once. */
  PSID principals[3] = {
    create_well_known_sid(WinLocalSystemSid),  /* SYSTEM       */
    copy_sid_from_token(TokenUser),            /* current user */
    copy_sid_from_token(TokenLogonSid)         /* logon session */
  };

  size_t count = 0;
  size_t sid_bytes = 0;
  for (size_t i = 0; i < 3; i++) {
    if (principals[i]) {
      count++;
      sid_bytes += GetLengthSid(principals[i]);
    }
  }

  BOOL ok = FALSE;
  PSECURITY_DESCRIPTOR sd = NULL;
  if (count > 0) {
    DWORD dacl_size = (DWORD)(sizeof(ACL)
        + count * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) + sid_bytes);
    uint8_t* block = malloc(SECURITY_DESCRIPTOR_MIN_LENGTH + dacl_size);
    if (block) {
      sd = (PSECURITY_DESCRIPTOR)block;
      PACL dacl = (PACL)(block + SECURITY_DESCRIPTOR_MIN_LENGTH);
      if (InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION)
          && InitializeAcl(dacl, dacl_size, ACL_REVISION)) {
        ok = TRUE;
        for (size_t i = 0; i < 3; i++) {
          if (!principals[i]) continue;
          if (!AddAccessAllowedAce(dacl, ACL_REVISION,
                                   GENERIC_READ | GENERIC_WRITE, principals[i])) {
            ok = FALSE;
            break;
          }
        }
        if (ok) ok = SetSecurityDescriptorDacl(sd, TRUE, dacl, FALSE);
      }
      if (!ok) {
        free(block);
        sd = NULL;
      }
    }
  }

  for (size_t i = 0; i < 3; i++) free(principals[i]);
  return sd;  /* NULL = fail closed: no descriptor, no server */
}

bool ipc_client_is_same_session(HANDLE pipe) {
  DWORD client_pid = 0;
  if (!GetNamedPipeClientProcessId(pipe, &client_pid) || client_pid == 0)
    return false;

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, client_pid);
  if (!process) return false;

  HANDLE token = NULL;
  DWORD client_session = 0;
  if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
    DWORD size = sizeof(DWORD);
    DWORD session = 0;
    if (GetTokenInformation(token, TokenSessionId, &session, size, &size))
      client_session = session;
    CloseHandle(token);
  }
  CloseHandle(process);
  if (client_session == 0) return false;  /* fail closed */

  DWORD server_session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &server_session);
  return ipc_sessions_match(client_session, server_session);
}

void ipc_pipe_name(const char* bs_name, char* out, size_t out_len) {
  if (!out || out_len == 0) return;
  snprintf(out, out_len, "\\\\.\\pipe\\" MACH_BS_NAME_FMT, bs_name);
}

/* --- client transport --------------------------------------------------------- */
static HANDLE ipc_open_client_pipe(const char* name) {
  for (int attempt = 0; attempt < 3; attempt++) {
    HANDLE pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;

    DWORD error = GetLastError();
    ipc_trace("open attempt %d: err=%lu\n", attempt, error);
    if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return NULL;
    /* The server thread may not have created its instance yet (startup race)
     * or is servicing the previous client; wait up to the IPC timeout. */
    BOOL waited = WaitNamedPipeA(name, IPC_TIMEOUT_MS);
    ipc_trace("  WaitNamedPipeA: %d err=%lu\n", waited, GetLastError());
    if (!waited) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND && attempt < 2) continue;
      return NULL;
    }
  }
  return NULL;
}

static char* ipc_empty_response(void) {
  char* empty = malloc(1);
  if (empty) *empty = '\0';
  return empty;
}

static char* ipc_read_response(HANDLE pipe, DWORD timeout_ms) {
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  DWORD capacity = 64;
  DWORD total = 0;
  char* response = malloc(capacity);
  if (!response) return NULL;

  for (;;) {
    DWORD available = 0;
    if (PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) && available > 0) {
      if (total + available + 1 > capacity) {
        DWORD grown = capacity;
        while (total + available + 1 > grown) grown *= 2;
        if (grown > 1024 * 1024) break;  /* protocol abuse: response too large */
        char* bigger = realloc(response, grown);
        if (!bigger) break;
        response = bigger;
        capacity = grown;
      }
      DWORD read = 0;
      if (!ReadFile(pipe, response + total, available, &read, NULL)) break;
      total += read;
      if (read > 0 && response[total - 1] == '\0')
        return response;  /* complete: the server NUL-terminates responses */
      continue;
    }
    if (GetTickCount64() >= deadline) break;
    Sleep(10);
  }

  free(response);
  return ipc_empty_response();  /* timeout/error: macOS parity (no reply) */
}

/* --- server transport --------------------------------------------------------- */
static uint8_t* ipc_read_frame(HANDLE pipe) {
  ULONGLONG deadline = GetTickCount64() + IPC_TIMEOUT_MS;
  DWORD available = 0;
  for (;;) {
    if (PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) && available > 0)
      break;
    if (GetTickCount64() >= deadline) return NULL;
    Sleep(10);
  }
  if (available > IPC_MAX_FRAME) return NULL;  /* oversized at the head of line */

  uint8_t* frame = malloc(IPC_MAX_FRAME + 1);
  if (!frame) return NULL;
  DWORD got = 0;
  if (!ReadFile(pipe, frame, IPC_MAX_FRAME + 1, &got, NULL)) {
    free(frame);
    return NULL;  /* oversized (ERROR_MORE_DATA) or read error: rejected */
  }
  if (got == 0 || got > IPC_MAX_FRAME || !ipc_frame_valid((const char*)frame, got)) {
    free(frame);
    return NULL;
  }
  return frame;
}

static DWORD WINAPI ipc_server_thread(LPVOID context) {
  struct mach_server* server = context;

  for (;;) {
    HANDLE pipe = CreateNamedPipeA(server->pipe_name,
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE
                                     | PIPE_REJECT_REMOTE_CLIENTS,
                                   1,
                                   IPC_MAX_FRAME,
                                   IPC_MAX_FRAME,
                                   0,
                                   &server->sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      ipc_trace("server: CreateNamedPipeA failed err=%lu\n", GetLastError());
      break;  /* terminal: cannot create instance */
    }
    ipc_trace("server: instance created\n");

    BOOL connected = ConnectNamedPipe(pipe, NULL);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
      ipc_trace("server: connect failed err=%lu\n", GetLastError());
      Sleep(50);
      CloseHandle(pipe);
      continue;
    }
    ipc_trace("server: client connected\n");

    if (!ipc_client_is_same_session(pipe)) {
      ipc_trace("server: REJECTED cross-session client\n");
      CloseHandle(pipe);  /* task 4.3: cross-session client rejected */
      continue;
    }

    uint8_t* frame = ipc_read_frame(pipe);
    ipc_trace("server: read_frame -> %s\n", frame ? "ok" : "NULL");
    if (frame) {
      struct mach_buffer buffer = { 0 };
      buffer.message.header.msgh_size = sizeof(struct mach_message);
      buffer.message.header.msgh_remote_port = (mach_port_t)pipe;  /* reply channel */
      buffer.message.descriptor.address = frame;
      server->handler(&buffer);
      ipc_trace("server: handler returned\n");
      free(frame);
    }
    CloseHandle(pipe);  /* single instance: recreated on the next iteration */
  }
  return 0;
}

bool mach_server_begin(struct mach_server* mach_server, mach_handler handler) {
  ipc_pipe_name(g_name, mach_server->pipe_name, sizeof(mach_server->pipe_name));

  mach_server->sd = ipc_create_security_descriptor();
  if (!mach_server->sd) return false;

  mach_server->sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  mach_server->sa.bInheritHandle = FALSE;
  mach_server->sa.lpSecurityDescriptor = mach_server->sd;

  mach_server->handler = handler;
  mach_server->is_running = true;

  mach_server->thread = CreateThread(NULL, 0, ipc_server_thread,
                                     (LPVOID)mach_server, 0, NULL);
  if (!mach_server->thread) {
    mach_server->is_running = false;
    return false;
  }
  return true;
}

char* mach_send_message(mach_port_t port, char* message, uint32_t len,
                        bool await_response) {
  if (!message || !port) return NULL;

  if (port == INVALID_HANDLE_VALUE) {
    /* Client path: mach_get_bs_port returns INVALID_HANDLE_VALUE on Windows,
     * so this branch opens the pipe by name (parity: macOS looks the port up
     * in the bootstrap namespace, which Windows does not have). */
    char name[IPC_PIPE_NAME_MAX];
    ipc_pipe_name(g_name, name, sizeof(name));
    if (len > IPC_MAX_FRAME) return NULL;  /* task 4.3 client-side cap */

    HANDLE pipe = ipc_open_client_pipe(name);
    if (!pipe) {
      ipc_trace("client: open FAILED for %s\n", name);
      return NULL;
    }
    ipc_trace("client: opened %s\n", name);

    DWORD written = 0;
    BOOL sent = WriteFile(pipe, message, len, &written, NULL);
    if (sent) sent = FlushFileBuffers(pipe);
    ipc_trace("client: write sent=%d written=%lu err=%lu\n", sent, written,
              GetLastError());
    if (!sent || !await_response) {
      CloseHandle(pipe);
      return NULL;
    }

    char* response = ipc_read_response(pipe, IPC_TIMEOUT_MS);
    CloseHandle(pipe);
    return response;
  }

  /* Server side: a live pipe HANDLE is the reply channel. Responses are never
   * awaited (matches message.c's mach_send_message(..., false) writeback). */
  (void)await_response;
  DWORD written = 0;
  if (!WriteFile(port, message, len, &written, NULL)) return NULL;
  FlushFileBuffers(port);
  return NULL;
}

mach_port_t mach_get_bs_port(char* bs_name) {
  (void)bs_name;
  return INVALID_HANDLE_VALUE;  /* sentinel: client path in mach_send_message */
}

#else
#include <mach/mach_port.h>
#include <mach/message.h>
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>

mach_port_t mach_get_bs_port(char* bs_name) {
  mach_port_name_t task = mach_task_self();

  mach_port_t bs_port;
  if (task_get_special_port(task,
                            TASK_BOOTSTRAP_PORT,
                            &bs_port            ) != KERN_SUCCESS) {
    return 0;
  }

  mach_port_t port;
  if (bootstrap_look_up(bs_port,
                        bs_name,
                        &port   ) != KERN_SUCCESS) {
    return 0;
  }

  return port;
}

void mach_receive_message(mach_port_t port, struct mach_buffer* buffer, bool timeout) {
  *buffer = (struct mach_buffer) { 0 };
  mach_msg_return_t msg_return;
  if (timeout)
    msg_return = mach_msg(&buffer->message.header,
                          MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                          0,
                          sizeof(struct mach_buffer),
                          port,
                          100,
                          MACH_PORT_NULL                  );
  else 
    msg_return = mach_msg(&buffer->message.header,
                          MACH_RCV_MSG,
                          0,
                          sizeof(struct mach_buffer),
                          port,
                          MACH_MSG_TIMEOUT_NONE,
                          MACH_PORT_NULL            );

  if (msg_return != MACH_MSG_SUCCESS) {
    buffer->message.descriptor.address = NULL;
  }
}

char* mach_send_message(mach_port_t port, char* message, uint32_t len, bool await_response) {
  if (!message || !port) return NULL;

  mach_port_t response_port;
    mach_port_name_t task = mach_task_self();
  if (await_response) {
    if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE,
                                 &response_port          ) != KERN_SUCCESS) {
      return NULL;
    }

    if (mach_port_insert_right(task, response_port,
                                     response_port,
                                     MACH_MSG_TYPE_MAKE_SEND)!= KERN_SUCCESS) {
      return NULL;
    }
  }

  struct mach_message msg = { 0 };
  msg.header.msgh_remote_port = port;
  if (await_response) {
    msg.header.msgh_local_port = response_port;
    msg.header.msgh_id = response_port;
    msg.header.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND,
                                              MACH_MSG_TYPE_MAKE_SEND,
                                              0,
                                              MACH_MSGH_BITS_COMPLEX  );
  } else {
    msg.header.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND
                                              & MACH_MSGH_BITS_REMOTE_MASK,
                                              0,
                                              0,
                                              MACH_MSGH_BITS_COMPLEX       );
  }

  msg.header.msgh_size = sizeof(struct mach_message);

  msg.msgh_descriptor_count = 1;
  msg.descriptor.address = message;
  msg.descriptor.size = len * sizeof(char);
  msg.descriptor.copy = MACH_MSG_VIRTUAL_COPY;
  msg.descriptor.deallocate = false;
  msg.descriptor.type = MACH_MSG_OOL_DESCRIPTOR;

  mach_msg(&msg.header,
           MACH_SEND_MSG,
           sizeof(struct mach_message),
           0,
           MACH_PORT_NULL,
           MACH_MSG_TIMEOUT_NONE,
           MACH_PORT_NULL             );

  if (await_response) {
    struct mach_buffer buffer = { 0 };
    mach_receive_message(response_port, &buffer, true);
    char* rsp = NULL;
    if (buffer.message.descriptor.address) {
      rsp = malloc(strlen(buffer.message.descriptor.address) + 1);
      memcpy(rsp, buffer.message.descriptor.address,
                  strlen(buffer.message.descriptor.address) + 1);
    } else {
      rsp = malloc(1);
      *rsp = '\0';
    }

    mach_msg_destroy(&buffer.message.header);
    mach_port_mod_refs(task, response_port, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(task, response_port);

    return rsp;
  }

  return NULL;
}

void mach_message_callback(CFMachPortRef port, void* message, CFIndex size, void* context) {
  struct mach_server* mach_server = context;
  struct mach_buffer buffer;
  buffer.message = *(struct mach_message*)message;
  mach_server->handler(&buffer);
  mach_msg_destroy(&buffer.message.header);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
extern char g_name[256];
bool mach_server_begin(struct mach_server* mach_server, mach_handler handler) {
  mach_server->task = mach_task_self();

  if (mach_port_allocate(mach_server->task,
                         MACH_PORT_RIGHT_RECEIVE,
                         &mach_server->port      ) != KERN_SUCCESS) {
    return false;
  }

  struct mach_port_limits limits = {};
  limits.mpl_qlimit = MACH_PORT_QLIMIT_LARGE;

  if (mach_port_set_attributes(mach_server->task,
                               mach_server->port,
                               MACH_PORT_LIMITS_INFO,
                               (mach_port_info_t)&limits,
                               MACH_PORT_LIMITS_INFO_COUNT) != KERN_SUCCESS) {
    return false;
  }

  if (mach_port_insert_right(mach_server->task,
                             mach_server->port,
                             mach_server->port,
                             MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
    return false;
  }

  if (task_get_special_port(mach_server->task,
                            TASK_BOOTSTRAP_PORT,
                            &mach_server->bs_port) != KERN_SUCCESS) {
    return false;
  }

  char bs_name[256];
  snprintf(bs_name, 256, MACH_BS_NAME_FMT, g_name);

  if (bootstrap_register(mach_server->bs_port,
                         bs_name,
                         mach_server->port    ) != KERN_SUCCESS) {
    return false;
  }

  mach_server->handler = handler;
  mach_server->is_running = true;

  CFMachPortContext context = {0, (void*)mach_server};

  CFMachPortRef cf_mach_port = CFMachPortCreateWithPort(NULL,
                                                        mach_server->port,
                                                        mach_message_callback,
                                                        &context,
                                                        false                );

  CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL,
                                                            cf_mach_port,
                                                            0            );

  CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
  CFRelease(source);
  CFRelease(cf_mach_port);
  return true;
}
#pragma clang diagnostic pop
#endif
