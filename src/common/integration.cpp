// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration.h"

#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <signal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "build_config.h"
#include "desktop_entry.h"
#include "spawn.h"
#include "suite.h"
#include "theme.h"
#include "util.h"
namespace sde {
namespace {
bool aero_installed() {
  std::vector<std::string> dirs = {std::string(g_get_user_data_dir())};
  for (const gchar *const *d = g_get_system_data_dirs(); d && *d; ++d) dirs.push_back(*d);
  dirs.push_back(std::string(SDE_DATADIR) + "/..");
  for (const auto &d : dirs)
    if (file_exists(d + "/themes/SDE-Aero/gtk-3.0/gtk.css")) return true;
  return false;
}

std::string rgba(const Color &c, double alpha) {
  char buf[64];
  snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %.2f)", int(std::lround(c.r * 255)),
           int(std::lround(c.g * 255)), int(std::lround(c.b * 255)), alpha);
  return buf;
}

const char *kCssBegin = "/* SDE Aero: managed by SDE Settings - begin */";
const char *kCssEnd = "/* SDE Aero: managed by SDE Settings - end */";

// Keeps the user's own CSS and replaces only SDE's marked block.
void write_user_css(const std::string &path, const std::string &block) {
  std::string text;
  bool exists = read_file(path, text);
  std::string rest = text;
  size_t a = rest.find(kCssBegin), b = rest.find(kCssEnd);
  if (a != std::string::npos && b != std::string::npos && b > a) {
    size_t end = b + std::string(kCssEnd).size();
    if (end < rest.size() && rest[end] == '\n') ++end;
    rest.erase(a, end - a);
  }
  std::string out = block.empty() ? rest : std::string(kCssBegin) + "\n" + block + kCssEnd + "\n" + rest;
  if (out == text || (!exists && out.empty())) return;
  write_file_atomic(path, out);
}
}  // namespace

std::string gtk_theme_name(const Config &cfg) {
  if (cfg.get_bool("aero", "enabled", true) && aero_installed()) return "SDE-Aero";
  return "Adwaita";
}

static bool theme_is_aero(const Config &cfg) { return gtk_theme_name(cfg) == "SDE-Aero"; }

void sync_gtk_settings(const Config &cfg) {
  int dpi = int(std::lround(96 * Theme::from_config(cfg).scale * 1024));
  bool dark = cfg.get("appearance", "theme", "light") == "dark";
  bool system = cfg.get("suite", "gtk_theme", "auto") == "system";
  std::string font =
      cfg.get("appearance", "font", "Sans") + " " + cfg.get("appearance", "font_size", "10");
  std::string icons = cfg.get("appearance", "icon_theme", "auto");
  if (icons == "auto") icons = "Adwaita";
  for (const char *version : {"gtk-3.0", "gtk-4.0"}) {
    std::string dir = config_home() + "/" + version;
    make_dirs(dir);
    GKeyFile *file = g_key_file_new();
    g_key_file_load_from_file(file, (dir + "/settings.ini").c_str(), G_KEY_FILE_KEEP_COMMENTS,
                              nullptr);
    if (!system) {
      g_key_file_set_string(file, "Settings", "gtk-theme-name", gtk_theme_name(cfg).c_str());
      g_key_file_set_boolean(file, "Settings", "gtk-application-prefer-dark-theme", dark);
    }
    g_key_file_set_integer(file, "Settings", "gtk-xft-dpi", dpi);
    g_key_file_set_string(file, "Settings", "gtk-font-name", font.c_str());
    g_key_file_set_string(file, "Settings", "gtk-icon-theme-name", icons.c_str());
    gsize size = 0;
    gchar *text = g_key_file_to_data(file, &size, nullptr);
    if (text) {
      write_file_atomic(dir + "/settings.ini", std::string(text, size));
      g_free(text);
    }
    g_key_file_unref(file);
  }
  // SDE Aero reads the accent and the glass opacity from the user's CSS.
  std::string block;
  if (!system && theme_is_aero(cfg)) {
    Theme t = Theme::from_config(cfg);
    Color accent = dark ? t.accent.mix(Color::hex(0xFFFFFF), 0.15) : t.accent;
    Color glass = dark ? Color::hex(0x1E2530) : Color::hex(0xF8FBFE);
    double alpha = std::max(0.70, 1.0 - t.transparency * 0.6 / 100.0);
    if (!t.glass) alpha = 1.0;
    block = "@define-color sde_accent " + accent.to_hex() + ";\n" +
            "@define-color sde_glass_bg " + rgba(glass, alpha) + ";\n";
  }
  for (const char *version : {"gtk-3.0", "gtk-4.0"})
    write_user_css(config_home() + "/" + version + "/gtk.css", block);
  // X11 GTK clients use XSETTINGS; the session supervises one xsettingsd.
  // GSettings also exposes the dark preference to GTK4/libadwaita and portals.
  auto quote = [](std::string value) {
    std::string out = "\"";
    for (char c : value) {
      if (c == '\\' || c == '"') out += '\\';
      if (c != '\n' && c != '\r') out += c;
    }
    return out + "\"";
  };
  std::string theme = gtk_theme_name(cfg);
  std::string text = "Net/ThemeName " + quote(theme + (dark ? "-dark" : "")) + "\n";
  if (theme == "SDE-Aero") text = "Net/ThemeName " + quote(dark ? "SDE-Aero-Dark" : "SDE-Aero") + "\n";
  text += "Net/IconThemeName " + quote(icons) + "\nGtk/FontName " + quote(font) + "\n";
  if (system) text = "Net/IconThemeName " + quote(icons) + "\nGtk/FontName " + quote(font) + "\n";
  text += "Xft/DPI " + std::to_string(dpi) + "\n";
  make_dirs(config_home() + "/sde");
  std::string old_text, xsettings = config_home() + "/sde/xsettingsd.conf";
  if (!read_file(xsettings, old_text) || old_text != text) {
    write_file_atomic(xsettings, text);
    // xsettingsd re-reads its file on SIGHUP, so open GTK windows follow the
    // new theme, icons and fonts immediately instead of at the next login.
    if (!env("DISPLAY").empty())
      spawn_detached({"pkill", "-HUP", "-u", std::to_string(getuid()), "-x", "xsettingsd"});
  }
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  GSettingsSchema *schema =
      source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE)
             : nullptr;
  if (schema) {
    GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
    if (!system && g_settings_schema_has_key(schema, "color-scheme") &&
        g_settings_is_writable(settings, "color-scheme"))
      g_settings_set_string(settings, "color-scheme", dark ? "prefer-dark" : "prefer-light");
    g_object_unref(settings);
    g_settings_schema_unref(schema);
  }
}
static bool has_drm_device() {
  for (const char *dev : {"/dev/dri/renderD128", "/dev/dri/card0", "/dev/dri/card1"})
    if (access(dev, R_OK | W_OK) == 0) return true;
  return false;
}

std::string write_compositor_config(const Config &cfg, bool safe) {
  Theme t = Theme::from_config(cfg);
  bool glass = t.glass, blur = glass && cfg.get_bool("aero", "blur", true);
  bool round = cfg.get_bool("aero", "rounded_corners", true);
  bool motion = cfg.get_bool("aero", "animations", true);
  int strength = std::max(1, std::min(12, cfg.get_int("aero", "blur_strength", 6)));
  double frame = glass ? std::max(0.45, 1.0 - t.transparency * 1.2 / 100.0) : 1.0;
  double menu = glass ? std::max(0.70, 1.0 - t.transparency * 0.6 / 100.0) : 1.0;
  char num[32];
  auto f = [&](double v) {
    snprintf(num, sizeof num, "%.2f", v);
    return std::string(num);
  };
  std::string c = "# Generated by SDE from [aero] in sde.conf. Edits are replaced; copy this\n"
                  "# file to ~/.config/sde/compositor.conf to keep your own configuration.\n";
  // auto: OpenGL needs a DRM device; virtual displays and machines without
  // one get XRender, which picom draws reliably everywhere.
  std::string backend = lower(cfg.get("compositor", "backend", "auto"));
  if (backend != "glx" && backend != "xrender") backend = has_drm_device() ? "glx" : "xrender";
  if (safe) backend = "xrender";
  safe = backend == "xrender";
  c += "backend = \"" + backend + "\";\n";
  c += "vsync = true;\nuse-damage = true;\n";
  c += "shadow = true;\nshadow-radius = 16;\nshadow-opacity = " + f(t.dark ? 0.42 : 0.24) +
       ";\nshadow-offset-x = -14;\nshadow-offset-y = -10;\n";
  c += "shadow-exclude = [\"window_type = 'dock'\", \"window_type = 'desktop'\", "
       "\"class_g = 'SDE-Panel'\", \"_GTK_FRAME_EXTENTS@:c\"];\n";
  if (motion)
    c += "fading = true;\nfade-in-step = 0.07;\nfade-out-step = 0.08;\nfade-delta = 8;\n"
         "no-fading-openclose = false;\n";
  else
    c += "fading = false;\n";
  c += "corner-radius = " + std::to_string(round ? 10 : 0) + ";\n";
  c += "rounded-corners-exclude = [\"window_type = 'dock'\", \"window_type = 'desktop'\", "
       "\"window_type = 'tooltip'\", \"window_type = 'dropdown_menu'\", "
       "\"window_type = 'popup_menu'\", \"window_type = 'menu'\", \"fullscreen\"];\n";
  c += "frame-opacity = " + f(frame) + ";\n";
  c += "detect-client-opacity = true;\n";
  if (blur) {
    if (safe)
      c += "blur: { method = \"gaussian\"; size = " + std::to_string(5 + strength / 2 * 2) +
           "; deviation = " + f(2.0 + strength * 0.5) + "; };\n";
    else
      c += "blur: { method = \"dual_kawase\"; strength = " + std::to_string(strength) + "; };\n";
    c += "blur-background = true;\nblur-background-frame = true;\nblur-background-fixed = false;\n";
    c += "blur-background-exclude = [\"window_type = 'desktop'\", \"_GTK_FRAME_EXTENTS@:c\", "
         "\"class_g = 'Xfce4-screenshooter'\"];\n";
  }
  c += "wintypes: {\n"
       "  tooltip = { fade = true; shadow = false; opacity = " + f(std::max(0.85, menu)) + "; };\n"
       "  dock = { shadow = false; clip-shadow-above = true; };\n"
       "  dnd = { shadow = false; };\n"
       "  popup_menu = { opacity = " + f(menu) + "; };\n"
       "  dropdown_menu = { opacity = " + f(menu) + "; };\n"
       "};\n";
  std::string path = config_home() + (safe ? "/sde/aero-compositor-xrender.conf" : "/sde/aero-compositor.conf");
  make_dirs(config_home() + "/sde");
  std::string old;
  if (!read_file(path, old) || old != c) write_file_atomic(path, c);
  return path;
}

void reload_compositor() {
  // picom re-reads its configuration file on SIGUSR1.
  spawn_detached({"pkill", "-USR1", "-u", std::to_string(getuid()), "-x", "sde-compositor"});
}

void migrate_legacy_suite_shortcuts() {
  const std::string folder = user_dir("DESKTOP") + "/SDE Suite";
  struct stat st {};
  if (lstat(folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
  for (const auto &app : suite_apps()) {
    std::string id = app.id == "calculator" ? "org.debion.Calculator" : app.id;
    DesktopEntry e;
    if (!e.load(std::string(SDE_DATADIR) + "/applications/" + id + ".desktop")) continue;
    std::string dir = folder + "/" + e.menu_category();
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    std::string target = dir + "/" + id + ".desktop", original, copy;
    // Clean up only identical generated launchers, never edited files/symlinks.
    if (lstat(target.c_str(), &st) == 0 && S_ISREG(st.st_mode) && read_file(e.path, original) &&
        read_file(target, copy) && original == copy)
      unlink(target.c_str());
    rmdir(dir.c_str());  // removes empty directories only
  }
  rmdir(folder.c_str());
}
}  // namespace sde
