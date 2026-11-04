// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "trash.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <set>

#include "desktop_model.h"
#include "file_ops.h"
#include "file_chooser_model.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {

std::string take(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

bool rename_noreplace(const std::string &src, const std::string &dst) {
  int r = renameat2(AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(), RENAME_NOREPLACE);
  if (r != 0 && (errno == EINVAL || errno == ENOSYS)) {
    struct stat st;
    if (lstat(dst.c_str(), &st) == 0) {
      errno = EEXIST;
      return false;
    }
    r = rename(src.c_str(), dst.c_str());
  }
  return r == 0;
}

void set_error(std::string *error, const std::string &text) {
  if (error) *error = text;
}

// A trash folder that exists and that this user may use.
bool usable_dir(const std::string &dir) {
  struct stat st {};
  return lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() && access(dir.c_str(), W_OK) == 0;
}

bool make_private_dir(const std::string &dir) {
  if (mkdir(dir.c_str(), 0700) == 0) return true;
  return errno == EEXIST && usable_dir(dir);
}

// mountinfo escapes spaces as \040.
std::string unescape_octal(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3' && s[i + 2] >= '0' && s[i + 2] <= '7' &&
        s[i + 3] >= '0' && s[i + 3] <= '7') {
      out += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

// The topdir trash of |topdir| for this user; "" when it cannot have one.
std::string topdir_trash(const std::string &topdir, bool create) {
  std::string uid = std::to_string((unsigned long)getuid());
  std::string shared = (topdir == "/" ? "" : topdir) + "/.Trash";
  struct stat st {};
  // $topdir/.Trash: a real folder with the sticky bit (made by an administrator).
  if (lstat(shared.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX)) {
    std::string mine = shared + "/" + uid;
    if (usable_dir(mine) || (create && make_private_dir(mine))) return mine;
  }
  std::string own = (topdir == "/" ? "" : topdir) + "/.Trash-" + uid;
  if (usable_dir(own)) return own;
  if (create && make_private_dir(own)) return own;
  return "";
}

std::string escape_trash_path(const std::string &path) {
  return take(g_uri_escape_string(path.c_str(), "/", FALSE));
}

}  // namespace

std::string home_trash() { return data_home() + "/Trash"; }

std::string mount_point_of(const std::string &path_in) {
  std::string path = chooser::normalize_path(path_in);
  struct stat st {};
  if (lstat(path.c_str(), &st) != 0) return "";
  dev_t dev = st.st_dev;
  std::string cur = path;
  for (;;) {
    std::string up = chooser::parent_dir(cur);
    if (up.empty()) return cur;
    struct stat ps {};
    if (stat(up.c_str(), &ps) != 0 || ps.st_dev != dev) return cur;
    cur = up;
  }
}

std::string trash_for(const std::string &path, bool create, std::string *error) {
  std::string home = home_trash();
  struct stat file {}, hs {};
  if (lstat(path.c_str(), &file) != 0) {
    set_error(error, strerror(errno));
    return "";
  }
  std::string data = data_home();
  if (create) make_dirs(data);
  if (stat(data.c_str(), &hs) == 0 && hs.st_dev == file.st_dev) {
    if (create) {
      make_private_dir(home);
      make_private_dir(home + "/files");
      make_private_dir(home + "/info");
    }
    return home;
  }
  std::string top = mount_point_of(path);
  if (top.empty()) {
    set_error(error, _("The drive of this item has no trash."));
    return "";
  }
  std::string t = topdir_trash(top, create);
  if (t.empty()) {
    set_error(error, _("The drive of this item has no trash."));
    return "";
  }
  if (create) {
    make_private_dir(t + "/files");
    make_private_dir(t + "/info");
  }
  return t;
}

bool can_trash(const std::string &path) {
  struct stat file {}, hs {};
  if (lstat(path.c_str(), &file) != 0) return false;
  // GVfs shares (their FUSE folders): no trash there.
  std::string runtime = env("XDG_RUNTIME_DIR");
  if (!runtime.empty() && starts_with(path, runtime + "/gvfs/")) return false;
  std::string parent = chooser::parent_dir(path);
  if (parent.empty() || access(parent.c_str(), W_OK) != 0) return false;
  std::string data = data_home();
  while (!data.empty() && stat(data.c_str(), &hs) != 0) data = chooser::parent_dir(data);
  if (!data.empty() && hs.st_dev == file.st_dev) return true;
  std::string top = mount_point_of(path);
  if (top.empty()) return false;
  if (!topdir_trash(top, false).empty()) return true;
  return access(top.c_str(), W_OK) == 0;
}

std::vector<std::string> trash_folders(const std::string &mountinfo_in) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  auto add = [&](const std::string &t) {
    if (!t.empty() && dir_exists(t + "/files") && seen.insert(t).second) out.push_back(t);
  };
  add(home_trash());
  std::string mountinfo = mountinfo_in;
  if (mountinfo.empty()) read_file("/proc/self/mountinfo", mountinfo);
  for (const auto &line : split(mountinfo, '\n')) {
    std::vector<std::string> f = split(line, ' ');
    if (f.size() < 5) continue;
    std::string point = unescape_octal(f[4]);
    // Pseudo file systems have no trash and stat()ing them can be slow.
    if (starts_with(point, "/proc") || starts_with(point, "/sys") || starts_with(point, "/dev") ||
        starts_with(point, "/run/user/") || point == "/run" || starts_with(point, "/snap/"))
      continue;
    add(topdir_trash(point, false));
  }
  return out;
}

std::string trash_info_text(const std::string &original, time_t when, const std::string &topdir) {
  std::string p = original;
  if (!topdir.empty() && topdir != "/" && starts_with(original, topdir + "/")) p = original.substr(topdir.size() + 1);
  char date[32];
  struct tm tm {};
  localtime_r(&when, &tm);
  strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
  return "[Trash Info]\nPath=" + escape_trash_path(p) + "\nDeletionDate=" + date + "\n";
}

bool parse_trash_info(const std::string &text, const std::string &topdir, std::string &original, time_t &deleted) {
  bool header = false;
  original.clear();
  deleted = 0;
  for (auto line : split(text, '\n')) {
    line = trim(line);
    if (line == "[Trash Info]") {
      header = true;
      continue;
    }
    if (!header) continue;
    if (!line.empty() && line[0] == '[') break;
    if (starts_with(line, "Path=")) {
      std::string p = take(g_uri_unescape_string(line.substr(5).c_str(), nullptr));
      if (p.empty()) p = line.substr(5);
      if (!p.empty() && p[0] != '/') p = (topdir == "/" || topdir.empty() ? std::string("/") : topdir + "/") + p;
      original = chooser::normalize_path(p);
    } else if (starts_with(line, "DeletionDate=")) {
      struct tm tm {};
      if (strptime(line.substr(13).c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
        tm.tm_isdst = -1;
        deleted = mktime(&tm);
      }
    }
  }
  return header && !original.empty();
}

bool move_to_trash(const std::string &path, TrashItem *item, std::string *error) {
  std::string trash = trash_for(path, true, error);
  if (trash.empty()) return false;
  std::string files = trash + "/files", info = trash + "/info";
  std::string name = basename_of(path);
  struct Ctx {
    std::string files, info;
  } ctx{files, info};
  auto taken = [](const std::string &n, void *d) {
    auto *c = static_cast<Ctx *>(d);
    return desk::path_exists(c->files + "/" + n) || desk::path_exists(c->info + "/" + n + ".trashinfo");
  };
  struct stat st {};
  bool is_dir = lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  if (taken(name, &ctx)) name = desk::keep_both_name(name, is_dir, taken, &ctx);
  // The info file first (O_EXCL reserves the name), then the rename.
  std::string info_path = info + "/" + name + ".trashinfo";
  int fd = open(info_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    set_error(error, format(_("Cannot move “%s” to the trash: %s"), basename_of(path).c_str(), strerror(errno)));
    return false;
  }
  time_t now = time(nullptr);
  std::string top = trash == home_trash() ? std::string() : mount_point_of(path);
  std::string text = trash_info_text(path, now, top);
  bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
  close(fd);
  if (!ok || !rename_noreplace(path, files + "/" + name)) {
    int err = errno;
    unlink(info_path.c_str());
    set_error(error, format(_("Cannot move “%s” to the trash: %s"), basename_of(path).c_str(), strerror(err)));
    return false;
  }
  if (item) {
    item->trash = trash;
    item->name = name;
    item->original = path;
    item->deleted = now;
  }
  return true;
}

std::vector<TrashItem> list_trash(const std::vector<std::string> &folders) {
  std::vector<TrashItem> out;
  for (const auto &t : folders) {
    std::string top = t == home_trash() ? std::string() : mount_point_of(t);
    DIR *d = opendir((t + "/files").c_str());
    if (!d) continue;
    while (struct dirent *e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      TrashItem it;
      it.trash = t;
      it.name = e->d_name;
      std::string text;
      if (!read_file(it.info_path(), text) || !parse_trash_info(text, top, it.original, it.deleted)) {
        // An orphan without its info: still shown, restorable only by dragging.
        it.original = "";
        struct stat st {};
        if (lstat(it.files_path().c_str(), &st) == 0) it.deleted = st.st_ctime;
      }
      out.push_back(std::move(it));
    }
    closedir(d);
  }
  std::stable_sort(out.begin(), out.end(), [](const TrashItem &a, const TrashItem &b) { return a.deleted > b.deleted; });
  return out;
}

std::vector<TrashItem> list_trash() { return list_trash(trash_folders()); }

bool trash_item_of(const std::string &files_path, TrashItem &item) {
  size_t at = files_path.rfind("/files/");
  if (at == std::string::npos) return false;
  item.trash = files_path.substr(0, at);
  item.name = files_path.substr(at + 7);
  if (item.name.empty() || item.name.find('/') != std::string::npos) return false;
  std::string top = item.trash == home_trash() ? std::string() : mount_point_of(item.trash);
  std::string text;
  if (!read_file(item.info_path(), text)) {
    item.original.clear();
    item.deleted = 0;
    return true;
  }
  parse_trash_info(text, top, item.original, item.deleted);
  return true;
}

bool restore_from_trash(const TrashItem &item, const std::string &to_in, std::string *error, int *err) {
  std::string to = to_in.empty() ? item.original : to_in;
  if (err) *err = 0;
  if (to.empty()) {
    set_error(error, format(_("The original place of “%s” is not known."), item.name.c_str()));
    if (err) *err = ENOENT;
    return false;
  }
  if (desk::path_exists(to)) {
    set_error(error, format(_("“%s” already exists."), to.c_str()));
    if (err) *err = EEXIST;
    return false;
  }
  make_dirs(chooser::parent_dir(to));
  if (!rename_noreplace(item.files_path(), to)) {
    int e = errno;
    if (e == EXDEV) {
      // Another file system (the drive's folder was replaced by a mount): copy.
      std::string copy_error;
      if (!desk::copy_path(item.files_path(), to, nullptr, &copy_error)) {
        set_error(error, copy_error);
        if (err) *err = EXDEV;
        return false;
      }
      desk::remove_tree(item.files_path());
    } else {
      set_error(error, format(_("Cannot restore “%s”: %s"), basename_of(to).c_str(), strerror(e)));
      if (err) *err = e;
      return false;
    }
  }
  unlink(item.info_path().c_str());
  return true;
}

bool erase_from_trash(const TrashItem &item, std::string *error) {
  if (!desk::remove_tree(item.files_path())) {
    set_error(error, format(_("Cannot delete “%s”: %s"), item.name.c_str(), strerror(errno)));
    return false;
  }
  unlink(item.info_path().c_str());
  return true;
}

size_t trash_count(size_t limit) {
  size_t n = 0;
  for (const auto &t : trash_folders()) {
    DIR *d = opendir((t + "/files").c_str());
    if (!d) continue;
    while (struct dirent *e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if (++n >= limit) break;
    }
    closedir(d);
    if (n >= limit) break;
  }
  return n;
}

bool trash_is_empty() { return trash_count(1) == 0; }

}  // namespace liria
}  // namespace sde
