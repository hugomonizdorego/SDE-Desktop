// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "codec.h"

#include <strings.h>

#include <cstdint>
#include <cstring>

namespace niny {

namespace {

// Windows-1252 0x80-0x9F. The five undefined bytes map to the C1 controls
// (as in ISO-8859-1), so every byte round-trips.
const unsigned kCp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

bool cp1252_defined(unsigned char b) {
  return b >= 0x80 && b <= 0x9F && b != 0x81 && b != 0x8D && b != 0x8F && b != 0x90 && b != 0x9D;
}

bool has_prefix(const std::string &s, const char *p, size_t n) {
  return s.size() >= n && memcmp(s.data(), p, n) == 0;
}

bool decode_utf16(const std::string &raw, size_t start, bool little, std::string &out) {
  out.clear();
  out.reserve(raw.size());
  size_t n = raw.size();
  if ((n - start) % 2) return false;
  for (size_t i = start; i + 1 < n; i += 2) {
    unsigned a = (unsigned char)raw[i], b = (unsigned char)raw[i + 1];
    unsigned u = little ? (a | b << 8) : (a << 8 | b);
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (i + 3 >= n) return false;
      unsigned c = (unsigned char)raw[i + 2], d = (unsigned char)raw[i + 3];
      unsigned lo = little ? (c | d << 8) : (c << 8 | d);
      if (lo < 0xDC00 || lo > 0xDFFF) return false;
      u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
      i += 2;
    } else if (u >= 0xDC00 && u <= 0xDFFF) {
      return false;
    }
    append_utf8(out, u);
  }
  return true;
}

void put16(std::string &out, unsigned u, bool little) {
  if (little) {
    out += char(u & 0xFF);
    out += char(u >> 8);
  } else {
    out += char(u >> 8);
    out += char(u & 0xFF);
  }
}

}  // namespace

const char *encoding_name(Encoding e) {
  switch (e) {
    case Encoding::Utf8: return "UTF-8";
    case Encoding::Utf8Bom: return "UTF-8 BOM";
    case Encoding::Utf16LE: return "UTF-16 LE";
    case Encoding::Utf16BE: return "UTF-16 BE";
    case Encoding::Latin1: return "ISO-8859-1";
    case Encoding::Windows1252: return "Windows-1252";
  }
  return "UTF-8";
}

const std::vector<Encoding> &all_encodings() {
  static const std::vector<Encoding> all{Encoding::Utf8,    Encoding::Utf8Bom, Encoding::Utf16LE,
                                         Encoding::Utf16BE, Encoding::Latin1,  Encoding::Windows1252};
  return all;
}

bool encoding_from_name(const std::string &name, Encoding &out) {
  for (Encoding e : all_encodings())
    if (strcasecmp(name.c_str(), encoding_name(e)) == 0) {
      out = e;
      return true;
    }
  if (strcasecmp(name.c_str(), "utf8") == 0) out = Encoding::Utf8;
  else if (strcasecmp(name.c_str(), "latin1") == 0 || strcasecmp(name.c_str(), "latin-1") == 0) out = Encoding::Latin1;
  else if (strcasecmp(name.c_str(), "cp1252") == 0) out = Encoding::Windows1252;
  else return false;
  return true;
}

const char *eol_name(Eol e) { return e == Eol::CRLF ? "CRLF" : e == Eol::CR ? "CR" : "LF"; }

bool eol_from_name(const std::string &name, Eol &out) {
  if (name == "LF") out = Eol::LF;
  else if (name == "CRLF") out = Eol::CRLF;
  else if (name == "CR") out = Eol::CR;
  else return false;
  return true;
}

void append_utf8(std::string &out, unsigned cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | cp >> 6);
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | cp >> 12);
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | cp >> 18);
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

unsigned next_utf8(const char *s, size_t len, size_t &i) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) {
    ++i;
    return c;
  }
  int n = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
  if (n < 0 || i + size_t(n) >= len) {
    ++i;
    return 0xFFFD;
  }
  unsigned cp = c & (0x3F >> n);
  for (int k = 1; k <= n; ++k) {
    unsigned char d = (unsigned char)s[i + k];
    if ((d & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    cp = cp << 6 | (d & 0x3F);
  }
  // Overlong forms and surrogates are invalid.
  if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++i;
    return 0xFFFD;
  }
  i += n + 1;
  return cp;
}

bool is_valid_utf8(const char *data, size_t len) {
  const unsigned char *s = (const unsigned char *)data;
  size_t i = 0;
  while (i < len) {
    // Fast path for ASCII runs.
    while (i + 8 <= len) {
      uint64_t v;
      memcpy(&v, s + i, 8);
      if (v & 0x8080808080808080ULL) break;
      i += 8;
    }
    if (i >= len) break;
    unsigned char c = s[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t before = i;
    unsigned cp = next_utf8(data, len, i);
    if (cp == 0xFFFD) {
      // A literal U+FFFD (EF BF BD) is fine.
      if (!(i - before == 3 && s[before] == 0xEF && s[before + 1] == 0xBF && s[before + 2] == 0xBD))
        return false;
    }
  }
  return true;
}

Encoding detect_encoding(const std::string &raw) {
  if (has_prefix(raw, "\xEF\xBB\xBF", 3)) return Encoding::Utf8Bom;
  if (has_prefix(raw, "\xFF\xFE", 2)) return Encoding::Utf16LE;
  if (has_prefix(raw, "\xFE\xFF", 2)) return Encoding::Utf16BE;
  if (is_valid_utf8(raw.data(), raw.size())) return Encoding::Utf8;
  for (unsigned char c : raw)
    if (cp1252_defined(c)) return Encoding::Windows1252;
  return Encoding::Latin1;
}

Eol detect_eol(const std::string &text, bool *mixed) {
  size_t lf = 0, crlf = 0, cr = 0;
  const char *s = text.data();
  size_t n = text.size();
  const char *p = s, *end = s + n;
  // Each scan looks for the next '\n' and the next '\r' only when the
  // previous hit was passed, so the whole text is read about twice.
  const char *next_lf = static_cast<const char *>(memchr(p, '\n', n));
  const char *next_cr = static_cast<const char *>(memchr(p, '\r', n));
  while (next_lf || next_cr) {
    if (next_lf && (!next_cr || next_lf < next_cr)) {
      ++lf;
      p = next_lf + 1;
    } else if (next_cr + 1 < end && next_cr[1] == '\n') {
      ++crlf;
      p = next_cr + 2;
    } else {
      ++cr;
      p = next_cr + 1;
    }
    if (next_lf && next_lf < p) next_lf = p < end ? static_cast<const char *>(memchr(p, '\n', size_t(end - p))) : nullptr;
    if (next_cr && next_cr < p) next_cr = p < end ? static_cast<const char *>(memchr(p, '\r', size_t(end - p))) : nullptr;
  }
  if (mixed) *mixed = (lf != 0) + (crlf != 0) + (cr != 0) > 1;
  if (crlf > lf && crlf >= cr) return Eol::CRLF;
  if (cr > lf && cr > crlf) return Eol::CR;
  return Eol::LF;
}

std::string normalize_eol(const std::string &text) {
  if (text.find('\r') == std::string::npos) return text;
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\r') {
      out += '\n';
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else {
      out += c;
    }
  }
  return out;
}

bool looks_binary(const std::string &raw) {
  if (has_prefix(raw, "\xFF\xFE", 2) || has_prefix(raw, "\xFE\xFF", 2)) return false;
  size_t n = raw.size() < 65536 ? raw.size() : 65536;
  return memchr(raw.data(), 0, n) != nullptr;
}

bool decode_as(const std::string &raw, Encoding enc, Decoded &out) {
  std::string text;
  switch (enc) {
    case Encoding::Utf8:
    case Encoding::Utf8Bom: {
      size_t start = has_prefix(raw, "\xEF\xBB\xBF", 3) ? 3 : 0;
      if (!is_valid_utf8(raw.data() + start, raw.size() - start)) return false;
      text.assign(raw, start, std::string::npos);
      break;
    }
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
      bool little = enc == Encoding::Utf16LE;
      size_t start = (little && has_prefix(raw, "\xFF\xFE", 2)) || (!little && has_prefix(raw, "\xFE\xFF", 2)) ? 2 : 0;
      if (!decode_utf16(raw, start, little, text)) return false;
      break;
    }
    case Encoding::Latin1:
    case Encoding::Windows1252:
      text.reserve(raw.size() + raw.size() / 8);
      for (unsigned char c : raw) {
        if (c < 0x80) text += char(c);
        else if (enc == Encoding::Windows1252 && c < 0xA0) append_utf8(text, kCp1252[c - 0x80]);
        else append_utf8(text, c);
      }
      break;
  }
  out.eol = detect_eol(text, &out.mixed_eol);
  out.text = normalize_eol(text);
  out.encoding = enc;
  out.binary = false;
  return true;
}

Decoded decode(const std::string &raw) {
  Decoded d;
  d.binary = looks_binary(raw);
  Encoding enc = detect_encoding(raw);
  if (!decode_as(raw, enc, d)) decode_as(raw, Encoding::Latin1, d);
  d.binary = looks_binary(raw);
  return d;
}

bool encode(const std::string &text, Encoding enc, Eol eol, std::string &out, size_t *bad_offset) {
  out.clear();
  out.reserve(text.size() + text.size() / 16 + 4);
  const char *nl = eol == Eol::CRLF ? "\r\n" : eol == Eol::CR ? "\r" : "\n";
  bool utf16 = enc == Encoding::Utf16LE || enc == Encoding::Utf16BE;
  bool little = enc == Encoding::Utf16LE;
  if (enc == Encoding::Utf8Bom) out += "\xEF\xBB\xBF";
  if (utf16) put16(out, 0xFEFF, little);
  if (enc == Encoding::Utf8 || enc == Encoding::Utf8Bom) {
    if (eol == Eol::LF) {
      out += text;
      return true;
    }
    for (char c : text) {
      if (c == '\n') out += nl;
      else out += c;
    }
    return true;
  }
  size_t i = 0, n = text.size();
  while (i < n) {
    size_t at = i;
    unsigned cp = next_utf8(text.data(), n, i);
    if (cp == '\n') {
      for (const char *p = nl; *p; ++p) {
        if (utf16) put16(out, (unsigned char)*p, little);
        else out += *p;
      }
      continue;
    }
    if (utf16) {
      if (cp >= 0x10000) {
        cp -= 0x10000;
        put16(out, 0xD800 + (cp >> 10), little);
        put16(out, 0xDC00 + (cp & 0x3FF), little);
      } else {
        put16(out, cp, little);
      }
      continue;
    }
    // Single byte encodings.
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF) || (enc == Encoding::Latin1 && cp <= 0xFF)) {
      out += char(cp);
      continue;
    }
    bool found = false;
    if (enc == Encoding::Windows1252) {
      for (int k = 0; k < 32; ++k)
        if (kCp1252[k] == cp) {
          out += char(0x80 + k);
          found = true;
          break;
        }
    }
    if (!found) {
      if (bad_offset) *bad_offset = at;
      return false;
    }
  }
  return true;
}

}  // namespace niny
