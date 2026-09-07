#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
// On Windows, layer.h wraps a DirectComposition visual. The geometry types
// come from the Skia seam (win_graphics_types.h) via context.h. The layer
// struct itself is opaque — its members live in platform/layer_dcomp.c.
#include "../platform/win_graphics_types.h"
#else
#include <CoreGraphics/CoreGraphics.h>

typedef void* CALayerRef;
typedef void* CAContextRef;

struct layer {
  CAContextRef context;
  CALayerRef root;
};
#endif

struct layer* layer_create(uint32_t cid, CGRect bounds);
void layer_destroy(struct layer* layer);

uint32_t layer_get_context_id(struct layer* layer);
void layer_set_bounds(struct layer* layer, CGRect bounds);
void layer_set_contents(struct layer* layer, CGImageRef image);

#ifdef __cplusplus
}
#endif
