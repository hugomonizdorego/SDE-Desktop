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

}  // namespace niny
