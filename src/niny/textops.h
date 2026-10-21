// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Text transformations used by Niny's editing commands. They work on plain
// strings so that they can be tested without a display.
#pragma once

#include <string>
#include <vector>

namespace niny {

struct IndentStyle {
  bool tabs = false;
  int width = 4;
  bool found = false;  // the text had indented lines
};

// Guesses the indentation of a file from its first few thousand lines.
IndentStyle detect_indent(const std::string &text, size_t max_bytes = 256 * 1024);

std::string leading_whitespace(const std::string &line);
// Indentation for a new line after |line|: the same, plus one level after an
// opening bracket or a ':' (Python, YAML).
std::string auto_indent(const std::string &line, const std::string &unit, bool colon_opens);

// Each line of |block| (whole lines) indented by |unit| / unindented by one
// level of |width| columns. Empty lines stay empty when indenting.
std::string indent_lines(const std::string &block, const std::string &unit);
std::string unindent_lines(const std::string &block, int width);

// Adds the line comment marker to every line (after the common indentation)
// or removes it when all non-blank lines already have it.
std::string toggle_line_comment(const std::string &block, const std::string &marker);
// Wraps |text| in a block comment, or unwraps it.
std::string toggle_block_comment(const std::string &text, const std::string &open, const std::string &close);

std::string trim_trailing_whitespace(const std::string &text);
std::string sort_lines(const std::string &block, bool reverse = false, bool unique = false);
std::string upper_case(const std::string &text);
std::string lower_case(const std::string &text);
std::string title_case(const std::string &text);

// Position of the bracket matching the one at |pos| in |text|, or -1.
// |styles| (same length, may be empty) lets strings and comments be skipped.
long match_bracket(const std::string &text, const std::string &styles, long pos, long max_distance = 1 << 20);
bool is_bracket(char c);

// Display column of byte |pos| in |line| with tabs expanded (0-based).
int display_column(const char *line, int len, int tab_width);

// file:///home/a%20b -> /home/a b; "" for URIs that are not local files.
// Plain paths are returned unchanged.
std::string uri_to_path(const std::string &arg);
// "+12" line arguments and "file:12" / "file:12:4" suffixes.
bool parse_line_arg(const std::string &arg, int &line);
bool split_position_suffix(const std::string &arg, std::string &path, int &line, int &column);

// Lower-case fuzzy match score for the command palette; -1 when |query|
// characters do not all appear in order in |text|.
int fuzzy_score(const std::string &query, const std::string &text);

// ---------------------------------------------------------- Notepad tools

// Characters (UTF-8 code points), words and lines of |text|.
struct TextCounts {
  long chars = 0, words = 0, lines = 0;
};
TextCounts count_text(const char *text, size_t len);
long utf8_length(const char *text, size_t len);
// Status bar text: "1,234 characters" or "13 of 1,234 characters".
std::string character_status(long selected, long total);
std::string group_thousands(long n);

// Text for Edit > Time/Date: "14:05 2026-10-21" (local time of |t|).
std::string date_time_text(long long t);

// Unicode control characters offered by "Display and Unicode".
struct UnicodeControl {
  const char *name;   // "Left-to-right mark"
  const char *abbr;   // "LRM"
  unsigned codepoint;
};
const std::vector<UnicodeControl> &unicode_controls();
std::string utf8_encode(unsigned codepoint);

// Visible markers used by "Show whitespace" for a byte, or nullptr.
const char *whitespace_marker(char c);

// Percent-encodes |text| for a URL query value (spaces become '+').
std::string url_query_encode(const std::string &text);

// One line of "ispell -a" output (hunspell/aspell pipe mode):
// "*" or "+ root" correct, "& word n off: a, b" and "# word off" misspelled.
struct SpellResult {
  bool known = true;  // false for an unrecognized line
  bool correct = true;
  std::vector<std::string> suggestions;
};
SpellResult parse_ispell_line(const std::string &line);
// Words worth spell checking (letters, apostrophes), with byte offsets.
struct WordSpan {
  size_t start = 0, end = 0;
};
std::vector<WordSpan> spell_words(const std::string &text);

}  // namespace niny
