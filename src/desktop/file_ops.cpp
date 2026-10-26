// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "config.h"
#include "desktop_model.h"
#include "i18n.h"
#include "safe_copy.h"
#include "util.h"

namespace sde {
namespace desk {

namespace {

void fail(std::string *error, const std::string &what, const std::string &path, int err) {
  if (error) *error = what + " \"" + path + "\": " + strerror(err);
}

std::string parent_of(const std::string &path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  return slash == 0 ? "/" : path.substr(0, slash);
}

bool rename_noreplace(const std::string &src, const std::string &dst) {
  int r = renameat2(AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(), RENAME_NOREPLACE);
  if (r != 0 && (errno == EINVAL || errno == ENOSYS)) {
    // File systems without the flag: check, then rename (small race).
    struct stat st;
    if (lstat(dst.c_str(), &st) == 0) {
      errno = EEXIST;
      return false;
    }
    r = rename(src.c_str(), dst.c_str());
  }
  return r == 0;
}

void keep_times(const std::string &dst, const struct stat &st, bool symlink) {
  struct timespec ts[2] = {st.st_atim, st.st_mtim};
  utimensat(AT_FDCWD, dst.c_str(), ts, symlink ? AT_SYMLINK_NOFOLLOW : 0);
}

// SDE_QA_COPY_RATE=<bytes per second> slows copies down for screenshots
// and UI tests of the transfer dialog (tools/showcase.py); 0 or unset: full
// speed.
double qa_copy_rate() {
  static double rate = -1;
  if (rate < 0) {
    const char *v = getenv("SDE_QA_COPY_RATE");
    rate = v ? atof(v) : 0;
    if (rate < 0) rate = 0;
  }
  return rate;
}

const SdeWriterOps *g_test_ops = nullptr;
void *g_test_ops_data = nullptr;

// sde.conf [files] safe_copy (auto, always, never), SDE_SAFE_COPY first.
SdeSafeMode safe_mode() {
  Config cfg;
  cfg.load();
  std::string configured = cfg.get("files", "safe_copy", "auto");
  return sde_safe_mode(configured.c_str());
}

// The writers of one run of file operations, one for every destination
// file system (src/common/safe_copy.h): the safe writer for removable,
// external and network drives, the page cache for internal disks.
class Writers {
 public:
  struct Drive {
    SdeWriter writer;
    SdeDest dest;
    dev_t dev = 0;
    bool safe = false;
  };

  explicit Writers(FileProgress *progress) : progress_(progress), mode_(safe_mode()) {}
  ~Writers() {
    for (auto &d : drives_) sde_writer_abandon(&d->writer);
  }
  Writers(const Writers &) = delete;
  Writers &operator=(const Writers &) = delete;

  // The writer for files created in the folder |dir|, or nullptr.
  Drive *for_dir(const std::string &dir) {
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) return nullptr;
    for (auto &d : drives_)
      if (d->dev == st.st_dev) return d.get();
    auto d = std::make_unique<Drive>();
    d->dev = st.st_dev;
    d->safe = sde_dest_probe(dir.c_str(), &d->dest) == 0 && sde_dest_wants_safe(&d->dest, mode_);
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    sde_writer_init(&d->writer, sde_writer_method_for(&d->dest, d->safe ? 1 : 0), fd, g_test_ops, g_test_ops_data);
    d->writer.confirmed_cb = confirmed;
    d->writer.confirmed_data = progress_;
    if (progress_ && d->safe && !announced_) {
      // The dialog names the first drive written to safely.
      announced_ = true;
      progress_->removable = d->dest.kind == SDE_DEST_REMOVABLE ||
                             (d->dest.kind != SDE_DEST_NETWORK && d->dest.dos_names);
      progress_->fat = d->dest.fat != 0;
      std::lock_guard<std::mutex> lock(progress_->mutex);
      progress_->drive = d->dest.name;
    }
    drives_.push_back(std::move(d));
    return drives_.back().get();
  }

  // Everything written to |drive| so far is on the drive.
  bool checkpoint(Drive *drive, std::string *error) {
    int err = sde_writer_checkpoint(&drive->writer);
    if (err) {
      if (error) *error = drive_error(err, drive, "");
      return false;
    }
    return true;
  }

  // A removable drive that lost items (the sources of a move) has them
  // deleted on the drive too.
  void sync_source(const std::string &dir) {
    SdeDest dest;
    if (sde_dest_probe(dir.c_str(), &dest) != 0 || !sde_dest_wants_safe(&dest, mode_)) return;
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    syncfs(fd);
    fsync(fd);
    close(fd);
  }

  // The last checkpoints (FileProgress::FINISHING while they run); their
  // errors go to |errors|, the drives that failed to |failed|. Returns their
  // number.
  int finish(std::vector<std::string> *errors, std::vector<dev_t> *failed = nullptr) {
    bool safe = false;
    for (auto &d : drives_) safe = safe || d->safe;
    if (progress_ && safe) progress_->phase = FileProgress::FINISHING;
    int count = 0;
    for (auto &d : drives_) {
      int err = sde_writer_finish(&d->writer);
      if (err) {
        ++count;
        if (errors) errors->push_back(drive_error(err, d.get(), ""));
        if (failed) failed->push_back(d->dev);
      }
    }
    drives_.clear();
    return count;
  }

  // A message for |err| of the destination |drive| while writing |path|
  // ("" for the drive as a whole).
  static std::string drive_error(int err, const Drive *drive, const std::string &path) {
    std::string name = drive && drive->safe ? std::string(drive->dest.name) : std::string();
    std::string file = basename_of(path);
    switch (err) {
      case ENOSPC:
      case EDQUOT:
        if (!name.empty()) return format(_("There is not enough space on %s."), name.c_str());
        return format(_("There is not enough space for \"%s\"."), file.c_str());
      case EROFS:
        if (!name.empty()) return format(_("%s is read-only."), name.c_str());
        break;
      case EFBIG:
        return format(_("\"%s\" is too large for the file system of the destination."), file.c_str());
      case EIO:
      case ENODEV:
      case ENXIO:
        if (drive && drive->safe && drive->dest.kind == SDE_DEST_REMOVABLE && !name.empty())
          return format(_("Could not write to %s: the drive was removed or does not work."), name.c_str());
        break;
      default:
        break;
    }
    if (path.empty()) return format(_("Could not write to %s: %s"), name.empty() ? "?" : name.c_str(), strerror(err));
    return "Cannot write \"" + path + "\": " + strerror(err);
  }

  unsigned char *buffer() { return buffer_.data(); }
  size_t buffer_size() const { return buffer_.size(); }

 private:
  static void confirmed(void *data, long long bytes) {
    if (data) static_cast<FileProgress *>(data)->done += bytes;
  }

  FileProgress *progress_;
  SdeSafeMode mode_;
  bool announced_ = false;
  std::vector<std::unique_ptr<Drive>> drives_;
  std::vector<unsigned char> buffer_ = std::vector<unsigned char>(1 << 20);
};

// Pausing, Cancel and SDE_QA_COPY_RATE between the reads of a copy.
struct CopyHook {
  FileProgress *progress;
  SdeWriter *writer;
  long long throttled;
  static int stop(void *data) {
    auto *h = static_cast<CopyHook *>(data);
    if (qa_copy_rate() > 0 && h->writer->written > h->throttled) {
      usleep(useconds_t(1e6 * double(h->writer->written - h->throttled) / qa_copy_rate()));
      h->throttled = h->writer->written;
    }
    return h->progress && !h->progress->wait_while_paused() ? 1 : 0;
  }
};

// The user's extended attributes (tags, origins), where the destination
// has them; errors do not matter.
void copy_xattrs(int in, int out) {
  ssize_t size = flistxattr(in, nullptr, 0);
  if (size <= 0) return;
  std::vector<char> names(size_t(size) + 1);
  size = flistxattr(in, names.data(), names.size());
  for (ssize_t i = 0; i < size; i += ssize_t(strlen(names.data() + i)) + 1) {
    const char *name = names.data() + i;
    if (strncmp(name, "user.", 5) != 0) continue;
    ssize_t n = fgetxattr(in, name, nullptr, 0);
    if (n < 0 || n > 65536) continue;
    std::vector<char> value(size_t(n) + 1);
    n = fgetxattr(in, name, value.data(), value.size());
    if (n >= 0 && fsetxattr(out, name, value.data(), size_t(n), 0) != 0 && (errno == ENOTSUP || errno == EROFS))
      break;
  }
}

bool copy_file(const std::string &src, const std::string &dst, const struct stat &st,
               FileProgress *progress, Writers &writers, std::string *error) {
  Writers::Drive *drive = writers.for_dir(parent_of(dst));
  if (!drive) {
    fail(error, "Cannot create", dst, errno);
    return false;
  }
  if (drive->dest.fat && (long long)st.st_size > SDE_FAT_MAX_FILE_SIZE) {
    if (error) *error = Writers::drive_error(EFBIG, drive, src);
    return false;
  }
  int in = open(src.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (in < 0) {
    fail(error, "Cannot read", src, errno);
    return false;
  }
  int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777 & ~S_ISUID & ~S_ISGID);
  if (out < 0) {
    int err = errno;
    if (error)
      *error = err == ENOSPC || err == EROFS || err == EDQUOT ? Writers::drive_error(err, drive, dst)
                                                              : "Cannot create \"" + dst + "\": " + strerror(err);
    close(in);
    return false;
  }
  if (progress) progress->set_current(src);
  posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
  CopyHook hook{progress, &drive->writer, drive->writer.written};
  SdeCopyHooks hooks;
  hooks.stop = CopyHook::stop;
  hooks.data = &hook;
  int source_error = 0;
  int err = sde_copy_data(&drive->writer, in, out, st.st_size, writers.buffer(), writers.buffer_size(), &hooks,
                          &source_error);
  if (!err) copy_xattrs(in, out);
  close(in);
  if (close(out) != 0 && !err) err = errno;
  if (err) {
    if (err == ECANCELED) {
      if (error) *error = "Cancelled";
    } else if (source_error) {
      fail(error, "Cannot read", src, source_error);
    } else if (error) {
      *error = Writers::drive_error(err, drive, dst);
    }
    unlink(dst.c_str());
    return false;
  }
  keep_times(dst, st, false);
  // Every few seconds everything so far is made durable on the drive.
  err = sde_writer_file_done(&drive->writer);
  if (err) {
    if (error) *error = Writers::drive_error(err, drive, dst);
    return false;
  }
  return true;
}

bool copy_tree(const std::string &src, const std::string &dst, FileProgress *progress, Writers &writers,
               std::string *error) {
  struct stat st;
  if (lstat(src.c_str(), &st) != 0) {
    fail(error, "Cannot read", src, errno);
    return false;
  }
  if (progress && progress->cancel) {
    if (error) *error = "Cancelled";
    return false;
  }
  if (S_ISLNK(st.st_mode)) {
    std::vector<char> target(size_t(st.st_size > 0 ? st.st_size : 4096) + 1);
    ssize_t n = readlink(src.c_str(), target.data(), target.size() - 1);
    if (n < 0) {
      fail(error, "Cannot read", src, errno);
      return false;
    }
    target[size_t(n)] = 0;
    if (symlink(target.data(), dst.c_str()) != 0) {
      fail(error, "Cannot create", dst, errno);
      return false;
    }
    keep_times(dst, st, true);
    if (progress) progress->files_done++;
    return true;
  }
  if (S_ISDIR(st.st_mode)) {
    if (mkdir(dst.c_str(), 0700) != 0) {
      fail(error, "Cannot create", dst, errno);
      return false;
    }
    if (progress) progress->files_done++;
    DIR *d = opendir(src.c_str());
    if (!d) {
      fail(error, "Cannot read", src, errno);
      return false;
    }
    bool ok = true;
    while (struct dirent *e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if (!copy_tree(src + "/" + e->d_name, dst + "/" + e->d_name, progress, writers, error)) {
        ok = false;
        break;
      }
    }
    closedir(d);
    chmod(dst.c_str(), st.st_mode & 07777);
    keep_times(dst, st, false);
    return ok;
  }
  if (S_ISREG(st.st_mode)) {
    bool ok = copy_file(src, dst, st, progress, writers, error);
    if (ok && progress) progress->files_done++;
    return ok;
  }
  // FIFOs and device nodes: recreate them, like cp -a.
  if (mknod(dst.c_str(), st.st_mode, st.st_rdev) != 0) {
    fail(error, "Cannot create", dst, errno);
    return false;
  }
  if (progress) progress->files_done++;
  return true;
}

bool copy_path_with(const std::string &src, const std::string &dst, FileProgress *progress, Writers &writers,
                    std::string *error) {
  if (path_exists(dst)) {
    fail(error, "Already exists:", dst, EEXIST);
    return false;
  }
  if (path_inside(dst, src)) {
    if (error) *error = "Cannot copy a folder into itself: \"" + src + "\"";
    return false;
  }
  if (copy_tree(src, dst, progress, writers, error)) return true;
  remove_tree(dst);  // never leave half a copy behind
  return false;
}

bool move_path_with(const std::string &src, const std::string &dst, FileProgress *progress, Writers &writers,
                    std::string *error) {
  if (src == dst) return true;
  if (path_inside(dst, src)) {
    if (error) *error = "Cannot move a folder into itself: \"" + src + "\"";
    return false;
  }
  if (rename_noreplace(src, dst)) return true;  // counted by run_file_ops ()
  if (errno != EXDEV) {
    fail(error, "Cannot move", src, errno);
    return false;
  }
  // Another file system: copy everything, then remove the source, but only
  // once the copy is on the drive.
  if (!copy_path_with(src, dst, progress, writers, error)) return false;
  Writers::Drive *drive = writers.for_dir(parent_of(dst));
  if (drive && !writers.checkpoint(drive, error)) {
    remove_tree(dst);  // not reliably there: the source stays
    return false;
  }
  if (!remove_tree(src)) {
    if (error) *error = "Copied, but could not remove \"" + src + "\"";
    return false;
  }
  writers.sync_source(parent_of(src));
  return true;
}

}  // namespace

void set_test_writer_ops(const SdeWriterOps *ops, void *data) {
  g_test_ops = ops;
  g_test_ops_data = data;
}

bool FileProgress::wait_while_paused() {
  while (pause && !cancel) usleep(50000);
  return !cancel;
}

long count_items(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return 0;
  long n = 1;
  if (!S_ISDIR(st.st_mode)) return n;
  DIR *d = opendir(path.c_str());
  if (!d) return n;
  while (struct dirent *e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    n += count_items(path + "/" + e->d_name);
  }
  closedir(d);
  return n;
}

bool path_exists(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

bool same_filesystem(const std::string &a, const std::string &dir) {
  struct stat sa, sb;
  if (lstat(a.c_str(), &sa) != 0 || stat(dir.c_str(), &sb) != 0) return false;
  return sa.st_dev == sb.st_dev;
}

bool path_inside(const std::string &path, const std::string &dir) {
  if (dir.empty()) return false;
  std::string d = dir;
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  if (path == d) return true;
  return starts_with(path, d == "/" ? d : d + "/");
}

long long tree_size(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return 0;
  if (!S_ISDIR(st.st_mode)) return S_ISREG(st.st_mode) ? st.st_size : 0;
  long long total = 0;
  DIR *d = opendir(path.c_str());
  if (!d) return 0;
  while (struct dirent *e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    total += tree_size(path + "/" + e->d_name);
  }
  closedir(d);
  return total;
}

bool remove_tree(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path.c_str());
    if (!d) return false;
    bool ok = true;
    while (struct dirent *e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      ok = remove_tree(path + "/" + e->d_name) && ok;
    }
    closedir(d);
    return rmdir(path.c_str()) == 0 && ok;
  }
  return unlink(path.c_str()) == 0;
}

bool copy_path(const std::string &src, const std::string &dst, FileProgress *progress,
               std::string *error) {
  Writers writers(progress);
  bool ok = copy_path_with(src, dst, progress, writers, error);
  std::vector<std::string> errors;
  if (writers.finish(&errors) && ok) {
    if (error) *error = errors.front();
    ok = false;
  }
  return ok;
}

bool move_path(const std::string &src, const std::string &dst, FileProgress *progress,
               std::string *error) {
  Writers writers(progress);
  bool ok = move_path_with(src, dst, progress, writers, error);
  std::vector<std::string> errors;
  if (writers.finish(&errors) && ok) {
    if (error) *error = errors.front();
    ok = false;
  }
  return ok;
}

bool link_path(const std::string &src, const std::string &dst, std::string *error) {
  if (symlink(src.c_str(), dst.c_str()) != 0) {
    fail(error, "Cannot create", dst, errno);
    return false;
  }
  return true;
}

std::string home_trash_dir() { return data_home() + "/Trash"; }

std::string trash_info(const std::string &path, long long when) {
  // The Path key is URI-escaped ("file://" left out), per the spec.
  std::string escaped = uri_from_path(path).substr(7);
  char date[32];
  time_t t = time_t(when);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
  return "[Trash Info]\nPath=" + escaped + "\nDeletionDate=" + date + "\n";
}

bool trash_path(const std::string &path, const std::string &trash_dir, std::string *error) {
  std::string files = trash_dir + "/files", info = trash_dir + "/info";
  make_dirs(files);
  make_dirs(info);
  chmod(trash_dir.c_str(), 0700);
  if (!same_filesystem(path, files)) {
    if (error) *error = "Not on the home file system";
    return false;
  }
  std::string name = basename_of(path);
  struct Ctx {
    std::string files, info;
  } ctx{files, info};
  auto taken = [](const std::string &n, void *d) {
    auto *c = static_cast<Ctx *>(d);
    return path_exists(c->files + "/" + n) || path_exists(c->info + "/" + n + ".trashinfo");
  };
  struct stat st;
  bool is_dir = lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  if (taken(name, &ctx)) name = keep_both_name(name, is_dir, taken, &ctx);
  // The info file first (O_EXCL reserves the name), then the rename.
  std::string info_path = info + "/" + name + ".trashinfo";
  int fd = open(info_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    fail(error, "Cannot create", info_path, errno);
    return false;
  }
  std::string text = trash_info(path, (long long)time(nullptr));
  bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
  close(fd);
  if (!ok || !rename_noreplace(path, files + "/" + name)) {
    int err = errno;
    unlink(info_path.c_str());
    fail(error, "Cannot move to the trash", path, err);
    return false;
  }
  return true;
}

int run_file_ops(const std::vector<FileOp> &ops, FileProgress *progress,
                 std::vector<std::string> *errors) {
  // Bytes to copy and items: a copy (or a move to another drive) counts
  // every item inside, a rename, a link or a trashing one item.
  std::vector<bool> copies(ops.size(), false);
  for (size_t i = 0; i < ops.size(); ++i)
    copies[i] = ops[i].kind == FileOp::COPY ||
                (ops[i].kind == FileOp::MOVE && !same_filesystem(ops[i].src, parent_of(ops[i].dst)));
  if (progress) {
    long long total = 0;
    long items = 0;
    for (size_t i = 0; i < ops.size(); ++i) {
      if (copies[i]) {
        total += tree_size(ops[i].src);
        items += count_items(ops[i].src);
      } else {
        items += 1;
      }
    }
    progress->total = total;
    progress->files_total = items;
  }
  int failed = 0;
  Writers writers(progress);
  std::vector<std::pair<dev_t, std::string>> copied;  // complete copies, by drive
  for (size_t i = 0; i < ops.size(); ++i) {
    const FileOp &op = ops[i];
    if (progress && !progress->wait_while_paused()) break;
    // Every destination drive gets a writer: renames and links there are
    // made durable at the end too.
    Writers::Drive *drive = op.kind != FileOp::TRASH ? writers.for_dir(parent_of(op.dst)) : nullptr;
    if (progress) progress->set_current(op.src);
    std::string error, aside;
    bool ok = false;
    if (op.kind == FileOp::TRASH) {
      ok = trash_path(op.src, home_trash_dir(), &error);
    } else {
      if (op.replace && path_exists(op.dst)) {
        if (path_inside(op.dst, op.src) || path_inside(op.src, op.dst)) {
          error = "Cannot replace \"" + op.dst + "\" with an item inside it";
          if (errors) errors->push_back(error);
          ++failed;
          if (progress) progress->files_done += copies[i] ? count_items(op.src) : 1;
          continue;
        }
        // Keep the old item until the new one is complete.
        struct Ctx {
          std::string dir;
        } ctx{parent_of(op.dst)};
        auto taken = [](const std::string &n, void *d) {
          return path_exists(static_cast<Ctx *>(d)->dir + "/" + n);
        };
        aside = ctx.dir + "/" + keep_both_name("." + basename_of(op.dst) + ".sde-replaced", true, taken, &ctx);
        if (!rename_noreplace(op.dst, aside)) {
          fail(&error, "Cannot replace", op.dst, errno);
          if (errors) errors->push_back(error);
          ++failed;
          if (progress) progress->files_done += copies[i] ? count_items(op.src) : 1;
          continue;
        }
      }
      if (op.kind == FileOp::MOVE) ok = move_path_with(op.src, op.dst, progress, writers, &error);
      else if (op.kind == FileOp::COPY) ok = copy_path_with(op.src, op.dst, progress, writers, &error);
      else ok = link_path(op.src, op.dst, &error);
      if (!aside.empty()) {
        if (ok) remove_tree(aside);
        else rename_noreplace(aside, op.dst);
      }
    }
    if (!ok) {
      ++failed;
      if (errors) errors->push_back(error);
    } else if (op.kind == FileOp::COPY && drive) {
      copied.push_back({drive->dev, op.dst});
    }
    if (progress && !copies[i]) progress->files_done++;
    // A rename out of a removable drive: the drive loses the item too.
    if (ok && op.kind == FileOp::MOVE && !copies[i]) writers.sync_source(parent_of(op.src));
  }
  // Also after a Cancel: what was written must reach the drive. A drive
  // that failed at the end (pulled out) has no reliable copy: they go, as
  // a copy that fails on the way does.
  std::vector<dev_t> lost;
  writers.finish(errors, &lost);
  for (auto &c : copied)
    if (std::find(lost.begin(), lost.end(), c.first) != lost.end()) {
      remove_tree(c.second);
      ++failed;
    }
  if (progress) progress->phase = FileProgress::DONE;
  return failed;
}

}  // namespace desk
}  // namespace sde
