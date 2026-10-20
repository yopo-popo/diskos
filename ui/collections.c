/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* collections.c - album collections on screen (see collections.h): what Albums shows, the cap that opens the
 * Collections sheet, the sheet itself (with its Reorder), and the collections' entries in the hold menu. */
#include "collections.h"
#include "shelvesdb.h"
#include "playlists.h"     /* playlists_dialog */
#include "itemmenu.h"
#include "rowdrag.h"       /* Reorder */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "i18n.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what Albums shows: kept as `collection` in diskos.conf; a collection is checked against shelves.db whenever
 * that changes ---- */
static int g_use_read;                  /* g_use was read from diskos.conf */
static long long g_use;                 /* 0 = every album, a collection's id, or COLLECTIONS_UNSORTED */
static int g_use_ok;                    /* g_use_name is its name as of shelves.db generation g_use_gen */
static unsigned g_use_gen;
static char g_use_name[LDB_NAME];
static void use_set(long long id){
    g_use = id > 0 || id == COLLECTIONS_UNSORTED ? id : 0;
    g_use_read = 1; g_use_ok = 0;
    cfg_set_int("collection", (int)g_use);
}
long long collections_in_use(void){
    if(!g_use_read){
        int v = cfg_get_int("collection", 0);
        g_use = v > 0 || v == COLLECTIONS_UNSORTED ? v : 0;
        g_use_read = 1;
    }
    if(g_use <= 0) return g_use;          /* every album, or the albums in none */
    unsigned gen = shdb_generation();
    if(g_use_ok && g_use_gen == gen) return g_use;
    int r = shdb_coll_name(g_use, g_use_name, sizeof g_use_name);
    if(r == 0){ use_set(0); return 0; }   /* deleted: every album again */
    if(r < 0) return 0;                   /* shelves.db can't be read now: every album, and ask again next time */
    g_use_ok = 1; g_use_gen = gen;
    return g_use;
}
const char *collections_name(void){
    long long id = collections_in_use();
    return id == COLLECTIONS_UNSORTED ? tr("Unsorted") : id ? g_use_name : "";
}
const char *collections_empty(void){
    return collections_in_use() == COLLECTIONS_UNSORTED ? "Every album is in a collection."
         : "No albums in this collection yet. Hold an album's cover and pick Add to collection.";
}

int collections_albums(ldb_list *l, coll_key *key){
    long long id = collections_in_use();
    unsigned gen = shdb_generation();
    if(ldb_list_current(l) && key->id == id && (!id || key->gen == gen)) return l->n;
    ldb_list_free(l);
    key->id = id; key->gen = gen;
    if(!id) return ldb_read_albums(l);
    if(!shdb_ready()) return -1;
    return id > 0 ? ldb_read_collection(id, l) : ldb_read_uncollected(l);
}
int collections_changed(const coll_key *key){
    long long id = collections_in_use();
    if(key->id != id) return 2;
    return id && key->gen != shdb_generation();
}

/* ---- the Collections sheet (SCR_COLLECTIONS), up from the cap: All albums, the collections, then Unsorted, a check on
 * the one shown; a hold on its title: New collection, Reorder (yopo 2026-10-20). As Add to collection (an album's hold
 * menu): only the collections, a check on those the album is in; a tap puts it in or takes it out, and the sheet
 * stays up so the album can go in several (yopo 2026-10-20). Holding a
 * collection: Rename, Delete. Reorder puts a handle (rowdrag) on each collection; Done or leaving ends it. ---- */
#define ROW_H 40
#define ROW_GAP 6
#define LIST_TOP 76       /* the first row; the title (and its back arrow) above it scroll away with the list (yopo) */
static lv_obj_t *g_title, *g_back, *g_list;
static int g_for_album;                 /* Add to collection, for g_album */
static im_item g_album;
static shdb_coll *g_colls;
static int g_ncolls;
static int g_reorder, g_dropped;        /* Reorder is on; a drag moved a collection (read the list again) */
static lv_obj_t **g_rows;               /* Reorder: the collections' rows */

static void sheet_reload(void);
static void reload_keep(void *unused);
/* The title follows the list's scroll, as on Library's top page */
static void head_follow(void){
    int32_t y = g_list ? lv_obj_get_scroll_y(g_list) : 0;
    if(g_title) lv_obj_set_style_translate_y(g_title, -y, 0);
    if(g_back) lv_obj_set_style_translate_y(g_back, -y, 0);
}
static void list_scroll_cb(lv_event_t *e){ (void)e; head_follow(); }
static void row_hold_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_LONG_PRESSED || g_reorder) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_ncolls) return;
    im_item it;
    memset(&it, 0, sizeof it);
    it.kind = IM_COLLECTION; it.where = IM_IN_LIBRARY; it.id = g_colls[i].id;
    snprintf(it.title, sizeof it.title, "%s", g_colls[i].name);
    itemmenu_open(&it);
}
/* A row: its name and a check at its end. i >= 0: collection i, which a hold opens the menu of (so a tap is
 * SHORT_CLICKED: LVGL clicks after a hold too). */
static lv_obj_t *sheet_row(const char *name, int checked, lv_event_cb_t cb, int i){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, ROW_H);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(r, cb, i >= 0 ? LV_EVENT_SHORT_CLICKED : LV_EVENT_CLICKED, (void *)(intptr_t)i, "collections.row", UI_CORE);
    if(i >= 0) ui_on(r, row_hold_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i, "collections.row.long", UI_CORE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, name);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, checked ? 204 : 236);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(l, TF(USER_16), 0);   /* collection names */
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    if(checked){
        lv_obj_t *c = lv_label_create(r);
        lv_label_set_text(c, LV_SYMBOL_OK);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_text_color(c, TC(ACCENT_PRIMARY), 0);
    }
    return r;
}
static void hint(const char *text){
    lv_obj_t *h = lv_label_create(g_list);
    lv_label_set_text(h, text);
    lv_obj_set_width(h, 268);
    lv_obj_set_style_pad_top(h, 8, 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(h, TF(UI_14), 0);
    lv_obj_set_style_text_color(h, TC(TEXT_MUTED), 0);
}
/* Put g_album in collection i, or take it out: its check shows which */
static void toggle(int i){
    int on = !g_colls[i].has;
    if(shdb_coll_set(g_colls[i].id, g_album.album_artist, g_album.album, on) != 1){ ui_toast(on ? "Couldn't add it" : "Couldn't take it out"); return; }
    g_colls[i].has = on;                /* a second tap before the rebuild takes it out again */
    lv_async_call(reload_keep, NULL);   /* after this event: the row tapped goes with the rebuild */
}
static void coll_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED || g_reorder) return;   /* Reorder: rows only move */
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_ncolls) return;
    if(g_for_album){ toggle(i); return; }
    use_set(g_colls[i].id);
    screen_back();   /* Albums reads it on the way back */
}
static void all_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    use_set(0);
    screen_back();
}
static void unsorted_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    use_set(COLLECTIONS_UNSORTED);
    screen_back();
}
static void new_done(const char *name){
    if(!name || !name[0]) return;
    long long id = shdb_coll_create(name);
    if(id == 0){ ui_toast("There's already a collection with that name"); return; }
    if(id < 0){ ui_toast("Couldn't make the collection"); return; }
    ui_toast("Collection made");
    sheet_reload();
}

/* Reorder: a drag puts the collection there */
static int drag_count(void *ctx){ (void)ctx; return g_rows ? g_ncolls : 0; }
static lv_obj_t *drag_row(void *ctx, int i){ (void)ctx; return g_rows && i >= 0 && i < g_ncolls ? g_rows[i] : NULL; }
static void drag_drop(void *ctx, int from, int to, int above, int below){
    (void)ctx; (void)above; (void)below;
    if(from < 0 || from >= g_ncolls) return;
    if(shdb_coll_place(g_colls[from].id, to) != 1) ui_toast("Couldn't move that");
    g_dropped = 1;
}
static void reload_keep(void *unused){   /* the sheet again, where it was scrolled to */
    (void)unused;
    if(!g_list) return;
    int32_t y = lv_obj_get_scroll_y(g_list);
    sheet_reload();
    lv_obj_update_layout(g_list);
    lv_obj_scroll_to_y(g_list, y, LV_ANIM_OFF);
    head_follow();
}
static void drag_done(void *ctx){
    (void)ctx;
    if(g_dropped){ g_dropped = 0; lv_async_call(reload_keep, NULL); }   /* the rows' indexes changed */
}
static rowdrag_list g_drag = { .count = drag_count, .row = drag_row, .drop = drag_drop, .done = drag_done };
static void reorder(int on){
    g_reorder = on;
    reload_keep(NULL);
}
static void done_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) reorder(0);
}

static void sheet_reload(void){
    if(!g_list) return;
    theme_title_text(g_title, tr(g_for_album ? "Add to collection" : "Collections"));
    lv_obj_clean(g_list);
    lv_obj_t *head = lv_obj_create(g_list);   /* the room the title takes above the first row */
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 268, LIST_TOP - ROW_GAP);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    free(g_colls); g_colls = NULL;
    free(g_rows); g_rows = NULL;
    int n = shdb_coll_list(g_for_album ? g_album.album_artist : NULL, g_for_album ? g_album.album : NULL, &g_colls);
    if(n < 0) ui_toast("Couldn't read your collections");
    g_ncolls = n > 0 ? n : 0;
    if(g_reorder && g_ncolls >= 2) g_rows = calloc((size_t)g_ncolls, sizeof *g_rows);
    if(!g_rows) g_reorder = 0;              /* nothing to reorder */
    g_drag.list = g_list;
    long long use = collections_in_use();
    if(n == 0 && use == COLLECTIONS_UNSORTED){ use_set(0); use = 0; }   /* no collections left: every album */
    if(g_reorder) sheet_row(tr_sym(LV_SYMBOL_OK, "Done"), 0, done_cb, -1);
    else if(!g_for_album) sheet_row(tr("All albums"), use == 0, all_cb, -1);
    for(int i = 0; i < g_ncolls; i++){
        const shdb_coll *c = &g_colls[i];
        lv_obj_t *r = sheet_row(c->name, !g_reorder && (g_for_album ? c->has : c->id == use), coll_cb, i);
        if(g_reorder){   /* the handle on the right */
            lv_obj_set_width(lv_obj_get_child(r, 0), 196);
            rowdrag_handle(r, i, &g_drag);
            g_rows[i] = r;
        }
    }
    if(g_reorder) return;
    if(g_ncolls && !g_for_album) sheet_row(tr("Unsorted"), use == COLLECTIONS_UNSORTED, unsorted_cb, -1);   /* the albums in none */
    if(!g_ncolls) hint(tr(g_for_album ? "No collections yet" : "No collections yet\nHold the title to make one"));
}
static void sheet_open(int for_album, const im_item *album){
    g_for_album = for_album;
    g_reorder = 0;
    if(album) g_album = *album;
    sheet_reload();
    if(g_list) lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    head_follow();
    screen_show(SCR_COLLECTIONS);
}
/* A hold on the sheet's title: its menu (yopo 2026-10-20). The title sits at the top edge, where the point the panel
 * reports wanders as the finger presses on the bezel, so this allows Playlists' title's 40 px of travel. */
#define TITLE_HOLD_SLOP 40
static void title_hold_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_LONG_PRESSED || g_for_album || g_reorder) return;
    if(ui_press_travel() > TITLE_HOLD_SLOP) return;   /* a swipe that began on the title */
    im_item it;
    memset(&it, 0, sizeof it);
    it.kind = IM_COLLECTIONS_PAGE; it.where = IM_IN_LIBRARY;
    snprintf(it.title, sizeof it.title, "%s", tr("Collections"));
    itemmenu_open(&it);
}

/* ---- the cap: the top of a circle peeking up from the bottom edge (y 326 down), below the cover flow's album
 * artist; the bezel rounds off its lower corners ---- */
#define CAP_R   80        /* the circle's radius: smaller is rounder */
#define CAP_TOP 326       /* where its top shows */
static void cap_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) sheet_open(0, NULL); }
lv_obj_t *collections_cap(lv_obj_t *parent, lv_obj_t **label){
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, 180 - CAP_R, CAP_TOP); lv_obj_set_size(b, 2 * CAP_R, 2 * CAP_R);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING);   /* it runs past the bottom edge: never part of the page's scroll size */
    ui_on(b, cap_cb, LV_EVENT_CLICKED, NULL, "collections.cap", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_DISABLED), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 8);
    if(label) *label = l;
    return b;
}

/* ---- the hold menu: Add to collection on an album held by its cover (yopo: not on its rows; only when there is a
 * collection to add it to), the sheet title's New collection and Reorder, a collection's Rename and Delete ---- */
static int add_shown(const im_item *it){ return it->kind == IM_ALBUM && it->on_art && it->album[0] && shdb_coll_count() > 0; }
static void add_run(const im_item *it){ sheet_open(1, it); }
static int page_shown(const im_item *it){ return it->kind == IM_COLLECTIONS_PAGE; }
static void page_new_run(const im_item *it){ (void)it; kbinput_open("Collection name", "", new_done); }
static int reorder_shown(const im_item *it){
    return it->kind == IM_COLLECTIONS_PAGE && g_ncolls >= 2;   /* the title's menu only (yopo) */
}
static void reorder_run(const im_item *it){ (void)it; reorder(1); }
static int coll_shown(const im_item *it){ return it->kind == IM_COLLECTION && it->id > 0; }
static long long g_edit;                /* the collection a rename or delete is for */
static void rename_done(const char *name){
    if(!name || !name[0]) return;
    int r = shdb_coll_rename(g_edit, name);
    if(r == 0){ ui_toast("There's already a collection with that name"); return; }
    if(r != 1){ ui_toast("Couldn't rename it"); return; }
    ui_toast("Renamed");
    sheet_reload();
}
static void rename_run(const im_item *it){ g_edit = it->id; kbinput_open("Collection name", it->title, rename_done); }
static void delete_yes(void){
    if(shdb_coll_delete(g_edit) != 1){ ui_toast("Couldn't delete it"); return; }
    ui_toast("Collection deleted");
    sheet_reload();
}
static void delete_run(const im_item *it){
    g_edit = it->id;
    char t[LDB_NAME + 32];
    snprintf(t, sizeof t, tr("Delete %s?"), it->title);
    static const char *const lb[] = { "Delete", "Cancel" };
    static void (*const fn[])(void) = { delete_yes, NULL };
    playlists_dialog(t, tr("Its albums stay in your library."), 2, lb, fn, 0);
}
static const im_entry E_ADD = { "Add to collection", add_shown, add_run, 302 };
static const im_entry E_PAGE_NEW = { "New collection", page_shown, page_new_run, 300 };
static const im_entry E_RENAME = { "Rename", coll_shown, rename_run, 400 };
static const im_entry E_REORDER = { "Reorder", reorder_shown, reorder_run, 405 };
static const im_entry E_DELETE = { "Delete collection", coll_shown, delete_run, 910 };

void collections_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    /* a sheet: it comes up from the bottom, so its hairline edge is the top one */
    lv_obj_set_style_border_side(root, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_left(root, 0, 0);
    lv_obj_set_style_pad_top(root, -1, 0);
    g_title = ui_header(root, "Collections");
    lv_obj_t *back = ui_header_back();
    if(g_title){   /* its menu (not through ui_on: see title_hold_cb); the title shows the press at once */
        lv_obj_add_flag(g_title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(g_title, 12);   /* as Library's title: the box is only 26 px tall, at the top edge */
        lv_obj_add_event_cb(g_title, title_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_set_style_text_opa(g_title, LV_OPA_50, LV_STATE_PRESSED);
    }
    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 46, 0); lv_obj_set_size(g_list, 280, 360);   /* the whole height: its first row at LIST_TOP */
    lv_obj_set_style_pad_bottom(g_list, 78, 0);   /* the last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, ROW_GAP, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);
    /* over the list, which starts at the top: the title takes its own presses (its hold), and a back arrow beside it
     * scrolls away with it (a theme's arrow at the left edge stays) */
    if(g_title) lv_obj_move_foreground(g_title);
    if(back && lv_obj_get_style_y(back, LV_PART_MAIN) < LIST_TOP){ lv_obj_move_foreground(back); g_back = back; }
    itemmenu_add(&E_ADD);
    itemmenu_add(&E_PAGE_NEW);
    itemmenu_add(&E_RENAME);
    itemmenu_add(&E_REORDER);
    itemmenu_add(&E_DELETE);
}
