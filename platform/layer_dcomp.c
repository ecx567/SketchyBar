// platform/layer_dcomp.c
//
// DirectComposition visual layer (S3: real implementation).
//
// Replaces src/layer.m (CAContext + CALayer). Each layer owns one
// IDCompositionVisual and one IDCompositionSurface:
//
//   - layer_set_bounds   -> visual SetOffsetX/Y (CG points x fixed 2.0 scale)
//   - layer_set_contents -> BeginDraw -> IDXGISurface::Map -> vertical-flip
//                           copy (the seam exports CG order, row 0 == bottom;
//                           DComp surfaces are top-down) -> Unmap -> EndDraw ->
//                           visual SetContent -> device Commit
//
// The D3D11 + DirectComposition device pair is a process singleton (like the
// macOS CAContext) created lazily on the first layer_create. D3D11CreateDevice
// prefers the hardware adapter and falls back to WARP (software rasterizer),
// so the layer also works in non-GPU / VM sessions.
//
// S3 scope (documented in apply-progress.md, task 3.3): the device, visual and
// surface are REAL, and the CPU upload path (BeginDraw/Map/EndDraw) is the one
// production uses; the visual tree is NOT attached to a window target yet —
// the window displays through UpdateLayeredWindow (window.c _WIN32 branch) and
// attaching the visual tree to an HWND target is the S7 hardware-composition
// follow-up.

#include "win_platform.h"
#include "layer.h"
#include "sk_backend.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>

#include <stdlib.h>
#include <string.h>

// Fixed CG user-space scale (mirrors SLSSetWindowResolution(..., 2.0f) on
// macOS and context_create(window->frame.size, 2.0f)). The layer surface is
// sized in DEVICE pixels = CG points x LAYER_SCALE. The dynamic-scale refactor
// lands with display_win.c (S7).
#define LAYER_SCALE 2.0f

struct layer {
  uint32_t context_id;
  IDCompositionVisual* visual;
  IDCompositionSurface* surface;
  LONG width;   // current surface size in device px (0 == not created)
  LONG height;
};

static IDCompositionDevice* g_device = NULL;

// Creates (once) the D3D11 + DirectComposition device pair. Returns S_OK when
// g_device is usable, E_FAIL otherwise. Never leaves a half-initialized
// device: every acquired COM object is released on failure.
static HRESULT dcomp_ensure_device(void) {
  if (g_device) return S_OK;

  const D3D_DRIVER_TYPE kDriverTypes[2] = {
    D3D_DRIVER_TYPE_HARDWARE,
    D3D_DRIVER_TYPE_WARP,   // software rasterizer: works headless / VM
  };

  for (size_t i = 0; i < 2; ++i) {
    ID3D11Device* d3d = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, kDriverTypes[i], NULL, 0,
                                   NULL, 0, D3D11_SDK_VERSION,
                                   &d3d, NULL, NULL);
    if (FAILED(hr) || !d3d) continue;

    IDXGIDevice* dxgi = NULL;
    hr = d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi);
    d3d->Release();
    if (FAILED(hr) || !dxgi) continue;

    hr = DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice),
                                  (void**)&g_device);
    dxgi->Release();
    if (SUCCEEDED(hr) && g_device) return S_OK;
  }
  return E_FAIL;
}

// (Re)creates the layer's composition surface at w x h device px. The surface
// is replaced in place so callers keep the same `layer` pointer; SetContent
// keeps the newest surface. IDCompositionSurface cannot change size.
static void layer_resize_surface(struct layer* layer, LONG w, LONG h) {
  if (w == layer->width && h == layer->height) return;
  if (w < 1) w = 1;
  if (h < 1) h = 1;

  IDCompositionSurface* surface = NULL;
  HRESULT hr = g_device->CreateSurface(w, h,
                                       DXGI_FORMAT_B8G8R8A8_UNORM,
                                       DXGI_ALPHA_MODE_PREMULTIPLIED,
                                       &surface);
  if (FAILED(hr) || !surface) return;

  if (layer->surface) layer->surface->Release();
  layer->surface = surface;
  layer->width = w;
  layer->height = h;
  layer->visual->SetContent(layer->surface);
}

struct layer* layer_create(uint32_t cid, CGRect bounds) {
  if (FAILED(dcomp_ensure_device())) return NULL;

  struct layer* layer = (struct layer*)calloc(1, sizeof(struct layer));
  if (!layer) return NULL;
  layer->context_id = cid;

  HRESULT hr = g_device->CreateVisual(&layer->visual);
  if (FAILED(hr) || !layer->visual) {
    free(layer);
    return NULL;
  }

  LONG w = (LONG)ceil(bounds.size.width * LAYER_SCALE);
  LONG h = (LONG)ceil(bounds.size.height * LAYER_SCALE);
  layer_resize_surface(layer, w, h);

  layer_set_bounds(layer, bounds);
  g_device->Commit();
  return layer;
}

void layer_destroy(struct layer* layer) {
  if (!layer) return;
  if (layer->visual)  layer->visual->Release();
  if (layer->surface) layer->surface->Release();
  free(layer);
}

uint32_t layer_get_context_id(struct layer* layer) {
  return layer ? layer->context_id : 0;
}

void layer_set_bounds(struct layer* layer, CGRect bounds) {
  if (!layer || !layer->visual) return;

  // The visual offset is the layer's position on the (future) window target.
  // S3 surfaces are bound at offset (0,0) once attached; the offset mirrors
  // the macOS CALayer frame translation so the semantics stay identical.
  layer->visual->SetOffsetX((float)(bounds.origin.x * LAYER_SCALE));
  layer->visual->SetOffsetY((float)(bounds.origin.y * LAYER_SCALE));

  LONG w = (LONG)ceil(bounds.size.width * LAYER_SCALE);
  LONG h = (LONG)ceil(bounds.size.height * LAYER_SCALE);
  layer_resize_surface(layer, w, h);
  g_device->Commit();
}

void layer_set_contents(struct layer* layer, CGImageRef image) {
  if (!layer || !layer->visual || !layer->surface || !image) return;

  size_t width = 0, height = 0;
  void* bgra = NULL;
  if (!sk_image_read_pixels((skbar_image*)image, &width, &height, &bgra)) return;

  LONG w = (LONG)width;
  LONG h = (LONG)height;
  layer_resize_surface(layer, w, h);

  POINT update_offset = { 0, 0 };
  IDXGISurface* dxgi = NULL;
  HRESULT hr = layer->surface->BeginDraw(NULL, __uuidof(IDXGISurface),
                                         (void**)&dxgi, &update_offset);
  if (FAILED(hr) || !dxgi) {
    free(bgra);
    return;
  }

  DXGI_MAPPED_RECT mapped = { 0 };
  hr = dxgi->Map(&mapped, DXGI_MAP_WRITE);
  if (SUCCEEDED(hr) && mapped.pBits) {
    // CG order (row 0 == bottom) -> DComp top-down (row 0 == top): copy the
    // rows in reverse so the destination lands upright. Skia export and the
    // composition surface are both premultiplied BGRA32, so this is the only
    // transform needed.
    const size_t row_bytes = width * 4u;
    uint8_t* dst = (uint8_t*)mapped.pBits
                 + (size_t)update_offset.y * mapped.Pitch
                 + (size_t)update_offset.x * 4u;
    for (size_t y = 0; y < height; ++y) {
      const uint8_t* src_row = (const uint8_t*)bgra + (height - 1u - y) * row_bytes;
      memcpy(dst + y * mapped.Pitch, src_row, row_bytes);
    }
    dxgi->Unmap();
  }
  dxgi->Release();
  layer->surface->EndDraw();

  g_device->Commit();
  free(bgra);
}