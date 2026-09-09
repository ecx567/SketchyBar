#include "bar_manager.h"
#include "event.h"
#ifndef _WIN32
#include <ApplicationServices/ApplicationServices.h>
#include <libgen.h>
#else
#include <windows.h>
#include <direct.h>   /* _chdir */
#include "win_platform.h"
#endif

extern char g_config_file[4096];
extern char g_name[256];
bool g_hotload = false;
int64_t g_last_hotload = 0;

#ifdef _WIN32
// Windows (S5): GetTickCount64-based debounce (the macOS handler uses a
// 1ULL<<30 ns window == ~1.07 s; 500 ms is the equivalent "don't hotload-loop"
// guard). Watcher thread + stop event owned by this TU.
#define HOTLOAD_DEBOUNCE_MS 500
static uint64_t g_last_hotload_ms = 0;
static HANDLE g_watch_thread = NULL;
static HANDLE g_watch_stop_event = NULL;
#endif

void hotload_set_state(int state) {
  g_hotload = state;
}

int hotload_get_state() {
  return g_hotload;
}

bool set_config_file_path(char* file) {
#ifdef _WIN32
  // Windows (S5): realpath() is POSIX-only; _fullpath resolves the same
  // absolute, canonical path and returns NULL on failure (nonexistent input),
  // matching realpath's contract.
  char* path = _fullpath(NULL, file, 0);
  if (path) {
    snprintf(g_config_file, sizeof(g_config_file), "%s", path);
    free(path);
    return true;
  }
  return false;
#else
  char* path = realpath(file, NULL);
  if (path) {
    snprintf(g_config_file, sizeof(g_config_file), "%s", path);
    free(path);
    return true;
  }
  return false;
#endif
}

static bool get_config_file(char *restrict filename, char *restrict buffer, int buffer_size) {
  char *xdg_home = getenv("XDG_CONFIG_HOME");
  if (xdg_home && *xdg_home) {
    snprintf(buffer, buffer_size, "%s/%s/%s", xdg_home, g_name, filename);
    if (file_exists(buffer)) return true;
  }

  char *home = getenv("HOME");
  if (!home) return false;

  snprintf(buffer, buffer_size, "%s/.config/%s/%s", home, g_name, filename);
  if (file_exists(buffer)) return true;

  snprintf(buffer, buffer_size, "%s/.%s", home, filename);
  return file_exists(buffer);
}

#ifdef _WIN32
// Windows dirname(): strips trailing separators, cuts at the last one and
// never returns NULL (drive roots / bare names collapse to "."). Mutates its
// input like the POSIX dirname it replaces.
static const char* win_dirname(char* file) {
  size_t len = strlen(file);
  while (len > 0 && (file[len - 1] == '/' || file[len - 1] == '\\'))
    file[--len] = '\0';
  if (len == 0) return ".";

  char* sep = NULL;
  for (size_t i = 0; i < len; i++) {
    if (file[i] == '/' || file[i] == '\\') sep = &file[i];
  }
  if (!sep) return ".";
  if (sep == file) return "\\";
  *sep = '\0';
  return file;
}
#endif

void exec_config_file() {
  if (!*g_config_file
    && !get_config_file("sketchybarrc", g_config_file, sizeof(g_config_file))) {
    printf("could not locate config file..\n");
    return;
  }

  if (!file_exists(g_config_file)) {
    printf("file '%s' does not exist..\n", g_config_file);
    return;
  }

#ifdef _WIN32
  // Windows (S5): setenv/chdir are POSIX; win_setenv (win_compat.h) and the
  // CRT's _chdir are the equivalents. ensure_executable_permission is a no-op
  // here (permission bits are meaningless; CreateProcess is the enforcement
  // boundary). fork_exec is the S1 shim - it fails gracefully when it cannot
  // mount the config as a shell command; the real CreateProcess-based config
  // runner is task 7.6.
  //
  // Deliberate deviation: dirname() on macOS mutates g_config_file in place
  // (and macOS calls it twice, walking up two levels); the Windows branch
  // resolves the directory ONCE into a local so g_config_file stays the file
  // path and repeated hotloads keep targeting the same file.
  char config_dir[4096];
  snprintf(config_dir, sizeof(config_dir), "%s", g_config_file);
  const char* dir = win_dirname(config_dir);
  win_setenv("CONFIG_DIR", dir);
  _chdir(dir);

  if (!fork_exec(g_config_file, NULL)) {
    printf("failed to execute file '%s'\n", g_config_file);
    return;
  }
#else
  setenv("CONFIG_DIR", dirname(g_config_file), 1);
  chdir(dirname(g_config_file));

  if (!ensure_executable_permission(g_config_file)) {
    printf("could not set the executable permission bit for '%s'\n", g_config_file);
    return;
  }

  if (!fork_exec(g_config_file, NULL)) {
    printf("failed to execute file '%s'\n", g_config_file);
    return;
  }
#endif
}

#ifndef _WIN32
static void handler(ConstFSEventStreamRef stream, void* context, size_t count, void* paths, const FSEventStreamEventFlags* flags, const FSEventStreamEventId* ids) {
  if (g_hotload && count > 0) {
    // Limit the hotload rate to avoid locking up the system on a hotload loop
    int64_t time = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX);
    if (time - g_last_hotload > (1ULL << 30)) {
      g_last_hotload = time;
      struct event event = { NULL, HOTLOAD };
      event_post(&event);
    }
  }
}
#endif

#ifdef _WIN32
// Windows (S5): ReadDirectoryChangesW watcher - a stop event plus the
// overlapped notification handle make WaitForMultipleObjects the wait (no
// polling). One notification burst per ChangeNotification, gated by the 500 ms
// debounce; events are marshalled to the pump thread by event_post (off-main
// thread -> skbar_win_post_event).
static DWORD WINAPI config_watch_thread(LPVOID param) {
  (void)param;

  char watch_dir[4096];
  snprintf(watch_dir, sizeof(watch_dir), "%s", g_config_file);
  const char* dir = win_dirname(watch_dir);

  HANDLE dir_handle = CreateFileA(dir, FILE_LIST_DIRECTORY,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE
                                    | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS
                                    | FILE_FLAG_OVERLAPPED,
                                  NULL);
  if (dir_handle == INVALID_HANDLE_VALUE) {
    printf("could not open config directory '%s' for watching (err %lu)\n",
           dir, GetLastError());
    return 1;
  }

  char buffer[64 * 1024];
  OVERLAPPED overlapped = {0};
  overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!overlapped.hEvent) {
    CloseHandle(dir_handle);
    return 1;
  }

  for (;;) {
    DWORD bytes = 0;
    if (!ReadDirectoryChangesW(dir_handle, buffer, sizeof(buffer), FALSE,
                               FILE_NOTIFY_CHANGE_FILE_NAME
                                 | FILE_NOTIFY_CHANGE_DIR_NAME
                                 | FILE_NOTIFY_CHANGE_LAST_WRITE
                                 | FILE_NOTIFY_CHANGE_SIZE
                                 | FILE_NOTIFY_CHANGE_ATTRIBUTES,
                               &bytes, &overlapped, NULL)) {
      printf("ReadDirectoryChangesW failed (err %lu)\n", GetLastError());
      break;
    }

    HANDLE waits[2] = { overlapped.hEvent, g_watch_stop_event };
    DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 + 1) break;   /* stop requested */
    if (result != WAIT_OBJECT_0) break;       /* wait error */
    ResetEvent(overlapped.hEvent);

    if (g_hotload) {
      uint64_t now = GetTickCount64();
      if (now - g_last_hotload_ms > HOTLOAD_DEBOUNCE_MS) {
        g_last_hotload_ms = now;
        struct event event = { NULL, HOTLOAD };
        event_post(&event);
      }
    }
  }

  CancelIo(dir_handle);
  CloseHandle(overlapped.hEvent);
  CloseHandle(dir_handle);
  return 0;
}

void hotload_stop_watching(void) {
  if (g_watch_stop_event) SetEvent(g_watch_stop_event);
  if (g_watch_thread) {
    WaitForSingleObject(g_watch_thread, 2000);
    CloseHandle(g_watch_thread);
    CloseHandle(g_watch_stop_event);
    g_watch_thread = NULL;
    g_watch_stop_event = NULL;
  }
}
#endif

int begin_receiving_config_change_events() {
#ifdef _WIN32
  // Windows (S5): spawns the RDCW watcher thread; idempotent.
  if (g_watch_thread) return 0;

  g_watch_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!g_watch_stop_event) return -1;
  g_watch_thread = CreateThread(NULL, 0, config_watch_thread, NULL, 0, NULL);
  if (!g_watch_thread) {
    CloseHandle(g_watch_stop_event);
    g_watch_stop_event = NULL;
    return -1;
  }
  return 0;
#else
  char* file = dirname(g_config_file);
  CFStringRef file_ref = CFStringCreateWithCString(
                             kCFAllocatorDefault, file, kCFStringEncodingUTF8);

  CFArrayRef paths = CFArrayCreate(NULL,
                                   (const void**)&file_ref,
                                   1,
                                   &kCFTypeArrayCallBacks);

  FSEventStreamRef stream = FSEventStreamCreate(
                                         kCFAllocatorDefault,
                                         handler,
                                         NULL,
                                         paths,
                                         kFSEventStreamEventIdSinceNow,
                                         0.5,
                                         kFSEventStreamCreateFlagNoDefer
                                         | kFSEventStreamCreateFlagFileEvents);

  CFRelease(file_ref);
  CFRelease(paths);

  FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(),
                                           kCFRunLoopDefaultMode);

  FSEventStreamStart(stream);
  return 0;
#endif
}
