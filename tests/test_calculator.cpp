// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for the Calculator engine (no display): parser, precedence,
// associativity, unary minus, implicit multiplication, percent, functions,
// angle units, errors, number formatting, programmer mode, the unit
// converter and the persistent history.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "converter.h"
#include "engine.h"
#include "programmer.h"
#include "state.h"

using namespace calc;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

// The displayed result of |expr|, or "error:<kind>".
static std::string show(const std::string &expr, Angle angle = Angle::Degrees, Number ans = 0) {
  Context ctx;
  ctx.angle = angle;
  ctx.ans = ans;
  Result r = evaluate(expr, ctx);
  if (!r.ok) return "error:" + std::to_string(int(r.error));
  return format_number(r.value);
}

static void expect(const std::string &expr, const std::string &want, Angle angle = Angle::Degrees) {
  std::string got = show(expr, angle);
  ++g_checks;
  if (got != want) {
    fprintf(stderr, "  %s = %s, expected %s\n", expr.c_str(), got.c_str(), want.c_str());
    ++g_failures;
  }
}

static std::string err(ErrorKind k) { return "error:" + std::to_string(int(k)); }

static void test_basics() {
  // The reported bug: "2 + 2" with spaces, typed or clicked.
  expect("2 + 2", "4");
  expect("2+2", "4");
  expect("  2   +   2  ", "4");
  expect("2 × 2", "4");
  expect("7 − 10", "-3");
  expect("8 ÷ 2", "4");
  expect("1.5", "1.5");
  expect(".5+.5", "1");
  expect("42", "42");
  // Precedence and associativity.
  expect("2+3*4", "14");
  expect("(2+3)*4", "20");
  expect("10-4-3", "3");
  expect("100/10/5", "2");
  expect("2^3^2", "512");
  expect("(2^3)^2", "64");
  expect("2**3", "8");
  expect("2*3^2", "18");
  expect("8/2*4", "16");
  expect("10 mod 3", "1");
  expect("-7 mod 3", "2");
  // Unary minus.
  expect("-5+2", "-3");
  expect("-2^2", "-4");
  expect("(-2)^2", "4");
  expect("2^-1", "0.5");
  expect("3*-2", "-6");
  expect("--3", "3");
  expect("-(2+3)", "-5");
  expect("2-−3", "5");
  // Implicit multiplication.
  expect("2π", "6.283185307179586");
  expect("2(3+4)", "14");
  expect("(1+2)(3+4)", "21");
  expect("2sqrt(9)", "6");
  expect("3√4", "6");
  expect("2e", "5.43656365691809");
  // Parentheses are closed at the end.
  expect("2*(3+4", "14");
  expect("((2", "2");
  // Decimal precision.
  expect("0.1+0.2", "0.3");
  expect("0.3-0.1-0.2", "0");
  expect("1/3*3", "1");
  expect("1.1*1.1", "1.21");
  expect("1-0.9", "0.1");
  expect("1/3", "0.3333333333333333");
  expect("2/3", "0.6666666666666667");
  expect("1e16+1-1e16", "1");
  expect("10^-2", "0.01");
  expect("1.5e3", "1500");
  expect("2E-3", "0.002");
  expect("1e−3", "0.001");
}

static void test_percent() {
  expect("50%", "0.5");
  expect("200+10%", "220");
  expect("200-10%", "180");
  expect("200*10%", "20");
  expect("200/10%", "2000");
  expect("10%*200", "20");
  expect("200+10%*2", "200.2");
  expect("(200+10)%", "2.1");
}

static void test_functions() {
  expect("sin(30)", "0.5");
  expect("sin 30", "0.5");
  expect("cos(60)", "0.5");
  expect("sin(180)", "0");
  expect("cos(90)", "0");
  expect("cos(180)", "-1");
  expect("sin(-90)", "-1");
  expect("tan(45)", "1");
  expect("tan(0)", "0");
  expect("sin(30)+2^3", "8.5");
  expect("sin(30)^2", "0.25");
  expect("asin(0.5)", "30");
  expect("acos(0)", "90");
  expect("atan(1)", "45");
  expect("cos(pi)", "-1", Angle::Radians);
  expect("sin(pi)", "0", Angle::Radians);
  expect("sin(pi/2)", "1", Angle::Radians);
  expect("asin(1)", "1.570796326794897", Angle::Radians);
  expect("sin(100)", "1", Angle::Gradians);
  expect("cos(200)", "-1", Angle::Gradians);
  expect("asin(1)", "100", Angle::Gradians);
  expect("sinh(0)", "0");
  expect("cosh(0)", "1");
  expect("tanh(0)", "0");
  expect("asinh(0)", "0");
  expect("acosh(1)", "0");
  expect("log(1000)", "3");
  expect("log(0.001)", "-3");
  expect("log 100", "2");
  expect("ln(e)", "1");
  expect("ln(1)", "0");
  expect("log2(1024)", "10");
  expect("exp(0)", "1");
  expect("sqrt(81)", "9");
  expect("√81", "9");
  expect("√(16)+1", "5");
  expect("cbrt(27)", "3");
  expect("∛-8", "-2");
  expect("root(16,4)", "2");
  expect("(-8)^(1/3)", "-2");
  expect("5!", "120");
  expect("0!", "1");
  expect("3!!", "720");
  expect("20!", "2.43290200817664e+18");
  expect("17!", "355687428096000");
  expect("abs(-3)", "3");
  expect("3²", "9");
  expect("2³", "8");
  expect("pi", "3.141592653589793");
  expect("π", "3.141592653589793");
  expect("e", "2.718281828459045");
  expect("ncr(5,2)", "10");
  expect("npr(5,2)", "20");
  expect("floor(2.7)+ceil(2.1)", "5");
  CHECK(show("ans*2", Angle::Degrees, 21) == "42");
  CHECK(show("Ans+1", Angle::Degrees, 41) == "42");
}

static void test_errors() {
  expect("1/0", err(ErrorKind::DivideByZero));
  expect("5 mod 0", err(ErrorKind::DivideByZero));
  expect("0/0", err(ErrorKind::Undefined));
  expect("0^-1", err(ErrorKind::DivideByZero));
  expect("sqrt(-1)", err(ErrorKind::Domain));
  expect("log(0)", err(ErrorKind::Domain));
  expect("ln(-1)", err(ErrorKind::Domain));
  expect("asin(2)", err(ErrorKind::Domain));
  expect("tan(90)", err(ErrorKind::Domain));
  expect("(-1)!", err(ErrorKind::Domain));
  expect("(-8)^0.5", err(ErrorKind::Domain));
  expect("10^5000", err(ErrorKind::Overflow));
  expect("2000!", err(ErrorKind::Overflow));
  expect("2+", err(ErrorKind::Incomplete));
  expect("", err(ErrorKind::Incomplete));
  expect("sin", err(ErrorKind::Incomplete));
  expect("2*", err(ErrorKind::Incomplete));
  expect("2+*3", err(ErrorKind::Syntax));
  expect("hello", err(ErrorKind::Syntax));
  expect("2)", err(ErrorKind::Syntax));
  expect("()", err(ErrorKind::Syntax));
  expect("1..2", err(ErrorKind::Syntax));
  expect("2 3", err(ErrorKind::Syntax));
  expect("2+2;x=1", err(ErrorKind::Syntax));
  expect("$", err(ErrorKind::Syntax));
  expect(std::string(20000, '1'), err(ErrorKind::Overflow));
  // Messages are what the display shows.
  Result r = evaluate("1/0");
  CHECK(!r.ok && r.message == "Cannot divide by zero");
  CHECK(evaluate("sqrt(-4)").message == "Invalid input");
  CHECK(evaluate("hello").position == 0);
}

static void test_formatting() {
  CHECK(format_number(0) == "0");
  CHECK(format_number(-0.0L) == "0");
  CHECK(format_number(4) == "4");
  CHECK(format_number(-3.5L) == "-3.5");
  CHECK(format_number(1234567) == "1234567");
  CHECK(format_number(1e15L) == "1000000000000000");
  CHECK(format_number(1e16L) == "1e+16");
  CHECK(format_number(1.5e20L) == "1.5e+20");
  CHECK(format_number(0.000001L) == "0.000001");
  CHECK(format_number(1e-10L) == "1e-10");
  CHECK(format_number(-2.5e-12L) == "-2.5e-12");
  CHECK(format_number(123.456L, 4) == "123.5");
  CHECK(format_number(9.99999999999999999L) == "10");
  CHECK(group_digits("1234567.891") == "1,234,567.891");
  CHECK(group_digits("-1234") == "-1,234");
  CHECK(group_digits("123") == "123");
  CHECK(group_digits("1.5e+20") == "1.5e+20");
  CHECK(display_number(-1234.5L) == "\xE2\x88\x92" "1,234.5");
  CHECK(pretty_operators("2*3-1/4") == "2\xC3\x97" "3\xE2\x88\x92" "1\xC3\xB7" "4");
  // Formatted results parse back to the same display.
  for (Number v : {1 / 3.0L, 1e-7L, 6.02214076e23L, -42.125L}) {
    Result r = evaluate(format_number(v));
    CHECK(r.ok && format_number(r.value) == format_number(v));
  }
  CHECK(continues_result('+') && continues_result('^') && !continues_result('5') && !continues_result('('));
}

static void test_programmer() {
  auto prog = [](const std::string &e, int base = 10, int bits = 64) {
    ProgResult r = evaluate_programmer(e, base, bits);
    return r.ok ? format_base(r.value, 10, bits) : "error:" + std::to_string(int(r.error));
  };
  CHECK(prog("2 + 2") == "4");
  CHECK(prog("2+3*4") == "14");
  CHECK(prog("FF", 16) == "255");
  CHECK(prog("ff + 1", 16) == "256");
  CHECK(prog("777", 8) == "511");
  CHECK(prog("1010", 2) == "10");
  CHECK(prog("0xFF + 0b1 + 0o10") == "264");
  CHECK(prog("12 AND 10") == "8");
  CHECK(prog("12 & 10") == "8");
  CHECK(prog("12 OR 3") == "15");
  CHECK(prog("12 XOR 10") == "6");
  CHECK(prog("12 ^ 10") == "6");
  CHECK(prog("NOT 0") == "-1");
  CHECK(prog("NOT 0", 10, 8) == "-1");
  CHECK(prog("~5") == "-6");
  CHECK(prog("12 NAND 10", 10, 8) == "-9");
  CHECK(prog("12 NOR 3", 10, 8) == "-16");
  CHECK(prog("1 << 4") == "16");
  CHECK(prog("256 >> 4") == "16");
  CHECK(prog("-16 >> 2") == "-4");  // arithmetic
  CHECK(prog("1 << 64") == "0");
  CHECK(prog("7 / 2") == "3");
  CHECK(prog("-7 / 2") == "-3");
  CHECK(prog("7 mod 3") == "1");
  CHECK(prog("7 % 3") == "1");
  CHECK(prog("1+2 << 1") == "6");  // C precedence
  CHECK(prog("(1+2) * 3") == "9");
  CHECK(prog("127 + 1", 10, 8) == "-128");  // wraps like a register
  CHECK(prog("255", 10, 8) == "-1");
  CHECK(prog("65535 + 1", 10, 16) == "0");
  CHECK(prog("1/0") == "error:" + std::to_string(int(ErrorKind::DivideByZero)));
  CHECK(prog("12 +") == "error:" + std::to_string(int(ErrorKind::Incomplete)));
  CHECK(prog("1.5") == "error:" + std::to_string(int(ErrorKind::Syntax)));
  CHECK(prog("FF") == "error:" + std::to_string(int(ErrorKind::Syntax)));  // not in DEC
  CHECK(prog("2", 2) == "error:" + std::to_string(int(ErrorKind::Syntax)));
  CHECK(prog("99999999999999999999") == "error:" + std::to_string(int(ErrorKind::Overflow)));
  CHECK(prog("-9223372036854775807 - 1") == "-9223372036854775808");
  CHECK(prog("(-9223372036854775807 - 1) / -1") == "-9223372036854775808");
  // Conversions between bases and word sizes.
  CHECK(format_base(255, 16, 64) == "FF");
  CHECK(format_base(255, 2, 64) == "11111111");
  CHECK(format_base(255, 8, 64) == "377");
  CHECK(format_base(255, 10, 8) == "-1");
  CHECK(format_base(uint64_t(-1), 16, 32) == "FFFFFFFF");
  CHECK(format_base(uint64_t(-1), 16, 64, true) == "FFFF FFFF FFFF FFFF");
  CHECK(format_base(0xABCDE, 16, 64, true) == "A BCDE");
  CHECK(format_base(1234567, 10, 64, true) == "1,234,567");
  CHECK(format_base(10, 2, 8, true) == "1010");
  CHECK(format_base(0, 2, 8) == "0");
  CHECK(to_signed(0x80, 8) == -128 && to_signed(0x7F, 8) == 127 && to_signed(0xFFFF, 16) == -1);
  CHECK(word_mask(8) == 0xFF && word_mask(64) == ~uint64_t(0));
  CHECK(digit_in_base('F', 16) && !digit_in_base('8', 8) && digit_in_base('1', 2) && !digit_in_base('A', 10));
  ProgResult a = evaluate_programmer("ans + 1", 10, 64, 41);
  CHECK(a.ok && a.value == 42);
}

static void test_converter() {
  load_currency_rates("");  // the built-in table, whatever the user has
  auto conv = [](const std::string &cat, const std::string &from, const std::string &to, Number v) {
    int c = find_category(cat);
    if (c < 0) return std::string("no category");
    const Category &category = categories()[size_t(c)];
    Number out;
    if (!convert(category, find_unit(category, from), find_unit(category, to), v, out)) return std::string("error");
    return format_number(out);
  };
  CHECK(conv("length", "km", "m", 1) == "1000");
  CHECK(conv("length", "in", "cm", 1) == "2.54");
  CHECK(conv("length", "mi", "km", 1) == "1.609344");
  CHECK(conv("length", "ft", "in", 1) == "12");
  CHECK(conv("mass", "lb", "kg", 1) == "0.45359237");
  CHECK(conv("mass", "kg", "g", 2.5L) == "2500");
  CHECK(conv("temperature", "c", "f", 100) == "212");
  CHECK(conv("temperature", "f", "c", 32) == "0");
  CHECK(conv("temperature", "c", "k", 0) == "273.15");
  CHECK(conv("temperature", "f", "c", -40) == "-40");
  CHECK(conv("temperature", "k", "c", 0) == "-273.15");
  CHECK(conv("temperature", "k", "r", 100) == "180");
  CHECK(conv("temperature", "c", "k", -300) == "error");  // below absolute zero
  CHECK(conv("area", "ha", "m2", 1) == "10000");
  CHECK(conv("area", "ac", "m2", 1) == "4046.8564224");
  CHECK(conv("volume", "l", "ml", 1) == "1000");
  CHECK(conv("volume", "gal", "l", 1) == "3.785411784");
  CHECK(conv("volume", "m3", "l", 1) == "1000");
  CHECK(conv("speed", "kmh", "mps", 36) == "10");
  CHECK(conv("speed", "mph", "kmh", 1) == "1.609344");
  CHECK(conv("time", "h", "min", 1.5L) == "90");
  CHECK(conv("time", "d", "s", 1) == "86400");
  CHECK(conv("data", "B", "bit", 1) == "8");
  CHECK(conv("data", "GiB", "MiB", 1) == "1024");
  CHECK(conv("data", "GB", "MB", 1) == "1000");
  CHECK(conv("currency", "EUR", "EUR", 5) == "5");
  CHECK(conv("currency", "USD", "USD", 7) == "7");
  CHECK(conv("length", "m", "m", 3) == "3");
  // Round trip through the base unit.
  CHECK(conv("volume", "cup", "tsp", 1) == "48");
  // User currency rates.
  std::string path = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/sde-calc-test-rates.conf";
  FILE *f = fopen(path.c_str(), "w");
  if (f) {
    fputs("# test\nUSD = 2\nGBP=0.5\nXXX=3\n", f);
    fclose(f);
    load_currency_rates(path);
    CHECK(conv("currency", "EUR", "USD", 10) == "20");
    CHECK(conv("currency", "GBP", "USD", 1) == "4");
    remove(path.c_str());
    load_currency_rates("");
  }
  CHECK(categories().size() == 9);
  for (const auto &c : categories()) {
    CHECK(c.units.size() >= 2);
    CHECK(c.default_from >= 0 && c.default_from < int(c.units.size()));
    CHECK(c.default_to >= 0 && c.default_to < int(c.units.size()));
  }
}

static void test_state() {
  History h;
  h.add({Mode::Standard, "2 + 2", "4"});
  h.add({Mode::Standard, "2 + 2", "4"});  // same as the last: kept once
  h.add({Mode::Scientific, "sin(30)\tx", "0.5"});
  h.add({Mode::Programmer, "", "0"});  // empty: ignored
  CHECK(h.entries().size() == 2);
  CHECK(h.entries()[1].expression == "sin(30) x");
  History back;
  back.parse(h.serialize());
  CHECK(back.entries().size() == 2 && back.entries()[0].result == "4" &&
        back.entries()[1].mode == Mode::Scientific);
  for (int i = 0; i < 150; ++i) back.add({Mode::Standard, std::to_string(i), std::to_string(i)});
  CHECK(back.entries().size() == History::kLimit && back.entries().back().expression == "149");
  back.remove(0);
  CHECK(back.entries().size() == History::kLimit - 1);
  back.parse("garbage\nstandard\tonly two\n# comment\nprogrammer\tFF\t255\n");
  CHECK(back.entries().size() == 1 && back.entries()[0].mode == Mode::Programmer);

  Settings s;
  s.mode = Mode::Programmer;
  s.angle = Angle::Gradians;
  s.base = 16;
  s.bits = 16;
  s.memory = 0.1L;
  s.has_memory = true;
  s.history_visible = false;
  s.category = "mass";
  s.from_unit = "kg";
  s.width = 400;
  s.height = 600;
  Settings t;
  t.parse(s.serialize());
  CHECK(t.mode == Mode::Programmer && t.angle == Angle::Gradians && t.base == 16 && t.bits == 16);
  CHECK(t.has_memory && format_number(t.memory) == "0.1" && !t.history_visible && t.category == "mass");
  CHECK(t.from_unit == "kg" && t.to_unit.empty() && t.width == 400 && t.height == 600);
  Settings bad;
  bad.parse("base=7\nbits=12\nmode=nonsense\nmemory=oops\n");
  CHECK(bad.base == 10 && bad.bits == 64 && bad.mode == Mode::Standard && !bad.has_memory);
  CHECK(mode_from_id(mode_id(Mode::Converter)) == Mode::Converter);
}

int main() {
  test_basics();
  test_percent();
  test_functions();
  test_errors();
  test_formatting();
  test_programmer();
  test_converter();
  test_state();
  printf("calculator: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
