#pragma once
// platform/win_compat.h
//
// Windows compiler shim for the three POSIX helpers the portable core relies
// on from src/misc/helpers.h. On Windows these POSIX primitives (fork/exec,
// chmod +x, mmap-based read_file) do not exist or behave differently, so they
// are replaced with Win32 equivalents. The portable core keeps calling the same
// function names; this header supplies the Windows implementations via the
// _WIN32 branch in helpers.h.
//
// Only included by src/misc/helpers.h under #ifdef _WIN32.

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Replaces POSIX `fork()` + `execvp()` used to run sketchybarrc / plugins.
// On Windows we spawn the command through CreateProcess (cmd /C or the sh
// path provided by the caller). Returns false if the process could not be
// launched, true otherwise.
static inline bool win_fork_exec(const char *command) {
  if (!command || !*command) return false;

  char cmdline[4096];
  _snprintf(cmdline, sizeof(cmdline), "/C %s", command);
  cmdline[sizeof(cmdline) - 1] = '\0';

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  memset(&pi, 0, sizeof(pi));

  BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0,
                           NULL, NULL, &si, &pi);
  if (ok) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  return ok != FALSE;
}

// NTFS has no executable bit; this is always a no-op that reports success.
static inline bool win_ensure_executable_permission(const char *filename) {
  (void)filename;
  return true;
}

// Replaces mmap-based read_file: reads the whole file into a freshly
// malloc'd NUL-terminated buffer (caller frees it).
static inline char *win_read_file(const char *path) {
  if (!path) return NULL;

  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return NULL;

  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart > (LONGLONG)0x7FFFFFFF) {
    CloseHandle(file);
    return NULL;
  }

  DWORD length = (DWORD)size.QuadPart;
  char *buffer = (char *)malloc((size_t)length + 1);
  if (!buffer) {
    CloseHandle(file);
    return NULL;
  }

  DWORD read = 0;
  BOOL ok = ReadFile(file, buffer, length, &read, NULL);
  CloseHandle(file);
  if (!ok) {
    free(buffer);
    return NULL;
  }
  buffer[read] = '\0';
  return buffer;
}

// Replaces POSIX setenv() used by the entry point (BAR_NAME, ...).
static inline bool win_setenv(const char *name, const char *value) {
  if (!name) return false;
  return _putenv_s(name, value ? value : "") == 0;
}

#ifdef __cplusplus
}
#endif
