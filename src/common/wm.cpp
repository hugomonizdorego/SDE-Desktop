// SPDX-License-Identifier: GPL-3.0-or-later
#include "wm.h"

#include "build_config.h"
#include "desktop_entry.h"
#include "spawn.h"
#include "config.h"
#include "theme.h"
#include "util.h"

namespace sde {

std::string openbox_config_path() { return config_home() + "/sde/openbox/rc.xml"; }

// Additions to the template after 0.1.0~alpha1, merged into existing copies.
static void migrate_openbox_config(const std::string &path) {
  std::string text;
  if (!read_file(path, text)) return;
  bool changed = false;
  const std::string old_lock = "<command>xdg-screensaver lock</command>";
  size_t lock = text.find(old_lock);
  if (lock != std::string::npos) {
    text.replace(lock, old_lock.size(), "<command>sde-lock</command>");
    changed = true;
  }
  if (text.find("<context name=\"Desktop\">") == std::string::npos) {
    size_t end = text.find("</mouse>");
    if (end != std::string::npos) {
      text.insert(end,
                  "  <context name=\"Desktop\">\n"
                  "      <mousebind button=\"Left\" action=\"Press\"><action name=\"Focus\"/></mousebind>\n"
                  "      <mousebind button=\"Right\" action=\"Press\"><action name=\"Focus\"/></mousebind>\n"
                  "    </context>\n  ");
      changed = true;
    }
  }
  if (changed) write_file_atomic(path, text);
}

bool ensure_openbox_config() {
  std::string path = openbox_config_path();
  if (file_exists(path)) {
    migrate_openbox_config(path);
    return true;
  }
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

bool set_openbox_font_size(int points) {
  std::string path = openbox_config_path();
  std::string text;
  if (!ensure_openbox_config() || !read_file(path, text)) return false;
  std::string out;
  size_t pos = 0;
  bool changed = false;
  for (;;) {
    size_t font = text.find("<font place=", pos);
    if (font == std::string::npos) break;
    size_t a = text.find("<size>", font), b = text.find("</size>", font), end = text.find("</font>", font);
    if (a == std::string::npos || b == std::string::npos || end == std::string::npos || b > end) break;
    std::string value = std::to_string(points);
    if (text.substr(a + 6, b - a - 6) != value) changed = true;
    out += text.substr(pos, a + 6 - pos) + value;
    pos = b;
  }
  out += text.substr(pos);
  return changed && write_file_atomic(path, out);
}

bool sync_openbox(const Config &cfg) {
  bool changed = set_openbox_theme(lower(cfg.get("appearance", "theme")) == "dark");
  Theme t = Theme::from_config(cfg);
  int pt = cfg.get_int("appearance", "font_size", 10);
  changed = set_openbox_font_size(int(pt * t.scale + 0.5)) || changed;
  return changed;
}

std::vector<std::string> window_manager_command(const std::string &configured) {
  std::string wm = trim(configured);
  if (wm.empty() || wm == "sde-wm" || wm == "openbox") {
    ensure_openbox_config();
    return {"sde-wm", "--config-file", openbox_config_path()};
  }
  return split_command(wm);
}

void reconfigure_window_manager() {
  if (!find_program("sde-wm").empty()) spawn_detached({"sde-wm", "--reconfigure"});
}

}  // namespace sde
