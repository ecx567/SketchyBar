// platform/sk_backend_stub.c
//
// Fail-closed stub backend for the Skia graphics seam.
//
// This implementation is compiled when no Skia SDK is available (SKIA_DIR /
// SKIA_INCLUDE_DIR unset) — i.e. the default CI configuration. It exists so
// the graphics seam and the bitmap_diff parity harness compile and link on
// Windows WITHOUT Skia, while being structurally incapable of passing the
// graphics gate:
//
//   - every object-creating function returns NULL
//   - every mutating operation is a no-op
//   - sk_context_read_pixels / sk_context_write_png return false
//     (read_pixels also reports the backend name via the harness message)
//
// The parity harness (tests/bitmap_diff_main.c) treats sk_context_create ==
// NULL as a hard failure with the documented message:
//
//   "skia backend unavailable (SKIA_DIR not configured); parity gate FAIL-CLOSED"
//
// The gate therefore cannot silently pass: without a real Skia SDK the result
// is always an explicit non-zero exit. This is the sanctioned behavior for CI
// until a Skia SDK is vendored (see openspec/changes/windows-port/apply-progress.md).

#include "sk_backend.h"

#include <stdlib.h>
#include <string.h>

const char* sk_context_backend_name(void) {
  return "stub";
}

skbar_context* sk_context_create(uint32_t pixel_width, uint32_t pixel_height,
                                 CGFloat scale, bool font_smoothing) {
  (void)pixel_width; (void)pixel_height; (void)scale; (void)font_smoothing;
  return NULL;
}

void sk_context_destroy(skbar_context* context) { (void)context; }
void sk_context_save(skbar_context* context)    { (void)context; }
void sk_context_restore(skbar_context* context) { (void)context; }
void sk_context_set_fill_rgba(skbar_context* c, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  (void)c; (void)r; (void)g; (void)b; (void)a;
}
void sk_context_set_stroke_rgba(skbar_context* c, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  (void)c; (void)r; (void)g; (void)b; (void)a;
}
void sk_context_set_line_width(skbar_context* context, CGFloat width) {
  (void)context; (void)width;
}
void sk_context_set_blend_mode(skbar_context* context, CGBlendMode mode) {
  (void)context; (void)mode;
}
void sk_context_set_allows_font_smoothing(skbar_context* context, bool enables) {
  (void)context; (void)enables;
}
void sk_context_set_interpolation_none(skbar_context* context, bool none) {
  (void)context; (void)none;
}
void sk_context_set_text_position(skbar_context* context, CGFloat x, CGFloat y) {
  (void)context; (void)x; (void)y;
}
void sk_context_add_path(skbar_context* context, const skbar_path* path) {
  (void)context; (void)path;
}
void sk_context_clip(skbar_context* context) { (void)context; }
void sk_context_draw_path(skbar_context* context, CGPathDrawingMode mode) {
  (void)context; (void)mode;
}
void sk_context_draw_image(skbar_context* context, CGRect rect, const skbar_image* image) {
  (void)context; (void)rect; (void)image;
}
void sk_context_draw_line(skbar_context* context, const skbar_line* line) {
  (void)context; (void)line;
}

bool sk_context_read_pixels(skbar_context* context, size_t* out_width,
                            size_t* out_height, void** out_bgra) {
  (void)context;
  if (out_width) *out_width = 0;
  if (out_height) *out_height = 0;
  *out_bgra = NULL;
  return false;
}

bool sk_context_write_png(skbar_context* context, const char* path) {
  (void)context; (void)path;
  return false;
}

skbar_path* sk_path_create(void) { return NULL; }
void sk_path_add_rect(skbar_path* path, CGRect rect) { (void)path; (void)rect; }
void sk_path_add_rounded_rect(skbar_path* path, CGRect rect, CGFloat corner_width, CGFloat corner_height) {
  (void)path; (void)rect; (void)corner_width; (void)corner_height;
}
void sk_path_destroy(skbar_path* path) { (void)path; }

skbar_font* sk_font_create(const char* family, const char* style, float size,
                           const char feature_tags[][5], size_t feature_count) {
  (void)family; (void)style; (void)size; (void)feature_tags; (void)feature_count;
  return NULL;
}
void sk_font_destroy(skbar_font* font) { (void)font; }
double sk_font_ascent(skbar_font* font)  { (void)font; return 0.0; }
double sk_font_descent(skbar_font* font) { (void)font; return 0.0; }
void sk_font_register(const char* font_path) { (void)font_path; }

skbar_line* sk_line_create(skbar_font* font, const char* text, size_t length) {
  (void)font; (void)text; (void)length;
  return NULL;
}
void sk_line_destroy(skbar_line* line) { (void)line; }
CGRect sk_line_get_bounds(skbar_line* line, CTLineBoundsOptions options) {
  (void)line; (void)options;
  return CGRectZero;
}

skbar_image* sk_image_decode_file(const char* path) { (void)path; return NULL; }
skbar_image* sk_icon_for_app(const char* app)        { (void)app; return NULL; }
skbar_image* sk_image_retain(skbar_image* image)     { return image; }
void sk_image_unref(skbar_image* image)              { (void)image; }
uint32_t sk_image_width(skbar_image* image)          { (void)image; return 0; }
uint32_t sk_image_height(skbar_image* image)         { (void)image; return 0; }
bool sk_image_read_pixels(skbar_image* image, size_t* out_width, size_t* out_height, void** out_bgra) {
  (void)image;
  if (out_width) *out_width = 0;
  if (out_height) *out_height = 0;
  *out_bgra = NULL;
  return false;
}
skbar_data* sk_image_source_data(skbar_image* image) { (void)image; return NULL; }

skbar_data* sk_data_create_copy(const void* bytes, size_t length) {
  (void)bytes; (void)length;
  return NULL;
}
size_t sk_data_length(skbar_data* data)    { (void)data; return 0; }
const void* sk_data_bytes(skbar_data* data) { (void)data; return NULL; }
void sk_data_release(skbar_data* data)      { (void)data; }