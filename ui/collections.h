/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* collections - album collections (shelves.db) on screen. Albums (the list and the cover flow of every album) shows
 * every album, a collection, or the albums in none (Unsorted), titled with its name; the cap at its bottom opens the
 * Collections sheet (SCR_COLLECTIONS) to pick one, and a hold on the sheet's title offers New collection and Reorder.
 * Holding an album's cover offers Add to collection: the same sheet, ticking the collections the album is in. */
#ifndef DISKOS_COLLECTIONS_H
#define DISKOS_COLLECTIONS_H
#include "lvgl/lvgl.h"
#include "librarydb.h"

#define COLLECTIONS_UNSORTED (-1)   /* collections_in_use(): the albums in no collection */

/* What Albums shows: 0 = every album, a collection's id, or COLLECTIONS_UNSORTED; and its name ("" for every album).
 * Kept across restarts. */
long long collections_in_use(void);
const char *collections_name(void);
/* What Albums says when what it shows has no albums (English: put it through tr()) */
const char *collections_empty(void);

/* What an album list was read for, so it is read again only when that changes. */
typedef struct { long long id; unsigned gen; } coll_key;
/* The albums Albums shows, into l: kept when it is still what would be read (key), else read again. Returns the
 * count, -1 = can't read. */
int collections_albums(ldb_list *l, coll_key *key);
/* Has what Albums shows changed since l was read for key? 0 = no, 1 = the same collection was edited (read it again
 * where it was), 2 = another one (from its top). */
int collections_changed(const coll_key *key);

/* The cap at the bottom of Albums: a small dome under everything else there, opening the Collections sheet. *label
 * gets its label (UI_14, dimmed, empty): the cover flow's counter, the list's album count. */
lv_obj_t *collections_cap(lv_obj_t *parent, lv_obj_t **label);

#endif
