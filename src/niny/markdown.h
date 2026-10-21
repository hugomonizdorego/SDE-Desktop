// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Markdown helpers for Niny: the formatting toolbar (headings, lists, bold,
// italic, links, clear formatting) edits plain strings, and a small parser
// feeds the read-only rendered view. No display is needed, so the unit tests
// use them directly.
#pragma once

#include <string>
#include <vector>

namespace niny {
namespace md {

// An edit of a text: replace [start, end) with |text|, then select
// [sel_start, sel_end) (positions in the text after the edit).
struct Edit {
  size_t start = 0, end = 0;
  std::string text;
  size_t sel_start = 0, sel_end = 0;
};

// Heading level (1..6) of a line, 0 for none.
int heading_level(const std::string &line);
// The line with its heading prefix set to |level| (0 removes it). List or
// quote markers are kept outside the heading only for plain lines.
std::string set_heading(const std::string &line, int level);

enum class ListKind { Bullet, Numbered, Task, Quote };
// Lines of |block| with the list marker added; when every non-blank line
// already has it, the marker is removed instead. Numbered lists count up.
std::string toggle_list(const std::string &block, ListKind kind);
// The list kind of a line, if any (Quote counts as a kind); false otherwise.
bool list_kind(const std::string &line, ListKind &kind);

// Enter at the end of a list item: the prefix for the next line ("- ",
// "3. ", "- [ ] ", "> " with the same indentation), or "" when |line| is not
// a list item. |ends| is set when the item is empty, so Enter ends the list.
std::string continue_list(const std::string &line, bool &ends);

// Toggles an inline marker ("**", "*", "~~", "`") around [a, b) of |text|.
// The marker is removed when it surrounds the selection (inside or just
// outside it); an empty selection inserts a pair and places the caret
// between the markers.
Edit toggle_wrap(const std::string &text, size_t a, size_t b, const std::string &marker);
// Turns [a, b) into "[text](url)" and selects "url" (or the text when the
// selection already looks like a web address).
Edit make_link(const std::string &text, size_t a, size_t b);
// Removes inline formatting, heading markers and links (keeps link text).
std::string clear_formatting(const std::string &text);

// ---------------------------------------------------------- rendered view

enum class Block { Paragraph, Heading, Bullet, Numbered, Task, Quote, Code, Rule, Blank };
struct Line {
  Block kind = Block::Paragraph;
  int level = 0;        // heading level, or list nesting (indent / 2)
  bool checked = false; // task lists
  std::string marker;   // "3." for numbered lists
  std::string text;     // inline Markdown (raw for code)
};
// Splits a document into blocks. Paragraph lines are joined with a space,
// fenced code keeps its lines.
std::vector<Line> parse(const std::string &doc);

struct Span {
  std::string text;
  bool bold = false, italic = false, code = false, strike = false, link = false;
  std::string url;
};
// Inline formatting of one block's text.
std::vector<Span> parse_inline(const std::string &text);

}  // namespace md
}  // namespace niny
