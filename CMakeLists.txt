cmake_minimum_required(VERSION 3.20)

# Native Windows port build system for SketchyBar.
#
# Coexists with the existing `makefile` (macOS). The Makefile is intentionally
# left untouched so the macOS build keeps working. This CMake project builds
# ONLY the Windows x64 target (no lipo/universal binaries).
#
# Requires:
#   - CMake >= 3.20
#   - clang-cl (or MSVC) with C99 support + a C++17 compiler, targeting x64
#   - Optional: a Skia SDK (S2 uses it for the real graphics backend). Point
#     SKIA_DIR at an unpacked Windows Skia SDK to opt in; without it the
#     fail-closed stub backend + parity gate are built instead.
#
# Primary target: `sketchybar` (S3+; S1/S2 keep it compile-only)
# Compile-check target: `platform_compile_check` (builds the platform shims)
# Graphics seam (S2): `graphics_seam_check` (portable drawing core against the
# Skia seam) and `bitmap_diff` (visual parity harness, registered with ctest).

project(sketchybar-windows
        VERSION 2.24.0
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single x64 target (ARM64 later).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "sketchybar Windows port currently requires a 64-bit (x64) target.")
endif()

# --- Compiler flags (mirror the macOS Makefile: -std=c99 -Wall -O3) ---------
if(MSVC)
  # clang-cl / MSVC. C99 via /std:c11 plus the flags closest to the Makefile.
  add_compile_options(/W4 /O2)
else()
  add_compile_options(-std=c99 -Wall -O3)
endif()

add_compile_options(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00)
# Windows headers define macro min/max which collide with std::min/std::max
# and <limits> std::numeric_limits<T>::max/min inside the Skia headers. Skia
# headers are not self-contained against those macros, so NOMINMAX must be a
# global define.
add_compile_definitions(NOMINMAX)

# --- Skia SDK discovery ------------------------------------------------------
# S2 consumes Skia only when a SDK is configured. Point SKIA_DIR at an unpacked
# Windows Skia SDK to opt in. Without one, the fail-closed stub backend is
# compiled (see platform/sk_backend_stub.c); the parity gate then cannot pass
# by design (documented in openspec/changes/windows-port/apply-progress.md).
set(SKIA_DIR "" CACHE PATH "Path to the Skia SDK for the Windows graphics backend")
# S2f: the vendored SDK (Aseprite m124, third_party/skia) uses the canonical
# Google root layout — headers at <SDK>/include/core/... So SKIA_INCLUDE_DIR
# resolves to the SDK ROOT and the backend's `#include "include/core/..."` and
# `#include "modules/skshaper/include/..."` includes resolve unchanged.
find_path(SKIA_INCLUDE_DIR include/core/SkCanvas.h
          HINTS ${SKIA_DIR})
if(SKIA_INCLUDE_DIR)
  message(STATUS "Skia SDK found at: ${SKIA_INCLUDE_DIR}")
  # Static libraries ship under out/Release-x64 in the vendored SDK.
  set(SKIA_LIBRARY_DIR "${SKIA_DIR}/out/Release-x64" CACHE PATH
      "Directory holding the vendored static Skia libraries")
  if(NOT EXISTS "${SKIA_LIBRARY_DIR}")
    set(SKIA_LIBRARY_DIR "${SKIA_DIR}/out")
  endif()
  # The Aseprite SDK is built with -MT (static Microsoft CRT — see
  # out/Release-x64/args.gn: extra_cflags=["-MT"]). Every target that links it
  # must use the same runtime library or the linker rejects the mismatch
  # (LNK2038 RuntimeLibrary).
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  # Link every static lib in the SDK's lib dir; lld-link pulls only the
  # referenced members, so the superset is safe and future-proof.
  file(GLOB SKIA_STATIC_LIBS "${SKIA_LIBRARY_DIR}/*.lib")
  list(LENGTH SKIA_STATIC_LIBS SKIA_STATIC_LIB_COUNT)
  message(STATUS "Skia static libraries: ${SKIA_LIBRARY_DIR} (${SKIA_STATIC_LIB_COUNT} libs)")
  # DirectWrite (SkFontMgr_New_DirectWrite) + the Win32 APIs the backend uses
  # (shell32/ole32/gdi32) must be linked into anything that links skia.lib.
  set(SKIA_HOST_LIBS ${SKIA_STATIC_LIBS} dwrite shell32 ole32 gdi32)
else()
  message(STATUS "Skia SDK not found; graphics parity gate FAIL-CLOSED (stub backend).")
endif()

# --- Graphics backend selection ----------------------------------------------
# Backend sources are the raw seam implementations (Skia C++ or stub C)
# plus the backend-agnostic glue (CFRelease / CGImage* / CFData* dispatch).
set(SKBAR_BACKEND_SOURCES
    platform/sk_backend_common.c)
if(SKIA_INCLUDE_DIR)
  list(APPEND SKBAR_BACKEND_SOURCES platform/sk_backend_skia.cpp)
  # freetype2.lib (via skshaper -> HarfBuzz) references plain zlib names;
  # the SDK ships Chromium-prefixed zlib only. See platform/skia_zlib_shim.c.
  list(APPEND SKBAR_BACKEND_SOURCES platform/skia_zlib_shim.c)
else()
  list(APPEND SKBAR_BACKEND_SOURCES platform/sk_backend_stub.c)
endif()

# --- Platform shims ----------------------------------------------------------
# The `platform/` tree isolates macOS-specific code behind a defined boundary.
# Each stub is a C module that later slices fill in. In S1 these are
# compile-only stubs; they establish the compile check for CI.
set(PLATFORM_SOURCES
    platform/win_main.c
    platform/win_events.c
    platform/layer_dcomp.c
    platform/display_win.c
    platform/workspace_win.c
    platform/wifi_win.c
    platform/media_win.c)

# --- Portable core (shared with macOS) --------------------------------------
# These files are the platform-agnostic core and must compile unchanged on
# Windows. They are accumulated slice-by-slice as the platform shims they
# reference become available (S4-S7). S1 contributes the source list so the
# compile-check target below exercises the shims in isolation.
set(PORTABLE_CORE "")

# --- link libraries (map of the macOS framework list for Win32) -------------
set(WIN32_LIBS
    dwmapi
    dcomp
    d2d1
    dxgi
    Winmm
    Setupapi)

# --- compile-check target ----------------------------------------------------
# Proves the platform shims + win_compat shim compile under clang-cl. Run with:
#   cmake --build . --target platform_compile_check
# This is the CI compile-check seam for S1 (no runtime yet).
add_library(platform_compile_check STATIC ${PLATFORM_SOURCES})
target_include_directories(platform_compile_check PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/platform)
target_compile_definitions(platform_compile_check PRIVATE _WIN32)

# Shipping ICU data next to Skia-backed binaries. SkIcuLoader resolves
# icudtl.dat relative to the executable dir; without it Skia runs with a null
# ICU fallback (fine for Latin shaping, degraded for complex scripts).
set(SKIA_ICU_DATA "${SKIA_DIR}/third_party/externals/icu/flutter/icudtl.dat")
function(skia_icu_data TARGET)
  if(EXISTS "${SKIA_ICU_DATA}")
    add_custom_command(TARGET ${TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SKIA_ICU_DATA}" "$<TARGET_FILE_DIR:${TARGET}>/icudtl.dat")
  endif()
endfunction()

# --- sketchybar target --------------------------------------------------------
add_executable(sketchybar ${PLATFORM_SOURCES} ${PORTABLE_CORE})
target_include_directories(sketchybar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/platform)
if(SKIA_INCLUDE_DIR)
  target_include_directories(sketchybar PRIVATE ${SKIA_INCLUDE_DIR})
endif()
target_compile_definitions(sketchybar PRIVATE _WIN32)
target_link_libraries(sketchybar PRIVATE ${WIN32_LIBS})
if(SKIA_INCLUDE_DIR)
  target_link_libraries(sketchybar PRIVATE ${SKIA_HOST_LIBS})
  target_link_directories(sketchybar PRIVATE ${SKIA_LIBRARY_DIR})
  skia_icu_data(sketchybar)
endif()

# --- graphics seam check (S2) --------------------------------------------------
# Compiles the portable drawing core (context, font, image, color) against the
# Skia seam on Windows. A STATIC library keeps undefined refs to macOS-only
# modules (bar_manager symbols, media events, ...) from blocking the compile
# seam until their Windows adapters land (S3-S7). Build it with:
#   cmake --build . --target graphics_seam_check
set(GRAPHICS_SEAM_SOURCES
    src/context.c
    src/font.c
    src/image.c
    src/color.c)

add_library(graphics_seam_check STATIC ${GRAPHICS_SEAM_SOURCES} ${SKBAR_BACKEND_SOURCES})
target_include_directories(graphics_seam_check PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/platform)
if(SKIA_INCLUDE_DIR)
  target_include_directories(graphics_seam_check PRIVATE ${SKIA_INCLUDE_DIR})
endif()
target_compile_definitions(graphics_seam_check PRIVATE _WIN32)
if(SKIA_INCLUDE_DIR)
  target_link_directories(graphics_seam_check PRIVATE ${SKIA_LIBRARY_DIR})
endif()

# --- visual parity gate (S2) ---------------------------------------------------
# bitmap_diff renders the reference cases through the seam and compares against
# the other-platform golden (<case>_<platform>.png). With the stub backend it
# exits non-zero with a documented message (the gate is fail-closed without a
# real Skia SDK). Registered with ctest so it runs via:
#   ctest --test-dir build -R bitmap_diff
#
# The harness calls the sk_* surface directly (it must not reference the
# portable core's bar_manager globals), so it links ONLY the backend.
add_executable(bitmap_diff tests/bitmap_diff_main.c ${SKBAR_BACKEND_SOURCES})
target_include_directories(bitmap_diff PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(bitmap_diff PRIVATE _WIN32)
if(SKIA_INCLUDE_DIR)
  target_include_directories(bitmap_diff PRIVATE ${SKIA_INCLUDE_DIR})
  target_link_directories(bitmap_diff PRIVATE ${SKIA_LIBRARY_DIR})
  target_link_libraries(bitmap_diff PRIVATE ${SKIA_HOST_LIBS})
  skia_icu_data(bitmap_diff)
endif()

enable_testing()
add_test(NAME bitmap_diff COMMAND bitmap_diff --check --case all)
