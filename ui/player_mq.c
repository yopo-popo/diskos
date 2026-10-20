/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* player_mq - the stock mq_player adapter, the only diskOS code that knows how mq_player keeps its play queue.
 *
 * mq_player plays LIST_SONG_0 (song.db) in ID order and reports the row it plays (the a2 pos_id). The adapter keeps that
 * table equal to our queue, row n = queue item n-1, starts it with the type-0 jump (ui_play_row) and reports the playing
 * row back with queue_set_current. Of mq_player's data it uses only LIST_SONG_0 (yopo, 2026-10-18).
 *
 * The other play paths (search, files, playlists, books) still have mq_player build a list of its own; they call
 * player_release first, and LIST_SONG_0 is then left alone until the next play of our queue. A list the player loads by
 * itself (Play Through Folders, after ours ends) is noticed when the playing row stops holding our song.
 *
 * Assumed, not yet checked on the Disc (yopo 2026-10-20: check later): the player plays a row whose file has no SONG
 * row, and the jump starts a freshly written LIST_SONG_0 from a stopped player too. CUE and ISO rows are untested.
 *
 * All calls are from the UI thread. */
#include "player_mq.h"
#include "screens.h"
#include "modes.h"
#include "config.h"
#include "ipc.h"
#include "sqlite3.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef PMQ_SONGDB
#define PMQ_SONGDB "/usr/data/fiio/db/song.db"
#endif
#define PMQ_TICK_MS  250
#define PMQ_START_MS 10000   /* after a start, how long the old song may still be reported */
#define PMQ_FOREVER  86400000   /* DURATION for a song of unknown length: a 0 row is skipped as already played */

int ui_play_row(int ord1, int fresh);   /* main.c: type-0 jump to row ord1; fresh = a new list. 1 = sent */

static sqlite3 *g_db;
static int g_owned;              /* LIST_SONG_0 holds our queue */
static int g_want = -1;          /* after a start: the item jumped to, until the player reports it */
static uint32_t g_want_at;
static unsigned g_checked_seq;   /* the track change last checked against LIST_SONG_0 */

static int db_open(void){
    if(g_db) return 1;
    if(sqlite3_open_v2(PMQ_SONGDB, &g_db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK){
        fprintf(stderr, "player_mq: can't open %s: %s\n", PMQ_SONGDB, sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return 0;
    }
    sqlite3_busy_timeout(g_db, 2000);   /* mq_player writes song.db too */
    return 1;
}

static int is_iso(const char *p){
    size_t n = strlen(p);
    return n > 4 && !strcasecmp(p + n - 4, ".iso");
}

/* Runs sql with ?1 = x, ?2 = y. 1 = done. */
static int sql2(const char *sql, int x, int y){
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, x);
    sqlite3_bind_int(st, 2, y);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Rows from+1..to hold queue items from..to-1 (the rows must not exist). The columns only the player uses get the values
 * its own type-5 lists have. 1 = written. */
static int rows_put(int from, int to){
    sqlite3_stmt *ins;
    if(sqlite3_prepare_v2(g_db, "INSERT INTO LIST_SONG_0 (ID,LIST_ID,POS_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,"
                                "IS_CUE,IS_ISO,OFFSET,DURATION,ADD_TIME,IS_SELECT,SONG_TYPE,ALBUM_ARTIST,IS_M3U,M3U_PATH) "
                                "VALUES (?1,NULL,NULL,?2,?3,?4,?5,?6,'',0,0,?7,?8,?9,?10,NULL,NULL,5,?11,0,'')",
                          -1, &ins, NULL) != SQLITE_OK) return 0;
    int ok = 1;
    for(int i = from; ok && i < to; i++){
        const queue_item *it = queue_get(i);
        const queue_unit *u = it->unit_id ? queue_unit_get(it->unit_id) : NULL;
        const char *name = strrchr(it->path, '/');
        name = name ? name + 1 : it->path;
        int iso = is_iso(it->path), cue = !iso && (it->start_ms > 0 || it->end_ms > 0);
        int len = it->end_ms > it->start_ms ? it->end_ms - it->start_ms : it->duration_ms;
        sqlite3_reset(ins);
        sqlite3_bind_int(ins, 1, i + 1);
        sqlite3_bind_text(ins, 2, it->path, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, it->title[0] ? it->title : name, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 5, it->album, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 6, it->artist, -1, SQLITE_STATIC);
        sqlite3_bind_int(ins, 7, cue);
        sqlite3_bind_int(ins, 8, iso);
        sqlite3_bind_int64(ins, 9, it->start_ms);
        sqlite3_bind_int64(ins, 10, len > 0 ? len : PMQ_FOREVER);
        sqlite3_bind_text(ins, 11, u ? u->album_artist : it->artist, -1, SQLITE_STATIC);
        ok = sqlite3_step(ins) == SQLITE_DONE;
    }
    sqlite3_finalize(ins);
    return ok;
}

/* One change to LIST_SONG_0, in one transaction, that keeps its rows 1..N equal to our queue. Inserting or removing
 * rewrites every row after the change: moving the rows' IDs instead was over ten times slower in the host test.
 * 1 = written. */
enum { ROWS_FROM, ROWS_RANGE };
static const char *const ROWS_OP[] = { "wrote", "rewrote" };
static int rows_change(int op, int a, int b){
    if(!db_open()) return 0;
    uint32_t t0 = lv_tick_get();
    if(op == ROWS_FROM) b = queue_count();
    int ok = sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK
          && sql2(op == ROWS_FROM ? "DELETE FROM LIST_SONG_0 WHERE ID > ?1"                 /* items a.. are new */
                                  : "DELETE FROM LIST_SONG_0 WHERE ID > ?1 AND ID <= ?2",   /* items a..b-1 changed */
                  a, b)
          && rows_put(a, b);
    if(ok) ok = sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if(!ok){
        fprintf(stderr, "player_mq: can't change the player's list: %s\n", sqlite3_errmsg(g_db));
        if(!sqlite3_get_autocommit(g_db)) sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }
    fprintf(stderr, "player_mq: %s rows %d-%d of %d in %u ms\n", ROWS_OP[op], a + 1, b, queue_count(),
            (unsigned)lv_tick_elaps(t0));
    return 1;
}

/* Does LIST_SONG_0 hold exactly our queue? 1 yes, 0 no, -1 can't read it. */
static int rows_match(void){
    if(!db_open()) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_db, "SELECT ID, PATH FROM LIST_SONG_0 ORDER BY ID", -1, &st, NULL) != SQLITE_OK) return -1;
    int n = queue_count(), i = 0, rc;
    while((rc = sqlite3_step(st)) == SQLITE_ROW){
        const char *p = (const char *)sqlite3_column_text(st, 1);
        if(i >= n || sqlite3_column_int(st, 0) != i + 1 || !p || strcmp(p, queue_get(i)->path)) break;
        i++;
    }
    sqlite3_finalize(st);
    if(rc == SQLITE_ROW) return 0;
    return rc == SQLITE_DONE ? i == n : -1;
}

/* Make the player play queue item index, writing its list first when it isn't ours. 1 = sent. */
static int start(int index, int fresh){
    if(index < 0 || index >= queue_count()) return 0;
    if(!g_owned){
        if(!rows_change(ROWS_FROM, 0, 0)){ ui_toast("Couldn't start playback"); return 0; }
        g_owned = 1;
        fresh = 1;
    }
    if(!ui_play_row(index + 1, fresh)) return 0;   /* refused or not sent: toasted there */
    g_want = index;
    g_want_at = lv_tick_get();
    return 1;
}

/* Mirror a queue change while the player plays our queue. Edits never reach the playing row, so it plays on undisturbed;
 * a reset rewrites everything and player_play starts it (and says so if that fails). */
static void edited(int op, int a, int b){
    if(!g_owned) return;          /* written in full at the next start */
    if(rows_change(op, a, b)) return;
    g_owned = 0;
    if(op != ROWS_FROM) ui_toast("Couldn't update the player's queue");
}

static void on_queue(const queue_event *ev, void *ctx){
    (void)ctx;
    switch(ev->type){
    case QEV_RESET:  edited(ROWS_FROM, 0, 0); break;
    case QEV_INSERT:
    case QEV_REMOVE: edited(ROWS_FROM, ev->index, 0); break;
    case QEV_MOVE:   edited(ROWS_RANGE, ev->index < ev->to ? ev->index : ev->to,
                            (ev->index > ev->to ? ev->index : ev->to) + ev->count); break;
    case QEV_JUMP:   start(ev->index, 0); break;
    }
}

/* The song paths of a2 frames are cut at 255 bytes. */
static int same_path(const char *ours, const char *player){
    size_t n = strlen(player);
    return n >= sizeof ((track_state_t *)0)->path - 1 ? !strncmp(ours, player, n) : !strcmp(ours, player);
}

/* Which queue item the player plays: the reported row, else the current or the next item when it holds this song. */
static int playing_index(const track_state_t *st){
    int n = queue_count(), i = st->pos_id - 1, c = queue_current();
    if(i >= 0 && i < n && same_path(queue_get(i)->path, st->path)) return i;
    if(c >= 0 && same_path(queue_get(c)->path, st->path)) return c;
    if(c >= 0 && c + 1 < n && same_path(queue_get(c + 1)->path, st->path)) return c + 1;
    return -1;
}

static void tick(lv_timer_t *t){
    (void)t;
    if(!g_owned) return;
    track_state_t st;
    ipc_get_state(&st);
    if(!st.have_track || st.provisional || !st.path[0]) return;
    int i = playing_index(&st);
    if(g_want >= 0){
        if(i != g_want && lv_tick_elaps(g_want_at) < PMQ_START_MS) return;   /* the old song until the jump lands */
        g_want = -1;
    }
    if(i < 0){
        if(st.path_seq == g_checked_seq) return;
        g_checked_seq = st.path_seq;
        if(rows_match() == 0){
            g_owned = 0;
            fprintf(stderr, "player_mq: the player loaded a list of its own; leaving it alone\n");
        }
        return;
    }
    if(i != queue_current()) queue_set_current(i);
    if((int)(st.pos_seq - st.path_seq) > 0) queue_set_position((int)st.position_ms, !ui_is_playing());
}

void player_init(void){
    static lv_timer_t *ticker;
    if(!ticker){
        queue_listen(on_queue, NULL);
        ticker = lv_timer_create(tick, PMQ_TICK_MS, NULL);
    }
    int m = rows_match();
    g_owned = m == 1;
    fprintf(stderr, "player_mq: the player's list %s our queue of %d\n",
            m == 1 ? "is" : m == 0 ? "isn't" : "can't be read, taken as not", queue_count());
}

int player_play(const queue_track *t, int n, const queue_album *unit, int start_at){
    if(modes_output_busy()){ ui_toast("Switching output - try again"); return 0; }
    if(!ui_local_playback_allowed()){ ui_toast("Return to local playback first"); return 0; }
    if(!t || n <= 0 || start_at < 0 || start_at >= n){ ui_toast("Couldn't play that list"); return 0; }
    queue_track *v = NULL;
    if(cfg_get_int("work_mode", 0) == 1 && n > 1){   /* Shuffle: the chosen song first, then the rest in a random order */
        v = malloc((size_t)n * sizeof *v);
        if(!v){ ui_toast("Couldn't play that list"); return 0; }
        memcpy(v, t, (size_t)n * sizeof *v);
        queue_track c = v[0]; v[0] = v[start_at]; v[start_at] = c;
        for(int i = n - 1; i > 1; i--){
            int j = 1 + rand() % i;
            c = v[i]; v[i] = v[j]; v[j] = c;
        }
        t = v; start_at = 0; unit = NULL;
    }
    int rc = queue_replace(t, n, unit, start_at);
    free(v);
    if(rc < 0){ ui_toast("Couldn't play that list"); return 0; }
    return start(start_at, 1);
}

int player_owns(void){ return g_owned; }

int player_release(void){
    int was = g_owned;
    g_owned = 0;
    g_want = -1;
    return was;
}
