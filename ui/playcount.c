/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include <stdio.h>
#include <string.h>
#include "playcount.h"

void playcount_reset(playcount_t *pc){ memset(pc, 0, sizeof *pc); }

int playcount_sample(playcount_t *pc, const char *path, const char *title, int playing, long position_ms, long duration_ms){
    if(!path || !path[0]){ playcount_reset(pc); return 0; }
    if(!title) title = "";
    if(!pc->active || strcmp(pc->path, path) || strcmp(pc->title, title)){   /* another song: follow it from here */
        playcount_reset(pc);
        snprintf(pc->path, sizeof pc->path, "%s", path);
        snprintf(pc->title, sizeof pc->title, "%s", title);
        pc->last_pos_ms = position_ms;
        pc->active = 1;
        return 0;
    }
    long delta = position_ms - pc->last_pos_ms;
    pc->last_pos_ms = position_ms;                         /* while paused too, so a paused seek isn't heard later */
    if(pc->counted && delta < -PLAYCOUNT_STEP_MS && position_ms < PLAYCOUNT_STEP_MS){
        pc->heard_ms = 0; pc->counted = 0;                 /* back at the start after it counted: a new play */
        return 0;
    }
    if(!playing) return 0;
    if(delta > 0 && delta <= PLAYCOUNT_STEP_MS) pc->heard_ms += delta;
    if(!pc->counted && duration_ms > 0 && pc->heard_ms * 100 >= (long long)duration_ms * PLAYCOUNT_PERCENT){
        pc->counted = 1;
        return 1;
    }
    return 0;
}
