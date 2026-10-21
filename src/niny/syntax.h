// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny syntax highlighting: language definitions and a line-oriented
// tokenizer. Each line is highlighted from the state the previous line
// ended in, so an edit only needs to re-highlight the lines up to the point
// where the state is unchanged again.
#pragma once

#include <string>
#include <vector>

namespace niny {

// Style characters for Fl_Text_Display style buffers ('A' + index).
enum Style : char {
  kPlain = 'A',
  kKeyword,    // B
  kType,       // C
  kString,     // D
  kComment,    // E
  kNumber,     // F
  kPreproc,    // G  preprocessor, decorators, attributes, macros
  kFunction,   // H
  kOperator,   // I
  kTag,        // J  markup tags, CSS elements
  kAttribute,  // K  markup attributes, keys, properties
  kAdded,      // L  diff
  kRemoved,    // M  diff
  kHeading,    // N  markdown headings, INI sections, diff headers
  kConstant,   // O  true/false/null, enum-like values
  kEscape,     // P  escapes and entities
  kSpecial,    // Q  variables, lifetimes, links, pseudo-classes
  kStrong,     // R  bold
  kEmphasis,   // S  italic
  kBuiltin,    // T
};
constexpr int kStyleCount = 20;

// The newline at the end of each line carries the tokenizer state of the
// next line, encoded as kStateBase + state (0..15). Display tables map these
// characters to plain text.
constexpr char kStateBase = 'a';
constexpr int kMaxStates = 16;
inline char state_char(int state) { return char(kStateBase + state); }
inline int char_state(char c) { return c >= kStateBase && c < kStateBase + kMaxStates ? c - kStateBase : -1; }
// Strings and comments: bracket matching skips them.
inline bool is_literal_style(char c) { return c == kString || c == kComment; }

struct Language;

const std::vector<const Language *> &all_languages();
const Language *plain_language();
const Language *find_language(const std::string &id);
// Picks a language from the file name, then from the first line (shebang,
// XML declaration, doctype). Plain text when nothing matches.
const Language *detect_language(const std::string &path, const std::string &first_line = "");

const std::string &language_id(const Language *lang);
const std::string &language_name(const Language *lang);
// Comment markers used by "Toggle Comment"; empty when the language has none.
const std::string &line_comment(const Language *lang);
const std::string &block_comment_open(const Language *lang);
const std::string &block_comment_close(const Language *lang);
bool is_plain(const Language *lang);

// Highlights one line (no newline) of |n| bytes into |out| (n style chars)
// and returns the state for the next line.
int highlight_line(const Language *lang, const char *s, int n, int state, char *out);

// Highlights a block of text made of whole lines; the newline characters get
// state characters. Returns the state after the last newline.
int highlight_text(const Language *lang, const char *s, size_t n, int state, char *out);

}  // namespace niny
