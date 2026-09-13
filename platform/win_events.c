// platform/win_events.c
//
// Windows system-event routing (S1 stub fleshed out in S5).
//
// Translates Win32 messages into the portable event model:
//   WM_MOUSEMOVE / WM_LBUTTONUP / WM_MOUSEWHEEL / WM_MOUSELEAVE -> MOUSE_*
//   WM_DISPLAYCHANGE                                        -> DISPLAY_CHANGED
//   WM_POWERBROADCAST                                       -> SYSTEM_WOKE / SYSTEM_WILL_SLEEP
//   WM_TIMER (animator id)                                  -> ANIMATOR_REFRESH
//   WM_APP_EVENT (marshalled struct event from worker threads) -> event_post
//
// All mouse events carry a malloc'd `struct CGEvent` mirror in the event
// context - the same field surface src/event.c's mouse handlers read through
// the CGEventGet* accessors on macOS. The mirror lives only for the duration
// of the inline event_post()/event_execute() on the pump thread, so it is
// freed immediately after posting. The S5 mirror always uses window number 0
// (bars/windows arrive in S6/S7; display_win.c will feed real window ids into
// this router then).
//
// Mouse coordinates: WM_MOUSEWHEEL lParam is in screen pixels; WM_MOUSEMOVE /
// WM_LBUTTONUP lParam is in client pixels of the hidden window (created at
// 0,0,0,0, so client origin == screen origin). The screen -> CG conversion
// inverts window.c's window_frame_to_device math (SPI_GETWORKAREA + scale):
//   cg.x = (px - work.left) / scale ;  cg.y = (work.bottom - py) / scale

#include "win_platform.h"

#include "event.h"
#include "bar_manager.h"

#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM */
#include <stdlib.h>
#include <stdio.h>

#define WM_APP_EVENT (WM_APP + 1)

// Mouse scale factor. Must match window.c's kWINDOW_SCALE (2.0) so a CG point
// hit-tested here maps through the same coordinate space the renderer uses.
#define kWIN_MOUSE_SCALE 2.0f

// ---------------------------------------------------------------------------
// Windows CGEvent mirror (completes the opaque type declared by the core).
// ---------------------------------------------------------------------------
struct CGEvent {
  CGPoint location;
  CGEventType type;
  int64_t window_number;
  int64_t button_number;
  CGEventFlags flags;
  int64_t delta_axis1;
};

CGPoint CGEventGetLocation(CGEventRef event) { return event->location; }
CGEventType CGEventGetType(CGEventRef event) { return event->type; }
long CGEventGetIntegerValueField(CGEventRef event, long field) {
  switch (field) {
    case kCGEventWindowNumber:          return (long)event->window_number;
    case kCGMouseEventButtonNumber:     return (long)event->button_number;
    case kCGScrollWheelEventDeltaAxis1: return (long)event->delta_axis1;
    default:                            return 0;
  }
}
CGEventFlags CGEventGetFlags(CGEventRef event) { return event->flags; }

// ---------------------------------------------------------------------------
// Coordinate conversion (inverse of window.c's frame math).
// ---------------------------------------------------------------------------
static CGPoint cg_point_from_screen(int px, int py) {
  RECT work = {0};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  CGPoint point;
  point.x = ((CGFloat)px - work.left) / kWIN_MOUSE_SCALE;
  point.y = ((CGFloat)work.bottom - py) / kWIN_MOUSE_SCALE;
  return point;
}

// ---------------------------------------------------------------------------
// Mouse hover bookkeeping (pump thread only).
// ---------------------------------------------------------------------------
static struct bar_item* g_hover_item;
static int g_hover_window;

static struct CGEvent* cg_event_create(CGEventType type, CGPoint point,
                                       int wid, int button,
                                       CGEventFlags flags, int delta) {
  struct CGEvent* event = (struct CGEvent*)malloc(sizeof(struct CGEvent));
  if (!event) return NULL;
  event->type = type;
  event->location = point;
  event->window_number = wid;
  event->button_number = button;
  event->flags = flags;
  event->delta_axis1 = delta;
  return event;
}

static CGEventFlags current_modifier_flags(void) {
  CGEventFlags flags = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) flags |= kCGEventFlagMaskShift;
  if (GetKeyState(VK_CONTROL) & 0x8000) flags |= kCGEventFlagMaskControl;
  if (GetKeyState(VK_MENU) & 0x8000) flags |= kCGEventFlagMaskAlternate;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
    flags |= kCGEventFlagMaskCommand;
  return flags;
}

// Post one MOUSE_* event carrying an owned mirror; the mirror is freed after
// the inline dispatch completes (event_post executes synchronously on the pump
// thread - the dispatched handlers only read the mirror). Returns the mirror's
// window number for callers that track hover windows.
static void post_mouse_event(enum event_type port_type, CGPoint point, int wid,
                             int button, CGEventFlags flags, int delta) {
  // The mirror's CGEventType is only meaningful for MOUSE_UP (bar_item_on_click
  // reads it); ENTERED/EXITED/SCROLLED handlers read location/window/delta.
  CGEventType cg_type = (port_type == MOUSE_UP) ? kCGEventLeftMouseUp
                                                : kCGEventNull;
  struct CGEvent* mirror = cg_event_create(cg_type, point, wid, button, flags,
                                           delta);
  if (!mirror) return;

  struct event event = { mirror, port_type };
  event_post(&event);
  free(mirror);
}

// ---------------------------------------------------------------------------
// Router helpers (all run on the pump thread).
// ---------------------------------------------------------------------------
static void handle_mouse_move(uintptr_t hwnd, uintptr_t lparam) {
  POINT pt = { GET_X_LPARAM((LPARAM)lparam), GET_Y_LPARAM((LPARAM)lparam) };
  ClientToScreen((HWND)hwnd, &pt);
  CGPoint point = cg_point_from_screen(pt.x, pt.y);
  struct bar_item* item = bar_manager_get_item_by_point(&g_bar_manager, point,
                                                        NULL);

  if (item != g_hover_item) {
    // Hover target changed: leave the old item, enter the new one. Window id 0
    // in S5 (no display windows yet); enter/exit transitions stay item-based.
    //
    // Enter/exit are routed DIRECTLY to the bar manager, not through event_post:
    // bar_manager.h's get_item_by_wid takes a uint32_t wid (event.c narrows the
    // int64 CGEvent field), so item identity cannot survive a queue round-trip
    // until real display windows exist in S6. The router already hit-tested the
    // exact item - resolving it again through a window id would be lossy.
    if (g_hover_item) {
      bar_manager_handle_mouse_exited(&g_bar_manager, g_hover_item);
    }
    g_hover_item = item;
    g_hover_window = 0;
    if (item) {
      bar_manager_handle_mouse_entered(&g_bar_manager, item);
    }
  }

  // S5 does NOT arm TrackMouseEvent: the hidden conduit window is zero-sized,
  // so TME_LEAVE would fire immediately and spurious-exit the hover chain.
  // WM_MOUSELEAVE is still handled below (explicit / future S7 bar windows arm
  // TME themselves when the cursor has a real surface to leave).
}

static void handle_mouse_leave(uintptr_t hwnd) {
  (void)hwnd;
  POINT pt;
  GetCursorPos(&pt);
  CGPoint point = cg_point_from_screen(pt.x, pt.y);
  if (g_hover_item) {
    // Same direct routing rationale as handle_mouse_move: the leaving item is
    // known exactly, so the exit is delivered without a queue round-trip.
    bar_manager_handle_mouse_exited(&g_bar_manager, g_hover_item);
  }
  g_hover_item = NULL;
  g_hover_window = 0;
}

static void handle_mouse_up(uintptr_t hwnd, uintptr_t lparam) {
  POINT pt = { GET_X_LPARAM((LPARAM)lparam), GET_Y_LPARAM((LPARAM)lparam) };
  ClientToScreen((HWND)hwnd, &pt);
  CGPoint point = cg_point_from_screen(pt.x, pt.y);
  struct bar_item* item = bar_manager_get_item_by_point(&g_bar_manager, point,
                                                        NULL);
  if (!item) return;
  post_mouse_event(MOUSE_UP, point, 0, 0, current_modifier_flags(), 0);
}

static void handle_mouse_wheel(uintptr_t wparam, uintptr_t lparam) {
  int delta = GET_WHEEL_DELTA_WPARAM((WPARAM)wparam) / WHEEL_DELTA;
  // lParam is in screen pixels on WM_MOUSEWHEEL (client coords on the rest).
  CGPoint point = cg_point_from_screen(GET_X_LPARAM((LPARAM)lparam),
                                       GET_Y_LPARAM((LPARAM)lparam));
  post_mouse_event(MOUSE_SCROLLED, point, 0, 0, 0, delta);
}

static void handle_animator_timer(uintptr_t wparam) {
  if (wparam != skbar_animator_timer_id()) return;
  // Timestamp the refresh with the QPC clock animation.c's animator->clock is
  // derived from, so animation_update's ratio math is platform-identical.
  struct event event = { (void*)(intptr_t)skbar_animator_timestamp(),
                         ANIMATOR_REFRESH };
  event_post(&event);
}

static void handle_display_change(void) {
  struct event event = { NULL, DISPLAY_CHANGED };
  event_post(&event);
}

static void handle_power_broadcast(uintptr_t wparam) {
  struct event event = { NULL, 0 };
  switch (wparam) {
    case PBT_APMSUSPEND:
      event.type = SYSTEM_WILL_SLEEP;
      break;
    case PBT_APMRESUMESUSPEND:
    case PBT_APMRESUMEAUTOMATIC:
    case PBT_APMRESUMECRITICAL:
      event.type = SYSTEM_WOKE;
      break;
    default:
      return;   /* unhandled power notification */
  }
  event_post(&event);
}

// Worker-thread producers marshal their struct event as a heap copy through
// WM_APP_EVENT; the pump executes it inline and frees the copy. Only the
// struct event itself is copied - the context pointer inside is NOT freed
// (S5 cross-thread producers post NULL context; ownership handoff for richer
// payloads is S7).
static void handle_app_event(uintptr_t wparam) {
  struct event* event = (struct event*)wparam;
  if (!event) return;
  event_post(event);
  free(event);
}

// ---------------------------------------------------------------------------
// Public contract (win_platform.h).
// ---------------------------------------------------------------------------
void skbar_win_events_init(void) {
  // S5: routing is a pure message->event table; the window + pump lifecycle is
  // owned by win_main.c. S6/S7 register display/bar windows here.
  g_hover_item = NULL;
  g_hover_window = 0;
}

void skbar_win_post_event(struct event* event) {
  struct event* copy = (struct event*)malloc(sizeof(struct event));
  if (!copy) return;
  *copy = *event;

  HWND hwnd = (HWND)skbar_win_event_hwnd();
  if (hwnd && PostMessageW(hwnd, WM_APP_EVENT, (WPARAM)copy, 0)) return;

  // Window not ready yet or post failed: drop the copy (matches macOS's
  // best-effort semantics for a main queue that is never created).
  free(copy);
}

int skbar_win_handle_message(uintptr_t hwnd, uintptr_t msg,
                             uintptr_t wparam, uintptr_t lparam) {
  switch (msg) {
    case WM_MOUSEMOVE:     handle_mouse_move(hwnd, lparam);     return 1;
    case WM_MOUSELEAVE:    handle_mouse_leave(hwnd);            return 1;
    case WM_LBUTTONUP:     handle_mouse_up(hwnd, lparam);       return 1;
    case WM_MOUSEWHEEL:    handle_mouse_wheel(wparam, lparam);  return 1;
    case WM_TIMER:         handle_animator_timer(wparam);       return 1;
    case WM_DISPLAYCHANGE: handle_display_change();             return 1;
    case WM_POWERBROADCAST: handle_power_broadcast(wparam);     return 1;
    case WM_APP_EVENT:     handle_app_event(wparam);            return 1;
    default:                                                     return 0;
  }
}