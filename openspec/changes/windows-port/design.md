# Design: Native Windows Port (Pixel-Identical SketchyBar)

## Technical Approach

Add a second platform layer to the existing fork without touching the portable core. The entire macOS visual output flows through a single `CGBitmapContext` created in `context.c` (`context_create`) and consumed by every draw routine as `CGContextRef` (`text_draw`, `background_draw`, `image_draw`, `graph_draw`, `slider_draw`, `alias_draw`, popup). We replace that context with a Skia-backed canvas implementing the exact CGContext call surface the draw code uses, and swap the five thin platform adapters (`.m` files) plus `window.c`/`surface.c`/`layer.m`/`mach.c`/`mouse.c`/`hotload.c` for Windows modules. The core — `message.c` grammar, `event.c` dispatch table, `bar_manager.c`, `bar_item.c`, `misc/*` — compiles unchanged.

The strategy is **per-area platform adapters behind one graphics seam** (exploration finding #2), not a unified backend.

## Architecture Decisions

### Decision: Platform boundary shape
| Option | Tradeoff | Decision |
|---|---|---|
| Per-area shim | Each adapter mirrors one module's surface; core untouched | **Chosen** — matches exploration #5; review-friendly |
| Unified `win32.c` megamodule | Fewer files, one compiler unit | Rejected — no reviewable slices |

Boundary: `platform/*` compile unit per area. Portable core never `#include`s Apple or Win32 headers; each `platform/<area>.c` exposes the same `_begin/_stop/_forced/...` symbols the core already calls (e.g. `begin_receiving_volume_events`, `forced_volume_event`, `workspace_get_scale`).

### Decision: Rendering backend — Skia over Direct2D
| Option | Tradeoff | Decision |
|---|---|---|
| **Skia** | Same `SkBlendMode::kDstOut` blend the CG `clip_rect` uses (`kCGBlendModeDestinationOut`); mature `SkPath`/`SkRRect`/`SkCanvas` raster to an `SkBitmap` (premul BGRA = matches `kCGImageAlphaPremultipliedFirst\|kCGBitmapByteOrder32Host`); Typeface/OpenType feature support; CPU-only raster easy to golden-diff | **Chosen** — GPU (WGPU) parity is a needed spike, but Skia CPU matches the CG raster semantics directly |
| Direct2D + DirectWrite | Native, section compositing | **Not chosen** — D2D raster is DIP/section-oriented, not a single flat premul bitmap; pixel parity diffing is harder |

Skia draws into the same RGBA premultiplied bitmap that `CGBitmapContextCreate` produced, so `surface_flush` (`CGBitmapContextCreateImage`) becomes `SkBitmap::readPixels` → the compositor copies the same bytes. Coordinate space is identical: `context_create` scales CTM by `scale` (2.0) and we mirror that with `SkCanvas::scale(scale,scale)` + `kNone_SkFilterQuality` for `kCGInterpolationNone`; **font smoothing maps to a Skia typeface `forceAutoHinting`/subpixel flag driven by `g_bar_manager.font_smoothing`** (read from `context.c` line 24).

### Decision: Graphics context implementation
`src/context.c` is the only file that creates the context; every consumer only calls CG draw APIs. So we keep `context.h`'s `CGContextRef` typedef, but in the Windows build `CGContextRef` is an opaque `struct skbar_context*` wrapping `SkCanvas`+`SkBitmap`. Implement the exact CG surface the code uses:
- `CGContextSaveGState` / `RestoreGState` → `SkCanvas::save/restore`
- `CGContextClip`, `CGContextAddPath`, `CGContextDrawPath(kCGPathFillStroke)` → `SkCanvas::clipPath`/`drawPath` with `SkPaint::Style::kFill_Style`+`kStroke_Style`
- `CGPathAddRoundedRect`/`CGPathAddRect` → `SkRRect`/`SkRect`
- `CGContextSetRGBFillColor`/`StrokeColor`/`SetLineWidth` → `SkPaint` members; colors come from `color.c` floats (AARRGGBB already parsed) — 1:1
- `CGContextSetTextPosition` + `CTLineDraw` → `SkCanvas::drawTextBlob` at the CTM position (metric mapping in the font decision)
- `CGContextDrawImage` → `SkCanvas::drawImageRect`
- `clip_rect`'s `kCGBlendModeDestinationOut` → `SkBlendMode::kDstOut` (**blocker check** — validate against `SkPaint::BlendMode`)

Add `CGContextSetBlendMode`/`SetAllowsFontSmoothing`/`SetInterpolationQuality` to the shim. This is the smallest-risk spike: render one bitmap on both platforms and diff.

### Decision: Typography — DirectWrite via Skia typeface, reusing the feature map
| Option | Tradeoff | Decision |
|---|---|---|
| Skia + DirectWrite typeface (SkTypeface::MakeFromName on Win) | DirectWrite shaping is used only via Skia's HarfBuzz; OpenType feature selection passed through; best 1:1 | **Chosen** |
| Pure DirectWrite layout + custom draw | Full control but duplicates CG text positioning code | Rejected — reintroduces the seam we closed |

`font.c`'s `feature_mappings[]` table (OpenType tag → `kLigaturesType` etc., plus the `35/<selector>` ss01..ss20 encoding) is already portable C; port it verbatim. CoreText metrics (`CTFontGetAscent/Descent`, `CTLineGetBoundsWithOptions(kCTLineBoundsUseGlyphPathBounds)`) drive `text->bounds`/`text->width` sizing in `text.c` — must be reproduced from the DirectWrite glyph run's bounds via Skia (`SkFont::getBounds`/text blob bounds) so item widths/layout stay pixel-identical. **Risk HIGH — isolate in the bitmap-diff harness slice.**

### Decision: Compositing — layered window + DirectComposition (+ blur spike)
| Option | Tradeoff | Decision |
|---|---|---|
| Layered window + DirectComposition visual | Full compositor control; per-surface windows; DirectComposition visual carries the Skia bitmap; matches SLS surface/per-window model | **Chosen** |
| DWM `SetWindowCompositionAttribute` backdrop blur | Cheap but blur semantics differ | Rejected as primary; revisit for `blur_radius` only |

Map `window.c`:
- `SLSNewWindowWithOpaqueShapeAndContext`, `W_ABOVE`, `SLSOrderWindow`, `window_set_level`, `window_order` → per-surface `HWND` with `WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`; z-order via `SetWindowPos(HWND_TOPMOST)`; `kCGSPreventsActivationTagBit` → `WS_EX_NOACTIVATE` (activation prevention); click-through done in the mouse decision.
- `SLSSetWindowOpacity(0)` + transparent window → `WS_EX_LAYERED` with `SetLayeredWindowAttributes`/UpdateLayeredWindow from the premul bitmap (opaque window frame, transparent pixels = alpha 0).
- `window_set_blur_radius` (`SLSSetWindowBackgroundBlurRadius`) → `SetWindowCompositionAttribute` backdrop `/ AcrylicBackdrop` — **spike** (below).
- `window_capture` (`SLSCaptureWindowsContentsToRectWithOptions`) → BitBlt/CopyFromScreen of the same rect.
- `windows_freeze`/`windows_unfreeze` (`SLSTransaction*`) → a single `BeginDeferWindowPos`/`EndDeferWindowPos`/composition visual batch so moves/resizes are atomic like the SLS transaction.
- `g_space`/`SLSSpaceCreate`/sticky → virtual-desktop awareness (Decision: Spaces).
- `layer.m` (`CAContext`+`CALayer`) is replaced by the DirectComposition `IDCompositionVisual` wrapping the surface's `ID2D1Bitmap`/`D3D11` texture — `layer_set_contents`/`layer_set_bounds` become visual set-content/set-offset.

**Spike first (exploration reco #4)**: one layered HWND rendering an `Skia` bitmap + blur backdrop, validated for order/click-through/non-activation before the full compositor workstream.

### Decision: IPC — named pipes, transactional with response
| Option | Tradeoff | Decision |
|---|---|---|
| **Named pipes** (CreateNamedPipe `FILE_FLAG_FIRST_PIPE_INSTANCE` / ConnectNamedPipe / ReadFile / WriteFile) | Full-duplex request/response mirrors `mach_send_message(…, await_response)`; client is a normal CLI process; per-instance overlapped IO | **Chosen** |
| Window messages (`SendMessage`) | 1:1 to a hidden HWND but marshals differently, no natural response framing | Rejected |
| ALPC | Fast but undocumented/undesirable | Rejected |

The server name is derived from `g_name` exactly like `MACH_BS_NAME_FMT "git.felix.%s"` → `\\.\pipe\git.felix.<name>`. Two functions replace `mach.h`:
- `char* ipc_send(…, await_response)` — opens the pipe, writes the message bytes (the same NUL-separated argv frame `client_send_message` builds), reads the response. Response is `open_memstream` output from `handle_message_mach` — the `message.c` grammar works **unchanged**.
- A pipe server thread posts `{ buffer, MACH_MESSAGE }` to `event_post` (same `handle_message_mach` entry). `event_mach_message` in `event.c` is untouched.

The lock file `g_lock_file` (`/tmp/<name>_<user>.lock`, `fcntl F_SETLK`) → a single-instance named mutex `Local\git.felix.<name>` (`CreateMutex`).

### Decision: Event model — producer swap, dispatch unchanged
`event.c`'s table (27 entries incl. `INIT_MUTEX`) stays. Each producer becomes a Windows callback posting the same `struct event`:
| macOS producer | Windows adapter | event type(s) |
|---|---|---|
| `FSEventStream` (`hotload.c`) | `ReadDirectoryChangesW` thread → `event_post(HOTLOAD)` (keep the 500ms rate limiter logic) | `HOTLOAD` |
| Carbon mouse (`mouse.c`) | `WM_MOUSEMOVE/WM_LBUTTONUP/WM_MOUSEWHEEL` hit-tested by `bar_manager_get_item_by_point`; scroll deltas copied | `MOUSE_UP/DRAGGED/ENTERED/EXITED/SCROLLED` |
| MediaRemote (`media.m`) | SMTC (`SystemMediaTransportControls` events + `Windows.Media` WinRT) | `MEDIA_CHANGED`, `COVER_CHANGED` |
| CoreWLAN/SCDynamicStore (`wifi.m`) | WinRT `NetworkInformation` / `INetworkListManager` | `WIFI_CHANGED` |
| CoreAudio (`volume.c`) | `IAudioEndpointVolume` (`IAudioEndpointVolumeCallback`) + IMM device notification | `VOLUME_CHANGED` |
| IOKit (`power.c`) | `GetSystemPowerStatus` + `WM_POWERBROADCAST` `PBT_APMPOWERSTATUSCHANGE` | `POWER_SOURCE_CHANGED` |
| DisplayServices (`display.c`) | `WM_DISPLAYCHANGE`, `EnumDisplayMonitors`, `RegisterDeviceNotification` | `DISPLAY_ADDED/REMOVED/MOVED/RESIZED/CHANGED` |
| NSWorkspace notifications (`workspace.m`) | `GetForegroundWindow`/`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`; taskbar auto-hide notify | `APPLICATION_FRONT_SWITCHED`, `MENU_BAR_HIDDEN_CHANGED`, `SYSTEM_WOKE`, `SYSTEM_WILL_SLEEP` |
| CVDisplayLink (`animation.c`) | 60Hz `SetTimer`/threadpool `QueryPerformanceCounter` tick | `ANIMATOR_REFRESH` |
| `event_post` GCD main-queue (`pthread_main_np`+`dispatch_sync`) | a single UI-thread message pump (`GetMessage`/`DispatchMessage`); `event_post` from worker threads → `PostMessage` a registered window msg, pump executes handler | all |

### Decision: Spaces scope — virtual-desktop aware (committed)
Windows 11 `IVirtualDesktopManager` (`GetWindowDesktopId`, `IsWindowOnCurrentVirtualDesktop`) + `VirtualDesktopManagerInternal` (COM) for change notifications → `SPACE_CHANGED`, `SPACE_WINDOWS_CHANGED`. `space_capture(sid)` (`SLSHWCaptureSpace`) in `image.c` has **no Windows equivalent** — document as a limitation in the spec; `space.` icon items degrade to desktop id/label. `--bar sticky` + `topmost` → `pinned to all virtual desktops` (IVirtualDesktopPinnedApps) where offered.

### Decision: System info adapters
| Info | macOS API | Windows API |
|---|---|---|
| volume/mute | CoreAudio 4x `AudioObjectPropertyAddress` | `IAudioEndpointVolume` scalar+`SetMute`; main+left stereo |
| power source | `IOPSCopyPowerSourcesInfo` | `GetSystemPowerStatus` (`ACLineStatus`) |
| wifi SSID | CoreWLAN `ssidData` | WinRT `WlanApi` `WLAN_AVAILABLE_NETWORK.ssid` |
| media now-playing | MediaRemote | SMTC `MediaPlaybackStatusChanged`/`MediaPropertyChanged` + `Windows.Media` |
| brightness | `DisplayServicesGetBrightness` | WMI `WmiMonitorBrightness` / `SetMonitorBrightness` |
| display scale | `workspace_get_scale` (`backingScaleFactor`) | `GetDpiForWindow`/`GetDpiForSystem` |
| top inset / notch | `NSScreen visibleFrame`, `safeAreaInsets.top` | **No menu-bar strip** — top inset = 0; notch = config-driven inset only (`display_height`, `notch_width/offset`) |

### Decision: Config & plugins — keep POSIX sh, vendor it
Preserve `sketchybarrc` semantics exactly: it is a POSIX shell script (bash arrays, `$CONFIG_DIR`, `${default[@]}`). Require/package a POSIX `sh` (MSYS2 bash or Git Bash) and launch it with `CreateProcess` instead of `fork_exec` (helpers.h line 454). `ensure_executable_permission` (line 427) → no-op on Windows (NTFS has no exec bit). `--load-font` uses `AddFontResourceEx`(private) instead of `CTFontManagerRegisterFontsForURL`. Keep the full `message.c` grammar as the contract; a **native parser is out of scope** (documented as a possible later slice). `--reload`/`--hotload` → `CreateProcess` the same sh with `--reload`, repost `HOTLOAD`.

### Decision: Build system — CMake + clang-cl (or Mingw-w64), single x64 target
| Option | Tradeoff | Decision |
|---|---|---|
| **CMake + clang-cl** | Same flags as Makefile (`-std=c99 -Wall -O3`); clean option toggling; Skia SDK-friendly | **Chosen** |
| MSVC native | Skia works but D2D/Skia config friction | Rejected |
| Plain Makefile (MinGW) | Keeps `make`, but no universal binary and poor Skia dep mgmt | Chosen only if clang-cl unavailable |

Drop `lipo` (single x64; ARM64 later). Replace `-framework Carbon…` with Skia libs + `dwmapi.lib`, `dcomp.lib`, `d2d1.lib`, `dxgi.lib`, `Winmm.lib`, `Setupapi.lib`. The six `.m` files are re-implemented as C modules (below).

## File Changes (Windows platform tree)

| File | Action | Description |
|---|---|---|
| `src/context.c/.h` | Modify | `CGContextRef` = `struct skbar_context*` (Skia) in Windows build; same create/CTM/interpolation/font-smoothing contract |
| `src/mach.c/.h` | Rewrite | named-pipe IPC (above); same send/await-response API (`mach_send_message`) |
| `src/window.c/.h` | Rewrite | layered HWND + DirectComposition visual; `windows_freeze/allow` = DeferWindowPos batch; sticky→VD |
| `src/surface.c/.h` | Rewrite | `surface_create/resize/flush` upload Skia bitmap to DC/D3D texture |
| `src/layer.m/.h` | Replace | `platform/layer_dcomp.c` (IDCompositionVisual) |
| `src/mouse.c/.h` | Rewrite | WM_* → event_post (hit-test logic stays in `bar_manager`) |
| `src/hotload.c/.h` | Rewrite | ReadDirectoryChangesW + sh CreateProcess |
| `src/display_nsscreen.m` | Replace | `platform/display_win.c` (EnumDisplayMonitors, DPI) |
| `src/workspace.m` | Replace | `platform/workspace_win.c` (scale, VD events, front app, sleep/wake, menu-bar) |
| `src/wifi.m` | Replace | `platform/wifi_win.c` |
| `src/media.m` | Replace | `platform/media_win.c` (SMTC) |
| `src/volume.c` | Modify | swap CoreAudio → IAudioEndpointVolume |
| `src/power.c` | Modify | swap IOKit → GetSystemPowerStatus |
| `src/display.c` | Modify | swap DisplayServices → WM_DISPLAYCHANGE |
| `src/font.c`, `src/color.c`, `src/animation.c` | Modify | swap CT/CG → Skia/DirectWrite equivalents; keep tables/logic |
| `src/sketchybar.c` | Modify | client/daemon split; single-instance mutex; WinMain message pump |
| `src/image.c` | Modify | PNG/JPEG decode → SkCodec; `app.` icon → SHGetFileInfo/IShellItemImageFactory |
| `src/text.c`, `src/background.c`, `src/graph.c`, `src/slider.c`, `src/image.c`, `src/alias.c`, `src/popup.c`, `src/shadow.c` | Unchanged in logic | draw via the `CGContextRef` shim (Skia) |
| `src/misc/helpers.h` | Modify | `fork_exec`→CreateProcess, `ensure_executable_permission`→no-op, `read_file` mmap→Win read |
| `CMakeLists.txt` | Create | replaces `makefile` |
| `platform/win_main.c`, `platform/win_events.c` | Create | message pump; WM→event routing |

Portable core **untouched**: `message.c`, `event.c`, `bar_manager.c`, `bar_item.c`, `bar.c`, `group.c`, `custom_events.c`, `misc/defines.h`, `misc/env_vars.h`, `misc/help.h`.

## Interfaces / Contracts

`context.h` (unchanged contract, Win implementation):
```c
typedef struct skbar_context* CGContextRef;   /* Windows build */
CGContextRef context_create(CGSize size, CGFloat scale);
/* draw API: SaveGState, Clip, AddPath, DrawPath, SetRGBFill/StrokeColor,
   SetLineWidth, SetTextPosition, DrawImage, clip_rect, SetBlendMode,
   SetAllowsFontSmoothing, SetInterpolationQuality */
```

`mach.h` (unchanged API, Win impl):
```c
char* mach_send_message(mach_port_t port, char* msg, uint32_t len, bool await_response);
bool mach_server_begin(struct mach_server*, mach_handler);   /* pipe server thread */
```

## Testing Strategy

| Layer | What | Approach |
|---|---|---|
| Unit (core) | `message.c` grammar, `event.c` dispatch, `bar_item` parsing, `color/font feature` table | **cmocka** (C99, no deps, Windows+macOS) — target portable core only |
| Visual parity | golden bitmap per item type | **bitmap-diff harness**: render known `sketchybarrc` fonts/items via the Skia backend in `context.c`-style pipeline on both platforms, write PNG, diff pixels. Text is the priority (CoreText vs DirectWrite glyph bounds). |
| IPC | named-pipe client↔server round-trip, timeout, malformed frame | cmocka over the `ipc_send` seam |
| Compositor | window order, click-through, non-activation, blur | spike harness (2–3 layered windows) before full port |

**Choose cmocka** (small, mirrors upstream's no-deps style). Bitmap diffs are generated goldens (excluded from authored review count under the 400-line budget).

## Threat Matrix

Applicable — this change bridges to OS process/subprocess and IPC boundaries that must be hardened.
| Boundary | Min adversarial cases | Applicability | Design response | Planned RED tests |
|---|---|---|---|---|
| Documentation-like paths | `sketchybarrc` / `plugins/*.sh` executed | **Applicable** — sh is exec'd via CreateProcess | Only exec files matching the resolved config/plugin dir; reject `..`/absolute escapes | Test: config path traversal blocked |
| Git repository selection | n/a | N/A — no git calls in runtime | | |
| Commit state | n/a | N/A | | |
| Push state | n/a | N/A | | |
| PR commands | n/a | N/A | | |
| IPC boundary | untrusted client connects to pipe | **Applicable** — named pipe is a local attack surface | Validate caller via same-session/security descriptor; frame length caps; timeout; reject >max buffer | Test: oversized frame, bad handle, timeout |

## Migration / Rollout
No data migration. Rollout is chained PR slices (below); each slice adds one Windows adapter while macOS build stays green via `#ifdef`/separate platform dir.

## Phasing / Slices (auto-chain, 400-line budget)

Reviewable, independent chained PR slices — each has clear start/finish, owns its verification, and lands behind its own branch on the feature chain:
1. **S1 Build skeleton** — CMakeLists + `platform/` tree + compiler shims (`fork_exec`→CreateProcess, exec no-op, `read_file`) ; macOS unaffected. Small.
2. **S2 Graphics seam** — Skia `skbar_context` + `context.c` swap + pixel-diff harness + text/font metric port. **The visual-parity anchors.**
3. **S3 Compositing spike + layered window** — 2–3 layered HWNDs + DirectComposition + blur, validated independently (reco #4). Highest risk; isolated.
4. **S4 IPC** — named-pipe server/client + `mach_send_message` swap + single-instance mutex.
5. **S5 Events (system)** — message pump, WM_* mouse, WM_DISPLAYCHANGE, hotload + sh exec.
6. **S6 System info adapters** — volume, power, wifi, media/SMTC, brightness (each small, can be one or split).
7. **S7 Spaces/VD + front-app + config semantics** — virtual-desktop events, menu-bar/taskbar, front-app, final `sketchybarrc` integration.

Each lands green on Windows and macOS; reviewer sees bounded, single-adapter diffs.

## Open Questions
- [ ] Does `kCGBlendModeDestinationOut` (`clip_rect`) map exactly to `SkBlendMode::kDstOut`? (blocking S2)
- [ ] CoreText vs DirectWrite glyph bounds for item width/layout — exactness must be validated, not assumed (S2 gate).
- [ ] Virtual-desktop change notifications: public `IVirtualDesktopManager` only exposes `IsWindowOnCurrentVirtualDesktop`; the `VirtualDesktopManagerInternal` COM service is undocumented — scope `space_windows_change` fidelity accordingly (S7).
- [ ] SMTC artwork + `media.artwork` image path parity (S6).
