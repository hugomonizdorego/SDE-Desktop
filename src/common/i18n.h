// SPDX-License-Identifier: GPL-3.0-or-later
// Translations with GNU gettext. Source strings are US English; translators
// add po/<lang>.po files (see po/README.md).
#pragma once

#include <libintl.h>

#include <string>

#define _(s) gettext(s)
#define N_(s) (s)

namespace sde {

// Sets the locale from the environment and binds the "sde" text domain.
// Numbers are always formatted with the C locale.
void init_i18n();

// printf-style formatting into a std::string ("%s", "%d" ...).
std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace sde
