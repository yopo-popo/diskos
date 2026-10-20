/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* queuemenu - the queue's entries in the hold menu (itemmenu.h). */
#ifndef DISKOS_QUEUEMENU_H
#define DISKOS_QUEUEMENU_H

#include "librarydb.h"
#include "queue.h"

void queuemenu_init(void);   /* add the entries; once, at startup */

/* What the entries do, for other buttons (the album page's): play these songs now, next or last; unit = they are that
 * album (one unit in the queue), NULL = loose songs. Toasts what happened. */
enum { QM_NOW, QM_NEXT, QM_LAST };
void queuemenu_songs(const ldb_item *v, int n, const queue_album *unit, int how);

#endif
