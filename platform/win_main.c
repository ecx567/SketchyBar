// platform/win_main.c
//
// Windows entry point and message pump (S1 stub fleshed out in S5).
//
// On Windows the daemon runs a hidden top-level window and pumps
// GetMessage/DispatchMessage (skbar_message_pump). Worker-thread event
// producers marshal onto this pump thread via skbar_win_post_event()
// (platform/win_events.c) + PostMessage(WM_APP_EVENT).
//
// S5 notes:
//  - The hidden window is a real (top-level, never shown) window rather than a
//    message-only HWND: broadcast messages that drive the portable event model
//    (WM_DISPLAYCHANGE, WM_POWERBROADCAST) only reach top-level windows. See
//    apply-progress.md tasks 5.1/5.2 for the deviation note.
//  - With the pump live, running the `sketchybar` executable now blocks for the
//    daemon lifetime (the S1 stub main returned immediately); src/sketchybar.c
//    joins the build in task 7.5 and this main is removed then.
//  - skbar_win_main is safe to call from a worker thread (the events test does
//    exactly that) - everything it needs lives on the calling thread.

#include "win_platform.h"

#ifndef SKBAR_NO_MAIN
#include <stdio.h>
#endif

#include <windows.h>
#include <objbase.h>

// HWND handed to the pump; read cross-thread through interlocked accessors.
static volatile PVOID g_event_hwnd;
static volatile DWORD g_pump_thread_id;

uintptr_t skbar_win_event_hwnd(void) {
  return (uintptr_t)InterlockedCompareExchangePointer(
             (volatile PVOID*)&g_event_hwnd, NULL, NULL);
}

int skbar_win_is_main_thread(void) {
  return GetCurrentThreadId() == g_pump_thread_id;
}

uint64_t skbar_win_monotonic_ns(void) {
  // GetTickCount64: 15.6 ms resolution is plenty for the portable core's
  // 150 ms scroll-coalescing window (see win_platform.h contract).
  return (uint64_t)GetTickCount64() * 1000000ULL;
}

static LRESULT CALLBACK event_wnd_proc(HWND hwnd, UINT msg,
                                       WPARAM wparam, LPARAM lparam) {
  if (msg == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }

  if (skbar_win_handle_message((uintptr_t)hwnd,
                               (uintptr_t)msg,
                               (uintptr_t)wparam,
                               (uintptr_t)lparam)) {
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void skbar_message_pump(void) {
  MSG message;
  while (GetMessageW(&message, NULL, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

int skbar_win_main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 1;

  WNDCLASSEXW wc = {0};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = event_wnd_proc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"sketchybar_event";
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    // The class outlives individual hidden windows (it is process-wide); a
    // re-registration is a normal observation, not an error.
    CoUninitialize();
    return 1;
  }

  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0,
                              0, 0, 0, 0,
                              NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) {
    CoUninitialize();
    return 1;
  }

  // Publish the window once (interlocked) so worker threads / the test harness
  // can poll skbar_win_event_hwnd() for pump readiness.
  InterlockedExchangePointer((volatile PVOID*)&g_event_hwnd, (PVOID)hwnd);
  g_pump_thread_id = GetCurrentThreadId();

  skbar_win_events_init();

  skbar_message_pump();

  // The pump exited: tear down in reverse order. KillTimer (animator) runs
  // from animation.c's animator_destroy_display_link and tolerates a dead
  // window, but DestroyWindow before it would leak the timer into the
  // destroyed-window callback path - so order: destroy window, then clear the
  // published handle so no late SetTimer lands on a zombie HWND.
  DestroyWindow(hwnd);
  InterlockedExchangePointer((volatile PVOID*)&g_event_hwnd, NULL);

  CoUninitialize();
  return 0;
}

#ifndef SKBAR_NO_MAIN
// S1 temporary entry point: lets the `sketchybar` executable target link before
// src/sketchybar.c (its _WIN32 main, task 1.5) joins PORTABLE_CORE, which
// becomes possible once the core headers compile on Windows (S2/S4). REMOVE
// THIS when src/sketchybar.c is added to PORTABLE_CORE (task 7.5) to avoid a
// duplicate main. The events test target defines SKBAR_NO_MAIN (its own main
// drives the suite) and links this file for the pump symbols only.
int main(int argc, char **argv) {
  return skbar_win_main(argc, argv);
}
#endif