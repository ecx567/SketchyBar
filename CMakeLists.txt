cmake_minimum_required(VERSION 3.20)

# Native Windows port build system for SketchyBar.
#
# Coexists with the existing `makefile` (macOS). The Makefile is intentionally
# left untouched so the macOS build keeps working. This CMake project builds
# ONLY the Windows x64 target (no lipo/universal binaries).
#
# Requires:
#   - CMake >= 3.20
#   - clang-cl (or MSVC) with C99 support, targeting x64
#   - Optional: a Skia SDK (S1 links nothing against Skia yet; the find module
#     establishes where S2 will locate the Skia libraries).
#
# Primary target: `sketchybar`
# Compile-check target: `platform_compile_check` (builds the platform shims).

project(sketchybar-windows
        VERSION 2.24.0
        LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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

# --- Skia SDK discovery ------------------------------------------------------
# S1 does not yet consume Skia (the graphics seam lands in S2). This find block
# establishes where the Skia libraries are located so S2 can link them.
# Point SKIA_DIR at an unpacked Skia SDK to opt in.
set(SKIA_DIR "" CACHE PATH "Path to the Skia SDK for the Windows graphics backend")
set(SKIA_INCLUDE_DIR "" CACHE PATH "Path to the Skia include directory")
find_path(SKIA_INCLUDE_DIR skia/core/SkCanvas.h
          HINTS ${SKIA_DIR}/include)
if(SKIA_INCLUDE_DIR)
  message(STATUS "Skia SDK found at: ${SKIA_INCLUDE_DIR}")
else()
  message(STATUS "Skia SDK not found; graphics seam deferred to S2 (SKIA_DIR unset).")
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
if(SKIA_INCLUDE_DIR AND SKIA_DIR)
  target_link_directories(sketchybar PRIVATE ${SKIA_DIR}/out)
endif()
