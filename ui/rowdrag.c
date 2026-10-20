/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "rowdrag.h"
#include "theme.h"
#include "theme_kit.h"
#include <stdlib.h>
#include <string.h>

/* The drag in progress. The row leaves the layout and follows the finger on top; a gap (an outlined placeholder)
 * holds the slot where it would land. Slots are counted among the other rows shown: slot j = before the j-th of
 * them, m = after the last. */
static struct {
    rowdrag_list *dl;                  /* NULL = no drag */
    lv_obj_t *row, *ph;                /* the lifted row; the gap */
    int from, m, s0, s;                /* the row; other rows shown; the slot it came from; the slot it would land in */
    int *oth;                          /* the other rows shown, in order */
    int *ytop;                         /* where each slot starts (list content y) with the row out of the list */
    unsigned char *ok;                 /* it may land in that slot */
    int x0, top0, h, fy0, fy;          /* the row's x, top and height and the finger's y at the start (content); the finger's y now (screen) */
    lv_color_t bg, bd;                 /* the row's look before the lift */
    lv_opa_t bg_opa;
    int32_t bd_w;
    lv_timer_t *scroll;
} g;
static uint32_t g_at;                  /* when the last drag began (lv_tick, 0 = never) */

static void rd_free(void){
    if(g.scroll) lv_timer_delete(g.scroll);
    free(g.oth); free(g.ytop); free(g.ok);
    memset(&g, 0, sizeof g);
}

/* The row or the gap went away under the drag (the list was rebuilt or closed): forget it, no callbacks. */
static void rd_deleted_cb(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target_obj(e);
    if(!g.dl || (o != g.row && o != g.ph)) return;
    lv_obj_t *other = o == g.row ? g.ph : g.row;
    if(other) lv_obj_remove_event_cb(other, rd_deleted_cb);
    if(other == g.ph) lv_obj_add_flag(g.ph, LV_OBJ_FLAG_HIDDEN);   /* left to whoever deletes the list */
    rd_free();
}

static void rd_ph_to(int s){   /* the gap before slot s's row, or after the last other row */
    if(!g.m) return;
    int a = (int)lv_obj_get_index(g.ph), b, idx;
    if(s < g.m){
        b = (int)lv_obj_get_index(g.dl->row(g.dl->ctx, g.oth[s]));
        idx = a < b ? b - 1 : b;
    } else {
        b = (int)lv_obj_get_index(g.dl->row(g.dl->ctx, g.oth[g.m - 1]));
        idx = (a < b ? b - 1 : b) + 1;
    }
    lv_obj_move_to_index(g.ph, idx);
}

/* The row follows the finger; the gap goes to the nearest slot it may land in (else back where it was). */
static void rd_place(void){
    lv_obj_t *l = g.dl->list;
    lv_area_t a; lv_obj_get_coords(l, &a);
    int t = g.top0 + g.fy - a.y1 + lv_obj_get_scroll_y(l) - g.fy0;
    if(t < g.ytop[0] - g.h / 2) t = g.ytop[0] - g.h / 2;
    if(t > g.ytop[g.m]) t = g.ytop[g.m];
    lv_obj_set_pos(g.row, g.x0, t);
    int best = g.s0, bd = abs(t - g.ytop[g.s0]);
    for(int j = 0; j <= g.m; j++){
        int d = abs(t - g.ytop[j]);
        if(g.ok[j] && d < bd){ best = j; bd = d; }
    }
    if(best != g.s){ g.s = best; rd_ph_to(best); }
}

/* Near the list's top or bottom the list scrolls under the row. */
static void rd_scroll_cb(lv_timer_t *tm){
    (void)tm;
    if(!g.dl) return;
    lv_obj_t *l = g.dl->list;
    lv_area_t a; lv_obj_get_coords(l, &a);
    int top = (a.y1 > 0 ? a.y1 : 0) + 40, bot = (a.y2 < 359 ? a.y2 : 359) - 84;
    if(top < 96) top = 96;                                               /* clear of the round screen's top and bottom */
    if(bot > 276) bot = 276;
    int y = g.fy, v = 0;
    if(y < top) v = -((top - y) / 4 + 1);
    else if(y > bot) v = (y - bot) / 4 + 1;
    if(v < -14) v = -14;
    if(v > 14) v = 14;
    if(!v) return;
    int sy = lv_obj_get_scroll_y(l), room = v < 0 ? sy : lv_obj_get_scroll_bottom(l);
    if(room <= 0) return;
    if(v < -room) v = -room;
    if(v > room) v = room;
    lv_obj_scroll_to_y(l, sy + v, LV_ANIM_OFF);
    rd_place();
}

static void rd_start(rowdrag_list *dl, int from){
    if(g.dl || !dl || !dl->list) return;
    if(dl->prepare) dl->prepare(dl->ctx, from);
    int n = dl->count(dl->ctx);
    lv_obj_t *r = from >= 0 && from < n ? dl->row(dl->ctx, from) : NULL;
    if(!r) return;
    g.oth = malloc((size_t)n * sizeof *g.oth);
    g.ytop = malloc((size_t)(n + 1) * sizeof *g.ytop);
    g.ok = malloc((size_t)n + 1);
    if(!g.oth || !g.ytop || !g.ok){ rd_free(); return; }
    g.dl = dl; g.row = r; g.from = from;
    lv_obj_t *l = dl->list;
    lv_obj_update_layout(l);
    g.x0 = lv_obj_get_x(r); g.top0 = lv_obj_get_y(r); g.h = lv_obj_get_height(r);
    int gap = lv_obj_get_style_pad_row(l, LV_PART_MAIN);

    g.ph = lv_obj_create(l);
    lv_obj_remove_style_all(g.ph);
    lv_obj_remove_flag(g.ph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g.ph, lv_obj_get_width(r), g.h);
    lv_obj_set_style_radius(g.ph, lv_obj_get_style_radius(r, LV_PART_MAIN), 0);
    lv_obj_set_style_border_width(g.ph, 1, 0);
    lv_obj_set_style_border_color(g.ph, TC(TEXT_MUTED), 0);
    lv_obj_set_style_border_opa(g.ph, LV_OPA_50, 0);
    lv_obj_move_to_index(g.ph, (int32_t)lv_obj_get_index(r));
    lv_obj_add_event_cb(g.ph, rd_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(r, rd_deleted_cb, LV_EVENT_DELETE, NULL);

    g.bg = lv_obj_get_style_bg_color(r, LV_PART_MAIN);
    g.bg_opa = lv_obj_get_style_bg_opa(r, LV_PART_MAIN);
    g.bd = lv_obj_get_style_border_color(r, LV_PART_MAIN);
    g.bd_w = lv_obj_get_style_border_width(r, LV_PART_MAIN);
    lv_obj_add_flag(r, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_move_foreground(r);                                           /* drawn over the others */
    lv_obj_set_pos(r, g.x0, g.top0);
    lv_obj_set_style_bg_color(r, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, TC(ACCENT_PRIMARY), 0);
    lv_obj_update_layout(l);

    int p = g.h + gap;                                                   /* rows after the gap move up by this */
    g.s0 = -1;
    for(int i = 0; i < n; i++){
        lv_obj_t *o = dl->row(dl->ctx, i);
        if(i == from || !o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) continue;
        if(g.s0 < 0 && i > from) g.s0 = g.m;
        int y = lv_obj_get_y(o);
        g.ytop[g.m] = g.s0 >= 0 ? y - p : y;
        g.oth[g.m++] = i;
    }
    if(g.s0 < 0) g.s0 = g.m;
    g.ytop[g.m] = g.m ? g.ytop[g.m - 1] + lv_obj_get_height(dl->row(dl->ctx, g.oth[g.m - 1])) + gap : g.top0;
    for(int j = 0; j <= g.m; j++){
        int above = j > 0 ? g.oth[j - 1] : -1, below = j < g.m ? g.oth[j] : -1;
        g.ok[j] = j != g.s0 && (!dl->accepts || dl->accepts(dl->ctx, from, j, above, below));
    }
    g.s = g.s0;

    lv_indev_t *in = lv_indev_active();
    lv_point_t pt = { 0, 0 };
    if(in) lv_indev_get_point(in, &pt);
    lv_area_t a; lv_obj_get_coords(l, &a);
    g.fy = pt.y;
    g.fy0 = pt.y - a.y1 + lv_obj_get_scroll_y(l);
    g.scroll = lv_timer_create(rd_scroll_cb, 30, NULL);
    g_at = lv_tick_get();
    if(!g_at) g_at = 1;
}

/* The row rejoins the list in the gap (moved back to where it was unless dropped), then the list hears of it. */
static void rd_end(int drop, int notify){
    if(!g.dl) return;
    rowdrag_list *dl = g.dl;
    int from = g.from, to = g.s, moved = drop && g.s != g.s0;
    int above = to > 0 ? g.oth[to - 1] : -1, below = to < g.m ? g.oth[to] : -1;
    if(!moved && g.s != g.s0) rd_ph_to(g.s0);
    lv_obj_t *r = g.row, *ph = g.ph;
    lv_obj_remove_event_cb(r, rd_deleted_cb);
    lv_obj_remove_event_cb(ph, rd_deleted_cb);
    lv_obj_move_to_index(r, (int32_t)lv_obj_get_index(ph));
    lv_obj_remove_flag(r, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_color(r, g.bg, 0);
    lv_obj_set_style_bg_opa(r, g.bg_opa, 0);
    lv_obj_set_style_border_color(r, g.bd, 0);
    lv_obj_set_style_border_width(r, g.bd_w, 0);
    lv_obj_delete(ph);
    rd_free();
    if(!notify) return;
    if(moved && dl->drop) dl->drop(dl->ctx, from, to, above, below);
    if(dl->done) dl->done(dl->ctx);
}

static void rd_handle_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    rowdrag_list *dl = lv_event_get_user_data(e);
    lv_obj_t *h = lv_event_get_current_target_obj(e);
    if(c == LV_EVENT_PRESSED){ rd_start(dl, (int)(intptr_t)lv_obj_get_user_data(h)); return; }
    if(!g.dl || g.dl != dl || lv_obj_get_parent(h) != g.row) return;
    if(c == LV_EVENT_PRESSING || c == LV_EVENT_RELEASED){
        lv_indev_t *in = lv_indev_active();
        lv_point_t p;
        if(in){
            lv_indev_get_point(in, &p);
            if(p.y >= 0 && p.y < 360){ g.fy = p.y; rd_place(); }   /* on release too: the driver can fold the last move into it */
        }
        if(c == LV_EVENT_RELEASED) rd_end(1, 1);
    } else if(c == LV_EVENT_PRESS_LOST) rd_end(0, 1);
}

lv_obj_t *rowdrag_handle(lv_obj_t *row, int i, rowdrag_list *dl){
    lv_obj_t *h = lv_obj_create(row);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, ROWDRAG_HANDLE_W, LV_PCT(100));
    lv_obj_align(h, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);   /* a drag on it moves the row, never the list */
    lv_obj_set_user_data(h, (void *)(intptr_t)i);
    for(int k = 0; k < 3; k++){                                          /* three lines */
        lv_obj_t *l = lv_obj_create(h);
        lv_obj_remove_style_all(l);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(l, 18, 2);
        lv_obj_set_style_radius(l, 1, 0);
        lv_obj_set_style_bg_color(l, TC(TEXT_MUTED), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, (k - 1) * 6);
    }
    ui_on(h, rd_handle_cb, LV_EVENT_PRESSED, dl, "row.drag", UI_CORE);
    lv_obj_add_event_cb(h, rd_handle_cb, LV_EVENT_PRESSING, dl);
    lv_obj_add_event_cb(h, rd_handle_cb, LV_EVENT_RELEASED, dl);
    lv_obj_add_event_cb(h, rd_handle_cb, LV_EVENT_PRESS_LOST, dl);
    return h;
}

int rowdrag_active(const rowdrag_list *dl){ return g.dl && (!dl || g.dl == dl); }
void rowdrag_cancel(void){ rd_end(0, 0); }

int rowdrag_handle_at(int x, int y){
    lv_point_t p = { x, y };
    lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &p);
    if(!o) return 0;
    uint32_t n = lv_obj_get_event_count(o);
    for(uint32_t i = 0; i < n; i++){
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if(d && lv_event_dsc_get_cb(d) == rd_handle_cb) return 1;
    }
    return 0;
}

int rowdrag_owns_gesture(uint32_t since){
    return g.dl || (g_at && (int32_t)(g_at - since) >= -150);   /* the press reaches LVGL a moment before main.c stamps it */
}
