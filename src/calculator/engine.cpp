// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Recursive descent parser and evaluator, see engine.h.
#include "engine.h"

#include <locale.h>
#include <stdlib.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "i18n.h"

namespace calc {

namespace {

const Number kPi = 3.141592653589793238462643383279502884L;
const Number kE = 2.718281828459045235360287471352662498L;

struct Fail {
  ErrorKind kind;
  int pos;
};

[[noreturn]] void fail(ErrorKind kind, int pos = -1) { throw Fail{kind, pos}; }

enum class Tok { Number, Name, Op, End };

struct Token {
  Tok type = Tok::End;
  Number value = 0;
  std::string name;  // lower case for Name
  char op = 0;       // + - * / ^ ! % ( ) , and 'S' (√), 'C' (∛), '2' (²), '3' (³)
  int pos = 0;
  bool space_before = false;
};

// strtold in the C locale whatever LC_NUMERIC says.
Number parse_decimal(const std::string &text) {
  static locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  return strtold_l(text.c_str(), nullptr, c_locale);
}

bool starts(const std::string &s, size_t i, const char *what) {
  return s.compare(i, std::strlen(what), what) == 0;
}

std::vector<Token> tokenize(const std::string &s) {
  std::vector<Token> out;
  size_t i = 0;
  bool space = false;
  auto push_op = [&](char op, size_t at, size_t len) {
    Token t;
    t.type = Tok::Op;
    t.op = op;
    t.pos = int(at);
    t.space_before = space;
    out.push_back(t);
    i = at + len;
    space = false;
  };
  while (i < s.size()) {
    unsigned char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++i;
      space = true;
      continue;
    }
    // Thin and no-break spaces of pasted, grouped numbers.
    if (starts(s, i, "\xE2\x80\x89") || starts(s, i, "\xE2\x80\xAF")) {
      i += 3;
      space = true;
      continue;
    }
    if (starts(s, i, "\xC2\xA0")) {
      i += 2;
      space = true;
      continue;
    }
    if (std::isdigit(c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
      size_t start = i;
      std::string text;
      bool dot = false;
      while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) {
        if (s[i] == '.') {
          if (dot) fail(ErrorKind::Syntax, int(i));
          dot = true;
        }
        text += s[i++];
      }
      // Exponent: 1.5e3, 2E-4, 1e−5 (only when digits follow, so "2e" is 2·e).
      if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        std::string sign;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
          sign = s[j];
          ++j;
        } else if (starts(s, j, "\xE2\x88\x92")) {
          sign = "-";
          j += 3;
        }
        if (j < s.size() && std::isdigit((unsigned char)s[j])) {
          text += "e" + sign;
          while (j < s.size() && std::isdigit((unsigned char)s[j])) text += s[j++];
          i = j;
        }
      }
      Token t;
      t.type = Tok::Number;
      t.value = parse_decimal(text);
      t.pos = int(start);
      t.space_before = space;
      out.push_back(t);
      space = false;
      continue;
    }
    if (std::isalpha(c) || c == '_') {
      size_t start = i;
      std::string name;
      while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_'))
        name += char(std::tolower((unsigned char)s[i++]));
      Token t;
      t.type = Tok::Name;
      t.name = name;
      t.pos = int(start);
      t.space_before = space;
      out.push_back(t);
      space = false;
      continue;
    }
    if (starts(s, i, "**")) { push_op('^', i, 2); continue; }
    switch (c) {
      case '+': case '-': case '*': case '/': case '^': case '!': case '%': case '(': case ')': case ',':
        push_op(char(c), i, 1);
        continue;
      case '[': case '{': push_op('(', i, 1); continue;
      case ']': case '}': push_op(')', i, 1); continue;
      case ':': push_op('/', i, 1); continue;
      default: break;
    }
    struct Alias { const char *utf8; char op; };
    static const Alias kAliases[] = {
        {"\xC3\x97", '*'},          // ×
        {"\xC2\xB7", '*'},          // ·
        {"\xE2\x8B\x85", '*'},      // ⋅
        {"\xE2\x88\x97", '*'},      // ∗
        {"\xC3\xB7", '/'},          // ÷
        {"\xE2\x88\x95", '/'},      // ∕
        {"\xE2\x88\x92", '-'},      // − minus sign
        {"\xE2\x80\x93", '-'},      // – en dash
        {"\xE2\x88\x9A", 'S'},      // √
        {"\xE2\x88\x9B", 'C'},      // ∛
        {"\xC2\xB2", '2'},          // ²
        {"\xC2\xB3", '3'},          // ³
    };
    bool matched = false;
    for (const auto &a : kAliases) {
      if (starts(s, i, a.utf8)) {
        push_op(a.op, i, std::strlen(a.utf8));
        matched = true;
        break;
      }
    }
    if (matched) continue;
    if (starts(s, i, "\xCF\x80")) {  // π
      Token t;
      t.type = Tok::Name;
      t.name = "pi";
      t.pos = int(i);
      t.space_before = space;
      out.push_back(t);
      i += 2;
      space = false;
      continue;
    }
    fail(ErrorKind::Syntax, int(i));
  }
  Token end;
  end.type = Tok::End;
  end.pos = int(s.size());
  out.push_back(end);
  return out;
}

Number checked(Number v, int pos = -1) {
  if (std::isnan(v)) fail(ErrorKind::Domain, pos);
  if (!std::isfinite(v)) fail(ErrorKind::Overflow, pos);
  return v;
}

bool is_integer(Number v) { return std::isfinite(v) && v == std::floor(v); }

// a + b with rounding noise of cancelling sums removed (0.3 - 0.1 - 0.2 = 0).
Number add(Number a, Number b) {
  Number r = a + b;
  if (r != 0 && std::fabs(r) < (std::fabs(a) + std::fabs(b)) * 1e-18L) r = 0;
  return checked(r);
}

Number factorial(Number x, int pos) {
  if (x < 0 && is_integer(x)) fail(ErrorKind::Domain, pos);
  if (x > 1754) fail(ErrorKind::Overflow, pos);
  if (is_integer(x)) {
    Number r = 1;
    for (int i = 2; i <= int(x); ++i) r *= i;
    return checked(r, pos);
  }
  return checked(std::tgamma(x + 1), pos);
}

Number power(Number a, Number b, int pos) {
  if (a == 0) {
    if (b < 0) fail(ErrorKind::DivideByZero, pos);
    return b == 0 ? 1 : 0;
  }
  if (a < 0 && !is_integer(b)) {
    // Real odd roots of negative numbers: (-8)^(1/3) = -2.
    Number inv = 1 / b;
    Number n = std::round(inv);
    if (std::fabs(inv - n) < 1e-12L && std::fmod(std::fabs(n), 2) == 1) return -std::pow(-a, b);
    fail(ErrorKind::Domain, pos);
  }
  if (is_integer(b) && std::fabs(b) <= 64) {
    // Repeated squaring keeps small integer powers exact (10^-2 = 0.01).
    Number result = 1, base = a;
    unsigned long long e = (unsigned long long)std::fabs(b);
    while (e) {
      if (e & 1) result *= base;
      base *= base;
      e >>= 1;
    }
    return checked(b < 0 ? 1 / result : result, pos);
  }
  return checked(std::pow(a, b), pos);
}

Number to_radians(Number x, Angle a) {
  switch (a) {
    case Angle::Degrees: return x * kPi / 180;
    case Angle::Gradians: return x * kPi / 200;
    case Angle::Radians: break;
  }
  return x;
}

Number from_radians(Number x, Angle a) {
  switch (a) {
    case Angle::Degrees: return x * 180 / kPi;
    case Angle::Gradians: return x * 200 / kPi;
    case Angle::Radians: break;
  }
  return x;
}

// Exact results at quarter turns: returns the quadrant (0-3) or -1.
int quarter_turn(Number x, Angle a) {
  Number quarter = a == Angle::Degrees ? 90 : a == Angle::Gradians ? 100 : 0;
  if (quarter == 0) return -1;
  Number k = x / quarter;
  if (!is_integer(k) || std::fabs(k) > 1e15L) return -1;
  long long q = (long long)std::fmod(k, 4);
  return int((q % 4 + 4) % 4);
}

Number reduce_turn(Number x, Angle a) {
  if (a == Angle::Degrees) return std::fmod(x, 360);
  if (a == Angle::Gradians) return std::fmod(x, 400);
  return x;
}

Number snap_tiny(Number r, Number arg) {
  if (std::fabs(r) < 1e-18L && std::fabs(arg) > 1e-6L) return 0;
  return r;
}

Number trig(const std::string &f, Number x, Angle angle, int pos) {
  if (f == "sin" || f == "cos" || f == "tan") {
    int q = quarter_turn(x, angle);
    if (q >= 0) {
      static const int kSin[] = {0, 1, 0, -1}, kCos[] = {1, 0, -1, 0};
      if (f == "sin") return kSin[q];
      if (f == "cos") return kCos[q];
      if (kCos[q] == 0) fail(ErrorKind::Domain, pos);
      return 0;
    }
    Number r = to_radians(reduce_turn(x, angle), angle);
    if (f == "sin") return snap_tiny(std::sin(r), x);
    if (f == "cos") return snap_tiny(std::cos(r), x);
    return checked(snap_tiny(std::tan(r), x), pos);
  }
  if (f == "asin" || f == "acos") {
    if (x < -1 || x > 1) fail(ErrorKind::Domain, pos);
    return from_radians(f == "asin" ? std::asin(x) : std::acos(x), angle);
  }
  return from_radians(std::atan(x), angle);  // atan
}

class Parser {
 public:
  Parser(const std::vector<Token> &t, const Context &c) : toks_(t), ctx_(c) {}

  Number run() {
    if (peek().type == Tok::End) fail(ErrorKind::Incomplete, peek().pos);
    V v = sum();
    if (peek().type == Tok::Op && peek().op == ')') fail(ErrorKind::Syntax, peek().pos);
    if (peek().type != Tok::End) fail(ErrorKind::Syntax, peek().pos);
    return v.v;
  }

 private:
  struct V {
    Number v = 0;
    bool percent = false;  // a bare "x%" term
  };

  const Token &peek() const { return toks_[i_]; }
  const Token &next() { return toks_[i_ < toks_.size() - 1 ? i_++ : i_]; }
  bool is_op(char op) const { return peek().type == Tok::Op && peek().op == op; }
  bool accept(char op) {
    if (!is_op(op)) return false;
    ++i_;
    return true;
  }
  void expect_operand() {
    if (peek().type == Tok::End) fail(ErrorKind::Incomplete, peek().pos);
  }

  V sum() {
    V left = product();
    for (;;) {
      if (is_op('+') || is_op('-')) {
        char op = next().op;
        expect_operand();
        V right = product();
        Number r = right.percent ? left.v * right.v : right.v;
        left.v = add(left.v, op == '+' ? r : -r);
        left.percent = false;
      } else {
        return left;
      }
    }
  }

  bool starts_operand() const {
    const Token &t = peek();
    if (t.type == Tok::Number || (t.type == Tok::Name && t.name != "mod")) return true;
    return t.type == Tok::Op && (t.op == '(' || t.op == 'S' || t.op == 'C');
  }

  V product() {
    V left = unary();
    for (;;) {
      char op = 0;
      int pos = peek().pos;
      if (is_op('*') || is_op('/')) {
        op = next().op;
      } else if (peek().type == Tok::Name && peek().name == "mod") {
        ++i_;
        op = 'm';
      } else if (starts_operand()) {
        if (peek().type == Tok::Number && toks_[i_ - 1].type == Tok::Number) fail(ErrorKind::Syntax, pos);
        op = '*';  // implicit: 2π, 2(3+4), (1+2)(3+4), 2sin(30)
      } else {
        return left;
      }
      expect_operand();
      V right = unary();
      if (op == '*') {
        left.v = checked(left.v * right.v, pos);
      } else if (op == '/') {
        if (right.v == 0) fail(left.v == 0 ? ErrorKind::Undefined : ErrorKind::DivideByZero, pos);
        left.v = checked(left.v / right.v, pos);
      } else {
        if (right.v == 0) fail(ErrorKind::DivideByZero, pos);
        Number r = std::fmod(left.v, right.v);
        // Mathematical modulo: the result has the sign of the divisor.
        if (r != 0 && ((r < 0) != (right.v < 0))) r += right.v;
        left.v = checked(r, pos);
      }
      left.percent = false;
    }
  }

  V unary() {
    if (is_op('-') || is_op('+')) {
      bool neg = next().op == '-';
      expect_operand();
      V v = unary();
      if (neg) v.v = -v.v;
      return v;
    }
    return pow_level();
  }

  V pow_level() {
    V base = postfix();
    if (is_op('^')) {
      int pos = next().pos;
      expect_operand();
      V exp = unary();  // right associative, allows 2^-1
      base.v = power(base.v, exp.v, pos);
      base.percent = false;
    }
    return base;
  }

  V postfix() {
    V v = primary();
    for (;;) {
      int pos = peek().pos;
      if (accept('!')) {
        v.v = factorial(v.v, pos);
        v.percent = false;
      } else if (accept('%')) {
        v.v = v.v / 100;
        v.percent = true;
      } else if (accept('2')) {
        v.v = checked(v.v * v.v, pos);
        v.percent = false;
      } else if (accept('3')) {
        v.v = checked(v.v * v.v * v.v, pos);
        v.percent = false;
      } else {
        return v;
      }
    }
  }

  std::vector<Number> arguments() {
    std::vector<Number> args;
    if (accept(')')) fail(ErrorKind::Syntax, toks_[i_ - 1].pos);
    for (;;) {
      expect_operand();
      args.push_back(sum().v);
      if (accept(',')) continue;
      if (accept(')') || peek().type == Tok::End) return args;  // closed at the end
      fail(ErrorKind::Syntax, peek().pos);
    }
  }

  Number call(const std::string &f, const std::vector<Number> &a, int pos) {
    auto want = [&](size_t n) {
      if (a.size() != n) fail(ErrorKind::Syntax, pos);
    };
    Angle angle = ctx_.angle;
    if (f == "root" || f == "ncr" || f == "npr" || f == "logb") {
      want(2);
      if (f == "root") {
        if (a[1] == 0) fail(ErrorKind::Domain, pos);
        return power(a[0], 1 / a[1], pos);
      }
      if (f == "logb") {
        if (a[0] <= 0 || a[1] <= 0 || a[1] == 1) fail(ErrorKind::Domain, pos);
        return checked(std::log(a[0]) / std::log(a[1]), pos);
      }
      if (!is_integer(a[0]) || !is_integer(a[1]) || a[0] < 0 || a[1] < 0 || a[1] > a[0])
        fail(ErrorKind::Domain, pos);
      Number r = 1;
      Number k = f == "ncr" ? std::min(a[1], a[0] - a[1]) : a[1];
      for (Number i = 0; i < k; ++i) {
        r = checked(r * (a[0] - i), pos);
        if (f == "ncr") r /= (i + 1);
      }
      return std::round(r);
    }
    want(1);
    Number x = a[0];
    if (f == "sin" || f == "cos" || f == "tan" || f == "asin" || f == "acos" || f == "atan")
      return trig(f, x, angle, pos);
    if (f == "sinh") return checked(std::sinh(x), pos);
    if (f == "cosh") return checked(std::cosh(x), pos);
    if (f == "tanh") return std::tanh(x);
    if (f == "asinh") return std::asinh(x);
    if (f == "acosh") {
      if (x < 1) fail(ErrorKind::Domain, pos);
      return std::acosh(x);
    }
    if (f == "atanh") {
      if (x <= -1 || x >= 1) fail(ErrorKind::Domain, pos);
      return std::atanh(x);
    }
    if (f == "sqrt") {
      if (x < 0) fail(ErrorKind::Domain, pos);
      return std::sqrt(x);
    }
    if (f == "cbrt") return std::cbrt(x);
    if (f == "log" || f == "ln" || f == "log2") {
      if (x <= 0) fail(ErrorKind::Domain, pos);
      if (f == "log") {
        Number r = std::log10(x), n = std::round(r);
        // Exact for powers of ten.
        return std::fabs(r - n) < 1e-15L && power(10, n, pos) == x ? n : r;
      }
      return f == "ln" ? std::log(x) : std::log2(x);
    }
    if (f == "exp") return checked(std::exp(x), pos);
    if (f == "abs") return std::fabs(x);
    if (f == "floor") return std::floor(x);
    if (f == "ceil") return std::ceil(x);
    if (f == "round") return std::round(x);
    if (f == "trunc" || f == "int") return std::trunc(x);
    if (f == "frac") return x - std::trunc(x);
    if (f == "fact") return factorial(x, pos);
    if (f == "sign") return x > 0 ? 1 : x < 0 ? -1 : 0;
    fail(ErrorKind::Syntax, pos);
  }

  static bool is_function(const std::string &n) {
    static const char *kNames[] = {"sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
                                   "asinh", "acosh", "atanh", "sqrt", "cbrt", "log", "ln", "log2",
                                   "exp", "abs", "floor", "ceil", "round", "trunc", "int", "frac",
                                   "fact", "sign", "root", "ncr", "npr", "logb"};
    for (const char *k : kNames)
      if (n == k) return true;
    return false;
  }

  static std::string alias(const std::string &n) {
    if (n == "arcsin") return "asin";
    if (n == "arccos") return "acos";
    if (n == "arctan") return "atan";
    if (n == "lg") return "log";
    if (n == "log10") return "log";
    return n;
  }

  V primary() {
    const Token &t = peek();
    V v;
    if (t.type == Tok::End) fail(ErrorKind::Incomplete, t.pos);
    if (t.type == Tok::Number) {
      ++i_;
      v.v = t.value;
      return v;
    }
    if (t.type == Tok::Op && t.op == '(') {
      ++i_;
      expect_operand();
      if (is_op(')')) fail(ErrorKind::Syntax, peek().pos);
      v = sum();
      if (!accept(')') && peek().type != Tok::End) fail(ErrorKind::Syntax, peek().pos);
      v.percent = false;
      return v;
    }
    if (t.type == Tok::Op && (t.op == 'S' || t.op == 'C')) {
      int pos = t.pos;
      bool square = t.op == 'S';
      ++i_;
      expect_operand();
      Number x = unary().v;
      v.v = call(square ? "sqrt" : "cbrt", {x}, pos);
      return v;
    }
    if (t.type == Tok::Name) {
      std::string n = alias(t.name);
      int pos = t.pos;
      ++i_;
      if (n == "pi") {
        v.v = kPi;
        return v;
      }
      if (n == "e") {
        v.v = kE;
        return v;
      }
      if (n == "ans") {
        v.v = ctx_.ans;
        return v;
      }
      if (is_function(n)) {
        expect_operand();
        std::vector<Number> args;
        if (accept('(')) {
          args = arguments();
        } else {
          args.push_back(unary().v);  // sin 30, √2, log 100
        }
        v.v = call(n, args, pos);
        return v;
      }
      fail(ErrorKind::Syntax, pos);
    }
    fail(ErrorKind::Syntax, t.pos);
  }

  const std::vector<Token> &toks_;
  const Context &ctx_;
  size_t i_ = 0;
};

}  // namespace

std::string error_message(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::Ok: return "";
    case ErrorKind::Incomplete: return _("Incomplete expression");
    case ErrorKind::Syntax: return _("Invalid expression");
    case ErrorKind::DivideByZero: return _("Cannot divide by zero");
    case ErrorKind::Undefined: return _("Result is undefined");
    case ErrorKind::Domain: return _("Invalid input");
    case ErrorKind::Overflow: return _("Result is too large");
  }
  return "";
}

Result evaluate(const std::string &expr, const Context &ctx) {
  Result r;
  if (expr.size() > 10000) {
    r.error = ErrorKind::Overflow;
    r.message = _("Expression is too long");
    return r;
  }
  try {
    std::vector<Token> tokens = tokenize(expr);
    Parser p(tokens, ctx);
    Number v = checked(p.run());
    r.ok = true;
    r.value = v == 0 ? 0 : v;  // no "-0"
  } catch (const Fail &f) {
    r.error = f.kind;
    r.position = f.pos;
    r.message = error_message(f.kind);
  }
  return r;
}

std::string format_number(Number v, int digits) {
  if (digits < 1) digits = 1;
  if (digits > 18) digits = 18;
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  if (v == 0) return "0";
  char buf[96];
  std::snprintf(buf, sizeof buf, "%.*Le", digits - 1, v);
  std::string s = buf;
  bool neg = s[0] == '-';
  if (neg) s.erase(0, 1);
  size_t epos = s.find('e');
  std::string mant = s.substr(0, epos);
  int exp10 = std::atoi(s.c_str() + epos + 1);
  std::string d;  // significant digits
  for (char c : mant)
    if (c != '.') d += c;
  while (d.size() > 1 && d.back() == '0') d.pop_back();
  std::string out;
  if (exp10 >= digits || exp10 < -9) {
    out = d.substr(0, 1);
    if (d.size() > 1) out += "." + d.substr(1);
    out += exp10 < 0 ? "e-" : "e+";
    out += std::to_string(exp10 < 0 ? -exp10 : exp10);
  } else if (exp10 >= 0) {
    if (int(d.size()) <= exp10 + 1) {
      out = d + std::string(exp10 + 1 - d.size(), '0');
    } else {
      out = d.substr(0, exp10 + 1) + "." + d.substr(exp10 + 1);
    }
  } else {
    out = "0." + std::string(-exp10 - 1, '0') + d;
  }
  return neg ? "-" + out : out;
}

std::string group_digits(const std::string &formatted, const std::string &sep) {
  if (formatted.find('e') != std::string::npos) return formatted;
  size_t start = formatted[0] == '-' ? 1 : 0;
  size_t end = formatted.find('.');
  if (end == std::string::npos) end = formatted.size();
  std::string out = formatted.substr(0, start);
  size_t n = end - start;
  for (size_t i = 0; i < n; ++i) {
    out += formatted[start + i];
    size_t left = n - i - 1;
    if (left && left % 3 == 0) out += sep;
  }
  out += formatted.substr(end);
  return out;
}

std::string display_number(Number v, int digits) {
  std::string s = group_digits(format_number(v, digits));
  std::string out;
  for (char c : s) {
    if (c == '-') out += "\xE2\x88\x92";
    else out += c;
  }
  return out;
}

std::string pretty_operators(const std::string &expr) {
  std::string out;
  for (size_t i = 0; i < expr.size(); ++i) {
    char c = expr[i];
    if (c == '*') out += "\xC3\x97";
    else if (c == '/') out += "\xC3\xB7";
    else if (c == '-') out += "\xE2\x88\x92";
    else out += c;
  }
  return out;
}

bool continues_result(char c) { return std::strchr("+-*/^%!", c) != nullptr && c != 0; }

}  // namespace calc
