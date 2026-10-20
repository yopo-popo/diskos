/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* queuemenu - the queue's entries in the hold menu: Play now (a new queue), Play next and Add to queue, on a song, an
 * album, an artist, a genre or a playlist. An album goes in as one unit; an artist's, a genre's or a playlist's songs go
 * in as loose songs, in the order the Library lists them. Play next and Add to queue are offered only while the player
 * plays our queue: on another list (a book, a search result) they would change nothing that plays.
 * On a row of the queue itself (Up Next; row_id = the song's queue id, or the album's unit id): Play from here, Go to
 * album (for a song the Library's own entry does it), Ungroup (the album's songs become loose songs) and Remove from
 * queue. The playing song and the album holding it are neither played from nor removed. */
#include "queuemenu.h"
#include "itemmenu.h"
#include "player_mq.h"
#include "librarydb.h"
#include "shelvesdb.h"
#include "musicdb.h"   /* mdb_artist_mode: the Artists grouping */
#include "screens.h"
#include <stdlib.h>
#include <string.h>

static int playable(const im_item *it){
    switch(it->kind){
    case IM_SONG:     return it->path[0] != 0;
    case IM_ALBUM:
    case IM_ARTIST:
    case IM_GENRE:    return 1;
    case IM_PLAYLIST: return it->id > 0;
    default:          return 0;
    }
}

/* The songs it stands for: a list read into l (free it), or for a song, its row in *one (its strings may be l's). Returns
 * the count, -1 = can't read. */
static int item_songs(const im_item *it, ldb_list *l, ldb_item *one, char *artist, int cap){
    memset(l, 0, sizeof *l);
    switch(it->kind){
    case IM_SONG:
        /* the hold menu's item has no artist or length: a song on an album has them in the album's list */
        if(it->album[0] && ldb_read_album_songs(it->album_artist, it->album, l) > 0)
            for(int i = 0; i < l->n; i++)
                if(l->v[i].start_ms == it->start_ms && !strcmp(l->v[i].path, it->path)){ *one = l->v[i]; return 1; }
        ldb_list_free(l);
        memset(l, 0, sizeof *l);
        memset(one, 0, sizeof *one);
        one->path = it->path; one->start_ms = it->start_ms; one->title = it->title;
        one->album = it->album[0] ? it->album : NULL; one->album_artist = it->album_artist;
        one->artist = ldb_song_artist(it->path, it->start_ms, 0, artist, cap) ? artist : "";
        return 1;
    case IM_ALBUM:    return ldb_read_album_songs(it->album_artist, it->album, l);
    case IM_ARTIST:   return ldb_read_scope_songs(mdb_artist_mode() ? LDB_SCOPE_GROUP : LDB_SCOPE_ARTIST, it->key, l);
    case IM_GENRE:    return ldb_read_scope_songs(LDB_SCOPE_GENRE, it->key, l);
    case IM_PLAYLIST: return shdb_ready() ? ldb_read_playlist(it->id, l) : -1;
    default:          return 0;
    }
}

void queuemenu_songs(const ldb_item *v, int n, const queue_album *unit, int how){
    queue_track *t = n > 0 ? malloc((size_t)n * sizeof *t) : NULL;
    int m = 0;
    for(int i = 0; t && i < n; i++){
        const ldb_item *s = &v[i];
        if(s->count == 2) continue;   /* Favourites and playlists: its file is gone */
        int sub = s->source == LDB_SRC_CUE || s->source == LDB_SRC_ISO;   /* a track inside a bigger file */
        t[m++] = (queue_track){ .path = s->path, .start_ms = (int)s->start_ms,
                                .end_ms = sub && s->duration_ms > 0 ? (int)s->start_ms + s->duration_ms : 0,
                                .duration_ms = s->duration_ms, .title = s->title, .artist = s->artist, .album = s->album };
    }
    if(n < 0) ui_toast("Couldn't read the songs");
    else if(n > 0 && !t) ui_toast("Couldn't play that");
    else if(!m) ui_toast("Nothing to play");
    else if(how == QM_NOW){
        if(player_play(t, m, unit, 0)) screen_show(SCR_NOWPLAYING);   /* refused: toasted there */
    } else {
        int rc = how == QM_NEXT ? queue_add_next(t, m, unit) : queue_add_last(t, m, unit);
        if(rc < 0) ui_toast("Couldn't add it");
        else if(player_owns()) ui_toast(how == QM_NEXT ? "Playing next" : "Added to queue");   /* else the adapter said why */
    }
    free(t);
}
static void run(const im_item *it, int how){
    ldb_list l;
    ldb_item one;
    char artist[LDB_NAME];
    int n = item_songs(it, &l, &one, artist, sizeof artist);
    queue_album u = { it->album_artist, it->album };
    queuemenu_songs(it->kind == IM_SONG ? &one : l.v, n, it->kind == IM_ALBUM ? &u : NULL, how);
    ldb_list_free(&l);
}

static int now_shown(const im_item *it){ return it->where != IM_IN_QUEUE && playable(it); }
static int add_shown(const im_item *it){ return now_shown(it) && player_owns() && queue_current() >= 0; }
static void now_run(const im_item *it){ run(it, QM_NOW); }
static void next_run(const im_item *it){ run(it, QM_NEXT); }
static void last_run(const im_item *it){ run(it, QM_LAST); }

/* ---- a row of the queue ---- */
static int q_first(const im_item *it){   /* the row's first song in the queue, -1 = gone */
    if(it->where != IM_IN_QUEUE || !it->row_id) return -1;
    if(it->kind == IM_SONG) return queue_find(it->row_id);
    int f;   /* the playing album: its songs after the playing one (its row stands for those) */
    return it->kind == IM_ALBUM && queue_unit_later(it->row_id, &f) > 0 ? f : -1;
}
static int q_later(const im_item *it){ int f = q_first(it); return f >= 0 && f > queue_current(); }
static int q_album(const im_item *it){ return it->kind == IM_ALBUM && q_first(it) >= 0; }
static void here_run(const im_item *it){
    int f = q_first(it);
    if(f <= queue_current()) return;
    if(queue_jump(f) == 0) screen_show(SCR_NOWPLAYING);
    else ui_toast("Couldn't play that");
}
static void album_run(const im_item *it){ library_open_album(it->album_artist, it->album); }
static void ungroup_run(const im_item *it){ if(queue_pop_unit(it->row_id) < 0) ui_toast("Couldn't do that"); }
static void remove_run(const im_item *it){
    int rc = it->kind == IM_ALBUM ? queue_remove_unit(it->row_id) : queue_remove_item(queue_find(it->row_id));
    if(rc < 0) ui_toast("Couldn't remove that");
}

static const im_entry E_NOW  = { "Play now", now_shown, now_run, 100 };
static const im_entry E_HERE = { "Play from here", q_later, here_run, 100 };
static const im_entry E_NEXT = { "Play next", add_shown, next_run, 110 };
static const im_entry E_LAST = { "Add to queue", add_shown, last_run, 120 };
static const im_entry E_ALBUM = { "Go to album", q_album, album_run, 200 };
static const im_entry E_UNGROUP = { "Ungroup", q_album, ungroup_run, 400 };
static const im_entry E_REMOVE = { "Remove from queue", q_later, remove_run, 900 };

void queuemenu_init(void){
    itemmenu_add(&E_NOW);
    itemmenu_add(&E_HERE);
    itemmenu_add(&E_NEXT);
    itemmenu_add(&E_LAST);
    itemmenu_add(&E_ALBUM);
    itemmenu_add(&E_UNGROUP);
    itemmenu_add(&E_REMOVE);
}
