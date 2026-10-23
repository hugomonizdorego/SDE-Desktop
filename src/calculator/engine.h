// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Calculator engine: expression parser and evaluator for the Standard and
// Scientific modes, written from scratch for SDE (no display needed).
//
// Grammar, lowest precedence first:
//
//   sum      := product (("+" | "-") product)*
//   product  := unary (("*" | "/" | "mod" | implicit) unary)*
//   unary    := ("-" | "+") unary | power
//   power    := postfix ("^" unary)?          right associative: 2^3^2 = 512
//   postfix  := primary ("!" | "%" | "²" | "³")*
//   primary  := number | constant | "(" sum ")" | function arg | name
//
// so -2^2 = -4, 2^-1 = 0.5, 2π and 2(3+4) multiply implicitly and missing
// closing parentheses at the end are added. Unicode operators (× ÷ − √ π
// ²) and ASCII ones are accepted alike; white space is ignored.
//
// Percent follows pocket calculators: "50%" is 0.5, "200 + 10%" is 220,
// "200 - 10%" is 180, "200 × 10%" is 20 and "200 ÷ 10%" is 2000.
//
// Numbers are long double (64-bit mantissa). Results are shown with 16
// significant digits, so binary rounding noise never shows (0.1 + 0.2 is
// 0.3); sums that cancel to rounding noise become exactly 0, and the
// trigonometric functions are exact at multiples of 90° (sin 180° = 0).
#pragma once

#include <cstdint>
#include <string>

namespace calc {

using Number = long double;

enum class Angle { Degrees, Radians, Gradians };

enum class ErrorKind {
  Ok,
  Incomplete,    // the expression ends early, e.g. "2+" (no error while typing)
  Syntax,        // misplaced token, unknown name
  DivideByZero,  // "Cannot divide by zero"
  Undefined,     // 0/0, 0^0 with a negative exponent ...
  Domain,        // sqrt(-1), log(0), asin(2): "Invalid input"
  Overflow,      // "Result is too large"
};

struct Result {
  bool ok = false;
  Number value = 0;
  ErrorKind error = ErrorKind::Ok;
  std::string message;  // translated, empty when ok
  int position = -1;    // byte offset of the problem, -1 when unknown
};

struct Context {
  Angle angle = Angle::Degrees;
  Number ans = 0;  // value of "Ans"
};

// Evaluates |expr|. Never throws; errors are reported in the Result.
Result evaluate(const std::string &expr, const Context &ctx = {});

// The message shown for an error kind (translated).
std::string error_message(ErrorKind kind);

// Formats a value for display with at most |digits| significant digits
// (1 to 18), trailing zeros removed; scientific notation ("1.5e+20") from
// 10^digits up and below 10^-9. Uses '.' and '-' (no grouping).
std::string format_number(Number v, int digits = 16);

// format_number() for the big display: digit groups ("1,234,567.5") and a
// real minus sign (U+2212).
std::string display_number(Number v, int digits = 16);

// Groups the integer digits of an already formatted number with |sep|.
std::string group_digits(const std::string &formatted, const std::string &sep = ",");

// Expression text as the user sees it: "*" becomes "×", "/" "÷", "-" "−".
std::string pretty_operators(const std::string &expr);

// True if |c| (ASCII) is an operator that continues an expression after a
// result, e.g. "+" after "= 4" gives "4+".
bool continues_result(char c);

}  // namespace calc
