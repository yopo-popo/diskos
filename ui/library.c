/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "folderbrowser.h"
#include "books.h"
#include "musicdb.h"
#include "listeningdb.h"   /* Most Played / Recent */
#include "shelvesdb.h"     /* Favourites */
#include "sdio.h"
#include "artcache.h"
#include "fonts_hebrew.h"
#include "fonts_icons.h"
#include "lvgl/src/indev/lv_indev_private.h"   /* vrow_held: the object a pointer holds */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include "config.h"
#include "i18n.h"

enum { VIEW_MENU, VIEW_SONGS, VIEW_ALBUMS, VIEW_ARTISTS, VIEW_PLAYLISTS, VIEW_FAVS, VIEW_GENRES, VIEW_GROUP, VIEW_MOSTPLAYED, VIEW_RECENT, VIEW_HISTORY, VIEW_COUNT };
#define CAT_FOLDERS 1000   /* menu sentinel: opens the folder browser (a screen, not a library view) */
#define CAT_BOOKS   1001   /* menu sentinel: opens the Books (audiobook) screen */

#define ROW_H 52
#define LIST_Y 70
#define LIST_H 252

static lv_obj_t *g_list;
static lv_obj_t *g_title;
static lv_obj_t *g_status;   /* the clock and battery line: shown on the Library root only */
static lv_obj_t *g_az_btn;    /* "A-Z" button -> opens the letter grid */
static lv_obj_t *g_grid;      /* full alphabet grid overlay */
static lv_obj_t *g_az_page[2];  /* the grid's letter pages: 0 English, 1 Hebrew */
static int       g_az_lang = 0; /* the page the grid shows: 0 English, 1 Hebrew */
static lv_obj_t *g_lhint = NULL, *g_lhint_lbl = NULL;  /* rim-scroll A-Z position hint */
static uint32_t  g_lhint_tick = 0;
static lv_timer_t *g_lhint_timer;
static unsigned char g_lhint_ch = 0;
static const lv_font_t *g_lhint_font = NULL;  /* the hint's own (theme) face, restored after a Hebrew letter */
static int g_view = VIEW_MENU;
static int g_drill_kind = 0;  /* 1 album, 2 artist, 3 genre */
/* Artist / Genre drills open on their ALBUMS (parity L23): level 1 = the album list (an "All Songs" row first), level 2 =
 * one album's songs by that artist / in that genre, level 0 = the flat all-songs drill (the old view; also a deep link). */
static int  g_scope_lvl = 0;
static int  g_scope_from_albums = 0;   /* the flat list was opened from the albums list ("All Songs"): Back returns there */
static char g_scope_album_artist[LDB_NAME], g_scope_album[LDB_NAME];   /* level 2: the open album's key */
static int  g_scope_total = 0;   /* songs in the whole scope (the "All Songs" row's count) */
static int g_deeplink = 0;    /* drill opened from the NP hub -> back returns to where Library was (g_prelink) */
static int g_has_header = 0;   /* a Play All / Shuffle row is the first list child */
static int g_hdr_extra_px = 0; /* extra leading height above the rows (the album cover header), for scroll math */
static char g_drill[LDB_NAME];   /* the open album's title, or the artist / genre */
static library_song_click_cb_t g_song_cb;

/* A-Z scratch, grown on entry. */
static int  g_first_cap = 0;
static unsigned char *g_first = NULL;   /* per-row jump-letter code (az_code) for the A-Z scrubber */
static int  g_count;

static void lib_ensure_first_cap(int need);
static char (*g_plnames)[MDB_STR] = NULL;   /* dynamic: grown to the real playlist count (no 64 cap) */
static long *g_plids = NULL;
static int  *g_plcounts = NULL;
static int   g_plcap = 0;                    /* allocated slots in g_plnames/g_plids/g_plcounts */

/* A view's rows: only those near the viewport exist as objects. LVGL moves, lays out and styles every child of a list on
 * each scroll step, so building all 10,000 rows of Songs made scrolling crawl (and a streamed fill kept adding to it).
 * A spacer as tall as all the rows keeps their place in the list, below any headers; the rows near the viewport are its
 * children at their own y, made and deleted as the list scrolls (vlist_sync). */
#define ROW_PITCH  (ROW_H + 4)          /* a row and the list's pad_row */
#define VROW_EXTRA 3                    /* rows kept past each edge of the viewport */
#define VROW_MAX   40
static lv_obj_t *g_vspace;              /* the spacer; NULL while the view has no rows */
static struct { lv_obj_t *obj; int idx; } g_vrow[VROW_MAX];
static int g_vrows;

/* Songs, Albums, Artists, Genres and their songs come from diskOS's own library.db (librarydb), in its order: English,
 * Hebrew, then '#'. The rows on screen carry their index into these lists. They play through the reserved custom-playlist
 * slot in that same order, as the stock player only plays its song.db rows (mdb_song_ids_for finds them). */
static ldb_list g_lsongs, g_lalbums, g_lalbum;
static ldb_list g_lartists, g_lgenres;  /* the Artists and Genres views */
static ldb_list g_lhist;                /* Most Played / Recent */
static ldb_list g_lfavs;                /* Favourites (shelves.db); count 1 = not in the library, 2 = its file is gone */
static int g_lartists_by_aa = -1;       /* the Artists grouping g_lartists was read in (mdb_artist_mode) */
static ldb_list g_lscope, g_lscope_songs;   /* an artist's / a genre's albums (level 1) and songs (level 0 or 2) */
static const ldb_list *g_lcur;          /* the song list on screen (Songs, an album, an artist's or a genre's songs) */
static int *g_lids;                     /* SONG.IDs of g_lcur's songs, found when it is first played */
static int  g_lids_ok;
static char g_alb_artist[LDB_NAME], g_alb_title[LDB_NAME];   /* the open album (g_drill_kind 1): its exact key */
/* The cover flow over Library (Album View = cover flow): Albums opens it on every album, the Artists list on one artist's.
 * Its covers open their songs here (library_wall_open / library_wall_open_album); Back from those returns to the wall,
 * and Back from the wall to the view it was opened from, where it was. */
static int     g_from_wall = 0;      /* the open drill came from the cover flow: Back returns to it */
static int     g_wall_kind = 0;      /* an artist's wall: the LDB_SCOPE_* its albums were read in (the Artists grouping then) */
static int     g_wall_view = -1;     /* the view the wall was opened from (-1 = none) */
static int     g_wall_pending = 0;   /* left a drill for the wall: the view is rebuilt on the next entry */
static int32_t g_wall_y = -1;        /* that view's scroll when the wall opened */
static void wall_opened(void){ g_from_wall = 0; g_wall_view = g_view; g_wall_y = g_list ? lv_obj_get_scroll_y(g_list) : 0; }

static void library_reload(void);

/* Jump-letter codes, one byte per row in g_first[]: 'A'..'Z' for Latin, '#' for anything else, and AZ_HEB+n for the
 * Hebrew letter alef+n (U+05D0+n, UTF-8 D7 90+n; tav is n=26). A final form counts as its base letter, which Unicode
 * puts right after it (kaf sofit U+05DA, kaf U+05DB), so finals fold with +1 and their codes never occur. Within one
 * alphabet the codes keep its order, so "the next letter that has rows" is the next larger code of that alphabet (the
 * lists don't order the alphabets by code: today's sort puts Hebrew before Latin). */
#define AZ_HEB      0x80
#define HEB_LETTERS 27            /* alef..tav, the 5 finals included */
#define HEB_FINALS  0x28A400u     /* bit n: alef+n is a final form (kaf, mem, nun, pe, tsadi sofit = n 10 13 15 19 21) */
static int heb_is_final(int n){ return (HEB_FINALS >> n) & 1; }
static void az_code_text(unsigned char code, char b[3]){   /* a code's letter as a UTF-8 string */
    if(code >= AZ_HEB){ b[0] = (char)0xD7; b[1] = (char)(0x90 + code - AZ_HEB); b[2] = 0; }
    else { b[0] = (char)code; b[1] = 0; }
}
/* library.db's jump letter (ldb_key_letter: 'A'..'Z', 0x80 + alef..tav without the finals, '#') as a code here */
static unsigned char az_code(int letter){
    static const unsigned char HEB_N[22] = { 0,1,2,3,4,5,6,7,8,9,11,12,14,16,17,18,20,22,23,24,25,26 };
    if(letter >= 0x80 && letter < 0x80 + 22) return (unsigned char)(AZ_HEB + HEB_N[letter - 0x80]);
    return (letter >= 'A' && letter <= 'Z') ? (unsigned char)letter : '#';
}
static void fmt_dur(char *b, size_t n, int ms){
    if(ms<=0){ b[0]=0; return; }
    long long t=((long long)ms+500)/1000; snprintf(b,n,"%lld:%02lld", t/60, t%60);   /* no int overflow near INT32_MAX */
}

/* ---- rows --------------------------------------------------------------- */
static lv_obj_t *base_row_in(lv_obj_t *parent){
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, ROW_H);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, TC(LIST_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}
static lv_obj_t *base_row(void){ return base_row_in(g_list); }
static void row_two(lv_obj_t *r, const char *top, const char *sub, const char *right, kit_value_kind_t kind){
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, top); lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, 12, sub&&sub[0]?6:16); lv_obj_set_size(t, right&&right[0]?186:242, 21);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* CJK titles render via Source Han Sans fallback */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_t *s = NULL;
    if(sub && sub[0]){
        s = lv_label_create(r);
        lv_label_set_text(s, sub); lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, 12, 28); lv_obj_set_size(s, right&&right[0]?186:242, 17);   /* stops before the duration, as the title does */
        lv_obj_set_style_text_font(s, TF(USER_14), 0);
        lv_obj_set_style_text_color(s, TC(TEXT_SECONDARY), 0);
    }
    if(right && right[0]){
        lv_obj_t *rl = lv_label_create(r);
        lv_label_set_text(rl, right);
        lv_obj_set_pos(rl, 204, 17); lv_obj_set_size(rl, 52, 18);
        lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(rl, TF(UI_14), 0);
        lv_obj_set_style_text_color(rl, TC(TEXT_MUTED), 0);
        kit_row_value(r, t, s, rl, kind);
    }
}

/* ---- callbacks ---------------------------------------------------------- */
static void reload_async(void *p){ (void)p; library_reload(); }

/* ---- playing library.db lists ---------------------------------------------------------------------------------- */
static void lcur_set(const ldb_list *l){ g_lcur = l; g_lids_ok = 0; }
static const int *lcur_ids(void){
    if(!g_lcur || g_lcur->n <= 0) return NULL;
    if(!g_lids_ok){
        free(g_lids);
        g_lids = malloc((size_t)g_lcur->n * sizeof *g_lids);
        if(!g_lids || mdb_song_ids_for(g_lcur->v, g_lcur->n, g_lids) < 0){ free(g_lids); g_lids = NULL; return NULL; }
        g_lids_ok = 1;
    }
    return g_lids;
}
/* Play list l from row i (-1 the first song, -2 a random one) in the list's order. ids = its songs' SONG.IDs.
 * Returns 1 if a play was sent; every failure toasts once (here or in ui_play_plan). */
static int lplay(const ldb_list *l, const int *ids, int i){
    if(!l || !ids || l->n <= 0){ ui_toast("Couldn't play that list"); return 0; }
    if(i == -2) i = rand() % l->n;
    else if(i < 0) i = 0;
    if(i >= l->n) return 0;
    for(int k = 0; ids[i] <= 0 && k < l->n; k++) i = (i + 1) % l->n;   /* Play All / Shuffle: the next song that can play */
    if(ids[i] <= 0){ ui_toast("Rescan to play these songs"); return 0; }
    mdb_plan_t plan;
    if(!mdb_plan_from_ids(ids, l->n, &plan)){ ui_toast("Couldn't play that list"); return 0; }
    int ok = ui_play_plan(&plan, ids[i]);
    mdb_plan_free(&plan);
    return ok;
}
static void lsong_play(int i){
    if(!g_lcur || i < 0 || i >= g_lcur->n) return;
    const int *ids = lcur_ids();
    if(ids && ids[i] <= 0){ ui_toast("Rescan to play this song"); return; }   /* never start a different song */
    if(!lplay(g_lcur, ids, i)) return;                      /* refused / not sent: already toasted, stay on the list */
    screen_show(SCR_NOWPLAYING);
}
static void lsong_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    lsong_play((int)(uintptr_t)lv_event_get_user_data(e));
}
/* Play list l (n = its read's result, freed here) from its first song. */
static int lplay_once(ldb_list *l, int n, const char *fail){
    int *ids = n > 0 ? malloc((size_t)n * sizeof *ids) : NULL, ok = 0;
    if(ids && mdb_song_ids_for(l->v, n, ids) >= 0) ok = lplay(l, ids, -1);
    else ui_toast(fail);
    free(ids);
    ldb_list_free(l);
    return ok;
}
int library_play_album(const char *album_artist, const char *album){
    ldb_list l;
    return lplay_once(&l, ldb_read_album_songs(album_artist, album, &l), "Couldn't play that album");
}
static void open_album(const char *album_artist, const char *album, int deeplink){
    snprintf(g_alb_artist, sizeof g_alb_artist, "%s", album_artist ? album_artist : "");
    snprintf(g_alb_title, sizeof g_alb_title, "%s", album ? album : "");
    snprintf(g_drill, sizeof g_drill, "%s", g_alb_title);   /* library_drill_context's album name (stock queues) */
    g_view=VIEW_GROUP; g_drill_kind=1; g_scope_lvl=0; g_scope_from_albums=0; g_deeplink=deeplink;
    library_reload();
}
static void lalbum_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_SHORT_CLICKED) return;
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    if(g_view!=VIEW_ALBUMS || i < 0 || i >= g_lalbums.n) return;
    g_from_wall = 0;
    open_album(g_lalbums.v[i].artist, g_lalbums.v[i].title, 0);   /* back returns to Albums */
}
static void lalbum_play_cb(lv_event_t *e){   /* long-press an album: play it now */
    if(lv_event_get_code(e)!=LV_EVENT_LONG_PRESSED) return;
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    if(g_view!=VIEW_ALBUMS || i < 0 || i >= g_lalbums.n) return;
    ui_set_workmode(0);                        /* sequential */
    if(library_play_album(g_lalbums.v[i].artist, g_lalbums.v[i].title)) screen_show(SCR_NOWPLAYING);
}

/* ---- "Remove from Favourites?" confirm (long-press is easy to trigger by
 * accident, so removal needs an explicit confirm). ------------------------ */
static lv_obj_t *g_fav_modal;
static char      g_fav_pending_path[1024];   /* the song to remove, by its file and start */
static long long g_fav_pending_start;
static void fav_modal_close(void){
    if(g_fav_modal){ lv_obj_delete_async(g_fav_modal); g_fav_modal = NULL; }
}
static void fav_cancel_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) fav_modal_close(); }
static void fav_confirm_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(!shdb_fav_set(g_fav_pending_path, g_fav_pending_start, 0, NULL, NULL, NULL)) ui_toast("Couldn't remove it");
    fav_modal_close();
    lv_async_call(reload_async, NULL);
}
static void fav_modal_pill(lv_obj_t *card, int x, const char *txt, theme_color_role_t col, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, 42); lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -16);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    ui_on(b, cb, LV_EVENT_CLICKED, NULL, "library.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, theme_color(col), 0);
    lv_obj_center(l);
}
static void fav_confirm(int i){   /* i = the row's index into g_lfavs */
    snprintf(g_fav_pending_path, sizeof g_fav_pending_path, "%s", g_lfavs.v[i].path);
    g_fav_pending_start = g_lfavs.v[i].start_ms;
    const char *title = g_lfavs.v[i].title[0] ? g_lfavs.v[i].title : "this song";
    fav_modal_close();
    g_fav_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_fav_modal);
    lv_obj_set_size(g_fav_modal, 360, 360); lv_obj_center(g_fav_modal);
    lv_obj_set_style_bg_color(g_fav_modal, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(g_fav_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(g_fav_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_fav_modal, LV_OBJ_FLAG_CLICKABLE);             /* absorb taps */
    ui_on(g_fav_modal, fav_cancel_cb, LV_EVENT_CLICKED, NULL, "library.fav_cancel", UI_CORE); /* tap outside = cancel */
    lv_obj_t *card = lv_obj_create(g_fav_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 264, 168); lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Remove from Favourites?");
    lv_obj_set_style_text_font(t, TF(UI_16), 0);
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_t *s = lv_label_create(card);
    lv_label_set_text(s, title);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, 224);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s, TF(USER_14), 0);   /* song title is user data: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(s, TC(TEXT_MUTED), 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 52);
    fav_modal_pill(card, -58, "Cancel", THEME_CLR_TEXT_SECONDARY, fav_cancel_cb);
    fav_modal_pill(card,  58, "Remove", THEME_CLR_STATUS_DANGER, fav_confirm_cb);
}

/* Favourites rows: short tap plays the list from that song, long-press asks to remove it. (SHORT_CLICKED so a
 * long-press doesn't also fire a play on release.) */
static void fav_row_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    if(g_view != VIEW_FAVS || g_lcur != &g_lfavs || i < 0 || i >= g_lfavs.n) return;
    if(c == LV_EVENT_SHORT_CLICKED){
        if(g_lfavs.v[i].count == 2) ui_toast("This song's file is gone");
        else lsong_play(i);
    } else if(c == LV_EVENT_LONG_PRESSED){
        fav_confirm(i);
    }
}
static void playlist_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    plview_open(g_plids[i], g_plnames[i]);   /* open the playlist (don't auto-play) */
}
static void pl_new_done(const char *name){   /* keyboard finished */
    if(name && name[0] && mdb_playlist_create(name) > 0) library_reload();
}
static void pl_new_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    kbinput_open("Playlist name", "", pl_new_done);   /* create an empty playlist */
}
/* an artist's (drill kind 2) or a genre's (3) library.db scope: an artist follows the Artists grouping (Settings) */
static int scope_of(int drill_kind){
    if(drill_kind==2 && g_from_wall) return g_wall_kind;   /* from an artist's cover flow: the grouping it was read in */
    return drill_kind==3 ? LDB_SCOPE_GENRE : mdb_artist_mode() ? LDB_SCOPE_GROUP : LDB_SCOPE_ARTIST;
}
/* an Artists / Genres row: index gi of the view's list, NULL if it is gone */
static const char *group_name(int gi){
    const ldb_list *l = g_view==VIEW_GENRES ? &g_lgenres : g_view==VIEW_ARTISTS ? &g_lartists : NULL;
    return l && gi >= 0 && gi < l->n ? l->v[gi].title : NULL;
}
static void group_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_SHORT_CLICKED) return;
    const char *name = group_name((int)(uintptr_t)lv_event_get_user_data(e));
    if(!name) return;
    g_from_wall = 0;
    if(g_view==VIEW_ARTISTS && cfg_get_int("album_view", 0)==1 && albumwall_open_artist(scope_of(2), name) > 0){
        wall_opened();   /* Album View = cover flow: the artist's albums as a cover flow */
        screen_show(SCR_ALBUMWALL);
        return;
    }                                             /* (an artist without albums opens on their songs, as in the list) */
    g_drill_kind = g_view==VIEW_GENRES ? 3 : 2;
    snprintf(g_drill, sizeof g_drill, "%s", name);
    g_scope_from_albums = 0;
    g_scope_lvl = 1;   /* its albums; the reload shows the flat list when it has none */
    g_view=VIEW_GROUP;
    g_deeplink=0;   /* normal in-library drill: back returns to the category list */
    library_reload();
}
/* Long-press an artist/genre row -> play the whole group right away
 * (saves drilling in + tapping Play All). Short tap still drills in. */
static void group_play_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_LONG_PRESSED) return;
    const char *name = group_name((int)(uintptr_t)lv_event_get_user_data(e));
    if(!name) return;
    ui_set_workmode(0);                        /* sequential */
    ldb_list l;
    int n = ldb_read_scope_songs(scope_of(g_view==VIEW_GENRES ? 3 : 2), name, &l);
    if(lplay_once(&l, n, "Couldn't play that list")) screen_show(SCR_NOWPLAYING);
}
static void settings_row_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_show(SCR_SETTINGS); }
static void menu_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int v=(int)(uintptr_t)lv_event_get_user_data(e);
    if(v==CAT_FOLDERS){ folderbrowser_open(); return; }   /* opens a screen, not a library view */
    if(v==CAT_BOOKS){ books_open(); return; }             /* audiobooks: a screen, not a library view */
    if(v==VIEW_ALBUMS && cfg_get_int("album_view", 0)==1){ albumwall_open_all(); wall_opened(); screen_show(SCR_ALBUMWALL); return; }  /* cover flow */
    g_view=v;
    g_drill_kind=0; library_reload();
}
/* Step one level back WITHIN the library (drill-in -> its category list -> the
 * category menu). Returns 1 if it handled an internal step, 0 if already at the
 * top menu (so the caller should leave the Library screen). */
static int prelink_restore(void);
static int prelink_reenter(void);
int library_back(void){
    if(g_view==VIEW_GROUP){
        if(g_from_wall && !g_deeplink && g_wall_view >= 0){   /* opened from the cover flow: back to the wall */
            g_from_wall = 0; g_view = g_wall_view; g_drill_kind = 0;
            g_wall_pending = 1;   /* rebuilt when Library is shown again (library_enter), not while it slides away */
            return 0;
        }
        if(g_drill_kind!=1 && g_scope_lvl==2){ g_scope_lvl=1; library_reload(); return 1; }   /* album songs -> that scope's albums */
        if(g_drill_kind!=1 && g_scope_lvl==0 && g_scope_from_albums){ g_scope_from_albums=0; g_scope_lvl=1; library_reload(); return 1; }   /* All Songs -> albums */
        if(g_deeplink){  /* opened from the hub -> back to where Library was before the link */
            int r = prelink_restore(); if(r >= 0) return r;
            g_deeplink=0; g_view=VIEW_MENU; g_drill_kind=0; library_reload(); return 0;
        }
        g_view=(g_drill_kind==1)?VIEW_ALBUMS:(g_drill_kind==3)?VIEW_GENRES:VIEW_ARTISTS;
        g_drill_kind=0; library_reload(); return 1;
    }
    if(g_deeplink && g_view==VIEW_ARTISTS){   /* a deep-linked artist drill moved to Artists by the Artist setting: Back still leaves the link */
        int r = prelink_restore(); if(r >= 0) return r;
        g_deeplink=0; g_view=VIEW_MENU; g_drill_kind=0; library_reload(); return 0;
    }
    if(g_view==VIEW_MOSTPLAYED || g_view==VIEW_RECENT){ g_view=VIEW_HISTORY; library_reload(); return 1; }  /* stats -> History */
    if(g_view!=VIEW_MENU){ g_view=VIEW_MENU; library_reload(); return 1; }
    return 0;
}
static void back_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(!library_back()) screen_back();
}

/* ---- artist / genre -> albums -> songs ---------------------------------------------------------------------------- */
static void scope_all_cb(lv_event_t *e){          /* "All Songs": the flat list (Play All / Shuffle on the whole artist / genre) */
    if(lv_event_get_code(e)!=LV_EVENT_SHORT_CLICKED) return;
    g_scope_lvl = 0; g_scope_from_albums = 1; library_reload();
}
static void scope_album_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_SHORT_CLICKED) return;
    int ai=(int)(uintptr_t)lv_event_get_user_data(e);
    if(g_view!=VIEW_GROUP || g_scope_lvl!=1 || ai<0 || ai>=g_lscope.n) return;
    snprintf(g_scope_album_artist, sizeof g_scope_album_artist, "%s", g_lscope.v[ai].artist);
    snprintf(g_scope_album, sizeof g_scope_album, "%s", g_lscope.v[ai].title);
    g_scope_lvl = 2; library_reload();
}
/* From an artist's cover flow (kind = its LDB_SCOPE_*): open the artist's songs on one album (album_artist + album), or
 * all of them (album NULL). library_enter shows them when the wall opens Library; Back returns to the wall. */
void library_wall_open(int kind, const char *artist, const char *album_artist, const char *album){
    g_wall_kind = kind;
    snprintf(g_drill, sizeof g_drill, "%s", artist ? artist : "");
    if(album){
        snprintf(g_scope_album_artist, sizeof g_scope_album_artist, "%s", album_artist ? album_artist : "");
        snprintf(g_scope_album, sizeof g_scope_album, "%s", album);
    }
    g_view = VIEW_GROUP; g_drill_kind = 2; g_scope_lvl = album ? 2 : 0; g_scope_from_albums = 0; g_deeplink = 0;
    g_from_wall = 1; g_wall_pending = 0;
}
/* From an artist's cover flow: play the artist's songs on that album (the list's long-press). 1 = sent. */
int library_wall_play(int kind, const char *artist, const char *album_artist, const char *album){
    ldb_list l;
    return lplay_once(&l, ldb_read_scope_album_songs(kind, artist, album_artist, album, &l), "Couldn't play that list");
}
/* From the cover flow of every album: open one album's songs. Back returns to the wall. */
void library_wall_open_album(const char *album_artist, const char *album){
    snprintf(g_alb_artist, sizeof g_alb_artist, "%s", album_artist ? album_artist : "");
    snprintf(g_alb_title, sizeof g_alb_title, "%s", album ? album : "");
    snprintf(g_drill, sizeof g_drill, "%s", g_alb_title);   /* library_drill_context's album name (stock queues) */
    g_view = VIEW_GROUP; g_drill_kind = 1; g_scope_lvl = 0; g_scope_from_albums = 0; g_deeplink = 0;
    g_from_wall = 1; g_wall_pending = 0;
}
static void scope_album_play_cb(lv_event_t *e){   /* long-press an album row: play it now (as the Albums list does) */
    if(lv_event_get_code(e)!=LV_EVENT_LONG_PRESSED) return;
    int ai=(int)(uintptr_t)lv_event_get_user_data(e);
    if(g_view!=VIEW_GROUP || g_scope_lvl!=1 || ai<0 || ai>=g_lscope.n) return;
    ui_set_workmode(0);                        /* sequential */
    ldb_list l;
    int n = ldb_read_scope_album_songs(scope_of(g_drill_kind), g_drill, g_lscope.v[ai].artist, g_lscope.v[ai].title, &l);
    if(lplay_once(&l, n, "Couldn't play that list")) screen_show(SCR_NOWPLAYING);
}

/* ---- A-Z grid ----------------------------------------------------------- */
/* build one row for the current view at list index i (data already prepared) */
static lv_obj_t *add_row(lv_obj_t *parent, int i){
    char dur[12];
    lv_obj_t *r = base_row_in(parent);
    if(g_view==VIEW_GROUP && g_drill_kind!=1 && g_scope_lvl==1){       /* artist/genre -> albums: "All Songs" first */
        char cnt[12];
        if(i==0){
            snprintf(cnt, sizeof cnt, "%d", g_scope_total);
            ui_on(r, scope_all_cb, LV_EVENT_SHORT_CLICKED, NULL, "library.scope_all", UI_CORE);
            row_two(r, tr("All Songs"), NULL, cnt, KIT_VALUE_COUNT);
        } else {
            const ldb_item *a = &g_lscope.v[i-1];
            snprintf(cnt, sizeof cnt, "%d", a->count);
            ui_on(r, scope_album_cb, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)(i-1), "library.scope_album", UI_CORE);
            ui_on(r, scope_album_play_cb, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)(i-1), "library.scope_album_play.long", UI_CORE);
            /* the album artist under the title, unless it's the artist open here */
            const char *sub = g_drill_kind!=3 && !strcmp(a->artist, g_drill) ? NULL : mdb_artist_missing(a->artist) ? "Unknown artist" : a->artist;
            char ysub[LDB_NAME + 16];
            if(g_drill_kind!=3 && a->year > 0){                     /* an artist's albums are by year: the year first */
                if(sub) snprintf(ysub, sizeof ysub, "%d \u00B7 %s", a->year, sub);
                else snprintf(ysub, sizeof ysub, "%d", a->year);
                sub = ysub;
            }
            row_two(r, a->title, sub, cnt, KIT_VALUE_COUNT);
        }
        theme_list_row(r);
        return r;
    }
    if(g_lcur){                                                         /* a song list (library.db) */
        const ldb_item *it = &g_lcur->v[i];
        fmt_dur(dur,sizeof dur,it->duration_ms);
        if(g_lcur == &g_lfavs){
            ui_on(r, fav_row_cb, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i, "library.fav_row", UI_CORE);
            ui_on(r, fav_row_cb, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)i, "library.fav_row.long", UI_CORE);
            row_two(r, it->title, it->count == 2 ? tr("Missing") : it->count ? tr("Not in the library") : it->artist,
                    dur, KIT_VALUE_DURATION);
            theme_list_row(r);
            return r;
        }
        ui_on(r, lsong_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i, "library.song", UI_CORE);
        if(g_view==VIEW_GROUP && (g_drill_kind==1 || g_scope_lvl==2) && it->track > 0 && cfg_get_int("track_numbers", 0)){
            char num[LDB_NAME + 16];                       /* album list + Track Numbers on: "3 Title" (stock "%d %s") */
            snprintf(num, sizeof num, "%d %s", it->track, it->title);
            row_two(r, num, it->artist, dur, KIT_VALUE_DURATION);
        } else row_two(r, it->title, it->artist, dur, KIT_VALUE_DURATION);
        theme_list_row(r);
        return r;
    }
    switch(g_view){
        case VIEW_ALBUMS: {                                            /* library.db */
            const ldb_item *a = &g_lalbums.v[i];
            ui_on(r, lalbum_cb, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i, "library.album", UI_CORE);
            ui_on(r, lalbum_play_cb, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)i, "library.album_play.long", UI_CORE);
            char cnt[12]; snprintf(cnt,sizeof cnt,"%d",a->count);
            const char *aa = mdb_artist_missing(a->artist) ? "Unknown artist" : a->artist;
            char ysub[LDB_NAME + 16];
            if(a->year > 0){ snprintf(ysub, sizeof ysub, "%d \u00B7 %s", a->year, aa); aa = ysub; }   /* the year first */
            row_two(r, a->title, aa, cnt, KIT_VALUE_COUNT);
            break; }
        case VIEW_ARTISTS:                                             /* library.db */
            ui_on(r, group_cb, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i, "library.group", UI_CORE);
            ui_on(r, group_play_cb, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)i, "library.group_play.long", UI_CORE);
            row_two(r, g_lartists.v[i].title[0] ? g_lartists.v[i].title : "Unknown artist", NULL, NULL, KIT_VALUE_COUNT);
            break;
        case VIEW_GENRES: {                                            /* library.db */
            ui_on(r, group_cb, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i, "library.group", UI_CORE);
            ui_on(r, group_play_cb, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)i, "library.group_play.long", UI_CORE);
            char cnt[12]; snprintf(cnt,sizeof cnt,"%d",g_lgenres.v[i].count);
            row_two(r, g_lgenres.v[i].title, NULL, cnt, KIT_VALUE_COUNT);
            break; }
    }
    theme_list_row(r);
    return r;
}
static void vlist_stop(void){ g_vspace = NULL; g_vrows = 0; }
static void vspace_delete_cb(lv_event_t *e){ if(lv_event_get_target(e) == g_vspace) vlist_stop(); }
/* a row a pointer is pressing, or pressed last: deleting it makes LVGL drop the touch, which stops a drag or a fling */
static int vrow_held(const lv_obj_t *r){
    for(lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        if(in->pointer.act_obj == r || in->pointer.last_obj == r) return 1;
    return 0;
}
/* make the rows near the viewport and delete the others (a held one stays until the pointer lets it go) */
static void vlist_sync(void){
    if(!g_vspace || g_count <= 0) return;
    int y = lv_obj_get_scroll_y(g_list) - lv_obj_get_y(g_vspace);    /* the viewport's top, in the spacer */
    int lo = (y > 0 ? y / ROW_PITCH : 0) - VROW_EXTRA;
    int hi = y + LIST_H < 0 ? -1 : (y + LIST_H) / ROW_PITCH + VROW_EXTRA;
    if(lo < 0) lo = 0;
    if(hi >= g_count) hi = g_count - 1;
    if(hi - lo > 63) hi = lo + 63;
    uint64_t have = 0;
    int k = 0;
    for(int j = 0; j < g_vrows; j++){
        int i = g_vrow[j].idx;
        if(i >= lo && i <= hi) have |= 1ull << (i - lo);
        else if(!vrow_held(g_vrow[j].obj)){ lv_obj_delete(g_vrow[j].obj); continue; }
        g_vrow[k++] = g_vrow[j];
    }
    g_vrows = k;
    for(int i = lo; i <= hi && g_vrows < VROW_MAX; i++){
        if(have >> (i - lo) & 1) continue;
        lv_obj_t *r = add_row(g_vspace, i);
        lv_obj_set_pos(r, 0, i * ROW_PITCH);
        g_vrow[g_vrows].obj = r; g_vrow[g_vrows].idx = i; g_vrows++;
    }
}
static void list_scroll_cb(lv_event_t *e){ (void)e; vlist_sync(); }
/* A-Z jumps land at once (every row's place exists), so there is never a pending target for a newer scroll to cancel. */
void library_jump_cancel(void){}
/* the view's n rows (g_count), below the headers already in the list */
static void fill_start(int n){
    vlist_stop();
    if(n <= 0) return;
    g_vspace = lv_obj_create(g_list);
    lv_obj_remove_style_all(g_vspace);
    lv_obj_set_size(g_vspace, 268, n * ROW_PITCH - 4);
    lv_obj_clear_flag(g_vspace, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_vspace, vspace_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_update_layout(g_list);       /* the spacer's y, and the scroll range the A-Z jumps clamp to */
    vlist_sync();
}

static void jump_to_letter(unsigned char L){
    if(g_count<=0) return;
    int idx=-1;
    for(int i=0;i<g_count;i++) if(g_first[i]==L){ idx=i; break; }
    if(idx<0){                                   /* nearest after: the first row of the next letter that has rows, */
        int heb = L >= AZ_HEB;                   /* in the same alphabet ('#' counts with Latin, as before) */
        unsigned char best=0;
        for(int i=0;i<g_count;i++){ unsigned char c=g_first[i]; if(c>L && (c>=AZ_HEB)==heb && (!best || c<best)){ best=c; idx=i; } }
        if(idx<0 && heb) for(int i=g_count-1;i>=0;i--) if(g_first[i]>=AZ_HEB){ idx=i; break; }   /* past tav: the last Hebrew row */
    }
    if(idx<0) idx=g_count-1;
    /* row pitch = ROW_H + the list's 4px flex pad_row, so idx*ROW_H alone lands
     * progressively short for later letters. */
    lv_obj_scroll_to_y(g_list, (idx + g_has_header)*ROW_PITCH, LV_ANIM_OFF);
}
/* close the grid and bring back the A-Z button it hid (the globe sits on the button's spot, and outline themes draw
 * the grid see-through) */
static void az_grid_close(void){
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);
}
static void letter_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    jump_to_letter((unsigned char)(intptr_t)lv_event_get_user_data(e));
    az_grid_close();
}
static void grid_bg_cb(lv_event_t *e){    /* tap outside closes */
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) az_grid_close();
}
/* the list row at the top of the viewport (the A-Z hint and the grid's opening page follow it) */
static int row_at_scroll(void){
    int idx = (lv_obj_get_scroll_y(g_list) - g_hdr_extra_px)/(ROW_H + 4) - g_has_header;
    if(idx < 0) idx = 0; else if(idx >= g_count) idx = g_count - 1;
    return idx;
}
static void az_set_lang(int lang){
    g_az_lang = lang;
    for(int k=0;k<2;k++)
        if(k==lang) lv_obj_clear_flag(g_az_page[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_az_page[k], LV_OBJ_FLAG_HIDDEN);
}
static void az_lang_cb(lv_event_t *e){     /* the globe button: flip to the other alphabet */
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) az_set_lang(!g_az_lang);
}
/* dim the letters this list has no rows for (they stay tappable: they jump to the next letter that has rows) */
static void az_dim_empty(void){
    unsigned char has[256] = {0};
    for(int i=0;i<g_count;i++) has[g_first[i]] = 1;
    for(int k=0;k<2;k++){
        uint32_t n = lv_obj_get_child_count(g_az_page[k]);
        for(uint32_t j=0;j<n;j++){
            lv_obj_t *cell = lv_obj_get_child(g_az_page[k], (int32_t)j);
            unsigned char code = (unsigned char)(intptr_t)lv_obj_get_user_data(cell);
            lv_obj_set_style_text_color(cell, has[code] ? TC(TEXT_PRIMARY) : TC(TEXT_DISABLED), 0);
        }
    }
}
static void az_btn_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(g_count > 0){                             /* open on the alphabet of the row in view; a '#' row keeps the last page */
        unsigned char c = g_first[row_at_scroll()];
        if(c >= AZ_HEB) g_az_lang = 1; else if(c >= 'A' && c <= 'Z') g_az_lang = 0;
    }
    az_dim_empty();
    az_set_lang(g_az_lang);
    lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void az_show(int on){
    if(on) lv_obj_clear_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);
    else { lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN); }
    /* the list sits on the centre line, except when the A-Z button needs the right edge: then it steps left for it */
    if(g_list){
        lv_obj_set_x(g_list, on ? 30 : 37);
        lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, on ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
}

/* ---- populate ----------------------------------------------------------- */
static lv_obj_t *empty_label(const char *txt){
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 268);
    theme_kit()->empty_state(KIT_EMPTY_LIBRARY, l);
    return l;
}
/* empty music view: the message + a tappable "Scan Library" so a new user isn't dead-ended */
void kit_stone_empty_scan(lv_obj_t *message, lv_obj_t *button);
static void scan_action_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    ui_rescan_library();
}
static void empty_scan(const char *txt){
    lv_obj_t *message = empty_label(txt);
    lv_obj_t *b = lv_button_create(g_list);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 200, 46);
    lv_obj_set_style_radius(b, 23, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(b, scan_action_cb, LV_EVENT_CLICKED, NULL, "library.scan_action", UI_CORE);
    if(!strcmp(theme_kit()->id, "default")){
        lv_obj_update_layout(b);
        lv_area_t a; lv_obj_get_coords(b, &a);
        lv_obj_set_style_translate_y(b, 216 - (a.y1 + a.y2) / 2, 0);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, tr_sym(LV_SYMBOL_REFRESH, "Scan Library"));
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    /* the button grows with a long translation ("Analyser la bibliotheque") up to what fits the round screen */
    lv_obj_update_layout(l);
    int lw = lv_obj_get_width(l);
    if(lw > 250){                                  /* too wide: one size smaller first */
        lv_obj_set_style_text_font(l, TF(UI_14), 0);
        lv_obj_update_layout(l);
        lw = lv_obj_get_width(l);
    }
    if(lw > 250){                                  /* still past the list's width: one line ending in "..." */
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_size(l, 250, lv_font_get_line_height(TF(UI_14)));
        lw = 250;
    }
    if(lw + 36 > 200) lv_obj_set_width(b, lw + 36);
    lv_obj_center(l);
    kit_stone_empty_scan(message, b);
}

/* ---- Play All / Shuffle All header for song lists ----------------------- */
/* list_type + filter name for the current song view (0100 jump-table types:
 * 1=all, 2=artist, 3=album, 10=genre). Returns 1 if playable. */
static int current_list_context(int *lt, char *name, int cap){
    if(g_view==VIEW_SONGS){ if(lt)*lt=1; if(name&&cap>0)name[0]=0; return 1; }
    if(g_view==VIEW_GROUP)  return library_drill_context(lt, name, cap);
    return 0;
}
/* Play the current list, setting the play-mode first (0=Sequential, 1=Shuffle).
 * Play-mode = 0102 (ground-truth captured 2026-06-25); ui_set_workmode applies it. */
static void play_list_mode(int mode){
    if(g_lcur){                                                       /* a song list (library.db) */
        cfg_set_int("work_mode", mode); ui_set_workmode(mode);
        if(lplay(g_lcur, lcur_ids(), mode == 1 ? -2 : -1)) screen_show(SCR_NOWPLAYING);
        return;
    }
    int lt; char nm[160];
    if(!current_list_context(&lt, nm, sizeof nm)) return;
    cfg_set_int("work_mode", mode); ui_set_workmode(mode);
    int pos = 1;
    if(mode == 1 && g_count > 1) pos = 1 + (rand() % g_count);   /* shuffle: start on a random track (not always the first) */
    if(!ui_play_list(lt, nm, pos)) return;
    screen_show(SCR_NOWPLAYING);
}
static void play_all_cb(lv_event_t *e){    if(lv_event_get_code(e)==LV_EVENT_CLICKED) play_list_mode(0); } /* sequential */
static void shuffle_all_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) play_list_mode(1); } /* shuffle */

static void hdr_btn(lv_obj_t *row, int x, const char *txt, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 128, 44); lv_obj_set_pos(b, x, 4);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(b, cb, LV_EVENT_CLICKED, NULL, "library.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    lv_obj_center(l);
}
/* Album detail header: the album cover on top (from the art cache) with the artist beneath it, shown
 * above the Play All / Shuffle row for an album drill - from EITHER the list view or the cover flow. */
static int g_hdr_cover_ctr;
static void add_album_cover_header(const char *album, const char *artist, const char *const *paths, int n){
    lv_obj_t *r = lv_obj_create(g_list);
    lv_obj_remove_style_all(r);
    artist = mdb_artist_missing(artist) ? "Unknown artist" : artist;
    int has_artist = 1;
    int hdr_h = has_artist ? 196 : 172;
    lv_obj_set_size(r, 286, hdr_h);   /* full list width so the cover screen-centres */
    g_hdr_extra_px = hdr_h + 4;        /* +flex pad_row: the A-Z scroll math subtracts this leading height */
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    const int CX = 7;   /* the list sits at x30/w286 (centre 173); +7 puts TOP_MID content at screen-centre 180 */

    /* Resolve the album's cached 148px cover (first track that has one) into a ROTATING /tmp file so
     * LVGL's image cache can't serve a previous album's bitmap for a reused filename. */
    char src[64]; src[0] = 0;
    for(int i=0;i<n;i++){
        char tmp[48]; snprintf(tmp, sizeof tmp, "/tmp/album_hdr%d.bmp", g_hdr_cover_ctr & 3);
        if(paths[i] && paths[i][0] && artcache_get_cover(paths[i], tmp)==0){
            snprintf(src, sizeof src, "A:%s", tmp); g_hdr_cover_ctr++;
            break;
        }
    }
    if(src[0]){
        lv_obj_t *img = lv_image_create(r);
        lv_image_set_src(img, src);                         /* native 148x148 BMP (no scale/rotate: SW renderer) */
        lv_obj_align(img, LV_ALIGN_TOP_MID, CX, 10);
    } else {                                                /* art-less album -> dark tile with its initial */
        lv_obj_t *ph = lv_obj_create(r);
        lv_obj_remove_style_all(ph);
        lv_obj_set_size(ph, 148, 148); lv_obj_align(ph, LV_ALIGN_TOP_MID, CX, 10);
        lv_obj_set_style_bg_color(ph, TC(SURFACE_RAISED), 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ph, 12, 0);
        lv_obj_t *in = lv_label_create(ph);
        char init[8]={0};   /* one WHOLE UTF-8 code point (a bare first byte tofus Cyrillic/CJK) */
        if(album && album[0]){ unsigned char c0=(unsigned char)album[0];
            int len=(c0<0x80)?1:((c0>>5)==0x6)?2:((c0>>4)==0xE)?3:((c0>>3)==0x1E)?4:1;
            for(int i=0;i<len && album[i];i++) init[i]=album[i];
            if(len==1 && init[0]>='a'&&init[0]<='z') init[0]-=32;
        } else init[0]='?';
        lv_label_set_text(in, init);
        /* ui_text_font tops out at 20 (would silently shrink a 28 request to 16): use the big latin face
         * for an ASCII initial, else the largest CJK-capable size so a non-Latin initial still renders. */
        lv_obj_set_style_text_font(in, (unsigned char)init[0] < 0x80 ? TF(UI_28) : TF(USER_20), 0);
        lv_obj_set_style_text_color(in, TC(TEXT_MUTED), 0);
        lv_obj_center(in);
    }
    if(has_artist){
        lv_obj_t *a = lv_label_create(r);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT); lv_obj_set_width(a, 240);
        lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(a, artist);
        lv_obj_set_style_text_font(a, TF(USER_16), 0);
        lv_obj_set_style_text_color(a, TC(TEXT_MUTED), 0);
        lv_obj_align(a, LV_ALIGN_TOP_MID, CX, 166);
    }
}

static void add_ldb_album_cover_header(const ldb_list *l, const char *album, const char *artist){   /* covers from its first songs' files */
    const char *paths[6]; int m = 0;
    for(int i=0;i<l->n && m<6;i++){
        int seen = 0;
        for(int j=0;j<m && !seen;j++) seen = !strcmp(paths[j], l->v[i].path);
        if(!seen) paths[m++] = l->v[i].path;
    }
    add_album_cover_header(album, artist, paths, m);
}

/* the first row of a song list: [ Play All ] [ Shuffle ]. centered=1 screen-centres the pair (used under
 * the album cover card, so it lines up with the centred cover instead of the left-aligned list rows). */
static void add_play_header(int centered){
    lv_obj_t *r = lv_obj_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, centered ? 286 : 268, ROW_H);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    int base = centered ? 16 : 0;                 /* r-local 16 => screen-centre (list is at x30/w286) */
    hdr_btn(r, base,       tr_sym(LV_SYMBOL_PLAY, "Play All"), play_all_cb);
    hdr_btn(r, base + 140, tr_sym(LV_SYMBOL_SHUFFLE, "Shuffle"), shuffle_all_cb);
    g_has_header = 1;
}

/* "Playlist" button under Play All / Shuffle on an album, artist or genre song list: opens the add-to-playlist picker with
 * exactly the songs this list shows, so each CUE/ISO track is added as its own row. */
static void add_plist_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(!g_lcur){ ui_toast("Nothing to add"); return; }
    const int *li = lcur_ids();                       /* its songs' SONG rows, in list order */
    int *ids = li ? malloc((size_t)g_lcur->n * sizeof *ids) : NULL, m = 0;
    if(!ids){ ui_toast("Couldn't add these songs"); return; }
    for(int i=0;i<g_lcur->n;i++) if(li[i] > 0) ids[m++] = li[i];
    if(m) plpick_set_ids(ids, m);
    free(ids);
    if(!m){ ui_toast("Rescan to add these songs"); return; }
    screen_show(SCR_PLPICK);
}
static void add_plist_row(int centered){
    lv_obj_t *r = lv_obj_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, centered ? 286 : 268, ROW_H);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    hdr_btn(r, (centered ? 16 : 0) + 70, tr_sym(LV_SYMBOL_PLUS, "Playlist"), add_plist_cb);   /* centred under the pair above */
    g_has_header = 2;                            /* Play All / Shuffle + this row */
}

static const char *VIEW_TITLE[] = { "Library","Songs","Albums","Artists","Playlists","Favourites","Genres","","Most Played","Recently Played","History" };
/* keep VIEW_TITLE[] in lockstep with the view enum so VIEW_TITLE[g_view] can't read out of bounds */
_Static_assert(sizeof(VIEW_TITLE)/sizeof(VIEW_TITLE[0]) == VIEW_COUNT, "VIEW_TITLE must have one entry per view");

static void library_reload(void){
    if(!g_list) return;
    if(g_lhint){ lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN); g_lhint_ch = 0; if(g_lhint_timer) lv_timer_pause(g_lhint_timer); }  /* don't leave a stale A-Z hint across views */
    vlist_stop();             /* before wiping rows: the scroll back to the top must not touch them */
    lv_obj_clean(g_list);
    lcur_set(NULL);
    g_count = 0;
    g_has_header = 0;
    g_hdr_extra_px = 0;
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);

    const char *ttl = (g_view==VIEW_GROUP) ? ((g_drill_kind!=1 && g_scope_lvl==2) ? g_scope_album : g_drill_kind==1 ? g_alb_title : g_drill) : VIEW_TITLE[g_view];
    if(g_view==VIEW_GROUP && g_drill_kind==2 && g_scope_lvl!=2 && !g_drill[0]) ttl = "Unknown artist";
    theme_title_text(g_title, (g_view==VIEW_GROUP) ? ttl : tr(ttl));
    if(g_status){ if(g_view==VIEW_MENU) lv_obj_remove_flag(g_status, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN); }
    az_show(0);

    /* one-time discoverability hints for the invisible long-press actions */
    static int s_group_hold_hint=0, s_fav_hold_hint=0;
    if((g_view==VIEW_ALBUMS || g_view==VIEW_ARTISTS || g_view==VIEW_GENRES) && !s_group_hold_hint){
        s_group_hold_hint=1; ui_toast("Hold an item to play all");
    } else if(g_view==VIEW_FAVS && !s_fav_hold_hint){
        s_fav_hold_hint=1; ui_toast("Hold to remove");
    }

    char dur[12];

    if(g_view==VIEW_MENU){
        /* "Most Played" + "Recently Played" are play STATS, not catalog axes -> grouped under History */
        static const char *CATS[] = { "Songs","Albums","Artists","Genres","Playlists","Favourites","Folders","Books","History" };
        static const int   CATV[] = { VIEW_SONGS,VIEW_ALBUMS,VIEW_ARTISTS,VIEW_GENRES,VIEW_PLAYLISTS,VIEW_FAVS,CAT_FOLDERS,CAT_BOOKS,VIEW_HISTORY };
        for(int i=0;i<(int)(sizeof CATS/sizeof CATS[0]);i++){
            if(!DISKOS_AUDIOBOOKS && CATV[i]==CAT_BOOKS) continue;   /* compile-time gate (DISKOS_AUDIOBOOKS, currently 1): hides the Books row when the audiobook feature is built out */
            lv_obj_t *r = base_row();
            ui_on(r, menu_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)CATV[i], "library.menu", UI_CORE);
            row_two(r, tr(CATS[i]), NULL, NULL, KIT_VALUE_COUNT);
            lv_obj_t *ch = lv_label_create(r);
            lv_label_set_text(ch, LV_SYMBOL_RIGHT);
            lv_obj_set_pos(ch, 240, 17);
            lv_obj_set_style_text_color(ch, TC(TEXT_DISABLED), 0);
            theme_list_row(r);
        }
        /* Settings: with Home and Quick settings gone, the Library root is the way in */
        lv_obj_t *r = base_row();
        ui_on(r, settings_row_cb, LV_EVENT_CLICKED, NULL, "library.settings", UI_CORE);
        row_two(r, tr_sym(LV_SYMBOL_SETTINGS, "Settings"), NULL, NULL, KIT_VALUE_COUNT);
        theme_list_row(r);
        return;
    }
    if(g_view==VIEW_HISTORY){          /* sub-menu: the two play-stats views */
        static const char *HCATS[] = { "Most Played","Recently Played" };
        static const int   HCATV[] = { VIEW_MOSTPLAYED, VIEW_RECENT };
        for(int i=0;i<2;i++){
            lv_obj_t *r = base_row();
            ui_on(r, menu_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)HCATV[i], "library.menu", UI_CORE);
            row_two(r, tr(HCATS[i]), NULL, NULL, KIT_VALUE_COUNT);
            lv_obj_t *ch = lv_label_create(r);
            lv_label_set_text(ch, LV_SYMBOL_RIGHT);
            lv_obj_set_pos(ch, 240, 17);
            lv_obj_set_style_text_color(ch, TC(TEXT_DISABLED), 0);
            theme_list_row(r);
        }
        return;
    }

    (void)dur;
    if(g_view==VIEW_GROUP && g_drill_kind!=1 && g_scope_lvl==1){       /* artist / genre -> its albums */
        ldb_list_free(&g_lscope);
        int n = ldb_read_scope_albums(scope_of(g_drill_kind), g_drill, &g_lscope);
        lib_ensure_first_cap(n + 1);
        if(n > g_first_cap - 1) n = g_first_cap - 1;
        if(n <= 0) g_scope_lvl = 0;                                     /* no albums (or gone in a rescan): the flat list */
        else {
            g_scope_total = ldb_scope_count(scope_of(g_drill_kind), g_drill);
            g_first[0] = '#';
            for(int i=0;i<n;i++) g_first[i+1]=az_code(g_lscope.v[i].letter);
            g_count = n + 1; fill_start(n + 1);
            if(n > 12 && g_drill_kind==3) az_show(1);                 /* an artist's albums are by year: no A-Z */
            return;
        }
    }
    if(g_view==VIEW_SONGS || g_view==VIEW_GROUP){                      /* library.db: every song, an album, or a scope's */
        int is_album = g_view==VIEW_GROUP && (g_drill_kind==1 || g_scope_lvl==2);
        ldb_list *l = g_view==VIEW_SONGS ? &g_lsongs : g_drill_kind==1 ? &g_lalbum : &g_lscope_songs;
        int n;
        if(g_view==VIEW_SONGS && ldb_list_current(l)) n = l->n;         /* unchanged since the last read: keep it */
        else {
            ldb_list_free(l);
            n = g_view==VIEW_SONGS ? ldb_read_songs(l)
              : g_drill_kind==1 ? ldb_read_album_songs(g_alb_artist, g_alb_title, l)
              : g_scope_lvl==2 ? ldb_read_scope_album_songs(scope_of(g_drill_kind), g_drill, g_scope_album_artist, g_scope_album, l)
              : ldb_read_scope_songs(scope_of(g_drill_kind), g_drill, l);
        }
        lib_ensure_first_cap(n);
        if(n > g_first_cap) n = g_first_cap;
        if(n<=0){ empty_scan(tr("No songs found")); return; }
        lcur_set(l);
        if(is_album){                                                   /* album: cover on top */
            if(g_drill_kind==1) add_ldb_album_cover_header(l, g_alb_title, g_alb_artist);
            else add_ldb_album_cover_header(l, g_scope_album, g_scope_album_artist);
        }
        add_play_header(is_album);                                      /* centre the pair under the album cover */
        if(g_view==VIEW_GROUP) add_plist_row(is_album);                 /* add these songs to a playlist */
        for(int i=0;i<n;i++) g_first[i]=az_code(l->v[i].letter);
        g_count=n; fill_start(n);
        if(g_view==VIEW_SONGS) az_show(1);
    } else if(g_view==VIEW_ALBUMS){                                    /* library.db */
        int n;
        if(ldb_list_current(&g_lalbums)) n = g_lalbums.n;
        else { ldb_list_free(&g_lalbums); n = ldb_read_albums(&g_lalbums); }
        lib_ensure_first_cap(n);
        if(n > g_first_cap) n = g_first_cap;
        if(n<=0){ empty_scan(tr("No albums found")); return; }
        for(int i=0;i<n;i++) g_first[i]=az_code(g_lalbums.v[i].letter);
        g_count=n; fill_start(n); az_show(1);
    } else if(g_view==VIEW_ARTISTS){                                   /* library.db, in the Artists grouping */
        int by_aa = mdb_artist_mode(), n;
        if(g_lartists_by_aa == by_aa && ldb_list_current(&g_lartists)) n = g_lartists.n;
        else { ldb_list_free(&g_lartists); n = ldb_read_artists(by_aa, &g_lartists); g_lartists_by_aa = by_aa; }
        lib_ensure_first_cap(n);
        if(n > g_first_cap) n = g_first_cap;
        if(n<=0){ empty_scan(tr("No artists found")); return; }
        for(int i=0;i<n;i++) g_first[i]=az_code(g_lartists.v[i].letter);
        g_count=n; fill_start(n); az_show(1);
    } else if(g_view==VIEW_GENRES){                                    /* library.db */
        int n;
        if(ldb_list_current(&g_lgenres)) n = g_lgenres.n;
        else { ldb_list_free(&g_lgenres); n = ldb_read_genres(&g_lgenres); }
        lib_ensure_first_cap(n);
        if(n > g_first_cap) n = g_first_cap;
        if(n<=0){ empty_scan(tr("No genres found")); return; }
        for(int i=0;i<n;i++) g_first[i]=az_code(g_lgenres.v[i].letter);
        g_count=n; fill_start(n); az_show(1);
    } else if(g_view==VIEW_FAVS){                                      /* shelves.db, library.db's songs */
        ldb_list_free(&g_lfavs);                                       /* the heart changes it: read it every time */
        int n = shdb_ready() ? ldb_read_favourites(&g_lfavs) : -1;
        lib_ensure_first_cap(n);
        if(n > g_first_cap) n = g_first_cap;
        if(n<=0){ empty_label(tr(n < 0 ? "Can't read Favourites" : "No Favourites yet")); return; }
        if(sd_io_begin()){                                             /* not in the library: is its file still there? */
            struct stat fs;
            for(int i=0;i<n;i++)
                if(g_lfavs.v[i].count == 1 && stat(g_lfavs.v[i].path, &fs) != 0 && errno == ENOENT) g_lfavs.v[i].count = 2;
            sd_io_end();
        }
        lcur_set(&g_lfavs);
        add_play_header(0);
        g_count=n; fill_start(n);   /* newest first -> no A-Z */
    } else if(g_view==VIEW_MOSTPLAYED || g_view==VIEW_RECENT){         /* listening.db's play counts, library.db's songs */
        ldb_list_free(&g_lhist);                                       /* plays change it: read it every time */
        int n = lsdb_ready() ? ldb_read_history(g_view==VIEW_RECENT, &g_lhist) : -1;
        lib_ensure_first_cap(n);
        if(n > g_first_cap) n = g_first_cap;
        if(n<=0){ empty_label(tr(n < 0 ? "Can't read play history" : "Nothing played yet")); return; }
        lcur_set(&g_lhist);
        g_count=n; fill_start(n);   /* ordered by plays / recency -> no Play-All header, no A-Z */
    } else { /* VIEW_PLAYLISTS */
        /* grow the buffers to the real playlist count (kept file-scope: playlist_cb reads
         * g_plids[i]/g_plnames[i] after this returns). A partial realloc keeps the old cap. */
        int num = mdb_playlist_num();
        /* overflow-guard the size_t multiply on 32-bit: MDB_STR is the larger stride so it
         * bounds both arrays. A pathological count just keeps the old capacity. */
        if(num > g_plcap && (size_t)num <= ((size_t)-1) / MDB_STR){
            char (*nn)[MDB_STR] = realloc(g_plnames, (size_t)num*MDB_STR);
            long  *ni           = realloc(g_plids,   (size_t)num*sizeof(long));
            int   *nc           = realloc(g_plcounts, (size_t)num*sizeof(int));
            if(nc) g_plcounts = nc;
            if(nn) g_plnames = nn;
            if(ni) g_plids   = ni;
            if(nn && ni && nc) g_plcap = num;
        }
        int n = (g_plcap > 0 && g_plnames && g_plids && g_plcounts) ? mdb_playlists_counted(g_plnames, g_plids, g_plcounts, g_plcap) : 0;
        /* "New Playlist" is always first so an empty library can still create one */
        lv_obj_t *nr=base_row();
        ui_on(nr, pl_new_cb, LV_EVENT_CLICKED, NULL, "library.pl_new", UI_CORE);
        kit_row_layout(nr, KIT_ROW_SYMBOL);
        row_two(nr, tr_sym(LV_SYMBOL_PLUS, "New Playlist"), NULL, NULL, KIT_VALUE_COUNT);
        theme_list_row(nr);
        for(int i=0;i<n;i++){
            lv_obj_t *r=base_row();
            ui_on(r, playlist_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i, "library.playlist", UI_CORE);
            char cnt[16]; snprintf(cnt,sizeof cnt,"%d", g_plcounts[i]);
            row_two(r, g_plnames[i], NULL, cnt, KIT_VALUE_COUNT);
            theme_list_row(r);
        }
    }
}

void library_set_song_click_cb(library_song_click_cb_t cb){ g_song_cb=cb; }

/* Rebuild the current Library view from the DB - used after an external change
 * (e.g. a playlist deleted from the playlist view) so the list isn't stale. */
void library_refresh(void){ if(g_list) library_reload(); }
/* Catalog rows change only on an explicit refresh (scan/settings) or a new drill.
 * Keep the rows on back navigation.
 * DB-backed views can change elsewhere while hidden; drills also re-resolve their
 * album cover after NP has populated the artwork cache. */
void library_enter(void){
    if(prelink_reenter()) return;
    if(g_wall_pending || (g_wall_view >= 0 && g_view == g_wall_view)){   /* back from the cover flow */
        int32_t y = g_view == g_wall_view ? g_wall_y : -1;
        g_wall_view = -1; g_wall_y = -1;
        if(g_wall_pending){ g_wall_pending = 0; library_reload(); }   /* the rows still show the songs it opened */
        else if(g_lhint){ lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN); g_lhint_ch = 0; if(g_lhint_timer) lv_timer_pause(g_lhint_timer); }
        if(g_list && y >= 0) lv_obj_scroll_to_y(g_list, y, LV_ANIM_OFF);   /* the view where it was */
        return;
    }
    if(g_view == VIEW_GROUP || g_view == VIEW_PLAYLISTS || g_view == VIEW_FAVS ||
       g_view == VIEW_MOSTPLAYED || g_view == VIEW_RECENT) library_refresh();
    else {
        /* Preserve entry's reset to the top and dismissal of the rim-scroll hint. */
        if(g_lhint){ lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN); g_lhint_ch = 0; if(g_lhint_timer) lv_timer_pause(g_lhint_timer); }
        if(g_list) lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    }
}
/* The Artists grouping changed (Settings): an open artist drill's key may not exist in the new grouping, so step back
 * to the Artists list; whatever Library shows is rebuilt from the new keys. */
void library_artist_mode_changed(void){
    if(g_view == VIEW_GROUP && g_drill_kind == 2){ g_view = VIEW_ARTISTS; g_drill_kind = 0; }
    g_from_wall = g_wall_pending = 0; g_wall_view = g_wall_y = -1;
    if(g_list) library_reload();
}
lv_obj_t *library_scroller(void){ return g_list; }

/* If the user is inside an album/artist/genre drill-in, report the player
 * list_type (3=album,2=artist,10=genre) + name so a song tap plays the exact
 * track within that list. Returns 0 when in a flat view (Songs/Search/Favs),
 * where the caller falls back to the song's own album. */
int library_drill_context(int *list_type, char *name, int cap){
    if(g_view != VIEW_GROUP) return 0;
    int lt = (g_drill_kind==1)?3 : (g_drill_kind==3)?10 : 2;
    if(list_type) *list_type = lt;
    if(name && cap>0) snprintf(name, cap, "%s", g_drill);
    return 1;
}

/* deep-link from the Now Playing 3-dot menu or the cover flow: jump straight to an album (its library.db key) / artist */
/* Where Library was before a Now Playing "Go to Album / Go to Artist" link, so Back from the link returns there
 * instead of to the Library menu (each section keeps its place). */
static struct {
    int valid, pushed, pending, view, drill_kind, scope_lvl, from_albums;
    int32_t scroll_y;
    char scope_aa[LDB_NAME], scope_album[LDB_NAME], drill[LDB_NAME], alb_artist[LDB_NAME], alb_title[LDB_NAME];
} g_prelink;
static void prelink_save(void){
    if(g_deeplink) return;   /* already inside a link: keep the spot from before the first one */
    /* The link pushes Library over the screen on top of Library's stack (the cover flow): Back pops to it. Called
     * before screen_show, so the top is still the old one. A restore not shown yet keeps its scroll: the rows are stale. */
    g_prelink.pushed = screen_section_top(SCR_LIBRARY) != SCR_LIBRARY;
    if(!g_prelink.pending) g_prelink.scroll_y = g_list ? lv_obj_get_scroll_y(g_list) : 0;
    g_prelink.valid = 1; g_prelink.pending = 0;
    g_prelink.view = g_view; g_prelink.drill_kind = g_drill_kind;
    g_prelink.scope_lvl = g_scope_lvl; g_prelink.from_albums = g_scope_from_albums;
    snprintf(g_prelink.scope_aa, sizeof g_prelink.scope_aa, "%s", g_scope_album_artist);
    snprintf(g_prelink.scope_album, sizeof g_prelink.scope_album, "%s", g_scope_album);
    snprintf(g_prelink.drill, sizeof g_prelink.drill, "%s", g_drill);
    snprintf(g_prelink.alb_artist, sizeof g_prelink.alb_artist, "%s", g_alb_artist);
    snprintf(g_prelink.alb_title, sizeof g_prelink.alb_title, "%s", g_alb_title);
}
/* Back out of a link. -1: no saved spot. 1: Library shows it again. 0: the link was pushed over another Library
 * screen, so the caller pops back to that screen and the saved view is rebuilt when Library is next entered. */
static int prelink_restore(void){
    if(!g_prelink.valid) return -1;
    g_prelink.valid = 0; g_deeplink = 0;
    g_view = g_prelink.view; g_drill_kind = g_prelink.drill_kind;
    g_scope_lvl = g_prelink.scope_lvl; g_scope_from_albums = g_prelink.from_albums;
    snprintf(g_scope_album_artist, sizeof g_scope_album_artist, "%s", g_prelink.scope_aa);
    snprintf(g_scope_album, sizeof g_scope_album, "%s", g_prelink.scope_album);
    snprintf(g_drill, sizeof g_drill, "%s", g_prelink.drill);
    snprintf(g_alb_artist, sizeof g_alb_artist, "%s", g_prelink.alb_artist);
    snprintf(g_alb_title, sizeof g_alb_title, "%s", g_prelink.alb_title);
    if(g_prelink.pushed){   /* not rebuilt while it slides away */
        if(g_wall_view >= 0 && g_view == g_wall_view) g_wall_pending = 1;   /* under a cover flow: its own re-entry */
        else g_prelink.pending = 1;
        return 0;
    }
    library_reload();
    if(g_list) lv_obj_scroll_to_y(g_list, g_prelink.scroll_y, LV_ANIM_OFF);
    return 1;
}
static int prelink_reenter(void){
    if(!g_prelink.pending) return 0;
    g_prelink.pending = 0;
    library_reload();
    if(g_list) lv_obj_scroll_to_y(g_list, g_prelink.scroll_y, LV_ANIM_OFF);
    return 1;
}
/* Back from another section (an edge pull): keep the list where it was. Only the views whose rows can change
 * elsewhere (favourites from the Now Playing heart, playlists, play stats) are rebuilt, at the same scroll. */
void library_return(void){
    if(!g_list) return;
    if(g_view == VIEW_PLAYLISTS || g_view == VIEW_FAVS || g_view == VIEW_MOSTPLAYED || g_view == VIEW_RECENT){
        int32_t y = lv_obj_get_scroll_y(g_list);
        library_refresh();
        lv_obj_scroll_to_y(g_list, y, LV_ANIM_OFF);
    }
}
void library_open_album(const char *album_artist, const char *album){ prelink_save(); open_album(album_artist, album, 1); }
void library_open_artist(const char *name){
    prelink_save();
    g_view=VIEW_GROUP; g_drill_kind=2; g_scope_lvl=0; g_scope_from_albums=0; g_deeplink=1; snprintf(g_drill, sizeof g_drill, "%s", name); library_reload();
}

/* ---- rim-scroll alphabet hint: a big centred letter shown while flying through
 * an alphabetical list, auto-hidden ~650ms after scrolling stops. -------------- */
static void lhint_timer_cb(lv_timer_t *t){
    if(!g_lhint || lv_obj_has_flag(g_lhint, LV_OBJ_FLAG_HIDDEN)){ lv_timer_pause(t); return; }
    if(lv_tick_elaps(g_lhint_tick) > 650){
        lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(t);
    }
}
/* called from the rim-scroll handler while the Library list is the one scrolling */
void library_scroll_letter_tick(void){
    if(!g_list || !g_lhint || g_count <= 0) return;
    /* Only the views that actually fill g_first[] (alphabetical lists) get the A-Z scrubber hint.
     * MostPlayed/Recent/Favourites/History (and Menu/Playlists) never populate g_first, so reading it there
     * showed stale letters from a prior view. Allowlist, so any new view defaults to no hint. */
    if(!(g_view==VIEW_SONGS || g_view==VIEW_GROUP || g_view==VIEW_ALBUMS ||
         g_view==VIEW_ARTISTS || g_view==VIEW_GENRES)) return;
    if(g_view==VIEW_GROUP && g_drill_kind==2 && g_scope_lvl==1) return;   /* an artist's albums are by year: no letters */
    unsigned char ch = g_first[row_at_scroll()];
    if(ch && ch != g_lhint_ch){
        const lv_font_t *cur = lv_obj_get_style_text_font(g_lhint_lbl, 0);
        int was_heb = cur == &font_hebrew_40 || cur == &font_hebrew_24;
        if(!was_heb) g_lhint_font = cur;          /* the face the theme gave it: Default 40px, the themed kits their 24px face */
        if(ch >= AZ_HEB) lv_obj_set_style_text_font(g_lhint_lbl,
            g_lhint_font && lv_font_get_line_height(g_lhint_font) < 36 ? &font_hebrew_24 : &font_hebrew_40, 0);
        else if(was_heb && g_lhint_font) lv_obj_set_style_text_font(g_lhint_lbl, g_lhint_font, 0);
        g_lhint_ch = ch; char b[3]; az_code_text(ch, b); lv_label_set_text(g_lhint_lbl, b);
    }
    if(g_lhint_timer) lv_timer_resume(g_lhint_timer);
    g_lhint_tick = lv_tick_get();
    lv_obj_clear_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
}

void library_ensure_capacity(void){
    /* The next reload sizes only the active view, after stopping its fill. */
}
static void lib_ensure_first_cap(int need){
    if(need <= g_first_cap) return;
    unsigned char *f = realloc(g_first, (size_t)need);
    if(f){ g_first = f; g_first_cap = need; }
}

void library_create(lv_obj_t *root){
    if(theme_screen_plain(THEME_PLAIN_LIBRARY)) theme_screen_solid(root);   /* the theme draws Library plain */
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title = ui_header_cb(root, "Library", back_cb);   /* shared header; back_cb pops the internal view stack */
    g_status = ui_status_line(root);

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 30, LIST_Y); lv_obj_set_size(g_list, 286, LIST_H);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* round bottom bezel */
    lv_obj_set_style_pad_row(g_list, 4, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(g_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

    /* "A-Z" button (bottom-right) opens the alphabet grid */
    g_az_btn = lv_button_create(root);
    lv_obj_remove_style_all(g_az_btn);
    lv_obj_set_pos(g_az_btn, 302, 158); lv_obj_set_size(g_az_btn, 44, 44);
    lv_obj_set_style_radius(g_az_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_az_btn, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(g_az_btn, LV_OPA_90, 0);
    ui_on(g_az_btn, az_btn_cb, LV_EVENT_CLICKED, NULL, "library.az_btn", UI_CORE);
    lv_obj_t *azl=lv_label_create(g_az_btn); lv_label_set_text(azl,"A-Z");
    lv_obj_set_style_text_font(azl,TF(UI_14),0);
    lv_obj_set_style_text_color(azl,TC(TEXT_PRIMARY),0); lv_obj_center(azl);
    lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);

    /* alphabet grid overlay */
    g_grid = lv_obj_create(root);
    lv_obj_remove_style_all(g_grid);
    kit_keep(g_grid);   /* canvas overlay; its letters still take the theme */
    lv_obj_set_size(g_grid, 360, 360); lv_obj_set_pos(g_grid, 0, 0);
    lv_obj_set_style_bg_color(g_grid, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_grid, LV_OPA_80, 0);
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(g_grid, grid_bg_cb, LV_EVENT_CLICKED, NULL, "library.grid_bg", UI_CORE);
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
    {   /* "Jump to" title disambiguates the grid as navigation, not a sort control (grid starts y0=48) */
        lv_obj_t *gt = lv_label_create(g_grid);
        lv_label_set_text(gt, tr("Jump to"));
        lv_obj_align(gt, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_set_style_text_font(gt, TF(UI_14), 0);
        lv_obj_set_style_text_color(gt, TC(TEXT_MUTED), 0);
    }
    {   /* language button (globe, as on a phone keyboard) on the A-Z button's spot: flips English <-> Hebrew letters */
        lv_obj_t *lb = lv_button_create(g_grid);
        lv_obj_remove_style_all(lb);
        lv_obj_set_pos(lb, 302, 158); lv_obj_set_size(lb, 44, 44);
        lv_obj_set_style_radius(lb, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lb, TC(SURFACE_RAISED), 0);
        lv_obj_set_style_bg_opa(lb, LV_OPA_90, 0);
        lv_obj_set_style_bg_color(lb, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(lb, TC(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_color(lb, TC(ON_ACCENT), LV_STATE_PRESSED);
        ui_on(lb, az_lang_cb, LV_EVENT_CLICKED, NULL, "library.az_lang", UI_CORE);
        lv_obj_t *gl = lv_label_create(lb);
        lv_label_set_text(gl, "\xEF\x82\xAC");             /* U+F0AC globe */
        lv_obj_set_style_text_font(gl, &font_icon_globe_20, 0);
        lv_obj_center(gl);
    }
    {   /* the two letter pages: 5 cells a row from y0=48 down, the last partial row centred so its cells avoid the
         * clipped bottom corners. Hebrew rows run right to left (alef top right). */
        static unsigned char PAGE_EN[27], PAGE_HE[HEB_LETTERS];
        int ne = 0, nh = 0;
        for(int i=0;i<26;i++) PAGE_EN[ne++] = (unsigned char)('A' + i);
        PAGE_EN[ne++] = '#';
        for(int n=0;n<HEB_LETTERS;n++) if(!heb_is_final(n)) PAGE_HE[nh++] = (unsigned char)(AZ_HEB + n);   /* 22 letters */
        PAGE_HE[nh++] = '#';
        const unsigned char *codes[2] = { PAGE_EN, PAGE_HE };
        const int counts[2] = { ne, nh };
        for(int k=0;k<2;k++){
            lv_obj_t *pg = lv_obj_create(g_grid);
            lv_obj_remove_style_all(pg);
            lv_obj_set_size(pg, 360, 360); lv_obj_set_pos(pg, 0, 0);
            lv_obj_clear_flag(pg, LV_OBJ_FLAG_CLICKABLE);   /* taps between cells fall through to the grid (closes it) */
            lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
            g_az_page[k] = pg;
            int cols=5, cw=50, ch=48, n=counts[k];      /* taller cells (46x44) */
            int x0=50;   /* left edge as before; the 50px pitch ends the right column at x=296, clear of the globe button at 302 */
            int rows=(n+cols-1)/cols;
            int y0=48;   /* anchor top at the original grid top; taller cells grow downward (centering would clip the top row's outer corners) */
            for(int i=0;i<n;i++){
                int r=i/cols, c=i%cols;
                int cells_in_row = (r==rows-1) ? (n - r*cols) : cols;  /* last row may be partial */
                int row_x0 = x0 + ((cols - cells_in_row)*cw)/2;
                int slot = k ? cells_in_row - 1 - c : c;              /* Hebrew: right to left */
                unsigned char code = codes[k][i];
                lv_obj_t *cell=lv_button_create(pg);
                lv_obj_remove_style_all(cell);
                lv_obj_set_pos(cell, row_x0+slot*cw, y0+r*ch); lv_obj_set_size(cell, cw-4, ch-4);
                lv_obj_set_style_radius(cell, 8, 0);
                lv_obj_set_style_bg_color(cell, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);
                lv_obj_set_style_text_color(cell, TC(TEXT_PRIMARY), 0);           /* the letter inherits these */
                lv_obj_set_style_text_color(cell, TC(ON_ACCENT), LV_STATE_PRESSED);
                lv_obj_set_user_data(cell, (void*)(intptr_t)code);                 /* az_dim_empty reads it */
                ui_on(cell, letter_cb, LV_EVENT_CLICKED, (void*)(intptr_t)code, "library.letter", UI_CORE);
                lv_obj_t *l=lv_label_create(cell);
                char b[3]; az_code_text(code, b); lv_label_set_text(l,b);
                lv_obj_set_style_text_font(l, code >= AZ_HEB ? &font_hebrew_20 : TF(UI_20), 0);
                lv_obj_center(l);
            }
        }
        az_set_lang(0);
    }

    /* alphabet hint overlay (rim-scroll position indicator) - centred, hidden */
    g_lhint = lv_obj_create(root);
    lv_obj_remove_style_all(g_lhint);
    lv_obj_set_size(g_lhint, 96, 96);
    lv_obj_center(g_lhint);
    lv_obj_set_style_radius(g_lhint, 22, 0);
    lv_obj_set_style_bg_color(g_lhint, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_lhint, LV_OPA_80, 0);
    lv_obj_clear_flag(g_lhint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
    g_lhint_lbl = lv_label_create(g_lhint);
    lv_obj_set_style_text_font(g_lhint_lbl, TF(UI_40), 0);
    lv_obj_set_style_text_color(g_lhint_lbl, TC(TEXT_PRIMARY), 0);
    lv_label_set_text(g_lhint_lbl, "A");
    lv_obj_center(g_lhint_lbl);
    g_lhint_timer = lv_timer_create(lhint_timer_cb, 150, NULL);
    if(g_lhint_timer) lv_timer_pause(g_lhint_timer);

    mdb_load();
    library_reload();
}
