/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef FOLDERBROWSER_H
#define FOLDERBROWSER_H
#include "lvgl/lvgl.h"
/* File/folder browser (SCR_FOLDER): a browsable directory tree of the SD card
 * (mounted at /tmp/sdcard). Tap a folder to descend; the left-edge swipe
 * ascends one level (folderbrowser_back) and the header goes to Library's top. Tap an audio file
 * to play that track. See folderbrowser.c for the honest limitation on
 * whole-folder queue playback. */
void folderbrowser_create(lv_obj_t *root);   /* build the screen once (from screens_init) */
void folderbrowser_open(void);               /* reset to the SD root + show SCR_FOLDER */
int  folderbrowser_back(void);               /* up one folder, where it was: 1, or 0 at the SD root */
#endif
