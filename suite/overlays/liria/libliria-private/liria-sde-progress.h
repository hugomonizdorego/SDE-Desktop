/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Liria's file operations window in the design of SDE's transfer dialog
 * (0.2.0 Alpha 2, added by SDE, see suite/redesign.py): the animated SDE
 * mark, "Copying 1,234 items from Documents to Backup", "42% complete",
 * Pause, Queue and Cancel, a progress bar or, with More details, the speed
 * graph, the item being copied and the time and items remaining. The model
 * and the drawing are SDE's own (liria-sde-transfer-model.h, a copy of
 * src/common/transfer_model.h), so Liria and the SDE desktop look and
 * estimate the same.
 */
#ifndef LIRIA_SDE_PROGRESS_H
#define LIRIA_SDE_PROGRESS_H

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "liria-progress-info.h"

typedef enum {
    LIRIA_SDE_TRANSFER_COPY,
    LIRIA_SDE_TRANSFER_MOVE,
    LIRIA_SDE_TRANSFER_DUPLICATE
} LiriaSdeTransferKind;

/* What a copy or a move reports, from the job's thread. begin () names the
 * folders (it may block, once); the others are cheap. */
void liria_sde_progress_transfer_begin (LiriaProgressInfo *info, LiriaSdeTransferKind kind, GList *files,
                                        GFile *destination);
void liria_sde_progress_transfer_counts (LiriaProgressInfo *info, goffset bytes_done, goffset bytes_total,
                                         int files_done, int files_total);
void liria_sde_progress_transfer_item (LiriaProgressInfo *info, GFile *file);
void liria_sde_progress_transfer_finishing (LiriaProgressInfo *info);
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
/* The file operations window. */
void liria_sde_progress_window_setup (GtkWidget *window);

#endif
