// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "file_chooser_model.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>

#include "i18n.h"
#include "util.h"

namespace sde {
namespace chooser {

namespace {

std::string take_gstr(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

bool is_dot_dir(const char *name) { return name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])); }

// A sort key that ignores case in every locale (the C locale's byte order
// would put "Zeta" before "alpha") and orders numbers naturally.
std::string name_key(const std::string &display) {
  gchar *folded = g_utf8_casefold(display.c_str(), -1);
  std::string key = take_gstr(g_utf8_collate_key_for_filename(folded, -1));
  g_free(folded);
  return key;
}

void fill_names(Entry &e) {
  e.display = take_gstr(g_filename_display_name(e.name.c_str()));
  e.key = name_key(e.display);
  e.fold = fold_text(e.display);
}

std::string join(const std::string &dir, const std::string &name) {
  if (dir.empty()) return name;
  return dir.back() == '/' ? dir + name : dir + "/" + name;
}

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ------------------------------------------------------------- entries

std::string fold_text(const std::string &text) {
  if (text.empty()) return text;
  std::string valid = g_utf8_validate(text.c_str(), -1, nullptr) ? text : take_gstr(g_utf8_make_valid(text.c_str(), -1));
  gchar *folded = g_utf8_casefold(valid.c_str(), -1);
  gchar *decomposed = g_utf8_normalize(folded, -1, G_NORMALIZE_ALL);
  g_free(folded);
  if (!decomposed) return valid;
  std::string out;
  out.reserve(strlen(decomposed));
  for (const gchar *p = decomposed; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    GUnicodeType t = g_unichar_type(c);
    if (t == G_UNICODE_NON_SPACING_MARK || t == G_UNICODE_SPACING_MARK || t == G_UNICODE_ENCLOSING_MARK) continue;
    out.append(p, size_t(g_utf8_next_char(p) - p));
  }
  g_free(decomposed);
  return out;
}

bool read_entry(int dirfd, const char *name, const std::string &dir, Entry &out) {
  struct stat st {};
  if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return false;
  Entry e;
  e.name = name;
  e.path = join(dir, e.name);
  e.link = S_ISLNK(st.st_mode);
  if (e.link) {
    struct stat target {};
    if (fstatat(dirfd, name, &target, 0) == 0) st = target;
    else e.broken = true;
  }
  e.dir = S_ISDIR(st.st_mode);
  e.size = S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
  e.mtime = st.st_mtime;
  size_t n = e.name.size();
  e.hidden = name[0] == '.' || (n > 1 && name[n - 1] == '~');
  fill_names(e);
  if (e.dir) e.mime = "inode/directory";
  else if (e.broken) e.mime = "inode/symlink";
  else if (S_ISSOCK(st.st_mode)) e.mime = "inode/socket";
  else if (S_ISFIFO(st.st_mode)) e.mime = "inode/fifo";
  else if (S_ISCHR(st.st_mode)) e.mime = "inode/chardevice";
  else if (S_ISBLK(st.st_mode)) e.mime = "inode/blockdevice";
  else e.mime = guess_mime(e.name, false);
  out = std::move(e);
  return true;
}

bool entry_for_path(const std::string &path, Entry &out) {
  std::string dir = parent_dir(path);
  if (dir.empty()) return false;
  std::string name = path.substr(path.rfind('/') + 1);
  int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = read_entry(fd, name.c_str(), dir, out);
  close(fd);
  return ok;
}

std::set<std::string> hidden_names(const std::string &dir) {
  std::set<std::string> out;
  std::string text;
  if (!read_file(join(dir, ".hidden"), text)) return out;
  for (auto &line : split(text, '\n')) {
    std::string n = line;
    if (!n.empty() && n.back() == '\r') n.pop_back();
    if (!n.empty()) out.insert(n);
  }
  return out;
}

bool list_directory(const std::string &dir, std::vector<Entry> &out, std::string *error) {
  out.clear();
  DIR *d = opendir(dir.c_str());
  if (!d) {
    if (error) *error = strerror(errno);
    return false;
  }
  std::set<std::string> hidden = hidden_names(dir);
  int fd = dirfd(d);
  while (struct dirent *de = readdir(d)) {
    if (is_dot_dir(de->d_name)) continue;
    Entry e;
    if (!read_entry(fd, de->d_name, dir, e)) continue;
    if (hidden.count(e.name)) e.hidden = true;
    out.push_back(std::move(e));
  }
  closedir(d);
  return true;
}

std::string sort_key_id(SortKey key) {
  switch (key) {
    case SortKey::Size: return "size";
    case SortKey::Modified: return "modified";
    case SortKey::Type: return "type";
    default: return "name";
  }
}

SortKey sort_key_from_id(const std::string &id) {
  if (id == "size") return SortKey::Size;
  if (id == "modified") return SortKey::Modified;
  if (id == "type") return SortKey::Type;
  return SortKey::Name;
}

bool entry_less(const Entry &a, const Entry &b, const SortOrder &order) {
  if (order.folders_first && a.dir != b.dir) return a.dir;
  int c = 0;
  switch (order.key) {
    case SortKey::Size:
      c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
      break;
    case SortKey::Modified:
      c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0;
      break;
    case SortKey::Type:
      if (a.dir != b.dir) c = a.dir ? -1 : 1;
      else c = a.mime.compare(b.mime);
      break;
    case SortKey::Name:
      break;
  }
  if (c == 0) c = a.key.compare(b.key);
  if (c == 0) c = a.name.compare(b.name);
  if (c == 0) c = a.path.compare(b.path);
  return order.descending ? c > 0 : c < 0;
}

void sort_entries(std::vector<Entry> &entries, const SortOrder &order) {
  std::stable_sort(entries.begin(), entries.end(),
                   [&](const Entry &a, const Entry &b) { return entry_less(a, b, order); });
}

bool matches_query(const Entry &e, const std::string &query) {
  if (query.empty()) return true;
  const std::string &hay = e.fold.empty() && !e.display.empty() ? fold_text(e.display) : e.fold;
  return hay.find(fold_text(query)) != std::string::npos;
}

// ------------------------------------------------------------- filters

bool glob_match(const std::string &pattern, const std::string &name) {
  if (pattern == "*") return true;
  return fnmatch(pattern.c_str(), name.c_str(), FNM_CASEFOLD | FNM_PERIOD) == 0 ||
         fnmatch(pattern.c_str(), name.c_str(), FNM_CASEFOLD) == 0;
}

std::vector<std::string> expand_braces(const std::string &pattern) {
  size_t open = pattern.find('{');
  if (open == std::string::npos) return {pattern};
  size_t close = pattern.find('}', open);
  if (close == std::string::npos) return {pattern};
  std::vector<std::string> out;
  std::string head = pattern.substr(0, open), tail = pattern.substr(close + 1);
  for (const auto &alt : split(pattern.substr(open + 1, close - open - 1), ',', true))
    for (const auto &rest : expand_braces(head + alt + tail)) out.push_back(rest);
  return out;
}

namespace {

// Spellings of one extension: the first is the one a saved file gets.
const std::vector<std::vector<std::string>> kSameExtension = {
    {"jpg", "jpeg", "jpe", "jfif"}, {"tif", "tiff"}, {"html", "htm"}, {"yaml", "yml"},
    {"mpg", "mpeg"}, {"md", "markdown"}, {"svg", "svgz"}, {"txt", "text"}};

std::string canonical_extension(const std::string &ext) {
  for (const auto &group : kSameExtension)
    for (const auto &e : group)
      if (e == ext) return group[0];
  return ext;
}

// "*.png" -> "png"; "" when the pattern is not "*.<plain text>".
std::string pattern_extension(const std::string &pattern) {
  if (pattern.size() < 3 || pattern.compare(0, 2, "*.") != 0) return "";
  std::string ext = pattern.substr(2);
  if (ext.find_first_of("*?[]{}/") != std::string::npos) return "";
  return lower(ext);
}

void add_patterns(Filter &f, const std::string &text) {
  std::string t = text;
  for (char &c : t)
    if (c == ';' || c == '\t') c = ' ';
  for (const auto &word : split(t, ' ')) {
    std::string w = trim(word);
    if (w.empty()) continue;
    for (const auto &p : expand_braces(w)) {
      if (p.empty()) continue;
      bool dup = std::any_of(f.patterns.begin(), f.patterns.end(),
                             [&](const std::string &q) { return lower(q) == lower(p); });
      if (!dup) f.patterns.push_back(p);
    }
  }
}

}  // namespace

bool Filter::all() const {
  if (patterns.empty() && mime_types.empty()) return true;
  for (const auto &p : patterns)
    if (p == "*" || p == "*.*") return true;
  return std::find(mime_types.begin(), mime_types.end(), "*/*") != mime_types.end() ||
         std::find(mime_types.begin(), mime_types.end(), "application/octet-stream") != mime_types.end();
}

bool Filter::matches_file(const std::string &file, const std::string &mime) const {
  if (all()) return true;
  for (const auto &p : patterns)
    if (glob_match(p, file)) return true;
  if (!mime.empty())
    for (const auto &m : mime_types) {
      if (m == mime) return true;
      if (m.size() > 2 && m.compare(m.size() - 2, 2, "/*") == 0 && mime.compare(0, m.size() - 1, m, 0, m.size() - 1) == 0)
        return true;
      if (g_content_type_is_a(mime.c_str(), m.c_str())) return true;
    }
  return false;
}

bool Filter::matches(const Entry &e) const { return e.dir || matches_file(e.name, e.mime); }

std::vector<std::string> Filter::extensions() const {
  std::vector<std::string> out;
  for (const auto &p : patterns) {
    std::string ext = pattern_extension(p);
    if (!ext.empty() && std::find(out.begin(), out.end(), ext) == out.end()) out.push_back(ext);
  }
  return out;
}

std::string Filter::extension() const {
  std::vector<std::string> exts = extensions();
  if (exts.empty() || all()) return "";
  std::string first = canonical_extension(exts[0]);
  for (const auto &e : exts)
    if (canonical_extension(e) != first) return "";
  // A filter of ".jpeg" files saves ".jpeg" files.
  return exts.size() == 1 ? exts[0] : first;
}

std::vector<Filter> parse_filters(const std::string &spec) {
  std::vector<Filter> out;
  // Several filters: one per line (Fl_Native_File_Chooser), or tab
  // separated "Name (patterns)" items (fl_file_chooser).
  std::vector<std::string> parts;
  if (spec.find('\n') != std::string::npos) {
    parts = split(spec, '\n');
  } else {
    std::vector<std::string> tabbed = split(spec, '\t');
    bool chooser_list = tabbed.size() > 1 && std::all_of(tabbed.begin(), tabbed.end(), [](const std::string &t) {
      std::string x = trim(t);
      return x.empty() || (x.back() == ')' && x.find('(') != std::string::npos);
    });
    parts = chooser_list ? tabbed : std::vector<std::string>{spec};
  }
  for (const auto &raw : parts) {
    std::string line = trim(raw);
    if (line.empty()) continue;
    Filter f;
    size_t tab = line.find('\t');
    size_t paren = line.rfind('(');
    if (tab != std::string::npos) {
      f.name = trim(line.substr(0, tab));
      add_patterns(f, line.substr(tab + 1));
    } else if (paren != std::string::npos && line.back() == ')') {
      f.name = trim(line.substr(0, paren));
      add_patterns(f, line.substr(paren + 1, line.size() - paren - 2));
    } else {
      add_patterns(f, line);
      f.name = line;
    }
    if (f.name.empty()) f.name = f.patterns.empty() ? std::string() : f.patterns[0];
    if (f.patterns.empty()) f.patterns.push_back("*");
    out.push_back(std::move(f));
  }
  return out;
}

std::pair<size_t, size_t> stem_range(const std::string &name) {
  static const char *const doubles[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.lzma",
                                        ".tar.Z", ".tar.lz4"};
  std::string low = lower(name);
  for (const char *d : doubles) {
    size_t n = strlen(d);
    if (low.size() > n && low.compare(low.size() - n, n, lower(d)) == 0) return {0, name.size() - n};
  }
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) return {0, name.size()};
  return {0, dot};
}

std::string name_for_filter(const std::string &name, const Filter &filter, const std::vector<Filter> &all) {
  std::string ext = filter.extension();
  if (name.empty() || ext.empty() || filter.matches_file(name, "")) return name;
  size_t dot = name.rfind('.');
  if (dot != std::string::npos && dot > 0 && dot + 1 < name.size()) {
    std::string old = lower(name.substr(dot + 1));
    for (const auto &f : all) {
      std::vector<std::string> exts = f.extensions();
      if (std::find(exts.begin(), exts.end(), old) != exts.end()) return name.substr(0, dot + 1) + ext;
    }
  }
  if (!name.empty() && name.back() == '.') return name + ext;
  return name + "." + ext;
}

// ------------------------------------------------------------- locations

void History::visit(const std::string &location) {
  if (location == current_) return;
  if (!current_.empty()) back_.push_back(current_);
  current_ = location;
  forward_.clear();
}

std::string History::back() {
  if (back_.empty()) return "";
  forward_.push_back(current_);
  current_ = back_.back();
  back_.pop_back();
  return current_;
}

std::string History::forward() {
  if (forward_.empty()) return "";
  back_.push_back(current_);
  current_ = forward_.back();
  forward_.pop_back();
  return current_;
}

std::string normalize_path(const std::string &path) {
  if (path.empty() || path[0] != '/') return path;
  std::vector<std::string> parts;
  for (const auto &p : split(path, '/')) {
    if (p.empty() || p == ".") continue;
    if (p == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(p);
  }
  std::string out;
  for (const auto &p : parts) out += "/" + p;
  return out.empty() ? "/" : out;
}

std::string parent_dir(const std::string &path) {
  std::string p = normalize_path(path);
  if (p.empty() || p == "/" || p[0] != '/') return "";
  size_t slash = p.rfind('/');
  return slash == 0 ? "/" : p.substr(0, slash);
}

std::string expand_location(const std::string &typed, const std::string &cwd) {
  std::string t = typed;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
  if (t.empty()) return "";
  if (starts_with(t, "file://")) {
    gchar *p = g_filename_from_uri(t.c_str(), nullptr, nullptr);
    return p ? normalize_path(take_gstr(p)) : "";
  }
  if (t[0] == '~') {
    size_t slash = t.find('/');
    std::string user = t.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
    std::string home;
    if (user.empty()) {
      home = home_dir();
    } else if (struct passwd *pw = getpwnam(user.c_str())) {
      home = pw->pw_dir;
    } else {
      return "";
    }
    return normalize_path(home + (slash == std::string::npos ? "" : t.substr(slash)));
  }
  if (t[0] == '/') return normalize_path(t);
  if (cwd.empty() || cwd[0] != '/') return "";
  return normalize_path(join(cwd, t));
}

std::vector<std::string> path_chain(const std::string &path) {
  std::string p = normalize_path(path);
  std::vector<std::string> out;
  if (p.empty() || p[0] != '/') return out;
  out.push_back("/");
  std::string cur;
  for (const auto &part : split(p, '/')) {
    if (part.empty()) continue;
    cur += "/" + part;
    out.push_back(cur);
  }
  return out;
}

std::vector<std::string> complete_location(const std::string &typed, const std::string &cwd, bool dirs_only,
                                           bool hidden, size_t limit) {
  std::vector<std::string> out;
  if (typed == "~") return {"~/"};
  size_t slash = typed.rfind('/');
  std::string dir_typed = slash == std::string::npos ? "" : typed.substr(0, slash + 1);
  std::string last = slash == std::string::npos ? typed : typed.substr(slash + 1);
  std::string folder = dir_typed.empty() ? cwd : expand_location(dir_typed, cwd);
  if (folder.empty() || folder[0] != '/') return out;
  DIR *d = opendir(folder.c_str());
  if (!d) return out;
  std::string want = fold_text(last);
  bool dots = hidden || (!last.empty() && last[0] == '.');
  struct Match {
    std::string key, text;
  };
  std::vector<Match> found;
  int fd = dirfd(d);
  while (struct dirent *de = readdir(d)) {
    if (is_dot_dir(de->d_name)) continue;
    if (de->d_name[0] == '.' && !dots) continue;
    std::string display = take_gstr(g_filename_display_name(de->d_name));
    if (fold_text(display).compare(0, want.size(), want) != 0) continue;
    bool is_dir = de->d_type == DT_DIR;
    if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
      struct stat st {};
      is_dir = fstatat(fd, de->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);
    }
    if (dirs_only && !is_dir) continue;
    Match m;
    m.key = name_key(display);
    m.text = dir_typed + de->d_name + (is_dir ? "/" : "");
    found.push_back(std::move(m));
  }
  closedir(d);
  std::sort(found.begin(), found.end(), [](const Match &a, const Match &b) { return a.key < b.key; });
  for (auto &m : found) {
    if (out.size() >= limit) break;
    out.push_back(std::move(m.text));
  }
  return out;
}

std::string common_prefix(const std::vector<std::string> &items) {
  if (items.empty()) return "";
  std::string p = items[0];
  for (const auto &s : items) {
    size_t n = 0;
    while (n < p.size() && n < s.size() && p[n] == s[n]) ++n;
    p.resize(n);
  }
  // Whole characters only.
  while (!p.empty() && (uint8_t(p.back()) & 0xC0) == 0x80) p.pop_back();
  if (!p.empty() && uint8_t(p.back()) >= 0xC0) p.pop_back();
  return p;
}

// ------------------------------------------------------------- places

std::vector<Place> standard_places(bool recent) {
  std::vector<Place> out;
  if (recent) {
    Place r;
    r.kind = Place::Kind::Recent;
    r.id = "recent";
    r.label = _("Recent");
    r.icon = "recent";
    out.push_back(r);
  }
  std::string home = home_dir();
  Place h;
  h.kind = Place::Kind::Home;
  h.id = "home";
  h.label = _("Home");
  h.path = home;
  h.icon = "home";
  out.push_back(h);
  static const struct {
    const char *key, *icon;
  } dirs[] = {{"DESKTOP", "desktop"}, {"DOCUMENTS", "documents"}, {"DOWNLOAD", "downloads"},
              {"MUSIC", "music"},     {"PICTURES", "pictures"},   {"VIDEOS", "videos"}};
  std::set<std::string> seen{normalize_path(home)};
  for (const auto &d : dirs) {
    std::string p = normalize_path(user_dir(d.key));
    if (p.empty() || seen.count(p) || !dir_exists(p)) continue;
    seen.insert(p);
    Place u;
    u.kind = Place::Kind::UserDir;
    u.id = std::string("user:") + d.key;
    u.path = p;
    u.label = take_gstr(g_filename_display_basename(p.c_str()));
    u.icon = d.icon;
    out.push_back(u);
  }
  return out;
}

std::vector<Place> parse_bookmarks(const std::string &text) {
  std::vector<Place> out;
  std::set<std::string> seen;
  for (const auto &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    size_t space = line.find(' ');
    std::string uri = space == std::string::npos ? line : line.substr(0, space);
    std::string label = space == std::string::npos ? "" : trim(line.substr(space + 1));
    if (!starts_with(uri, "file://")) continue;
    gchar *p = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
    if (!p) continue;
    std::string path = normalize_path(take_gstr(p));
    if (path.empty() || !seen.insert(path).second) continue;
    Place b;
    b.kind = Place::Kind::Bookmark;
    b.id = "bookmark:" + path;
    b.path = path;
    b.label = label.empty() ? take_gstr(g_filename_display_basename(path.c_str())) : label;
    b.icon = "bookmark";
    out.push_back(b);
  }
  return out;
}

std::vector<Place> read_bookmarks() {
  std::string text;
  if (!read_file(config_home() + "/gtk-3.0/bookmarks", text)) read_file(home_dir() + "/.gtk-bookmarks", text);
  std::vector<Place> out;
  for (auto &b : parse_bookmarks(text))
    if (dir_exists(b.path)) out.push_back(std::move(b));
  return out;
}

namespace {

// mountinfo escapes spaces, tabs, newlines and backslashes as \ooo.
std::string unescape_mount(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && isdigit(uint8_t(s[i + 1])) &&
        isdigit(uint8_t(s[i + 2])) && isdigit(uint8_t(s[i + 3]))) {
      out += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

bool pseudo_fs(const std::string &type) {
  static const std::set<std::string> pseudo = {
      "proc",      "sysfs",      "tmpfs",     "devtmpfs",   "devpts",     "cgroup",       "cgroup2",
      "overlay",   "squashfs",   "autofs",    "binfmt_misc", "debugfs",   "tracefs",      "securityfs",
      "pstore",    "efivarfs",   "bpf",       "mqueue",     "hugetlbfs",  "configfs",     "fusectl",
      "nsfs",      "ramfs",      "rpc_pipefs", "nfsd",      "selinuxfs",  "fuse.gvfsd-fuse", "fuse.portal",
      "fuse.snapfuse", "fuse.lxcfs", "fuse.xdg-document-portal", "iso9660x", "zfs-snapshot", "erofs"};
  return pseudo.count(type) != 0;
}

}  // namespace

std::vector<Place> parse_mounts(const std::string &mountinfo, const std::string &user, const std::string &home) {
  std::vector<Place> out;
  std::string run_media = "/run/media/" + user + "/";
  for (const auto &line : split(mountinfo, '\n')) {
    std::vector<std::string> f = split(line, ' ');
    size_t sep = 0;
    while (sep < f.size() && f[sep] != "-") ++sep;
    if (f.size() < 5 || sep + 2 >= f.size()) continue;
    std::string point = unescape_mount(f[4]), type = f[sep + 1];
    if (pseudo_fs(type)) continue;
    bool removable = starts_with(point, "/media/") || starts_with(point, run_media) ||
                     (starts_with(point, "/run/media/") && user.empty());
    bool shown = removable || starts_with(point, "/mnt/") ||
                 (!home.empty() && home != "/" && starts_with(point, home + "/"));
    // The folders that hold the mounts are not drives themselves.
    if (!shown || point == "/media/" + user || point.back() == '/') continue;
    out.erase(std::remove_if(out.begin(), out.end(), [&](const Place &p) { return p.path == point; }), out.end());
    Place d;
    d.kind = Place::Kind::Drive;
    d.id = "drive:" + point;
    d.path = point;
    d.label = take_gstr(g_filename_display_basename(point.c_str()));
    d.removable = removable;
    static const std::set<std::string> remote = {"nfs", "nfs4", "cifs", "smb3", "fuse.sshfs", "9p", "fuse.rclone",
                                                 "davfs", "fuse.davfs2", "afs", "ceph", "glusterfs"};
    d.icon = remote.count(type) ? "network" : removable ? "usb" : "drive";
    out.push_back(d);
  }
  std::stable_sort(out.begin(), out.end(), [](const Place &a, const Place &b) {
    if (a.removable != b.removable) return a.removable;
    return a.label < b.label;
  });
  return out;
}

std::vector<Place> mounted_drives() {
  std::string text;
  if (!read_file("/proc/self/mountinfo", text)) return {};
  return parse_mounts(text, user_name(), home_dir());
}

std::vector<Place> network_places() {
  std::vector<Place> out;
  std::string runtime = env("XDG_RUNTIME_DIR");
  if (runtime.empty()) return out;
  std::string base = runtime + "/gvfs";
  DIR *d = opendir(base.c_str());
  if (!d) return out;
  while (struct dirent *de = readdir(d)) {
    if (de->d_name[0] == '.') continue;
    std::string name = de->d_name;
    size_t colon = name.find(':');
    if (colon == std::string::npos) continue;
    std::map<std::string, std::string> kv;
    for (const auto &pair : split(name.substr(colon + 1), ',')) {
      size_t eq = pair.find('=');
      if (eq != std::string::npos) kv[pair.substr(0, eq)] = take_gstr(g_uri_unescape_string(pair.substr(eq + 1).c_str(), nullptr));
    }
    std::string host = kv.count("host") ? kv["host"] : kv["server"];
    if (host.empty()) continue;
    Place p;
    p.kind = Place::Kind::Network;
    p.path = base + "/" + name;
    p.id = "network:" + p.path;
    p.icon = "network";
    if (kv.count("share")) p.label = format(_("%s on %s"), kv["share"].c_str(), host.c_str());
    else if (kv.count("user")) p.label = format(_("%s on %s"), kv["user"].c_str(), host.c_str());
    else p.label = host;
    out.push_back(p);
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const Place &a, const Place &b) { return a.label < b.label; });
  return out;
}

// ------------------------------------------------------------- recent files

std::string recent_file_path() { return data_home() + "/recently-used.xbel"; }

std::vector<RecentFile> parse_recent(const std::string &xbel) {
  std::vector<RecentFile> out;
  if (xbel.empty()) return out;
  GBookmarkFile *bf = g_bookmark_file_new();
  GError *err = nullptr;
  if (!g_bookmark_file_load_from_data(bf, xbel.data(), xbel.size(), &err)) {
    if (err) g_error_free(err);
    g_bookmark_file_free(bf);
    return out;
  }
  gsize n = 0;
  gchar **uris = g_bookmark_file_get_uris(bf, &n);
  std::set<std::string> seen;
  for (gsize i = 0; i < n; ++i) {
    if (!g_str_has_prefix(uris[i], "file://")) continue;
    gchar *path = g_filename_from_uri(uris[i], nullptr, nullptr);
    if (!path) continue;
    RecentFile r;
    r.path = take_gstr(path);
    if (!seen.insert(r.path).second) continue;
    r.mime = take_gstr(g_bookmark_file_get_mime_type(bf, uris[i], nullptr));
    for (GDateTime *t : {g_bookmark_file_get_added_date_time(bf, uris[i], nullptr),
                         g_bookmark_file_get_modified_date_time(bf, uris[i], nullptr),
                         g_bookmark_file_get_visited_date_time(bf, uris[i], nullptr)})
      if (t) r.time = std::max(r.time, time_t(g_date_time_to_unix(t)));
    out.push_back(std::move(r));
  }
  g_strfreev(uris);
  g_bookmark_file_free(bf);
  std::stable_sort(out.begin(), out.end(), [](const RecentFile &a, const RecentFile &b) { return a.time > b.time; });
  return out;
}

std::vector<RecentFile> read_recent(size_t limit) {
  std::string text;
  if (!read_file(recent_file_path(), text)) return {};
  std::vector<RecentFile> out;
  for (auto &r : parse_recent(text)) {
    if (out.size() >= limit) break;
    struct stat st {};
    if (stat(r.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    out.push_back(std::move(r));
  }
  return out;
}

bool add_recent(const std::string &path, const std::string &mime, const std::string &app) {
  std::string file = recent_file_path();
  GBookmarkFile *bf = g_bookmark_file_new();
  std::string text;
  if (read_file(file, text) && !text.empty()) {
    GError *err = nullptr;
    if (!g_bookmark_file_load_from_data(bf, text.data(), text.size(), &err)) {
      // An unreadable list is left as it is rather than replaced.
      if (err) g_error_free(err);
      g_bookmark_file_free(bf);
      return false;
    }
  }
  gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  if (!uri) {
    g_bookmark_file_free(bf);
    return false;
  }
  std::string name = app.empty() ? "sde" : app;
  std::string exec = "'" + name + " %u'";
  if (!mime.empty()) g_bookmark_file_set_mime_type(bf, uri, mime.c_str());
  g_bookmark_file_add_application(bf, uri, name.c_str(), exec.c_str());
  GDateTime *now = g_date_time_new_now_utc();
  g_bookmark_file_set_visited_date_time(bf, uri, now);
  g_bookmark_file_set_modified_date_time(bf, uri, now);
  g_date_time_unref(now);
  g_free(uri);
  make_dirs(data_home());
  GError *err = nullptr;
  bool ok = g_bookmark_file_to_file(bf, file.c_str(), &err);
  if (err) g_error_free(err);
  g_bookmark_file_free(bf);
  return ok;
}

// ------------------------------------------------------------- types

std::string guess_mime(const std::string &name, bool dir) {
  if (dir) return "inode/directory";
  gboolean uncertain = FALSE;
  gchar *type = g_content_type_guess(name.c_str(), nullptr, 0, &uncertain);
  std::string out = take_gstr(type);
  if (out.empty()) out = "application/octet-stream";
  return out;
}

std::string mime_description(const std::string &mime) {
  if (mime == "inode/directory") return _("Folder");
  std::string d = take_gstr(g_content_type_get_description(mime.c_str()));
  return d.empty() ? mime : d;
}

std::vector<std::string> mime_icon_names(const std::string &mime) {
  std::vector<std::string> out;
  auto add = [&](const std::string &n) {
    if (!n.empty() && !ends_with(n, "-symbolic") && std::find(out.begin(), out.end(), n) == out.end())
      out.push_back(n);
  };
  if (mime == "inode/directory") {
    add("folder");
    return out;
  }
  if (GIcon *icon = g_content_type_get_icon(mime.c_str())) {
    if (G_IS_THEMED_ICON(icon))
      for (const gchar *const *n = g_themed_icon_get_names(G_THEMED_ICON(icon)); n && *n; ++n) add(*n);
    g_object_unref(icon);
  }
  add(take_gstr(g_content_type_get_generic_icon_name(mime.c_str())));
  add("text-x-generic");
  add("unknown");
  return out;
}

bool can_thumbnail(const std::string &mime) {
  static std::set<std::string> types;
  static std::once_flag once;
  std::call_once(once, [] {
    GSList *formats = gdk_pixbuf_get_formats();
    for (GSList *l = formats; l; l = l->next) {
      auto *fmt = static_cast<GdkPixbufFormat *>(l->data);
      if (gdk_pixbuf_format_is_disabled(fmt)) continue;
      gchar **mimes = gdk_pixbuf_format_get_mime_types(fmt);
      for (gchar **m = mimes; m && *m; ++m) types.insert(*m);
      g_strfreev(mimes);
    }
    g_slist_free(formats);
  });
  if (types.count(mime)) return true;
  // Aliases and subclasses ("image/x-png").
  for (const auto &t : types)
    if (starts_with(mime, "image/") && g_content_type_is_a(mime.c_str(), t.c_str())) return true;
  return false;
}

std::string size_text(long long bytes) { return take_gstr(g_format_size(guint64(bytes < 0 ? 0 : bytes))); }

std::string time_text(time_t t, time_t now) {
  if (t <= 0) return "";
  GDateTime *when = g_date_time_new_from_unix_local(t);
  GDateTime *today = g_date_time_new_from_unix_local(now);
  if (!when || !today) {
    if (when) g_date_time_unref(when);
    if (today) g_date_time_unref(today);
    return "";
  }
  int y1, m1, d1, y2, m2, d2;
  g_date_time_get_ymd(when, &y1, &m1, &d1);
  g_date_time_get_ymd(today, &y2, &m2, &d2);
  GDate a, b;
  g_date_clear(&a, 1);
  g_date_clear(&b, 1);
  g_date_set_dmy(&a, GDateDay(d1), GDateMonth(m1), GDateYear(y1));
  g_date_set_dmy(&b, GDateDay(d2), GDateMonth(m2), GDateYear(y2));
  int days = g_date_days_between(&a, &b);
  std::string clock = take_gstr(g_date_time_format(when, "%H:%M"));
  std::string out;
  if (days == 0) out = format(_("Today, %s"), clock.c_str());
  else if (days == 1) out = format(_("Yesterday, %s"), clock.c_str());
  else if (days > 1 && days < 7) out = take_gstr(g_date_time_format(when, "%A, %H:%M"));
  else if (y1 == y2) out = take_gstr(g_date_time_format(when, "%e %B"));
  else out = take_gstr(g_date_time_format(when, "%e %B %Y"));
  g_date_time_unref(when);
  g_date_time_unref(today);
  return trim(out);
}

// ------------------------------------------------------------- folder reader

FolderReader::FolderReader(WakeFn wake) : wake_(std::move(wake)) { thread_ = std::thread([this] { worker(); }); }

FolderReader::~FolderReader() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    ++generation_;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

uint64_t FolderReader::start(const std::string &dir) {
  std::lock_guard<std::mutex> lock(mu_);
  Job j;
  j.gen = ++generation_;
  j.dir = dir;
  jobs_.clear();
  ready_.clear();
  jobs_.push_back(j);
  cv_.notify_all();
  return j.gen;
}

uint64_t FolderReader::start_search(const std::string &dir, const std::string &query, bool hidden, int max_depth) {
  std::lock_guard<std::mutex> lock(mu_);
  Job j;
  j.gen = ++generation_;
  j.dir = dir;
  j.query = fold_text(query);
  j.search = true;
  j.hidden = hidden;
  j.depth = max_depth;
  jobs_.clear();
  ready_.clear();
  jobs_.push_back(j);
  cv_.notify_all();
  return j.gen;
}

void FolderReader::cancel() {
  std::lock_guard<std::mutex> lock(mu_);
  ++generation_;
  jobs_.clear();
  ready_.clear();
}

bool FolderReader::take(Batch &out) {
  std::lock_guard<std::mutex> lock(mu_);
  while (!ready_.empty() && ready_.front().generation != generation_) ready_.pop_front();
  if (ready_.empty()) return false;
  out = std::move(ready_.front());
  ready_.pop_front();
  return true;
}

bool FolderReader::push(uint64_t gen, std::vector<Entry> &pending, bool done, const std::string &error) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (gen != generation_) return false;
    // Batches the dialog has not collected yet are merged: one wake-up.
    if (!ready_.empty() && ready_.back().generation == gen && !ready_.back().done) {
      Batch &b = ready_.back();
      for (auto &e : pending) b.entries.push_back(std::move(e));
      b.done = done;
      b.error = error;
    } else {
      Batch b;
      b.generation = gen;
      b.entries = std::move(pending);
      b.done = done;
      b.error = error;
      ready_.push_back(std::move(b));
    }
  }
  pending.clear();
  if (wake_) wake_();
  return true;
}

void FolderReader::read_folder(uint64_t gen, const std::string &dir) {
  std::vector<Entry> pending;
  if (dir == "recent:") {
    for (auto &r : read_recent()) {
      Entry e;
      if (!entry_for_path(r.path, e)) continue;
      if (!r.mime.empty() && !e.dir) e.mime = r.mime;
      e.mtime = std::max(e.mtime, r.time);
      e.folder = parent_dir(r.path);
      pending.push_back(std::move(e));
    }
    push(gen, pending, true);
    return;
  }
  DIR *d = opendir(dir.c_str());
  if (!d) {
    push(gen, pending, true, strerror(errno));
    return;
  }
  std::set<std::string> hidden = hidden_names(dir);
  int fd = dirfd(d);
  // The first entries go out at once (the dialog shows something within a
  // frame), the rest in batches about ten times a second.
  double started = now_seconds(), last = started;
  size_t first = 96;
  bool sent = false;
  while (struct dirent *de = readdir(d)) {
    if (gen != generation_) break;
    if (is_dot_dir(de->d_name)) continue;
    Entry e;
    if (!read_entry(fd, de->d_name, dir, e)) continue;
    if (hidden.count(e.name)) e.hidden = true;
    pending.push_back(std::move(e));
    double now = now_seconds();
    if ((!sent && (pending.size() >= first || now - started > 0.03)) || (sent && now - last > 0.1)) {
      if (!push(gen, pending, false)) break;
      sent = true;
      last = now;
    }
  }
  closedir(d);
  if (gen == generation_) push(gen, pending, true);
}

void FolderReader::search(uint64_t gen, const std::string &dir, const std::string &query, bool hidden,
                          int max_depth) {
  std::vector<Entry> pending;
  std::deque<std::pair<std::string, int>> todo{{dir, 0}};
  size_t visited = 0, found = 0;
  double last = now_seconds();
  while (!todo.empty() && gen == generation_ && visited < 300000 && found < 5000) {
    auto [folder, depth] = todo.front();
    todo.pop_front();
    DIR *d = opendir(folder.c_str());
    if (!d) continue;
    int fd = dirfd(d);
    while (struct dirent *de = readdir(d)) {
      if (gen != generation_) break;
      if (is_dot_dir(de->d_name)) continue;
      ++visited;
      bool dot = de->d_name[0] == '.';
      if (dot && !hidden) continue;
      std::string display = take_gstr(g_filename_display_name(de->d_name));
      bool match = fold_text(display).find(query) != std::string::npos;
      bool is_dir = de->d_type == DT_DIR;
      if (match || de->d_type == DT_UNKNOWN) {
        Entry e;
        if (!read_entry(fd, de->d_name, folder, e)) continue;
        is_dir = e.dir && !e.link;
        if (match) {
          e.folder = folder;
          pending.push_back(std::move(e));
          ++found;
        }
      }
      if (is_dir && depth + 1 <= max_depth) todo.push_back({join(folder, de->d_name), depth + 1});
    }
    closedir(d);
    double now = now_seconds();
    if (!pending.empty() && now - last > 0.1) {
      if (!push(gen, pending, false)) return;
      last = now;
    }
  }
  if (gen == generation_) push(gen, pending, true);
}

void FolderReader::worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = jobs_.front();
      jobs_.pop_front();
    }
    if (job.search) search(job.gen, job.dir, job.query, job.hidden, job.depth);
    else read_folder(job.gen, job.dir);
  }
}

// ------------------------------------------------------------- thumbnails

namespace {

std::string cache_home() { return env("XDG_CACHE_HOME", home_dir() + "/.cache"); }

std::string file_uri(const std::string &path) {
  gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  return uri ? take_gstr(uri) : "file://" + path;
}

std::string md5_hex(const std::string &text) {
  return take_gstr(g_compute_checksum_for_string(G_CHECKSUM_MD5, text.c_str(), gssize(text.size())));
}

uint32_t be32(const std::string &s, size_t at) {
  return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) | (uint32_t(uint8_t(s[at + 2])) << 8) |
         uint32_t(uint8_t(s[at + 3]));
}

// The tEXt chunk |key| of PNG data.
std::string png_text(const std::string &png, const std::string &key) {
  if (png.size() < 8 || png.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) return "";
  size_t at = 8;
  while (at + 12 <= png.size()) {
    uint32_t len = be32(png, at);
    if (len > png.size() - at - 12) break;
    if (png.compare(at + 4, 4, "tEXt") == 0) {
      size_t body = at + 8;
      if (len > key.size() && png.compare(body, key.size(), key) == 0 && png[body + key.size()] == '\0')
        return png.substr(body + key.size() + 1, len - key.size() - 1);
    } else if (png.compare(at + 4, 4, "IEND") == 0) {
      break;
    }
    at += 12 + len;
  }
  return "";
}

struct ReadCursor {
  const std::string *data;
  size_t pos;
};

cairo_status_t read_from_string(void *closure, unsigned char *data, unsigned int len) {
  auto *c = static_cast<ReadCursor *>(closure);
  if (c->pos + len > c->data->size()) return CAIRO_STATUS_READ_ERROR;
  memcpy(data, c->data->data() + c->pos, len);
  c->pos += len;
  return CAIRO_STATUS_SUCCESS;
}

bool private_dirs(const std::string &path) {
  std::string cur;
  for (const auto &part : split(path, '/')) {
    if (part.empty()) continue;
    cur += "/" + part;
    if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
  }
  return dir_exists(path);
}

cairo_surface_t *surface_from_pixbuf(GdkPixbuf *pb) {
  if (!pb) return nullptr;
  int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
  int n = gdk_pixbuf_get_n_channels(pb), stride = gdk_pixbuf_get_rowstride(pb);
  bool alpha = gdk_pixbuf_get_has_alpha(pb);
  if (w <= 0 || h <= 0 || (n != 3 && n != 4) || gdk_pixbuf_get_bits_per_sample(pb) != 8) return nullptr;
  const guchar *src = gdk_pixbuf_read_pixels(pb);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  unsigned char *dst = cairo_image_surface_get_data(s);
  int dstride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < h; ++y) {
    const guchar *p = src + size_t(y) * size_t(stride);
    uint32_t *row = reinterpret_cast<uint32_t *>(dst + size_t(y) * size_t(dstride));
    for (int x = 0; x < w; ++x, p += n) {
      uint32_t a = alpha && n == 4 ? p[3] : 255;
      row[x] = (a << 24) | ((p[0] * a / 255) << 16) | ((p[1] * a / 255) << 8) | (p[2] * a / 255);
    }
  }
  cairo_surface_mark_dirty(s);
  return s;
}

// A picture decoded at most |max_dim| on its longest side (never enlarged).
GdkPixbuf *decode(const std::string &path, int max_dim, int *w, int *h) {
  int iw = 0, ih = 0;
  if (!gdk_pixbuf_get_file_info(path.c_str(), &iw, &ih)) return nullptr;
  if (w) *w = iw;
  if (h) *h = ih;
  // Gigantic pictures are left alone (a thumbnail is not worth a gigabyte).
  if (iw > 0 && ih > 0 && double(iw) * ih > 150e6) return nullptr;
  GError *err = nullptr;
  GdkPixbuf *pb = nullptr;
  if (iw > 0 && ih > 0 && iw <= max_dim && ih <= max_dim)
    pb = gdk_pixbuf_new_from_file(path.c_str(), &err);
  else
    pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), max_dim, max_dim, TRUE, &err);
  if (err) g_error_free(err);
  if (!pb) return nullptr;
  GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(pb);
  g_object_unref(pb);
  return upright;
}

std::string fail_path(const std::string &path) {
  return cache_home() + "/thumbnails/fail/sde-file-chooser/" + md5_hex(file_uri(path)) + ".png";
}

}  // namespace

std::string thumbnail_dir_name(int size) {
  if (size <= 128) return "normal";
  if (size <= 256) return "large";
  if (size <= 512) return "x-large";
  return "xx-large";
}

std::string thumbnail_path(const std::string &path, int size) {
  return cache_home() + "/thumbnails/" + thumbnail_dir_name(size) + "/" + md5_hex(file_uri(path)) + ".png";
}

cairo_surface_t *cached_thumbnail(const std::string &path, time_t mtime, int size, int *w, int *h,
                                  bool smaller) {
  std::string mtime_text = std::to_string((long long)mtime);
  // The size asked for, else a bigger one (scaled down when drawn), else
  // (|smaller|) a smaller one: better than an icon.
  std::vector<int> sizes;
  for (int s : {128, 256, 512, 1024})
    if (s >= size || (s == 1024 && size > 1024)) sizes.push_back(s);
  if (smaller)
    for (int s : {512, 256, 128})
      if (s < size && std::find(sizes.begin(), sizes.end(), s) == sizes.end()) sizes.push_back(s);
  for (int s : sizes) {
    std::string data;
    if (!read_file(thumbnail_path(path, s), data)) continue;
    if (png_text(data, "Thumb::MTime") != mtime_text) continue;
    ReadCursor cursor{&data, 0};
    cairo_surface_t *surface = cairo_image_surface_create_from_png_stream(read_from_string, &cursor);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      continue;
    }
    if (w) *w = atoi(png_text(data, "Thumb::Image::Width").c_str());
    if (h) *h = atoi(png_text(data, "Thumb::Image::Height").c_str());
    return surface;
  }
  return nullptr;
}

cairo_surface_t *make_thumbnail(const std::string &path, time_t mtime, int size, int *w, int *h) {
  std::string cache = cache_home() + "/thumbnails/";
  if (starts_with(path, cache)) return nullptr;
  std::string mtime_text = std::to_string((long long)mtime);
  std::string failed;
  if (read_file(fail_path(path), failed) && png_text(failed, "Thumb::MTime") == mtime_text) return nullptr;
  int box = size <= 128 ? 128 : size <= 256 ? 256 : size <= 512 ? 512 : 1024;
  int iw = 0, ih = 0;
  GdkPixbuf *pb = decode(path, box, &iw, &ih);
  if (w) *w = iw;
  if (h) *h = ih;
  struct stat st {};
  std::string file_size = stat(path.c_str(), &st) == 0 ? std::to_string((long long)st.st_size) : "0";
  std::string uri = file_uri(path);
  if (!pb) {
    // Remembered, so that it is not tried again until the file changes.
    std::string target = fail_path(path);
    if (private_dirs(target.substr(0, target.rfind('/')))) {
      GdkPixbuf *dot = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
      if (dot) {
        gdk_pixbuf_fill(dot, 0);
        std::string tmp = target + "." + std::to_string(getpid()) + ".tmp";
        if (gdk_pixbuf_save(dot, tmp.c_str(), "png", nullptr, "tEXt::Thumb::URI", uri.c_str(), "tEXt::Thumb::MTime",
                            mtime_text.c_str(), "tEXt::Software", "SDE file chooser", nullptr))
          rename(tmp.c_str(), target.c_str());
        else
          unlink(tmp.c_str());
        g_object_unref(dot);
      }
    }
    return nullptr;
  }
  std::string target = thumbnail_path(path, box);
  std::string dir = target.substr(0, target.rfind('/'));
  if (private_dirs(dir)) {
    static std::atomic<unsigned> counter{0};
    std::string tmp = dir + "/.sde-file-chooser-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".png";
    std::string wt = std::to_string(iw), ht = std::to_string(ih);
    if (gdk_pixbuf_save(pb, tmp.c_str(), "png", nullptr, "tEXt::Thumb::URI", uri.c_str(), "tEXt::Thumb::MTime",
                        mtime_text.c_str(), "tEXt::Thumb::Size", file_size.c_str(), "tEXt::Thumb::Image::Width",
                        wt.c_str(), "tEXt::Thumb::Image::Height", ht.c_str(), "tEXt::Software", "SDE file chooser",
                        nullptr)) {
      chmod(tmp.c_str(), 0600);
      if (rename(tmp.c_str(), target.c_str()) != 0) unlink(tmp.c_str());
    } else {
      unlink(tmp.c_str());
    }
  }
  cairo_surface_t *s = surface_from_pixbuf(pb);
  g_object_unref(pb);
  return s;
}

cairo_surface_t *load_picture(const std::string &path, int max_dim, int *w, int *h) {
  GdkPixbuf *pb = decode(path, max_dim, w, h);
  if (!pb) return nullptr;
  cairo_surface_t *s = surface_from_pixbuf(pb);
  g_object_unref(pb);
  return s;
}

Thumbnailer::Thumbnailer(WakeFn wake, int threads) : wake_(std::move(wake)) {
  threads = std::max(1, std::min(4, threads));
  for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { worker(); });
}

Thumbnailer::~Thumbnailer() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  for (auto &t : threads_)
    if (t.joinable()) t.join();
  for (auto &r : done_)
    if (r.surface) cairo_surface_destroy(r.surface);
}

namespace {
std::string job_key(const Thumbnailer::Job &j) {
  return j.path + "\n" + std::to_string(j.size) + (j.picture ? "p" : "t");
}
}  // namespace

void Thumbnailer::request(std::vector<Job> jobs) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    for (auto &j : jobs)
      if (!running_.count(job_key(j))) queue_.push_back(std::move(j));
  }
  cv_.notify_all();
}

std::vector<Thumbnailer::Result> Thumbnailer::take() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Result> out;
  out.swap(done_);
  return out;
}

bool Thumbnailer::busy() {
  std::lock_guard<std::mutex> lock(mu_);
  return !queue_.empty() || !running_.empty();
}

void Thumbnailer::worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
      running_.insert(job_key(job));
    }
    Result r;
    r.job = job;
    if (job.picture) {
      r.surface = load_picture(job.path, job.size, &r.width, &r.height);
    } else {
      r.surface = cached_thumbnail(job.path, job.mtime, job.size, &r.width, &r.height, !job.generate);
      if (!r.surface && job.generate) r.surface = make_thumbnail(job.path, job.mtime, job.size, &r.width, &r.height);
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_.erase(job_key(job));
      if (stop_) {
        if (r.surface) cairo_surface_destroy(r.surface);
        return;
      }
      done_.push_back(std::move(r));
    }
    if (wake_) wake_();
  }
}

// ------------------------------------------------------------- state

std::string State::path() { return state_home() + "/sde/file-chooser.conf"; }

State State::parse(const std::string &text) {
  State s;
  std::string section;
  for (const auto &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    bool yes = value == "true" || value == "1";
    if (section == "window") {
      if (key == "width") s.width = std::max(0, std::min(20000, atoi(value.c_str())));
      if (key == "height") s.height = std::max(0, std::min(20000, atoi(value.c_str())));
    } else if (section == "view") {
      if (key == "list") s.list_view = yes;
      if (key == "hidden") s.show_hidden = yes;
      if (key == "preview") s.show_preview = yes;
      if (key == "sort") s.sort.key = sort_key_from_id(value);
      if (key == "descending") s.sort.descending = yes;
      if (key == "folders-first") s.sort.folders_first = yes;
    } else if (section == "folders") {
      if (!key.empty() && !value.empty() && value[0] == '/') s.folders[key] = value;
    }
  }
  return s;
}

std::string State::text() const {
  std::ostringstream out;
  out << "# The SDE file chooser remembers its size, view and the last folder of each program.\n";
  out << "[window]\nwidth=" << width << "\nheight=" << height << "\n\n";
  out << "[view]\nlist=" << (list_view ? "true" : "false") << "\nhidden=" << (show_hidden ? "true" : "false")
      << "\npreview=" << (show_preview ? "true" : "false") << "\nsort=" << sort_key_id(sort.key)
      << "\ndescending=" << (sort.descending ? "true" : "false")
      << "\nfolders-first=" << (sort.folders_first ? "true" : "false") << "\n\n";
  out << "[folders]\n";
  for (const auto &kv : folders) {
    if (kv.first.empty() || kv.first.find_first_of("=\n[]") != std::string::npos) continue;
    if (kv.second.find('\n') != std::string::npos) continue;
    out << kv.first << "=" << kv.second << "\n";
  }
  return out.str();
}

State State::load() {
  std::string text;
  if (!read_file(path(), text)) return State();
  return parse(text);
}

bool State::save() const {
  std::string p = path();
  make_dirs(p.substr(0, p.rfind('/')));
  return write_file_atomic(p, text());
}

std::string program_id() {
  std::string name = program_invocation_short_name ? program_invocation_short_name : "";
  if (name.empty()) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) name = basename_of(std::string(buf, size_t(n)));
  }
  for (char &c : name)
    if (c == '=' || c == '[' || c == ']' || c == '\n' || c == ' ') c = '_';
  return name.empty() ? "sde" : name;
}

}  // namespace chooser
}  // namespace sde
