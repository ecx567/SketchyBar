#include "context.h"
#ifdef _WIN32
#include "../platform/sk_backend.h"
#else
#include "animation.h"
#include "bar_manager.h"
#endif

#ifdef _WIN32
CGContextRef context_create(CGSize size, CGFloat scale) {
  size_t pixel_width = (size_t)ceil(size.width * scale);
  size_t pixel_height = (size_t)ceil(size.height * scale);
  if (pixel_width == 0 || pixel_height == 0) return NULL;

  // The Windows bar_manager (S4) owns the font_smoothing preference; until it
  // lands, the seam uses the macOS default (true).
  return (CGContextRef) sk_context_create((uint32_t)pixel_width,
                                          (uint32_t)pixel_height,
                                          scale,
                                          true);
}

// -----------------------------------------------------------------------------
// CGContext shims on top of the Skia seam.
// These give the portable drawing routines (helpers.h clip_rect, image_draw,
// and later text_draw) the exact CGContext API they call on macOS, backed by
// sk_context_* on Windows. They are the only CG-named entry points the core
// needs; everything below is extern "C" from sk_backend.h.
// -----------------------------------------------------------------------------

void CGContextSaveGState(CGContextRef context) {
  sk_context_save(context);
}

void CGContextRestoreGState(CGContextRef context) {
  sk_context_restore(context);
}

void CGContextClip(CGContextRef context) {
  sk_context_clip(context);
}

void CGContextAddPath(CGContextRef context, CGPathRef path) {
  sk_context_add_path(context, path);
}

void CGContextDrawPath(CGContextRef context, CGPathDrawingMode mode) {
  sk_context_draw_path(context, mode);
}

void CGContextSetRGBFillColor(CGContextRef context, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  sk_context_set_fill_rgba(context, r, g, b, a);
}

void CGContextSetRGBStrokeColor(CGContextRef context, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  sk_context_set_stroke_rgba(context, r, g, b, a);
}

void CGContextSetLineWidth(CGContextRef context, CGFloat width) {
  sk_context_set_line_width(context, width);
}

void CGContextSetBlendMode(CGContextRef context, CGBlendMode mode) {
  sk_context_set_blend_mode(context, mode);
}

void CGContextSetAllowsFontSmoothing(CGContextRef context, bool allows) {
  sk_context_set_allows_font_smoothing(context, allows);
}

void CGContextSetInterpolationQuality(CGContextRef context, CGInterpolationQuality quality) {
  sk_context_set_interpolation_none(context, quality == kCGInterpolationNone);
}

void CGContextSetTextPosition(CGContextRef context, CGFloat x, CGFloat y) {
  sk_context_set_text_position(context, x, y);
}

void CGContextDrawImage(CGContextRef context, CGRect rect, CGImageRef image) {
  sk_context_draw_image(context, rect, image);
}

CGMutablePathRef CGPathCreateMutable(void) {
  return (CGMutablePathRef)sk_path_create();
}

void CGPathAddRect(CGMutablePathRef path, const void* transform, CGRect rect) {
  (void)transform;
  sk_path_add_rect(path, rect);
}

void CGPathAddRoundedRect(CGMutablePathRef path, const void* transform, CGRect rect,
                          CGFloat corner_width, CGFloat corner_height) {
  (void)transform;
  sk_path_add_rounded_rect(path, rect, corner_width, corner_height);
}

#else
CGContextRef context_create(CGSize size, CGFloat scale) {
  size_t pixel_width = (size_t)ceil(size.width * scale);
  size_t pixel_height = (size_t)ceil(size.height * scale);
  if (pixel_width == 0 || pixel_height == 0) return NULL;

  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(NULL,
                                               pixel_width,
                                               pixel_height,
                                               8,
                                               pixel_width * 4,
                                               color_space,
                                               kCGImageAlphaPremultipliedFirst
                                               | kCGBitmapByteOrder32Host);
  CGColorSpaceRelease(color_space);
  if (!context) return NULL;

  CGContextScaleCTM(context, scale, scale);
  CGContextSetInterpolationQuality(context, kCGInterpolationNone);
  CGContextSetAllowsFontSmoothing(context, g_bar_manager.font_smoothing);
  return context;
}
#endif
