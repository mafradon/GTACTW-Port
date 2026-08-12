/* mpg123_patch.c -- redirect mpg123 symbols in libCTW.so to system libmpg123 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <mpg123.h>

#include "so_util.h"
#include "mpg123_patch.h"

extern so_module gtactw_mod;

static int mpg123_param_hook(mpg123_handle *mh, enum mpg123_parms key,
                              long val, double fval) {
    val |= MPG123_FUZZY | MPG123_SEEKBUFFER | MPG123_GAPLESS;
    return mpg123_param(mh, key, val, fval);
}


/*
 * off_t ABI bridge: Android armeabi-v7a uses 32-bit off_t; our system libmpg123
 * uses 64-bit off_t (_FILE_OFFSET_BITS=64).  On ARM32 AAPCS a 64-bit argument
 * after a pointer lands in the aligned register pair r2:r3 (r1 is padding), so
 * the game's 32-bit value in r1 is completely ignored by the system function and
 * the remaining arguments shift, producing garbage pointers → crash.
 *
 * Each wrapper accepts the 32-bit convention (int32_t in r1) and explicitly
 * widens to off_t before calling the real symbol.
 */

static int32_t mpg123_feedseek32(mpg123_handle *mh,
                                  int32_t sampleoff,
                                  int whence,
                                  int32_t *input_offset) {
    off_t input64 = 0;
    int32_t ret = (int32_t)mpg123_feedseek(mh, (off_t)sampleoff, whence, &input64);
    if (input_offset)
        *input_offset = (int32_t)input64;
    return ret;
}

static int32_t mpg123_seek32(mpg123_handle *mh, int32_t sampleoff, int whence) {
    return (int32_t)mpg123_seek(mh, (off_t)sampleoff, whence);
}

static int mpg123_set_filesize32(mpg123_handle *mh, int32_t filesize) {
    return mpg123_set_filesize(mh, (off_t)filesize);
}

/* ── soft-float ABI bridges ──────────────────────────────────────────────
 *
 * libCTW.so is armeabi-v7a = soft-float, so a double argument arrives as an
 * aligned r2:r3 register PAIR. This binary is hard-float and expects it in
 * d0. These three mpg123 entry points are the only ones taking a float or
 * double BY VALUE; everything else takes ints or pointers and is unaffected.
 * (The same class of bug made pedestrian voices play at a fraction of speed
 * through OpenAL — see src/openal_patch.c.)
 */
#define SOFTFP __attribute__((pcs("aapcs")))

SOFTFP static int mpg123_volume_abi(mpg123_handle *mh, double vol)
    { return mpg123_volume(mh, vol); }
SOFTFP static int mpg123_volume_change_abi(mpg123_handle *mh, double change)
    { return mpg123_volume_change(mh, change); }
SOFTFP static int mpg123_eq_abi(mpg123_handle *mh, enum mpg123_channels ch,
                                int band, double val)
    { return mpg123_eq(mh, ch, band, val); }

static void hook_mpg(const char *name, uintptr_t impl) {
    uintptr_t sym = so_symbol(&gtactw_mod, name);
    if (sym) hook_addr(sym, impl);
}

void patch_mpg123(void) {
    hook_mpg("mpg123_add_string",              (uintptr_t)mpg123_add_string);
    hook_mpg("mpg123_add_substring",           (uintptr_t)mpg123_add_substring);
    hook_mpg("mpg123_clip",                    (uintptr_t)mpg123_clip);
    hook_mpg("mpg123_close",                   (uintptr_t)mpg123_close);
    hook_mpg("mpg123_copy_string",             (uintptr_t)mpg123_copy_string);
    hook_mpg("mpg123_current_decoder",         (uintptr_t)mpg123_current_decoder);
    hook_mpg("mpg123_decode",                  (uintptr_t)mpg123_decode);
    hook_mpg("mpg123_decode_frame",            (uintptr_t)mpg123_decode_frame);
    hook_mpg("mpg123_decoder",                 (uintptr_t)mpg123_decoder);
    hook_mpg("mpg123_decoders",                (uintptr_t)mpg123_decoders);
    hook_mpg("mpg123_delete",                  (uintptr_t)mpg123_delete);
    hook_mpg("mpg123_delete_pars",             (uintptr_t)mpg123_delete_pars);
    hook_mpg("mpg123_enc_from_id3",            (uintptr_t)mpg123_enc_from_id3);
    hook_mpg("mpg123_encodings",               (uintptr_t)mpg123_encodings);
    hook_mpg("mpg123_encsize",                 (uintptr_t)mpg123_encsize);
    hook_mpg("mpg123_eq",                      (uintptr_t)mpg123_eq_abi);
    hook_mpg("mpg123_errcode",                 (uintptr_t)mpg123_errcode);
    hook_mpg("mpg123_exit",                    (uintptr_t)mpg123_exit);
    hook_mpg("mpg123_feature",                 (uintptr_t)mpg123_feature);
    hook_mpg("mpg123_feed",                    (uintptr_t)mpg123_feed);
    hook_mpg("mpg123_feedseek",                (uintptr_t)mpg123_feedseek32);
    hook_mpg("mpg123_fmt",                     (uintptr_t)mpg123_fmt);
    hook_mpg("mpg123_fmt_all",                 (uintptr_t)mpg123_fmt_all);
    hook_mpg("mpg123_fmt_none",                (uintptr_t)mpg123_fmt_none);
    hook_mpg("mpg123_fmt_support",             (uintptr_t)mpg123_fmt_support);
    hook_mpg("mpg123_format",                  (uintptr_t)mpg123_format);
    hook_mpg("mpg123_format_all",              (uintptr_t)mpg123_format_all);
    hook_mpg("mpg123_format_none",             (uintptr_t)mpg123_format_none);
    hook_mpg("mpg123_format_support",          (uintptr_t)mpg123_format_support);
    hook_mpg("mpg123_framebyframe_decode",     (uintptr_t)mpg123_framebyframe_decode);
    hook_mpg("mpg123_framebyframe_next",       (uintptr_t)mpg123_framebyframe_next);
    hook_mpg("mpg123_free_string",             (uintptr_t)mpg123_free_string);
    hook_mpg("mpg123_geteq",                   (uintptr_t)mpg123_geteq);
    hook_mpg("mpg123_getformat",               (uintptr_t)mpg123_getformat);
    hook_mpg("mpg123_getpar",                  (uintptr_t)mpg123_getpar);
    hook_mpg("mpg123_getparam",                (uintptr_t)mpg123_getparam);
    hook_mpg("mpg123_getstate",                (uintptr_t)mpg123_getstate);
    hook_mpg("mpg123_getvolume",               (uintptr_t)mpg123_getvolume);
    hook_mpg("mpg123_grow_string",             (uintptr_t)mpg123_grow_string);
    hook_mpg("mpg123_icy",                     (uintptr_t)mpg123_icy);
    hook_mpg("mpg123_icy2utf8",                (uintptr_t)mpg123_icy2utf8);
    hook_mpg("mpg123_id3",                     (uintptr_t)mpg123_id3);
    hook_mpg("mpg123_index",                   (uintptr_t)mpg123_index);
    hook_mpg("mpg123_info",                    (uintptr_t)mpg123_info);
    hook_mpg("mpg123_init",                    (uintptr_t)mpg123_init);
    hook_mpg("mpg123_init_string",             (uintptr_t)mpg123_init_string);
    hook_mpg("mpg123_length",                  (uintptr_t)mpg123_length);
    hook_mpg("mpg123_meta_check",              (uintptr_t)mpg123_meta_check);
    hook_mpg("mpg123_new",                     (uintptr_t)mpg123_new);
    hook_mpg("mpg123_new_pars",                (uintptr_t)mpg123_new_pars);
    hook_mpg("mpg123_open",                    (uintptr_t)mpg123_open);
    hook_mpg("mpg123_open_fd",                 (uintptr_t)mpg123_open_fd);
    hook_mpg("mpg123_open_feed",               (uintptr_t)mpg123_open_feed);
    hook_mpg("mpg123_open_handle",             (uintptr_t)mpg123_open_handle);
    hook_mpg("mpg123_outblock",                (uintptr_t)mpg123_outblock);
    hook_mpg("mpg123_par",                     (uintptr_t)mpg123_par);
    hook_mpg("mpg123_param",                   (uintptr_t)mpg123_param_hook);
    hook_mpg("mpg123_parnew",                  (uintptr_t)mpg123_parnew);
    hook_mpg("mpg123_plain_strerror",          (uintptr_t)mpg123_plain_strerror);
    hook_mpg("mpg123_position",                (uintptr_t)mpg123_position);
    hook_mpg("mpg123_rates",                   (uintptr_t)mpg123_rates);
    hook_mpg("mpg123_read",                    (uintptr_t)mpg123_read);
    hook_mpg("mpg123_replace_buffer",          (uintptr_t)mpg123_replace_buffer);
    hook_mpg("mpg123_replace_reader",          (uintptr_t)mpg123_replace_reader);
    hook_mpg("mpg123_replace_reader_handle",   (uintptr_t)mpg123_replace_reader_handle);
    hook_mpg("mpg123_reset_eq",                (uintptr_t)mpg123_reset_eq);
    hook_mpg("mpg123_resize_string",           (uintptr_t)mpg123_resize_string);
    hook_mpg("mpg123_safe_buffer",             (uintptr_t)mpg123_safe_buffer);
    hook_mpg("mpg123_scan",                    (uintptr_t)mpg123_scan);
    hook_mpg("mpg123_seek",                    (uintptr_t)mpg123_seek32);
    hook_mpg("mpg123_seek_frame",              (uintptr_t)mpg123_seek_frame);
    hook_mpg("mpg123_set_filesize",            (uintptr_t)mpg123_set_filesize32);
    hook_mpg("mpg123_set_index",               (uintptr_t)mpg123_set_index);
    hook_mpg("mpg123_set_string",              (uintptr_t)mpg123_set_string);
    hook_mpg("mpg123_set_substring",           (uintptr_t)mpg123_set_substring);
    hook_mpg("mpg123_store_utf8",              (uintptr_t)mpg123_store_utf8);
    hook_mpg("mpg123_strerror",                (uintptr_t)mpg123_strerror);
    hook_mpg("mpg123_strlen",                  (uintptr_t)mpg123_strlen);
    hook_mpg("mpg123_supported_decoders",      (uintptr_t)mpg123_supported_decoders);
    hook_mpg("mpg123_tell",                    (uintptr_t)mpg123_tell);
    hook_mpg("mpg123_tell_stream",             (uintptr_t)mpg123_tell_stream);
    hook_mpg("mpg123_tellframe",               (uintptr_t)mpg123_tellframe);
    hook_mpg("mpg123_timeframe",               (uintptr_t)mpg123_timeframe);
    hook_mpg("mpg123_tpf",                     (uintptr_t)mpg123_tpf);
    hook_mpg("mpg123_volume",                  (uintptr_t)mpg123_volume_abi);
    hook_mpg("mpg123_volume_change",           (uintptr_t)mpg123_volume_change_abi);
}
