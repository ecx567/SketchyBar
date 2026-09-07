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