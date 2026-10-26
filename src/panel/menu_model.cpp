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
      // Not UTF-8: most likely a legacy Latin-1 (Windows-1252) file. Its
      // characters are what was meant, and the UI font has them; U+FFFD
      // was drawn as an empty box (FLTK has no fallback fonts).
      static const uint16_t cp1252[32] = {
          0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
          0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
          0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
          0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
      uint32_t latin = c >= 0xA0 ? c : cp1252[c - 0x80];
      if (latin) encode_utf8(latin, out);
      else out += '?';
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
// |slash|: also "/", which separates the parts of a folder.
std::string escape_name(const std::string &s, bool slash = false) {
  std::string out;
  for (char c : s) {
    if (c == ',' || c == ';' || c == ':' || c == '%' || c == '\n' || c == '\r' || (slash && c == '/')) {
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
  auto fresh = [&](const std::string &id) {
    if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) return false;
    seen.push_back(id);
    return true;
  };
  for (auto &part : split(text, ';')) {
    if (trim(part).empty()) continue;
    // Empty fields kept: a group without a name starts with ",".
    auto fields = split(part, ',', true);
    TileGroup g;
    g.name = trim(unescape_name(fields[0]));
    for (size_t i = 1; i < fields.size(); ++i) {
      std::string f = trim(fields[i]);
      if (f.empty()) continue;
      Tile t;
      bool folder = starts_with(f, "folder:");
      size_t colon = f.rfind(':');
      if (folder && colon < 7) colon = std::string::npos;  // no size after the folder
      if (colon != std::string::npos && colon + 1 < f.size()) t.size = tile_size_from(f[colon + 1]);
      std::string body = trim(colon == std::string::npos ? f : f.substr(0, colon));
      if (folder) {
        auto parts = split(body.substr(7), '/', true);
        t.folder = true;
        t.name = parts.empty() ? "" : trim(unescape_name(parts[0]));
        for (size_t k = 1; k < parts.size(); ++k) {
          std::string id = trim(unescape_name(parts[k]));
          if (fresh(id)) t.apps.push_back(id);
        }
        if (t.apps.empty()) continue;
        if (t.apps.size() == 1) t = Tile{t.apps[0], t.size};
        g.tiles.push_back(t);
        continue;
      }
      t.id = body;
      if (fresh(t.id)) g.tiles.push_back(t);
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
    for (auto &t : g.tiles) {
      if (t.folder) {
        s += ",folder:" + escape_name(t.name, true);
        for (auto &id : t.apps) s += "/" + escape_name(id, true);
      } else {
        s += "," + t.id;
      }
      s += std::string(":") + tile_size_code(t.size);
    }
  }
  // Nothing pinned is saved as ";" so that the defaults do not come back.
  return s.empty() ? ";" : s;
}

std::vector<std::string> tile_ids(const std::vector<TileGroup> &groups) {
  std::vector<std::string> out;
  for (auto &g : groups)
    for (auto &t : g.tiles)
      for (auto &id : tile_apps(t)) out.push_back(id);
  return out;
}

std::vector<std::string> tile_apps(const Tile &t) {
  if (t.folder) return t.apps;
  return t.id.empty() ? std::vector<std::string>() : std::vector<std::string>{t.id};
}

Tile make_folder(const std::string &name, const std::vector<std::string> &apps, TileSize size) {
  Tile t;
  t.folder = true;
  t.name = name;
  t.apps = apps;
  t.size = size;
  return t;
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

TileCategory tile_category(const DesktopEntry &e) {
  std::string c = e.menu_category();
  if (c == "Internet") return TileCategory::Internet;
  if (c == "Office") return TileCategory::Office;
  if (c == "Multimedia") return TileCategory::Multimedia;
  if (c == "Graphics") return TileCategory::Graphics;
  if (c == "Development") return TileCategory::Development;
  if (c == "Games") return TileCategory::Games;
  if (c == "Education" || c == "Science") return TileCategory::Education;
  if (c == "Settings" || c == "System") return TileCategory::System;
  if (c == "Accessories") return TileCategory::Accessories;
  return TileCategory::Other;
}

namespace {

// The roles of each category's default tiles, in order; an application
// fits a role with any of its freedesktop categories.
const std::vector<std::vector<std::string>> &category_roles(TileCategory c) {
  static const std::vector<std::vector<std::vector<std::string>>> roles = {
      // Internet
      {{"WebBrowser"}, {"Email"}, {"Chat", "InstantMessaging", "IRCClient"},
       {"VideoConference", "Telephony"}, {"FileTransfer", "P2P"}, {"RemoteAccess"}, {"Feed", "News"}},
      // Office
      {{"WordProcessor"}, {"Spreadsheet"}, {"Presentation"}, {"Viewer"},
       {"Calendar", "ContactManagement", "ProjectManagement"}, {"Database"}, {"Finance"}, {"Dictionary"}},
      // Multimedia
      {{"Player"}, {"Music", "Audio"}, {"Video", "TV"}, {"AudioVideoEditing", "Recorder"},
       {"DiscBurning"}, {"Mixer", "Sequencer"}},
      // Graphics
      {{"Photography", "Viewer"}, {"RasterGraphics"}, {"VectorGraphics"}, {"2DGraphics"},
       {"3DGraphics"}, {"Scanning", "OCR"}},
      // Development
      {{"IDE"}, {"TextEditor"}, {"Debugger"}, {"RevisionControl"}, {"GUIDesigner"},
       {"WebDevelopment"}, {"Building"}, {"Profiling", "Translation"}},
      // Games, Education & Science: no roles, A-Z
      {},
      {},
      // System & Settings
      {{"FileManager"}, {"TerminalEmulator"}, {"DesktopSettings"}, {"PackageManager"}, {"Monitor"},
       {"HardwareSettings"}, {"Security"}},
      // Accessories
      {{"TextEditor"}, {"Calculator"}, {"Archiving", "Compression"}, {"Clock"}, {"Accessibility"}},
  };
  static const std::vector<std::vector<std::string>> none;
  int i = int(c);
  return i >= 0 && i < int(roles.size()) ? roles[size_t(i)] : none;
}

bool same_name(const std::string &a, const std::string &b) { return lower(trim(a)) == lower(trim(b)); }

int group_named(const std::vector<TileGroup> &groups, const std::string &name) {
  for (size_t i = 0; i < groups.size(); ++i)
    if (same_name(groups[i].name, name)) return int(i);
  return -1;
}

const std::string &category_name(const std::vector<std::string> &names, TileCategory c) {
  static const std::string empty;
  size_t i = size_t(c);
  return i < names.size() ? names[i] : empty;
}

bool pinned_in(const std::vector<TileGroup> &groups, const std::string &id) {
  return find_app(groups, id).group >= 0;
}

// Whole rows of the six-cell group: medium tiles by three; with one, two,
// four or five tiles the first of a row is wide (W M, W M / M M M).
bool wide_at(size_t n, size_t i) {
  return n == 1 || (n == 2 && i == 0) || (n == 4 && i % 2 == 0) || (n == 5 && i == 0);
}

const DesktopEntry *installed(const std::vector<DesktopEntry> &apps, const std::string &id) {
  if (id.empty()) return nullptr;
  for (auto &a : apps)
    if (a.id == id) return &a;
  return nullptr;
}

bool has_category(const DesktopEntry &e, const std::string &cat) {
  return std::find(e.categories.begin(), e.categories.end(), cat) != e.categories.end();
}

}  // namespace

std::vector<TileGroup> category_groups(const std::vector<DesktopEntry> &apps,
                                       const std::vector<std::string> &preferred,
                                       const std::vector<std::string> &names, size_t per_group) {
  std::vector<TileGroup> out;
  for (int ci = 0; ci < kTileCategories; ++ci) {
    TileCategory c = TileCategory(ci);
    const auto &roles = category_roles(c);
    struct Candidate {
      const DesktopEntry *e;
      size_t role;  // roles.size(): none of them
      bool sde;
      std::string sort;
    };
    std::vector<Candidate> list;
    for (auto &a : apps) {
      if (tile_category(a) != c) continue;
      size_t role = roles.size();
      for (size_t r = 0; r < roles.size() && role == roles.size(); ++r)
        for (auto &cat : roles[r])
          if (std::find(a.categories.begin(), a.categories.end(), cat) != a.categories.end()) {
            role = r;
            break;
          }
      bool sde = std::find(preferred.begin(), preferred.end(), a.id) != preferred.end();
      list.push_back({&a, role, sde, lower(a.name)});
    }
    if (list.empty()) continue;
    std::stable_sort(list.begin(), list.end(), [](const Candidate &x, const Candidate &y) {
      if (x.role != y.role) return x.role < y.role;
      if (x.sde != y.sde) return x.sde;
      return x.sort < y.sort;
    });
    // One application per role first (a browser, a mail client, a chat
    // before a second browser), then the others in the same order.
    std::vector<const DesktopEntry *> picked;
    size_t last_role = size_t(-1);
    for (auto &cand : list)
      if (cand.role < roles.size() && cand.role != last_role && picked.size() < per_group) {
        picked.push_back(cand.e);
        last_role = cand.role;
      }
    for (auto &cand : list)
      if (picked.size() < per_group && std::find(picked.begin(), picked.end(), cand.e) == picked.end())
        picked.push_back(cand.e);
    size_t n = picked.size();
    TileGroup g{category_name(names, c), {}};
    for (size_t i = 0; i < n; ++i)
      g.tiles.push_back(Tile{picked[i]->id, wide_at(n, i) ? TileSize::Wide : TileSize::Medium});
    out.push_back(g);
  }
  return out;
}

bool category_shaped(const std::vector<TileGroup> &groups, const std::vector<DesktopEntry> &apps,
                     const std::vector<std::vector<std::string>> &names, size_t per_group) {
  if (groups.empty()) return false;
  int last = -1;
  for (auto &g : groups) {
    // The category the group is named after, later than the one before.
    int cat = -1;
    for (size_t c = 0; c < names.size() && cat < 0; ++c)
      for (auto &n : names[c])
        if (!trim(n).empty() && same_name(g.name, n)) {
          cat = int(c);
          break;
        }
    if (cat <= last || g.tiles.empty() || g.tiles.size() > per_group) return false;
    last = cat;
    for (auto &t : g.tiles) {
      if (t.folder || (t.size != TileSize::Medium && t.size != TileSize::Wide)) return false;
      const DesktopEntry *e = installed(apps, t.id);
      if (e && int(tile_category(*e)) != cat) return false;
    }
  }
  return true;
}

namespace {

// A role of the default favorites: SDE's application for it, the user's
// default (a member of DefaultApps), else an application with every
// category of one of |cats| (tried in order).
struct FavoriteRole {
  FavoriteGroup group;
  bool main;  // a medium tile, else a small one
  const char *sde;
  std::string DefaultApps::*fallback;
  std::vector<std::vector<std::string>> cats;
};

const std::vector<FavoriteRole> &favorite_roles() {
  using G = FavoriteGroup;
  static const std::vector<FavoriteRole> roles = {
      {G::Work, true, nullptr, &DefaultApps::browser, {{"WebBrowser"}}},
      {G::Work, true, "liria.desktop", &DefaultApps::files, {{"FileManager"}}},
      {G::Work, true, nullptr, &DefaultApps::mail, {{"Email"}}},
      {G::Work, true, "niny.desktop", nullptr, {{"TextEditor"}}},
      {G::Work, true, "xanara.desktop", &DefaultApps::documents, {{"Office", "Viewer"}}},
      {G::Work, false, "org.debion.Calculator.desktop", nullptr, {{"Calculator"}}},
      {G::Work, false, "taca.desktop", nullptr, {{"Archiving"}, {"Compression"}}},
      {G::Media, true, "sde-gallery.desktop", nullptr, {{"Photography"}}},
      {G::Media, true, "sde-view.desktop", nullptr, {{"RasterGraphics", "Viewer"}, {"Viewer", "Photography"}}},
      {G::Media, false, nullptr, &DefaultApps::video, {{"Video", "Player"}, {"AudioVideo", "Player"}}},
      {G::Media, false, nullptr, &DefaultApps::music, {{"Audio", "Player"}, {"Music", "Player"}}},
      {G::System, true, "sde-settings.desktop", nullptr, {{"DesktopSettings"}}},
      {G::System, true, "merkadu.desktop", nullptr, {{"PackageManager"}}},
      {G::System, false, "sde-update.desktop", nullptr, {}},
      {G::System, false, "sde-system-monitor.desktop", nullptr, {{"Monitor"}}},
      {G::System, false, nullptr, &DefaultApps::terminal, {{"TerminalEmulator"}}},
  };
  return roles;
}

const std::string &favorite_name(const std::vector<std::string> &names, FavoriteGroup g) {
  static const std::string empty;
  size_t i = size_t(g);
  return i < names.size() ? names[i] : empty;
}

}  // namespace

std::vector<TileGroup> favorite_groups(const std::vector<DesktopEntry> &apps, const DefaultApps &defaults,
                                       const std::vector<std::string> &preferred,
                                       const std::vector<std::string> &names) {
  const auto &roles = favorite_roles();
  // SDE's application of a role is kept for it: the category matches of
  // other roles leave it alone (Merkadu's role does not take the Software
  // Updater, which is a package manager too).
  std::vector<std::string> reserved;
  for (auto &r : roles)
    if (r.sde && installed(apps, r.sde)) reserved.push_back(r.sde);
  std::vector<std::string> used;
  auto unused = [&](const DesktopEntry *e) {
    return e && std::find(used.begin(), used.end(), e->id) == used.end();
  };
  auto kept = [&](const DesktopEntry &e) {
    return std::find(reserved.begin(), reserved.end(), e.id) != reserved.end();
  };
  std::vector<TileGroup> out;
  for (int gi = 0; gi < kFavoriteGroups; ++gi) {
    std::vector<std::string> mains, others;
    for (auto &r : roles) {
      if (int(r.group) != gi) continue;
      const DesktopEntry *pick = r.sde ? installed(apps, r.sde) : nullptr;
      if (!unused(pick) && r.fallback) {
        pick = installed(apps, defaults.*r.fallback);
        if (pick && kept(*pick)) pick = nullptr;
      }
      for (size_t k = 0; !unused(pick) && k < r.cats.size(); ++k) {
        pick = nullptr;
        bool pick_sde = false;
        for (auto &a : apps) {
          bool fits = unused(&a) && !kept(a);
          for (auto &c : r.cats[k]) fits = fits && has_category(a, c);
          if (!fits) continue;
          bool sde = std::find(preferred.begin(), preferred.end(), a.id) != preferred.end();
          if (!pick || (sde && !pick_sde) || (sde == pick_sde && lower(a.name) < lower(pick->name)))
            pick = &a, pick_sde = sde;
        }
      }
      if (!unused(pick)) continue;
      used.push_back(pick->id);
      (r.main ? mains : others).push_back(pick->id);
    }
    // Small tiles only next to medium ones; up to four share the place of
    // a medium tile.
    if (mains.empty()) mains.swap(others);
    if (mains.empty()) continue;
    if (others.size() > 4) others.resize(4);
    size_t units = mains.size() + (others.empty() ? 0 : 1);
    TileGroup g{favorite_name(names, FavoriteGroup(gi)), {}};
    for (size_t i = 0; i < mains.size(); ++i)
      g.tiles.push_back(Tile{mains[i], wide_at(units, i) ? TileSize::Wide : TileSize::Medium});
    for (auto &id : others) g.tiles.push_back(Tile{id, TileSize::Small});
    out.push_back(g);
  }
  return out;
}

FavoriteGroup favorite_group(const DesktopEntry &e) {
  switch (tile_category(e)) {
    case TileCategory::Multimedia:
    case TileCategory::Graphics:
    case TileCategory::Games: return FavoriteGroup::Media;
    case TileCategory::System: return FavoriteGroup::System;
    default: return FavoriteGroup::Work;
  }
}

bool pin_favorite(std::vector<TileGroup> &groups, const DesktopEntry &e, const std::vector<DesktopEntry> &apps,
                  const std::vector<std::string> &names, TileSize size) {
  if (e.id.empty() || pinned_in(groups, e.id)) return false;
  FavoriteGroup kind = favorite_group(e);
  int target = group_named(groups, favorite_name(names, kind));
  if (target < 0) {
    // A favorites group the user renamed: the one with the most
    // applications of this kind; else the first group.
    int best = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
      int n = 0;
      for (auto &t : groups[g].tiles)
        for (auto &id : tile_apps(t))
          if (const DesktopEntry *a = installed(apps, id)) n += favorite_group(*a) == kind ? 1 : 0;
      if (n > best) best = n, target = int(g);
    }
    if (target < 0 && !groups.empty()) target = 0;
  }
  if (target < 0) {
    groups.push_back(TileGroup{favorite_name(names, kind), {}});
    target = int(groups.size()) - 1;
  }
  groups[size_t(target)].tiles.push_back(Tile{e.id, size});
  return true;
}

std::vector<TileGroup> adopt_favorites(const std::vector<TileGroup> &groups, const std::vector<bool> &automatic,
                                       const std::vector<std::string> &picks,
                                       const std::vector<TileGroup> &favorites,
                                       const std::vector<DesktopEntry> &apps,
                                       const std::vector<std::string> &names) {
  auto is_auto = [&](size_t g) { return g < automatic.size() && automatic[g]; };
  size_t first = groups.size();
  for (size_t g = 0; g < groups.size() && first == groups.size(); ++g)
    if (is_auto(g)) first = g;
  if (first == groups.size()) return groups;
  // The user's own groups stay; what they hold is not repeated among the
  // favorites.
  std::vector<TileGroup> own;
  for (size_t g = 0; g < groups.size(); ++g)
    if (!is_auto(g)) own.push_back(groups[g]);
  std::vector<TileGroup> fav;
  for (auto g : favorites) {
    g.tiles.erase(std::remove_if(g.tiles.begin(), g.tiles.end(),
                                 [&](const Tile &t) { return pinned_in(own, t.id); }),
                  g.tiles.end());
    if (!g.tiles.empty()) fav.push_back(g);
  }
  // Applications the user pinned into the old groups join the favorites
  // group of their kind (made where it belongs when it is missing).
  for (size_t g = first; g < groups.size(); ++g) {
    if (!is_auto(g)) continue;
    for (auto &t : groups[g].tiles)
      for (auto &id : tile_apps(t)) {
        const DesktopEntry *e = installed(apps, id);
        if (!e || std::find(picks.begin(), picks.end(), id) != picks.end() || pinned_in(own, id) ||
            pinned_in(fav, id))
          continue;
        FavoriteGroup kind = favorite_group(*e);
        int target = group_named(fav, favorite_name(names, kind));
        if (target < 0) {
          size_t at = 0;
          for (int k = 0; k < int(kind); ++k)
            if (at < fav.size() && same_name(fav[at].name, favorite_name(names, FavoriteGroup(k)))) ++at;
          fav.insert(fav.begin() + long(at), TileGroup{favorite_name(names, kind), {}});
          target = int(at);
        }
        fav[size_t(target)].tiles.push_back(Tile{id, t.folder ? TileSize::Medium : t.size});
      }
  }
  // The favorites where the first old group was.
  std::vector<TileGroup> out;
  for (size_t g = 0; g < groups.size(); ++g) {
    if (g == first) out.insert(out.end(), fav.begin(), fav.end());
    if (!is_auto(g)) out.push_back(groups[g]);
  }
  return out;
}

// ---------------------------------------------------------------- folders

TilePlace find_app(const std::vector<TileGroup> &groups, const std::string &id) {
  if (id.empty()) return {};
  for (size_t g = 0; g < groups.size(); ++g)
    for (size_t i = 0; i < groups[g].tiles.size(); ++i) {
      const Tile &t = groups[g].tiles[i];
      if (!t.folder && t.id == id) return {int(g), int(i), -1};
      for (size_t m = 0; t.folder && m < t.apps.size(); ++m)
        if (t.apps[m] == id) return {int(g), int(i), int(m)};
    }
  return {};
}

TilePlace take_app(std::vector<TileGroup> &groups, const std::string &id, Tile *tile) {
  TilePlace p = find_app(groups, id);
  if (p.group < 0) return p;
  auto &tiles = groups[size_t(p.group)].tiles;
  Tile &t = tiles[size_t(p.index)];
  if (p.member < 0) {
    if (tile) *tile = t;
    tiles.erase(tiles.begin() + p.index);
    return p;
  }
  if (tile) *tile = Tile{id, TileSize::Medium};
  t.apps.erase(t.apps.begin() + p.member);
  // A folder holds two applications or more.
  if (t.apps.size() == 1) t = Tile{t.apps[0], t.size};
  else if (t.apps.empty()) tiles.erase(tiles.begin() + p.index);
  return p;
}

bool drop_app(std::vector<TileGroup> &groups, const std::string &id, TileSize size, const TileDrop &drop,
              int *source, bool *made, TilePlace *at) {
  if (source) *source = -1;
  if (made) *made = false;
  if (id.empty() || drop.group < 0 || drop.group > int(groups.size())) return false;
  TilePlace from = find_app(groups, id);
  TileDrop d = drop;
  bool onto = d.kind != TileDrop::Place;
  if (onto && (d.group >= int(groups.size()) || d.index < 0 || d.index >= int(groups[size_t(d.group)].tiles.size())))
    return false;
  if (from.group == d.group && from.index == d.index && from.member < 0 && onto) return false;  // onto itself
  if (from.group == d.group && from.member < 0 && !onto && (d.index == from.index || d.index == from.index + 1))
    return false;  // to where it is
  if (onto && from.group == d.group && from.index == d.index) {
    // Inside its own folder: dropped on the folder it stays, else it moves.
    if (d.kind == TileDrop::Merge) return false;
    auto &apps = groups[size_t(d.group)].tiles[size_t(d.index)].apps;
    int to = std::max(0, std::min(d.member, int(apps.size())));
    if (to > from.member) --to;
    if (to == from.member) return false;
    apps.erase(apps.begin() + from.member);
    apps.insert(apps.begin() + to, id);
    if (at) *at = {d.group, d.index, to};
    return true;
  }
  Tile tile{id, size};
  if (from.group >= 0) {
    take_app(groups, id, &tile);
    if (source) *source = from.group;
    // Its tile left the group: the tiles after it moved up by one.
    if (from.member < 0 && from.group == d.group && from.index < d.index) --d.index;
  }
  if (!onto) {
    if (d.group >= int(groups.size())) {
      groups.push_back(TileGroup{"", {tile}});
      if (at) *at = {int(groups.size()) - 1, 0, -1};
      return true;
    }
    auto &tiles = groups[size_t(d.group)].tiles;
    int index = std::max(0, std::min(d.index, int(tiles.size())));
    tiles.insert(tiles.begin() + index, tile);
    if (at) *at = {d.group, index, -1};
    return true;
  }
  Tile &target = groups[size_t(d.group)].tiles[size_t(d.index)];
  if (target.folder) {
    int m = d.kind == TileDrop::IntoFolder ? std::max(0, std::min(d.member, int(target.apps.size())))
                                           : int(target.apps.size());
    target.apps.insert(target.apps.begin() + m, id);
    if (at) *at = {d.group, d.index, m};
  } else {
    // An application's tile becomes a folder of both, in its size.
    target = make_folder("", {target.id, id}, target.size);
    if (made) *made = true;
    if (at) *at = {d.group, d.index, 1};
  }
  return true;
}

bool drop_tile(std::vector<TileGroup> &groups, int group, int index, const TileDrop &drop, TilePlace *at) {
  if (group < 0 || group >= int(groups.size()) || index < 0 || index >= int(groups[size_t(group)].tiles.size()))
    return false;
  if (drop.kind != TileDrop::Place || drop.group < 0 || drop.group > int(groups.size())) return false;
  if (drop.group == group && (drop.index == index || drop.index == index + 1)) return false;
  auto &from = groups[size_t(group)].tiles;
  Tile t = from[size_t(index)];
  from.erase(from.begin() + index);
  if (drop.group >= int(groups.size())) {
    groups.push_back(TileGroup{"", {t}});
    if (at) *at = {int(groups.size()) - 1, 0, -1};
    return true;
  }
  int to = drop.index - (drop.group == group && index < drop.index ? 1 : 0);
  auto &tiles = groups[size_t(drop.group)].tiles;
  to = std::max(0, std::min(to, int(tiles.size())));
  tiles.insert(tiles.begin() + to, t);
  if (at) *at = {drop.group, to, -1};
  return true;
}

std::string folder_name(const std::vector<std::string> &ids, const std::vector<DesktopEntry> &apps,
                        const std::vector<std::string> &names) {
  int cat = -1;
  for (auto &id : ids) {
    const DesktopEntry *e = installed(apps, id);
    int c = e ? int(tile_category(*e)) : int(TileCategory::Other);
    if (c == int(TileCategory::Other) || (cat >= 0 && c != cat)) return "";
    cat = c;
  }
  return cat >= 0 ? category_name(names, TileCategory(cat)) : "";
}

void folder_grid(TileSize size, int &cols, int &rows) {
  cols = size == TileSize::Wide ? 4 : size == TileSize::Large ? 3 : 2;
  rows = size == TileSize::Large ? 3 : 2;
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
