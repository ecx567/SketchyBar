#pragma once
/**
 * Named-pipe IPC seam for the Windows port (slice S4, tasks 4.1-4.5).
 *
 * Declares the helpers that src/mach.c implements so the named-pipe server
 * (the Windows stand-in for the macOS bootstrap/mach transport) and the S4
 * test suite share one ABI. Keeping the seam in `platform/` mirrors
 * sk_backend.h and win_platform.h: portable code talks to the seam, the
 * platform fills it in.
 */

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest accepted frame (the NUL-separated argv payload). Mirrors the macOS
 * OOL-message transport, which carries the same payload as one blob. */
#define IPC_MAX_FRAME 65536
/* Client-side connect/read deadlines, matching the macOS mach_msg 100ms
 * timeout (mach_receive_message's 100). */
#define IPC_TIMEOUT_MS 100
/* Room for "\\.\pipe\" (9) + "git.felix." (10) + longest bar name + NUL. */
#define IPC_PIPE_NAME_MAX 280

/* A valid frame is `argv[0]'\0'argv[1]'\0'...argv[n-1]'\0''\0'`:
 * non-empty tokens, single NUL separators, one trailing double NUL.
 * `len` includes the final NUL. */
bool ipc_frame_valid(const char* frame, size_t len);

bool ipc_sessions_match(DWORD session_a, DWORD session_b);

/* Builds the pipe DACL: SYSTEM + the current user + the caller's logon
 * session (S-1-5-5-X-Y). The pipe name is machine-global, so everyone else is
 * denied. Caller owns the returned descriptor (free() when done). */
PSECURITY_DESCRIPTOR ipc_create_security_descriptor(void);

/* Fail-closed session check (task 4.3): the connected client must run in the
 * same Windows session as the server, otherwise the bar rejects it. */
bool ipc_client_is_same_session(HANDLE pipe);

/* Writes "\\.\pipe\git.felix.<bs_name>" into `out` (NUL-terminated). */
void ipc_pipe_name(const char* bs_name, char* out, size_t out_len);