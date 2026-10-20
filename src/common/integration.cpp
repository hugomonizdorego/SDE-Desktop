// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration.h"

#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>

#include "build_config.h"
#include "desktop_entry.h"
#include "icons.h"
#include "suite.h"
#include "theme.h"
#include "util.h"
namespace sde {
void sync_gtk_settings(const Config &cfg) {
  int dpi = int(std::lround(96 * Theme::from_config(cfg).scale * 1024));
  bool dark = cfg.get("appearance", "theme", "light") == "dark";
  bool system = cfg.get("suite", "gtk_theme", "auto") == "system";
  std::string font =
      cfg.get("appearance", "font", "Sans") + " " + cfg.get("appearance", "font_size", "10");
  std::string icons = cfg.get("appearance", "icon_theme", "auto");
  if (icons == "auto") {
    IconLoader::set_theme("auto");
    icons = IconLoader::theme();
  }
  for (const char *version : {"gtk-3.0", "gtk-4.0"}) {
    std::string dir = config_home() + "/" + version;
    make_dirs(dir);
    GKeyFile *file = g_key_file_new();
    g_key_file_load_from_file(file, (dir + "/settings.ini").c_str(), G_KEY_FILE_KEEP_COMMENTS,
                              nullptr);
    if (!system) {
      g_key_file_set_string(file, "Settings", "gtk-theme-name", "Adwaita");
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
  std::string text = "Net/ThemeName " + quote(dark ? "Adwaita-dark" : "Adwaita") + "\n";
  text += "Net/IconThemeName " + quote(icons) + "\nGtk/FontName " + quote(font) + "\n";
  if (system) text = "Net/IconThemeName " + quote(icons) + "\nGtk/FontName " + quote(font) + "\n";
  text += "Xft/DPI " + std::to_string(dpi) + "\n";
  make_dirs(config_home() + "/sde");
  write_file_atomic(config_home() + "/sde/xsettingsd.conf", text);
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  GSettingsSchema *schema =
      source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE)
             : nullptr;
  if (schema) {
    GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
    if (g_settings_schema_has_key(schema, "icon-theme") &&
        g_settings_is_writable(settings, "icon-theme"))
      g_settings_set_string(settings, "icon-theme", icons.c_str());
    if (!system && g_settings_schema_has_key(schema, "color-scheme") &&
        g_settings_is_writable(settings, "color-scheme"))
      g_settings_set_string(settings, "color-scheme", dark ? "prefer-dark" : "prefer-light");
    g_object_unref(settings);
    g_settings_schema_unref(schema);
  }
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
