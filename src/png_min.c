/* png_min.c — minimal PNG decoder (8-bit RGB/RGBA, non-interlaced).
 *
 * Scope is deliberately narrow: it exists to load one Android drawable out of
 * the game's APK. Both images shipped in gtacw.apk's res/drawable-xxxhdpi-v4/
 * are depth-8, non-interlaced, colour type 2 or 6, so that is exactly what is
 * implemented. Everything else is refused with a message instead of being
 * half-decoded into garbage.
 */

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "png_min.h"

static unsigned rd32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8)  |  (unsigned)p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Undo the per-scanline filter. `bpp` is bytes per pixel (filtering works on
 * whole bytes, offset by one pixel), `prev` may be NULL for the first row. */
static void unfilter(int type, unsigned char *row, const unsigned char *prev,
                     size_t stride, int bpp)
{
    switch (type) {
    case 0:
        break;
    case 1:
        for (size_t i = bpp; i < stride; i++) row[i] = (unsigned char)(row[i] + row[i - bpp]);
        break;
    case 2:
        if (prev) for (size_t i = 0; i < stride; i++) row[i] = (unsigned char)(row[i] + prev[i]);
        break;
    case 3:
        for (size_t i = 0; i < stride; i++) {
            int a = (i >= (size_t)bpp) ? row[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            row[i] = (unsigned char)(row[i] + ((a + b) >> 1));
        }
        break;
    case 4:
        for (size_t i = 0; i < stride; i++) {
            int a = (i >= (size_t)bpp) ? row[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
            row[i] = (unsigned char)(row[i] + paeth(a, b, c));
        }
        break;
    default:
        break;   /* caller validates; treat unknown as None */
    }
}

unsigned char *png_decode_rgba(const unsigned char *data, size_t len,
                               int *out_w, int *out_h, const char **err)
{
    const char *dummy;
    if (!err) err = &dummy;
    *err = NULL;

    static const unsigned char sig[8] = { 0x89,'P','N','G','\r','\n',0x1a,'\n' };
    if (len < 8 + 25 || memcmp(data, sig, 8) != 0) { *err = "not a PNG"; return NULL; }

    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0, seen_ihdr = 0;
    unsigned char *idat = NULL;
    size_t idat_len = 0;

    /* Walk the chunk list, concatenating IDAT payloads. */
    size_t i = 8;
    while (i + 12 <= len) {
        unsigned clen = rd32(data + i);
        const unsigned char *ctype4 = data + i + 4;
        const unsigned char *payload = data + i + 8;
        if (clen > len || i + 12 + clen > len) { *err = "truncated chunk"; goto fail; }

        if (!memcmp(ctype4, "IHDR", 4)) {
            if (clen < 13) { *err = "short IHDR"; goto fail; }
            w         = (int)rd32(payload);
            h         = (int)rd32(payload + 4);
            depth     = payload[8];
            ctype     = payload[9];
            interlace = payload[12];
            seen_ihdr = 1;
            if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { *err = "implausible size"; goto fail; }
            if (depth != 8)        { *err = "only 8-bit samples supported";   goto fail; }
            if (ctype != 2 && ctype != 6) { *err = "only RGB/RGBA supported"; goto fail; }
            if (interlace != 0)    { *err = "interlaced PNG not supported";   goto fail; }
        } else if (!memcmp(ctype4, "IDAT", 4)) {
            if (!seen_ihdr) { *err = "IDAT before IHDR"; goto fail; }
            unsigned char *n = realloc(idat, idat_len + clen);
            if (!n) { *err = "out of memory"; goto fail; }
            idat = n;
            memcpy(idat + idat_len, payload, clen);
            idat_len += clen;
        } else if (!memcmp(ctype4, "IEND", 4)) {
            break;
        }
        i += 12 + clen;
    }

    if (!seen_ihdr || !idat) { *err = "no image data"; goto fail; }

    int  bpp    = (ctype == 6) ? 4 : 3;
    size_t stride = (size_t)w * bpp;
    size_t raw_len = (stride + 1) * (size_t)h;   /* +1 filter byte per row */

    unsigned char *raw = malloc(raw_len);
    if (!raw) { *err = "out of memory"; goto fail; }

    z_stream zs;
    memset(&zs, 0, sizeof zs);
    zs.next_in   = idat;
    zs.avail_in  = (uInt)idat_len;
    zs.next_out  = raw;
    zs.avail_out = (uInt)raw_len;
    if (inflateInit(&zs) != Z_OK) { free(raw); *err = "inflateInit failed"; goto fail; }
    int zr = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (zs.total_out != raw_len) {
        free(raw);
        *err = (zr == Z_DATA_ERROR) ? "corrupt zlib stream" : "unexpected image size";
        goto fail;
    }

    unsigned char *out = malloc((size_t)w * h * 4);
    if (!out) { free(raw); *err = "out of memory"; goto fail; }

    unsigned char *prev = NULL;
    for (int y = 0; y < h; y++) {
        unsigned char *row = raw + (stride + 1) * (size_t)y;
        int ftype = row[0];
        row++;                                  /* past the filter byte */
        if (ftype > 4) { free(raw); free(out); *err = "bad filter type"; goto fail; }
        unfilter(ftype, row, prev, stride, bpp);

        unsigned char *dst = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = row[x * bpp + 0];
            dst[x * 4 + 1] = row[x * bpp + 1];
            dst[x * 4 + 2] = row[x * bpp + 2];
            dst[x * 4 + 3] = (bpp == 4) ? row[x * bpp + 3] : 255;
        }
        prev = row;
    }

    free(raw);
    free(idat);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return out;

fail:
    free(idat);
    return NULL;
}
