/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* shelvesdb - /usr/data/diskos/db/shelves.db, what the owner puts together: favourites, playlists and album
 * collections now; their own genres later. It changes only when they edit something, so it lives apart from
 * listening.db (written on every song).
 *
 * A song is a track_refs row: its file path and its start in the file (CUE/ISO), with a snapshot of its title and
 * album that shows what a song whose file has gone was. Nothing here stores a library.db row number.
 *
 * After every change a copy goes to the SD card (SHELVES_BACKUP). A new shelves.db (first start, or a factory reset of
 * /usr/data) takes that copy in as soon as the card can be read, and never writes over it before then. */
#ifndef DISKOS_SHELVESDB_H
#define DISKOS_SHELVESDB_H
#include "librarydb.h"

#define SHELVESDB_PATH   LIBRARYDB_DIR "/shelves.db"
#ifndef SHELVES_BACKUP_DIR
#define SHELVES_BACKUP_DIR "/tmp/sdcard/.diskos/backup"
#endif
#define SHELVES_BACKUP   SHELVES_BACKUP_DIR "/shelves.db"
/* PRAGMA user_version of shelves.db; a file from a newer build is left alone */
#define SHELVESDB_SCHEMA 2

/* Is this song a favourite? 1 = yes, 0 = no, -1 = can't read. */
int shdb_fav_get(const char *path, long long start_ms);
/* Make it a favourite (on) or not. title / album_artist / album are its snapshot (NULL = unknown). 1 = saved. */
int shdb_fav_set(const char *path, long long start_ms, int on, const char *title, const char *album_artist,
                 const char *album);
/* Grows with every change, so a cache of favourite states knows when to ask again. */
unsigned shdb_generation(void);

/* ---- playlists and their folders ----
 * A playlist is a name and its songs in order; a song may be in it more than once. Playlists and folders sit in a
 * folder (0 = the top), each at a position there, and folders can hold folders. Playlist names are unique: they name
 * the exported files. */
typedef struct {
    const char *path; long long start_ms;
    const char *title, *album_artist, *album;   /* its snapshot (NULL = unknown) */
} shdb_song;
typedef struct {
    int is_folder;
    long long id;
    char name[LDB_NAME];
    int count;                 /* a playlist: its songs; a folder: the playlists and folders in it */
} shdb_entry;
/* The folders and playlists in a folder (0 = the top), in their order there. Returns how many (*out is malloc'd: free
 * it), -1 = can't read. */
int shdb_pl_list(long long folder_id, shdb_entry **out);
/* A new, empty playlist at the end of a folder (0 = the top). Its id; 0 = the name is taken, -1 = failed. */
long long shdb_pl_create(const char *name, long long folder_id);
long long shdb_pl_find(const char *name);                  /* the playlist's id; 0 = none, -1 = can't read */
int shdb_name(int is_folder, long long id, char *out, int cap);   /* a playlist's or folder's name; 1 = found, 0 = none, -1 = can't read */
int shdb_pl_rename(long long id, const char *name);        /* 1 = done, 0 = the name is taken, -1 = failed */
int shdb_pl_delete(long long id);                          /* 1 = done */
/* Add songs at the end, in order: all or none. Returns how many, -1 = failed. */
int shdb_pl_add(long long id, const shdb_song *songs, int n);
int shdb_pl_has(long long id, const char *path, long long start_ms);   /* times the song is in it; -1 = can't read */
int shdb_pl_remove(long long item_id);                     /* one entry (ldb_item.row_id); 1 = done */
long long shdb_folder_create(const char *name, long long parent_id);   /* its id, -1 = failed */
int shdb_folder_rename(long long id, const char *name);    /* 1 = done */
int shdb_folder_delete(long long id);                      /* what it holds moves up to its parent. 1 = done */
/* Move a playlist (is_folder 0) or a folder to the end of folder_id (0 = the top). A folder never goes into itself
 * or a folder inside it. 1 = done, 0 = not allowed, -1 = failed. */
int shdb_move(int is_folder, long long id, long long folder_id);
/* The folder a playlist or folder is in: its id, 0 = the top, -1 = no such item. */
long long shdb_parent(int is_folder, long long id);
/* Reordering by hand: put a playlist (is_folder 0) or folder at place `to` (0 = first) among what its folder holds, in
 * shdb_pl_list's order; or a playlist's entry (ldb_item.row_id) at place `to` among its songs. The rest keep their
 * order. 1 = done, 0 = failed or no such item. */
int shdb_place(int is_folder, long long id, int to);
int shdb_pl_item_place(long long item_id, int to);

/* ---- album collections ----
 * A collection is a name and a set of albums, in the order the owner arranges them; an album can be in any number.
 * Albums are kept as library.db keys them, album artist ("" = none) + album title, so a rescan never breaks a
 * collection. Names are unique, ignoring case. */
typedef struct {
    long long id;
    char name[LDB_NAME];
    int count;                 /* its albums, also those no longer in the library */
    int has;                   /* the album shdb_coll_list was asked about is in it */
} shdb_coll;
/* Every collection, in order; has = the album (album_artist, album) is in it (album NULL = never). Returns how many
 * (*out is malloc'd: free it), -1 = can't read. */
int shdb_coll_list(const char *album_artist, const char *album, shdb_coll **out);
long long shdb_coll_create(const char *name);              /* a new, empty one at the end: its id; 0 = the name is taken, -1 = failed */
int shdb_coll_name(long long id, char *out, int cap);      /* 1 = found, 0 = none, -1 = can't read */
int shdb_coll_rename(long long id, const char *name);      /* 1 = done, 0 = the name is taken, -1 = failed */
int shdb_coll_delete(long long id);                        /* its albums stay in the library. 1 = done */
int shdb_coll_count(void);                                 /* how many there are, -1 = can't read */
int shdb_coll_place(long long id, int to);                 /* Reorder: to place `to` (0 = first). 1 = done */
/* Put the album in the collection (on) or take it out. 1 = done (also when it already was, or wasn't, in it). */
int shdb_coll_set(long long id, const char *album_artist, const char *album, int on);

/* Open shelves.db now (taking in the SD card's copy when it is new). 1 = open. */
int shdb_ready(void);
void shdb_close(void);

#endif
