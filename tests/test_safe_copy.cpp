// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of safe copying (0.2.0 Alpha 3, src/common/safe_copy.h, also
// built into Liria): the writer copies real files in a temporary folder,
// while its system calls (sync_file_range, fdatasync, syncfs, fsync, the
// clock) go to a simulated drive with its own speed and a page cache of
// written but unwritten data. The tests check that
//   - the progress never runs ahead of the drive, and no more than the
//     window is ever written but not on the drive;
//   - "done" comes only after everything is on the drive and the file
//     system and the folder were synced after the last write;
//   - the drive stays busy: large files and many small files take about
//     the drive's own time, and the window follows the drive's speed;
//   - holes stay holes, errors of the drive and Cancel stop the copy;
// and the parts of sde_dest_probe () that need no drive.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "safe_copy.h"

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

namespace {

// A drive with a page cache in front of it. Written data is "dirty" until
// the drive has written it; the drive writes what was submitted to it in
// order, at |rate| bytes per second, and keeps time with the copy.
struct Drive {
  struct Extent {
    ino_t ino;
    long long offset, length;
    bool submitted;
  };
  double rate = 20e6;          // the drive, bytes per second
  double memory = 2e9;         // writing into the page cache
  double read_rate = 400e6;    // the source disk
  double flush_cost = 0.02;    // syncfs () and fsync (): metadata and the drive's cache
  double t = 0;                // simulated time
  double drive_clock = 0;      // the drive has worked up to this time
  std::deque<Extent> dirty;    // written, not yet on the drive, in writing order
  long long written = 0, on_drive = 0;
  long long max_dirty = 0;
  double last_write = -1, last_sync_fs = -1, last_sync_dir = -1;
  int sync_fs_calls = 0, sync_range_waits = 0, datasyncs = 0;
  int fail_wait_after = -1;    // the nth wait fails with EIO (-1: never)
  int fail_write_after = -1;   // the nth write fails with ENOSPC

  // The drive writes submitted extents, oldest first, until |until|; it
  // cannot work in the past, so it idles up to |until| when it has nothing.
  void run(double until) {
    for (auto it = dirty.begin(); it != dirty.end() && drive_clock < until;) {
      if (!it->submitted) {
        ++it;
        continue;
      }
      double need = it->length / rate;
      if (drive_clock + need <= until) {
        drive_clock += need;
        on_drive += it->length;
        it = dirty.erase(it);
      } else {
        long long part = (long long)((until - drive_clock) * rate);
        it->offset += part;
        it->length -= part;
        on_drive += part;
        drive_clock = until;
      }
    }
    if (drive_clock < until) drive_clock = until;
  }
  void advance(double dt) {
    t += dt;
    run(t);
  }
  // Waits until nothing of |ino| in [offset, offset + length) is dirty and
  // submitted (|ino| 0: everything).
  void wait(ino_t ino, long long offset, long long length) {
    for (;;) {
      bool busy = false;
      for (auto &e : dirty)
        if (e.submitted && (ino == 0 || (e.ino == ino && e.offset < offset + length && offset < e.offset + e.length)))
          busy = true;
      if (!busy) return;
      // To the end of the next extent.
      double step = 1e-4;
      for (auto &e : dirty)
        if (e.submitted) {
          step = std::max(step, e.length / rate);
          break;
        }
      advance(step);
    }
  }
  void submit(ino_t ino, long long offset, long long length) {
    for (auto &e : dirty)
      if ((ino == 0 || e.ino == ino) && e.offset < offset + length && offset < e.offset + e.length)
        e.submitted = true;
  }
  long long dirty_bytes() const {
    long long n = 0;
    for (auto &e : dirty) n += e.length;
    return n;
  }
};

ino_t inode_of(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 ? st.st_ino : 0;
}

int drive_sync_range(void *data, int fd, long long offset, long long length, unsigned int flags) {
  auto *d = static_cast<Drive *>(data);
  ino_t ino = inode_of(fd);
  if (flags & SYNC_FILE_RANGE_WRITE) d->submit(ino, offset, length);
  if (flags & SYNC_FILE_RANGE_WAIT_AFTER) {
    if (d->fail_wait_after >= 0 && d->sync_range_waits++ == d->fail_wait_after) {
      errno = EIO;
      return -1;
    }
    d->wait(ino, offset, length);
  }
  d->advance(2e-5);  // a system call
  return 0;
}

int drive_datasync(void *data, int fd) {
  auto *d = static_cast<Drive *>(data);
  d->datasyncs++;
  d->submit(0, 0, 1LL << 62);  // like ntfs-3g: the whole device
  d->wait(0, 0, 1LL << 62);
  d->advance(d->flush_cost);
  return 0;
}

int drive_sync_fs(void *data, int fd) {
  auto *d = static_cast<Drive *>(data);
  d->sync_fs_calls++;
  d->submit(0, 0, 1LL << 62);
  d->wait(0, 0, 1LL << 62);
  d->advance(d->flush_cost);
  d->last_sync_fs = d->t;
  return 0;
}

int drive_sync_dir(void *data, int fd) {
  auto *d = static_cast<Drive *>(data);
  d->advance(d->flush_cost / 4);
  d->last_sync_dir = d->t;
  return 0;
}

long long drive_read(void *data, int fd, void *buffer, size_t size, long long offset) {
  auto *d = static_cast<Drive *>(data);
  long long n = pread(fd, buffer, size, offset);
  if (n > 0) d->advance(n / d->read_rate);
  return n;
}

long long drive_write(void *data, int fd, const void *buffer, size_t size, long long offset) {
  auto *d = static_cast<Drive *>(data);
  if (d->fail_write_after >= 0 && d->written >= d->fail_write_after) {
    errno = ENOSPC;
    return -1;
  }
  long long n = pwrite(fd, buffer, size, offset);
  if (n <= 0) return n;
  d->dirty.push_back({inode_of(fd), offset, n, false});
  d->written += n;
  d->last_write = d->t;
  d->advance(n / d->memory);
  d->max_dirty = std::max(d->max_dirty, d->dirty_bytes());
  return n;
}

double drive_now(void *data) { return static_cast<Drive *>(data)->t; }

const SdeWriterOps kDriveOps = {drive_sync_range, drive_datasync, drive_sync_fs, drive_sync_dir,
                                drive_read,       drive_write,    drive_now};

// What the copy reported, checked against the drive at every report.
struct Watch {
  Drive *drive = nullptr;
  SdeWriter *writer = nullptr;
  long long reported = 0;
  long long max_lead = -(1LL << 62);
  long long max_excess = 0;  // dirty beyond the window
};

void on_confirmed(void *data, long long bytes) {
  auto *w = static_cast<Watch *>(data);
  w->reported += bytes;
  w->max_lead = std::max(w->max_lead, w->reported - w->drive->on_drive);
}

std::string g_dir;

std::string make_file(const std::string &name, long long size, unsigned char seed) {
  std::string path = g_dir + "/" + name;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::vector<unsigned char> block(1 << 16);
  for (long long done = 0; done < size;) {
    for (size_t i = 0; i < block.size(); ++i) block[i] = (unsigned char)(seed + (done + i) * 7 / 3);
    long long n = std::min<long long>(block.size(), size - done);
    if (write(fd, block.data(), n) != n) break;
    done += n;
  }
  close(fd);
  return path;
}

bool same_content(const std::string &a, const std::string &b) {
  FILE *fa = fopen(a.c_str(), "rb"), *fb = fopen(b.c_str(), "rb");
  bool same = fa && fb;
  while (same) {
    int ca = fgetc(fa), cb = fgetc(fb);
    if (ca != cb) same = false;
    if (ca == EOF || cb == EOF) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  return same;
}

// SDE_TEST_VERBOSE=1 prints what each copy measured.
void report(const char *name, const Drive &d, const SdeWriter &w, const Watch &watch) {
  if (!getenv("SDE_TEST_VERBOSE")) return;
  printf("%-12s time %.2f s, written %lld, lead %lld, max dirty %lld, window %lld, rate %.0f, syncfs %d, "
         "datasync %d\n", name, d.t, d.written, watch.max_lead, d.max_dirty, w.window, w.rate, d.sync_fs_calls,
         d.datasyncs);
}

struct Copy {
  Drive drive;
  SdeWriter writer;
  Watch watch;
  std::vector<unsigned char> buffer = std::vector<unsigned char>(1 << 20);

  explicit Copy(double rate, SdeWriterMethod method = SDE_WRITER_RANGE) {
    drive.rate = rate;
    int dir = open(g_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    sde_writer_init(&writer, method, dir, &kDriveOps, &drive);
    watch.drive = &drive;
    watch.writer = &writer;
    writer.confirmed_cb = on_confirmed;
    writer.confirmed_data = &watch;
  }
  // Copies |src| to |dst|; returns the error.
  int file(const std::string &src, const std::string &dst, const SdeCopyHooks *hooks = nullptr) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    struct stat st;
    fstat(in, &st);
    int source_error = 0;
    int error = sde_copy_data(&writer, in, out, st.st_size, buffer.data(), buffer.size(), hooks, &source_error);
    // The window may have grown since the last write: never beyond it
    // and one piece and one buffer.
    long long excess = drive.dirty_bytes() - (SDE_WRITER_WINDOW_MAX + SDE_WRITER_WINDOW_MAX / 4 + (1 << 20));
    watch.max_excess = std::max(watch.max_excess, excess);
    close(in);
    close(out);
    if (!error) error = sde_writer_file_done(&writer);
    return error;
  }
};

// A large file on a slow drive: the progress follows the drive, the dirty
// data stays within the window, "done" only when it is all there, and the
// drive is never idle while the copy waits.
void test_large_file_slow_drive() {
  std::string src = make_file("large.bin", 200LL << 20, 1);
  Copy c(10e6);  // a USB 2 stick
  CHECK(c.file(src, g_dir + "/large.copy") == 0);
  double before_finish = c.drive.t;
  CHECK(sde_writer_finish(&c.writer) == 0);
  report("large", c.drive, c.writer, c.watch);
  CHECK(c.watch.max_lead <= 0);                      // never ahead of the drive
  CHECK(c.drive.max_dirty <= 16LL << 20);            // ~1 s of a 10 MB/s drive, and a piece
  CHECK(c.drive.dirty_bytes() == 0);                 // all on the drive
  CHECK(c.drive.last_sync_fs >= c.drive.last_write);  // synced after the last write
  CHECK(c.drive.last_sync_dir >= c.drive.last_sync_fs);
  CHECK(c.watch.reported == 200LL << 20);
  double ideal = (200LL << 20) / 10e6;
  CHECK(c.drive.t < ideal * 1.05 + 0.5);  // the drive kept busy
  CHECK(c.drive.t - before_finish < 1.5);   // the last flush is short
  CHECK(c.writer.window >= SDE_WRITER_WINDOW_MIN && c.writer.window <= 16LL << 20);
  CHECK(std::fabs(c.writer.rate - 10e6) < 1.5e6);
  CHECK(same_content(src, g_dir + "/large.copy"));
  unlink(src.c_str());
  unlink((g_dir + "/large.copy").c_str());
}

// A fast drive gets the largest window and is not slowed down.
void test_fast_drive_window() {
  std::string src = make_file("fast.bin", 600LL << 20, 2);
  Copy c(400e6);
  c.drive.read_rate = 3e9;  // the drive is the slowest part
  CHECK(c.file(src, g_dir + "/fast.copy") == 0);
  CHECK(sde_writer_finish(&c.writer) == 0);
  report("fast", c.drive, c.writer, c.watch);
  CHECK(c.writer.window == SDE_WRITER_WINDOW_MAX);
  CHECK(c.watch.max_lead <= 0);
  CHECK(c.drive.t < (600LL << 20) / 400e6 * 1.10 + 0.2);
  CHECK(c.watch.max_excess <= 0);
  unlink(src.c_str());
  unlink((g_dir + "/fast.copy").c_str());
}

// Many small files: one piece each, the drive busy, checkpoints every few
// seconds and not more, the progress never ahead.
void test_many_small_files() {
  mkdir((g_dir + "/small").c_str(), 0755);
  mkdir((g_dir + "/small.copy").c_str(), 0755);
  std::vector<std::string> names;
  for (int i = 0; i < 600; ++i) names.push_back(make_file("small/f" + std::to_string(i), 48 * 1024 + i, i & 0xff));
  Copy c(8e6);
  long long total = 0;
  for (int i = 0; i < 600; ++i) {
    CHECK(c.file(names[i], g_dir + "/small.copy/f" + std::to_string(i)) == 0);
    total += 48 * 1024 + i;
  }
  CHECK(sde_writer_finish(&c.writer) == 0);
  report("small", c.drive, c.writer, c.watch);
  CHECK(c.watch.max_lead <= 0);
  CHECK(c.watch.reported == total);
  CHECK(c.drive.dirty_bytes() == 0);
  CHECK(c.drive.last_sync_fs >= c.drive.last_write);
  double ideal = total / 8e6;
  CHECK(c.drive.t < ideal * 1.15 + 0.5);
  // A checkpoint every SDE_WRITER_CHECKPOINT seconds and the last one.
  CHECK(c.drive.sync_fs_calls <= int(c.drive.t / SDE_WRITER_CHECKPOINT) + 2);
  CHECK(c.drive.sync_fs_calls >= int(c.drive.t / SDE_WRITER_CHECKPOINT));
  CHECK(c.drive.max_dirty <= 16LL << 20);
  bool same = true;
  for (int i = 0; i < 600; i += 37) same = same && same_content(names[i], g_dir + "/small.copy/f" + std::to_string(i));
  CHECK(same);
  for (int i = 0; i < 600; ++i) {
    unlink(names[i].c_str());
    unlink((g_dir + "/small.copy/f" + std::to_string(i)).c_str());
  }
  rmdir((g_dir + "/small").c_str());
  rmdir((g_dir + "/small.copy").c_str());
}

// A copy that reads slowly: pieces in flight for long are confirmed, so
// the progress is not held back by the window.
void test_slow_source() {
  std::string src = make_file("slowsrc.bin", 40LL << 20, 3);
  Copy c(100e6);
  c.drive.read_rate = 5e6;  // a slow network share as the source
  long long lag = 0;
  struct Hook {
    Copy *c;
    long long *lag;
  } hook{&c, &lag};
  SdeCopyHooks hooks;
  hooks.stop = [](void *data) {
    auto *h = static_cast<Hook *>(data);
    // What the drive has, but the progress does not show yet.
    *h->lag = std::max(*h->lag, h->c->drive.on_drive - h->c->watch.reported);
    return 0;
  };
  hooks.data = &hook;
  CHECK(c.file(src, g_dir + "/slowsrc.copy", &hooks) == 0);
  CHECK(sde_writer_finish(&c.writer) == 0);
  report("slow source", c.drive, c.writer, c.watch);
  if (getenv("SDE_TEST_VERBOSE")) printf("slow source lag %lld\n", lag);
  // At 5 MB/s a piece waits at most SDE_WRITER_LATE seconds (and a buffer).
  CHECK(lag <= (long long)(5e6 * SDE_WRITER_LATE) + (2 << 20) + c.writer.chunk);
  unlink(src.c_str());
  unlink((g_dir + "/slowsrc.copy").c_str());
}

// FUSE and network drives: fdatasync () windows, the same guarantees.
void test_fsync_method() {
  std::string src = make_file("fuse.bin", 64LL << 20, 4);
  Copy c(15e6, SDE_WRITER_FSYNC);
  CHECK(c.file(src, g_dir + "/fuse.copy") == 0);
  CHECK(sde_writer_finish(&c.writer) == 0);
  report("fsync", c.drive, c.writer, c.watch);
  CHECK(c.watch.max_lead <= 0);
  CHECK(c.drive.datasyncs > 3);
  CHECK(c.drive.max_dirty <= 20LL << 20);
  CHECK(c.drive.dirty_bytes() == 0);
  CHECK(same_content(src, g_dir + "/fuse.copy"));
  unlink(src.c_str());
  unlink((g_dir + "/fuse.copy").c_str());
}

// The page cache mode (internal disks): no waiting, everything at once.
void test_cache_method() {
  std::string src = make_file("cache.bin", 32LL << 20, 5);
  Copy c(10e6, SDE_WRITER_CACHE);
  CHECK(c.file(src, g_dir + "/cache.copy") == 0);
  CHECK(sde_writer_finish(&c.writer) == 0);
  CHECK(c.watch.reported == 32LL << 20);
  CHECK(c.drive.sync_fs_calls == 0 && c.drive.datasyncs == 0);
  CHECK(c.drive.dirty_bytes() == 32LL << 20);  // still in memory: the old behaviour
  unlink(src.c_str());
  unlink((g_dir + "/cache.copy").c_str());
}

// Holes are not written, and the copy has the same size and content.
void test_sparse_file() {
  std::string src = g_dir + "/sparse.bin";
  int fd = open(src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::vector<char> data(1 << 20, 'x');
  CHECK(pwrite(fd, data.data(), data.size(), 0) == (ssize_t)data.size());
  CHECK(pwrite(fd, data.data(), data.size(), 64LL << 20) == (ssize_t)data.size());
  CHECK(ftruncate(fd, 128LL << 20) == 0);  // a hole at the end too
  close(fd);
  Copy c(50e6);
  CHECK(c.file(src, g_dir + "/sparse.copy") == 0);
  CHECK(sde_writer_finish(&c.writer) == 0);
  struct stat a, b;
  stat(src.c_str(), &a);
  stat((g_dir + "/sparse.copy").c_str(), &b);
  CHECK(a.st_size == b.st_size);
  // tmpfs and ext4 report holes: only the data was written.
  if (a.st_blocks * 512 < a.st_size / 2) CHECK(c.drive.written <= 4LL << 20);
  CHECK(same_content(src, g_dir + "/sparse.copy"));
  unlink(src.c_str());
  unlink((g_dir + "/sparse.copy").c_str());
}

// An error of the drive (a pulled stick: EIO) and a full drive stop the
// copy with the error; Cancel stops it with ECANCELED.
void test_errors_and_cancel() {
  std::string src = make_file("err.bin", 48LL << 20, 6);
  {
    Copy c(10e6);
    c.drive.fail_wait_after = 2;
    CHECK(c.file(src, g_dir + "/err.copy") == EIO);
    sde_writer_abandon(&c.writer);
  }
  {
    Copy c(10e6);
    c.drive.fail_write_after = 20LL << 20;
    CHECK(c.file(src, g_dir + "/err.copy") == ENOSPC);
    sde_writer_abandon(&c.writer);
  }
  {
    Copy c(10e6);
    int calls = 0;
    SdeCopyHooks hooks;
    hooks.stop = [](void *data) { return ++*static_cast<int *>(data) > 5 ? 1 : 0; };
    hooks.data = &calls;
    CHECK(c.file(src, g_dir + "/err.copy", &hooks) == ECANCELED);
    CHECK(sde_writer_finish(&c.writer) == 0);  // what was written is still synced
    CHECK(c.drive.dirty_bytes() == 0);
  }
  unlink(src.c_str());
  unlink((g_dir + "/err.copy").c_str());
}

// The policy and the parts of the probe that need no drive.
void test_policy() {
  SdeDest d;
  memset(&d, 0, sizeof d);
  strcpy(d.fs_type, "ext4");
  d.kind = SDE_DEST_INTERNAL;
  CHECK(!sde_dest_wants_safe(&d, SDE_SAFE_AUTO));
  CHECK(sde_dest_wants_safe(&d, SDE_SAFE_ALWAYS));
  d.kind = SDE_DEST_REMOVABLE;
  CHECK(sde_dest_wants_safe(&d, SDE_SAFE_AUTO));
  CHECK(!sde_dest_wants_safe(&d, SDE_SAFE_NEVER));
  CHECK(sde_writer_method_for(&d, 1) == SDE_WRITER_RANGE);
  CHECK(sde_writer_method_for(&d, 0) == SDE_WRITER_CACHE);
  d.kind = SDE_DEST_INTERNAL;
  d.dos_names = 1;  // an NTFS or exFAT partition
  CHECK(sde_dest_wants_safe(&d, SDE_SAFE_AUTO));
  d.user_space = 1;
  CHECK(sde_writer_method_for(&d, 1) == SDE_WRITER_FSYNC);
  d.user_space = 0;
  d.dos_names = 0;
  d.kind = SDE_DEST_NETWORK;
  CHECK(sde_writer_method_for(&d, 1) == SDE_WRITER_FSYNC);
  char s[] = "/media/me/My\\040Stick";
  sde_safe_unescape_octal(s);
  CHECK(strcmp(s, "/media/me/My Stick") == 0);
  char l[] = "MY\\x20STICK";
  sde_safe_unescape_hex(l);
  CHECK(strcmp(l, "MY STICK") == 0);
  // This folder is on some mount; its name never is the whole path.
  CHECK(sde_dest_probe(g_dir.c_str(), &d) == 0);
  CHECK(d.fs_type[0] != '\0' && d.mount_point[0] == '/');
  CHECK(sde_dest_probe("/nonexistent/folder", &d) == -1);
  setenv("SDE_SAFE_COPY", "never", 1);
  CHECK(sde_safe_mode("always") == SDE_SAFE_NEVER);  // the environment first
  setenv("SDE_SAFE_COPY", "always", 1);
  CHECK(sde_safe_mode(nullptr) == SDE_SAFE_ALWAYS);
  unsetenv("SDE_SAFE_COPY");
  setenv("SDE_LIRIA_SAFE_COPY", "never", 1);
  CHECK(sde_safe_mode(nullptr) == SDE_SAFE_NEVER);
  unsetenv("SDE_LIRIA_SAFE_COPY");
  CHECK(sde_safe_mode(nullptr) == SDE_SAFE_AUTO);
  CHECK(sde_safe_mode("always") == SDE_SAFE_ALWAYS);  // then sde.conf
  CHECK(sde_safe_mode("bogus") == SDE_SAFE_AUTO);
}

}  // namespace

int main() {
  const char *tmp = getenv("TMPDIR");
  std::string base = std::string(tmp && *tmp ? tmp : "/tmp") + "/sde-safe-copy-XXXXXX";
  std::vector<char> dir(base.begin(), base.end());
  dir.push_back('\0');
  if (!mkdtemp(dir.data())) {
    perror("mkdtemp");
    return 1;
  }
  g_dir = dir.data();
  test_policy();
  test_large_file_slow_drive();
  test_fast_drive_window();
  test_many_small_files();
  test_slow_source();
  test_fsync_method();
  test_cache_method();
  test_sparse_file();
  test_errors_and_cancel();
  rmdir(g_dir.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-safe-copy tests passed\n");
  return 0;
}
