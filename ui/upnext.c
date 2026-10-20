/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "musicdb.h"
#include "ipc.h"
#include "queue.h"
#include "player_mq.h"
#include "itemmenu.h"
#include "librarydb.h"
#include "i18n.h"
#include "rowdrag.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Up Next (SCR_UPNEXT), the Queue section's page. While the player plays our queue (player_mq.c) it shows the
 * queue from the playing song on and edits it, as yopo designed it: an album is one row that a tap opens or closes, a
 * hold on any row opens the item menu, the handle on a row's right drags it (an album as a whole) and a leftward swipe
 * removes it, with Undo for a few seconds. The playing song and the album holding it stay put.
 * When the player plays its own list (a book, a search result), this page only reads that list (LIST_SONG_0, shuffle
 * order in LIST_SONG_3: mdb_upnext) and jumps within it (ui_queue_jump), never showing a guessed order: while the player
 * is rebuilding the list it says so and retries.
 * The title slides away with the rows (yopo 2026-10-20), as on Library's top page. */
#define UN_SHOW   100                  /* rows shown; the rest is summarised in a footer */
#define UN_FIRST  12                   /* rows built at once (a screenful), the rest a few per tick */
#define UN_RETRY  8                    /* 1 s retries while the player's list is being rebuilt */
#define ROW_W     268                  /* rows stay clear of the round edge */
#define ROW_H     52
#define ROW_GAP   6
#define ROW_P     (ROW_H + ROW_GAP)
#define LIST_Y    72                   /* where the first row starts; the title sits above it and scrolls with it */
#define SWIPE_START  12                /* leftward travel that makes a press on a row a swipe */
#define SWIPE_REMOVE 90                /* released this far left: removed */
#define UNDO_MS   5000
static lv_obj_t   *g_un_list, *g_un_title, *g_un_head;   /* g_un_head: the screen's title */
static lv_timer_t *g_un_fill, *g_un_watch;
static int         g_un_n, g_un_fill_i, g_un_more, g_un_retries, g_un_retry;   /* g_un_retry: last read was transient */
static int         g_un_ours;          /* the list shows our queue (else the player's own list) */

/* the player's own list */
static mdb_qrow_t  g_un_rows[UN_SHOW + 1];
static int         g_un_mode = -1;     /* the play mode the list was read in */
static char        g_un_path[256];     /* the song that was current when the list was read */
static int         g_un_pos;           /* its pos_id then (0 = unknown) */
static unsigned    g_un_seq;           /* its path_seq then: a track change since means the list is stale */

/* our queue: one entry per row shown */
typedef struct {
    queue_row r;
    int64_t   id;                      /* the song's queue id, or the album's unit id */
    int       end;                     /* the first song after this row's own songs */
    int       fixed;                   /* the playing song, or the album holding it: it stays put */
    lv_obj_t *obj;
} un_qrow;
static un_qrow     g_qr[UN_SHOW];
static int         g_q_pending, g_q_dirty, g_undo_wanted;
static lv_obj_t   *g_undo;
static lv_timer_t *g_undo_t;
static uint32_t    g_gest_at;          /* when a swipe on a row last started (lv_tick, 0 = never) */

static void un_rebuild(void);
static void un_rebuild_at(int scroll_y);

static void un_fmt_dur(int ms, char *out, size_t n){
    if(ms <= 0 || ms >= 86400000){ out[0] = 0; return; }   /* 24 h = the length written for a song of unknown length */
    long long t = ((long long)ms + 500) / 1000; snprintf(out, n, "%lld:%02lld", t / 60, t % 60);   /* no int overflow near INT32_MAX */
}

static lv_obj_t *un_note(const char *msg){
    lv_obj_t *l = lv_label_create(g_un_list);
    lv_label_set_text(l, msg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 260);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    return l;
}

static void un_empty(const char *msg){
    theme_kit()->empty_state(KIT_EMPTY_UPNEXT, un_note(msg));
}

/* The title rides on the list's scroll (it isn't in the list: the theme's header places it). */
static void un_head_sync(void){
    if(g_un_head && g_un_list) lv_obj_set_style_translate_y(g_un_head, -lv_obj_get_scroll_y(g_un_list), 0);
}
static void un_scroll_cb(lv_event_t *e){ (void)e; un_head_sync(); }

static lv_obj_t *un_label(lv_obj_t *r, const char *text, int x, int y, int w, int h, const lv_font_t *f, lv_color_t c){
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, x, y); lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}
static lv_color_t un_now_color(void){
    return !strcmp(theme_kit()->id, "terminal") ? TC(ACCENT_PRIMARY) : TC(STATUS_INFO);
}

/* A caption for the play modes where "what plays next" is not simply the next row. Our queue already is the shuffled
 * order, so it needs no shuffle caption. */
static void un_mode_caption(int mode, int ours){
    const char *c = mode == 1 && !ours ? "Shuffle is on - this is the shuffled order"
                  : mode == 2 ? "Repeat One - this song repeats"
                  : mode == 3 ? "Repeat All - the queue starts again after the last song"
                  : mode == 4 ? "Single - playback stops after this song"
                  : NULL;
    if(c) un_note(c);
}

/* ======== the player's own list ======== */

/* The list must still describe what is playing: same track (identity epoch), same queue row (an unknown pos_id on
 * either side counts only if it is unknown on both - a zero-to-known change means the list was read blind), same
 * play mode. */
static int un_still_current(void){
    track_state_t st; ipc_get_state(&st);
    if(!st.have_track || st.path_seq != g_un_seq || strcmp(st.path, g_un_path) != 0) return 0;
    return st.pos_id == g_un_pos && ui_play_mode_now() == g_un_mode;
}

/* Only the current label follows live metadata; queue order and scroll stay intact. */
static void un_update_title(const track_state_t *st){
    if(!g_un_title) return;
    const char *text = "";
    if(st->have_track && st->path_seq == g_un_seq && st->pos_id == g_un_pos &&
       !strcmp(st->path, g_un_path) && !strcmp(st->path, g_un_rows[0].path))
        text = st->title_ready ? (st->title[0] ? st->title : "Untitled") : "";
    if(strcmp(lv_label_get_text(g_un_title), text)) lv_label_set_text(g_un_title, text);
}

static void un_row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_un_n) return;
    if(i == 0){ screen_show(SCR_NOWPLAYING); return; }             /* the current song */
    if(!un_still_current()){ un_rebuild(); ui_toast("Queue changed"); return; }   /* never jump from a stale list */
    /* and the target row itself must still be that song at that position (a rebuild can keep the current song) */
    int v = mdb_queue_row_valid(g_un_rows[i].base_id, g_un_rows[i].ord, g_un_rows[i].path);
    if(v < 0){ ui_toast("Queue is busy - try again"); return; }
    if(v == 0){ un_rebuild(); ui_toast("Queue changed"); return; }
    if(ui_queue_jump(g_un_rows[i].ord) == 0) screen_show(SCR_NOWPLAYING);
}

static void un_add_row(int i){
    const mdb_qrow_t *q = &g_un_rows[i];
    int cur = (i == 0);
    lv_obj_t *r = lv_button_create(g_un_list);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);                  /* a press near the bottom must not scroll the list */
    lv_obj_set_size(r, ROW_W, ROW_H);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, (cur ? TC(SURFACE_RAISED) : TC(SURFACE)), 0);
    lv_obj_set_style_bg_opa(r, cur ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, un_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i, "upnext.un_row", UI_CORE);

    lv_obj_t *t = un_label(r, "", 14, 6, 186, 21, TF(USER_16), TC(TEXT_PRIMARY));   /* title (may be non-Latin) */
    if(cur){
        g_un_title = t;
        track_state_t st; ipc_get_state(&st); un_update_title(&st);
    } else lv_label_set_text(t, q->title[0] ? q->title : "Untitled");

    /* artist; "Now Playing" on the current row */
    lv_obj_t *s = un_label(r, cur ? "Now Playing" : q->artist, 14, 28, 186, 17, cur ? TF(UI_14) : TF(USER_14),
                           cur ? un_now_color() : TC(TEXT_MUTED));

    char dur[12]; un_fmt_dur(q->dur_ms, dur, sizeof dur);
    if(dur[0]){
        lv_obj_t *d = un_label(r, dur, 202, 17, 54, 18, TF(UI_12), TC(TEXT_MUTED));   /* length, right-aligned */
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
        kit_row_value(r, t, s, d, KIT_VALUE_DURATION);
    }
}

static void un_footer(void){
    if(g_un_mode == 2) return;                                  /* Repeat One: the caption already says it */
    if(g_un_n == 1 && !g_un_more){
        un_note(g_un_mode == 3 ? "End of the queue - then it starts again" : "Nothing after this song");
        return;
    }
    if(g_un_more > 0){
        char b[48]; snprintf(b, sizeof b, "+ %d more song%s", g_un_more, g_un_more == 1 ? "" : "s");
        un_note(b);
    }
}

static void un_player_rebuild(void){
    track_state_t st; ipc_get_state(&st);
    g_un_seq = st.path_seq;
    g_un_pos = st.pos_id;                                        /* what the list was read against, even when empty, */
    int mode = ui_play_mode_now();                               /* so the watcher only rebuilds on a real change   */
    g_un_mode = mode;
    if(!st.have_track || !st.path[0]){ un_empty("Nothing playing"); return; }
    if(mdb_is_book_path(st.path)){ un_empty("Audiobooks play one book at a time - use Chapters"); return; }
    snprintf(g_un_path, sizeof g_un_path, "%s", st.path);
    int n = mode < 0 ? MDB_UPNEXT_NOTFOUND : mdb_upnext(mode == 1, st.pos_id, st.path, g_un_rows, UN_SHOW + 1, &g_un_more);
    if(n == MDB_UPNEXT_NOTFOUND || n == MDB_UPNEXT_BUSY){           /* transient: the watch timer retries */
        g_un_retry = 1;
        un_empty(g_un_retries < UN_RETRY ? "Queue updating..." : "The queue isn't available right now");
        return;
    }
    if(n == MDB_UPNEXT_ERROR){ un_empty("Couldn't read the queue"); return; }
    if(n == MDB_UPNEXT_EMPTY){ un_empty("The queue is empty"); return; }
    g_un_retries = 0;
    g_un_n = n;
    un_mode_caption(mode, 0);
}

/* ======== our queue ======== */

static int un_ours_now(void){ return player_owns() && queue_current() >= 0; }

/* Rows drag by their handle (rowdrag.c); g_qdl describes this list to it. */
static int       q_count(void *ctx);
static lv_obj_t *q_row(void *ctx, int k);
static void      q_prepare(void *ctx, int k);
static int       q_accepts(void *ctx, int k, int to, int above, int below);
static void      q_drop(void *ctx, int k, int to, int above, int below);
static void      q_done(void *ctx);
static rowdrag_list g_qdl = { NULL, q_count, q_row, q_prepare, q_accepts, q_drop, q_done, NULL };
static struct {
    unsigned ver;                      /* queue_version() when the drag began: a change meanwhile cancels the drop */
    int kcur;                          /* the playing song's row */
} g_qd;

/* A leftward swipe on a row. down: a finger is on a row (rebuilding then would delete the row under it). */
static struct {
    int armed, on, used, down, k, dx, sy0;
    lv_point_t p0;
} g_sw;

static void un_gesture_mark(void){ g_gest_at = lv_tick_get(); if(!g_gest_at) g_gest_at = 1; }

static void undo_hide(void){
    if(g_undo_t){ lv_timer_delete(g_undo_t); g_undo_t = NULL; }
    if(g_undo) lv_obj_add_flag(g_undo, LV_OBJ_FLAG_HIDDEN);
}
static void undo_timer_cb(lv_timer_t *t){ (void)t; g_undo_t = NULL; if(g_undo) lv_obj_add_flag(g_undo, LV_OBJ_FLAG_HIDDEN); }
static void undo_show(void){
    if(!g_undo) return;
    if(g_undo_t) lv_timer_delete(g_undo_t);
    g_undo_t = lv_timer_create(undo_timer_cb, UNDO_MS, NULL);
    lv_timer_set_repeat_count(g_undo_t, 1);
    lv_obj_remove_flag(g_undo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_undo);
}
static void undo_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    undo_hide();
    if(queue_undo() < 0) ui_toast("Nothing to undo");
}

/* A finger on a row or a drag: a rebuild waits for it to lift (rebuilding deletes the row under the finger, and
 * LVGL then drops the rest of the touch). */
static int q_busy(void){
    if(rowdrag_active(&g_qdl)) return 1;
    if(!g_sw.down) return 0;
    for(lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        if(lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) return 1;
    g_sw.down = 0;                                                       /* its release never reached the row */
    return 0;
}

/* Rebuild once the current event is over, keeping the scroll; while busy, once the finger lifts. */
static void q_async(void *unused){
    (void)unused;
    g_q_pending = 0;
    if(!g_un_list || screen_current() != SCR_UPNEXT) return;
    if(q_busy()){ g_q_dirty = 1; return; }
    un_rebuild_at(lv_obj_get_scroll_y(g_un_list));
    if(g_undo_wanted && g_un_ours && queue_can_undo()) undo_show();
    else if(!queue_can_undo()) undo_hide();
    g_undo_wanted = 0;
}
static void q_schedule(void){
    if(!g_un_list || screen_current() != SCR_UPNEXT) return;   /* shown later: upnext_refresh reads it fresh */
    if(q_busy()){ g_q_dirty = 1; return; }
    if(!g_q_pending){ g_q_pending = 1; lv_async_call(q_async, NULL); }
}
static void un_on_queue(const queue_event *ev, void *ctx){
    (void)ctx;
    if(ev->type == QEV_REMOVE) g_undo_wanted = 1;
    q_schedule();
}

/* Tap: an album opens or closes, a song plays (the playing one shows Now Playing). SHORT_CLICKED: LVGL sends CLICKED
 * after a hold too, which would play the song under the menu the hold just opened. */
static void q_row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED || rowdrag_active(&g_qdl) || g_sw.used) return;
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if(k < 0 || k >= g_un_n) return;
    const un_qrow *q = &g_qr[k];
    if(q->r.kind == QROW_UNIT){
        const queue_unit *u = queue_unit_get(q->id);
        if(u && queue_set_collapsed(q->id, !u->collapsed) < 0) ui_toast("Couldn't change that");
        return;
    }
    int i = queue_find(q->id);
    if(i < 0){ q_schedule(); return; }
    if(i == queue_current()){ screen_show(SCR_NOWPLAYING); return; }
    if(queue_jump(i) == 0) screen_show(SCR_NOWPLAYING);
    else ui_toast("Couldn't play that");
}

/* Hold: the item menu for the song or the album (itemmenu.h). row_id carries its queue id. */
static void q_hold_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_LONG_PRESSED || rowdrag_active(&g_qdl) || g_sw.on) return;
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if(k < 0 || k >= g_un_n) return;
    const un_qrow *q = &g_qr[k];
    im_item it;
    memset(&it, 0, sizeof it);
    it.where = IM_IN_QUEUE;
    it.row_id = q->id;
    if(q->r.kind == QROW_UNIT){
        const queue_unit *u = queue_unit_get(q->id);
        if(!u) return;
        it.kind = IM_ALBUM;
        snprintf(it.title, sizeof it.title, "%s", u->album[0] ? u->album : "Unknown album");
        snprintf(it.album_artist, sizeof it.album_artist, "%s", u->album_artist);
        snprintf(it.album, sizeof it.album, "%s", u->album);
    } else {
        const queue_item *s = queue_get(queue_find(q->id));
        if(!s) return;
        const queue_unit *u = s->unit_id ? queue_unit_get(s->unit_id) : NULL;
        it.kind = IM_SONG;
        snprintf(it.title, sizeof it.title, "%s", s->title[0] ? s->title : "Untitled");
        snprintf(it.path, sizeof it.path, "%s", s->path);
        it.start_ms = s->start_ms;
        if(u){   /* its album's key, as the Library knows it */
            snprintf(it.album_artist, sizeof it.album_artist, "%s", u->album_artist);
            snprintf(it.album, sizeof it.album, "%s", u->album);
        } else if(!ldb_song_album(s->path, s->start_ms, it.album_artist, sizeof it.album_artist, it.album, sizeof it.album)){
            it.album_artist[0] = 0;
            snprintf(it.album, sizeof it.album, "%s", s->album);
        }
    }
    itemmenu_open(&it);
}

/* ---- swipe to remove ---- */
static void q_swipe_back(lv_obj_t *r){
    lv_obj_set_style_translate_x(r, 0, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
}
static void q_swipe_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if(k < 0 || k >= g_un_n) return;
    lv_obj_t *r = g_qr[k].obj;
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if(in) lv_indev_get_point(in, &p);
    if(c == LV_EVENT_PRESSED){
        memset(&g_sw, 0, sizeof g_sw);
        g_sw.down = 1;
        g_sw.armed = !rowdrag_active(&g_qdl) && !g_qr[k].fixed;
        g_sw.k = k; g_sw.p0 = p;
        g_sw.sy0 = lv_obj_get_scroll_y(g_un_list);
        return;
    }
    if(c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST){
        int mine = g_sw.on && g_sw.k == k;
        if(mine && c == LV_EVENT_RELEASED && p.x >= 0 && p.x < 360){   /* the driver can fold the last move into the release */
            int dx = p.x - g_sw.p0.x;
            g_sw.dx = dx < 0 ? dx : 0;
        }
        int remove = mine && c == LV_EVENT_RELEASED && g_sw.dx <= -SWIPE_REMOVE;
        if(mine) q_swipe_back(r);
        g_sw.armed = g_sw.on = g_sw.down = 0;
        if(remove){
            const un_qrow *q = &g_qr[k];
            int rc = q->r.kind == QROW_UNIT ? queue_remove_unit(q->id) : queue_remove_item(queue_find(q->id));
            if(rc < 0) ui_toast("Couldn't remove that");
        }
        if(g_q_dirty){ g_q_dirty = 0; q_schedule(); }                    /* a change that waited for the finger */
        return;
    }
    if(c != LV_EVENT_PRESSING || !g_sw.armed || g_sw.k != k) return;
    int dx = p.x - g_sw.p0.x, dy = p.y - g_sw.p0.y, ady = dy < 0 ? -dy : dy;
    int scrolled = lv_obj_get_scroll_y(g_un_list) != g_sw.sy0 || (in && lv_indev_get_scroll_obj(in));
    if(rowdrag_active(&g_qdl) || itemmenu_is_open() || scrolled){   /* the list moved under it (a scroll, the rim): not a swipe */
        if(g_sw.on) q_swipe_back(r);
        g_sw.armed = g_sw.on = 0;
        return;
    }
    if(!g_sw.on){
        if(ady > SWIPE_START){ g_sw.armed = 0; return; }       /* vertical: the list's scroll */
        if(dx > -SWIPE_START || -dx <= 2 * ady) return;
        g_sw.on = g_sw.used = 1;
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLL_CHAIN_VER);   /* from here the row has the finger: no scroll */
        un_gesture_mark();
    }
    g_sw.dx = dx < 0 ? dx : 0;
    lv_obj_set_style_translate_x(r, g_sw.dx, 0);
    lv_obj_set_style_bg_color(r, g_sw.dx <= -SWIPE_REMOVE ? TC(STATUS_DANGER) : TC(SURFACE), 0);
}

/* ---- drag to reorder (rowdrag.c) ---- */
static void q_fill_all(void);
static int q_count(void *ctx){ (void)ctx; return g_un_n; }
static lv_obj_t *q_row(void *ctx, int k){ (void)ctx; return k >= 0 && k < g_un_n ? g_qr[k].obj : NULL; }

/* Every row built before measuring; an album moves as its header, so its songs hide. */
static void q_prepare(void *ctx, int k){
    (void)ctx;
    q_fill_all();
    g_sw.armed = 0;
    g_qd.ver = queue_version();
    g_qd.kcur = 0;
    for(int j = 0; j < g_un_n; j++)
        if(g_qr[j].r.kind == QROW_ITEM && g_qr[j].r.index == queue_current()){ g_qd.kcur = j; break; }
    if(g_qr[k].r.kind == QROW_UNIT)
        for(int j = k + 1; j < g_un_n && g_qr[j].r.kind == QROW_ITEM && g_qr[j].r.unit_id == g_qr[k].id; j++)
            lv_obj_add_flag(g_qr[j].obj, LV_OBJ_FLAG_HIDDEN);
}

/* Row k dropped between rows ia and ib (-1 = none): the song it lands before, and the album a song stays in (0 = it
 * becomes a loose song). 0 = the queue doesn't allow it there. A song stays in its album only between the album's
 * own rows: dropped above the header or below the last song it leaves the album (a one-song album keeps its song). */
static int q_place(int k, int ia, int ib, int *before, int64_t *into){
    if(ia < 0 || ia < g_qd.kcur) return 0;                               /* never above or at the playing song */
    const un_qrow *d = &g_qr[k], *a = &g_qr[ia], *b = ib >= 0 ? &g_qr[ib] : NULL;
    *before = b ? b->r.index : a->end;
    *into = 0;
    int64_t in = 0;                                                      /* the album the gap is inside */
    if(b) in = b->r.kind == QROW_ITEM && b->r.unit_id && a->r.unit_id == b->r.unit_id ? b->r.unit_id : 0;
    else if(a->r.unit_id){                                               /* the rows stop (UN_SHOW) inside an album */
        const queue_item *nx = queue_get(*before);
        if(nx && nx->unit_id == a->r.unit_id) in = a->r.unit_id;
    }
    if(d->r.kind == QROW_UNIT) return !in && queue_can_move_unit(d->id, *before);
    int64_t own = d->r.unit_id;
    if(in && in != own) return 0;                                        /* never inside another album */
    *into = in ? own : own && queue_unit_range(own, NULL) == 1 ? own : 0;
    int i = queue_find(d->id);
    return i >= 0 && queue_can_move_item(i, *before, *into);
}

static int q_accepts(void *ctx, int k, int to, int above, int below){
    (void)ctx; (void)to;
    int before; int64_t into;
    return q_place(k, above, below, &before, &into);
}

static void q_drop(void *ctx, int k, int to, int above, int below){
    (void)ctx; (void)to;
    int before, rc; int64_t into;
    if(queue_version() != g_qd.ver) rc = 1;                              /* the queue changed under the drag */
    else if(!q_place(k, above, below, &before, &into)) rc = QERR_PLACE;
    else if(g_qr[k].r.kind == QROW_UNIT) rc = queue_move_unit(g_qr[k].id, before);
    else rc = queue_move_item(queue_find(g_qr[k].id), before, into);
    if(rc > 0) ui_toast("Queue changed");
    else if(rc < 0) ui_toast("Couldn't move that");
}

static void q_done(void *ctx){
    (void)ctx;
    g_q_dirty = 0;
    q_schedule();                                                        /* back to plain rows, moved or not */
}

/* ---- rows ---- */
static void q_add_row(int k){
    un_qrow *q = &g_qr[k];
    int cur = queue_current();
    int unit = q->r.kind == QROW_UNIT;
    int now = !unit && q->r.index == cur;                                /* the playing song */
    int fixed = q->fixed = now || (unit && q->r.index == cur);           /* it and its album stay put */
    int inner = !unit && q->r.unit_id;                                   /* a song of an album */
    lv_obj_t *r = q->obj = lv_button_create(g_un_list);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);                  /* a press near the bottom must not scroll the list */
    lv_obj_set_size(r, ROW_W, ROW_H);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, now ? TC(SURFACE_RAISED) : TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, now ? LV_OPA_COVER : unit ? LV_OPA_80 : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);                 /* a sideways move is the row's swipe, not a scroll */
    ui_on(r, q_row_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)k, "upnext.row", UI_CORE);
    ui_on(r, q_hold_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k, "upnext.row.hold", UI_CORE);
    lv_obj_add_event_cb(r, q_swipe_cb, LV_EVENT_PRESSED, (void *)(intptr_t)k);   /* every row: a press holds rebuilds */
    lv_obj_add_event_cb(r, q_swipe_cb, LV_EVENT_PRESSING, (void *)(intptr_t)k);
    lv_obj_add_event_cb(r, q_swipe_cb, LV_EVENT_RELEASED, (void *)(intptr_t)k);
    lv_obj_add_event_cb(r, q_swipe_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)k);
    if(!fixed) rowdrag_handle(r, k, &g_qdl);
    int right = fixed ? 200 : ROW_W - ROWDRAG_HANDLE_W - 56;             /* where the text ends (the length sits after it) */
    if(unit){
        const queue_unit *u = queue_unit_get(q->id);
        un_label(r, u && u->album[0] ? u->album : "Unknown album", 30, 6, right - 30, 21, TF(USER_16), TC(TEXT_PRIMARY));
        char sub[300], cnt[24];
        int n = q->r.count;
        if(fixed) snprintf(cnt, sizeof cnt, "%d to go", n);                /* the playing album: from its playing song on */
        else snprintf(cnt, sizeof cnt, "%d song%s", n, n == 1 ? "" : "s");
        snprintf(sub, sizeof sub, "%s%s%s", u ? u->album_artist : "", u && u->album_artist[0] ? " - " : "", cnt);
        un_label(r, sub, 30, 28, right - 30, 17, TF(USER_14), TC(TEXT_MUTED));
        lv_obj_t *ch = lv_label_create(r);                               /* open / closed; after the name, which themes */
        lv_label_set_text(ch, u && u->collapsed ? LV_SYMBOL_RIGHT : LV_SYMBOL_DOWN);   /* take for the row's title */
        lv_obj_set_pos(ch, 10, 17);
        lv_obj_set_style_text_color(ch, TC(TEXT_MUTED), 0);
        return;
    }
    const queue_item *it = queue_get(q->r.index);
    if(!it) return;
    int x = inner ? 24 : 14;
    if(inner){                                                           /* a line marks the album's songs */
        lv_obj_t *bar = lv_obj_create(r);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(bar, 10, 10); lv_obj_set_size(bar, 3, ROW_H - 20);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
    }
    const char *title = it->title;
    if(!title[0]){ const char *sl = strrchr(it->path, '/'); title = sl ? sl + 1 : it->path; }
    lv_obj_t *t = un_label(r, title[0] ? title : "Untitled", x, 6, right - x, 21, TF(USER_16), TC(TEXT_PRIMARY));
    lv_obj_t *s = un_label(r, now ? "Now Playing" : it->artist, x, 28, right - x, 17, now ? TF(UI_14) : TF(USER_14),
                           now ? un_now_color() : TC(TEXT_MUTED));
    int len = it->end_ms > it->start_ms ? it->end_ms - it->start_ms : it->duration_ms;
    char dur[12]; un_fmt_dur(len, dur, sizeof dur);
    if(dur[0]){
        lv_obj_t *d = un_label(r, dur, right + 2, 17, 54, 18, TF(UI_12), TC(TEXT_MUTED));   /* length, right-aligned */
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
        kit_row_value(r, t, s, d, KIT_VALUE_DURATION);
        if(!fixed){                                                      /* the theme's look, but clear of the handle */
            lv_obj_set_pos(d, right + 2, 17); lv_obj_set_size(d, 54, 18);
            lv_obj_set_width(t, right - x); lv_obj_set_width(s, right - x);
        }
    }
}

static void q_footer(void){
    int mode = ui_play_mode_now();
    if(mode == 2) return;                                                /* Repeat One: the caption already says it */
    if(g_un_more > 0){
        char b[48]; snprintf(b, sizeof b, "+ %d more song%s", g_un_more, g_un_more == 1 ? "" : "s");
        un_note(b);
    } else if(queue_remaining() <= 1)
        un_note(mode == 3 ? "End of the queue - then it starts again" : "Nothing after this song");
}

static void q_rebuild(void){
    g_un_mode = ui_play_mode_now();
    int rows = queue_row_count(), cur = queue_current();
    int n = rows < UN_SHOW ? rows : UN_SHOW;
    for(int k = 0; k < n; k++){
        un_qrow *q = &g_qr[k];
        memset(q, 0, sizeof *q);
        queue_row_get(k, &q->r);
        if(q->r.kind == QROW_UNIT){
            const queue_unit *u = queue_unit_get(q->r.unit_id);
            q->id = q->r.unit_id;
            q->end = u && u->collapsed ? q->r.index + q->r.count : q->r.index;
        } else {
            const queue_item *it = queue_get(q->r.index);
            const queue_unit *u = q->r.unit_id ? queue_unit_get(q->r.unit_id) : NULL;
            q->id = it ? it->id : 0;
            q->end = q->r.index + 1;
            if(u && u->collapsed && k > 0 && g_qr[k - 1].r.kind == QROW_UNIT)   /* the playing song of a closed album */
                q->end = g_qr[k - 1].r.index + g_qr[k - 1].r.count;
        }
    }
    if(n > 1 && n < rows && g_qr[n - 1].r.kind == QROW_UNIT) n--;        /* never end on an album's header */
    g_un_n = n;
    g_un_more = queue_count() - (n ? g_qr[n - 1].end : cur);
    if(g_un_more < 0) g_un_more = 0;
    un_mode_caption(g_un_mode, 1);
}

/* Build the rest at once (a drag measures every row). */
static void un_fill_stop(void){ if(g_un_fill){ lv_timer_delete(g_un_fill); g_un_fill = NULL; } }
static void q_fill_all(void){
    if(!g_un_ours || g_un_fill_i >= g_un_n) return;
    un_fill_stop();
    for(; g_un_fill_i < g_un_n; g_un_fill_i++) q_add_row(g_un_fill_i);
    q_footer();
}

/* ======== both ======== */

static void un_fill_cb(lv_timer_t *t){
    (void)t;
    int end = g_un_fill_i + 20; if(end > g_un_n) end = g_un_n;
    for(; g_un_fill_i < end; g_un_fill_i++){ if(g_un_ours) q_add_row(g_un_fill_i); else un_add_row(g_un_fill_i); }
    if(g_un_fill_i >= g_un_n){ un_fill_stop(); if(g_un_ours) q_footer(); else un_footer(); }
}

/* Read the list again and build it, scrolled to scroll_y (rows up to there are built at once). */
static void un_rebuild_at(int scroll_y){
    if(!g_un_list) return;
    un_fill_stop();
    if(rowdrag_active(&g_qdl)) rowdrag_cancel();
    memset(&g_sw, 0, sizeof g_sw);
    g_q_dirty = 0;
    g_un_title = NULL;
    lv_obj_clean(g_un_list);
    g_un_n = 0; g_un_more = 0; g_un_path[0] = 0; g_un_pos = 0; g_un_retry = 0; g_un_mode = -1;
    lv_obj_t *sp = lv_obj_create(g_un_list);                             /* room for the title, which slides with it */
    lv_obj_remove_style_all(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sp, ROW_W, LIST_Y - ROW_GAP);
    g_un_ours = un_ours_now();
    if(g_un_ours) q_rebuild(); else un_player_rebuild();
    if(!g_un_ours || !queue_can_undo()) undo_hide();
    g_un_fill_i = 0;
    if(g_un_n){
        int first = g_un_ours ? (scroll_y + 360 - LIST_Y) / ROW_P + 2 : 0;
        if(first < UN_FIRST) first = UN_FIRST;
        if(first > g_un_n) first = g_un_n;
        for(; g_un_fill_i < first; g_un_fill_i++){ if(g_un_ours) q_add_row(g_un_fill_i); else un_add_row(g_un_fill_i); }
        if(g_un_fill_i < g_un_n) g_un_fill = lv_timer_create(un_fill_cb, 16, NULL);
        else if(g_un_ours) q_footer(); else un_footer();
    }
    lv_obj_update_layout(g_un_list);
    lv_obj_scroll_to_y(g_un_list, scroll_y, LV_ANIM_OFF);
    un_head_sync();
}
static void un_rebuild(void){ un_rebuild_at(0); }

/* While open: follow track changes, retry a player's list that was being rebuilt, and switch views when the player
 * starts or stops playing our queue. Our queue tells its own changes (un_on_queue). */
static void un_watch_cb(lv_timer_t *t){
    (void)t;
    if(screen_current() != SCR_UPNEXT){
        lv_timer_delete(g_un_watch); g_un_watch = NULL;
        if(rowdrag_active(&g_qdl)) rowdrag_cancel();
        undo_hide();
        return;
    }
    if(un_ours_now() != g_un_ours){ g_un_retries = 0; un_rebuild(); return; }
    if(g_un_ours){
        if(ui_play_mode_now() != g_un_mode || (g_q_dirty && !q_busy())){ g_q_dirty = 0; q_schedule(); }
        return;
    }
    if(g_un_retry && g_un_retries < UN_RETRY){ g_un_retries++; un_rebuild(); return; }
    /* a new track, a new queue row for the same file (CUE), a pos_id that became known, or a new play mode */
    track_state_t st; ipc_get_state(&st);
    if(st.path_seq != g_un_seq || st.pos_id != g_un_pos || ui_play_mode_now() != g_un_mode){ g_un_retries = 0; un_rebuild(); }
    else un_update_title(&st);
}

static void un_list_deleted(lv_event_t *e){
    (void)e;
    un_fill_stop();
    memset(&g_sw, 0, sizeof g_sw);                                       /* a drag forgets its rows itself (rowdrag.c) */
    if(g_q_pending){ lv_async_call_cancel(q_async, NULL); g_q_pending = 0; }
    if(g_undo_t){ lv_timer_delete(g_undo_t); g_undo_t = NULL; }
    if(g_un_watch){ lv_timer_delete(g_un_watch); g_un_watch = NULL; }
    g_un_list = g_qdl.list = NULL; g_un_title = NULL; g_un_head = NULL; g_undo = NULL; g_un_n = 0;
}

void upnext_create(lv_obj_t *root){
    static int listening;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    g_un_head = ui_header(root, "Up Next");
    { lv_obj_t *b = ui_header_back(); if(b) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN); }   /* the Queue section's root: Back leads nowhere */

    g_un_list = g_qdl.list = lv_obj_create(root);                         /* the whole height: the title scrolls with it */
    lv_obj_remove_style_all(g_un_list);
    lv_obj_set_pos(g_un_list, 30, 0); lv_obj_set_size(g_un_list, 300, 360);   /* to the screen's bottom edge */
    lv_obj_set_style_pad_bottom(g_un_list, 60, 0);                 /* last row stops clear of the round bezel (y 300) */
    lv_obj_set_style_bg_opa(g_un_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_un_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_un_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_un_list, ROW_GAP, 0);
    lv_obj_set_scroll_dir(g_un_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_un_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_un_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(g_un_list, un_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(g_un_list, un_list_deleted, LV_EVENT_DELETE, NULL);

    g_undo = lv_button_create(root);                                     /* a few seconds after a remove */
    lv_obj_remove_style_all(g_undo);
    lv_obj_set_pos(g_undo, 120, 292); lv_obj_set_size(g_undo, 120, 40);
    lv_obj_set_style_radius(g_undo, 20, 0);
    lv_obj_set_style_bg_color(g_undo, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_undo, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_undo, TC(RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(g_undo, 1, 0);
    lv_obj_set_style_border_color(g_undo, TC(ACCENT_PRIMARY), 0);
    ui_on(g_undo, undo_cb, LV_EVENT_CLICKED, NULL, "upnext.undo", UI_REACHABLE);
    lv_obj_t *ul = lv_label_create(g_undo);
    lv_label_set_text(ul, tr("Undo"));
    lv_obj_set_style_text_font(ul, TF(UI_16), 0);
    lv_obj_set_style_text_color(ul, TC(TEXT_PRIMARY), 0);
    lv_obj_center(ul);
    lv_obj_add_flag(g_undo, LV_OBJ_FLAG_HIDDEN);

    if(!listening) listening = queue_listen(un_on_queue, NULL) == 0;
}

/* Called on every SCR_UPNEXT show (screenmgr), so a deep link or a back-navigation also reads the queue fresh. */
void upnext_refresh(void){
    g_un_retries = 0;
    g_undo_wanted = 0;
    un_rebuild();
    if(!g_un_watch) g_un_watch = lv_timer_create(un_watch_cb, 1000, NULL);
}

void upnext_open(void){ screen_show(SCR_UPNEXT); }

/* For the touch loop (main.c): the list the rim fast-scrolls, and whether a row swipe started with the press made at
 * `since`, so its release is no swipe or edge pull (row drags: rowdrag_owns_gesture). The press reaches LVGL a moment
 * before the touch loop stamps it, hence the slack. */
lv_obj_t *upnext_scroller(void){ return g_un_list; }
int upnext_owns_gesture(uint32_t since){
    return g_sw.on || (g_gest_at && (int32_t)(g_gest_at - since) >= -150);
}
