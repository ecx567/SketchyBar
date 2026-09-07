/*
 * platform/skia_zlib_shim.c
 *
 * Linkage shim for the vendored Skia SDK (third_party/skia).
 *
 * The SDK's bundled zlib is Chromium's build, which exports every public zlib
 * entry point with the Cr_z_* symbol prefix (Cr_z_inflate, Cr_z_inflateEnd, ...).
 * freetype2.lib (pulled in through skshaper -> HarfBuzz) was instead compiled
 * against plain zlib names, so a static link leaves `inflate`, `inflateEnd`,
 * `inflateInit2_` and `inflateReset` undefined.
 *
 * This TU forwards exactly the four plain names freetype needs to the prefixed
 * implementations. The z_stream layout below mirrors the public zlib 1.2.x /
 * zlib-ng layout byte-for-byte (both freetype and Chromium's zlib build against
 * that same ABI).
 */

#include <stddef.h>

typedef unsigned char Bytef;
typedef unsigned int  uInt;
typedef unsigned long uLong;
typedef void         *voidpf;

typedef voidpf (*alloc_func)(voidpf opaque, uInt items, uInt size);
typedef void   (*free_func)(voidpf opaque, voidpf address);

struct internal_state;

typedef struct z_stream_s {
    const Bytef        *next_in;
    uInt                avail_in;
    uLong               total_in;
    Bytef              *next_out;
    uInt                avail_out;
    uLong               total_out;
    const char         *msg;
    struct internal_state *state;
    alloc_func          zalloc;
    free_func           zfree;
    voidpf              opaque;
    int                 data_type;
    uLong               adler;
    uLong               reserved;
} z_stream;

typedef z_stream *z_streamp;

/* Chromium-prefixed implementations provided by the SDK (skia.lib / zlib.lib). */
int Cr_z_inflate(z_streamp strm, int flush);
int Cr_z_inflateEnd(z_streamp strm);
int Cr_z_inflateInit2_(z_streamp strm, int window_bits,
                       const char *version, int stream_size);
int Cr_z_inflateReset(z_streamp strm);

int inflate(z_streamp strm, int flush) {
    return Cr_z_inflate(strm, flush);
}

int inflateEnd(z_streamp strm) {
    return Cr_z_inflateEnd(strm);
}

int inflateInit2_(z_streamp strm, int window_bits,
                  const char *version, int stream_size) {
    return Cr_z_inflateInit2_(strm, window_bits, version, stream_size);
}

int inflateReset(z_streamp strm) {
    return Cr_z_inflateReset(strm);
}