/* installer.c — first-run data extractor for the GTA: Chinatown Wars port.
 *
 * The port ships no game data. The player supplies the Android APK and OBB;
 * this program unpacks them into the game directory and shows progress on a
 * full-screen SDL2 splash.
 *
 * Design notes
 * ------------
 *  * NO artwork is redistributed. The background is pulled at runtime out of
 *    the player's own APK (res/drawable-xxxhdpi-v4/banner.png). That drawable
 *    is a PNG, which core SDL2 cannot load, so png_min.c decodes it with zlib
 *    rather than dragging in SDL2_image.
 *  * SDL2 core only. Text is drawn from a bitmap font baked into the binary
 *    (installer_font.h) — no SDL2_ttf. Neither of those satellite libraries
 *    can be relied upon across CFWs.
 *  * The renderer is 2D (accelerated if available, software otherwise) — but
 *    that does NOT make it GL-free. SDL2's KMSDRM backend is GBM/EGL based and
 *    has no plain-framebuffer path, so EGL is loaded at window-creation time
 *    regardless. The launcher therefore has to export SDL_VIDEODRIVER,
 *    SDL_VIDEO_EGL_DRIVER, SDL_VIDEO_GL_DRIVER and LD_LIBRARY_PATH BEFORE
 *    running this; without them it fails with "Can't load EGL/GL library on
 *    window creation" and falls back to text.
 *  * If SDL cannot open a window at all, everything degrades to line-by-line
 *    text on stdout, which the launcher shows on the console. A silent
 *    multi-minute extraction would look like a hang.
 *  * Extraction shells out to unzip and counts its "inflating:" lines. That is
 *    exactly what the shell installer did before, so the behaviour is proven;
 *    unzip is already required by every CFW that could run the old installer.
 *
 * Exit codes: 0 = installed (or nothing to do), 1 = missing archives,
 *             2 = extraction failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <SDL2/SDL.h>

#include "installer_font.h"
#include "png_min.h"

#define SCREEN_W 640
#define SCREEN_H 480

/* Artwork comes out of the player's own APK — nothing copyrighted ships in
 * this package. res/drawable-xxxhdpi-v4/banner.png is 320x180, so it lands at
 * exactly 2x across the top and leaves a clean strip underneath for the UI. */
#define APK_ART   "res/drawable-xxxhdpi-v4/banner.png"
#define ART_W     640
#define ART_H     360
#define ART_Y     ((SCREEN_H - ART_H) / 2)   /* centred: 60 -> 60..420 */

/* The artwork is centred, so the UI lives in the two 60px letterbox bands it
 * leaves behind: the heading above it, everything else below. */
#define R_TITLE   14           /* step heading, 2x  ->  14..46  */
#define R_INFO    422          /* "n / n  -  member" -> 422..438 */
#define R_FOOTER  461          /* "Mafradon"        -> 461..477 */

#define BAR_X 40
#define BAR_Y 442              /* with its 2px edge -> 440..458 */
#define BAR_W 500
#define BAR_H 14

static const SDL_Color COL_DARK  = { 235, 238, 242, 255 };  /* UI strip is black */
static const SDL_Color COL_FOOT  = { 150, 158, 168, 255 };
/* These are only ever drawn on the dark translucent panel, so they are the
 * bright variants rather than the ones that would suit the pale sky band. */
static const SDL_Color COL_ERR   = { 255, 105, 95,  255 };
static const SDL_Color COL_OK    = { 105, 220, 130, 255 };
static const SDL_Color BAR_FILL  = { 240, 170, 30, 255 };  /* amber, pops on the blue */
static const SDL_Color BAR_TROUGH= { 60,  100, 130, 255 };
static const SDL_Color BAR_EDGE  = { 20,  35,  50, 255 };

typedef struct {
    int           active;      /* 0 => text-only fallback */
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *bg;
    SDL_Texture  *font;        /* one row of 95 glyphs, white on transparent */
} ui_t;

/* ---------------------------------------------------------------- text ---- */

static SDL_Texture *build_font_texture(SDL_Renderer *ren)
{
    int nglyph = FONT_LAST - FONT_FIRST + 1;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(
        0, nglyph * FONT_W, FONT_H, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;

    /* We index the surface as 32-bit words, so refuse anything that isn't
     * 4 bytes per pixel rather than writing past the end of the buffer. */
    if (s->format->BytesPerPixel != 4) {
        SDL_FreeSurface(s);
        return NULL;
    }

    SDL_LockSurface(s);
    Uint32 *px = (Uint32 *)s->pixels;
    int pitch = s->pitch / 4;
    /* White where the glyph bit is set, fully transparent elsewhere; the draw
     * colour is applied later with SDL_SetTextureColorMod. */
    Uint32 on  = SDL_MapRGBA(s->format, 255, 255, 255, 255);
    Uint32 off = SDL_MapRGBA(s->format, 255, 255, 255, 0);
    for (int g = 0; g < nglyph; g++)
        for (int row = 0; row < FONT_H; row++) {
            unsigned char bits = font_bits[g][row];
            for (int col = 0; col < FONT_W; col++)
                px[row * pitch + g * FONT_W + col] =
                    (bits & (0x80 >> col)) ? on : off;
        }
    SDL_UnlockSurface(s);

    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }

static void draw_text(ui_t *ui, int x, int y, int scale, SDL_Color c, const char *s)
{
    if (!ui->active || !ui->font) return;
    SDL_SetTextureColorMod(ui->font, c.r, c.g, c.b);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++, x += FONT_W * scale) {
        int ch = *p;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        SDL_Rect src = { (ch - FONT_FIRST) * FONT_W, 0, FONT_W, FONT_H };
        SDL_Rect dst = { x, y, FONT_W * scale, FONT_H * scale };
        SDL_RenderCopy(ui->ren, ui->font, &src, &dst);
    }
}

static void draw_text_centre(ui_t *ui, int y, int scale, SDL_Color c, const char *s)
{
    draw_text(ui, (SCREEN_W - text_w(s, scale)) / 2, y, scale, c, s);
}

/* Shorten in the middle so both the start and the extension stay readable. */
static void fit_text(char *dst, size_t dstsz, const char *src, int max_chars)
{
    size_t len = strlen(src);
    if ((int)len <= max_chars || max_chars < 8) {
        snprintf(dst, dstsz, "%s", src);
        return;
    }
    int keep_head = (max_chars - 3) / 2;
    int keep_tail = max_chars - 3 - keep_head;
    snprintf(dst, dstsz, "%.*s...%s", keep_head, src, src + len - keep_tail);
}

/* ---------------------------------------------------------------- chrome -- */

static void ui_begin(ui_t *ui)
{
    if (!ui->active) return;
    SDL_SetRenderDrawColor(ui->ren, 0, 0, 0, 255);
    SDL_RenderClear(ui->ren);
    if (ui->bg) {
        SDL_Rect dst = { 0, ART_Y, ART_W, ART_H };
        SDL_RenderCopy(ui->ren, ui->bg, NULL, &dst);
    }
}

static void ui_footer(ui_t *ui)
{
    if (!ui->active) return;
    draw_text_centre(ui, R_FOOTER, 1, COL_FOOT, "Mafradon");
}

static void ui_end(ui_t *ui)
{
    if (!ui->active) return;
    SDL_RenderPresent(ui->ren);
}

/* Message screens can't use the flat sky/blue bands alone — they need more
 * lines than those bands hold, and text over the artwork is unreadable. Drop a
 * translucent slate over the middle of the art and write on that instead. */
#define PANEL_X 40
#define PANEL_Y 140
#define PANEL_W 560
#define PANEL_H 200

static const SDL_Color COL_PANEL_TEXT = { 232, 240, 248, 255 };

static void draw_panel(ui_t *ui)
{
    if (!ui->active) return;
    SDL_Rect r = { PANEL_X, PANEL_Y, PANEL_W, PANEL_H };
    SDL_SetRenderDrawBlendMode(ui->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ui->ren, 10, 20, 30, 215);
    SDL_RenderFillRect(ui->ren, &r);
    SDL_SetRenderDrawColor(ui->ren, BAR_FILL.r, BAR_FILL.g, BAR_FILL.b, 255);
    SDL_RenderDrawRect(ui->ren, &r);
    SDL_SetRenderDrawBlendMode(ui->ren, SDL_BLENDMODE_NONE);
}

static void draw_bar(ui_t *ui, int pct)
{
    if (!ui->active) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    SDL_Rect edge   = { BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4 };
    SDL_Rect trough = { BAR_X, BAR_Y, BAR_W, BAR_H };
    SDL_Rect fill   = { BAR_X, BAR_Y, BAR_W * pct / 100, BAR_H };

    SDL_SetRenderDrawColor(ui->ren, BAR_EDGE.r, BAR_EDGE.g, BAR_EDGE.b, 255);
    SDL_RenderFillRect(ui->ren, &edge);
    SDL_SetRenderDrawColor(ui->ren, BAR_TROUGH.r, BAR_TROUGH.g, BAR_TROUGH.b, 255);
    SDL_RenderFillRect(ui->ren, &trough);
    if (fill.w > 0) {
        SDL_SetRenderDrawColor(ui->ren, BAR_FILL.r, BAR_FILL.g, BAR_FILL.b, 255);
        SDL_RenderFillRect(ui->ren, &fill);
    }

    char pctbuf[8];
    snprintf(pctbuf, sizeof pctbuf, "%3d%%", pct);
    draw_text(ui, BAR_X + BAR_W + 14, BAR_Y - 1, 1, COL_FOOT, pctbuf);
}

/* One full frame of the extraction screen. */
static void draw_progress(ui_t *ui, const char *title, const char *sub,
                          const char *file, int pct)
{
    if (!ui->active) return;
    ui_begin(ui);
    draw_text_centre(ui, R_TITLE, 2, COL_DARK, title);

    /* sub + file share one row; 80 chars fit across at scale 1. */
    char info[96];
    if (sub && file && *file) snprintf(info, sizeof info, "%s  -  %s", sub, file);
    else if (sub)             snprintf(info, sizeof info, "%s", sub);
    else if (file)            snprintf(info, sizeof info, "%s", file);
    else                      info[0] = 0;
    if (info[0]) {
        char shown[96];
        fit_text(shown, sizeof shown, info, 78);
        draw_text_centre(ui, R_INFO, 1, COL_FOOT, shown);
    }
    draw_bar(ui, pct);
    ui_footer(ui);
    ui_end(ui);
}

/* Keep the compositor/input happy; also lets the user see a responsive screen. */
static void pump(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) { /* nothing is interactive during install */ }
}

/* ------------------------------------------------------------------ util -- */

/* Single-quote a path for /bin/sh: ' -> '\'' */
static void shell_quote(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;
    if (dstsz < 3) { if (dstsz) dst[0] = 0; return; }
    dst[o++] = '\'';
    for (const char *p = src; *p && o + 5 < dstsz; p++) {
        if (*p == '\'') {
            memcpy(dst + o, "'\\''", 4);
            o += 4;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o++] = '\'';
    dst[o]   = 0;
}

/* First file in dir whose name ends with ext (case-insensitive). */
static int find_by_ext(const char *dir, const char *ext, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name), el = strlen(ext);
        if (nl <= el) continue;
        if (strcasecmp(e->d_name + nl - el, ext) == 0) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* The launcher unbinds the vtconsole before running us, because SDL needs the
 * framebuffer. If we end up in text mode that leaves our output going to a
 * console nobody can see — a silent black screen for the whole extraction,
 * which is the exact thing the text fallback exists to avoid. So put the
 * console back. The launcher chmods these for us; failing is harmless. */
static void rebind_console(void)
{
    static const char *paths[] = { "/sys/class/vtconsole/vtcon0/bind",
                                   "/sys/class/vtconsole/vtcon1/bind" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE *f = fopen(paths[i], "w");
        if (!f) continue;
        fputs("1\n", f);
        fclose(f);
    }
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Pull one member out of the APK and turn it into a texture.
 *
 * `unzip -p` streams the member to stdout, so nothing is written to disk. The
 * APK is the player's own file and is deleted once the install succeeds, which
 * is fine: the installer only ever runs while it is still present. */
static SDL_Texture *load_apk_art(SDL_Renderer *ren, const char *apk, const char *member)
{
    char qa[4096], cmd[8300];
    shell_quote(qa, sizeof qa, apk);
    snprintf(cmd, sizeof cmd, "unzip -p %s '%s' 2>/dev/null", qa, member);

    FILE *f = popen(cmd, "r");
    if (!f) return NULL;

    size_t cap = 1 << 18, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { pclose(f); return NULL; }
    for (;;) {
        if (len == cap) {
            unsigned char *n = realloc(buf, cap * 2);
            if (!n) { free(buf); pclose(f); return NULL; }
            buf = n; cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        if (got == 0) break;
        len += got;
    }
    pclose(f);

    if (len == 0) {
        free(buf);
        fprintf(stderr, "installer: %s not found inside the APK\n", member);
        return NULL;
    }

    int w = 0, h = 0;
    const char *err = NULL;
    unsigned char *rgba = png_decode_rgba(buf, len, &w, &h, &err);
    free(buf);
    if (!rgba) {
        fprintf(stderr, "installer: could not decode %s (%s)\n", member, err ? err : "?");
        return NULL;
    }

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(
        rgba, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    SDL_Texture *t = s ? SDL_CreateTextureFromSurface(ren, s) : NULL;
    if (s) SDL_FreeSurface(s);
    free(rgba);
    if (!t) fprintf(stderr, "installer: texture failed (%s)\n", SDL_GetError());
    return t;
}

/* ------------------------------------------------------------- extraction -- */

static long count_members(const char *archive, const char *filter)
{
    char qa[4096], cmd[8300];
    shell_quote(qa, sizeof qa, archive);
    if (filter && *filter)
        snprintf(cmd, sizeof cmd, "unzip -Z1 %s '%s' 2>/dev/null", qa, filter);
    else
        snprintf(cmd, sizeof cmd, "unzip -Z1 %s 2>/dev/null", qa);

    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    long n = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) n++;
    pclose(f);
    return n;
}

/* Returns 0 on success. Repaints only when the percentage changes, so a
 * 2600-member archive costs ~100 frames rather than 2600. */
static int extract(ui_t *ui, const char *archive, const char *dest,
                   const char *filter, int junk_paths, const char *title)
{
    char qa[4096], qd[4096], cmd[8600];
    shell_quote(qa, sizeof qa, archive);
    shell_quote(qd, sizeof qd, dest);

    /* Counting members reads the whole archive once, which on a slow SD card
     * is a visible stall for a 900MB OBB. Put a frame up BEFORE that, so the
     * screen never sits blank while it happens. */
    draw_progress(ui, title, "reading archive...", base_name(archive), 0);

    long total = count_members(archive, filter);
    if (total <= 0) total = 1;

    if (filter && *filter)
        snprintf(cmd, sizeof cmd, "unzip -o %s %s '%s' -d %s 2>&1",
                 junk_paths ? "-j" : "", qa, filter, qd);
    else
        snprintf(cmd, sizeof cmd, "unzip -o %s %s -d %s 2>&1",
                 junk_paths ? "-j" : "", qa, qd);

    fprintf(stderr, "installer: %s (%ld members)\n", base_name(archive), total);
    draw_progress(ui, title, "starting...", base_name(archive), 0);
    fflush(stdout);

    FILE *f = popen(cmd, "r");
    if (!f) {
        fprintf(stderr, "installer: failed to run unzip\n");
        return -1;
    }

    char line[8192], sub[64], shown[96];
    long count = 0;
    int last = -1;
    while (fgets(line, sizeof line, f)) {
        const char *tag = NULL;
        if      (strstr(line, "inflating:"))  tag = "inflating:";
        else if (strstr(line, "extracting:")) tag = "extracting:";
        else if (strstr(line, "creating:"))   tag = "creating:";

        if (!tag) {
            if (strstr(line, "cannot find") || strstr(line, "End-of-central-directory")
                || strncmp(line, "error", 5) == 0)
                fprintf(stderr, "installer: unzip: %s", line);
            continue;
        }

        count++;
        int pct = (int)(count * 100 / total);
        if (pct == last) continue;
        last = pct;

        /* name follows the tag; trim whitespace and the trailing newline */
        const char *name = strstr(line, tag) + strlen(tag);
        while (*name == ' ' || *name == '\t') name++;
        char nm[4096];
        snprintf(nm, sizeof nm, "%s", name);
        nm[strcspn(nm, "\r\n")] = 0;
        char *sp = strpbrk(nm, " \t");   /* unzip appends notes on some lines */
        if (sp) *sp = 0;

        snprintf(sub, sizeof sub, "%ld / %ld files", count, total);
        fit_text(shown, sizeof shown, base_name(nm), 60);

        if (ui->active) {
            pump();
            draw_progress(ui, title, sub, shown, pct);
        } else if (pct % 5 == 0) {
            printf("  %s  %3d%%  (%ld/%ld)\n", title, pct, count, total);
            fflush(stdout);
        }
    }

    int rc = pclose(f);
    draw_progress(ui, title, "done", "", 100);
    return (rc == -1) ? -1 : 0;
}


/* ── welcome / setup screen ──────────────────────────────────────────────
 *
 * Shown once, before anything is unpacked. Two jobs:
 *   * pick a language — the engine ships English/French/German/Italian/
 *     Spanish/Japanese in the GXT and selects via the device locale, which
 *     handhelds do not set. The choice is written to conf/language.txt and
 *     the launcher turns it into $LANG on the way into the game.
 *   * check free space BEFORE unpacking ~900MB, because running out midway
 *     leaves a half-extracted directory that looks like a corrupt install.
 */
static const struct { const char *code, *name; } LANGS[] = {
    { "en", "English"  }, { "fr", "Francais" }, { "de", "Deutsch"  },
    { "it", "Italiano" }, { "es", "Espanol"  }, { "ja", "Nihongo"  },
};
#define NLANGS ((int)(sizeof LANGS / sizeof LANGS[0]))

static void human_size(char *dst, size_t n, unsigned long long bytes)
{
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) snprintf(dst, n, "%.1f GB", gb);
    else           snprintf(dst, n, "%.0f MB", (double)bytes / (1024.0 * 1024.0));
}

static unsigned long long file_size(const char *p)
{
    struct stat st;
    return (stat(p, &st) == 0) ? (unsigned long long)st.st_size : 0ULL;
}

static unsigned long long free_space(const char *dir)
{
    struct statvfs v;
    if (statvfs(dir, &v) != 0) return 0ULL;
    return (unsigned long long)v.f_bavail * (unsigned long long)v.f_frsize;
}

static int read_language(const char *dir)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/conf/language.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char buf[16] = { 0 };
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return 0; }
    fclose(f);
    for (int i = 0; i < NLANGS; i++)
        if (strncasecmp(buf, LANGS[i].code, 2) == 0) return i;
    return 0;
}

static void write_language(const char *dir, int idx)
{
    char d[4096], p[4096];
    snprintf(d, sizeof d, "%s/conf", dir);
    mkdir(d, 0777);
    snprintf(p, sizeof p, "%s/conf/language.txt", dir);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "installer: cannot write %s\n", p); return; }
    fprintf(f, "%s\n", LANGS[idx].code);
    fclose(f);
    fprintf(stderr, "installer: language set to %s\n", LANGS[idx].code);
}

/* Returns 1 to proceed with the install, 0 to abort. */
static int welcome_screen(ui_t *ui, const char *dir,
                          const char *obb, const char *apk)
{
    int lang = read_language(dir);

    /* Peak usage is the extracted data while the archives are still present,
     * so the OBB's own size is a good proxy for what we must have spare.
     * Measured on-device: a 906,553,432-byte OBB unpacks to ~889 MB, i.e.
     * almost exactly its own size, plus ~9.5 MB of GXT from the APK's assets.
     * The 5% headroom below makes the check land ~3% conservative, which is
     * the direction we want — refusing early beats dying half-extracted and
     * leaving a directory that looks like a corrupt install. */
    unsigned long long need  = file_size(obb) + file_size(apk) / 2;
    need += need / 20;                                  /* 5% headroom */
    unsigned long long avail = free_space(dir);
    int enough = (avail == 0) || (avail >= need);       /* unknown => allow */

    char s_need[32], s_free[32];
    human_size(s_need, sizeof s_need, need);
    human_size(s_free, sizeof s_free, avail);

    if (!ui->active) {                                  /* text fallback */
        printf("\n  First-run setup: language=%s  needs %s, %s free%s\n",
               LANGS[lang].name, s_need, s_free,
               enough ? "" : "  *** NOT ENOUGH SPACE ***");
        fflush(stdout);
        write_language(dir, lang);
        return enough;
    }

    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i))) break;

    /* If there is no pad we cannot rely on being able to confirm anything, so
     * show the screen briefly and continue rather than trapping the user. */
    int countdown = pad ? 0 : 20;
    Uint32 last_tick = SDL_GetTicks();
    int result = -1;

    while (result < 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int left = 0, right = 0, ok = 0, cancel = 0;
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  left = 1;   break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: right = 1;  break;
                case SDL_CONTROLLER_BUTTON_A:
                case SDL_CONTROLLER_BUTTON_START:      ok = 1;     break;
                case SDL_CONTROLLER_BUTTON_B:          cancel = 1; break;
                default: break;
                }
            } else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_LEFT:  left = 1;  break;
                case SDLK_RIGHT: right = 1; break;
                case SDLK_RETURN: case SDLK_SPACE: ok = 1; break;
                case SDLK_ESCAPE: cancel = 1; break;
                default: break;
                }
            }
            if (left)  { lang = (lang + NLANGS - 1) % NLANGS; countdown = 0; }
            if (right) { lang = (lang + 1) % NLANGS;          countdown = 0; }
            if (cancel) result = 0;
            if (ok)     result = enough ? 1 : 0;
        }

        if (countdown > 0 && SDL_GetTicks() - last_tick >= 1000) {
            last_tick = SDL_GetTicks();
            if (--countdown == 0) result = enough;
        }

        char line[128];
        ui_begin(ui);
        draw_panel(ui);
        draw_text_centre(ui, 158, 2, COL_PANEL_TEXT, "FIRST-RUN SETUP");

        snprintf(line, sizeof line, "<  %s  >", LANGS[lang].name);
        draw_text_centre(ui, 200, 1, COL_PANEL_TEXT, "Language");
        draw_text_centre(ui, 218, 2, BAR_FILL,     line);

        snprintf(line, sizeof line, "needs %s   free %s", s_need, s_free);
        draw_text_centre(ui, 258, 1, enough ? COL_OK : COL_ERR, line);

        if (!enough)
            draw_text_centre(ui, 276, 1, COL_ERR, "not enough space to install");

        if (countdown > 0)
            snprintf(line, sizeof line, "no controller - starting in %2ds", countdown);
        else if (enough)
            snprintf(line, sizeof line, "%s",
                     "left/right  change     A or START  install     B  quit");
        else
            snprintf(line, sizeof line, "%s", "B  quit");
        draw_text_centre(ui, 306, 1, COL_PANEL_TEXT, line);

        ui_footer(ui);
        ui_end(ui);
        SDL_Delay(16);
    }

    if (pad) SDL_GameControllerClose(pad);
    if (result) write_language(dir, lang);
    return result;
}

/* ------------------------------------------------------------------ main -- */

static void screen_missing(ui_t *ui, const char *dir, const char *obb, const char *apk)
{
    char line[256];
    for (int s = 20; s > 0; s--) {
        if (ui->active) {
            pump();
            ui_begin(ui);
            draw_panel(ui);
            draw_text_centre(ui, 158, 2, COL_ERR, "SETUP INCOMPLETE");
            draw_text_centre(ui, 200, 1, COL_PANEL_TEXT, "Copy BOTH of these into:");
            char shown[96];
            fit_text(shown, sizeof shown, dir, 66);
            draw_text_centre(ui, 218, 1, COL_PANEL_TEXT, shown);

            if (obb) { snprintf(line, sizeof line, "found   %s", base_name(obb)); }
            else     { snprintf(line, sizeof line, "MISSING   main.4...gtactw.obb"); }
            draw_text_centre(ui, 252, 1, obb ? COL_OK : COL_ERR, line);

            if (apk) { snprintf(line, sizeof line, "found   %s", base_name(apk)); }
            else     { snprintf(line, sizeof line, "MISSING   gtacw.apk"); }
            draw_text_centre(ui, 270, 1, apk ? COL_OK : COL_ERR, line);

            snprintf(line, sizeof line, "returning to the menu in %2ds", s);
            draw_text_centre(ui, 306, 1, COL_PANEL_TEXT, line);
            ui_footer(ui);
            ui_end(ui);
        } else if (s == 20) {
            printf("\n  SETUP INCOMPLETE — copy BOTH files into %s\n", dir);
            printf("    %s main.4.com.rockstargames.gtactw.obb\n", obb ? "[ok]     " : "[MISSING]");
            printf("    %s gtacw.apk\n", apk ? "[ok]     " : "[MISSING]");
            fflush(stdout);
        }
        SDL_Delay(1000);
    }
}

static void screen_failed(ui_t *ui, const char *msg)
{
    fprintf(stderr, "installer: FAILED — %s\n", msg);
    for (int s = 15; s > 0; s--) {
        if (!ui->active) break;
        pump();
        ui_begin(ui);
        draw_panel(ui);
        draw_text_centre(ui, 180, 2, COL_ERR, "INSTALL FAILED");
        char shown[96];
        fit_text(shown, sizeof shown, msg, 66);
        draw_text_centre(ui, 230, 1, COL_PANEL_TEXT, shown);
        draw_text_centre(ui, 262, 1, COL_PANEL_TEXT, "see gtactw.log for details");
        ui_footer(ui);
        ui_end(ui);
        SDL_Delay(1000);
    }
    if (!ui->active) printf("  INSTALL FAILED: %s\n", msg);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : getenv("GTACTW_DIR");
    if (!dir || !*dir) dir = ".";

    /* Sentinels: ROM.WAD comes from the OBB, GXT.obb.mp3 from the APK assets. */
    char rom[4096], gxt[4096], eng[4096];
    snprintf(rom, sizeof rom, "%s/ROM.WAD", dir);
    snprintf(gxt, sizeof gxt, "%s/GXT.obb.mp3", dir);
    snprintf(eng, sizeof eng, "%s/libCTW.so", dir);
    if (file_exists(rom) && file_exists(gxt) && file_exists(eng)) {
        fprintf(stderr, "installer: game data already present, nothing to do\n");
        return 0;
    }

    ui_t ui;
    memset(&ui, 0, sizeof ui);

    char obb[4096], apk[4096];
    int have_obb = find_by_ext(dir, ".obb", obb, sizeof obb);
    int have_apk = find_by_ext(dir, ".apk", apk, sizeof apk);

    /* The APK is both the source of the game's text assets AND of the artwork
     * this screen is drawn from, so without it there is nothing to show and
     * nothing to do. Report it as plain text and stop. */
    if (!have_apk) {
        rebind_console();
        fprintf(stderr, "installer: gtacw.apk file not found\n");
        printf("\n  gtacw.apk file not found\n\n");
        printf("  Copy gtacw.apk into:\n    %s\n\n", dir);
        fflush(stdout);
        return 1;
    }

    /* Video only — no audio, no GL. A failure here is not fatal: we fall back
     * to printing progress, because a silent install looks like a hang. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0) {
        ui.win = SDL_CreateWindow("GTA: Chinatown Wars — Installer",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SCREEN_W, SCREEN_H,
                                  SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (ui.win) {
            ui.ren = SDL_CreateRenderer(ui.win, -1, SDL_RENDERER_ACCELERATED);
            if (!ui.ren)
                ui.ren = SDL_CreateRenderer(ui.win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (ui.ren) {
            /* Everything is authored for 640x480; let SDL letterbox it onto
             * whatever panel this device actually has. */
            SDL_RenderSetLogicalSize(ui.ren, SCREEN_W, SCREEN_H);
            SDL_ShowCursor(SDL_DISABLE);

            /* Smooth the 2x upscale of the 320x180 banner. */
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

            /* Not fatal — ui_begin() just paints black behind the text. */
            ui.bg = load_apk_art(ui.ren, apk, APK_ART);

            ui.font = build_font_texture(ui.ren);
            if (ui.font) {
                ui.active = 1;
            } else {
                /* Without glyphs we could only draw a bar with no words on it,
                 * which tells the user nothing. Text mode is more useful. */
                fprintf(stderr, "installer: no font texture (%s) — text mode\n",
                        SDL_GetError());
            }
        } else {
            fprintf(stderr, "installer: no renderer (%s) — text mode\n", SDL_GetError());
        }
    } else {
        fprintf(stderr, "installer: SDL_Init failed (%s) — text mode\n", SDL_GetError());
    }

    if (!ui.active) rebind_console();

    int rc = 0;
    if (!have_obb) {
        screen_missing(&ui, dir, have_obb ? obb : NULL, have_apk ? apk : NULL);
        rc = 1;
        goto done;
    }

    if (!welcome_screen(&ui, dir, obb, apk)) {
        fprintf(stderr, "installer: cancelled at the setup screen\n");
        rc = 1;
        goto done;
    }

    if (extract(&ui, obb, dir, NULL, 0, "Extracting game data") != 0 || !file_exists(rom)) {
        screen_failed(&ui, "the OBB did not produce ROM.WAD");
        rc = 2;
        goto done;
    }
    remove(obb);

    if (extract(&ui, apk, dir, "assets/*", 1, "Extracting text and fonts") != 0
        || !file_exists(gxt)) {
        screen_failed(&ui, "the APK did not produce GXT.obb.mp3");
        rc = 2;
        goto done;
    }
    /* The engine itself. libCTW.so is Rockstar's code and is NOT shipped with
     * this port — it is lifted out of the player's own APK here, exactly like
     * the game data, so the package redistributes nothing copyrighted. */
    if (extract(&ui, apk, dir, "lib/armeabi-v7a/libCTW.so", 1,
                "Extracting game engine") != 0 || !file_exists(eng)) {
        screen_failed(&ui, "the APK did not contain libCTW.so");
        rc = 2;
        goto done;
    }

    remove(apk);

    if (ui.active) {
        ui_begin(&ui);
        draw_text_centre(&ui, R_TITLE, 2, COL_DARK, "READY");
        draw_text_centre(&ui, R_INFO, 1, COL_FOOT, "starting the game");
        draw_bar(&ui, 100);
        ui_footer(&ui);
        ui_end(&ui);
        SDL_Delay(2500);
    } else {
        printf("  installation complete\n");
    }

done:
    if (ui.font) SDL_DestroyTexture(ui.font);
    if (ui.bg)   SDL_DestroyTexture(ui.bg);
    if (ui.ren)  SDL_DestroyRenderer(ui.ren);
    if (ui.win)  SDL_DestroyWindow(ui.win);
    SDL_Quit();
    return rc;
}
