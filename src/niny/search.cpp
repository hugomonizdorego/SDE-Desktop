// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "search.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

namespace niny {

namespace {

inline unsigned char fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

struct FoldEq {
  bool operator()(char a, char b) const { return fold((unsigned char)a) == fold((unsigned char)b); }
};
struct FoldHash {
  size_t operator()(char c) const { return fold((unsigned char)c); }
};

const char *line_start(const char *base, const char *p) {
  while (p > base && p[-1] != '\n') --p;
  return p;
}
const char *line_end(const char *p, const char *end) {
  const void *nl = memchr(p, '\n', size_t(end - p));
  return nl ? static_cast<const char *>(nl) : end;
}

}  // namespace

bool is_word_byte(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

bool Searcher::set(const std::string &pattern, const SearchOptions &options, std::string *error) {
  pattern_ = pattern;
  options_ = options;
  re_.reset();
  valid_ = !pattern.empty();
  folded_.clear();
  for (char c : pattern) folded_ += char(fold((unsigned char)c));
  if (!valid_ || !options.regex) return valid_;
  try {
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (!options.case_sensitive) flags |= std::regex::icase;
    std::string p = options.whole_word ? "\\b(?:" + pattern + ")\\b" : pattern;
    re_ = std::make_shared<std::regex>(p, flags);
  } catch (const std::regex_error &e) {
    if (error) *error = e.what();
    valid_ = false;
  }
  return valid_;
}

bool Searcher::word_ok(const std::string &text, size_t start, size_t end) const {
  if (!options_.whole_word || options_.regex) return true;
  if (start > 0 && is_word_byte((unsigned char)text[start - 1]) && is_word_byte((unsigned char)text[start]))
    return false;
  if (end < text.size() && end > 0 && is_word_byte((unsigned char)text[end]) &&
      is_word_byte((unsigned char)text[end - 1]))
    return false;
  return true;
}

size_t Searcher::plain_find(const std::string &text, size_t from) const {
  while (from <= text.size()) {
    std::string::const_iterator it;
    if (options_.case_sensitive) {
      std::boyer_moore_horspool_searcher<std::string::const_iterator> s(pattern_.begin(), pattern_.end());
      it = std::search(text.begin() + long(from), text.end(), s);
    } else {
      std::boyer_moore_horspool_searcher<std::string::const_iterator, FoldHash, FoldEq> s(folded_.begin(),
                                                                                           folded_.end());
      it = std::search(text.begin() + long(from), text.end(), s);
    }
    if (it == text.end()) return std::string::npos;
    size_t pos = size_t(it - text.begin());
    if (word_ok(text, pos, pos + pattern_.size())) return pos;
    from = pos + 1;
  }
  return std::string::npos;
}

bool Searcher::match_in_line(const char *lb, const char *le, const char *from, Match &m, const char *base) const {
  std::cmatch cm;
  auto flags = std::regex_constants::match_not_null;
  if (from > lb) flags |= std::regex_constants::match_prev_avail;
  if (std::regex_search(from, le, cm, *re_, flags)) {
    m.start = size_t(cm[0].first - base);
    m.end = size_t(cm[0].second - base);
    return true;
  }
  return false;
}

bool Searcher::last_in_line(const char *lb, const char *le, const char *limit, Match &m, const char *base) const {
  bool found = false;
  const char *p = lb;
  while (p <= le) {
    Match cur;
    if (options_.regex) {
      if (!match_in_line(lb, le, p, cur, base)) break;
    } else {
      // Plain search inside the line.
      const char *hit = nullptr;
      for (const char *q = p; q + pattern_.size() <= le; ++q) {
        bool eq = options_.case_sensitive ? memcmp(q, pattern_.data(), pattern_.size()) == 0
                                          : std::equal(pattern_.begin(), pattern_.end(), q, FoldEq());
        if (eq) {
          size_t s = size_t(q - base), e = s + pattern_.size();
          bool ok = true;
          if (options_.whole_word) {
            if (q > base && is_word_byte((unsigned char)q[-1]) && is_word_byte((unsigned char)q[0])) ok = false;
            if (ok && base + e < le && is_word_byte((unsigned char)base[e]) &&
                is_word_byte((unsigned char)base[e - 1]))
              ok = false;
          }
          if (ok) {
            hit = q;
            cur.start = s;
            cur.end = e;
            break;
          }
        }
      }
      if (!hit) break;
    }
    if (base + cur.end > limit) break;
    m = cur;
    found = true;
    p = base + (cur.end > cur.start ? cur.end : cur.start + 1);
  }
  return found;
}

bool Searcher::find(const std::string &text, size_t from, bool forward, bool wrap, Match &m) const {
  if (!valid_ || text.empty()) return false;
  if (from > text.size()) from = text.size();
  const char *base = text.data(), *end = base + text.size();
  if (forward) {
    for (int pass = 0; pass < (wrap ? 2 : 1); ++pass) {
      size_t start = pass == 0 ? from : 0;
      if (!options_.regex) {
        size_t pos = plain_find(text, start);
        if (pos != std::string::npos) {
          m.start = pos;
          m.end = pos + pattern_.size();
          return true;
        }
        continue;
      }
      const char *p = base + start;
      const char *lb = line_start(base, p);
      while (lb <= end) {
        const char *le = line_end(lb, end);
        if (match_in_line(lb, le, p > lb ? p : lb, m, base)) return true;
        if (le >= end) break;
        lb = p = le + 1;
      }
    }
    return false;
  }
  for (int pass = 0; pass < (wrap ? 2 : 1); ++pass) {
    const char *limit = pass == 0 ? base + from : end;
    const char *lb = line_start(base, limit);
    const char *le = line_end(lb, end);
    while (true) {
      if (last_in_line(lb, le, limit, m, base)) return true;
      if (lb == base) break;
      le = lb - 1;
      lb = line_start(base, le);
    }
  }
  return false;
}

std::vector<Match> Searcher::find_all(const std::string &text, size_t limit) const {
  std::vector<Match> out;
  if (!valid_) return out;
  size_t from = 0;
  Match m;
  while (out.size() < limit && from <= text.size() && find(text, from, true, false, m)) {
    out.push_back(m);
    from = m.end > m.start ? m.end : m.start + 1;
  }
  return out;
}

size_t Searcher::count(const std::string &text, size_t limit) const { return find_all(text, limit).size(); }

bool Searcher::matches_at(const std::string &text, size_t start, size_t end) const {
  if (!valid_ || end <= start || end > text.size()) return false;
  Match m;
  if (!find(text, start, true, false, m)) return false;
  return m.start == start && m.end == end;
}

std::string Searcher::expand(const std::string &text, const Match &m, const std::string &replacement) const {
  if (!options_.regex) return replacement;
  // Escapes first: \n, \t, \\ .
  std::string fmt;
  for (size_t i = 0; i < replacement.size(); ++i) {
    char c = replacement[i];
    if (c == '\\' && i + 1 < replacement.size()) {
      char d = replacement[++i];
      if (d == 'n') fmt += '\n';
      else if (d == 't') fmt += '\t';
      else if (d == '\\') fmt += '\\';
      else {
        fmt += '\\';
        fmt += d;
      }
    } else {
      fmt += c;
    }
  }
  const char *base = text.data();
  const char *lb = line_start(base, base + m.start);
  const char *le = line_end(base + m.start, base + text.size());
  std::cmatch cm;
  auto flags = std::regex_constants::match_not_null;
  if (base + m.start > lb) flags |= std::regex_constants::match_prev_avail;
  if (!std::regex_search(base + m.start, le, cm, *re_, flags | std::regex_constants::match_continuous))
    return fmt;
  return cm.format(fmt);
}

size_t Searcher::replace_all(const std::string &text, const std::string &replacement, std::string &out) const {
  auto all = find_all(text, std::string::npos);
  out.clear();
  if (all.empty()) {
    out = text;
    return 0;
  }
  out.reserve(text.size());
  size_t last = 0;
  for (const Match &m : all) {
    out.append(text, last, m.start - last);
    out += expand(text, m, replacement);
    last = m.end;
  }
  out.append(text, last, std::string::npos);
  return all.size();
}

}  // namespace niny
