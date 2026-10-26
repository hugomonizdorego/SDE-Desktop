/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Liria's file operations window in the design of SDE's transfer dialog
 * (0.2.0 Alpha 3, added by SDE, see suite/redesign.py): a penguin carries
 * papers from one folder to the other (a bin for Delete and the Trash),
 * "Copying 3 items to Backup" with Pause, Queue and Cancel, a slim bar,
 * "780 MB of 1.2 GB · 22 MB/s · About 40 seconds left", the item being
 * copied and Details (from, to, items); further operations stand under the
 * first as its queue. The picture and the model are SDE's own
 * (liria-sde-transfer-scene.h and liria-sde-transfer-model.h, copies of
 * src/common/transfer_scene.h and transfer_model.h), so Liria and the SDE
 * desktop look and estimate the same.
 */
#ifndef LIRIA_SDE_PROGRESS_H
#define LIRIA_SDE_PROGRESS_H

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "liria-progress-info.h"

typedef enum {
    LIRIA_SDE_TRANSFER_COPY,
    LIRIA_SDE_TRANSFER_MOVE,
    LIRIA_SDE_TRANSFER_DUPLICATE,
    LIRIA_SDE_TRANSFER_DELETE,
    LIRIA_SDE_TRANSFER_TRASH
} LiriaSdeTransferKind;

/* What an operation reports, from the job's thread. begin () names the
 * folders (it may block, once); the others are cheap. Delete and Trash
 * only begin: their texts and progress stay their own. */
void liria_sde_progress_transfer_begin (LiriaProgressInfo *info, LiriaSdeTransferKind kind, GList *files,
                                        GFile *destination);
void liria_sde_progress_transfer_counts (LiriaProgressInfo *info, goffset bytes_done, goffset bytes_total,
                                         int files_done, int files_total);
void liria_sde_progress_transfer_item (LiriaProgressInfo *info, GFile *file);
/* The destination drive, when it is written safely (liria-sde-copy.c):
 * its name, whether it may be unplugged once done, whether it is FAT. */
void liria_sde_progress_transfer_drive (LiriaProgressInfo *info, const char *name, gboolean removable,
                                        gboolean fat);
/* Writing the last data to the drive. */
void liria_sde_progress_transfer_finishing (LiriaProgressInfo *info);
/* The drive failed: |message| says how (the operation is not done). */
void liria_sde_progress_transfer_failed (LiriaProgressInfo *info, const char *message);
/* Slows a copy down to LIRIA_SDE_QA_COPY_RATE bytes per second (tests and
 * screenshots of the window); does nothing without it. */
void liria_sde_progress_transfer_throttle (goffset bytes);

/* The operation's widget: builds the layout into |box| with the buttons of
 * the operation and returns the labels and the bar that the operation's
 * own status, details and progress go to. */
void liria_sde_progress_build (GtkWidget *box, LiriaProgressInfo *info, GtkWidget *start_button,
                               GtkWidget *queue_button, GtkWidget *cancel_button, GtkWidget **status,
                               GtkWidget **progress_bar, GtkWidget **details);
/* 0 running, 1 paused (or pausing), 2 queued. */
void liria_sde_progress_set_state (GtkWidget *box, int state);
/* The operation is over: TRUE when |box| shows that for a moment (done:
 * the penguin's jump and, for a removable drive, "The files are on
 * SANDISK - you can remove it."; failed: until its cross is clicked) and
 * calls |remove| (|data|) later; FALSE when the caller removes it now
 * (cancelled, or the window was never shown). */
gboolean liria_sde_progress_finish (GtkWidget *box, GSourceFunc remove, gpointer data);
/* TRUE (and the operation goes) when the cross of a finished operation
 * was clicked; FALSE: it cancels a running one. */
gboolean liria_sde_progress_close (GtkWidget *box);
/* TRUE once the operation is over (it does not count as running). */
gboolean liria_sde_progress_is_finished (GtkWidget *box);
/* The file operations window. */
void liria_sde_progress_window_setup (GtkWidget *window);

#endif
