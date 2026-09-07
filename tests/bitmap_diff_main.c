// tests/bitmap_diff_main.c
//
// Visual parity harness for the S2 graphics seam.
//
// Renders the reference cases through the ACTIVE rendering stack and compares
// the pixel buffer against the golden produced by the OTHER platform:
//
//   macOS (--render):  CoreText/CoreGraphics -> <case>_macos.png  (goldens)
//   Windows (--check): sk_* seam (Skia)       -> fresh render <-> <case>_macos.png
//
// The reference cases pin the shaper/renderer seam (OpenType features,
// subpixel AA, coordinate model). The gate is FAIL-CLOSED: without a real
// Skia SDK the stub backend cannot create a context, and the harness exits
// non-zero with a documented message.
//
// Exit codes:
//   0 PASS                  1 usage error
//   2 backend unavailable   3 golden file missing
//   4 diff exceeded         5 determinism check failed
//
// The harness calls ONLY the sk_* seam surface (Windows) — never the portable
// core — so it links the backend alone (no bar_manager globals).
//
// Determinism (Windows only): every render is performed twice and the buffers
// must be byte-identical.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "../platform/sk_backend.h"
#else
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreServices/CoreServices.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#endif

#define CANVAS_W 320
#define CANVAS_H 48
#define CANVAS_SCALE 2.0f

#define EXIT_PASS 0
#define EXIT_USAGE 1
#define EXIT_BACKEND_UNAVAILABLE 2
#define EXIT_GOLDEN_MISSING 3
#define EXIT_DIFF_EXCEEDED 4
#define EXIT_DETERMINISM_FAILED 5

#define MAX_TAGS 8u
#define DIFF_TOLERANCE 12   /* per-channel delta */
#define DIFF_MAX_PERCENT 2.0

struct ref_case {
  const char* name;
  const char* family;
  const char* style;
  float size;
  const char* feature_tags;  /* comma-separated 4-char tags, already resolved */
  const char* text;
};

#ifdef __APPLE__
/* Numeric CoreText feature/selector pairs matching the resolved tags above
   (see src/font.c feature_mappings[]; values from CTFontFeatures.h). */
struct mac_feature_pair {
  const char* tag;
  int feature;
  int selector;
};

static const struct mac_feature_pair k_pair_sets[][4] = {
  { { "liga", 1, 2 }, { "tnum", 6, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },   /* arial */
  { { "liga", 1, 2 }, { "onum", 21, 1 }, { 0, 0, 0 }, { 0, 0, 0 } },  /* times */
  { { "zero", 14, 9 }, { "salt", 35, 2 }, { 0, 0, 0 }, { 0, 0, 0 } }, /* courier */
};
#endif

static const struct ref_case k_cases[] = {
  { "arial",   "Arial",         "Regular", 14.f, "liga,tnum", "ffi fj fl AV 0123456789" },
  { "times",   "Times New Roman","Regular", 16.f, "liga,onum", "ffi fj fl AV 0123456789" },
  { "courier", "Courier New",   "Regular", 14.f, "zero,salt", "ffi fj fl AV 0123456789" },
};
#define CASE_COUNT ((int)(sizeof(k_cases) / sizeof(k_cases[0])))

struct pixel_buffer {
  size_t width;
  size_t height;
  uint8_t* data;
};

static void pixel_buffer_free(struct pixel_buffer* buf) {
  free(buf->data);
  buf->data = NULL;
  buf->width = buf->height = 0;
}

static void usage(void) {
  printf("usage: bitmap_diff (--render | --check) [--case <name>|all]\n"
         "  --render  render the cases and write <case>_<platform>.png + determinism check\n"
         "  --check   compare a fresh render against the other platform's golden\n"
         "  --case    select one case by name, or 'all' (default)\n");
}

/* --- Windows (sk_* seam) ---------------------------------------------------- */

#ifdef _WIN32

static const char* this_platform_tag(void) { return "windows"; }
static const char* other_platform_tag(void) { return "macos"; }

static int win_render(const struct ref_case* c, struct pixel_buffer* out) {
  char golden_owned[512] = { 0 };

  skbar_context* context = sk_context_create(CANVAS_W * (uint32_t)CANVAS_SCALE,
                                             CANVAS_H * (uint32_t)CANVAS_SCALE,
                                             CANVAS_SCALE, true);
  if (!context) {
    printf("skia backend unavailable (SKIA_DIR not configured); parity gate FAIL-CLOSED\n");
    return EXIT_BACKEND_UNAVAILABLE;
  }

  /* white background, CG order coordinates (user space) */
  skbar_path* bg = sk_path_create();
  sk_path_add_rect(bg, CGRectMake(0, 0, CANVAS_W, CANVAS_H));
  sk_context_set_fill_rgba(context, 1, 1, 1, 1);
  sk_context_add_path(context, bg);
  sk_context_draw_path(context, kCGPathFill);
  sk_path_destroy(bg);

  /* parse feature tags */
  char tags[MAX_TAGS][5] = { { 0 } };
  size_t tag_count = 0;
  size_t feat_len = strlen(c->feature_tags);
  char* copy = (char*)malloc(feat_len + 1);
  if (!copy) { sk_context_destroy(context); return EXIT_USAGE; }
  memcpy(copy, c->feature_tags, feat_len + 1);
  char* tok = strtok(copy, ",");
  while (tok && tag_count < MAX_TAGS) {
    size_t n = strlen(tok);
    if (n == 4) {
      memcpy(tags[tag_count], tok, 4);
      tags[tag_count][4] = '\0';
      ++tag_count;
    }
    tok = strtok(NULL, ",");
  }
  free(copy);

  skbar_font* font = sk_font_create(c->family, c->style, c->size, tags, tag_count);
  if (!font) { sk_context_destroy(context); return EXIT_BACKEND_UNAVAILABLE; }

  skbar_line* line = sk_line_create(font, c->text, strlen(c->text));
  if (!line) { sk_font_destroy(font); sk_context_destroy(context); return EXIT_BACKEND_UNAVAILABLE; }

  sk_context_set_fill_rgba(context, 0, 0, 0, 1);
  sk_context_set_text_position(context, 12, 12);
  sk_context_draw_line(context, line);

  if (out && out->data) {
    snprintf(golden_owned, sizeof(golden_owned), "%s_%s.png", c->name, this_platform_tag());
    sk_context_write_png(context, golden_owned);
  }

  int rc = EXIT_PASS;
  if (out) {
    if (!sk_context_read_pixels(context, &out->width, &out->height, (void**)&out->data)) {
      rc = EXIT_BACKEND_UNAVAILABLE;
    }
  }

  sk_line_destroy(line);
  sk_font_destroy(font);
  sk_context_destroy(context);
  return rc;
}

static int win_determinism_check(const struct ref_case* c) {
  struct pixel_buffer a = { 0 }, b = { 0 };
  int rc = win_render(c, &a);
  if (rc != EXIT_PASS) return rc;
  rc = win_render(c, &b);
  if (rc != EXIT_PASS) { pixel_buffer_free(&a); return rc; }

  size_t bytes = a.width * a.height * 4;
  if (bytes != b.width * b.height * 4 || memcmp(a.data, b.data, bytes) != 0) {
    printf("determinism FAILED for '%s': two renders differ\n", c->name);
    rc = EXIT_DETERMINISM_FAILED;
  } else {
    printf("determinism OK for '%s' (%zu bytes)\n", c->name, bytes);
  }
  pixel_buffer_free(&a);
  pixel_buffer_free(&b);
  return rc;
}

static struct pixel_buffer win_load_golden(const char* path) {
  struct pixel_buffer buf = { 0 };
  skbar_image* image = sk_image_decode_file(path);
  if (!image) return buf;
  size_t w = 0, h = 0;
  void* pixels = NULL;
  if (!sk_image_read_pixels(image, &w, &h, &pixels)) {
    sk_image_unref(image);
    return buf;
  }
  buf.width = w;
  buf.height = h;
  buf.data = (uint8_t*)pixels;
  sk_image_unref(image);
  return buf;
}

#else /* __APPLE__ ------------------------------------------------------------ */

static const char* this_platform_tag(void) { return "macos"; }
static const char* other_platform_tag(void) { return "windows"; }

static CFMutableArrayRef mac_feature_array(int case_index);

static int mac_render(const struct ref_case* c, struct pixel_buffer* out) {
  int case_index = (int)(c - k_cases);  /* k_cases order == k_pair_sets order */
  const size_t pw = (size_t)CANVAS_W * (size_t)CANVAS_SCALE;
  const size_t ph = (size_t)CANVAS_H * (size_t)CANVAS_SCALE;
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(NULL, pw, ph, 8, pw * 4, cs,
                                           kCGImageAlphaPremultipliedFirst
                                           | kCGBitmapByteOrder32Host);
  CGColorSpaceRelease(cs);
  if (!ctx) return EXIT_BACKEND_UNAVAILABLE;

  CGContextScaleCTM(ctx, CANVAS_SCALE, CANVAS_SCALE);
  CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
  CGContextFillRect(ctx, CGRectMake(0, 0, CANVAS_W, CANVAS_H));

  CFStringRef family = CFStringCreateWithCString(kCFAllocatorDefault,
                                                 c->family,
                                                 kCFStringEncodingUTF8);
  if (!family) { CGContextRelease(ctx); return EXIT_BACKEND_UNAVAILABLE; }

  CTFontDescriptorRef descriptor = CTFontCreateWithName(family, c->size, NULL);
  CFRelease(family);

  CFMutableArrayRef feature_array = mac_feature_array(case_index);
  CFTypeRef attr_keys[] = { kCTFontFeatureSettingsAttribute };
  CFTypeRef attr_vals[] = { feature_array };
  CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, attr_keys,
                                             attr_vals, 1, NULL, NULL);
  CTFontDescriptorRef styled = CTFontDescriptorCreateCopyWithAttributes(descriptor, attrs);
  CFRelease(attrs);
  CFRelease(feature_array);

  CTFontRef font = CTFontCreateWithFontDescriptor(styled, c->size, NULL);
  CFRelease(styled);
  CFRelease(descriptor);

  CFStringRef text = CFStringCreateWithCString(kCFAllocatorDefault, c->text,
                                               kCFStringEncodingUTF8);
  CFDictionaryRef font_attr = CFDictionaryCreate(kCFAllocatorDefault,
      (const void**)&kCTFontAttributeName, (const void**)&font, 1, NULL, NULL);
  CFAttributedStringRef attr_string =
      CFAttributedStringCreate(kCFAllocatorDefault, text, font_attr);
  CFRelease(font_attr);
  CFRelease(text);

  CTLineRef line = CTLineCreateWithAttributedString(attr_string);
  CFRelease(attr_string);

  CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
  CGContextSetTextPosition(ctx, 12, 12);
  CTLineDraw(line, ctx);

  CFRelease(line);
  CFRelease(font);

  /* golden PNG */
  char path[512];
  snprintf(path, sizeof(path), "%s_%s.png", c->name, this_platform_tag());
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  if (img) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                           (const UInt8*)path,
                                                           strlen(path), false);
    if (url) {
      CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url,
                                                                   kUTTypePNG, 1, NULL);
      if (dest) {
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
      }
      CFRelease(url);
    }
    CGImageRelease(img);
  }

  int rc = EXIT_PASS;
  if (out) {
    out->width = pw;
    out->height = ph;
    out->data = (uint8_t*)malloc(pw * ph * 4);
    if (out->data) memcpy(out->data, CGBitmapContextGetData(ctx), pw * ph * 4);
    else rc = EXIT_BACKEND_UNAVAILABLE;
  }

  CGContextRelease(ctx);
  return rc;
}

static CFMutableArrayRef mac_feature_array(int case_index) {
  CFMutableArrayRef array = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);
  for (int i = 0; i < 4; ++i) {
    const struct mac_feature_pair* pair = &k_pair_sets[case_index][i];
    if (!pair->tag) break;
    int feature = pair->feature, selector = pair->selector;
    CFNumberRef f_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &feature);
    CFNumberRef s_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &selector);
    CFTypeRef keys[] = { kCTFontFeatureTypeIdentifierKey, kCTFontFeatureSelectorIdentifierKey };
    CFTypeRef vals[] = { f_num, s_num };
    CFDictionaryRef dict = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2, NULL, NULL);
    CFArrayAppendValue(array, dict);
    CFRelease(dict);
    CFRelease(f_num);
    CFRelease(s_num);
  }
  return array;
}

static int mac_determinism_check(const struct ref_case* c) {
  /* Determinism is a Windows-seam property; macOS renders once as the golden
     producer. Return pass so --render works on both platforms. */
  (void)c;
  return EXIT_PASS;
}

static struct pixel_buffer mac_load_golden(const char* path) {
  (void)path;
  struct pixel_buffer buf = { 0 };
  return buf; /* --check is a Windows-side operation (see ctest wiring) */
}

#endif

/* Platform uniform surface used by main(). */
#ifdef _WIN32
#define PLATFORM_RENDER        win_render
#define PLATFORM_DETERMINISM   win_determinism_check
#define PLATFORM_LOAD_GOLDEN   win_load_golden
#else
#define PLATFORM_RENDER        mac_render
#define PLATFORM_DETERMINISM   mac_determinism_check
#define PLATFORM_LOAD_GOLDEN   mac_load_golden
#endif

/* --- shared helpers ----------------------------------------------------------- */

static int case_index_for_name(const char* name) {
  if (strcmp(name, "all") == 0) return -1;
  for (int i = 0; i < CASE_COUNT; ++i)
    if (strcmp(name, k_cases[i].name) == 0) return i;
  return -2;
}

static bool compare_buffers(const struct pixel_buffer* a,
                            const struct pixel_buffer* b,
                            const struct ref_case* c) {
  if (a->width != b->width || a->height != b->height) {
    printf("size mismatch for '%s': render %zux%zu vs golden %zux%zu\n",
           c->name, a->width, a->height, b->width, b->height);
    return false;
  }
  size_t pixels = a->width * a->height;
  size_t differing = 0;
  const uint8_t* pa = a->data;
  const uint8_t* pb = b->data;
  for (size_t i = 0; i < pixels; ++i) {
    bool pixel_diff = false;
    for (int ch = 0; ch < 4; ++ch) {
      int delta = (int)pa[i * 4 + ch] - (int)pb[i * 4 + ch];
      if (delta < 0) delta = -delta;
      if (delta > DIFF_TOLERANCE) { pixel_diff = true; break; }
    }
    if (pixel_diff) ++differing;
  }
  double percent = (double)differing * 100.0 / (double)pixels;
  printf("%s: %.2f%% of pixels differ (threshold %.1f%%) -> %s\n",
         c->name, percent, DIFF_MAX_PERCENT,
         percent < DIFF_MAX_PERCENT ? "PASS" : "FAIL");
  return percent < DIFF_MAX_PERCENT;
}

int main(int argc, char** argv) {
  bool do_render = false;
  bool do_check = false;
  const char* case_name = "all";

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--render") == 0) do_render = true;
    else if (strcmp(argv[i], "--check") == 0) do_check = true;
    else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) case_name = argv[++i];
    else { usage(); return EXIT_USAGE; }
  }
  if (do_render == do_check) { usage(); return EXIT_USAGE; }

  int first = case_index_for_name(case_name);
  int last = first;
  if (first == -1) { first = 0; last = CASE_COUNT - 1; }
  else if (first == -2) { usage(); return EXIT_USAGE; }

  printf("bitmap_diff: backend=%s mode=%s platform_golden=%s\n",
#ifdef _WIN32
         sk_context_backend_name(),
#else
         "coretext",
#endif
         do_render ? "render" : "check",
         other_platform_tag());

  for (int ci = first; ci <= last; ++ci) {
    const struct ref_case* c = &k_cases[ci];

    if (do_render) {
      struct pixel_buffer out = { 0 };
      int rc = PLATFORM_RENDER(c, &out);
      if (rc != EXIT_PASS) return rc;
      pixel_buffer_free(&out);
      rc = PLATFORM_DETERMINISM(c);
      if (rc != EXIT_PASS) return rc;
      continue;
    }

    /* --check: fresh render vs other-platform golden */
    char golden[512];
    snprintf(golden, sizeof(golden), "%s_%s.png", c->name, other_platform_tag());
    struct pixel_buffer fresh = { 0 };
    struct pixel_buffer gldn = { 0 };
    int rc = PLATFORM_RENDER(c, &fresh);
    if (rc != EXIT_PASS) return rc;
    gldn = PLATFORM_LOAD_GOLDEN(golden);
    if (!gldn.data) {
      printf("golden file missing: %s\n", golden);
      pixel_buffer_free(&fresh);
      return EXIT_GOLDEN_MISSING;
    }
    bool ok = compare_buffers(&fresh, &gldn, c);
    pixel_buffer_free(&fresh);
    pixel_buffer_free(&gldn);
    if (!ok) return EXIT_DIFF_EXCEEDED;
  }

  printf("bitmap_diff: PASS\n");
  return EXIT_PASS;
}