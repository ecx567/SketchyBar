#pragma once
// platform/win_platform.h
//
// The Windows platform abstraction boundary.
//
// This is the single header that defines WHERE the platform boundary lives for
// the SketchyBar Windows port. The portable core (src/message.c, src/event.c,
// src/bar_manager.c, src/bar_item.c, src/misc/*) never #includes Apple or Win32
// headers directly; every OS-specific concern is routed through a `platform/*`
// module that exposes the symbols the core already calls (_begin/_stop/_forced,
// etc.). This mirrors the design's "per-area shim" decision.
//
// In S1 the modules below are compile-time stubs. Later slices (S2-S7) fill each
// one in without touching the core.
//
//   win_main.c        entry point + message pump            (S5)
//   win_events.c      WM_* -> event_post routing            (S5)
//   layer_dcomp.c     DirectComposition visual layer        (S3)
//   display_win.c     EnumDisplayMonitors / DPI             (S6/S7)
//   workspace_win.c   virtual desktops + front app          (S7)
//   wifi_win.c        WlanApi / WinRT NetworkInformation    (S6)
//   media_win.c       SMTC media now-playing                (S6)
//
// Only Windows-targeting translation units include this header.

// ---------------------------------------------------------------------------
// Graphics seam stub (S2 fills this in).
//
// On macOS the entire visual output flows through a single CGBitmapContext
// (src/context.c) consumed by every draw routine as CGContextRef. On Windows
// CGContextRef is an opaque `struct skbar_context*` that will wrap an
// SkCanvas + SkBitmap. In S1 we only define the opaque type so downstream
// slices (and the core headers that reference CGContextRef) have a stable
// compile-time placeholder.
// ---------------------------------------------------------------------------
struct skbar_context;

// In the Windows build, CGContextRef is the opaque skbar_context pointer.
// Kept behind a guard so the macOS path keeps using the real CoreGraphics
// typedef. Headers that need this typedef include win_platform.h first.
typedef struct skbar_context* SKB_CONTEXT_REF;

// The portable core's struct event (src/event.h) is referenced by the S5
// marshalling seam below; win_platform.h deliberately stays windows.h-free so
// keep this as a forward declaration and let the caller include event.h.
#include <stdint.h>
struct event;

// Abstraction-boundary markers for the portable core: the set of symbols each
// platform module is REQUIRED to expose so the core compiles unchanged. S1
// declares only the entry points relevant to the skeleton; later slices extend
// this contract as their modules land.
//
// Entry / lifecycle (platform/win_main.c, S5)
int  skbar_win_main(int argc, char** argv);
void skbar_message_pump(void);

// Event routing (platform/win_events.c, S5)
void skbar_win_events_init(void);

// ---------------------------------------------------------------------------
// S5 seam: asynchronous event marshalling (src/event.c, worker threads).
//
// event_post() is the portable core's single entry point for producing events.
// On macOS it dispatch_sync's onto the main queue; on Windows the main thread
// is the message pump, so off-pump producers marshal a heap copy of the struct
// event through skbar_win_post_event() (PostMessage WM_APP_EVENT; the pump
// dispatches and frees the copy). Events produced ON the pump thread execute
// inline via event_execute() with no copy.
//
// Ownership: the async path transfers ONLY the struct event copy - the
// context pointer (event->context) is NOT freed by the pump. S5 produces
// NULL-context events from worker threads (hotload); S6/S7 producers that
// cross threads (mach frames, media payloads) must hand off ownership.
// ---------------------------------------------------------------------------
int      skbar_win_is_main_thread(void);
void     skbar_win_post_event(struct event* event);
uintptr_t skbar_win_event_hwnd(void);   /* 0 until the hidden event window exists */

// Internal platform router (win_main.c <-> win_events.c): routes one Win32
// message. The raw message args are bit patterns - win_events.c casts them
// back to windows.h types so win_platform.h never touches Win32 headers.
// Returns 1 when the message was handled.
int skbar_win_handle_message(uintptr_t hwnd, uintptr_t msg,
                             uintptr_t wparam, uintptr_t lparam);

// ---------------------------------------------------------------------------
// S5 time seam.
// ---------------------------------------------------------------------------
// Monotonic clock in nanoseconds, replacing macOS
// clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX) used by src/event.c's
// scroll coalescing (SCROLL_TIMEOUT). GetTickCount64-based (15.6ms tick
// resolution - sufficient for the 150ms window; QueryPerformanceCounter is
// S7 polish if scroll grouping ever needs finer granularity).
uint64_t skbar_win_monotonic_ns(void);

// Animator 60 Hz tick (src/animation.c, S5). The timer id lives in
// animation.c; win_events.c's WM_TIMER routing consults it and stamps the
// ANIMATOR_REFRESH context with skbar_animator_timestamp() (QPC ticks, with
// animator->clock == QPC frequency so animation_update's ratio math is
// unchanged).
uintptr_t skbar_animator_timer_id(void);   /* 0 when the animator is idle */
uint64_t  skbar_animator_timestamp(void);
