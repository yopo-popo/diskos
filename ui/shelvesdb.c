/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* shelvesdb - favourites, playlists and album collections in shelves.db, and its copy on the SD card (see shelvesdb.h). */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
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
    /* version 2: playlists and their folders (parent / folder NULL = the top). Names compare without case: exported
     * files named "Rock" and "rock" would be one file on the card. */
    "CREATE TABLE IF NOT EXISTS playlist_folders ("
    " id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
    " parent_id INTEGER REFERENCES playlist_folders (id),"
    " position INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS playlists ("
    " id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    " folder_id INTEGER REFERENCES playlist_folders (id),"
    " position INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS playlist_items ("
    " id INTEGER PRIMARY KEY,"
    " playlist_id INTEGER NOT NULL REFERENCES playlists (id) ON DELETE CASCADE,"
    " track_ref_id INTEGER NOT NULL REFERENCES track_refs (id),"
    " position INTEGER NOT NULL, added_at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS playlist_items_in_order ON playlist_items (playlist_id, position);"
    "CREATE INDEX IF NOT EXISTS playlist_items_by_song ON playlist_items (track_ref_id);"
    "CREATE INDEX IF NOT EXISTS playlists_in_folder ON playlists (folder_id);"
    "CREATE INDEX IF NOT EXISTS playlist_folders_in_folder ON playlist_folders (parent_id);"
    "PRAGMA user_version = 2;";
/* Album collections came after version 2 without a new version, so a build from before them still opens the file and
 * leaves these tables alone. Run on every open; each statement is whole on its own. */
static const char *COLLECTIONS =
    "CREATE TABLE IF NOT EXISTS collections ("
    " id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    " position INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
    /* an album as library.db keys it: no album artist is '' */
    "CREATE TABLE IF NOT EXISTS collection_albums ("
    " id INTEGER PRIMARY KEY,"
    " collection_id INTEGER NOT NULL REFERENCES collections (id) ON DELETE CASCADE,"
    " album_artist TEXT NOT NULL DEFAULT '', album_title TEXT NOT NULL, added_at INTEGER NOT NULL,"
    " UNIQUE (collection_id, album_artist, album_title));";

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
/* A version 2 copy's folders and playlists too, after MERGE_SQL brought its songs. They keep their ids shifted past
 * this file's, and go before the ones made here. A playlist whose name is taken here comes in as "Name (2)". */
static const char *MERGE_PLAYLISTS_SQL =
    "CREATE TEMP TABLE merge_off AS SELECT"
    " (SELECT IFNULL(MAX(id), 0) FROM main.playlist_folders) AS f, (SELECT IFNULL(MAX(id), 0) FROM main.playlists) AS p,"
    " (SELECT IFNULL(MAX(position), -1) + 1 FROM (SELECT position FROM b.playlist_folders WHERE parent_id IS NULL"
    "  UNION ALL SELECT position FROM b.playlists WHERE folder_id IS NULL)) AS top;"
    "UPDATE main.playlist_folders SET position = position + (SELECT top FROM merge_off) WHERE parent_id IS NULL;"
    "UPDATE main.playlists SET position = position + (SELECT top FROM merge_off) WHERE folder_id IS NULL;"
    "INSERT INTO playlist_folders (id, name, parent_id, position, created_at)"
    " SELECT bf.id + o.f, bf.name, bf.parent_id + o.f, bf.position, bf.created_at FROM b.playlist_folders bf CROSS JOIN merge_off o;"
    "INSERT OR IGNORE INTO playlists (id, name, folder_id, position, created_at, updated_at)"
    " SELECT bp.id + o.p, CASE WHEN EXISTS (SELECT 1 FROM main.playlists m WHERE m.name = bp.name) THEN bp.name || ' (2)'"
    " ELSE bp.name END, bp.folder_id + o.f, bp.position, bp.created_at, bp.updated_at FROM b.playlists bp CROSS JOIN merge_off o;"
    "INSERT INTO playlist_items (playlist_id, track_ref_id, position, added_at)"
    " SELECT bi.playlist_id + o.p, r.id, bi.position, bi.added_at FROM b.playlist_items bi CROSS JOIN merge_off o"
    " JOIN b.track_refs br ON br.id = bi.track_ref_id JOIN main.track_refs r ON r.path = br.path AND r.start_ms = br.start_ms"
    " WHERE bi.playlist_id + o.p IN (SELECT id FROM main.playlists) ORDER BY bi.id;"
    "DROP TABLE merge_off;";
/* The copy's collections, when it has them: they go before the ones made here, and one with the name of a collection
 * here is combined with it (its albums are added to that one). */
static const char *MERGE_COLLECTIONS_SQL =
    "CREATE TEMP TABLE merge_coff AS SELECT IFNULL(MAX(position), -1) + 1 AS top FROM b.collections;"
    "UPDATE main.collections SET position = position + (SELECT top FROM merge_coff);"
    "INSERT OR IGNORE INTO main.collections (name, position, created_at, updated_at)"
    " SELECT name, position, created_at, updated_at FROM b.collections ORDER BY position, id;"
    "INSERT OR IGNORE INTO main.collection_albums (collection_id, album_artist, album_title, added_at)"
    " SELECT m.id, IFNULL(ba.album_artist, ''), ba.album_title, ba.added_at FROM b.collection_albums ba"
    " JOIN b.collections bc ON bc.id = ba.collection_id JOIN main.collections m ON m.name = bc.name ORDER BY ba.id;"
    "DROP TABLE merge_coff;";
/* songs nothing uses any more */
static const char *PRUNE_REFS =
    "DELETE FROM track_refs WHERE id NOT IN (SELECT track_ref_id FROM favourites)"
    " AND id NOT IN (SELECT track_ref_id FROM playlist_items);";

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
    if(v < 1){                                               /* unreadable or empty: nothing to take in */
        fprintf(stderr, "shelvesdb: %s can't be read; the next copy replaces it\n", SHELVES_BACKUP);
        return sqlite3_exec(d, "UPDATE meta SET value = 1 WHERE key = 'backup_seen';", 0, 0, 0) == SQLITE_OK;
    }
    /* a copy from a build before collections has no collections tables */
    int colls = query_int(d, "SELECT count(*) FROM b.sqlite_master WHERE type = 'table'"
                             " AND name IN ('collections', 'collection_albums');") == 2;
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    if(sqlite3_exec(d, MERGE_SQL, 0, 0, 0) == SQLITE_OK
       && (v < 2 || sqlite3_exec(d, MERGE_PLAYLISTS_SQL, 0, 0, 0) == SQLITE_OK)
       && (!colls || sqlite3_exec(d, MERGE_COLLECTIONS_SQL, 0, 0, 0) == SQLITE_OK)
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
    /* new, or from an older build: the schema only adds, so running it again brings an old file up to date */
    if(v >= 0 && v < SHELVESDB_SCHEMA && sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK){
        ok = sqlite3_exec(d, SCHEMA, 0, 0, 0) == SQLITE_OK && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK;
        if(!ok) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    }
    if(!ok){
        fprintf(stderr, "shelvesdb: can't use %s (version %lld): %s\n", SHELVESDB_PATH, v, sqlite3_errmsg(d));
        sqlite3_close(d);
        return NULL;
    }
    if(sqlite3_exec(d, COLLECTIONS, 0, 0, 0) != SQLITE_OK)   /* then only collections fail; the rest still works */
        fprintf(stderr, "shelvesdb: can't add the collections tables: %s\n", sqlite3_errmsg(d));
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

/* ---- changes ---- */
/* Every change: begin_change takes g_mu and a write transaction (NULL = can't); end_change commits (ok) or rolls back,
 * releases g_mu and, when something changed, copies the file to the card. */
static sqlite3 *begin_change(void){
    pthread_mutex_lock(&g_mu);
    sqlite3 *d = db_locked();
    return d && sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK ? d : NULL;
}
static int end_change(sqlite3 *d, int ok){
    if(d && ok) ok = sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK;
    if(d && !ok) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    ok = d && ok;
    if(ok) atomic_fetch_add(&g_gen, 1);
    pthread_mutex_unlock(&g_mu);
    if(ok) backup_soon();
    return ok;
}

/* Prepares sql with its parameters ?1.. from fmt: 'i' a long long, 'n' a long long with 0 bound as NULL (the top
 * folder), 't' a string (NULL or "" bound as NULL), 's' a string as it is (NULL bound as ""). NULL = can't. */
static sqlite3_stmt *prep(sqlite3 *d, const char *sql, const char *fmt, va_list ap){
    sqlite3_stmt *st = NULL;
    if(!d || sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){ sqlite3_finalize(st); return NULL; }
    for(int i = 0; fmt[i]; i++){
        if(fmt[i] == 't'){
            const char *t = va_arg(ap, const char *);
            if(t && t[0]) sqlite3_bind_text(st, i + 1, t, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, i + 1);
        } else if(fmt[i] == 's'){
            const char *t = va_arg(ap, const char *);
            sqlite3_bind_text(st, i + 1, t ? t : "", -1, SQLITE_TRANSIENT);
        } else {
            long long v = va_arg(ap, long long);
            if(fmt[i] == 'n' && v <= 0) sqlite3_bind_null(st, i + 1); else sqlite3_bind_int64(st, i + 1, v);
        }
    }
    return st;
}
/* Runs a statement that returns no rows. 1 = done. */
static int execf(sqlite3 *d, const char *sql, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    sqlite3_stmt *st = prep(d, sql, fmt, ap);
    va_end(ap);
    int ok = st && sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
/* The first column of a query's first row; dflt when it has no row, -1 when it fails. */
static long long intf(sqlite3 *d, long long dflt, const char *sql, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    sqlite3_stmt *st = prep(d, sql, fmt, ap);
    va_end(ap);
    long long v = -1;
    if(st){
        int rc = sqlite3_step(st);
        v = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : rc == SQLITE_DONE ? dflt : -1;
    }
    sqlite3_finalize(st);
    return v;
}
/* A prepared statement for the caller to run (and finalize); NULL = can't. */
static sqlite3_stmt *stmtf(sqlite3 *d, const char *sql, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    sqlite3_stmt *st = prep(d, sql, fmt, ap);
    va_end(ap);
    return st;
}
#define NOW "CAST(strftime('%s', 'now') AS INTEGER)"
/* the song's row; a known snapshot value replaces the old one. ?1 path, ?2 start, ?3.. title, album artist, album */
#define UPSERT_REF \
    "INSERT INTO track_refs (path, start_ms, title, album_artist, album_title) VALUES (?1, ?2, ?3, ?4, ?5)" \
    " ON CONFLICT (path, start_ms) DO UPDATE SET title = COALESCE(excluded.title, title)," \
    " album_artist = COALESCE(excluded.album_artist, album_artist)," \
    " album_title = COALESCE(excluded.album_title, album_title);"

/* ---- favourites ---- */
int shdb_fav_get(const char *path, long long start_ms){
    if(!path || !path[0]) return 0;
    pthread_mutex_lock(&g_mu);
    long long n = intf(db_locked(), 0, "SELECT 1 FROM favourites f JOIN track_refs r ON r.id=f.track_ref_id"
                                       " WHERE r.path=?1 AND r.start_ms=?2;", "ti", path, start_ms > 0 ? start_ms : 0);
    pthread_mutex_unlock(&g_mu);
    return (int)n;
}
int shdb_fav_set(const char *path, long long start_ms, int on, const char *title, const char *album_artist,
                 const char *album){
    if(!path || !path[0]) return 0;
    if(start_ms < 0) start_ms = 0;
    sqlite3 *d = begin_change();
    int ok = d != NULL;
    if(ok && on)   /* the song's row, then the favourite; already one = no change */
        ok = execf(d, UPSERT_REF, "tittt", path, start_ms, title, album_artist, album)
          && execf(d, "INSERT OR IGNORE INTO favourites (track_ref_id, added_at)"
                      " SELECT id, " NOW " FROM track_refs WHERE path=?1 AND start_ms=?2;", "ti", path, start_ms);
    else if(ok)    /* the favourite, then the song's row when nothing else uses it */
        ok = execf(d, "DELETE FROM favourites WHERE track_ref_id IN (SELECT id FROM track_refs WHERE path=?1 AND start_ms=?2);",
                   "ti", path, start_ms)
          && execf(d, "DELETE FROM track_refs WHERE path=?1 AND start_ms=?2 AND id NOT IN (SELECT track_ref_id FROM favourites)"
                      " AND id NOT IN (SELECT track_ref_id FROM playlist_items);", "ti", path, start_ms);
    return end_change(d, ok);
}

/* ---- playlists and their folders ---- */
/* the next position at the end of folder ?1 (NULL = the top) */
#define END_OF_FOLDER \
    "(SELECT IFNULL(MAX(p), -1) + 1 FROM (SELECT position AS p FROM playlists WHERE folder_id IS ?1" \
    " UNION ALL SELECT position FROM playlist_folders WHERE parent_id IS ?1))"

int shdb_pl_list(long long folder_id, shdb_entry **out){
    *out = NULL;
    pthread_mutex_lock(&g_mu);
    sqlite3_stmt *st = NULL;
    int n = -1;
    sqlite3 *d = db_locked();
    if(d && sqlite3_prepare_v2(d,
        "SELECT 1, f.id, f.name, (SELECT count(*) FROM playlists WHERE folder_id = f.id)"
        " + (SELECT count(*) FROM playlist_folders c WHERE c.parent_id = f.id), f.position"
        " FROM playlist_folders f WHERE f.parent_id IS ?1"
        " UNION ALL SELECT 0, p.id, p.name, (SELECT count(*) FROM playlist_items WHERE playlist_id = p.id), p.position"
        " FROM playlists p WHERE p.folder_id IS ?1 ORDER BY 5, 1 DESC, 2;", -1, &st, NULL) == SQLITE_OK){
        if(folder_id > 0) sqlite3_bind_int64(st, 1, folder_id); else sqlite3_bind_null(st, 1);
        shdb_entry *v = NULL; int cap = 0, rc;
        n = 0;
        while((rc = sqlite3_step(st)) == SQLITE_ROW){
            if(n == cap){
                int nc = cap ? cap * 2 : 32;
                shdb_entry *p = realloc(v, (size_t)nc * sizeof *v);
                if(!p){ rc = SQLITE_NOMEM; break; }
                v = p; cap = nc;
            }
            shdb_entry *e = &v[n++];
            e->is_folder = sqlite3_column_int(st, 0);
            e->id = sqlite3_column_int64(st, 1);
            const unsigned char *name = sqlite3_column_text(st, 2);
            snprintf(e->name, sizeof e->name, "%s", name ? (const char *)name : "");
            e->count = sqlite3_column_int(st, 3);
        }
        if(rc != SQLITE_DONE){ free(v); v = NULL; n = -1; }
        *out = v;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mu);
    return n;
}

long long shdb_pl_create(const char *name, long long folder_id){
    if(!name || !name[0]) return -1;
    sqlite3 *d = begin_change();
    long long taken = intf(d, 0, "SELECT 1 FROM playlists WHERE name = ?1;", "t", name), id = -1;
    int ok = taken == 0
          && execf(d, "INSERT INTO playlists (name, folder_id, position, created_at, updated_at)"
                      " VALUES (?2, ?1, " END_OF_FOLDER ", " NOW ", " NOW ");", "nt", folder_id, name);
    if(ok) id = sqlite3_last_insert_rowid(d);
    if(!end_change(d, ok)) return taken == 1 ? 0 : -1;
    return id;
}
long long shdb_pl_find(const char *name){
    if(!name || !name[0]) return 0;
    pthread_mutex_lock(&g_mu);
    long long id = intf(db_locked(), 0, "SELECT id FROM playlists WHERE name = ?1;", "t", name);
    pthread_mutex_unlock(&g_mu);
    return id;
}
/* The name a query (?1 = an id) finds, into out. 1 = found, 0 = none, -1 = can't read. */
static int name_of(const char *sql, long long id, char *out, int cap){
    if(cap > 0) out[0] = 0;
    pthread_mutex_lock(&g_mu);
    sqlite3_stmt *st = stmtf(db_locked(), sql, "i", id);
    int r = -1;
    if(st){
        int rc = sqlite3_step(st);
        if(rc == SQLITE_ROW){ snprintf(out, (size_t)cap, "%s", (const char *)sqlite3_column_text(st, 0)); r = 1; }
        else if(rc == SQLITE_DONE) r = 0;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mu);
    return r;
}
int shdb_name(int is_folder, long long id, char *out, int cap){
    return name_of(is_folder ? "SELECT name FROM playlist_folders WHERE id = ?1;" : "SELECT name FROM playlists WHERE id = ?1;",
                   id, out, cap);
}
int shdb_pl_rename(long long id, const char *name){
    if(!name || !name[0]) return -1;
    sqlite3 *d = begin_change();
    long long taken = intf(d, 0, "SELECT 1 FROM playlists WHERE name = ?1 AND id <> ?2;", "ti", name, id);
    int ok = taken == 0 && execf(d, "UPDATE playlists SET name = ?1, updated_at = " NOW " WHERE id = ?2;", "ti", name, id)
          && sqlite3_changes(d) == 1;
    if(!end_change(d, ok)) return taken == 1 ? 0 : -1;
    return 1;
}
int shdb_pl_delete(long long id){
    sqlite3 *d = begin_change();
    int ok = execf(d, "DELETE FROM playlists WHERE id = ?1;", "i", id) && sqlite3_changes(d) == 1   /* its songs go too */
          && execf(d, PRUNE_REFS, "");
    return end_change(d, ok);
}
int shdb_pl_add(long long id, const shdb_song *songs, int n){
    if(n <= 0) return 0;
    sqlite3 *d = begin_change();
    long long pos = intf(d, -1, "SELECT IFNULL(MAX(position), -1) + 1 FROM playlist_items WHERE playlist_id = ?1;", "i", id);
    int ok = pos >= 0 && execf(d, "UPDATE playlists SET updated_at = " NOW " WHERE id = ?1;", "i", id) && sqlite3_changes(d) == 1;
    sqlite3_stmt *ref = NULL, *item = NULL;
    ok = ok && sqlite3_prepare_v2(d, UPSERT_REF, -1, &ref, NULL) == SQLITE_OK
            && sqlite3_prepare_v2(d, "INSERT INTO playlist_items (playlist_id, track_ref_id, position, added_at)"
                                     " SELECT ?3, id, ?4, " NOW " FROM track_refs WHERE path = ?1 AND start_ms = ?2;",
                                  -1, &item, NULL) == SQLITE_OK;
    for(int i = 0; ok && i < n; i++){
        const shdb_song *s = &songs[i];
        long long start = s->start_ms > 0 ? s->start_ms : 0;
        if(!s->path || !s->path[0]){ ok = 0; break; }
        const char *snap[3] = { s->title, s->album_artist, s->album };
        sqlite3_reset(ref); sqlite3_reset(item);
        sqlite3_bind_text(ref, 1, s->path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ref, 2, start);
        for(int k = 0; k < 3; k++)
            if(snap[k] && snap[k][0]) sqlite3_bind_text(ref, 3 + k, snap[k], -1, SQLITE_STATIC); else sqlite3_bind_null(ref, 3 + k);
        sqlite3_bind_text(item, 1, s->path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(item, 2, start);
        sqlite3_bind_int64(item, 3, id);
        sqlite3_bind_int64(item, 4, pos + i);
        ok = sqlite3_step(ref) == SQLITE_DONE && sqlite3_step(item) == SQLITE_DONE && sqlite3_changes(d) == 1;
    }
    sqlite3_finalize(ref); sqlite3_finalize(item);
    return end_change(d, ok) ? n : -1;
}
int shdb_pl_has(long long id, const char *path, long long start_ms){
    if(!path || !path[0]) return 0;
    pthread_mutex_lock(&g_mu);
    long long n = intf(db_locked(), 0, "SELECT count(*) FROM playlist_items i JOIN track_refs r ON r.id = i.track_ref_id"
                                       " WHERE i.playlist_id = ?1 AND r.path = ?2 AND r.start_ms = ?3;",
                       "iti", id, path, start_ms > 0 ? start_ms : 0);
    pthread_mutex_unlock(&g_mu);
    return (int)n;
}
int shdb_pl_remove(long long item_id){
    sqlite3 *d = begin_change();
    long long pid = intf(d, 0, "SELECT playlist_id FROM playlist_items WHERE id = ?1;", "i", item_id);
    int ok = pid > 0 && execf(d, "DELETE FROM playlist_items WHERE id = ?1;", "i", item_id)
          && execf(d, "UPDATE playlists SET updated_at = " NOW " WHERE id = ?1;", "i", pid)
          && execf(d, PRUNE_REFS, "");
    return end_change(d, ok);
}

long long shdb_folder_create(const char *name, long long parent_id){
    if(!name || !name[0]) return -1;
    sqlite3 *d = begin_change();
    int ok = execf(d, "INSERT INTO playlist_folders (name, parent_id, position, created_at)"
                      " VALUES (?2, ?1, " END_OF_FOLDER ", " NOW ");", "nt", parent_id, name);
    long long id = ok ? sqlite3_last_insert_rowid(d) : -1;
    return end_change(d, ok) ? id : -1;
}
int shdb_folder_rename(long long id, const char *name){
    if(!name || !name[0]) return 0;
    sqlite3 *d = begin_change();
    int ok = execf(d, "UPDATE playlist_folders SET name = ?1 WHERE id = ?2;", "ti", name, id) && sqlite3_changes(d) == 1;
    return end_change(d, ok);
}
int shdb_folder_delete(long long id){
    sqlite3 *d = begin_change();
    /* what it holds goes to the end of its parent, in the order it had */
    long long parent = intf(d, -1, "SELECT IFNULL(parent_id, 0) FROM playlist_folders WHERE id = ?1;", "i", id);
    long long base = parent < 0 ? -1 : intf(d, -1, "SELECT " END_OF_FOLDER ";", "n", parent);
    int ok = base >= 0
          && execf(d, "UPDATE playlists SET folder_id = ?1, position = ?2 + position WHERE folder_id = ?3;", "nii", parent, base, id)
          && execf(d, "UPDATE playlist_folders SET parent_id = ?1, position = ?2 + position WHERE parent_id = ?3;", "nii", parent, base, id)
          && execf(d, "DELETE FROM playlist_folders WHERE id = ?1;", "i", id);
    return end_change(d, ok);
}
int shdb_move(int is_folder, long long id, long long folder_id){
    sqlite3 *d = begin_change();
    long long here = intf(d, -1, is_folder ? "SELECT IFNULL(parent_id, 0) FROM playlist_folders WHERE id = ?1;"
                                           : "SELECT IFNULL(folder_id, 0) FROM playlists WHERE id = ?1;", "i", id);
    /* a folder can't go into itself or anything inside it: walk up from the target */
    long long loop = !is_folder || folder_id <= 0 ? 0
        : intf(d, -1, "WITH RECURSIVE up(fid) AS (SELECT ?1 UNION ALL SELECT f.parent_id FROM playlist_folders f JOIN up"
                      " ON f.id = up.fid WHERE f.parent_id IS NOT NULL) SELECT EXISTS (SELECT 1 FROM up WHERE fid = ?2);",
               "ii", folder_id, id);
    if(here < 0 || loop != 0){ end_change(d, 0); return here < 0 || loop < 0 ? -1 : 0; }
    if(here == (folder_id > 0 ? folder_id : 0)){ end_change(d, 0); return 1; }   /* already there */
    int ok = execf(d, is_folder ? "UPDATE playlist_folders SET parent_id = ?1, position = " END_OF_FOLDER " WHERE id = ?2;"
                                : "UPDATE playlists SET folder_id = ?1, position = " END_OF_FOLDER " WHERE id = ?2;",
                   "ni", folder_id, id);
    return end_change(d, ok) ? 1 : -1;
}
long long shdb_parent(int is_folder, long long id){
    pthread_mutex_lock(&g_mu);
    long long p = intf(db_locked(), -1, is_folder ? "SELECT IFNULL(parent_id, 0) FROM playlist_folders WHERE id = ?1;"
                                                  : "SELECT IFNULL(folder_id, 0) FROM playlists WHERE id = ?1;", "i", id);
    pthread_mutex_unlock(&g_mu);
    return p;
}

/* ---- reordering ---- */
/* One list put in a new order: sel gives its rows as (kind, id, position) in their order, the row (kind, id) goes to
 * place `to`, the others keeping their order. Positions become 0, 1, 2...; only rows whose position changes are written,
 * by upd[kind] (?1 position, ?2 id). 1 = done; 0 when the row isn't in the list. */
typedef struct { int kind; long long id, pos; } place_row;
static int place_in(sqlite3_stmt *sel, sqlite3_stmt *const upd[2], int kind, long long id, int to){
    place_row *v = NULL, moved = { -1, 0, 0 };
    int n = 0, cap = 0, rc = SQLITE_ERROR, ok = sel != NULL;
    while(ok && (rc = sqlite3_step(sel)) == SQLITE_ROW){
        place_row r = { sqlite3_column_int(sel, 0), sqlite3_column_int64(sel, 1), sqlite3_column_int64(sel, 2) };
        if(r.kind == kind && r.id == id){ moved = r; continue; }
        if(n == cap){
            int nc = cap ? cap * 2 : 64;
            place_row *p = realloc(v, (size_t)nc * sizeof *v);
            if(!p){ ok = 0; break; }
            v = p; cap = nc;
        }
        v[n++] = r;
    }
    ok = ok && rc == SQLITE_DONE && moved.kind >= 0;
    if(to < 0) to = 0;
    if(to > n) to = n;
    for(int i = 0; ok && i <= n; i++){
        const place_row *r = i < to ? &v[i] : i == to ? &moved : &v[i - 1];
        if(r->pos == i) continue;
        sqlite3_stmt *u = upd[r->kind != 0];
        sqlite3_reset(u);
        sqlite3_bind_int64(u, 1, i);
        sqlite3_bind_int64(u, 2, r->id);
        ok = sqlite3_step(u) == SQLITE_DONE;
    }
    free(v);
    return ok;
}
int shdb_place(int is_folder, long long id, int to){
    sqlite3 *d = begin_change();
    long long here = intf(d, -1, is_folder ? "SELECT IFNULL(parent_id, 0) FROM playlist_folders WHERE id = ?1;"
                                           : "SELECT IFNULL(folder_id, 0) FROM playlists WHERE id = ?1;", "i", id);
    sqlite3_stmt *sel = here < 0 ? NULL   /* in shdb_pl_list's order */
        : stmtf(d, "SELECT 1, id, position FROM playlist_folders WHERE parent_id IS ?1"
                   " UNION ALL SELECT 0, id, position FROM playlists WHERE folder_id IS ?1 ORDER BY 3, 1 DESC, 2;", "n", here);
    sqlite3_stmt *upd[2] = { stmtf(d, "UPDATE playlists SET position = ?1 WHERE id = ?2;", ""),
                             stmtf(d, "UPDATE playlist_folders SET position = ?1 WHERE id = ?2;", "") };
    int ok = upd[0] && upd[1] && place_in(sel, upd, is_folder != 0, id, to);
    sqlite3_finalize(sel); sqlite3_finalize(upd[0]); sqlite3_finalize(upd[1]);
    return end_change(d, ok);
}
int shdb_pl_item_place(long long item_id, int to){
    sqlite3 *d = begin_change();
    long long pid = intf(d, 0, "SELECT playlist_id FROM playlist_items WHERE id = ?1;", "i", item_id);
    sqlite3_stmt *sel = pid <= 0 ? NULL   /* in ldb_read_playlist's order */
        : stmtf(d, "SELECT 0, id, position FROM playlist_items WHERE playlist_id = ?1 ORDER BY position, id;", "i", pid);
    sqlite3_stmt *upd[2] = { stmtf(d, "UPDATE playlist_items SET position = ?1 WHERE id = ?2;", ""), NULL };
    upd[1] = upd[0];
    int ok = upd[0] && place_in(sel, upd, 0, item_id, to)
          && execf(d, "UPDATE playlists SET updated_at = " NOW " WHERE id = ?1;", "i", pid);
    sqlite3_finalize(sel); sqlite3_finalize(upd[0]);
    return end_change(d, ok);
}

/* ---- album collections ---- */
int shdb_coll_list(const char *album_artist, const char *album, shdb_coll **out){
    *out = NULL;
    pthread_mutex_lock(&g_mu);
    sqlite3_stmt *st = stmtf(db_locked(),
        "SELECT c.id, c.name, (SELECT count(*) FROM collection_albums a WHERE a.collection_id = c.id),"
        " EXISTS (SELECT 1 FROM collection_albums a WHERE a.collection_id = c.id AND a.album_artist = ?1 AND a.album_title = ?2)"
        " FROM collections c ORDER BY c.position, c.id;", "ss", album_artist, album);
    int n = -1;
    if(st){
        shdb_coll *v = NULL; int cap = 0, rc;
        n = 0;
        while((rc = sqlite3_step(st)) == SQLITE_ROW){
            if(n == cap){
                int nc = cap ? cap * 2 : 16;
                shdb_coll *p = realloc(v, (size_t)nc * sizeof *v);
                if(!p){ rc = SQLITE_NOMEM; break; }
                v = p; cap = nc;
            }
            shdb_coll *c = &v[n++];
            c->id = sqlite3_column_int64(st, 0);
            const unsigned char *name = sqlite3_column_text(st, 1);
            snprintf(c->name, sizeof c->name, "%s", name ? (const char *)name : "");
            c->count = sqlite3_column_int(st, 2);
            c->has = album && sqlite3_column_int(st, 3);
        }
        if(rc != SQLITE_DONE){ free(v); v = NULL; n = -1; }
        *out = v;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mu);
    return n;
}
long long shdb_coll_create(const char *name){
    if(!name || !name[0]) return -1;
    sqlite3 *d = begin_change();
    long long taken = intf(d, 0, "SELECT 1 FROM collections WHERE name = ?1;", "t", name), id = -1;
    int ok = taken == 0
          && execf(d, "INSERT INTO collections (name, position, created_at, updated_at)"
                      " VALUES (?1, (SELECT IFNULL(MAX(position), -1) + 1 FROM collections), " NOW ", " NOW ");", "t", name);
    if(ok) id = sqlite3_last_insert_rowid(d);
    if(!end_change(d, ok)) return taken == 1 ? 0 : -1;
    return id;
}
int shdb_coll_name(long long id, char *out, int cap){
    return name_of("SELECT name FROM collections WHERE id = ?1;", id, out, cap);
}
int shdb_coll_rename(long long id, const char *name){
    if(!name || !name[0]) return -1;
    sqlite3 *d = begin_change();
    long long taken = intf(d, 0, "SELECT 1 FROM collections WHERE name = ?1 AND id <> ?2;", "ti", name, id);
    int ok = taken == 0 && execf(d, "UPDATE collections SET name = ?1, updated_at = " NOW " WHERE id = ?2;", "ti", name, id)
          && sqlite3_changes(d) == 1;
    if(!end_change(d, ok)) return taken == 1 ? 0 : -1;
    return 1;
}
int shdb_coll_delete(long long id){
    sqlite3 *d = begin_change();
    int ok = execf(d, "DELETE FROM collections WHERE id = ?1;", "i", id) && sqlite3_changes(d) == 1;   /* its rows go too */
    return end_change(d, ok);
}
int shdb_coll_count(void){
    pthread_mutex_lock(&g_mu);
    long long n = intf(db_locked(), -1, "SELECT count(*) FROM collections;", "");
    pthread_mutex_unlock(&g_mu);
    return (int)n;
}
int shdb_coll_place(long long id, int to){
    sqlite3 *d = begin_change();
    sqlite3_stmt *sel = stmtf(d, "SELECT 0, id, position FROM collections ORDER BY position, id;", "");   /* shdb_coll_list's order */
    sqlite3_stmt *upd[2] = { stmtf(d, "UPDATE collections SET position = ?1 WHERE id = ?2;", ""), NULL };
    upd[1] = upd[0];
    int ok = upd[0] && place_in(sel, upd, 0, id, to);
    sqlite3_finalize(sel); sqlite3_finalize(upd[0]);
    return end_change(d, ok);
}
int shdb_coll_set(long long id, const char *album_artist, const char *album, int on){
    if(!album || !album[0]) return 0;
    sqlite3 *d = begin_change();
    int ok = on ? execf(d, "INSERT OR IGNORE INTO collection_albums (collection_id, album_artist, album_title, added_at)"
                           " SELECT id, ?2, ?3, " NOW " FROM collections WHERE id = ?1;", "iss", id, album_artist, album)
                : execf(d, "DELETE FROM collection_albums WHERE collection_id = ?1 AND album_artist = ?2 AND album_title = ?3;",
                        "iss", id, album_artist, album);
    ok = ok && execf(d, "UPDATE collections SET updated_at = " NOW " WHERE id = ?1;", "i", id) && sqlite3_changes(d) == 1;
    return end_change(d, ok);
}
