// platform/win_events.c
//
// Windows system-event routing (S1 stub; S5 fills this in).
//
// Translates Win32 messages into the portable event model: WM_MOUSEMOVE /
// WM_LBUTTONUP / WM_MOUSEWHEEL are hit-tested against item frames and posted
// as MOUSE_*; WM_DISPLAYCHANGE and WM_POWERBROADCAST become DISPLAY_* and
// SYSTEM_WOKE / SYSTEM_WILL_SLEEP. All events are posted via event_post() with
// the same `struct event` the macOS producers use, so src/event.c dispatch is
// unchanged.
//
// S1: compile-time stub that establishes the file and the init contract.

#include "win_platform.h"

#include <windows.h>

void skbar_win_events_init(void) {
  // S5: register the hidden message-only window and its WM_* handlers.
}
