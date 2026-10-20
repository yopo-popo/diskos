/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* itemmenu - the menu a long press opens on a row: a song, album, artist, genre, playlist or playlist folder, wherever
 * it is listed (Library, a playlist, Favourites, the Queue...). It is one menu for the whole UI (yopo, 2026-10-20).
 *
 * Modules add their entries once (itemmenu_add). A row's long press fills an im_item and calls itemmenu_open, which
 * shows the entries whose shown() accepts that item, in `order`. An entry runs after the menu has closed, with its
 * own copy of the item, so the row and its list may already be gone.
 *
 * Order bands, so entries from different modules sit together: 100 play (Play now, Play next, Add to queue,
 * Play from here), 200 go to (album, artist), 300 collect (Add to playlist, Favourites, Move to folder),
 * 400 edit (Rename, Export), 900 remove (Remove from playlist / queue, Delete). */
#ifndef DISKOS_ITEMMENU_H
#define DISKOS_ITEMMENU_H

/* IM_PLAYLISTS_PAGE: the Playlists page itself (a hold on its title) */
typedef enum { IM_SONG, IM_ALBUM, IM_ARTIST, IM_GENRE, IM_PLAYLIST, IM_FOLDER, IM_PLAYLISTS_PAGE } im_kind;
/* where the row is listed */
typedef enum { IM_IN_LIBRARY, IM_IN_PLAYLIST, IM_IN_FAVOURITES, IM_IN_QUEUE, IM_IN_NOWPLAYING, IM_IN_PLAYLISTS } im_where;

typedef struct {
    im_kind  kind;
    char     title[256];                  /* the menu's heading: the song, album, artist, genre, playlist or folder name */
    char     path[512];                   /* song: its file */
    long long start_ms;                   /* song: its start in the file (CUE/ISO), else 0 */
    char     album_artist[256], album[256];   /* song: its album ("" = none); album row: the album's key */
    char     key[256];                    /* artist row: the artist ("" = unknown); genre row: the genre */
    long long id;                         /* playlist / folder row: its shelves.db id; Playlists page: the folder shown (0 = top) */
    im_where where;
    long long where_id;                   /* the list the row is in: a playlist's or folder's id, else 0 */
    long long row_id;                     /* the row's own id in that list (a playlist entry), else 0 */
    void    *ctx;                         /* the opener's data, passed through untouched */
} im_item;

typedef struct {
    const char *label;                    /* English; shown through tr() */
    int  (*shown)(const im_item *it);     /* 1 = list this entry for it (NULL = always) */
    void (*run)(const im_item *it);
    int order;
} im_entry;

/* Add an entry (e must stay valid: static). Adding the same entry twice does nothing. 0 = no room. */
int  itemmenu_add(const im_entry *e);
/* Open the menu for it (copied). Nothing opens when no entry is shown for it. */
void itemmenu_open(const im_item *it);
void itemmenu_close(void);
int  itemmenu_is_open(void);

#endif
