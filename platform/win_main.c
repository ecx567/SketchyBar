// platform/win_main.c
//
// Windows entry point and message pump (S1 stub; S5 fills this in).
//
// On Windows the daemon runs a WinMain that initializes COM, creates a hidden
// message-only window, and pumps GetMessage/DispatchMessage. Worker-thread
// event_producers use PostMessage to marshal onto this UI thread.
//
// S1: compile-time stub that establishes the file and the entry-point contract
// (see platform/win_platform.h). Nothing runs yet.

#include "win_platform.h"

#include <windows.h>

// Declared in src/sketchybar.c for the WinMain path; the real daemon logic and
// the message pump land in S5.
int skbar_win_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 0;
}

void skbar_message_pump(void) {
  // S5: GetMessage/DispatchMessage loop over the hidden HWND.
}
