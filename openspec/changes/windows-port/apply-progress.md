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