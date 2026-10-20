// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "xsettings.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace sde {
namespace xsettings {

bool Setting::operator==(const Setting &o) const {
  if (type != o.type) return false;
  if (type == INTEGER) return integer == o.integer;
  if (type == STRING) return string == o.string;
  return memcmp(color, o.color, sizeof color) == 0;
}

namespace {

void skip_space(const std::string &s, size_t &i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}

bool parse_number(const std::string &s, size_t &i, long &out) {
  skip_space(s, i);
  const char *start = s.c_str() + i;
  char *end = nullptr;
  long v = strtol(start, &end, 10);
  if (end == start) return false;
  out = v;
  i += size_t(end - start);
  return true;
}

bool parse_value(const std::string &s, size_t i, Setting &out, std::string &why) {
  skip_space(s, i);
  if (i >= s.size()) {
    why = "missing value";
    return false;
  }
  if (s[i] == '"') {
    out.type = Setting::STRING;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) ++i;
      out.string += s[i];
    }
    if (i >= s.size()) {
      why = "unterminated string";
      return false;
    }
    ++i;
  } else if (s[i] == '(') {
    out.type = Setting::COLOR;
    ++i;
    long parts[4] = {0, 0, 0, 0xFFFF};
    int n = 0;
    for (; n < 4; ++n) {
      if (!parse_number(s, i, parts[n])) break;
      skip_space(s, i);
      if (i < s.size() && s[i] == ',') ++i;
    }
    skip_space(s, i);
    if (n < 3 || i >= s.size() || s[i] != ')') {
      why = "color needs (red, green, blue[, alpha])";
      return false;
    }
    ++i;
    for (int k = 0; k < 4; ++k) {
      if (parts[k] < 0 || parts[k] > 0xFFFF) {
        why = "color component out of range 0-65535";
        return false;
      }
      out.color[k] = uint16_t(parts[k]);
    }
  } else {
    out.type = Setting::INTEGER;
    long v;
    if (!parse_number(s, i, v) || v < INT32_MIN || v > INT32_MAX) {
      why = "value must be an integer, a \"string\" or a (color)";
      return false;
    }
    out.integer = int32_t(v);
  }
  skip_space(s, i);
  if (i < s.size() && s[i] != '#') {
    why = "unexpected text after the value";
    return false;
  }
  return true;
}

void put32(std::vector<uint8_t> &b, uint32_t v) {
  uint8_t raw[4];
  memcpy(raw, &v, 4);  // machine byte order, announced in the header
  b.insert(b.end(), raw, raw + 4);
}
void put16(std::vector<uint8_t> &b, uint16_t v) {
  uint8_t raw[2];
  memcpy(raw, &v, 2);
  b.insert(b.end(), raw, raw + 2);
}
void pad4(std::vector<uint8_t> &b) {
  while (b.size() % 4) b.push_back(0);
}

}  // namespace

Settings parse(const std::string &text, std::vector<std::string> *errors) {
  Settings out;
  size_t start = 0;
  int number = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    ++number;
    size_t i = 0;
    skip_space(line, i);
    if (i >= line.size() || line[i] == '#') {
      if (end == text.size()) break;
      continue;
    }
    size_t name_end = i;
    while (name_end < line.size() && !isspace((unsigned char)line[name_end])) ++name_end;
    std::string name = line.substr(i, name_end - i);
    Setting s;
    std::string why;
    if (!parse_value(line, name_end, s, why)) {
      if (errors) errors->push_back("line " + std::to_string(number) + ": " + name + ": " + why);
    } else {
      out[name] = s;
    }
    if (end == text.size()) break;
  }
  return out;
}

bool merge(Settings &current, const Settings &next, uint32_t serial) {
  bool changed = current.size() != next.size();
  Settings merged;
  for (const auto &kv : next) {
    Setting s = kv.second;
    auto old = current.find(kv.first);
    if (old != current.end() && old->second == s) {
      s.serial = old->second.serial;
    } else {
      s.serial = serial;
      changed = true;
    }
    merged[kv.first] = s;
  }
  for (const auto &kv : current)
    if (!next.count(kv.first)) changed = true;
  current = std::move(merged);
  return changed;
}

std::vector<uint8_t> encode(const Settings &settings, uint32_t serial) {
  std::vector<uint8_t> b;
  uint16_t probe = 1;
  bool little = *reinterpret_cast<uint8_t *>(&probe) == 1;
  b.push_back(little ? 0 : 1);  // LSBFirst = 0, MSBFirst = 1
  b.insert(b.end(), 3, 0);
  put32(b, serial);
  put32(b, uint32_t(settings.size()));
  for (const auto &kv : settings) {
    const Setting &s = kv.second;
    b.push_back(uint8_t(s.type));
    b.push_back(0);
    put16(b, uint16_t(kv.first.size()));
    b.insert(b.end(), kv.first.begin(), kv.first.end());
    pad4(b);
    put32(b, s.serial);
    if (s.type == Setting::INTEGER) {
      put32(b, uint32_t(s.integer));
    } else if (s.type == Setting::STRING) {
      put32(b, uint32_t(s.string.size()));
      b.insert(b.end(), s.string.begin(), s.string.end());
      pad4(b);
    } else {
      // The specification orders color components red, blue, green, alpha.
      put16(b, s.color[0]);
      put16(b, s.color[2]);
      put16(b, s.color[1]);
      put16(b, s.color[3]);
    }
  }
  return b;
}

}  // namespace xsettings
}  // namespace sde
