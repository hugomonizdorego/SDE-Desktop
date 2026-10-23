// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "textops.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <set>
#include <sstream>

#include "codec.h"
#include "syntax.h"

namespace niny {

namespace {

std::vector<std::string> split_lines(const std::string &block, bool &trailing_nl) {
  std::vector<std::string> lines;
  size_t i = 0;
  trailing_nl = !block.empty() && block.back() == '\n';
  while (i < block.size()) {
    size_t e = block.find('\n', i);
    if (e == std::string::npos) e = block.size();
    lines.push_back(block.substr(i, e - i));
    i = e + 1;
  }
  if (block.empty()) lines.push_back("");
  return lines;
}

std::string join_lines(const std::vector<std::string> &lines, bool trailing_nl) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size() || trailing_nl) out += '\n';
  }
  return out;
}

bool blank(const std::string &s) { return s.find_first_not_of(" \t\r") == std::string::npos; }

template <typename F>
std::string map_chars(const std::string &text, F f) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    unsigned cp = next_utf8(text.data(), text.size(), i);
    append_utf8(out, f(cp));
  }
  return out;
}

unsigned to_upper(unsigned cp) {
  if (cp < 0x80) return unsigned(toupper(int(cp)));
  return unsigned(towupper(wint_t(cp)));
}
unsigned to_lower(unsigned cp) {
  if (cp < 0x80) return unsigned(tolower(int(cp)));
  return unsigned(towlower(wint_t(cp)));
}

}  // namespace

IndentStyle detect_indent(const std::string &text, size_t max_bytes) {
  IndentStyle s;
  size_t n = std::min(text.size(), max_bytes);
  int tab_lines = 0, space_lines = 0;
  int deltas[17] = {0};
  int prev = 0;
  size_t i = 0;
  bool in_block_comment = false;
  while (i < n) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos || e > n) e = n;
    const char *l = text.data() + i;
    size_t len = e - i;
    size_t k = 0;
    while (k < len && (l[k] == ' ' || l[k] == '\t')) ++k;
    if (k < len) {
      // Lines continuing a /* comment */ (" * ...") do not count.
      bool star = l[k] == '*' && k > 0 && l[k - 1] == ' ';
      if (len - k >= 2 && l[k] == '/' && l[k + 1] == '*') in_block_comment = true;
      if (!star && !in_block_comment) {
        if (l[0] == '\t') {
          ++tab_lines;
          prev = 0;
        } else if (l[0] == ' ') {
          ++space_lines;
          int cols = int(k);
          int d = cols - prev;
          if (d > 0 && d <= 16) ++deltas[d];
          prev = cols;
        } else {
          prev = 0;
        }
      }
      if (std::string(l, len).find("*/") != std::string::npos) in_block_comment = false;
    }
    i = e + 1;
  }
  if (tab_lines == 0 && space_lines == 0) return s;
  s.found = true;
  if (tab_lines > space_lines) {
    s.tabs = true;
    return s;
  }
  int best = 0;
  for (int w : {2, 4, 8, 3}) {
    if (deltas[w] > deltas[best] * (best == 4 ? 1 : 1)) best = w;
  }
  if (best == 0) best = 4;
  // Files with mostly 4-space steps sometimes contain 2-space alignment;
  // prefer 4 when both are common.
  if (best == 2 && deltas[4] * 2 >= deltas[2]) best = 4;
  s.width = best;
  return s;
}

std::string leading_whitespace(const std::string &line) {
  size_t k = 0;
  while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
  return line.substr(0, k);
}

std::string auto_indent(const std::string &line, const std::string &unit, bool colon_opens) {
  std::string ws = leading_whitespace(line);
  size_t e = line.find_last_not_of(" \t\r");
  if (e != std::string::npos) {
    char last = line[e];
    if (last == '{' || last == '[' || last == '(' || (colon_opens && last == ':')) ws += unit;
  }
  return ws;
}

std::string indent_lines(const std::string &block, const std::string &unit) {
  bool nl;
  auto lines = split_lines(block, nl);
  for (auto &l : lines)
    if (!l.empty()) l = unit + l;
  return join_lines(lines, nl);
}

std::string unindent_lines(const std::string &block, int width) {
  bool nl;
  auto lines = split_lines(block, nl);
  for (auto &l : lines) {
    if (!l.empty() && l[0] == '\t') {
      l.erase(0, 1);
      continue;
    }
    int k = 0;
    while (k < width && k < int(l.size()) && l[size_t(k)] == ' ') ++k;
    l.erase(0, size_t(k));
  }
  return join_lines(lines, nl);
}

std::string toggle_line_comment(const std::string &block, const std::string &marker) {
  if (marker.empty()) return block;
  bool nl;
  auto lines = split_lines(block, nl);
  bool all = true, any = false;
  size_t indent = std::string::npos;
  for (auto &l : lines) {
    if (blank(l)) continue;
    any = true;
    size_t k = leading_whitespace(l).size();
    indent = std::min(indent, k);
    if (l.compare(k, marker.size(), marker) != 0) all = false;
  }
  if (!any) return block;
  for (auto &l : lines) {
    if (blank(l)) continue;
    if (all) {
      size_t k = leading_whitespace(l).size();
      size_t cut = marker.size();
      if (k + cut < l.size() && l[k + cut] == ' ') ++cut;
      l.erase(k, cut);
    } else {
      l.insert(indent, marker + " ");
    }
  }
  return join_lines(lines, nl);
}

std::string toggle_block_comment(const std::string &text, const std::string &open, const std::string &close) {
  if (open.empty()) return text;
  size_t a = text.find_first_not_of(" \t\n");
  size_t b = text.find_last_not_of(" \t\n");
  if (a != std::string::npos && text.compare(a, open.size(), open) == 0 && b + 1 >= open.size() + close.size() &&
      b + 1 >= close.size() && text.compare(b + 1 - close.size(), close.size(), close) == 0 &&
      b + 1 - close.size() >= a + open.size()) {
    std::string inner = text.substr(a + open.size(), b + 1 - close.size() - a - open.size());
    if (!inner.empty() && inner.front() == ' ') inner.erase(0, 1);
    if (!inner.empty() && inner.back() == ' ') inner.pop_back();
    return text.substr(0, a) + inner + text.substr(b + 1);
  }
  return open + " " + text + " " + close;
}

std::string trim_trailing_whitespace(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i <= text.size()) {
    size_t e = text.find('\n', i);
    bool last = e == std::string::npos;
    if (last) e = text.size();
    size_t k = e;
    while (k > i && (text[k - 1] == ' ' || text[k - 1] == '\t')) --k;
    out.append(text, i, k - i);
    if (last) break;
    out += '\n';
    i = e + 1;
  }
  return out;
}

std::string sort_lines(const std::string &block, bool reverse, bool unique) {
  bool nl;
  auto lines = split_lines(block, nl);
  std::stable_sort(lines.begin(), lines.end());
  if (unique) lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
  if (reverse) std::reverse(lines.begin(), lines.end());
  return join_lines(lines, nl);
}

std::string upper_case(const std::string &text) { return map_chars(text, to_upper); }
std::string lower_case(const std::string &text) { return map_chars(text, to_lower); }
std::string title_case(const std::string &text) {
  bool start = true;
  return map_chars(text, [&](unsigned cp) {
    bool letter = cp >= 0x80 ? iswalpha(wint_t(cp)) != 0 : isalnum(int(cp)) != 0 || cp == '\'';
    unsigned r = letter ? (start ? to_upper(cp) : to_lower(cp)) : cp;
    start = !letter;
    return r;
  });
}

bool is_bracket(char c) { return c && strchr("()[]{}", c) != nullptr; }

long match_bracket(const std::string &text, const std::string &styles, long pos, long max_distance) {
  if (pos < 0 || pos >= long(text.size())) return -1;
  char c = text[size_t(pos)];
  const char *open = "([{", *close = ")]}";
  const char *o = strchr(open, c), *cl = strchr(close, c);
  if (!c || (!o && !cl)) return -1;
  bool forward = o != nullptr;
  char want = forward ? close[o - open] : open[cl - close];
  bool use_styles = styles.size() == text.size();
  bool literal = use_styles && is_literal_style(styles[size_t(pos)]);
  int depth = 0;
  long step = forward ? 1 : -1;
  for (long i = pos, n = 0; i >= 0 && i < long(text.size()) && n <= max_distance; i += step, ++n) {
    if (use_styles && is_literal_style(styles[size_t(i)]) != literal) continue;
    char d = text[size_t(i)];
    if (d == c) ++depth;
    else if (d == want && --depth == 0) return i;
  }
  return -1;
}

int display_column(const char *line, int len, int tab_width) {
  int col = 0;
  for (int i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)line[i];
    if (c == '\t') col += tab_width - col % tab_width;
    else if ((c & 0xC0) != 0x80) ++col;
  }
  return col;
}

std::string uri_to_path(const std::string &arg) {
  if (arg.compare(0, 7, "file://") != 0) {
    if (arg.find("://") != std::string::npos) return "";
    return arg;
  }
  std::string rest = arg.substr(7);
  // file://host/path: only localhost or an empty host.
  if (rest.empty() || rest[0] != '/') {
    size_t slash = rest.find('/');
    std::string host = rest.substr(0, slash);
    if (host != "localhost" || slash == std::string::npos) return "";
    rest = rest.substr(slash);
  }
  std::string out;
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && isxdigit((unsigned char)rest[i + 1]) &&
        isxdigit((unsigned char)rest[i + 2])) {
      out += char(strtol(rest.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else if (rest[i] == '?' || rest[i] == '#') {
      break;
    } else {
      out += rest[i];
    }
  }
  return out;
}

bool parse_line_arg(const std::string &arg, int &line) {
  if (arg.size() < 2 || arg[0] != '+') return false;
  for (size_t i = 1; i < arg.size(); ++i)
    if (!isdigit((unsigned char)arg[i])) return false;
  line = atoi(arg.c_str() + 1);
  return true;
}

bool split_position_suffix(const std::string &arg, std::string &path, int &line, int &column) {
  // path:LINE or path:LINE:COL
  size_t c1 = arg.rfind(':');
  if (c1 == std::string::npos || c1 == 0 || c1 + 1 >= arg.size()) return false;
  auto digits = [](const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return isdigit((unsigned char)c); });
  };
  std::string last = arg.substr(c1 + 1);
  if (!digits(last)) return false;
  size_t c0 = arg.rfind(':', c1 - 1);
  if (c0 != std::string::npos && c0 > 0 && digits(arg.substr(c0 + 1, c1 - c0 - 1))) {
    path = arg.substr(0, c0);
    line = atoi(arg.substr(c0 + 1, c1 - c0 - 1).c_str());
    column = atoi(last.c_str());
    return true;
  }
  path = arg.substr(0, c1);
  line = atoi(last.c_str());
  column = 0;
  return true;
}

int fuzzy_score(const std::string &query, const std::string &text) {
  if (query.empty()) return 0;
  size_t qi = 0;
  int score = 0, run = 0;
  bool prev_sep = true;
  for (size_t i = 0; i < text.size() && qi < query.size(); ++i) {
    char t = char(tolower((unsigned char)text[i]));
    char q = char(tolower((unsigned char)query[qi]));
    bool sep = !isalnum((unsigned char)text[i]);
    if (t == q) {
      score += 1 + run * 3 + (prev_sep ? 6 : 0) + (i == qi ? 2 : 0);
      ++run;
      ++qi;
    } else {
      run = 0;
    }
    prev_sep = sep;
  }
  if (qi < query.size()) return -1;
  // Shorter texts first among equal matches.
  return score * 100 - int(std::min<size_t>(text.size(), 99));
}


// ---------------------------------------------------------- Notepad tools

long utf8_length(const char *text, size_t len) {
  long n = 0;
  for (size_t i = 0; i < len; ++i)
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++n;
  return n;
}

TextCounts count_text(const char *text, size_t len) {
  TextCounts c;
  bool in_word = false;
  if (len) c.lines = 1;
  for (size_t i = 0; i < len; ++i) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    if ((ch & 0xC0) != 0x80) ++c.chars;
    if (ch == '\n') ++c.lines;
    bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
    if (space) {
      in_word = false;
    } else if (!in_word && (ch & 0xC0) != 0x80) {
      in_word = true;
      ++c.words;
    }
  }
  return c;
}

std::string group_thousands(long n) {
  std::string digits = std::to_string(n < 0 ? -n : n), out;
  int k = 0;
  for (size_t i = digits.size(); i-- > 0;) {
    out.insert(out.begin(), digits[i]);
    if (++k % 3 == 0 && i > 0) out.insert(out.begin(), ',');
  }
  return n < 0 ? "-" + out : out;
}

std::string character_status(long selected, long total) {
  if (selected > 0)
    return group_thousands(selected) + " of " + group_thousands(total) + (total == 1 ? " character" : " characters");
  return group_thousands(total) + (total == 1 ? " character" : " characters");
}

std::string date_time_text(long long t) {
  time_t tt = time_t(t);
  struct tm tmv;
  localtime_r(&tt, &tmv);
  char buf[64];
  strftime(buf, sizeof buf, "%H:%M %Y-%m-%d", &tmv);
  return buf;
}

const std::vector<UnicodeControl> &unicode_controls() {
  static const std::vector<UnicodeControl> k = {
      {"Left-to-right mark", "LRM", 0x200E},
      {"Right-to-left mark", "RLM", 0x200F},
      {"Arabic letter mark", "ALM", 0x061C},
      {"Zero width joiner", "ZWJ", 0x200D},
      {"Zero width non-joiner", "ZWNJ", 0x200C},
      {"Zero width space", "ZWSP", 0x200B},
      {"Word joiner", "WJ", 0x2060},
      {"No-break space", "NBSP", 0x00A0},
      {"Left-to-right embedding", "LRE", 0x202A},
      {"Right-to-left embedding", "RLE", 0x202B},
      {"Left-to-right override", "LRO", 0x202D},
      {"Right-to-left override", "RLO", 0x202E},
      {"Pop directional formatting", "PDF", 0x202C},
      {"Left-to-right isolate", "LRI", 0x2066},
      {"Right-to-left isolate", "RLI", 0x2067},
      {"First strong isolate", "FSI", 0x2068},
      {"Pop directional isolate", "PDI", 0x2069},
  };
  return k;
}

std::string utf8_encode(unsigned cp) {
  std::string o;
  if (cp < 0x80) {
    o += char(cp);
  } else if (cp < 0x800) {
    o += char(0xC0 | (cp >> 6));
    o += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    o += char(0xE0 | (cp >> 12));
    o += char(0x80 | ((cp >> 6) & 0x3F));
    o += char(0x80 | (cp & 0x3F));
  } else {
    o += char(0xF0 | (cp >> 18));
    o += char(0x80 | ((cp >> 12) & 0x3F));
    o += char(0x80 | ((cp >> 6) & 0x3F));
    o += char(0x80 | (cp & 0x3F));
  }
  return o;
}

const char *whitespace_marker(char c) {
  switch (c) {
    case ' ': return "\xC2\xB7";      // middle dot
    case '\t': return "\xE2\x86\x92"; // rightwards arrow
    case '\n': return "\xC2\xB6";     // pilcrow
    default: return nullptr;
  }
}

std::string url_query_encode(const std::string &text) {
  static const char hex[] = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : text) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += char(c);
    } else if (c == ' ') {
      o += '+';
    } else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

SpellResult parse_ispell_line(const std::string &line) {
  SpellResult r;
  if (line.empty()) {
    r.known = false;
    return r;
  }
  char c = line[0];
  if (c == '*' || c == '+' || c == '-') return r;
  if (c == '#') {
    r.correct = false;
    return r;
  }
  if (c == '&' || c == '?') {
    r.correct = false;
    size_t colon = line.find(": ");
    if (colon == std::string::npos) return r;
    std::string rest = line.substr(colon + 2);
    size_t start = 0;
    while (start < rest.size()) {
      size_t comma = rest.find(", ", start);
      std::string w = rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      while (!w.empty() && (w.back() == '\r' || w.back() == ' ')) w.pop_back();
      if (!w.empty()) r.suggestions.push_back(w);
      if (comma == std::string::npos) break;
      start = comma + 2;
    }
    return r;
  }
  r.known = false;
  return r;
}

std::vector<WordSpan> spell_words(const std::string &text) {
  std::vector<WordSpan> out;
  size_t n = text.size(), i = 0;
  auto letter = [&](size_t k) {
    unsigned char ch = static_cast<unsigned char>(text[k]);
    return isalpha(ch) || ch >= 0x80;
  };
  while (i < n) {
    if (!letter(i)) {
      ++i;
      continue;
    }
    size_t s = i;
    bool digits = false;
    while (i < n && (letter(i) || isdigit(static_cast<unsigned char>(text[i])) ||
                     (text[i] == '\'' && i + 1 < n && letter(i + 1)))) {
      if (isdigit(static_cast<unsigned char>(text[i]))) digits = true;
      ++i;
    }
    // Skip words glued to paths, addresses and identifiers.
    bool glued = (s > 0 && (text[s - 1] == '_' || text[s - 1] == '/' || text[s - 1] == '@' || text[s - 1] == '.' ||
                            text[s - 1] == '\\')) ||
                 (i < n && (text[i] == '_' || text[i] == '/' || text[i] == '@' ||
                            (text[i] == '.' && i + 1 < n && letter(i + 1))));
    if (!digits && !glued && i - s >= 2) out.push_back({s, i});
    if (glued)
      while (i < n && (text[i] == '_' || text[i] == '/' || text[i] == '@' || text[i] == '.' || letter(i) ||
                       isdigit(static_cast<unsigned char>(text[i]))))
        ++i;
  }
  return out;
}

}  // namespace niny
