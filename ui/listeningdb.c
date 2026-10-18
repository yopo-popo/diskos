/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* listeningdb - the 'history' part of listening.db: play counts and audiobook bookmarks (see listeningdb.h). */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "listeningdb.h"

/* this part's version in schema_part; a file written by a newer build is left alone */
#define HISTORY_VERSION 1

static const char *HISTORY_SCHEMA =
    "CREATE TABLE IF NOT EXISTS schema_part (part TEXT PRIMARY KEY, version INTEGER NOT NULL);"
    /* one row per song ever played: how often, and when last (last_play_seq grows with every play) */
    "CREATE TABLE IF NOT EXISTS listened_songs ("
    " id INTEGER PRIMARY KEY, path TEXT NOT NULL, start_ms INTEGER NOT NULL DEFAULT 0,"
    " play_count INTEGER NOT NULL DEFAULT 0, last_play_time INTEGER, last_play_seq INTEGER NOT NULL DEFAULT 0,"
    " UNIQUE (path, start_ms));"
    "CREATE INDEX IF NOT EXISTS listened_by_count ON listened_songs (play_count DESC, last_play_seq DESC);"
    "CREATE INDEX IF NOT EXISTS listened_by_last ON listened_songs (last_play_seq DESC);"
    /* one bookmark per book: member_path is the file that was playing (a multipart book has several) */
    "CREATE TABLE IF NOT EXISTS book_progress ("
    " book_key TEXT PRIMARY KEY, member_path TEXT NOT NULL, position_ms INTEGER NOT NULL DEFAULT 0,"
    " updated_at INTEGER NOT NULL DEFAULT 0, completed INTEGER NOT NULL DEFAULT 0, seq INTEGER NOT NULL DEFAULT 0);"
    "INSERT OR IGNORE INTO schema_part (part, version) VALUES ('history', 1);";

static sqlite3 *g_db;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static long long query_int(sqlite3 *db, const char *sql){
    sqlite3_stmt *s = NULL; long long v = -1;
    if(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}
/* the open connection, opening it (and creating the file and this part's tables) the first time; NULL = unavailable */
static sqlite3 *db_locked(void){
    if(g_db) return g_db;
    if(ldb_make_dir() != 0) return NULL;
    sqlite3 *d = NULL;
    if(ldb_open(LISTENINGDB_PATH, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, &d) != SQLITE_OK) return NULL;
    /* WAL: the queue's connection and readers never wait on a play being counted. NORMAL syncs at checkpoints only,
     * which can lose the last play or bookmark to a power cut but never damages the file. */
    sqlite3_stmt *s = NULL; int wal = 0;
    if(sqlite3_prepare_v2(d, "PRAGMA journal_mode=WAL;", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        wal = !strcmp((const char *)sqlite3_column_text(s, 0), "wal");
    sqlite3_finalize(s);
    int ok = wal
          && sqlite3_exec(d, "PRAGMA synchronous=NORMAL; PRAGMA journal_size_limit=1048576;", 0, 0, 0) == SQLITE_OK
          && sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK;
    if(ok){
        ok = sqlite3_exec(d, HISTORY_SCHEMA, 0, 0, 0) == SQLITE_OK
          && query_int(d, "SELECT version FROM schema_part WHERE part='history';") == HISTORY_VERSION;
        if(!ok || sqlite3_exec(d, "COMMIT;", 0, 0, 0) != SQLITE_OK){ sqlite3_exec(d, "ROLLBACK;", 0, 0, 0); ok = 0; }
    }
    if(!ok){
        fprintf(stderr, "listeningdb: can't use %s: %s\n", LISTENINGDB_PATH, sqlite3_errmsg(d));
        sqlite3_close(d);
        return NULL;
    }
    g_db = d;
    return g_db;
}

int lsdb_ready(void){
    pthread_mutex_lock(&g_mu);
    int ok = db_locked() != NULL;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void lsdb_close(void){
    pthread_mutex_lock(&g_mu);
    if(g_db){ sqlite3_close(g_db); g_db = NULL; }
    pthread_mutex_unlock(&g_mu);
}

int lsdb_record_play(const char *path, long long start_ms){
    if(!path || !path[0]) return 0;
    pthread_mutex_lock(&g_mu);
    sqlite3 *d = db_locked();
    sqlite3_stmt *st = NULL; int ok = 0;
    if(d && sqlite3_prepare_v2(d,
        "INSERT INTO listened_songs (path, start_ms, play_count, last_play_time, last_play_seq)"
        " VALUES (?1, ?2, 1, ?3, (SELECT IFNULL(max(last_play_seq), 0) + 1 FROM listened_songs))"
        " ON CONFLICT (path, start_ms) DO UPDATE SET play_count = play_count + 1,"
        " last_play_time = excluded.last_play_time, last_play_seq = excluded.last_play_seq;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, start_ms > 0 ? start_ms : 0);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int lsdb_book_progress(const char *key, char *member_out, int member_cap, long *position_ms, int *completed_out,
                       long *updated_out, long long *seq_out){
    if(completed_out) *completed_out = 0;
    if(updated_out) *updated_out = 0;
    if(seq_out) *seq_out = 0;
    if(!key) return -1;
    pthread_mutex_lock(&g_mu);
    sqlite3 *d = db_locked();
    sqlite3_stmt *st = NULL; int found = -1;
    if(d && sqlite3_prepare_v2(d, "SELECT member_path, position_ms, completed, updated_at, seq FROM book_progress"
                                  " WHERE book_key=?1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        int rc = sqlite3_step(st);
        if(rc == SQLITE_ROW){
            const unsigned char *m = sqlite3_column_text(st, 0);
            if(member_out && member_cap > 0) snprintf(member_out, (size_t)member_cap, "%s", m ? (const char *)m : "");
            if(position_ms) *position_ms = (long)sqlite3_column_int64(st, 1);
            if(completed_out) *completed_out = sqlite3_column_int(st, 2);
            if(updated_out) *updated_out = (long)sqlite3_column_int64(st, 3);
            if(seq_out) *seq_out = sqlite3_column_int64(st, 4);
            found = 1;
        } else if(rc == SQLITE_DONE) found = 0;              /* BUSY / IOERR stay -1: a read error, not "no bookmark" */
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mu);
    return found;
}

int lsdb_book_save(const char *key, const char *member_path, long position_ms, int completed){
    if(!key || !member_path) return 0;
    pthread_mutex_lock(&g_mu);
    sqlite3 *d = db_locked();
    sqlite3_stmt *st = NULL; int ok = 0;
    if(d && sqlite3_prepare_v2(d,
        "INSERT INTO book_progress (book_key, member_path, position_ms, updated_at, completed, seq)"
        " VALUES (?1, ?2, ?3, ?4, ?5, (SELECT IFNULL(max(seq), 0) + 1 FROM book_progress))"
        " ON CONFLICT (book_key) DO UPDATE SET member_path = excluded.member_path, position_ms = excluded.position_ms,"
        " updated_at = excluded.updated_at, completed = excluded.completed, seq = excluded.seq;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, member_path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, position_ms < 0 ? 0 : position_ms);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));
        sqlite3_bind_int(st, 5, completed ? 1 : 0);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mu);
    return ok;
}
