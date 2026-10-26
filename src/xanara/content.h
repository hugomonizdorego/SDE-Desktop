// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Content streams: the page description language of PDF split into
// operations (operands and operator) that remember where they are in the
// stream, so that an edit replaces exactly the operations it changes and
// leaves every other byte of the stream as it was. The same lexer reads
// the CMaps of fonts. Plus the syntax helpers for writing new content.
#pragma once

#include <string>
#include <vector>

namespace xanara {

struct Operand {
  enum Kind { Null, Number, Boolean, Name, String, Array, Dict, Other } kind = Null;
  double num = 0;
  bool boolean = false;
  bool hex = false;              // a string written as <hex>
  std::string str;               // string bytes (decoded) or name (without '/')
  std::vector<Operand> items;    // array items; dictionaries: key, value, key, value...
  bool is_number() const { return kind == Number; }
  bool is_name(const char *n) const { return kind == Name && str == n; }
  const Operand *get(const std::string &key) const;  // dictionary lookup
};

struct Op {
  std::string name;            // the operator; "BI" for a whole inline image
  std::vector<Operand> args;   // inline images: the image dictionary
  size_t begin = 0, end = 0;   // the bytes of operands and operator
  size_t data_begin = 0, data_end = 0;  // inline image data
  double num(size_t i, double fallback = 0) const;
};

// Splits |data| into operations. Never throws: damaged content ends the
// list where it cannot be read any further.
std::vector<Op> parse_content(const std::string &data);

// Number syntax for new content: at most |decimals| digits, no exponent.
std::string pdf_num(double v, int decimals = 4);
std::string pdf_hex(const std::string &bytes);  // <48656C6C6F>
std::string pdf_literal(const std::string &bytes);  // (escaped)
std::string pdf_name(const std::string &name);  // "/Name", with #xx where needed
std::string unparse(const Operand &o);

// Replaces byte ranges of |data|; the ranges must not overlap.
struct Splice {
  size_t begin = 0, end = 0;
  std::string text;
};
std::string apply_splices(const std::string &data, std::vector<Splice> splices);

}  // namespace xanara
