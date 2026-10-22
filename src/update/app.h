// SPDX-License-Identifier: GPL-3.0-or-later
// Software Updater (sde-update) main window.
#pragma once

#include <ctime>
#include <string>

namespace sde {
namespace update {

// Directory with the Python helpers (check.py, transaction.py, ...).
// SDE_UPDATE_HELPERS overrides it for development and UI tests.
std::string helper_dir();
std::string helper(const std::string &name);

// State of the background notifier, shared with the window: Remind Me Later
// sets |remind_until| (the notifier stays quiet until then).
struct NotifierState {
  std::time_t last_check = 0;
  std::string notified;  // signature of the last update set we notified about
  std::time_t remind_until = 0;
};
NotifierState load_notifier_state();
void save_notifier_state(const NotifierState &s);

// Shows the window and runs the FLTK loop. |page| is updates, history,
// sources, maintenance, settings or about (the window opens on the
// updater; the other pages are behind its Settings… button).
int run_app(const std::string &page);

}  // namespace update
}  // namespace sde
