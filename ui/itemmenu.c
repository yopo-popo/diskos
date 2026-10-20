/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* itemmenu.c - the hold menu (itemmenu.h): the item's name, then its entries as a column of buttons over a scrim on
 * lv_layer_top. A tap outside the buttons closes it; a screen change closes it (npmenu_close_transients). */
#include "itemmenu.h"
#include "lvgl/lvgl.h"
#include "theme.h"
#include "theme_kit.h"
#include "i18n.h"
#include <stdint.h>
#include <string.h>

#define IM_MAX 32
#define ROW_W 256
#define ROW_H 46
#define ROW_GAP 6
#define TITLE_H 34          /* the heading and the gap under it */

static const im_entry *g_entries[IM_MAX];   /* by order; equal orders keep the order they were added in */
static int g_n;

static lv_obj_t *g_menu;                     /* the scrim; NULL = closed */
static im_item g_item;                       /* the open menu's item */
static const im_entry *g_shown[IM_MAX];
static im_item g_run_item;                   /* the chosen entry and its item, run once the menu is gone */
static const im_entry *g_run;

int itemmenu_add(const im_entry *e){
    if(!e || !e->label || !e->run) return 0;
    for(int i = 0; i < g_n; i++) if(g_entries[i] == e) return 1;
    if(g_n >= IM_MAX) return 0;
    int at = g_n;
    while(at > 0 && g_entries[at-1]->order > e->order){ g_entries[at] = g_entries[at-1]; at--; }
    g_entries[at] = e;
    g_n++;
    return 1;
}

int itemmenu_is_open(void){ return g_menu != NULL; }

void itemmenu_close(void){
    if(g_menu){ lv_obj_delete_async(g_menu); g_menu = NULL; }
}

static void run_async(void *unused){
    (void)unused;
    const im_entry *e = g_run;
    g_run = NULL;
    if(e) e->run(&g_run_item);
}
static void entry_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || !g_menu) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= IM_MAX || !g_shown[i]) return;
    g_run = g_shown[i];
    g_run_item = g_item;
    itemmenu_close();
    lv_async_call(run_async, NULL);   /* out of this button's event: the entry may open a screen or another popup */
}
static void outside_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) itemmenu_close();
}

static void add_button(lv_obj_t *list, int i){
    const im_entry *en = g_shown[i];
    lv_obj_t *b = lv_button_create(list);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, ROW_W, ROW_H);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    ui_on(b, entry_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i, "itemmenu.entry", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, tr(en->label));
    lv_obj_set_width(l, ROW_W - 32);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, en->order >= 900 ? TC(STATUS_DANGER) : TC(TEXT_PRIMARY), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
}

void itemmenu_open(const im_item *it){
    if(!it) return;
    itemmenu_close();
    g_item = *it;
    int n = 0;
    memset(g_shown, 0, sizeof g_shown);
    for(int i = 0; i < g_n; i++)
        if(!g_entries[i]->shown || g_entries[i]->shown(&g_item)) g_shown[n++] = g_entries[i];
    if(n == 0) return;

    g_menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_menu);
    lv_obj_set_size(g_menu, 360, 360); lv_obj_center(g_menu);
    lv_obj_set_style_bg_color(g_menu, TC(SCRIM), 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_80, 0);
    lv_obj_clear_flag(g_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_CLICKABLE);             /* absorbs taps: a tap outside the buttons closes */
    ui_on(g_menu, outside_cb, LV_EVENT_CLICKED, NULL, "itemmenu.close", UI_CORE);

    /* A short menu sits in the middle of the round screen; a long one starts near the top and scrolls, its last
     * button clearing the bezel. */
    int list_h = n * ROW_H + (n - 1) * ROW_GAP;
    int fits = TITLE_H + list_h <= 240;
    int top = fits ? (360 - TITLE_H - list_h) / 2 : 40;

    lv_obj_t *t = lv_label_create(g_menu);
    lv_label_set_text(t, g_item.title[0] ? g_item.title : "");
    lv_obj_set_width(t, 232);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(t, TF(USER_16), 0);   /* user text: Hebrew and other scripts */
    lv_obj_set_style_text_color(t, TC(TEXT_MUTED), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, top);

    lv_obj_t *list = lv_obj_create(g_menu);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, ROW_W, fits ? list_h : 360 - top - TITLE_H);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, top + TITLE_H);
    lv_obj_set_style_pad_row(list, ROW_GAP, 0);
    if(!fits) lv_obj_set_style_pad_bottom(list, 44, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    if(fits) lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    for(int i = 0; i < n; i++) add_button(list, i);
}
