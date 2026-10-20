/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* queue - the play queue (see queue.h). Memory is the working copy; listening.db is written first in
 * one transaction per edit, and memory changes only after that commits. If listening.db can't be written
 * the queue carries on in memory only. */
#include "queue.h"
#include "librarydb.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUEUE_SCHEMA 1
#define POS_SAVE_MS 10000   /* playing position is written at most this often (plus on pause) */

static sqlite3 *g_db;
static int64_t g_list;                  /* live queue_list id, 0 = none yet */
static queue_item **g_it;
static int g_n, g_cap;
static queue_unit **g_un;
static int g_nu, g_ucap;
static int g_cur = -1;
static unsigned g_ver = 1, g_edit_ver = 1;
static int g_pos_ms, g_paused, g_saved_pos_ms, g_saved_paused;
static int g_repeat, g_shuffle;
static int64_t g_max_id;                /* ids handed out in memory-only mode continue from here */

static struct { queue_listener fn; void *ctx; } g_lis[4];

static queue_row *g_rows;
static int g_nrows, g_rcap;
static unsigned g_rows_ver;

static struct {
    int valid;
    unsigned edit_ver;
    int index, n;
    queue_item **items;
    int64_t unit_id;                    /* unit the items were in, 0 = loose */
    queue_unit *gone;                   /* the unit itself, when it went with them */
} g_undo;

/* ---------- small helpers ---------- */

static char *dupz(const char *s) { return strdup(s ? s : ""); }

static void item_free(queue_item *it){
    if(!it) return;
    free(it->path); free(it->title); free(it->artist); free(it->album);
    free(it);
}

static void unit_free(queue_unit *u){
    if(!u) return;
    free(u->album_artist); free(u->album);
    free(u);
}

static queue_item *item_new(const queue_track *t){
    queue_item *it = calloc(1, sizeof *it);
    if(!it) return NULL;
    it->path = dupz(t->path); it->title = dupz(t->title);
    it->artist = dupz(t->artist); it->album = dupz(t->album);
    if(!it->path || !it->title || !it->artist || !it->album) { item_free(it); return NULL; }
    it->start_ms = t->start_ms; it->end_ms = t->end_ms; it->duration_ms = t->duration_ms;
    return it;
}

static queue_unit *unit_new(const char *aa, const char *al, int collapsed){
    queue_unit *u = calloc(1, sizeof *u);
    if(!u) return NULL;
    u->album_artist = dupz(aa); u->album = dupz(al);
    if(!u->album_artist || !u->album) { unit_free(u); return NULL; }
    u->collapsed = collapsed;
    return u;
}

static queue_item **items_new(const queue_track *t, int n){
    queue_item **v = calloc(n ? n : 1, sizeof *v);
    if(!v) return NULL;
    for(int i = 0; i < n; i++){
        if(!(v[i] = item_new(&t[i]))){
            while(i--) item_free(v[i]);
            free(v);
            return NULL;
        }
    }
    return v;
}

static void items_free(queue_item **v, int n){
    if(!v) return;
    for(int i = 0; i < n; i++) item_free(v[i]);
    free(v);
}

static void emit(int type, int index, int count, int to){
    queue_event ev = { type, index, count, to };
    g_ver++;
    for(int i = 0; i < 4; i++)
        if(g_lis[i].fn) g_lis[i].fn(&ev, g_lis[i].ctx);
}

static void undo_clear(void){
    if(g_undo.items) items_free(g_undo.items, g_undo.n);
    unit_free(g_undo.gone);
    memset(&g_undo, 0, sizeof g_undo);
}

static int unit_idx(int64_t id){
    for(int i = 0; i < g_nu; i++) if(g_un[i]->id == id) return i;
    return -1;
}

static int range_of(int64_t uid, int *first){
    int f = -1, c = 0;
    if(uid){
        for(int i = 0; i < g_n; i++){
            if(g_it[i]->unit_id != uid) { if(f >= 0) break; continue; }
            if(f < 0) f = i;
            c++;
        }
    }
    if(first) *first = f;
    return c;
}

/* Unit whose songs sit on both sides of the gap before index p (0 = not inside a unit). */
static int64_t gap_unit(int p){
    if(p <= 0 || p >= g_n) return 0;
    int64_t a = g_it[p - 1]->unit_id;
    return (a && a == g_it[p]->unit_id) ? a : 0;
}

static int64_t cur_id(void) { return g_cur >= 0 ? g_it[g_cur]->id : 0; }
static int64_t cur_unit(void) { return g_cur >= 0 && g_cur < g_n ? g_it[g_cur]->unit_id : 0; }

/* The unit's songs after the playing one: all of them for a unit still to come, the later ones for the unit holding
 * the playing song, none for a played unit. */
static int later_of(int64_t uid, int *first){
    int f, c = range_of(uid, &f);
    if(c && f <= g_cur){
        int e = f + c;
        f = g_cur + 1;
        c = e > f ? e - f : 0;
    }
    if(first) *first = c ? f : -1;
    return c;
}

/* ---------- memory changes (called only after the database committed) ---------- */

static int mem_reserve(int extra_items, int extra_units){
    if(g_n + extra_items > g_cap){
        int cap = g_cap ? g_cap : 64;
        while(cap < g_n + extra_items) cap *= 2;
        queue_item **v = realloc(g_it, (size_t)cap * sizeof *v);
        if(!v) return QERR_NOMEM;
        g_it = v; g_cap = cap;
    }
    if(g_nu + extra_units > g_ucap){
        int cap = g_ucap ? g_ucap * 2 : 16;
        while(cap < g_nu + extra_units) cap *= 2;
        queue_unit **v = realloc(g_un, (size_t)cap * sizeof *v);
        if(!v) return QERR_NOMEM;
        g_un = v; g_ucap = cap;
    }
    return 0;
}

static void mem_insert(int p, queue_item **v, int n){
    memmove(g_it + p + n, g_it + p, (size_t)(g_n - p) * sizeof *g_it);
    memcpy(g_it + p, v, (size_t)n * sizeof *g_it);
    g_n += n;
}

static void mem_cut(int p, int n, queue_item **out){
    if(out) memcpy(out, g_it + p, (size_t)n * sizeof *g_it);
    memmove(g_it + p, g_it + p + n, (size_t)(g_n - p - n) * sizeof *g_it);
    g_n -= n;
}

/* Block [a, a+c) ends up starting at index t of the new list. tmp holds c pointers. */
static void mem_move(int a, int c, int t, queue_item **tmp){
    mem_cut(a, c, tmp);
    mem_insert(t, tmp, c);
}

static void unit_push(queue_unit *u) { g_un[g_nu++] = u; }

/* The songs of unit uid from index `from` on (all but `skip`) go to the new unit u2 (memory half of a split). */
static void mem_split(int64_t uid, int from, int skip, queue_unit *u2){
    for(int i = from; i < g_n && g_it[i]->unit_id == uid; i++)
        if(i != skip) g_it[i]->unit_id = u2->id;
    unit_push(u2);
}

static queue_unit *unit_take(int64_t id){
    int i = unit_idx(id);
    if(i < 0) return NULL;
    queue_unit *u = g_un[i];
    g_un[i] = g_un[--g_nu];
    return u;
}

/* ---------- database ---------- */

static int dbx(const char *sql, const int64_t *a, int n){
    if(!g_db) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK){
        fprintf(stderr, "queue: %s: %s\n", sqlite3_errmsg(g_db), sql);
        return QERR_DB;
    }
    for(int i = 0; i < n; i++) sqlite3_bind_int64(st, i + 1, a[i]);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc != SQLITE_DONE && rc != SQLITE_ROW){
        fprintf(stderr, "queue: %s: %s\n", sqlite3_errmsg(g_db), sql);
        return QERR_DB;
    }
    return 0;
}
#define DBX(sql, ...) dbx(sql, (const int64_t[]){ __VA_ARGS__ }, \
                          (int)(sizeof((const int64_t[]){ __VA_ARGS__ }) / sizeof(int64_t)))

static int tx_begin(void){
    if(!g_db) return 0;
    return sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK ? 0 : QERR_DB;
}

static int tx_end(int rc){
    if(!g_db) return rc;
    if(rc == 0 && sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) return 0;
    if(!sqlite3_get_autocommit(g_db)) sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
    return rc ? rc : QERR_DB;
}

static void db_drop(void){
    fprintf(stderr, "queue: can't write listening.db (%s), keeping the queue in memory only\n",
            g_db ? sqlite3_errmsg(g_db) : "?");
    if(g_db) sqlite3_close(g_db);
    g_db = NULL;
}

static int64_t new_id(void){
    if(!g_db) return ++g_max_id;
    int64_t id = sqlite3_last_insert_rowid(g_db);
    if(id > g_max_id) g_max_id = id;
    return id;
}

/* Created outside the edit's transaction; an unused empty list left by a failed edit is harmless. */
static int sql_ensure_list(void){
    if(!g_db || g_list) return 0;
    if(DBX("INSERT INTO queue_list (created) VALUES (?)", (int64_t)time(NULL))) return QERR_DB;
    g_list = new_id();
    return 0;
}

static int sql_shift(int from1, int to1, int delta){
    return DBX("UPDATE queue_item SET pos = pos + ?3 WHERE queue_id = ?4 AND pos BETWEEN ?1 AND ?2",
               from1, to1, delta, g_list);
}

static int sql_set_pos(int64_t id, int pos1) { return DBX("UPDATE queue_item SET pos = ? WHERE id = ?", pos1, id); }

static int sql_set_unit(int64_t item, int64_t unit){
    return DBX("UPDATE queue_item SET unit_id = NULLIF(?, 0) WHERE id = ?", unit, item);
}

static int sql_del_unit(int64_t id) { return DBX("DELETE FROM queue_unit WHERE id = ?", id); }

static int sql_new_unit(int64_t list, queue_unit *u){
    if(g_db){
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(g_db, "INSERT INTO queue_unit (queue_id, album_artist, album, collapsed) "
                               "VALUES (?, ?, ?, ?)", -1, &st, NULL) != SQLITE_OK) return QERR_DB;
        sqlite3_bind_int64(st, 1, list);
        sqlite3_bind_text(st, 2, u->album_artist, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, u->album, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 4, u->collapsed);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if(rc != SQLITE_DONE) return QERR_DB;
    }
    u->id = new_id();
    return 0;
}

/* Inserts v[0..n) at 1-based pos1.. with each item's unit_id as set; fills in their ids. */
static int sql_insert(int64_t list, queue_item **v, int n, int pos1){
    sqlite3_stmt *st = NULL;
    if(g_db && sqlite3_prepare_v2(g_db, "INSERT INTO queue_item (queue_id, pos, unit_id, path, start_ms, "
                                   "end_ms, duration_ms, title, artist, album) "
                                   "VALUES (?, ?, NULLIF(?, 0), ?, ?, ?, ?, ?, ?, ?)", -1, &st, NULL) != SQLITE_OK)
        return QERR_DB;
    for(int i = 0; i < n; i++){
        if(st){
            queue_item *it = v[i];
            sqlite3_bind_int64(st, 1, list);
            sqlite3_bind_int(st, 2, pos1 + i);
            sqlite3_bind_int64(st, 3, it->unit_id);
            sqlite3_bind_text(st, 4, it->path, -1, SQLITE_STATIC);
            sqlite3_bind_int(st, 5, it->start_ms);
            sqlite3_bind_int(st, 6, it->end_ms);
            sqlite3_bind_int(st, 7, it->duration_ms);
            sqlite3_bind_text(st, 8, it->title, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 9, it->artist, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 10, it->album, -1, SQLITE_STATIC);
            if(sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return QERR_DB; }
            sqlite3_reset(st);
        }
        v[i]->id = new_id();
    }
    if(st) sqlite3_finalize(st);
    return 0;
}

static int sql_state(int64_t list, int64_t current){
    return DBX("INSERT OR REPLACE INTO queue_state (id, queue_id, current_item_id, position_ms, paused, "
               "repeat, shuffle) VALUES (1, NULLIF(?, 0), NULLIF(?, 0), ?, ?, ?, ?)",
               list, current, g_pos_ms, g_paused, g_repeat, g_shuffle);
}

/* Database half of mem_split: creates u2 and moves the songs to it. */
static int sql_split(int64_t uid, int from, int skip, queue_unit *u2){
    int rc = sql_new_unit(g_list, u2);
    for(int i = from; !rc && i < g_n && g_it[i]->unit_id == uid; i++)
        if(i != skip) rc = sql_set_unit(g_it[i]->id, u2->id);
    return rc;
}

/* Block [a, a+c) moves to start at index t of the new list. */
static int sql_move(int a, int c, int t){
    int rc = 0;
    for(int k = 0; !rc && k < c; k++) rc = sql_set_pos(g_it[a + k]->id, -(k + 1));
    if(!rc) rc = t > a ? sql_shift(a + c + 1, t + c, -c) : sql_shift(t + 1, a, c);
    for(int k = 0; !rc && k < c; k++) rc = sql_set_pos(g_it[a + k]->id, t + 1 + k);
    return rc;
}

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS queue_list ("
    " id INTEGER PRIMARY KEY, created INTEGER NOT NULL, ended INTEGER);"
    "CREATE TABLE IF NOT EXISTS queue_unit ("
    " id INTEGER PRIMARY KEY, queue_id INTEGER NOT NULL REFERENCES queue_list (id),"
    " album_artist TEXT NOT NULL, album TEXT NOT NULL, collapsed INTEGER NOT NULL DEFAULT 1);"
    "CREATE TABLE IF NOT EXISTS queue_item ("
    " id INTEGER PRIMARY KEY, queue_id INTEGER NOT NULL REFERENCES queue_list (id),"
    " pos INTEGER NOT NULL, unit_id INTEGER REFERENCES queue_unit (id),"
    " path TEXT NOT NULL, start_ms INTEGER NOT NULL DEFAULT 0, end_ms INTEGER NOT NULL DEFAULT 0,"
    " duration_ms INTEGER NOT NULL DEFAULT 0,"
    " title TEXT NOT NULL DEFAULT '', artist TEXT NOT NULL DEFAULT '', album TEXT NOT NULL DEFAULT '');"
    "CREATE INDEX IF NOT EXISTS queue_item_order ON queue_item (queue_id, pos);"
    "CREATE INDEX IF NOT EXISTS queue_item_unit ON queue_item (unit_id);"
    "CREATE TABLE IF NOT EXISTS queue_state ("
    " id INTEGER PRIMARY KEY CHECK (id = 1), queue_id INTEGER, current_item_id INTEGER,"
    " position_ms INTEGER NOT NULL DEFAULT 0, paused INTEGER NOT NULL DEFAULT 0,"
    " repeat INTEGER NOT NULL DEFAULT 0, shuffle INTEGER NOT NULL DEFAULT 0);";

/* 0 = ready, 1 = file is from a newer queue schema (leave it alone), <0 = error. */
static int db_schema(void){
    int rc = tx_begin(), ver = 0;
    if(!rc && sqlite3_exec(g_db, "CREATE TABLE IF NOT EXISTS schema_part ("
                            "part TEXT PRIMARY KEY, version INTEGER NOT NULL)", NULL, NULL, NULL) != SQLITE_OK)
        rc = QERR_DB;
    if(!rc){
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(g_db, "SELECT version FROM schema_part WHERE part = 'queue'", -1, &st, NULL)
            != SQLITE_OK) rc = QERR_DB;
        else {
            if(sqlite3_step(st) == SQLITE_ROW) ver = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
    }
    if(!rc && ver > QUEUE_SCHEMA) { tx_end(QERR_DB); return 1; }
    if(!rc && ver < QUEUE_SCHEMA){
        if(sqlite3_exec(g_db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) rc = QERR_DB;
        if(!rc) rc = DBX("INSERT OR REPLACE INTO schema_part (part, version) VALUES ('queue', ?)", QUEUE_SCHEMA);
    }
    return tx_end(rc);
}

static char *col_dup(sqlite3_stmt *st, int i) { return dupz((const char *)sqlite3_column_text(st, i)); }

static int db_load(void){
    sqlite3_stmt *st;
    int64_t list = 0, current = 0;
    if(sqlite3_prepare_v2(g_db, "SELECT s.queue_id, s.current_item_id, s.position_ms, s.paused, s.repeat, "
                           "s.shuffle FROM queue_state s JOIN queue_list l ON l.id = s.queue_id "
                           "WHERE s.id = 1 AND l.ended IS NULL", -1, &st, NULL) != SQLITE_OK) return QERR_DB;
    if(sqlite3_step(st) == SQLITE_ROW){
        list = sqlite3_column_int64(st, 0);
        current = sqlite3_column_int64(st, 1);
        g_pos_ms = g_saved_pos_ms = sqlite3_column_int(st, 2);
        g_paused = g_saved_paused = sqlite3_column_int(st, 3);
        g_repeat = sqlite3_column_int(st, 4);
        g_shuffle = sqlite3_column_int(st, 5);
    }
    sqlite3_finalize(st);
    if(!list) return 0;
    g_list = list;

    if(sqlite3_prepare_v2(g_db, "SELECT id, album_artist, album, collapsed FROM queue_unit WHERE queue_id = ?",
                           -1, &st, NULL) != SQLITE_OK) return QERR_DB;
    sqlite3_bind_int64(st, 1, list);
    while(sqlite3_step(st) == SQLITE_ROW){
        queue_unit *u = unit_new((const char *)sqlite3_column_text(st, 1),
                                 (const char *)sqlite3_column_text(st, 2), sqlite3_column_int(st, 3));
        if(!u || mem_reserve(0, 1)) { unit_free(u); sqlite3_finalize(st); return QERR_NOMEM; }
        u->id = sqlite3_column_int64(st, 0);
        if(u->id > g_max_id) g_max_id = u->id;
        unit_push(u);
    }
    sqlite3_finalize(st);

    int bad_pos = 0;
    if(sqlite3_prepare_v2(g_db, "SELECT id, unit_id, path, start_ms, end_ms, duration_ms, title, artist, album, "
                           "pos FROM queue_item WHERE queue_id = ? ORDER BY pos, id", -1, &st, NULL) != SQLITE_OK)
        return QERR_DB;
    sqlite3_bind_int64(st, 1, list);
    while(sqlite3_step(st) == SQLITE_ROW){
        queue_item *it = calloc(1, sizeof *it);
        if(!it || mem_reserve(1, 0)) { free(it); sqlite3_finalize(st); return QERR_NOMEM; }
        it->id = sqlite3_column_int64(st, 0);
        it->unit_id = sqlite3_column_int64(st, 1);
        if(it->unit_id && unit_idx(it->unit_id) < 0) it->unit_id = 0;
        it->path = col_dup(st, 2);
        it->start_ms = sqlite3_column_int(st, 3);
        it->end_ms = sqlite3_column_int(st, 4);
        it->duration_ms = sqlite3_column_int(st, 5);
        it->title = col_dup(st, 6); it->artist = col_dup(st, 7); it->album = col_dup(st, 8);
        if(!it->path || !it->title || !it->artist || !it->album) { item_free(it); sqlite3_finalize(st); return QERR_NOMEM; }
        if(sqlite3_column_int(st, 9) != g_n + 1) bad_pos = 1;
        if(it->id > g_max_id) g_max_id = it->id;
        g_it[g_n++] = it;
    }
    sqlite3_finalize(st);

    /* Units left without songs are dropped. */
    for(int i = g_nu - 1; i >= 0; i--)
        if(!range_of(g_un[i]->id, NULL)) unit_free(unit_take(g_un[i]->id));

    if(bad_pos) {   /* shouldn't happen; renumber so pos = index + 1 again */
        int rc = tx_begin();
        for(int i = 0; !rc && i < g_n; i++) rc = sql_set_pos(g_it[i]->id, i + 1);
        if(tx_end(rc)) return QERR_DB;
    }
    g_cur = g_n ? 0 : -1;
    for(int i = 0; i < g_n; i++) if(g_it[i]->id == current) { g_cur = i; break; }
    return 0;
}

/* ---------- open / close ---------- */

static void mem_clear(void){
    for(int i = 0; i < g_n; i++) item_free(g_it[i]);
    for(int i = 0; i < g_nu; i++) unit_free(g_un[i]);
    g_n = g_nu = 0;
    g_cur = -1;
    undo_clear();
}

int queue_open(const char *path){
    queue_close();
    if(!path){
        path = LISTENINGDB_PATH;
        ldb_make_dir();
    }
    /* ldb_open turns foreign keys on and waits up to 2 s for another writer. WAL as agreed for listening.db: the
     * history part's connection and ours never block each other's readers. */
    int wal = 0;
    if(ldb_open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &g_db) == SQLITE_OK){
        sqlite3_stmt *st = NULL;
        if(sqlite3_prepare_v2(g_db, "PRAGMA journal_mode=WAL", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            wal = !strcmp((const char *)sqlite3_column_text(st, 0), "wal");
        sqlite3_finalize(st);
    }
    if(!wal || sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL) != SQLITE_OK){
        fprintf(stderr, "queue: can't open %s, queue kept in memory only\n", path);
        if(g_db) sqlite3_close(g_db);
        g_db = NULL;
        emit(QEV_RESET, 0, 0, 0);
        return 0;
    }
    int rc = db_schema();
    if(rc == 1) fprintf(stderr, "queue: %s has a newer queue schema, queue kept in memory only\n", path);
    if(rc == 0) rc = db_load();
    if(rc){
        mem_clear();
        g_list = 0;
        g_pos_ms = g_paused = g_repeat = g_shuffle = 0;
        if(rc != 1) fprintf(stderr, "queue: can't read %s, queue kept in memory only\n", path);
        sqlite3_close(g_db);
        g_db = NULL;
    }
    emit(QEV_RESET, 0, g_n, 0);
    return 0;
}

void queue_close(void){
    if(g_db){
        if(g_pos_ms != g_saved_pos_ms || g_paused != g_saved_paused) sql_state(g_list, cur_id());
        sqlite3_close(g_db);
        g_db = NULL;
    }
    mem_clear();
    free(g_it); g_it = NULL; g_cap = 0;
    free(g_un); g_un = NULL; g_ucap = 0;
    free(g_rows); g_rows = NULL; g_rcap = g_nrows = 0;
    g_list = 0;
    g_pos_ms = g_paused = g_saved_pos_ms = g_saved_paused = g_repeat = g_shuffle = 0;
    g_max_id = 0;
}

int queue_persistent(void) { return g_db != NULL; }

int queue_listen(queue_listener fn, void *ctx){
    for(int i = 0; i < 4; i++)
        if(!g_lis[i].fn) { g_lis[i].fn = fn; g_lis[i].ctx = ctx; return 0; }
    return QERR_ARG;
}

void queue_unlisten(queue_listener fn, void *ctx){
    for(int i = 0; i < 4; i++)
        if(g_lis[i].fn == fn && g_lis[i].ctx == ctx) g_lis[i].fn = NULL;
}

/* ---------- reading ---------- */

int queue_count(void) { return g_n; }
int queue_current(void) { return g_cur; }
int queue_remaining(void) { return g_cur >= 0 ? g_n - g_cur : 0; }
unsigned queue_version(void) { return g_ver; }
const queue_item *queue_get(int i) { return i >= 0 && i < g_n ? g_it[i] : NULL; }

const queue_unit *queue_unit_get(int64_t id){
    int i = unit_idx(id);
    return i >= 0 ? g_un[i] : NULL;
}

int queue_find(int64_t id){
    for(int i = 0; i < g_n; i++) if(g_it[i]->id == id) return i;
    return -1;
}

int queue_unit_range(int64_t id, int *first) { return range_of(id, first); }
int queue_unit_later(int64_t id, int *first) { return later_of(id, first); }

static int rows_reserve(int extra){
    if(g_nrows + extra <= g_rcap) return 0;
    int cap = g_rcap ? g_rcap : 256;
    while(cap < g_nrows + extra) cap *= 2;
    queue_row *v = realloc(g_rows, (size_t)cap * sizeof *v);
    if(!v) return QERR_NOMEM;
    g_rows = v; g_rcap = cap;
    return 0;
}

/* The playing song comes first, on its own; its album's later songs follow under the album's header. */
static int rows_build(void){
    if(g_rows_ver == g_ver) return 0;
    g_nrows = 0;
    if(g_cur >= 0 && g_cur < g_n){
        if(rows_reserve(1)) return QERR_NOMEM;
        g_rows[g_nrows++] = (queue_row){ QROW_ITEM, g_cur, 1, g_it[g_cur]->unit_id };
    }
    for(int i = g_cur < 0 ? g_n : g_cur + 1; i < g_n;){
        queue_item *it = g_it[i];
        if(rows_reserve(1)) return QERR_NOMEM;
        if(!it->unit_id){
            g_rows[g_nrows++] = (queue_row){ QROW_ITEM, i, 1, 0 };
            i++;
            continue;
        }
        int c = 0;
        while(i + c < g_n && g_it[i + c]->unit_id == it->unit_id) c++;
        g_rows[g_nrows++] = (queue_row){ QROW_UNIT, i, c, it->unit_id };
        int u = unit_idx(it->unit_id);
        if(u >= 0 && g_un[u]->collapsed){ i += c; continue; }
        if(rows_reserve(c)) return QERR_NOMEM;
        for(int k = 0; k < c; k++, i++) g_rows[g_nrows++] = (queue_row){ QROW_ITEM, i, 1, it->unit_id };
    }
    g_rows_ver = g_ver;
    return 0;
}

int queue_row_count(void) { return rows_build() ? 0 : g_nrows; }

int queue_row_get(int r, queue_row *out){
    if(rows_build() || r < 0 || r >= g_nrows) return QERR_ARG;
    *out = g_rows[r];
    return 0;
}

/* ---------- editing ---------- */

#define WITH_RETRY(call) do {                              \
        int rc_ = (call);                                  \
        if(rc_ == QERR_DB && g_db) { db_drop(); rc_ = (call); } \
        return rc_;                                        \
    } while(0)

static int replace_impl(const queue_track *t, int n, const queue_album *album, int start){
    queue_item **v = items_new(t, n);
    queue_unit *u = album && n ? unit_new(album->album_artist, album->album, 0) : NULL;
    if(!v || (album && n && !u)) { items_free(v, n); unit_free(u); return QERR_NOMEM; }
    int rc = mem_reserve(n > g_n ? n - g_n : 0, 1);   /* memory is cleared before the new items go in */

    int64_t list = 0, now = (int64_t)time(NULL);
    if(!rc) rc = tx_begin();
    if(!rc && g_list) rc = DBX("UPDATE queue_list SET ended = ? WHERE id = ?", now, g_list);
    if(!rc) {   /* keep the last QUEUE_KEEP_OLD replaced queues */
        const char *old = "SELECT id FROM queue_list WHERE ended IS NOT NULL ORDER BY id DESC LIMIT -1 OFFSET ?";
        char sql[256];
        snprintf(sql, sizeof sql, "DELETE FROM queue_item WHERE queue_id IN (%s)", old);
        rc = DBX(sql, QUEUE_KEEP_OLD);
        snprintf(sql, sizeof sql, "DELETE FROM queue_unit WHERE queue_id IN (%s)", old);
        if(!rc) rc = DBX(sql, QUEUE_KEEP_OLD);
        snprintf(sql, sizeof sql, "DELETE FROM queue_list WHERE id IN (%s)", old);
        if(!rc) rc = DBX(sql, QUEUE_KEEP_OLD);
    }
    if(!rc && n && g_db){
        rc = DBX("INSERT INTO queue_list (created) VALUES (?)", now);
        if(!rc) list = new_id();
    }
    if(!rc && u) rc = sql_new_unit(list, u);
    for(int i = 0; i < n; i++) v[i]->unit_id = u ? u->id : 0;
    if(!rc) rc = sql_insert(list, v, n, 1);
    g_pos_ms = 0; g_paused = 0;
    if(!rc) rc = sql_state(list, n ? v[start]->id : 0);
    rc = tx_end(rc);
    if(rc) { items_free(v, n); unit_free(u); return rc; }

    mem_clear();
    g_list = list;
    mem_insert(0, v, n);
    free(v);
    if(u) unit_push(u);
    g_cur = n ? start : -1;
    g_saved_pos_ms = 0; g_saved_paused = 0;
    g_edit_ver++;
    emit(QEV_RESET, 0, n, 0);
    return 0;
}

int queue_replace(const queue_track *t, int n, const queue_album *unit, int start){
    if(n < 0 || (n && (!t || start < 0 || start >= n))) return QERR_ARG;
    WITH_RETRY(replace_impl(t, n, unit, start));
}

/* Inserts tracks before index p (p > current). A gap inside a unit splits it: the songs after p become a
 * second unit of the same album, so the new songs play right there. */
static int add_impl(int p, const queue_track *t, int n, const queue_album *album, int *at){
    int was_empty = g_n == 0;
    int64_t split = gap_unit(p);
    queue_unit *su = split ? g_un[unit_idx(split)] : NULL;
    queue_unit *u2 = su ? unit_new(su->album_artist, su->album, su->collapsed) : NULL;
    queue_unit *nu = album ? unit_new(album->album_artist, album->album, was_empty ? 0 : 1) : NULL;
    queue_item **v = items_new(t, n);
    int rc = (!v || (su && !u2) || (album && !nu)) ? QERR_NOMEM : mem_reserve(n, 2);
    if(!rc) rc = sql_ensure_list();
    if(!rc) rc = tx_begin();
    if(!rc && u2) rc = sql_new_unit(g_list, u2);
    for(int i = p; !rc && u2 && i < g_n && g_it[i]->unit_id == split; i++) rc = sql_set_unit(g_it[i]->id, u2->id);
    if(!rc && nu) rc = sql_new_unit(g_list, nu);
    for(int i = 0; v && i < n; i++) v[i]->unit_id = nu ? nu->id : 0;
    if(!rc) rc = sql_shift(p + 1, INT_MAX, n);
    if(!rc) rc = sql_insert(g_list, v, n, p + 1);
    if(!rc && was_empty) rc = sql_state(g_list, v[0]->id);
    rc = tx_end(rc);
    if(rc) { items_free(v, n); unit_free(u2); unit_free(nu); return rc; }

    if(u2){
        for(int i = p; i < g_n && g_it[i]->unit_id == split; i++) g_it[i]->unit_id = u2->id;
        unit_push(u2);
    }
    if(nu) unit_push(nu);
    mem_insert(p, v, n);
    free(v);
    g_edit_ver++;
    *at = p;
    if(was_empty){
        g_cur = 0;
        emit(QEV_RESET, 0, n, 0);
    } else {
        emit(QEV_INSERT, p, n, 0);
    }
    return 0;
}

/* Right after the playing song, so the latest add next always plays first (yopo 2026-10-20). */
static int add_next_impl(const queue_track *t, int n, const queue_album *album){
    int at;
    return add_impl(g_cur + 1, t, n, album, &at);
}

int queue_add_next(const queue_track *t, int n, const queue_album *unit){
    if(!t || n <= 0) return QERR_ARG;
    WITH_RETRY(add_next_impl(t, n, unit));
}

static int add_last_impl(const queue_track *t, int n, const queue_album *album){
    int at;
    return add_impl(g_n, t, n, album, &at);
}

int queue_add_last(const queue_track *t, int n, const queue_album *unit){
    if(!t || n <= 0) return QERR_ARG;
    WITH_RETRY(add_last_impl(t, n, unit));
}

/* Neighbours of gap q in the list without block [a, a+c). */
static queue_item *red_at(int k, int a, int c) { return g_it[k < a ? k : k + c]; }

static int64_t red_gap_unit(int q, int a, int c){
    int m = g_n - c;
    if(q <= 0 || q >= m) return 0;
    int64_t l = red_at(q - 1, a, c)->unit_id;
    return (l && l == red_at(q, a, c)->unit_id) ? l : 0;
}

static int red_touches(int q, int a, int c, int64_t uid){
    int m = g_n - c;
    return (q > 0 && red_at(q - 1, a, c)->unit_id == uid) || (q < m && red_at(q, a, c)->unit_id == uid);
}

/* The gap right after the playing song is never inside its album, so something can play next ahead of the album's
 * later songs: landing there (gap q, counted without the moved songs) splits the album, as add next does. Returns
 * the album to split, or 0. */
static int64_t cur_split(int q, int64_t gap_uid){
    return q == g_cur + 1 && gap_uid && gap_uid == cur_unit() ? gap_uid : 0;
}

/* A new unit for the songs of `split` after the playing one (0 = none). */
static int split_new(int64_t split, queue_unit **u2){
    *u2 = NULL;
    if(!split) return 0;
    queue_unit *su = g_un[unit_idx(split)];
    if(!(*u2 = unit_new(su->album_artist, su->album, su->collapsed)) || mem_reserve(0, 1)){
        unit_free(*u2); *u2 = NULL;
        return QERR_NOMEM;
    }
    return 0;
}

static int move_item_impl(int index, int q, int64_t into, int64_t from_unit, int unit_size, int64_t split){
    queue_item *tmp[1];
    queue_unit *u2;
    int leaving = from_unit && into != from_unit;
    int rc = split_new(split, &u2);
    if(rc) return rc;
    rc = tx_begin();
    if(!rc && u2) rc = sql_split(split, g_cur + 1, index, u2);
    if(!rc && leaving) rc = sql_set_unit(g_it[index]->id, 0);
    if(!rc && leaving && unit_size == 1) rc = sql_del_unit(from_unit);
    if(!rc && q != index) rc = sql_move(index, 1, q);
    rc = tx_end(rc);
    if(rc){ unit_free(u2); return rc; }

    if(u2) mem_split(split, g_cur + 1, index, u2);
    if(leaving){
        g_it[index]->unit_id = 0;
        if(unit_size == 1) unit_free(unit_take(from_unit));
    }
    if(q != index) mem_move(index, 1, q, tmp);
    g_edit_ver++;
    if(q != index) emit(QEV_MOVE, index, 1, q);
    else emit(QEV_META, index, 1, 0);
    return 0;
}

/* 0 = the move is allowed (or changes nothing), else QERR_*; *q = where the song lands without it in the list,
 * *split = the playing album it splits (0 = none). */
static int move_item_check(int index, int before, int64_t into_unit, int *q, int64_t *split){
    if(index <= g_cur || index >= g_n || before < 0 || before > g_n) return QERR_ARG;
    int64_t own = g_it[index]->unit_id;
    if(into_unit && into_unit != own) return QERR_ARG;
    *q = before > index ? before - 1 : before;
    if(*q <= g_cur) return QERR_PLACE;
    int64_t gu = red_gap_unit(*q, index, 1);
    *split = gu != into_unit ? cur_split(*q, gu) : 0;
    if(gu && gu != into_unit && !*split) return QERR_PLACE;
    if(into_unit && range_of(own, NULL) > 1 && !red_touches(*q, index, 1, into_unit)) return QERR_PLACE;
    return 0;
}

int queue_can_move_item(int index, int before, int64_t into_unit){
    int q;
    int64_t split;
    return move_item_check(index, before, into_unit, &q, &split) == 0;
}

int queue_move_item(int index, int before, int64_t into_unit){
    int q;
    int64_t split;
    int rc = move_item_check(index, before, into_unit, &q, &split);
    if(rc) return rc;
    int64_t own = g_it[index]->unit_id;
    if(q == index && into_unit == own) return 0;
    WITH_RETRY(move_item_impl(index, q, into_unit, own, range_of(own, NULL), split));
}

/* split: the album whose songs after the playing one get a unit of their own (the moved songs themselves when the
 * playing album's later songs move, else the ones a drop right after the playing song goes ahead of). */
static int move_unit_impl(int f, int c, int q, int64_t split){
    queue_unit *u2;
    queue_item **tmp = malloc((size_t)c * sizeof *tmp);
    if(!tmp) return QERR_NOMEM;
    int rc = split_new(split, &u2);
    if(!rc) rc = tx_begin();
    if(!rc && u2) rc = sql_split(split, g_cur + 1, -1, u2);
    if(!rc) rc = sql_move(f, c, q);
    rc = tx_end(rc);
    if(!rc){
        if(u2) mem_split(split, g_cur + 1, -1, u2);
        mem_move(f, c, q, tmp);
        g_edit_ver++;
        emit(QEV_MOVE, f, c, q);
    } else unit_free(u2);
    free(tmp);
    return rc;
}

/* 0 = allowed, 1 = dropped onto itself (nothing to do), else QERR_*. The playing album moves its later songs. */
static int move_unit_check(int64_t uid, int before, int *f, int *c, int *q, int64_t *split){
    *c = later_of(uid, f);
    *split = *c && uid == cur_unit() ? uid : 0;
    if(!*c || before < 0 || before > g_n) return QERR_ARG;
    if(before >= *f && before <= *f + *c) return 1;
    *q = before > *f ? before - *c : before;
    if(*q <= g_cur) return QERR_PLACE;
    int64_t gu = red_gap_unit(*q, *f, *c);
    if(gu){
        if(*split || !cur_split(*q, gu)) return QERR_PLACE;
        *split = gu;
    }
    return 0;
}

int queue_can_move_unit(int64_t uid, int before){
    int f, c, q;
    int64_t split;
    return move_unit_check(uid, before, &f, &c, &q, &split) >= 0;
}

int queue_move_unit(int64_t uid, int before){
    int f, c, q;
    int64_t split;
    int rc = move_unit_check(uid, before, &f, &c, &q, &split);
    if(rc) return rc > 0 ? 0 : rc;
    WITH_RETRY(move_unit_impl(f, c, q, split));
}

/* Removes [p, p+c); whole = the unit uid goes with them. */
static int remove_impl(int p, int c, int64_t uid, int whole){
    queue_item **out = malloc((size_t)c * sizeof *out);
    if(!out) return QERR_NOMEM;
    int rc = tx_begin();
    for(int k = 0; !rc && k < c; k++) rc = DBX("DELETE FROM queue_item WHERE id = ?", g_it[p + k]->id);
    if(!rc) rc = sql_shift(p + c + 1, INT_MAX, -c);
    if(!rc && whole) rc = sql_del_unit(uid);
    rc = tx_end(rc);
    if(rc) { free(out); return rc; }

    undo_clear();
    mem_cut(p, c, out);
    g_undo.items = out;
    g_undo.n = c;
    g_undo.index = p;
    g_undo.unit_id = uid;
    if(whole) g_undo.gone = unit_take(uid);
    g_edit_ver++;
    g_undo.edit_ver = g_edit_ver;
    g_undo.valid = 1;
    emit(QEV_REMOVE, p, c, 0);
    return 0;
}

int queue_remove_item(int index){
    if(index <= g_cur || index >= g_n) return QERR_ARG;
    int64_t uid = g_it[index]->unit_id;
    WITH_RETRY(remove_impl(index, 1, uid, uid && range_of(uid, NULL) == 1));
}

/* The playing album keeps its playing (and played) songs; undo puts the later ones back in it. */
int queue_remove_unit(int64_t uid){
    int f, c = later_of(uid, &f);
    if(!c) return QERR_ARG;
    WITH_RETRY(remove_impl(f, c, uid, uid != cur_unit()));
}

int queue_can_undo(void){
    return g_undo.valid && g_undo.edit_ver == g_edit_ver && g_undo.index > g_cur && g_undo.index <= g_n;
}

static int undo_impl(void){
    int p = g_undo.index, n = g_undo.n;
    queue_unit *gone = g_undo.gone;
    int rc = mem_reserve(n, 1);
    if(!rc) rc = sql_ensure_list();
    if(!rc) rc = tx_begin();
    if(!rc && gone) rc = sql_new_unit(g_list, gone);
    int64_t uid = gone ? gone->id : g_undo.unit_id;
    for(int k = 0; k < n; k++) g_undo.items[k]->unit_id = uid;
    if(!rc) rc = sql_shift(p + 1, INT_MAX, n);
    if(!rc) rc = sql_insert(g_list, g_undo.items, n, p + 1);
    rc = tx_end(rc);
    if(rc) return rc;

    if(gone) unit_push(gone);
    mem_insert(p, g_undo.items, n);
    free(g_undo.items);
    memset(&g_undo, 0, sizeof g_undo);
    g_edit_ver++;
    emit(QEV_INSERT, p, n, 0);
    return 0;
}

int queue_undo(void){
    if(!queue_can_undo()) return QERR_UNDO;
    WITH_RETRY(undo_impl());
}

static int pop_impl(int64_t uid){
    int rc = tx_begin();
    if(!rc) rc = DBX("UPDATE queue_item SET unit_id = NULL WHERE unit_id = ?", uid);
    if(!rc) rc = sql_del_unit(uid);
    rc = tx_end(rc);
    if(rc) return rc;
    for(int i = 0; i < g_n; i++) if(g_it[i]->unit_id == uid) g_it[i]->unit_id = 0;
    unit_free(unit_take(uid));
    g_edit_ver++;
    emit(QEV_META, 0, 0, 0);
    return 0;
}

int queue_pop_unit(int64_t uid){
    if(unit_idx(uid) < 0) return QERR_ARG;
    WITH_RETRY(pop_impl(uid));
}

static int collapse_impl(int64_t uid, int collapsed){
    if(DBX("UPDATE queue_unit SET collapsed = ? WHERE id = ?", collapsed, uid)) return QERR_DB;
    g_un[unit_idx(uid)]->collapsed = collapsed;
    emit(QEV_META, 0, 0, 0);
    return 0;
}

int queue_set_collapsed(int64_t uid, int collapsed){
    if(unit_idx(uid) < 0) return QERR_ARG;
    collapsed = !!collapsed;
    if(g_un[unit_idx(uid)]->collapsed == collapsed) return 0;
    WITH_RETRY(collapse_impl(uid, collapsed));
}

/* The unit holding the new current item opens when it starts playing, as agreed (units show collapsed except the
 * playing one). A playing unit the user collapsed stays so while its next songs play. */
static int current_impl(int index, int type){
    int64_t uid = g_it[index]->unit_id;
    int u = uid ? unit_idx(uid) : -1;
    int expand = u >= 0 && g_un[u]->collapsed && !(g_cur >= 0 && g_cur < g_n && g_it[g_cur]->unit_id == uid);
    int pos = g_pos_ms, paused = g_paused;
    g_pos_ms = 0; g_paused = 0;
    int rc = tx_begin();
    if(!rc && expand) rc = DBX("UPDATE queue_unit SET collapsed = 0 WHERE id = ?", uid);
    if(!rc) rc = sql_state(g_list, g_it[index]->id);
    rc = tx_end(rc);
    if(rc) { g_pos_ms = pos; g_paused = paused; return rc; }
    if(expand) g_un[u]->collapsed = 0;
    g_cur = index;
    g_saved_pos_ms = 0; g_saved_paused = 0;
    emit(type, index, 1, 0);
    return 0;
}

int queue_jump(int index){
    if(index < 0 || index >= g_n) return QERR_ARG;
    WITH_RETRY(current_impl(index, QEV_JUMP));
}

int queue_set_current(int index){
    if(index < 0 || index >= g_n) return QERR_ARG;
    if(index == g_cur) return 0;
    WITH_RETRY(current_impl(index, QEV_CURRENT));
}

void queue_set_position(int ms, int paused){
    g_pos_ms = ms;
    g_paused = !!paused;
    if(g_paused == g_saved_paused && abs(ms - g_saved_pos_ms) < POS_SAVE_MS) return;
    if(sql_state(g_list, cur_id()) == 0) { g_saved_pos_ms = g_pos_ms; g_saved_paused = g_paused; }
}

int queue_position(int *paused){
    if(paused) *paused = g_paused;
    return g_pos_ms;
}

void queue_set_repeat(int mode){
    g_repeat = mode;
    sql_state(g_list, cur_id());
    emit(QEV_META, 0, 0, 0);
}

int queue_repeat(void) { return g_repeat; }

void queue_set_shuffle(int on){
    g_shuffle = !!on;
    sql_state(g_list, cur_id());
    emit(QEV_META, 0, 0, 0);
}

int queue_shuffle(void) { return g_shuffle; }
