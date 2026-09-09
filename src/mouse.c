// src/mouse.c
//
// Portable mouse backend entry point. On macOS this installs Carbon mouse
// event handlers that forward mouse messages as MOUSE_* events carrying a
// CGEventRef context. On Windows there is no Carbon dispatcher: the hidden
// event window's WM_MOUSE* router (platform/win_events.c) is the mouse
// backend, posting the same MOUSE_* events with an equivalent CGEvent mirror
// in the context. mouse_begin() exists on both platforms so the portable
// core's init calls compile unchanged.

#include "mouse.h"

#ifdef _WIN32
void mouse_begin(void) {
  // Windows (S5): mouse transit routing is owned by win_events.c's router
  // (WM_MOUSEMOVE/WM_LBUTTONUP/WM_MOUSEWHEEL/WM_MOUSELEAVE -> MOUSE_*). There
  // is nothing to install here; this no-op satisfies the shared init contract.
}
#else
#include <Carbon/Carbon.h>

static const EventTypeSpec mouse_events [] = {
    { kEventClassMouse, kEventMouseUp },
    { kEventClassMouse, kEventMouseDragged },
    { kEventClassMouse, kEventMouseEntered },
    { kEventClassMouse, kEventMouseExited },
    { kEventClassMouse, kEventMouseWheelMoved },
    { kEventClassMouse, kEventMouseScroll }
};

static int carbon_event_translation[] = {
  [kEventMouseUp] = MOUSE_UP,
  [kEventMouseDragged] = MOUSE_DRAGGED,
  [kEventMouseEntered] = MOUSE_ENTERED,
  [kEventMouseExited]  = MOUSE_EXITED,
  [kEventMouseWheelMoved] = MOUSE_SCROLLED,
  [kEventMouseScroll] = MOUSE_SCROLLED
};

static pascal OSStatus mouse_handler(EventHandlerCallRef next, EventRef e, void *data) {
  enum event_type event_type = carbon_event_translation[GetEventKind(e)];

  CGEventRef cg_event = CopyEventCGEvent(e);
  struct event event = { (void *) cg_event, event_type };
  event_post(&event);
  CFRelease(cg_event);

  return CallNextEventHandler(next, e);
}

void mouse_begin(void) {
  InstallEventHandler(GetEventDispatcherTarget(),
                      NewEventHandlerUPP(mouse_handler),
                      GetEventTypeCount(mouse_events),
                      mouse_events, 0, 0);
}
#endif