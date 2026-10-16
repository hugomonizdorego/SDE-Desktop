// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.h"

#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "build_config.h"

namespace sde {

void init_i18n() {
  setlocale(LC_ALL, "");
  setlocale(LC_NUMERIC, "C");
  bindtextdomain("sde", SDE_LOCALEDIR);
  bind_textdomain_codeset("sde", "UTF-8");
  textdomain("sde");
}

std::string format(const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string out;
  if (n > 0) {
    std::vector<char> buf(size_t(n) + 1);
    vsnprintf(buf.data(), buf.size(), fmt, ap2);
    out.assign(buf.data(), size_t(n));
  }
  va_end(ap2);
  return out;
}

}  // namespace sde
