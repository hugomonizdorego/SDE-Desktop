// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// A small JSON reader and writer for Merkadu: snapd's REST answers, the
// test fixtures and the image cache index. Strict RFC 8259 syntax; numbers
// are doubles (and exact 64-bit integers when they fit).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sde {
namespace merkadu {

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double num = 0;
  int64_t integer = 0;  // Number: the value when it is an integer
  std::string str;
  std::vector<Json> items;                           // Array
  std::vector<std::pair<std::string, Json>> fields;  // Object, in order

  const Json *get(const std::string &key) const;  // nullptr when absent
  // Convenience lookups with fallbacks (wrong types give the fallback).
  std::string text(const std::string &key, const std::string &fallback = "") const;
  int64_t number(const std::string &key, int64_t fallback = 0) const;
  bool flag(const std::string &key, bool fallback = false) const;
  const std::vector<Json> &array(const std::string &key) const;  // empty when absent
  // Strings of an array of strings.
  std::vector<std::string> strings(const std::string &key) const;

  bool is_object() const { return type == Object; }
  bool is_array() const { return type == Array; }
};

// Parses |text|; false (and |out| untouched) on malformed input or nesting
// deeper than 64 levels.
bool parse_json(const std::string &text, Json &out);
// |s| as a JSON string literal, with the quotes.
std::string json_quote(const std::string &s);

}  // namespace merkadu
}  // namespace sde
