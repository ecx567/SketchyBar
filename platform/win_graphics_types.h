#pragma once
// platform/win_graphics_types.h
//
// Windows-only substitute for the CoreGraphics / CoreText / CoreFoundation type
// surface that the portable draw path references on macOS. The portable core is
// never touched for macOS; under `_WIN32` this header supplies the exact CG/CT
// type names, constants and function declarations, backed by the Skia seam
// (platform/sk_backend.h). All custom types are opaque pointers handed between
// the C core and the C++ Skia backend, so no Skia header ever leaks into the
// portable C code.
//
// Only included from src/misc/helpers.h and src/context.h under `_WIN32`.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <sys/types.h> /* pid_t for the portable headers (UCRT) */

#ifdef __cplusplus
extern "C" {
#endif

// --- scalar / geometry types -----------------------------------------------
// 64-bit macOS uses `double` for CGFloat; the Windows build mirrors that so
// all coordinate math stays byte-identical.
typedef double CGFloat;

typedef struct { CGFloat x; CGFloat y; } CGPoint;
typedef struct { CGFloat width; CGFloat height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;

static const CGPoint CGPointZero = { 0, 0 };
static const CGSize CGSizeZero = { 0, 0 };
static const CGRect CGRectZero = { { 0, 0 }, { 0, 0 } };
static const CGRect CGRectNull = { { INFINITY, INFINITY }, { 0, 0 } };

static inline CGPoint CGPointMake(CGFloat x, CGFloat y) {
  CGPoint p; p.x = x; p.y = y; return p;
}
static inline CGSize CGSizeMake(CGFloat width, CGFloat height) {
  CGSize s; s.width = width; s.height = height; return s;
}
static inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat width, CGFloat height) {
  CGRect r; r.origin.x = x; r.origin.y = y; r.size.width = width; r.size.height = height; return r;
}

// --- context / image / path / font / data opaques (implemented by the backend) -
struct skbar_context;
struct skbar_image;
struct skbar_path;
struct skbar_font;
struct skbar_line;
struct skbar_data;

// Typedef the tags themselves so the seam can use `skbar_context*` and friends
// directly (sk_backend.h) while the CG aliases below keep the portable core
// compiling with macOS spelling.
typedef struct skbar_context skbar_context;
typedef struct skbar_image skbar_image;
typedef struct skbar_path skbar_path;
typedef struct skbar_font skbar_font;
typedef struct skbar_line skbar_line;
typedef struct skbar_data skbar_data;

typedef struct skbar_context* CGContextRef;
typedef struct skbar_image* CGImageRef;
typedef struct skbar_path* CGMutablePathRef;
typedef CGMutablePathRef CGPathRef;
typedef struct skbar_font* CTFontRef;
typedef struct skbar_line* CTLineRef;

// Mach-ism referenced by the portable core headers (bar_item.h event_port).
// Mach ports are uint32 over the wire; a placeholder type is enough to keep
// the core compiling until the real Windows event-port adapter lands.
typedef uint32_t mach_port_t;

// POSIX-ism referenced by the portable core headers (alias.h pid).
// The UCRT <sys/types.h> (Win10 SDK 26100) does NOT provide pid_t; alias.h
// uses it for the app/window process id. A plain int covers it until the
// Windows process adapter lands.
#ifndef _PID_T_DEFINED
typedef int pid_t;
#define _PID_T_DEFINED
#endif

// --- CF-lite opaques (type-only; the seam never creates real CF objects) ---
typedef struct CFString* CFStringRef;
typedef struct CFNumber* CFNumberRef;
typedef struct CFDictionary* CFDictionaryRef;
typedef struct CFArray* CFArrayRef;
typedef struct CFData* CFDataRef;
typedef struct CFURL* CFURLRef;
typedef struct CFUUID* CFUUIDRef;
typedef struct CFType* CFTypeRef;
typedef struct CFRunLoopTimer* CFRunLoopTimerRef;
typedef struct CGColorSpace* CGColorSpaceRef;
typedef struct skbar_data* CGDataProviderRef;
typedef struct CGEvent* CGEventRef;
typedef struct CVDisplayLink* CVDisplayLinkRef;
typedef long CFIndex;

// --- enums/constants (values mirror the macOS headers 1:1) -----------------
typedef enum {
  kCGBitmapAlphaInfoMask = 0x1F,
  kCGImageAlphaPremultipliedFirst = 1,
  kCGBitmapByteOrder32Host = 0x0200, /* little-endian 32-bit */
} CGBitmapInfo;

typedef enum {
  kCGInterpolationDefault = 0,
  kCGInterpolationNone = 1,
  kCGInterpolationLow = 2,
  kCGInterpolationMedium = 4,
  kCGInterpolationHigh = 3,
} CGInterpolationQuality;

typedef enum {
  kCGBlendModeNormal = 0,
  kCGBlendModeMultiply = 1,
  kCGBlendModeScreen = 2,
  kCGBlendModeOverlay = 3,
  kCGBlendModeDarken = 4,
  kCGBlendModeLighten = 5,
  kCGBlendModeColorDodge = 6,
  kCGBlendModeColorBurn = 7,
  kCGBlendModeSoftLight = 8,
  kCGBlendModeHardLight = 9,
  kCGBlendModeDifference = 10,
  kCGBlendModeExclusion = 11,
  kCGBlendModeHue = 12,
  kCGBlendModeSaturation = 13,
  kCGBlendModeColor = 14,
  kCGBlendModeLuminosity = 15,
  kCGBlendModeClear = 16,
  kCGBlendModeCopy = 17,
  kCGBlendModeSourceIn = 18,
  kCGBlendModeSourceOut = 19,
  kCGBlendModeSourceAtop = 20,
  kCGBlendModeDestinationOver = 21,
  kCGBlendModeDestinationIn = 22,
  kCGBlendModeDestinationOut = 23, /* clip_rect: must map to SkBlendMode::kDstOut */
  kCGBlendModeDestinationAtop = 24,
  kCGBlendModeXOR = 25,
  kCGBlendModePlusDarker = 26,
  kCGBlendModePlusLighter = 27,
} CGBlendMode;

typedef enum {
  kCGPathFill = 0,
  kCGPathEvenOddFill = 1,
  kCGPathFillStroke = 2,
  kCGPathEvenOddFillStroke = 3,
  /* Stroke-only was missing from the original alias set (the portable core
   * only ever passes fill/fill-stroke modes). Added by S2f so the Skia
   * backend's defensive switch covers the full CoreGraphics mode set. */
  kCGPathStroke = 4,
} CGPathDrawingMode;

typedef enum {
  /* CTLineBoundsOptions values mirror CoreText 1:1 (text.c uses
   * kCTLineBoundsUseGlyphPathBounds for width/layout). */
  kCTLineBoundsExcludeTypographicLeading = 1 << 0,
  kCTLineBoundsUseGlyphPathBounds = 1 << 2,
  kCTLineBoundsUseOpticalBounds = 1 << 3,
  kCTLineBoundsIncludeLanguageExtents = 1 << 4,
} CTLineBoundsOptions;

/* true-type feature constants used verbatim by src/font.c's feature table
 * (values from CoreText's CTFontFeatures.h; on Windows the OpenType tag
 * column drives shaping, the numeric pair only feeds the table). */
typedef enum {
  kLigaturesType = 1,
  kCommonLigaturesOnSelector = 2,
  kRareLigaturesOnSelector = 4,
  kVerticalPositionType = 10,
  kSuperiorsSelector = 1,
  kInferiorsSelector = 2,
  kFractionsType = 11,
  kVerticalFractionsSelector = 1,
  kDiagonalFractionsSelector = 2,
  kTypographicExtrasType = 14,
  kSlashedZeroOnSelector = 9,
  kNumberCaseType = 21,
  kUpperCaseNumbersSelector = 0,
  kLowerCaseNumbersSelector = 1,
  kStylisticAlternativesType = 35,
  kContextualAlternatesType = 36,
  kContextualAlternatesOnSelector = 2,
  kSwashAlternatesOnSelector = 4,
  kContextualSwashAlternatesOnSelector = 6,
  kLowerCaseType = 37,
  kLowerCaseSmallCapsSelector = 1,
  kUpperCaseType = 38,
  kUpperCaseSmallCapsSelector = 1,
  kNumberSpacingType = 6,
  kMonospacedNumbersSelector = 0,
  kProportionalNumbersSelector = 1,
} CTFontFeatureConstants;

/* CGEvent/flag constants referenced by helpers.h inline helpers. Values mirror
 * the macOS CGEventTypes.h header exactly. */
typedef enum {
  kCGEventLeftMouseDown = 1,
  kCGEventLeftMouseUp = 2,
  kCGEventRightMouseDown = 3,
  kCGEventRightMouseUp = 4,
} CGEventType;
typedef enum {
  kCGEventFlagMaskShift = 1 << 17,
  kCGEventFlagMaskControl = 1 << 18,
  kCGEventFlagMaskAlternate = 1 << 19,
  kCGEventFlagMaskCommand = 1 << 20,
  kCGEventFlagMaskSecondaryFn = 1 << 22,
} CGEventFlags;

// --- CG context shim (implemented in src/context.c, _WIN32 branch) ---------
CGContextRef context_create(CGSize size, CGFloat scale);

void CGContextSaveGState(CGContextRef context);
void CGContextRestoreGState(CGContextRef context);
void CGContextClip(CGContextRef context);
void CGContextAddPath(CGContextRef context, CGPathRef path);
void CGContextDrawPath(CGContextRef context, CGPathDrawingMode mode);
void CGContextSetRGBFillColor(CGContextRef context, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
void CGContextSetRGBStrokeColor(CGContextRef context, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
void CGContextSetLineWidth(CGContextRef context, CGFloat width);
void CGContextSetTextPosition(CGContextRef context, CGFloat x, CGFloat y);
void CGContextSetBlendMode(CGContextRef context, CGBlendMode mode);
void CGContextSetAllowsFontSmoothing(CGContextRef context, bool allows);
void CGContextSetInterpolationQuality(CGContextRef context, CGInterpolationQuality quality);
void CGContextDrawImage(CGContextRef context, CGRect rect, CGImageRef image);

CGMutablePathRef CGPathCreateMutable(void);
void CGPathAddRect(CGMutablePathRef path, const void* transform, CGRect rect);
void CGPathAddRoundedRect(CGMutablePathRef path, const void* transform, CGRect rect, CGFloat corner_width, CGFloat corner_height);
void CGPathRelease(CGPathRef path);

// --- text shim (implemented by the backend, text.c consumes these in S3) ---
void CTLineDraw(CTLineRef line, CGContextRef context);
CGRect CTLineGetBoundsWithOptions(CTLineRef line, CTLineBoundsOptions options);
void CTLineGetTypographicBounds(CTLineRef line, CGFloat* ascent, CGFloat* descent, CGFloat* leading);
double CTFontGetAscent(CTFontRef font);
double CTFontGetDescent(CTFontRef font);

// --- image / data shim (implemented by the backend seam) ---------------------
// CGDataProviderRef is a view over an image's ORIGINAL ENCODED SOURCE BYTES,
// modeled as a skbar_data blob (see sk_backend.h). CFDataRef is ALSO modeled
// as skbar_data (image.c treats CGDataProviderCopyData's result as CFData).

CGImageRef CGImageCreateCopy(CGImageRef image);
CGDataProviderRef CGImageGetDataProvider(CGImageRef image);
CFDataRef CGDataProviderCopyData(CGDataProviderRef provider);
uint32_t CGImageGetWidth(CGImageRef image);
uint32_t CGImageGetHeight(CGImageRef image);
void CGImageRelease(CGImageRef image);

size_t CFDataGetLength(CFDataRef data);
const void* CFDataGetBytePtr(CFDataRef data);
void CGPathRelease(CGPathRef path);

// NULL-safe release dispatcher (see sk_backend_common.c). macOS semantics are
// preserved: CFRelease(NULL) is the only deliberate deviation, required by the
// fail-closed stub backends producing NULL objects.
void CFRelease(void* obj);

static inline bool CGSizeEqualToSize(CGSize a, CGSize b) {
  return a.width == b.width && a.height == b.height;
}

#ifdef __cplusplus
}
#endif