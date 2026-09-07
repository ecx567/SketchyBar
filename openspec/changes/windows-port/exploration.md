# Change Exploration: Windows Port

## Introduction

This exploration covers the SketchyBar codebase (fork of FelixKratz/SketchyBar, v2.24.0, C99 + Objective-C, macOS-only) with the goal of producing a strategy for a native Windows port that preserves the exact visual design and behavior of the macOS version. This document is analysis only; no implementation artifacts were produced.

Discovery is based on the source tree at the repository root, the `makefile`, the demo config `sketchybarrc`, and the plugin scripts under `plugins/`.

## Current State

- [x] Map every Apple/private framework dependency per source file
- [x] Identify the rendering pipeline and whether draw calls share a single seam
- [x] Map the event/notification model (who produces events, who consumes them)
- [x] Document the config/plugin script semantics that must be preserved
- [x] Document the build system (frameworks, `.om` objects, universal binary)
- [ ] Validate Windows API equivalents (deferred to the research phase)
- [ ] Decide the abstraction boundary shape (per-area shim vs. unified backend)

Stack facts:

- Language: C99 (`-std=c99 -Wall -O3 -ffast-math -fvisibility=hidden -fno-common`) plus 5 Objective-C files (`display_nsscreen.m`, `workspace.m`, `wifi.m`, `media.m`, `layer.m`) compiled as `.om`.
- Linked frameworks: Carbon, AppKit, QuartzCore, CoreAudio, CoreWLAN, CoreVideo, IOKit, plus private: SkyLight (SLS*), DisplayServices, MediaRemote.
- Build: Makefile only. Universal binary via separate x86/arm64 builds merged with `lipo`. Targets: all/clean/arm/x86/profile/leak/universal/debug/asan.
- No test suite, no CI.

## Findings

### 1. Apple API map (per area, with port implications)

| Area | Files | Apple/private APIs | Port surface |
|---|---|---|---|
| Entry point / client mode | `sketchybar.c` | SLSMainConnectionID, SLSGetSpaceManagementMode, SLSRegisterNotifyProc (event codes 904, 905, 1401, 1508, 1322, 1327, 1328), SLSWindowManagementBridgeSetDelegate, dlsym'd SLSTransactionAddPostDecodeAction, CGSetLocalEventsSuppressionInterval, mach client | Lock file `/tmp/<name>_<pid>.lock`; CLI flags `--message/-m`, `--version/-v`, `--config/-c`, `--help/-h`; `setenv("BAR_NAME")`. Space-management mode + window bridge are Mission Control integration — Windows equivalent is minimal. |
| IPC | `mach.c` | bootstrap_look_up, task_get_special_port(TASK_BOOTSTRAP_PORT), mach_msg send/recv (100 ms receive timeout), response port | Core of the `sketchybar --set` client protocol; must be replaced by a Windows IPC (named pipe or window message) with the same message/response semantics. |
| Window / compositing | `window.c`, `surface.c`, `layer.m` | SLSAddSurface/SLSBindSurface/SLSSetSurfaceBounds/SLSSetSurfaceResolution(2.0)/SLSSetSurfaceOpacity/SLSSetSurfaceColorSpace/SLSOrderSurface/SLSFlushSurface/SLSRemoveSurface, CGSNewRegionWithRect, tags kCGSExposeFadeTagBit\|kCGSPreventsActivationTagBit, CAContext/CALayer (private) | The hardest surface. A layered, click-through, always-on-top, non-activating window. Windows: WS_EX_LAYERED + WS_EX_TOOLWINDOW + WS_EX_NOACTIVATE, WS_EX_TRANSPARENT for mouse pass-through, DirectComposition or DWM redirection. Popup/bar ordering (`order_mode W_ABOVE`) maps to z-order management. |
| Render target | `context.c` | CGBitmapContextCreate (8-bit, premultiplied first, 32-bit host byte order, scale 2.0), CGContextScaleCTM, kCGInterpolationNone, CGContextSetAllowsFontSmoothing | ONE bitmap context per surface; all draw code targets `CGContextRef` only. This is the cleanest port seam: swap CGContext for a Skia/Direct2D canvas with the same coordinate space (points × 2.0 scale). |
| Draw primitives | `text.c`, `font.c`, `image.c`, `background.c`, `shadow.c`, `color.c`, `graph.c`, `slider.c`, `popup.c`, `alias.c`, `bar_item.c` | CoreText (CTFont, CTLine, OpenType→TrueType feature map: liga/dlig/tnum/pnum/smcp/c2sc/onum/lnum/afrc/frac/subs/sups/zero/swsh/cswh/calt/salt/ss01..ss20), CGImage, CGPath/rect ops | All use CGContextRef + CGColor (0xAARRGGBB parsed in `color.c`, clamped floats). DirectWrite has the same OpenType feature model for 1:1 typography. Image pipeline (`image_load`, `app.` prefix → `workspace_icon_for_app`) needs an icon-extraction equivalent (HICON/SHGetFileInfo / IShellItemImageFactory). |
| Animation clock | `animation.c` | CVDisplayLink → dispatches ANIMATOR_REFRESH with hostTime; interpolation tanh/sin/square/exp/linear; duration = frames/60 | Replace with QPC/timeGetTime-driven timer at 60 Hz equivalent. Pure logic — port 1:1. |
| System events | `event.c`, `event.h` | clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX), pthread_main_np, dispatch_sync/async main queue | 25 event types enum: APPLICATION_FRONT_SWITCHED, SPACE_CHANGED, DISPLAY_ADDED/REMOVED/MOVED/RESIZED/CHANGED, MENU_BAR_HIDDEN_CHANGED, SYSTEM_WOKE, SYSTEM_WILL_SLEEP, SHELL_REFRESH, ANIMATOR_REFRESH, MACH_MESSAGE, MOUSE_UP/DRAGGED/ENTERED/EXITED/SCROLLED, VOLUME_CHANGED, WIFI_CHANGED, BRIGHTNESS_CHANGED, POWER_SOURCE_CHANGED, MEDIA_CHANGED, COVER_CHANGED, SPACE_WINDOWS_CHANGED, DISTRIBUTED_NOTIFICATION, HOTLOAD. Event dispatch is a simple table — portable. |
| Display management | `display.c`, `display_nsscreen.m`, `workspace.m` | CGDisplayAdd/Remove/Moved/DesktopShapeChanged flags, DisplayServicesGetBrightness + register brightness notifications, NSScreen frame/visibleFrame (top inset, −1 px Tahoe quirk), backingScaleFactor, safeAreaInsets.top (notch), CGDisplayIsBuiltin | Display add/remove/resolution events → Windows WM_DISPLAYCHANGE / EnumDisplayMonitors; multiple-display adid mapping (`display_arrangement`, `display_active_display_adid`). "Notch" is a Hardware-agnostic bar-top inset concept — keep as a config-driven inset. Menu bar hiding (MENU_BAR_HIDDEN_CHANGED) → taskbar auto-hide events on Windows. |
| Front app / spaces / windows | `bar_manager.c`, `app_windows.c`, `workspace.m` | NSDistributedNotificationCenter (front app, space change), SLSWindowIterator* + tags/attributes filter, SLSRequestNotificationsForWindows, SLSHWCaptureSpace | Front app: GetForegroundWindow/GetGUIThreadInfo. Spaces: Windows has no equivalent — virtual desktop changes via IVirtualDesktopManager (needs per-desktop mapping decision). App windows: EnumWindows + window title/class filtering; window events via WinEvent hook (EVENT_OBJECT_CREATE/DESTROY/SHOW/HIDE). |
| Mouse | `mouse.c` | Carbon kEventClassMouse (up/dragged/entered/exited/wheel/scroll), CopyEventCGEvent | Per-item hit testing already exists (bar_manager_get_item_by_point); platform mouse events → WM_MOUSEMOVE/WM_LBUTTONUP/WM_MOUSEWHEEL hit-tested against item frames. |
| Config hot reload | `hotload.c` | FSEventStream (NOT kqueue), realpath, setenv, chdir, fork/exec | Config search order: XDG_CONFIG_HOME → `$HOME/.config/<name>/` → `$HOME/.<file>`; CONFIG_DIR env; executable bit check. Windows: ReadDirectoryChangesW + CreateProcess (sh or bash for `sketchybarrc`). |
| System info | `media.m`, `wifi.m`, `power.c`, `volume.c` | MediaRemote (MRMediaRemoteGetNowPlayingInfo etc.; locked for third-party use on macOS 15.3), CoreWLAN + SCDynamicStore (key `.*/Network/Global/IPv4`), IOKit IOPSCopyPowerSourcesInfo + notifications, CoreAudio AudioObjectGetPropertyData (volume/mute, main+left) | Windows equivalents: SMTC (SystemMediaTransportControls), WinRT NetworkInformation, GetSystemPowerStatus + power events (GUID_ACDC_POWER_SOURCE), IAudioEndpointVolume. |
| Command dispatch | `message.c`, `misc/defines.h` | open_memstream, regex (POSIX regcomp) for item selectors | Pure parsing/batching logic — fully portable (POSIX regex also on Windows, or swappable). Full domain vocabulary captured: --add/--set/--default/--push/--trigger/--animate/--bar/--subscribe/--query/--reorder/--rename/--remove/--move/--exit/--hotload/--reload/--load-font; item sub-domains icon/label/background/graph/alias/popup/shadow/image/knob/slider/font/color/border_color/highlight_color/fill_color. |
| Plugins | `plugins/*.sh`, `sketchybarrc` | n/a (shell scripts) | Contract: scripts read env vars (SENDER, NAME, SELECTED, INFO) and call back via `sketchybar --set …`. Runs wherever a POSIX sh is available (Git Bash/MSYS2) — semantics preserved as long as the `sketchybar` client binary and env-var contract exist on Windows. |

### 2. Visual design preservation (rendering pipeline)

The pipeline is deliberately simple and centralized:

1. `context.c` creates one `CGBitmapContextCreate` per surface at 2.0 scale, `kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host`, `kCGInterpolationNone`, font smoothing toggle from the bar.
2. Every draw routine (`bar_item_draw`, `background_draw`, `text_draw`, `image_draw`, `graph_draw`, `slider_draw`, `alias_draw`, popup draw) receives the same `CGContextRef` and uses only CG color/path/image/text calls.
3. The bitmap is flushed to the compositor via `SLSFlushSurface` inside the SLS surface bound to the window (`surface.c`, `layer.m`).

This means the ENTIRE visual output is produced by CGContext calls. If that graphics backend is replaced by a canvas API with the same operation set (Skia, Direct2D, or WGPU), pixel output can be preserved 1:1 — the abstraction boundary is a single `context` module, not a scatter of platform calls. Typography is the riskiest visual detail and must be validated in the research phase (CoreText shaping/features vs. DirectWrite with the same OpenType feature list).

Non-graphics visual details to preserve: window flags (topmost `sticky`, `show_in_fullscreen`, `position`, `margin`, `blur_radius` — blur requires Windows translucency: acrylic/backdrop blur; `hidden`, `notch_width/offset/display_height`, `align`, `font_smoothing`, `shadow`).

### 3. Event model — producers and consumers

- Producers: display callbacks (`display.c`), CVDisplayLink (`animation.c`), Carbon mouse (`mouse.c`), CoreAudio/CoreWLAN/IOKit/MediaRemote handlers (`volume.c`, `wifi.c`, `power.c`, `media.m`), distributed notifications (`workspace.m`, `bar_manager.c`), mach messages (`mach.c`), FSEventStream (`hotload.c`), app window notifications (`app_windows.c`).
- Consumers: single event dispatch table in `event.c` → `bar_manager_handle_*` + `bar_manager_custom_events_trigger` (which feeds subscribed plugins with env vars).
- Plumbing used: mach ports, Grand Central Dispatch (dispatch_async to main queue, `pthread_main_np`), clock_gettime_nsec_np, run loop sources (IOPSNotificationCreateRunLoopSource, CFRunLoopAddSource, FSEventStreamScheduleWithRunLoop).

Windows translation: each producer becomes a Windows callback (WinEvent hook, WM_DISPLAYCHANGE, IMM device notifications, power broadcast WTS/PBT events, timer tick); event types and the dispatch table stay identical. GCD main-queue semantics → a single UI thread message pump (`GetMessage`/`DispatchMessage`); the `system_woke` late-double-post workaround (500 ms delayed re-post) maps to a `SetTimer`/threadpool delay.

### 4. Config and plugin semantics to preserve

- `sketchybarrc` is a shell script executed once; it issues `sketchybar --…` commands that go through the same mach IPC as runtime updates. Semantics are preserved by shipping/handling it as a shell script (sh/bash on Windows) or by a future native parser — the message grammar is fully defined in `message.c` + `misc/defines.h`.
- `CONFIG_DIR` and `PLUGIN_DIR=$CONFIG_DIR/plugins` env contract; `--load-font` registers fonts; `--reload` re-executes config (HOTLOAD event); item regex selectors (`/pattern/`) are supported by `--set`.
- Plugins: `#!/bin/sh` scripts reading `$SENDER/$NAME/$SELECTED/$INFO` and echoing back `sketchybar --set …`. Portable under MSYS2/Git Bash; `ensure_executable_permission` on macOS becomes a no-op/different check on Windows (everything is executable there; the fork/exec path is CreateProcess).

### 5. Build system translation

- The Makefile's framework list collapses to a single graphics backend + Win32/WinRT APIs; no `lipo` needed (single x64 target; ARM64 possible later).
- The 5 `.m` files must be re-implemented in C (or replaced by Windows-specific modules) — they contain only observers/property reads, no shared logic user-visible: `display_nsscreen.m` (insets), `workspace.m` (scale, notifications, icons), `wifi.m`, `media.m`, `layer.m`.
- A suggested structure: keep the portable core (`message.c`, `event.c`, `bar_manager.c` logic, `bar_item.c`, `misc/*`, config grammar) untouched; isolate platform code behind the areas in the table above.

## Critical Findings

1. The codebase does NOT use kqueue — file watching is `FSEventStream` in `src/hotload.c`. The Windows equivalent is ReadDirectoryChangesW, not a kqueue translation.
2. All visual output flows through a single `CGBitmapContext` (`context.c`) consumed by every draw routine as `CGContextRef` — this is the primary port seam and makes 1:1 visual preservation achievable with a context-compatible backend (Skia/Direct2D/WGPU) rather than a rewrite.
3. The private SkyLight surface/window stack (SLSAddSurface, CGSNewRegionWithRect, CAContext) has no direct Windows analogue; the equivalent must be assembled from layered windows + DirectComposition — this is the highest-risk area and should be its own workstream with a spike first.
4. `sketchybar --set` IPC is mach-based and synchronous-with-response (`open_memstream` → mach send). Windows needs an equivalent transactional IPC; message grammar itself is portable.
5. Space/Mission Control concepts (spaces, space_change, space_windows_change) have no OS-level Windows equivalent. Virtual desktops exist but with a much thinner API (IVirtualDesktopManager, no per-desktop window enumeration without a COM service). This requires a scoping decision: mimic virtual-desktop awareness (Windows 11 API) vs. keep the events space-neutral.
6. All 5 Objective-C files are thin platform adapters, consistent with the "fork cast" — porting them as C modules preserves behavior and removes the ObjC toolchain requirement entirely.

## Risks

| Risk | Level | Mitigation |
|---|---|---|
| SkyLight compositing semantics (order, tags, click-through, activation prevention, blur) differ subtly from layered-window + DirectComposition | HIGH | Spike first: 2–3 layered windows (bar + popup) with acrylic blur before the full port |
| Text rendering differences (CoreText vs DirectWrite shaping) change pixel appearance | HIGH | Validation harness: render known sketchybarrc fonts to bitmaps on both platforms and diff; reuse the OpenType feature map (already enumerated in `font.c`) |
| Spaces/Mission Control events have no Windows equivalent → behavior drift for space-dependent items | MEDIUM | Decide scope early; option: virtual desktop events via Windows 11 API or space-neutral degradation |
| Config runs as POSIX shell — MSYS2/Git Bash dependency | MEDIUM | Vendor a sh (or document MSYS2 prerequisite); optionally a native grammar parser later (message grammar is complete) |
| Media/MR, WiFi, brightness, power event parity takes time and has API churn (MediaRemote already locked on macOS 15.3) | MEDIUM | Event producers are isolated; ship core first, add media/wifi/brightness producers iteratively |
| No tests/CI upstream means regression risk during translation | MEDIUM | Introduce snapshot tests at the CGContext seam: golden bitmap per item type; keep them OS-neutral |

## Recommendations

1. Proceed with the SDD design phase scoped around the abstraction boundary in this document (single graphics seam + per-area platform adapters listed in the API map).
2. Keep the portable core untouched: `message.c`, `event.c` dispatch, `bar_manager.c` logic, `bar_item.c`, `misc/*`, item types (`graph`, `slider`, `background`, `text`, `image`, `alias`, `popup`, `shadow`, `color`).
3. Run research lanes (external evidence) for: (a) Direct2D/DirectWrite vs Skia for pixel-identical reproduction, (b) Windows layered-window + blur/acrylic semantics vs SLS surfaces, (c) virtual-desktop API options, (d) SMTC/WinRT media metadata parity, (e) IPC choice (named pipes vs ALPC vs window messages) with response semantics.
4. Spike before full implementation: minimal layered bar window rendering a bitmap from the existing `context.c`-style pipeline.
5. Decide the spaces scope (virtual desktop awareness vs space-neutral) as an explicit requirement in the spec, since it changes the event surface for plugins.
6. Plan the Windows build as a CMake+MSVC (or CMake+clang-cl) project replacing the Makefile; single x64 target initially; remove `lipo`.

## Artifacts

- This exploration document (analysis only; no source files modified).
- Companion Engram observation (topic `sdd/sketchybar/explore`) with the per-file API map and port notes.