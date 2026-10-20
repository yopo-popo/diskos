/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* playlists.c - Add to playlist and the playlist entries of the hold menu (see playlists.h). */
#include "playlists.h"
#include "shelvesdb.h"
#include "itemmenu.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "i18n.h"
#include "musicdb.h"
#include "ipc.h"
#include "folderbrowser.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- a small dialog: a question and up to three buttons; a tap outside is Cancel ---- */
typedef void (*dlg_fn)(void);
static lv_obj_t *g_dlg;
static dlg_fn g_dlg_fn[3];
static void dlg_close(void){ if(g_dlg){ lv_obj_delete_async(g_dlg); g_dlg = NULL; } }
static void dlg_btn_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || !g_dlg) return;
    dlg_fn f = g_dlg_fn[(int)(intptr_t)lv_event_get_user_data(e)];
    dlg_close();
    if(f) f();
}
static void dlg_outside_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) dlg_close(); }
/* labels[i] runs fns[i] (NULL = only closes); the button danger (-1 = none) is drawn as a destructive one */
static void dialog(const char *title, const char *sub, int n, const char *const *labels, const dlg_fn *fns, int danger){
    dlg_close();
    g_dlg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_dlg);
    lv_obj_set_size(g_dlg, 360, 360); lv_obj_center(g_dlg);
    lv_obj_set_style_bg_color(g_dlg, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(g_dlg, LV_OPA_70, 0);
    lv_obj_clear_flag(g_dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_dlg, LV_OBJ_FLAG_CLICKABLE);
    ui_on(g_dlg, dlg_outside_cb, LV_EVENT_CLICKED, NULL, "playlists.dialog_close", UI_CORE);
    lv_obj_t *card = lv_obj_create(g_dlg);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, 264);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(card, 20, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, 224);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* may carry a playlist name */
    lv_obj_set_style_text_color(t, TC(TEXT_PRIMARY), 0);
    if(sub && sub[0]){
        lv_obj_t *s = lv_label_create(card);
        lv_label_set_text(s, sub);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s, 224);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(s, TF(UI_14), 0);
        lv_obj_set_style_text_color(s, TC(TEXT_MUTED), 0);
    }
    for(int i = 0; i < n && i < 3; i++){
        g_dlg_fn[i] = fns[i];
        lv_obj_t *b = lv_button_create(card);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 216, 40);
        lv_obj_set_ext_click_area(b, 3);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, i == danger ? TC(DANGER_BUTTON_SURFACE) : TC(SURFACE_RAISED), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
        ui_on(b, dlg_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i, "playlists.dialog", UI_CORE);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, tr(labels[i]));
        lv_obj_set_style_text_font(l, TF(UI_16), 0);
        lv_obj_set_style_text_color(l, i == danger ? TC(STATUS_DANGER) : fns[i] ? TC(TEXT_PRIMARY) : TC(TEXT_MUTED), 0);
        lv_obj_center(l);
    }
}
void plpick_close_transients(void){ dlg_close(); }

/* ---- the songs Add to playlist adds: copies, so the list they came from may go ---- */
static shdb_song *g_songs;
static int g_nsongs;
static char *g_pool;
static size_t slen(const char *s){ return s ? strlen(s) + 1 : 0; }
static const char *scopy(char **at, const char *s){
    if(!s) return NULL;
    size_t n = strlen(s) + 1;
    memcpy(*at, s, n);
    const char *r = *at;
    *at += n;
    return r;
}
static int pick_set(const ldb_item *v, int n){
    free(g_songs); free(g_pool);
    g_songs = NULL; g_pool = NULL; g_nsongs = 0;
    size_t len = 1;
    for(int i = 0; i < n; i++) len += slen(v[i].path) + slen(v[i].title) + slen(v[i].album_artist) + slen(v[i].album);
    g_songs = malloc((size_t)n * sizeof *g_songs);
    g_pool = malloc(len);
    if(!g_songs || !g_pool){ free(g_songs); free(g_pool); g_songs = NULL; g_pool = NULL; return 0; }
    char *at = g_pool;
    for(int i = 0; i < n; i++){
        if(!v[i].path || !v[i].path[0]) continue;
        shdb_song *s = &g_songs[g_nsongs++];
        s->path = scopy(&at, v[i].path);
        s->start_ms = v[i].start_ms;
        s->title = scopy(&at, v[i].title);
        s->album_artist = scopy(&at, v[i].album_artist);
        s->album = scopy(&at, v[i].album);
    }
    return g_nsongs;
}

/* ---- the picker (SCR_PLPICK): New Playlist, then the folders and playlists of one folder. For Move to folder it
 * shows only folders, to choose where a playlist or folder goes. ---- */
static lv_obj_t *g_title, *g_list;
static int g_moving;                  /* choosing a folder for g_move */
static im_item g_move;
static long long g_folder;            /* the folder shown, 0 = the top */
static char g_folder_name[LDB_NAME];
static shdb_entry *g_entries;
static int g_nentries;
static long long g_target;            /* the playlist being added to */
static char g_target_name[LDB_NAME];

static void plpick_reload(void);
static lv_obj_t *pick_row(const char *text, lv_event_cb_t cb, void *ud){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, 52);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, LV_EVENT_CLICKED, ud, "playlists.pick", UI_CORE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 236);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(l, TF(USER_16), 0);   /* playlist names */
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    return r;
}

/* add the songs to g_target; skip_dups leaves out the ones already in it */
static void add_to_target(int skip_dups){
    shdb_song *v = g_songs;
    int n = g_nsongs;
    shdb_song *keep = NULL;
    if(skip_dups){
        keep = malloc((size_t)(n ? n : 1) * sizeof *keep);
        if(!keep){ ui_toast("Couldn't add the songs"); return; }
        int k = 0;
        for(int i = 0; i < n; i++) if(shdb_pl_has(g_target, v[i].path, v[i].start_ms) == 0) keep[k++] = v[i];
        v = keep; n = k;
    }
    int added = n ? shdb_pl_add(g_target, v, n) : 0;
    free(keep);
    char msg[LDB_NAME + 48];
    if(added < 0){ ui_toast("Couldn't add the songs"); return; }
    if(added == 0) snprintf(msg, sizeof msg, "%s", tr("Nothing new to add"));
    else if(g_nsongs == 1) snprintf(msg, sizeof msg, tr("Added to %s"), g_target_name);
    else snprintf(msg, sizeof msg, tr("Added %d songs to %s"), added, g_target_name);
    ui_toast(msg);
    if(screen_current() == SCR_PLPICK) screen_back();
}
static void add_all(void){ add_to_target(0); }
static void add_new(void){ add_to_target(1); }
static void pick_playlist(long long id, const char *name){
    g_target = id;
    snprintf(g_target_name, sizeof g_target_name, "%s", name);
    int dups = 0;
    for(int i = 0; i < g_nsongs; i++) if(shdb_pl_has(id, g_songs[i].path, g_songs[i].start_ms) > 0) dups++;
    if(!dups){ add_all(); return; }
    /* the same song can be in a playlist twice, but only when asked for */
    static const char *const one[] = { "Add again", "Cancel" }, *const all[] = { "Add them again", "Cancel" },
                             *const some[] = { "Add them again", "Skip them", "Cancel" };
    static const dlg_fn again[] = { add_all, NULL }, again_or_skip[] = { add_all, add_new, NULL };
    char t[96];
    if(g_nsongs == 1) dialog(tr("Already in this playlist"), tr("Add it again?"), 2, one, again, -1);
    else if(dups == g_nsongs) dialog(tr("These songs are already in this playlist"), NULL, 2, all, again, -1);
    else {
        snprintf(t, sizeof t, tr("%d of these songs are already in this playlist"), dups);
        dialog(t, NULL, 3, some, again_or_skip, -1);
    }
}
static void moved(int r){
    if(r == 1){ ui_toast("Moved"); library_refresh(); if(screen_current() == SCR_PLPICK) screen_back(); }
    else ui_toast(r == 0 ? "A folder can't go inside itself" : "Couldn't move it");
}
static void move_here_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) moved(shdb_move(g_move.kind == IM_FOLDER, g_move.id, g_folder));
}
static void entry_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_nentries) return;
    if(g_entries[i].is_folder){
        g_folder = g_entries[i].id;
        snprintf(g_folder_name, sizeof g_folder_name, "%s", g_entries[i].name);
        plpick_reload();
        return;
    }
    if(!g_nsongs){ ui_toast("Nothing to add"); return; }
    pick_playlist(g_entries[i].id, g_entries[i].name);
}
static void up_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    long long p = shdb_parent(1, g_folder);
    g_folder = p > 0 ? p : 0;
    if(!g_folder || shdb_name(1, g_folder, g_folder_name, sizeof g_folder_name) != 1) g_folder_name[0] = 0;
    plpick_reload();
}
static void new_done(const char *name){
    if(!name || !name[0]) return;
    long long id = shdb_pl_create(name, g_folder);
    if(id == 0){ ui_toast("There's already a playlist with that name"); return; }
    if(id < 0){ ui_toast("Couldn't make the playlist"); return; }
    plpick_reload();
    if(!g_nsongs){ ui_toast("Playlist made"); return; }
    g_target = id;
    snprintf(g_target_name, sizeof g_target_name, "%s", name);
    add_all();
}
static void new_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) kbinput_open("Playlist name", "", new_done);
}
static void new_folder_done(const char *name){   /* Move to folder: a new folder here, and the move into it */
    if(!name || !name[0]) return;
    long long id = shdb_folder_create(name, g_folder);
    if(id <= 0){ ui_toast("Couldn't make the folder"); return; }
    moved(shdb_move(g_move.kind == IM_FOLDER, g_move.id, id));
}
static void new_folder_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) kbinput_open("Folder name", "", new_folder_done);
}
static void plpick_reload(void){
    if(!g_list) return;
    if(g_title) theme_title_text(g_title, g_folder ? g_folder_name : tr(g_moving ? "Move to" : "Add to Playlist"));
    lv_obj_clean(g_list);
    free(g_entries); g_entries = NULL;
    g_nentries = shdb_pl_list(g_folder, &g_entries);
    if(g_nentries < 0){ g_nentries = 0; ui_toast("Couldn't read your playlists"); }
    char text[LDB_NAME + 16];
    if(g_folder){ snprintf(text, sizeof text, LV_SYMBOL_LEFT "  %s", tr("Back")); pick_row(text, up_cb, NULL); }
    if(g_moving){
        snprintf(text, sizeof text, LV_SYMBOL_OK "  %s", tr(g_folder ? "Move into this folder" : "Move to the top"));
        pick_row(text, move_here_cb, NULL);
        snprintf(text, sizeof text, LV_SYMBOL_PLUS "  %s", tr("New Folder"));
        pick_row(text, new_folder_cb, NULL);
    } else {
        snprintf(text, sizeof text, LV_SYMBOL_PLUS "  %s", tr("New Playlist"));
        pick_row(text, new_cb, NULL);
    }
    for(int i = 0; i < g_nentries; i++){
        /* moving: only the folders it can go into */
        if(g_moving && (!g_entries[i].is_folder || (g_move.kind == IM_FOLDER && g_entries[i].id == g_move.id))) continue;
        if(g_entries[i].is_folder) snprintf(text, sizeof text, LV_SYMBOL_DIRECTORY "  %s", g_entries[i].name);
        else snprintf(text, sizeof text, "%s", g_entries[i].name);
        pick_row(text, entry_cb, (void *)(intptr_t)i);
    }
}
static void pick_open(void){
    g_folder = 0; g_folder_name[0] = 0;
    plpick_reload();
    if(g_list) lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    screen_show(SCR_PLPICK);
}
void playlists_add_songs(const ldb_item *v, int n){
    if(n <= 0 || !v){ ui_toast("Nothing to add"); return; }
    if(!pick_set(v, n)){ ui_toast("Couldn't add the songs"); return; }
    g_moving = 0;
    pick_open();
}
void playlists_add_current(void){
    track_state_t now;
    ipc_get_state(&now);
    if(!now.have_track || !now.path[0]){ ui_toast("No song to add"); return; }
    long long start = 0;
    int known = ldb_track_start(now.path, now.title, &start);
    const char *ext = strrchr(now.path, '.');
    if(!known && ext && (!strcasecmp(ext, ".cue") || !strcasecmp(ext, ".iso"))){
        ui_toast("Couldn't tell which track this is");   /* a CUE/ISO file holds many: never add the wrong one */
        return;
    }
    char aa[LDB_NAME], al[LDB_NAME];
    if(!ldb_song_album(now.path, start, aa, sizeof aa, al, sizeof al)){   /* not in the library: what the player shows */
        snprintf(aa, sizeof aa, "%s", now.artist);
        snprintf(al, sizeof al, "%s", now.album);
    }
    ldb_item it;
    memset(&it, 0, sizeof it);
    it.title = now.title; it.path = now.path; it.start_ms = start; it.album_artist = aa; it.album = al;
    playlists_add_songs(&it, 1);
}

/* ---- the hold menu's playlist entries ---- */
static int is_song(const im_item *it){ return it->kind == IM_SONG && it->path[0]; }
static int add_shown(const im_item *it){
    return is_song(it) || it->kind == IM_ALBUM || it->kind == IM_ARTIST || it->kind == IM_GENRE ||
           (it->kind == IM_PLAYLIST && it->id > 0);
}
static void add_run(const im_item *it){
    if(is_song(it)){
        ldb_item x;
        memset(&x, 0, sizeof x);
        x.title = it->title; x.path = it->path; x.start_ms = it->start_ms;
        x.album_artist = it->album_artist; x.album = it->album[0] ? it->album : NULL;
        playlists_add_songs(&x, 1);
        return;
    }
    ldb_list l;
    int n;
    switch(it->kind){
    case IM_ALBUM:  n = ldb_read_album_songs(it->album_artist, it->album, &l); break;
    case IM_ARTIST: n = ldb_read_scope_songs(mdb_artist_mode() ? LDB_SCOPE_GROUP : LDB_SCOPE_ARTIST, it->key, &l); break;
    case IM_GENRE:  n = ldb_read_scope_songs(LDB_SCOPE_GENRE, it->key, &l); break;
    default:        n = shdb_ready() ? ldb_read_playlist(it->id, &l) : -1; break;
    }
    if(n > 0) playlists_add_songs(l.v, n);
    else ui_toast(n < 0 ? "Couldn't read the songs" : "Nothing to add");
    ldb_list_free(&l);
}
static int remove_shown(const im_item *it){ return it->where == IM_IN_PLAYLIST && it->row_id > 0; }
static void remove_run(const im_item *it){
    if(shdb_pl_remove(it->row_id) != 1){ ui_toast("Couldn't remove it"); return; }
    ui_toast("Removed from playlist");
    plview_changed();
}

static int list_shown(const im_item *it){ return it->kind == IM_PLAYLIST && it->id > 0; }
static int folder_shown(const im_item *it){ return it->kind == IM_FOLDER && it->id > 0; }
static int either_shown(const im_item *it){ return list_shown(it) || folder_shown(it); }
static im_item g_edit;   /* the playlist or folder a rename or delete is for */
static void rename_done(const char *name){
    if(!name || !name[0]) return;
    int r = g_edit.kind == IM_FOLDER ? shdb_folder_rename(g_edit.id, name) : shdb_pl_rename(g_edit.id, name);
    if(r == 0 && g_edit.kind == IM_PLAYLIST){ ui_toast("There's already a playlist with that name"); return; }
    if(r != 1){ ui_toast("Couldn't rename it"); return; }
    ui_toast("Renamed");
    library_refresh();
    plview_changed();
}
static void rename_run(const im_item *it){
    g_edit = *it;
    kbinput_open(it->kind == IM_FOLDER ? "Folder name" : "Playlist name", it->title, rename_done);
}
static void export_run(const im_item *it){
    char file[LDB_NAME + 16], msg[LDB_NAME + 64];
    if(!playlists_export(it->id, it->title, file, sizeof file)){ ui_toast("Couldn't save it to the card"); return; }
    snprintf(msg, sizeof msg, tr("Saved as Playlists/%s"), file);
    ui_toast(msg);
}
static void delete_yes(void){
    if(shdb_pl_delete(g_edit.id) != 1){ ui_toast("Couldn't delete it"); return; }
    ui_toast("Playlist deleted");
    if(screen_current() == SCR_PLVIEW && plview_current() == g_edit.id) screen_back();
    library_refresh();
}
static void delete_run(const im_item *it){
    g_edit = *it;
    char t[LDB_NAME + 32];
    snprintf(t, sizeof t, tr("Delete %s?"), it->title);
    static const char *const lb[] = { "Delete", "Cancel" };
    static const dlg_fn fn[] = { delete_yes, NULL };
    dialog(t, tr("The songs stay in your library."), 2, lb, fn, 0);
}
static void move_run(const im_item *it){
    g_move = *it;
    g_moving = 1;
    pick_open();
}
static void folder_delete_run(const im_item *it){
    if(shdb_folder_delete(it->id) != 1){ ui_toast("Couldn't delete it"); return; }
    ui_toast("Folder deleted, what it held moved up");
    library_refresh();
}

/* Reorder: the list the row is in gets its handles: a playlist's songs (a song's menu, or the playlist's own menu on its
 * screen), else the Playlists list */
static int reorder_shown(const im_item *it){
    return (it->kind == IM_SONG && it->where == IM_IN_PLAYLIST && it->row_id > 0)
        || ((it->kind == IM_PLAYLIST || it->kind == IM_FOLDER || it->kind == IM_PLAYLISTS_PAGE) && it->where == IM_IN_PLAYLISTS);
}
static void reorder_run(const im_item *it){
    if(it->kind == IM_SONG || screen_current() == SCR_PLVIEW) plview_reorder(1);
    else library_pl_reorder(1);
}
/* The Playlists page's own menu (a hold on its title, yopo 2026-10-20): New Playlist and New Folder in the folder shown,
 * Reorder, Import playlists */
static int page_shown(const im_item *it){ return it->kind == IM_PLAYLISTS_PAGE; }
static long long g_new_in;   /* the folder New Playlist / New Folder make it in */
static void page_new_done(const char *name){
    if(!name || !name[0]) return;
    long long id = shdb_pl_create(name, g_new_in);
    if(id == 0) ui_toast("There's already a playlist with that name");
    else if(id < 0) ui_toast("Couldn't make the playlist");
    else library_refresh();
}
static void page_new_run(const im_item *it){ g_new_in = it->id; kbinput_open("Playlist name", "", page_new_done); }
static void page_folder_done(const char *name){
    if(!name || !name[0]) return;
    if(shdb_folder_create(name, g_new_in) > 0) library_refresh();
    else ui_toast("Couldn't make the folder");
}
static void page_folder_run(const im_item *it){ g_new_in = it->id; kbinput_open("Folder name", "", page_folder_done); }
/* Import playlist: the owner browses the card to one .m3u / .m3u8 file (yopo: never search the card for them); it
 * becomes a playlist in the folder shown */
static void import_picked(const char *path){
    char name[LDB_NAME], msg[LDB_NAME + 64];
    int found = 0, missed = 0;
    long long id = playlists_import_file(path, g_new_in, name, sizeof name, &found, &missed);
    if(id < 0) snprintf(msg, sizeof msg, "%s", tr("Couldn't import that file"));
    else if(id == 0) snprintf(msg, sizeof msg, "%s", tr("None of its songs are on the card"));
    else if(missed) snprintf(msg, sizeof msg, tr("Imported %s: %d of %d songs found"), name, found, found + missed);
    else if(found == 1) snprintf(msg, sizeof msg, tr("Imported %s, 1 song"), name);
    else snprintf(msg, sizeof msg, tr("Imported %s, %d songs"), name, found);
    ui_toast(msg);
    if(id <= 0) return;   /* still browsing: another file can be picked */
    library_refresh();
    if(screen_current() == SCR_FOLDER) screen_back();
}
static void page_import_run(const im_item *it){ g_new_in = it->id; folderbrowser_pick_playlist(import_picked); }
static const im_entry E_ADD = { "Add to playlist", add_shown, add_run, 300 };
static const im_entry E_MOVE = { "Move to folder", either_shown, move_run, 310 };
static const im_entry E_RENAME = { "Rename", either_shown, rename_run, 400 };
static const im_entry E_REORDER = { "Reorder", reorder_shown, reorder_run, 405 };
static const im_entry E_PAGE_NEW = { "New Playlist", page_shown, page_new_run, 300 };
static const im_entry E_PAGE_FOLDER = { "New Folder", page_shown, page_folder_run, 301 };
static const im_entry E_PAGE_IMPORT = { "Import playlist", page_shown, page_import_run, 420 };
static const im_entry E_EXPORT = { "Save to the card", list_shown, export_run, 410 };
static const im_entry E_REMOVE = { "Remove from playlist", remove_shown, remove_run, 900 };
static const im_entry E_DELETE = { "Delete playlist", list_shown, delete_run, 910 };
static const im_entry E_FOLDER_DELETE = { "Delete folder", folder_shown, folder_delete_run, 910 };

void plpick_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    g_title = ui_header(root, "Add to Playlist");
    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 46, 76); lv_obj_set_size(g_list, 280, 284);   /* down to the screen's bottom edge */
    lv_obj_set_style_pad_bottom(g_list, 78, 0);   /* the last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 10, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    itemmenu_add(&E_ADD);
    itemmenu_add(&E_MOVE);
    itemmenu_add(&E_RENAME);
    itemmenu_add(&E_REORDER);
    itemmenu_add(&E_PAGE_NEW);
    itemmenu_add(&E_PAGE_FOLDER);
    itemmenu_add(&E_PAGE_IMPORT);
    itemmenu_add(&E_EXPORT);
    itemmenu_add(&E_REMOVE);
    itemmenu_add(&E_DELETE);
    itemmenu_add(&E_FOLDER_DELETE);
}
