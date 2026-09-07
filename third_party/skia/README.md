# Vendored Skia SDK (Windows x64) — provenance manifest

This directory holds an unpacked, PREBUILT Skia SDK used by the Windows port's
real graphics backend (`platform/sk_backend_skia.cpp`). The binaries are large
and are NOT tracked by git; this README is the tracked provenance record.

## Source

| Field       | Value |
|-------------|-------|
| Repository  | https://github.com/aseprite/skia (Aseprite's maintained Skia fork) |
| Release tag | `m124-08a5439a6b` |
| Artifact    | `Skia-Windows-Release-x64.zip` |
| Download    | https://github.com/aseprite/skia/releases/download/m124-08a5439a6b/Skia-Windows-Release-x64.zip |
| Size        | 27.42 MB (zip), ~130 MB unpacked, 1972 files |
| SHA-256     | `5A371A4B2819BB4EB96E36CD75FA623585E1D5477E253A970302B6F2471B6934` |
| Checked     | 2026-09-07 (S2f) |

Why Aseprite m124: it is the closest maintained Skia line to the API the
backend pins (classic `SkShaper::Make()` / `SkShaper::Feature` / run-handler
shaping), and it ships static x64 Windows libraries out of the box. Later
rewrites (SkShapingOptions-shaped SkShaper, `SkFontMgr::RefDefault` removal)
break that surface.

## Layout (after unpacking into this directory)

- `include/`            — public headers (canonical Google root layout:
                          `include/core/SkCanvas.h`, `modules/skshaper/include/SkShaper.h`)
- `out/Release-x64/`    — static libraries + `args.gn`
- `src/`                — SDK sources (informational; not built by us)
- `third_party/`        — vendored third-party sources inside the SDK

## Build configuration (out/Release-x64/args.gn)

```gn
is_debug=false
is_official_build=true
skia_use_freetype=true
skia_use_harfbuzz=true
skia_pdf_subset_harfbuzz=true
target_cpu="x64"
cc=clang
cxx=clang++
clang_win_version="18.1"
extra_cflags=["-MT"]       # STATIC Microsoft CRT — match this in CMake
```

Key consequences we depend on:

- Static CRT (`-MT`): any target linking `skia.lib` must build with the same
  runtime (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`), or lld-link/MSVC
  rejects the mismatch (LNK2038).
- HarfBuzz shaping enabled (needed by `SkShaper` feature support).
- `SkFontMgr` is runtime-backable by DirectWrite; the backend uses
  `SkFontMgr_New_DirectWrite()` (there is no `SkFontMgr::RefDefault()` in this
  revision).

## 20 static libraries in out/Release-x64

skia, skshaper, skunicode, skparagraph, skottie, skresources, sksg, svg,
freetype2, harfbuzz, icu, libjpeg, libpng, libwebp, libwebp_sse41, skcms,
bentleyottmann, expat, wuffs, zlib.

CMake globs `*.lib` and links the superset; lld-link only pulls referenced
members, so no symbol is dropped.

## Reproducing

```powershell
# 1. Download + verify
Invoke-WebRequest -Uri https://github.com/aseprite/skia/releases/download/m124-08a5439a6b/Skia-Windows-Release-x64.zip `
  -OutFile Skia-Windows-Release-x64.zip
(Get-FileHash Skia-Windows-Release-x64.zip -Algorithm SHA256).Hash   # must equal the value above
# 2. Unpack so that include/ lands at third_party/skia/include
Expand-Archive Skia-Windows-Release-x64.zip -DestinationPath third_party/skia/
# 3. Configure with the SDK
cmake -S . -B build\s2f -G Ninja -DSKIA_DIR=D:\world\Contribuciones\SketchyBar\third_party\skia
```

## License

Skia is BSD 3-Clause (see `include/core/SkTypes.h` header comment and the
SDK's LICENSE files under `LICENSE`/third-party notices shipped in the
artifact). No modifications are made to the SDK.