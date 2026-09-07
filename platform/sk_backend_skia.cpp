// platform/sk_backend_skia.cpp
//
// The real Skia implementation of the sk_* graphics seam (see sk_backend.h).
//
// BUILD GATE: this file is only compiled when CMake finds a Skia SDK
// (SKIA_INCLUDE_DIR resolves include/core/SkCanvas.h). CI without a SDK builds
// sk_backend_stub.c instead, so this translation unit is compile-gated —
// it is verified on a machine that configures SKIA_DIR, not in this repo's
// stub CI. API surface pinned to the long-lived Skia core+shaper headers
// (SkImages::DeferredFromEncodedData, SkSurfaces::Raster, SkShaper::Feature);
// if a Skia bump breaks a symbol here, fix it in the SAME commit that bumps
// the vendored SDK revision.
//
// Coordinate model: the canvas matrix embeds the CG user-space transform
// (origin bottom-left, Y up) so the portable drawing core can render with
// macOS coordinates unchanged:
//
//   [x']   [ scale  0   0 ] [x]
//   [y'] = [ 0    -scale H ] [y]      H = pixel height (y' = H - scale*y)
//   [1 ]   [ 0      0   1 ] [1]
//
// The backing pixels stay top-down (CUDA/GPU/DIB convention), but every API
// that EXPORTS pixels to the harness (sk_context_read_pixels,
// sk_image_read_pixels) hands back CG order (row 0 == bottom) so buffers can
// be memcmp'd against CGBitmapContextCreate memory.

#include "sk_backend.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkTypeface_win.h"
#include "modules/skshaper/include/SkShaper.h"

#include <shobjidl_core.h>
#include <shlguid.h>  // BHID_SFUIObject (thumbnail/icon shell binder)

namespace {

constexpr uint32_t kMagic = 0x5A5A5A5Au;

// A CGContext-replaceable draw state. sk_context_save/restore stack these the
// same way CG does (full graphics state), in addition to SkCanvas save/restore.
struct SKBarDrawState {
  SkPaint paint;
  SkPoint text_position = SkPoint::Make(0.f, 0.f);
  CGBlendMode blend_mode = kCGBlendModeNormal;
  bool font_smoothing = true;
  bool interpolation_none = false;
  // SkPath is a value type (not ref-counted), so no sk_sp<SkPath>. has_*
  // tracks the "no current path" state that sk_context_add_path(0) clears.
  SkPath current_path;
  bool has_current_path = false;
};

SkBlendMode to_sk_blend(CGBlendMode mode) {
  switch (mode) {
    case kCGBlendModeMultiply:        return SkBlendMode::kMultiply;
    case kCGBlendModeScreen:          return SkBlendMode::kScreen;
    case kCGBlendModeOverlay:         return SkBlendMode::kOverlay;
    case kCGBlendModeDarken:          return SkBlendMode::kDarken;
    case kCGBlendModeLighten:         return SkBlendMode::kLighten;
    case kCGBlendModeColorDodge:      return SkBlendMode::kColorDodge;
    case kCGBlendModeColorBurn:       return SkBlendMode::kColorBurn;
    case kCGBlendModeSoftLight:       return SkBlendMode::kSoftLight;
    case kCGBlendModeHardLight:       return SkBlendMode::kHardLight;
    case kCGBlendModeDifference:      return SkBlendMode::kDifference;
    case kCGBlendModeExclusion:       return SkBlendMode::kExclusion;
    case kCGBlendModeHue:             return SkBlendMode::kHue;
    case kCGBlendModeSaturation:      return SkBlendMode::kSaturation;
    case kCGBlendModeColor:           return SkBlendMode::kColor;
    case kCGBlendModeLuminosity:      return SkBlendMode::kLuminosity;
    case kCGBlendModeClear:           return SkBlendMode::kClear;
    case kCGBlendModeCopy:            return SkBlendMode::kSrc;
    case kCGBlendModeSourceIn:        return SkBlendMode::kSrcIn;
    case kCGBlendModeSourceOut:       return SkBlendMode::kSrcOut;
    case kCGBlendModeSourceAtop:      return SkBlendMode::kSrcATop;
    case kCGBlendModeDestinationOver: return SkBlendMode::kDstOver;
    case kCGBlendModeDestinationIn:   return SkBlendMode::kDstIn;
    case kCGBlendModeDestinationOut:  return SkBlendMode::kDstOut;
    case kCGBlendModeDestinationAtop: return SkBlendMode::kDstATop;
    case kCGBlendModeXOR:             return SkBlendMode::kXor;
    // kCGBlendModePlusDarker cannot be expressed exactly in Skia; kPlus is the
    // documented approximation (parity delta is surfaced by the harness).
    case kCGBlendModePlusDarker:      return SkBlendMode::kPlus;
    case kCGBlendModePlusLighter:     return SkBlendMode::kPlus;
    case kCGBlendModeNormal:
    default:                          return SkBlendMode::kSrcOver;
  }
}

SkFontStyle to_sk_style(const char* style) {
  // Pragmatic subset of the macOS font-style names SketchyBar passes through.
  // Weight 700 == CoreText's Bold trait; widths default to normal.
  bool bold   = style && (std::strstr(style, "Bold") || std::strstr(style, "Demibold") || std::strstr(style, "Semi"));
  bool italic = style && (std::strstr(style, "Italic") || std::strstr(style, "Oblique"));
  int weight  = bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight;
SkFontStyle::Slant slant = italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant;
  return SkFontStyle(weight, SkFontStyle::kNormal_Width, slant);
}

// CG order == bottom row first; helper flips a top-down buffer in place.
void flip_vertical(uint8_t* bgra, uint32_t width, uint32_t height) {
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  uint8_t* scratch = static_cast<uint8_t*>(std::malloc(row_bytes));
  if (!scratch) return;
  for (uint32_t y = 0; y < height / 2u; ++y) {
    uint8_t* a = bgra + static_cast<size_t>(y) * row_bytes;
    uint8_t* b = bgra + static_cast<size_t>(height - 1u - y) * row_bytes;
    std::memcpy(scratch, a, row_bytes);
    std::memcpy(a, b, row_bytes);
    std::memcpy(b, scratch, row_bytes);
  }
  std::free(scratch);
}

}  // namespace

struct skbar_context {
  skbar_object base;
  sk_sp<SkSurface> surface;
  SkCanvas* canvas = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<SKBarDrawState> states;  // index 0 is the initial state
};

struct skbar_path {
  skbar_object base;
  SkPath path;
};

struct skbar_font {
  skbar_object base;
  sk_sp<SkTypeface> typeface;
  float size = 0.f;
  bool font_smoothing = true;
  std::vector<SkShaper::Feature> features;
};

struct skbar_line {
  skbar_object base;
  sk_sp<SkTextBlob> blob;
  CGRect bounds;
};

struct skbar_data {
  skbar_object base;
  size_t length = 0;
  uint8_t* bytes = nullptr;
};

struct skbar_image {
  skbar_object base;
  sk_sp<SkImage> image;
  sk_sp<SkData> source;   // original encoded bytes (ref'd)
  skbar_data* source_view = nullptr;  // lazily created; owned by this struct
  uint32_t width = 0;
  uint32_t height = 0;
  std::atomic<int32_t> refcount{1};
};

extern "C" {

const char* sk_context_backend_name(void) { return "skia"; }

// --- context ------------------------------------------------------------------

skbar_context* sk_context_create(uint32_t pixel_width,
                                 uint32_t pixel_height,
                                 CGFloat scale,
                                 bool font_smoothing) {
  if (pixel_width == 0 || pixel_height == 0) return nullptr;
  auto* ctx = new skbar_context();
  ctx->base.magic = kMagic;
  ctx->base.kind = SKBAR_KIND_CONTEXT;
  ctx->width = pixel_width;
  ctx->height = pixel_height;

  SkImageInfo info = SkImageInfo::MakeN32Premul(static_cast<int>(pixel_width),
                                                static_cast<int>(pixel_height));
  ctx->surface = SkSurfaces::Raster(info);
  if (!ctx->surface) {
    delete ctx;
    return nullptr;
  }
  ctx->canvas = ctx->surface->getCanvas();

  // CG user space: origin bottom-left, Y up; device space is top-down.
  SkMatrix cg;
  cg.setAll(static_cast<SkScalar>(scale), 0, 0,
            0, -static_cast<SkScalar>(scale), static_cast<SkScalar>(pixel_height),
            0, 0, 1);
  ctx->canvas->concat(cg);

  SKBarDrawState initial;
  initial.font_smoothing = font_smoothing;
  ctx->states.push_back(initial);
  return ctx;
}

void sk_context_destroy(skbar_context* context) {
  delete context;
}

void sk_context_save(skbar_context* context) {
  context->canvas->save();
  context->states.push_back(context->states.back());
}

void sk_context_restore(skbar_context* context) {
  if (context->states.size() > 1) context->states.pop_back();
  context->canvas->restore();
}

static SKBarDrawState& draw_state(skbar_context* context) {
  return context->states.back();
}

void sk_context_set_fill_rgba(skbar_context* context, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  draw_state(context).paint.setColor(SkColor4f{static_cast<float>(r),
                                               static_cast<float>(g),
                                               static_cast<float>(b),
                                               static_cast<float>(a)});
}

void sk_context_set_stroke_rgba(skbar_context* context, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  SKBarDrawState& s = draw_state(context);
  s.paint.setColor(SkColor4f{static_cast<float>(r),
                             static_cast<float>(g),
                             static_cast<float>(b),
                             static_cast<float>(a)});
  s.paint.setStyle(SkPaint::kStroke_Style);
}

void sk_context_set_line_width(skbar_context* context, CGFloat width) {
  draw_state(context).paint.setStrokeWidth(static_cast<SkScalar>(width));
}

void sk_context_set_blend_mode(skbar_context* context, CGBlendMode mode) {
  SKBarDrawState& s = draw_state(context);
  s.blend_mode = mode;
  s.paint.setBlendMode(to_sk_blend(mode));
}

void sk_context_set_allows_font_smoothing(skbar_context* context, bool enables) {
  draw_state(context).font_smoothing = enables;
}

void sk_context_set_interpolation_none(skbar_context* context, bool none) {
  draw_state(context).interpolation_none = none;
}

void sk_context_set_text_position(skbar_context* context, CGFloat x, CGFloat y) {
  draw_state(context).text_position = SkPoint::Make(static_cast<SkScalar>(x),
                                                    static_cast<SkScalar>(y));
}

void sk_context_add_path(skbar_context* context, const skbar_path* path) {
  // CGContextAddPath appends; the seam stores the most recently added path,
  // which covers every call site in the portable core (documented deviation).
  SKBarDrawState& s = draw_state(context);
  if (path) {
    s.current_path = path->path;
    s.has_current_path = true;
  } else {
    s.has_current_path = false;
  }
}

void sk_context_clip(skbar_context* context) {
  const SKBarDrawState& s = draw_state(context);
  if (s.has_current_path) context->canvas->clipPath(s.current_path, SkClipOp::kIntersect, true);
}

void sk_context_draw_path(skbar_context* context, CGPathDrawingMode mode) {
  SKBarDrawState& s = draw_state(context);
  if (!s.has_current_path) return;

  SkPath path = s.current_path;
  // Windows header names the even-odd modes kCGPathEvenOddFill(Stroke) (the
  // Apple spellings kCGPathEOFill/kCGPathEOFillStroke do not exist there).
  if (mode == kCGPathEvenOddFill || mode == kCGPathEvenOddFillStroke) {
    path.setFillType(SkPathFillType::kEvenOdd);
  }

  switch (mode) {
    case kCGPathStroke:
      s.paint.setStyle(SkPaint::kStroke_Style);
      context->canvas->drawPath(path, s.paint);
      break;
    case kCGPathFillStroke:
    case kCGPathEvenOddFillStroke:
      // CG composites fill-then-stroke with an implicit alpha blend between
      // the two passes; kStrokeAndFill is the documented single-pass
      // approximation (surfaced by the parity harness).
      s.paint.setStyle(SkPaint::kStrokeAndFill_Style);
      context->canvas->drawPath(path, s.paint);
      break;
    case kCGPathFill:
    case kCGPathEvenOddFill:
    default:
      s.paint.setStyle(SkPaint::kFill_Style);
      context->canvas->drawPath(path, s.paint);
      break;
  }
}

void sk_context_draw_image(skbar_context* context, CGRect rect, const skbar_image* image) {
  if (!image || !image->image) return;
  SkSamplingOptions sampling = draw_state(context).interpolation_none
      ? SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone)
      : SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
  SkRect dst = SkRect::MakeXYWH(static_cast<SkScalar>(rect.origin.x),
                                static_cast<SkScalar>(rect.origin.y),
                                static_cast<SkScalar>(rect.size.width),
                                static_cast<SkScalar>(rect.size.height));
  context->canvas->drawImageRect(image->image,
                                 SkRect::MakeWH(static_cast<SkScalar>(image->width),
                                                static_cast<SkScalar>(image->height)),
                                 dst, sampling, &draw_state(context).paint,
                                 SkCanvas::SrcRectConstraint::kFast_SrcRectConstraint);
}

void sk_context_draw_line(skbar_context* context, const skbar_line* line) {
  if (!line || !line->blob) return;
  SKBarDrawState& s = draw_state(context);
  // m124: drawTextBlob requires the explicit paint argument.
  context->canvas->drawTextBlob(line->blob, s.text_position.x(), s.text_position.y(), s.paint);
}

bool sk_context_read_pixels(skbar_context* context, size_t* out_width, size_t* out_height, void** out_bgra) {
  if (!out_bgra) return false;
  *out_bgra = nullptr;
  const size_t row_bytes = static_cast<size_t>(context->width) * 4u;
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(row_bytes * context->height));
  if (!buf) return false;

  SkImageInfo info = SkImageInfo::MakeN32Premul(static_cast<int>(context->width),
                                                static_cast<int>(context->height));
  if (!context->canvas->readPixels(info, buf, row_bytes, 0, 0)) {
    std::free(buf);
    return false;
  }
  // Export in CG order so the harness can memcmp against CoreGraphics.
  flip_vertical(buf, context->width, context->height);
  if (out_width) *out_width = context->width;
  if (out_height) *out_height = context->height;
  *out_bgra = buf;
  return true;
}

bool sk_context_write_png(skbar_context* context, const char* path) {
  if (!context->surface) return false;
  sk_sp<SkImage> snapshot = context->surface->makeImageSnapshot();
  if (!snapshot) return false;

  // m124 drift: SkImage::encodeToData was removed from the vendored SDK;
  // encode through the standalone PNG encoder instead.
  SkPixmap pm;
  if (!snapshot->peekPixels(&pm)) return false;
  SkDynamicMemoryWStream stream;
  if (!SkPngEncoder::Encode(&stream, pm, SkPngEncoder::Options{})) return false;
  sk_sp<SkData> png = stream.detachAsData();
  if (!png) return false;

  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return false;
  const size_t written = fwrite(png->data(), 1, png->size(), f);
  const int close_rc = fclose(f);
  return written == png->size() && close_rc == 0;
}

// --- paths ---------------------------------------------------------------------

skbar_path* sk_path_create(void) {
  auto* path = new skbar_path();
  path->base.magic = kMagic;
  path->base.kind = SKBAR_KIND_PATH;
  return path;
}

void sk_path_add_rect(skbar_path* path, CGRect rect) {
  path->path.addRect(SkRect::MakeXYWH(static_cast<SkScalar>(rect.origin.x),
                                      static_cast<SkScalar>(rect.origin.y),
                                      static_cast<SkScalar>(rect.size.width),
                                      static_cast<SkScalar>(rect.size.height)));
}

void sk_path_add_rounded_rect(skbar_path* path, CGRect rect, CGFloat corner_width, CGFloat corner_height) {
  path->path.addRoundRect(SkRect::MakeXYWH(static_cast<SkScalar>(rect.origin.x),
                                           static_cast<SkScalar>(rect.origin.y),
                                           static_cast<SkScalar>(rect.size.width),
                                           static_cast<SkScalar>(rect.size.height)),
                          static_cast<SkScalar>(corner_width),
                          static_cast<SkScalar>(corner_height));
}

void sk_path_destroy(skbar_path* path) {
  delete path;
}

// --- fonts ---------------------------------------------------------------------

skbar_font* sk_font_create(const char* family,
                           const char* style,
                           float size,
                           const char feature_tags[][5],
                           size_t feature_count) {
  // m124 drift: SkTypeface::MakeFromName and SkFontMgr::RefDefault no longer
  // exist in the vendored SDK; the DirectWrite font manager is the supported
  // system-font source on Windows (include/ports/SkTypeface_win.h).
  sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_DirectWrite();
  if (!font_mgr) return nullptr;
  sk_sp<SkTypeface> typeface = font_mgr->matchFamilyStyle(family, to_sk_style(style));
  if (!typeface) return nullptr;

  auto* font = new skbar_font();
  font->base.magic = kMagic;
  font->base.kind = SKBAR_KIND_FONT;
  font->typeface = std::move(typeface);
  font->size = size;
  font->font_smoothing = true;

  for (size_t i = 0; i < feature_count; ++i) {
    if (!feature_tags[i][0]) continue;
    SkShaper::Feature f;
    f.tag = SkSetFourByteTag(static_cast<uint8_t>(feature_tags[i][0]),
                             static_cast<uint8_t>(feature_tags[i][1]),
                             static_cast<uint8_t>(feature_tags[i][2]),
                             static_cast<uint8_t>(feature_tags[i][3]));
    f.value = 1;
    f.start = 0;
    f.end = static_cast<size_t>(-1);
    font->features.push_back(f);
  }
  return font;
}

void sk_font_destroy(skbar_font* font) {
  delete font;
}

double sk_font_ascent(skbar_font* font) {
  SkFontMetrics m;
  SkFont(font->typeface, font->size).getMetrics(&m);
  return -m.fAscent;  // CoreText reports positive ascent
}

double sk_font_descent(skbar_font* font) {
  SkFontMetrics m;
  SkFont(font->typeface, font->size).getMetrics(&m);
  return m.fDescent;
}

void sk_font_register(const char* font_path) {
  // Documented no-op: system fonts resolve via the DirectWrite-backed
  // SkFontMgr. External font registration arrives with the S7 follow-up.
  (void)font_path;
}

// --- text lines -----------------------------------------------------------------

skbar_line* sk_line_create(skbar_font* font, const char* text, size_t length) {
  if (!font || !text) return nullptr;

  SkFont skfont(font->typeface, font->size);
  if (font->font_smoothing) {
    skfont.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    skfont.setSubpixel(true);
  } else {
    skfont.setEdging(SkFont::Edging::kAlias);
    skfont.setSubpixel(false);
  }

  std::unique_ptr<SkShaper> shaper = SkShaper::Make();
  if (!shaper) return nullptr;

  SkTextBlobBuilderRunHandler builder(text, SkPoint::Make(0.f, 0.f));
  const SkScalar no_wrap = 1e6f;
  if (font->features.empty()) {
    // m124 still ships the simple font+width form.
    shaper->shape(text, length, skfont, true, no_wrap, &builder);
  } else {
    // m124 drift: the feature-taking overload requires the full run-iterator
    // set (the simple font+features form arrived in a later Skia). Trivial
    // iterators pin the same single-run, LTR, Latin baseline the simple form
    // would use, so parity semantics are unchanged.
    SkShaper::TrivialFontRunIterator font_it(skfont, length);
    SkShaper::TrivialBiDiRunIterator bidi_it(0, length);  // 0 == LTR
    SkShaper::TrivialScriptRunIterator script_it(SkSetFourByteTag('L', 'a', 't', 'n'), length);
    SkShaper::TrivialLanguageRunIterator lang_it("en", length);
    // m124: shape() returns void; success is proven by makeBlob() below.
    shaper->shape(text, length, font_it, bidi_it, script_it, lang_it,
                  font->features.data(), font->features.size(),
                  no_wrap, &builder);
  }

  auto* line = new skbar_line();
  line->base.magic = kMagic;
  line->base.kind = SKBAR_KIND_LINE;
  line->blob = builder.makeBlob();
  if (!line->blob) {
    delete line;
    return nullptr;
  }

  const SkRect b = line->blob->bounds();
  line->bounds = CGRectMake(b.left(), b.bottom(), b.width(), b.height());
  return line;
}

void sk_line_destroy(skbar_line* line) {
  delete line;
}

CGRect sk_line_get_bounds(skbar_line* line, CTLineBoundsOptions options) {
  (void)options;  // flags are surface-approximated in sk_backend_common.c
  return line ? line->bounds : CGRectMake(0, 0, 0, 0);
}

// --- data blobs -------------------------------------------------------------------

skbar_data* sk_data_create_copy(const void* bytes, size_t length) {
  auto* data = new skbar_data();
  data->base.magic = kMagic;
  data->base.kind = SKBAR_KIND_DATA;
  data->length = length;
  if (length > 0) {
    data->bytes = static_cast<uint8_t*>(std::malloc(length));
    if (!data->bytes) {
      delete data;
      return nullptr;
    }
    std::memcpy(data->bytes, bytes, length);
  }
  return data;
}

size_t sk_data_length(skbar_data* data) {
  return data ? data->length : 0;
}

const void* sk_data_bytes(skbar_data* data) {
  return data ? data->bytes : nullptr;
}

void sk_data_release(skbar_data* data) {
  if (!data) return;
  std::free(data->bytes);
  delete data;
}

// --- images -----------------------------------------------------------------------

static skbar_image* make_image(sk_sp<SkImage> image, sk_sp<SkData> source) {
  if (!image) return nullptr;
  auto* img = new skbar_image();
  img->base.magic = kMagic;
  img->base.kind = SKBAR_KIND_IMAGE;
  img->image = std::move(image);
  img->source = std::move(source);
  img->width = static_cast<uint32_t>(img->image->width());
  img->height = static_cast<uint32_t>(img->image->height());
  return img;
}

skbar_image* sk_image_decode_file(const char* path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
  const long len = ftell(f);
  if (len <= 0) { fclose(f); return nullptr; }
  rewind(f);

  uint8_t* raw = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(len)));
  if (!raw) { fclose(f); return nullptr; }
  const size_t got = fread(raw, 1, static_cast<size_t>(len), f);
  fclose(f);
  if (got != static_cast<size_t>(len)) { std::free(raw); return nullptr; }

  sk_sp<SkData> source = SkData::MakeFromMalloc(raw, static_cast<size_t>(len));
  sk_sp<SkImage> image;
  if (source) image = SkImages::DeferredFromEncodedData(source);
  return make_image(std::move(image), std::move(source));
}

skbar_image* sk_icon_for_app(const char* app) {
  // App icons via the Win32 thumbnail API (IShellItemImageFactory), the
  // closest Windows equivalent to workspace_icon_for_app. Uses shell32+ole32.
  if (!app || !app[0]) return nullptr;

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  skbar_image* result = nullptr;

  // SHCreateItemFromParsingName takes a wide string; the seam receives UTF-8.
  wchar_t wide_path[1024] = { 0 };
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, app, -1,
                                       wide_path, (int)(sizeof(wide_path) / sizeof(wide_path[0])));
  if (wlen == 0) {
    CoUninitialize();
    return nullptr;
  }

  IShellItem* item = nullptr;
  HRESULT hr = SHCreateItemFromParsingName(
      wide_path, nullptr, IID_PPV_ARGS(&item));
  if (SUCCEEDED(hr) && item) {
    IShellItemImageFactory* factory = nullptr;
    hr = item->BindToHandler(nullptr, BHID_SFUIObject,
                             IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr) && factory) {
      HBITMAP hbmp = nullptr;
      hr = factory->GetImage(SIZE{64, 64},
                             SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                             &hbmp);
      if (SUCCEEDED(hr) && hbmp) {
        BITMAP bm = {};
        if (GetObject(hbmp, sizeof(bm), &bm) != 0 && bm.bmWidth > 0 && bm.bmHeight > 0) {
          const int w = bm.bmWidth;
          const int h = bm.bmHeight;  // HBITMAP is bottom-up (positive height)
          const size_t row_bytes = static_cast<size_t>(w) * 4u;
          uint8_t* buf = static_cast<uint8_t*>(std::malloc(row_bytes * static_cast<size_t>(h)));
          if (buf) {
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = h;      // positive -> bottom-up DIB
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            HDC hdc = CreateCompatibleDC(nullptr);
            if (hdc) {
              HGDIOBJ old = SelectObject(hdc, hbmp);
              if (GetDIBits(hdc, hbmp, 0, static_cast<UINT>(h), buf, &bmi,
                            DIB_RGB_COLORS) == static_cast<int>(h)) {
                // DIB row 0 is the BOTTOM row; Skia wants top-down, so flip.
                flip_vertical(buf, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                SkBitmap bitmap;
                bitmap.setInfo(SkImageInfo::MakeN32Premul(w, h));
                if (bitmap.tryAllocPixels()) {
                  std::memcpy(bitmap.getPixels(), buf, row_bytes * static_cast<size_t>(h));
                  SkPixmap pixmap;
                  if (bitmap.peekPixels(&pixmap)) {
                    sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(pixmap);
                    result = make_image(std::move(image), nullptr);
                  }
                }
              }
              SelectObject(hdc, old);
              DeleteDC(hdc);
            }
            std::free(buf);
          }
        }
        DeleteObject(hbmp);
      }
      factory->Release();
    }
    if (item) item->Release();
  }
  CoUninitialize();
  return result;
}

skbar_image* sk_image_retain(skbar_image* image) {
  if (image) image->refcount.fetch_add(1);
  return image;
}

void sk_image_unref(skbar_image* image) {
  if (!image) return;
  if (image->refcount.fetch_sub(1) == 1) {
    if (image->source_view) sk_data_release(image->source_view);
    delete image;
  }
}

uint32_t sk_image_width(skbar_image* image) {
  return image ? image->width : 0;
}

uint32_t sk_image_height(skbar_image* image) {
  return image ? image->height : 0;
}

bool sk_image_read_pixels(skbar_image* image, size_t* out_width, size_t* out_height, void** out_bgra) {
  if (!image || !image->image || !out_bgra) return false;
  *out_bgra = nullptr;

  sk_sp<SkImage> raster = image->image->makeRasterImage();
  if (!raster) return false;

  const size_t row_bytes = static_cast<size_t>(image->width) * 4u;
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(row_bytes * image->height));
  if (!buf) return false;

  SkImageInfo info = SkImageInfo::MakeN32Premul(static_cast<int>(image->width),
                                                static_cast<int>(image->height));
  if (!raster->readPixels(info, buf, row_bytes, 0, 0)) {
    std::free(buf);
    return false;
  }
  flip_vertical(buf, image->width, image->height);  // export in CG order
  if (out_width) *out_width = image->width;
  if (out_height) *out_height = image->height;
  *out_bgra = buf;
  return true;
}

skbar_data* sk_image_source_data(skbar_image* image) {
  if (!image || !image->source) return nullptr;
  if (!image->source_view) {
    image->source_view = sk_data_create_copy(image->source->data(),
                                             static_cast<size_t>(image->source->size()));
  }
  return image->source_view;  // borrowed; owned by the image
}

}  // extern "C"