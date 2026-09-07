#include "surface.h"
#include "misc/helpers.h"
#include "window.h"
#include "layer.h"
#ifdef _WIN32
#include "../platform/sk_backend.h"
#endif

void surface_destroy(struct surface* surface) {
  if (!surface) return;
#ifdef _WIN32
  // Windows: the layer is a DirectComposition visual (no SLS surface to
  // remove); the context is the Skia canvas.
  if (surface->layer) layer_destroy(surface->layer);
  if (surface->context) CGContextRelease(surface->context);
#else
  if (surface->id) SLSRemoveSurface(g_connection, surface->wid, surface->id);
  if (surface->layer) layer_destroy(surface->layer);
  if (surface->context) CGContextRelease(surface->context);
#endif
  free(surface);
}

struct surface* surface_create(struct window* window) {
#ifdef _WIN32
  if (!window->id) return NULL;

  struct surface* surface = malloc(sizeof(struct surface));
  if (!surface) return NULL;
  memset(surface, 0, sizeof(struct surface));

  surface->wid = window->id;
  // DirectComposition layer (layer_dcomp.c). A NULL layer is tolerated: the
  // window still displays through UpdateLayeredWindow (window.c flush path).
  surface->layer = layer_create(0, window->frame);
  surface->context = context_create(window->frame.size, 2.0f);
  return surface;
#else
  if (!window->id) return NULL;

  struct surface* surface = malloc(sizeof(struct surface));
  if (!surface) return NULL;
  memset(surface, 0, sizeof(struct surface));

  surface->wid = window->id;
  surface->layer = layer_create(g_connection, window->frame);

  if (SLSAddSurface(g_connection, window->id, &surface->id) != kCGErrorSuccess
      || surface->id == 0) {
    surface_destroy(surface);
    return NULL;
  }

  SLSBindSurface(g_connection, window->id,
                               surface->id,
                               0x4,
                               0,
                               layer_get_context_id(surface->layer));

  SLSSetSurfaceBounds(g_connection, window->id, surface->id, window->frame);
  SLSSetSurfaceResolution(g_connection, window->id, surface->id, 2.0);
  SLSSetSurfaceOpacity(g_connection, window->id, surface->id, false);

  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  SLSSetSurfaceColorSpace(g_connection, window->id, surface->id, color_space);
  CGColorSpaceRelease(color_space);

  SLSOrderSurface(g_connection, window->id, surface->id, W_ABOVE, 0);
  SLSFlushSurface(g_connection, window->id, surface->id, 0);

  surface->context = context_create(window->frame.size, 2.0f);
  return surface;
#endif
}

void surface_resize(struct surface* surface, struct window* window) {
#ifdef _WIN32
  if (!surface) return;
  // Windows: recreate the Skia canvas and re-target the layer bounds; the
  // deferred HWND resize happens in window_apply_frame (window.c).
  if (surface->context) CGContextRelease(surface->context);
  surface->context = context_create(window->frame.size, 2.0f);
  if (surface->layer) layer_set_bounds(surface->layer, window->frame);
#else
  if (surface->context) CGContextRelease(surface->context);
  surface->context = context_create(window->frame.size, 2.0f);

  if (__builtin_available(macOS 26.0, *)) {
    SLSTransactionSetSurfaceBounds(g_transaction, window->id,
                                                  surface->id,
                                                  window->frame);
    // On macOS 26+ layer_set_bounds is defered to the post decode action
    // scheculed in window.c
  } else {
    SLSSetSurfaceBounds(g_connection, window->id, surface->id, window->frame);
    layer_set_bounds(surface->layer, window->frame);
  }
#endif
}

void surface_flush(struct surface* surface) {
#ifdef _WIN32
  // Windows: snapshot the canvas and upload it to the DirectComposition
  // layer (the ULW display copy happens in window_flush, window.c).
  if (!surface || !surface->context || !surface->layer) return;
  skbar_image* content = sk_context_snapshot(surface->context);
  if (!content) return;
  layer_set_contents(surface->layer, content);
  sk_image_unref(content);
#else
  if (!surface->context) return;
  CGImageRef content = CGBitmapContextCreateImage(surface->context);
  if (!content) return;

  layer_set_contents(surface->layer, content);
  SLSFlushSurface(g_connection, surface->wid, surface->id, 0);
  CGImageRelease(content);
#endif
}