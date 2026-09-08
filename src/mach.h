#pragma once
#ifdef _WIN32
#include "../platform/win_ipc.h"
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define MACH_BS_NAME_FMT "git.felix.%s"

/* Windows type surface (slice S4). macOS names its transport types from
 * mach/mach.h; Windows mirrors the small subset this codebase touches so the
 * portable core (message.c and friends, S5+) compiles unchanged. mach_port_t
 * is a pipe HANDLE on Windows: NULL means "no port", INVALID_HANDLE_VALUE is
 * the sentinel mach_get_bs_port returns for "client, open by name". */
typedef HANDLE mach_port_t;
typedef mach_port_t mach_port_name_t;
typedef uint32_t mach_msg_size_t;
typedef uint32_t mach_msg_bits_t;

struct mach_msg_header_t {
  mach_msg_bits_t msgh_bits;
  mach_msg_size_t msgh_size;
  mach_port_t msgh_remote_port;
  mach_port_t msgh_local_port;
};

struct mach_msg_ool_descriptor_t {
  void* address;
  mach_msg_size_t size;
  bool deallocate;
  uint8_t type;
};

struct mach_message {
  struct mach_msg_header_t header;
  mach_msg_size_t msgh_descriptor_count;
  struct mach_msg_ool_descriptor_t descriptor;
};

struct mach_buffer {
  struct mach_message message;
  uint8_t trailer[64];
};

#define MACH_HANDLER(name) void name(struct mach_buffer* message)
typedef MACH_HANDLER(mach_handler);

struct mach_server {
  bool is_running;
  mach_port_name_t task;
  mach_port_t port;
  mach_port_t bs_port;

  mach_handler* handler;

  /* Windows-only state (slice S4): the named-pipe instance, its security
   * descriptor and the servicing thread. */
  HANDLE pipe;
  HANDLE thread;
  char pipe_name[IPC_PIPE_NAME_MAX];
  SECURITY_ATTRIBUTES sa;
  PSECURITY_DESCRIPTOR sd;
};
#else
#include <bootstrap.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

#define MACH_BS_NAME_FMT "git.felix.%s"

struct mach_message {
  mach_msg_header_t header;
  mach_msg_size_t msgh_descriptor_count;
  mach_msg_ool_descriptor_t descriptor;
};

struct mach_buffer {
  struct mach_message message;
  mach_msg_trailer_t trailer;
};

#define MACH_HANDLER(name) void name(struct mach_buffer* message)
typedef MACH_HANDLER(mach_handler);

struct mach_server {
  bool is_running;
  mach_port_name_t task;
  mach_port_t port;
  mach_port_t bs_port;

  mach_handler* handler;
};
#endif

bool mach_server_begin(struct mach_server* mach_server, mach_handler handler);
char* mach_send_message(mach_port_t port, char* message, uint32_t len, bool await_response);
mach_port_t mach_get_bs_port(char* bs_name);

#ifdef _WIN32
/* Windows-only teardown (slice S4): stops the named-pipe servicing thread
 * begun by mach_server_begin and frees its security descriptor. Used by the
 * ipc_pipe test harness so each test owns an isolated server and leaves no
 * zombie thread racing the next test. A no-op for a server that was never
 * started or already stopped. */
void mach_server_stop(struct mach_server* server);
#endif
