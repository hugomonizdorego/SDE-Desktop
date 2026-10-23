// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit converter: length, mass, temperature, area, volume, speed, time,
// data and currency. Every unit but temperatures is a factor of the
// category's base unit (exact SI / international definitions). Currency
// uses a small offline table of approximate rates that the user can replace
// in ~/.config/sde/calculator-currency.conf ("USD = 1.08" per euro).
#pragma once

#include <string>
#include <vector>

#include "engine.h"

namespace calc {

struct Unit {
  std::string id;      // "km"
  std::string name;    // "Kilometers" (translated)
  std::string symbol;  // "km"
  Number factor = 1;   // value of one unit in the base unit
};

struct Category {
  std::string id;    // "length"
  std::string name;  // "Length" (translated)
  std::vector<Unit> units;
  int default_from = 0, default_to = 1;
  bool temperature = false;
  std::string note;  // e.g. where currency rates come from
};

// All categories (currency with the user's rates, read once).
const std::vector<Category> &categories();
// Re-reads the currency file (for tests: an explicit path).
void load_currency_rates(const std::string &path);

int find_category(const std::string &id);
int find_unit(const Category &c, const std::string &id);

// Converts |v| from unit |from| to unit |to| of |c|. False when the input
// is out of range (below absolute zero) or the result is not finite.
bool convert(const Category &c, int from, int to, Number v, Number &out);

}  // namespace calc
