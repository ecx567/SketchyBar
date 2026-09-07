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
