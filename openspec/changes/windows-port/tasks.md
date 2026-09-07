# Tasks: Native Windows Port (Pixel-Identical SketchyBar)

## Review Workload Forecast

| Field | Value |
|-------|-------|
| Estimated changed lines | 1100–1400 (7 slices) |
| 400-line budget risk | High |
| Chained PRs recommended | Yes |
| Suggested split | S1→S2→S3→S4→S5→S6→S7 (stacked-to-main) |
| Delivery strategy | auto-chain |
| Chain strategy | stacked-to-main |

Decision needed before apply: No
Chained PRs recommended: Yes
Chain strategy: stacked-to-main
400-line budget risk: High

### Suggested Work Units

| Unit | Goal | Likely PR | Focused test command | Runtime harness | Rollback boundary |
|------|------|-----------|----------------------|-----------------|-------------------|
| S1 | Build skeleton + platform shims | PR 1 | `cmake --build . --target sketchybar` | N/A — compile-only; no runtime | CMakeLists.txt + platform/* removed; macOS Makefile unaffected |
| S2 | Skia graphics seam + visual parity | PR 2 | `ctest --test-dir build -R bitmap_diff` | bitmap-diff harness renders reference items, outputs PNG diffs | context.c .m→.c swap reverted; Skia libs removed from CMake |
| S3 | Compositing spike + layered window | PR 3 | `ctest --test-dir build -R compositor_spike` | 2-3 layered HWNDs rendered with blur, validated visually | platform/win_window.c + layer_dcomp.c removed; surface.c reverted |
| S4 | Named-pipe IPC | PR 4 | `ctest --test-dir build -R ipc_pipe` | Client sends message, server echoes, round-trip timed | mach.c reverted to Mach ports; named pipe code removed |
| S5 | System events + message pump | PR 5 | `ctest --test-dir build -R events` | sketchybar starts, logs events, responds to config | platform/win_events.c + win_main.c removed; mouse.c/hotload.c reverted |
| S6 | System info adapters | PR 6 | `ctest --test-dir build -R sysinfo` | Volume/power/wifi/media/brightness queries return valid data | platform/*_win.c files removed; volume.c/power.c/display.c reverted |
| S7 | Spaces/VD + front-app + config | PR 7 | `ctest --test-dir build -R spaces && sketchybar --version` | Full sketchybarrc load + virtual desktop switch detection | platform/workspace_win.c + hotload.c reverted |

## S1: Build Skeleton (PR 1)

- [x] 1.1 Create `CMakeLists.txt` with clang-cl flags (`-std=c99 -Wall -O3`), Skia SDK find, link libs (`dwmapi.lib dcomp.lib d2d1.lib dxgi.lib Winmm.lib Setupapi.lib`), single x64 target, `#ifdef _WIN32` include paths
- [x] 1.2 Create `platform/` directory structure: `platform/win_main.c`, `platform/win_events.c`, `platform/layer_dcomp.c`, `platform/display_win.c`, `platform/workspace_win.c`, `platform/wifi_win.c`, `platform/media_win.c`
- [x] 1.3 Create `platform/win_compat.h` — `fork_exec` → `CreateProcess` shim, `ensure_executable_permission` → no-op, `read_file` → `ReadFile`/`MapViewOfFile`
- [x] 1.4 Modify `src/misc/helpers.h` — `#ifdef _WIN32` blocks: `fork_exec` uses `CreateProcess`, `ensure_executable_permission` is empty, `read_file` uses Win32 file I/O
- [x] 1.5 Modify `src/sketchybar.c` — `#ifdef _WIN32` for `main` vs `WinMain` entry, single-instance mutex (`CreateMutex("Local\git.felix.<name>")`)
- [x] 1.6 Verify: macOS Makefile unchanged (confirmed via git — no diff); Windows CMake builds portable core files without errors (no toolchain on dev machine — compile check established via `platform_compile_check` CMake target for CI; preprocessor guards statically verified balanced)

## S2: Graphics Seam + Visual Parity (PR 2)

- [ ] 2.1 Modify `src/context.c` — `#ifdef _WIN32`: `CGContextRef` = `struct skbar_context*` wrapping `SkCanvas`+`SkBitmap`; implement `context_create` with `SkCanvas::scale(scale,scale)`, `SkBitmap::allocPixels(BGRA,premul)`
- [ ] 2.2 Implement CG shim in `src/context.c` (Win32 path): `CGContextSaveGState/RestoreGState` → `SkCanvas::save/restore`; `CGContextClip` → `SkCanvas::clipPath`; `CGContextAddPath/DrawPath` → `SkCanvas::drawPath`; `SetRGBFillColor/StrokeColor/LineWidth` → `SkPaint`
- [ ] 2.3 Implement text shim: `CGContextSetTextPosition` + `CTLineDraw` → `SkCanvas::drawTextBlob`; map `CTFontGetAscent/Descent` → `SkFont::getMetrics`; validate glyph bounds match CoreText within tolerance
- [ ] 2.4 Verify blend mode: `SkBlendMode::kDstOut` maps to `clip_rect`'s `kCGBlendModeDestinationOut`; add `CGContextSetBlendMode` shim
- [ ] 2.5 Modify `src/font.c` — DirectWrite typeface via `SkTypeface::MakeFromName`; reuse `feature_mappings[]` table verbatim; map OpenType feature tags to Skia/HarfBuzz
- [ ] 2.6 Modify `src/image.c` — PNG/JPEG decode via `SkCodec`; `app.` icon via `SHGetFileInfo`/`IShellItemImageFactory`
- [ ] 2.7 Modify `src/color.c` — ensure `0xAARRGGBB` parsing feeds Skia `SkColor` correctly
- [ ] 2.8 Gate task: CoreText vs DirectWrite glyph bounds bitmap-diff must pass before proceeding — render 3 reference fonts with OpenType features, diff rendered bitmaps, assert per-pixel diff < 2%

## S3: Compositing Spike + Layered Window (PR 3)

- [ ] 3.1 Spike: create 2-3 layered `HWND`s with `WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE`, upload Skia bitmap via `UpdateLayeredWindow`, validate transparency/always-on-top/click-through/non-activation
- [ ] 3.2 Spike: add acrylic blur via `SetWindowCompositionAttribute` (ACCBENT) with `blur_radius` param; validate blur renders correctly on layered window
- [ ] 3.3 Create `platform/layer_dcomp.c` — replace `layer.m`: `IDCompositionVisual` wrapping `ID2D1Bitmap`/`D3D11` texture; `layer_set_contents`/`layer_set_bounds` set visual content/offset
- [ ] 3.4 Rewrite `src/window.c` — `WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE`; z-order via `SetWindowPos(HWND_TOPMOST)`; `windows_freeze/unfreeze` → `BeginDeferWindowPos`/`EndDeferWindowPos` batch
- [ ] 3.5 Rewrite `src/surface.c` — `surface_create/resize/flush`: upload `SkBitmap` to DirectComposition visual texture
- [ ] 3.6 Modify `window_set_blur_radius` — `SetWindowCompositionAttribute` backdrop acrylic with configurable radius
- [ ] 3.7 Implement `window_capture` — `BitBlt`/`CopyFromScreen` of the window rect (replaces `SLSCaptureWindowsContentsToRectWithOptions`)

## S4: Named-Pipe IPC (PR 4)

- [ ] 4.1 Rewrite `src/mach.c` — pipe server: `CreateNamedPipe("\\.\pipe\git.felix.<name>", PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE)`, server thread reads frames, posts `{buffer, MACH_MESSAGE}` to `event_post`
- [ ] 4.2 Rewrite `src/mach.c` — `mach_send_message`: open pipe, write NUL-separated argv frame (same as `client_send_message`), read response from `open_memstream` output; timeout=100ms matching macOS
- [ ] 4.3 Implement IPC frame validation: reject oversized frames (>64KB), malformed NUL separators, connection from non-same-session caller (security descriptor)
- [ ] 4.4 Modify `src/sketchybar.c` — lock file (`/tmp/<name>_<user>.lock` + `fcntl`) → named mutex `Local\git.felix.<name>` (`CreateMutex` + `GetLastError==ERROR_ALREADY_EXISTS`)
- [ ] 4.5 cmocka test: named-pipe round-trip, timeout, oversized frame rejection, non-session caller rejection

## S5: System Events + Message Pump (PR 5)

- [ ] 5.1 Create `platform/win_main.c` — `WinMain`: initialize COM, create hidden message-only `HWND`, `GetMessage`/`DispatchMessage` pump, `event_post` from worker threads → `PostMessage(registered_msg)`
- [ ] 5.2 Create `platform/win_events.c` — `WM_MOUSEMOVE/WM_LBUTTONUP/WM_MOUSEWHEEL` → hit-test via `bar_manager_get_item_by_point` → `event_post(MOUSE_*)`; `WM_DISPLAYCHANGE` → `event_post(DISPLAY_*)`; `WM_POWERBROADCAST` → `event_post(SYSTEM_WOKE/SYSTEM_WILL_SLEEP)`
- [ ] 5.3 Rewrite `src/animation.c` — 60Hz `SetTimer`/`QueryPerformanceCounter` tick replaces `CVDisplayLink`; `ANIMATOR_REFRESH` dispatched on timer
- [ ] 5.4 Rewrite `src/mouse.c` — remove Carbon kEventClassMouse; mouse events now routed from `platform/win_events.c` WM_* handlers
- [ ] 5.5 Rewrite `src/hotload.c` — `ReadDirectoryChangesW` thread watches config dir; 500ms rate limiter preserved; `fork_exec --reload` → `CreateProcess` with sh
- [ ] 5.6 Modify `src/event.c` — `event_post` uses `PostMessage(hwnd, WM_APP_EVENT, ...)` instead of `dispatch_async`; main pump dispatches `bar_manager_handle_*`
- [ ] 5.7 cmocka test: event dispatch, mouse hit-test, config dir change triggers HOTLOAD

## S6: System Info Adapters (PR 6)

- [ ] 6.1 Modify `src/volume.c` — CoreAudio `AudioObjectGetPropertyData` → `IAudioEndpointVolume` scalar + `SetMute`; IMM device notification for `VOLUME_CHANGED`
- [ ] 6.2 Modify `src/power.c` — IOKit `IOPSCopyPowerSourcesInfo` → `GetSystemPowerStatus` (`ACLineStatus`); `WM_POWERBROADCAST PBT_APMPOWERSTATUSCHANGE` for `POWER_SOURCE_CHANGED`
- [ ] 6.3 Create `platform/wifi_win.c` — WinRT `NetworkInformation` / `WlanApi` → `WIFI_CHANGED` with SSID
- [ ] 6.4 Create `platform/media_win.c` — SMTC `MediaPlaybackStatusChanged`/`MediaPropertyChanged` → `MEDIA_CHANGED`, `COVER_CHANGED`; extract artwork byte array
- [ ] 6.5 Modify `src/display.c` — DisplayServices → `WM_DISPLAYCHANGE` + `EnumDisplayMonitors` for `DISPLAY_ADDED/REMOVED/MOVED/RESIZED/CHANGED`; brightness via WMI `WmiMonitorBrightness`
- [ ] 6.6 Verify SMTC artwork parity: render `media.artwork` item, compare PNG output with macOS reference
- [ ] 6.7 cmocka test: volume query returns 0.0–1.0, power status valid, wifi SSID non-empty

## S7: Spaces/VD + Front-App + Config Semantics (PR 7)

- [ ] 7.1 Create `platform/workspace_win.c` — Windows 11 `IVirtualDesktopManager` COM: `GetWindowDesktopId`, `IsWindowOnCurrentVirtualDesktop`; register for `VirtualDesktopManagerInternal` change notifications → `SPACE_CHANGED`, `SPACE_WINDOWS_CHANGED`
- [ ] 7.2 Implement `space_capture(sid)` degradation: document no Windows equivalent for `SLSHWCaptureSpace`; `space.` icon items degrade to desktop id/label
- [ ] 7.3 Implement `--bar sticky` → pin to all virtual desktops via `IVirtualDesktopPinnedApps` where offered
- [ ] 7.4 Implement front-app detection: `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` → `APPLICATION_FRONT_SWITCHED`; taskbar auto-hide notify → `MENU_BAR_HIDDEN_CHANGED`
- [ ] 7.5 Modify `src/sketchybar.c` — Windows client path: connect named pipe, send `--set`/`--reload`/`--exit` commands; daemon path: message pump + all adapters
- [ ] 7.6 Implement config runner: `CreateProcess` with `sh.exe` (MSYS2/Git Bash) executing `sketchybarrc`; inject `SENDER`, `NAME`, `SELECTED`, `INFO` env vars; `--load-font` via `AddFontResourceEx`
- [ ] 7.7 Integrate hot-reload: `ReadDirectoryChangesW` → `HOTLOAD` event → re-exec config via `CreateProcess`
- [ ] 7.8 Modify `src/display.c` — `display_arrangement` via `EnumDisplayMonitors`/`GetMonitorInfo`; DPI via `GetDpiForWindow`
- [ ] 7.9 End-to-end: sketchybar starts with sample `sketchybarrc`, items render, IPC responds, config reload works
- [ ] 7.10 All cmocka unit tests pass: `ctest --test-dir build`
