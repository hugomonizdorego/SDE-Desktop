// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// Tests for the XSETTINGS model of sde-xsettingsd.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "xsettings.h"

using namespace sde::xsettings;

static int failures = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      ++failures;                                                   \
    }                                                               \
  } while (0)

static uint32_t u32(const std::vector<uint8_t> &b, size_t at) {
  uint32_t v;
  memcpy(&v, b.data() + at, 4);
  return v;
}

int main() {
  std::vector<std::string> errors;
  auto s = parse("# SDE\nNet/ThemeName \"SDE-Dark\"\nXft/DPI 98304\n\n"
                 "Gtk/CursorThemeSize 24  # comment\nNet/Color (65535, 0, 32768)\n"
                 "Bad/Value nope\nGtk/FontName \"Noto \\\"Sans\\\" 10\"\n",
                 &errors);
  CHECK(s.size() == 5);
  CHECK(errors.size() == 1 && errors[0].find("line 7") == 0);
  CHECK(s["Net/ThemeName"].type == Setting::STRING && s["Net/ThemeName"].string == "SDE-Dark");
  CHECK(s["Xft/DPI"].type == Setting::INTEGER && s["Xft/DPI"].integer == 98304);
  CHECK(s["Gtk/CursorThemeSize"].integer == 24);
  CHECK(s["Net/Color"].type == Setting::COLOR && s["Net/Color"].color[0] == 65535 &&
        s["Net/Color"].color[2] == 32768 && s["Net/Color"].color[3] == 0xFFFF);
  CHECK(s["Gtk/FontName"].string == "Noto \"Sans\" 10");

  Settings current;
  CHECK(merge(current, s, 0));
  auto changed = parse("Net/ThemeName \"SDE\"\nXft/DPI 98304\n");
  CHECK(merge(current, changed, 7));
  CHECK(current.size() == 2);
  CHECK(current["Net/ThemeName"].serial == 7);  // changed
  CHECK(current["Xft/DPI"].serial == 0);        // unchanged keeps its serial
  CHECK(!merge(current, changed, 8));

  // Property layout: header 12 bytes; each setting padded to 4 bytes.
  Settings one = parse("Xft/DPI 98304\n");
  auto blob = encode(one, 3);
  CHECK(blob.size() == 12 + 4 + 8 + 4 + 4);
  CHECK(u32(blob, 4) == 3 && u32(blob, 8) == 1);
  CHECK(blob[12] == Setting::INTEGER && blob[14] == 7);
  CHECK(memcmp(blob.data() + 16, "Xft/DPI", 7) == 0 && blob[23] == 0);
  CHECK(u32(blob, 28) == 98304);
  auto str = encode(parse("Net/ThemeName \"SDE\"\n"), 1);
  CHECK(str.size() == 12 + 4 + 16 + 4 + 4 + 4);  // name 13 -> 16, value 3 -> 4

  if (failures) return 1;
  printf("xsettings: all checks passed\n");
  return 0;
}
