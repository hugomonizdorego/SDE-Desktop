// SPDX-License-Identifier: GPL-3.0-or-later
#include "wm.h"

#include <FL/Fl.H>

#include <algorithm>
#include <iterator>

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
  // Alpha 15: the iconify animation moves the frame in 20 ms steps on the
  // CPU and delays every minimize; the compositor already fades windows.
  // Migrated once, so a user who turns it back on keeps it.
  const std::string marker = "<!-- sde: animateIconify migrated -->";
  if (text.find(marker) == std::string::npos) {
    const std::string anim = "<animateIconify>yes</animateIconify>";
    size_t at = text.find(anim);
    if (at != std::string::npos) text.replace(at, anim.size(), "<animateIconify>no</animateIconify>");
    size_t root = text.find("<openbox_config");
    size_t close = root == std::string::npos ? std::string::npos : text.find('>', root);
    if (close != std::string::npos) {
      text.insert(close + 1, "\n  " + marker);
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

// Reads (and with |wanted| replaces) the text of the first <tag>...</tag>
// inside <theme>. False when it is missing.
static bool theme_value(std::string &text, const std::string &tag, std::string &current,
                        const std::string *wanted) {
  size_t theme = text.find("<theme>"), end = text.find("</theme>");
  if (theme == std::string::npos || end == std::string::npos) return false;
  const std::string open = "<" + tag + ">", close = "</" + tag + ">";
  size_t a = text.find(open, theme), b = text.find(close, theme);
  if (a == std::string::npos || b == std::string::npos || b < a || b > end) return false;
  current = text.substr(a + open.size(), b - a - open.size());
  if (wanted) text.replace(a + open.size(), current.size(), *wanted);
  return true;
}

bool set_openbox_theme(const std::string &wanted, const std::string &layout) {
  std::string path = openbox_config_path();
  std::string text;
  if (!ensure_openbox_config() || !read_file(path, text)) return false;
  std::string current, before = text;
  if (!theme_value(text, "name", current, nullptr)) return false;
  // Respect a theme the user picked by hand (anything that is not SDE's:
  // SDE, SDE-Dark, SDE-Aero..., SDE-Graphite-Light-Blue, ...).
  if (current == "SDE" || starts_with(current, "SDE-")) theme_value(text, "name", current, &wanted);
  std::string old_layout;
  // The layouts SDE has used; anything else was chosen by hand.
  if (theme_value(text, "titleLayout", old_layout, nullptr) &&
      (old_layout == "NLIMC" || old_layout == "CIML" || old_layout == "CIMLN"))
    theme_value(text, "titleLayout", old_layout, &layout);
  return text != before && write_file_atomic(path, text);
}

bool set_openbox_theme(bool dark, const std::string &style) {
  return set_openbox_theme(theme_dir(style, dark, "blue"),
                           theme_title_layout(lower(style) == "aero" ? "fluent" : lower(style)));
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
  Theme t = Theme::from_config(cfg);
  bool changed = set_openbox_theme(t.gtk_theme, theme_title_layout(t.family));
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

void fit_to_work_area(int &x, int &y, int &w, int &h) {
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh);
  // SDE WM title bar: the scaled title font plus 2 x 8 px padding and the
  // border (build-themes.py), with a little air around the frame.
  const int title = S(26) + 18, border = 2, air = S(12);
  w = std::max(1, std::min(w, sw - 2 * border - 2 * air));
  h = std::max(1, std::min(h, sh - title - border - 2 * air));
  x = sx + (sw - w) / 2;
  y = sy + title + (sh - title - border - h) / 2;
}

}  // namespace sde
