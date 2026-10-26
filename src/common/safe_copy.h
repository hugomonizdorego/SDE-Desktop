/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Safe copying (0.2.0 Alpha 3): a copy to a USB stick is on the stick when
 * it says it is done. Shared by the SDE desktop's file operations
 * (src/desktop/file_ops.cpp) and Liria's (suite/redesign.py copies this
 * file into the Liria tree as libliria-private/liria-sde-safe-copy.h).
 * Plain C99 for Linux that also compiles as C++; GPL-2.0-or-later like
 * Liria, so the file can be part of both. docs/TRANSFERS.md has the design
 * and the measurements.
 *
 * Linux writes data into memory (the page cache) first and to the drive
 * later. A copy to a USB stick therefore "finishes" while most of it is
 * still in memory: the bar, the speed and the time left are wrong, Eject
 * then waits for minutes, and a stick pulled after "done" loses files. Here
 * a copy to such a drive goes through an SdeWriter that
 *
 *   - starts writing every piece to the drive as soon as it is written
 *     (sync_file_range (SYNC_FILE_RANGE_WRITE)) and keeps only a small
 *     window in flight: about one second of the drive's own speed, 4 to 64
 *     MiB, and at most SDE_WRITER_SLOTS pieces (small files are one piece
 *     each). With more in flight, or a piece in flight for longer than
 *     SDE_WRITER_LATE seconds, it waits for the oldest piece. Progress counts
 *     only pieces the drive has taken, so the bar, the speed and the time
 *     left follow the drive from the start, and the last flush is short;
 *   - makes everything durable at checkpoints, every SDE_WRITER_CHECKPOINT
 *     seconds between files and at the end: the window drained, syncfs ()
 *     (data and metadata of the file system: directory entries, the FAT,
 *     the journal) and fsync () of the destination folder (on FAT this also
 *     empties the drive's own write cache). An operation is done only
 *     after its last checkpoint; a move deletes its sources only after a
 *     checkpoint made their copies durable;
 *   - on file systems in user space (ntfs-3g and exFAT through FUSE keep the
 *     data in the cache of the block device, not of the file) and on
 *     network file systems confirms a window with fdatasync () instead;
 *   - writes the holes of sparse files as holes, and drops pieces the drive
 *     has taken from the page cache, so a large copy does not push the
 *     programs' own data out of memory.
 *
 * sde_dest_probe () tells what a destination is: an internal disk, a
 * removable or external drive (USB, SD card, FireWire, Thunderbolt, also
 * under LUKS, LVM or RAID) or a network share; the file system, the mount
 * point and a name for messages ("SANDISK"). sde_dest_wants_safe () is the
 * policy: removable, external and network destinations and FAT, exFAT,
 * NTFS, UDF and HFS+ file systems (sticks and external disks) use the safe
 * writer; internal disks keep the page cache (measured in docs/TRANSFERS.md:
 * the safe writer would make every copy on them wait for the disk, for no
 * gain in safety a user can see). sde.conf [files] safe_copy = auto |
 * always | never changes it, SDE_SAFE_COPY (the older SDE_LIRIA_SAFE_COPY
 * too) overrides both (sde_safe_mode ()).
 *
 * SdeWriterOps lets tests replace the system calls with a simulated slow
 * drive (tests/test_safe_copy.cpp).
 */
#ifndef SDE_SAFE_COPY_H
#define SDE_SAFE_COPY_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------- the destination */

typedef enum {
    SDE_DEST_INTERNAL,  /* a fixed disk of this computer */
    SDE_DEST_REMOVABLE, /* a USB stick, an SD card, an external disk */
    SDE_DEST_NETWORK    /* NFS, SMB, SSHFS, ... */
} SdeDestKind;

typedef struct {
    SdeDestKind kind;
    char fs_type[48];         /* "vfat", "ext4", "fuseblk" */
    char mount_point[1024];
    char source[512];         /* the mount's source: "/dev/sdb1", "//server/share" */
    char name[256];           /* for messages: the label, else the mount point's name; may be "" */
    unsigned int major, minor; /* of the mounted file system */
    int fat;                  /* FAT12/16/32: no file of 4 GiB or more */
    int dos_names;            /* FAT, exFAT, NTFS: no  : * ? " < > | \  in names */
    int user_space;           /* FUSE: ntfs-3g, exFAT FUSE, SSHFS, ... */
} SdeDest;

/* Largest file a FAT file system can hold: 4 GiB - 1 byte. */
#define SDE_FAT_MAX_FILE_SIZE 4294967295LL

static inline int
sde_safe_type_in (const char *type, const char *const *names)
{
    for (; *names != NULL; names++)
        if (strcmp (type, *names) == 0)
            return 1;
    return 0;
}

/* /proc/self/mountinfo writes a space as \040, a tab as \011 ... */
static inline void
sde_safe_unescape_octal (char *s)
{
    char *in = s, *out = s;

    while (*in != '\0')
    {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '3' && in[2] >= '0' && in[2] <= '7' && in[3] >= '0' &&
            in[3] <= '7')
        {
            *out++ = (char) (((in[1] - '0') << 6) | ((in[2] - '0') << 3) | (in[3] - '0'));
            in += 4;
        }
        else
        {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

/* udev writes a space in /dev/disk/by-label as \x20. */
static inline void
sde_safe_unescape_hex (char *s)
{
    char *in = s, *out = s;

    while (*in != '\0')
    {
        unsigned int value;

        if (in[0] == '\\' && in[1] == 'x' && sscanf (in + 2, "%2x", &value) == 1 && in[2] != '\0' && in[3] != '\0')
        {
            *out++ = (char) value;
            in += 4;
        }
        else
        {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

static inline void
sde_safe_copy_string (char *to, size_t size, const char *from)
{
    size_t n = strlen (from);

    if (n >= size)
        n = size - 1;
    memcpy (to, from, n);
    to[n] = '\0';
}

/* The first line of a small file in /sys, without the newline; "" when
 * there is none. */
static inline void
sde_safe_read_line (const char *path, char *out, size_t size)
{
    FILE *f = fopen (path, "re");

    out[0] = '\0';
    if (f == NULL)
        return;
    if (fgets (out, (int) size, f) == NULL)
        out[0] = '\0';
    fclose (f);
    out[strcspn (out, "\n")] = '\0';
}

/* TRUE when the block device |major|:|minor| is a removable or external
 * drive: on USB, FireWire or a memory stick reader, an SD card (not the
 * computer's own eMMC), a disk with removable media, a device behind an
 * external port (Thunderbolt), or a mapping (LUKS, LVM, RAID) of such a
 * drive. */
static inline int
sde_block_is_removable (unsigned int major, unsigned int minor, int depth)
{
    char link[96], real[PATH_MAX], disk[PATH_MAX], path[PATH_MAX + 32], line[64];
    struct stat st;
    DIR *dir;
    int found = 0;
    char *slash;

    if (depth > 4)
        return 0;
    snprintf (link, sizeof link, "/sys/dev/block/%u:%u", major, minor);
    if (realpath (link, real) == NULL)
        return 0;
    /* A mapping: the devices it is made of. */
    snprintf (path, sizeof path, "%s/slaves", real);
    dir = opendir (path);
    if (dir != NULL)
    {
        struct dirent *e;

        while (!found && (e = readdir (dir)) != NULL)
        {
            unsigned int a, b;

            if (e->d_name[0] == '.')
                continue;
            snprintf (path, sizeof path, "/sys/class/block/%s/dev", e->d_name);
            sde_safe_read_line (path, line, sizeof line);
            if (sscanf (line, "%u:%u", &a, &b) == 2)
                found = sde_block_is_removable (a, b, depth + 1);
        }
        closedir (dir);
        if (found)
            return 1;
    }
    if (strstr (real, "/usb") != NULL || strstr (real, "/firewire") != NULL || strstr (real, "/ieee1394") != NULL ||
        strstr (real, "/memstick") != NULL)
        return 1;
    /* The disk of a partition. */
    sde_safe_copy_string (disk, sizeof disk, real);
    snprintf (path, sizeof path, "%s/partition", real);
    if (stat (path, &st) == 0 && (slash = strrchr (disk, '/')) != NULL)
        *slash = '\0';
    if (strstr (real, "/mmc") != NULL)
    {
        snprintf (path, sizeof path, "%s/device/type", disk);
        sde_safe_read_line (path, line, sizeof line);
        if (strcmp (line, "SD") == 0)
            return 1;
    }
    snprintf (path, sizeof path, "%s/removable", disk);
    sde_safe_read_line (path, line, sizeof line);
    if (strcmp (line, "1") == 0)
        return 1;
    /* A device behind an external port ("removable" since Linux 5.12:
     * Thunderbolt and USB4 docks and enclosures). */
    snprintf (path, sizeof path, "%s/device", disk);
    if (realpath (path, real) == NULL)
        return 0;
    while ((slash = strrchr (real, '/')) != NULL && slash - real > (long) strlen ("/sys/devices"))
    {
        snprintf (path, sizeof path, "%s/removable", real);
        sde_safe_read_line (path, line, sizeof line);
        if (strcmp (line, "removable") == 0)
            return 1;
        *slash = '\0';
    }
    return 0;
}

/* The label of the block device |source| from /dev/disk/by-label, or "". */
static inline void
sde_safe_device_label (const char *source, char *out, size_t size)
{
    char want[PATH_MAX], path[PATH_MAX + 32], target[PATH_MAX];
    DIR *dir;
    struct dirent *e;

    out[0] = '\0';
    if (strncmp (source, "/dev/", 5) != 0 || realpath (source, want) == NULL)
        return;
    dir = opendir ("/dev/disk/by-label");
    if (dir == NULL)
        return;
    while ((e = readdir (dir)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        snprintf (path, sizeof path, "/dev/disk/by-label/%s", e->d_name);
        if (realpath (path, target) != NULL && strcmp (target, want) == 0)
        {
            sde_safe_copy_string (out, size, e->d_name);
            sde_safe_unescape_hex (out);
            break;
        }
    }
    closedir (dir);
}

/* Fills |dest| for the folder |path| (which must exist). Returns 0, or -1
 * when the folder or its mount cannot be found. */
static inline int
sde_dest_probe (const char *path, SdeDest *dest)
{
    static const char *const network[] = { "nfs", "nfs4", "cifs", "smb3", "smbfs", "ncpfs", "9p", "ceph",
                                           "afs", "davfs", "lustre", "fuse.sshfs", "fuse.rclone",
                                           "fuse.gvfsd-fuse", "fuse.glusterfs", "fuse.s3fs", "fuse.davfs2",
                                           NULL };
    static const char *const fat[] = { "vfat", "msdos", "fat", "umsdos", "fuse.fusefat", NULL };
    static const char *const dos[] = { "exfat", "fuse.exfat", "ntfs", "ntfs3", "fuseblk", "fuse.ntfs-3g", NULL };
    char real[PATH_MAX], *line = NULL;
    size_t capacity = 0, best = 0;
    FILE *f;
    int found = 0;

    memset (dest, 0, sizeof (*dest));
    if (path == NULL || realpath (path, real) == NULL)
        return -1;
    f = fopen ("/proc/self/mountinfo", "re");
    if (f == NULL)
        return -1;
    /* The mount with the longest mount point that holds the folder (the
     * last one of equal length: mounts stack). */
    while (getline (&line, &capacity, f) > 0)
    {
        char *fields[16], *save = NULL, *token;
        int n = 0, dash = -1;
        unsigned int major, minor;
        size_t length;

        line[strcspn (line, "\n")] = '\0';
        for (token = strtok_r (line, " ", &save); token != NULL && n < 16; token = strtok_r (NULL, " ", &save))
        {
            if (dash < 0 && n >= 6 && strcmp (token, "-") == 0)
                dash = n;
            fields[n++] = token;
        }
        if (n < 7 || dash < 0 || dash + 2 >= n || sscanf (fields[2], "%u:%u", &major, &minor) != 2)
            continue;
        sde_safe_unescape_octal (fields[4]);
        length = strlen (fields[4]);
        if (!(strcmp (fields[4], "/") == 0 ||
              (strncmp (real, fields[4], length) == 0 && (real[length] == '/' || real[length] == '\0'))))
            continue;
        if (length < best)
            continue;
        best = length;
        found = 1;
        dest->major = major;
        dest->minor = minor;
        sde_safe_copy_string (dest->mount_point, sizeof dest->mount_point, fields[4]);
        sde_safe_copy_string (dest->fs_type, sizeof dest->fs_type, fields[dash + 1]);
        sde_safe_unescape_octal (fields[dash + 2]);
        sde_safe_copy_string (dest->source, sizeof dest->source, fields[dash + 2]);
    }
    free (line);
    fclose (f);
    if (!found)
        return -1;

    dest->fat = sde_safe_type_in (dest->fs_type, fat);
    dest->dos_names = dest->fat || sde_safe_type_in (dest->fs_type, dos);
    dest->user_space = strncmp (dest->fs_type, "fuse", 4) == 0;
    if (sde_safe_type_in (dest->fs_type, network))
        dest->kind = SDE_DEST_NETWORK;
    else if (dest->major != 0 && sde_block_is_removable (dest->major, dest->minor, 0))
        dest->kind = SDE_DEST_REMOVABLE;
    else
        dest->kind = SDE_DEST_INTERNAL;

    sde_safe_device_label (dest->source, dest->name, sizeof dest->name);
    if (dest->name[0] == '\0' && strcmp (dest->mount_point, "/") != 0)
    {
        const char *base = strrchr (dest->mount_point, '/');

        sde_safe_copy_string (dest->name, sizeof dest->name, base != NULL ? base + 1 : dest->mount_point);
    }
    return 0;
}

typedef enum {
    SDE_SAFE_AUTO,   /* removable, external and network drives, and their file systems */
    SDE_SAFE_ALWAYS, /* every destination */
    SDE_SAFE_NEVER   /* the page cache everywhere (the behaviour before 0.2.0 Alpha 3) */
} SdeSafeMode;

/* "always", "never" or "auto"; |fallback| for anything else. */
static inline SdeSafeMode
sde_safe_mode_parse (const char *value, SdeSafeMode fallback)
{
    if (value != NULL && strcmp (value, "always") == 0)
        return SDE_SAFE_ALWAYS;
    if (value != NULL && strcmp (value, "never") == 0)
        return SDE_SAFE_NEVER;
    if (value != NULL && strcmp (value, "auto") == 0)
        return SDE_SAFE_AUTO;
    return fallback;
}

/* The mode: SDE_SAFE_COPY, else the older SDE_LIRIA_SAFE_COPY, else
 * |configured| (sde.conf [files] safe_copy, read by the caller; NULL for
 * none). */
static inline SdeSafeMode
sde_safe_mode (const char *configured)
{
    const char *value = getenv ("SDE_SAFE_COPY");

    if (value == NULL || *value == '\0')
        value = getenv ("SDE_LIRIA_SAFE_COPY");
    if (value == NULL || *value == '\0')
        value = configured;
    return sde_safe_mode_parse (value, SDE_SAFE_AUTO);
}

/* The policy: whether copies to |dest| go through the safe writer. */
static inline int
sde_dest_wants_safe (const SdeDest *dest, SdeSafeMode mode)
{
    static const char *const slow[] = { "udf", "hfsplus", "hfs", "iso9660", NULL };

    if (mode == SDE_SAFE_NEVER)
        return 0;
    if (mode == SDE_SAFE_ALWAYS)
        return 1;
    return dest->kind != SDE_DEST_INTERNAL || dest->dos_names || sde_safe_type_in (dest->fs_type, slow);
}

/* -------------------------------------------------------------- the writer */

#define SDE_WRITER_SLOTS 64            /* pieces (and so files) in flight at most */
#define SDE_WRITER_WINDOW_MIN (4LL << 20)
#define SDE_WRITER_WINDOW_MAX (64LL << 20)
#define SDE_WRITER_WINDOW_START (8LL << 20)
#define SDE_WRITER_WINDOW_SECONDS 1.0  /* the window holds about this much of the drive's time */
#define SDE_WRITER_LATE 0.3            /* a piece in flight this long is waited for */
#define SDE_WRITER_CHECKPOINT 5.0      /* seconds between checkpoints */

typedef enum {
    SDE_WRITER_CACHE, /* no waiting: the page cache, as before (internal disks) */
    SDE_WRITER_RANGE, /* sync_file_range () pieces (file systems in the kernel) */
    SDE_WRITER_FSYNC  /* fdatasync () windows (FUSE, network) */
} SdeWriterMethod;

/* The system calls the writer makes, replaceable for tests. Each returns
 * what the call returns (0 or -1 with errno). */
typedef struct SdeWriterOps {
    int (*sync_range) (void *data, int fd, long long offset, long long length, unsigned int flags);
    int (*datasync) (void *data, int fd);
    int (*sync_fs) (void *data, int fd);
    int (*sync_dir) (void *data, int fd);
    long long (*read) (void *data, int fd, void *buffer, size_t size, long long offset);
    long long (*write) (void *data, int fd, const void *buffer, size_t size, long long offset);
    double (*now) (void *data); /* seconds on a monotonic clock */
} SdeWriterOps;

typedef struct {
    int fd;              /* the writer's own descriptor of the file (a dup) */
    long long offset, length;
    double since;        /* when its writing was started */
} SdeWriterPiece;

typedef struct SdeWriter SdeWriter;
struct SdeWriter {
    SdeWriterMethod method;
    int dir_fd;          /* the destination folder: syncfs (), fsync () */
    long long window;    /* bytes in flight at most (adapts to the drive) */
    long long chunk;     /* bytes per piece of a large file */
    long long pending;   /* bytes in flight: written, not yet on the drive */
    long long written;   /* bytes written so far */
    long long confirmed; /* bytes on the drive so far */
    double rate;         /* the drive's speed, bytes per second (0: not known yet) */
    double rate_time;
    long long rate_bytes;
    double last_checkpoint;
    int files_since_checkpoint;
    int error;           /* the first error of the drive (errno), 0 for none */
    SdeWriterPiece pieces[SDE_WRITER_SLOTS];
    int first, count;
    /* Called for every piece the drive has taken (bytes), from the copying
     * thread. */
    void (*confirmed_cb) (void *data, long long bytes);
    void *confirmed_data;
    const SdeWriterOps *ops;
    void *ops_data;
};

static inline double
sde_writer_clock (void *data)
{
    struct timespec ts;

    (void) data;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline int
sde_writer_sys_sync_range (void *data, int fd, long long offset, long long length, unsigned int flags)
{
    (void) data;
    return sync_file_range (fd, (off_t) offset, (off_t) length, flags);
}

static inline int
sde_writer_sys_datasync (void *data, int fd)
{
    (void) data;
    return fdatasync (fd);
}

static inline int
sde_writer_sys_sync_fs (void *data, int fd)
{
    (void) data;
    return syncfs (fd);
}

static inline int
sde_writer_sys_sync_dir (void *data, int fd)
{
    (void) data;
    return fsync (fd);
}

static inline long long
sde_writer_sys_read (void *data, int fd, void *buffer, size_t size, long long offset)
{
    (void) data;
    return (long long) pread (fd, buffer, size, (off_t) offset);
}

static inline long long
sde_writer_sys_write (void *data, int fd, const void *buffer, size_t size, long long offset)
{
    (void) data;
    return (long long) pwrite (fd, buffer, size, (off_t) offset);
}

static inline const SdeWriterOps *
sde_writer_system_ops (void)
{
    static const SdeWriterOps ops = { sde_writer_sys_sync_range, sde_writer_sys_datasync, sde_writer_sys_sync_fs,
                                      sde_writer_sys_sync_dir, sde_writer_sys_read, sde_writer_sys_write,
                                      sde_writer_clock };
    return &ops;
}

/* The method for a destination: FUSE and network file systems confirm
 * with fdatasync (), the others with sync_file_range (). */
static inline SdeWriterMethod
sde_writer_method_for (const SdeDest *dest, int safe)
{
    if (!safe)
        return SDE_WRITER_CACHE;
    return dest->user_space || dest->kind == SDE_DEST_NETWORK ? SDE_WRITER_FSYNC : SDE_WRITER_RANGE;
}

/* |dir_fd|: an open folder on the destination (the writer closes it), or
 * -1. |ops| NULL: the real system calls. */
static inline void
sde_writer_init (SdeWriter *w, SdeWriterMethod method, int dir_fd, const SdeWriterOps *ops, void *ops_data)
{
    memset (w, 0, sizeof (*w));
    w->method = method;
    w->dir_fd = dir_fd;
    w->window = SDE_WRITER_WINDOW_START;
    w->chunk = SDE_WRITER_WINDOW_START / 4;
    w->ops = ops != NULL ? ops : sde_writer_system_ops ();
    w->ops_data = ops_data;
    w->last_checkpoint = w->rate_time = w->ops->now (w->ops_data);
}

/* The window follows the drive: about SDE_WRITER_WINDOW_SECONDS of its
 * speed, measured over a quarter of a second or more. */
static inline void
sde_writer_measure (SdeWriter *w, double now)
{
    double span = now - w->rate_time;

    if (span < 0.25)
        return;
    {
        double rate = (double) (w->confirmed - w->rate_bytes) / span;
        long long window;

        w->rate = w->rate > 0.0 ? w->rate * 0.6 + rate * 0.4 : rate;
        window = (long long) (w->rate * SDE_WRITER_WINDOW_SECONDS);
        if (window < SDE_WRITER_WINDOW_MIN)
            window = SDE_WRITER_WINDOW_MIN;
        if (window > SDE_WRITER_WINDOW_MAX)
            window = SDE_WRITER_WINDOW_MAX;
        w->window = window;
        w->chunk = window / 4;
    }
    w->rate_time = now;
    w->rate_bytes = w->confirmed;
}

static inline void
sde_writer_fail (SdeWriter *w, int error)
{
    if (w->error == 0)
        w->error = error != 0 ? error : EIO;
}

/* The oldest piece leaves the window; |taken|: the drive has it, so it
 * counts for the progress (after an error of the drive nothing does). */
static inline void
sde_writer_pop (SdeWriter *w, int taken)
{
    SdeWriterPiece *piece = &w->pieces[w->first];
    int next = (w->first + 1) % SDE_WRITER_SLOTS;
    long long bytes = piece->length;

    if (w->method == SDE_WRITER_RANGE && bytes > 0 && taken)
        /* Clean now: no need to keep it in memory. */
        posix_fadvise (piece->fd, (off_t) piece->offset, (off_t) bytes, POSIX_FADV_DONTNEED);
    /* The descriptor goes once the file's last piece goes. */
    if (w->count == 1 || w->pieces[next].fd != piece->fd)
        close (piece->fd);
    w->first = next;
    w->count--;
    w->pending -= bytes;
    if (!taken || bytes <= 0)
        return;
    w->confirmed += bytes;
    if (w->confirmed_cb != NULL)
        w->confirmed_cb (w->confirmed_data, bytes);
}

/* Waits until the oldest piece (RANGE) or everything in flight (FSYNC) is
 * on the drive. Returns 0 or the error. */
static inline int
sde_writer_confirm_oldest (SdeWriter *w)
{
    SdeWriterPiece *piece;

    if (w->count == 0)
        return 0;
    piece = &w->pieces[w->first];
    if (w->method == SDE_WRITER_FSYNC)
    {
        /* One fdatasync () of the newest file: user-space file systems
         * sync their whole device, and network ones wrote the older files
         * when they were closed. */
        SdeWriterPiece *newest = &w->pieces[(w->first + w->count - 1) % SDE_WRITER_SLOTS];

        if (w->ops->datasync (w->ops_data, newest->fd) != 0 && errno != EINVAL && errno != EROFS)
            sde_writer_fail (w, errno);
        while (w->count > 0)
            sde_writer_pop (w, w->error == 0);
    }
    else
    {
        if (piece->length > 0 &&
            w->ops->sync_range (w->ops_data, piece->fd, piece->offset, piece->length,
                                SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                                    SYNC_FILE_RANGE_WAIT_AFTER) != 0 &&
            errno != ESPIPE && errno != EINVAL && errno != ENOSYS)
            sde_writer_fail (w, errno);
        sde_writer_pop (w, w->error == 0);
    }
    sde_writer_measure (w, w->ops->now (w->ops_data));
    return w->error;
}

/* Waits while too much is in flight, or a piece has been in flight too
 * long (it is probably on the drive: the wait is short and the progress
 * stays close to the drive when the copy reads slowly). */
static inline int
sde_writer_settle (SdeWriter *w)
{
    while (w->count > 0 && w->error == 0)
    {
        double now = w->ops->now (w->ops_data);

        if (!(w->pending > w->window || w->count >= SDE_WRITER_SLOTS ||
              now - w->pieces[w->first].since >= SDE_WRITER_LATE))
            break;
        sde_writer_confirm_oldest (w);
    }
    return w->error;
}

/* |fd| has [offset, offset + length) written: starts writing it to the
 * drive and keeps it in flight. Returns 0 or the error of the drive. */
static inline int
sde_writer_add (SdeWriter *w, int fd, long long offset, long long length)
{
    SdeWriterPiece *piece;
    int slot, own;

    w->written += length;
    if (w->method == SDE_WRITER_CACHE)
    {
        w->confirmed += length;
        if (w->confirmed_cb != NULL && length > 0)
            w->confirmed_cb (w->confirmed_data, length);
        return 0;
    }
    if (w->count >= SDE_WRITER_SLOTS)
        sde_writer_confirm_oldest (w);
    if (w->method == SDE_WRITER_RANGE && length > 0 &&
        w->ops->sync_range (w->ops_data, fd, offset, length, SYNC_FILE_RANGE_WRITE) != 0 && errno != ESPIPE &&
        errno != EINVAL && errno != ENOSYS)
        sde_writer_fail (w, errno);
    /* The same descriptor as the previous piece of this file, or a new one
     * the writer owns (the caller closes its own). */
    slot = (w->first + w->count) % SDE_WRITER_SLOTS;
    own = -1;
    if (w->count > 0)
    {
        SdeWriterPiece *last = &w->pieces[(slot + SDE_WRITER_SLOTS - 1) % SDE_WRITER_SLOTS];
        struct stat a, b;

        if (fstat (last->fd, &a) == 0 && fstat (fd, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
            own = last->fd;
    }
    if (own < 0)
        own = fcntl (fd, F_DUPFD_CLOEXEC, 3);
    if (own < 0)
    {
        /* No descriptor left: wait for this piece now. */
        if (length > 0 &&
            w->ops->sync_range (w->ops_data, fd, offset, length,
                                SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) !=
                0 &&
            errno != ESPIPE && errno != EINVAL && errno != ENOSYS)
            sde_writer_fail (w, errno);
        if (w->method == SDE_WRITER_FSYNC && w->ops->datasync (w->ops_data, fd) != 0 && errno != EINVAL)
            sde_writer_fail (w, errno);
        if (w->error == 0)
        {
            w->confirmed += length;
            if (w->confirmed_cb != NULL && length > 0)
                w->confirmed_cb (w->confirmed_data, length);
        }
        return w->error;
    }
    piece = &w->pieces[slot];
    piece->fd = own;
    piece->offset = offset;
    piece->length = length;
    piece->since = w->ops->now (w->ops_data);
    w->count++;
    w->pending += length;
    return sde_writer_settle (w);
}

/* Everything in flight is on the drive. */
static inline int
sde_writer_drain (SdeWriter *w)
{
    while (w->count > 0)
        sde_writer_confirm_oldest (w);
    return w->error;
}

/* Makes everything written so far durable: the window drained, syncfs ()
 * and fsync () of the destination folder (on FAT also the drive's write
 * cache). Returns 0 or the error. */
static inline int
sde_writer_checkpoint (SdeWriter *w)
{
    sde_writer_drain (w);
    if (w->method != SDE_WRITER_CACHE && w->dir_fd >= 0)
    {
        if (w->ops->sync_fs (w->ops_data, w->dir_fd) != 0)
            sde_writer_fail (w, errno);
        if (w->ops->sync_dir (w->ops_data, w->dir_fd) != 0 && errno != EINVAL && errno != EROFS &&
            errno != ENOSYS)
            sde_writer_fail (w, errno);
    }
    w->last_checkpoint = w->ops->now (w->ops_data);
    w->files_since_checkpoint = 0;
    return w->error;
}

/* After each file: a checkpoint every SDE_WRITER_CHECKPOINT seconds, so
 * the files done before an interruption are complete on the drive and the
 * last flush stays short. Returns 0 or the error. */
static inline int
sde_writer_file_done (SdeWriter *w)
{
    w->files_since_checkpoint++;
    if (w->method != SDE_WRITER_CACHE &&
        w->ops->now (w->ops_data) - w->last_checkpoint >= SDE_WRITER_CHECKPOINT)
        return sde_writer_checkpoint (w);
    return w->error;
}

/* The last checkpoint, then the writer is closed. Returns 0 or the error. */
static inline int
sde_writer_finish (SdeWriter *w)
{
    int error = sde_writer_checkpoint (w);

    if (w->dir_fd >= 0)
        close (w->dir_fd);
    w->dir_fd = -1;
    return error;
}

/* Forgets what is in flight (after an error of the drive). */
static inline void
sde_writer_abandon (SdeWriter *w)
{
    while (w->count > 0)
    {
        SdeWriterPiece *piece = &w->pieces[w->first];
        int next = (w->first + 1) % SDE_WRITER_SLOTS;

        if (w->count == 1 || w->pieces[next].fd != piece->fd)
            close (piece->fd);
        w->first = next;
        w->count--;
    }
    w->pending = 0;
    if (w->dir_fd >= 0)
        close (w->dir_fd);
    w->dir_fd = -1;
}

/* ------------------------------------------------------------ the copy loop */

typedef struct {
    /* Nonzero stops the copy (ECANCELED); may block while paused. */
    int (*stop) (void *data);
    void *data;
} SdeCopyHooks;

/* Copies the data of the open regular file |in| (|size| bytes) into the
 * empty file |out| through |w|, the holes of a sparse file as holes, with
 * |buffer| of |buffer_size| bytes. Returns 0 or an errno: of the source
 * (*source_error set too), of the destination or the drive, or ECANCELED.
 * The file's last pieces may still be in flight: sde_writer_drain () or a
 * checkpoint waits for them. */
static inline int
sde_copy_data (SdeWriter *w, int in, int out, long long size, unsigned char *buffer, size_t buffer_size,
               const SdeCopyHooks *hooks, int *source_error)
{
    long long offset = 0, piece_start = 0;
    int holes = 1, error;

    *source_error = 0;
    while (offset < size)
    {
        long long data_start = offset, data_end = size;

        if (hooks != NULL && hooks->stop != NULL && hooks->stop (hooks->data))
            return ECANCELED;
        /* Where the next data is, when the source has holes. */
        if (holes)
        {
            off_t data = lseek (in, (off_t) offset, SEEK_DATA);

            if (data < 0 && errno == ENXIO)
                break; /* a hole up to the end */
            if (data < 0)
            {
                holes = 0;
            }
            else
            {
                off_t hole = lseek (in, data, SEEK_HOLE);

                data_start = (long long) data;
                if (hole > data && (long long) hole < size)
                    data_end = (long long) hole;
            }
        }
        if (data_start >= size)
            break;
        if (data_start > offset)
        {
            /* A hole: what came before it is a piece of its own. */
            if (offset > piece_start && (error = sde_writer_add (w, out, piece_start, offset - piece_start)) != 0)
                return error;
            offset = piece_start = data_start;
        }
        while (offset < data_end)
        {
            long long want = (long long) buffer_size, n, done = 0;

            if (hooks != NULL && hooks->stop != NULL && hooks->stop (hooks->data))
                return ECANCELED;
            if (want > data_end - offset)
                want = data_end - offset;
            n = w->ops->read (w->ops_data, in, buffer, (size_t) want, offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                return *source_error = errno;
            if (n == 0)
            {
                /* The source became shorter while it was copied. */
                size = data_end = offset;
                break;
            }
            while (done < n)
            {
                long long put = w->ops->write (w->ops_data, out, buffer + done, (size_t) (n - done), offset + done);

                if (put < 0 && errno == EINTR)
                    continue;
                if (put <= 0)
                    return put < 0 ? errno : ENOSPC;
                done += put;
            }
            offset += n;
            if (offset - piece_start >= w->chunk)
            {
                if ((error = sde_writer_add (w, out, piece_start, offset - piece_start)) != 0)
                    return error;
                piece_start = offset;
            }
        }
    }
    if (offset > piece_start && (error = sde_writer_add (w, out, piece_start, offset - piece_start)) != 0)
        return error;
    /* A hole at the end: the size, without writing it. */
    {
        struct stat st;

        if (fstat (out, &st) == 0 && st.st_size < size && ftruncate (out, (off_t) size) != 0)
            return errno;
    }
    return w->error;
}

#ifdef __cplusplus
}
#endif

#endif /* SDE_SAFE_COPY_H */
