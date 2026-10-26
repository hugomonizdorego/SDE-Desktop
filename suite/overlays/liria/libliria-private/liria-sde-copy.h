/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Safe copies to removable, external and network drives (added by SDE, see
 * suite/redesign.py). Used by liria-file-operations.c. The writer itself
 * is SDE's own src/common/safe_copy.h, copied in as liria-sde-safe-copy.h
 * and shared with the SDE desktop.
 */
#ifndef LIRIA_SDE_COPY_H
#define LIRIA_SDE_COPY_H

#include <gio/gio.h>

/* Largest file a FAT12/16/32 file system can hold: 4 GiB - 1 byte. */
#define LIRIA_SDE_FAT_MAX_FILE_SIZE G_GUINT64_CONSTANT (4294967295)

typedef struct LiriaSdeFlush LiriaSdeFlush;

/* A writer for one destination, or NULL when the destination is an
 * internal disk (or not a local path) and the page cache is used: removable
 * and external drives (USB, SD cards, FireWire, Thunderbolt, also under
 * LUKS or LVM), network shares and FAT, exFAT, NTFS, UDF and HFS+ file
 * systems; sde.conf [files] safe_copy = auto | always | never and
 * SDE_SAFE_COPY change it. */
LiriaSdeFlush *liria_sde_flush_new          (GFile *destination_dir);
void           liria_sde_flush_free         (LiriaSdeFlush *flush);

/* File system type ("vfat", "exfat", "fuseblk", ...). */
const char    *liria_sde_flush_get_fs_type  (LiriaSdeFlush *flush);
/* TRUE when file names must avoid  : * ? " < > | \  (FAT, exFAT, NTFS). */
gboolean       liria_sde_flush_needs_dos_names (LiriaSdeFlush *flush);
/* TRUE when files of 4 GiB or more cannot be stored (FAT). */
gboolean       liria_sde_flush_has_4g_limit (LiriaSdeFlush *flush);
/* TRUE when @file is a regular file too large for this destination. */
gboolean       liria_sde_flush_too_large    (LiriaSdeFlush *flush, GFile *file);
/* Display name of the drive ("SANDISK"), for "Writing ... to SANDISK". */
const char    *liria_sde_flush_get_name     (LiriaSdeFlush *flush);
/* TRUE when the user may unplug the drive once the operation is done (not
 * a network share). */
gboolean       liria_sde_flush_is_removable (LiriaSdeFlush *flush);

/* What the writer reports while it copies: |reported| gets the bytes the
 * drive has taken (from any file, also those still in flight when their
 * copy returned), |tick| runs between reads (pausing, the QA copy rate)
 * with the bytes written since the last tick and returns TRUE to stop. */
void           liria_sde_flush_set_reporter (LiriaSdeFlush *flush,
                                             void (*reported) (gpointer data, goffset bytes),
                                             gboolean (*tick) (gpointer data, goffset written),
                                             gpointer data);

/* Copies (or moves) one regular file through the writer: the progress
 * counts only data the drive has taken (through the reporter, not
 * @progress_callback), holes stay holes, a cancelled or failed copy is
 * removed. A move deletes the source only after a checkpoint made the copy
 * durable (every few seconds and at the end). *handled is FALSE when the
 * source is not a local regular file: the caller then uses
 * g_file_copy/g_file_move. */
gboolean       liria_sde_flush_copy_file    (LiriaSdeFlush *flush,
                                             GFile *source,
                                             GFile *destination,
                                             GFileCopyFlags flags,
                                             gboolean is_move,
                                             GCancellable *cancellable,
                                             GFileProgressCallback progress_callback,
                                             gpointer progress_data,
                                             gboolean *handled,
                                             GError **error);

/* A moved folder whose files wait for a checkpoint before they are
 * deleted waits too: TRUE when @folder was put in the queue (it is
 * deleted after its files), FALSE when the caller deletes it now. */
gboolean       liria_sde_flush_defer_delete (LiriaSdeFlush *flush, GFile *folder);

/* The end of the operation: what is in flight on the drive (the progress
 * goes on through the reporter), syncfs () and fsync () of the folder, then
 * the sources of a move are deleted. FALSE with @error when the drive
 * failed (the sources are kept then). Blocks; call it from the job's
 * thread. */
gboolean       liria_sde_flush_finish       (LiriaSdeFlush *flush, GError **error);
/* The number of moved items that could not be deleted from where they
 * were, and the first error (owned by @flush). */
int            liria_sde_flush_get_delete_failures (LiriaSdeFlush *flush, const char **first_error);

/* Large regular files (32 MiB and more) on internal disks, copied or moved
 * in slices of about a tenth of a second with the progress after each, so
 * the progress, the speed graph and Pause follow them (GLib 2.80 copies a
 * local file in one call and reports it at the end); shared data (reflink)
 * where the file system can. *handled is FALSE for other files: the
 * caller then uses g_file_copy/g_file_move. */
gboolean       liria_sde_copy_large_file    (GFile *source,
                                             GFile *destination,
                                             GFileCopyFlags flags,
                                             gboolean is_move,
                                             GCancellable *cancellable,
                                             GFileProgressCallback progress_callback,
                                             gpointer progress_data,
                                             gboolean *handled,
                                             GError **error);

/* Everything written so far to the drive is durable (a checkpoint). */
gboolean       liria_sde_flush_sync         (LiriaSdeFlush *flush);

/* syncfs () and fsync () for the file system that holds @path; blocks. */
gboolean       liria_sde_sync_path          (const char *path);

/* After a deletion: the removable, external and network drives that held
 * @files (GFile list) get it written; @announce is called with each
 * drive's name before its sync. */
void           liria_sde_sync_drives_of     (GList *files,
                                             void (*announce) (gpointer data, const char *name),
                                             gpointer data);
/* After emptying the trash: the drives with a trash folder of this user. */
void           liria_sde_sync_trash_drives  (void (*announce) (gpointer data, const char *name),
                                             gpointer data);

#endif
