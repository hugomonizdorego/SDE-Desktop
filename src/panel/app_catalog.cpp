// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_catalog.h"

#include <FL/Fl.H>
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

#include "icons.h"
#include "menu_perf.h"
#include "util.h"

using namespace sde;

namespace app_catalog {

const DesktopEntry *Snapshot::find(const std::string &id) const {
  auto it = by_id.find(id);
  return it == by_id.end() ? nullptr : &apps[it->second];
}

namespace {

// One .desktop file found in an applications directory.
struct File {
  std::string id, path;
  long long mtime_ns = 0, size = 0, ino = 0;
};

// The parsed file, kept to skip parsing it again while it is unchanged.
struct Parsed {
  std::string id;
  long long mtime_ns = -1, size = -1, ino = -1;
  bool ok = false;
  DesktopEntry entry;
  menu::SearchKeys keys;
};

const unsigned kDirMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                          IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
const unsigned kParentMask = IN_CREATE | IN_MOVED_TO | IN_ONLYDIR;
const double kSettle = 0.3;      // seconds of quiet before a refresh
const double kSliceMs = 4.0;     // parsing budget per idle slice

struct State {
  SnapshotPtr snapshot;
  std::unordered_map<std::string, Parsed> parsed;  // by path
  std::string fingerprint;
  unsigned generation = 0;
  // The first load, in slices.
  bool loading = false;
  std::vector<File> files;
  size_t next = 0;
  // inotify: applications directories, and data directories without one.
  int fd = -1;
  std::unordered_map<int, std::string> dirs, parents;
  std::string stamp;  // applications_stamp() of the last refresh (fallback)
  std::vector<std::pair<void (*)(void *), void *>> listeners;
};

State &state() {
  static State s;
  return s;
}

long long mtime_ns(const struct stat &st) {
  return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

// As load_applications(): every data directory in XDG precedence order,
// sub-directories as "sub-name.desktop", the first file of an ID winning.
void scan_dir(const std::string &dir, const std::string &prefix, std::map<std::string, File> &found,
              std::vector<std::string> &dirs, int depth = 0) {
  if (depth > 8) return;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  dirs.push_back(dir);
  std::vector<std::pair<std::string, unsigned char>> names;
  while (dirent *e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    names.push_back({e->d_name, e->d_type});
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (auto &n : names) {
    std::string full = dir + "/" + n.first;
    struct stat st;
    bool have = false, is_dir = n.second == DT_DIR;
    if (n.second == DT_UNKNOWN || n.second == DT_LNK) {
      have = stat(full.c_str(), &st) == 0;
      is_dir = have && S_ISDIR(st.st_mode);
    }
    if (is_dir) {
      scan_dir(full, prefix + n.first + "-", found, dirs, depth + 1);
      continue;
    }
    if (!ends_with(n.first, ".desktop")) continue;
    std::string id = prefix + n.first;
    if (found.count(id)) continue;
    if (!have && stat(full.c_str(), &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    found[id] = File{id, full, mtime_ns(st), (long long)st.st_size, (long long)st.st_ino};
  }
}

std::vector<File> scan(std::vector<std::string> &dirs) {
  std::map<std::string, File> found;
  for (auto &d : data_dirs()) scan_dir(d + "/applications", "", found, dirs);
  std::vector<File> out;
  out.reserve(found.size());
  for (auto &kv : found) out.push_back(std::move(kv.second));
  return out;
}

// Reads |f| unless the copy from an earlier scan is still current.
void parse(State &s, const File &f) {
  Parsed &p = s.parsed[f.path];
  if (p.ok && p.id == f.id && p.mtime_ns == f.mtime_ns && p.size == f.size && p.ino == f.ino)
    return;
  if (!p.ok && p.id == f.id && p.mtime_ns == f.mtime_ns && p.size == f.size && p.ino == f.ino)
    return;  // a broken file, unchanged
  p = Parsed();
  p.id = f.id;
  p.mtime_ns = f.mtime_ns;
  p.size = f.size;
  p.ino = f.ino;
  // Huge files are no desktop entries (and must not stall the panel).
  if (f.size > (1 << 20)) return;
  p.ok = p.entry.load(f.path, f.id);
  if (!p.ok) return;
  // Names are drawn on one line: no control characters, valid UTF-8.
  for (std::string *t : {&p.entry.name, &p.entry.generic_name, &p.entry.comment}) {
    *t = menu::clean_text(*t);
  }
  p.keys = menu::search_keys(p.entry);
}

void notify(State &s) {
  // New applications may have brought their icons.
  IconLoader::forget_missing();
  auto listeners = s.listeners;
  for (auto &l : listeners) l.first(l.second);
}

// Builds and publishes the snapshot of |files| (all parsed). Returns true
// when the applications changed.
bool publish(State &s, const std::vector<File> &files) {
  // Forget files that are gone.
  std::set<std::string> present;
  for (auto &f : files) present.insert(f.path);
  for (auto it = s.parsed.begin(); it != s.parsed.end();)
    it = present.count(it->first) ? std::next(it) : s.parsed.erase(it);
  std::vector<DesktopEntry> entries;
  entries.reserve(files.size());
  std::vector<const Parsed *> sources;
  for (auto &f : files) {
    auto it = s.parsed.find(f.path);
    if (it == s.parsed.end() || !it->second.ok) continue;
    entries.push_back(it->second.entry);
    sources.push_back(&it->second);
  }
  // Which entries load_applications() would list (NoDisplay, OnlyShowIn,
  // TryExec, the SDE suite replacing upstream programs...).
  filter_applications(entries, "SDE");
  std::set<std::string> keep;
  for (auto &e : entries) keep.insert(e.id);
  struct Row {
    const Parsed *p;
    std::string letter, sort;
    bool symbol;
  };
  std::vector<Row> rows;
  for (auto *p : sources)
    if (keep.count(p->entry.id)) {
      std::string l = menu::index_letter(p->entry.name);
      rows.push_back({p, l, lower(p->entry.name), l == "#"});
    }
  // The menu's A-Z order (menu::alphabetical()).
  std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    if (a.symbol != b.symbol) return a.symbol;
    if (a.letter != b.letter) return a.letter < b.letter;
    return a.sort < b.sort;
  });
  std::string fp;
  for (auto &r : rows) {
    fp += r.p->entry.id;
    fp += '\t';
    fp += r.p->entry.path;
    fp += '\t';
    fp += std::to_string(r.p->mtime_ns) + '\t' + std::to_string(r.p->size) + '\n';
  }
  if (s.snapshot && fp == s.fingerprint) return false;
  auto snap = std::make_shared<Snapshot>();
  snap->generation = ++s.generation;
  snap->apps.reserve(rows.size());
  for (auto &r : rows) {
    snap->apps.push_back(r.p->entry);
    snap->keys.push_back(r.p->keys);
    snap->letters.push_back(r.letter);
    snap->category.push_back(r.p->entry.menu_category());
    snap->mtime.push_back(r.p->mtime_ns / 1000000000LL);
  }
  for (size_t i = 0; i < snap->apps.size(); ++i) snap->by_id.emplace(snap->apps[i].id, i);
  for (auto &c : menu_categories())
    if (std::find(snap->category.begin(), snap->category.end(), c) != snap->category.end())
      snap->categories.push_back(c);
  s.fingerprint = fp;
  s.snapshot = snap;
  return true;
}

void watch(State &s, const std::vector<std::string> &dirs) {
  if (s.fd < 0) return;
  std::set<std::string> want(dirs.begin(), dirs.end());
  for (auto &kv : s.dirs) want.erase(kv.second);
  for (auto &d : want) {
    int wd = inotify_add_watch(s.fd, d.c_str(), kDirMask);
    if (wd >= 0) s.dirs[wd] = d;
  }
  // A data directory without applications: wait for them to appear.
  for (auto &d : data_dirs()) {
    std::string apps = d + "/applications";
    bool watched = false;
    for (auto &kv : s.dirs) watched = watched || kv.second == apps;
    bool parent = false;
    for (auto &kv : s.parents) parent = parent || kv.second == d;
    if (watched || parent || !dir_exists(d)) continue;
    int wd = inotify_add_watch(s.fd, d.c_str(), kParentMask);
    if (wd >= 0) s.parents[wd] = d;
  }
}

void finish_load(State &s) {
  while (s.next < s.files.size()) parse(s, s.files[s.next++]);
  s.loading = false;
  bool changed = publish(s, s.files);
  s.files.clear();
  s.next = 0;
  if (changed) notify(s);
}

void slice(void *) {
  State &s = state();
  if (!s.loading) return;
  double end = menu_perf::now_ms() + kSliceMs;
  while (s.next < s.files.size() && menu_perf::now_ms() < end) parse(s, s.files[s.next++]);
  if (s.next < s.files.size()) {
    Fl::repeat_timeout(0, slice);
    return;
  }
  finish_load(s);
}

void begin_load() {
  State &s = state();
  if (s.snapshot || s.loading) return;
  std::vector<std::string> dirs;
  s.stamp = applications_stamp();
  s.files = scan(dirs);
  s.next = 0;
  s.loading = true;
  watch(s, dirs);
  Fl::add_timeout(0, slice);
}

void refresh_timeout(void *) { refresh(); }

void on_events(int fd, void *) {
  State &s = state();
  alignas(struct inotify_event) char buf[8192];
  bool relevant = false;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) break;
    for (char *p = buf; p < buf + n;) {
      auto *ev = reinterpret_cast<struct inotify_event *>(p);
      p += sizeof(struct inotify_event) + ev->len;
      std::string name = ev->len ? std::string(ev->name) : std::string();
      if (ev->mask & IN_Q_OVERFLOW) {
        relevant = true;
        continue;
      }
      if (s.parents.count(ev->wd)) {
        if (ev->mask & IN_IGNORED) s.parents.erase(ev->wd);
        else if (name == "applications") relevant = true;
        continue;
      }
      if (ev->mask & IN_IGNORED) {
        s.dirs.erase(ev->wd);
        relevant = true;
        continue;
      }
      if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) || (ev->mask & IN_ISDIR) ||
          ends_with(name, ".desktop"))
        relevant = true;
    }
  }
  if (relevant) {
    // Package managers write many files: refresh once they are quiet.
    Fl::remove_timeout(refresh_timeout);
    Fl::add_timeout(kSettle, refresh_timeout);
  }
}

}  // namespace

SnapshotPtr current() {
  State &s = state();
  if (!s.snapshot) {
    if (!s.loading) {
      std::vector<std::string> dirs;
      s.stamp = applications_stamp();
      s.files = scan(dirs);
      s.next = 0;
      s.loading = true;
      watch(s, dirs);
    }
    Fl::remove_timeout(slice);
    finish_load(s);
    if (!s.snapshot) s.snapshot = std::make_shared<Snapshot>();
  }
  return s.snapshot;
}

bool ready() { return state().snapshot != nullptr; }

void start() {
  State &s = state();
  if (s.fd < 0) {
    s.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s.fd >= 0) Fl::add_fd(s.fd, FL_READ, on_events, nullptr);
  }
  // A moment after the panel is up, so that its first frames come first.
  Fl::add_timeout(0.4, [](void *) { begin_load(); });
}

bool watching() { return state().fd >= 0; }

void poll() {
  State &s = state();
  if (s.fd >= 0 || !s.snapshot) return;
  std::string stamp = applications_stamp();
  if (stamp != s.stamp) refresh();
}

void refresh() {
  State &s = state();
  Fl::remove_timeout(refresh_timeout);
  if (s.loading || !s.snapshot) {
    current();
    return;
  }
  double t0 = menu_perf::now_ms();
  std::vector<std::string> dirs;
  s.stamp = applications_stamp();
  std::vector<File> files = scan(dirs);
  for (auto &f : files) parse(s, f);
  bool changed = publish(s, files);
  watch(s, dirs);
  if (menu_perf::enabled())
    menu_perf::log("catalog refresh ms=" + std::to_string(menu_perf::now_ms() - t0) +
                   " apps=" + std::to_string(s.snapshot->apps.size()) +
                   " changed=" + std::to_string(changed ? 1 : 0));
  if (changed) notify(s);
}

void subscribe(void (*fn)(void *), void *data) { state().listeners.push_back({fn, data}); }

void unsubscribe(void *data) {
  auto &l = state().listeners;
  l.erase(std::remove_if(l.begin(), l.end(), [&](auto &x) { return x.second == data; }), l.end());
}

}  // namespace app_catalog
