/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* playlists - the owner's playlists (shelves.db) on screen: Add to playlist (SCR_PLPICK), the playlist entries of the
 * hold menu (itemmenu.h), and playlists as .m3u8 files on the card. */
#ifndef DISKOS_PLAYLISTS_H
#define DISKOS_PLAYLISTS_H
#include "librarydb.h"

/* Add to playlist: the picker for these songs (a list as shown, copied), or for the song playing now. */
void playlists_add_songs(const ldb_item *v, int n);
void playlists_add_current(void);

/* Close the picker's dialogs (a screen change). */
void plpick_close_transients(void);
/* playlistview.c: the playlist shown (0 = none), and its rows or name changed (reread, keeping the place) */
long long plview_current(void);
void plview_changed(void);

/* Mark the songs of a playlist list whose file is gone: count 1 (not in the library) becomes 2. */
void playlists_mark_missing(ldb_list *l);

/* Write a playlist to /tmp/sdcard/Playlists/<name>.m3u8, its paths relative to that file. 1 = written; file gets the
 * file's name. */
int playlists_export(long long id, const char *name, char *file, int cap);
/* Read every .m3u / .m3u8 on the card into playlists; a file whose playlist name exists is skipped. Returns how many
 * playlists were made, -1 = the card can't be read. *skipped (may be NULL) = files left out. */
int playlists_import_card(int *skipped);

#endif
