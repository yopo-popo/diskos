/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* librarydb - diskOS's own music library database, /usr/data/diskos/db/library.db.
 *
 * library.db holds only what a rescan can rebuild from the files on the SD card (songs, albums, artists, the genres
 * written in the files). What the owner creates (collections, their genres, playlists) goes in shelves.db, and what is
 * written while listening (play counts, bookmarks, the queue) in listening.db; neither stores a library.db row number.
 * FiiO's song.db stays only while the stock mq_player still plays.
 *
 * The scanner rebuilds library.db on every scan into library.db.new beside it and renames it over the old file only
 * when the scan read the whole card, so a failed or cancelled scan keeps the previous library. Songs the scan could not
 * read this time are carried over from the previous file (ldb_keep). Sort keys are decided here when the scanner
 * writes (ldb_sort_key): English, then Hebrew, then everything else ('#'), skipping punctuation a name starts with. */
#ifndef DISKOS_LIBRARYDB_H
#define DISKOS_LIBRARYDB_H
#include "sqlite3.h"

#ifndef LIBRARYDB_DIR
#define LIBRARYDB_DIR   "/usr/data/diskos/db"
#endif
#define LIBRARYDB_PATH  LIBRARYDB_DIR "/library.db"
/* PRAGMA user_version of library.db. Bump it whenever the schema or the sort-key rule changes: a library.db with
 * another version is never read or carried over, the next scan rebuilds it from the card. */
#define LIBRARYDB_SCHEMA 2

#define LDB_NAME 256            /* longest stored name in bytes, with its NUL (the scanner's TAGLEN) */
#define LDB_MAXV 16             /* values kept per multi-valued tag (artists, genres) */

/* Open a library.db, shelves.db or listening.db connection. Every connection goes through here, because SQLite only
 * enforces REFERENCES / ON DELETE CASCADE after PRAGMA foreign_keys = ON on that connection. flags as sqlite3_open_v2.
 * Returns SQLITE_OK with *out set; on failure *out is NULL. */
int ldb_open(const char *path, int flags, sqlite3 **out);

/* The sort key of a name: a group mark ('1' English, '2' Hebrew, '3' everything else) and the name folded so a plain
 * byte comparison gives the order: Latin letters without case or accents, Hebrew final letters as their normal
 * form, points and direction marks dropped, leading spaces skipped. Always NUL-terminated, valid UTF-8. */
void ldb_sort_key(const char *name, char *out, int cap);
/* The jump-bar letter a key files under: 'A'..'Z', 0x80 + 0..21 for alef..tav, or '#'. */
int ldb_key_letter(const char *key);

/* Split tag values into names: each value on ';' and, when split_feat is set, on "feat." as a whole word in any case
 * (brackets around a "(feat. X)" part are dropped). Never on ',', '/', '&' or 'x'. Pieces are trimmed, empty ones
 * dropped and repeats removed, in order. Returns the count written to out (at most max). */
int ldb_split_names(const char *const *vals, int nvals, int split_feat, char (*out)[LDB_NAME], int max);

enum { LDB_SRC_FILE = 0, LDB_SRC_CUE = 1, LDB_SRC_ISO = 2 };
enum { LDB_RG_TG = 1, LDB_RG_TP = 2, LDB_RG_AG = 4, LDB_RG_AP = 8 };

/* One song. Strings are UTF-8; NULL or "" means not tagged unless noted. Numbers: 0 means not tagged. */
typedef struct {
    const char *path;                 /* absolute path on the SD card */
    long long start_ms, end_ms;       /* a slice of the file (CUE/ISO); end_ms < 0 = to the end of the file */
    int source;                       /* LDB_SRC_* */
    const char *album_title;          /* NULL = the song has no album tag (then album_artist is ignored) */
    const char *album_artist;         /* with an album: never NULL, "" when unknown */
    const char *title;                /* never NULL or "" (the scanner falls back to the file name) */
    const char *title_sort;
    const char (*artists)[LDB_NAME];  /* credited artists, already split, in order */
    const char (*artist_sorts)[LDB_NAME]; /* NULL, or one sort name per artist ("" = none) */
    int n_artists;
    const char (*genres)[LDB_NAME];   /* genres written in the file, already split */
    int n_genres;
    int disc, track, year;
    double bpm;
    const char *initial_key;
    int rg_flags;                     /* LDB_RG_*: which of the four ReplayGain values are set */
    double rg_track_gain, rg_track_peak, rg_album_gain, rg_album_peak;
    long long duration_ms;            /* <= 0 = unknown */
    int sample_rate, bits, channels;
    const char *codec;
    int has_lyrics;                   /* 0 none, 1 plain, 2 synced */
    long long file_mtime, file_size;
    /* album-level tags, merged over the album's songs */
    int track_total, disc_total, compilation;
    const char *release_type, *album_artist_sort, *album_sort;
} ldb_track;

/* What ldb_keep carries over from the previous library.db, for things this scan could not read. */
enum {
    LDB_KEEP_PATH,      /* every song of this exact path (an unreadable file or disc image) */
    LDB_KEEP_TREE,      /* every song under this folder (path ends in '/'): an entry the walk could not resolve */
    LDB_KEEP_CUE_TREE   /* CUE songs under this folder that no sheet wrote this scan (an unreadable sheet) */
};

typedef struct {
    long long tracks, albums, artists, carried;   /* rows in the finished file; songs carried over */
    long long bytes;                              /* size of the finished file */
    int carry_failed;                             /* the previous library.db could not be read: nothing carried */
} ldb_stats;

/* Create LIBRARYDB_DIR (owner-only) if needed, for library.db and the user files beside it. 0 = it is a directory. */
int ldb_make_dir(void);

typedef struct ldb_writer ldb_writer;
/* Start writing library.db.new. reserve() = bytes of /usr/data that must stay free right now (song.db's journal and
 * growth); it is asked again as the file grows, so library.db always gives way to song.db. Returns NULL when
 * library.db is not written this scan, with the reason in why. */
ldb_writer *ldb_begin(long long (*reserve)(void), char *why, int whycap);
/* 1 while /usr/data still has reserve() free (the scanner asks before song.db commits) */
int ldb_room_ok(ldb_writer *w);
/* Add one song (0 ok, -1 the writer has failed). Songs under a .nomedia folder are left out silently. */
int ldb_add(ldb_writer *w, const ldb_track *t);
/* Remove this path's songs written so far (its whole-file row, when a CUE sheet replaces it with tracks). */
void ldb_drop_path(ldb_writer *w, const char *path);
void ldb_keep(ldb_writer *w, const char *path, int kind);
/* A folder holding a .nomedia file: it and its subfolders stay out of the library. */
void ldb_nomedia(ldb_writer *w, const char *dir);
int ldb_in_nomedia(const ldb_writer *w, const char *path);
/* Finish the file (carry-over, albums, artists, sort keys, indexes) and close it. cancel() is polled during the
 * long statements; returning nonzero stops them. 0 = ready to publish. */
int ldb_finish(ldb_writer *w, int (*cancel)(void *), void *ctx);
/* Rename the finished file over library.db (0 ok). Then, or instead, ldb_end frees the writer and removes an
 * unpublished library.db.new. */
int ldb_publish(ldb_writer *w);
void ldb_end(ldb_writer *w);
int ldb_failed(const ldb_writer *w);
const char *ldb_error(const ldb_writer *w);
void ldb_get_stats(const ldb_writer *w, ldb_stats *st);

/* ---- reading: the Library's lists ----
 * Each read opens library.db read-only, loads one whole list in library order (English, Hebrew, then '#') and closes
 * the file again, so a scan that replaced library.db is seen by the next read. A list owns its strings; keep it as
 * long as the rows on screen use it and ask ldb_list_current whether the file is still the one it was read from. */
typedef struct {
    const char *title;          /* song title / album title */
    const char *artist;         /* song: its artists joined with ", " ("" = none); album: the album artist ("" = unknown) */
    const char *path;           /* song: its file; album: its first song's file (for the cover) */
    const char *album;          /* song: its album, NULL = none (album lists: NULL) */
    const char *album_artist;   /* song: with an album, its album artist (album lists: NULL) */
    long long start_ms;         /* song: where it starts in the file (CUE/ISO), else 0 */
    int source;                 /* song: LDB_SRC_* */
    int disc, track;            /* song: 0 = not tagged */
    int duration_ms;            /* song: 0 = unknown */
    int count;                  /* album: its songs */
    int letter;                 /* the jump-bar letter of its sort key (ldb_key_letter) */
} ldb_item;
typedef struct {
    ldb_item *v; int n;
    char *pool;                 /* the strings */
    long long stamp[4];         /* the library.db it was read from */
} ldb_list;
/* Each returns the number of rows (0 = none), or -1 when library.db is missing, from another schema version or can't
 * be read; out is then an empty list. Free it with ldb_list_free (also fine on an empty or zeroed list). */
int  ldb_read_songs(ldb_list *out);                                    /* every song, by title, then artist */
int  ldb_read_albums(ldb_list *out);                                   /* every album, by title, then album artist */
int  ldb_read_album_songs(const char *album_artist, const char *album, ldb_list *out);   /* disc and track order */
/* The Artists view's names: every credited artist, or (by_album_artist) every album artist, plus the first artist of
 * songs without an album. "" = Unknown artist (songs without one), last. */
int  ldb_read_artists(int by_album_artist, ldb_list *out);
int  ldb_read_genres(ldb_list *out);                                   /* the files' genres; count = their songs */
/* An artist's or a genre's songs ("scope"): LDB_SCOPE_ARTIST = the songs crediting key ("" = songs with no artist),
 * LDB_SCOPE_GROUP = the songs under key in Artists by album artist, LDB_SCOPE_GENRE = the songs with genre key. */
enum { LDB_SCOPE_ARTIST, LDB_SCOPE_GROUP, LDB_SCOPE_GENRE };
/* the scope's albums (artist = the album artist, path = its first song, count = the scope's songs in it); a genre's are
 * only those whose every song has the genre */
int  ldb_read_scope_albums(int kind, const char *key, ldb_list *out);
int  ldb_read_scope_songs(int kind, const char *key, ldb_list *out);  /* by title, then artist */
int  ldb_read_scope_album_songs(int kind, const char *key, const char *album_artist, const char *album, ldb_list *out);
int  ldb_scope_count(int kind, const char *key);                       /* its songs, -1 = can't read */
int  ldb_list_current(const ldb_list *l);   /* 1 = library.db is still the file l was read from */
void ldb_list_free(ldb_list *l);
/* The album's first songs' files, at most max, distinct (cover lookups). Returns the count, 0 on any failure. */
int  ldb_album_paths(const char *album_artist, const char *album, char (*paths)[512], int max);
/* The Artists-view name of the song at path (start_ms < 0 = its first song in the file): its first artist, or its
 * ldb_read_artists(1) name. 1 = found and not "". */
int  ldb_song_artist(const char *path, long long start_ms, int by_album_artist, char *out, int cap);
/* The album of the song at path (start_ms < 0 = its first song in the file). 1 = found. */
int  ldb_song_album(const char *path, long long start_ms, char *album_artist, int aacap, char *album, int acap);
/* The start in its file of the song playing as path + title (a CUE/ISO track; 0 for a plain file). 1 = in the library. */
int  ldb_track_start(const char *path, const char *title, long long *start_ms);
/* Most Played (by_recent 0: most plays first) or Recent (1: last played first): the songs listening.db counted that
 * are in the library. The caller opens listening.db first (lsdb_ready). 0 = nothing played yet, -1 = can't read. */
int  ldb_read_history(int by_recent, ldb_list *out);

#endif
