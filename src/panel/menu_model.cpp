// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_model.h"

#include <dirent.h>

#include <algorithm>

#include "util.h"

namespace sde {
namespace menu {

int match_score(const DesktopEntry &e, const std::vector<std::string> &terms) {
  std::string name = lower(e.name), generic = lower(e.generic_name), comment = lower(e.comment);
  auto argv = e.exec.empty() ? std::vector<std::string>() : e.command();
  std::string exec = lower(basename_of(argv.empty() ? "" : argv[0]));
  int total = 0;
  for (auto &t : terms) {
    if (t.empty()) continue;
    int best = 0;
    if (starts_with(name, t)) best = 100;
    else if (name.find(" " + t) != std::string::npos) best = 80;
    else if (name.find(t) != std::string::npos) best = 60;
    else if (generic.find(t) != std::string::npos) best = 40;
    else if (!exec.empty() && starts_with(exec, t)) best = 35;
    if (!best)
      for (auto &k : e.keywords)
        if (starts_with(lower(k), t)) best = 30;
    if (!best && comment.find(t) != std::string::npos) best = 15;
    if (!best) return 0;
    total += best;
  }
  return total;
}

std::vector<std::string> parse_id_list(const std::string &list) {
  std::vector<std::string> out;
  for (auto &part : split(list, ';')) {
    std::string id = trim(part);
    if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

std::string join_id_list(const std::vector<std::string> &ids) {
  std::string s;
  for (auto &id : ids) s += (s.empty() ? "" : ";") + id;
  return s;
}

void remember(std::vector<std::string> &list, const std::string &id, size_t cap) {
  if (id.empty()) return;
  list.erase(std::remove(list.begin(), list.end(), id), list.end());
  list.insert(list.begin(), id);
  if (list.size() > cap) list.resize(cap);
}

std::vector<std::string> default_favorites(const std::vector<DesktopEntry> &apps, size_t max) {
  // One application per role, in this order (freedesktop categories).
  static const std::vector<std::vector<std::string>> roles = {
      {"FileManager"}, {"WebBrowser"},  {"TerminalEmulator"}, {"TextEditor"},
      {"WordProcessor"}, {"Spreadsheet"}, {"Calculator"},     {"Email"},
      {"Player", "AudioVideo"}, {"Viewer", "Graphics"}, {"PackageManager"}, {"Settings"},
  };
  std::vector<std::string> out;
  for (auto &role : roles) {
    if (out.size() >= max) break;
    for (auto &cat : role) {
      auto it = std::find_if(apps.begin(), apps.end(), [&](const DesktopEntry &a) {
        return std::find(a.categories.begin(), a.categories.end(), cat) != a.categories.end() &&
               std::find(out.begin(), out.end(), a.id) == out.end();
      });
      if (it != apps.end()) {
        out.push_back(it->id);
        break;
      }
    }
  }
  // Few known roles: fill up with the first applications.
  for (auto &a : apps) {
    if (out.size() >= std::min<size_t>(max, 4)) break;
    if (std::find(out.begin(), out.end(), a.id) == out.end()) out.push_back(a.id);
  }
  return out;
}

std::string index_letter(const std::string &name) {
  std::string n = trim(name);
  if (n.empty()) return "#";
  unsigned char c = static_cast<unsigned char>(n[0]);
  if (c >= 'a' && c <= 'z') return std::string(1, char(c - 'a' + 'A'));
  if (c >= 'A' && c <= 'Z') return std::string(1, char(c));
  if (c < 0x80) return "#";
  // First UTF-8 character as it is.
  size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  return n.substr(0, std::min(len, n.size()));
}

std::vector<const DesktopEntry *> alphabetical(const std::vector<DesktopEntry> &apps) {
  std::vector<const DesktopEntry *> out;
  out.reserve(apps.size());
  for (auto &a : apps) out.push_back(&a);
  std::stable_sort(out.begin(), out.end(), [](const DesktopEntry *a, const DesktopEntry *b) {
    std::string la = index_letter(a->name), lb = index_letter(b->name);
    // "#" (digits and symbols) first, then A-Z, then other scripts.
    bool ha = la == "#", hb = lb == "#";
    if (ha != hb) return ha;
    if (la != lb) return la < lb;
    return lower(a->name) < lower(b->name);
  });
  return out;
}

std::string file_icon(const std::string &path, bool dir) {
  if (dir) return "folder";
  std::string base = lower(basename_of(path));
  size_t dot = base.rfind('.');
  std::string ext = dot == std::string::npos ? "" : base.substr(dot + 1);
  static const std::vector<std::pair<std::vector<std::string>, std::string>> map = {
      {{"png", "jpg", "jpeg", "gif", "svg", "webp", "bmp", "tif", "tiff"}, "image-x-generic"},
      {{"mp3", "ogg", "oga", "flac", "wav", "m4a", "opus"}, "audio-x-generic"},
      {{"mp4", "mkv", "webm", "avi", "mov", "ogv"}, "video-x-generic"},
      {{"pdf"}, "application-pdf"},
      {{"odt", "doc", "docx", "rtf"}, "x-office-document"},
      {{"ods", "xls", "xlsx", "csv"}, "x-office-spreadsheet"},
      {{"odp", "ppt", "pptx"}, "x-office-presentation"},
      {{"zip", "tar", "gz", "xz", "bz2", "7z", "zst", "deb"}, "package-x-generic"},
      {{"sh", "py", "c", "cpp", "h", "js", "rs", "html", "css"}, "text-x-script"},
  };
  for (auto &m : map)
    if (std::find(m.first.begin(), m.first.end(), ext) != m.first.end()) return m.second;
  return "text-x-generic";
}

void FileIndex::clear() {
  items_.clear();
  built_ = false;
}

void FileIndex::scan(const std::string &dir, int depth, size_t limit) {
  if (depth < 0 || items_.size() >= limit) return;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> subdirs;
  while (dirent *ent = readdir(d)) {
    if (items_.size() >= limit) break;
    std::string name = ent->d_name;
    if (name.empty() || name[0] == '.') continue;
    std::string path = dir + "/" + name;
    bool is_dir = ent->d_type == DT_DIR || (ent->d_type == DT_UNKNOWN && dir_exists(path));
    items_.push_back({path, lower(name), is_dir});
    if (is_dir) subdirs.push_back(path);
  }
  closedir(d);
  std::sort(subdirs.begin(), subdirs.end());
  for (auto &s : subdirs) scan(s, depth - 1, limit);
}

void FileIndex::build(const std::vector<std::string> &roots, size_t limit, int depth) {
  items_.clear();
  std::vector<std::string> seen;
  for (auto &r : roots) {
    if (r.empty() || std::find(seen.begin(), seen.end(), r) != seen.end() || !dir_exists(r))
      continue;
    seen.push_back(r);
    // Roots below the home folder are scanned on their own; the home folder
    // itself only one level deep, so that its big trees do not dominate.
    scan(r, r == home_dir() ? std::min(depth, 1) : depth, limit);
  }
  // A folder can be reached from two roots (home and ~/Documents).
  std::sort(items_.begin(), items_.end(), [](const Item &a, const Item &b) { return a.path < b.path; });
  items_.erase(std::unique(items_.begin(), items_.end(),
                           [](const Item &a, const Item &b) { return a.path == b.path; }),
               items_.end());
  built_ = true;
}

bool FileIndex::is_dir(const std::string &path) const {
  for (auto &i : items_)
    if (i.path == path) return i.dir;
  return false;
}

std::vector<std::string> FileIndex::search(const std::vector<std::string> &terms,
                                           size_t max) const {
  std::vector<std::pair<int, const Item *>> hits;
  bool any = false;
  for (auto &t : terms) any = any || !t.empty();
  if (!any) return {};
  for (auto &i : items_) {
    bool all = true;
    for (auto &t : terms)
      if (!t.empty() && i.name_lower.find(t) == std::string::npos) all = false;
    if (!all) continue;
    int rank = starts_with(i.name_lower, terms[0]) ? 0 : 2;
    if (!i.dir) rank += 1;
    hits.push_back({rank, &i});
  }
  std::stable_sort(hits.begin(), hits.end(), [](auto &a, auto &b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second->path.size() < b.second->path.size();
  });
  std::vector<std::string> out;
  for (auto &h : hits) {
    if (out.size() >= max) break;
    out.push_back(h.second->path);
  }
  return out;
}

}  // namespace menu
}  // namespace sde
