/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Safe copies to removable and slow drives (added by SDE, see
 * suite/redesign.py). Used by liria-file-operations.c.
 */
#ifndef LIRIA_SDE_COPY_H
#define LIRIA_SDE_COPY_H

#include <gio/gio.h>

/* Largest file a FAT12/16/32 file system can hold: 4 GiB - 1 byte. */
#define LIRIA_SDE_FAT_MAX_FILE_SIZE G_GUINT64_CONSTANT (4294967295)

typedef struct LiriaSdeFlush LiriaSdeFlush;

/* A writer for one destination, or NULL when the destination is an
 * internal disk (or not a local path) and the normal fast copy is used. */
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
/* Display name of the drive, for "Writing data to ..." messages. */
const char    *liria_sde_flush_get_name     (LiriaSdeFlush *flush);

/* Copies (or moves) one regular file with large buffers, keeps at most a
 * few MiB of unwritten data in the page cache and reports progress only
 * for data the device has accepted. *handled is FALSE when the source is
 * not a local regular file: the caller then uses g_file_copy/g_file_move.
 * A move deletes the source only after the copy reached the device. */
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

/* Writes everything still cached for the destination file system to the
 * device (syncfs). Blocks; call it from a job thread. */
gboolean       liria_sde_flush_sync         (LiriaSdeFlush *flush);

/* syncfs() for the file system that holds @path; blocks. */
gboolean       liria_sde_sync_path          (const char *path);

#endif
