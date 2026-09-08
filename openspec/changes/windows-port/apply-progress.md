# S2 Apply Progress — Graphics Seam + Visual Parity

Change: `windows-port` · Slice: S2 · Branch: `feat/windows-port-s2`
Status: **IMPLEMENTED — parity gate FAIL-CLOSED until a Skia SDK is configured**

## What this slice delivers

The portable drawing core (`src/context.c`, `src/font.c`, `src/image.c`, `src/color.c`)
now compiles on Windows against a **Skia graphics seam**:

```
src/context.c (Win branch) ─► CG-named shims (in context.c) ─► sk_* surface
                                                                  │
                              platform/win_graphics_types.h ◄─────┘ (types/constants)
                              platform/sk_backend.h                (seam contract)
                              platform/sk_backend_common.c          (CF/CG/CT glue)
                              platform/sk_backend_skia.cpp          (real Skia, SKIA_DIR-gated)
                              platform/sk_backend_stub.c            (fail-closed stub)
```

- `win_graphics_types.h` — full Windows mirror of the CG/CF/CT type + constant surface
  the portable core headers reference (blend modes, drawing modes, feature constants,
  event masks, `CGFloat`/`CGPoint`/`CGSize`/`CGRect`, opaques, `pid_t`/`mach_port_t`).
- `sk_backend.h` — the seam contract: opaque `skbar_*` objects with a common
  `{magic, kind}` header (drives the `CFRelease` dispatch), plus the sk_* surface.
- `sk_backend_common.c` — backend-agnostic glue: `CFRelease` dispatch, `CGImageRelease`,
  `CGPathRelease`, `CFDataGetLength/GetBytePtr`, `CGImageGetWidth/Height/CreateCopy/
  GetDataProvider`, `CGDataProviderCopyData`, `CTLineDraw/GetBoundsWithOptions/
  GetTypographicBounds`, `CTFontGetAscent/Descent`.
- `sk_backend_skia.cpp` — real backend: SkBitmap BGRA-premul surface, CG coordinate
  model (Y-flip + scale via `SkMatrix::MakeAll(scale,0,0, 0,-scale,height)`), SkShaper
  feature shaping, `IShellItemImageFactory` app icons, PNG encode via
  `SkImages::DeferredFromEncodedData`/`encodeToData`. **Compile-gated on SKIA_DIR.**
- `sk_backend_stub.c` — fail-closed stub: every creation returns NULL; the harness
  exits 2 with a documented message.
- Portable-core edits are **additive `#ifdef _WIN32` only** (verified: `git diff` on
  `src/` has 0 removed lines; `makefile` untouched — macOS paths byte-identical).
- `bitmap_diff` harness + ctest: 3 reference cases (Arial `liga,tnum`; Times New Roman
  `liga,onum`; Courier New `zero,salt`), two-render determinism on Windows,
  per-pixel diff tolerance (12/255 per channel, >2% differing pixels fails).
- CI (`windows-build.yml`) builds `graphics_seam_check` + `bitmap_diff` alongside the
  existing compile check; ctest is intentionally not run in CI (fail-closed gate).

## Build evidence (local, stub backend)

Configure + all targets compile clean under clang-cl 23.1.0 (only CRT deprecation
warnings from `getenv`/`strtok`):

- `graphics_seam_check.lib` — builds (7/7)
- `bitmap_diff.exe` — builds (4/4), runs:
  - `--check --case all` → exit **2**, message: `skia backend unavailable (SKIA_DIR not configured); parity gate FAIL-CLOSED`
  - `--render --case arial` → exit **2** (same fail-closed path)
- `sketchybar.exe` — full target still links (regression check)
- `ctest --test-dir build/s2-check -R bitmap_diff` → expected fail, exit 8

Full transcript: `build/s2-check/build-evidence.txt`
Evidence sha256: `865DA7EE2DE0F373158CCFB53D15B930E8887D29992F566ED37B0C012E877481`

## Task status

2.1–2.8 all implemented per `tasks.md` (checked with acceptance notes inline).

## Deviations from design (all documented in code comments)

| Design said | Did / deviation |
|---|---|
| `SkCanvas::scale(scale, scale)` | Y-flip + scale matrix (CG origin bottom-left); PNG export stays top-down; `sk_context_read_pixels`/`sk_image_read_pixels` export CG order (row 0 == bottom) so buffers memcmp against `CGBitmapContextCreate` |
| feature_mappings moved to seam | Kept verbatim in `font.c` on both platforms; `get_opentype_tag()` reverse-maps numeric pairs; first-match wins → `(35,2)` resolves to `"salt"` (matches macOS descriptor behavior) |
| `kCGBlendModePlusDarker` | Mapped to `SkBlendMode::kPlus` (no exact Skia analog; surfaced by harness) |
| CG `AddPath` appends | Seam stores the most recently added path (covers every portable-core call site) |
| CG `DrawPath` FillStroke | Single-pass `kStrokeAndFill` (CG composites fill then stroke; documented approximation) |
| `CFRelease(NULL)` | NULL-safe on Windows (deliberate deviation: macOS would crash) |
| `CGImageCreateCopy` | `sk_image_retain` (refcount bump, same pointer; documented deep-copy deviation) |
| `CTLineGetTypographicBounds` | Approximation from blob bounds (S3 revisit listed in tasks) |
| `space.` image items | Explicitly fail with a message (no Windows equivalent; S7 degradation task 7.2) |
| `color.c` | **No code change** — `0xAARRGGBB` == `SkColor` by construction (verified `color_init`/`color_set_hex` feed the same uint32; unit asserted in `sk_context_set_fill_rgba`) |

## Known constraints / risks

1. **No Skia SDK available at S2 time.** `sk_backend_skia.cpp` is written against the
   long-lived Skia core+shaper API but is **compile-gated on SKIA_DIR** — it has not
   been compile-verified. Verified on a machine with `SKIA_DIR` configured; API drift
   there is a `size:exception`-level follow-up, not a silent regression.
2. **Parity gate cannot pass without the SDK** by design (fail-closed, honest). The
   golden-vs-Skia bitmap diff requires a macOS run to produce `*_macos.png` goldens
   and a SKIA_DIR Windows run to check them.
3. S3/S4 work (link target globals: `g_bar_manager`, media events) still unresolved in
   the static/lib seam — by design the seam is a static library tolerating undefined
   refs until those adapters land; `bitmap_diff` deliberately links backend + main only.
4. `sk_icon_for_app` uses `IShellItemImageFactory` (shell32/ole32 added to link libs);
   scale is 1.0 until S7 `workspace_get_scale`.

## Deliverables

- 4 work-unit commits on `feat/windows-port-s2` (no push):
  1. `0803d97` seam surface + types + common glue + stub + CMake + gitignore
  2. `657da9e` Skia backend (SKIA_DIR-gated)
  3. `ba1e42f` portable-core `_WIN32` branches
  4. `0b29d8e` parity harness + ctest + CI wiring
- Untracked (deliberately NOT committed): `.atl/`, `openspec/changes/windows-port/proposal.md`, `openspec/changes/windows-port/specs/` — flag for the archive slice.
- Line budget: ~1.4k LOC across the slice vs the 400-line budget → report honestly,
  recommend `size:exception`.

---

# S2f — Skia SDK verification (bound sha256 `865DA7EE2DE0F373158CCFB53D15B930E8887D29992F566ED37B0C012E877481`)

Status: **VERIFIED — real Skia backend compiles, links, and passes the renderability gate**

## SDK

- Aseprite/skia `m124-08a5439a6b` Windows x64 Release (private-master build):
  `https://github.com/aseprite/skia/releases/download/m124-08a5439a6b/Skia-Windows-Release-x64.zip`
- Zip SHA-256: `5A371A4B2819BB4EB96E36CD75FA623585E1D5477E253A970302B6F2471B6934`
- Vendored at `third_party/skia/` (130 MB, 1972 files; gitignored except README.md manifest).
- 20 static libs in `out/Release-x64/`; built with `extra_cflags=["-MT"]` → targets must
  link with `/MT` (`CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded"` when Skia is found).

## Verified m124 API drift (previously never compile-checked)

| Backend code (pre-m124 API) | m124 reality | Fix |
|---|---|---|
| `#include "include/core/SkEncodedImageFormat.h"` | file does not exist | removed |
| `SkTypeface::MakeFromName(...)` | removed from m124 | `SkFontMgr_New_DirectWrite()` + `matchFamilyStyle` (`include/ports/SkTypeface_win.h`) |
| `SkFontMgr::RefDefault()` | does not exist | n/a (DirectWrite factory path) |
| `snapshot->encodeToData(SkEncodedImageFormat::kPNG, 100)` | removed | `SkPngEncoder::Encode(&stream, pm, SkPngEncoder::Options{})` (`include/encode/SkPngEncoder.h`) — note m124 has **no default Options arg** |
| `shaper->shape(...)` returning `bool` | returns `void` in m124 | success proven via `makeBlob()`; drop `if (!shaped)` |
| feature-taking `shape` (font+features form) | does not exist | full run-iterator form (`TrivialFontRunIterator/BiDi/Script/Language`) |
| `struct DrawState` | collides with Win32 `DrawStateA` macro (winuser.h) | renamed `SKBarDrawState` |
| `sk_sp<SkPath>` | SkPath is a value type | `SkPath` + `has_current_path` bool |
| `drawImageRect(..., constraint?)` 5-arg | 5-arg overload removed | pass `SkCanvas::SrcRectConstraint::kFast_SrcRectConstraint` |
| `drawTextBlob(blob, x, y)` 3-arg | removed | pass explicit paint |
| `SkFontStyle(int,int,int)` | 3rd arg is `Slant` enum | type slant as `SkFontStyle::Slant` |
| `kCGPathEOFill`/`kCGPathEOFillStroke` | not in `win_graphics_types.h` | header names them `kCGPathEvenOddFill(Stroke)` |
| `kCGPathStroke` | missing from the enum entirely | added `kCGPathStroke = 4` (additive, documented) |
| `SHCreateItemFromParsingName(const char*)` | takes `PCWSTR` only | UTF-8→UTF-16 via `MultiByteToWideChar` |
| `BHID_SFUIObject` | defined in `um/ShlGuid.h`, not shobjidl_core.h | `#include <shlguid.h>` |
| freetype2.lib wants plain `inflate` | SDK zlib is Chromium-prefixed (`Cr_z_*`) | `platform/skia_zlib_shim.c` forwards the 4 needed names to `Cr_z_*` |

`SkTextBlobBuilderRunHandler` verified at `modules/skshaper/include/SkShaper.h:278` — the
existing `SkShaper.h` include is sufficient (no drift).

## Build evidence (clang-cl 23.1.0, MSVC 14.44.35207, Win SDK 10.0.26100.0)

1. `cmake --build build/s2f --target bitmap_diff` → clean compile + link:
   - `bitmap_diff.exe` (5.6 MB) — Skia backend + skshaper + harfbuzz + freetype + zlib shim.
2. `bitmap_diff.exe --render --case all` → **exit 0, PASS**:
   - `determinism OK for 'arial'`, `'times'`, `'courier'` (245760 bytes each, two renders identical)
   - golden PNGs are the macOS-side artifact; Windows `--render` is determinism-only by harness design.
3. `bitmap_diff.exe --check --case all` → **exit 3** `golden file missing: arial_macos.png`
   (expected: goldens require a macOS render run; gate is fail-closed without them).
4. `ctest --test-dir build/s2f -R bitmap_diff` → fails with the same exit-3 golden-gap (recorded, not a regression).
5. `cmake --build build/s2f` (all targets) → `graphics_seam_check.lib`, `sketchybar.exe` (111 KB),
   `platform_compile_check.lib` all build with the real backend.
6. `sketchybar.exe` smoke run → starts, exits rc=0 cleanly (no crash).
7. ICU data: SDK ships `third_party/externals/icu/flutter/icudtl.dat`; post-build copy next to
   each Skia-backed exe (SkIcuLoader resolves it relative to the executable).

Full transcript: `build/s2f/s2f-evidence.txt` (sha256 `B3B5555F35CDDC86464358B76624157AD469AEAE5230A2FFD3D3BC83E886FD1D`)

## Bound remediation

- Every unverified API assumption recorded in the S2 evidence (`build/s2-check/build-evidence.txt`)
  is now compile/link/run-verified against the vendored m124 SDK (table above).
- Remaining honest gap: cross-platform **pixel parity** needs `*_macos.png` goldens from a
  macOS run (no macOS hardware in S2f). Renderability + determinism are proven on Windows.
- `size:exception` stands (drift fixes + shim are additive, documented).

## S2f artifacts

- `third_party/skia/README.md` — SDK provenance manifest (committed; SDK itself gitignored).
- `platform/skia_zlib_shim.c` — zlib name forwarding (committed).
- `platform/sk_backend_skia.cpp` — drift fixes (all commented `m124 drift:`).
- `platform/win_graphics_types.h` — `kCGPathStroke` added.
- `CMakeLists.txt` — SDK discovery, `/MT`, NOMINMAX, ICU data copy, shim source.

## S3: Compositing spike + layered window (PR 3)

Executed on `feat/windows-port-s3` (parent `7411436` = S2 tip). Commits:

- `8df30dc` feat(S3): add snapshot and BGRA image wrap to graphics seam
- `f0b1edb` feat(S3): integrate DirectComposition layer and compositor spike
- `209c3e8` feat(S3): add Windows compositor paths to window.c
- `bfdaf96` feat(S3): add Windows surface paths for DirectComposition upload

### Design decisions

- **Two display paths in S3**: `surface_flush` uploads the canvas snapshot to the
  DirectComposition layer (the compositor path); `window_flush` ALSO displays the
  same pixels via `UpdateLayeredWindow` (the production layered-window path). The
  DComp visual tree is NOT attached to an HWND target in S3 — attaching the root
  visual (`IDCompositionTarget::SetRoot`) is the S7 hardware-composition follow-up.
  Both paths are exercised by the spike.
- **Seam pixel order proven**: `sk_context_read_pixels` exports CG user-space order
  (row 0 == BOTTOM). A positive-height DIB is also bottom-up, so the ULW upload is
  a byte-for-byte memcpy (no flip) — proven by the spike's asymmetric bands
  (blue bottom / white middle / red top on an 80x20 pt canvas) with CPU-side row
  checks on the raw buffer.
- **dcomp.h requires C++**: the Windows SDK's `dcomp.h` declares overloaded
  `STDMETHOD`s (`IDCompositionScaleTransform::SetScaleX(float)` +
  `SetScaleX(IDCompositionAnimation*)`) which are legal only in C++. `layer_dcomp.c`
  is therefore compiled as CXX (`set_source_files_properties LANGUAGE CXX`); the
  seam's `extern "C"` guards keep link compatibility, and `layer.h` gained the
  matching `extern "C"` block.
- **Monotonic window id**: `window->id` is a monotonic counter on Windows (not a
  truncated HWND); the real handle lives in the new `window->hwnd` member
  (`src/window.h`, `_WIN32`-only).
- **`SetWindowCompositionAttribute` real signature**: `WINDOWCOMPOSITIONATTRIBDATA`
  wrapper (Attribute/Data/SizeOfData) AROUND the accent policy; passing the policy
  flat sets Attribute == AccentState and fails.
- **Corrected earlier spike draft**: the leftover `tests/spike_compositor.c` had two
  real bugs — a top-down DIB comment that would render the bar vertically flipped,
  and a 3-argument `CHECK(fn(hwnd,&data),...)` macro misuse that never compiled.
  The committed spike is a rewrite.

### Honest gaps (documented, by design)

- **blur radius approximation (task 3.6)**: Windows exposes NO per-window blur
  radius API. The radius value is carried in the acrylic policy's `AccentFlags`
  slot (best effort). Visual quality of the approximation requires an
  interactive-desktop check (spike ran PASSED on the dev desktop; radius tuning is
  an S7 polish item).
- **Click-through parity (task 3.1)**: spike validates layered/tool/no-activate
  styles, topmost, and non-activation. `WS_EX_TRANSPARENT` (mouse pass-through)
  is deliberately deferred to the S5 mouse adapter, which owns hit-testing.
- **Capture DIB alpha (task 3.7)**: the DIB alpha channel from BitBlt is treated
  as opaque (alpha-forced) when wrapping into the seam image — the SLS capture
  path returned the rendered surface premultiplied; desktop composition does not
  carry window alpha into a screen DC capture.
- **No desktop in CI**: `compositor_spike` self-detects (CreateWindowExW == NULL →
  SKIP desktop checks, exit 0); `--strict` demands a desktop (exit 1 otherwise,
  used for the interactive validation run). Documented in the spike header.

### Build evidence (clang-cl 23.1.0, Win SDK 10.0.26100.0, Skia m124)

1. `cmake -S . -B build/s3 -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   -DCMAKE_BUILD_TYPE=Release -DSKIA_DIR=third_party/skia` → configure OK, Skia SDK found (20 libs).
2. `cmake --build build/s3` → `sketchybar.exe`, `compositor_seam_check.lib`, `compositor_spike.exe` build clean.
3. `compositor_spike.exe` → **16/16 PASS, exit 0** (interactive desktop present):
   ex-style constants, class registration, user32 proc resolution, seam dims +
   CG-order layout, layered/tool/no-activate styles, ULW upload, topmost,
   non-activation, acrylic enable + disable, `sk_image_from_bgra` wrap, DComp
   layer round-trip.
4. `ctest --test-dir build/s3 -R compositor_spike` → **Passed** (0.20 s).
5. `sketchybar.exe` smoke → **rc=0** (S1 entry-point contract preserved).
6. `bitmap_diff --render --case all` → **PASS, exit 0** (S2 regression preserved).
7. `git diff --stat makefile` → **empty** (macOS build untouched).

### Outcome

- S3 implemented, verified locally, committed in 4 work units (+ this doc).
- **Budget overage**: ~1,152 changed lines vs the 800-line budget (782 tracked
  insertions + ~370 spike). Per protocol the settle was NOT requested; the split
  proposal follows below.
- Suggested next step: reviewer/owner decision between accepting the overage or
  splitting S3a (3.1–3.3: spike + layer_dcomp + seam) from S3b (3.4–3.7:
  window.c/surface.c) — both halves are independently buildable commits already.

---

# S4 — Named-Pipe IPC (PR 4)

Change: `windows-port` · Slice: S4 · Branch: `feat/windows-port-s4`
Status: **IMPLEMENTED — cmocka suite passes locally, CI wired**

## What this slice delivers

Named-pipe IPC replaces Mach ports for cross-process communication on Windows:

```
src/mach.c (_WIN32 path) ─► CreateNamedPipe("\\.\pipe\git.felix.<name>")
    │                           │
    │  server thread            │  ConnectNamedPipe → ReadFile (frame)
    │  posts {buf,MACH_MESSAGE} │  to event_post
    │                           │
    ├──► mach_send_message ─────┤  port==NULL→NULL, INVALID_HANDLE→client
    │   (NUL-separated argv     │  (WriteFile+FlushFileBuffers, 100ms read)
    │    frame, same as macOS)  │
    │                           │
    platform/win_ipc.h ◄────────┘  seam: ipc_frame_valid, ipc_sessions_match,
                                   ipc_create_security_descriptor,
                                   ipc_client_is_same_session, ipc_pipe_name
```

- **Pipe server** (task 4.1): `CreateNamedPipe(PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE)`,
  `PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_REJECT_REMOTE_CLIENTS`. Server thread
  reads NUL-separated frames, posts `{buffer, MACH_MESSAGE}` to `event_post`. Single-instance
  with `CloseHandle` + recreate on client disconnect.
- **Client send** (task 4.2): `mach_send_message` three-way dispatch:
  `port==NULL→NULL` (parity with macOS), `port==INVALID_HANDLE_VALUE→client` (open pipe,
  `WriteFile`+`FlushFileBuffers`, 100ms byte-mode `await_response`, timeout→`""` via
  `malloc(1)`), real handle→write-response-only (`await_response→NULL`).
- **Frame validation** (task 4.3): `ipc_frame_valid` rejects `>64KB` and malformed NUL
  separators. 3-layer session security: DACL (SYSTEM + current user + logon session SID),
  `PIPE_REJECT_REMOTE_CLIENTS`, post-connect `ipc_client_is_same_session` via
  `GetNamedPipeClientProcessId`→`OpenProcess`→`OpenProcessToken`→`TokenSessionId` vs
  `ProcessIdToSessionId`.
- **Single-instance mutex** (task 4.4): `acquire_lockfile` Windows path:
  `SetLastError(0)` → `CreateMutexA("Local\\git.felix.<name>")` → `GetLastError()` →
  `CloseHandle` on `ERROR_ALREADY_EXISTS`.
- **cmocka test suite** (task 4.5): 6 cases in `tests/ipc_pipe_test.c`:
  round-trip echo, timeout 300ms→`""`, oversized 100KB wire + "later requests unaffected",
  NUL-malformed frames, frame unit validation, session guard with DACL introspection via
  `ConvertSidToStringSidA`. Shim (`tests/cmocka_shim.{h,c}`) under `SKBAR_CMOCKA_SHIM`
  for CI without cmocka installed. CTest target `ipc_pipe` wired. Unique `g_name` per test.

## Build evidence

- Branch `feat/windows-port-s4`, commit `843ae49` (964 insertions, 2 deletions, 9 files).
- Toolchain: clang-cl 23.1.0, ninja 1.13.2, cmake 4.4.3.
- CI (`windows-build.yml`): `ipc_seam_check` + `ipc_pipe` targets added; ctest wired.
- Local build: `cmake --build build/s4 --target ipc_seam_check ipc_pipe` compiles clean.
- `ctest --test-dir build/s4 -R ipc_pipe`: 6/6 PASS.

### Re-verification note (apply re-run, 2026-09-07) — FLAKY, not green

Re-running `ctest --test-dir build/s4 -R ipc_pipe` today (same committed `843ae49`
binary, `cmake --build` reports "no work to do") yielded **4/6 PASS on average**:
`test_round_trip` failed 3/5 runs, `test_timeout` 5/5, `test_session_guard` 5/5;
`test_frame_validation`, `test_oversized_rejected`, `test_malformed_wire_rejected`
passed in every run (including their post-rejection canary requests).

Root cause (confirmed via `SBAR_IPC_TRACE=1`, compiled-in runtime toggle):
failures are all `mach_send_message` client-open returning NULL, i.e. a
server-lifecycle race, NOT a frame/security/DACL defect:

1. **Cold-start open race** (`err=2` FILE_NOT_FOUND): the server thread creates the
   pipe instance asynchronously after `CreateThread` in `mach_server_begin`, but
   `ipc_open_client_pipe` retries FILE_NOT_FOUND back-to-back with ~0ms waits
   (the `WaitNamedPipeA(100ms)` only blocks on the ERROR_PIPE_BUSY branch). A
   client opening before the instance exists fails instantly.
2. **Zombie server threads + shared static `g_server`**: each test's server loop is
   immortal and re-reads `server->pipe_name`/`server->handler` on every iteration
   from the single reused struct. When the next test overwrites `pipe_name`, old
   threads race the current test's thread for the instance — observed as
   `CreateNamedPipeA failed err=5` (ERROR_ACCESS_DENIED) bursts, which also
   kills the current test's own server thread when it loses.
3. **Single 100ms busy-wait too short** (`err=231` PIPE_BUSY): test 6's raw
   connect+close leaves the server inside its `ipc_read_frame` deadline loop
   (≤100ms) plus recreate; the client's one `WaitNamedPipeA(100ms)` expires
   (err=121) → immediate NULL per the `!=FILE_NOT_FOUND → return NULL` branch.

Production note: the daemon is long-lived (instance already created, blocked in
`ConnectNamedPipe`), so the cold-start race does NOT apply to the real
client→bar path; the busy-recreate window could still produce a rare IPC
timeout. The failures are confined to the test harness reusing one server struct
across concurrent immortal threads. Fix candidates (a follow-up, NOT in this
commit's scope): per-test isolated server structs + terminate prior threads, and
make the FILE_NOT_FOUND branch actually wait with a retry deadline.

## Files changed

| File | Change |
|------|--------|
| `src/mach.c` | 401 lines: Windows pipe server + client + frame validation + session security |
| `src/mach.h` | 66 lines: `_WIN32` type surface + new mach_server members |
| `platform/win_ipc.h` | 43 lines: seam header (NEW) |
| `src/sketchybar.c` | 7 lines: mutex lockfile fix (SetLastError(0) + CloseHandle) |
| `tests/ipc_pipe_test.c` | 247 lines: 6-test cmocka suite (NEW) |
| `tests/cmocka_shim.h` | 61 lines: cmocka header shim (NEW) |
| `tests/cmocka_shim.c` | 61 lines: cmocka function stubs (NEW) |
| `CMakeLists.txt` | 69 lines: ipc_seam_check target, cmocka resolution, ipc_pipe target |
| `.github/workflows/windows-build.yml` | 11 lines: new targets in CI |

## Deviations from design

| Design said | Did / deviation |
|---|---|
| `open_memstream` output as response | Client reads response via 100ms byte-mode `ReadFile` (no `open_memstream` on Windows); timeout returns `""` via `malloc(1)` |
| Single security descriptor | 3-layer security: DACL + `PIPE_REJECT_REMOTE_CLIENTS` + post-connect session check (more robust than design's minimum) |
| `mach.h` two functions | Full seam header `platform/win_ipc.h` with 5 utility functions (frame validation, session match, security descriptor creation, client session check, pipe name builder) |
| cmocka installed | Shim fallback (`SKBAR_CMOCKA_SHIM`) for CI without cmocka; `find_package(cmocka)` → manual download → shim chain |

## Known constraints / risks

1. **No network on dev machine**: `FetchContent(DOWNLOAD)` fails; cmocka shim is the workaround. CI has network.
2. **Session SID extraction**: Uses `OpenProcessToken`→`TokenSessionId` comparison. Works for same-session clients; cross-session scenarios documented as rejected by design.
3. **Message-mode pipe**: `PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE` preserves frame boundaries without explicit length prefix; each `WriteFile` is one atomic message.

---

# S4 verification

Change: `windows-port` · Slice: S4 (tasks 4.1-4.5) · Branch: `feat/windows-port-s4`
Verified: 2026-09-07, HEAD = `5c3b341` (`843ae49` + harness determinism fix `5c3b341`)
Scope: **SLICE-SCOPED** — this documents S4 evidence only. The final whole-change
verify-report remains pending (dispatcher blocks it until S5-S7 complete; 26/50 tasks).

## Per-task result

| Task | Acceptance note | Result | Evidence |
|---|---|---|---|
| 4.1 Pipe server | `CreateNamedPipeA(PIPE_ACCESS_DUPLEX\|FILE_FLAG_FIRST_PIPE_INSTANCE)`; `PIPE_TYPE_MESSAGE\|PIPE_READMODE_MESSAGE\|PIPE_REJECT_REMOTE_CLIENTS`, 1 instance; thread reads frames, calls handler with `{buffer, MACH_MESSAGE}` semantics (`msgh_remote_port` = pipe handle as reply channel); single-instance recreate on disconnect | **PASS** | `src/mach.c:300-308` (flags + instance count), `:336-343` (read_frame → buffer → `server->handler`; reply handle in `msgh_remote_port`), `:347` (`CloseHandle` then loop recreates) |
| 4.2 `mach_send_message` client path | `port==NULL→NULL`; `INVALID_HANDLE_VALUE→client` (open by name, WriteFile+FlushFileBuffers, `await_response`→`ipc_read_response` 100 ms deadline, timeout→`""` via `malloc(1)`); real handle→write-response-only, `await_response` ignored, returns NULL; `len>IPC_MAX_FRAME→NULL` client cap | **PASS** | `src/mach.c:405` (NULL), `:407-434` (client path; 100 ms = `IPC_TIMEOUT_MS`), `:228-232`/`:265` (`ipc_empty_response` = `malloc(1)`), `:437-443` (reply-only, `(void)await_response`, NULL), `:413` (cap). `mach_get_bs_port` returns `INVALID_HANDLE_VALUE` sentinel `:446-449` |
| 4.3 Frame validation + 3-layer session security | `ipc_frame_valid` rejects >64 KB, empty interior tokens, no trailing NUL, empty, zero tokens, NULL; server rejects oversized at head of line + malformed frames; DACL (SYSTEM S-1-5-18 + current user + logon session S-1-5-5-X-Y) + `PIPE_REJECT_REMOTE_CLIENTS` + post-connect `ipc_client_is_same_session` (GetNamedPipeClientProcessId→OpenProcess→OpenProcessToken→TokenSessionId vs ProcessIdToSessionId) | **PASS** | `src/mach.c:42-56` (grammar), `:278` (head-of-line oversized), `:287` (malformed/oversized read rejected), `:113-161` (DACL: `WinLocalSystemSid`, `TokenUser`, `TokenLogonSid`), `:303` (REJECT_REMOTE_CLIENTS), `:163-186` (same-session check, fail-closed). Pinned at runtime by `test_session_guard` + `test_frame_validation` (DACL introspection via `ConvertSidToStringSidA`) |
| 4.4 Single-instance mutex | `SetLastError(0)` before `CreateMutexA`; `GetLastError()` captured immediately; `CloseHandle` on `ERROR_ALREADY_EXISTS`; name `Local\git.felix.<name>` | **PASS** | `src/sketchybar.c` `acquire_lockfile` (`_WIN32` branch): `SetLastError(0)` → `CreateMutexA(NULL, TRUE, mutex_name)` → `DWORD mutex_error = GetLastError()` → `CloseHandle` + exit on `ERROR_ALREADY_EXISTS` |
| 4.5 cmocka suite + harness fix | 6 tests (frame_validation, round_trip, timeout, oversized_rejected, malformed_wire_rejected, session_guard); `cmocka_shim.{h,c}` under `SKBAR_CMOCKA_SHIM`; ctest target `ipc_pipe`; harness fix `5c3b341` (per-test isolated `mach_server` structs, `mach_server_stop` teardown, `IPC_CONNECT_ATTEMPTS=30` with `WaitNamedPipeA` on PIPE_BUSY + `Sleep` on FILE_NOT_FOUND); unique `g_name` per test | **PASS** | `tests/ipc_pipe_test.c` (6 `cmocka_unit_test` entries `:250-258`; per-test `struct mach_server server = {0}` + `stop_server`; `wait_pipe` + retry loop `:74-82`; `unique_bar_name` `ipc_test_<n>` `:42-45`); `mach_server_stop` `src/mach.c:375-401`; `IPC_CONNECT_ATTEMPTS 30` `src/mach.c:199-226`; `CMakeLists.txt` (find_package→FetchContent→shim degrade chain; `add_test(NAME ipc_pipe ...)`) |

## Test determinism proof

- **This verification: 20/20 consecutive `ctest --test-dir build/s4 -R ipc_pipe` runs PASS** (exit 0 every run; runs 1-5 recorded first, then the full 20-run series for the aggregate claim). Each run: `1/1 Test #3: ipc_pipe ... Passed 0.57 sec`, `[ PASSED ] 6 test(s).` All six tests execute and pass (`-V` listing confirms each name).
- Prior flake is gone: the apply re-run note above recorded 4/6 average (round_trip 3/5, timeout 5/5, session_guard 5/5 failures) on `843ae49` alone — root cause was the server-lifecycle race (shared static `g_server` + 3-attempt connect). `5c3b341` (per-test servers + `mach_server_stop` + `IPC_CONNECT_ATTEMPTS=30` with real waits) makes the suite deterministic: **25 consecutive green runs total (5 + 20) in this session**.
- Direct binary run `build/s4/ipc_pipe.exe` → `[ PASSED ] 6 test(s).`, exit 0.
- Build evidence: forced rebuild (`touch src/mach.c tests/ipc_pipe_test.c` + `cmake --build build/s4 --target ipc_seam_check ipc_pipe`) → exit 0, clean; the only diagnostic is the pre-existing `getenv` deprecation warning at `src/mach.c:12` (`[-Wdeprecated-declarations]`), consistent with the rest of the tree.
- Output hashes (this session): build `71AA391EC7D7E291B419A7F40FBA91919E14AC2394AB4E8B66ED126E267D70F6` (no-op rebuild), ctest run `135F1C8BF160EB7D4048DD96DF2F7C9E0E28E3206B51A4480BC3628FA406BADE`.

## macOS isolation proof

- `git diff --exit-code 47585f9..HEAD -- makefile` → exit 0 (**empty**; macOS build untouched).
- `src/mach.c`: **451 insertions, 0 deletions**; `src/mach.h`: **75 insertions, 0 deletions** (526 insertions / 0 deletions combined). All additions sit inside the `#ifdef _WIN32` · `#else` · `#endif` structure (`src/mach.c:3`/`:451`, `src/mach.h:2`/`:66`/`:98`); the macOS bodies (Mach bootstrap/CFMachPort path, macOS `struct mach_server`) are byte-identical.

## Residual gaps (honest)

1. **Cross-session rejection is pinned, not end-to-end executed**: fabricating a real cross-session client requires a second Windows session, unavailable in one process. The rejection branch is verified by (a) DACL introspection — the descriptor carries SYSTEM `S-1-5-18` and the logon `S-1-5-5-X-Y` SID — (b) `ipc_sessions_match(7,7)`/`(7,8)` decision logic, (c) the live `ipc_client_is_same_session` check on a same-session client, and (d) code-path reading of `ipc_server_thread:330-334` (close + continue on mismatch). A session-mismatch e2e test (two RDP sessions) is a machine-level follow-up.
2. **CI verdict pending**: `windows-build.yml` now builds `ipc_seam_check` + `ipc_pipe` and runs `ctest -R ipc_pipe`, but no CI run of this branch has executed yet (dev machine offline). CI result is unknown until the next push.
3. **`ipc_frame_valid` is lenient about the trailing double NUL**: the grammar contract (win_ipc.h) says `...argv[n-1]'\0''\0'`, but the implementation accepts a single trailing NUL (`"a\0"`, `"a\0b\0"` pass) — only empty *interior* tokens, a missing final NUL, zero tokens, >64 KB, empty, and NULL are rejected (`src/mach.c:42-56`). The test ground truth pins exactly this (`"a\0\0"` true, `"abc"` false, `"a\0\0b\0"` false). Harmless: the real client always emits the double NUL and the handler parses NUL-separated argv either way. Recorded for the archive, not a defect.
4. **`SBAR_IPC_TRACE` helper** (`src/mach.c:11-18`) is a retained diagnostic toggle (getenv) — that is the source of the only build warning; pre-existing pattern, removal deferred.

## Slice-scope note

This section is S4-only evidence. The final whole-change `verify-report` (canonical
yaml envelope, spec-scenario compliance matrix over all requirements in
`specs/windows-port/spec.md`) is NOT produced here — it stays pending until S5-S7
implement and verify (5.1-5.7, 6.1-6.7, 7.1-7.10). Archive of this change must wait
for that report.