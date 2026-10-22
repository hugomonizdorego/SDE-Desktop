// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

#include "util.h"

namespace sde {
namespace desk {

// ---------------------------------------------------------------- actions

bool ActionSet::allows(DropAction a) const {
  switch (a) {
    case DropAction::Move: return move;
    case DropAction::Copy: return copy;
    case DropAction::Link: return link;
    case DropAction::Ask: return ask;
    default: return false;
  }
}

DropAction choose_drop_action(TargetKind target, unsigned mods, bool same_fs, DropAction suggested,
                              const ActionSet &allowed) {
  auto fit = [&](DropAction want) {
    if (allowed.allows(want)) return want;
    if (want == DropAction::Ask) want = same_fs ? DropAction::Move : DropAction::Copy;
    if (allowed.allows(want)) return want;
    if (allowed.copy) return DropAction::Copy;
    if (allowed.move) return DropAction::Move;
    return DropAction::NoDrop;
  };
  switch (target) {
    case TargetKind::NoTarget: return DropAction::NoDrop;
    case TargetKind::Trash: return allowed.move ? DropAction::Move : DropAction::NoDrop;
    case TargetKind::Launcher:
      // Opening never takes the files away from the source.
      if (allowed.copy) return DropAction::Copy;
      if (allowed.link) return DropAction::Link;
      return allowed.move ? DropAction::Move : DropAction::NoDrop;
    default: break;
  }
  bool ctrl = mods & MOD_CTRL, shift = mods & MOD_SHIFT, alt = mods & MOD_ALT;
  if (ctrl && shift) return fit(DropAction::Link);
  if (ctrl) return fit(DropAction::Copy);
  if (shift) return fit(DropAction::Move);
  if (alt || suggested == DropAction::Ask) return fit(DropAction::Ask);
  return fit(same_fs ? DropAction::Move : DropAction::Copy);
}

const char *xdnd_action_name(DropAction a) {
  switch (a) {
    case DropAction::Move: return "XdndActionMove";
    case DropAction::Copy: return "XdndActionCopy";
    case DropAction::Link: return "XdndActionLink";
    case DropAction::Ask: return "XdndActionAsk";
    default: return "";
  }
}

DropAction xdnd_action_from_name(const std::string &name) {
  if (name == "XdndActionMove") return DropAction::Move;
  if (name == "XdndActionCopy") return DropAction::Copy;
  if (name == "XdndActionLink") return DropAction::Link;
  if (name == "XdndActionAsk") return DropAction::Ask;
  // XdndActionPrivate and unknown actions: treat as a copy.
  if (starts_with(name, "XdndAction")) return DropAction::Copy;
  return DropAction::NoDrop;
}

// --------------------------------------------------------------------- URIs

std::string uri_from_path(const std::string &path) {
  static const char *hex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : path) {
    if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string percent_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
        isxdigit((unsigned char)s[i + 2])) {
      out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string path_from_uri(const std::string &uri) {
  std::string u = trim(uri);
  if (!starts_with(lower(u.substr(0, 7)), "file://")) return "";
  std::string rest = u.substr(7);
  if (starts_with(rest, "localhost/")) rest = rest.substr(9);
  if (rest.empty() || rest[0] != '/') return "";  // file://host/... is not local
  size_t q = rest.find_first_of("?#");
  if (q != std::string::npos) rest = rest.substr(0, q);
  std::string p = percent_decode(rest);
  if (p.find('\0') != std::string::npos) return "";
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

std::vector<std::string> parse_uri_list(const std::string &text) {
  std::vector<std::string> out;
  for (auto &line : split(text, '\n')) {
    std::string l = trim(line);
    if (l.empty() || l[0] == '#') continue;
    out.push_back(l);
  }
  return out;
}

std::string format_uri_list(const std::vector<std::string> &paths) {
  std::string out;
  for (auto &p : paths) out += uri_from_path(p) + "\r\n";
  return out;
}

namespace {

bool is_link_uri(const std::string &s) {
  std::string l = lower(s);
  if (s.find_first_of(" \t") != std::string::npos) return false;
  for (const char *p : {"http://", "https://", "ftp://", "ftps://", "sftp://", "smb://", "mailto:"})
    if (starts_with(l, p) && l.size() > strlen(p)) return true;
  return false;
}

}  // namespace

DropPayload classify_drop(const std::string &data, bool uri_list) {
  DropPayload p;
  std::string text = data;
  // Some sources NUL-terminate the data.
  while (!text.empty() && text.back() == '\0') text.pop_back();
  std::vector<std::string> lines = parse_uri_list(text);
  std::vector<std::string> files, urls;
  bool all = !lines.empty();
  for (auto &l : lines) {
    std::string path = path_from_uri(l);
    if (path.empty() && !uri_list && l[0] == '/' && l.find('\0') == std::string::npos) path = l;
    if (!path.empty()) files.push_back(path);
    else if (is_link_uri(l)) urls.push_back(l);
    else all = false;
  }
  if (uri_list || all) {
    p.files = files;
    p.urls = urls;
    if (!uri_list && !all) p.text = text;
    return p;
  }
  p.text = text;
  return p;
}

std::string link_file_name(const std::string &url) {
  std::string u = trim(url), name;
  std::string l = lower(u);
  if (starts_with(l, "mailto:")) {
    name = u.substr(7);
    size_t q = name.find('?');
    if (q != std::string::npos) name = name.substr(0, q);
  } else {
    size_t s = u.find("://");
    name = s == std::string::npos ? u : u.substr(s + 3);
    size_t end = name.find_first_of("/?#");
    if (end != std::string::npos) name = name.substr(0, end);
    size_t at = name.rfind('@');
    if (at != std::string::npos) name = name.substr(at + 1);
    size_t colon = name.rfind(':');
    if (colon != std::string::npos && name.find(']') == std::string::npos) name = name.substr(0, colon);
  }
  name = percent_decode(name);
  std::string clean;
  for (char c : name)
    if (c != '/' && c != '\0' && c != '\n' && c != '\r') clean += c;
  clean = trim(clean);
  if (clean.empty() || clean == "." || clean == "..") clean = "Link";
  return clean + ".desktop";
}

std::string link_desktop_entry(const std::string &url, const std::string &name) {
  auto oneline = [](std::string s) {
    for (auto &c : s)
      if (c == '\n' || c == '\r') c = ' ';
    return s;
  };
  return "[Desktop Entry]\nVersion=1.0\nType=Link\nName=" + oneline(name) + "\nURL=" + oneline(url) +
         "\nIcon=text-html\n";
}

// ---------------------------------------------------------------- names

std::string keep_both_name(const std::string &name, bool is_dir,
                           bool (*exists)(const std::string &name, void *data), void *data) {
  std::string stem = name, ext;
  if (!is_dir) {
    std::string l = lower(name);
    size_t cut = name.rfind('.');
    // Double extensions people expect to stay together.
    for (const char *e : {".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst"})
      if (ends_with(l, e) && l.size() > strlen(e)) cut = name.size() - strlen(e);
    if (cut != std::string::npos && cut > 0) {
      stem = name.substr(0, cut);
      ext = name.substr(cut);
    }
  }
  // "Photo (2)" -> keep counting from 2.
  int n = 2;
  size_t open = stem.rfind(" (");
  if (open != std::string::npos && !stem.empty() && stem.back() == ')') {
    std::string digits = stem.substr(open + 2, stem.size() - open - 3);
    if (!digits.empty() && std::all_of(digits.begin(), digits.end(), ::isdigit) && digits.size() < 9) {
      n = std::stoi(digits) + 1;
      stem = stem.substr(0, open);
    }
  }
  for (;; ++n) {
    std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
    if (!exists(candidate, data)) return candidate;
  }
}

// ---------------------------------------------------------------- positions

std::string PositionStore::escape_key(const std::string &key) {
  std::string out;
  for (size_t i = 0; i < key.size(); ++i) {
    char c = key[i];
    if (c == '\\') out += "\\\\";
    else if (c == '=') out += "\\e";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (i == 0 && (c == '[' || c == '#' || c == ';' || c == ' ')) out += "\\" + std::string(1, c);
    else if (c == ' ' && i + 1 == key.size()) out += "\\s";
    else out += c;
  }
  return out;
}

std::string PositionStore::unescape_key(const std::string &key) {
  std::string out;
  for (size_t i = 0; i < key.size(); ++i) {
    if (key[i] != '\\' || i + 1 >= key.size()) {
      out += key[i];
      continue;
    }
    char n = key[++i];
    if (n == 'e') out += '=';
    else if (n == 'n') out += '\n';
    else if (n == 'r') out += '\r';
    else if (n == 's') out += ' ';
    else out += n;
  }
  return out;
}

void PositionStore::parse(const std::string &text) {
  cells_.clear();
  auto_arrange = false;
  std::string section;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line[0] == '[') {
      section = trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos
                                                                        : line.find(']') - 1));
      continue;
    }
    // The first '=' not escaped separates key and value.
    size_t eq = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '\\') {
        ++i;
        continue;
      }
      if (line[i] == '=') {
        eq = i;
        break;
      }
    }
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = trim(line.substr(eq + 1));
    if (section == "desktop") {
      if (trim(key) == "auto_arrange") auto_arrange = value == "true";
    } else if (section == "positions") {
      int col = 0, row = 0;
      char extra = 0;
      if (sscanf(value.c_str(), "%d,%d%c", &col, &row, &extra) == 2 && col >= 0 && row >= 0 &&
          col < 10000 && row < 10000)
        cells_[unescape_key(key)] = Cell{col, row};
    }
  }
}

std::string PositionStore::format() const {
  std::string out =
      "# SDE desktop icon positions (grid column,row per file name).\n"
      "# Written by sde-desktop; delete this file to arrange the icons again.\n"
      "[desktop]\nauto_arrange=";
  out += auto_arrange ? "true" : "false";
  out += "\n\n[positions]\n";
  for (auto &kv : cells_)
    out += escape_key(kv.first) + "=" + std::to_string(kv.second.col) + "," +
           std::to_string(kv.second.row) + "\n";
  return out;
}

bool PositionStore::load(const std::string &path) {
  path_ = path;
  std::string text;
  if (!read_file(path, text)) {
    cells_.clear();
    auto_arrange = false;
    return false;
  }
  parse(text);
  return true;
}

bool PositionStore::save() const {
  if (path_.empty()) return false;
  return write_file_atomic(path_, format());
}

bool PositionStore::get(const std::string &key, Cell *out) const {
  auto it = cells_.find(key);
  if (it == cells_.end()) return false;
  if (out) *out = it->second;
  return true;
}

void PositionStore::set(const std::string &key, Cell c) { cells_[key] = c; }
void PositionStore::erase(const std::string &key) { cells_.erase(key); }

void PositionStore::rename(const std::string &from, const std::string &to) {
  auto it = cells_.find(from);
  if (it == cells_.end() || from == to) return;
  Cell c = it->second;
  cells_.erase(it);
  cells_[to] = c;
}

void PositionStore::prune(const std::vector<std::string> &keep) {
  std::set<std::string> k(keep.begin(), keep.end());
  for (auto it = cells_.begin(); it != cells_.end();) {
    if (!k.count(it->first)) it = cells_.erase(it);
    else ++it;
  }
}

std::string default_positions_path() { return config_home() + "/sde/desktop-positions.ini"; }

std::vector<Cell> place_icons(const std::vector<std::string> &keys, int cols, int rows,
                              const PositionStore &store) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  std::vector<Cell> out(keys.size());
  std::vector<bool> placed(keys.size(), false);
  std::set<Cell> taken;
  if (!store.auto_arrange) {
    for (size_t i = 0; i < keys.size(); ++i) {
      Cell c;
      if (!store.get(keys[i], &c)) continue;
      if (c.col >= cols || c.row >= rows || taken.count(c)) continue;
      out[i] = c;
      placed[i] = true;
      taken.insert(c);
    }
  }
  Cell next{0, 0};
  for (size_t i = 0; i < keys.size(); ++i) {
    if (placed[i]) continue;
    while (taken.count(next)) {
      if (++next.row >= rows) {
        next.row = 0;
        ++next.col;
      }
    }
    out[i] = next;
    taken.insert(next);
  }
  return out;
}

}  // namespace desk
}  // namespace sde
