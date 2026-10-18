/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* favourites - the playing song's heart, kept in shelves.db: Now Playing, its Options menu and the Quick settings tile
 * ask and change it here. The song is the player's file plus, for a CUE/ISO track, its start (found in library.db). */
#ifndef DISKOS_FAVOURITES_H
#define DISKOS_FAVOURITES_H
#include "ipc.h"

/* 1 = the song st plays is a favourite. Cheap to ask on every update: the answer is kept until the song or
 * shelves.db changes. */
int fav_current(const track_state_t *st);
/* Make it a favourite (on) or not, with a snapshot of its title and album. 1 = saved. */
int fav_current_set(const track_state_t *st, int on);

#endif
