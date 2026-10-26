// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The content stream lexer and parser; see content.h. It follows the
// syntax of PDF 32000-1 section 7.2 and 8.9.7 (inline images) and is
// lenient where real files are not: stray delimiters, unbalanced arrays
// and garbage are skipped instead of failing the whole page.
#include "content.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xanara {

namespace {

bool is_ws(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool is_delim(unsigned char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}
bool is_regular(unsigned char c) { return !is_ws(c) && !is_delim(c); }
int hexval(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct Lexer {
  const std::string &s;
  size_t i = 0;
  explicit Lexer(const std::string &data) : s(data) {}

  void skip_ws() {
    while (i < s.size()) {
      unsigned char c = (unsigned char)s[i];
      if (is_ws(c)) {
        ++i;
      } else if (c == '%') {
        while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i;
      } else {
        break;
      }
    }
  }

  std::string literal() {  // at '('
    std::string out;
    int depth = 1;
    ++i;
    while (i < s.size()) {
      char c = s[i++];
      if (c == '(') {
        ++depth;
        out += c;
      } else if (c == ')') {
        if (--depth == 0) break;
        out += c;
      } else if (c == '\\') {
        if (i >= s.size()) break;
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case '\r':
            if (i < s.size() && s[i] == '\n') ++i;
            break;
          case '\n': break;
          default:
            if (e >= '0' && e <= '7') {
              int v = e - '0';
              for (int k = 0; k < 2 && i < s.size() && s[i] >= '0' && s[i] <= '7'; ++k) v = v * 8 + (s[i++] - '0');
              out += char(v & 0xFF);
            } else {
              out += e;
            }
        }
      } else if (c == '\r') {
        // An end of line inside a string reads as a single line feed.
        if (i < s.size() && s[i] == '\n') ++i;
        out += '\n';
      } else {
        out += c;
      }
    }
    return out;
  }

  std::string hexstring() {  // after '<'
    std::string out;
    int hi = -1;
    while (i < s.size() && s[i] != '>') {
      int v = hexval((unsigned char)s[i++]);
      if (v < 0) continue;
      if (hi < 0) {
        hi = v;
      } else {
        out += char(hi * 16 + v);
        hi = -1;
      }
    }
    if (hi >= 0) out += char(hi * 16);
    if (i < s.size()) ++i;  // '>'
    return out;
  }

  std::string name() {  // after '/'
    std::string out;
    while (i < s.size() && is_regular((unsigned char)s[i])) {
      char c = s[i++];
      if (c == '#' && i + 1 < s.size() && hexval((unsigned char)s[i]) >= 0 && hexval((unsigned char)s[i + 1]) >= 0) {
        out += char(hexval((unsigned char)s[i]) * 16 + hexval((unsigned char)s[i + 1]));
        i += 2;
      } else {
        out += c;
      }
    }
    return out;
  }

  std::string word() {
    size_t b = i;
    while (i < s.size() && is_regular((unsigned char)s[i])) ++i;
    return s.substr(b, i - b);
  }

  static bool number_word(const std::string &w, double &v) {
    if (w.empty()) return false;
    size_t k = 0;
    bool neg = false;
    while (k < w.size() && (w[k] == '+' || w[k] == '-')) neg = neg != (w[k++] == '-');  // "--5" and "+-5" read leniently
    bool digits = false, dot = false;
    double val = 0, scale = 0.1;
    for (; k < w.size(); ++k) {
      char c = w[k];
      if (c >= '0' && c <= '9') {
        digits = true;
        if (!dot) val = val * 10 + (c - '0');
        else {
          val += (c - '0') * scale;
          scale /= 10;
        }
      } else if (c == '.' && !dot) {
        dot = true;
      } else {
        break;  // "12.5.3" or "1e5": the readable prefix
      }
    }
    if (!digits) return false;
    if (k < w.size() && !(w[k] == '.' || (w[k] >= '0' && w[k] <= '9') || w[k] == 'e' || w[k] == 'E'))
      return false;  // a keyword that starts like a number
    v = neg ? -val : val;
    return true;
  }

  // One value; false at the end, at a keyword (|kw| set) or at a closing
  // delimiter (|kw| = "]" or ">>").
  bool value(Operand &o, std::string &kw, int depth) {
    for (;;) {
      skip_ws();
      if (i >= s.size()) {
        kw.clear();
        return false;
      }
      unsigned char c = (unsigned char)s[i];
      if (c == '(') {
        o = Operand();
        o.kind = Operand::String;
        o.str = literal();
        return true;
      }
      if (c == '<') {
        if (i + 1 < s.size() && s[i + 1] == '<') {
          i += 2;
          o = Operand();
          o.kind = Operand::Dict;
          collect(o, ">>", depth + 1);
          return true;
        }
        ++i;
        o = Operand();
        o.kind = Operand::String;
        o.hex = true;
        o.str = hexstring();
        return true;
      }
      if (c == '>') {
        if (i + 1 < s.size() && s[i + 1] == '>') {
          i += 2;
          kw = ">>";
          return false;
        }
        ++i;  // stray
        continue;
      }
      if (c == '[') {
        ++i;
        o = Operand();
        o.kind = Operand::Array;
        collect(o, "]", depth + 1);
        return true;
      }
      if (c == ']') {
        ++i;
        kw = "]";
        return false;
      }
      if (c == '/') {
        ++i;
        o = Operand();
        o.kind = Operand::Name;
        o.str = name();
        return true;
      }
      if (c == '{' || c == '}' || c == ')') {
        ++i;
        continue;
      }
      std::string w = word();
      if (w.empty()) {
        ++i;
        continue;
      }
      double v;
      if (number_word(w, v)) {
        o = Operand();
        o.kind = Operand::Number;
        o.num = v;
        return true;
      }
      if (w == "true" || w == "false") {
        o = Operand();
        o.kind = Operand::Boolean;
        o.boolean = w == "true";
        return true;
      }
      if (w == "null") {
        o = Operand();
        return true;
      }
      kw = w;
      return false;
    }
  }

  void collect(Operand &container, const char *close, int depth) {
    if (depth > 64) return;
    for (;;) {
      Operand item;
      std::string kw;
      if (value(item, kw, depth)) {
        container.items.push_back(std::move(item));
        continue;
      }
      if (kw.empty() || kw == close) return;
      if (kw == "]" || kw == ">>") return;  // unbalanced: close what is open
      // A keyword inside an array (damaged content): keep it as text.
      Operand other;
      other.kind = Operand::Other;
      other.str = kw;
      container.items.push_back(other);
    }
  }
};

double inline_number(const Operand &dict, const char *a, const char *b, double fallback) {
  const Operand *v = dict.get(a);
  if (!v) v = dict.get(b);
  return v && v->kind == Operand::Number ? v->num : fallback;
}

// The length of an unfiltered inline image, or 0 when it cannot be known.
size_t inline_length(const Operand &dict) {
  const Operand *f = dict.get("F");
  if (!f) f = dict.get("Filter");
  if (f && !(f->kind == Operand::Array && f->items.empty())) return 0;
  double w = inline_number(dict, "W", "Width", 0), h = inline_number(dict, "H", "Height", 0);
  const Operand *mask = dict.get("IM");
  if (!mask) mask = dict.get("ImageMask");
  bool is_mask = mask && mask->kind == Operand::Boolean && mask->boolean;
  double bpc = is_mask ? 1 : inline_number(dict, "BPC", "BitsPerComponent", 8);
  int comps = 1;
  const Operand *cs = dict.get("CS");
  if (!cs) cs = dict.get("ColorSpace");
  if (!is_mask && cs && cs->kind == Operand::Name) {
    if (cs->str == "RGB" || cs->str == "DeviceRGB" || cs->str == "CalRGB") comps = 3;
    else if (cs->str == "CMYK" || cs->str == "DeviceCMYK") comps = 4;
    else if (cs->str != "G" && cs->str != "DeviceGray" && cs->str != "CalGray") return 0;  // named resource
  } else if (!is_mask && cs && cs->kind == Operand::Array) {
    if (cs->items.empty() || !(cs->items[0].is_name("I") || cs->items[0].is_name("Indexed"))) return 0;
  }
  if (w <= 0 || h <= 0 || w > 1e5 || h > 1e5) return 0;
  size_t row = (size_t(w) * size_t(comps) * size_t(bpc) + 7) / 8;
  return row * size_t(h);
}

}  // namespace

const Operand *Operand::get(const std::string &key) const {
  if (kind != Dict) return nullptr;
  for (size_t k = 0; k + 1 < items.size(); k += 2)
    if (items[k].kind == Name && items[k].str == key) return &items[k + 1];
  return nullptr;
}

double Op::num(size_t i, double fallback) const {
  return i < args.size() && args[i].kind == Operand::Number ? args[i].num : fallback;
}

std::vector<Op> parse_content(const std::string &data) {
  std::vector<Op> ops;
  Lexer lx(data);
  std::vector<Operand> args;
  size_t begin = std::string::npos;
  for (;;) {
    lx.skip_ws();
    size_t at = lx.i;
    Operand o;
    std::string kw;
    if (lx.value(o, kw, 0)) {
      if (begin == std::string::npos) begin = at;
      if (args.size() < 65536) args.push_back(std::move(o));
      continue;
    }
    if (kw.empty()) break;
    if (kw == "]" || kw == ">>") continue;  // stray closing delimiter
    Op op;
    op.name = kw;
    op.begin = begin == std::string::npos ? at : begin;
    if (kw == "BI") {
      // Inline image: key/value pairs up to ID, the data, then EI.
      Operand dict;
      dict.kind = Operand::Dict;
      for (;;) {
        Operand v;
        std::string k2;
        if (lx.value(v, k2, 1)) {
          dict.items.push_back(std::move(v));
          continue;
        }
        if (k2 == "ID" || k2.empty()) {
          kw = k2;
          break;
        }
      }
      if (kw != "ID") break;
      if (lx.i < data.size() && is_ws((unsigned char)data[lx.i])) ++lx.i;
      size_t start = lx.i, stop = std::string::npos;
      size_t len = inline_length(dict);
      if (len && start + len <= data.size()) {
        size_t k = start + len;
        while (k < data.size() && is_ws((unsigned char)data[k])) ++k;
        if (data.compare(k, 2, "EI") == 0 && (k + 2 >= data.size() || !is_regular((unsigned char)data[k + 2])))
          stop = start + len;
      }
      size_t ei_end = std::string::npos;
      if (stop == std::string::npos) {
        // Filtered data: the first "EI" between white space that is
        // followed by something that reads like content again.
        for (size_t k = start; k + 1 < data.size(); ++k) {
          if (data[k] != 'E' || data[k + 1] != 'I') continue;
          if (k == start || !is_ws((unsigned char)data[k - 1])) continue;
          if (k + 2 < data.size() && !is_ws((unsigned char)data[k + 2]) && !is_delim((unsigned char)data[k + 2]))
            continue;
          bool text = true;
          for (size_t m = k + 2; m < data.size() && m < k + 24; ++m) {
            unsigned char c = (unsigned char)data[m];
            if (c > 126 || (c < 32 && !is_ws(c))) {
              text = false;
              break;
            }
          }
          if (!text) continue;
          stop = k - 1;
          ei_end = k + 2;
          break;
        }
        if (stop == std::string::npos) break;
      }
      if (ei_end == std::string::npos) {
        size_t k = stop;
        while (k < data.size() && is_ws((unsigned char)data[k])) ++k;
        ei_end = k + 2;
      }
      op.args.push_back(std::move(dict));
      op.data_begin = start;
      op.data_end = stop;
      lx.i = std::min(ei_end, data.size());
      op.end = lx.i;
      ops.push_back(std::move(op));
      args.clear();
      begin = std::string::npos;
      continue;
    }
    op.args = std::move(args);
    op.end = lx.i;
    ops.push_back(std::move(op));
    args.clear();
    begin = std::string::npos;
  }
  return ops;
}

std::string pdf_num(double v, int decimals) {
  if (!std::isfinite(v)) v = 0;
  char buf[64];
  snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 10), v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0" || s.empty()) s = "0";
  return s;
}

std::string pdf_hex(const std::string &bytes) {
  static const char *digits = "0123456789ABCDEF";
  std::string out = "<";
  for (unsigned char c : bytes) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out + ">";
}

std::string pdf_literal(const std::string &bytes) {
  std::string out = "(";
  for (unsigned char c : bytes) {
    if (c == '(' || c == ')' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c < 32 || c > 126) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\%03o", c);
      out += buf;
    } else {
      out += char(c);
    }
  }
  return out + ")";
}

std::string pdf_name(const std::string &name) {
  std::string out = "/";
  for (unsigned char c : name) {
    if (c < 0x21 || c > 0x7E || is_delim(c) || c == '#') {
      char buf[8];
      snprintf(buf, sizeof buf, "#%02X", c);
      out += buf;
    } else {
      out += char(c);
    }
  }
  return out;
}

std::string unparse(const Operand &o) {
  switch (o.kind) {
    case Operand::Number: return pdf_num(o.num, 6);
    case Operand::Boolean: return o.boolean ? "true" : "false";
    case Operand::Name: return pdf_name(o.str);
    case Operand::String: return o.hex ? pdf_hex(o.str) : pdf_literal(o.str);
    case Operand::Array:
    case Operand::Dict: {
      std::string out = o.kind == Operand::Array ? "[" : "<<";
      for (size_t k = 0; k < o.items.size(); ++k) {
        if (k) out += ' ';
        out += unparse(o.items[k]);
      }
      return out + (o.kind == Operand::Array ? "]" : ">>");
    }
    case Operand::Other: return o.str;
    case Operand::Null: return "null";
  }
  return "null";
}

std::string apply_splices(const std::string &data, std::vector<Splice> splices) {
  std::sort(splices.begin(), splices.end(), [](const Splice &a, const Splice &b) { return a.begin < b.begin; });
  std::string out;
  out.reserve(data.size() + 256);
  size_t at = 0;
  for (const auto &s : splices) {
    if (s.begin < at || s.end < s.begin || s.end > data.size()) continue;
    out.append(data, at, s.begin - at);
    out += s.text;
    at = s.end;
  }
  out.append(data, at, std::string::npos);
  return out;
}

}  // namespace xanara
