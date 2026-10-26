// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "deb.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

#include "i18n.h"
#include "sha256.h"
#include "util.h"

namespace sde {
namespace debrun {

namespace {

bool same_name(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// ------------------------------------------------------ versions (dpkg)

int order(char c) {
  if (is_digit(c)) return 0;
  if (is_alpha(c)) return (unsigned char)c;
  if (c == '~') return -1;
  if (c) return (unsigned char)c + 256;
  return 0;
}

// dpkg's verrevcmp(): alternating runs of non-digits (compared by order())
// and digits (compared as numbers).
int verrevcmp(const char *a, const char *b) {
  while (*a || *b) {
    int first_diff = 0;
    while ((*a && !is_digit(*a)) || (*b && !is_digit(*b))) {
      int ac = order(*a), bc = order(*b);
      if (ac != bc) return ac - bc;
      ++a;
      ++b;
    }
    while (*a == '0') ++a;
    while (*b == '0') ++b;
    while (is_digit(*a) && is_digit(*b)) {
      if (!first_diff) first_diff = *a - *b;
      ++a;
      ++b;
    }
    if (is_digit(*a)) return 1;
    if (is_digit(*b)) return -1;
    if (first_diff) return first_diff;
  }
  return 0;
}

struct Version {
  unsigned long epoch = 0;
  std::string upstream, revision;
};

Version split_version(const std::string &v) {
  Version out;
  std::string rest = trim(v);
  size_t colon = rest.find(':');
  if (colon != std::string::npos) {
    out.epoch = strtoul(rest.substr(0, colon).c_str(), nullptr, 10);
    rest = rest.substr(colon + 1);
  }
  size_t dash = rest.rfind('-');
  if (dash != std::string::npos) {
    out.revision = rest.substr(dash + 1);
    rest = rest.substr(0, dash);
  }
  out.upstream = rest;
  return out;
}

// ------------------------------------------------------------ ar archive

struct Member {
  std::string name;
  uint64_t offset = 0, size = 0;
};

bool pread_all(int fd, void *buf, size_t n, uint64_t off) {
  char *p = static_cast<char *>(buf);
  while (n) {
    ssize_t r = pread(fd, p, n, off_t(off));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    p += r;
    n -= size_t(r);
    off += uint64_t(r);
  }
  return true;
}

// The members of the ar archive in |fd| (|size| bytes); false when it is
// not one or a header is damaged.
bool ar_members(int fd, uint64_t size, std::vector<Member> *out, std::string *error) {
  char magic[8];
  if (size < 8 || !pread_all(fd, magic, 8, 0) || memcmp(magic, "!<arch>\n", 8) != 0) {
    *error = _("This file is not a Debian package (it is not an ar archive).");
    return false;
  }
  uint64_t pos = 8;
  while (pos < size) {
    if (size - pos < 60) {
      if (size - pos == 1) break;  // padding byte
      *error = _("The package is damaged: an archive header is cut off.");
      return false;
    }
    char h[60];
    if (!pread_all(fd, h, 60, pos) || h[58] != '`' || h[59] != '\n') {
      *error = _("The package is damaged: an archive header is not valid.");
      return false;
    }
    std::string name = trim(std::string(h, 16));
    if (!name.empty() && name.back() == '/') name.pop_back();  // GNU ar
    std::string len = trim(std::string(h + 48, 10));
    if (len.empty() || len.size() > 10 || !std::all_of(len.begin(), len.end(), is_digit)) {
      *error = _("The package is damaged: an archive member has no valid size.");
      return false;
    }
    Member m;
    m.name = name;
    m.offset = pos + 60;
    m.size = strtoull(len.c_str(), nullptr, 10);
    if (m.size > size - m.offset) {
      *error = _("The package is damaged or incomplete: it is shorter than its contents. Was the download finished?");
      return false;
    }
    out->push_back(m);
    pos = m.offset + m.size + (m.size & 1);
    if (out->size() > 64) {
      *error = _("This file is not a Debian package (too many archive members).");
      return false;
    }
  }
  return true;
}

// libarchive reading a member of the ar archive straight from the file.
struct Slice {
  int fd = -1;
  uint64_t start = 0, size = 0, pos = 0;
  std::vector<char> buf = std::vector<char>(1 << 16);
};

la_ssize_t slice_read(struct archive *a, void *data, const void **out) {
  auto *s = static_cast<Slice *>(data);
  if (s->pos >= s->size) return 0;
  size_t want = size_t(std::min<uint64_t>(s->buf.size(), s->size - s->pos));
  for (;;) {
    ssize_t n = pread(s->fd, s->buf.data(), want, off_t(s->start + s->pos));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      archive_set_error(a, errno, "read error");
      return -1;
    }
    s->pos += uint64_t(n);
    *out = s->buf.data();
    return n;
  }
}

la_int64_t slice_skip(struct archive *, void *data, la_int64_t request) {
  auto *s = static_cast<Slice *>(data);
  if (request <= 0) return 0;
  uint64_t left = s->size - s->pos;
  uint64_t n = std::min<uint64_t>(uint64_t(request), left);
  s->pos += n;
  return la_int64_t(n);
}

// A tar member opened with the filters dpkg allows.
struct Tar {
  struct archive *a = nullptr;
  Slice slice;
  ~Tar() {
    if (a) archive_read_free(a);
  }
  bool open(int fd, const Member &m) {
    a = archive_read_new();
    if (!a) return false;
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_lzma(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_zstd(a);
    archive_read_support_format_tar(a);
    slice.fd = fd;
    slice.start = m.offset;
    slice.size = m.size;
    archive_read_set_callback_data(a, &slice);
    archive_read_set_read_callback(a, slice_read);
    archive_read_set_skip_callback(a, slice_skip);
    return archive_read_open1(a) == ARCHIVE_OK;
  }
  // Reads the data of the current entry, at most |limit| bytes; false when
  // it is longer or damaged.
  bool data(std::string *out, uint64_t limit) {
    out->clear();
    char buf[16384];
    for (;;) {
      la_ssize_t n = archive_read_data(a, buf, sizeof buf);
      if (n < 0) return false;
      if (n == 0) return true;
      if (out->size() + size_t(n) > limit) return false;
      out->append(buf, size_t(n));
    }
  }
};

// "./usr/bin/x" -> "/usr/bin/x"; "" for the root.
std::string clean_path(const char *raw) {
  std::string p = raw ? raw : "";
  while (starts_with(p, "./")) p.erase(0, 2);
  while (!p.empty() && p[0] == '/') p.erase(0, 1);
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (p.empty() || p == ".") return "";
  return "/" + p;
}

bool member_is(const std::string &name, const char *base) {
  if (name == base) return true;
  std::string b = base;
  for (const char *ext : {".gz", ".xz", ".zst", ".bz2", ".lzma"})
    if (name == b + ext) return true;
  return false;
}

// ----------------------------------------------------- the application

// Plausible application icons inside data.tar, kept while it is read.
bool icon_candidate(const std::string &path, uint64_t size) {
  if (size == 0 || size > (1u << 20)) return false;
  std::string l = lower(path);
  bool png = ends_with(l, ".png"), svg = ends_with(l, ".svg") || ends_with(l, ".svgz");
  if (!png && !svg) return false;
  if (starts_with(l, "/usr/share/pixmaps/")) return l.find('/', 19) == std::string::npos;
  if (starts_with(l, "/usr/share/icons/hicolor/")) {
    // .../hicolor/<size>/apps/<name>, sizes from 32 px (and scalable).
    std::vector<std::string> parts = split(l.substr(25), '/');
    if (parts.size() != 3 || parts[1] != "apps") return false;
    if (parts[0] == "scalable") return true;
    int px = atoi(parts[0].c_str());
    return px >= 32 && px <= 1024;
  }
  std::string base = basename_of(l);
  return (starts_with(l, "/opt/") || starts_with(l, "/usr/share/") || starts_with(l, "/usr/lib/")) &&
         (base.find("icon") != std::string::npos || base.find("logo") != std::string::npos);
}

// The [Desktop Entry] keys of a desktop file (unlocalized ones).
std::map<std::string, std::string> desktop_keys(const std::string &text) {
  std::map<std::string, std::string> out;
  bool in = false;
  for (const auto &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      in = line == "[Desktop Entry]";
      continue;
    }
    if (!in) continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    if (key.find('[') != std::string::npos || out.count(key)) continue;
    out[key] = trim(line.substr(eq + 1));
  }
  return out;
}

// How well icon file |path| shows icon |name|: higher is better, 0 none.
int icon_score(const std::string &path, const std::string &name) {
  std::string base = basename_of(path);
  size_t dot = base.find('.');
  std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
  if (stem != name) return 0;
  if (starts_with(path, "/usr/share/icons/hicolor/")) {
    std::string dir = split(path.substr(25), '/')[0];
    if (dir == "scalable") return 2000;
    int px = atoi(dir.c_str());
    return px > 256 ? 1000 + (1024 - px) : 1000 + px;  // 256 px best among PNGs
  }
  if (starts_with(path, "/usr/share/pixmaps/")) return ends_with(path, ".svg") ? 900 : 800;
  return 100;
}

// ------------------------------------------------------ analysis helpers

bool contains_word(const std::string &text, const std::string &word) {
  for (size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1)) {
    bool before = pos == 0 || !(isalnum((unsigned char)text[pos - 1]) || text[pos - 1] == '-' ||
                                text[pos - 1] == '_' || text[pos - 1] == '.');
    size_t end = pos + word.size();
    bool after = end >= text.size() || !(isalnum((unsigned char)text[end]) || text[end] == '-' ||
                                         text[end] == '_' || text[end] == '.');
    if (before && after) return true;
  }
  return false;
}

// update-alternatives --install LINK NAME PATH PRIORITY: the NAMEs.
std::vector<std::string> alternative_names(const std::string &text) {
  std::vector<std::string> out;
  const std::string key = "update-alternatives";
  for (size_t pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + 1)) {
    size_t eol = text.find('\n', pos);
    std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    std::vector<std::string> words;
    for (auto &w : split(line, ' '))
      if (!trim(w).empty()) words.push_back(trim(w));
    for (size_t i = 0; i + 2 < words.size(); ++i)
      if (words[i] == "--install") {
        std::string name = words[i + 2];
        name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return c == '"' || c == '\''; }),
                   name.end());
        if (!name.empty() && name[0] != '$' && std::find(out.begin(), out.end(), name) == out.end())
          out.push_back(name);
      }
  }
  return out;
}

void add_change(std::vector<Change> &out, Change::Kind kind, const std::string &detail) {
  for (auto &c : out)
    if (c.kind == kind) {
      if (!detail.empty() && std::find(c.details.begin(), c.details.end(), detail) == c.details.end())
        c.details.push_back(detail);
      return;
    }
  Change c;
  c.kind = kind;
  if (!detail.empty()) c.details.push_back(detail);
  out.push_back(c);
}

bool under(const std::string &path, std::initializer_list<const char *> dirs) {
  for (const char *d : dirs)
    if (starts_with(path, d) && path.size() > strlen(d)) return true;
  return false;
}

}  // namespace

// ================================================================ deb822

std::string Paragraph::get(const std::string &name) const {
  for (const auto &f : fields)
    if (same_name(f.first, name)) return f.second;
  return "";
}

bool Paragraph::has(const std::string &name) const {
  for (const auto &f : fields)
    if (same_name(f.first, name)) return true;
  return false;
}

std::vector<Paragraph> parse_paragraphs(const std::string &text) {
  std::vector<Paragraph> out;
  Paragraph cur;
  bool in_field = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t eol = text.find('\n', pos);
    std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? text.size() + 1 : eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line).empty()) {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur = Paragraph();
      in_field = false;
      continue;
    }
    if (line[0] == '#') continue;
    if (line[0] == ' ' || line[0] == '\t') {
      if (in_field && !cur.fields.empty()) {
        std::string &v = cur.fields.back().second;
        v += "\n" + line.substr(1);
      }
      continue;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0 || line.find(' ') < colon) {
      in_field = false;
      continue;
    }
    cur.fields.emplace_back(line.substr(0, colon), trim(line.substr(colon + 1)));
    in_field = true;
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

std::string description_text(const std::string &field, std::string *synopsis) {
  size_t nl = field.find('\n');
  if (synopsis) *synopsis = trim(field.substr(0, nl));
  if (nl == std::string::npos) return "";
  // What was appended last: nothing (or a paragraph break), wrapped text,
  // or a line kept as it is.
  enum { start, text, verbatim } last = start;
  std::string out;
  for (auto &line : split(field.substr(nl + 1), '\n', true)) {
    if (trim(line).empty()) continue;
    if (trim(line) == ".") {
      while (!out.empty() && out.back() == '\n') out.pop_back();
      if (!out.empty()) out += "\n\n";
      last = start;
    } else if (line[0] == ' ' || line[0] == '\t') {
      // Indented further: lists and examples keep their lines.
      if (last == text) out += "\n";
      out += line + "\n";
      last = verbatim;
    } else {
      if (last == text) out += " ";
      out += trim(line);
      last = text;
    }
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

// ===================================================== names and versions

bool valid_package_name(const std::string &name) {
  if (name.size() < 2 || name.size() > 255) return false;
  if (!(is_digit(name[0]) || (name[0] >= 'a' && name[0] <= 'z'))) return false;
  for (char c : name)
    if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '+' || c == '-' || c == '.')) return false;
  return true;
}

bool valid_version(const std::string &v) {
  if (v.empty() || v.size() > 256) return false;
  std::string rest = v;
  size_t colon = rest.find(':');
  if (colon != std::string::npos) {
    if (colon == 0 || colon > 9) return false;
    for (size_t i = 0; i < colon; ++i)
      if (!is_digit(rest[i])) return false;
    rest = rest.substr(colon + 1);
  }
  size_t dash = rest.rfind('-');
  std::string upstream = dash == std::string::npos ? rest : rest.substr(0, dash);
  std::string revision = dash == std::string::npos ? "" : rest.substr(dash + 1);
  if (upstream.empty() || !is_digit(upstream[0])) return false;
  if (dash != std::string::npos && revision.empty()) return false;
  for (char c : upstream)
    if (!(is_digit(c) || is_alpha(c) || c == '.' || c == '+' || c == '~' || c == '-')) return false;
  for (char c : revision)
    if (!(is_digit(c) || is_alpha(c) || c == '.' || c == '+' || c == '~')) return false;
  return true;
}

bool valid_architecture(const std::string &arch) {
  if (arch.empty() || arch.size() > 32) return false;
  if (!(arch[0] >= 'a' && arch[0] <= 'z')) return false;
  for (char c : arch)
    if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '-')) return false;
  return true;
}

int compare_versions(const std::string &a, const std::string &b) {
  Version x = split_version(a), y = split_version(b);
  if (x.epoch != y.epoch) return x.epoch < y.epoch ? -1 : 1;
  int r = verrevcmp(x.upstream.c_str(), y.upstream.c_str());
  if (r) return r < 0 ? -1 : 1;
  r = verrevcmp(x.revision.c_str(), y.revision.c_str());
  return r < 0 ? -1 : r > 0 ? 1 : 0;
}

// ============================================================== relations

std::string Relation::text() const {
  std::string out = name;
  if (!arch_qualifier.empty()) out += ":" + arch_qualifier;
  if (!op.empty()) out += " (" + op + " " + version + ")";
  return out;
}

std::string alternatives_text(const Alternatives &alts) {
  std::string out;
  for (const auto &r : alts) out += (out.empty() ? "" : " | ") + r.text();
  return out;
}

bool parse_relations(const std::string &field, std::vector<Alternatives> *out, std::string *error) {
  out->clear();
  bool ok = true;
  auto fail = [&](const std::string &what) {
    if (ok && error) *error = what;
    ok = false;
  };
  std::string text = field;
  std::replace(text.begin(), text.end(), '\n', ' ');
  for (const auto &group : split(text, ',')) {
    if (trim(group).empty()) continue;
    Alternatives alts;
    for (const auto &raw : split(group, '|')) {
      // Drop [arch] restrictions and <profile> lists (outside the
      // parentheses, where "<" is an operator).
      std::string s;
      int depth = 0;
      for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '(') ++depth;
        if (ch == ')') --depth;
        if (depth == 0 && (ch == '[' || ch == '<')) {
          size_t close = raw.find(ch == '[' ? ']' : '>', i);
          if (close == std::string::npos) {
            fail(format(_("unclosed \"%c\" in \"%s\""), ch, trim(raw).c_str()));
            break;
          }
          i = close;
          continue;
        }
        s += ch;
      }
      s = trim(s);
      if (s.empty()) {
        fail(format(_("an empty alternative in \"%s\""), trim(group).c_str()));
        continue;
      }
      Relation r;
      size_t paren = s.find('(');
      std::string name = trim(s.substr(0, paren));
      size_t colon = name.find(':');
      if (colon != std::string::npos) {
        r.arch_qualifier = name.substr(colon + 1);
        name = name.substr(0, colon);
      }
      r.name = name;
      if (!valid_package_name(r.name)) fail(format(_("\"%s\" is not a package name"), name.c_str()));
      if (paren != std::string::npos) {
        size_t close = s.find(')', paren);
        if (close == std::string::npos) {
          fail(format(_("unclosed \"(\" in \"%s\""), s.c_str()));
          close = s.size();
        }
        if (close < s.size() && !trim(s.substr(close + 1)).empty())
          fail(format(_("unexpected text after \"%s\""), s.substr(0, close + 1).c_str()));
        std::string c = trim(s.substr(paren + 1, close - paren - 1));
        size_t i = 0;
        while (i < c.size() && (c[i] == '<' || c[i] == '>' || c[i] == '=')) ++i;
        r.op = c.substr(0, i);
        r.version = trim(c.substr(i));
        if (r.op == "<") r.op = "<=";  // obsolete spellings dpkg still accepts
        if (r.op == ">") r.op = ">=";
        if (r.op != "<<" && r.op != "<=" && r.op != "=" && r.op != ">=" && r.op != ">>")
          fail(format(_("\"%s\" is not a version relation"), c.c_str()));
        if (!valid_version(r.version)) fail(format(_("\"%s\" is not a version"), r.version.c_str()));
      }
      alts.push_back(r);
    }
    if (!alts.empty()) out->push_back(alts);
  }
  return ok;
}

bool version_satisfies(const std::string &version, const std::string &op, const std::string &want) {
  if (op.empty()) return true;
  int c = compare_versions(version, want);
  if (op == "<<") return c < 0;
  if (op == "<=") return c <= 0;
  if (op == "=") return c == 0;
  if (op == ">=") return c >= 0;
  if (op == ">>") return c > 0;
  return false;
}

// ============================================================== the file

void choose_application(Package *p, const std::vector<std::pair<std::string, std::string>> &launchers,
                        const std::vector<std::pair<std::string, std::string>> &icons) {
  std::map<std::string, std::string> chosen;
  for (int pass = 0; pass < 2 && chosen.empty(); ++pass)
    for (const auto &l : launchers) {
      auto keys = desktop_keys(l.second);
      if (keys.empty() || (keys.count("Type") && keys["Type"] != "Application")) continue;
      if (pass == 0 && (keys["NoDisplay"] == "true" || keys["Hidden"] == "true")) continue;
      chosen = keys;
      p->app_launcher = l.first;
      break;
    }
  p->app_name = chosen["Name"];
  p->app_comment = chosen["Comment"];
  p->app_icon = chosen["Icon"];
  // The icon file: Icon= as a path, or the best size of its name.
  std::string best;
  int best_score = 0;
  std::string icon = p->app_icon;
  if (!icon.empty() && icon[0] != '/') {
    for (const char *ext : {".png", ".svg", ".xpm"})
      if (ends_with(icon, ext)) icon.erase(icon.size() - strlen(ext));
  }
  for (const auto &i : icons) {
    int score = icon.empty() ? 0 : icon[0] == '/' ? (i.first == icon ? 5000 : 0) : icon_score(i.first, icon);
    if (score > best_score) {
      best_score = score;
      best = i.first;
    }
  }
  if (best.empty()) {
    // No icon of that name (Chrome installs its own with xdg-icon-resource
    // from /opt/google/chrome/product_logo_*.png): a logo or icon in /opt,
    // the 256 px one first, else the largest file.
    uint64_t best_size = 0;
    for (const auto &i : icons) {
      if (!starts_with(i.first, "/opt/")) continue;
      std::string base = lower(basename_of(i.first));
      uint64_t s = i.second.size() + (base.find("256") != std::string::npos ? (1ull << 40) : 0) +
                   (base.find("128") != std::string::npos ? (1ull << 39) : 0);
      if (s > best_size) {
        best_size = s;
        best = i.first;
      }
    }
  }
  for (const auto &i : icons)
    if (i.first == best) {
      p->icon_path = i.first;
      p->icon_data = i.second;
      break;
    }
}

bool read_package(const std::string &path, Package *out, std::string *error, const ReadOptions &opt) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) {
    *error = errno == ENOENT ? format(_("The file %s does not exist."), path.c_str())
             : errno == EACCES ? format(_("You may not read the file %s."), path.c_str())
                               : format(_("The file %s cannot be opened: %s"), path.c_str(), strerror(errno));
    return false;
  }
  bool ok = read_package_fd(fd, path, out, error, opt);
  close(fd);
  return ok;
}

bool read_package_fd(int fd, const std::string &path, Package *out, std::string *error, const ReadOptions &opt) {
  *out = Package();
  out->path = path;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    *error = format(_("%s is not a file."), path.c_str());
    return false;
  }
  out->file_size = uint64_t(st.st_size);
  std::vector<Member> members;
  if (!ar_members(fd, out->file_size, &members, error)) return false;
  if (members.empty() || members[0].name != "debian-binary") {
    *error = _("This file is not a Debian package: it is an ar archive without \"debian-binary\".");
    return false;
  }
  {
    std::string v(size_t(std::min<uint64_t>(members[0].size, 64)), '\0');
    if (!pread_all(fd, &v[0], v.size(), members[0].offset)) {
      *error = _("The package cannot be read.");
      return false;
    }
    out->format = trim(v);
    if (!starts_with(out->format, "2.")) {
      *error = format(_("This package has the format \"%s\", which dpkg cannot install (2.0 is expected)."),
                      out->format.c_str());
      return false;
    }
  }
  const Member *control = nullptr, *data = nullptr;
  for (size_t i = 1; i < members.size(); ++i) {
    const Member &m = members[i];
    if (!control && member_is(m.name, "control.tar")) {
      control = &m;
      out->control_member = m.name;
    } else if (control && !data && member_is(m.name, "data.tar")) {
      data = &m;
      out->data_member = m.name;
    } else if (m.name.empty() || m.name[0] != '_') {
      *error = format(_("The package has an unexpected member \"%s\"."), m.name.c_str());
      return false;
    }
  }
  if (!control || !data) {
    *error = _("This package has no control or data archive: it is damaged or not a Debian package.");
    return false;
  }
  if (control->size > kMaxControlTar) {
    *error = _("The control archive of this package is too large.");
    return false;
  }

  // ------------------------------------------------- control.tar
  {
    Tar t;
    if (!t.open(fd, *control)) {
      *error = _("The control archive of this package cannot be read: the package is damaged.");
      return false;
    }
    struct archive_entry *e;
    std::string control_text;
    bool have_control = false;
    int r;
    while ((r = archive_read_next_header(t.a, &e)) == ARCHIVE_OK) {
      std::string name = clean_path(archive_entry_pathname(e));
      if (name.empty() || archive_entry_filetype(e) != AE_IFREG) continue;
      name.erase(0, 1);
      std::string text;
      if (name == "control") {
        if (!t.data(&text, 1 << 20)) break;
        control_text = text;
        have_control = true;
      } else if (name == "preinst" || name == "postinst" || name == "prerm" || name == "postrm" ||
                 name == "config") {
        Script s;
        s.name = name;
        std::string full;
        bool whole = t.data(&full, kMaxScript);
        if (!whole) full.resize(std::min(full.size(), kMaxScript));
        s.binary = full.compare(0, 4, "\x7f" "ELF") == 0 || full.find('\0') != std::string::npos;
        s.text = s.binary ? std::string() : full;
        out->scripts.push_back(s);
      } else if (name == "conffiles") {
        if (t.data(&text, 4 << 20))
          for (auto &line : split(text, '\n')) {
            std::string f = trim(line);
            if (starts_with(f, "remove-on-upgrade ")) continue;
            if (!f.empty()) out->conffiles.push_back(f);
          }
      } else if (name == "triggers") {
        if (t.data(&text, 1 << 20)) out->triggers = text;
      } else if (name == "templates") {
        out->has_templates = true;
      } else if (name == "md5sums") {
        out->has_md5sums = true;
      }
    }
    if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
      *error = _("The control archive of this package is damaged.");
      return false;
    }
    if (!have_control) {
      *error = _("This package has no control file: it is damaged or not a Debian package.");
      return false;
    }
    std::vector<Paragraph> ps = parse_paragraphs(control_text);
    if (ps.empty()) {
      *error = _("The control file of this package is empty.");
      return false;
    }
    out->control = ps[0];
  }
  const Paragraph &c = out->control;
  out->name = c.get("Package");
  out->version = c.get("Version");
  out->architecture = c.get("Architecture");
  out->maintainer = c.get("Maintainer");
  out->homepage = c.get("Homepage");
  out->section = c.get("Section");
  out->priority = c.get("Priority");
  out->source = c.get("Source");
  out->multi_arch = c.get("Multi-Arch");
  out->essential = lower(c.get("Essential")) == "yes";
  out->description = description_text(c.get("Description"), &out->summary);
  out->depends = c.get("Depends");
  out->pre_depends = c.get("Pre-Depends");
  out->recommends = c.get("Recommends");
  out->suggests = c.get("Suggests");
  out->conflicts = c.get("Conflicts");
  out->breaks = c.get("Breaks");
  out->replaces = c.get("Replaces");
  out->provides = c.get("Provides");
  std::string size = c.get("Installed-Size");
  if (!size.empty() && size.size() < 15 && std::all_of(size.begin(), size.end(), is_digit))
    out->installed_size = atoll(size.c_str());
  if (!valid_package_name(out->name)) {
    *error = out->name.empty() ? std::string(_("The control file of this package names no package."))
                               : format(_("\"%s\" is not a valid package name."), out->name.c_str());
    return false;
  }
  if (!valid_version(out->version)) {
    *error = format(_("\"%s\" is not a valid version of a package."), out->version.c_str());
    return false;
  }
  if (!valid_architecture(out->architecture)) {
    *error = format(_("\"%s\" is not a valid architecture."), out->architecture.c_str());
    return false;
  }

  // --------------------------------------------------- data.tar
  if (opt.data) {
    Tar t;
    if (!t.open(fd, *data)) {
      *error = _("The data archive of this package cannot be read: the package is damaged.");
      return false;
    }
    std::vector<std::pair<std::string, std::string>> launchers, icons;
    uint64_t icon_bytes = 0;
    struct archive_entry *e;
    int r;
    while ((r = archive_read_next_header(t.a, &e)) == ARCHIVE_OK) {
      std::string p = clean_path(archive_entry_pathname(e));
      if (p.empty()) continue;
      Entry en;
      en.path = p;
      en.mode = unsigned(archive_entry_perm(e)) & 07777;
      en.size = uint64_t(std::max<la_int64_t>(0, archive_entry_size(e)));
      if (archive_entry_hardlink(e)) {
        en.type = 'h';
        en.target = clean_path(archive_entry_hardlink(e));
        en.size = 0;
      } else {
        switch (archive_entry_filetype(e)) {
          case AE_IFREG: en.type = 'f'; break;
          case AE_IFDIR: en.type = 'd'; break;
          case AE_IFLNK:
            en.type = 'l';
            en.target = archive_entry_symlink(e) ? archive_entry_symlink(e) : "";
            break;
          case AE_IFCHR: en.type = 'c'; break;
          case AE_IFBLK: en.type = 'b'; break;
          default: en.type = 'p'; break;
        }
      }
      ++out->file_count;
      if (en.type == 'f') {
        out->data_bytes += en.size;
        bool launcher = starts_with(p, "/usr/share/applications/") && ends_with(p, ".desktop") &&
                        p.find('/', 24) == std::string::npos;
        if (launcher) out->launchers.push_back(p);
        std::string bytes;
        if (launcher && en.size <= (256u << 10) && launchers.size() < 64) {
          if (t.data(&bytes, 256u << 10)) launchers.emplace_back(p, bytes);
        } else if (icon_candidate(p, en.size) && icons.size() < 400 && icon_bytes + en.size <= (24u << 20)) {
          if (t.data(&bytes, 1u << 20)) {
            icon_bytes += bytes.size();
            icons.emplace_back(p, bytes);
          }
        }
      }
      if (out->files.size() < opt.max_files) out->files.push_back(std::move(en));
    }
    if (r != ARCHIVE_EOF) {
      *error = _("The data archive of this package is damaged or incomplete.");
      return false;
    }
    choose_application(out, launchers, icons);
  }

  if (opt.hash) {
    Sha256 h;
    char buf[1 << 16];
    uint64_t off = 0;
    while (off < out->file_size) {
      size_t n = size_t(std::min<uint64_t>(sizeof buf, out->file_size - off));
      if (!pread_all(fd, buf, n, off)) {
        *error = _("The package cannot be read.");
        return false;
      }
      h.update(buf, n);
      off += n;
    }
    out->sha256 = h.hex_digest();
  }
  return true;
}

// ======================================================= beyond the files

std::vector<std::string> source_urls(const std::string &text) {
  std::vector<std::string> out;
  auto add = [&](std::string url) {
    while (!url.empty() && (url.back() == '"' || url.back() == '\'' || url.back() == '\\' || url.back() == ';' ||
                            url.back() == ')'))
      url.pop_back();
    if ((starts_with(url, "https://") || starts_with(url, "http://")) && url.size() > 10 && url.size() < 300 &&
        std::find(out.begin(), out.end(), url) == out.end())
      out.push_back(url);
  };
  auto token_at = [&](size_t i) {
    size_t end = i;
    while (end < text.size() && !isspace((unsigned char)text[end]) && text[end] != '"' && text[end] != '\'' &&
           text[end] != '>' && text[end] != '|')
      ++end;
    return text.substr(i, end - i);
  };
  for (size_t pos = text.find("deb "); pos != std::string::npos; pos = text.find("deb ", pos + 1)) {
    if (pos > 0 && (isalnum((unsigned char)text[pos - 1]) || text[pos - 1] == '-' || text[pos - 1] == '.'))
      continue;  // "lib.deb ", "x-deb "
    size_t i = pos + 4;
    while (i < text.size() && text[i] == ' ') ++i;
    if (i < text.size() && text[i] == '[') {
      size_t close = text.find(']', i);
      if (close == std::string::npos || close - i > 400) continue;
      i = close + 1;
      while (i < text.size() && text[i] == ' ') ++i;
    }
    add(token_at(i));
  }
  for (size_t pos = text.find("URIs:"); pos != std::string::npos; pos = text.find("URIs:", pos + 1)) {
    size_t i = pos + 5;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    add(token_at(i));
  }
  return out;
}

std::vector<Change> system_changes(const Package &p) {
  std::vector<Change> out;
  // Files.
  for (const Entry &e : p.files) {
    const std::string &f = e.path;
    if (e.type == 'd') continue;
    if (under(f, {"/etc/apt/sources.list.d/"}) || f == "/etc/apt/sources.list") add_change(out, Change::apt_source, f);
    else if (under(f, {"/etc/apt/trusted.gpg.d/", "/etc/apt/keyrings/", "/usr/share/keyrings/"}) ||
             f == "/etc/apt/trusted.gpg")
      add_change(out, Change::apt_key, f);
    else if (under(f, {"/etc/cron.d/", "/etc/cron.hourly/", "/etc/cron.daily/", "/etc/cron.weekly/",
                       "/etc/cron.monthly/", "/var/spool/cron/"}))
      add_change(out, Change::cron, f);
    else if ((under(f, {"/lib/systemd/system/", "/usr/lib/systemd/system/", "/etc/systemd/system/",
                        "/usr/lib/systemd/user/", "/etc/systemd/user/"}) &&
              (ends_with(f, ".service") || ends_with(f, ".timer") || ends_with(f, ".socket") ||
               ends_with(f, ".path"))) ||
             under(f, {"/etc/init.d/"}))
      add_change(out, Change::service, f);
    else if (under(f, {"/etc/xdg/autostart/"}))
      add_change(out, Change::autostart, f);
    else if (under(f, {"/etc/sudoers.d/"}) || f == "/etc/sudoers")
      add_change(out, Change::sudoers, f);
    else if (under(f, {"/etc/polkit-1/rules.d/", "/usr/share/polkit-1/rules.d/", "/etc/polkit-1/localauthority/",
                       "/var/lib/polkit-1/localauthority/"}))
      add_change(out, Change::polkit, f);
    else if (under(f, {"/lib/udev/rules.d/", "/usr/lib/udev/rules.d/", "/etc/udev/rules.d/"}))
      add_change(out, Change::udev, f);
    else if (under(f, {"/lib/modules/", "/usr/lib/modules/", "/etc/modprobe.d/", "/etc/modules-load.d/",
                       "/usr/lib/modprobe.d/", "/usr/lib/modules-load.d/"}) ||
             (under(f, {"/usr/src/"}) && ends_with(f, "/dkms.conf")))
      add_change(out, Change::kernel, f);
    else if (under(f, {"/etc/pam.d/", "/usr/lib/pam.d/", "/etc/security/"}) ||
             (f.find("/security/pam_") != std::string::npos && ends_with(f, ".so")))
      add_change(out, Change::pam, f);
    else if (under(f, {"/etc/profile.d/", "/etc/environment.d/", "/etc/ld.so.conf.d/", "/usr/lib/environment.d/"}) ||
             f == "/etc/environment" || f == "/etc/ld.so.preload")
      add_change(out, Change::environment, f);
    if (e.type == 'f' && (e.mode & 06000))
      add_change(out, Change::setuid, f + ((e.mode & 04000) ? "" : " (set-group-ID)"));
  }
  // Maintainer scripts.
  std::string all;
  for (const Script &s : p.scripts) {
    add_change(out, Change::scripts, s.name);
    all += s.text + "\n";
  }
  if (!all.empty()) {
    std::vector<std::string> urls = source_urls(all);
    bool writes_sources = all.find("sources.list") != std::string::npos || !urls.empty();
    if (writes_sources) {
      add_change(out, Change::apt_source, "");
      for (auto &u : urls) add_change(out, Change::apt_source, u);
    }
    if (all.find("apt-key") != std::string::npos || all.find("trusted.gpg") != std::string::npos ||
        all.find("/etc/apt/keyrings") != std::string::npos || all.find("/usr/share/keyrings") != std::string::npos)
      add_change(out, Change::apt_key, "");
    for (const Script &s : p.scripts)
      if (contains_word(s.text, "curl") || contains_word(s.text, "wget"))
        add_change(out, Change::downloads, s.name);
    if (contains_word(all, "adduser") || contains_word(all, "useradd") || contains_word(all, "addgroup") ||
        contains_word(all, "groupadd") || contains_word(all, "systemd-sysusers"))
      add_change(out, Change::users, "");
    if (all.find("systemctl enable") != std::string::npos ||
        all.find("deb-systemd-helper enable") != std::string::npos || contains_word(all, "update-rc.d"))
      add_change(out, Change::service, "");
    if (all.find("/etc/cron") != std::string::npos) add_change(out, Change::cron, "");
    for (auto &name : alternative_names(all)) add_change(out, Change::alternatives, name);
    if (contains_word(all, "dkms")) add_change(out, Change::kernel, "");
    if (all.find("chmod 4") != std::string::npos || all.find("chmod u+s") != std::string::npos ||
        all.find("chmod +s") != std::string::npos || contains_word(all, "setcap"))
      add_change(out, Change::setuid, "");
    if (all.find("db_input") != std::string::npos) add_change(out, Change::debconf, "");
  }
  for (const Script &s : p.scripts)
    if (s.binary) add_change(out, Change::scripts, s.name);
  if (p.has_templates) add_change(out, Change::debconf, "");
  for (auto &f : p.conffiles) add_change(out, Change::conffiles, f);
  if (p.essential) add_change(out, Change::essential, "");
  // The most important first.
  std::stable_sort(out.begin(), out.end(), [](const Change &a, const Change &b) { return a.kind < b.kind; });
  return out;
}

}  // namespace debrun
}  // namespace sde
