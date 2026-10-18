/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef PLAYCOUNT_H
#define PLAYCOUNT_H

/* Pure play counting (yopo, 2026-10-18): a song counts as played once the time actually heard reaches
 * PLAYCOUNT_PERCENT of its length, however the listener jumped around in it. It counts at that moment, and once per
 * play: a song that starts over (repeat one, or a jump back to its start) after it counted is a new play. Fed the
 * player's position about once a second; kept dependency-free so it is tested off the Disc. */
#define PLAYCOUNT_PERCENT  80
#define PLAYCOUNT_STEP_MS  4000   /* a forward step larger than this between two samples is a seek, not listening */

typedef struct {
    char path[256], title[256];   /* the song followed: CUE tracks of one file differ only by title */
    long last_pos_ms;
    long long heard_ms;
    int  active, counted;
} playcount_t;

void playcount_reset(playcount_t *pc);
/* One sample. playing = the main loop's play state (the player's own flag is unreliable). Returns 1 exactly when this
 * sample completes a play. */
int playcount_sample(playcount_t *pc, const char *path, const char *title, int playing, long position_ms, long duration_ms);

#endif
