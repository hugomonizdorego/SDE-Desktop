/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Safe copies to removable, external and network drives (added by SDE).
 *
 * Linux keeps written data in the page cache and writes it back later. A
 * copy to a USB stick therefore "finished" long before the data was on the
 * stick: the progress bar was wrong, unmounting hung for minutes and a
 * stick pulled after "done" lost files. A large cache of dirty pages for
 * one slow device can also stall the whole desktop.
 *
 * Regular files copied to such a drive (liria_sde_flush_new () decides,
 * with SDE's sde_dest_probe ()) are written here instead of with
 * g_file_copy (), through SDE's writer (liria-sde-safe-copy.h, a copy of
 * src/common/safe_copy.h shared with the SDE desktop): at most about a
 * second of the drive's speed is in flight, the progress counts only data
 * the drive has taken, checkpoints (syncfs () and fsync () of the folder)
 * every few seconds and at the end make it durable, and the operation is
 * reported done only after the last one (0.2.0 Alpha 3). A move deletes
 * its sources only after a checkpoint, so a stick pulled during a move
 * never loses a file. The metadata (permissions, times, extended
 * attributes, SELinux labels) is copied by g_file_copy_attributes () as
 * GIO does; symbolic links, folders and special files keep GIO's path.
 * Internal disks keep the page cache, except that large files are copied
 * in slices there (liria_sde_copy_large_file (), 0.2.0 Alpha 2) so that
 * the progress and Pause follow them.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gio/gunixmounts.h>

#include "liria-sde-copy.h"
#include "liria-sde-safe-copy.h"

#define BUFFER_SIZE (1024 * 1024)
#define DEFERRED_MAX 2000
#define LARGE_FILE (32 * 1024 * 1024)
#define SLICE_MIN (1024 * 1024)
#define SLICE_MAX (64 * 1024 * 1024)

struct LiriaSdeFlush {
	SdeDest dest;
	SdeWriter writer;
	char *name;
	gboolean removable;
	guchar *buffer;
	/* Sources of a move, deleted after the next checkpoint (files first,
	 * then their folders). */
	GPtrArray *deferred;
	GHashTable *source_dirs;   /* folders that lost items (paths) */
	int delete_failures;
	char *first_delete_error;
	void (*reported) (gpointer data, goffset bytes);
	gboolean (*tick) (gpointer data, goffset written);
	gpointer report_data;
	goffset ticked;
};

/* sde.conf [files] safe_copy: the user's file, then the system's. */
static char *
safe_copy_setting (void)
{
	const gchar *const *system = g_get_system_config_dirs ();
	char *value = NULL;
	int i;

	for (i = -1; value == NULL && (i < 0 || system[i] != NULL); i++) {
		char *path = g_build_filename (i < 0 ? g_get_user_config_dir () : system[i], "sde", "sde.conf", NULL);
		GKeyFile *file = g_key_file_new ();

		if (g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL))
			value = g_key_file_get_string (file, "files", "safe_copy", NULL);
		g_key_file_unref (file);
		g_free (path);
	}
	if (value != NULL)
		g_strstrip (value);
	return value;
}

static void
flush_confirmed (void *data, long long bytes)
{
	LiriaSdeFlush *flush = data;

	if (flush->reported != NULL)
		flush->reported (flush->report_data, (goffset) bytes);
}

LiriaSdeFlush *
liria_sde_flush_new (GFile *destination_dir)
{
	LiriaSdeFlush *flush;
	SdeDest dest;
	char *path, *setting;
	SdeSafeMode mode;
	int fd;

	if (destination_dir == NULL || !g_file_is_native (destination_dir))
		return NULL;
	path = g_file_get_path (destination_dir);
	if (path == NULL)
		return NULL;
	setting = safe_copy_setting ();
	mode = sde_safe_mode (setting);
	g_free (setting);
	if (sde_dest_probe (path, &dest) != 0 || !sde_dest_wants_safe (&dest, mode)) {
		g_free (path);
		return NULL;
	}

	flush = g_new0 (LiriaSdeFlush, 1);
	flush->dest = dest;
	flush->removable = dest.kind == SDE_DEST_REMOVABLE || (dest.kind != SDE_DEST_NETWORK && dest.dos_names);
	flush->name = dest.name[0] != '\0' ? g_filename_display_name (dest.name) : g_filename_display_basename (path);
	fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	sde_writer_init (&flush->writer, sde_writer_method_for (&dest, 1), fd, NULL, NULL);
	flush->writer.confirmed_cb = flush_confirmed;
	flush->writer.confirmed_data = flush;
	flush->deferred = g_ptr_array_new_with_free_func (g_free);
	flush->source_dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_free (path);
	return flush;
}

void
liria_sde_flush_free (LiriaSdeFlush *flush)
{
	if (flush == NULL)
		return;
	sde_writer_abandon (&flush->writer);
	g_ptr_array_unref (flush->deferred);
	g_hash_table_unref (flush->source_dirs);
	g_free (flush->buffer);
	g_free (flush->name);
	g_free (flush->first_delete_error);
	g_free (flush);
}

const char *
liria_sde_flush_get_fs_type (LiriaSdeFlush *flush)
{
	return flush->dest.fs_type;
}

const char *
liria_sde_flush_get_name (LiriaSdeFlush *flush)
{
	return flush->name;
}

gboolean
liria_sde_flush_is_removable (LiriaSdeFlush *flush)
{
	return flush->removable;
}

gboolean
liria_sde_flush_needs_dos_names (LiriaSdeFlush *flush)
{
	return flush->dest.dos_names;
}

gboolean
liria_sde_flush_has_4g_limit (LiriaSdeFlush *flush)
{
	return flush->dest.fat;
}

gboolean
liria_sde_flush_too_large (LiriaSdeFlush *flush, GFile *file)
{
	struct stat st;
	char *path;
	gboolean large = FALSE;

	if (flush == NULL || !flush->dest.fat || !g_file_is_native (file))
		return FALSE;
	path = g_file_get_path (file);
	if (path && lstat (path, &st) == 0 && S_ISREG (st.st_mode) &&
	    (guint64) st.st_size > LIRIA_SDE_FAT_MAX_FILE_SIZE)
		large = TRUE;
	g_free (path);
	return large;
}

void
liria_sde_flush_set_reporter (LiriaSdeFlush *flush,
                              void (*reported) (gpointer data, goffset bytes),
                              gboolean (*tick) (gpointer data, goffset written),
                              gpointer data)
{
	flush->reported = reported;
	flush->tick = tick;
	flush->report_data = data;
	flush->ticked = flush->writer.written;
}

gboolean
liria_sde_sync_path (const char *path)
{
	int fd = open (path, O_RDONLY | O_CLOEXEC);
	int r;

	if (fd < 0)
		return FALSE;
	r = syncfs (fd);
	fsync (fd);
	close (fd);
	return r == 0;
}

/* The sources that waited for the checkpoint go now; the drives they were
 * on lose them durably too when those are removable. */
static void
delete_deferred (LiriaSdeFlush *flush)
{
	GHashTableIter iter;
	gpointer key;
	guint i;

	for (i = 0; i < flush->deferred->len; i++) {
		const char *path = g_ptr_array_index (flush->deferred, i);
		struct stat st;
		int r;

		if (lstat (path, &st) != 0)
			continue;
		r = S_ISDIR (st.st_mode) ? rmdir (path) : unlink (path);
		if (r != 0) {
			int code = errno;

			flush->delete_failures++;
			if (flush->first_delete_error == NULL) {
				char *display = g_filename_display_name (path);

				flush->first_delete_error = g_strdup_printf ("%s: %s", display, g_strerror (code));
				g_free (display);
			}
		} else {
			char *dir = g_path_get_dirname (path);

			g_hash_table_add (flush->source_dirs, dir);
		}
	}
	g_ptr_array_set_size (flush->deferred, 0);
	if (g_hash_table_size (flush->source_dirs) > 0) {
		char *setting = safe_copy_setting ();
		SdeSafeMode mode = sde_safe_mode (setting);

		g_free (setting);
		g_hash_table_iter_init (&iter, flush->source_dirs);
		while (g_hash_table_iter_next (&iter, &key, NULL)) {
			SdeDest dest;

			if (sde_dest_probe (key, &dest) == 0 &&
			    (dest.major != flush->dest.major || dest.minor != flush->dest.minor) &&
			    sde_dest_wants_safe (&dest, mode))
				liria_sde_sync_path (key);
		}
		g_hash_table_remove_all (flush->source_dirs);
	}
}

gboolean
liria_sde_flush_sync (LiriaSdeFlush *flush)
{
	gboolean ok = sde_writer_checkpoint (&flush->writer) == 0;

	if (ok)
		delete_deferred (flush);
	return ok;
}

gboolean
liria_sde_flush_defer_delete (LiriaSdeFlush *flush, GFile *folder)
{
	char *path;

	if (flush->deferred->len == 0)
		return FALSE;
	path = g_file_get_path (folder);
	if (path == NULL)
		return FALSE;
	g_ptr_array_add (flush->deferred, path);
	return TRUE;
}

static void
set_drive_error (LiriaSdeFlush *flush, GError **error, int code)
{
	if (code == ENOSPC || code == EDQUOT)
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, _("There is not enough space on %s."), flush->name);
	else if (code == EROFS)
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_READ_ONLY, _("%s is read-only."), flush->name);
	else if ((code == EIO || code == ENODEV || code == ENXIO) && flush->removable)
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             _("Could not write to %s: the drive was removed or does not work."), flush->name);
	else
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (code), _("Could not write to %s: %s"),
		             flush->name, g_strerror (code));
}

gboolean
liria_sde_flush_finish (LiriaSdeFlush *flush, GError **error)
{
	int code = sde_writer_finish (&flush->writer);

	if (code != 0) {
		/* The copies may not be complete: the sources stay. */
		g_ptr_array_set_size (flush->deferred, 0);
		set_drive_error (flush, error, code);
		return FALSE;
	}
	delete_deferred (flush);
	return TRUE;
}

int
liria_sde_flush_get_delete_failures (LiriaSdeFlush *flush, const char **first_error)
{
	if (first_error != NULL)
		*first_error = flush->first_delete_error;
	return flush->delete_failures;
}

static void
set_errno_error (GError **error, int code, const char *format, const char *name)
{
	GIOErrorEnum io = g_io_error_from_errno (code);
	char *display = g_filename_display_name (name);

	if (code == EFBIG)
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             _("The file is too large for the file system of the destination."));
	else
		g_set_error (error, G_IO_ERROR, io, format, display, g_strerror (code));
	g_free (display);
}

/* Creates the destination file the way g_file_copy () does: an existing
 * file is an error unless G_FILE_COPY_OVERWRITE, and never a folder or the
 * source itself. Returns -1 with |error| set. */
static int
open_destination (const struct stat *st, const char *dest_path, GFileCopyFlags flags, GError **error)
{
	struct stat dst;
	int out;

	if ((flags & G_FILE_COPY_OVERWRITE) && lstat (dest_path, &dst) == 0) {
		if (S_ISDIR (dst.st_mode)) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
			                     _("Can’t copy over directory"));
			return -1;
		}
		if (dst.st_dev == st->st_dev && dst.st_ino == st->st_ino) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file exists"));
			return -1;
		}
		unlink (dest_path);
	}
	out = open (dest_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (out < 0) {
		int code = errno;

		if (code == EEXIST)
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file exists"));
		else if (code == EINVAL || code == EILSEQ)
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
			             _("Invalid filename %s"), dest_path);
		else
			set_errno_error (error, code, _("Error opening file “%s”: %s"), dest_path);
	}
	return out;
}

typedef struct {
	LiriaSdeFlush *flush;
	GCancellable *cancellable;
} StopData;

/* Between reads: Pause and the QA copy rate (the reporter's tick), Cancel. */
static int
copy_stop (void *data)
{
	StopData *stop = data;
	LiriaSdeFlush *flush = stop->flush;

	if (flush->tick != NULL) {
		goffset written = flush->writer.written - flush->ticked;

		flush->ticked = flush->writer.written;
		if (flush->tick (flush->report_data, written))
			return 1;
	}
	return g_cancellable_is_cancelled (stop->cancellable) ? 1 : 0;
}

/* After a checkpoint the sources that waited for it go. */
static int
file_done (LiriaSdeFlush *flush)
{
	double before = flush->writer.last_checkpoint;
	int code;

	if (flush->deferred->len >= DEFERRED_MAX)
		code = sde_writer_checkpoint (&flush->writer);
	else
		code = sde_writer_file_done (&flush->writer);
	if (code == 0 && flush->writer.last_checkpoint != before)
		delete_deferred (flush);
	return code;
}

gboolean
liria_sde_flush_copy_file (LiriaSdeFlush *flush,
                           GFile *source,
                           GFile *destination,
                           GFileCopyFlags flags,
                           gboolean is_move,
                           GCancellable *cancellable,
                           GFileProgressCallback progress_callback,
                           gpointer progress_data,
                           gboolean *handled,
                           GError **error)
{
	char *src_path = NULL, *dest_path = NULL;
	struct stat st;
	int in = -1, out = -1, code, source_error = 0;
	gboolean ok = FALSE, created = FALSE;
	StopData stop = { flush, cancellable };
	SdeCopyHooks hooks;

	*handled = FALSE;
	if (!g_file_is_native (source) || !g_file_is_native (destination))
		return FALSE;
	src_path = g_file_get_path (source);
	dest_path = g_file_get_path (destination);
	if (src_path == NULL || dest_path == NULL || lstat (src_path, &st) != 0 || !S_ISREG (st.st_mode))
		goto out;
	*handled = TRUE;

	in = open (src_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (in < 0) {
		set_errno_error (error, errno, _("Error opening file “%s”: %s"), src_path);
		goto out;
	}
	out = open_destination (&st, dest_path, flags, error);
	if (out < 0)
		goto out;
	created = TRUE;
	posix_fadvise (in, 0, 0, POSIX_FADV_SEQUENTIAL);
	if (flush->buffer == NULL)
		flush->buffer = g_malloc (BUFFER_SIZE);
	hooks.stop = copy_stop;
	hooks.data = &stop;
	code = sde_copy_data (&flush->writer, in, out, st.st_size, flush->buffer, BUFFER_SIZE, &hooks, &source_error);
	if (code == ECANCELED) {
		g_cancellable_set_error_if_cancelled (cancellable, error);
		if (error != NULL && *error == NULL)
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, _("Operation was cancelled"));
		goto out;
	}
	if (code != 0 && source_error != 0) {
		set_errno_error (error, code, _("Error reading from file “%s”: %s"), src_path);
		goto out;
	}
	if (code == EFBIG) {
		set_errno_error (error, code, NULL, dest_path);
		goto out;
	}
	if (code != 0) {
		set_drive_error (flush, error, code);
		goto out;
	}
	if (close (out) != 0) {
		out = -1;
		set_drive_error (flush, error, errno);
		goto out;
	}
	out = -1;
	if (is_move) {
		/* What g_file_move () keeps: permissions, times, extended
		 * attributes and SELinux labels (a copy gets them from
		 * copy_move_file ()). The source is deleted after the next
		 * checkpoint (file_done (), the end). */
		g_file_copy_attributes (source, destination, flags | G_FILE_COPY_ALL_METADATA, cancellable, NULL);
		g_ptr_array_add (flush->deferred, src_path);
		src_path = NULL;
	}
	code = file_done (flush);
	if (code != 0) {
		set_drive_error (flush, error, code);
		created = FALSE; /* complete, but the drive failed: keep what is there */
		goto out;
	}
	(void) progress_callback;
	(void) progress_data;
	ok = TRUE;

out:
	if (in >= 0)
		close (in);
	if (out >= 0)
		close (out);
	if (!ok && created && dest_path)
		unlink (dest_path);
	g_free (src_path);
	g_free (dest_path);
	return ok;
}

/* The drives (removable, external, network) of the folders of |files|,
 * each once. */
static GPtrArray *
drives_of (GList *files)
{
	GPtrArray *drives = g_ptr_array_new_with_free_func (g_free);
	char *setting = safe_copy_setting ();
	SdeSafeMode mode = sde_safe_mode (setting);
	GList *l;

	g_free (setting);
	for (l = files; l != NULL; l = l->next) {
		GFile *parent = G_IS_FILE (l->data) ? g_file_get_parent (l->data) : NULL;
		char *path = parent != NULL ? g_file_get_path (parent) : NULL;
		SdeDest *dest = g_new0 (SdeDest, 1);
		guint i;
		gboolean known = FALSE;

		if (path != NULL && sde_dest_probe (path, dest) == 0 && sde_dest_wants_safe (dest, mode)) {
			for (i = 0; i < drives->len && !known; i++)
				known = strcmp (((SdeDest *) g_ptr_array_index (drives, i))->mount_point, dest->mount_point) == 0;
			if (!known) {
				g_ptr_array_add (drives, dest);
				dest = NULL;
			}
		}
		g_free (dest);
		g_free (path);
		g_clear_object (&parent);
	}
	return drives;
}

static void
sync_drives (GPtrArray *drives, void (*announce) (gpointer data, const char *name), gpointer data)
{
	guint i;

	for (i = 0; i < drives->len; i++) {
		SdeDest *dest = g_ptr_array_index (drives, i);
		char *name = dest->name[0] != '\0' ? g_filename_display_name (dest->name)
		                                   : g_filename_display_basename (dest->mount_point);

		if (announce != NULL)
			announce (data, name);
		liria_sde_sync_path (dest->mount_point);
		g_free (name);
	}
}

void
liria_sde_sync_drives_of (GList *files, void (*announce) (gpointer data, const char *name), gpointer data)
{
	GPtrArray *drives = drives_of (files);

	sync_drives (drives, announce, data);
	g_ptr_array_unref (drives);
}

void
liria_sde_sync_trash_drives (void (*announce) (gpointer data, const char *name), gpointer data)
{
	GList *mounts = g_unix_mounts_get (NULL), *l, *roots = NULL;
	char *trash = g_strdup_printf (".Trash-%u", (unsigned int) getuid ());
	GPtrArray *drives;

	for (l = mounts; l != NULL; l = l->next) {
		const char *root = g_unix_mount_get_mount_path (l->data);
		char *dir = g_build_filename (root, trash, NULL);

		/* drives_of () looks at the folder of each file: the trash
		 * folder's own. */
		if (g_file_test (dir, G_FILE_TEST_IS_DIR))
			roots = g_list_prepend (roots, g_file_new_build_filename (dir, "files", NULL));
		g_free (dir);
	}
	drives = drives_of (roots);
	sync_drives (drives, announce, data);
	g_ptr_array_unref (drives);
	g_list_free_full (roots, g_object_unref);
	g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
	g_free (trash);
}

gboolean
liria_sde_copy_large_file (GFile *source,
                           GFile *destination,
                           GFileCopyFlags flags,
                           gboolean is_move,
                           GCancellable *cancellable,
                           GFileProgressCallback progress_callback,
                           gpointer progress_data,
                           gboolean *handled,
                           GError **error)
{
	char *src_path = NULL, *dest_path = NULL;
	struct stat st;
	int in = -1, out = -1;
	goffset done = 0, slice = 4 * 1024 * 1024;
	gboolean ok = FALSE, created = FALSE, kernel = TRUE;
	guchar *buffer = NULL;
	gint64 last;

	*handled = FALSE;
	if (!g_file_is_native (source) || !g_file_is_native (destination) ||
	    g_strcmp0 (g_getenv ("SDE_LIRIA_SAFE_COPY"), "never") == 0)
		return FALSE;
	src_path = g_file_get_path (source);
	dest_path = g_file_get_path (destination);
	if (src_path == NULL || dest_path == NULL || lstat (src_path, &st) != 0 || !S_ISREG (st.st_mode) ||
	    st.st_size < LARGE_FILE)
		goto out;
	*handled = TRUE;

	in = open (src_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (in < 0) {
		set_errno_error (error, errno, _("Error opening file “%s”: %s"), src_path);
		goto out;
	}
	out = open_destination (&st, dest_path, flags, error);
	if (out < 0)
		goto out;
	created = TRUE;
#ifdef FICLONE
	/* A file system that shares data (Btrfs, XFS) has the copy at once,
	 * as with g_file_copy (). */
	if (ioctl (out, FICLONE, in) == 0) {
		done = st.st_size;
		if (progress_callback)
			progress_callback (done, st.st_size, progress_data);
		goto copied;
	}
#endif
	posix_fadvise (in, 0, 0, POSIX_FADV_SEQUENTIAL);
	last = g_get_monotonic_time ();
	for (;;) {
		ssize_t n;
		gint64 now;

		if (kernel) {
			n = copy_file_range (in, NULL, out, NULL, (size_t) slice, 0);
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && done == 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS ||
			                           errno == EOPNOTSUPP || errno == EBADF || errno == EPERM)) {
				/* Not between these file systems: read and write. */
				kernel = FALSE;
				continue;
			}
			if (n < 0) {
				set_errno_error (error, errno, _("Error writing to file “%s”: %s"), dest_path);
				goto out;
			}
		} else {
			ssize_t written = 0;

			if (buffer == NULL)
				buffer = g_malloc (BUFFER_SIZE);
			n = read (in, buffer, (size_t) MIN (slice, BUFFER_SIZE));
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0) {
				set_errno_error (error, errno, _("Error reading from file “%s”: %s"), src_path);
				goto out;
			}
			while (written < n) {
				ssize_t w = write (out, buffer + written, n - written);

				if (w < 0) {
					if (errno == EINTR)
						continue;
					set_errno_error (error, errno, _("Error writing to file “%s”: %s"), dest_path);
					goto out;
				}
				written += w;
			}
		}
		if (n == 0)
			break;
		done += n;
		if (progress_callback)
			progress_callback (done, st.st_size, progress_data);
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			goto out;
		/* About ten reports a second, whatever the speed of the disks (the
		 * time includes a pause in the progress callback). */
		now = g_get_monotonic_time ();
		if (now - last < G_USEC_PER_SEC / 20 && slice < SLICE_MAX)
			slice *= 2;
		else if (now - last > G_USEC_PER_SEC / 4 && slice > SLICE_MIN)
			slice /= 2;
		last = now;
	}

#ifdef FICLONE
copied:
#endif
	if (close (out) != 0) {
		out = -1;
		set_errno_error (error, errno, _("Error closing file “%s”: %s"), dest_path);
		goto out;
	}
	out = -1;
	if (is_move) {
		g_file_copy_attributes (source, destination, flags | G_FILE_COPY_ALL_METADATA, cancellable, NULL);
		if (!g_file_delete (source, cancellable, error)) {
			created = FALSE; /* keep the complete copy */
			goto out;
		}
	}
	ok = TRUE;

out:
	if (in >= 0)
		close (in);
	if (out >= 0)
		close (out);
	if (!ok && created && dest_path)
		unlink (dest_path);
	g_free (buffer);
	g_free (src_path);
	g_free (dest_path);
	return ok;
}
