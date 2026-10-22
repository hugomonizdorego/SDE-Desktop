// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Integer evaluator of the Programmer mode, see programmer.h.
#include "programmer.h"

#include <cctype>
#include <cstring>
#include <vector>

#include "i18n.h"

namespace calc {

uint64_t word_mask(int bits) { return bits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1); }

int64_t to_signed(uint64_t v, int bits) {
  v &= word_mask(bits);
  if (bits < 64 && (v >> (bits - 1)) & 1) v |= ~word_mask(bits);
  return int64_t(v);
}

bool digit_in_base(char c, int base) {
  int d;
  if (c >= '0' && c <= '9') d = c - '0';
  else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
  else return false;
  return d < base;
}

namespace {

struct Fail {
  ErrorKind kind;
};
[[noreturn]] void fail(ErrorKind k) { throw Fail{k}; }

struct Token {
  enum Type { Number, Op, End } type = End;
  uint64_t value = 0;
  std::string op;  // "+", "<<", "and", ...
};

int digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 99;
}

uint64_t parse_literal(const std::string &digits, int base) {
  if (digits.empty()) fail(ErrorKind::Syntax);
  unsigned __int128 v = 0;
  for (char c : digits) {
    if (c == '_' || c == '\'') continue;
    int d = digit_value(c);
    if (d >= base) fail(ErrorKind::Syntax);
    v = v * base + d;
    if (v > ~uint64_t(0)) fail(ErrorKind::Overflow);
  }
  return uint64_t(v);
}

bool starts(const std::string &s, size_t i, const char *w) { return s.compare(i, std::strlen(w), w) == 0; }

std::vector<Token> tokenize(const std::string &s, int base, uint64_t ans) {
  std::vector<Token> out;
  size_t i = 0;
  auto op = [&](const std::string &o, size_t len) {
    Token t;
    t.type = Token::Op;
    t.op = o;
    out.push_back(t);
    i += len;
  };
  while (i < s.size()) {
    unsigned char c = s[i];
    if (std::isspace(c)) {
      ++i;
      continue;
    }
    if (std::isalnum(c) || c == '_') {
      size_t j = i;
      std::string word;
      while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '\'')) word += s[j++];
      std::string low;
      for (char ch : word) low += char(std::tolower((unsigned char)ch));
      Token t;
      t.type = Token::Number;
      if (low.size() > 2 && low[0] == '0' && (low[1] == 'x' || low[1] == 'o' || (low[1] == 'b' && base != 16))) {
        t.value = parse_literal(word.substr(2), low[1] == 'x' ? 16 : low[1] == 'o' ? 8 : 2);
        out.push_back(t);
        i = j;
        continue;
      }
      if (low == "and" || low == "or" || low == "xor" || low == "not" || low == "nand" || low == "nor" ||
          low == "mod" || low == "shl" || low == "shr") {
        std::string o = low == "shl" ? "<<" : low == "shr" ? ">>" : low;
        op(o, j - i);
        continue;
      }
      if (low == "ans") {
        t.value = ans;
        out.push_back(t);
        i = j;
        continue;
      }
      bool digits = true;
      for (char ch : low)
        if (ch != '_' && ch != '\'' && !digit_in_base(ch, 16)) digits = false;
      if (!digits) fail(ErrorKind::Syntax);
      t.value = parse_literal(word, base);
      out.push_back(t);
      i = j;
      continue;
    }
    if (starts(s, i, "<<")) { op("<<", 2); continue; }
    if (starts(s, i, ">>")) { op(">>", 2); continue; }
    if (starts(s, i, "\xC3\x97")) { op("*", 2); continue; }        // ×
    if (starts(s, i, "\xC3\xB7")) { op("/", 2); continue; }        // ÷
    if (starts(s, i, "\xE2\x88\x92")) { op("-", 3); continue; }    // −
    if (std::strchr("+-*/%&|^~()", c)) {
      static const char *kMap[][2] = {{"&", "and"}, {"|", "or"}, {"^", "xor"}, {"~", "not"}, {"%", "mod"}};
      std::string o(1, char(c));
      for (auto &m : kMap)
        if (o == m[0]) o = m[1];
      op(o, 1);
      continue;
    }
    fail(ErrorKind::Syntax);
  }
  out.push_back(Token{});
  return out;
}

class Parser {
 public:
  Parser(const std::vector<Token> &t, int bits) : t_(t), bits_(bits), mask_(word_mask(bits)) {}

  uint64_t run() {
    if (t_[0].type == Token::End) fail(ErrorKind::Incomplete);
    uint64_t v = level(0);
    if (t_[i_].type != Token::End) fail(ErrorKind::Syntax);
    return v & mask_;
  }

 private:
  bool is(const char *o) const { return t_[i_].type == Token::Op && t_[i_].op == o; }
  void need() const {
    if (t_[i_].type == Token::End) fail(ErrorKind::Incomplete);
  }

  // 0: or nor, 1: xor, 2: and nand, 3: shifts, 4: + -, 5: * / mod
  uint64_t level(int n) {
    if (n == 6) return unary();
    static const std::vector<std::vector<const char *>> kOps = {
        {"or", "nor"}, {"xor"}, {"and", "nand"}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "mod"}};
    uint64_t left = level(n + 1);
    for (;;) {
      const char *found = nullptr;
      for (const char *o : kOps[n])
        if (is(o)) found = o;
      if (!found) return left;
      ++i_;
      need();
      uint64_t right = level(n + 1);
      left = apply(found, left, right) & mask_;
    }
  }

  uint64_t apply(const std::string &o, uint64_t a, uint64_t b) {
    int64_t sa = to_signed(a, bits_), sb = to_signed(b, bits_);
    if (o == "or") return a | b;
    if (o == "nor") return ~(a | b);
    if (o == "xor") return a ^ b;
    if (o == "and") return a & b;
    if (o == "nand") return ~(a & b);
    if (o == "+") return a + b;
    if (o == "-") return a - b;
    if (o == "*") return a * b;
    if (o == "<<") return b >= uint64_t(bits_) ? 0 : a << b;
    if (o == ">>") {
      if (b >= uint64_t(bits_)) return sa < 0 ? ~uint64_t(0) : 0;
      return uint64_t(sa >> b);  // arithmetic shift
    }
    if (sb == 0) fail(ErrorKind::DivideByZero);
    if (sb == -1) return o == "/" ? uint64_t(0) - a : 0;  // avoids INT64_MIN / -1
    if (o == "/") return uint64_t(sa / sb);
    return uint64_t(sa % sb);
  }

  uint64_t unary() {
    if (is("-")) {
      ++i_;
      need();
      return (uint64_t(0) - unary()) & mask_;
    }
    if (is("+")) {
      ++i_;
      need();
      return unary();
    }
    if (is("not")) {
      ++i_;
      need();
      return ~unary() & mask_;
    }
    return primary();
  }

  uint64_t primary() {
    const Token &t = t_[i_];
    if (t.type == Token::End) fail(ErrorKind::Incomplete);
    if (t.type == Token::Number) {
      ++i_;
      return t.value & mask_;
    }
    if (t.op == "(") {
      ++i_;
      need();
      uint64_t v = level(0);
      if (is(")")) ++i_;
      else if (t_[i_].type != Token::End) fail(ErrorKind::Syntax);
      return v;
    }
    fail(ErrorKind::Syntax);
  }

  const std::vector<Token> &t_;
  int bits_;
  uint64_t mask_;
  size_t i_ = 0;
};

}  // namespace

ProgResult evaluate_programmer(const std::string &expr, int base, int bits, uint64_t ans) {
  ProgResult r;
  if (bits != 8 && bits != 16 && bits != 32) bits = 64;
  try {
    auto tokens = tokenize(expr, base, ans);
    Parser p(tokens, bits);
    r.value = p.run();
    r.ok = true;
  } catch (const Fail &f) {
    r.error = f.kind;
    r.message = f.kind == ErrorKind::Overflow ? std::string(_("Number is too large")) : error_message(f.kind);
  }
  return r;
}

std::string format_base(uint64_t v, int base, int bits, bool group) {
  v &= word_mask(bits);
  std::string digits;
  bool neg = false;
  uint64_t u = v;
  if (base == 10) {
    int64_t s = to_signed(v, bits);
    neg = s < 0;
    u = neg ? uint64_t(0) - uint64_t(s) : uint64_t(s);
  }
  do {
    digits.insert(digits.begin(), "0123456789ABCDEF"[u % base]);
    u /= base;
  } while (u);
  if (group) {
    int every = base == 10 || base == 8 ? 3 : 4;
    const char *sep = base == 10 ? "," : " ";
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
      out += digits[i];
      size_t left = digits.size() - i - 1;
      if (left && left % every == 0) out += sep;
    }
    digits = out;
  }
  return neg ? "-" + digits : digits;
}

}  // namespace calc
