// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "state.h"

#include <cstdlib>

#include "i18n.h"
#include "util.h"

namespace calc {

const char *mode_id(Mode m) {
  switch (m) {
    case Mode::Standard: return "standard";
    case Mode::Scientific: return "scientific";
    case Mode::Programmer: return "programmer";
    case Mode::Converter: return "converter";
  }
  return "standard";
}

Mode mode_from_id(const std::string &id) {
  if (id == "scientific") return Mode::Scientific;
  if (id == "programmer") return Mode::Programmer;
  if (id == "converter") return Mode::Converter;
  return Mode::Standard;
}

std::string mode_name(Mode m) {
  switch (m) {
    case Mode::Standard: return _("Standard");
    case Mode::Scientific: return _("Scientific");
    case Mode::Programmer: return _("Programmer");
    case Mode::Converter: return _("Converter");
  }
  return "";
}

namespace {

std::string clean(const std::string &s) {
  std::string out;
  for (char c : s) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
  return out;
}

}  // namespace

void History::add(const HistoryEntry &e) {
  if (e.expression.empty()) return;
  if (!entries_.empty() && entries_.back().expression == e.expression && entries_.back().result == e.result &&
      entries_.back().mode == e.mode)
    return;
  entries_.push_back(HistoryEntry{e.mode, clean(e.expression), clean(e.result)});
  while (entries_.size() > kLimit) entries_.erase(entries_.begin());
}

void History::remove(size_t index) {
  if (index < entries_.size()) entries_.erase(entries_.begin() + long(index));
}

std::string History::serialize() const {
  std::string out = "# SDE Calculator history: mode, expression, result\n";
  for (const auto &e : entries_) out += std::string(mode_id(e.mode)) + "\t" + e.expression + "\t" + e.result + "\n";
  return out;
}

void History::parse(const std::string &text) {
  entries_.clear();
  for (const auto &line : sde::split(text, '\n')) {
    if (line.empty() || line[0] == '#') continue;
    auto fields = sde::split(line, '\t', true);
    if (fields.size() != 3 || fields[1].empty()) continue;
    add(HistoryEntry{mode_from_id(fields[0]), fields[1], fields[2]});
  }
}

bool History::load(const std::string &path) {
  std::string text;
  if (!sde::read_file(path, text)) return false;
  parse(text);
  return true;
}

bool History::save(const std::string &path) const {
  sde::make_dirs(path.substr(0, path.rfind('/')));
  return sde::write_file_atomic(path, serialize());
}

std::string Settings::serialize() const {
  std::string out = "# SDE Calculator preferences\n";
  out += std::string("mode=") + mode_id(mode) + "\n";
  out += std::string("angle=") + (angle == Angle::Radians ? "rad" : angle == Angle::Gradians ? "grad" : "deg") + "\n";
  out += "base=" + std::to_string(base) + "\n";
  out += "bits=" + std::to_string(bits) + "\n";
  if (has_memory) out += "memory=" + format_number(memory, 18) + "\n";
  out += std::string("history=") + (history_visible ? "1" : "0") + "\n";
  out += "category=" + category + "\n";
  if (!from_unit.empty()) out += "from=" + from_unit + "\n";
  if (!to_unit.empty()) out += "to=" + to_unit + "\n";
  if (width > 0 && height > 0) out += "size=" + std::to_string(width) + "x" + std::to_string(height) + "\n";
  return out;
}

void Settings::parse(const std::string &text) {
  for (const auto &line : sde::split(text, '\n')) {
    auto eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string k = sde::trim(line.substr(0, eq)), v = sde::trim(line.substr(eq + 1));
    if (k == "mode") mode = mode_from_id(v);
    else if (k == "angle") angle = v == "rad" ? Angle::Radians : v == "grad" ? Angle::Gradians : Angle::Degrees;
    else if (k == "base") {
      int b = std::atoi(v.c_str());
      if (b == 2 || b == 8 || b == 10 || b == 16) base = b;
    } else if (k == "bits") {
      int b = std::atoi(v.c_str());
      if (b == 8 || b == 16 || b == 32 || b == 64) bits = b;
    } else if (k == "memory") {
      Result r = evaluate(v);
      if (r.ok) {
        memory = r.value;
        has_memory = true;
      }
    } else if (k == "history") history_visible = v != "0";
    else if (k == "category") category = v;
    else if (k == "from") from_unit = v;
    else if (k == "to") to_unit = v;
    else if (k == "size") {
      auto x = v.find('x');
      if (x != std::string::npos) {
        width = std::atoi(v.c_str());
        height = std::atoi(v.c_str() + x + 1);
      }
    }
  }
}

bool Settings::load(const std::string &path) {
  std::string text;
  if (!sde::read_file(path, text)) return false;
  parse(text);
  return true;
}

bool Settings::save(const std::string &path) const {
  sde::make_dirs(path.substr(0, path.rfind('/')));
  return sde::write_file_atomic(path, serialize());
}

std::string history_path() { return sde::state_home() + "/sde/calculator-history.tsv"; }
std::string settings_path() { return sde::state_home() + "/sde/calculator.ini"; }

}  // namespace calc
