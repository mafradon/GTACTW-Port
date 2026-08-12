/* main.c -- GTA: Chinatown Wars loader for R36S (RK3326 / ArkOS) */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fenv.h>
#include <setjmp.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <zlib.h>
#include <linux/input.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include "config.h"
#include "so_util.h"
#include "jni_patch.h"
#include "openal_patch.h"
#include "opengl_patch.h"
#include "mpg123_patch.h"

/* ── Globals shared with other modules ──────────────────────────────────── */

so_module           gtactw_mod;
SDL_Window         *g_window  = NULL;
SDL_GLContext       g_gl_ctx  = NULL;
SDL_GameController *g_gamepad = NULL;
int                 g_gamepad_buttons = 0;
float               g_gamepad_axis[6] = { 0 };
char                g_data_path[512]  = DATA_PATH;

/* ── C++ ABI symbols ─────────────────────────────────────────────────────── */
extern int  __cxa_atexit(void (*)(void *), void *, void *) __attribute__((weak));
extern void __cxa_finalize(void *)                         __attribute__((weak));

/* __cxa_guard: replace bionic libc++ implementation with a simple GCC-compatible one.
 * Guard object layout: byte 0 = initialized flag.
 * Returns 1 (proceed) if not yet initialized, 0 if done. */
static int  __cxa_guard_acquire_impl(long *g) { return (*(char*)g == 0); }
static void __cxa_guard_release_impl(long *g) { *(char*)g = 1; }
static void __cxa_guard_abort_impl  (long *g) { (void)g; }

/* ── glibc gettid wrapper (kernel syscall) ───────────────────────────────── */
static pid_t _gettid(void) { return (pid_t)syscall(SYS_gettid); }

/* clock_gettime, __clock_gettime64, and clock_gettime64_safe are defined in
 * clock_fix.c (separate TU that avoids <time.h>'s __asm__ alias which would
 * cause duplicate symbols when both are defined in the same translation unit) */
extern int clock_gettime(clockid_t, struct timespec *);
extern int __clock_gettime64(clockid_t, void *);
extern int clock_gettime64_safe(clockid_t, void *);
extern int gettimeofday64_safe(void *, void *);
extern int gettimeofday_safe(void *, void *);

/* ── isfinite / signbit: glibc provides these as macros, expose functions ── */
static int _isfinite(double d) { return isfinite(d); }
static int _signbit(double d)  { return signbit(d); }

/* ── Stub helpers ────────────────────────────────────────────────────────── */

static int  ret0(void)  { return 0; }
static int  ret1(void)  { return 1; }

static volatile int g_malloc_count = 0;
static void *malloc_debug(size_t n) {
    return malloc(n);
}

/* ── ARM EABI memory helpers: argument order differs from glibc ──────────── */
/* __aeabi_memset(dst, n, c) — note: n and c are SWAPPED vs memset(dst,c,n) */
static void __aeabi_memset_impl(void *dst, size_t n, int c)  { memset(dst, c, n); }
/* __aeabi_memclr(dst, n) — zero n bytes */
static void __aeabi_memclr_impl(void *dst, size_t n)         { memset(dst, 0, n); }

/* ── Android log → stderr ────────────────────────────────────────────────── */

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
#else
    (void)prio; (void)tag; (void)fmt;
#endif
    return 0;
}

/* ── Screen size (hooked into .so) ───────────────────────────────────────── */

int OS_ScreenGetWidth(void)  { return SCREEN_W; }
int OS_ScreenGetHeight(void) { return SCREEN_H; }

/* ── Bionic pthread/semaphore ABI shims ──────────────────────────────────────
 * On Android bionic (32-bit): mutex=4 bytes, cond=4 bytes, sem=4 bytes.
 * On glibc (32-bit):          mutex=24 bytes, cond=48 bytes, sem=16 bytes.
 * pthread_attr_t: bionic=24 bytes, glibc=36 bytes.
 *
 * Strategy for mutex/cond/sem: store a heap pointer to a real glibc object in
 * the game's 4-byte bionic slot.  Lazy-init handles zero-initialised globals.
 * Strategy for attr: use our own 24-byte layout, ignore glibc's larger struct.
 */

static int pthread_mutex_init_fake(pthread_mutex_t **m,
                                    const pthread_mutexattr_t *a) {
    pthread_mutex_t *real = calloc(1, sizeof(pthread_mutex_t));
    pthread_mutex_init(real, a);
    *m = real;
    return 0;
}
static int pthread_mutex_destroy_fake(pthread_mutex_t **m) {
    if (*m) { pthread_mutex_destroy(*m); free(*m); *m = NULL; }
    return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **m) {
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_mutex_lock(*m);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **m) {
    if (!*m) return 0;
    return pthread_mutex_unlock(*m);
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **m) {
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_mutex_trylock(*m);
}

static int pthread_cond_init_fake(pthread_cond_t **c,
                                   const pthread_condattr_t *a) {
    pthread_cond_t *real = calloc(1, sizeof(pthread_cond_t));
    pthread_cond_init(real, a);
    *c = real;
    return 0;
}
static int pthread_cond_destroy_fake(pthread_cond_t **c) {
    if (*c) { pthread_cond_destroy(*c); free(*c); *c = NULL; }
    return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **c, pthread_mutex_t **m) {
    if (!*c) pthread_cond_init_fake(c, NULL);
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_cond_wait(*c, *m);
}
static int pthread_cond_timedwait_fake(pthread_cond_t **c, pthread_mutex_t **m,
                                        const struct timespec *t) {
    if (!*c) pthread_cond_init_fake(c, NULL);
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_cond_timedwait(*c, *m, t);
}
static int pthread_cond_signal_fake(pthread_cond_t **c) {
    if (*c) return pthread_cond_signal(*c);
    return 0;
}
static int pthread_cond_broadcast_fake(pthread_cond_t **c) {
    if (*c) return pthread_cond_broadcast(*c);
    return 0;
}

static int sem_init_fake(sem_t **s, int pshared, unsigned int value) {
    sem_t *real = calloc(1, sizeof(sem_t));
    sem_init(real, pshared, value);
    *s = real;
    return 0;
}
static int sem_destroy_fake(sem_t **s) {
    if (*s) { sem_destroy(*s); free(*s); *s = NULL; }
    return 0;
}
static int sem_wait_fake(sem_t **s) {
    if (!*s) sem_init_fake(s, 0, 0);
    return sem_wait(*s);
}
static int sem_post_fake(sem_t **s) {
    if (!*s) sem_init_fake(s, 0, 0);
    return sem_post(*s);
}
static int sem_trywait_fake(sem_t **s) {
    if (!*s) return EAGAIN;
    return sem_trywait(*s);
}
static int sem_getvalue_fake(sem_t **s, int *val) {
    if (!*s) { if (val) *val = 0; return 0; }
    return sem_getvalue(*s, val);
}

/* pthread_attr_t: bionic layout (24 bytes) — store only what we need */
typedef struct {
    uint32_t flags;
    void    *stack_base;
    size_t   stack_size;
    size_t   guard_size;
    int32_t  sched_policy;
    int32_t  sched_priority;
} bionic_attr_t;

static int pthread_attr_init_fake(bionic_attr_t *a) {
    memset(a, 0, sizeof(*a));
    a->guard_size = 4096;
    return 0;
}
static int pthread_attr_destroy_fake(bionic_attr_t *a)          { (void)a; return 0; }
static int pthread_attr_setstacksize_fake(bionic_attr_t *a, size_t s) {
    a->stack_size = s; return 0;
}
static int pthread_attr_getstacksize_fake(bionic_attr_t *a, size_t *s) {
    *s = a->stack_size; return 0;
}
static int pthread_attr_getstack_fake(bionic_attr_t *a, void **base, size_t *s) {
    *base = a->stack_base; *s = a->stack_size; return 0;
}
static int pthread_attr_setschedparam_fake(bionic_attr_t *a,
                                            const struct sched_param *p) {
    a->sched_priority = p->sched_priority; return 0;
}
static int pthread_attr_getschedparam_fake(bionic_attr_t *a, struct sched_param *p) {
    p->sched_priority = a->sched_priority; return 0;
}

/* ── Real pthread_create (RTLD_NEXT avoids recursion when we define the symbol) */
static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                   void *(*)(void *), void *) = NULL;
static void init_real_pthread_create(void) {
    if (!real_pthread_create)
        real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
}

/* Global pthread_create — intercepts thread creation from ALL loaded shared libs.
 * The executable's strong definition preempts libpthread's for every .so loaded
 * with RTLD_GLOBAL (which includes system libopenal.so). Lets us catch any
 * library that tries to spawn a thread with a NULL start_routine. */
int pthread_create(pthread_t *tidp, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    init_real_pthread_create();
    /* Use volatile to prevent -O2 from eliminating the NULL check based on
     * the nonnull prototype attribute. */
    void *(*volatile sr)(void *) = start_routine;
    pthread_t *volatile tp = tidp;
    if (!sr) {
        if (tp) *tp = 0;
        return 0;
    }
    return real_pthread_create(tidp, attr, sr, arg);
}

/* ── Bionic TSD stubs ────────────────────────────────────────────────────────
 * The PSVita port stubs all TSD operations as no-ops and that works fine.
 * Our real bionic→glibc key mapping caused pthread_kill to be called with
 * a corrupted TID when keys or their values got out of sync with glibc
 * internals.  Match the PSVita approach: key_create/delete/get/set are all
 * ret0 — NVThreadGetCurrentJNIEnv() is already hooked directly so the game's
 * JNI env lookup never needs TSD. */

/* Global pthread_cancel interceptor — the game does not import pthread_cancel,
 * but SDL2/OpenAL may call it to stop their internal threads.  Log it so we
 * can identify which thread is being cancelled and what its pthread_t is. */
int pthread_cancel(pthread_t thread) {
    static int (*real_pc)(pthread_t) = NULL;
    if (!real_pc)
        real_pc = dlsym(RTLD_NEXT, "pthread_cancel");
    return real_pc(thread);
}

/* pthread_create_fake: called from libCTW.so's GOT (bionic ABI, pointer-redirect
 * attr).  Uses a trampoline to log thread start/stop and handles NULL guards. */
typedef struct { void *(*func)(void *); void *arg; } pt_tramp_t;

static void *pthread_tramp(void *p) {
    pt_tramp_t *t = p;
    void *(*f)(void *) = t->func;
    void *a = t->arg;
    free(t);
    return f(a);
}

static int pthread_create_fake(pthread_t *tidp, bionic_attr_t *attr,
                                void *func, void *arg) {
    (void)attr;
    if (!func) {
        if (tidp) *tidp = 0;
        return 0;
    }
    init_real_pthread_create();
    pt_tramp_t *t = malloc(sizeof(*t));
    t->func = (void *(*)(void *))func;
    t->arg  = arg;
    pthread_t tid;
    int r = real_pthread_create(&tid, NULL, pthread_tramp, t);
    if (tidp) *tidp = tid;
    return r;
}

/* ── OS_ThreadLaunch / OS_ThreadWait (real pthreads for worker threads) ── */

typedef struct { int (*func)(void *); void *arg; } thread_args;

static void *thread_trampoline(void *p) {
    thread_args *ta = p;
    ta->func(ta->arg);
    free(ta);
    return NULL;
}

void *OS_ThreadLaunch(int (*func)(void *), void *arg, int cpu,
                      const char *name, int unused, int priority) {
    (void)cpu; (void)unused; (void)priority;
    pthread_t *tid = malloc(sizeof(pthread_t));
    thread_args *ta = malloc(sizeof(thread_args));
    ta->func = func;
    ta->arg  = arg;
    init_real_pthread_create();
    real_pthread_create(tid, NULL, thread_trampoline, ta);
    pthread_setname_np(*tid, name ? name : "worker");
    return tid;
}

void OS_ThreadWait(void *thread) {
    if (!thread) return;
    pthread_join(*(pthread_t *)thread, NULL);
    free(thread);
}

/* ── stat hook: game checks mtime at statbuf+0x50 (Android struct layout) ── */

static int stat_hook(const char *path, void *statbuf) {
    struct stat st;
    int r = stat(path, &st);
    if (r == 0)
        *(int *)((char *)statbuf + 0x50) = (int)st.st_mtime;
    return r;
}

/* ── ctype / stdio ABI compatibility ─────────────────────────────────────── */

/* Android libc exposes these as pointers; glibc does too but at different
 * symbol names.  We provide matching data so the game finds valid tables. */

static const short C_tolower_tab[257] = {
    -1,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
    0x40,'a','b','c','d','e','f','g',
    'h','i','j','k','l','m','n','o',
    'p','q','r','s','t','u','v','w',
    'x','y','z',0x5b,0x5c,0x5d,0x5e,0x5f,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
    0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,
    0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
    0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
    0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,
    0xd0,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,
    0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,
    0xe0,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
    0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,
    0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
    0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
};
static const short *tolower_tab_ptr = C_tolower_tab;

/* Android __sF is an array of embedded bionic FILE structs (~84 bytes each).
 * Allocate enough space so __sF[1] (stdout) lands inside our buffer.
 * resolve_stream() maps bionic-fake addresses back to real glibc streams. */
#define BIONIC_FILE_SIZE 84
static char  sF_fake[3 * BIONIC_FILE_SIZE];
static FILE *stderr_fake;
static int   stack_chk_guard_fake = 0x42424242;

/* ctype_ pointer: provided via android_ctype_table below */

/* ── Touchscreen evdev reader ───────────────────────────────────────────────
 * Reads multitouch type-B (MT slot) events from /dev/input/event1
 * (Hynitron cst3xx Touchscreen) and dispatches to the game via AND_TouchEvent.
 * action: 0=down, 1=move, 2=up
 *
 * TOUCH_MAX_X/Y: native reporting range of the touchscreen.  Defaults to
 * SCREEN_W/H (640×480); adjust here if the touchscreen reports different coords.
 */
#define MAX_TOUCH_SLOTS 5
#define TOUCH_MAX_X     SCREEN_W
#define TOUCH_MAX_Y     SCREEN_H

static int g_touch_fd = -1;

typedef struct {
    int tracking_id;   /* -1 = slot empty */
    int x, y;
    int prev_active;
    int dirty;
} touch_slot_t;

static touch_slot_t g_slots[MAX_TOUCH_SLOTS];
static int          g_cur_slot = 0;

static void init_touchscreen(void) {
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        g_slots[i].tracking_id = -1;
        g_slots[i].prev_active  = 0;
        g_slots[i].dirty        = 0;
    }
    g_touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (g_touch_fd < 0)
        fprintf(stderr, "touchscreen: open /dev/input/event1: %s\n", strerror(errno));
    else
        fprintf(stderr, "touchscreen: opened fd=%d\n", g_touch_fd);
}

static void process_touch_events(void) {
    if (g_touch_fd < 0) return;
    struct input_event ev;
    while (read(g_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_SLOT:
                if ((unsigned)ev.value < MAX_TOUCH_SLOTS)
                    g_cur_slot = ev.value;
                break;
            case ABS_MT_TRACKING_ID:
                if (g_cur_slot < MAX_TOUCH_SLOTS)
                    g_slots[g_cur_slot].tracking_id = ev.value;
                break;
            case ABS_MT_POSITION_X:
                if (g_cur_slot < MAX_TOUCH_SLOTS) {
                    g_slots[g_cur_slot].x = ev.value * SCREEN_W / TOUCH_MAX_X;
                    g_slots[g_cur_slot].dirty = 1;
                }
                break;
            case ABS_MT_POSITION_Y:
                if (g_cur_slot < MAX_TOUCH_SLOTS) {
                    g_slots[g_cur_slot].y = ev.value * SCREEN_H / TOUCH_MAX_Y;
                    g_slots[g_cur_slot].dirty = 1;
                }
                break;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
                int active = (g_slots[i].tracking_id != -1);
                if (active && !g_slots[i].prev_active) {
                    send_touch_event(0, i, g_slots[i].x, g_slots[i].y); /* ACTION_DOWN */
                } else if (!active && g_slots[i].prev_active) {
                    send_touch_event(1, i, g_slots[i].x, g_slots[i].y); /* ACTION_UP */
                } else if (active && g_slots[i].dirty) {
                    send_touch_event(2, i, g_slots[i].x, g_slots[i].y); /* ACTION_MOVE */
                }
                g_slots[i].prev_active = active;
                g_slots[i].dirty       = 0;
            }
        }
    }
}

/* ── ProcessEvents: called once per frame by the game ────────────────────── */

int ProcessEvents(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            return 1; /* signal exit */
    }

    /* Update gamepad state */
    if (g_gamepad) {
        int mask = 0;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_A))         mask |= 0x001;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_B))         mask |= 0x002;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_X))         mask |= 0x004;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_Y))         mask |= 0x008;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_START))     mask |= 0x010;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_BACK))      mask |= 0x020;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  mask |= 0x040;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= 0x080;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP))   mask |= 0x100;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) mask |= 0x200;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mask |= 0x400;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))mask |= 0x800;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_LEFTSTICK)) mask |= 0x1000;
        if (SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSTICK))mask |= 0x2000;

        /* Start+Select held together → clean exit */
        if ((mask & 0x010) && (mask & 0x020)) {
            fprintf(stderr, "Start+Select: exiting\n");
            fflush(stderr);
            SDL_Quit();
            exit(0);
        }

        g_gamepad_buttons = mask;

        /* Axes: normalise SDL's -32768..32767 to -1..1 */
        g_gamepad_axis[0] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX)  / 32767.0f;
        g_gamepad_axis[1] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY)  / 32767.0f;
        g_gamepad_axis[2] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        g_gamepad_axis[3] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        /* Triggers: 0..32767 → 0..1 */
        g_gamepad_axis[4] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 32767.0f;
        g_gamepad_axis[5] = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    }

    process_touch_events();

    return 0; /* 1 = exit */
}

/* ── __assert2 (Android assertion handler) ───────────────────────────────── */
static void __assert2_impl(const char *file, int line,
                            const char *func, const char *expr) {
    fprintf(stderr, "ASSERT FAIL: %s:%d %s(): %s\n", file, line, func, expr);
    fflush(stderr);
    /* Don't call real abort() — that raises SIGABRT through glibc internals
     * bypassing our raise_hook. Just hang so we can see the log. */
    for (;;) usleep(1000000);
}

static void abort_hook(void) {
    write(2, "ABORT_HOOK\n", 11);  /* confirm hook fires */
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "abort() intercepted from game (caller=%p):\n",
            __builtin_return_address(0));
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  bt[%d] %s\n", i, syms ? syms[i] : "?");
    free(syms);
    fflush(stderr);
    /* Swallow — loop so execution doesn't continue off the end */
    for (;;) usleep(1000000);
}

/* Intercept raise() from libCTW.so: log and swallow.
 * bionic's abort() calls raise(SIGABRT); some internal crash paths call raise(SIGSEGV).
 * Swallowing lets us observe what happens next instead of dying immediately. */
static int raise_hook(int sig) {
    write(2, "RAISE_HOOK\n", 11);  /* async-signal-safe confirm */
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "raise(%d) intercepted from game (caller=%p):\n",
            sig, __builtin_return_address(0));
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  bt[%d] %s\n", i, syms ? syms[i] : "?");
    free(syms);
    fflush(stderr);
    return 0;  /* swallow — do not deliver signal */
}

/* ── __gnu_Unwind_Find_exidx (ARM EHABI — no C++ exceptions needed) ──────── */
static void *__gnu_Unwind_Find_exidx_stub(void *pc, int *pcount) {
    (void)pc;
    if (pcount) *pcount = 0;
    return NULL;
}

/* ── Bionic-compatible _ctype_ table ─────────────────────────────────────── */
/* Bionic bit flags: _U=0x01 _L=0x02 _D=0x04 _C=0x08 _P=0x10 _S=0x20 _X=0x40 _B=0x80 */
static const char android_ctype_table[257] = {
    0,                                                /* [0]   EOF */
    0x08,                                             /* [1]   0x00 NUL   ctrl */
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,         /* [2-9] 0x01-0x08  ctrl */
    0x28,0x28,0x28,0x28,0x28,                         /* [10-14] 0x09-0x0D HT/LF/VT/FF/CR ctrl+space */
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,         /* [15-22] 0x0E-0x15 ctrl */
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08, /* [23-32] 0x16-0x1F ctrl */
    0xa0,                                             /* [33]  0x20 SP    space+blank */
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10, /* ! " # ... / */
    0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44, /* 0-9 digit+hex */
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,              /* : ; < = > ? @ */
    0x41,0x41,0x41,0x41,0x41,0x41,                   /* A-F upper+hex */
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, /* G-Z upper */
    0x10,0x10,0x10,0x10,0x10,0x10,                   /* [ \ ] ^ _ ` */
    0x42,0x42,0x42,0x42,0x42,0x42,                   /* a-f lower+hex */
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, /* g-z lower */
    0x10,0x10,0x10,0x10,0x08,                        /* { | } ~ DEL */
    /* 0x80-0xFF: non-ASCII */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
/* _ctype_ is a char* pointing to android_ctype_table[1] so that ptr[c] works for c=0..255 */
static const char *ctype_ptr_val = android_ctype_table + 1;

/* ── ImmVibe stubs (haptics — stub as no-ops) ────────────────────────────── */

static int ImmVibeInitialize2(void *p)                               { (void)p; return 0; }
static int ImmVibeOpenDevice(int d, int *h)                          { (void)d; if(h)*h=0; return 0; }
static int ImmVibeCloseDevice(int h)                                 { (void)h; return 0; }
static int ImmVibeTerminate(void)                                     { return 0; }
static int ImmVibePlayUHLEffect(int h, int e, int i, int *p)         { (void)h;(void)e;(void)i;(void)p; return 0; }
static int ImmVibeStopPlayingEffect(int h, int e)                    { (void)h;(void)e; return 0; }
static int ImmVibeGetEffectState(int h, int e, int *s)               { (void)h;(void)e; if(s)*s=0; return 0; }
static int ImmVibeGetIVTEffectIndexFromName(void *d, void *n, int *i){ (void)d;(void)n;(void)i; return 0; }

/* resolve_stream: bionic __sF[n] lands inside sF_fake[] — map back to glibc streams. */
static FILE *resolve_stream(FILE *s) {
    ptrdiff_t off = (char *)s - sF_fake;
    if (off >= 0 && off < (ptrdiff_t)sizeof(sF_fake)) {
        int idx = (int)(off / BIONIC_FILE_SIZE);
        if (idx == 0) return stdin;
        if (idx == 1) return stdout;
        if (idx == 2) return stderr;
    }
    return s;
}

static int    fclose_fake(FILE *s)                               { return fclose(resolve_stream(s)); }
static int    feof_fake(FILE *s)                                 { return feof(resolve_stream(s)); }
static int    fflush_fake(FILE *s)                               { return fflush(resolve_stream(s)); }
static int    fgetc_fake(FILE *s)                                { return fgetc(resolve_stream(s)); }
static char  *fgets_fake(char *b, int n, FILE *s)                { return fgets(b, n, resolve_stream(s)); }
static int    fprintf_fake(FILE *s, const char *fmt, ...)        { va_list ap; va_start(ap, fmt); int r = vfprintf(resolve_stream(s), fmt, ap); va_end(ap); return r; }
static int    fputc_fake(int c, FILE *s)                         { return fputc(c, resolve_stream(s)); }
static int    fputs_fake(const char *b, FILE *s)                 { return fputs(b, resolve_stream(s)); }
static wint_t fputwc_fake(wchar_t c, FILE *s)                   { return fputwc(c, resolve_stream(s)); }
static size_t fread_fake(void *p, size_t sz, size_t n, FILE *s)  { return fread(p, sz, n, resolve_stream(s)); }
static int    fseek_fake(FILE *s, long o, int w)                 { return fseek(resolve_stream(s), o, w); }
static long   ftell_fake(FILE *s)                                { return ftell(resolve_stream(s)); }

/* fwrite_safe: glibc's _IO_fwrite uses NEON/ldm which faults on misaligned src.
 * Copy misaligned source buffers to the heap before passing to fwrite. */
static size_t fwrite_safe(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    stream = resolve_stream(stream);
    /* ptr in loader binary range is always wrong — game heap is at 0xec000000+ */
    if ((uintptr_t)ptr < 0x10000000) {
        fprintf(stderr, "fwrite_safe: suspicious ptr=%p size=%zu nmemb=%zu stream=%p LR=%p\n",
                ptr, size, nmemb, (void *)stream,
                __builtin_return_address(0));
        fflush(stderr);
        return 0;
    }
    if ((uintptr_t)ptr & 7) {
        size_t total = size * nmemb;
        void *buf = malloc(total);
        if (buf) {
            memcpy(buf, ptr, total);
            size_t ret = fwrite(buf, size, nmemb, stream);
            free(buf);
            return ret;
        }
        return 0;  /* malloc failed, can't safely write unaligned buf */
    }
    return fwrite(ptr, size, nmemb, stream);
}

static FILE *fopen_fake(const char *path, const char *mode);
static int   pthread_kill_fake(pthread_t thread, int sig);
static int   open_fake(const char *path, int flags, ...);

/* ── Softfp ABI thunks for math functions ────────────────────────────────── *
 * libCTW.so (Android armeabi-v7a) uses soft-float calling convention:        *
 * scalars in integer registers r0-r3.  System libm uses hard-float (VFP).    *
 * These thunks (pcs("aapcs")) receive args from int registers and return      *
 * results in int registers, bridging to/from the hard-float system functions. */
static SOFTFP float  acosf_abi(float x)                  { return acosf(x);       }
static SOFTFP float  asinf_abi(float x)                  { return asinf(x);       }
static SOFTFP double atan_abi(double x)                   { return atan(x);        }
static SOFTFP float  atan2f_abi(float y, float x)         { return atan2f(y, x);   }
static SOFTFP float  atanf_abi(float x)                   { return atanf(x);       }
static SOFTFP double atof_abi(const char *s)              { return atof(s);        }
static SOFTFP double cos_abi(double x)                    { return cos(x);         }
static SOFTFP float  cosf_abi(float x)                    { return cosf(x);        }
static SOFTFP double exp_abi(double x)                    { return exp(x);         }
static SOFTFP double exp2_abi(double x)                   { return exp2(x);        }
static SOFTFP float  expf_abi(float x)                    { return expf(x);        }
static SOFTFP double floor_abi(double x)                  { return floor(x);       }
static SOFTFP float  floorf_abi(float x)                  { return floorf(x);      }
static SOFTFP float  log10f_abi(float x)                  { return log10f(x);      }
static SOFTFP float  logf_abi(float x)                    { return logf(x);        }
static SOFTFP double pow_abi(double x, double y)          { return pow(x, y);      }
static SOFTFP float  powf_abi(float x, float y)           { return powf(x, y);     }
static SOFTFP double sin_abi(double x)                    { return sin(x);         }
static SOFTFP float  sinf_abi(float x)                    { return sinf(x);        }
static SOFTFP double tan_abi(double x)                    { return tan(x);         }
static SOFTFP float  strtof_abi(const char *s, char **e)  { return strtof(s, e);   }

/* ── Symbol table ────────────────────────────────────────────────────────── */

static so_default_dynlib default_dynlib[] = {
    /* ── AEABI helpers ──────────────────────────────────────────────────── */
    /* memclr(dst,n) and memset(dst,n,c) have different arg order than glibc */
    { "__aeabi_memclr",   (uintptr_t)__aeabi_memclr_impl },
    { "__aeabi_memclr4",  (uintptr_t)__aeabi_memclr_impl },
    { "__aeabi_memclr8",  (uintptr_t)__aeabi_memclr_impl },
    { "__aeabi_memcpy",   (uintptr_t)memcpy              },
    { "__aeabi_memcpy4",  (uintptr_t)memcpy              },
    { "__aeabi_memcpy8",  (uintptr_t)memcpy              },
    { "__aeabi_memmove",  (uintptr_t)memmove             },
    { "__aeabi_memmove4", (uintptr_t)memmove             },
    { "__aeabi_memmove8", (uintptr_t)memmove             },
    { "__aeabi_memset",   (uintptr_t)__aeabi_memset_impl },
    { "__aeabi_memset4",  (uintptr_t)__aeabi_memset_impl },
    { "__aeabi_memset8",  (uintptr_t)__aeabi_memset_impl },

    /* ── Android-specific (android/log, asset mgr, android/native) ──────── */
    { "__android_log_print", (uintptr_t)__android_log_print },
    { "AAssetManager_fromJava", (uintptr_t)ret0 },
    { "AAssetManager_open",     (uintptr_t)ret0 },
    { "AAsset_close",           (uintptr_t)ret0 },
    { "AAsset_getLength",       (uintptr_t)ret0 },
    { "AAsset_getRemainingLength", (uintptr_t)ret0 },
    { "AAsset_read",            (uintptr_t)ret0 },
    { "AAsset_seek",            (uintptr_t)ret0 },

    /* ── Haptics (libImmEmulatorJ) ────────────────────────────────────── */
    { "ImmVibeInitialize2",              (uintptr_t)ImmVibeInitialize2              },
    { "ImmVibeOpenDevice",               (uintptr_t)ImmVibeOpenDevice               },
    { "ImmVibeCloseDevice",              (uintptr_t)ImmVibeCloseDevice              },
    { "ImmVibeTerminate",                (uintptr_t)ImmVibeTerminate                },
    { "ImmVibePlayUHLEffect",            (uintptr_t)ImmVibePlayUHLEffect            },
    { "ImmVibeStopPlayingEffect",        (uintptr_t)ImmVibeStopPlayingEffect        },
    { "ImmVibeGetEffectState",           (uintptr_t)ImmVibeGetEffectState           },
    { "ImmVibeGetIVTEffectIndexFromName",(uintptr_t)ImmVibeGetIVTEffectIndexFromName},

    /* ── Standard C / POSIX (forward to glibc) ───────────────────────── */
    { "abort",        (uintptr_t)abort_hook   },
    { "acosf",        (uintptr_t)acosf_abi     },
    { "asinf",        (uintptr_t)asinf_abi    },
    { "atan",         (uintptr_t)atan_abi     },
    { "atan2f",       (uintptr_t)atan2f_abi   },
    { "atanf",        (uintptr_t)atanf_abi    },
    { "atof",         (uintptr_t)atof_abi     },
    { "atoi",         (uintptr_t)atoi         },
    { "calloc",       (uintptr_t)calloc       },
    { "clock_gettime",(uintptr_t)clock_gettime },  /* our safe syscall version */
    { "close",        (uintptr_t)close        },
    { "closedir",     (uintptr_t)closedir     },
    { "cos",          (uintptr_t)cos_abi       },
    { "cosf",         (uintptr_t)cosf_abi     },
    { "dladdr",       (uintptr_t)dladdr       },
    { "exp",          (uintptr_t)exp_abi       },
    { "exp2",         (uintptr_t)exp2_abi     },
    { "expf",         (uintptr_t)expf_abi     },
    { "fclose",       (uintptr_t)fclose_fake   },
    { "feof",         (uintptr_t)feof_fake     },
    { "fegetround",   (uintptr_t)fegetround    },
    { "fesetround",   (uintptr_t)fesetround    },
    { "fflush",       (uintptr_t)fflush_fake   },
    { "fgetc",        (uintptr_t)fgetc_fake    },
    { "fgets",        (uintptr_t)fgets_fake    },
    { "floor",        (uintptr_t)floor_abi      },
    { "floorf",       (uintptr_t)floorf_abi    },
    { "fopen",        (uintptr_t)fopen_fake    },
    { "fprintf",      (uintptr_t)fprintf_fake  },
    { "fputc",        (uintptr_t)fputc_fake    },
    { "fputs",        (uintptr_t)fputs_fake    },
    { "fputwc",       (uintptr_t)fputwc_fake   },
    { "fread",        (uintptr_t)fread_fake    },
    { "free",         (uintptr_t)free          },
    { "fseek",        (uintptr_t)fseek_fake    },
    { "ftell",        (uintptr_t)ftell_fake    },
    { "fwrite",       (uintptr_t)fwrite_safe   },
    { "getenv",       (uintptr_t)getenv       },
    { "gettid",       (uintptr_t)_gettid      },
    { "gettimeofday", (uintptr_t)gettimeofday_safe },
    { "gmtime",       (uintptr_t)gmtime       },
    { "gzclose",      (uintptr_t)gzclose      },
    { "gzgets",       (uintptr_t)gzgets       },
    { "gzopen",       (uintptr_t)gzopen       },
    { "isspace",      (uintptr_t)isspace      },
    { "localtime",    (uintptr_t)localtime    },
    { "localtime_r",  (uintptr_t)localtime_r  },
    { "log",          (uintptr_t)log          },
    { "log10f",       (uintptr_t)log10f_abi    },
    { "logf",         (uintptr_t)logf_abi     },
    { "longjmp",      (uintptr_t)longjmp      },
    { "lseek",        (uintptr_t)lseek        },
    { "malloc",       (uintptr_t)malloc_debug  },
    { "memchr",       (uintptr_t)memchr       },
    { "memcmp",       (uintptr_t)memcmp       },
    { "mkdir",        (uintptr_t)mkdir        },
    { "nanosleep",    (uintptr_t)nanosleep    },
    { "open",         (uintptr_t)open_fake    },
    { "opendir",      (uintptr_t)opendir      },
    { "pow",          (uintptr_t)pow_abi       },
    { "powf",         (uintptr_t)powf_abi     },
    { "prctl",        (uintptr_t)ret0         },
    { "pthread_attr_destroy",      (uintptr_t)pthread_attr_destroy_fake         },
    { "pthread_attr_getschedparam",(uintptr_t)pthread_attr_getschedparam_fake   },
    { "pthread_attr_getstack",     (uintptr_t)pthread_attr_getstack_fake        },
    { "pthread_attr_init",         (uintptr_t)pthread_attr_init_fake            },
    { "pthread_attr_setschedparam",(uintptr_t)pthread_attr_setschedparam_fake   },
    { "pthread_attr_setstacksize", (uintptr_t)pthread_attr_setstacksize_fake    },
    { "pthread_cond_broadcast",  (uintptr_t)pthread_cond_broadcast_fake  },
    { "pthread_cond_destroy",    (uintptr_t)pthread_cond_destroy_fake    },
    { "pthread_cond_init",       (uintptr_t)pthread_cond_init_fake       },
    { "pthread_cond_signal",     (uintptr_t)pthread_cond_signal_fake     },
    { "pthread_cond_timedwait",  (uintptr_t)pthread_cond_timedwait_fake  },
    { "pthread_cond_wait",       (uintptr_t)pthread_cond_wait_fake       },
    { "pthread_create",          (uintptr_t)pthread_create_fake          },
    { "pthread_getspecific",     (uintptr_t)ret0                         },
    { "pthread_join",            (uintptr_t)pthread_join                 },
    { "pthread_kill",            (uintptr_t)pthread_kill_fake            },
    { "pthread_key_create",      (uintptr_t)ret0                         },
    { "pthread_key_delete",      (uintptr_t)ret0                         },
    { "pthread_mutexattr_destroy",(uintptr_t)ret0                        },
    { "pthread_mutexattr_init",  (uintptr_t)ret0                         },
    { "pthread_mutexattr_settype",(uintptr_t)ret0                        },
    { "pthread_mutex_destroy",   (uintptr_t)pthread_mutex_destroy_fake   },
    { "pthread_mutex_init",      (uintptr_t)pthread_mutex_init_fake      },
    { "pthread_mutex_lock",      (uintptr_t)pthread_mutex_lock_fake      },
    { "pthread_mutex_unlock",    (uintptr_t)pthread_mutex_unlock_fake    },
    { "pthread_once",            (uintptr_t)pthread_once                 },
    { "pthread_self",            (uintptr_t)pthread_self                 },
    { "pthread_setname_np",      (uintptr_t)pthread_setname_np           },
    { "pthread_setschedparam",   (uintptr_t)pthread_setschedparam        },
    { "pthread_setspecific",     (uintptr_t)ret0                         },
    { "putchar",      (uintptr_t)putchar      },
    { "puts",         (uintptr_t)puts         },
    { "qsort",        (uintptr_t)qsort        },
    { "raise",        (uintptr_t)raise_hook   },
    { "rand",         (uintptr_t)rand         },
    { "read",         (uintptr_t)read         },
    { "readdir",      (uintptr_t)readdir      },
    { "realloc",      (uintptr_t)realloc      },
    { "sched_get_priority_max", (uintptr_t)sched_get_priority_max },
    { "sched_get_priority_min", (uintptr_t)sched_get_priority_min },
    { "sched_yield",  (uintptr_t)sched_yield  },
    { "sem_destroy",  (uintptr_t)sem_destroy_fake  },
    { "sem_getvalue", (uintptr_t)sem_getvalue_fake },
    { "sem_init",     (uintptr_t)sem_init_fake     },
    { "sem_post",     (uintptr_t)sem_post_fake     },
    { "sem_trywait",  (uintptr_t)sem_trywait_fake  },
    { "sem_wait",     (uintptr_t)sem_wait_fake     },
    { "setjmp",       (uintptr_t)setjmp       },
    { "sigaction",    (uintptr_t)ret0         },
    { "sigemptyset",  (uintptr_t)ret0         },
    { "sin",          (uintptr_t)sin_abi       },
    { "sinf",         (uintptr_t)sinf_abi     },
    { "srand",        (uintptr_t)srand        },
    { "stat",         (uintptr_t)stat_hook    },
    { "strcasecmp",   (uintptr_t)strcasecmp   },
    { "strcat",       (uintptr_t)strcat       },
    { "strchr",       (uintptr_t)strchr       },
    { "strcmp",       (uintptr_t)strcmp       },
    { "strcpy",       (uintptr_t)strcpy       },
    { "strerror",     (uintptr_t)strerror     },
    { "strlen",       (uintptr_t)strlen       },
    { "strncasecmp",  (uintptr_t)strncasecmp  },
    { "strncmp",      (uintptr_t)strncmp      },
    { "strncpy",      (uintptr_t)strncpy      },
    { "strpbrk",      (uintptr_t)strpbrk      },
    { "strstr",       (uintptr_t)strstr       },
    { "strtof",       (uintptr_t)strtof_abi   },
    { "strtol",       (uintptr_t)strtol       },
    { "strtoul",      (uintptr_t)strtoul      },
    { "syscall",      (uintptr_t)syscall      },
    { "sysconf",      (uintptr_t)sysconf      },
    { "tan",          (uintptr_t)tan_abi       },
    { "time",         (uintptr_t)time         },
    { "toupper",      (uintptr_t)toupper      },
    { "usleep",       (uintptr_t)usleep       },
    { "vasprintf",    (uintptr_t)vasprintf    },

    /* ── EGL: stub all three so the game uses its static GOT imports, which
       go through our softfp→hardfp thunks. Real eglGetProcAddress would
       return hard-float pointers the game calls with soft-float convention. ── */
    { "eglGetDisplay",    (uintptr_t)ret0 },
    { "eglGetProcAddress",(uintptr_t)ret0 },
    { "eglQueryString",   (uintptr_t)ret0 },

    /* ── OpenGL ES 2: all symbols resolved so GOT is never left at PLT garbage ── */
    { "glActiveTexture",              (uintptr_t)glActiveTexture               },
    { "glBufferData",                 (uintptr_t)glBufferData                  },
    { "glBufferSubData",              (uintptr_t)glBufferSubData               },
    { "glDeleteBuffers",              (uintptr_t)glDeleteBuffers               },
    { "glGenBuffers",                 (uintptr_t)glGenBuffers                  },
    { "glGetFloatv",                  (uintptr_t)glGetFloatv                   },
    { "glGetIntegerv",                (uintptr_t)glGetIntegerv                 },
    { "glStencilFunc",                (uintptr_t)glStencilFunc                 },
    { "glStencilMask",                (uintptr_t)glStencilMask                 },
    { "glStencilOp",                  (uintptr_t)glStencilOp                   },
    { "glTexSubImage2D",              (uintptr_t)glTexSubImage2DHook           },
    { "glBlendEquationSeparate",      (uintptr_t)glBlendEquationSeparate       },
    { "glBlendFuncSeparate",          (uintptr_t)glBlendFuncSeparate           },
    { "glPixelStorei",                (uintptr_t)glPixelStorei                 },
    { "glReadPixels",                 (uintptr_t)glReadPixels                  },
    { "glIsEnabled",                  (uintptr_t)glIsEnabled                   },
    { "glFrontFace",                  (uintptr_t)glFrontFace                   },
    { "glPolygonOffset",              (uintptr_t)glPolygonOffset_abi           },
    { "glSampleCoverage",             (uintptr_t)glSampleCoverage_abi          },
    { "glUniform1f",                  (uintptr_t)glUniform1f_abi               },
    { "glUniform1fv",                 (uintptr_t)glUniform1fv                  },
    { "glUniform1iv",                 (uintptr_t)glUniform1iv                  },
    { "glUniform2f",                  (uintptr_t)glUniform2f_abi               },
    { "glUniform2fv",                 (uintptr_t)glUniform2fv                  },
    { "glUniform2i",                  (uintptr_t)glUniform2i                   },
    { "glUniform2iv",                 (uintptr_t)glUniform2iv                  },
    { "glUniform3f",                  (uintptr_t)glUniform3f_abi               },
    { "glUniform3iv",                 (uintptr_t)glUniform3iv                  },
    { "glUniform4fv",                 (uintptr_t)glUniform4fvHook              },
    { "glUniform4i",                  (uintptr_t)glUniform4i                   },
    { "glUniform4iv",                 (uintptr_t)glUniform4iv                  },
    { "glUniformMatrix2fv",           (uintptr_t)glUniformMatrix2fv            },
    { "glVertexAttrib1f",             (uintptr_t)glVertexAttrib1f_abi          },
    { "glVertexAttrib4fv",            (uintptr_t)glVertexAttrib4fv             },
    { "glIsBuffer",                   (uintptr_t)glIsBuffer                    },
    { "glIsProgram",                  (uintptr_t)glIsProgram                   },
    { "glIsShader",                   (uintptr_t)glIsShader                    },
    { "glIsTexture",                  (uintptr_t)glIsTexture                   },
    { "glClearDepthf",                (uintptr_t)glClearDepthf_abi             },
    { "glClearStencil",               (uintptr_t)glClearStencil                },
    { "glCopyTexImage2D",             (uintptr_t)glCopyTexImage2D              },
    { "glCopyTexSubImage2D",          (uintptr_t)glCopyTexSubImage2D           },
    { "glDepthRangef",                (uintptr_t)glDepthRangef_abi             },
    { "glDetachShader",               (uintptr_t)glDetachShader                },
    { "glFinish",                     (uintptr_t)glFinish                      },
    { "glFlush",                      (uintptr_t)glFlush                       },
    { "glGenerateMipmap",             (uintptr_t)glGenerateMipmap              },
    { "glGetActiveAttrib",            (uintptr_t)glGetActiveAttrib             },
    { "glGetActiveUniform",           (uintptr_t)glGetActiveUniform            },
    { "glGetAttachedShaders",         (uintptr_t)glGetAttachedShaders          },
    { "glGetBooleanv",                (uintptr_t)glGetBooleanv                 },
    { "glGetBufferParameteriv",       (uintptr_t)glGetBufferParameteriv        },
    { "glGetFramebufferAttachmentParameteriv", (uintptr_t)glGetFramebufferAttachmentParameteriv },
    { "glGetRenderbufferParameteriv", (uintptr_t)glGetRenderbufferParameteriv  },
    { "glGetTexParameterfv",          (uintptr_t)glGetTexParameterfv           },
    { "glGetTexParameteriv",          (uintptr_t)glGetTexParameteriv           },
    { "glGetUniformfv",               (uintptr_t)glGetUniformfv                },
    { "glGetUniformiv",               (uintptr_t)glGetUniformiv                },
    { "glGetVertexAttribfv",          (uintptr_t)glGetVertexAttribfv           },
    { "glGetVertexAttribiv",          (uintptr_t)glGetVertexAttribiv           },
    { "glGetVertexAttribPointerv",    (uintptr_t)glGetVertexAttribPointerv     },
    { "glHint",                       (uintptr_t)glHint                        },
    { "glLineWidth",                  (uintptr_t)glLineWidth_abi               },
    { "glReleaseShaderCompiler",      (uintptr_t)glReleaseShaderCompiler       },
    { "glShaderBinary",               (uintptr_t)glShaderBinary                },
    { "glTexParameterfv",             (uintptr_t)glTexParameterfv              },
    { "glTexParameteriv",             (uintptr_t)glTexParameteriv              },
    { "glValidateProgram",            (uintptr_t)glValidateProgram             },
    { "glDrawElements",               (uintptr_t)glDrawElementsHook           },
    { "glAttachShader",               (uintptr_t)glAttachShader               },
    { "glBindAttribLocation",         (uintptr_t)glBindAttribLocationHook     },
    { "glBindBuffer",                 (uintptr_t)glBindBuffer                 },
    { "glBindFramebuffer",            (uintptr_t)glBindFramebufferHook        },
    { "glBindRenderbuffer",           (uintptr_t)glBindRenderbuffer           },
    { "glBindTexture",                (uintptr_t)glBindTexture                },
    { "glBlendEquation",              (uintptr_t)glBlendEquation              },
    { "glBlendFunc",                  (uintptr_t)glBlendFuncHook              },
    { "glCheckFramebufferStatus",     (uintptr_t)glCheckFramebufferStatus     },
    { "glClear",                      (uintptr_t)glClear                      },
    { "glClearColor",                 (uintptr_t)glClearColorHook             },
    { "glColorMask",                  (uintptr_t)glColorMask                  },
    { "glCompileShader",              (uintptr_t)glCompileShaderHook          },
    { "glCompressedTexImage2D",       (uintptr_t)glCompressedTexImage2DHook   },
    { "glCreateProgram",              (uintptr_t)glCreateProgram              },
    { "glCreateShader",               (uintptr_t)glCreateShader               },
    { "glCullFace",                   (uintptr_t)glCullFace                   },
    { "glDeleteFramebuffers",         (uintptr_t)glDeleteFramebuffers         },
    { "glDeleteProgram",              (uintptr_t)glDeleteProgram              },
    { "glDeleteRenderbuffers",        (uintptr_t)glDeleteRenderbuffers        },
    { "glDeleteShader",               (uintptr_t)glDeleteShader               },
    { "glDeleteTextures",             (uintptr_t)glDeleteTextures             },
    { "glDepthFunc",                  (uintptr_t)glDepthFunc                  },
    { "glDepthMask",                  (uintptr_t)glDepthMaskHook              },
    { "glDisable",                    (uintptr_t)glDisableHook                },
    { "glDisableVertexAttribArray",   (uintptr_t)glDisableVertexAttribArray   },
    { "glDrawArrays",                 (uintptr_t)glDrawArraysHook             },
    { "glEnable",                     (uintptr_t)glEnableHook                 },
    { "glEnableVertexAttribArray",    (uintptr_t)glEnableVertexAttribArray    },
    { "glFramebufferRenderbuffer",    (uintptr_t)glFramebufferRenderbuffer    },
    { "glFramebufferTexture2D",       (uintptr_t)glFramebufferTexture2DHook   },
    { "glGenFramebuffers",            (uintptr_t)glGenFramebuffers            },
    { "glGenRenderbuffers",           (uintptr_t)glGenRenderbuffers           },
    { "glGenTextures",                (uintptr_t)glGenTextures                },
    { "glGetAttribLocation",          (uintptr_t)glGetAttribLocation          },
    { "glGetError",                   (uintptr_t)glGetError                   },
    { "glGetProgramInfoLog",          (uintptr_t)glGetProgramInfoLog          },
    { "glGetProgramiv",               (uintptr_t)glGetProgramiv               },
    { "glGetShaderInfoLog",           (uintptr_t)glGetShaderInfoLog           },
    { "glGetShaderiv",                (uintptr_t)glGetShaderiv                },
    { "glGetString",                  (uintptr_t)glGetString                  },
    { "glGetUniformLocation",         (uintptr_t)glGetUniformLocation         },
    { "glLinkProgram",                (uintptr_t)glLinkProgramHook            },
    { "glRenderbufferStorage",        (uintptr_t)glRenderbufferStorage        },
    { "glScissor",                    (uintptr_t)glScissor                    },
    { "glShaderSource",               (uintptr_t)glShaderSourceHook           },
    { "glTexImage2D",                 (uintptr_t)glTexImage2DHook             },
    { "glTexParameterf",              (uintptr_t)glTexParameterf_abi          },
    { "glTexParameteri",              (uintptr_t)glTexParameteri              },
    { "glUniform1i",                  (uintptr_t)glUniform1i                  },
    { "glUniform3fv",                 (uintptr_t)glUniform3fvHook             },
    { "glUniform4f",                  (uintptr_t)glUniform4fHook              },
    { "glUniformMatrix3fv",           (uintptr_t)glUniformMatrix3fv           },
    { "glUniformMatrix4fv",           (uintptr_t)glUniformMatrix4fvHook       },
    { "glUseProgram",                 (uintptr_t)glUseProgramHook             },
    { "glVertexAttrib2f",             (uintptr_t)glVertexAttrib2f_abi         },
    { "glVertexAttrib3f",             (uintptr_t)glVertexAttrib3f_abi         },
    { "glVertexAttrib4f",             (uintptr_t)glVertexAttrib4f_abi         },
    { "glVertexAttribPointer",        (uintptr_t)glVertexAttribPointerHook    },
    { "glViewport",                   (uintptr_t)glViewport                   },

    /* ── Misc ABI ─────────────────────────────────────────────────────── */
    { "__assert2",            (uintptr_t)__assert2_impl                    },
    { "__cxa_atexit",         (uintptr_t)__cxa_atexit                      },
    { "__cxa_finalize",       (uintptr_t)__cxa_finalize                    },
    { "__gnu_Unwind_Find_exidx", (uintptr_t)__gnu_Unwind_Find_exidx_stub   },
    { "__stack_chk_fail",     (uintptr_t)abort                             },
    { "__stack_chk_guard",    (uintptr_t)&stack_chk_guard_fake             },
    { "__errno",              (uintptr_t)__errno_location                  },
    { "__sF",                 (uintptr_t)sF_fake                           },
    { "stderr",               (uintptr_t)&stderr_fake                      },
    { "_ctype_",              (uintptr_t)&ctype_ptr_val                    },
    { "_tolower_tab_",        (uintptr_t)&tolower_tab_ptr                  },
    { "__isfinite",           (uintptr_t)_isfinite                         },
    { "__signbit",            (uintptr_t)_signbit                          },
    { "__fpclassifyd",        (uintptr_t)__fpclassify                      },
    { "pthread_attr_getschedparam",  (uintptr_t)pthread_attr_getschedparam_fake },
    { "pthread_attr_getstacksize",   (uintptr_t)pthread_attr_getstacksize_fake  },
    { "pthread_attr_setschedparam",  (uintptr_t)pthread_attr_setschedparam_fake },
    { "sched_get_priority_min",      (uintptr_t)sched_get_priority_min          },
};

/* ── NVThreadSpawnProc replacement ──────────────────────────────────────────
 * NVThreadSpawnInfo layout (bionic side, all 4-byte words unless noted):
 *   +0  thread_arg  – first arg to pass to the actual thread function
 *   +4  thread_func – the real thread start routine
 *   +8  attach_env  – byte: 1 = need NV JNI context (we skip); 0 = plain call
 * We skip the NV JNI context setup (which needs a live C++ NVThread object)
 * and just call thread_func(thread_arg) directly, matching the cbz path.
 */
static void *nvthread_spawn_proc_hook(void *arg) {
    void  *thread_arg  = *(void **)  ((char *)arg + 0);
    void *(*thread_func)(void *) =
        (void *(*)(void *))(uintptr_t)(*(uint32_t *)((char *)arg + 4));
    free(arg);
    return thread_func(thread_arg);
}

/* ── pthread_kill interceptor ───────────────────────────────────────────────
 * The game stores bionic pthread_t values (small integers or bionic struct
 * pointers) and passes them to pthread_kill.  Glibc's pthread_kill
 * dereferences the handle as a struct pthread* — if the handle is not a real
 * glibc struct, this crashes or sends a signal to the wrong thread.
 * Two layers of interception:
 *   1. pthread_kill_fake: in the dynlib table for libCTW.so's GOT (if imported)
 *   2. Global pthread_kill: symbol interposition catches calls from SDL2/OpenAL */
static int pthread_kill_fake(pthread_t thread, int sig) {
    (void)thread; (void)sig;
    return 0;
}

/* Global interposition: catches pthread_kill from ALL shared libs. */
int pthread_kill(pthread_t thread, int sig) {
    static int (*real_pk)(pthread_t, int) = NULL;
    if (!real_pk) real_pk = dlsym(RTLD_NEXT, "pthread_kill");
    if (sig == SIGSEGV || sig == SIGABRT || sig == SIGILL || sig == SIGBUS)
        return 0;
    return real_pk(thread, sig);
}

/* ── fopen/open wrappers ─────────────────────────────────────────────────── */
static FILE *fopen_fake(const char *path, const char *mode) {
    return fopen(path, mode);
}

static int open_fake(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return open(path, flags, mode);
}

/* ── patch_game ──────────────────────────────────────────────────────────── */

static void patch_game(void) {
    /* JNI env accessor used by NvEventQueue threading */
    hook_addr(so_symbol(&gtactw_mod, "_Z24NVThreadGetCurrentJNIEnvv"),
              (uintptr_t)NVThreadGetCurrentJNIEnv);

    /* Worker thread management */
    hook_addr(so_symbol(&gtactw_mod,
        "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"),
        (uintptr_t)OS_ThreadLaunch);
    hook_addr(so_symbol(&gtactw_mod, "_Z13OS_ThreadWaitPv"),
              (uintptr_t)OS_ThreadWait);

    /* Screen dimensions */
    hook_addr(so_symbol(&gtactw_mod, "_Z17OS_ScreenGetWidthv"),
              (uintptr_t)OS_ScreenGetWidth);
    hook_addr(so_symbol(&gtactw_mod, "_Z18OS_ScreenGetHeightv"),
              (uintptr_t)OS_ScreenGetHeight);

    /* Skip Android device-chip enumeration */
    hook_addr(so_symbol(&gtactw_mod, "_Z20AND_SystemInitializev"),
              (uintptr_t)ret0);

    /* Replace bionic libc++ guard functions with GCC-compatible stubs */
    hook_addr(so_symbol(&gtactw_mod, "__cxa_guard_acquire"), (uintptr_t)__cxa_guard_acquire_impl);
    hook_addr(so_symbol(&gtactw_mod, "__cxa_guard_release"), (uintptr_t)__cxa_guard_release_impl);
    hook_addr(so_symbol(&gtactw_mod, "__cxa_guard_abort"),   (uintptr_t)__cxa_guard_abort_impl);

    /* Event loop (our SDL2 version) */
    hook_addr(so_symbol(&gtactw_mod, "_Z13ProcessEventsb"),
              (uintptr_t)ProcessEvents);

    /* _ZL17NVThreadSpawnProcPv: static NVEvent thread launcher.
     * It calls pthread_getspecific to get a C++ NV thread context, but we
     * never call pthread_setspecific, so the lookup returns NULL → crash.
     * Hook it to call the actual thread function directly (skipping JNI env). */
    /* The Thumb bit (+1) goes to hook_addr as the patch target address */
    hook_addr(gtactw_mod.load_bias + 0x78efb9,
              (uintptr_t)nvthread_spawn_proc_hook);

}

/* resolve addr via dladdr and write "  TAG: lib + 0xOFFSET  (sym+delta)\n".
 * Also recognises addresses inside libCTW.so (loaded via our own loader, so
 * dladdr can't see them) and reports their offset from load_bias — that's
 * the offset you can look up with `objdump -d libCTW.so`. */
static void write_addr(const char *tag, unsigned addr) {
    char buf[512];
    int n;
    /* Check libCTW.so range first — our loader's mmap isn't visible to dladdr */
    uintptr_t lb  = gtactw_mod.load_bias;
    uintptr_t end = gtactw_mod.text_base + gtactw_mod.text_size;
    if (lb && addr >= lb && (uintptr_t)addr < end) {
        n = snprintf(buf, sizeof(buf), "  %s: %08x libCTW.so + 0x%x\n",
                     tag, addr, (unsigned)((uintptr_t)addr - lb));
        write(2, buf, n);
        return;
    }
    Dl_info info;
    if (dladdr((void *)(uintptr_t)addr, &info))
        n = snprintf(buf, sizeof(buf), "  %s: %s + 0x%tx  (sym %s+%td)\n",
            tag,
            info.dli_fname,
            (char *)(uintptr_t)addr - (char *)info.dli_fbase,
            info.dli_sname ? info.dli_sname : "?",
            info.dli_sname ? (char *)(uintptr_t)addr - (char *)info.dli_saddr : 0);
    else
        n = snprintf(buf, sizeof(buf), "  %s: %08x <not in any DSO>\n", tag, addr);
    write(2, buf, n);
}

/* ── SIGSEGV handler: print fault PC, LR, all regs, annotated stack ─────── */
static void segv_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *ctx = uc;
    unsigned r0  = ctx->uc_mcontext.arm_r0;
    unsigned r1  = ctx->uc_mcontext.arm_r1;
    unsigned r2  = ctx->uc_mcontext.arm_r2;
    unsigned r3  = ctx->uc_mcontext.arm_r3;
    unsigned r4  = ctx->uc_mcontext.arm_r4;
    unsigned r5  = ctx->uc_mcontext.arm_r5;
    unsigned r6  = ctx->uc_mcontext.arm_r6;
    unsigned r7  = ctx->uc_mcontext.arm_r7;
    unsigned r8  = ctx->uc_mcontext.arm_r8;
    unsigned r9  = ctx->uc_mcontext.arm_r9;
    unsigned r10 = ctx->uc_mcontext.arm_r10;
    unsigned fp  = ctx->uc_mcontext.arm_fp;   /* r11 */
    unsigned ip  = ctx->uc_mcontext.arm_ip;   /* r12 */
    unsigned pc  = ctx->uc_mcontext.arm_pc;
    unsigned lr  = ctx->uc_mcontext.arm_lr;
    unsigned sp  = ctx->uc_mcontext.arm_sp;
    unsigned fa  = (unsigned)(uintptr_t)si->si_addr;
    char buf[512];
    int n;

    n = snprintf(buf, sizeof(buf),
        "\n=== CRASH sig=%d si_code=%d thread=%08x ===\n"
        "  PC=%08x LR=%08x SP=%08x fault=%08x\n"
        "  r0=%08x r1=%08x r2=%08x r3=%08x\n"
        "  r4=%08x r5=%08x r6=%08x r7=%08x\n"
        "  r8=%08x r9=%08x r10=%08x fp=%08x ip=%08x\n",
        sig, si->si_code, (unsigned)(uintptr_t)pthread_self(),
        pc, lr, sp, fa,
        r0, r1, r2, r3,
        r4, r5, r6, r7,
        r8, r9, r10, fp, ip);
    write(2, buf, n);

    write_addr("PC", pc);
    write_addr("LR", lr);

    /* Annotated stack: 32 words; check libCTW.so range and dladdr */
    write(2, "  Stack (SP+0 .. SP+31):\n", 25);
    unsigned *spp = (unsigned *)(uintptr_t)sp;
    uintptr_t lb  = gtactw_mod.load_bias;
    uintptr_t end = gtactw_mod.text_base + gtactw_mod.text_size;
    for (int i = 0; i < 32; i++) {
        unsigned word = spp[i];
        Dl_info info;
        if (lb && word >= lb && (uintptr_t)word < end) {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x  libCTW.so+0x%x\n",
                         i, word, (unsigned)((uintptr_t)word - lb));
        } else if (dladdr((void *)(uintptr_t)word, &info) && info.dli_fname) {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x  %s+0x%tx\n",
                i, word,
                info.dli_sname ? info.dli_sname : info.dli_fname,
                info.dli_sname
                    ? (char *)(uintptr_t)word - (char *)info.dli_saddr
                    : (char *)(uintptr_t)word - (char *)info.dli_fbase);
        } else {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x\n", i, word);
        }
        write(2, buf, n);
    }

    /* /proc/self/maps for base addresses */
    write(2, "  /proc/self/maps:\n", 19);
    int mfd = open("/proc/self/maps", O_RDONLY);
    if (mfd >= 0) {
        char mbuf[4096];
        ssize_t rd;
        while ((rd = read(mfd, mbuf, sizeof(mbuf))) > 0)
            write(2, mbuf, rd);
        close(mfd);
    }
    write(2, "\n", 1);

    signal(sig, SIG_DFL);
    raise(sig);
}

/* ── Conditional libc time-function patching ───────────────────────────────
 * History: one device (RK3566/dArkOS ARM32) shipped a glibc whose
 * __clock_gettime64 / __gettimeofday64 dispatched through a NULL vDSO pointer
 * and jumped to PC=0 (SIGSEGV).  The workaround overwrote the function's
 * prologue with a trampoline into our own direct-syscall implementation.
 *
 * That workaround was UNCONDITIONAL, and on mainstream glibc (AmberELEC
 * RG351MP glibc 2.38, ROCKNIX glibc 2.41 — GitHub issues #1, #3) the native
 * function works perfectly.  Overwriting a healthy prologue then crashes with
 * SIGILL / ILL_ILLOPC at fn+0 (the trampoline bytes get decoded in the wrong
 * instruction set: our Thumb2 bytes over an ARM-mode libc function).
 *
 * Fix: probe the native function once, under a temporary SIGILL/SIGSEGV/SIGBUS
 * guard, BEFORE any threads are spawned.  Patch only if the native call
 * actually faults.  Healthy devices keep their untouched libc; the one broken
 * device still gets the trampoline.  The trampoline encoding is also made
 * ISA-aware (ARM vs Thumb2) as defence-in-depth for the fault branch. */

static sigjmp_buf            probe_jmp;
static volatile sig_atomic_t probe_faulted;

static void probe_fault_handler(int sig) {
    (void)sig;
    probe_faulted = 1;
    siglongjmp(probe_jmp, 1);
}

/* Call fn(a0, a1) under a fault guard.  Returns 1 if it faulted, else 0.
 * MUST run single-threaded (installs process-wide signal handlers). */
static int libc_time_fn_faults(void *fn, long a0, long a1) {
    struct sigaction sa, old_ill, old_segv, old_bus;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = probe_fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL,  &sa, &old_ill);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS,  &sa, &old_bus);

    probe_faulted = 0;
    if (sigsetjmp(probe_jmp, 1) == 0) {
        int (*f)(long, long) = (int (*)(long, long))fn;
        f(a0, a1);
    }

    sigaction(SIGILL,  &old_ill,  NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS,  &old_bus,  NULL);
    return (int)probe_faulted;
}

/* Overwrite the prologue at `sym` with a literal-load branch to `target_fn`.
 * ISA of the branch matches the ISA of `sym` (Thumb bit in the symbol value);
 * `target_fn`'s own Thumb bit is preserved so interworking is correct.       */
static void install_time_trampoline(void *sym, void *target_fn, const char *name) {
    uint8_t  *fn     = (uint8_t *)((uintptr_t)sym & ~1u);   /* strip Thumb bit */
    int       thumb  = (uintptr_t)sym & 1u;                 /* target ISA of sym */
    uintptr_t target = (uintptr_t)target_fn;               /* keep its Thumb bit */

    uint8_t trampoline[8];
    if (thumb) {
        /* Thumb2:  ldr.w pc, [pc, #0]   (T3 literal load into PC) */
        trampoline[0] = 0xDF; trampoline[1] = 0xF8;
        trampoline[2] = 0x00; trampoline[3] = 0xF0;
    } else {
        /* ARM:     ldr pc, [pc, #-4]    (e51ff004) — loads the word that
         * immediately follows; LDR-into-PC interworks on ARMv5T+, so a
         * Thumb target address (bit0=1) switches state correctly. */
        trampoline[0] = 0x04; trampoline[1] = 0xF0;
        trampoline[2] = 0x1F; trampoline[3] = 0xE5;
    }
    trampoline[4] = (uint8_t)(target);
    trampoline[5] = (uint8_t)(target >> 8);
    trampoline[6] = (uint8_t)(target >> 16);
    trampoline[7] = (uint8_t)(target >> 24);

    uintptr_t pgsz = 4096;
    uintptr_t page = (uintptr_t)fn & ~(pgsz - 1u);
    if (mprotect((void *)page, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        fprintf(stderr, "%s: mprotect failed: %s\n", name, strerror(errno));
        return;
    }
    memcpy(fn, trampoline, 8);
    __builtin___clear_cache((char *)fn, (char *)fn + 8);
    mprotect((void *)page, pgsz, PROT_READ | PROT_EXEC);
    fprintf(stderr, "%s: %s-mode trampoline @ %p -> %p\n",
            name, thumb ? "Thumb" : "ARM", (void *)fn, target_fn);
}

/* Look up `symname` in libc; probe it; patch only if the native call faults. */
static void patch_libc_time_fn(const char *symname, void *target_fn,
                               long a0, long a1, const char *name) {
    void *libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (!libc) {
        fprintf(stderr, "%s: libc.so.6 not found via dlopen\n", name);
        return;
    }
    void *sym = dlsym(libc, symname);
    dlclose(libc);
    if (!sym) {
        fprintf(stderr, "%s: %s not in libc\n", name, symname);
        return;
    }

    if (!libc_time_fn_faults(sym, a0, a1)) {
        fprintf(stderr, "%s: native %s works, leaving libc untouched\n",
                name, symname);
        return;
    }
    fprintf(stderr, "%s: native %s FAULTS, installing trampoline\n",
            name, symname);
    install_time_trampoline(sym, target_fn, name);
}

/* Probe scratch buffers: sized for the largest 64-bit-time struct glibc uses. */
static long probe_buf[4];

static void patch_libc_clock64(void) {
    patch_libc_time_fn("__clock_gettime64", (void *)clock_gettime64_safe,
                       (long)CLOCK_MONOTONIC, (long)probe_buf,
                       "patch_libc_clock64");
}

static void patch_libc_gettimeofday64(void) {
    patch_libc_time_fn("__gettimeofday64", (void *)gettimeofday64_safe,
                       (long)probe_buf, 0, "patch_libc_gettimeofday64");
}

static void patch_libc_gettimeofday(void) {
    patch_libc_time_fn("gettimeofday", (void *)gettimeofday_safe,
                       (long)probe_buf, 0, "patch_libc_gettimeofday");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    patch_libc_clock64();
    patch_libc_gettimeofday64();
    patch_libc_gettimeofday();

    /* Allow runtime data path override via GTACTW_DIR env var */
    const char *env_dir = getenv("GTACTW_DIR");
    if (env_dir)
        snprintf(g_data_path, sizeof(g_data_path), "%s", env_dir);

    /* sF_fake is zero-filled BSS; resolve_stream() maps bionic __sF offsets to real streams.
     * stderr_fake is accessed as &stderr_fake by the game's extern FILE* stderr binding. */
    stderr_fake = stderr;

    /* ── SDL2 init ──────────────────────────────────────────────────── */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    g_window = SDL_CreateWindow(
        "GTA: Chinatown Wars",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_W, SCREEN_H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP
    );
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(g_window, g_gl_ctx);
    SDL_GL_SetSwapInterval(1);

    /* Install signal handlers AFTER SDL_Init so we override SDL2's handlers.
     * SDL2 installs its own SIGSEGV handler during SDL_Init which re-raises
     * (via tgkill, giving si_code=-6) and obscures the real fault PC.
     * Installing here gives us the real hardware-fault PC/LR/stack.
     * Also catch SIGABRT (bionic abort path uses SIGABRT, not SIGSEGV).    */
    {
        struct sigaction sa = { .sa_sigaction = segv_handler,
                                .sa_flags = SA_SIGINFO | SA_RESETHAND };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGILL,  &sa, NULL);
        sigaction(SIGBUS,  &sa, NULL);
    }

    /* ── Gamepad ────────────────────────────────────────────────────── */
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_gamepad = SDL_GameControllerOpen(i);
            if (g_gamepad) {
                fprintf(stderr, "Gamepad: %s\n", SDL_GameControllerName(g_gamepad));
                break;
            }
        }
    }

    /* ── Load libCTW.so ─────────────────────────────────────────────── */
    char so_path[560];
    snprintf(so_path, sizeof(so_path), "%s/libCTW.so", g_data_path);
    fprintf(stderr, "so_load: %s\n", so_path);
    if (so_load(&gtactw_mod, so_path) < 0) {
        fprintf(stderr, "Failed to load %s\n", so_path);
        return 1;
    }
    fprintf(stderr, "so_load OK\n");

    so_relocate(&gtactw_mod);
    fprintf(stderr, "so_relocate OK\n");

    so_resolve(&gtactw_mod, default_dynlib,
               sizeof(default_dynlib), 0);
    fprintf(stderr, "so_resolve OK\n");

    /* Write ALSOFT config before dlopen so the mixing thread never tries RTKit.
     * RTKit is unavailable on this device; without this, ALSOFT logs an error
     * and the subsequent thread-priority path causes a NULL-PC crash. */
    {
        const char *conf_path = "/tmp/gtactw-alsoft.conf";
        FILE *cf = fopen(conf_path, "w");
        if (cf) {
            fprintf(cf,
                "[general]\n"
                "rt-prio = 0\n"
                "drivers = alsa\n"
                "\n"
                "[alsa]\n"
                "device = default\n"
                "capture = \n"
            );
            fclose(cf);
            setenv("ALSOFT_CONF", conf_path, 1);
            fprintf(stderr, "ALSOFT_CONF written: %s\n", conf_path);
        }
    }

    patch_openal();
    fprintf(stderr, "patch_openal OK\n");
    patch_mpg123();
    fprintf(stderr, "patch_mpg123 OK\n");
    patch_opengl();
    fprintf(stderr, "patch_opengl OK\n");
    patch_game();
    fprintf(stderr, "patch_game OK\n");

    so_flush_caches(&gtactw_mod);
    fprintf(stderr, "so_initialize...\n");
    so_initialize(&gtactw_mod);
    fprintf(stderr, "so_initialize OK\n");

    /* ── Touchscreen init ───────────────────────────────────────────── */
    init_touchscreen();

    /* ── JNI bootstrap and game loop ────────────────────────────────── */
    fprintf(stderr, "jni_init...\n");
    jni_init();
    jni_resolve_touch();
    fprintf(stderr, "jni_load...\n");
    jni_load(); /* never returns — runs the game loop */

    /* Unreachable */
    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
