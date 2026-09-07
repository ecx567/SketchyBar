// tests/spike_compositor.c
//
// S3 compositor spike: validates the layered-window + acrylic-blur path the
// Windows compositor is built from (tasks 3.1, 3.2) and exercises the
// DirectComposition layer of task 3.3.
//
// Checks:
//   1. A top-level window with WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE
//      is created and keeps those styles (window.c _WIN32 open path parity).
//   2. UpdateLayeredWindow uploads the Skia seam's bitmap through the EXACT
//      production byte path: CG-order pixels (row 0 == bottom) -> positive-
//      height (bottom-up) DIB -> byte-for-byte memcpy. The CPU-side layout
//      checks below prove that no vertical flip is needed (the leftover S2
//      draft used a top-down DIB + CG-order pixels, which would render every
//      bar upside down).
//   3. SetWindowPos(HWND_TOPMOST) succeeds and the window reports
//      WS_EX_TOPMOST.
//   4. Non-activation: showing the layered window never changes the
//      foreground window.
//   5. SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)
//      succeeds on the layered window and can be disabled again.
//   6. DirectComposition layer round-trip (layer_create / layer_set_bounds /
//      layer_set_contents / layer_destroy) through the real seam.
//
// Headless honesty: in CI (no interactive desktop) CreateWindowEx returns
// NULL and the desktop-dependent checks are SKIPPED (reported, exit 0) while
// the CPU-side checks (seam render, CG-order layout, DComp device) still run.
// Pass --strict to demand a real desktop: the spike then exits non-zero if the
// window cannot be created. Run compositor_spike.exe on a desktop session for
// the full validation (documented in apply-progress.md, task 3.2).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sk_backend.h"
#include "layer.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// --- Acrylic blur structures (mirror window.c _WIN32 branch) ------------------
typedef enum {
  ACCENT_DISABLED = 0,
  ACCENT_ENABLE_GRADIENT = 1,
  ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
  ACCENT_ENABLE_ACRYLICBLURBEHIND = 3,
  ACCENT_ENABLE_ACRYLIC = 4,
} AccentState;

typedef struct {
  AccentState AccentState;
  int         AccentFlags;
  int         GradientColor; /* ABGR */
  int         AnimationId;
} AccentPolicy;

// Real user32 signature: a WINDOWCOMPOSITIONATTRIBDATA wrapper (attribute
// code + pointer + size) around the accent policy. Passing the policy flat
// would set attribute == AccentState (3) and fail; keep the two-struct shape.
typedef struct {
  DWORD  Attribute;
  void*  Data;
  SIZE_T SizeOfData;
} WindowCompositionAttributeData;

typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WindowCompositionAttributeData*);

#define BAR_EX_STYLE (WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
#define BAR_CLASS L"sketchybar_layered"

static int g_checked = 0;
static int g_passed  = 0;
static int g_skipped = 0;

// NOTE: expressions passed to CHECK must be parenthesized when they contain a
// comma (e.g. CHECK((fn(hwnd, &data)) != 0, ...)) — the preprocessor splits
// macro arguments on top-level commas.
#define CHECK(cond, msg) do { \
  g_checked++; \
  if (cond) { g_passed++; printf("  [PASS] %s\n", msg); } \
  else      { printf("  [FAIL] %s\n", msg); } \
} while (0)

#define SKIP(msg) do { g_skipped++; printf("  [SKIP] %s\n", msg); } while (0)

static BOOL register_bar_class(void) {
  WNDCLASSW wc = { 0 };
  wc.lpfnWndProc   = DefWindowProcW;
  wc.hInstance     = GetModuleHandleW(NULL);
  wc.lpszClassName = BAR_CLASS;
  return RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static HWND create_bar_window(int w, int h) {
  return CreateWindowExW(BAR_EX_STYLE, BAR_CLASS, L"sketchybar",
                        WS_POPUP, 0, 0, w, h,
                        NULL, NULL, GetModuleHandleW(NULL), NULL);
}

// --- task 3.1: the production Skia render -> ULW pipeline ---------------------

// Renders an asymmetric pattern through the REAL seam so a vertical flip is
// detectable: blue bottom band, white middle band, red top band (CG user
// space, y == 0 at the bottom). A fresh path per band: paths accumulate
// rects, and reusing one path would overpaint every band with the last color.
static int render_pattern(skbar_context** out_ctx, size_t* out_w, size_t* out_h) {
  const size_t w = 320, h = 40;  // 160x20 points @ 2.0 scale
  skbar_context* ctx = sk_context_create((uint32_t)w, (uint32_t)h, 2.0f, false);
  if (!ctx) return -1;

  skbar_path* path = sk_path_create();
  if (!path) { sk_context_destroy(ctx); return -1; }

  sk_context_set_fill_rgba(ctx, 0.0, 0.0, 1.0, 1.0);        // blue (bottom)
  sk_path_add_rect(path, CGRectMake(0, 0, 80, 6));
  sk_context_add_path(ctx, path);
  sk_context_draw_path(ctx, kCGPathFill);
  sk_path_destroy(path);

  path = sk_path_create();
  if (!path) { sk_context_destroy(ctx); return -1; }
  sk_context_set_fill_rgba(ctx, 1.0, 1.0, 1.0, 1.0);        // white (middle)
  sk_path_add_rect(path, CGRectMake(0, 6, 80, 8));
  sk_context_add_path(ctx, path);
  sk_context_draw_path(ctx, kCGPathFill);
  sk_path_destroy(path);

  path = sk_path_create();
  if (!path) { sk_context_destroy(ctx); return -1; }
  sk_context_set_fill_rgba(ctx, 1.0, 0.0, 0.0, 1.0);        // red (top)
  sk_path_add_rect(path, CGRectMake(0, 14, 80, 6));
  sk_context_add_path(ctx, path);
  sk_context_draw_path(ctx, kCGPathFill);
  sk_path_destroy(path);

  *out_ctx = ctx;
  *out_w = w;
  *out_h = h;
  return 0;
}

// Uploads a CG-order BGRA buffer through UpdateLayeredWindow. Positive-height
// DIB == bottom-up == CG order, so this is a byte-for-byte memcpy — the exact
// copy window.c's window_upload_layered performs.
static BOOL upload_cg_bgra(HWND hwnd, const void* bgra, size_t w, size_t h) {
  BITMAPINFO bmi = { 0 };
  bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth       = (LONG)w;
  bmi.bmiHeader.biHeight      = (LONG)h; /* positive: bottom-up == CG order */
  bmi.bmiHeader.biPlanes      = 1;
  bmi.bmiHeader.biBitCount    = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = NULL;
  HDC screen = GetDC(NULL);
  HDC mem    = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (!dib || !bits) {
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return FALSE;
  }

  memcpy(bits, bgra, w * h * 4u);

  HGDIOBJ old = SelectObject(mem, dib);
  POINT src = { 0, 0 };
  SIZE  sz  = { (LONG)w, (LONG)h };
  BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  BOOL ok = UpdateLayeredWindow(hwnd, screen, NULL, &sz, mem, &src,
                                0, &blend, ULW_ALPHA);

  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
  return ok;
}

// CPU-side layout proof (no desktop needed): the seam must export row 0 as the
// BOTTOM row (blue) and the last row as the TOP row (red).
static void check_buffer_layout(const void* bgra, size_t w, size_t h) {
  const uint32_t* px = (const uint32_t*)bgra;
  uint32_t bottom_left = px[0];                 // row 0 == bottom (blue)
  uint32_t top_left    = px[(h - 1) * w];       // last row == top (red)
  CHECK((bottom_left & 0x00FFFFFFu) == 0x000000FFu,
        "seam exports CG order (row 0 == bottom blue band)");
  CHECK((top_left & 0x00FFFFFFu) == 0x00FF0000u,
        "seam exports CG order (last row == top red band)");
}

// --- task 3.3: DirectComposition layer round-trip -----------------------------
// Exercises layer_create/layer_set_bounds/layer_set_contents/layer_destroy
// against the real DComp device (CPU upload; no window-target attach yet —
// that is the S7 hardware-composition follow-up).
static void exercise_dcomp_layer(const void* bgra, size_t w, size_t h) {
  struct layer* layer = layer_create(0, CGRectMake(0, 0, 80, 20));
  if (!layer) {
    SKIP("DirectComposition layer (no DComp device in this session)");
    return;
  }

  // Mirror the production flip (layer_set_contents flips CG order -> top-down
  // for the DComp surface; window_capture flips the DIB the same way).
  size_t row_bytes = w * 4u;
  uint8_t* topdown = malloc(row_bytes * h);
  if (!topdown) {
    layer_destroy(layer);
    SKIP("DirectComposition layer contents buffer");
    return;
  }
  for (size_t y = 0; y < h; ++y) {
    memcpy(topdown + y * row_bytes,
           (const uint8_t*)bgra + (h - 1u - y) * row_bytes, row_bytes);
  }

  skbar_image* image = sk_image_from_bgra(topdown, (uint32_t)w, (uint32_t)h);
  CHECK(image != NULL, "sk_image_from_bgra wraps top-down BGRA");

  layer_set_bounds(layer, CGRectMake(50, 25, 40, 10));
  layer_set_contents(layer, (CGImageRef)image);
  // A detached composition surface has no read-back API: success of the
  // BeginDraw/Map/EndDraw round-trip is the property exercised here.
  printf("  [INFO] DComp layer round-trip (create/bounds/contents/destroy)\n");

  layer_destroy(layer);
  if (image) sk_image_unref(image);
  free(topdown);
}

#endif /* _WIN32 */

int main(int argc, char** argv) {
  printf("=== compositor_spike (S3) ===\n");

  bool strict = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--strict") == 0) strict = true;
  }
  if (strict) printf("[--strict] desktop failures are hard failures\n");

#ifdef _WIN32
  // Headless-safe checks (no window needed) ---------------------------------
  CHECK((BAR_EX_STYLE == (WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)),
        "bar ex-style constants resolve");
  CHECK(register_bar_class(), "layered window class registers");
  {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    pfnSetWindowCompositionAttribute fn = user32
      ? (pfnSetWindowCompositionAttribute)GetProcAddress(
            user32, "SetWindowCompositionAttribute")
      : NULL;
    CHECK(fn != NULL, "SetWindowCompositionAttribute resolves from user32");
  }

  // Render + layout proof through the seam -----------------------------------
  skbar_context* ctx = NULL;
  size_t w = 0, h = 0;
  void* bgra = NULL;
  if (render_pattern(&ctx, &w, &h) != 0 || !ctx) {
    printf("  [INFO] Skia backend unavailable (stub build: SKIA_DIR unset)\n");
    SKIP("seam render + CG-order layout checks (needs Skia SDK)");
  } else {
    size_t pw = 0, ph = 0;
    if (!sk_context_read_pixels(ctx, &pw, &ph, &bgra) || !bgra) {
      SKIP("seam read_pixels (needs Skia SDK)");
    } else {
      CHECK((pw == w && ph == h), "seam read_pixels returns expected dims");
      check_buffer_layout(bgra, w, h);

      // Desktop-dependent checks ---------------------------------------------
      HWND hwnd = create_bar_window((int)w, (int)h);
      if (!hwnd) {
        printf("  [INFO] CreateWindowExW returned NULL - no interactive desktop.\n");
        if (strict) {
          printf("  [FAIL] --strict: desktop session required.\n");
          g_checked++;  // counted, not passed -> non-zero exit
        } else {
          SKIP("window creation / styles / ULW / topmost / non-activation / acrylic");
        }
      } else {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        CHECK((ex & WS_EX_LAYERED) != 0, "window is layered (WS_EX_LAYERED)");
        CHECK((ex & WS_EX_TOOLWINDOW) != 0, "window is a tool window (WS_EX_TOOLWINDOW)");
        CHECK((ex & WS_EX_NOACTIVATE) != 0, "window never activates (WS_EX_NOACTIVATE)");

        // Task 3.1: production byte-path ULW upload.
        CHECK(upload_cg_bgra(hwnd, bgra, w, h),
              "UpdateLayeredWindow uploads the seam bitmap (BGRA, no flip)");

        // Task 3.1: always-on-top.
        CHECK(SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
              "SetWindowPos(HWND_TOPMOST) succeeds");
        LONG_PTR ex2 = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        CHECK((ex2 & WS_EX_TOPMOST) != 0, "window is topmost (WS_EX_TOPMOST)");

        // Task 3.1: non-activation.
        HWND before = GetForegroundWindow();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, (int)w, (int)h, SWP_NOACTIVATE);
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        CHECK(GetForegroundWindow() == before,
              "layered window never activates (foreground unchanged)");

        // Task 3.2: acrylic blur on the layered window, then disable.
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        pfnSetWindowCompositionAttribute fn = (pfnSetWindowCompositionAttribute)
          GetProcAddress(user32, "SetWindowCompositionAttribute");
        AccentPolicy policy = { 0 };
        policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
        policy.AccentFlags = 0;
        policy.GradientColor = 0x00000000;  // blur-only tint (macOS parity)
        policy.AnimationId = 0;
        WindowCompositionAttributeData attr = {
          (DWORD)19, &policy, sizeof(policy)  // WCA_ACCENT_POLICY
        };
        CHECK((fn(hwnd, &attr)) != 0, "SetWindowCompositionAttribute acrylic succeeds");
        policy.AccentState = ACCENT_DISABLED;
        CHECK((fn(hwnd, &attr)) != 0, "SetWindowCompositionAttribute disable succeeds");

        DestroyWindow(hwnd);
      }

      // Task 3.3: DComp layer round-trip (independent of the window path).
      exercise_dcomp_layer(bgra, w, h);
    }
    if (bgra) free(bgra);
  }
  if (ctx) sk_context_destroy(ctx);

  printf("\nspike: %d checked, %d passed, %d skipped -> exit %s\n",
         g_checked, g_passed, g_skipped,
         g_passed == g_checked ? "0" : "1");
  return g_passed == g_checked ? 0 : 1;
#else
  (void)strict;
  printf("compositor_spike: Windows-only; skipped on macOS.\n");
  return 0;
#endif
}