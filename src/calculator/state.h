// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Persistent Calculator state: the history of calculations and the
// preferences (mode, angle unit, base, word size, memory, history panel).
//
//   $XDG_STATE_HOME/sde/calculator-history.tsv   mode TAB expression TAB result
//   $XDG_STATE_HOME/sde/calculator.ini           key=value
#pragma once

#include <string>
#include <vector>

#include "engine.h"

namespace calc {

enum class Mode { Standard, Scientific, Programmer, Converter };

const char *mode_id(Mode m);  // "standard", ...
Mode mode_from_id(const std::string &id);
std::string mode_name(Mode m);  // translated

struct HistoryEntry {
  Mode mode = Mode::Standard;
  std::string expression;
  std::string result;
};

class History {
 public:
  static const size_t kLimit = 100;

  const std::vector<HistoryEntry> &entries() const { return entries_; }  // oldest first
  void add(const HistoryEntry &e);  // drops an identical last entry and the oldest beyond kLimit
  void remove(size_t index);
  void clear() { entries_.clear(); }

  bool load(const std::string &path);
  bool save(const std::string &path) const;
  // The file format (one entry per line).
  std::string serialize() const;
  void parse(const std::string &text);

 private:
  std::vector<HistoryEntry> entries_;
};

struct Settings {
  Mode mode = Mode::Standard;
  Angle angle = Angle::Degrees;
  int base = 10;
  int bits = 64;
  Number memory = 0;
  bool has_memory = false;
  bool history_visible = true;
  std::string category = "length";
  std::string from_unit, to_unit;
  int width = 0, height = 0;

  bool load(const std::string &path);
  bool save(const std::string &path) const;
  std::string serialize() const;
  void parse(const std::string &text);
};

std::string history_path();   // in state_home()/sde
std::string settings_path();

}  // namespace calc
