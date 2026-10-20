/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* rowdrag - reorder a list by dragging a row's handle (Up Next, playlists).
 *
 * Pressing the handle on a row's right lifts the row at once: it follows the finger over the other rows, an outlined
 * gap shows where it would land, and near the list's top or bottom the list scrolls under it. Releasing drops it in
 * the gap. A drag never scrolls the list, and the rim fast-scroll leaves a press on a handle alone (main.c).
 *
 * The list is a vertically scrolling flex column. The rows that take part are given by count/row, in screen order;
 * other children (a title spacer, notes, a footer) stay where they are. Rows hidden when the drag starts are skipped,
 * which lets an album's songs travel with its header (prepare hides them).
 *
 * Indexes: from, above and below are row indexes (0..count-1); above/below are the rows shown right above and below
 * the dropped row, -1 for none. to is where it lands counted among the rows shown (0 = first): with no hidden rows,
 * its new index. One drag at a time. */
#ifndef DISKOS_ROWDRAG_H
#define DISKOS_ROWDRAG_H
#include "lvgl/lvgl.h"
#include <stdint.h>

#define ROWDRAG_HANDLE_W 48

typedef struct rowdrag_list {
    lv_obj_t *list;
    int       (*count)(void *ctx);
    lv_obj_t *(*row)(void *ctx, int i);
    void (*prepare)(void *ctx, int from);   /* optional, before measuring: build every row, hide rows that travel along
                                             * (it must not delete rows) */
    int  (*accepts)(void *ctx, int from, int to, int above, int below);   /* optional: 1 = it may land there */
    void (*drop)(void *ctx, int from, int to, int above, int below);      /* released somewhere new */
    void (*done)(void *ctx);                /* optional, after every drag, dropped or not (e.g. rebuild the list) */
    void *ctx;
} rowdrag_list;
/* Before drop and done run, the row is back in the list's layout where it was dropped (or where it was), looking as
 * before the lift, so a list that isn't rebuilt already shows the new order. */

/* Adds the handle (three lines, ROWDRAG_HANDLE_W wide, the row's full height) on row i's right; dl outlives the row. */
lv_obj_t *rowdrag_handle(lv_obj_t *row, int i, rowdrag_list *dl);
int  rowdrag_active(const rowdrag_list *dl);   /* a drag is on (dl NULL = on any list) */
void rowdrag_cancel(void);                     /* the row goes back where it was: no drop, no done */

/* For the touch loop (main.c): a handle under the point, and whether a drag began with the press made at `since`
 * (lv_tick), so its release is no swipe or edge pull. */
int  rowdrag_handle_at(int x, int y);
int  rowdrag_owns_gesture(uint32_t since);

#endif
