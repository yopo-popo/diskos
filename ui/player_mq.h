/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* player_mq - plays our queue (queue.h) on the stock mq_player. The next player (MPD or our own engine) gets an adapter
 * with these same player_* calls; player_owns and player_release go away with the stock player's own play paths. */
#ifndef DISKOS_PLAYER_MQ_H
#define DISKOS_PLAYER_MQ_H

#include "queue.h"

/* After queue_open: load the saved queue into the player's list and follow the queue's changes. */
void player_init(void);

/* Play these songs as the new queue, from t[start]. unit = an album played as one block (NULL = loose songs). With
 * Shuffle chosen, t[start] plays first and the rest follow in a random order, as loose songs. 1 = the play was sent;
 * every failure toasts once. */
int  player_play(const queue_track *t, int n, const queue_album *unit, int start);

/* While the stock player still builds lists of its own (search, files, playlists, books): */
int  player_owns(void);       /* 1 while the player plays our queue */
int  player_release(void);    /* call before such a play; 1 if the player was playing our queue */

#endif
