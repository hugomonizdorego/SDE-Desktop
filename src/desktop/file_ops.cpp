// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "desktop_model.h"
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

bool copy_file(const std::string &src, const std::string &dst, const struct stat &st,
               FileProgress *progress, std::string *error) {
  int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    fail(error, "Cannot read", src, errno);
    return false;
  }
  int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777 & ~S_ISUID & ~S_ISGID);
  if (out < 0) {
    fail(error, "Cannot create", dst, errno);
    close(in);
    return false;
  }
  if (progress) progress->set_current(src);
  static const size_t kBuf = 1 << 20;
  std::vector<char> buf(kBuf);
  bool ok = true;
  for (;;) {
    if (progress && !progress->wait_while_paused()) {
      ok = false;
      if (error) *error = "Cancelled";
      break;
    }
    ssize_t n = read(in, buf.data(), kBuf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      fail(error, "Cannot read", src, errno);
      ok = false;
      break;
    }
    if (n == 0) break;
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(out, buf.data() + off, size_t(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        fail(error, "Cannot write", dst, errno ? errno : ENOSPC);
        ok = false;
        break;
      }
      off += w;
    }
    if (!ok) break;
    if (progress) progress->done += n;
    if (qa_copy_rate() > 0) usleep(useconds_t(1e6 * double(n) / qa_copy_rate()));
  }
  close(in);
  if (ok && fsync(out) != 0 && errno != EINVAL) {
    fail(error, "Cannot write", dst, errno);
    ok = false;
  }
  if (close(out) != 0 && ok) {
    fail(error, "Cannot write", dst, errno);
    ok = false;
  }
  if (!ok) {
    unlink(dst.c_str());
    return false;
  }
  keep_times(dst, st, false);
  return true;
}

bool copy_tree(const std::string &src, const std::string &dst, FileProgress *progress,
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
      if (!copy_tree(src + "/" + e->d_name, dst + "/" + e->d_name, progress, error)) {
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
    bool ok = copy_file(src, dst, st, progress, error);
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

}  // namespace

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
  if (path_exists(dst)) {
    fail(error, "Already exists:", dst, EEXIST);
    return false;
  }
  if (path_inside(dst, src)) {
    if (error) *error = "Cannot copy a folder into itself: \"" + src + "\"";
    return false;
  }
  if (copy_tree(src, dst, progress, error)) return true;
  remove_tree(dst);  // never leave half a copy behind
  return false;
}

bool move_path(const std::string &src, const std::string &dst, FileProgress *progress,
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
  // Another file system: copy everything, then remove the source.
  if (!copy_path(src, dst, progress, error)) return false;
  if (!remove_tree(src)) {
    if (error) *error = "Copied, but could not remove \"" + src + "\"";
    return false;
  }
  return true;
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
  for (size_t i = 0; i < ops.size(); ++i) {
    const FileOp &op = ops[i];
    if (progress && !progress->wait_while_paused()) break;
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
      if (op.kind == FileOp::MOVE) ok = move_path(op.src, op.dst, progress, &error);
      else if (op.kind == FileOp::COPY) ok = copy_path(op.src, op.dst, progress, &error);
      else ok = link_path(op.src, op.dst, &error);
      if (!aside.empty()) {
        if (ok) remove_tree(aside);
        else rename_noreplace(aside, op.dst);
      }
    }
    if (!ok) {
      ++failed;
      if (errors) errors->push_back(error);
    }
    if (progress && !copies[i]) progress->files_done++;
  }
  return failed;
}

}  // namespace desk
}  // namespace sde
