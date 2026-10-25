// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sde {
namespace merkadu {

namespace {

struct Parser {
  const std::string &s;
  size_t i = 0;
  int depth = 0;

  explicit Parser(const std::string &text) : s(text) {}

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }

  bool literal(const char *word) {
    size_t n = 0;
    while (word[n]) ++n;
    if (s.compare(i, n, word) != 0) return false;
    i += n;
    return true;
  }

  static void utf8(std::string &out, unsigned cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(unsigned &v) {
    if (i + 4 > s.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= unsigned(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }

  bool string(std::string &out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return false;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) return false;
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            unsigned lo;
            if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return false;
            i += 2;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp < 0xE000) {
            return false;
          }
          utf8(out, cp);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool number(Json &out) {
    size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    if (i >= s.size()) return false;
    if (s[i] == '0') {
      ++i;
    } else if (s[i] >= '1' && s[i] <= '9') {
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    } else {
      return false;
    }
    bool integral = true;
    if (i < s.size() && s[i] == '.') {
      integral = false;
      ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      integral = false;
      ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    std::string text = s.substr(start, i - start);
    out.type = Json::Number;
    out.num = std::strtod(text.c_str(), nullptr);
    if (integral && text.size() < 19)
      out.integer = std::strtoll(text.c_str(), nullptr, 10);
    else
      out.integer = std::isfinite(out.num) && std::fabs(out.num) < 9.2e18 ? int64_t(out.num) : 0;
    return true;
  }

  bool value(Json &out) {
    ws();
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '{') {
      if (++depth > 64) return false;
      ++i;
      out.type = Json::Object;
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
        --depth;
        return true;
      }
      for (;;) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        Json v;
        if (!value(v)) return false;
        out.fields.emplace_back(std::move(key), std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          ++i;
          --depth;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      if (++depth > 64) return false;
      ++i;
      out.type = Json::Array;
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
        --depth;
        return true;
      }
      for (;;) {
        Json v;
        if (!value(v)) return false;
        out.items.push_back(std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          ++i;
          --depth;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      out.type = Json::String;
      return string(out.str);
    }
    if (literal("true")) {
      out.type = Json::Bool;
      out.b = true;
      return true;
    }
    if (literal("false")) {
      out.type = Json::Bool;
      return true;
    }
    if (literal("null")) {
      out.type = Json::Null;
      return true;
    }
    return number(out);
  }
};

const std::vector<Json> &empty_items() {
  static const std::vector<Json> none;
  return none;
}

}  // namespace

const Json *Json::get(const std::string &key) const {
  if (type != Object) return nullptr;
  for (auto &f : fields)
    if (f.first == key) return &f.second;
  return nullptr;
}

std::string Json::text(const std::string &key, const std::string &fallback) const {
  const Json *v = get(key);
  return v && v->type == String ? v->str : fallback;
}

int64_t Json::number(const std::string &key, int64_t fallback) const {
  const Json *v = get(key);
  if (!v) return fallback;
  if (v->type == Number) return v->integer;
  if (v->type == String) {
    char *end = nullptr;
    long long n = std::strtoll(v->str.c_str(), &end, 10);
    if (end && *end == '\0' && !v->str.empty()) return n;
  }
  return fallback;
}

bool Json::flag(const std::string &key, bool fallback) const {
  const Json *v = get(key);
  return v && v->type == Bool ? v->b : fallback;
}

const std::vector<Json> &Json::array(const std::string &key) const {
  const Json *v = get(key);
  return v && v->type == Array ? v->items : empty_items();
}

std::vector<std::string> Json::strings(const std::string &key) const {
  std::vector<std::string> out;
  for (auto &item : array(key))
    if (item.type == String) out.push_back(item.str);
  return out;
}

bool parse_json(const std::string &text, Json &out) {
  Parser p(text);
  Json v;
  if (!p.value(v)) return false;
  p.ws();
  if (p.i != text.size()) return false;
  out = std::move(v);
  return true;
}

std::string json_quote(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  return out + "\"";
}

}  // namespace merkadu
}  // namespace sde
