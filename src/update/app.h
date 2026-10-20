// SPDX-License-Identifier: GPL-3.0-or-later
// SDE Update main window.
#pragma once

#include <string>

namespace sde {
namespace update {

// Directory with the Python helpers (check.py, transaction.py, ...).
// SDE_UPDATE_HELPERS overrides it for development and UI tests.
std::string helper_dir();
std::string helper(const std::string &name);

// Shows the window and runs the FLTK loop. |page| is updates, history,
// sources, maintenance, settings or about.
int run_app(const std::string &page);

}  // namespace update
}  // namespace sde
