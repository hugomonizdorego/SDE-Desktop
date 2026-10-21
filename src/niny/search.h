// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny find & replace engine: plain or regular expression (std::regex,
// ECMAScript syntax) search with case and whole-word options. Regular
// expressions match within one line, which keeps them fast and bounded on
// large files.
#pragma once

#include <cstddef>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace niny {

struct SearchOptions {
  bool case_sensitive = false;
  bool whole_word = false;
  bool regex = false;
};

struct Match {
  size_t start = 0, end = 0;
};

class Searcher {
 public:
  // Returns false (and sets |error|) for an invalid regular expression.
  bool set(const std::string &pattern, const SearchOptions &options, std::string *error = nullptr);
  bool valid() const { return valid_; }
  const std::string &pattern() const { return pattern_; }
  const SearchOptions &options() const { return options_; }

  // First match starting at or after |from| (forward) or ending at or before
  // |from| (backward), wrapping around the text when |wrap|.
  bool find(const std::string &text, size_t from, bool forward, bool wrap, Match &m) const;
  // All matches, at most |limit|.
  std::vector<Match> find_all(const std::string &text, size_t limit = 100000) const;
  size_t count(const std::string &text, size_t limit = 100000) const;
  // Whether text[start, end) is exactly a match (for "Replace" on a selection).
  bool matches_at(const std::string &text, size_t start, size_t end) const;
  // Replacement text for the match at text[start, end): "$1"/"$&" groups and
  // \n \t escapes in regex mode, literal otherwise.
  std::string expand(const std::string &text, const Match &m, const std::string &replacement) const;
  // Replaces every match; returns the number of replacements.
  size_t replace_all(const std::string &text, const std::string &replacement, std::string &out) const;

 private:
  bool match_in_line(const char *line_begin, const char *line_end, const char *from, Match &m,
                     const char *base) const;
  bool last_in_line(const char *line_begin, const char *line_end, const char *limit, Match &m,
                    const char *base) const;
  bool word_ok(const std::string &text, size_t start, size_t end) const;
  size_t plain_find(const std::string &text, size_t from) const;

  std::string pattern_, folded_;
  SearchOptions options_;
  std::shared_ptr<std::regex> re_;
  bool valid_ = false;
};

bool is_word_byte(unsigned char c);

}  // namespace niny
