// SPDX-License-Identifier: GPL-3.0-or-later
#include "wm.h"

#include "build_config.h"
#include "desktop_entry.h"
#include "spawn.h"
#include "util.h"

namespace sde {

std::string openbox_config_path() { return config_home() + "/sde/openbox/rc.xml"; }

bool ensure_openbox_config() {
  std::string path = openbox_config_path();
  if (file_exists(path)) return true;
  std::string tmpl;
  if (!read_file(std::string(SDE_DATADIR) + "/openbox/rc.xml", tmpl)) return false;
  return write_file_atomic(path, tmpl);
}

bool set_openbox_theme(bool dark) {
  std::string path = openbox_config_path();
  std::string text;
  if (!ensure_openbox_config() || !read_file(path, text)) return false;
  // The template marks the theme name line so it can be found reliably.
  const std::string open = "<name>", close = "</name>";
  size_t theme = text.find("<theme>");
  if (theme == std::string::npos) return false;
  size_t a = text.find(open, theme), b = text.find(close, theme);
  if (a == std::string::npos || b == std::string::npos || b < a) return false;
  std::string current = text.substr(a + open.size(), b - a - open.size());
  std::string wanted = dark ? "SDE-Dark" : "SDE";
  // Respect a theme the user picked by hand (anything that is not SDE's).
  if (current != "SDE" && current != "SDE-Dark") return false;
  if (current == wanted) return false;
  text.replace(a + open.size(), current.size(), wanted);
  return write_file_atomic(path, text);
}

std::vector<std::string> window_manager_command(const std::string &configured) {
  std::string wm = trim(configured);
  if (wm.empty() || wm == "openbox") {
    ensure_openbox_config();
    return {"openbox", "--config-file", openbox_config_path()};
  }
  return split_command(wm);
}

void reconfigure_window_manager() {
  if (!find_program("openbox").empty()) spawn_detached({"openbox", "--reconfigure"});
}

}  // namespace sde
