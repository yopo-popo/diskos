/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "ui.h"
#include "theme.h"
#include "theme_kit.h"
#include "lvgl/lvgl.h"
#include "art.h"
#include "netart.h"
#include "ipc.h"
#include "musicdb.h"   /* persistent per-song accent cache */
#include "listeningdb.h"   /* play counts */
#include "favourites.h"   /* the heart (shelves.db) */
#include "playcount.h"
#include "scanner.h"   /* scan_read_chapters: Now Playing shows a book's current chapter */
#include "artcache.h"   /* persistent decoded-cover cache on SD */
#include "config.h"
#include "i18n.h"
#include "screens.h"
#include "modes.h"
#include "sdio.h"
#include "anim.h"
#include "fonts_intl.h"   /* Cyrillic/Greek/Latin-ext fallback (issue #3) */
#include "fonts_hebrew.h"   /* Hebrew font fallback */
#include "fontft.h"
int screen_figure_number(int which);

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>


LV_FONT_DECLARE(font_icons_28)
#define HEART_FILLED  "\xEF\x80\x84"   /* FA f004 solid heart */
#define HEART_OUTLINE "\xEF\x82\x8A"   /* FA f08a outline heart */
#define MOON_ICON     "\xEF\x86\x86"   /* FA f186 moon (audiobook sleep timer) */
#define MODE_ARROW    "\xEF\x85\xB8"   /* FA f178 long-arrow-right (sequential) */

#define ARC_D        312
#define ARC_SWEEP    286
#define ARC_ROT      127

static lv_obj_t *ring;
static lv_obj_t *title;
static lv_obj_t *artist;
static lv_obj_t *btn_pp;
static lv_obj_t *btn_prev;
static lv_obj_t *btn_next;
static lv_obj_t *btn_fav;
static lv_obj_t *fav_icon;
static int g_np_ov_on;          /* Now Playing's controls show over the art (see "Now Playing: two states") */
static uint32_t g_np_ov_touch;  /* the last touch while they show */
static int g_np_fav, g_np_have, g_fav_px, g_fav_py;
static char g_np_curpath[520];   /* the track the last update showed (the seek scrub latches it) */
static lv_obj_t *btn_mode;
static lv_obj_t *mode_icon;
static lv_obj_t *mode_one;
static int g_np_mode = -1, g_mode_px, g_mode_py;
static lv_obj_t *t_elapsed;
static lv_obj_t *t_remain;

static lv_color_t accent;
static char last_path[256];
/* Play counts (listening.db): followed about once a second by a timer of its own, because ui_update is skipped while
 * the screen is off, which is when most listening happens */
static playcount_t g_playcount;
static void playcount_tick(lv_timer_t *t){
    (void)t;
    track_state_t st;
    ipc_get_state(&st);
    if(!st.have_track || mdb_is_book_path(st.path)){ playcount_reset(&g_playcount); return; }   /* books keep bookmarks */
    if(!st.title_ready) return;            /* hold until the real title: it tells the CUE tracks of one file apart */
    if(playcount_sample(&g_playcount, st.path, st.title, ui_is_playing(), st.position_ms, st.duration_ms)){
        long long start = 0;
        ldb_track_start(st.path, st.title, &start);
        lsdb_record_play(st.path, start);
    }
}
/* Art cache key = album title + the track's PARENT DIRECTORY (not artist). This reuses art
 * within a real album AND within a compilation/various-artists album (same folder), but does
 * NOT reuse across two genuinely-different albums that merely share a title in different
 * folders. For a flat library it degrades to album-only - no worse than before. */
static char last_art_key[420];  /* key of the art currently shown */
static char want_art_key[420];  /* key the CURRENT track wants; a finished decode applies only if it matches */
static char last_seed_a[IPC_TITLE_CAPACITY];
static char last_seed_b[160];
static char cover_src[48];
static int  cover_valid;     /* 1 when cover_src points at a freshly decoded cover */
static int  coverdsc_valid;  /* 1 only when g_coverdsc holds THIS track's RAM decode (else stale) */
static char thumb_src[48]; /* tiny 42px thumb for the Home pill */
static int  thumb_valid;
static char backdrop_src[48]; /* full-screen blurred backdrop */
static int  backdrop_valid;
static int displayed_idx;   /* art buffer (0/1) currently shown; new decodes target ^1 */
static int32_t shown_progress;
static int  g_scrubbing;     /* finger on the seek arc - don't fight it */
static long g_track_dur;     /* current track duration (ms) for seek math */
static long g_seek_lo, g_seek_hi;  /* the ms window the ring spans + seeks within: a chapter for a book, the whole track otherwise */
/* After releasing a seek, the player keeps streaming the OLD position for a
 * beat before it processes the jump, which makes the arc snap back then jump
 * forward. Hold the display at the seeked target and ignore stale echoes until
 * the player's stream reaches it (or the window lapses). */
static uint32_t g_seek_hold_until = 0;
static long     g_seek_target_ms = -1;   /* ABSOLUTE ms we seeked to - window-independent, so a seek that lands on a chapter boundary (window flips) isn't mistaken for a stale echo */
static long     g_scrub_lo, g_scrub_hi;  /* the ms window LATCHED when a ring drag starts - playback advancing under the finger (a chapter boundary crossed mid-drag) must not reinterpret the arc */
static char     g_scrub_path[520];       /* the track the drag STARTED on - if the track changes before release, the release seek (computed from the old track's window) must NOT be applied to the new track */
/* A seek and a back/hub swipe can both start anywhere on the ring, so we tell
 * them apart by DIRECTION: a seek follows the ring (curved / has a vertical
 * component) while back/hub is a long, straight, horizontal slide. Once a drag
 * looks horizontal we freeze the bar (no seek preview) and let the main loop
 * navigate; otherwise the ring scrubs as normal. */
static int g_seek_sx, g_seek_sy;   /* seek gesture press origin */
static int g_seek_cand;            /* press landed on the ring band (seek candidate) */
static int g_seek_on;              /* seek confirmed & actively scrubbing */

static lv_color_t accent_from(const char *a, const char *b)
{
    unsigned h = 2166136261u;
    for(const char *p = a; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    for(const char *p = b; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return theme_accent_from_hsv(h % 360, 65, 84);   /* no-art fallback: forced good S/V */
}

static void mmss(long ms, char *buf, size_t len)
{
    if(ms < 0) ms = 0;
    long total = ms / 1000;
    long sec = total % 60;
    long hr  = total / 3600;
    if(hr > 0){                                  /* audiobook-length: H:MM:SS (music tracks stay M:SS) */
        long min = (total / 60) % 60;
        if(hr > 999) hr = 999;
        snprintf(buf, len, "%ld:%02ld:%02ld", hr, min, sec);
    } else {
        snprintf(buf, len, "%ld:%02ld", total / 60, sec);
    }
}

static void copy_cstr(char *dst, size_t dst_len, const char *src)
{
    if(dst_len == 0) return;
    if(src == NULL) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

/* The play/pause glyph. A play triangle's weight is on its flat side, so a centred box looks left of centre:
 * its text sits right by a tenth of the glyph's width (padding, so a label that is itself the button keeps its box). */
/* where a glyph's ink sits inside its line: the offset that puts the ink's middle on the line box's middle (icon
 * glyphs sit high or low in their line; centring the line box leaves them visibly off) */
int ui_glyph_ink_dy(const lv_font_t *f, uint32_t cp)
{
    lv_font_glyph_dsc_t g;
    if(!f || !lv_font_get_glyph_dsc(f, &g, cp, 0)) return 0;
    int lh = lv_font_get_line_height(f);
    int ink_top = (lh - f->base_line) - (g.ofs_y + g.box_h);      /* from the line's top to the ink's top */
    return lh / 2 - (ink_top + g.box_h / 2);
}
/* a one-glyph label centred in its box by its ink (moved with translate_y, so its box and alignment stay) */
void ui_glyph_center_ink(lv_obj_t *label)
{
    if(!label || !lv_obj_check_type(label, &lv_label_class)) return;
    const unsigned char *t = (const unsigned char *)lv_label_get_text(label);
    /* the first character, decoded here (icon glyphs are 3-byte UTF-8: U+E000..U+FFFF) */
    if(!t || (t[0] & 0xF0) != 0xE0 || (t[1] & 0xC0) != 0x80 || (t[2] & 0xC0) != 0x80) return;   /* words keep their baseline */
    uint32_t cp = ((uint32_t)(t[0] & 0x0F) << 12) | ((uint32_t)(t[1] & 0x3F) << 6) | (t[2] & 0x3F);
    if(cp < 0xE000) return;                                       /* icon glyphs only */
    lv_obj_set_style_translate_y(label, ui_glyph_ink_dy(lv_obj_get_style_text_font(label, 0), cp), 0);
}
void ui_pp_glyph(lv_obj_t *label, int playing)
{
    if(label == NULL) return;
    const char *t = playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    if(strcmp(lv_label_get_text(label), t) != 0) lv_label_set_text(label, t);
    const lv_font_t *f = lv_obj_get_style_text_font(label, 0);
    int dx = 0;
    lv_font_glyph_dsc_t g;
    if(!playing && lv_font_get_glyph_dsc(f, &g, 0xF04B, 0)) dx = (g.box_w + 5) / 10;
    if(lv_obj_get_style_pad_left(label, 0) != dx){
        lv_obj_set_style_pad_left(label, dx, 0);
        lv_obj_set_style_pad_right(label, -dx, 0);
    }
    /* a label that IS the button (a theme gave it a fixed height): its glyph's ink centred top to bottom too */
    int32_t sh = lv_obj_get_style_height(label, 0);
    if(sh != LV_SIZE_CONTENT && !LV_COORD_IS_PCT(sh) && sh > 0){
        int bw = lv_obj_get_style_border_width(label, 0), lh = lv_font_get_line_height(f);
        int pt = sh / 2 - bw - lh / 2 + ui_glyph_ink_dy(f, playing ? 0xF04C : 0xF04B);
        if(pt < 0) pt = 0;
        if(lv_obj_get_style_pad_top(label, 0) != pt) lv_obj_set_style_pad_top(label, pt, 0);
    }
}

/* The input a label was last given, kept beside it: a LONG_DOT label edits its own copy ("Long na..."), so comparing
 * with lv_label_get_text() never matched and every update set and fitted the text again. A new width, font or style
 * needs no new text: the label refits its kept original itself. Theme, font size and language change only with a
 * restart. Owned by the label (freed with it); UI thread only. The label's text is set only through these. */
typedef struct { char *text; } label_input_t;
static void label_input_free(lv_event_t *e)
{
    label_input_t *in = lv_event_get_user_data(e);
    if(in){ free(in->text); free(in); }
}
/* 1 when txt differs from the label's last input (or none is kept), and keeps txt as the new one */
int ui_label_input_changed(lv_obj_t *obj, const char *txt)
{
    if(obj == NULL) return 0;
    if(txt == NULL) txt = "";
    label_input_t *in = NULL;
    for(uint32_t i = 0; i < lv_obj_get_event_count(obj); i++){
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if(lv_event_dsc_get_cb(d) == label_input_free){ in = lv_event_dsc_get_user_data(d); break; }
    }
    if(in && in->text && strcmp(in->text, txt) == 0) return 0;
    if(!in){
        in = calloc(1, sizeof *in);
        if(!in) return 1;                                   /* nothing kept: always set, as before */
        if(!lv_obj_add_event_cb(obj, label_input_free, LV_EVENT_DELETE, in)){ free(in); return 1; }
    }
    free(in->text);
    in->text = strdup(txt);                                 /* NULL: compares unequal next time */
    return 1;
}
int ui_label_set_changed(lv_obj_t *obj, const char *txt)
{
    if(!ui_label_input_changed(obj, txt)) return 0;
    lv_label_set_text(obj, txt ? txt : "");
    return 1;
}
static void set_label_text_changed(lv_obj_t *obj, const char *txt)
{
    ui_label_set_changed(obj, txt);
}

/* ---- Now Playing seek recognizer ---------------------------------------
 * The ring is DISPLAY-ONLY (not clickable); main.c feeds raw touch into these.
 * A seek is recognized only when the press lands on the ring band within the
 * progress sweep (and below the top drawer zone) AND the drag follows the ring
 * rather than being a straight horizontal nav slide -- so nav swipes and the
 * top-drawer pull never move the arc. "Classify first, move the arc second." */
#define RING_CX  180
#define RING_CY  180
static int g_ring_d = ARC_D;      /* the drawn ring diameter; the seek band follows it ("something": at the bezel) */
#define RING_R   (g_ring_d / 2)

/* A theme may seek on a straight bar instead of the ring (ui_np_seek_line): the band is then the bar's box. */
void ui_np_backdrop_enabled(int on){ (void)on; }   /* Now Playing always shows its art now: a theme's request is a no-op */
static int g_pp_filled;               /* the theme's play key is an accent-filled button (ui_np_pp_filled) */
void ui_np_pp_filled(int on){ g_pp_filled = on; }
static lv_obj_t *g_seek_bar;          /* the line-seek bar (lv_bar or lv_slider), NULL = ring */
static int32_t g_seek_shown;          /* the value last shown by seek_show() */
static void seek_show(int32_t v){
    g_seek_shown = v;
    lv_arc_set_value(ring, v);
    if(g_seek_bar){
        if(lv_obj_check_type(g_seek_bar, &lv_slider_class)) lv_slider_set_value(g_seek_bar, v, LV_ANIM_OFF);
        else lv_bar_set_value(g_seek_bar, v, LV_ANIM_OFF);
    }
}
void ui_np_seek_line(lv_obj_t *bar){
    g_seek_bar = bar;
    if(bar){
        if(lv_obj_check_type(bar, &lv_slider_class)) lv_slider_set_range(bar, 0, 1000); else lv_bar_set_range(bar, 0, 1000);
        seek_show(g_seek_shown);
    }
}
static int seek_on_band(int x, int y, int tol){
    if(g_seek_bar){
        lv_area_t a; lv_obj_get_coords(g_seek_bar, &a);
        return x >= a.x1 - 12 && x <= a.x2 + 12 && y >= a.y1 - tol && y <= a.y2 + tol;
    }
    int dx = x - RING_CX, dy = y - RING_CY;
    int d2 = dx*dx + dy*dy;
    int lo = RING_R - tol, hi = RING_R + tol;
    return d2 >= lo*lo && d2 <= hi*hi;
}
static double seek_rel_angle(int x, int y){
    double a = atan2((double)(y - RING_CY), (double)(x - RING_CX)) * 57.2957795;
    double rel = a - ARC_ROT;
    while(rel < 0) rel += 360.0;
    while(rel >= 360.0) rel -= 360.0;
    return rel;   /* degrees from the start of the sweep, clockwise */
}
static int32_t seek_pt_to_value(int x, int y){
    if(g_seek_bar){
        lv_area_t a; lv_obj_get_coords(g_seek_bar, &a);
        int w = a.x2 - a.x1; if(w < 1) w = 1;
        int32_t v = (int32_t)((x - a.x1) * 1000L / w);
        return v < 0 ? 0 : v > 1000 ? 1000 : v;
    }
    double rel = seek_rel_angle(x, y);
    if(rel > ARC_SWEEP) rel = (rel - ARC_SWEEP < 360.0 - rel) ? ARC_SWEEP : 0.0;
    int32_t v = (int32_t)(rel / ARC_SWEEP * 1000.0 + 0.5);
    if(v < 0) v = 0;
    if(v > 1000) v = 1000;
    return v;
}

/* A ring drag moves along the ring: more travel around it than in or out, from where the press landed. */
static int seek_along_ring(int dx, int dy){
    double rx = g_seek_sx - RING_CX, ry = g_seek_sy - RING_CY, r = sqrt(rx*rx + ry*ry);
    if(r < 1) return 0;
    return fabs((dx*ry - dy*rx) / r) >= fabs((dx*rx + dy*ry) / r);
}
/* press: returns 1 if this touch could be a seek (and arms the recognizer) */
int ui_np_seek_press(int x, int y){
    g_seek_cand = 0; g_seek_on = 0; g_scrubbing = 0;   /* also clears a stuck scrub from a missed release */
    if(!g_np_ov_on) return 0;                     /* only the art shows: a touch is a tap on it, never a seek */
    g_np_ov_touch = lv_tick_get();                /* a touch keeps the controls up */
    if(y < 40) return 0;                          /* top drawer zone */
    if(g_track_dur <= 0) return 0;                /* nothing to seek */
    if(!seek_on_band(x, y, 34)) return 0;         /* not on the ring (grab band) */
    if(g_seek_bar){ if(x < 64) return 0; }        /* line seek: a touch from the left edge stays a back swipe */
    else if(seek_rel_angle(x, y) > ARC_SWEEP) return 0;/* in the bottom gap (transport buttons), not the arc */
    g_seek_sx = x; g_seek_sy = y; g_seek_cand = 1;
    return 1;
}
/* move: returns 1 while a seek owns the gesture (so main.c skips navigation) */
int ui_np_seek_move(int x, int y){
    if(g_np_ov_on) g_np_ov_touch = lv_tick_get();
    if(!g_seek_cand) return 0;
    int dx = x - g_seek_sx, dy = y - g_seek_sy;
    int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
    if(!seek_on_band(x, y, 48)){                   /* wandered off the ring -> a nav swipe */
        if(g_seek_on){ g_seek_on = 0; g_scrubbing = 0; seek_show(shown_progress); }
        g_seek_cand = 0; return 0;
    }
    if(!g_seek_on){
        if(adx < 10 && ady < 10) return 0;            /* not enough travel to classify; hold the arc */
        if(!g_seek_bar && !seek_along_ring(dx, dy)){ g_seek_cand = 0; return 0; } /* ring: mostly in or out across it -> a nav swipe (along it, even sideways at the top, is a seek); a line seek IS horizontal */
        g_seek_on = 1; g_scrubbing = 1;               /* confirmed: a deliberate ring drag */
        g_scrub_lo = g_seek_lo; g_scrub_hi = g_seek_hi;   /* latch the window NOW so it can't shift under the finger */
        if(g_scrub_hi <= g_scrub_lo){ g_scrub_lo = 0; g_scrub_hi = g_track_dur; }
        copy_cstr(g_scrub_path, sizeof g_scrub_path, g_np_curpath);   /* latch WHICH track this drag is on */
    }
    int32_t v = seek_pt_to_value(x, y);
    seek_show(v);
    shown_progress = v;   /* keep the render cache in sync with the direct arc write, else set_progress_changed() can skip a needed reset (e.g. drag to a chapter end -> next chapter's progress 0 == a stale cached 0 -> arc stuck at 100%) */
    {
        long lo = g_scrub_lo, hi = g_scrub_hi;   /* latched window (music: whole track) */
        if(hi > lo){
            long span = hi - lo;
            long rel  = (long)((int64_t)v * span / 1000);   /* preview is window-relative (chapter for a book) */
            char b[12]; mmss(rel, b, sizeof b); set_label_text_changed(t_elapsed, b);
            char r[12]; mmss(span - rel, r, sizeof r);
            char rr[14]; snprintf(rr, sizeof rr, "-%s", r); set_label_text_changed(t_remain, rr);
        }
    }
    return 1;
}
/* release: commit the seek if one was active. returns 1 if it consumed the gesture */
int ui_np_seek_release(int x, int y){
    (void)x; (void)y;
    int consumed = 0;
    /* If the track changed while the finger was down, the drag was computed against the OLD track's
     * window - applying it to the NEW track would scramble its position and cancel its resume. Compare
     * against a FRESH player-state read (g_np_curpath is only a per-tick UI snapshot and can lag the
     * real track). Drop the seek on a mismatch. */
    if(g_seek_on && g_scrub_path[0]){
        track_state_t s; ipc_get_state(&s);
        if(strcmp(g_scrub_path, s.path) != 0){
            g_seek_cand = 0; g_seek_on = 0; g_scrubbing = 0;
            seek_show(shown_progress);   /* snap the arc back to the real position */
            return 1;                                 /* consumed the gesture, but issued NO seek */
        }
    }
    if(g_seek_on && g_track_dur > 0){
        int32_t v = g_seek_shown;
        long lo = g_scrub_lo, hi = g_scrub_hi;   /* the window latched at drag start */
        if(hi <= lo){ lo = 0; hi = g_track_dur; }   /* fall back to the whole track */
        long ms = (hi > lo) ? lo + (long)((int64_t)v * (hi - lo) / 1000) : 0;   /* latched window-relative v -> absolute ms */
        g_seek_target_ms = ms;
        g_seek_hold_until = lv_tick_get() + 2500;   /* suppress stale echo ~2.5s */
        /* only hand control away from a pending book-resume if the seek actually goes out; a failed
         * send must leave the resume pending so the saved position isn't lost. */
        if(ui_seek_to(ms) == 0) ui_book_user_seeked(ms);
        consumed = 1;
    }
    g_seek_cand = 0; g_seek_on = 0; g_scrubbing = 0;
    return consumed;
}

static void set_progress_changed(int32_t value)
{
    if(g_scrubbing) return;   /* the finger owns the arc while scrubbing */
    if(value < 0) value = 0;
    if(value > 1000) value = 1000;

    if(ring && shown_progress != value) {
        seek_show(value);
        shown_progress = value;
    }
}
int ui_np_progress(void){ return shown_progress; }

static void style_text(lv_obj_t *obj, const lv_font_t *font, lv_color_t color)
{
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(obj, 0, LV_PART_MAIN);
}

int ui_transport_command(const char *cmd)
{
    if(!cmd) return -1;
    int is_next = !strcmp(cmd, "0201000C0001");
    int is_prev = !strcmp(cmd, "0201000C0002");
    int is_toggle = !strcmp(cmd, "0201000C0000");
    if(!is_next && !is_prev && !is_toggle) return -1;
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return -1; }
    if(!ui_local_playback_allowed()){
        ui_toast("Return to local playback first"); return -1;
    }
    ui_defer_sleep();
    if(is_next || is_prev){
        track_state_t st; ipc_get_state(&st);
        if(mdb_is_book_path(st.path)){
            long tgt = st.position_ms + (is_next ? 30000 : -15000);
            if(tgt < 0) tgt = 0;
            if(st.duration_ms > 0 && tgt > st.duration_ms) tgt = st.duration_ms;
            if(ui_seek_to(tgt) < 0){ ui_toast("Couldn't seek - try again"); return -1; }
            g_seek_target_ms = tgt;
            g_seek_hold_until = lv_tick_get() + 2500;
            ui_book_user_seeked(tgt);
            return 0;
        }
    }
    ui_note_transport_sent();
    if(ipc_send_cmd(cmd) < 0){ ui_toast("Player busy - try again"); return -1; }
    if(is_toggle){
        ui_pp_tap_hint();
        ui_pp_glyph(btn_pp, ui_pp_icon_playing(ui_is_playing()));
    } else {
        ui_cancel_book_resume();
        ui_disarm_book_eoc();
    }
    return 0;
}
static void transport_cb(lv_event_t *e)
{
    ui_transport_command((const char *)lv_event_get_user_data(e));
}

static void make_clickable(lv_obj_t *o, const char *cmd)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(o, 18);
    char an[24];
    ui_on(o, transport_cb, LV_EVENT_CLICKED, (void *)cmd, ui_transport_action(cmd, "np", an, sizeof an), UI_CORE);
}

static void apply_accent(void)
{
    if(ring) {
        lv_obj_set_style_arc_color(ring, accent, LV_PART_INDICATOR);
    }

    if(btn_pp) {
        if(g_pp_filled) lv_obj_set_style_bg_color(btn_pp, accent, LV_PART_MAIN);
        else lv_obj_set_style_text_color(btn_pp, accent, LV_PART_MAIN);
    }

    if(fav_icon && g_np_fav) {
        lv_obj_set_style_text_color(fav_icon, accent, LV_PART_MAIN);
    }

    if(mode_icon && g_np_mode > 0) {
        lv_obj_set_style_text_color(mode_icon, accent, LV_PART_MAIN);
    }

    /* keep the other accent-bearing surfaces in lockstep so no screen shows a stale
     * (hardcoded-pink) accent: Home's now-playing capsule. */
    home_set_accent(accent);
}

/* the live accent (user-picked static, or the album-derived dynamic colour). Other
 * modules paint with this instead of a hardcoded constant. */
lv_color_t ui_current_accent(void){ return accent; }

/* ---- shared standard header (back chevron + centred title) ------------------------------------- */
static void ui_header_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }
/* the edge Back sits where a screen's list starts: once the screen has built its body (after this call), raise Back
 * above it so its whole target takes taps. The object is checked first: the screen may have been rebuilt meanwhile. */
static void header_back_raise(void *o){ lv_obj_move_foreground((lv_obj_t *)o); }
/* a Back deleted before its raise ran (screen rebuilt) cancels it, so the address is never reused for another object */
static void header_back_deleted(lv_event_t *e){ lv_async_call_cancel(header_back_raise, lv_event_get_target(e)); }
/* full form: custom back handler (e.g. Library pops its view stack before leaving the screen). */
/* the width of the round screen at height y, less a margin each side (text wider than this leaves the glass) */
int ui_round_width(int y, int margin){
    int dy = y < 180 ? 180 - y : y - 180;
    if(dy >= 180) return 0;
    int w = 2 * (int)sqrtf((float)(180 * 180 - dy * dy)) - 2 * margin;
    return w > 0 ? w : 0;
}
/* a one-line title placed centred at mid-height cy: its face steps down (via next) until the title fits the glass at
 * its top row; the box is exactly that wide */
static void title_fit(lv_obj_t *t, const char *text, const lv_font_t *f, int cy, int pad_top,
                      const lv_font_t *(*next)(const lv_font_t *)){
    for(int k = 0; k < 3; k++){
        int h = lv_font_get_line_height(f) + pad_top, w = ui_round_width(cy - h / 2 + pad_top + 2, 10);
        if(w > 250) w = 250;
        lv_point_t sz; lv_text_get_size(&sz, text, f, lv_obj_get_style_text_letter_space(t, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const lv_font_t *sm = next ? next(f) : NULL;
        if(sz.x <= w || !sm || k == 2){
            lv_obj_set_style_text_font(t, f, 0);
            lv_obj_set_user_data(t, (void *)&theme_title_tag);   /* a title to the theme kit, whatever face it fits in */
            lv_obj_set_pos(t, 180 - w / 2, cy - h / 2); lv_obj_set_size(t, w, h);
            lv_obj_set_style_pad_top(t, pad_top, 0);
            return;
        }
        f = sm;
    }
}
static const lv_font_t *dot_smaller(const lv_font_t *f){ return f == TF(HEADER) ? TF(DATE) : NULL; }
static lv_obj_t *g_header_back;   /* the back arrow of the header built last */
lv_obj_t *ui_header_back(void){ return g_header_back; }
lv_obj_t *ui_header_cb(lv_obj_t *root, const char *title, lv_event_cb_t back_cb){ return theme_kit()->header(root, tr(title), back_cb); }
lv_obj_t *kit_default_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *back = g_header_back = lv_button_create(root);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, 72, 24); lv_obj_set_size(back, 44, 40);   /* inset from the clipped corner */
    lv_obj_set_ext_click_area(back, 10);                            /* easier near the round bezel */
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_style_bg_color(back, TC(LIST_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_70, LV_STATE_PRESSED);
    ui_on(back, back_cb ? back_cb : ui_header_back_cb, LV_EVENT_CLICKED, NULL, "nav.back", UI_CORE);
    lv_obj_t *ic = lv_label_create(back);
    lv_label_set_text(ic, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(ic, TF(UI_20), 0);
    lv_obj_set_style_text_color(ic, TC(TEXT_SECONDARY), 0);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *t = lv_label_create(root);
    if(theme_trait(THEME_TRAIT_DOT_TITLES)){
        /* dot-matrix title across the top, upper case; the back arrow moves to the left edge, mid-height */
        /* the dot face, or its smaller size when the title is too long for the top band; the box follows the face
         * with 6 px of headroom (the dot glyphs rise above the line box) so the caps never clip */
        char up[160];
        title_fit(t, theme_upper(up, sizeof up, title), TF(HEADER), 46, 6, dot_smaller);
        theme_title_text(t, title);
        lv_obj_set_pos(back, 0, 140); lv_obj_set_size(back, 30, 80);   /* the round screen's left edge, mid-height */
        lv_obj_set_ext_click_area(back, 0);                            /* stops where rows start: no stray Back */
        lv_async_call(header_back_raise, back);          /* above the body the screen builds after this header */
        lv_obj_add_event_cb(back, header_back_deleted, LV_EVENT_DELETE, NULL);
        lv_label_set_text(ic, "\xEF\x81\xA0");                /* f060 arrow-left */
        lv_obj_set_style_text_font(ic, TF(ICON_20), 0);
        lv_obj_set_style_text_color(ic, TC(TEXT_PRIMARY), 0);
        if(theme_trait(THEME_TRAIT_MATRIX_CLOCK)) kit_some_dot_arrow(ic, 1);
    } else {
    theme_title_text(t, title);                     /* the theme's title case (Default: as given) */
    /* Screen-centred (label centre = 104+76 = 180) but bounded to a zone that clears the back chevron on
     * both sides, so a long album/playlist name truncates with an ellipsis instead of sliding under it. */
    lv_obj_set_pos(t, 104, 30); lv_obj_set_size(t, 152, 26);
    }
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    if(!theme_trait(THEME_TRAIT_DOT_TITLES))
        lv_obj_set_style_text_font(t, TF(HEADER), 0);   /* headers show folder/album/playlist names: chain (issue #3) */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    return t;
}
lv_obj_t *ui_header(lv_obj_t *root, const char *title){ return ui_header_cb(root, title, NULL); }
lv_obj_t *kit_edge_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb);
void ui_back_glyph(lv_obj_t *label){   /* the edge Back's face: the theme's text, else the arrow icon */
    const char *t = theme_kit()->back_text;
    lv_label_set_text(label, t ? t : "\xEF\x81\xA0");     /* f060 arrow-left */
    lv_obj_set_style_text_font(label, t ? TF(UI_20) : TF(ICON_20), 0);
    lv_obj_set_style_text_color(label, TC(TEXT_PRIMARY), 0);
}
int ui_edge_nav(void){   /* 1 = this theme puts Back on the screen's left edge (screens with their own headers follow) */
    return theme_kit()->header != theme_kit_default.header || theme_trait(THEME_TRAIT_DOT_TITLES);
}
/* The edge header (themes): the title across the top in the theme's header face and case, Back as an arrow on the
 * round screen's left edge at mid-height, raised above the body the screen builds after this call. */
lv_obj_t *kit_edge_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *back = g_header_back = lv_button_create(root);
    lv_obj_remove_style_all(back);
    /* 30 px wide with no extra reach: it stops where the screen's rows and sliders start (x 31), so a tap on a row's
     * left edge never goes Back; 80 px tall keeps the target easy to hit along the rim */
    lv_obj_set_pos(back, 0, 140); lv_obj_set_size(back, 30, 80);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_style_bg_color(back, TC(LIST_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_70, LV_STATE_PRESSED);
    ui_on(back, back_cb ? back_cb : ui_header_back_cb, LV_EVENT_CLICKED, NULL, "nav.back", UI_CORE);
    lv_async_call(header_back_raise, back);
    lv_obj_add_event_cb(back, header_back_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *ic = lv_label_create(back);
    ui_back_glyph(ic);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -2);
    lv_obj_t *t = lv_label_create(root);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    theme_title_text(t, title);                       /* the theme's form first: that is the text that must fit */
    char shown[160]; lv_snprintf(shown, sizeof shown, "%s", lv_label_get_text(t));
    /* the box is as wide as the glass at the title's height (never past the round edge); a long title steps down
     * to the theme's smaller faces; the height follows the face, so its caps never clip */
    title_fit(t, shown, TF(HEADER), 46, 0, theme_title_smaller);
    theme_title_text(t, title);                       /* laid out again at the final size */
    return t;
}

/* Favorites heart on Now Playing: filled+accent when the current track is a
 * favorite, outline+grey otherwise. Tap toggles it (movement-guarded so a swipe
 * doesn't accidentally favorite); it is saved in shelves.db (favourites.h). */
static void fav_refresh(int on, int have)
{
    g_np_fav = on; g_np_have = have;
    if(!btn_fav || !fav_icon) return;
    /* every playback update lands here: change only what differs (each set marks the shared screen for layout) */
    if(have == lv_obj_has_flag(btn_fav, LV_OBJ_FLAG_HIDDEN)){
        if(have) lv_obj_remove_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);
    }
    set_label_text_changed(fav_icon, on ? HEART_FILLED : HEART_OUTLINE);
    lv_color_t c = on ? accent : TC(TEXT_MUTED);
    if(!lv_color_eq(lv_obj_get_style_text_color(fav_icon, LV_PART_MAIN), c)) lv_obj_set_style_text_color(fav_icon, c, LV_PART_MAIN);
}

static void fav_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *id = lv_indev_active();
    if(code == LV_EVENT_PRESSED) {
        if(id) { lv_point_t p; lv_indev_get_point(id, &p); g_fav_px = p.x; g_fav_py = p.y; }
    } else if(code == LV_EVENT_CLICKED) {
        if(!g_np_have) return;
        if(id) {
            lv_point_t p; lv_indev_get_point(id, &p);
            int dx = p.x - g_fav_px, dy = p.y - g_fav_py;
            if((dx<0?-dx:dx) > 18 || (dy<0?-dy:dy) > 18) return;   /* was a swipe (cst816t wobble tolerance) */
        }
        track_state_t st; ipc_get_state(&st);   /* the song playing now, not the one shown at the last update */
        int on = !fav_current(&st);
        if(!fav_current_set(&st, on)){ ui_toast("Couldn't save the favourite"); return; }
        fav_refresh(on, 1);
        ui_toast(on ? "Added to Favourites" : "Removed from Favourites");
    }
}

/* Play-mode toggle on Now Playing: cycles work_mode 0..4 via 0102 (ui_set_workmode).
 * Stock order (ground-truth captured 2026-06-25): 0=Sequential, 1=Shuffle,
 * 2=Repeat One, 3=Repeat All, 4=Single (play one, stop). Icons (existing glyphs):
 *   0 arrow         (grey/inactive base)
 *   1 shuffle
 *   2 loop + "1"
 *   3 loop
 *   4 arrow + "1"   (single = play-one)
 * The "1" overlay (mode_one) is shown for Repeat One and Single. */
#define WORKMODE_COUNT 5
static void mode_refresh(int wm)
{
    if(!btn_mode || !mode_icon) return;
    if(wm == g_np_mode) return;
    g_np_mode = wm;
    const char *icon = (wm == 1) ? LV_SYMBOL_SHUFFLE
                     : (wm == 2 || wm == 3) ? LV_SYMBOL_LOOP
                     : MODE_ARROW;                       /* 0 and 4 use the arrow */
    lv_label_set_text(mode_icon, icon);
    lv_obj_set_style_text_color(mode_icon, wm ? accent : TC(TEXT_MUTED), LV_PART_MAIN);
    if(wm == 2 || wm == 4) lv_obj_remove_flag(mode_one, LV_OBJ_FLAG_HIDDEN);
    else                   lv_obj_add_flag(mode_one, LV_OBJ_FLAG_HIDDEN);
}

static void mode_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *id = lv_indev_active();
    if(code == LV_EVENT_PRESSED) {
        if(id) { lv_point_t p; lv_indev_get_point(id, &p); g_mode_px = p.x; g_mode_py = p.y; }
    } else if(code == LV_EVENT_CLICKED) {
        if(id) {
            lv_point_t p; lv_indev_get_point(id, &p);
            int dx = p.x - g_mode_px, dy = p.y - g_mode_py;
            if((dx<0?-dx:dx) > 18 || (dy<0?-dy:dy) > 18) return;   /* was a swipe (cst816t wobble tolerance) */
        }
        int wm = (cfg_get_int("work_mode", 0) + 1) % WORKMODE_COUNT;
        cfg_set_int("work_mode", wm);
        ui_set_workmode(wm);
        mode_refresh(wm);
        static const char *const MODE_NAMES[WORKMODE_COUNT] =
            { "Sequential", "Shuffle", "Repeat One", "Repeat All", "Single" };
        if(wm >= 0 && wm < WORKMODE_COUNT) ui_toast(MODE_NAMES[wm]);
    }
}

static lv_label_long_mode_t g_title_scroll_mode = LV_LABEL_LONG_CLIP;

/* Hidden metadata updates must not recreate the title animation. */
void ui_np_title_scroll(int visible)
{
    if(!title) return;
    visible = visible && !lv_obj_has_flag(lv_obj_get_parent(title), LV_OBJ_FLAG_HIDDEN);
    lv_label_long_mode_t mode = lv_label_get_long_mode(title);
    if(visible){
        if(mode == LV_LABEL_LONG_CLIP && g_title_scroll_mode != LV_LABEL_LONG_CLIP)
            lv_label_set_long_mode(title, g_title_scroll_mode);
        g_title_scroll_mode = LV_LABEL_LONG_CLIP;
    } else if(mode == LV_LABEL_LONG_SCROLL || mode == LV_LABEL_LONG_SCROLL_CIRCULAR){
        g_title_scroll_mode = mode;
        lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    }
}

/* Decode the 148px cover BMP into a RAM ARGB8888 descriptor. A file-sourced
 * lv_bmp image can't be rotated by the SW renderer (renders black) because the
 * image cache is off; a true-colour RAM buffer transforms fine, so the vinyl
 * can spin. The BMP is 24-bit BGR, bottom-up, 148*3=444 bytes/row (no padding). */
#define CBMP 148
static uint8_t g_coverbuf[CBMP*CBMP*4];
static lv_image_dsc_t g_coverdsc;
static int load_cover_dsc(const char *path)
{
    FILE *f = fopen(path, "rb"); if(!f) return -1;
    uint8_t hdr[54];
    if(fread(hdr,1,54,f)!=54){ fclose(f); return -1; }
    uint32_t off = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((uint32_t)hdr[13]<<24);
    int w = hdr[18]|(hdr[19]<<8), h = hdr[22]|(hdr[23]<<8);
    int bpp = hdr[28]|(hdr[29]<<8);
    uint32_t comp = hdr[30]|(hdr[31]<<8)|(hdr[32]<<16)|((uint32_t)hdr[33]<<24);
    if(w!=CBMP || h!=CBMP || bpp!=24 || comp!=0){ fclose(f); return -1; }
    static uint8_t row[CBMP*3];
    if(fseek(f, off, SEEK_SET)!=0){ fclose(f); return -1; }
    for(int yy=0; yy<CBMP; yy++){
        if(fread(row,1,CBMP*3,f)!=(size_t)(CBMP*3)){ fclose(f); return -1; }
        uint8_t *d = g_coverbuf + (CBMP-1-yy)*CBMP*4;   /* bottom-up -> top-down */
        for(int x=0;x<CBMP;x++){
            d[x*4+0]=row[x*3+0]; d[x*4+1]=row[x*3+1];   /* B, G */
            d[x*4+2]=row[x*3+2]; d[x*4+3]=0xFF;          /* R, A */
        }
    }
    fclose(f);
    g_coverdsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_coverdsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    g_coverdsc.header.w      = CBMP;
    g_coverdsc.header.h      = CBMP;
    g_coverdsc.header.stride = CBMP*4;
    g_coverdsc.data          = g_coverbuf;
    g_coverdsc.data_size     = sizeof g_coverbuf;
    return 0;
}

/* The accent path (OKLab helpers + read_cover_bmp + accent_from_buf) is compiled
 * at -O0: at -Os this float-heavy code SIGSEGVs on the worker/prewarm thread
 * (heisenbug - adding any logging hid it; bounds/indices verified clean by two
 * static-analysis passes). -O0 makes the codegen deterministic. It runs off the UI thread
 * so the speed cost is irrelevant. */
#pragma GCC push_options
#pragma GCC optimize ("O0")

/* ---- Apple-grade album accent: OKLab k-means clustering ->
 * Vibrant-on-dark swatch scoring -> tone-map -> WCAG contrast-lift vs the dark UI
 * -> neutral fallbacks. Runs once per cover decode. */
static float g_lin_lut[256]; static int g_lin_lut_init = 0;
static void lin_lut_init(void){
    for(int i=0;i<256;i++){ float x=i/255.0f; g_lin_lut[i]= x<=0.04045f ? x/12.92f : powf((x+0.055f)/1.055f, 2.4f); }
    g_lin_lut_init=1;
}
static float a_clamp01(float x){ return x<0?0:(x>1?1:x); }
static float a_smooth(float a,float b,float x){ if(b<=a) return x>=b?1.0f:0.0f; float t=a_clamp01((x-a)/(b-a)); return t*t*(3.0f-2.0f*t); }
static void rgb2oklab(int R,int G,int B, float *L,float *a,float *bb){
    float r=g_lin_lut[R], g=g_lin_lut[G], bl=g_lin_lut[B];
    float l=0.4122214708f*r+0.5363325363f*g+0.0514459929f*bl;
    float m=0.2119034982f*r+0.6806995451f*g+0.1073969566f*bl;
    float s=0.0883024619f*r+0.2817188376f*g+0.6299787005f*bl;
    float l_=cbrtf(l), m_=cbrtf(m), s_=cbrtf(s);
    *L =0.2104542553f*l_+0.7936177850f*m_-0.0040720468f*s_;
    *a =1.9779984951f*l_-2.4285922050f*m_+0.4505937099f*s_;
    *bb=0.0259040371f*l_+0.7827717662f*m_-0.8086757660f*s_;
}
/* OKLab -> sRGB (Ottosson inverse). Returns 1 if in gamut; always fills R/G/B (clamped). */
static int oklab2rgb(float L,float a,float b, int *R,int *G,int *B){
    float l_=L+0.3963377774f*a+0.2158037573f*b;
    float m_=L-0.1055613458f*a-0.0638541728f*b;
    float s_=L-0.0894841775f*a-1.2914855480f*b;
    float l=l_*l_*l_, m=m_*m_*m_, s=s_*s_*s_;
    float lin[3];
    lin[0]= 4.0767416621f*l-3.3077115913f*m+0.2309699292f*s;
    lin[1]=-1.2684380046f*l+2.6097574011f*m-0.3413193965f*s;
    lin[2]=-0.0041960863f*l-0.7034186147f*m+1.7076147010f*s;
    int in = 1; int *o[3]={R,G,B};
    for(int i=0;i<3;i++){
        float x=lin[i];
        if(x<-0.001f||x>1.001f) in=0;
        if(x<0)x=0; if(x>1)x=1;
        float v = x<=0.0031308f ? 12.92f*x : 1.055f*powf(x,1.0f/2.4f)-0.055f;
        *o[i] = (int)(v*255.0f+0.5f);
    }
    return in;
}
static float wcag_lum(int R,int G,int B){ return 0.2126f*g_lin_lut[R]+0.7152f*g_lin_lut[G]+0.0722f*g_lin_lut[B]; }

/* Read a 148x148 BGR24 BMP (as written by art_make_all) into a caller-owned
 * CBMP*CBMP*4 BGRA buffer. Thread-safe (no globals) so the art worker / prewarm
 * thread can compute accents off the UI thread. Returns 0 on success. */
static int read_cover_bmp(const char *path, uint8_t *buf)
{
    FILE *f = fopen(path, "rb"); if(!f) return -1;
    uint8_t hdr[54];
    if(fread(hdr,1,54,f)!=54){ fclose(f); return -1; }
    uint32_t off = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((uint32_t)hdr[13]<<24);
    int w = hdr[18]|(hdr[19]<<8), h = hdr[22]|(hdr[23]<<8);
    int bpp = hdr[28]|(hdr[29]<<8);
    uint32_t comp = hdr[30]|(hdr[31]<<8)|(hdr[32]<<16)|((uint32_t)hdr[33]<<24);
    if(w!=CBMP || h!=CBMP || bpp!=24 || comp!=0){ fclose(f); return -1; }   /* only the tightly-packed 24bpp BMPs we write */
    uint8_t row[CBMP*3];
    if(fseek(f, off, SEEK_SET)!=0){ fclose(f); return -1; }
    for(int yy=0; yy<CBMP; yy++){
        if(fread(row,1,CBMP*3,f)!=(size_t)(CBMP*3)){ fclose(f); return -1; }
        uint8_t *d = buf + (CBMP-1-yy)*CBMP*4;       /* bottom-up -> top-down */
        for(int x=0;x<CBMP;x++){
            d[x*4+0]=row[x*3+0]; d[x*4+1]=row[x*3+1];
            d[x*4+2]=row[x*3+2]; d[x*4+3]=0xFF;
        }
    }
    fclose(f);
    return 0;
}

/* Core accent computation from a 148x148 BGRA buffer. Reentrant: heap scratch,
 * reads only the passed buffer + the read-only g_lin_lut (seeded once at startup),
 * so the UI thread, art worker, and prewarm thread can all call it concurrently.
 * Returns 1 and fills *out, or 0 if no usable colour. */
static int accent_from_buf(const uint8_t *cbuf, uint32_t *out_rgb)
{
    if(!cbuf) return 0;
    float *pL=malloc(2025*sizeof(float)), *pA=malloc(2025*sizeof(float)),
          *pB=malloc(2025*sizeof(float)), *pW=malloc(2025*sizeof(float));
    if(!pL||!pA||!pB||!pW){ free(pL);free(pA);free(pB);free(pW); return 0; }
    int rc = 0;
    int n=0; float wsum=0,csum=0;
    for(int y=8;y<140;y+=3){
        const uint8_t *rp=cbuf+(size_t)y*CBMP*4;
        for(int x=8;x<140 && n<2025;x+=3){
            const uint8_t *p=rp+x*4; int B=p[0],G=p[1],R=p[2];   /* B,G,R,A */
            float L,a,b; rgb2oklab(R,G,B,&L,&a,&b);
            float C=sqrtf(a*a+b*b), w=1.0f;
            if(C<0.025f||L<0.08f||L>0.96f) w=0.35f;             /* de-weight near grey/black/white */
            pL[n]=L;pA[n]=a;pB[n]=b;pW[n]=w; wsum+=w; csum+=C*w; n++;
        }
    }
    if(n<32) goto done;
    if(wsum>0 && csum/wsum < 0.025f){ *out_rgb=0xAEB4BE; rc=1; goto done; }   /* greyscale -> soft cool neutral */

    /* k-means (k=8), farthest-point seeding (deterministic) */
    { const int K=8; float cL[8],cA[8],cB[8],pop[8];
    { int bi=0; float bc=-1; for(int i=0;i<n;i++){ float c=pA[i]*pA[i]+pB[i]*pB[i]; if(c>bc){bc=c;bi=i;} }
      cL[0]=pL[bi];cA[0]=pA[bi];cB[0]=pB[bi]; }
    for(int k=1;k<K;k++){ int bi=0; float bd=-1;
        for(int i=0;i<n;i++){ float md=1e9f;
            for(int j=0;j<k;j++){ float dL=pL[i]-cL[j],da=pA[i]-cA[j],db=pB[i]-cB[j]; float d=1.25f*dL*dL+da*da+db*db; if(d<md)md=d; }
            if(md>bd){bd=md;bi=i;} }
        cL[k]=pL[bi];cA[k]=pA[bi];cB[k]=pB[bi]; }
    for(int it=0;it<10;it++){
        float sL[8]={0},sA[8]={0},sB[8]={0},sw[8]={0};
        for(int i=0;i<n;i++){ int bj=0; float bd=1e9f;
            for(int j=0;j<K;j++){ float dL=pL[i]-cL[j],da=pA[i]-cA[j],db=pB[i]-cB[j]; float d=1.25f*dL*dL+da*da+db*db; if(d<bd){bd=d;bj=j;} }
            float w=pW[i]; sL[bj]+=pL[i]*w; sA[bj]+=pA[i]*w; sB[bj]+=pB[i]*w; sw[bj]+=w; }
        for(int j=0;j<K;j++){ if(sw[j]>0){cL[j]=sL[j]/sw[j];cA[j]=sA[j]/sw[j];cB[j]=sB[j]/sw[j];} pop[j]=sw[j]; }
    }
    /* score clusters against a Vibrant-on-dark target (L 0.72, C 0.16) */
    float best=-1; int bk=-1;
    for(int j=0;j<K;j++){
        float L=cL[j],a=cA[j],b=cB[j]; float C=sqrtf(a*a+b*b);
        float pf = wsum>0 ? pop[j]/wsum : 0;
        if(pf<0.008f||C<0.035f||L<0.18f||L>0.92f) continue;
        float popS=sqrtf(pf);
        float lS=1.0f-a_clamp01(fabsf(L-0.72f)/0.32f);
        float cS=1.0f-a_clamp01(fabsf(C-0.16f)/0.14f);
        float vivid=a_smooth(0.07f,0.18f,C);
        float darkP=a_smooth(0.18f,0.38f,L);
        float lightP=1.0f-a_smooth(0.88f,0.98f,L);
        float sc=(0.42f*popS+0.28f*cS+0.22f*lS+0.08f*vivid)*darkP*lightP;
        if(pf<0.02f && C<0.12f) sc*=0.5f;                  /* stability: distrust small low-chroma specks */
        if(sc>best){best=sc;bk=j;}
    }
    if(bk<0){ *out_rgb=0xAEB4BE; rc=1; goto done; }    /* no usable colour -> neutral */

    float L=cL[bk],a=cA[bk],b=cB[bk]; float C=sqrtf(a*a+b*b), H=atan2f(b,a);
    /* tone-map (preserve hue) */
    if(L<0.58f)L=0.58f; else if(L>0.84f)L=0.84f;
    if(C<0.075f)C=0.075f; else if(C>0.22f)C=0.22f;
    if(C>0.18f&&L>0.72f) C*=0.92f;
    if(C<0.10f) C=C+(0.10f-C)*0.35f;
    a=C*cosf(H); b=C*sinf(H);
    /* WCAG contrast lift vs #1C1C1E (>=4.5:1 for glyphs); shrink C only if out of gamut */
    float Ybg=wcag_lum(0x1C,0x1C,0x1E); int R=0,G=0,Bo=0;
    for(int iter=0;iter<48;iter++){
        a=C*cosf(H); b=C*sinf(H);
        int in=oklab2rgb(L,a,b,&R,&G,&Bo);
        if(!in && C>0.04f){ C*=0.94f; continue; }
        float Y=wcag_lum(R,G,Bo); float hi=Y>Ybg?Y:Ybg, lo=Y>Ybg?Ybg:Y;
        if((hi+0.05f)/(lo+0.05f) >= 4.5f || L>=0.88f) break;
        L+=0.015f;
    }
    a=C*cosf(H); b=C*sinf(H); oklab2rgb(L,a,b,&R,&G,&Bo);
    *out_rgb = ((uint32_t)R<<16)|((uint32_t)G<<8)|(uint32_t)Bo;
    rc=1;
    }
done:
    free(pL);free(pA);free(pB);free(pW);
    return rc;
}
#pragma GCC pop_options

static int g_accent_gray = 0;   /* currently showing the idle/no-track neutral */
/* Accent config, mirrored from settings (read on the UI thread + art workers).
 * mode 0 = dynamic (album-art OKLab); 1 = static (fixed user colour: no OKLab
 * compute, no SONG.ACCENT read/write). g_accent_color is 0xRRGGBB. */
static _Atomic int g_accent_mode = 0;   /* read by art worker/prewarm threads -> atomic */
static uint32_t    g_accent_color = 0xF23260;   /* UI-thread only */
static uint32_t    g_static_last  = 0xFFFFFFFFu; /* last static accent actually applied (skip redundant re-applies) */
int ui_accent_is_static(void){ return g_accent_mode == 1; }   /* worker/prewarm gate */

static void update_accent_seed(const track_state_t *st)
{
    if(g_accent_mode == 1){            /* static: fixed colour; repaint only when it actually changed -
                                        * the 332x332 arc invalidation is ~85% of the screen every tick */
        if(g_static_last != g_accent_color){
            accent = theme_accent_from_rgb(g_accent_color);
            apply_accent();
            g_static_last = g_accent_color;
        }
        return;
    }
    if(!st || !st->have_track){
        /* no track (idle, or the ~1s while playback is starting): show a neutral light
         * grey instead of a stale/weird accent from the last track. Apply once. */
        if(!g_accent_gray){
            accent = TC(ACCENT_IDLE);
            g_accent_gray = 1;
            last_seed_a[0] = 0; last_seed_b[0] = 0;
            apply_accent();
        }
        return;
    }
    g_accent_gray = 0;
    const char *seed_a = st->album[0] ? st->album : (st->title_ready ? st->title : "");
    const char *seed_b = st->artist;

    if(strcmp(last_seed_a, seed_a) == 0 && strcmp(last_seed_b, seed_b) == 0) {
        return;
    }

    copy_cstr(last_seed_a, sizeof(last_seed_a), seed_a);
    copy_cstr(last_seed_b, sizeof(last_seed_b), seed_b);
    /* Use the cached per-song album-art accent if we have it (instant, consistent). If not,
     * DON'T flash an arbitrary text-hash colour (that looked "random" between tracks) - keep
     * the current accent until apply_art() applies the real OKLab colour when the cover
     * finishes decoding. So the accent only ever shows a real, album-derived colour. */
    int rgb = mdb_song_accent(st->path);
    if(rgb){ accent = theme_accent_from_rgb(rgb); apply_accent(); }
}

/* set by clear_art_state / apply_art (main thread); main.c re-pushes the
 * art-dependent surfaces (Home pill / backdrop / Options thumb) when set. */
static int g_art_applied = 0;

/* Only current small artwork may act as the NP placeholder. */
static int g_np_small_valid, g_np_cover_loading;

/* ---- Now Playing: two states -------------------------------------------------------------------
 * The cover art fills the round screen and turns slowly while music plays. A tap shows the controls (g_np_ov)
 * over the same art dimmed (yopo 2026-10-20: sharp, not blurred), which keeps turning; a tap on an empty spot, or NP_OV_IDLE_MS
 * without a touch, hides them again. The picture is the shared sharp 360 px cover (sharpart.c's sharp_cover_*);
 * until it lands, the small 148 px cover is enlarged in its place. diskOS turns it (and the side pages' blurred copy)
 * (sharp_rotate, ~6 ms a frame) into opaque images, with sharp_spin_start's spin (27 s a turn, 30 frames a
 * second). */
#define NP_W              360
#define NP_OV_IDLE_MS     5000
#define NP_OV_FADE_MS     220
#define NP_BLUR_TINT      150     /* how far the blurred art leans to the theme's image tint (of 255), as the old backdrop */
#define NP_DIM_OPA        NP_BLUR_TINT   /* the sharp art under the controls leans to the image tint as far */
#define NP_BS             (NP_W / 4)   /* the blur works on a quarter-size copy */
#define NP_LOG_MS         10000   /* the frame cost goes to diskos_boot.log ("NP spin") this often while turning */
static lv_obj_t *g_np_art, *g_np_dim;           /* the turning art, and its dimming under the controls */
static lv_obj_t *g_np_noart, *g_np_noart_title; /* no cover: a dark disc with the song title */
static lv_obj_t *g_np_tap;                       /* full-screen tap target under the controls */
static lv_obj_t *g_np_ov;                        /* the controls and the song's info */
static uint32_t *g_np_up;                        /* the small cover enlarged to NP_W, while there is no sharp one */
static uint32_t *g_np_blur;                      /* the blurred, dimmed copy of the current picture */
static uint32_t *g_np_out, *g_np_outb;           /* what g_np_art and the side pages' backdrops show */
static lv_image_dsc_t g_np_out_dsc, g_np_outb_dsc;
static const uint32_t *g_np_src;                 /* the unturned picture; NULL = no art */
static int32_t g_np_angle;                       /* 0.1 degree */
static int g_np_spinning;
static lv_timer_t *g_np_ov_tm;
static int g_np_tap_px, g_np_tap_py;
static uint32_t g_np_log_t0, g_np_turn_us, g_np_turn_n, g_np_draw_us, g_np_draw_n, g_np_draw_t0;

static uint32_t np_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u); }
static inline uint32_t np_lerp(uint32_t a, uint32_t b, uint32_t w){   /* per channel a..b, w in 0..256 */
    uint32_t rb = (((a & 0x00FF00FFu) * (256 - w) + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * (256 - w) + ((b >> 8) & 0x00FF00FFu) * w)) & 0xFF00FF00u;
    return rb | ag;
}
/* Sample positions for growing an n-pixel line to NP_W pixels (bilinear, pixel centres lined up). */
static void np_grow_map(int n, int16_t *i0, int16_t *i1, uint8_t *f)
{
    for(int d = 0; d < NP_W; d++){
        int32_t v = (int32_t)(((int64_t)(2 * d + 1) * n * 32768) / NP_W) - 32768;   /* 16.16 */
        if(v < 0) v = 0;
        i0[d] = (int16_t)(v >> 16);
        i1[d] = (int16_t)(i0[d] + 1 < n ? i0[d] + 1 : n - 1);
        f[d] = (uint8_t)((v >> 8) & 0xFF);
    }
}
/* The small cover (sw x sw BGRA bytes) grown to NP_W x NP_W. */
__attribute__((optimize("O2")))
static void np_enlarge(const uint8_t *src, int sw, uint32_t *dst)
{
    static int16_t i0[NP_W], i1[NP_W]; static uint8_t f[NP_W];
    np_grow_map(sw, i0, i1, f);
    for(int y = 0; y < NP_W; y++){
        const uint8_t *r0 = src + i0[y] * sw * 4, *r1 = src + i1[y] * sw * 4;
        for(int x = 0; x < NP_W; x++){
            const uint8_t *a = r0 + i0[x] * 4, *b = r0 + i1[x] * 4, *c = r1 + i0[x] * 4, *d = r1 + i1[x] * 4;
            uint32_t pa = a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16, pb = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16;
            uint32_t pc = c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16, pd = d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16;
            dst[y * NP_W + x] = np_lerp(np_lerp(pa, pb, f[x]), np_lerp(pc, pd, f[x]), f[y]) | 0xFF000000u;
        }
    }
}
/* One box-blur pass (radius 4) over every row (horiz) or column of an NP_BS x NP_BS channel. */
static void np_box(const uint8_t *in, uint8_t *out, int horiz)
{
    const int r = 4, n = NP_BS, w = 2 * r + 1;
    for(int line = 0; line < n; line++){
        int base = horiz ? line * n : line, step = horiz ? 1 : n;
        int sum = 0;
        for(int k = -r; k <= r; k++) sum += in[base + (k < 0 ? 0 : k) * step];
        for(int i = 0; i < n; i++){
            out[base + i * step] = (uint8_t)(sum / w);
            int add = i + r + 1, sub = i - r;
            if(add > n - 1) add = n - 1;
            if(sub < 0) sub = 0;
            sum += in[base + add * step] - in[base + sub * step];
        }
    }
}
/* The soft copy behind the side pages (Lyrics, Options): shrink 4x, blur that (three box passes each way, close to a gaussian),
 * grow it back and lean it towards the theme's image tint so the text over it reads. */
__attribute__((optimize("O2")))
static void np_make_blur(const uint32_t *src, uint32_t *dst)
{
    static uint8_t ch[3][NP_BS * NP_BS], tmp[NP_BS * NP_BS];
    static int16_t i0[NP_W], i1[NP_W]; static uint8_t f[NP_W];
    for(int by = 0; by < NP_BS; by++)
        for(int bx = 0; bx < NP_BS; bx++){
            uint32_t s0 = 0, s1 = 0, s2 = 0;
            for(int y = 0; y < 4; y++){
                const uint32_t *p = src + (by * 4 + y) * NP_W + bx * 4;
                for(int x = 0; x < 4; x++){ s0 += p[x] & 0xFF; s1 += (p[x] >> 8) & 0xFF; s2 += (p[x] >> 16) & 0xFF; }
            }
            int i = by * NP_BS + bx;
            ch[0][i] = (uint8_t)(s0 >> 4); ch[1][i] = (uint8_t)(s1 >> 4); ch[2][i] = (uint8_t)(s2 >> 4);
        }
    for(int k = 0; k < 3; k++)
        for(int pass = 0; pass < 3; pass++){ np_box(ch[k], tmp, 1); np_box(tmp, ch[k], 0); }
    uint32_t tint = lv_color_to_u32(TC(IMAGE_TINT));
    uint32_t tk[3];
    for(int k = 0; k < 3; k++) tk[k] = ((tint >> (8 * k)) & 0xFF) * NP_BLUR_TINT;
    const uint32_t keep = 255 - NP_BLUR_TINT;
    np_grow_map(NP_BS, i0, i1, f);
    for(int y = 0; y < NP_W; y++){
        int r0 = i0[y] * NP_BS, r1 = i1[y] * NP_BS;
        uint32_t fy = f[y];
        for(int x = 0; x < NP_W; x++){
            uint32_t fx = f[x], px = 0xFF000000u;
            for(int k = 0; k < 3; k++){
                const uint8_t *c = ch[k];
                uint32_t top = c[r0 + i0[x]] * (256 - fx) + c[r0 + i1[x]] * fx;
                uint32_t bot = c[r1 + i0[x]] * (256 - fx) + c[r1 + i1[x]] * fx;
                uint32_t v = (top * (256 - fy) + bot * fy) >> 16;
                px |= ((v * keep + tk[k]) / 255) << (8 * k);
            }
            dst[y * NP_W + x] = px;
        }
    }
}
static void np_dsc(lv_image_dsc_t *d, uint32_t *px)
{
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_XRGB8888;   /* opaque: a plain copy for LVGL, no per-pixel blend */
    d->header.w = d->header.h = NP_W;
    d->header.stride = NP_W * 4;
    d->data = (const uint8_t *)px;
    d->data_size = NP_W * NP_W * 4;
}
/* The picture buffers, allocated once; 0 when memory is short (Now Playing then shows the no-art disc). */
static int np_bufs(void)
{
    const size_t n = (size_t)NP_W * NP_W;
    if(!g_np_out){ g_np_out = calloc(n, 4); if(g_np_out) np_dsc(&g_np_out_dsc, g_np_out); }   /* zeroed: outside the circle stays black */
    if(!g_np_outb){ g_np_outb = calloc(n, 4); if(g_np_outb) np_dsc(&g_np_outb_dsc, g_np_outb); }
    if(!g_np_blur) g_np_blur = malloc(n * 4);
    return g_np_out && g_np_outb && g_np_blur;
}
static void np_vis(lv_obj_t *o, int on)
{
    if(!o || on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static void np_turn(lv_obj_t *img, const uint32_t *src, uint32_t *out, lv_image_dsc_t *dsc)
{
    uint32_t t = np_us();
    sharp_rotate(src, out, g_np_angle);
    g_np_turn_us += np_us() - t; g_np_turn_n++;
    lv_image_cache_drop(dsc);
    if(img) lv_obj_invalidate(img);
}
/* Now Playing's art behind other pages: Options the sharp one (g_np_out) under a dimming like the controls', the rest
 * (Lyrics, and as a trial Library's top page and Up Next) the blurred copy (g_np_outb) (yopo 2026-10-20). Each page has
 * its own image, which np_draw turns while that page is the screen. */
#define NP_BD_MAX 4
#define NP_BD_BLUR  1
#define NP_BD_SHARP 2
static struct { lv_obj_t *img, *dim; int scr, kind, off; } g_np_bd[NP_BD_MAX];   /* off: the page hides it (ui_np_backdrop_on) */
static void np_bd_bind(int i)
{
    int art = g_np_src != NULL && !g_np_bd[i].off;
    const lv_image_dsc_t *d = g_np_bd[i].kind == NP_BD_SHARP ? &g_np_out_dsc : &g_np_outb_dsc;
    if(art && lv_image_get_src(g_np_bd[i].img) != d) lv_image_set_src(g_np_bd[i].img, d);
    np_vis(g_np_bd[i].img, art);   /* no art: the page's own canvas */
    if(g_np_bd[i].dim) np_vis(g_np_bd[i].dim, art);
}
/* Which kinds of backdrop the current screen shows (NP_BD_* bits; 0 without art). */
static int np_bd_shown(void)
{
    if(!g_np_src) return 0;
    int cur = screen_current(), k = 0;
    for(int i = 0; i < NP_BD_MAX; i++) if(g_np_bd[i].img && !g_np_bd[i].off && g_np_bd[i].scr == cur) k |= g_np_bd[i].kind;
    return k;
}
static void np_bd_invalidate(void)
{
    int cur = screen_current();
    for(int i = 0; i < NP_BD_MAX; i++) if(g_np_bd[i].img && g_np_bd[i].scr == cur) lv_obj_invalidate(g_np_bd[i].img);
}
static void np_bd_deleted(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    for(int i = 0; i < NP_BD_MAX; i++) if(g_np_bd[i].img == o) g_np_bd[i].img = g_np_bd[i].dim = NULL;
}
/* Turns the pictures that show to the current angle: the sharp one for Now Playing or the Options backdrop, the
 * blurred one for the other backdrops. */
static void np_draw(void)
{
    if(!g_np_src || !g_np_art) return;
    int np = screen_current() == SCR_NOWPLAYING, bd = np_bd_shown();
    int art = np && !lv_obj_has_flag(g_np_art, LV_OBJ_FLAG_HIDDEN);
    if(art || (bd & NP_BD_SHARP)) np_turn(art ? g_np_art : NULL, g_np_src, g_np_out, &g_np_out_dsc);
    if(bd & NP_BD_BLUR) np_turn(NULL, g_np_blur, g_np_outb, &g_np_outb_dsc);
    if(bd) np_bd_invalidate();
}
/* A side page's backdrop: the bottom children of its root (the picture, and for Options its dimming above it). */
void ui_np_backdrop(lv_obj_t *root, int scr)
{
    int i = 0;
    while(i < NP_BD_MAX && g_np_bd[i].img) i++;
    if(i == NP_BD_MAX) return;
    g_np_bd[i].scr = scr;
    g_np_bd[i].kind = scr == SCR_NPHUB ? NP_BD_SHARP : NP_BD_BLUR;
    g_np_bd[i].dim = NULL; g_np_bd[i].off = 0;
    if(g_np_bd[i].kind == NP_BD_SHARP){
        lv_obj_t *d = lv_obj_create(root);
        lv_obj_remove_style_all(d);
        lv_obj_set_pos(d, 0, 0);
        lv_obj_set_size(d, NP_W, NP_W);
        lv_obj_set_style_bg_color(d, TC(IMAGE_TINT), 0);
        lv_obj_set_style_bg_opa(d, NP_DIM_OPA, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_background(d);
        g_np_bd[i].dim = d;
    }
    lv_obj_t *o = lv_image_create(root);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, NP_W, NP_W);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(o);
    lv_obj_add_event_cb(o, np_bd_deleted, LV_EVENT_DELETE, NULL);
    g_np_bd[i].img = o;
    np_bd_bind(i);
}
/* Entering a page with a backdrop, or the picture changed: bind every backdrop and turn its art to the current angle. */
void ui_np_backdrop_sync(void)
{
    for(int i = 0; i < NP_BD_MAX; i++) if(g_np_bd[i].img) np_bd_bind(i);
    if(np_bd_shown()) np_draw();
}
/* A page shows (on) or hides its backdrop, e.g. Library only on its top page. */
void ui_np_backdrop_on(int scr, int on)
{
    for(int i = 0; i < NP_BD_MAX; i++)
        if(g_np_bd[i].img && g_np_bd[i].scr == scr && g_np_bd[i].off == !!on){ g_np_bd[i].off = !on; np_bd_bind(i); }
    if(on && np_bd_shown()) np_draw();
}
/* The screens on which the art turns: Now Playing and its side pages. */
int ui_np_turns_on(int scr)
{
    if(scr == SCR_NOWPLAYING) return 1;
    for(int i = 0; i < NP_BD_MAX; i++) if(g_np_bd[i].img && !g_np_bd[i].off && g_np_bd[i].scr == scr) return 1;
    return 0;   /* e.g. Library below its top page: nothing there turns */
}
/* Keeps Now Playing's art still on the screen while a page holding it slides: the picture and its dimming move
 * against the page, so only the page's own items move (yopo 2026-10-20). Runs on every step of a page slide. */
void ui_np_art_follow(lv_obj_t *root)
{
    int32_t x = -lv_obj_get_style_x(root, 0), y = -lv_obj_get_style_y(root, 0);
    for(int i = -1; i < NP_BD_MAX; i++){
        lv_obj_t *img = i < 0 ? g_np_art : g_np_bd[i].img, *dim = i < 0 ? g_np_dim : g_np_bd[i].dim;
        if(!img || lv_obj_get_parent(img) != root) continue;
        lv_obj_set_pos(img, x, y);
        if(dim) lv_obj_set_pos(dim, x, y);
    }
}
/* Shows what fits the state (stopping a fade): the art alone, the art dimmed under the controls, or the no-art disc
 * (with the title while the controls are hidden). */
static void np_show_now(void)
{
    if(!g_np_ov) return;
    lv_anim_delete(g_np_ov, NULL);
    lv_anim_delete(g_np_dim, NULL);
    lv_obj_set_style_opa(g_np_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(g_np_dim, LV_OPA_COVER, 0);
    int art = g_np_src != NULL, ov = g_np_ov_on;
    np_vis(g_np_art, art);
    np_vis(g_np_dim, art && ov);
    np_vis(g_np_noart, !art);
    np_vis(g_np_noart_title, !art && !ov);
    np_vis(g_np_ov, ov);
    np_draw();
}
static void np_ov_faded(lv_anim_t *a){ (void)a; np_show_now(); }
/* Shows (on) or hides the controls. Animated: the art keeps turning while the dimming and the controls fade, then
 * np_show_now settles the end state. */
static void np_ov_set(int on, int animate)
{
    if(!g_np_ov) { g_np_ov_on = 0; return; }
    if(on == g_np_ov_on){ if(!animate) np_show_now(); return; }
    g_np_ov_on = on;
    if(g_np_ov_tm){
        if(on){ g_np_ov_touch = lv_tick_get(); lv_timer_reset(g_np_ov_tm); lv_timer_resume(g_np_ov_tm); }
        else lv_timer_pause(g_np_ov_tm);
    }
    if(!animate){ np_show_now(); return; }
    int art = g_np_src != NULL;
    lv_opa_t from = on ? LV_OPA_TRANSP : LV_OPA_COVER, to = on ? LV_OPA_COVER : LV_OPA_TRANSP;
    np_vis(g_np_noart_title, 0);
    np_vis(g_np_art, art);
    np_vis(g_np_dim, art);
    np_vis(g_np_ov, 1);
    np_draw();
    if(art){ lv_obj_set_style_opa(g_np_dim, from, 0); anim_fade(g_np_dim, from, to, NP_OV_FADE_MS, NULL); }
    lv_obj_set_style_opa(g_np_ov, from, 0);
    anim_fade(g_np_ov, from, to, NP_OV_FADE_MS, np_ov_faded);
}
/* The controls hide after NP_OV_IDLE_MS without a touch (a finger on the ring keeps them). */
static void np_ov_idle(lv_timer_t *t)
{
    (void)t;
    if(g_scrubbing || g_seek_cand){ g_np_ov_touch = lv_tick_get(); return; }
    if(g_np_ov_on && lv_tick_elaps(g_np_ov_touch) >= NP_OV_IDLE_MS) np_ov_set(0, 1);
}
/* A still tap shows or hides the controls; a slide is a gesture for main.c (section pull, options swipe). A tap on
 * the ring's band while the controls show is left alone: that is where a seek starts. */
static void np_tap_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p = { 0, 0 };
    lv_indev_t *id = lv_indev_active();
    if(id) lv_indev_get_point(id, &p);
    if(code == LV_EVENT_PRESSED){ g_np_tap_px = p.x; g_np_tap_py = p.y; return; }
    if(code != LV_EVENT_CLICKED) return;
    int dx = p.x - g_np_tap_px, dy = p.y - g_np_tap_py;
    if((dx < 0 ? -dx : dx) > 18 || (dy < 0 ? -dy : dy) > 18) return;   /* was a swipe (cst816t wobble tolerance) */
    if(g_np_ov_on && seek_on_band(g_np_tap_px, g_np_tap_py, 34)) return;
    np_ov_set(!g_np_ov_on, 1);
}
static void np_noart_text(void)
{
    if(g_np_noart_title) set_label_text_changed(g_np_noart_title, lv_label_get_text(title));
}
static void np_log(void)
{
    uint32_t el = lv_tick_elaps(g_np_log_t0);
    if(el < NP_LOG_MS) return;
    if(g_np_turn_n)
        fprintf(stderr, "NP spin: %u frames in %u ms, turn %u us, draw %u us (averages)\n", (unsigned)g_np_draw_n, (unsigned)el,
                (unsigned)(g_np_turn_us / g_np_turn_n), (unsigned)(g_np_draw_n ? g_np_draw_us / g_np_draw_n : 0));
    g_np_log_t0 = lv_tick_get();
    g_np_turn_us = g_np_turn_n = g_np_draw_us = g_np_draw_n = 0;
}
/* LVGL's own render time per frame (refresh start to rendered and flushed), counted while the art turns. */
static void np_render_cb(lv_event_t *e)
{
    if(!g_np_spinning) return;
    if(lv_event_get_code(e) == LV_EVENT_REFR_START) g_np_draw_t0 = np_us();
    else if(g_np_draw_t0){ g_np_draw_us += np_us() - g_np_draw_t0; g_np_draw_n++; g_np_draw_t0 = 0; }
}
static void np_spin_cb(void *var, int32_t v)
{
    static uint32_t last;
    (void)var;
    if(!sharp_spin_due(&last)) return;
    g_np_angle = v % 3600;
    np_draw();
    np_log();
}
/* main.c, every loop: the art turns only while playing, on screen and lit; paused it stays at its angle. */
void ui_np_spin(int want)
{
    want = want && g_np_src && g_np_art;
    if(want == g_np_spinning) return;
    g_np_spinning = want;
    if(want){
        g_np_log_t0 = lv_tick_get();
        g_np_turn_us = g_np_turn_n = g_np_draw_us = g_np_draw_n = g_np_draw_t0 = 0;
        sharp_spin_start(g_np_art, g_np_angle, np_spin_cb);
    } else lv_anim_delete(g_np_art, np_spin_cb);   /* paused it stays at its angle */
}

/* The picture changed (a cover arrived or went, the track or the theme changed): use the best one there is. */
void ui_np_cover_changed(void)
{
    if(!g_np_art) return;
    const lv_image_dsc_t *sharp = sharp_cover_get();
    const uint32_t *src = NULL;
    if(sharp && sharp->header.w == NP_W && sharp->header.h == NP_W) src = (const uint32_t *)sharp->data;
    else if(g_np_small_valid && coverdsc_valid){
        if(!g_np_up) g_np_up = malloc((size_t)NP_W * NP_W * 4);
        if(g_np_up){ np_enlarge(g_coverdsc.data, CBMP, g_np_up); src = g_np_up; }
    }
    if(src && !np_bufs()) src = NULL;
    g_np_src = src;
    if(src){
        np_make_blur(src, g_np_blur);
        if(lv_image_get_src(g_np_art) != &g_np_out_dsc) lv_image_set_src(g_np_art, &g_np_out_dsc);
    }
    if(!src) ui_np_spin(0);
    np_show_now();
    ui_np_backdrop_sync();
}

static void np_sharp_demand(void)
{
    if (screen_current() == SCR_NOWPLAYING) sharp_cover_request();
}

/* Drop all album-art state to the no-art fallback (used for no-track, empty
 * path, and failed decode) so stale art isn't republished to Home. */
static void clear_art_state(void)
{
    cover_valid = thumb_valid = backdrop_valid = 0;
    coverdsc_valid = 0;
    last_art_key[0] = '\0';
    g_np_small_valid = g_np_cover_loading = 0;
    ui_np_cover_changed();
    g_art_applied = 1;   /* re-push: clears Home/Saver since validity flags are now 0 */
}

/* ---- album-art worker ---------------------------------------------------
 * art_make_all() forks the artwork helper (hundreds of ms) - running it inline stalled
 * touch/render on every cross-album track change. It now runs on a detached
 * worker thread that ONLY writes the /tmp BMPs (touches no LVGL / g_coverbuf
 * state); the main thread applies the result in ui_art_poll() (LVGL is not
 * thread-safe). A new decode ALWAYS targets the non-displayed buffer
 * (displayed_idx ^ 1), so an in-flight decode can never overwrite the /tmp BMP the
 * shown art (Home pill / backdrop) may still be rendering from. */
typedef struct {
    char track[256];
    int  idx;
    char out[40], tout[40], bout[40];
    char art_key[420];       /* album+dir reuse/staleness key (see last_art_key) */
    int  is_clear;           /* "no track" - handled on the main thread; worker skips */
    char artist[160], album[160], title[IPC_TITLE_CAPACITY];   /* for the online-art fallback (netart.c) */
    int  online;             /* Online Album Art was on when this was requested (read on the main thread) */
    int  online_only;        /* committed-title retry after a local miss */
    int  from_net;           /* (result) the picture came from Online Album Art */
} art_req_t;

/* Serializes the two artwork decoders (the live NP worker below + the background cover prewarm) so
 * they never run two ~19MB image pipelines at once (OOM/stall). Held only around the decoder call. */
static pthread_mutex_t g_decode_mu = PTHREAD_MUTEX_INITIALIZER;
/* Shared by ALL artwork decoders (live NP, prewarm, AND the sharp cover) so at most one runs at
 * once - two ~19MB pipelines together risk OOM. sharpart.c wraps its decode with these. */
void ui_decode_lock(void){ pthread_mutex_lock(&g_decode_mu); }
void ui_decode_unlock(void){ pthread_mutex_unlock(&g_decode_mu); }

/* Album-cover prewarm work queue. The MAIN thread enqueues representative track paths (built with the
 * in-memory album/track caches, which are NOT thread-safe); the prewarm worker only pops + decodes, so
 * no mdb in-memory cache is ever touched off the UI thread. Bounded ring: enqueue fails (0) when full,
 * and the enumerator then pauses rather than dropping albums. */
#define PWQ_MAX 48   /* the worker drains at ~2/s; the seed self-throttles on a full queue (resumes the same album), so a small ring is plenty */
static char            pwq[PWQ_MAX][512];
static int             pwq_head, pwq_tail;
static pthread_mutex_t pwq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pwq_cv;
static pthread_once_t  pwq_once = PTHREAD_ONCE_INIT;
static _Atomic int g_prewarm_mode = 0;   /* cfg mirror: settings (main) writes, worker reads */
static unsigned pwq_mode_seq;                     /* guarded by pwq_mu */
static void pwq_init(void){
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&pwq_cv, &attr);
    pthread_condattr_destroy(&attr);
}
void ui_set_prewarm_mode(int m){                 /* 0=covers only 1=idle 2=idle&charging */
    m = (m<0)?0:(m>2?2:m);
    pthread_once(&pwq_once, pwq_init);
    pthread_mutex_lock(&pwq_mu);
    if(g_prewarm_mode != m){
        g_prewarm_mode = m;
        pwq_mode_seq++;
        pthread_cond_signal(&pwq_cv);
    }
    pthread_mutex_unlock(&pwq_mu);
}
int ui_prewarm_enqueue(const char *path){          /* MAIN thread; returns 1 if queued, 0 if full/dup */
    if(!path || !path[0]) return 1;
    int ok = 0;
    pthread_once(&pwq_once, pwq_init);
    pthread_mutex_lock(&pwq_mu);
    int nt = (pwq_tail+1) % PWQ_MAX;
    if(nt != pwq_head){
        int dup = 0;                                /* cheap dedup: same path already pending */
        for(int i=pwq_head; i!=pwq_tail; i=(i+1)%PWQ_MAX) if(!strcmp(pwq[i], path)){ dup=1; break; }
        if(dup) ok = 1;
        else { copy_cstr(pwq[pwq_tail], sizeof pwq[pwq_tail], path); pwq_tail = nt; ok = 1; pthread_cond_signal(&pwq_cv); }
    }
    pthread_mutex_unlock(&pwq_mu);
    return ok;
}
static int pwq_pop(char *out, int cap){            /* worker thread; 1 if an item was dequeued */
    int got = 0;
    pthread_mutex_lock(&pwq_mu);
    if(pwq_head != pwq_tail){ copy_cstr(out, cap, pwq[pwq_head]); pwq_head = (pwq_head+1)%PWQ_MAX; got = 1; }
    pthread_mutex_unlock(&pwq_mu);
    return got;
}
static void pw_sleep(int secs){                    /* idle sleep, but wake early the moment cover work is queued */
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += secs;
    pthread_once(&pwq_once, pwq_init);
    pthread_mutex_lock(&pwq_mu);
    unsigned seq = pwq_mode_seq;
    while(pwq_head == pwq_tail && seq == pwq_mode_seq){
        /* Mode 0 has no sweep window to recheck. Active modes keep their delay. */
        if(g_prewarm_mode == 0) pthread_cond_wait(&pwq_cv, &pwq_mu);
        else if(pthread_cond_timedwait(&pwq_cv, &pwq_mu, &until) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&pwq_mu);
}

static pthread_mutex_t g_art_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_art_title_pending;   /* main thread: local work started before title commitment */
static uint64_t g_art_token;
static unsigned g_art_generation;
static art_req_t g_art_target;      /* latest requested decode          (guarded) */
static unsigned  g_art_req      = 0;/* bumps on each request            (guarded) */
static int       g_art_inflight = 0;/* a worker is running              (guarded) */
static int       g_art_ready    = 0;/* a result is waiting for the main thread (guarded) */
static int       g_art_rc       = -1;
static unsigned  g_art_done_req = 0;
static art_req_t g_art_result;      /* the req the waiting result is for (guarded) */
/* g_art_applied is declared above clear_art_state (main-thread only) */

/* main thread only: push a finished decode onto the NP cover + validity flags */
static void apply_art(const art_req_t *job)
{
    snprintf(cover_src,    sizeof cover_src,    "A:/tmp/cover%d.bmp",    job->idx);
    snprintf(thumb_src,    sizeof thumb_src,    "A:/tmp/thumb%d.bmp",    job->idx);
    snprintf(backdrop_src, sizeof backdrop_src, "A:/tmp/backdrop%d.bmp", job->idx);
    coverdsc_valid = (load_cover_dsc(job->out) == 0);   /* track whether g_coverdsc is THIS track's */
    cover_valid = thumb_valid = backdrop_valid = 1;
    g_np_small_valid = 1; g_np_cover_loading = 0;
    ui_np_cover_changed();
    np_sharp_demand();
    /* accent was computed + cached off the UI thread (art worker / prewarm), so just
     * read it - never run OKLab on the UI thread. If absent (no art, or no usable
     * colour), keep the text-hash accent set by update_accent_seed().
     * In static mode the user picked a fixed accent - never let cached art override it. */
    if(g_accent_mode != 1){
        int acc_rgb = mdb_song_accent(job->track);
        if(acc_rgb){ accent = theme_accent_from_rgb(acc_rgb); apply_accent(); }
    }
    copy_cstr(last_art_key, sizeof last_art_key, job->art_key);   /* cache key (album+dir) */
    displayed_idx = job->idx;   /* this buffer is now the one on screen */
    g_art_applied = 1;          /* main.c re-pushes Home/Saver/npmenu art next loop */
}

static void *art_worker(void *arg)
{
    (void)arg;
    for(;;){
        art_req_t job; unsigned my_req, my_gen;
        pthread_mutex_lock(&g_art_mu);
        job = g_art_target; my_req = g_art_req;
        my_gen = art_cancel_gen();   /* capture the cancel token WITH the request: art_cancel() is only called
                                      * by the setter under g_art_mu, so it can't interleave this snapshot */
        pthread_mutex_unlock(&g_art_mu);

        int rc, from_net = 0;                             /* from_net: the picture came from Online Album Art */
        char fp0[24] = "";                                /* the cover's fingerprint (for the accent's freshness) */
        if(job.is_clear) rc = -1;
        else if(!job.online_only && artcache_get(job.track, job.out, job.tout, job.bout) == 0){ rc = 0;   /* cached -> fast copy, no decode */
               if(artcache_key(job.track, fp0, sizeof fp0) != 0) fp0[0] = 0; }
        else { int have_fp = artcache_key(job.track, fp0, sizeof fp0) == 0;   /* identity BEFORE decoding */
               pthread_mutex_lock(&g_decode_mu);                                     /* miss -> decode (killable), then cache */
               /* Pass OUR captured token: if a skip supersedes this decode any time after the snapshot (it
                * always bumps g_cancel_gen via art_cancel), the per-child gen check trips and no obsolete
                * decode runs - closing the window between request-validation and the decode. */
               rc = job.online_only ? -1 : art_make_all_ex_gen(job.track, job.out, job.tout, job.bout, 1, my_gen);
               /* no cover of its own: a picture saved by Online Album Art (netart.c), decoded the same way */
               char np[300];
               if(rc != 0 && rc != -2 && job.online && netart_have(job.artist, job.album, job.title, np, sizeof np)){
                   if(netart_touch(np)){                      /* in use: the prune leaves it alone (and it is still there) */
                       rc = art_make_all_ex_gen(np, job.out, job.tout, job.bout, 1, my_gen);
                       from_net = rc == 0;
                       int ex = art_last_exit();              /* the helper itself said: no picture / unsupported / malformed */
                       if(rc != 0 && (ex == 3 || ex == 4 || ex == 5)) netart_reject(np);   /* never offer it again */
                   }
               }
               pthread_mutex_unlock(&g_decode_mu);
               /* an online picture is never cached as the song's own cover: turning the setting off must drop it */
               if(rc == 0 && have_fp && !from_net) artcache_put_if(job.track, fp0, job.out, job.tout, job.bout); }

        /* compute + cache the OKLab accent HERE (off the UI thread) so apply_art()
         * never blocks: by the time the main thread applies this decode, the accent
         * is already in the DB and is just read back. Skipped in static-accent mode. */
        if(rc == 0 && !from_net && !job.is_clear && !ui_accent_is_static() && fp0[0] && !mdb_song_accent_fresh(job.track, fp0)){
            uint8_t *buf = malloc(CBMP*CBMP*4);
            if(buf){
                uint32_t rgb;
                if(read_cover_bmp(job.out, buf) == 0 && accent_from_buf(buf, &rgb))
                    mdb_set_song_accent(job.track, (int)(rgb & 0xFFFFFF), fp0);
                free(buf);
            }
        }

        job.from_net = from_net;
        pthread_mutex_lock(&g_art_mu);
        if(g_art_req == my_req){            /* still the latest request -> publish + stop */
            g_art_rc = rc; g_art_result = job; g_art_done_req = my_req;
            g_art_ready = 1; g_art_inflight = 0;
            pthread_mutex_unlock(&g_art_mu);
            return NULL;
        }
        pthread_mutex_unlock(&g_art_mu);    /* a newer request arrived -> loop, serve it */
    }
}

/* main thread: clear art NOW + invalidate any in-flight decode (so a track that
 * was mid-decode can't publish stale art after a no-track / clear). */
static void art_request_clear(void)
{
    g_art_title_pending = 0;
    g_np_small_valid = 0;
    sharp_cover_set_source("");
    clear_art_state();
    pthread_mutex_lock(&g_art_mu);
    g_art_req++;
    memset(&g_art_target, 0, sizeof g_art_target);
    g_art_target.is_clear = 1;
    pthread_mutex_unlock(&g_art_mu);
    art_cancel();   /* kill any in-flight decode so it can't publish stale art */
}

/* ---- audiobook chapters: Now Playing shows the current chapter instead of a meaningless album ----
 * A book's chapters are read once per book (chpl parse), then the current one is picked by position. */
#define NP_MAXCHAP 256   /* a chpl count is a single byte (<=255) - cover every declared chapter */
static chapter_t g_chaps[NP_MAXCHAP];
static int       g_nchaps;
static char      g_chap_path[256];

/* Swap the prev/next transport glyphs between music (track skip) and audiobook (-15s/+30s). Only
 * touches the widgets when the mode actually changes, so it's cheap to call every update. */
static int g_np_bookmode = -1;   /* -1 unknown, 0 music, 1 book */
static void np_transport_glyphs(int book)
{
    if(book == g_np_bookmode) return;
    g_np_bookmode = book;
    lv_obj_set_style_text_font(btn_prev, book ? TF(UI_22) : TF(UI_28), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_next, book ? TF(UI_22) : TF(UI_28), LV_PART_MAIN);
    lv_label_set_text(btn_prev, book ? "-15" : LV_SYMBOL_PREV);
    lv_label_set_text(btn_next, book ? "+30" : LV_SYMBOL_NEXT);
}

/* ---- audiobook sleep timer (Now Playing options, for a book) ----------------------------------- */
/* End of the chapter that currently contains the playback position (in book ms), for "End of chapter"
 * sleep. Loads the book's chapters once (shared with book_chapter_window). */
static long book_cur_chapter_end(const track_state_t *st)
{
    if(strcmp(g_chap_path, st->path) != 0){
        g_nchaps = scan_read_chapters(st->path, g_chaps, NP_MAXCHAP);
        copy_cstr(g_chap_path, sizeof g_chap_path, st->path);
    }
    if(g_nchaps <= 0) return st->duration_ms;
    int cur = 0;
    for(int i = 0; i < g_nchaps; i++){ if(g_chaps[i].start_ms <= st->position_ms) cur = i; else break; }
    if(cur + 1 < g_nchaps) return g_chaps[cur+1].start_ms;
    return st->duration_ms > 0 ? st->duration_ms : g_chaps[cur].start_ms;   /* last chapter ends at the book end */
}

/* The [start,end] ms window of the chapter currently under the position, for a book with chapters.
 * Returns 1 (window set) or 0 (not a chaptered book -> caller uses the whole track). Reuses g_chaps. */
static int book_chapter_window(const track_state_t *st, long *lo, long *hi)
{
    if(!mdb_is_book_path(st->path)) return 0;
    if(strcmp(g_chap_path, st->path) != 0){
        g_nchaps = scan_read_chapters(st->path, g_chaps, NP_MAXCHAP);
        copy_cstr(g_chap_path, sizeof g_chap_path, st->path);
    }
    if(g_nchaps <= 0) return 0;
    int cur = 0;
    for(int i = 0; i < g_nchaps; i++){ if(g_chaps[i].start_ms <= st->position_ms) cur = i; else break; }
    long a = g_chaps[cur].start_ms;
    long b = (cur + 1 < g_nchaps) ? g_chaps[cur+1].start_ms
                                  : (st->duration_ms > 0 ? st->duration_ms : a);
    if(b <= a) return 0;                 /* degenerate chapter -> fall back to the whole track */
    *lo = a; *hi = b;
    return 1;
}

static lv_obj_t *g_sleep_dlg;
void ui_np_close_overlays(void){ if(g_sleep_dlg){ lv_obj_del(g_sleep_dlg); g_sleep_dlg = NULL; } }
int ui_np_cover_hit(int x, int y){   /* main.c: while only the art shows, every press is a tap on it (never a seek) */
    (void)x; (void)y;
    return !g_np_ov_on;
}
int  ui_np_overlay_active(void){ return g_sleep_dlg != NULL; }   /* 1 while the sleep popover is up (main loop suppresses the ring/nav gesture) */

static void sleep_opt_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int v = (int)(intptr_t)lv_event_get_user_data(e);   /* minutes; 0 = off; -1 = end of chapter */
    if(v == -1){
        track_state_t st; ipc_get_state(&st);
        if(!mdb_is_book_path(st.path)){ ui_set_sleep_timer(0); ui_np_close_overlays(); return; }  /* context changed away from a book */
        long tgt = book_cur_chapter_end(&st);
        int at_end = (st.duration_ms > 0 && tgt >= st.duration_ms);   /* book_cur_chapter_end returns EXACTLY the duration only for the last / chapterless chapter -> the target is the file end (a rollover may fulfil it); a mid-book boundary is < duration */
        if(tgt > st.position_ms){ ui_set_sleep_eoc(tgt, st.path, at_end); ui_toast("Sleep at end of chapter"); }
        else { ui_set_sleep_timer(0); ui_toast("Sleep timer off"); }
    } else if(v > 0){
        ui_set_sleep_timer(v);
        char b[24]; snprintf(b, sizeof b, "Sleep in %d min", v); ui_toast(b);
    } else {
        ui_set_sleep_timer(0); ui_toast("Sleep timer off");
    }
    ui_np_close_overlays();
}
static void sleep_scrim_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) ui_np_close_overlays(); }

/* The sleep-timer chooser (Now Playing options, for a book). */
void ui_sleep_popover(void)
{
    if(g_sleep_dlg){ ui_np_close_overlays(); return; }
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());               /* full-screen dim; tap outside closes */
    g_sleep_dlg = scrim;
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, 360, 360); lv_obj_center(scrim);
    lv_obj_set_style_bg_color(scrim, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(scrim, sleep_scrim_cb, LV_EVENT_CLICKED, NULL, "ui.sleep_scrim", UI_CORE);

    lv_obj_t *panel = lv_obj_create(scrim);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 204, 272); lv_obj_center(panel);   /* corners ~10 px inside the round glass (212x288 grazed it) */
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, TC(BORDER), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_set_style_pad_ver(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(panel);
    lv_label_set_text(hdr, tr("Sleep Timer"));
    lv_obj_set_style_text_font(hdr, TF(UI_16), 0);
    lv_obj_set_style_text_color(hdr, TC(TEXT_PRIMARY), 0);
    if(!strcmp(theme_kit()->id, "blueprint")){
        char figure[96];
        lv_snprintf(figure, sizeof figure, "fig.%d  %s", screen_figure_number(screen_current()), tr("Sleep Timer"));
        lv_point_t size;
        lv_text_get_size(&size, figure, TF(UI_16), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if(size.x <= 188) lv_label_set_text(hdr, figure); /* keep the translated title when a prefix would dot */
        lv_obj_set_style_text_color(hdr, TC(ACCENT_PRIMARY), 0);
    }

    static const char *OPT_T[] = { "15 min", "30 min", "45 min", "60 min", "End of chapter", "Off" };
    static const int   OPT_V[] = { 15, 30, 45, 60, -1, 0 };
    for(unsigned i = 0; i < sizeof OPT_V / sizeof OPT_V[0]; i++){
        lv_obj_t *b = lv_button_create(panel);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 188, 32);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, TC(RAISED_PRESSED), LV_STATE_PRESSED);
        ui_on(b, sleep_opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)OPT_V[i], "ui.sleep_opt", UI_CORE);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, tr(OPT_T[i]));
        lv_obj_set_style_text_font(l, TF(UI_16), 0);
        lv_obj_set_style_text_color(l, OPT_V[i] == -1 ? accent : TC(TEXT_PRIMARY), 0);
        lv_obj_center(l);
    }
}

/* art reuse/staleness key: album title + the track's parent directory. Audiobooks (.m4b) each carry
 * their own cover, so key them by full path - two books sharing an album tag in one folder must not
 * reuse each other's cover. */
static void make_art_key(const track_state_t *st, char *out, int cap)
{
    if(mdb_is_book_path(st->path)){ snprintf(out, cap, "%s", st->path); return; }
    char dir[256]; snprintf(dir, sizeof dir, "%s", st->path);
    char *slash = strrchr(dir, '/'); if(slash) *slash = '\0'; else dir[0] = '\0';
    snprintf(out, cap, "%s\n%s", st->album, dir);
}

/* The SD card opened for diskOS (the launch gate passed): a cover requested before that - a UI started mid-song,
 * e.g. by a theme change - failed and is never retried for the same song. Forget the path so the next track-state
 * update requests it again. */
static int g_art_force;   /* main thread only: run ui_update on the next loop (no player frame needed) */
void ui_art_retry(void){ sharp_cover_invalidate(); last_path[0] = '\0'; last_art_key[0] = '\0'; g_art_force = 1; }
/* redo the current song's cover from scratch (no same-album reuse): Online Album Art was switched */
void ui_art_redo(void){ ui_art_retry(); }
/* main.c: 1 once after a redo/landed picture, so the main loop runs its update even without a player frame */
int ui_take_art_force(void){ int f = g_art_force; g_art_force = 0; return f; }

/* Entry also handles unchanged state after a fresh theme process or SD denial. */
void ui_np_enter(void)
{
    if (!g_np_small_valid && !sharp_cover_get()) ui_art_retry();
    g_art_force = 1;
    np_ov_set(0, 0);   /* Now Playing opens on the art */
    sharp_cover_request();
}

/* Two independent state pieces: last_path owns local decoding/display; the token,
 * generation and pending flag own the title-dependent online request.
 * Invariant: a title ready on the first snapshot (non-Opus, CUE/SACD,
 * streams) preserves the pre-Opus PATH lifecycle exactly: same path means
 * no restart, loading state or refetch. Only a pending identity adds work:
 * local/path work starts immediately; netart_* waits for its committed title
 * and is submitted once for that playback identity after a local miss.
 *
 * path    identity    title             local/decode       action
 * changed any         ready (even empty) any                normal path request
 * changed any         pending           any                local only; arm online
 * same    changed     ready (even empty) any                keep path; disarm online
 * same    changed     pending           any                keep path; arm new identity
 * same    same        pending           any                wait for commitment
 * same    same        committed         running/unconsumed wait for local result
 * same    same        committed         present/cancelled  disarm; keep cover
 * same    same        committed         absent             disarm; online only once
 * Commitment before/after decode uses the same consumed-result barrier.
 * Empty ready titles retain netart's existing artist/album fallbacks. */
static void update_cover_for_path(const track_state_t *st)
{
    int path_changed = strcmp(last_path, st->path) != 0;
    int identity_changed = st->playback_token != g_art_token ||
                           st->player_generation != g_art_generation;
    if(path_changed) g_np_small_valid = 0;
    sharp_cover_set_source(st->path);
    if(path_changed || identity_changed){
        g_art_token = st->playback_token;
        g_art_generation = st->player_generation;
        g_art_title_pending = st->have_track && !st->title_ready &&
                              cfg_get_int("online_art", 0);
    }
    int online_only = 0;
    if(!path_changed){
        if(!g_art_title_pending) return;
        if(!st->have_track){ g_art_title_pending = 0; return; }
        if(!st->title_ready) return;
        pthread_mutex_lock(&g_art_mu);
        int waiting = g_art_inflight || g_art_ready;
        int missed = g_art_rc != 0 && g_art_rc != -2;
        pthread_mutex_unlock(&g_art_mu);
        if(waiting) return;   /* finish and apply local work before reconciling */
        g_art_title_pending = 0;
        if(!missed || !cfg_get_int("online_art", 0)) return;
        online_only = 1;
    }
    np_sharp_demand();
    char art_key[420]; make_art_key(st, art_key, sizeof art_key);
    copy_cstr(want_art_key, sizeof want_art_key, art_key);   /* what the live track wants now */

    /* Art is shared within an album/folder: same key + valid art -> reuse, skip decoding. */
    if(st->album[0] && cover_valid && strcmp(last_art_key, art_key) == 0) {
        g_art_title_pending = 0;
        copy_cstr(last_path, sizeof(last_path), st->path);
        /* art is already valid for this album - ensure the cover is shown (recovers
         * if a previous in-flight load had swapped in the loading glyph and then a
         * same-album track arrived before that decode landed). */
        g_np_small_valid = 1; g_np_cover_loading = 0;
        ui_np_cover_changed();
        return;
    }

    copy_cstr(last_path, sizeof(last_path), st->path);

    if(!st->path[0]) { art_request_clear(); return; }

    /* New album/art incoming: hide the now-wrong NP cover and show a loading glyph
     * while the worker decodes, so the previous album's art doesn't masquerade as
     * this track's. apply_art() (success) or clear_art_state() (fail) restores it.
     * NB: do NOT touch cover_valid here - the Home thumb/backdrop keep last-good art until the
     * new decode lands; only Now Playing shows the loading state. */
    g_np_small_valid = 0; g_np_cover_loading = 1;
    ui_np_cover_changed();
    coverdsc_valid = 0;   /* the RAM decode is now the PREVIOUS album's - don't let the accent use stale art */

    /* ALWAYS target the non-displayed buffer so an in-flight decode never
     * overwrites the /tmp BMP the shown art may still reference. */
    int nidx = displayed_idx ^ 1;
    art_req_t job; memset(&job, 0, sizeof job);
    job.idx = nidx;
    copy_cstr(job.track, sizeof job.track, st->path);
    copy_cstr(job.art_key, sizeof job.art_key, art_key);
    copy_cstr(job.artist, sizeof job.artist, st->artist);
    copy_cstr(job.album,  sizeof job.album,  st->album);
    copy_cstr(job.title,  sizeof job.title,  st->title_ready ? st->title : "");
    job.online_only = online_only;
    job.online = st->title_ready && cfg_get_int("online_art", 0);
    snprintf(job.out,  sizeof job.out,  "/tmp/cover%d.bmp",    nidx);
    snprintf(job.tout, sizeof job.tout, "/tmp/thumb%d.bmp",    nidx);
    snprintf(job.bout, sizeof job.bout, "/tmp/backdrop%d.bmp", nidx);

    int launch = 0;
    pthread_mutex_lock(&g_art_mu);
    g_art_req++;
    g_art_target = job;
    if(!g_art_inflight){ g_art_inflight = 1; launch = 1; }
    else art_cancel();   /* a worker is mid-decode on a now-stale track -> kill its decoder so it serves this
                          * one. Done UNDER g_art_mu (not after unlocking): otherwise the worker could read
                          * the NEW target and start its decoder in the gap, and this cancel would kill THAT
                          * valid decode instead (leaving art missing). Deadlock-free: no path holds g_pid_mu
                          * (which art_cancel takes) while acquiring g_art_mu, and kill() doesn't block. */
    pthread_mutex_unlock(&g_art_mu);

    if(launch){
        pthread_t th;
        if(pthread_create(&th, NULL, art_worker, NULL) == 0){
            pthread_detach(th);
        } else {                            /* no worker (memory pressure): show the no-art placeholder */
            /* Never decode on the UI thread: waiting for the decode lock plus a decode could freeze the screen
             * for many seconds. The next track change tries to start a worker again. */
            pthread_mutex_lock(&g_art_mu); g_art_inflight = 0; pthread_mutex_unlock(&g_art_mu);
            clear_art_state();
        }
    }
    /* else: a worker is already running; it will pick up g_art_target when it loops */
}

/* main-thread timer: apply a finished decode (or re-clear) */
void ui_art_poll(lv_timer_t *t)
{
    (void)t;
    if(g_art_title_pending){
        track_state_t st; ipc_get_state(&st); update_cover_for_path(&st);
    }
    { char got[512];                        /* Online Album Art saved a picture: redo that song's cover if it's on */
      if(netart_take_ready(got, sizeof got) && cfg_get_int("online_art", 0) && strcmp(got, last_path) == 0) ui_art_retry(); }
    int ready = 0, rc = -1; art_req_t res; unsigned done_req = 0, cur_req = 0;
    pthread_mutex_lock(&g_art_mu);
    if(g_art_ready){
        ready = 1; g_art_ready = 0;
        rc = g_art_rc; res = g_art_result; done_req = g_art_done_req; cur_req = g_art_req;
    }
    pthread_mutex_unlock(&g_art_mu);
    if(!ready) return;
    if(done_req != cur_req) return;        /* superseded - a newer decode is in flight */
    if(res.is_clear)      clear_art_state();/* main already cleared; idempotent */
    else if(rc == 0 && res.from_net && !cfg_get_int("online_art", 0)){
        clear_art_state();                  /* an online picture that landed after the setting was turned off */
    }
    else if(rc == 0){
        /* only apply if the live track still wants this album - guards the rare
         * A(X)->B(Y, decoding)->C(X) race where Y would otherwise stomp correct X art. */
        if(strcmp(res.art_key, want_art_key) == 0){
            apply_art(&res);
            /* an online picture may be one song's own (not its album's) and must disappear when the setting goes off:
             * never reuse it for the next track of the album */
            if(res.from_net) last_art_key[0] = '\0';
        }
    }
    else {
        clear_art_state();
        /* no cover of its own: look it up online in the background (Settings > Display > Online Album Art) */
        if(rc != -2 && res.online && cfg_get_int("online_art", 0) && strcmp(res.art_key, want_art_key) == 0)
            netart_request(res.track, res.artist, res.album, res.title);
    }
}

/* main.c calls this each loop: returns 1 once after art was (re)applied so the
 * dependent surfaces (Home pill / backdrop / Options thumb) get re-pushed. */
int ui_take_art_applied(void)
{
    if(!g_art_applied) return 0;
    g_art_applied = 0;
    return 1;
}

/* Settings hook: apply the chosen accent mode/colour immediately (main thread). */
void ui_set_accent_config(int mode, int rgb){
    g_accent_mode  = mode ? 1 : 0;
    g_accent_color = (uint32_t)rgb & 0xFFFFFF;
    if(g_accent_mode){
        accent = theme_accent_from_rgb(g_accent_color);   /* paint the fixed colour now */
        g_accent_gray = 0;
    } else {
        last_seed_a[0] = '\0'; last_seed_b[0] = '\0';   /* force dynamic recompute next ui_update */
    }
    apply_accent();
    g_static_last = g_accent_mode ? g_accent_color : 0xFFFFFFFFu;   /* config just repainted; keep the guard in sync */
    g_art_applied = 1;   /* re-push Home/Saver/npmenu accent surfaces */
}

/* ---- background art prewarm (user-gated, heat-safe) ------------------------
 * Fills the SD cover cache (+ DB accent, dynamic mode only) so song-switching is
 * instant even on first play. The earlier always-on version overheated the device
 * to 50C, so this only works while the user-chosen window holds AND the battery is
 * cool, and it paces hard. Modes: 0=covers only (album covers preload; no per-track sweep) 1=+per-track sweep when idle 2=+sweep when idle AND charging. */

static int pw_temp_dc(void){            /* battery temp, tenths-C; -1 on read failure */
    FILE *f=fopen("/sys/class/power_supply/cw221X-bat/temp","r"); if(!f) return -1;
    int t=-1; if(fscanf(f,"%d",&t)!=1) t=-1; fclose(f); return t;
}
static int pw_charging(void){
    /* cw221X exposes NO `status` node - charging shows via current_now (1=charging, 0=battery on
     * this driver). The old `status` read always failed -> prewarm mode "idle & charging" never ran. */
    long cur=0;
    FILE *f=fopen("/sys/class/power_supply/cw221X-bat/current_now","r"); if(!f) return 0;
    if(fscanf(f,"%ld",&cur)!=1) cur=0; fclose(f);
    return cur > 0;
}
/* 1 if the user-selected window currently allows background work. IDLE is mandatory
 * for every active mode: never decode while the user is actively looking at the screen
 * (heat-/UX-safety), even on charge. m1=when idle, m2=idle AND charging. */
static int pw_window_open(void){
    if(!sd_io_allowed()) return 0;
    int m=g_prewarm_mode; if(m==0) return 0;
    if(!ui_main_is_idle()) return 0;   /* screen dimmed/off required */
    if(m==2) return pw_charging();     /* idle & charging */
    return 1;                          /* m==1: when idle */
}

/* Block until heat + the live decode allow another decode. Shared by both prewarm phases. */
static void pw_wait_window(int *hot){
    for(;;){
        if(!sd_io_allowed()){ usleep(200000); continue; }
        int t = pw_temp_dc();                       /* battery temp, tenths-C; fail-closed if unreadable */
        if(t < 0){ sleep(15); continue; }
        if(*hot){ if(t < 400) *hot=0; else { sleep(15); continue; } }   /* hysteresis: pause >=42C, resume <40C */
        else if(t >= 420){ *hot=1; sleep(15); continue; }
        int busy; pthread_mutex_lock(&g_art_mu); busy=g_art_inflight; pthread_mutex_unlock(&g_art_mu);
        if(busy){ usleep(300*1000); continue; }     /* never a 2nd decoder while the user's decode runs */
        return;
    }
}

/* PHASE 1 (boot): proactively decode ONE cover per album so the Album (cover flow) view is populated
 * without playing every album first. Per-album (hundreds) not per-track (thousands), and strictly
 * battery-temp throttled, so it fills in a couple of minutes on first boot and is near-instant on later
 * boots (the SD cache persists). Runs regardless of the idle gate - users want covers ready while they
 * browse - but yields to the live decode and to heat. DB access is safe: g_db is opened FULLMUTEX. */
static void *art_prewarm_worker(void *arg)
{
    (void)arg;
    nice(15);                 /* lowest CPU priority */
    const char *PC="/tmp/pw_cover.bmp", *PT="/tmp/pw_thumb.bmp", *PB="/tmp/pw_backdrop.bmp";
    int id = 0, hot = 0, did_work = 0;   /* hot = temp hysteresis latch */
    for(;;){
        /* PHASE 1: album-cover prewarm. The MAIN thread enqueues representative track paths (built with
         * the non-thread-safe album/track caches); the worker only pops + decodes here, so no mdb
         * in-memory cache is touched off the UI thread. Runs regardless of the idle gate (users want
         * covers while browsing) but stays temp-throttled and serialized with the live NP decoder. */
        char qpath[512];
        if(pwq_pop(qpath, sizeof qpath)){
            pw_wait_window(&hot);                        /* heat gate + yields to the live decode */
            char fp0[24];
            int cached = artcache_lookup(qpath, fp0, sizeof fp0);   /* one fingerprint: hit, or the key to store under */
            if(cached == 1){ usleep(15*1000); continue; }   /* already cached (the UI thread no longer checks) */
            if(cached == 0){
                pthread_mutex_lock(&g_decode_mu);
                int r = art_make_all(qpath, PC, PT, PB);
                pthread_mutex_unlock(&g_decode_mu);
                if(r == 0) artcache_put_if(qpath, fp0, PC, PT, PB);
            }
            usleep(120*1000);                            /* light pace; the temp gate is the real limiter */
            continue;
        }
        /* PHASE 2: per-track idle sweep (accents + any remaining covers), gated on the user setting. */
        if(!pw_window_open()){ id=0; did_work=0; pw_sleep(5); continue; }   /* not allowed now -> wait (wake for queued covers) */

        /* temp throttle: fail-closed if unreadable; hysteresis pause >=42C, resume <40C. */
        int t = pw_temp_dc();
        if(t < 0){ sleep(15); continue; }
        if(hot){ if(t < 400) hot=0; else { sleep(15); continue; } }
        else if(t >= 420){ hot=1; sleep(15); continue; }

        /* never run a 2nd decoder while the live (user) decode is in flight */
        { int busy; pthread_mutex_lock(&g_art_mu); busy=g_art_inflight; pthread_mutex_unlock(&g_art_mu);
          if(busy){ usleep(300*1000); continue; } }

        char path[300];
        if(!mdb_prewarm_next(id, &id, path, sizeof path)){   /* swept the whole library */
            id=0; pw_sleep(did_work?60:1800); did_work=0; continue;   /* rest longer if nothing pending (wake for queued covers) */
        }
        if(!path[0]) continue;

        int have_art = artcache_has(path);
        char acc_fp[24];
        int need_acc = !ui_accent_is_static() && artcache_key(path, acc_fp, sizeof acc_fp) == 0
                       && !mdb_song_accent_fresh(path, acc_fp);
        if(have_art && !need_acc){ usleep(15*1000); continue; }   /* already done -> light skip */

        if(!have_art){
            char fp0[24];
            if(artcache_key(path, fp0, sizeof fp0) != 0){ usleep(300*1000); continue; }
            pthread_mutex_lock(&g_decode_mu);
            int r = art_make_all(path, PC, PT, PB);
            pthread_mutex_unlock(&g_decode_mu);
            if(r == 0) artcache_put_if(path, fp0, PC, PT, PB);
            else { usleep(300*1000); continue; }                  /* undecodable -> skip */
        }
        if(need_acc){
            /* Materialise THIS path's cover before reading it. If the cached read fails (SD vanished mid-
             * sweep), PC still holds the PREVIOUS track's image; computing+storing an accent from it would
             * mislabel this path permanently (a nonzero accent skips future correction). Only proceed on a
             * confirmed fresh cover for this path (a just-decoded !have_art cover is already in PC). */
            int cover_ready = have_art ? (artcache_get_cover(path, PC) == 0) : 1;   /* the accent reads only the cover */
            if(cover_ready){
                uint8_t *buf = malloc(CBMP*CBMP*4);
                if(buf){
                    uint32_t rgb;
                    if(read_cover_bmp(PC, buf) == 0 && accent_from_buf(buf, &rgb))
                        mdb_set_song_accent(path, (int)(rgb & 0xFFFFFF), acc_fp);
                    free(buf);
                }
            }
        }
        did_work = 1;
        sleep(3);     /* hard pace between decodes: keep heat low */
    }
    return NULL;
}

/* main.c calls this once at startup. The worker self-gates on g_prewarm_mode, so
 * it's safe to always spawn (it just sleeps while mode==off). */
void ui_start_art_prewarm(void)
{
    pthread_t th;
    if(pthread_create(&th, NULL, art_prewarm_worker, NULL) == 0) pthread_detach(th);
}

/* Expose the current decoded cover to other surfaces (e.g. the Home pill).
 * Returns the "A:/tmp/coverN.bmp" path, or NULL when there's no usable art. */
const char *ui_current_cover_src(void)
{
    return cover_valid ? cover_src : NULL;
}

/* Native-size 42px thumb path for the Home pill, or NULL when no art. */
const char *ui_current_thumb_src(void)
{
    return thumb_valid ? thumb_src : NULL;
}

/* Full-screen blurred backdrop path (for Home), or NULL. */
const char *ui_current_backdrop_src(void)
{
    return backdrop_valid ? backdrop_src : NULL;
}

/* Montserrat covers Latin + Latin-Extended only; it has no Cyrillic/Greek (issue #3:
 * "Cyrillic metadata" rendered as boxes) and no CJK ("CJK metadata"). Build a per-size fallback chain:
 *   Montserrat(size) -> font_intl(size) [Cyrillic/Greek/Latin-ext, Noto, uncompressed]
 *                    -> font_hebrew(size) -> stock TTC [CJK, matched size] -> Source Han 16 -> supplement.
 * The intl fonts are generated at matched ascent/descent (base_line 4/5/6 == Montserrat's)
 * so fallback glyphs sit on the same baseline; LVGL uses the PRIMARY font's line-height, and
 * each intl line-height (20/23/28) fits within Montserrat's (20/24/28) so nothing clips.
 * We keep mutable copies because .fallback must be set on non-const fonts. */
static lv_font_t s_cjk; /* existing subset plus the small music-metadata supplement */
static lv_font_t s_ft[41], s_font[41], s_intl[41], s_hebrew[41];
static const lv_font_t *ui_intl_font(int px)
{
    switch(px) {
    case 14: return &font_intl_14;
    case 16: return &font_intl_16;
    case 18: return &font_intl_18;
    case 20: return &font_intl_20;
    default: return NULL;
    }
}

static const lv_font_t *ui_hebrew_font(int px)
{
    switch(px) {
    case 14: return &font_hebrew_14;
    case 16: return &font_hebrew_16;
    case 18: return &font_hebrew_18;
    case 20: return &font_hebrew_20;
    default: return NULL;
    }
}
static void ui_fonts_init(void)
{
    if(s_cjk.get_glyph_dsc) return;
    s_cjk = lv_font_source_han_16_cjk; s_cjk.fallback = &font_cjk_extra_16;
}

/* Public accessor for the fallback-chained text font (Montserrat -> intl -> CJK) so other screens
 * render user text (track/album/artist names) with full glyph coverage. */
const lv_font_t *ui_text_font(int px){
    return ui_font_cjk(px);
}

/* Shared CJK-capable chain at each available Montserrat role size. */
const lv_font_t *ui_font_cjk(int size)
{
    ui_fonts_init();
    if(size != 10 && size != 12 && size != 14 && size != 16 && size != 18 && size != 20 &&
       size != 22 && size != 24 && size != 28 && size != 32 && size != 36 && size != 40) size = 16;
    const lv_font_t *base = theme_font_base(size);
    if(!s_font[size].get_glyph_dsc) {
        const lv_font_t *intl = ui_intl_font(size);
        const lv_font_t *hebrew = ui_hebrew_font(size);
        const lv_font_t *ft = fontft_font(size);
        const lv_font_t *next = &s_cjk;
        if(ft) { s_ft[size] = *ft; s_ft[size].fallback = next; next = &s_ft[size]; }
        if(hebrew) { s_hebrew[size] = *hebrew; s_hebrew[size].fallback = next; next = &s_hebrew[size]; }
        if(intl) { s_intl[size] = *intl; s_intl[size].fallback = next; next = &s_intl[size]; }
        s_font[size] = *base;
        s_font[size].fallback = next;
    }
    return &s_font[size];
}

/* The clock and battery line, at the top of the Library root (main's clock_tick and status poll feed it). */
static lv_obj_t *g_status;
static char g_status_hm[12];
static int  g_status_batt = -1, g_status_charging;
static void status_paint(void){
    if(!g_status) return;
    char t[48];
    int b = g_status_batt;   /* the same glyphs as Home's status had: the bolt while charging, else the battery level */
    const char *bs = g_status_charging ? LV_SYMBOL_CHARGE : b >= 90 ? LV_SYMBOL_BATTERY_FULL : b >= 65 ? LV_SYMBOL_BATTERY_3 :
                     b >= 40 ? LV_SYMBOL_BATTERY_2 : b >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    if(b >= 0) snprintf(t, sizeof t, "%s   %s %d%%", g_status_hm, bs, b);
    else snprintf(t, sizeof t, "%s", g_status_hm);
    if(strcmp(lv_label_get_text(g_status), t)) lv_label_set_text(g_status, t);
}
static void status_deleted(lv_event_t *e){ if(lv_event_get_target(e) == (void *)g_status) g_status = NULL; }
lv_obj_t *ui_status_line(lv_obj_t *parent)
{
    g_status = lv_label_create(parent);
    style_text(g_status, TF(UI_14), TC(TEXT_SECONDARY));
    lv_obj_align(g_status, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_clear_flag(g_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_status, status_deleted, LV_EVENT_DELETE, NULL);
    lv_label_set_text(g_status, "");
    status_paint();
    return g_status;
}
void ui_status_clock(const char *hm){ snprintf(g_status_hm, sizeof g_status_hm, "%s", hm ? hm : ""); status_paint(); }
void ui_status_batt(int batt, int charging){ g_status_batt = batt; g_status_charging = charging; status_paint(); }

void ui_create(lv_obj_t *root)
{
    lv_obj_t *scr = root;
    if(theme_screen_plain(THEME_PLAIN_NOWPLAYING)) theme_screen_solid(scr);   /* the theme draws Now Playing plain */

    ui_fonts_init();
    static lv_timer_t *s_playcount_timer;
    if(!s_playcount_timer) s_playcount_timer = lv_timer_create(playcount_tick, 1000, NULL);
    lin_lut_init();   /* seed the OKLab sRGB LUT once, up front: dynamic accent compute
                       * (art worker) needs it whether or not the prewarm ever runs. */
    /* settings_apply_startup() ran before us and already loaded g_accent_mode/g_accent_color
     * from cfg, so honor a saved STATIC accent here - otherwise Home (created right
     * after, and now reading the live accent) would paint the default until the first
     * ui_update() corrected them. Dynamic mode keeps the default until an album accent lands. */
    accent = (g_accent_mode == 1) ? theme_accent_from_rgb(g_accent_color) : TC(ACCENT_PRIMARY);
    last_path[0] = '\0';
    last_art_key[0] = '\0';
    want_art_key[0] = '\0';
    last_seed_a[0] = '\0';
    last_seed_b[0] = '\0';
    cover_src[0] = '\0';
    cover_valid = 0;
    coverdsc_valid = 0;
    thumb_src[0] = '\0';
    thumb_valid = 0;
    backdrop_valid = 0;
    displayed_idx = 0;
    shown_progress = 0;

    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, TC(CANVAS), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    backdrop_src[0] = '\0';
    g_np_mode = -1;        /* the widgets are new: let the refreshes below paint them */
    g_np_bookmode = -1;

    /* Now Playing: two states (see above ui_np_cover_changed) */
    ui_np_spin(0);
    g_np_ov_on = 0;
    if(!g_np_ov_tm) g_np_ov_tm = lv_timer_create(np_ov_idle, 250, NULL);
    lv_timer_pause(g_np_ov_tm);
    static int s_np_perf;
    if(!s_np_perf && lv_display_get_default()){
        s_np_perf = 1;
        lv_display_add_event_cb(lv_display_get_default(), np_render_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(lv_display_get_default(), np_render_cb, LV_EVENT_RENDER_READY, NULL);
    }

    /* ---- the art: a dark disc with the title when there is none; the turning picture, dimmed under the controls ---- */
    g_np_noart = lv_obj_create(scr);
    lv_obj_remove_style_all(g_np_noart);
    lv_obj_set_size(g_np_noart, NP_W, NP_W);
    lv_obj_center(g_np_noart);
    lv_obj_set_style_radius(g_np_noart, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_np_noart, TC(ART_PLACEHOLDER), 0);
    lv_obj_set_style_bg_opa(g_np_noart, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_np_noart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    g_np_noart_title = lv_label_create(g_np_noart);
    lv_obj_set_width(g_np_noart_title, 240);
    style_text(g_np_noart_title, TF(USER_20), TC(TEXT_SECONDARY));
    lv_label_set_long_mode(g_np_noart_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_np_noart_title, tr("No Track"));
    lv_obj_center(g_np_noart_title);

    g_np_art = lv_image_create(scr);
    g_np_dim = lv_obj_create(scr);   /* over the art while the controls show, so their text reads */
    lv_obj_remove_style_all(g_np_dim);
    lv_obj_set_style_bg_color(g_np_dim, TC(IMAGE_TINT), 0);
    lv_obj_set_style_bg_opa(g_np_dim, NP_DIM_OPA, 0);
    lv_obj_t *pics[2] = { g_np_art, g_np_dim };
    for(int i = 0; i < 2; i++){
        lv_obj_set_size(pics[i], NP_W, NP_W);
        lv_obj_center(pics[i]);
        lv_obj_clear_flag(pics[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pics[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- a still tap anywhere shows or hides the controls (the buttons above it take their own taps) ---- */
    g_np_tap = lv_obj_create(scr);
    lv_obj_remove_style_all(g_np_tap);
    lv_obj_set_size(g_np_tap, NP_W, NP_W);
    lv_obj_center(g_np_tap);
    lv_obj_clear_flag(g_np_tap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_np_tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_np_tap, np_tap_cb, LV_EVENT_PRESSED, NULL);
    ui_on(g_np_tap, np_tap_cb, LV_EVENT_CLICKED, NULL, "ui.np_tap", UI_CORE);

    /* ---- the controls: seek ring, title, artist, transport, play mode, heart, times ---- */
    g_np_ov = lv_obj_create(scr);
    lv_obj_remove_style_all(g_np_ov);
    lv_obj_set_size(g_np_ov, NP_W, NP_W);
    lv_obj_center(g_np_ov);
    lv_obj_clear_flag(g_np_ov, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_np_ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ov = g_np_ov;

    ring = lv_arc_create(ov);
    g_ring_d = ARC_D;
    lv_obj_set_size(ring, g_ring_d, g_ring_d);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_range(ring, 0, 1000);
    lv_arc_set_value(ring, 0);
    lv_arc_set_bg_angles(ring, 0, ARC_SWEEP);
    lv_arc_set_rotation(ring, ARC_ROT);
    lv_arc_set_mode(ring, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, TC(CONTROL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_INDICATOR);
    /* draggable seek: a small knob thumb + scrub handler */
    lv_obj_set_style_bg_color(ring, TC(CONTROL_KNOB), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ring, 5, LV_PART_KNOB);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    /* display-only: seek is driven by the NP recognizer (ui_np_seek_*) fed from
     * main.c's raw touch loop, so nav swipes / edge pulls never grab the arc */
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    title = lv_label_create(ov);
    g_title_scroll_mode = LV_LABEL_LONG_CLIP;
    lv_obj_set_width(title, 236);   /* clear of the ring at its height */
    style_text(title, TF(USER_20), TC(TEXT_PRIMARY));
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(title, tr("No Track"));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 96);

    artist = lv_label_create(ov);
    lv_obj_set_width(artist, 230);
    style_text(artist, TF(USER_16), TC(TEXT_SECONDARY));
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(artist, "");
    lv_obj_align(artist, LV_ALIGN_TOP_MID, 0, 126);

    btn_prev = lv_label_create(ov);
    style_text(btn_prev, TF(UI_28), TC(TEXT_PRIMARY));
    lv_label_set_text(btn_prev, LV_SYMBOL_PREV);
    lv_obj_align(btn_prev, LV_ALIGN_TOP_MID, -72, 168);

    btn_pp = lv_label_create(ov);
    style_text(btn_pp, TF(UI_32), accent);
    ui_pp_glyph(btn_pp, 0);
    lv_obj_align(btn_pp, LV_ALIGN_TOP_MID, 0, 164);

    btn_next = lv_label_create(ov);
    style_text(btn_next, TF(UI_28), TC(TEXT_PRIMARY));
    lv_label_set_text(btn_next, LV_SYMBOL_NEXT);
    lv_obj_align(btn_next, LV_ALIGN_TOP_MID, 72, 168);

    make_clickable(btn_prev, "0201000C0002");
    make_clickable(btn_pp,   "0201000C0000");
    make_clickable(btn_next, "0201000C0001");

    /* Play-mode toggle (left) and the favourites heart (right), under the transport */
    btn_mode = lv_button_create(ov);
    lv_obj_remove_style_all(btn_mode);
    lv_obj_set_size(btn_mode, 44, 44);
    lv_obj_align(btn_mode, LV_ALIGN_TOP_MID, -52, 222);
    lv_obj_add_flag(btn_mode, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_mode, mode_click_cb, LV_EVENT_PRESSED, NULL);
    ui_on(btn_mode, mode_click_cb, LV_EVENT_CLICKED, NULL, "ui.mode", UI_CORE);
    mode_icon = lv_label_create(btn_mode);
    lv_obj_set_style_text_font(mode_icon, TF(ICON_28), LV_PART_MAIN);
    lv_label_set_text(mode_icon, MODE_ARROW);
    lv_obj_center(mode_icon);
    mode_one = lv_label_create(btn_mode);
    lv_obj_set_style_text_font(mode_one, TF(UI_10), LV_PART_MAIN);
    lv_obj_set_style_text_color(mode_one, TC(TEXT_PRIMARY), LV_PART_MAIN);
    lv_label_set_text(mode_one, "1");
    lv_obj_center(mode_one);   /* sits between the loop arrows */
    lv_obj_add_flag(mode_one, LV_OBJ_FLAG_HIDDEN);
    mode_refresh(cfg_get_int("work_mode", 0));

    btn_fav = lv_button_create(ov);
    lv_obj_remove_style_all(btn_fav);
    lv_obj_set_size(btn_fav, 44, 44);
    lv_obj_align(btn_fav, LV_ALIGN_TOP_MID, 52, 222);
    lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_fav, fav_click_cb, LV_EVENT_PRESSED, NULL);
    ui_on(btn_fav, fav_click_cb, LV_EVENT_CLICKED, NULL, "ui.fav", UI_CORE);
    fav_icon = lv_label_create(btn_fav);
    lv_obj_set_style_text_font(fav_icon, TF(ICON_28), LV_PART_MAIN);
    lv_obj_set_style_text_color(fav_icon, TC(TEXT_MUTED), LV_PART_MAIN);
    lv_label_set_text(fav_icon, HEART_OUTLINE);
    lv_obj_center(fav_icon);
    lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);   /* shown once a track loads */

    /* Times sit as a centered pair in the ring's bottom gap, so they never clash with the progress fill. */
    t_elapsed = lv_label_create(ov);
    lv_obj_set_width(t_elapsed, 80);   /* fits -999:59:59 at Montserrat 14 */
    style_text(t_elapsed, TF(UI_14), TC(TEXT_SECONDARY));
    lv_label_set_text(t_elapsed, "0:00");
    lv_obj_align(t_elapsed, LV_ALIGN_TOP_MID, -46, 318);

    lv_obj_t *t_sep = lv_label_create(ov);
    style_text(t_sep, TF(UI_14), TC(TEXT_SEPARATOR));
    lv_label_set_text(t_sep, "/");
    lv_obj_align(t_sep, LV_ALIGN_TOP_MID, 0, 318);

    t_remain = lv_label_create(ov);
    lv_obj_set_width(t_remain, 80);   /* fits -999:59:59 at Montserrat 14 */
    style_text(t_remain, TF(UI_14), TC(TEXT_SECONDARY));
    lv_label_set_text(t_remain, "0:00");
    lv_obj_align(t_remain, LV_ALIGN_TOP_MID, 46, 318);

    np_noart_text();
    apply_accent();
    ui_np_cover_changed();   /* binds the picture again (and tints the blur for this theme) */
}

void ui_update(const track_state_t *st)
{
    char elapsed_buf[12];   /* room for H:MM:SS (audiobook-length) - mmss now emits hours */
    char remain_core[12];
    char remain_buf[14];    /* "-" + H:MM:SS */
    long dur;
    long pos;
    long remain;
    int32_t progress;

    mode_refresh(cfg_get_int("work_mode", 0));   /* keep in sync with Tune menu */

    if(st == NULL || !st->have_track) {
        last_path[0] = '\0';
        update_accent_seed(st);   /* no-track -> neutral light grey (resets seed + applies once via the gate) */

        set_label_text_changed(title, "No Track");
        set_label_text_changed(artist, "");
        np_noart_text();
        set_label_text_changed(t_elapsed, "0:00");
        set_label_text_changed(t_remain, "0:00");
        ui_pp_glyph(btn_pp, 0);
        art_request_clear();   /* clear art + invalidate any in-flight decode (was a bare fallback) */
        g_np_curpath[0] = '\0';
        fav_refresh(0, 0);
        ui_np_close_overlays();   /* no track -> dismiss any open sleep popover */
        g_track_dur = 0;
        set_progress_changed(0);
        return;
    }

    update_cover_for_path(st);   /* first: invalidates stale RAM art on a new album */
    update_accent_seed(st);      /* then: art-accent if THIS track's cover is ready, else text-hash */

    set_label_text_changed(title, st->title[0] ? st->title : "Untitled");
    set_label_text_changed(artist, st->artist[0] ? st->artist : "");
    np_noart_text();
    snprintf(g_np_curpath, sizeof g_np_curpath, "%s", st->path);
    fav_refresh(fav_current(st), 1);

    /* Audiobook transport: -15/+30 skips instead of track prev/next, and no shuffle or favourite
     * (they make no sense for a book; its sleep timer is in Now Playing options). */
    {
        int is_book = mdb_is_book_path(st->path);
        np_transport_glyphs(is_book);
        if(is_book){
            lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_mode, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(btn_mode, LV_OBJ_FLAG_HIDDEN);
            ui_np_close_overlays();   /* not a book -> dismiss any open sleep popover (its context is gone) */
        }
    }

    dur = st->duration_ms;
    g_track_dur = dur;
    pos = st->position_ms;
    if(dur < 0) dur = 0;
    if(pos < 0) pos = 0;
    if(dur > 0 && pos > dur) pos = dur;

    /* Seek/progress window: a book's ring + times track the CURRENT CHAPTER (the whole-book
     * ring is unusable - 1 degree can be minutes on a long book). Music uses the whole track. */
    if(!book_chapter_window(st, &g_seek_lo, &g_seek_hi)){ g_seek_lo = 0; g_seek_hi = dur; }
    long span = g_seek_hi - g_seek_lo;
    long rel  = pos - g_seek_lo;
    if(rel < 0) rel = 0;
    if(span > 0 && rel > span) rel = span;

    progress = (span > 0) ? (int32_t)(((long long)rel * 1000LL) / (long long)span) : 0;

    /* seek echo-suppression: while the post-seek hold is active and the player
     * is still streaming a stale (far-from-target) position, keep the arc and
     * times pinned at the seeked target instead of snapping back. */
    if(g_seek_hold_until) {
        long d = pos - g_seek_target_ms; if(d < 0) d = -d;   /* ABSOLUTE ms gap - unaffected by a chapter-window flip at a seek-to-boundary */
        if(lv_tick_get() < g_seek_hold_until && d > 3000) {
            ui_pp_glyph(btn_pp, ui_pp_icon_playing(st->state == 2));
            return;   /* ignore this stale echo */
        }
        g_seek_hold_until = 0; g_seek_target_ms = -1;   /* caught up or window lapsed */
    }

    mmss(rel, elapsed_buf, sizeof(elapsed_buf));   /* elapsed within the window (chapter for a book) */
    set_label_text_changed(t_elapsed, elapsed_buf);

    if(span > 0) {
        remain = span - rel;
        if(remain < 0) remain = 0;
        mmss(remain, remain_core, sizeof(remain_core));
        snprintf(remain_buf, sizeof(remain_buf), "-%s", remain_core);
    } else {
        snprintf(remain_buf, sizeof(remain_buf), "0:00");
    }
    set_label_text_changed(t_remain, remain_buf);
    set_progress_changed(progress);

    ui_pp_glyph(btn_pp, ui_pp_icon_playing(st->state == 2));
}

/* ---- volume overlay: a draggable arc on lv_layer_top (shows over any screen).
 * It appears on a hardware-volume change (a714) and is also touch-adjustable -
 * dragging the arc calls ui_set_volume() so you can set the level on-screen
 * instead of hammering the buttons (which double-press into next/prev). */
static lv_obj_t *g_vol_panel, *g_vol_arc, *g_vol_num;
static lv_timer_t *g_vol_timer;
static int g_vol_suppress;   /* 1 while we set the arc programmatically (no echo loop) */
static uint32_t g_vol_last_send;  /* throttle: tick of the last 0715 we sent */
static int g_vol_pending;         /* 1 if a value changed but was throttled (commit on release) */
static void vol_hide_cb(lv_timer_t *t){
    if(g_vol_panel) lv_obj_add_flag(g_vol_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
}
/* Throttle volume commits to <=1 per 120ms while dragging (the arc fires
 * VALUE_CHANGED every step - sending each one hammers the player). The on-screen
 * number tracks live; the final value is always committed on RELEASED. */
static void vol_arc_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(g_vol_suppress) return;                       /* programmatic update, ignore */
    int v = lv_arc_get_value(g_vol_arc);
    char b[12]; snprintf(b, sizeof b, "%d/%d", v, ui_volume_cap()); lv_label_set_text(g_vol_num, b);
    if(code == LV_EVENT_VALUE_CHANGED){
        if(lv_tick_elaps(g_vol_last_send) >= 120){
            ui_set_volume(v); g_vol_last_send = lv_tick_get(); g_vol_pending = 0;
        } else {
            g_vol_pending = 1;                       /* defer to release */
        }
        if(g_vol_timer){ lv_timer_reset(g_vol_timer); lv_timer_resume(g_vol_timer); }
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST){
        /* commit the throttled final value on release OR press-lost (LVGL clears
         * LV_STATE_PRESSED on both - without this a deferred value never sends). */
        if(g_vol_pending){ ui_set_volume(v); g_vol_pending = 0; g_vol_last_send = lv_tick_get(); }
    }
}
void ui_show_volume(int vol)
{
    if(!g_vol_panel){
        lv_obj_t *top = lv_layer_top();
        /* translucent circular backdrop, centred */
        g_vol_panel = lv_obj_create(top);
        lv_obj_remove_style_all(g_vol_panel);
        lv_obj_set_size(g_vol_panel, 200, 200);
        lv_obj_center(g_vol_panel);
        lv_obj_set_style_radius(g_vol_panel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_vol_panel, TC(CANVAS), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_vol_panel, 240, LV_PART_MAIN);   /* 205 let the track title show through and clash
                                                                      * with the level in every theme: now a faint hint */
        lv_obj_clear_flag(g_vol_panel, LV_OBJ_FLAG_SCROLLABLE);
        kit_keep(g_vol_panel);   /* the round volume dial stays round in every theme; its arc and number take the theme */

        g_vol_arc = lv_arc_create(g_vol_panel);
        lv_obj_set_size(g_vol_arc, 184, 184);
        lv_obj_center(g_vol_arc);
        lv_arc_set_rotation(g_vol_arc, 135);
        lv_arc_set_bg_angles(g_vol_arc, 0, 270);
        lv_obj_set_style_arc_width(g_vol_arc, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_vol_arc, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(g_vol_arc, TC(CONTROL_TRACK_STRONG), LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_vol_arc, TC(CONTROL_FILL), LV_PART_INDICATOR);
        /* the knob in the theme's accent: unset, it fell back to LVGL's own default (blue in Default dark, red in light) */
        lv_obj_set_style_bg_color(g_vol_arc, TC(ACCENT_PRIMARY), LV_PART_KNOB);
        ui_on(g_vol_arc, vol_arc_cb, LV_EVENT_VALUE_CHANGED, NULL, "ui.vol_arc.value", UI_CORE);
        lv_obj_add_event_cb(g_vol_arc, vol_arc_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(g_vol_arc, vol_arc_cb, LV_EVENT_PRESS_LOST, NULL);

        lv_obj_t *spk = lv_label_create(g_vol_panel);
        lv_label_set_text(spk, LV_SYMBOL_VOLUME_MAX);
        lv_obj_align(spk, LV_ALIGN_CENTER, 0, -22);
        lv_obj_set_style_text_color(spk, TC(TEXT_SECONDARY), LV_PART_MAIN);

        g_vol_num = lv_label_create(g_vol_panel);
        lv_obj_align(g_vol_num, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_text_font(g_vol_num, TF(UI_28), LV_PART_MAIN);
        lv_obj_set_style_text_color(g_vol_num, TC(TEXT_PRIMARY), LV_PART_MAIN);

        g_vol_timer = lv_timer_create(vol_hide_cb, 1800, NULL);
        lv_timer_pause(g_vol_timer);
    }
    if(vol < 0) vol = 0; if(vol > VOL_MAX) vol = VOL_MAX;
    /* Don't yank the arc out from under an active finger drag: while the arc is
     * pressed, vol_arc_cb already tracks the live value, and a lagging a714 echo
     * would jump it back. Skip the programmatic value/number set in that case. */
    if(!(lv_obj_get_state(g_vol_arc) & LV_STATE_PRESSED)){
        int cap = ui_volume_cap();               /* the full dial is Max Volume, so a drag can't pass it */
        g_vol_suppress = 1;
        lv_arc_set_range(g_vol_arc, 0, cap);
        lv_arc_set_value(g_vol_arc, vol);
        g_vol_suppress = 0;
        char b[12]; snprintf(b, sizeof b, "%d/%d", vol, cap); lv_label_set_text(g_vol_num, b);
    }
    lv_obj_remove_flag(g_vol_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_vol_panel);
    lv_timer_reset(g_vol_timer);
    lv_timer_resume(g_vol_timer);
}
