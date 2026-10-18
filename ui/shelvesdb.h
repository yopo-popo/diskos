/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* shelvesdb - /usr/data/diskos/db/shelves.db, what the owner puts together: favourites now; playlists, album
 * collections and their own genres later. It changes only when they edit something, so it lives apart from
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
#define SHELVESDB_SCHEMA 1

/* Is this song a favourite? 1 = yes, 0 = no, -1 = can't read. */
int shdb_fav_get(const char *path, long long start_ms);
/* Make it a favourite (on) or not. title / album_artist / album are its snapshot (NULL = unknown). 1 = saved. */
int shdb_fav_set(const char *path, long long start_ms, int on, const char *title, const char *album_artist,
                 const char *album);
/* Grows with every change, so a cache of favourite states knows when to ask again. */
unsigned shdb_generation(void);

/* Open shelves.db now (taking in the SD card's copy when it is new). 1 = open. */
int shdb_ready(void);
void shdb_close(void);

#endif
