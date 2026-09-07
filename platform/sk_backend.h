#pragma once
// platform/sk_backend.h
//
// The Skia graphics seam for the native Windows port.
//
// This header defines the raw backend surface every portable drawing routine
// reaches on Windows. The portable core is C and must stay C99; Skia is C++.
// The seam keeps those two worlds apart:
//
//   - src/context.c (Windows branch) implements the CG-named shims
//     (CGContextSaveGState, CGContextDrawPath, ...) declared in
//     platform/win_graphics_types.h on top of the functions below.
//   - platform/sk_backend_skia.cpp implements this surface with real Skia.
//   - platform/sk_backend_stub.c implements it as a fail-closed stub so the
//     gate compiles in CI without a Skia SDK (SKIA_DIR unset): every creation
//     returns NULL and the parity harness exits non-zero with a documented
//     message. The graphics gate is therefore always honest: "unproven"
//     until a Skia SDK is configured.
//
// All sk_ functions are extern "C" and never throw. Structs are opaque so no
// Skia header ever leaks into the portable C core.

#include "win_graphics_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- object identity (drives the CFRelease dispatch in sk_backend_common.c) -
// Every backend object begins with `struct skbar_object base;` so CFRelease /
// CGImageRelease / CGPathRelease can route to the right destructor without
// the portable core knowing concrete types.
typedef enum skbar_kind {
  SKBAR_KIND_CONTEXT = 0x534B4354, /* 'SKCT' */
  SKBAR_KIND_FONT   = 0x534B464E, /* 'SKFN' */
  SKBAR_KIND_LINE   = 0x534B4C4E, /* 'SKLN' */
  SKBAR_KIND_IMAGE  = 0x534B494D, /* 'SKIM' */
  SKBAR_KIND_PATH   = 0x534B5041, /* 'SKPA' */
  SKBAR_KIND_DATA   = 0x534B4441, /* 'SKDA' */
} skbar_kind;

struct skbar_object {
  uint32_t magic;
  skbar_kind kind;
};

// --- backend identity --------------------------------------------------------
const char* sk_context_backend_name(void);

// --- context (a skbar_context wraps one SkBitmap-backed SkCanvas) -----------
// pixel_width/pixel_height are in DEVICE pixels; scale is the CG-style user
// space scale (retina). The backend embeds the Y-flip + scale into the canvas
// matrix so user-space coordinates match macOS CoreGraphics (origin bottom-left).
skbar_context* sk_context_create(uint32_t pixel_width,
                                 uint32_t pixel_height,
                                 CGFloat scale,
                                 bool font_smoothing);
void sk_context_destroy(skbar_context* context);

void sk_context_save(skbar_context* context);
void sk_context_restore(skbar_context* context);

void sk_context_set_fill_rgba(skbar_context* context, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
void sk_context_set_stroke_rgba(skbar_context* context, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
void sk_context_set_line_width(skbar_context* context, CGFloat width);
void sk_context_set_blend_mode(skbar_context* context, CGBlendMode mode);
void sk_context_set_allows_font_smoothing(skbar_context* context, bool enables);
void sk_context_set_interpolation_none(skbar_context* context, bool none);
void sk_context_set_text_position(skbar_context* context, CGFloat x, CGFloat y);

// CG semantics: AddPath appends to the implicit current path; Clip intersects
// the clip region with that path; DrawPath strokes/fills the current path.
void sk_context_add_path(skbar_context* context, const skbar_path* path);
void sk_context_clip(skbar_context* context);
void sk_context_draw_path(skbar_context* context, CGPathDrawingMode mode);
void sk_context_draw_image(skbar_context* context, CGRect rect, const skbar_image* image);
void sk_context_draw_line(skbar_context* context, const skbar_line* line);

// Pixels for the parity harness: BGRA premultiplied, bottom-left == row 0
// (matches CGBitmapContextCreate with kCGImageAlphaPremultipliedFirst |
// kCGBitmapByteOrder32Host). Caller frees *out_bgra with free().
bool sk_context_read_pixels(skbar_context* context, size_t* out_width, size_t* out_height, void** out_bgra);

// Encodes the canvas as PNG (golden files). Returns false on failure.
bool sk_context_write_png(skbar_context* context, const char* path);

// --- paths --------------------------------------------------------------------
skbar_path* sk_path_create(void);
void sk_path_add_rect(skbar_path* path, CGRect rect);
void sk_path_add_rounded_rect(skbar_path* path, CGRect rect, CGFloat corner_width, CGFloat corner_height);
void sk_path_destroy(skbar_path* path);

// --- fonts --------------------------------------------------------------------
// family/style mirror the macOS descriptor (e.g. family="Arial", style="Bold",
// size=14). feature_tags are 4-char OpenType feature tags ("liga", "tnum"...)
// resolved by src/font.c from the `features` config string. The backend keeps
// them as SkShaper::Feature during shape.
skbar_font* sk_font_create(const char* family,
                           const char* style,
                           float size,
                           const char feature_tags[][5],
                           size_t feature_count);
void sk_font_destroy(skbar_font* font);
double sk_font_ascent(skbar_font* font);
double sk_font_descent(skbar_font* font);

// Registers an external font file. On Windows this is a documented no-op in
// both backends until the Skia font-collection path lands (S7 follow-up);
// system fonts resolve through DirectWrite-backed SkFontMgr.
void sk_font_register(const char* font_path);

// --- text lines ---------------------------------------------------------------
// A skbar_line is a shaped text blob. Returns NULL if shaping failed.
skbar_line* sk_line_create(skbar_font* font, const char* text, size_t length);
void sk_line_destroy(skbar_line* line);
CGRect sk_line_get_bounds(skbar_line* line, CTLineBoundsOptions options);

// --- images -------------------------------------------------------------------
// decode/icon return NULL on failure. All returned images are refcounted:
// sk_image_retain bumps, sk_image_unref drops (NULL-safe in the stub).
skbar_image* sk_image_decode_file(const char* path);
skbar_image* sk_icon_for_app(const char* app);
skbar_image* sk_image_retain(skbar_image* image);
void sk_image_unref(skbar_image* image);
uint32_t sk_image_width(skbar_image* image);
uint32_t sk_image_height(skbar_image* image);

// Rasterizes the image into a CG-order BGRA buffer (row 0 = bottom, see
// sk_context_read_pixels). Returns false when the image cannot be rasterized.
// Caller frees *out_bgra with free().
bool sk_image_read_pixels(skbar_image* image, size_t* out_width, size_t* out_height, void** out_bgra);

// Borrows the ORIGINAL ENCODED SOURCE BYTES as a data blob (equivalent to
// CGImageGetDataProvider: image.c copies it via CGDataProviderCopyData and
// compares bytes to dedupe identical files). Borrowed, no retain.
skbar_data* sk_image_source_data(skbar_image* image);

// --- data blobs (mirror CFData for image.c's dedup path) ----------------------
skbar_data* sk_data_create_copy(const void* bytes, size_t length);
size_t sk_data_length(skbar_data* data);
const void* sk_data_bytes(skbar_data* data);
void sk_data_release(skbar_data* data);

#ifdef __cplusplus
}
#endif