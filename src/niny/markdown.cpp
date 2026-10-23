// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "markdown.h"

#include <cctype>
#include <cstdlib>

namespace niny {
namespace md {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }
bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || (static_cast<unsigned char>(c) >= 0x80); }

std::vector<std::string> split_lines(const std::string &s, bool &trailing_newline) {
  std::vector<std::string> out;
  size_t start = 0;
  trailing_newline = !s.empty() && s.back() == '\n';
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    if (nl == std::string::npos) {
      if (start < s.size() || out.empty()) out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, nl - start));
    start = nl + 1;
    if (start == s.size()) break;
  }
  return out;
}

std::string join_lines(const std::vector<std::string> &lines, bool trailing_newline) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) out += '\n';
    out += lines[i];
  }
  if (trailing_newline) out += '\n';
  return out;
}

size_t indent_end(const std::string &line) {
  size_t i = 0;
  while (i < line.size() && is_space(line[i])) ++i;
  return i;
}

// Length of the list marker of |kind| at |at| (including the space), or 0.
size_t marker_len(const std::string &line, size_t at, ListKind kind) {
  auto starts = [&](const char *m) { return line.compare(at, std::char_traits<char>::length(m), m) == 0; };
  switch (kind) {
    case ListKind::Task:
      for (const char *m : {"- [ ] ", "- [x] ", "- [X] ", "* [ ] ", "* [x] "})
        if (starts(m)) return 6;
      return 0;
    case ListKind::Bullet:
      if (marker_len(line, at, ListKind::Task)) return 0;
      if (at + 1 < line.size() && (line[at] == '-' || line[at] == '*' || line[at] == '+') && line[at + 1] == ' ')
        return 2;
      return 0;
    case ListKind::Numbered: {
      size_t i = at;
      while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])) && i - at < 9) ++i;
      if (i == at || i + 1 >= line.size() + 0 || i >= line.size()) return 0;
      if ((line[i] == '.' || line[i] == ')') && i + 1 < line.size() && line[i + 1] == ' ') return i + 2 - at;
      return 0;
    }
    case ListKind::Quote:
      if (starts("> ")) return 2;
      if (at < line.size() && line[at] == '>') return 1;
      return 0;
  }
  return 0;
}

bool any_marker(const std::string &line, size_t at, size_t &len) {
  for (ListKind k : {ListKind::Task, ListKind::Bullet, ListKind::Numbered, ListKind::Quote})
    if ((len = marker_len(line, at, k))) return true;
  return false;
}

size_t run_forward(const std::string &t, size_t pos, size_t limit, char c) {
  size_t n = 0;
  while (pos + n < limit && t[pos + n] == c) ++n;
  return n;
}
size_t run_backward(const std::string &t, size_t pos, size_t limit, char c) {
  // Counts c characters ending just before |pos|, not going below |limit|.
  size_t n = 0;
  while (pos > limit + n && t[pos - n - 1] == c) ++n;
  return n;
}

// How many marker characters to remove for |marker| given the runs on both
// sides, or 0 when the marker is not there.
size_t removable(const std::string &marker, size_t lead, size_t trail) {
  size_t n = lead < trail ? lead : trail;
  if (marker == "*") return (n == 1 || n >= 3) ? 1 : 0;
  if (marker == "**") return n >= 2 ? 2 : 0;
  return 0;
}

}  // namespace

int heading_level(const std::string &line) {
  size_t n = 0;
  while (n < line.size() && line[n] == '#' && n < 7) ++n;
  if (n == 0 || n > 6) return 0;
  if (n < line.size() && line[n] != ' ' && line[n] != '\t') return 0;
  return int(n);
}

std::string set_heading(const std::string &line, int level) {
  std::string body = line;
  int cur = heading_level(line);
  if (cur) {
    body = line.substr(size_t(cur));
    size_t i = 0;
    while (i < body.size() && is_space(body[i])) ++i;
    body = body.substr(i);
  }
  if (level <= 0) return body;
  if (level > 6) level = 6;
  return std::string(size_t(level), '#') + " " + body;
}

bool list_kind(const std::string &line, ListKind &kind) {
  size_t at = indent_end(line);
  for (ListKind k : {ListKind::Task, ListKind::Bullet, ListKind::Numbered, ListKind::Quote})
    if (marker_len(line, at, k)) {
      kind = k;
      return true;
    }
  return false;
}

std::string toggle_list(const std::string &block, ListKind kind) {
  bool nl = false;
  std::vector<std::string> lines = split_lines(block, nl);
  bool all = true, any = false;
  for (auto &l : lines) {
    if (indent_end(l) == l.size()) continue;
    any = true;
    if (!marker_len(l, indent_end(l), kind)) all = false;
  }
  if (!any) {
    // An empty line: start a list item.
    switch (kind) {
      case ListKind::Bullet: return block + "- ";
      case ListKind::Numbered: return block + "1. ";
      case ListKind::Task: return block + "- [ ] ";
      case ListKind::Quote: return block + "> ";
    }
  }
  int number = 1;
  for (auto &l : lines) {
    size_t at = indent_end(l);
    if (at == l.size()) continue;
    if (all) {
      l.erase(at, marker_len(l, at, kind));
      continue;
    }
    size_t old = 0;
    if (any_marker(l, at, old) && kind != ListKind::Quote) l.erase(at, old);
    std::string m;
    switch (kind) {
      case ListKind::Bullet: m = "- "; break;
      case ListKind::Numbered: m = std::to_string(number++) + ". "; break;
      case ListKind::Task: m = "- [ ] "; break;
      case ListKind::Quote: m = "> "; break;
    }
    l.insert(at, m);
  }
  return join_lines(lines, nl);
}

std::string continue_list(const std::string &line, bool &ends) {
  ends = false;
  size_t at = indent_end(line);
  std::string ind = line.substr(0, at);
  ListKind k;
  if (!list_kind(line, k)) return "";
  size_t ml = marker_len(line, at, k);
  ends = line.find_first_not_of(" \t", at + ml) == std::string::npos;
  switch (k) {
    case ListKind::Task: return ind + line.substr(at, 2) + "[ ] ";
    case ListKind::Bullet: return ind + line.substr(at, 2);
    case ListKind::Numbered: {
      long n = std::strtol(line.c_str() + at, nullptr, 10);
      size_t p = at;
      while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p]))) ++p;
      return ind + std::to_string(n + 1) + line[p] + " ";
    }
    case ListKind::Quote: return ind + "> ";
  }
  return "";
}

Edit toggle_wrap(const std::string &text, size_t a, size_t b, const std::string &marker) {
  Edit e;
  if (b < a) std::swap(a, b);
  if (b > text.size()) b = text.size();
  if (a > b) a = b;
  size_t m = marker.size();
  bool stars = marker == "*" || marker == "**";
  // Marker inside the selection.
  if (b - a >= 2 * m + (stars ? 0 : 0)) {
    size_t cut = 0;
    if (stars) {
      size_t lead = run_forward(text, a, b, '*'), trail = run_backward(text, b, a, '*');
      if (lead + trail < b - a) cut = removable(marker, lead, trail);
    } else if (text.compare(a, m, marker) == 0 && text.compare(b - m, m, marker) == 0 && b - a > 2 * m) {
      cut = m;
    }
    if (cut) {
      e.start = a;
      e.end = b;
      e.text = text.substr(a + cut, b - a - 2 * cut);
      e.sel_start = a;
      e.sel_end = a + e.text.size();
      return e;
    }
  }
  // Marker just outside the selection.
  {
    size_t cut = 0;
    if (stars) {
      size_t lead = run_backward(text, a, 0, '*'), trail = run_forward(text, b, text.size(), '*');
      if (b > a || lead + trail >= 2) cut = removable(marker, lead, trail);
    } else if (a >= m && b + m <= text.size() && text.compare(a - m, m, marker) == 0 &&
               text.compare(b, m, marker) == 0) {
      cut = m;
    }
    if (cut) {
      e.start = a - cut;
      e.end = b + cut;
      e.text = text.substr(a, b - a);
      e.sel_start = a - cut;
      e.sel_end = e.sel_start + e.text.size();
      return e;
    }
  }
  // Wrap, keeping surrounding spaces outside the markers.
  while (a < b && is_space(text[a])) ++a;
  while (b > a && (is_space(text[b - 1]) || text[b - 1] == '\n')) --b;
  e.start = a;
  e.end = b;
  e.text = marker + text.substr(a, b - a) + marker;
  e.sel_start = a + m;
  e.sel_end = b + m;
  return e;
}

Edit make_link(const std::string &text, size_t a, size_t b) {
  Edit e;
  if (b < a) std::swap(a, b);
  if (b > text.size()) b = text.size();
  std::string sel = text.substr(a, b - a);
  e.start = a;
  e.end = b;
  bool url = sel.compare(0, 7, "http://") == 0 || sel.compare(0, 8, "https://") == 0 || sel.compare(0, 4, "www.") == 0;
  if (sel.empty()) {
    e.text = "[](https://)";
    e.sel_start = e.sel_end = a + 1;
  } else if (url) {
    e.text = "[" + sel + "](" + sel + ")";
    e.sel_start = a + 1;
    e.sel_end = a + 1 + sel.size();
  } else {
    e.text = "[" + sel + "](https://)";
    e.sel_start = a + sel.size() + 3;
    e.sel_end = e.sel_start + 8;
  }
  return e;
}

std::string clear_formatting(const std::string &text) {
  bool nl = false;
  std::vector<std::string> lines = split_lines(text, nl);
  for (auto &l : lines) {
    std::string body = set_heading(l, 0);
    std::string out;
    for (auto &s : parse_inline(body)) out += s.text;
    l = out;
  }
  return join_lines(lines, nl);
}

// ------------------------------------------------------------------ parse

std::vector<Line> parse(const std::string &doc) {
  std::vector<Line> out;
  bool nl = false;
  std::vector<std::string> lines = split_lines(doc, nl);
  bool fence = false;
  std::string fence_mark;
  for (auto &raw : lines) {
    std::string l = raw;
    if (!l.empty() && l.back() == '\r') l.pop_back();
    size_t ind = indent_end(l);
    std::string t = l.substr(ind);
    if (t.compare(0, 3, "```") == 0 || t.compare(0, 3, "~~~") == 0) {
      if (!fence) {
        fence = true;
        fence_mark = t.substr(0, 3);
        continue;
      }
      if (t.compare(0, 3, fence_mark) == 0) {
        fence = false;
        continue;
      }
    }
    if (fence) {
      Line c;
      c.kind = Block::Code;
      c.text = l;
      out.push_back(c);
      continue;
    }
    if (t.empty()) {
      if (!out.empty() && out.back().kind != Block::Blank) out.push_back(Line{Block::Blank, 0, false, "", ""});
      continue;
    }
    // Indented code (4 spaces) after a blank line.
    if (ind >= 4 && l[0] == ' ' && (out.empty() || out.back().kind == Block::Blank || out.back().kind == Block::Code)) {
      Line c;
      c.kind = Block::Code;
      c.text = l.substr(4);
      out.push_back(c);
      continue;
    }
    Line x;
    int level_ws = 0;
    for (size_t i = 0; i < ind; ++i) level_ws += l[i] == '\t' ? 4 : 1;
    x.level = level_ws / 2;
    // Thematic break.
    {
      char c = t[0];
      if (c == '-' || c == '*' || c == '_') {
        int n = 0;
        bool only = true;
        for (char ch : t) {
          if (ch == c) ++n;
          else if (!is_space(ch)) only = false;
        }
        if (only && n >= 3) {
          out.push_back(Line{Block::Rule, 0, false, "", ""});
          continue;
        }
      }
    }
    int h = heading_level(t);
    size_t ml = 0;
    if (h) {
      x.kind = Block::Heading;
      x.level = h;
      x.text = set_heading(t, 0);
      while (!x.text.empty() && (x.text.back() == '#' || is_space(x.text.back()))) {
        if (x.text.back() == '#' && x.text.size() > 1 && !is_space(x.text[x.text.size() - 2]) &&
            x.text[x.text.size() - 2] != '#')
          break;
        x.text.pop_back();
      }
    } else if ((ml = marker_len(t, 0, ListKind::Task))) {
      x.kind = Block::Task;
      x.checked = t[3] == 'x' || t[3] == 'X';
      x.text = t.substr(ml);
    } else if ((ml = marker_len(t, 0, ListKind::Bullet))) {
      x.kind = Block::Bullet;
      x.text = t.substr(ml);
    } else if ((ml = marker_len(t, 0, ListKind::Numbered))) {
      x.kind = Block::Numbered;
      x.marker = t.substr(0, ml - 1);
      if (!x.marker.empty() && x.marker.back() == ')') x.marker.back() = '.';
      x.text = t.substr(ml);
    } else if ((ml = marker_len(t, 0, ListKind::Quote))) {
      x.kind = Block::Quote;
      x.text = t.substr(ml);
      if (!out.empty() && out.back().kind == Block::Quote) {
        out.back().text += (x.text.empty() ? "" : " " + x.text);
        continue;
      }
    } else {
      x.kind = Block::Paragraph;
      x.level = 0;
      x.text = t;
      if (!out.empty() && out.back().kind == Block::Paragraph) {
        out.back().text += " " + t;
        continue;
      }
    }
    out.push_back(x);
  }
  while (!out.empty() && out.back().kind == Block::Blank) out.pop_back();
  return out;
}

std::vector<Span> parse_inline(const std::string &text) {
  std::vector<Span> out;
  Span cur;
  auto flush = [&] {
    if (!cur.text.empty()) out.push_back(cur);
    cur.text.clear();
  };
  auto style = [&](Span s) {
    flush();
    cur.bold = s.bold;
    cur.italic = s.italic;
    cur.strike = s.strike;
  };
  bool bold = false, italic = false, strike = false;
  size_t n = text.size();
  for (size_t i = 0; i < n;) {
    char c = text[i];
    if (c == '\\' && i + 1 < n && std::ispunct(static_cast<unsigned char>(text[i + 1]))) {
      cur.text += text[i + 1];
      i += 2;
      continue;
    }
    if (c == '`') {
      size_t close = text.find('`', i + 1);
      if (close != std::string::npos) {
        flush();
        Span s;
        s.code = true;
        s.text = text.substr(i + 1, close - i - 1);
        out.push_back(s);
        i = close + 1;
        continue;
      }
    }
    if ((c == '[') || (c == '!' && i + 1 < n && text[i + 1] == '[')) {
      size_t open = c == '!' ? i + 1 : i;
      size_t close = text.find("](", open + 1);
      size_t end = close == std::string::npos ? std::string::npos : text.find(')', close + 2);
      if (close != std::string::npos && end != std::string::npos && text.find('\n', open) > end) {
        flush();
        Span s;
        s.link = true;
        s.bold = bold;
        s.italic = italic;
        s.url = text.substr(close + 2, end - close - 2);
        // Link text may hold inline formatting; keep its plain text.
        for (auto &inner : parse_inline(text.substr(open + 1, close - open - 1))) s.text += inner.text;
        if (s.text.empty()) s.text = s.url;
        out.push_back(s);
        i = end + 1;
        continue;
      }
    }
    bool two_star = (c == '*' || c == '_') && i + 1 < n && text[i + 1] == c;
    bool two_tilde = c == '~' && i + 1 < n && text[i + 1] == '~';
    if (two_star || two_tilde) {
      std::string mk = text.substr(i, 2);
      bool &flag = two_tilde ? strike : bold;
      char prev = i ? text[i - 1] : ' ', next = i + 2 < n ? text[i + 2] : ' ';
      bool word_ok = c != '_' || (!flag ? !is_word(prev) : !is_word(next));
      if (!flag && word_ok && !is_space(next) && text.find(mk, i + 2) != std::string::npos) {
        flag = true;
        Span s;
        s.bold = bold;
        s.italic = italic;
        s.strike = strike;
        style(s);
        i += 2;
        continue;
      }
      if (flag && word_ok && !is_space(prev)) {
        flag = false;
        Span s;
        s.bold = bold;
        s.italic = italic;
        s.strike = strike;
        style(s);
        i += 2;
        continue;
      }
    }
    if (c == '*' || c == '_') {
      char prev = i ? text[i - 1] : ' ', next = i + 1 < n ? text[i + 1] : ' ';
      bool word_ok = c != '_' || (!italic ? !is_word(prev) : !is_word(next));
      if (!italic && word_ok && !is_space(next) && text.find(c, i + 1) != std::string::npos) {
        italic = true;
        Span s;
        s.bold = bold;
        s.italic = italic;
        s.strike = strike;
        style(s);
        ++i;
        continue;
      }
      if (italic && word_ok && !is_space(prev)) {
        italic = false;
        Span s;
        s.bold = bold;
        s.italic = italic;
        s.strike = strike;
        style(s);
        ++i;
        continue;
      }
    }
    cur.text += c;
    ++i;
  }
  flush();
  return out;
}

}  // namespace md
}  // namespace niny
