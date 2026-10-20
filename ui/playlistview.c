/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "i18n.h"
#include "theme.h"
#include "theme_kit.h"
#include "player_mq.h"     /* play the playlist as our queue */
#include "queuemenu.h"
#include "shelvesdb.h"
#include "playlists.h"
#include "itemmenu.h"
#include "rowdrag.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Playlist detail (SCR_PLVIEW): opened by tapping a playlist in the Library. Header name + Play / Play next / Add to queue / Shuffle icons + a 3-dot
 * button that opens the playlist's hold menu (Rename, Save to the card, Delete...). The songs come from shelves.db;
 * holding one opens its hold menu, with Remove from playlist. */

static long long g_pid;
static char g_name[LDB_NAME];
static lv_obj_t *g_title_lbl, *g_song_list;
static lv_obj_t *g_play_btn, *g_next_btn, *g_last_btn, *g_shuffle_btn;   /* LV_STATE_DISABLED when the playlist is empty */
static ldb_list g_pl;                          /* its songs, kept while the rows use them */
static lv_obj_t *g_menu_btn, *g_done_btn;
static int g_reorder;                          /* Reorder: a handle on each row, and Done in place of the buttons */
static lv_obj_t **g_rowobj;                    /* Reorder: the rows built, by song */
static int g_dropped;
static rowdrag_list g_drag;                    /* set up in plview_create */

/* Enable/disable the transport buttons for an empty playlist: at 0 songs Play/Shuffle would only
 * toast, so show them disabled. LV_STATE_DISABLED (not clearing CLICKABLE) so the theme kit still
 * classifies and styles them like a populated playlist's, and LVGL sends no press/click to them.
 * The disabled look (one neutral colour for both labels, faded button) is set up in plview_create. */
/* Play / Play next / Add to queue / Shuffle / the menu, or Done while reordering */
static void plv_show_mode(void){
    lv_obj_t *b[6] = { g_play_btn, g_next_btn, g_last_btn, g_shuffle_btn, g_menu_btn, g_done_btn };
    for(int i = 0; i < 6; i++){
        if(!b[i]) continue;
        if((i == 5) == (g_reorder != 0)) lv_obj_remove_flag(b[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(b[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void pl_set_transport_enabled(int on){
    lv_obj_t *btns[4] = { g_play_btn, g_next_btn, g_last_btn, g_shuffle_btn };
    for(int i=0;i<4;i++){
        if(!btns[i]) continue;
        if(on) lv_obj_remove_state(btns[i], LV_STATE_DISABLED); else lv_obj_add_state(btns[i], LV_STATE_DISABLED);
        for(uint32_t c = 0; c < lv_obj_get_child_count(btns[i]); c++){   /* the icon and its small mark */
            lv_obj_t *l = lv_obj_get_child(btns[i], (int32_t)c);
            if(on) lv_obj_remove_state(l, LV_STATE_DISABLED); else lv_obj_add_state(l, LV_STATE_DISABLED);
        }
    }
}

/* ---- playing ------------------------------------------------------------ */
/* Play the playlist from song i (-1 = the first that can play, -2 = a random one) as the new queue: loose songs in the
 * playlist's order, by their files, so songs outside the library play too. Songs whose file is gone are left out.
 * 1 = sent; every failure toasts once (here or in player_play). */
static int plv_play(int i){
    int n = g_pl.n;
    if(n <= 0){ ui_toast("Playlist is empty"); return 0; }
    if(i >= n) return 0;
    if(i >= 0 && g_pl.v[i].count == 2){ ui_toast("This song's file is gone"); return 0; }   /* never start another song */
    if(i == -2) i = rand() % n;
    else if(i < 0) i = 0;
    for(int k = 0; g_pl.v[i].count == 2 && k < n; k++) i = (i + 1) % n;   /* Play / Shuffle: the next song that can play */
    if(g_pl.v[i].count == 2){ ui_toast("These songs' files are gone"); return 0; }
    queue_track *t = malloc((size_t)n * sizeof *t);
    if(!t){ ui_toast("Couldn't play this playlist"); return 0; }
    int m = 0, start = 0;
    for(int k = 0; k < n; k++){
        const ldb_item *s = &g_pl.v[k];
        if(s->count == 2) continue;
        if(k == i) start = m;
        int sub = s->source == LDB_SRC_CUE || s->source == LDB_SRC_ISO;   /* a track inside a bigger file */
        t[m++] = (queue_track){ .path = s->path, .start_ms = (int)s->start_ms,
                                .end_ms = sub && s->duration_ms > 0 ? (int)s->start_ms + s->duration_ms : 0,
                                .duration_ms = s->duration_ms, .title = s->title, .artist = s->artist, .album = s->album };
    }
    int ok = player_play(t, m, NULL, start);
    free(t);
    return ok;
}

/* ---- song list ---------------------------------------------------------- */
static void plv_reload(void);   /* fwd */
/* Hold a song: its hold menu, with Remove from playlist. user data = the 1-based row. */
static void plv_hold_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_LONG_PRESSED || g_reorder) return;   /* Reorder: rows only move */
    int i = (int)(intptr_t)lv_event_get_user_data(e) - 1;
    if(i < 0 || i >= g_pl.n) return;
    const ldb_item *s = &g_pl.v[i];
    im_item it;
    memset(&it, 0, sizeof it);
    it.kind = IM_SONG;
    snprintf(it.title, sizeof it.title, "%s", s->title);
    snprintf(it.path, sizeof it.path, "%s", s->path);
    it.start_ms = s->start_ms;
    snprintf(it.album_artist, sizeof it.album_artist, "%s", s->album_artist ? s->album_artist : "");
    snprintf(it.album, sizeof it.album, "%s", s->album ? s->album : "");
    it.where = IM_IN_PLAYLIST; it.where_id = g_pid; it.row_id = s->row_id;
    itemmenu_open(&it);
}
static void plv_song_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_SHORT_CLICKED || g_reorder) return;   /* SHORT_CLICKED so a hold never also plays */
    if(plv_play((int)(intptr_t)lv_event_get_user_data(e) - 1)) screen_show(SCR_NOWPLAYING);   /* refused: stay (toasted) */
}
/* Gaps preserve scroll extent and row-index decoration while rows stream.
 * Theme row marks repeat every three children, so a gap keeps that phase. */
typedef struct plv_gap {
    int lo, hi;
    lv_obj_t *pad[3];
    struct plv_gap *next;
} plv_gap_t;
static plv_gap_t *g_gaps;
static unsigned char g_built[8192];
static int g_fill_i, g_fill_n, g_loading;
static int g_two_line;   /* some snapshot row has both a title and an artist line */
static lv_timer_t *g_fill_timer;

/* A theme that numbers rows (Blueprint) reads the snapshot here, never the rows built so far: the row count, whether
 * any row is two-line, and a built row's 1-based position (0 = not a song row of this list). */
int playlistview_row_pos(lv_obj_t *row, int *rows, int *two_line){
    if(rows) *rows = g_fill_n;
    if(two_line) *two_line = g_two_line;
    if(!row || !g_song_list || lv_obj_get_parent(row) != g_song_list) return 0;
    return (int)(intptr_t)lv_obj_get_user_data(row);
}

static void plv_fill_stop(void){
    if(g_fill_timer){ lv_timer_del(g_fill_timer); g_fill_timer = NULL; }
}
static void plv_gaps_free(void){
    while(g_gaps){ plv_gap_t *next = g_gaps->next; free(g_gaps); g_gaps = next; }
}
static int plv_gap_size(plv_gap_t *g){
    int n = g->hi - g->lo;
    int pads = n ? (n % 3 ? n % 3 : 3) : 0;
    for(int j=0;j<3;j++){
        if(j >= pads){ if(g->pad[j]) lv_obj_delete(g->pad[j]); g->pad[j] = NULL; continue; }
        if(!g->pad[j]){
            g->pad[j] = lv_obj_create(g_song_list);
            lv_obj_remove_style_all(g->pad[j]);
            lv_obj_clear_flag(g->pad[j], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            kit_keep(g->pad[j]);
        }
        lv_obj_set_size(g->pad[j], 268, j ? 46 : (n - pads + 1)*52 - 6);
    }
    return pads;
}
static void plv_add_row(int i){
    if(i < 0 || i >= g_fill_n || g_built[i]) return;
    plv_gap_t **link = &g_gaps;
    while(*link && (*link)->hi <= i) link = &(*link)->next;
    plv_gap_t *gap = *link;
    if(!gap || i < gap->lo) return;
    plv_gap_t *right = NULL;
    if(i > gap->lo && i+1 < gap->hi){
        right = calloc(1, sizeof *right);
        if(!right) return;
        right->lo = i+1; right->hi = gap->hi; right->next = gap->next;
    }
    int index = lv_obj_get_index(gap->pad[0]);
    lv_obj_t *r = lv_button_create(g_song_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 46);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, TC(LIST_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(r, (void*)(intptr_t)(i+1));   /* playlistview_row_pos */
    ui_on(r, plv_song_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)(i+1), "playlistview.plv_song", UI_CORE);
    ui_on(r, plv_hold_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)(i+1), "playlistview.plv_hold.long", UI_CORE);
    const ldb_item *s = &g_pl.v[i];
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, s->title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    int w = g_reorder ? 196 : 244;   /* Reorder: clear of the handle */
    lv_obj_set_pos(t, 12, 5); lv_obj_set_size(t, w, 19);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* CJK titles like Library/Search */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_t *a = lv_label_create(r);
    /* a song not in the library says so: "Missing" when its file is gone too */
    lv_label_set_text(a, s->count == 2 ? tr("Missing") : s->count == 1 ? tr("Not in the library") : s->artist ? s->artist : "");
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(a, 12, 25); lv_obj_set_size(a, w, 16);
    lv_obj_set_style_text_font(a, TF(USER_14), 0);
    lv_obj_set_style_text_color(a, TC(TEXT_SECONDARY), 0);
    if(!g_reorder && !strcmp(theme_kit()->id, "terminal") && s->duration_ms > 0){
        long long sec = ((long long)s->duration_ms + 500) / 1000;
        char dur[16]; snprintf(dur, sizeof dur, "%lld:%02lld", sec / 60, sec % 60);
        lv_obj_t *d = lv_label_create(r);
        lv_label_set_text(d, dur);
        lv_obj_set_pos(d, 198, 17); lv_obj_set_size(d, 58, 18);
        lv_obj_set_style_text_font(d, TF(UI_14), 0);
        lv_obj_set_style_text_color(d, TC(TEXT_SECONDARY), 0);
        kit_row_value(r, t, a, d, KIT_VALUE_DURATION);
    }
    theme_list_row(r);
    if(g_rowobj){ rowdrag_handle(r, i, &g_drag); g_rowobj[i] = r; }

    g_built[i] = 1;
    if(i == gap->lo){
        gap->lo++;
        int pads = plv_gap_size(gap);
        lv_obj_move_to_index(r, index++);
        for(int j=0;j<pads;j++) lv_obj_move_to_index(gap->pad[j], index++);
        if(!pads){ *link = gap->next; free(gap); }
        return;
    }
    gap->hi = i;
    int left = plv_gap_size(gap);
    int tail = right ? plv_gap_size(right) : 0;
    for(int j=0;j<left;j++) lv_obj_move_to_index(gap->pad[j], index++);
    lv_obj_move_to_index(r, index++);
    for(int j=0;j<tail;j++) lv_obj_move_to_index(right->pad[j], index++);
    if(left){ if(right) gap->next = right; }
    else { *link = right ? right : gap->next; free(gap); }
}
static void plv_visible(void){
    if(g_loading || g_fill_n <= 0) return;
    g_loading = 1;
    int first = lv_obj_get_scroll_y(g_song_list) / 52;
    if(first < 0) first = 0;
    int end = first + 7; if(end > g_fill_n) end = g_fill_n;
    for(int i=first;i<end;i++) plv_add_row(i);
    g_loading = 0;
}
static void plv_scroll_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_SCROLL && screen_current()==SCR_PLVIEW) plv_visible();
}
static void plv_fill_cb(lv_timer_t *t){
    (void)t;
    if(screen_current()!=SCR_PLVIEW){ plv_fill_stop(); return; }
    uint32_t start = lv_tick_get();
    int end = g_fill_i + 40; if(end > g_fill_n) end = g_fill_n;
    g_loading = 1;
    do { plv_add_row(g_fill_i++); }
    while(g_fill_i < end && lv_tick_elaps(start) < 4);
    g_loading = 0;
    if(g_fill_i >= g_fill_n) plv_fill_stop();
}
static void plv_reload(void){
    if(shdb_name(0, g_pid, g_name, sizeof g_name) != 1 && !g_name[0]) snprintf(g_name, sizeof g_name, "%s", tr("Playlist"));
    theme_title_text(g_title_lbl, g_name);
    if(!g_song_list) return;
    g_loading = 1;
    if(rowdrag_active(&g_drag)) rowdrag_cancel();
    plv_fill_stop();
    plv_gaps_free();
    lv_obj_clean(g_song_list);   /* existing entry/removal scroll reset */
    free(g_rowobj); g_rowobj = NULL;
    memset(g_built, 0, sizeof g_built);
    g_fill_i = g_fill_n = g_two_line = 0;
    ldb_list_free(&g_pl);
    int got = shdb_ready() ? ldb_read_playlist(g_pid, &g_pl) : -1;
    playlists_mark_missing(&g_pl);
    int n = got;
    if(n > 8192) n = 8192;   /* display cap only; playback and Shuffle still use the full playlist */
    if(n > 0) g_gaps = calloc(1, sizeof *g_gaps);
    if(n > 0 && !g_gaps) n = 0;
    if(g_reorder && n >= 2) g_rowobj = calloc((size_t)n, sizeof *g_rowobj);
    if(!g_rowobj) g_reorder = 0;   /* nothing to reorder */
    plv_show_mode();
    pl_set_transport_enabled(n > 0);
    if(n<=0){
        plv_fill_stop();
        lv_obj_t *l = lv_label_create(g_song_list);
        lv_label_set_text(l, got < 0 ? tr("Can't read this playlist") : tr("Empty playlist\nHold a song, then Add to playlist"));
        lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
        lv_obj_set_style_text_font(l, TF(UI_14), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        g_loading = 0;
        return;
    }
    g_fill_n = n;
    g_two_line = 1;   /* every row has a second line: the artist, or why it can't play */
    g_gaps->hi = n;
    plv_gap_size(g_gaps);
    int first = n < 18 ? n : 18;
    for(;g_fill_i<first;g_fill_i++) plv_add_row(g_fill_i);
    g_loading = 0;
    if(g_fill_i < n) g_fill_timer = lv_timer_create(plv_fill_cb, 16, NULL);
    else plv_fill_stop();
    { static int hinted = 0; if(!hinted) hinted = ui_toast_hint("Hold a song for more"); }
}
/* Public: rebuild the list from the DB. Called by the screen manager on EVERY entry
 * to SCR_PLVIEW (incl. back-nav), so tap positions can't go stale after the playlist's
 * membership changes elsewhere (e.g. add-to-playlist from Now Playing). */
void plview_refresh(void){ g_reorder = 0; plv_reload(); }
long long plview_current(void){ return g_pid; }
/* A song removed or the playlist renamed while it shows: read it again where it was. */
void plview_changed(void){
    if(screen_current() != SCR_PLVIEW) return;
    int32_t y = g_song_list ? lv_obj_get_scroll_y(g_song_list) : 0;
    plv_reload();
    if(g_song_list && y > 0) lv_obj_scroll_to_y(g_song_list, y, LV_ANIM_OFF);
}

void plview_open(long pid, const char *name){
    g_pid = pid;
    snprintf(g_name, sizeof g_name, "%s", name ? name : "");
    screen_show(SCR_PLVIEW);   /* transition() calls plview_refresh() -> plv_reload() */
}

/* ---- Reorder (yopo 2026-10-20) ------------------------------------------ */
/* The hold menu's Reorder puts a handle (rowdrag) on each song; a drag moves the song there, Done ends it. */
static int plv_drag_count(void *ctx){ (void)ctx; return g_rowobj ? g_fill_n : 0; }
static lv_obj_t *plv_drag_row(void *ctx, int i){ (void)ctx; return g_rowobj && i >= 0 && i < g_fill_n ? g_rowobj[i] : NULL; }
static void plv_drag_prepare(void *ctx, int from){   /* every row built (no spacers left) before the drag measures */
    (void)ctx; (void)from;
    g_loading = 1;
    for(int i = 0; i < g_fill_n; i++) plv_add_row(i);
    g_loading = 0;
    plv_fill_stop();
}
static void plv_drag_drop(void *ctx, int from, int to, int above, int below){
    (void)ctx; (void)above; (void)below;
    if(from < 0 || from >= g_pl.n) return;
    if(shdb_pl_item_place(g_pl.v[from].row_id, to) != 1) ui_toast("Couldn't move that");
    g_dropped = 1;
}
static void plv_changed_async(void *unused){ (void)unused; plview_changed(); }
static void plv_drag_done(void *ctx){
    (void)ctx;
    if(g_dropped){ g_dropped = 0; lv_async_call(plv_changed_async, NULL); }   /* the rows' indexes changed */
}
void plview_reorder(int on){
    g_reorder = on;
    plview_changed();
}
static void done_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) plview_reorder(0);
}

/* ---- play / shuffle ----------------------------------------------------- */
static void play_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(g_pl.n < 1){ ui_toast("Playlist is empty"); return; }
    ui_set_workmode(0);
    if(plv_play(-1)) screen_show(SCR_NOWPLAYING);
}
static void shuffle_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(g_pl.n < 1){ ui_toast("Playlist is empty"); return; }
    srand(lv_tick_get());
    ui_set_workmode(1);                       /* shuffle play-mode */
    if(plv_play(-2)) screen_show(SCR_NOWPLAYING);   /* + random start track */
}
/* Play next / Add to queue: the playlist's songs, loose, into our queue (as the hold menu's entries) */
static void plv_queue(int how){
    if(g_pl.n < 1){ ui_toast("Playlist is empty"); return; }
    if(!player_owns() || queue_current() < 0){ ui_toast("Works while your queue plays"); return; }
    queuemenu_songs(g_pl.v, g_pl.n, NULL, how);
}
static void next_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) plv_queue(QM_NEXT); }
static void last_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) plv_queue(QM_LAST); }

/* ---- 3-dot: the playlist's hold menu ------------------------------------ */
static void open_menu_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    im_item it;
    memset(&it, 0, sizeof it);
    it.kind = IM_PLAYLIST; it.id = g_pid; it.where = IM_IN_PLAYLISTS;
    snprintf(it.title, sizeof it.title, "%s", g_name);
    itemmenu_open(&it);
}

/* ---- screen ------------------------------------------------------------- */
static lv_obj_t *icon_btn(lv_obj_t *root, int x, int y, int w, const char *sym,
                          lv_color_t fg, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_ext_click_area(b, 6);   /* easier hit, esp. the 36px menu button */
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(b, cb, LV_EVENT_CLICKED, NULL, "playlistview.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_width(l, w - 8);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return b;
}
/* An icon-only button, as on an album's page; badge = the small mark (up: Play next, plus: Add to queue) */
static lv_obj_t *icon_only(lv_obj_t *root, int x, const char *sym, const char *badge, int up, lv_event_cb_t cb){
    lv_obj_t *b = icon_btn(root, x, 68, 52, sym, TC(TEXT_PRIMARY), cb);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(l, TF(UI_18), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, badge ? -4 : 0, 0);
    if(badge){
        lv_obj_t *k = lv_label_create(b);
        lv_label_set_text(k, badge);
        lv_obj_set_style_text_font(k, TF(UI_12), 0);
        lv_obj_set_style_text_color(k, TC(TEXT_PRIMARY), 0);
        lv_obj_align(k, LV_ALIGN_CENTER, 11, up ? -9 : 9);
    }
    lv_obj_set_style_opa(b, LV_OPA_60, LV_STATE_DISABLED);   /* empty playlist: faded, one neutral colour */
    for(uint32_t c = 0; c < lv_obj_get_child_count(b); c++)
        lv_obj_set_style_text_color(lv_obj_get_child(b, (int32_t)c), TC(TEXT_DISABLED), LV_STATE_DISABLED);
    return b;
}
void plview_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title_lbl = library_header(root, "Playlist");   /* back arrow / title: Library's top page; dynamic title = playlist name */
    lv_obj_set_style_text_align(g_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    if(!ui_edge_nav()) lv_obj_align(g_title_lbl, LV_ALIGN_TOP_MID, 0, 30);   /* edge-nav themes place their own */

    /* Play / Play next / Add to queue / Shuffle / 3-dots row, icons only as on an album's page:
     * 4 x 52 + 36 + 4 x 6 = 268, centred (46..314) */
    g_play_btn    = icon_only(root, 46,  LV_SYMBOL_PLAY, NULL, 0, play_cb);
    g_next_btn    = icon_only(root, 104, LV_SYMBOL_LIST, LV_SYMBOL_UP, 1, next_cb);
    g_last_btn    = icon_only(root, 162, LV_SYMBOL_LIST, LV_SYMBOL_PLUS, 0, last_cb);
    g_shuffle_btn = icon_only(root, 220, LV_SYMBOL_SHUFFLE, NULL, 0, shuffle_cb);
    /* "more/options" (edit/rename/delete). Drawn "three dots" - Apple-Music-style More - because the
     * bullet glyph isn't in this tree's montserrat; LV_SYMBOL_LIST misread as a track list. */
    lv_obj_t *menu_btn = g_menu_btn = icon_btn(root, 278, 68, 36, "", TC(TEXT_SECONDARY), open_menu_cb);
    for(int i=0;i<3;i++){
        lv_obj_t *d = lv_obj_create(menu_btn);
        lv_obj_remove_style_all(d);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);   /* decorative: let taps fall through to the button */
        lv_obj_set_size(d, 4, 4);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, TC(TEXT_SECONDARY), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_CENTER, (i-1)*7, 0);   /* -7 / 0 / +7 px */
    }

    g_song_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_song_list);
    lv_obj_set_pos(g_song_list, 41, 118);   /* its 268 px rows (5 px in) land centred on the screen */
    lv_obj_set_size(g_song_list, 290, 242);   /* down to the screen's bottom edge */
    lv_obj_set_style_pad_bottom(g_song_list, 60, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_song_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_song_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_song_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_song_list, 6, 0);
    lv_obj_set_scroll_dir(g_song_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_song_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_song_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(g_song_list, plv_scroll_cb, LV_EVENT_SCROLL, NULL);

    /* Reorder: Done takes the place of the buttons */
    g_done_btn = icon_btn(root, 60, 68, 240, tr_sym(LV_SYMBOL_OK, "Done"), TC(ACCENT_PRIMARY), done_cb);
    lv_obj_add_flag(g_done_btn, LV_OBJ_FLAG_HIDDEN);
    g_drag = (rowdrag_list){ .list = g_song_list, .count = plv_drag_count, .row = plv_drag_row,
                             .prepare = plv_drag_prepare, .drop = plv_drag_drop, .done = plv_drag_done };
}

lv_obj_t *playlistview_scroller(void){ return g_song_list; }
