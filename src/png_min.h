/* png_min.h — just enough PNG to load the installer's artwork.
 *
 * The installer takes its background out of the player's own APK, and Android
 * drawables are PNG, which core SDL2 cannot load (SDL_LoadBMP only). Rather
 * than pull in SDL2_image — which is not present on every CFW — we decode the
 * one image ourselves with zlib, which the port already bundles.
 */
#ifndef PNG_MIN_H
#define PNG_MIN_H

#include <stddef.h>

/* Decode an 8-bit, non-interlaced RGB or RGBA PNG into a freshly malloc'd
 * RGBA8888 buffer (w*h*4 bytes, caller frees).
 *
 * Returns NULL on failure and, if err is non-NULL, points *err at a static
 * description of why. Anything outside the supported subset (16-bit samples,
 * palettes, greyscale, Adam7 interlacing) is reported rather than guessed at.
 */
unsigned char *png_decode_rgba(const unsigned char *data, size_t len,
                               int *out_w, int *out_h, const char **err);

#endif /* PNG_MIN_H */
