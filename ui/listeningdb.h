/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* listeningdb - /usr/data/diskos/db/listening.db, what is written while you listen: play counts, audiobook bookmarks
 * and the queue. It is kept apart from shelves.db (favourites, playlists), which changes only when you edit it, so the
 * file written on every song can never take your playlists with it.
 *
 * Several modules share the file. Each opens it with its own connection and creates and upgrades only its own tables;
 * schema_part(part, version) holds one row per part: 'history' (this module: play counts and bookmarks) and 'queue'
 * (the queue module's queue_* tables). No table of one part references another part's.
 *
 * Nothing is ordered by time: the Disc's clock starts at 2020-03-01 until it syncs. Order comes from sequence numbers
 * that only grow; the times are stored for information. */
#ifndef DISKOS_LISTENINGDB_H
#define DISKOS_LISTENINGDB_H
#include "librarydb.h"

#define LISTENINGDB_PATH LIBRARYDB_DIR "/listening.db"

/* Count one play of a song (path + its start in the file, 0 unless a CUE/ISO track): its play count goes up and it
 * becomes the most recent. 1 = written. */
int lsdb_record_play(const char *path, long long start_ms);

/* A book's bookmark. 1 = found (the outputs set), 0 = none, -1 = read error (must not be taken as "start from zero").
 * seq_out = its place in "continue listening" (higher = saved later). Any output may be NULL. */
int lsdb_book_progress(const char *key, char *member_out, int member_cap, long *position_ms, int *completed_out,
                       long *updated_out, long long *seq_out);
/* Save a book's bookmark (insert or replace). 1 = written, 0 = failed (the caller retries). */
int lsdb_book_save(const char *key, const char *member_path, long position_ms, int completed);

/* Open listening.db now, creating it and this module's tables (a reader attaching the file needs it to exist).
 * 1 = open. */
int lsdb_ready(void);
void lsdb_close(void);

#endif
