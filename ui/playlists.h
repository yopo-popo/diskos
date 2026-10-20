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

/* A small dialog over the screen: a question, a line under it (NULL = none) and up to three buttons, labels[i] running
 * fns[i] (NULL only closes); button danger (-1 = none) is drawn as destructive. A tap outside is Cancel. */
void playlists_dialog(const char *title, const char *sub, int n, const char *const *labels, void (*const *fns)(void), int danger);
/* Close the picker's dialogs (a screen change). */
void plpick_close_transients(void);
/* playlistview.c: the playlist shown (0 = none), and its rows or name changed (reread, keeping the place) */
long long plview_current(void);
void plview_changed(void);
/* Reorder (a handle on each row, until Done): a playlist's songs (playlistview.c), the Playlists list shown (library.c) */
void plview_reorder(int on);
void library_pl_reorder(int on);

/* Mark the songs of a playlist list whose file is gone: count 1 (not in the library) becomes 2. */
void playlists_mark_missing(ldb_list *l);

/* Write a playlist to /tmp/sdcard/Playlists/<name>.m3u8, its paths relative to that file. 1 = written; file gets the
 * file's name. */
int playlists_export(long long id, const char *name, char *file, int cap);
/* Read one .m3u / .m3u8 file (the one the owner picked; nothing searches the card for them) into a new playlist at the
 * end of folder_id (0 = the top). A name in use gets " (2)" and so on. Returns its id, with its name, the songs added
 * (found) and the songs not on the card (missed); 0 = none of its songs are on the card, -1 = can't read it. */
long long playlists_import_file(const char *file, long long folder_id, char *name, int cap, int *found, int *missed);

#endif
