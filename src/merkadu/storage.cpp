// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "i18n.h"
#include "json.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

constexpr int kMaxDepth = 256;

struct Usage {
  dev_t dev = 0;
  long long bytes = 0;
  std::set<std::pair<dev_t, ino_t>> linked;  // files with more than one name, counted once
};

// Adds what the folder |fd| holds (and closes |fd|).
void add_usage(int fd, Usage &u, int depth) {
  DIR *d = fdopendir(fd);
  if (!d) {
    close(fd);
    return;
  }
  int dfd = ::dirfd(d);
  while (dirent *e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    struct stat st;
    if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (!S_ISDIR(st.st_mode) && st.st_nlink > 1 && !u.linked.insert({st.st_dev, st.st_ino}).second) continue;
    u.bytes += (long long)st.st_blocks * 512;
    if (S_ISDIR(st.st_mode) && st.st_dev == u.dev && depth < kMaxDepth) {
      int sub = openat(dfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (sub >= 0) add_usage(sub, u, depth + 1);
    }
  }
  closedir(d);
}

// Removes |name| in the folder |dir| and all it holds, never following a
// link and never leaving the file system |dev|.
bool remove_tree(int dir, const std::string &name, dev_t dev, int depth) {
  struct stat st;
  if (fstatat(dir, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
  if (!S_ISDIR(st.st_mode)) return unlinkat(dir, name.c_str(), 0) == 0 || errno == ENOENT;
  if (st.st_dev != dev || depth >= kMaxDepth) {
    errno = EXDEV;
    return false;
  }
  int fd = openat(dir, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  // Read-only folders (caches of compilers, for example) are made
  // writable first: they are the user's own.
  if ((st.st_mode & S_IRWXU) != S_IRWXU) fchmod(fd, st.st_mode | S_IRWXU);
  DIR *d = fdopendir(fd);
  if (!d) {
    close(fd);
    return false;
  }
  std::vector<std::string> names;
  while (dirent *e = readdir(d))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
  bool ok = true;
  for (const auto &n : names) ok = remove_tree(::dirfd(d), n, dev, depth + 1) && ok;
  closedir(d);
  if (!ok) return false;
  return unlinkat(dir, name.c_str(), AT_REMOVEDIR) == 0 || errno == ENOENT;
}

std::string link_target(const std::string &path) {
  char buf[4096];
  ssize_t n = readlink(path.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return "";
  return std::string(buf, size_t(n));
}

bool is_link(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

bool exists_no_follow(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

// The links are relative ("../.var/app"): they stay right when the home
// folder is renamed (Control Center can rename accounts).
std::string relative_target(const StoragePaths &p, const std::string &to) {
  std::string prefix = p.home + "/";
  if (starts_with(to, prefix) && p.folder.rfind('/') == p.home.size()) return "../" + to.substr(prefix.size());
  return to;
}

// snapd's data folder of this user: the hidden one when snapd uses it.
std::string snap_folder(const StoragePaths &p) {
  if (dir_exists(p.snap_hidden)) return p.snap_hidden;
  if (dir_exists(p.snap_data)) return p.snap_data;
  return "";
}

// A link at |path| to |target|: made, or corrected when it points
// elsewhere. Something that is not a link stays (the user's own).
bool place_link(const std::string &path, const std::string &target, std::string &error) {
  if (exists_no_follow(path)) {
    if (!is_link(path)) return true;
    if (link_target(path) == target) return true;
    if (unlink(path.c_str()) != 0) {
      error = format("%s: %s", path.c_str(), strerror(errno));
      return false;
    }
  }
  if (symlink(target.c_str(), path.c_str()) != 0) {
    error = format("%s: %s", path.c_str(), strerror(errno));
    return false;
  }
  return true;
}

bool our_readme(const std::string &path, const std::string &readme) {
  std::string text;
  if (!read_file(path, text)) return false;
  // Merkadu's, in whatever language it was written in: its first line.
  std::string first = readme.substr(0, readme.find('\n'));
  return text == readme || (!first.empty() && starts_with(text, first));
}

}  // namespace

StoragePaths storage_paths(const std::string &folder_name) {
  StoragePaths p;
  p.home = home_dir();
  while (p.home.size() > 1 && p.home.back() == '/') p.home.pop_back();
  p.flatpak_data = join_path(p.home, ".var/app");
  p.snap_data = join_path(p.home, "snap");
  p.snap_hidden = join_path(p.home, ".snap/data");
  std::string name = trim(folder_name);
  if (name.empty() || name.find('/') != std::string::npos || name[0] == '.') name = "Merkadu";
  p.folder = join_path(p.home, name);
  return p;
}

long long disk_usage(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return -1;
  Usage u;
  u.dev = st.st_dev;
  u.bytes = (long long)st.st_blocks * 512;
  if (S_ISDIR(st.st_mode)) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) add_usage(fd, u, 0);
  }
  return u.bytes;
}

std::vector<DataDir> scan_app_data(const StoragePaths &p, const std::set<std::string> &flatpak_ids,
                                   bool flatpak_known, const std::set<std::string> &snap_names, bool snap_known,
                                   bool measure) {
  std::vector<DataDir> out;
  auto scan = [&](Source s, const std::string &dir, const std::set<std::string> &ids, bool known) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto &name : names) {
      std::string path = join_path(dir, name);
      struct stat st;
      // Links and files are not data of an app.
      if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
      DataDir dd;
      dd.source = s;
      dd.id = name;
      dd.path = path;
      dd.installed = !known || ids.count(name) > 0;
      if (measure) dd.size = disk_usage(path);
      out.push_back(std::move(dd));
    }
  };
  scan(Source::Flatpak, p.flatpak_data, flatpak_ids, flatpak_known);
  // While snapd moves a snap to the hidden folder it can be in both.
  scan(Source::Snap, p.snap_hidden, snap_names, snap_known);
  scan(Source::Snap, p.snap_data, snap_names, snap_known);
  return out;
}

bool remove_app_data(const StoragePaths &p, const std::string &path, std::string &error) {
  size_t slash = path.rfind('/');
  std::string parent = slash == std::string::npos ? "" : path.substr(0, slash);
  std::string name = slash == std::string::npos ? "" : path.substr(slash + 1);
  if (name.empty() || name == "." || name == ".." ||
      (parent != p.flatpak_data && parent != p.snap_data && parent != p.snap_hidden)) {
    error = format(_("%s is not a folder of app data."), path.c_str());
    return false;
  }
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return true;  // gone already
  if (!S_ISDIR(st.st_mode)) {
    error = format(_("%s is not a folder of app data."), path.c_str());
    return false;
  }
  int dir = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) {
    error = format("%s: %s", parent.c_str(), strerror(errno));
    return false;
  }
  bool ok = remove_tree(dir, name, st.st_dev, 0);
  int saved = errno;
  close(dir);
  if (!ok) error = format("%s: %s", path.c_str(), strerror(saved));
  return ok;
}

FolderState folder_state(const StoragePaths &p) {
  FolderState f;
  struct stat st;
  if (stat(p.folder.c_str(), &st) == 0) {
    f.exists = S_ISDIR(st.st_mode);
    f.blocked = !f.exists;
  } else if (exists_no_follow(p.folder)) {
    f.blocked = true;  // a broken link
  }
  f.snap_target = snap_folder(p);
  if (!f.exists) return f;
  f.flatpak = link_target(join_path(p.folder, "Flatpak")) == relative_target(p, p.flatpak_data);
  f.snap = !f.snap_target.empty() && link_target(join_path(p.folder, "Snap")) == relative_target(p, f.snap_target);
  return f;
}

bool make_folder(const StoragePaths &p, const std::string &readme_name, const std::string &readme,
                 std::string &error) {
  FolderState f = folder_state(p);
  if (f.blocked) {
    error = format(_("%s is already taken by a file."), p.folder.c_str());
    return false;
  }
  if (!f.exists && mkdir(p.folder.c_str(), 0755) != 0) {
    error = format("%s: %s", p.folder.c_str(), strerror(errno));
    return false;
  }
  // Flatpak makes ~/.var/app when an app first runs; made now, the link
  // is never broken.
  if (!dir_exists(p.flatpak_data) && !make_dirs(p.flatpak_data)) {
    error = format("%s: %s", p.flatpak_data.c_str(), strerror(errno));
    return false;
  }
  if (!place_link(join_path(p.folder, "Flatpak"), relative_target(p, p.flatpak_data), error)) return false;
  // snapd's folder is never made here (with the hidden snap folder there is
  // no ~/snap, as asked): the link comes once snapd made one.
  std::string snap = join_path(p.folder, "Snap");
  if (!f.snap_target.empty()) {
    if (!place_link(snap, relative_target(p, f.snap_target), error)) return false;
  } else if (is_link(snap)) {
    unlink(snap.c_str());
  }
  if (!readme_name.empty() && !readme.empty()) {
    std::string path = join_path(p.folder, readme_name);
    std::string text;
    if ((!read_file(path, text) || (text != readme && our_readme(path, readme))) &&
        !write_file_atomic(path, readme)) {
      error = format("%s: %s", path.c_str(), strerror(errno));
      return false;
    }
  }
  return true;
}

bool remove_folder(const StoragePaths &p, const std::string &readme_name, const std::string &readme,
                   std::string &error) {
  FolderState f = folder_state(p);
  if (!f.exists) return true;
  for (const char *name : {"Flatpak", "Snap"}) {
    std::string path = join_path(p.folder, name);
    if (is_link(path) && unlink(path.c_str()) != 0) {
      error = format("%s: %s", path.c_str(), strerror(errno));
      return false;
    }
  }
  if (!readme_name.empty()) {
    std::string path = join_path(p.folder, readme_name);
    if (our_readme(path, readme)) unlink(path.c_str());
  }
  // Kept when the user put something of their own in it.
  rmdir(p.folder.c_str());
  return true;
}

bool parse_snap_revisions(const std::string &json, std::vector<SnapRevision> &out) {
  Json root;
  if (!parse_json(json, root) || root.text("type") != "sync") return false;
  const Json *result = root.get("result");
  if (!result || !result->is_array()) return false;
  for (auto &s : result->items) {
    SnapRevision r;
    r.name = s.text("name");
    r.title = s.text("title");
    if (r.title.empty()) r.title = r.name;
    r.revision = s.text("revision");
    if (r.revision.empty()) {
      // Older snapd: a number.
      if (const Json *rev = s.get("revision"))
        if (rev->type == Json::Number) r.revision = std::to_string((long long)rev->integer);
    }
    r.version = s.text("version");
    r.active = s.text("status") == "active";
    if (s.get("installed-size")) r.size = (long long)s.number("installed-size");
    if (!r.name.empty() && !r.revision.empty()) out.push_back(std::move(r));
  }
  return true;
}

}  // namespace merkadu
}  // namespace sde
