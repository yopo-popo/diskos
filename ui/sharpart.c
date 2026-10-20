/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "art.h"
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The sharp cover: the current track's art decoded at the full 360 px, unrotated, for Now Playing, which turns it with
 * sharp_rotate and sharp_spin_start. (This was the vinyl screensaver's; the screensaver is gone.) UI thread owns
 * its lifetime.
 *
 * diskOS rotates the cover itself into an opaque copy instead of asking LVGL to transform the image every frame.
 * LVGL's generic transform cost ~19 ms/frame with anti-aliasing OFF (jagged edges) and ~52 ms with it on; this
 * fixed-point bilinear loop over the round screen only measured ~6 ms/frame on the Disc with smooth edges, so the
 * spin costs a fraction of the CPU (heat/battery) and looks better. */
#define VIN_W 360
static uint8_t *g_vbuf = NULL;       /* 360x360 BGRA (== LVGL ARGB8888 byte order) */
static int g_vbuf_valid = 0;         /* does g_vbuf hold a good decode of the CURRENT track's cover? A failed/absent
                                        decode must NOT show the previous track's buffer, so users gate on this. */
static lv_image_dsc_t g_vdsc;

static inline uint32_t vin_px(const uint32_t *src, int x, int y){
    return ((unsigned)x < VIN_W && (unsigned)y < VIN_W) ? src[y * VIN_W + x] : 0;   /* outside: transparent */
}
static inline uint32_t vin_lerp(uint32_t a, uint32_t b, uint32_t w){   /* per-channel a..b, w in 0..256 */
    uint32_t rb = (((a & 0x00FF00FFu) * (256 - w) + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * (256 - w) + ((b >> 8) & 0x00FF00FFu) * w)) & 0xFF00FF00u;
    return rb | ag;
}
/* Turns a 360x360 BGRA picture about its centre by tenths of a degree into out, inside the round screen only
 * (the rest of out is left alone). The result is opaque: a square picture turned about its centre always covers the
 * round screen, and an opaque image is a plain copy for LVGL instead of a per-pixel alpha blend (~4-5 ms a frame). */
__attribute__((optimize("O2")))
void sharp_rotate(const uint32_t *src, uint32_t *out, int32_t tenths)
{
    double a = (double)tenths * 3.14159265358979 / 1800.0;
    int32_t c = (int32_t)lround(cos(a) * 65536.0), sn = (int32_t)lround(sin(a) * 65536.0);
    const int32_t h = VIN_W / 2;
    for (int y = 0; y < VIN_W; y++) {
        int32_t dy = y - h;
        int32_t hw = (int32_t)sqrt((double)(h * h - dy * dy));   /* only the round screen's span of this row */
        int x0 = h - hw, x1 = h + hw;
        if (x0 < 0) x0 = 0;
        if (x1 > VIN_W) x1 = VIN_W;
        /* inverse map (destination -> source), stepped along the row; +0.5 px so pixel centres line up */
        int32_t u = (x0 - h) * c + dy * sn + (h << 16), v = -(x0 - h) * sn + dy * c + (h << 16);
        uint32_t *o = out + y * VIN_W;
        for (int x = x0; x < x1; x++, u += c, v -= sn) {
            int xi = u >> 16, yi = v >> 16;
            uint32_t fx = (u >> 8) & 0xFF, fy = (v >> 8) & 0xFF;
            uint32_t p00, p10, p01, p11;
            if ((unsigned)xi < VIN_W - 1 && (unsigned)yi < VIN_W - 1) {   /* interior: no bounds checks */
                const uint32_t *q = src + yi * VIN_W + xi;
                p00 = q[0]; p10 = q[1]; p01 = q[VIN_W]; p11 = q[VIN_W + 1];
            } else {                                                      /* the square's edge: soft, transparent out */
                p00 = vin_px(src, xi, yi); p10 = vin_px(src, xi + 1, yi);
                p01 = vin_px(src, xi, yi + 1); p11 = vin_px(src, xi + 1, yi + 1);
            }
            o[x] = vin_lerp(vin_lerp(p00, p10, fx), vin_lerp(p01, p11, fx), fy) | 0xFF000000u;
        }
    }
}

/* The sharp cover, decoded OFF the UI thread.
 *
 * The UI thread only posts a request (the track's path + a generation number) and returns. A worker thread takes
 * the shared decode lock and runs diskos-artdec in "saver" mode on the TRACK ITSELF (embedded art, else its sidecar
 * cover), through art_make_saver: the same SD lease, deadline, revocation and staged-publish rules as all artwork.
 * It reads the result into a NEW buffer and hands it back. A poll timer on the UI thread installs it only if its
 * generation is still the current one, so a track change during the decode can never show the previous cover.
 * A track change hides the old cover at once. If the worker cannot start, the cover simply stays hidden. */
void ui_decode_lock(void); void ui_decode_unlock(void);
static char g_vpath[1024];               /* UI thread: the track the sharp cover is for ("" = none) */
static unsigned g_vgen;                  /* UI thread: bumped on every track change */
static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vcv = PTHREAD_COND_INITIALIZER;
static int g_vwork_started, g_vwant;     /* under g_vmu: worker running; a request is pending */
static char g_vwant_path[1024];          /* under g_vmu */
static unsigned g_vwant_gen;             /* under g_vmu */
static int64_t g_vwant_deadline;         /* under g_vmu: CLOCK_MONOTONIC ms, fixed when the request was posted */
static int g_vfails;                     /* UI thread: failed attempts for the current track (retry limit) */
#define VIN_RETRIES 3
static int64_t vin_now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static uint8_t *g_vres; static unsigned g_vres_gen; static int g_vres_ready;   /* under g_vmu: finished decode */
static unsigned g_vasked_gen = ~0u;      /* UI thread: generation last requested (no duplicate requests) */
static lv_timer_t *g_vpoll;
static int g_vpending;

static void *sharp_worker(void *arg)
{
    (void)arg;
    const size_t need = (size_t)VIN_W * VIN_W * 4;
    for (;;) {
        pthread_mutex_lock(&g_vmu);
        while (!g_vwant) pthread_cond_wait(&g_vcv, &g_vmu);
        char path[1024]; snprintf(path, sizeof path, "%s", g_vwant_path);
        unsigned gen = g_vwant_gen; int64_t deadline = g_vwant_deadline; g_vwant = 0;
        pthread_mutex_unlock(&g_vmu);

        char out[64]; snprintf(out, sizeof out, "/tmp/.diskos-sharp-%u.bgra", gen);
        uint8_t *buf = NULL;
        ui_decode_lock();                               /* one decoder at a time, shared with NP + prewarm */
        /* after waiting for the lock: a newer request, or a spent deadline, means this one is not worth decoding */
        pthread_mutex_lock(&g_vmu);
        int superseded = g_vgen != gen;
        pthread_mutex_unlock(&g_vmu);
        int rc = (superseded || vin_now_ms() >= deadline) ? -1 : art_make_saver(path, out, deadline);
        ui_decode_unlock();
        if (rc == 0) {
            int fd = open(out, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            buf = malloc(need);
            size_t got = 0;
            if (fd >= 0 && buf) {
                ssize_t n;
                while (got < need && (n = read(fd, buf + got, need - got)) > 0) got += (size_t)n;
                char extra; if (got == need && read(fd, &extra, 1) != 0) got = 0;   /* too long is as bad as too short */
            }
            if (fd >= 0) close(fd);
            if (got != need) { free(buf); buf = NULL; }
        }
        unlink(out);
        pthread_mutex_lock(&g_vmu);
        if (gen != g_vgen) { free(buf); pthread_mutex_unlock(&g_vmu); continue; }
        free(g_vres);                                   /* an older, never-collected result */
        g_vres = buf; g_vres_gen = gen; g_vres_ready = 1;
        pthread_mutex_unlock(&g_vmu);
    }
    return NULL;
}

const lv_image_dsc_t *sharp_cover_get(void)
{
    return g_vbuf_valid ? &g_vdsc : NULL;
}

/* Tell Now Playing and evict the cache before freeing the pixels. */
void sharp_cover_invalidate(void)
{
    pthread_mutex_lock(&g_vmu);
    g_vgen++; g_vwant = 0;
    free(g_vres); g_vres = NULL; g_vres_ready = 0;
    pthread_mutex_unlock(&g_vmu);
    g_vpending = 0;
    if (g_vpoll) lv_timer_pause(g_vpoll);
    g_vbuf_valid = 0; g_vfails = 0; g_vasked_gen = ~0u;
    ui_np_cover_changed();
    lv_image_cache_drop(&g_vdsc);
    free(g_vbuf); g_vbuf = NULL;
    g_vdsc.data = NULL;
}

void sharp_cover_set_source(const char *path)
{
    const char *p = path ? path : "";
    if (!strcmp(p, g_vpath)) return;
    sharp_cover_invalidate();
    snprintf(g_vpath, sizeof g_vpath, "%s", p);
}

/* UI thread: install a finished decode if it is still the current track's. The descriptor belongs to this file. */
static void sharp_poll(lv_timer_t *t)
{
    pthread_mutex_lock(&g_vmu);
    if (!g_vres_ready) { pthread_mutex_unlock(&g_vmu); return; }
    uint8_t *buf = g_vres; unsigned gen = g_vres_gen;
    g_vres = NULL; g_vres_ready = 0;
    pthread_mutex_unlock(&g_vmu);
    if (gen != g_vgen) { free(buf); if (!g_vpending) lv_timer_pause(t); return; }
    g_vpending = 0;
    lv_timer_pause(t);
    if (!buf) {
        if (++g_vfails < VIN_RETRIES) g_vasked_gen = ~0u;
        return;                         /* retry only on another visible demand */
    }
    g_vbuf = buf; g_vbuf_valid = 1;
    memset(&g_vdsc, 0, sizeof g_vdsc);
    g_vdsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_vdsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    g_vdsc.header.w = g_vdsc.header.h = VIN_W;
    g_vdsc.header.stride = VIN_W * 4;
    g_vdsc.data = buf; g_vdsc.data_size = (uint32_t)VIN_W * VIN_W * 4;
    ui_np_cover_changed();
}

/* UI thread: visible consumers coalesce requests for the exact current source. */
void sharp_cover_request(void)
{
    if (!g_vpath[0] || g_vbuf_valid || g_vasked_gen == g_vgen) return;
    if (!g_vpoll) g_vpoll = lv_timer_create(sharp_poll, 150, NULL);
    if (!g_vpoll) { if (++g_vfails >= VIN_RETRIES) g_vasked_gen = g_vgen; return; }
    lv_timer_pause(g_vpoll);
    pthread_mutex_lock(&g_vmu);
    if (!g_vwork_started) {
        pthread_t th;
        if (pthread_create(&th, NULL, sharp_worker, NULL) == 0) { pthread_detach(th); g_vwork_started = 1; }
    }
    if (g_vwork_started) {
        snprintf(g_vwant_path, sizeof g_vwant_path, "%s", g_vpath);
        g_vwant_gen = g_vgen; g_vwant = 1;
        g_vwant_deadline = vin_now_ms() + 15000;
        pthread_cond_signal(&g_vcv);
        g_vasked_gen = g_vgen;
        g_vpending = 1;
        lv_timer_reset(g_vpoll);
        lv_timer_resume(g_vpoll);
    } else if (++g_vfails >= VIN_RETRIES) g_vasked_gen = g_vgen;
    pthread_mutex_unlock(&g_vmu);
}

/* The spin (once the vinyl screensaver's): one linear, endless turn every SHARP_TURN_MS starting at `from` (0.1 degree).
 * The exec callback lets a frame through only when sharp_spin_due says so: 30 fps. A frame costs ~7 ms (sharp_rotate),
 * so the loop would otherwise run ~60 fps and spend the saving on frames nobody needs; at 27 s a turn, 30 fps is under
 * half a degree per frame (smooth), and the cap sits far above the render time, so the cadence stays even. */
#define SHARP_TURN_MS  27000   /* ~27s/rev - slow, calm spin */
#define SHARP_FRAME_MS 33      /* 30 fps */
void sharp_spin_start(void *var, int32_t from, lv_anim_exec_xcb_t cb)
{
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, from + 3600);
    lv_anim_set_time(&a, SHARP_TURN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);   /* constant angular speed */
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
int sharp_spin_due(uint32_t *last)
{
    uint32_t now = lv_tick_get();
    if (now - *last < SHARP_FRAME_MS) return 0;
    *last = now;
    return 1;
}
