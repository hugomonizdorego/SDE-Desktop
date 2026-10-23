// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_model.h"

#include <dirent.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "util.h"

namespace sde {
namespace menu {

namespace {

// Decodes the UTF-8 sequence at |i|: false when it is not valid UTF-8
// (stray continuation bytes, overlong forms, surrogates, > U+10FFFF).
bool decode_utf8(const std::string &s, size_t i, uint32_t &cp, size_t &len) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) {
    cp = c;
    len = 1;
    return true;
  }
  if (c >= 0xC2 && c <= 0xDF) len = 2, cp = c & 0x1F;
  else if (c >= 0xE0 && c <= 0xEF) len = 3, cp = c & 0x0F;
  else if (c >= 0xF0 && c <= 0xF4) len = 4, cp = c & 0x07;
  else return false;
  if (i + len > s.size()) return false;
  for (size_t k = 1; k < len; ++k) {
    unsigned char d = static_cast<unsigned char>(s[i + k]);
    if ((d & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (d & 0x3F);
  }
  if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
      (cp >= 0xD800 && cp <= 0xDFFF))
    return false;
  return true;
}

void encode_utf8(uint32_t cp, std::string &out) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Base letters of U+00C0..U+017F; "" keeps the character (x and division
// signs), nullptr: not a Latin letter of these blocks.
const char *latin_base(uint32_t cp) {
  static const char *const latin1[64] = {
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
      "d", "n", "o", "o", "o", "o", "o", "",  "o", "u", "u", "u", "u", "y", "th", "ss",
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
      "d", "n", "o", "o", "o", "o", "o", "",  "o", "u", "u", "u", "u", "y", "th", "y"};
  if (cp >= 0xC0 && cp <= 0xFF) return latin1[cp - 0xC0];
  // Latin Extended-A: runs of letters with the same base, upper and lower.
  static const struct {
    uint32_t last;
    const char *base;
  } ext[] = {{0x105, "a"}, {0x10D, "c"}, {0x111, "d"}, {0x11B, "e"}, {0x123, "g"},
             {0x127, "h"}, {0x131, "i"}, {0x133, "ij"}, {0x135, "j"}, {0x138, "k"},
             {0x142, "l"}, {0x14B, "n"}, {0x151, "o"}, {0x153, "oe"}, {0x159, "r"},
             {0x161, "s"}, {0x167, "t"}, {0x173, "u"}, {0x175, "w"}, {0x178, "y"},
             {0x17E, "z"}, {0x17F, "s"}};
  if (cp >= 0x100 && cp <= 0x17F)
    for (auto &r : ext)
      if (cp <= r.last) return r.base;
  return nullptr;
}

bool separator(char c) {
  return c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == '/' || c == ',' ||
         c == ':' || c == '+' || c == '&' || c == '[';
}

// |term| starts a word of |text| (not at its very beginning).
bool word_start(const std::string &text, const std::string &term) {
  for (size_t p = text.find(term, 1); p != std::string::npos; p = text.find(term, p + 1))
    if (separator(text[p - 1])) return true;
  return false;
}

// The letters of |term| appear in |text| in order, the first one at the
// start of a word.
bool fuzzy(const std::string &text, const std::string &term) {
  for (size_t w = 0; w < text.size(); ++w) {
    if ((w > 0 && !separator(text[w - 1])) || text[w] != term[0]) continue;
    size_t j = 1;
    for (size_t i = w + 1; i < text.size() && j < term.size(); ++i)
      if (text[i] == term[j]) ++j;
    if (j == term.size()) return true;
  }
  return false;
}

}  // namespace

std::string fold(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out += char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      ++i;
      continue;
    }
    uint32_t cp = 0;
    size_t len = 1;
    if (!decode_utf8(s, i, cp, len)) {
      out += s[i++];
      continue;
    }
    i += len;
    if (const char *base = latin_base(cp)) {
      if (*base) {
        out += base;
        continue;
      }
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) cp += 0x20;  // Greek capitals
    else if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;            // Cyrillic capitals
    else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;            // Cyrillic with signs
    encode_utf8(cp, out);
  }
  return out;
}

std::string clean_text(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7F) {
      out += ' ';
      ++i;
      continue;
    }
    uint32_t cp = 0;
    size_t len = 1;
    if (!decode_utf8(s, i, cp, len)) {
      out += "\xEF\xBF\xBD";  // U+FFFD REPLACEMENT CHARACTER
      ++i;
      continue;
    }
    out.append(s, i, len);
    i += len;
  }
  return trim(out);
}

SearchKeys search_keys(const DesktopEntry &e) {
  SearchKeys k;
  k.name = fold(e.name);
  k.generic = fold(e.generic_name);
  k.comment = fold(e.comment);
  auto argv = e.exec.empty() ? std::vector<std::string>() : e.command();
  k.exec = fold(basename_of(argv.empty() ? "" : argv[0]));
  for (auto &kw : e.keywords) k.keywords.push_back(fold(kw));
  std::string id = fold(e.id);
  if (ends_with(id, ".desktop")) id.resize(id.size() - 8);
  std::string part;
  for (size_t i = 0; i <= id.size(); ++i) {
    if (i < id.size() && id[i] != '.' && id[i] != '-' && id[i] != '_') {
      part += id[i];
      continue;
    }
    if (part.size() >= 3 && part != "org" && part != "com" && part != "net")
      k.id_parts.push_back(part);
    part.clear();
  }
  // 0.2: SDE Settings becomes the Control Center; both names find it.
  if (e.id == "sde-settings.desktop")
    for (const char *alias : {"settings", "control center", "control panel", "preferences"})
      k.keywords.push_back(alias);
  return k;
}

std::vector<std::string> search_terms(const std::string &query) {
  std::vector<std::string> out;
  for (auto &t : split(fold(query), ' '))
    if (!t.empty()) out.push_back(t);
  return out;
}

int match_keys(const SearchKeys &k, const std::vector<std::string> &terms) {
  int total = 0;
  for (auto &t : terms) {
    if (t.empty()) continue;
    int best = 0;
    if (starts_with(k.name, t)) best = 100;
    else if (word_start(k.name, t)) best = 80;
    else if (k.name.find(t) != std::string::npos) best = 60;
    else if (starts_with(k.generic, t) || word_start(k.generic, t)) best = 45;
    else if (k.generic.find(t) != std::string::npos) best = 40;
    else if (!k.exec.empty() && starts_with(k.exec, t)) best = 35;
    if (!best)
      for (auto &kw : k.keywords)
        if (starts_with(kw, t) || word_start(kw, t)) {
          best = 30;
          break;
        }
    if (!best)
      for (auto &part : k.id_parts)
        if (starts_with(part, t)) {
          best = 25;
          break;
        }
    if (!best && k.comment.find(t) != std::string::npos) best = 15;
    if (!best && t.size() >= 3 && fuzzy(k.name, t)) best = 8;
    if (!best) return 0;
    total += best;
  }
  return total;
}

int match_score(const DesktopEntry &e, const std::vector<std::string> &terms) {
  std::vector<std::string> folded;
  for (auto &t : terms) folded.push_back(fold(t));
  return match_keys(search_keys(e), folded);
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

int tile_cols(TileSize s) { return s == TileSize::Small ? 1 : s == TileSize::Medium ? 2 : 4; }
int tile_rows(TileSize s) { return s == TileSize::Small ? 1 : s == TileSize::Large ? 4 : 2; }
char tile_size_code(TileSize s) {
  return s == TileSize::Small ? 's' : s == TileSize::Wide ? 'w' : s == TileSize::Large ? 'l' : 'm';
}
TileSize tile_size_from(char c) {
  return c == 's'   ? TileSize::Small
         : c == 'w' ? TileSize::Wide
         : c == 'l' ? TileSize::Large
                    : TileSize::Medium;
}

namespace {
std::string escape_name(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == ',' || c == ';' || c == ':' || c == '%' || c == '\n' || c == '\r') {
      char buf[4];
      snprintf(buf, sizeof buf, "%%%02X", (unsigned char)c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}
std::string unescape_name(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned v = 0;
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
        isxdigit((unsigned char)s[i + 2]) && sscanf(s.substr(i + 1, 2).c_str(), "%2x", &v) == 1) {
      out += char(v);
      i += 2;
      continue;
    }
    out += s[i];
  }
  return out;
}
}  // namespace

std::vector<TileGroup> parse_tile_groups(const std::string &text) {
  std::vector<TileGroup> out;
  std::vector<std::string> seen;
  for (auto &part : split(text, ';')) {
    if (trim(part).empty()) continue;
    auto fields = split(part, ',');
    TileGroup g;
    g.name = trim(unescape_name(fields[0]));
    for (size_t i = 1; i < fields.size(); ++i) {
      std::string f = trim(fields[i]);
      if (f.empty()) continue;
      Tile t;
      size_t colon = f.rfind(':');
      t.id = trim(colon == std::string::npos ? f : f.substr(0, colon));
      if (colon != std::string::npos && colon + 1 < f.size()) t.size = tile_size_from(f[colon + 1]);
      if (t.id.empty() || std::find(seen.begin(), seen.end(), t.id) != seen.end()) continue;
      seen.push_back(t.id);
      g.tiles.push_back(t);
    }
    out.push_back(g);
  }
  return out;
}

std::string format_tile_groups(const std::vector<TileGroup> &groups) {
  std::string s;
  for (auto &g : groups) {
    if (!s.empty()) s += ";";
    s += escape_name(g.name);
    for (auto &t : g.tiles) s += "," + t.id + ":" + tile_size_code(t.size);
  }
  // Nothing pinned is saved as ";" so that the defaults do not come back.
  return s.empty() ? ";" : s;
}

std::vector<std::string> tile_ids(const std::vector<TileGroup> &groups) {
  std::vector<std::string> out;
  for (auto &g : groups)
    for (auto &t : g.tiles) out.push_back(t.id);
  return out;
}

std::vector<TileGroup> groups_from_favorites(const std::vector<std::string> &favorites,
                                             bool user_list, const std::string &first,
                                             const std::string &second) {
  std::vector<TileGroup> out;
  if (favorites.empty()) return out;
  TileGroup a{first, {}}, b{second, {}};
  for (size_t i = 0; i < favorites.size(); ++i)
    (user_list || i < 6 ? a : b).tiles.push_back(Tile{favorites[i], TileSize::Medium});
  out.push_back(a);
  if (!b.tiles.empty()) out.push_back(b);
  return out;
}

std::vector<TileCell> pack_tiles(const std::vector<Tile> &tiles, int columns, int *rows) {
  columns = std::max(1, columns);
  std::vector<std::vector<char>> used;  // [row][col]
  auto free_at = [&](int r, int c, int w, int h) {
    for (int y = r; y < r + h && y < int(used.size()); ++y)
      for (int x = c; x < c + w; ++x)
        if (used[size_t(y)][size_t(x)]) return false;
    return true;
  };
  std::vector<TileCell> out;
  int height = 0;
  for (auto &t : tiles) {
    TileCell cell;
    cell.cols = std::min(columns, tile_cols(t.size));
    cell.rows = tile_rows(t.size);
    bool placed = false;
    for (int r = 0; !placed; ++r)
      for (int c = 0; c + cell.cols <= columns && !placed; ++c)
        if (free_at(r, c, cell.cols, cell.rows)) {
          cell.row = r;
          cell.col = c;
          placed = true;
        }
    while (int(used.size()) < cell.row + cell.rows) used.emplace_back(size_t(columns), 0);
    for (int y = cell.row; y < cell.row + cell.rows; ++y)
      for (int x = cell.col; x < cell.col + cell.cols; ++x) used[size_t(y)][size_t(x)] = 1;
    height = std::max(height, cell.row + cell.rows);
    out.push_back(cell);
  }
  if (rows) *rows = height;
  return out;
}

std::vector<Usage> parse_usage(const std::string &text, long long now) {
  std::vector<Usage> out;
  int bare = 0;
  for (auto &line : split(text, '\n')) {
    std::string l = trim(line);
    if (l.empty() || l[0] == '#') continue;
    auto f = split(l, '\t');
    Usage u;
    u.id = trim(f[0]);
    if (u.id.empty()) continue;
    if (f.size() >= 3) {
      char *end = nullptr;
      long c = std::strtol(f[1].c_str(), &end, 10);
      if (end == f[1].c_str() || c < 0) continue;
      long long t = std::strtoll(f[2].c_str(), &end, 10);
      if (end == f[2].c_str()) continue;
      u.count = unsigned(c);
      u.last = t;
    } else if (f.size() == 1 && l.find(' ') == std::string::npos) {
      // Older plain recent list: newest first.
      u.count = 1;
      u.last = now - 60LL * ++bare;
    } else {
      continue;
    }
    auto it = std::find_if(out.begin(), out.end(), [&](const Usage &o) { return o.id == u.id; });
    if (it == out.end()) {
      out.push_back(u);
    } else {
      it->count += u.count;
      it->last = std::max(it->last, u.last);
    }
  }
  return out;
}

std::string format_usage(const std::vector<Usage> &usage) {
  std::string s;
  for (auto &u : usage)
    s += u.id + "\t" + std::to_string(u.count) + "\t" + std::to_string(u.last) + "\n";
  return s;
}

double frecency(const Usage &u, long long now) {
  if (u.count == 0) return 0;
  double age = std::max(0.0, double(now - u.last));
  const double day = 86400, week = 7 * day;
  double score = double(u.count) * std::pow(2.0, -age / week);
  // Opened today: a boost that fades over the day.
  if (age < day) score += 2.0 * (1.0 - age / day);
  return score;
}

void record_launch(std::vector<Usage> &usage, const std::string &id, long long now, size_t cap) {
  if (id.empty()) return;
  auto it = std::find_if(usage.begin(), usage.end(), [&](const Usage &u) { return u.id == id; });
  if (it == usage.end()) {
    usage.push_back({id, 1, now});
  } else {
    ++it->count;
    it->last = std::max(it->last, now);
  }
  if (usage.size() > cap) {
    // Keep the best |cap|; the application just opened always stays.
    std::stable_sort(usage.begin(), usage.end(), [&](const Usage &a, const Usage &b) {
      if (a.id == id || b.id == id) return a.id == id && b.id != id;
      return frecency(a, now) > frecency(b, now);
    });
    usage.resize(cap);
  }
}

void forget(std::vector<Usage> &usage, const std::string &id) {
  usage.erase(std::remove_if(usage.begin(), usage.end(),
                             [&](const Usage &u) { return u.id == id; }),
              usage.end());
}

std::vector<std::string> rank_frecent(const std::vector<Usage> &usage, long long now, size_t max,
                                      const std::vector<std::string> &exclude) {
  std::vector<const Usage *> list;
  for (auto &u : usage)
    if (u.count && std::find(exclude.begin(), exclude.end(), u.id) == exclude.end())
      list.push_back(&u);
  std::stable_sort(list.begin(), list.end(), [&](const Usage *a, const Usage *b) {
    double fa = frecency(*a, now), fb = frecency(*b, now);
    if (std::fabs(fa - fb) > 1e-9) return fa > fb;
    if (a->last != b->last) return a->last > b->last;
    return a->id < b->id;
  });
  std::vector<std::string> out;
  for (auto *u : list) {
    if (out.size() >= max) break;
    out.push_back(u->id);
  }
  return out;
}

std::vector<std::string> by_last_use(const std::vector<Usage> &usage, size_t max) {
  std::vector<const Usage *> list;
  for (auto &u : usage) list.push_back(&u);
  std::stable_sort(list.begin(), list.end(),
                   [](const Usage *a, const Usage *b) { return a->last > b->last; });
  std::vector<std::string> out;
  for (auto *u : list) {
    if (out.size() >= max) break;
    out.push_back(u->id);
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
  stack_.clear();
  roots_.clear();
  built_ = building_ = false;
}

// Lists one directory; its sub-directories are scanned next, in name order
// (as the recursive scan of alpha 15 did).
void FileIndex::scan_one(const std::string &dir, int depth) {
  if (depth < 0 || items_.size() >= limit_) return;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> subdirs;
  while (dirent *ent = readdir(d)) {
    if (items_.size() >= limit_) break;
    std::string name = ent->d_name;
    if (name.empty() || name[0] == '.') continue;
    std::string path = dir + "/" + name;
    bool is_dir = ent->d_type == DT_DIR || (ent->d_type == DT_UNKNOWN && dir_exists(path));
    items_.push_back({path, fold(name), is_dir});
    if (is_dir) subdirs.push_back(path);
  }
  closedir(d);
  std::sort(subdirs.begin(), subdirs.end());
  for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack_.push_back({*it, depth - 1});
}

void FileIndex::begin(const std::vector<std::string> &roots, size_t limit, int depth) {
  clear();
  limit_ = limit;
  std::vector<std::string> seen;
  for (auto &r : roots) {
    if (r.empty() || std::find(seen.begin(), seen.end(), r) != seen.end() || !dir_exists(r))
      continue;
    seen.push_back(r);
    // Roots below the home folder are scanned on their own; the home folder
    // itself only one level deep, so that its big trees do not dominate.
    roots_.push_back({r, r == home_dir() ? std::min(depth, 1) : depth});
  }
  std::reverse(roots_.begin(), roots_.end());
  building_ = true;
}

bool FileIndex::step(double budget_ms) {
  if (!building_) return built_;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  auto elapsed = [&] {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) * 1000.0 + (t.tv_nsec - t0.tv_nsec) / 1e6;
  };
  while (items_.size() < limit_) {
    if (stack_.empty()) {
      if (roots_.empty()) break;
      stack_.push_back(roots_.back());
      roots_.pop_back();
    }
    Pending p = stack_.back();
    stack_.pop_back();
    scan_one(p.dir, p.depth);
    if (budget_ms > 0 && elapsed() >= budget_ms) return false;
  }
  finish();
  return true;
}

void FileIndex::finish() {
  stack_.clear();
  roots_.clear();
  // A folder can be reached from two roots (home and ~/Documents).
  std::sort(items_.begin(), items_.end(), [](const Item &a, const Item &b) { return a.path < b.path; });
  items_.erase(std::unique(items_.begin(), items_.end(),
                           [](const Item &a, const Item &b) { return a.path == b.path; }),
               items_.end());
  building_ = false;
  built_ = true;
}

void FileIndex::build(const std::vector<std::string> &roots, size_t limit, int depth) {
  begin(roots, limit, depth);
  step(0);
}

bool FileIndex::is_dir(const std::string &path) const {
  auto it = std::lower_bound(items_.begin(), items_.end(), path,
                             [](const Item &a, const std::string &p) { return a.path < p; });
  return it != items_.end() && it->path == path && it->dir;
}

std::vector<FileIndex::Hit> FileIndex::search_hits(const std::vector<std::string> &terms,
                                                   size_t max) const {
  std::vector<std::pair<int, const Item *>> hits;
  bool any = false;
  for (auto &t : terms) any = any || !t.empty();
  if (!any || !built_) return {};
  for (auto &i : items_) {
    bool all = true;
    for (auto &t : terms)
      if (!t.empty() && i.name_lower.find(t) == std::string::npos) {
        all = false;
        break;
      }
    if (!all) continue;
    int rank = starts_with(i.name_lower, terms[0]) ? 0 : 2;
    if (!i.dir) rank += 1;
    hits.push_back({rank, &i});
  }
  std::stable_sort(hits.begin(), hits.end(), [](auto &a, auto &b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second->path.size() < b.second->path.size();
  });
  std::vector<Hit> out;
  for (auto &h : hits) {
    if (out.size() >= max) break;
    out.push_back({h.second->path, h.second->dir});
  }
  return out;
}

std::vector<std::string> FileIndex::search(const std::vector<std::string> &terms,
                                           size_t max) const {
  std::vector<std::string> out;
  for (auto &h : search_hits(terms, max)) out.push_back(h.path);
  return out;
}

}  // namespace menu
}  // namespace sde
