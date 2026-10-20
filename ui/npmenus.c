/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "anim.h"
#include "config.h"
#include "i18n.h"
#include "musicdb.h"
#include "ipc.h"
#include "favourites.h"
#include "playlists.h"
#include "itemmenu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ui_label_set_changed(lv_obj_t *obj, const char *txt);   /* ui.c: a label's text only when its input changes */

/* Two Now Playing side panels:
 *  - SCR_NPMENU  (right "3-dot" icon): context - Song Info / Go to Album / Artist
 *  - SCR_TUNE    (left "tuning" icon): quick playback - Play Mode / Equalizer    */



/* ---- shared current-track context (set from main.c) --------------------- */
static char g_path[256];
static int  g_fav, g_have;
static lv_obj_t *g_fav_lbl;

static void fav_refresh(void){   /* every playback update: the text only when it changed */
    ui_label_set_changed(g_fav_lbl, tr(g_fav ? "Remove from Favourites" : "Add to Favourites"));
}
void npmenu_set(const track_state_t *st, int playing, const void *thumb_src){
    (void)playing; (void)thumb_src;
    g_have = st && st->have_track;
    snprintf(g_path,   sizeof g_path,   "%s", g_have ? st->path   : "");
    g_fav = g_have ? fav_current(st) : 0;
    fav_refresh();
}

/* ---- generic list row --------------------------------------------------- */
static lv_obj_t *menu_row(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *ud){
    lv_obj_t *r = lv_button_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 52);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, ud, "npmenus.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, tr(text));
    lv_obj_set_pos(l, 16, 16);
    lv_obj_set_style_text_font(l, TF(USER_16), 0);   /* rows carry playlist names: chain (issue #3) */
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    return r;
}

static lv_obj_t *panel_header(lv_obj_t *root, const char *title){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    return ui_header(root, title);   /* shared standard header; returns the title label */
}

static lv_obj_t *panel_list(lv_obj_t *root){
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 46, 76); lv_obj_set_size(list, 280, 250);
    lv_obj_set_style_pad_bottom(list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

/* ---- context menu (SCR_NPMENU) ------------------------------------------ */
static void ctx_info_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED){ screen_show(SCR_SONGINFO); } }
static void ctx_album_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    /* drill the library's album for this track (its path), not the player's metadata album string, which may
     * not match the library and show an empty list */
    char aa[LDB_NAME], album[LDB_NAME];
    if(!ldb_song_album(g_path, -1, aa, sizeof aa, album, sizeof album)){ ui_toast("No album info"); return; }
    library_open_album(aa, album);   /* opens Library on it */
}
static void ctx_artist_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    /* the library's artist for this track (its path), as the Artists list names it in its grouping */
    char a[LDB_NAME];
    if(!ldb_song_artist(g_path, -1, mdb_artist_mode(), a, sizeof a)){ ui_toast("No artist info"); return; }
    library_open_artist(a);   /* opens Library on their albums */
}
static void ctx_addpl_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) playlists_add_current();   /* the song playing now (just its CUE/ISO track) */
}
static void ctx_fav_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    track_state_t now; ipc_get_state(&now);   /* the song playing now */
    if(!now.have_track){ ui_toast("No track playing"); return; }
    int on = !fav_current(&now);
    if(!fav_current_set(&now, on)){ ui_toast("Couldn't save the favourite"); return; }
    g_fav = on;
    fav_refresh();
    ui_toast(g_fav ? "Added to Favourites" : "Removed from Favourites");   /* match Now Playing */
}
void npmenu_create(lv_obj_t *root){
    panel_header(root, "Options");
    lv_obj_t *list = panel_list(root);
    lv_obj_t *favrow = menu_row(list, "Add to Favourites", ctx_fav_cb, NULL);
    g_fav_lbl = lv_obj_get_child(favrow, 0);
    fav_refresh();
    menu_row(list, "Add to Playlist", ctx_addpl_cb,  NULL);
    menu_row(list, "Song Info",       ctx_info_cb,   NULL);
    menu_row(list, "Go to Album",     ctx_album_cb,  NULL);
    menu_row(list, "Go to Artist",    ctx_artist_cb, NULL);
}

/* Public: dismiss transient popups on any screen navigation (the hold menu, Add to playlist's dialogs), so none can
 * float onto a later screen and act on stale state. Called from the screen-manager transition. */
void npmenu_close_transients(void){ itemmenu_close(); plpick_close_transients(); }

/* ---- Now Playing hub (SCR_NPHUB, "Options"): the page left of Now Playing (a rightward swipe). Secondary
 * actions for the current track (play-mode & favorite live on the Now Playing screen). - */
static void hub_eq_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED){ screen_show(SCR_TUNE); } }

static void hub_sleep_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) ui_sleep_popover(); }
static void hub_chapters_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) chapters_open(); }
static void hub_settings_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_show(SCR_SETTINGS); }

static lv_obj_t *g_hub_list;
/* (Re)populate the hub for the current track. Books get Chapters and drop the music-only entries
 * (Go to Album / Go to Artist / Add to Playlist) that make no sense for a single audiobook. Lyrics is the page
 * right of Now Playing (a leftward swipe), not a row. */
static void nphub_populate(void){
    if(!g_hub_list) return;
    lv_obj_clean(g_hub_list);
    track_state_t st; ipc_get_state(&st);
    int book = st.path[0] && mdb_is_book_path(st.path);
    if(book){
        /* navigate, understand the book, then the rest - the order the listener reaches for most first */
        menu_row(g_hub_list, "Chapters",        hub_chapters_cb, NULL);
        menu_row(g_hub_list, "Book details",    ctx_info_cb,     NULL);   /* Song Info screen, relabelled for books */
        menu_row(g_hub_list, "Sleep Timer",     hub_sleep_cb,    NULL);
        menu_row(g_hub_list, "Equalizer",       hub_eq_cb,       NULL);
        menu_row(g_hub_list, "Settings",        hub_settings_cb, NULL);
    } else {
        menu_row(g_hub_list, "Equalizer",       hub_eq_cb,     NULL);
        menu_row(g_hub_list, "Song Info",       ctx_info_cb,   NULL);
        menu_row(g_hub_list, "Go to Album",     ctx_album_cb,  NULL);
        menu_row(g_hub_list, "Go to Artist",    ctx_artist_cb, NULL);
        menu_row(g_hub_list, "Add to Playlist", ctx_addpl_cb,  NULL);
        menu_row(g_hub_list, "Settings",        hub_settings_cb, NULL);
    }
}
void nphub_refresh(void){ nphub_populate(); }   /* called on each SCR_NPHUB show (screenmgr) */

void nphub_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_np_backdrop(root, SCR_NPHUB);   /* Now Playing's blurred art, turning (the page left of Now Playing) */
    g_hub_list = panel_list(root);
    lv_obj_set_pos(g_hub_list, 46, 0); lv_obj_set_size(g_hub_list, 280, 360);   /* the whole page: no header */
    lv_obj_set_style_pad_top(g_hub_list, 70, 0);   /* rows clear of the round bezel at both ends */
    lv_obj_set_style_pad_bottom(g_hub_list, 70, 0);
    nphub_populate();
}

/* ---- tuning menu (SCR_TUNE): Play Mode + Equalizer cyclers --------------- */
static const char *const T_MODE[] = { "Sequential","Shuffle","Repeat One","Repeat All","Single" };
static const char *const T_EQ[]   = { "Off","Jazz","Rock","R&B","Hip-Hop","Pop","Dance","Classical","Retro","Sibilance 1","Sibilance 2",
                                      "USER1","USER2","USER3","USER4","USER5","USER6","USER7","USER8","USER9","USER10" };  /* 11..20 = user PEQ slots */

static lv_obj_t *g_mode_val, *g_eq_val;
static int g_tune_built = 0;

/* re-sync the Tune panel's Play Mode / EQ labels from live cfg. The panel is built once
 * but work_mode/eq_preset also change from the NP mode button, Settings and play paths,
 * so without this the labels go stale on re-open. Called on SCR_TUNE show. */
void tune_refresh(void){
    if(!g_tune_built) return;
    int m = cfg_get_int("work_mode", 0); if(m<0||m>4) m=0;
    int q = cfg_get_int("eq_preset", 0); if(q<0||q>20) q=0;
    if(g_mode_val) lv_label_set_text(g_mode_val, tr(T_MODE[m]));
    if(g_eq_val)   lv_label_set_text(g_eq_val,   tr(T_EQ[q]));
}

static void cyc_apply(const char *key, const char *const *opts, int n, void *valobj, int dir){
    int base = cfg_get_int(key, 0); if(base < 0 || base >= n) base = 0;   /* normalize a corrupt stored value BEFORE stepping (no signed overflow, no OOB) */
    int v = base + dir;
    if(v<0) v=n-1; else if(v>=n) v=0;
    if(strcmp(key,"eq_preset")) cfg_set_int(key, v);   /* EQ persists on a successful send via ui_eq_select */
    if(!strcmp(key,"work_mode")){ if(!ui_book_active()) ui_set_workmode(v); }   /* an active book stays in Single; cfg still updates so music later uses the chosen mode */
    else if(!strcmp(key,"eq_preset")){ if(ui_eq_select(v) < 0){ v = cfg_get_int("eq_preset", 0); if(v<0||v>=n) v=0; } }   /* failed send -> label keeps the real preset (clamped: a corrupt stored value must not index opts[] OOB) */
    if(valobj) lv_label_set_text((lv_obj_t*)valobj, tr(opts[v]));
}
static void mode_dir_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) cyc_apply("work_mode", T_MODE, 5, g_mode_val, (int)(intptr_t)lv_event_get_user_data(e)); }
static void eq_dir_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) cyc_apply("eq_preset", T_EQ, 21, g_eq_val, (int)(intptr_t)lv_event_get_user_data(e)); }
static void eq_custom_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_show(SCR_EQ); }

/* a "< label : value >" stepper row */
static lv_obj_t *cyc_row(lv_obj_t *parent, int y, const char *label,
                         lv_event_cb_t cb, lv_obj_t **out_val, const char *cur){
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 300, 64); lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, tr(label));
    lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(lb, TF(UI_14), 0);
    lv_obj_set_style_text_color(lb, TC(TEXT_MUTED), 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, tr(cur));
    lv_obj_align(val, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_font(val, TF(UI_20), 0);
    lv_obj_set_style_text_color(val, TC(ACCENT_PRIMARY), 0);
    *out_val = val;

    lv_obj_t *l = lv_button_create(row); lv_obj_remove_style_all(l);
    lv_obj_set_size(l, 44, 44); lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 8);
    lv_obj_t *li=lv_label_create(l); lv_label_set_text(li, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(li, TC(TEXT_PRIMARY), 0); lv_obj_center(li);
    ui_on(l, cb, LV_EVENT_CLICKED, (void*)(intptr_t)-1, "npmenus.cb", UI_CORE);
    lv_obj_t *r = lv_button_create(row); lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 44, 44); lv_obj_align(r, LV_ALIGN_RIGHT_MID, -4, 8);
    lv_obj_t *ri=lv_label_create(r); lv_label_set_text(ri, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ri, TC(TEXT_PRIMARY), 0); lv_obj_center(ri);
    ui_on(r, cb, LV_EVENT_CLICKED, (void*)(intptr_t)1, "npmenus.cb", UI_CORE);
    return row;
}

void tune_create(lv_obj_t *root){
    panel_header(root, "Playback");
    int m = cfg_get_int("work_mode", 0); if(m<0||m>4) m=0;
    int q = cfg_get_int("eq_preset", 0); if(q<0||q>20) q=0;
    cyc_row(root, 84,  "PLAY MODE",  mode_dir_cb, &g_mode_val, T_MODE[m]);
    cyc_row(root, 158, "EQUALIZER",  eq_dir_cb,   &g_eq_val,   T_EQ[q]);
    g_tune_built = 1;

    lv_obj_t *cust = lv_button_create(root);
    lv_obj_remove_style_all(cust);
    lv_obj_set_size(cust, 180, 40); lv_obj_align(cust, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_set_style_radius(cust, 20, 0);
    lv_obj_set_style_bg_color(cust, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(cust, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cust, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    ui_on(cust, eq_custom_cb, LV_EVENT_CLICKED, NULL, "npmenus.eq_custom", UI_CORE);
    lv_obj_t *cl=lv_label_create(cust); lv_label_set_text(cl, tr("Custom EQ"));
    lv_obj_set_style_text_font(cl, TF(UI_16), 0);
    lv_obj_set_style_text_color(cl, TC(TEXT_PRIMARY), 0); lv_obj_center(cl);
}
