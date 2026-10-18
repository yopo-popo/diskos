/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* favourites - the playing song's heart (see favourites.h). */
#include <stdio.h>
#include <string.h>
#include "favourites.h"
#include "shelvesdb.h"

static struct {
    char path[256], title[IPC_TITLE_CAPACITY];   /* the song followed: CUE tracks of one file differ only by title */
    long long start_ms;
    unsigned gen;                                /* shdb_generation() when fav was read */
    int valid, fav;
} g_c;

static void follow(const track_state_t *st){
    if(g_c.valid && !strcmp(g_c.path, st->path) && !strcmp(g_c.title, st->title)) return;
    snprintf(g_c.path, sizeof g_c.path, "%s", st->path);
    snprintf(g_c.title, sizeof g_c.title, "%s", st->title);
    ldb_track_start(st->path, st->title, &g_c.start_ms);   /* 0 for a song not in the library */
    g_c.valid = 1;
    g_c.gen = shdb_generation() - 1;                       /* read below */
}
int fav_current(const track_state_t *st){
    if(!st || !st->have_track || !st->path[0]) return 0;
    follow(st);
    unsigned gen = shdb_generation();
    if(g_c.gen != gen){ g_c.fav = shdb_fav_get(g_c.path, g_c.start_ms) == 1; g_c.gen = gen; }
    return g_c.fav;
}
int fav_current_set(const track_state_t *st, int on){
    if(!st || !st->have_track || !st->path[0]) return 0;
    follow(st);
    char aa[LDB_NAME], al[LDB_NAME];
    if(!ldb_song_album(g_c.path, g_c.start_ms, aa, sizeof aa, al, sizeof al)){   /* not in the library: what the player shows */
        snprintf(aa, sizeof aa, "%s", st->artist);
        snprintf(al, sizeof al, "%s", st->album);
    }
    return shdb_fav_set(g_c.path, g_c.start_ms, on, st->title, aa, al);
}
