/* openal_patch.c -- Replace the embedded Android AudioTrack OpenAL Soft
 *                   with the system OpenAL Soft (ALSA/PipeWire backend).
 *
 * The game has OpenAL Soft statically compiled in with only the Android
 * AudioTrack backend.  We hook every al / alc symbol to redirect to the
 * system libopenal.so which has proper Linux audio backends.
 *
 * We use dlsym rather than linking directly so we handle version differences
 * automatically: if a symbol exists in both the game .so and system OpenAL
 * it gets hooked; if it's missing from either side it's silently skipped.
 */

#include <dlfcn.h>
#include <stdio.h>

#include "so_util.h"
#include "openal_patch.h"

extern so_module gtactw_mod;

static void *libopenal = NULL;

/* Override alcCreateContext to force 44100 Hz */
typedef void *ALCdevice;
typedef void *ALCcontext;
typedef int   ALCint;
#define ALC_FREQUENCY 0x1007

static ALCcontext *(*real_alcCreateContext)(ALCdevice *, const ALCint *) = NULL;

static ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *requested) {
    /* The game passes no attributes of its own (verified on-device), so this
     * only pins the output rate rather than overriding anything it asked for. */
    (void)requested;
    const ALCint attr[] = { ALC_FREQUENCY, 44100, 0 };
    if (real_alcCreateContext)
        return real_alcCreateContext(dev, attr);
    return NULL;
}

/* ── soft-float ABI bridges ──────────────────────────────────────────────
 *
 * libCTW.so is armeabi-v7a = **soft-float**: float and double arguments (and
 * return values) travel in the general-purpose registers. This binary and the
 * system OpenAL are **hard-float**, using s0-s15 / d0-d7. Every hooked entry
 * point taking or returning a float by value therefore needs a thunk, or the
 * callee reads a register the caller never wrote.
 *
 * It fails intermittently rather than outright, which is what makes it nasty:
 * soft-float code computes in VFP and then copies to the GP register, so the
 * VFP register usually still holds the right value. It cost us pedestrian
 * voices playing at a fraction of speed — the game always set AL_PITCH to
 * 1.0, but the hard-float callee was reading stale VFP contents like 20/127.
 *
 * pcs("aapcs") tells GCC to give these functions the base (soft-float) PCS,
 * which fixes arguments AND return values. Functions taking a const float*
 * (alSourcefv, alListenerfv, alGetSourcef, …) pass a pointer in a GP register
 * and are already correct, so they are deliberately left alone.
 */
#define SOFTFP __attribute__((pcs("aapcs")))

static void   (*real_alSourcef)(unsigned, int, float);
static void   (*real_alSource3f)(unsigned, int, float, float, float);
static void   (*real_alListenerf)(int, float);
static void   (*real_alListener3f)(int, float, float, float);
static void   (*real_alBufferf)(unsigned, int, float);
static void   (*real_alBuffer3f)(unsigned, int, float, float, float);
static void   (*real_alEffectf)(unsigned, int, float);
static void   (*real_alFilterf)(unsigned, int, float);
static void   (*real_alAuxiliaryEffectSlotf)(unsigned, int, float);
static void   (*real_alDopplerFactor)(float);
static void   (*real_alDopplerVelocity)(float);
static void   (*real_alSpeedOfSound)(float);
static void   (*real_alSourcedSOFT)(unsigned, int, double);
static void   (*real_alSource3dSOFT)(unsigned, int, double, double, double);
static float  (*real_alGetFloat)(int);
static double (*real_alGetDouble)(int);

SOFTFP static void al_alSourcef(unsigned s, int p, float v)
    { real_alSourcef(s, p, v); }
SOFTFP static void al_alSource3f(unsigned s, int p, float a, float b, float c)
    { real_alSource3f(s, p, a, b, c); }
SOFTFP static void al_alListenerf(int p, float v)
    { real_alListenerf(p, v); }
SOFTFP static void al_alListener3f(int p, float a, float b, float c)
    { real_alListener3f(p, a, b, c); }
SOFTFP static void al_alBufferf(unsigned b, int p, float v)
    { real_alBufferf(b, p, v); }
SOFTFP static void al_alBuffer3f(unsigned b, int p, float x, float y, float z)
    { real_alBuffer3f(b, p, x, y, z); }
SOFTFP static void al_alEffectf(unsigned e, int p, float v)
    { real_alEffectf(e, p, v); }
SOFTFP static void al_alFilterf(unsigned f, int p, float v)
    { real_alFilterf(f, p, v); }
SOFTFP static void al_alAuxiliaryEffectSlotf(unsigned a, int p, float v)
    { real_alAuxiliaryEffectSlotf(a, p, v); }
SOFTFP static void al_alDopplerFactor(float v)   { real_alDopplerFactor(v); }
SOFTFP static void al_alDopplerVelocity(float v) { real_alDopplerVelocity(v); }
SOFTFP static void al_alSpeedOfSound(float v)    { real_alSpeedOfSound(v); }
SOFTFP static void al_alSourcedSOFT(unsigned s, int p, double v)
    { real_alSourcedSOFT(s, p, v); }
SOFTFP static void al_alSource3dSOFT(unsigned s, int p, double a, double b, double c)
    { real_alSource3dSOFT(s, p, a, b, c); }
SOFTFP static float  al_alGetFloat(int p)  { return real_alGetFloat(p); }
SOFTFP static double al_alGetDouble(int p) { return real_alGetDouble(p); }

/* Hook a symbol: look it up in both the game .so and system OpenAL, patch if found */
static void hook_al(const char *name) {
    uintptr_t game_sym = so_symbol(&gtactw_mod, name);
    if (!game_sym) return;

    void *sys_sym = dlsym(libopenal, name);
    if (!sys_sym) return;

    hook_addr(game_sym, (uintptr_t)sys_sym);
}

/* Full list of AL/ALC symbols the game may export */
static const char *al_symbols[] = {
    "alAuxiliaryEffectSlotf", "alAuxiliaryEffectSlotfv",
    "alAuxiliaryEffectSloti", "alAuxiliaryEffectSlotiv",
    "alBuffer3f", "alBuffer3i", "alBufferData",
    "alBufferSamplesSOFT", "alBufferSubDataSOFT", "alBufferSubSamplesSOFT",
    "alBufferf", "alBufferfv", "alBufferi", "alBufferiv",
    "alDeferUpdatesSOFT",
    "alDeleteAuxiliaryEffectSlots", "alDeleteBuffers",
    "alDeleteEffects", "alDeleteFilters", "alDeleteSources",
    "alDisable", "alDistanceModel", "alDopplerFactor", "alDopplerVelocity",
    "alEffectf", "alEffectfv", "alEffecti", "alEffectiv",
    "alEnable",
    "alFilterf", "alFilterfv", "alFilteri", "alFilteriv",
    "alGenAuxiliaryEffectSlots", "alGenBuffers",
    "alGenEffects", "alGenFilters", "alGenSources",
    "alGetAuxiliaryEffectSlotf", "alGetAuxiliaryEffectSlotfv",
    "alGetAuxiliaryEffectSloti", "alGetAuxiliaryEffectSlotiv",
    "alGetBoolean", "alGetBooleanv",
    "alGetBuffer3f", "alGetBuffer3i",
    "alGetBufferSamplesSOFT",
    "alGetBufferf", "alGetBufferfv", "alGetBufferi", "alGetBufferiv",
    "alGetDouble", "alGetDoublev",
    "alGetEffectf", "alGetEffectfv", "alGetEffecti", "alGetEffectiv",
    "alGetEnumValue", "alGetError",
    "alGetFilterf", "alGetFilterfv", "alGetFilteri", "alGetFilteriv",
    "alGetFloat", "alGetFloatv", "alGetInteger", "alGetIntegerv",
    "alGetListener3f", "alGetListener3i",
    "alGetListenerf", "alGetListenerfv", "alGetListeneri", "alGetListeneriv",
    "alGetProcAddress",
    "alGetSource3dSOFT", "alGetSource3f", "alGetSource3i",
    "alGetSource3i64SOFT", "alGetSourcedSOFT", "alGetSourcedvSOFT",
    "alGetSourcef", "alGetSourcefv", "alGetSourcei",
    "alGetSourcei64SOFT", "alGetSourcei64vSOFT", "alGetSourceiv",
    "alGetString",
    "alIsAuxiliaryEffectSlot", "alIsBuffer",
    "alIsBufferFormatSupportedSOFT",
    "alIsEffect", "alIsEnabled", "alIsExtensionPresent",
    "alIsFilter", "alIsSource",
    "alListener3f", "alListener3i",
    "alListenerf", "alListenerfv", "alListeneri", "alListeneriv",
    "alProcessUpdatesSOFT",
    "alSource3dSOFT", "alSource3f", "alSource3i",
    "alSource3i64SOFT",
    "alSourcePause", "alSourcePausev",
    "alSourcePlay", "alSourcePlayv",
    "alSourceQueueBuffers", "alSourceRewind", "alSourceRewindv",
    "alSourceStop", "alSourceStopv", "alSourceUnqueueBuffers",
    "alSourcedSOFT", "alSourcedvSOFT",
    "alSourcef", "alSourcefv",
    "alSourcei", "alSourcei64SOFT", "alSourcei64vSOFT", "alSourceiv",
    "alSpeedOfSound",
    "alcCaptureCloseDevice", "alcCaptureOpenDevice",
    "alcCaptureSamples", "alcCaptureStart", "alcCaptureStop",
    "alcCloseDevice",
    "alcDestroyContext",
    "alcGetContextsDevice", "alcGetCurrentContext",
    "alcGetEnumValue", "alcGetError",
    "alcGetIntegerv", "alcGetProcAddress", "alcGetString",
    "alcGetThreadContext",
    "alcIsExtensionPresent", "alcIsRenderFormatSupportedSOFT",
    "alcLoopbackOpenDeviceSOFT",
    "alcMakeContextCurrent", "alcOpenDevice",
    "alcProcessContext", "alcRenderSamplesSOFT",
    "alcSetThreadContext", "alcSuspendContext",
    NULL
};

static int ret0_al(void) { return 0; }

void patch_openal(void) {
    libopenal = dlopen("libopenal.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!libopenal) {
        libopenal = dlopen("libopenal.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!libopenal) {
        fprintf(stderr, "patch_openal: could not open libopenal: %s\n", dlerror());
        fprintf(stderr, "Audio will be silent.\n");
        return;
    }

    /* Disable the embedded ALSOFT's Android AudioTrack backend so it doesn't
     * try to spawn a JNI thread.  These are internal ALSOFT functions. */
    uintptr_t sym;
    sym = so_symbol(&gtactw_mod, "alc_audiotrack_probe");
    if (sym) { fprintf(stderr, "patch_openal: stubbing alc_audiotrack_probe\n"); hook_addr(sym, (uintptr_t)ret0_al); }
    sym = so_symbol(&gtactw_mod, "alc_audiotrack_init");
    if (sym) { fprintf(stderr, "patch_openal: stubbing alc_audiotrack_init\n");  hook_addr(sym, (uintptr_t)ret0_al); }
    sym = so_symbol(&gtactw_mod, "alc_audiotrack_deinit");
    if (sym) { fprintf(stderr, "patch_openal: stubbing alc_audiotrack_deinit\n");hook_addr(sym, (uintptr_t)ret0_al); }

    /* Hook alcCreateContext specially to force 44100 Hz */
    real_alcCreateContext = dlsym(libopenal, "alcCreateContext");
    uintptr_t game_alcCreateContext = so_symbol(&gtactw_mod, "alcCreateContext");
    if (game_alcCreateContext && real_alcCreateContext)
        hook_addr(game_alcCreateContext, (uintptr_t)alcCreateContextHook);

    /* Hook all other AL/ALC symbols */
    for (int i = 0; al_symbols[i]; i++)
        hook_al(al_symbols[i]);

    /* Soft-float thunks. These MUST come after the bulk loop above, which
     * points these symbols straight at the hard-float system entry points. */
    #define BRIDGE(name)                                                    \
        do {                                                                \
            real_##name = dlsym(libopenal, #name);                          \
            uintptr_t gs = so_symbol(&gtactw_mod, #name);                   \
            if (gs && real_##name) hook_addr(gs, (uintptr_t)al_##name);     \
        } while (0)

    BRIDGE(alSourcef);        BRIDGE(alSource3f);
    BRIDGE(alListenerf);      BRIDGE(alListener3f);
    BRIDGE(alBufferf);        BRIDGE(alBuffer3f);
    BRIDGE(alEffectf);        BRIDGE(alFilterf);
    BRIDGE(alAuxiliaryEffectSlotf);
    BRIDGE(alDopplerFactor);  BRIDGE(alDopplerVelocity);
    BRIDGE(alSpeedOfSound);
    BRIDGE(alSourcedSOFT);    BRIDGE(alSource3dSOFT);
    BRIDGE(alGetFloat);       BRIDGE(alGetDouble);
    #undef BRIDGE

    fprintf(stderr, "patch_openal: soft-float bridges installed\n");
    fflush(stderr);
}
