// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Programmer mode: integer expressions in two's complement with a word size
// of 8, 16, 32 or 64 bits, entered in hexadecimal, decimal, octal or binary.
//
// Operators, lowest precedence first (as in C):
//   OR |    XOR ^    AND &    << >> (arithmetic)    + -    * / mod %
//   unary - ~ NOT, and NAND / NOR as binary operators at the AND / OR level.
// Literals use the current base; 0x.., 0o.. (and 0b.. outside HEX) override
// it. Results wrap around to the word size like a CPU register.
#pragma once

#include <cstdint>
#include <string>

#include "engine.h"

namespace calc {

struct ProgResult {
  bool ok = false;
  uint64_t value = 0;  // masked to the word size
  ErrorKind error = ErrorKind::None;
  std::string message;
};

uint64_t word_mask(int bits);
// Sign-extends the low |bits| bits.
int64_t to_signed(uint64_t v, int bits);

ProgResult evaluate_programmer(const std::string &expr, int base, int bits, uint64_t ans = 0);

// |v| in |base| (DEC is signed). |group|: spaces every 4 digits (HEX, BIN),
// 3 digits (OCT) or commas every 3 digits (DEC).
std::string format_base(uint64_t v, int base, int bits, bool group = false);

// Whether a digit character is valid in a base (e.g. 'A' in 16).
bool digit_in_base(char c, int base);

}  // namespace calc
