/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Safe copies to removable and slow drives (added by SDE).
 *
 * Linux keeps written data in the page cache and writes it back later. A
 * copy to a USB stick therefore "finishes" long before the data is on the
 * stick: the progress bar is wrong, unmounting hangs for minutes and a
 * stick pulled after "done" loses files. A large cache of dirty pages for
 * one slow device can also stall the whole desktop.
 *
 * For destinations on removable devices (USB, SD/MMC) and on FAT, exFAT
 * and NTFS file systems, regular files are copied here instead of with
 * g_file_copy(): large buffers, sync_file_range() windows that keep at
 * most ~16 MiB of a file unwritten, progress that counts only data the
 * device accepted, a syncfs() after every 32 MiB / 3 s of small files and
 * a final syncfs() before the operation reports completion. Internal
 * disks keep the normal fast path.
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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gio/gunixmounts.h>

#include "liria-sde-copy.h"

#define BUFFER_SIZE (4 * 1024 * 1024)
#define WINDOW_SIZE (8 * 1024 * 1024)
#define BATCH_BYTES (32 * 1024 * 1024)
#define BATCH_USEC (3 * G_USEC_PER_SEC)

struct LiriaSdeFlush {
	char *fs_type;
	char *name;
	int dir_fd;
	gboolean dos_names;
	gboolean limit_4g;
	guchar *buffer;
	goffset unsynced;
	gint64 last_sync;
};

static gboolean
read_sys_flag (const char *dir, const char *name)
{
	char *file = g_build_filename (dir, name, NULL);
	char *text = NULL;
	gboolean set = FALSE;

	if (g_file_get_contents (file, &text, NULL, NULL))
		set = g_ascii_strtoll (text, NULL, 10) != 0;
	g_free (text);
	g_free (file);
	return set;
}

/* TRUE for a block device on USB, MMC/SD or one marked removable. */
static gboolean
device_is_removable (const char *device)
{
	char *name, *sys, *real;
	gboolean removable = FALSE;

	if (device == NULL || !g_str_has_prefix (device, "/dev/"))
		return FALSE;
	real = realpath (device, NULL);
	name = g_path_get_basename (real ? real : device);
	free (real);
	sys = g_build_filename ("/sys/class/block", name, NULL);
	real = realpath (sys, NULL);
	if (real != NULL) {
		char *partition = g_build_filename (real, "partition", NULL);
		char *disk = g_file_test (partition, G_FILE_TEST_EXISTS) ? g_path_get_dirname (real) : g_strdup (real);

		g_free (partition);
		removable = strstr (real, "/usb") != NULL || strstr (real, "/mmc") != NULL ||
		            read_sys_flag (disk, "removable");
		g_free (disk);
		free (real);
	}
	g_free (sys);
	g_free (name);
	return removable;
}

static gboolean
type_is (const char *type, const char *const *names)
{
	for (; *names; names++)
		if (g_strcmp0 (type, *names) == 0)
			return TRUE;
	return FALSE;
}

LiriaSdeFlush *
liria_sde_flush_new (GFile *destination_dir)
{
	static const char *const fat[] = { "vfat", "msdos", "fat", "msdosfs", "fuse.fusefat", "fusefat", NULL };
	static const char *const dos[] = { "exfat", "fuse.exfat", "ntfs", "ntfs3", "fuseblk", "fuse.ntfs-3g", NULL };
	static const char *const slow[] = { "udf", "hfsplus", NULL };
	const char *mode = g_getenv ("SDE_LIRIA_SAFE_COPY");
	GUnixMountEntry *entry;
	LiriaSdeFlush *flush;
	const char *type = NULL;
	char *path;
	gboolean is_fat, is_dos, removable = FALSE;

	if (g_strcmp0 (mode, "never") == 0 || destination_dir == NULL || !g_file_is_native (destination_dir))
		return NULL;
	path = g_file_get_path (destination_dir);
	if (path == NULL)
		return NULL;
	entry = g_unix_mount_for (path, NULL);
	if (entry != NULL) {
		type = g_unix_mount_get_fs_type (entry);
		removable = device_is_removable (g_unix_mount_get_device_path (entry));
	}
	is_fat = type_is (type, fat);
	is_dos = is_fat || type_is (type, dos);
	if (!(removable || is_dos || type_is (type, slow) || g_strcmp0 (mode, "always") == 0)) {
		if (entry != NULL)
			g_unix_mount_free (entry);
		g_free (path);
		return NULL;
	}

	flush = g_new0 (LiriaSdeFlush, 1);
	flush->fs_type = g_strdup (type ? type : "");
	flush->dos_names = is_dos;
	flush->limit_4g = is_fat;
	flush->dir_fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	flush->last_sync = g_get_monotonic_time ();
	if (entry != NULL) {
		const char *mount_path = g_unix_mount_get_mount_path (entry);
		flush->name = g_filename_display_basename (mount_path && strcmp (mount_path, "/") ? mount_path : path);
		g_unix_mount_free (entry);
	} else {
		flush->name = g_filename_display_basename (path);
	}
	g_free (path);
	return flush;
}

void
liria_sde_flush_free (LiriaSdeFlush *flush)
{
	if (flush == NULL)
		return;
	if (flush->dir_fd >= 0)
		close (flush->dir_fd);
	g_free (flush->buffer);
	g_free (flush->fs_type);
	g_free (flush->name);
	g_free (flush);
}

const char *
liria_sde_flush_get_fs_type (LiriaSdeFlush *flush)
{
	return flush->fs_type;
}

const char *
liria_sde_flush_get_name (LiriaSdeFlush *flush)
{
	return flush->name;
}

gboolean
liria_sde_flush_needs_dos_names (LiriaSdeFlush *flush)
{
	return flush->dos_names;
}

gboolean
liria_sde_flush_has_4g_limit (LiriaSdeFlush *flush)
{
	return flush->limit_4g;
}

gboolean
liria_sde_flush_too_large (LiriaSdeFlush *flush, GFile *file)
{
	struct stat st;
	char *path;
	gboolean large = FALSE;

	if (flush == NULL || !flush->limit_4g || !g_file_is_native (file))
		return FALSE;
	path = g_file_get_path (file);
	if (path && lstat (path, &st) == 0 && S_ISREG (st.st_mode) &&
	    (guint64) st.st_size > LIRIA_SDE_FAT_MAX_FILE_SIZE)
		large = TRUE;
	g_free (path);
	return large;
}

gboolean
liria_sde_sync_path (const char *path)
{
	int fd = open (path, O_RDONLY | O_CLOEXEC);
	int r;

	if (fd < 0)
		return FALSE;
	r = syncfs (fd);
	close (fd);
	return r == 0;
}

gboolean
liria_sde_flush_sync (LiriaSdeFlush *flush)
{
	int r = 0;

	if (flush->dir_fd >= 0)
		r = syncfs (flush->dir_fd);
	else
		sync ();
	flush->unsynced = 0;
	flush->last_sync = g_get_monotonic_time ();
	return r == 0;
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

/* Starts writeback of [offset, offset + length) and waits for it. */
static gboolean
write_back (int fd, goffset offset, goffset length, gboolean wait)
{
	unsigned int flags = SYNC_FILE_RANGE_WRITE;

	if (wait)
		flags = SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER;
	return sync_file_range (fd, offset, length, flags) == 0 || errno == ESPIPE || errno == EINVAL ||
	       errno == ENOSYS;
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
	struct stat st, dst;
	int in = -1, out = -1, open_flags;
	goffset written = 0, confirmed = 0, window_start = 0, previous_start = -1, previous_length = 0;
	gboolean ok = FALSE, created = FALSE;

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
	open_flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
	if ((flags & G_FILE_COPY_OVERWRITE) && lstat (dest_path, &dst) == 0) {
		if (S_ISDIR (dst.st_mode)) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
			                     _("Can’t copy over directory"));
			goto out;
		}
		if (dst.st_dev == st.st_dev && dst.st_ino == st.st_ino) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file exists"));
			goto out;
		}
		unlink (dest_path);
	}
	out = open (dest_path, open_flags, 0666);
	if (out < 0) {
		int code = errno;

		if (code == EEXIST)
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file exists"));
		else if (code == EINVAL || code == EILSEQ)
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
			             _("Invalid filename %s"), dest_path);
		else
			set_errno_error (error, code, _("Error opening file “%s”: %s"), dest_path);
		goto out;
	}
	created = TRUE;
	posix_fadvise (in, 0, 0, POSIX_FADV_SEQUENTIAL);
	if (flush->buffer == NULL)
		flush->buffer = g_malloc (BUFFER_SIZE);

	for (;;) {
		ssize_t n = read (in, flush->buffer, BUFFER_SIZE);
		ssize_t done = 0;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			set_errno_error (error, errno, _("Error reading from file “%s”: %s"), src_path);
			goto out;
		}
		if (n == 0)
			break;
		while (done < n) {
			ssize_t w = write (out, flush->buffer + done, n - done);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				set_errno_error (error, errno, _("Error writing to file “%s”: %s"), dest_path);
				goto out;
			}
			done += w;
		}
		written += n;
		if (written - window_start >= WINDOW_SIZE) {
			/* Start writing this window; wait for the one before. */
			write_back (out, window_start, written - window_start, FALSE);
			if (previous_start >= 0) {
				if (!write_back (out, previous_start, previous_length, TRUE)) {
					set_errno_error (error, errno, _("Error writing to file “%s”: %s"), dest_path);
					goto out;
				}
				confirmed = previous_start + previous_length;
				if (progress_callback)
					progress_callback (confirmed, st.st_size, progress_data);
			}
			previous_start = window_start;
			previous_length = written - window_start;
			window_start = written;
		}
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			goto out;
	}

	if (written > window_start)
		write_back (out, window_start, written - window_start, FALSE);
	if (is_move) {
		/* The source is deleted below: the copy must be on the device. */
		if (fdatasync (out) != 0 && errno != EINVAL) {
			set_errno_error (error, errno, _("Error writing to file “%s”: %s"), dest_path);
			goto out;
		}
	} else {
		flush->unsynced += written - confirmed;
	}
	if (close (out) != 0) {
		out = -1;
		set_errno_error (error, errno, _("Error closing file “%s”: %s"), dest_path);
		goto out;
	}
	out = -1;
	if (!is_move && (flush->unsynced >= BATCH_BYTES ||
	                 (flush->unsynced > 0 && g_get_monotonic_time () - flush->last_sync >= BATCH_USEC)))
		liria_sde_flush_sync (flush);
	if (progress_callback)
		progress_callback (written, st.st_size, progress_data);

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
	g_free (src_path);
	g_free (dest_path);
	return ok;
}
