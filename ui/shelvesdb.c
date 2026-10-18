/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* shelvesdb - favourites in shelves.db, and its copy on the SD card (see shelvesdb.h). */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "shelvesdb.h"
#include "sdio.h"
#include "artcache.h"   /* sd_write_begin / sd_write_end */

static const char *SCHEMA =
    /* backup_seen = 1 once this file has taken in the SD card's copy (or the card had none) */
    "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value);"
    "INSERT OR IGNORE INTO meta (key, value) VALUES ('backup_seen', 0);"
    /* a song you attached your own data to; the snapshot shows what a song whose file has gone was */
    "CREATE TABLE IF NOT EXISTS track_refs ("
    " id INTEGER PRIMARY KEY, path TEXT NOT NULL, start_ms INTEGER NOT NULL DEFAULT 0,"
    " title TEXT, album_artist TEXT, album_title TEXT,"
    " UNIQUE (path, start_ms));"
    /* favourites: the newest has the highest id */
    "CREATE TABLE IF NOT EXISTS favourites ("
    " id INTEGER PRIMARY KEY,"
    " track_ref_id INTEGER NOT NULL UNIQUE REFERENCES track_refs (id),"
    " added_at INTEGER NOT NULL);"
    "PRAGMA user_version = 1;";

/* The card's copy (attached as b) into this file: its songs, then its favourites in its order, older than the ones made
 * here before the card came (those stay newest; one in both moves up to its newer place). */
static const char *MERGE_SQL =
    "INSERT OR IGNORE INTO track_refs (path, start_ms, title, album_artist, album_title)"
    " SELECT path, start_ms, title, album_artist, album_title FROM b.track_refs ORDER BY id;"
    "CREATE TEMP TABLE merge_local AS SELECT track_ref_id, added_at FROM favourites ORDER BY id;"
    "DELETE FROM favourites;"
    "INSERT INTO favourites (track_ref_id, added_at)"
    " SELECT r.id, f.added_at FROM b.favourites f JOIN b.track_refs br ON br.id = f.track_ref_id"
    " JOIN main.track_refs r ON r.path = br.path AND r.start_ms = br.start_ms ORDER BY f.id;"
    "INSERT OR REPLACE INTO favourites (track_ref_id, added_at) SELECT track_ref_id, added_at FROM merge_local ORDER BY rowid;"
    "DROP TABLE merge_local;";

static sqlite3 *g_db;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_gen;

static long long query_int(sqlite3 *db, const char *sql){
    sqlite3_stmt *s = NULL; long long v = -1;
    if(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

/* src -> dst through a temporary file beside dst, synced before the rename publishes it. 0 = done. */
static int copy_file(const char *src, const char *dst){
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *in = fopen(src, "rb");
    if(!in) return -1;
    FILE *out = fopen(tmp, "wb");
    if(!out){ fclose(in); return -1; }
    char buf[16384]; size_t n; int ok = 1;
    while((n = fread(buf, 1, sizeof buf, in)) > 0) if(fwrite(buf, 1, n, out) != n){ ok = 0; break; }
    if(ferror(in)) ok = 0;
    fclose(in);
    if(fflush(out) != 0) ok = 0;
    if(ok && fsync(fileno(out)) != 0 && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) ok = 0;
    if(fclose(out) != 0) ok = 0;
    if(ok && rename(tmp, dst) == 0) return 0;
    unlink(tmp);
    return -1;
}

/* ---- the SD card's copy ---- */
/* A shelves.db that hasn't seen the card's copy (a new file: first start, a factory reset of /usr/data, or the card
 * mounting after the UI started) takes it in once the card can be read, and until then no copy is written over it.
 * For a new file that is a restore; favourites added before the card came are kept as well. */
#define MERGE_COPY "/tmp/diskos-shelves.merge"
#define SNAPSHOT   "/tmp/diskos-shelves.snapshot"
#ifndef SHELVES_CARD_RETRY_S
#define SHELVES_CARD_RETRY_S 10
#endif
static int g_seen;              /* meta backup_seen, under g_mu */
static struct timespec g_tried; /* the last look at the card: one at most every SHELVES_CARD_RETRY_S while it can't be read */
static void backup_soon(void);

/* attach_rc = the ATTACH's result. 1 = taken in (or nothing to take), 0 = try again later. */
static int merge_attached(sqlite3 *d, int attach_rc){
    long long v = attach_rc == SQLITE_DONE ? query_int(d, "PRAGMA b.user_version;")
                : (attach_rc == SQLITE_NOTADB || attach_rc == SQLITE_CORRUPT) ? -1 : -2;
    if(v == -2) return 0;
    if(v > SHELVESDB_SCHEMA){                                /* from a newer build: never written over */
        fprintf(stderr, "shelvesdb: %s is from a newer version (%lld); not copying to the card\n", SHELVES_BACKUP, v);
        return 0;
    }
    if(v != SHELVESDB_SCHEMA){                               /* unreadable or empty: nothing to take in */
        fprintf(stderr, "shelvesdb: %s can't be read; the next copy replaces it\n", SHELVES_BACKUP);
        return sqlite3_exec(d, "UPDATE meta SET value = 1 WHERE key = 'backup_seen';", 0, 0, 0) == SQLITE_OK;
    }
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    if(sqlite3_exec(d, MERGE_SQL, 0, 0, 0) == SQLITE_OK
       && sqlite3_exec(d, "UPDATE meta SET value = 1 WHERE key = 'backup_seen';", 0, 0, 0) == SQLITE_OK
       && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK){
        fprintf(stderr, "shelvesdb: took in %s\n", SHELVES_BACKUP);
        atomic_fetch_add(&g_gen, 1);
        return 1;
    }
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* Caller holds g_mu. */
static void merge_backup_locked(sqlite3 *d){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if(g_seen || (g_tried.tv_sec && now.tv_sec - g_tried.tv_sec < SHELVES_CARD_RETRY_S)) return;
    g_tried = now;
    if(!sd_io_begin()) return;                               /* the card isn't readable yet */
    struct stat st;
    int rc = lstat(SHELVES_BACKUP, &st);
    int none = rc != 0 && errno == ENOENT;
    unlink(MERGE_COPY);
    int copied = rc == 0 && S_ISREG(st.st_mode) && copy_file(SHELVES_BACKUP, MERGE_COPY) == 0;
    sd_io_end();
    int seen = 0;
    if(none) seen = sqlite3_exec(d, "UPDATE meta SET value = 1 WHERE key = 'backup_seen';", 0, 0, 0) == SQLITE_OK;
    else if(copied){
        sqlite3_stmt *s = NULL;
        if(sqlite3_prepare_v2(d, "ATTACH DATABASE ?1 AS b;", -1, &s, NULL) == SQLITE_OK){
            sqlite3_bind_text(s, 1, MERGE_COPY, -1, SQLITE_STATIC);
            int arc = sqlite3_step(s);
            sqlite3_finalize(s);
            seen = merge_attached(d, arc);
            if(arc == SQLITE_DONE) sqlite3_exec(d, "DETACH DATABASE b;", 0, 0, 0);
        } else sqlite3_finalize(s);
    }
    unlink(MERGE_COPY);
    if(seen){ g_seen = 1; backup_soon(); }
}

/* A copy is made by a worker thread, so a heart tap never waits for the card: VACUUM INTO takes a consistent snapshot
 * into RAM under the lock, then the snapshot goes to the card without it. Changes made while it works are picked up
 * by another round. */
static int g_backup_dirty, g_backup_running;
static pthread_mutex_t g_backup_mu = PTHREAD_MUTEX_INITIALIZER;

static void backup_once(void){
    unlink(SNAPSHOT);
    pthread_mutex_lock(&g_mu);
    if(g_db) merge_backup_locked(g_db);
    int ok = g_db && g_seen && sqlite3_exec(g_db, "VACUUM INTO '" SNAPSHOT "';", 0, 0, 0) == SQLITE_OK;
    pthread_mutex_unlock(&g_mu);
    if(ok && sd_io_begin()){
        if(sd_write_begin()){                  /* not while the card is lent to a computer */
            mkdir("/tmp/sdcard/.diskos", 0755);
            mkdir(SHELVES_BACKUP_DIR, 0755);
            if(copy_file(SNAPSHOT, SHELVES_BACKUP) != 0) fprintf(stderr, "shelvesdb: can't copy to %s\n", SHELVES_BACKUP);
            sd_write_end();
        }
        sd_io_end();
    }
    unlink(SNAPSHOT);
}
static void *backup_thread(void *arg){
    (void)arg;
    pthread_mutex_lock(&g_backup_mu);
    while(g_backup_dirty){
        g_backup_dirty = 0;
        pthread_mutex_unlock(&g_backup_mu);
        backup_once();
        pthread_mutex_lock(&g_backup_mu);
    }
    g_backup_running = 0;
    pthread_mutex_unlock(&g_backup_mu);
    return NULL;
}
static void backup_soon(void){
    pthread_mutex_lock(&g_backup_mu);
    g_backup_dirty = 1;
    if(!g_backup_running){
        pthread_t t; pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if(pthread_create(&t, &a, backup_thread, NULL) == 0) g_backup_running = 1;
        pthread_attr_destroy(&a);
    }
    pthread_mutex_unlock(&g_backup_mu);
}

/* ---- the connection ---- */
static sqlite3 *db_locked(void){
    if(g_db){ merge_backup_locked(g_db); return g_db; }
    if(ldb_make_dir() != 0) return NULL;
    sqlite3 *d = NULL;
    if(ldb_open(SHELVESDB_PATH, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, &d) != SQLITE_OK) return NULL;
    /* a rollback journal (not WAL): written rarely, and the file itself is always the whole state */
    long long v = query_int(d, "PRAGMA user_version;");
    int ok = v == SHELVESDB_SCHEMA;
    if(v == 0 && sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK){
        ok = sqlite3_exec(d, SCHEMA, 0, 0, 0) == SQLITE_OK && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK;
        if(!ok) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    }
    if(!ok){
        fprintf(stderr, "shelvesdb: can't use %s (version %lld): %s\n", SHELVESDB_PATH, v, sqlite3_errmsg(d));
        sqlite3_close(d);
        return NULL;
    }
    g_db = d;
    g_seen = query_int(d, "SELECT value FROM meta WHERE key = 'backup_seen';") == 1;
    merge_backup_locked(d);
    return g_db;
}
int shdb_ready(void){
    pthread_mutex_lock(&g_mu);
    int ok = db_locked() != NULL;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void shdb_close(void){
    pthread_mutex_lock(&g_mu);
    if(g_db){ sqlite3_close(g_db); g_db = NULL; }
    pthread_mutex_unlock(&g_mu);
}
unsigned shdb_generation(void){ return atomic_load(&g_gen); }

/* ---- favourites ---- */
int shdb_fav_get(const char *path, long long start_ms){
    if(!path || !path[0]) return 0;
    pthread_mutex_lock(&g_mu);
    sqlite3 *d = db_locked();
    sqlite3_stmt *st = NULL; int r = -1;
    if(d && sqlite3_prepare_v2(d, "SELECT 1 FROM favourites f JOIN track_refs r ON r.id=f.track_ref_id"
                                  " WHERE r.path=?1 AND r.start_ms=?2;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, start_ms > 0 ? start_ms : 0);
        int rc = sqlite3_step(st);
        r = rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* Prepares sql, binds the song as ?1/?2 and the text values as ?3.. (NULL or "" = NULL), steps it once. 1 = done. */
static int run(sqlite3 *d, const char *sql, const char *path, long long start_ms, int ntext, const char *const *text){
    sqlite3_stmt *st = NULL;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, start_ms > 0 ? start_ms : 0);
    for(int i = 0; i < ntext; i++){
        if(text[i] && text[i][0]) sqlite3_bind_text(st, 3 + i, text[i], -1, SQLITE_STATIC);
        else sqlite3_bind_null(st, 3 + i);
    }
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
int shdb_fav_set(const char *path, long long start_ms, int on, const char *title, const char *album_artist,
                 const char *album){
    if(!path || !path[0]) return 0;
    const char *snap[3] = { title, album_artist, album };
    pthread_mutex_lock(&g_mu);
    sqlite3 *d = db_locked();
    int ok = d && sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK;
    if(ok && on){
        /* the song's row (a known snapshot value replaces the old one), then the favourite; already one = no change */
        ok = run(d, "INSERT INTO track_refs (path, start_ms, title, album_artist, album_title) VALUES (?1, ?2, ?3, ?4, ?5)"
                    " ON CONFLICT (path, start_ms) DO UPDATE SET title = COALESCE(excluded.title, title),"
                    " album_artist = COALESCE(excluded.album_artist, album_artist),"
                    " album_title = COALESCE(excluded.album_title, album_title);", path, start_ms, 3, snap)
          && run(d, "INSERT OR IGNORE INTO favourites (track_ref_id, added_at)"
                    " SELECT id, CAST(strftime('%s', 'now') AS INTEGER) FROM track_refs WHERE path=?1 AND start_ms=?2;",
                 path, start_ms, 0, NULL);
    } else if(ok){
        /* the favourite, then the song's row when nothing else uses it */
        ok = run(d, "DELETE FROM favourites WHERE track_ref_id IN (SELECT id FROM track_refs WHERE path=?1 AND start_ms=?2);",
                 path, start_ms, 0, NULL)
          && run(d, "DELETE FROM track_refs WHERE path=?1 AND start_ms=?2"
                    " AND id NOT IN (SELECT track_ref_id FROM favourites);", path, start_ms, 0, NULL);
    }
    if(ok) ok = sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK;
    if(!ok && d) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    if(ok) atomic_fetch_add(&g_gen, 1);
    pthread_mutex_unlock(&g_mu);
    if(ok) backup_soon();
    return ok;
}
