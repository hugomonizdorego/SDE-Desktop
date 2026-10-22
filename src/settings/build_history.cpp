// SPDX-License-Identifier: GPL-3.0-or-later
#include "build_history.h"

#include <unistd.h>

#include <cstdlib>
#include <map>
#include <memory>

#include "build_config.h"
#include "util.h"

namespace sde {
namespace {

// A small JSON reader for this one file: objects, arrays, strings, numbers
// and literals; enough for build-history.json, nothing more.
struct Value {
  enum Kind { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } kind = NUL;
  std::string str;
  std::vector<Value> items;
  std::map<std::string, Value> members;
  const Value *get(const std::string &key) const {
    auto it = members.find(key);
    return it == members.end() ? nullptr : &it->second;
  }
  std::string text(const std::string &key) const {
    const Value *v = get(key);
    return v && (v->kind == STRING || v->kind == NUMBER) ? v->str : "";
  }
};

class Reader {
 public:
  explicit Reader(const std::string &s) : s_(s) {}
  bool parse(Value &v) {
    if (!value(v, 0)) return false;
    space();
    return i_ == s_.size();
  }

 private:
  void space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
  }
  bool lit(const char *w) {
    size_t n = std::string(w).size();
    if (s_.compare(i_, n, w) != 0) return false;
    i_ += n;
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
  bool hex4(unsigned &cp) {
    if (i_ + 4 > s_.size()) return false;
    char buf[5] = {s_[i_], s_[i_ + 1], s_[i_ + 2], s_[i_ + 3], 0};
    char *end = nullptr;
    cp = unsigned(strtoul(buf, &end, 16));
    if (end != buf + 4) return false;
    i_ += 4;
    return true;
  }
  bool string(std::string &out) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) return false;
      char e = s_[i_++];
      switch (e) {
        case '"': case '\\': case '/': out += e; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) {
            unsigned lo;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool value(Value &v, int depth) {
    if (depth > 32) return false;
    space();
    if (i_ >= s_.size()) return false;
    char c = s_[i_];
    if (c == '"') {
      v.kind = Value::STRING;
      return string(v.str);
    }
    if (c == '{') {
      v.kind = Value::OBJECT;
      ++i_;
      space();
      if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
      for (;;) {
        space();
        std::string key;
        if (!string(key)) return false;
        space();
        if (i_ >= s_.size() || s_[i_++] != ':') return false;
        if (!value(v.members[key], depth + 1)) return false;
        space();
        if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
        return false;
      }
    }
    if (c == '[') {
      v.kind = Value::ARRAY;
      ++i_;
      space();
      if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
      for (;;) {
        v.items.emplace_back();
        if (!value(v.items.back(), depth + 1)) return false;
        space();
        if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
        return false;
      }
    }
    if (lit("true")) { v.kind = Value::BOOL; v.str = "true"; return true; }
    if (lit("false")) { v.kind = Value::BOOL; v.str = "false"; return true; }
    if (lit("null")) { v.kind = Value::NUL; return true; }
    size_t start = i_;
    while (i_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' ||
                              s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
      ++i_;
    if (i_ == start) return false;
    v.kind = Value::NUMBER;
    v.str = s_.substr(start, i_ - start);
    return true;
  }
  const std::string &s_;
  size_t i_ = 0;
};

}  // namespace

bool parse_build_history(const std::string &json, BuildHistory &out) {
  Value root;
  if (!Reader(json).parse(root) || root.kind != Value::OBJECT) return false;
  const Value *entries = root.get("entries");
  if (!entries || entries->kind != Value::ARRAY) return false;
  out = BuildHistory();
  out.version = root.text("version");
  if (const Value *b = root.get("build")) {
    out.commit = b->text("commit");
    out.number = b->text("number");
    out.date = b->text("date");
  }
  for (const auto &e : entries->items) {
    if (e.kind != Value::OBJECT) continue;
    BuildEntry be;
    be.title = e.text("title");
    be.date = e.text("date");
    be.summary = e.text("summary");
    if (const Value *items = e.get("items"))
      for (const auto &i : items->items)
        if (i.kind == Value::STRING) be.items.push_back(i.str);
    if (!be.title.empty()) out.entries.push_back(std::move(be));
  }
  return true;
}

std::string build_history_path() {
  for (const auto &d : data_dirs())
    if (file_exists(d + "/sde/build-history.json")) return d + "/sde/build-history.json";
  std::string installed = std::string(SDE_DATADIR) + "/build-history.json";
  if (file_exists(installed)) return installed;
  // Development builds: build/src/sde-settings and build/build-history.json.
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    std::string dir = buf;
    for (int up = 0; up < 3 && dir.rfind('/') != std::string::npos; ++up) {
      dir = dir.substr(0, dir.rfind('/'));
      if (file_exists(dir + "/build-history.json")) return dir + "/build-history.json";
    }
  }
  return "";
}

}  // namespace sde
