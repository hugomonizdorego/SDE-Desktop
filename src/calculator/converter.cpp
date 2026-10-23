// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tables of the Converter mode, see converter.h.
#include "converter.h"

#include <cmath>

#include "i18n.h"
#include "util.h"

namespace calc {

namespace {

struct CurrencyRow {
  const char *code;
  const char *name;
  double per_euro;  // approximate units per euro (offline sample)
};

const CurrencyRow kCurrencies[] = {
    {"EUR", N_("Euro"), 1.0},
    {"USD", N_("US Dollar"), 1.16},
    {"GBP", N_("British Pound"), 0.87},
    {"CHF", N_("Swiss Franc"), 0.94},
    {"JPY", N_("Japanese Yen"), 172.0},
    {"CNY", N_("Chinese Yuan"), 8.3},
    {"CAD", N_("Canadian Dollar"), 1.61},
    {"AUD", N_("Australian Dollar"), 1.77},
    {"BRL", N_("Brazilian Real"), 6.3},
    {"MXN", N_("Mexican Peso"), 21.5},
    {"INR", N_("Indian Rupee"), 101.0},
    {"KRW", N_("South Korean Won"), 1620.0},
    {"SEK", N_("Swedish Krona"), 11.0},
    {"NOK", N_("Norwegian Krone"), 11.7},
    {"DKK", N_("Danish Krone"), 7.46},
    {"PLN", N_("Polish Zloty"), 4.26},
    {"CZK", N_("Czech Koruna"), 24.4},
    {"HUF", N_("Hungarian Forint"), 390.0},
    {"TRY", N_("Turkish Lira"), 48.0},
    {"ZAR", N_("South African Rand"), 20.3},
};

std::vector<Category> g_categories;
std::string g_currency_path;
bool g_loaded = false;

Unit u(const char *id, const char *name, const char *symbol, Number factor) {
  return Unit{id, _(name), symbol, factor};
}

Category currency(const std::string &path) {
  Category c;
  c.id = "currency";
  c.name = _("Currency");
  std::string text;
  bool custom = !path.empty() && sde::read_file(path, text);
  for (const auto &row : kCurrencies) {
    Number rate = row.per_euro;
    if (custom) {
      for (const auto &line : sde::split(text, '\n')) {
        auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        if (sde::trim(line.substr(0, eq)) != row.code) continue;
        Number r = std::strtold(sde::trim(line.substr(eq + 1)).c_str(), nullptr);
        if (r > 0 && std::isfinite(r)) rate = r;
      }
    }
    c.units.push_back(Unit{row.code, _(row.name), row.code, 1 / rate});
  }
  c.default_from = 0;
  c.default_to = 1;
  c.note = custom ? sde::format(_("Rates from %s"), path.c_str())
                  : std::string(_("Approximate offline rates, not live. Set your own in "
                                  "~/.config/sde/calculator-currency.conf."));
  return c;
}

Category make_category(const char *id, const std::string &name, bool temperature = false) {
  Category c;
  c.id = id;
  c.name = name;
  c.temperature = temperature;
  return c;
}

void build() {
  g_categories.clear();
  Category length = make_category("length", _("Length"));
  length.units = {u("nm", N_("Nanometers"), "nm", 1e-9L),    u("um", N_("Micrometers"), "µm", 1e-6L),
                  u("mm", N_("Millimeters"), "mm", 1e-3L),   u("cm", N_("Centimeters"), "cm", 1e-2L),
                  u("m", N_("Meters"), "m", 1),              u("km", N_("Kilometers"), "km", 1000),
                  u("in", N_("Inches"), "in", 0.0254L),      u("ft", N_("Feet"), "ft", 0.3048L),
                  u("yd", N_("Yards"), "yd", 0.9144L),       u("mi", N_("Miles"), "mi", 1609.344L),
                  u("nmi", N_("Nautical miles"), "nmi", 1852)};
  length.default_from = 5;
  length.default_to = 9;
  g_categories.push_back(length);

  Category mass = make_category("mass", _("Mass"));
  mass.units = {u("mg", N_("Milligrams"), "mg", 1e-6L),   u("g", N_("Grams"), "g", 1e-3L),
                u("kg", N_("Kilograms"), "kg", 1),        u("t", N_("Metric tons"), "t", 1000),
                u("oz", N_("Ounces"), "oz", 0.028349523125L), u("lb", N_("Pounds"), "lb", 0.45359237L),
                u("st", N_("Stones"), "st", 6.35029318L), u("ct", N_("Carats"), "ct", 0.0002L)};
  mass.default_from = 2;
  mass.default_to = 5;
  g_categories.push_back(mass);

  Category temp = make_category("temperature", _("Temperature"), true);
  temp.units = {u("c", N_("Celsius"), "°C", 1), u("f", N_("Fahrenheit"), "°F", 1), u("k", N_("Kelvin"), "K", 1),
                u("r", N_("Rankine"), "°R", 1)};
  g_categories.push_back(temp);

  Category area = make_category("area", _("Area"));
  area.units = {u("mm2", N_("Square millimeters"), "mm²", 1e-6L), u("cm2", N_("Square centimeters"), "cm²", 1e-4L),
                u("m2", N_("Square meters"), "m²", 1),          u("ha", N_("Hectares"), "ha", 1e4L),
                u("km2", N_("Square kilometers"), "km²", 1e6L), u("in2", N_("Square inches"), "in²", 0.00064516L),
                u("ft2", N_("Square feet"), "ft²", 0.09290304L), u("yd2", N_("Square yards"), "yd²", 0.83612736L),
                u("ac", N_("Acres"), "ac", 4046.8564224L),      u("mi2", N_("Square miles"), "mi²", 2589988.110336L)};
  area.default_from = 2;
  area.default_to = 6;
  g_categories.push_back(area);

  // Base: liter.
  Category volume = make_category("volume", _("Volume"));
  volume.units = {u("ml", N_("Milliliters"), "mL", 1e-3L),          u("l", N_("Liters"), "L", 1),
                  u("m3", N_("Cubic meters"), "m³", 1000),           u("cm3", N_("Cubic centimeters"), "cm³", 1e-3L),
                  u("tsp", N_("Teaspoons (US)"), "tsp", 0.00492892159375L),
                  u("tbsp", N_("Tablespoons (US)"), "tbsp", 0.01478676478125L),
                  u("floz", N_("Fluid ounces (US)"), "fl oz", 0.0295735295625L),
                  u("cup", N_("Cups (US)"), "cup", 0.2365882365L), u("pt", N_("Pints (US)"), "pt", 0.473176473L),
                  u("qt", N_("Quarts (US)"), "qt", 0.946352946L),  u("gal", N_("Gallons (US)"), "gal", 3.785411784L),
                  u("galuk", N_("Gallons (UK)"), "gal (UK)", 4.54609L),
                  u("in3", N_("Cubic inches"), "in³", 0.016387064L), u("ft3", N_("Cubic feet"), "ft³", 28.316846592L)};
  volume.default_from = 1;
  volume.default_to = 10;
  g_categories.push_back(volume);

  Category speed = make_category("speed", _("Speed"));
  speed.units = {u("mps", N_("Meters per second"), "m/s", 1), u("kmh", N_("Kilometers per hour"), "km/h", 1 / 3.6L),
                 u("mph", N_("Miles per hour"), "mph", 0.44704L), u("kn", N_("Knots"), "kn", 1852 / 3600.0L),
                 u("fps", N_("Feet per second"), "ft/s", 0.3048L)};
  g_categories.push_back(speed);

  Category time = make_category("time", _("Time"));
  time.units = {u("ns", N_("Nanoseconds"), "ns", 1e-9L), u("us", N_("Microseconds"), "µs", 1e-6L),
                u("ms", N_("Milliseconds"), "ms", 1e-3L), u("s", N_("Seconds"), "s", 1),
                u("min", N_("Minutes"), "min", 60),       u("h", N_("Hours"), "h", 3600),
                u("d", N_("Days"), "d", 86400),           u("wk", N_("Weeks"), "wk", 604800),
                u("yr", N_("Years (365.25 days)"), "yr", 31557600)};
  time.default_from = 5;
  time.default_to = 4;
  g_categories.push_back(time);

  // Base: bit.
  Category data = make_category("data", _("Data"));
  data.units = {u("bit", N_("Bits"), "bit", 1),          u("B", N_("Bytes"), "B", 8),
                u("kB", N_("Kilobytes"), "kB", 8e3L),     u("MB", N_("Megabytes"), "MB", 8e6L),
                u("GB", N_("Gigabytes"), "GB", 8e9L),     u("TB", N_("Terabytes"), "TB", 8e12L),
                u("KiB", N_("Kibibytes"), "KiB", 8192),   u("MiB", N_("Mebibytes"), "MiB", 8 * 1048576.0L),
                u("GiB", N_("Gibibytes"), "GiB", 8 * 1073741824.0L),
                u("TiB", N_("Tebibytes"), "TiB", 8 * 1099511627776.0L)};
  data.default_from = 4;
  data.default_to = 8;
  g_categories.push_back(data);

  g_categories.push_back(currency(g_currency_path));
}

Number to_kelvin(int unit, Number v) {
  switch (unit) {
    case 0: return v + 273.15L;
    case 1: return (v - 32) * 5 / 9 + 273.15L;
    case 3: return v * 5 / 9;
    default: return v;
  }
}

Number from_kelvin(int unit, Number k) {
  switch (unit) {
    case 0: return k - 273.15L;
    case 1: return (k - 273.15L) * 9 / 5 + 32;
    case 3: return k * 9 / 5;
    default: return k;
  }
}

}  // namespace

void load_currency_rates(const std::string &path) {
  g_currency_path = path;
  build();
  g_loaded = true;
}

const std::vector<Category> &categories() {
  if (!g_loaded) load_currency_rates(sde::config_home() + "/sde/calculator-currency.conf");
  return g_categories;
}

int find_category(const std::string &id) {
  const auto &all = categories();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].id == id) return int(i);
  return -1;
}

int find_unit(const Category &c, const std::string &id) {
  for (size_t i = 0; i < c.units.size(); ++i)
    if (c.units[i].id == id) return int(i);
  return -1;
}

bool convert(const Category &c, int from, int to, Number v, Number &out) {
  if (from < 0 || to < 0 || from >= int(c.units.size()) || to >= int(c.units.size())) return false;
  if (c.temperature) {
    Number k = to_kelvin(from, v);
    if (k < -1e-12L) return false;  // below absolute zero
    out = from_kelvin(to, k < 0 ? 0 : k);
    if (std::fabs(out) < 1e-12L) out = 0;
  } else {
    out = from == to ? v : v * c.units[from].factor / c.units[to].factor;
  }
  return std::isfinite(out);
}

}  // namespace calc
