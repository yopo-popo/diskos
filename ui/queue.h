/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* queue - diskOS play queue. Owns the queue fully; knows nothing about any player.
 *
 * The queue is a list of items (file path + CUE start/end), some grouped into album units. It lives in
 * memory and in listening.db (tables queue_*, schema_part row 'queue'). A player adapter listens for
 * changes and makes the player's own list match; it reports back which item plays with queue_set_current.
 *
 * Rules the module enforces (agreed with yopo):
 * - Nothing is ever placed at or above the playing item; edits happen after it.
 * - The playing item can't be moved or removed, and the unit holding it can't be moved or removed whole.
 * - Items of a unit are always next to each other. Nothing is dropped inside another unit.
 *
 * All calls are from the UI thread. Indexes are 0-based positions in the whole list (played items too).
 */
#ifndef DISKOS_QUEUE_H
#define DISKOS_QUEUE_H

#include <stdint.h>
#include "listeningdb.h"   /* only its path: the queue_* tables live in listening.db */

#define QUEUE_KEEP_OLD 10   /* replaced queues kept in listening.db */

/* Input when adding songs. Strings are copied; NULL is stored as "". */
typedef struct {
    const char *path;
    int start_ms, end_ms;   /* CUE range; 0, 0 = whole file */
    int duration_ms;        /* 0 = unknown */
    const char *title, *artist, *album;
} queue_track;

/* Input when adding an album as one unit. */
typedef struct {
    const char *album_artist, *album;
} queue_album;

typedef struct {
    int64_t id;             /* stable while the item is in the queue */
    int64_t unit_id;        /* 0 = loose song */
    char *path;
    int start_ms, end_ms, duration_ms;
    char *title, *artist, *album;
} queue_item;

typedef struct {
    int64_t id;
    char *album_artist, *album;
    int collapsed;
} queue_unit;

/* Rows for the queue screen: the playing item and everything after it. A unit shows a header row and,
 * unless collapsed, a row per item; the playing unit, collapsed, shows only its playing item. */
enum { QROW_ITEM, QROW_UNIT };
typedef struct {
    int kind;
    int index;              /* QROW_ITEM: the item; QROW_UNIT: its first item from the playing one on */
    int count;              /* QROW_UNIT: items shown under it (from the playing one on) */
    int64_t unit_id;        /* the item's or header's unit, 0 for a loose item */
} queue_row;

/* Change events for listeners. Indexes refer to the list after the change. */
enum {
    QEV_RESET,              /* whole list replaced (replace, open) */
    QEV_INSERT,             /* count items inserted at index */
    QEV_REMOVE,             /* count items removed from index (index = where they were) */
    QEV_MOVE,               /* count items moved from index (before the move) to start at to (after it) */
    QEV_JUMP,               /* user picked index to play now (queue_jump); the player must follow */
    QEV_CURRENT,            /* player reported it plays index (queue_set_current) */
    QEV_META                /* units changed (collapse, pop, split); no item moved */
};
typedef struct {
    int type, index, count, to;
} queue_event;
typedef void (*queue_listener)(const queue_event *ev, void *ctx);

enum { QREPEAT_OFF, QREPEAT_ALL, QREPEAT_ONE };

/* Error codes (negative). */
enum { QERR_ARG = -1, QERR_PLACE = -2, QERR_DB = -3, QERR_NOMEM = -4, QERR_UNDO = -5 };

/* Opens listening.db (NULL = LISTENINGDB_PATH; another path is for tests) and loads the live queue. If the file can't be used the queue
 * still works in memory only; queue_persistent() says which. */
int  queue_open(const char *path);
void queue_close(void);
int  queue_persistent(void);

int  queue_listen(queue_listener fn, void *ctx);   /* up to 4 listeners */
void queue_unlisten(queue_listener fn, void *ctx);

/* Reading. Pointers stay valid until the next change. */
int  queue_count(void);
int  queue_current(void);                          /* -1 when empty */
int  queue_remaining(void);                        /* items from the playing one to the end */
const queue_item *queue_get(int index);
const queue_unit *queue_unit_get(int64_t unit_id);
int  queue_find(int64_t item_id);                  /* index or -1 */
int  queue_unit_range(int64_t unit_id, int *first);/* item count, first index in *first */
unsigned queue_version(void);                      /* bumps on every change */

int  queue_row_count(void);
int  queue_row_get(int r, queue_row *out);         /* 0 or QERR_ARG */

/* Editing. All return 0 or a negative QERR_*. */
int  queue_replace(const queue_track *t, int n, const queue_album *unit, int start);
int  queue_add_next(const queue_track *t, int n, const queue_album *unit);
int  queue_add_last(const queue_track *t, int n, const queue_album *unit);
/* Drag a song to land before item `before` (count() = the end). into_unit is the song's own unit to keep
 * it in (dropped between or next to that unit's songs), or 0 to make it loose (the unit shrinks). A song
 * is never dropped inside another unit. */
int  queue_move_item(int index, int before, int64_t into_unit);
int  queue_move_unit(int64_t unit_id, int before);
/* 1 = that drop is allowed (dropping onto itself counts): for showing where a drag can land. */
int  queue_can_move_item(int index, int before, int64_t into_unit);
int  queue_can_move_unit(int64_t unit_id, int before);
int  queue_remove_item(int index);
int  queue_remove_unit(int64_t unit_id);
int  queue_undo(void);                             /* brings back the last removal, if nothing else changed */
int  queue_can_undo(void);
int  queue_pop_unit(int64_t unit_id);              /* songs become loose, in place */
int  queue_set_collapsed(int64_t unit_id, int collapsed);
int  queue_jump(int index);                        /* user picked an item to play */

/* Play state. */
int  queue_set_current(int index);                 /* from the player adapter */
void queue_set_position(int ms, int paused);
int  queue_position(int *paused);
void queue_set_repeat(int mode);
int  queue_repeat(void);
void queue_set_shuffle(int on);                    /* stored only; shuffle with units comes later */
int  queue_shuffle(void);

#endif
