#include "window.h"
#ifndef _WIN32
#include "bar_manager.h"
#endif
#include "misc/helpers.h"
#include "surface.h"
#include "layer.h"
#ifndef _WIN32
#include <pthread.h>
#endif

#ifdef _WIN32
// The Windows compositor (S3): each window is a layered HWND; the Skia seam
// provides pixel access (read_pixels for the ULW display path, snapshot as
// the layer contents source in surface.c).
#include "../platform/sk_backend.h"
#endif

#ifndef _WIN32
extern struct bar_manager g_bar_manager;
extern CGError (* SBSLSTransactionAddPostDecodeAction)(CFTypeRef, void (^)());
#endif
extern int64_t g_disable_capture;

int g_space = 0;

#ifdef _WIN32
// Fixed CG user-space scale (mirrors SLSSetWindowResolution(..., 2.0f)).
// Multi-monitor/DPI mapping lands with display_win.c (S7).
#define kWINDOW_SCALE 2.0f

static HDWP g_hdwp = NULL;
static bool g_class_registered = false;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
  // No window messages are handled yet; the mouse-tracking adapter (S5) and
  // the WM_PAINT-free draw flow arrive in later slices.
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Win32 requires a registered class before CreateWindowExW. Shared by every
// bar window; registered once per process.
static bool window_register_class(void) {
  if (g_class_registered) return true;
  WNDCLASSW wc = { 0 };
  wc.lpfnWndProc = window_proc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"sketchybar_layered";
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  g_class_registered = true;
  return true;
}

// macOS user-space points (origin BOTTOM-left) -> device pixels (origin
// TOP-left) against the primary monitor's work area. The window is placed
// bottom-up relative to the work area, mirroring macOS SLS coordinates.
// S3 targets the primary monitor; the multi-monitor mapping lands with S7
// (display_win.c).
static void window_frame_to_device(CGRect frame, int* out_x, int* out_y,
                                   int* out_w, int* out_h) {
  RECT work = { 0 };
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int width = (int)(frame.size.width * kWINDOW_SCALE);
  int height = (int)(frame.size.height * kWINDOW_SCALE);
  int origin_x_px = (int)(frame.origin.x * kWINDOW_SCALE);
  int origin_y_px = (int)(frame.origin.y * kWINDOW_SCALE);  // from the bottom
  *out_w = width;
  *out_h = height;
  *out_x = work.left + origin_x_px;
  *out_y = work.top + (work.bottom - work.top) - origin_y_px - height;
}

// Displays the current window bitmap via UpdateLayeredWindow: the production
// display path for layered windows (windows with WS_EX_LAYERED are painted
// exclusively through ULW, never WM_PAINT). The seam exports CG-order pixels
// (row 0 == BOTTOM); a positive-height DIB is ALSO bottom-up, so the copy is
// byte-for-byte (no vertical flip) — see tests/spike_compositor.c which
// validates exactly this layout math.
static bool window_upload_layered(struct window* window) {
  if (!window->context || !window->hwnd) return false;

  size_t width = 0, height = 0;
  void* bgra = NULL;
  if (!sk_context_read_pixels(window->context, &width, &height, &bgra)) {
    return false;
  }

  HDC screen = GetDC(NULL);
  HDC mem = CreateCompatibleDC(screen);

  BITMAPINFO bmi = { 0 };
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = (LONG)width;
  bmi.bmiHeader.biHeight = (LONG)height;  // positive -> bottom-up == CG order
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = NULL;
  HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  bool ok = false;
  if (dib && bits) {
    memcpy(bits, bgra, width * height * 4u);
    HGDIOBJ old = SelectObject(mem, dib);
    POINT src = { 0, 0 };
    SIZE size = { (LONG)width, (LONG)height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    ok = UpdateLayeredWindow(window->hwnd, screen, NULL, &size, mem, &src,
                             0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
  } else if (dib) {
    DeleteObject(dib);
  }
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
  free(bgra);
  return ok;
}
#endif

void window_init(struct window* window) {
  window->context = NULL;
  window->surface = NULL;
  window->parent = NULL;
  window->frame = CGRectNull;
  window->id = 0;
  window->origin = CGPointZero;
  window->needs_move = false;
  window->needs_resize = false;
  window->order_mode = W_ABOVE;
  window->refc = 1;
#ifndef _WIN32
  // (no extra members on macOS; the HWND member is _WIN32-only)
#else
  window->hwnd = NULL;
#endif
}

#ifndef _WIN32
static CFTypeRef window_create_region(struct window* window, CGRect frame) {
  CFTypeRef frame_region;
  CGSNewRegionWithRect(&frame, &frame_region);
  return frame_region;
}
#endif

struct window* window_create() {
  struct window* window = malloc(sizeof(struct window));
  memset(window, 0, sizeof(struct window));
  window_init(window);
  return window;
}

void window_destroy(struct window* window) {
  if (--window->refc > 0) return;
  window_close(window);
  free(window);
}

#ifndef _WIN32
static void window_clear_background(struct window* window) {
  if (window->context) {
    CGContextClearRect(window->context, window->frame);
    CGContextFlush(window->context);
  }
}
#endif

void window_open(struct window* window, CGRect frame) {
#ifdef _WIN32
  if (!window_register_class()) return;

  int width = (int)(frame.size.width * kWINDOW_SCALE);
  int height = (int)(frame.size.height * kWINDOW_SCALE);

  window->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                                 WS_EX_NOACTIVATE,
                                 L"sketchybar_layered", L"sketchybar",
                                 WS_POPUP, 0, 0, width, height,
                                 NULL, NULL, GetModuleHandleW(NULL), NULL);
  if (!window->hwnd) return;

  window->origin = frame.origin;
  window->frame.origin = CGPointZero;
  window->frame.size = frame.size;

  // Monotonic id (macOS parity); the HWND itself is the Windows identity.
  static uint32_t g_window_id = 1;
  window->id = g_window_id++;

  // Always-on-top z-order; the window never activates (WS_EX_NOACTIVATE plus
  // SWP_NOACTIVATE on every position change).
  SetWindowPos(window->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

  window->context = context_create(frame.size, 2.0f);
  window->surface = surface_create(window);
  window->needs_move = false;
  window->needs_resize = false;
#else
  uint64_t set_tags = kCGSExposeFadeTagBit | kCGSPreventsActivationTagBit;
  uint64_t clear_tags = 0;

  window->origin = frame.origin;
  window->frame.origin = CGPointZero;
  window->frame.size = frame.size;
  frame.origin = CGPointZero;

  uint32_t id;
  CFTypeRef frame_region = window_create_region(window, frame);
  CFTypeRef empty_region = CGRegionCreateEmptyRegion();
  SLSNewWindowWithOpaqueShapeAndContext(g_connection,
                                        kCGBackingStoreBuffered,
                                        frame_region,
                                        empty_region,
                                        13 | (1 << 18),
                                        &set_tags,
                                        window->origin.x,
                                        window->origin.y,
                                        64,
                                        &id,
                                        NULL                    );
  CFRelease(empty_region);
  CFRelease(frame_region);

  window->id = id;

  SLSSetWindowResolution(g_connection, window->id, 2.0f);
  SLSSetWindowTags(g_connection, window->id, &set_tags, 64);
  SLSClearWindowTags(g_connection, window->id, &clear_tags, 64);
  SLSSetWindowOpacity(g_connection, window->id, 0);

  window->context = SLWindowContextCreate(g_connection, window->id, NULL);
  window_clear_background(window);

  CGContextSetInterpolationQuality(window->context, kCGInterpolationNone);
  window->surface = surface_create(window);
  window->needs_move = false;
  window->needs_resize = false;


  if (g_bar_manager.sticky) {
    if (!g_space) {
      g_space = SLSSpaceCreate(g_connection, 1, 0);
      SLSSpaceSetAbsoluteLevel(g_connection, g_space, 0);

      CFArrayRef space_list = cfarray_of_cfnumbers(&g_space,
                                                   sizeof(uint32_t),
                                                   1,
                                                   kCFNumberSInt32Type);
      SLSShowSpaces(g_connection, space_list);
      CFRelease(space_list);
    }

    CFArrayRef window_list = cfarray_of_cfnumbers(&window->id,
                                                  sizeof(uint32_t),
                                                  1,
                                                  kCFNumberSInt32Type);

    SLSSpaceAddWindowsAndRemoveFromSpaces(g_connection,
                                          g_space,
                                          window_list,
                                          0x7          );

    CFRelease(window_list);
  }
#endif
}

void window_clear(struct window* window) {
  window->context = NULL;
  window->surface = NULL;
  window->parent = NULL;
  window->id = 0;
  window->origin = CGPointZero;
  window->frame = CGRectNull;
  window->needs_move = false;
  window->needs_resize = false;
  window->refc = 1;
#ifdef _WIN32
  window->hwnd = NULL;
#endif
}

void window_flush(struct window* window) {
#ifdef _WIN32
  // Two-phase display: the DirectComposition layer receives the snapshot
  // (compositor path, layer_dcomp.c) and the layered window shows the same
  // pixels via UpdateLayeredWindow (the S3 display path).
  surface_flush(window->surface);
  window_upload_layered(window);
#else
  surface_flush(window->surface);
#endif
}

void windows_freeze() {
#ifdef _WIN32
  // Deferred window positioning (one DeferWindowPos batch per frame,
  // committed at windows_unfreeze) mirrors the macOS transaction model.
  if (g_hdwp) return;
  g_hdwp = BeginDeferWindowPos(16);
#else
  if (g_transaction) return;

  if (__builtin_available(macOS 26.0, *)) { }
  else SLSDisableUpdate(g_connection);

  g_transaction = SLSTransactionCreate(g_connection);
#endif
}

void windows_unfreeze() {
#ifdef _WIN32
  if (g_hdwp) {
    EndDeferWindowPos(g_hdwp);
    g_hdwp = NULL;
  }
#else
  if (g_transaction) {
    SLSTransactionCommit(g_transaction, 0x0);
    CFRelease(g_transaction);
    g_transaction = NULL;

    if (__builtin_available(macOS 26.0, *)) { }
    else SLSReenableUpdate(g_connection);
  }
#endif
}

void window_set_frame(struct window* window, CGRect frame) {
  if (window->needs_move
      || !CGPointEqualToPoint(window->origin, frame.origin)) {
    window->needs_move = true;
    window->origin = frame.origin;
  }

  if (window->needs_resize
      || !CGSizeEqualToSize(window->frame.size, frame.size)) {
    window->needs_resize = true;
    window->frame.size = frame.size;
  }
}

void window_move(struct window* window, CGPoint point) {
#ifdef _WIN32
  window->origin = point;
  if (!window->hwnd) return;

  int x = 0, y = 0, w = 0, h = 0;
  window_frame_to_device(window->frame, &x, &y, &w, &h);
  UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER;
  if (g_hdwp) {
    g_hdwp = DeferWindowPos(g_hdwp, window->hwnd, NULL, x, y, 0, 0, flags);
  } else {
    SetWindowPos(window->hwnd, NULL, x, y, 0, 0, flags);
  }
#else
  window->origin = point;

  if (__builtin_available(macOS 12.0, *)) {
    // Monterey and later
    windows_freeze();
    SLSTransactionMoveWindowWithGroup(g_transaction, window->id, point);
  } else {
    // Big Sur and previous
    SLSMoveWindow(g_connection, window->id, &point);
    CFNumberRef number = CFNumberCreate(NULL,
                                        kCFNumberSInt32Type,
                                        &window->id         );

    const void* values[1] = { number };
    CFArrayRef array = CFArrayCreate(NULL, values , 1, &kCFTypeArrayCallBacks);
    SLSReassociateWindowsSpacesByGeometry(g_connection, array);
    CFRelease(array);
    CFRelease(number);
  }

#endif
}

#ifndef _WIN32
static void window_defer_update(struct window* window) {
  void (^block)() = ^{
    if (!window->surface) return;
    if (--window->refc <= 0) window_destroy(window);
    else {
      layer_set_bounds(window->surface->layer, window->frame);
      window_flush(window);
    }
  };

  if (pthread_main_np()) dispatch_async(dispatch_get_main_queue(), block);
  else dispatch_sync(dispatch_get_main_queue(), block);
}
#endif

bool window_apply_frame(struct window* window, bool forced) {
#ifdef _WIN32
  if (window->needs_resize || forced) {
    windows_freeze();
    surface_resize(window->surface, window);

    if (window->hwnd) {
      int x = 0, y = 0, w = 0, h = 0;
      window_frame_to_device(window->frame, &x, &y, &w, &h);
      UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
      if (g_hdwp) {
        g_hdwp = DeferWindowPos(g_hdwp, window->hwnd, NULL, x, y, w, h, flags);
      } else {
        SetWindowPos(window->hwnd, NULL, x, y, w, h, flags);
      }
    }

    window->needs_move = false;
    window->needs_resize = false;
    return true;
  } else if (window->needs_move) {
    window_move(window, window->origin);
    window->needs_move = false;
    return false;
  }
  return false;
#else
  if (window->needs_resize || forced) {
    windows_freeze();
    CFTypeRef frame_region = window_create_region(window, window->frame);
    window->refc++;

    if (__builtin_available(macOS 26.0, *)) {
      SLSTransactionSetWindowShape(g_transaction, window->id,
                                                  window->origin.x,
                                                  window->origin.y,
                                                  frame_region);
      window_move(window, window->origin);
      if (SBSLSTransactionAddPostDecodeAction) {
        SBSLSTransactionAddPostDecodeAction(g_transaction, ^{
          window_defer_update(window);
        });
      } else window_defer_update(window);
    }
    else if (__builtin_available(macOS 13.0, *)) {
      // Ventura and later
      SLSSetWindowShape(g_connection, window->id,
                                      g_nirvana.x,
                                      g_nirvana.y,
                                      frame_region);
      window_clear_background(window);
      window_move(window, window->origin);
      window_defer_update(window);
    } else {
      // Monterey and previous
      if (window->parent) {
        SLSOrderWindow(g_connection, window->id, 0, window->parent->id);
      }

      SLSSetWindowShape(g_connection, window->id, 0, 0, frame_region);
      window_clear_background(window);

      if (window->parent) {
        window_order(window, window->parent, window->order_mode);
      }
      window_move(window, window->origin);
      window_defer_update(window);
    }

    surface_resize(window->surface, window);
    CFRelease(frame_region);

    window->needs_move = false;
    window->needs_resize = false;
    return true;
  } else if (window->needs_move) {
    window_move(window, window->origin);
    window->needs_move = false;
    return false;
  }
  return false;
#endif
}

void window_send_to_space(struct window* window, uint64_t dsid) {
#ifdef _WIN32
  // Virtual desktops are not modeled yet: the bar lives on the primary
  // desktop. Desktop-switch events arrive with workspace_win.c (S7).
  (void)window; (void)dsid;
#else
  CFArrayRef window_list = cfarray_of_cfnumbers(&window->id,
                                                sizeof(uint32_t),
                                                1,
                                                kCFNumberSInt32Type);

  SLSMoveWindowsToManagedSpace(g_connection, window_list, dsid);
  if (CGPointEqualToPoint(window->origin, g_nirvana)) {
    SLSMoveWindow(g_connection, window->id, &g_nirvana);
  }
  CFRelease(window_list);
#endif
}

void window_close(struct window* window) {
#ifdef _WIN32
  if (!window->hwnd) return;

  ShowWindow(window->hwnd, SW_HIDE);
  DestroyWindow(window->hwnd);
  window->hwnd = NULL;

  surface_destroy(window->surface);
  if (window->context) CGContextRelease(window->context);
  window_clear(window);
#else
  if (!window->id) return;

  SLSOrderWindow(g_connection, window->id, 0, 0);
  surface_destroy(window->surface);
  if (window->context) CGContextRelease(window->context);
  SLSReleaseWindow(g_connection, window->id);
  window_clear(window);
#endif
}

void window_set_level(struct window* window, uint32_t level) {
#ifdef _WIN32
  // No SLS-style levels on Windows; z-order is window_order only. Z-band
  // emulation for layered windows is an S7 follow-up.
  (void)window; (void)level;
#else
  windows_freeze();
  if (__builtin_available(macOS 14.0, *)) {
    // Sonoma and later
    SLSTransactionSetWindowLevel(g_transaction, window->id, level);
  } else {
    // Ventura and previous
    SLSSetWindowLevel(g_connection, window->id, level);
  }
#endif
}

void window_order(struct window* window, struct window* parent, int mode) {
#ifdef _WIN32
  window->parent = parent;
  if (mode != W_OUT) window->order_mode = mode;
  if (!window->hwnd) return;

  if (mode == W_OUT) {
    ShowWindow(window->hwnd, SW_HIDE);
    return;
  }

  // W_ABOVE with no parent keeps the window always-on-top (the bar's
  // overlay semantics); W_BELOW without a parent drops it to the bottom.
  HWND after = (parent && parent->hwnd) ? parent->hwnd
               : (mode == W_BELOW ? HWND_BOTTOM : HWND_TOPMOST);
  SetWindowPos(window->hwnd, after, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
  windows_freeze();
  window->parent = parent;
  if (mode != W_OUT) window->order_mode = mode;

  if (__builtin_available(macOS 14.0, *)) {
    // Sonoma and later
    SLSTransactionOrderWindow(g_transaction,
                              window->id,
                              mode,
                              parent ? parent->id : 0);
  } else {
    // Ventura and previous
    SLSOrderWindow(g_connection, window->id, mode, parent ? parent->id : 0);
  }
#endif
}

void window_assign_mouse_tracking_area(struct window* window, CGRect rect) {
#ifdef _WIN32
  // WM_MOUSEMOVE hit-testing arrives with the mouse adapter (S5).
  (void)window; (void)rect;
#else
  SLSRemoveAllTrackingAreas(g_connection, window->id);
  SLSAddTrackingRect(g_connection, window->id, rect);
#endif
}

void window_set_blur_radius(struct window* window, uint32_t blur_radius) {
#ifdef _WIN32
  if (!window->hwnd) return;

  // SetWindowCompositionAttribute is exported by user32 on Win10+; resolve
  // at runtime so older systems degrade to no blur instead of crashing.
  typedef enum {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLIC = 4,
  } ACCENT_STATE;
  typedef struct {
    ACCENT_STATE AccentState;
    int AccentFlags;
    int GradientColor; /* ABGR */
    int AnimationId;
  } ACCENT_POLICY_DEF;
  typedef struct {
    DWORD Attribute;
    void* Data;
    SIZE_T SizeOfData;
  } WINDOWCOMPOSITIONATTRIBDATA_DEF;
  typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA_DEF*);

  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  pfnSetWindowCompositionAttribute fn = user32
      ? (pfnSetWindowCompositionAttribute)GetProcAddress(
            user32, "SetWindowCompositionAttribute")
      : NULL;
  if (!fn) return;

  // Windows exposes NO per-window blur radius API (documented in
  // apply-progress.md, task 3.6): the radius value is carried in the
  // AccentFlags slot of the acrylic policy as a best-effort approximation.
  // blur_radius == 0 disables the backdrop, matching macOS.
  ACCENT_POLICY_DEF data = { 0 };
  data.AccentState = blur_radius > 0 ? ACCENT_ENABLE_ACRYLICBLURBEHIND
                                     : ACCENT_DISABLED;
  data.AccentFlags = (int)blur_radius;
  data.GradientColor = 0x00000000;  // no tint: blur-only, macOS-parity
  data.AnimationId = 0;

  const int WCA_ACCENT_POLICY = 19;
  WINDOWCOMPOSITIONATTRIBDATA_DEF composition = {
    WCA_ACCENT_POLICY, &data, sizeof(data)
  };
  fn(window->hwnd, &composition);
#else
  SLSSetWindowBackgroundBlurRadius(g_connection, window->id, blur_radius);
  if (window->context) window_clear_background(window);
#endif
}

void window_disable_shadow(struct window* window) {
#ifdef _WIN32
  // Layered tool windows never receive a drop shadow on Windows 10/11, so
  // there is nothing to disable (macOS shadow_density analog).
  (void)window;
#else
  CFIndex shadow_density = 0;
  CFNumberRef shadow_density_cf = CFNumberCreate(kCFAllocatorDefault,
                                                 kCFNumberCFIndexType,
                                                 &shadow_density      );

  const void *keys[1] = { CFSTR("com.apple.WindowShadowDensity") };
  const void *values[1] = { shadow_density_cf };
  CFDictionaryRef shadow_props_cf = CFDictionaryCreate(NULL,
                                             keys,
                                             values,
                                             1,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);

  SLSWindowSetShadowProperties(window->id, shadow_props_cf);
  CFRelease(shadow_density_cf);
  CFRelease(shadow_props_cf);
#endif
}

CGImageRef window_capture(struct window* window, bool* disabled) {
#ifdef _WIN32
  // g_disable_capture parity: macOS samples CLOCK_MONOTONIC_RAW_APPROX (ns)
  // and holds capture for ~1 s after a disable; Windows samples
  // GetTickCount64 (ms) with the same 1000 ms window. Negative locks off.
  if (g_disable_capture) {
    int64_t time = (int64_t)GetTickCount64();
    if (g_disable_capture < 0) {
      *disabled = true;
      return NULL;
    } else if (time - g_disable_capture > 1000) {
      g_disable_capture = 0;
    } else {
      *disabled = true;
      return NULL;
    }
  }

  *disabled = false;
  if (!window->hwnd) return NULL;

  RECT rect;
  if (!GetWindowRect(window->hwnd, &rect)) return NULL;
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return NULL;

  HDC screen = GetDC(NULL);
  HDC mem = CreateCompatibleDC(screen);

  BITMAPINFO bmi = { 0 };
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = height;  // positive -> bottom-up (GDI order)
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = NULL;
  HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  CGImageRef image = NULL;
  if (dib && bits) {
    HGDIOBJ old = SelectObject(mem, dib);
    BOOL ok = BitBlt(mem, 0, 0, width, height, screen, rect.left, rect.top,
                     SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    if (ok) {
      // GDI DIB rows are bottom-up; sk_image_from_bgra expects top-down, so
      // flip before wrapping (the DIB alpha channel is treated as opaque —
      // documented in apply-progress.md, task 3.7).
      size_t row_bytes = (size_t)width * 4u;
      uint8_t* topdown = malloc(row_bytes * (size_t)height);
      if (topdown) {
        for (int y = 0; y < height; ++y) {
          memcpy(topdown + (size_t)y * row_bytes,
                 (uint8_t*)bits + (size_t)(height - 1 - y) * row_bytes,
                 row_bytes);
        }
        image = (CGImageRef)sk_image_from_bgra(topdown,
                                               (uint32_t)width,
                                               (uint32_t)height);
        free(topdown);
      }
    }
    DeleteObject(dib);
  }
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
  return image;
#else
  if (g_disable_capture) {
    int64_t time = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX);
    if (g_disable_capture < 0) {
      *disabled = true;
      return NULL;
    } else if (time - g_disable_capture > (1ULL << 30)) {
      g_disable_capture = 0;
    } else {
      *disabled = true;
      return NULL;
    }
  }

  *disabled = false;
  CGImageRef image_ref = NULL;

  uint64_t wid = window->id;
  SLSCaptureWindowsContentsToRectWithOptions(g_connection,
                                             &wid,
                                             true,
                                             CGRectNull,
                                             1 << 8,
                                             &image_ref  );

  CGRect bounds;
  SLSGetScreenRectForWindow(g_connection, wid, &bounds);
  bounds.size.width = (uint32_t) (bounds.size.width + 0.5);
  window->frame.size = bounds.size;

  return image_ref;
#endif
}