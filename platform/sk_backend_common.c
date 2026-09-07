// platform/sk_backend_common.c
//
// Backend-agnostic glue for the Skia graphics seam: the CF/CG-named helpers the
// portable core calls (CFRelease, CGImageRelease, CGPathRelease, CFData*,
// CGImageGet*...) are implemented once here on top of the opaque sk_* surface.
// Both backends (skia / stub) link this file, so the portable C code observes
// the exact same CG-emulation behavior regardless of which backend is built.
//
// Only compiled in the Windows build (_WIN32); macOS keeps real CoreGraphics.

#include "sk_backend.h"

#include <stdlib.h>
#include <string.h>

// --- object release dispatch ---------------------------------------------------
// macOS CFRelease(NULL) is a crash; on Windows the stub backend legitimately
// produces NULL objects, so the seam is deliberately NULL-safe. This is a
// documented, Windows-only fail-safe (image.c calls CGImageRelease on possibly
// unset refs; font.c guards with if (ct_font)).
void CFRelease(void* obj) {
  if (!obj) return;

  struct skbar_object* base = (struct skbar_object*)obj;
  switch (base->kind) {
    case SKBAR_KIND_CONTEXT: sk_context_destroy((skbar_context*)obj); break;
    case SKBAR_KIND_FONT:    sk_font_destroy((skbar_font*)obj);       break;
    case SKBAR_KIND_LINE:    sk_line_destroy((skbar_line*)obj);       break;
    case SKBAR_KIND_IMAGE:   sk_image_unref((skbar_image*)obj);       break;
    case SKBAR_KIND_PATH:    sk_path_destroy((skbar_path*)obj);       break;
    case SKBAR_KIND_DATA:    sk_data_release((skbar_data*)obj);       break;
    default: break;
  }
}

void CGImageRelease(CGImageRef image) {
  CFRelease((void*)image);
}

// CGContextRelease mirrors CGImageRelease: a skbar_context IS the CGContextRef
// (its destructor releases the backing SkSurface). Called by window_close /
// surface_resize on Windows.
void CGContextRelease(CGContextRef context) {
  CFRelease((void*)context);
}

void CGPathRelease(CGPathRef path) {
  CFRelease((void*)path);
}

// --- CFData emulation -----------------------------------------------------------
size_t CFDataGetLength(CFDataRef data) {
  return sk_data_length((skbar_data*)data);
}

const void* CFDataGetBytePtr(CFDataRef data) {
  return sk_data_bytes((skbar_data*)data);
}

// --- CGImage helpers ------------------------------------------------------------
uint32_t CGImageGetWidth(CGImageRef image) {
  return sk_image_width((skbar_image*)image);
}

uint32_t CGImageGetHeight(CGImageRef image) {
  return sk_image_height((skbar_image*)image);
}

// image_copy(): macOS makes a deep copy; here a shared refcount is equivalent
// because the image is immutable. Same pointer returned, refcount bumped.
CGImageRef CGImageCreateCopy(CGImageRef image) {
  return (CGImageRef)sk_image_retain((skbar_image*)image);
}

// The "data provider" of a decoded image is its original encoded source bytes.
// GetDataProvider borrows the internal blob (no retain); CopyData duplicates it
// (matches CGDataProviderCopyData ownership: caller must CFRelease).
CGDataProviderRef CGImageGetDataProvider(CGImageRef image) {
  return (CGDataProviderRef)sk_image_source_data((skbar_image*)image);
}

CFDataRef CGDataProviderCopyData(CGDataProviderRef provider) {
  return (CFDataRef)sk_data_create_copy(sk_data_bytes((skbar_data*)provider),
                                        sk_data_length((skbar_data*)provider));
}

// --- text metrics / drawing (consumed by src/text.c from S3) --------------------
void CTLineDraw(CTLineRef line, CGContextRef context) {
  sk_context_draw_line((skbar_context*)context, (skbar_line*)line);
}

CGRect CTLineGetBoundsWithOptions(CTLineRef line, CTLineBoundsOptions options) {
  return sk_line_get_bounds((skbar_line*)line, options);
}

void CTLineGetTypographicBounds(CTLineRef line, CGFloat* ascent, CGFloat* descent, CGFloat* leading) {
  CGRect bounds = sk_line_get_bounds((skbar_line*)line, 0);
  if (ascent)  *ascent = bounds.size.height;
  if (descent) *descent = 0;
  if (leading) *leading = 0;
}

double CTFontGetAscent(CTFontRef font) {
  return sk_font_ascent((skbar_font*)font);
}

double CTFontGetDescent(CTFontRef font) {
  return sk_font_descent((skbar_font*)font);
}