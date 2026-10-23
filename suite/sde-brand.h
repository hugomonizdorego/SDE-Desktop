/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 * GTK startup integration for SDE's native upstream-derived applications.
 * Kept GPLv2+ so it is compatible with the original application licenses.
 */
#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* sudo liria keeps X11 credentials, but never the user's D-Bus connection or
 * preferences. No setuid binary and no change to the X server access list. */
static void sde_liria_root(int argc, char **argv) {
#if SDE_NATIVE_LIRIA
  if (geteuid() != 0 || !g_getenv("SUDO_UID") || g_getenv("SDE_LIRIA_ROOT_BUS")) return;
  const char *base = strrchr(argv[0], '/');
  if (g_strcmp0(base ? base + 1 : argv[0], "liria") != 0) return;
  struct passwd *root = getpwuid(0);
  if (!root || !root->pw_dir) { g_printerr("Liria: root home is unavailable\n"); exit(1); }
  g_setenv("HOME", root->pw_dir, TRUE);
  const char *vars[] = {"XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
                        "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", NULL};
  for (int i = 0; vars[i]; ++i) g_unsetenv(vars[i]);
  g_setenv("SDE_LIRIA_ROOT_BUS", "1", TRUE);
  char **command = g_new0(char *, argc + 4);
  command[0] = (char *)"/usr/bin/dbus-run-session";
  command[1] = (char *)"--";
  command[2] = (char *)"/usr/bin/liria";
  for (int i = 1; i < argc; ++i) command[i + 2] = argv[i];
  execv(command[0], command);
  g_printerr("Liria: could not start a private administrator D-Bus session\n");
  exit(1);
#else
  (void)argc; (void)argv;
#endif
}

static void sde_read_key(GKeyFile *key, const char *group, const char *name, gchar **out) {
  gchar *value = g_key_file_get_string(key, group, name, NULL);
  if (value) { g_free(*out); *out = g_strstrip(value); }
}

static void sde_read_theme(const gchar *file, gchar **theme, gboolean *system, gchar **style,
                           gchar **accent, gchar **accent_name) {
  GKeyFile *key = g_key_file_new();
  if (g_key_file_load_from_file(key, file, G_KEY_FILE_NONE, NULL)) {
    sde_read_key(key, "appearance", "theme", theme);
    sde_read_key(key, "appearance", "style", style);
    sde_read_key(key, "appearance", "accent", accent);
    sde_read_key(key, "appearance", "accent_name", accent_name);
    gchar *value = g_key_file_get_string(key, "suite", "gtk_theme", NULL);
    if (value) { *system = g_str_equal(value, "system"); g_free(value); }
  }
  g_key_file_unref(key);
}

/* Accent label of the theme name, as sde::theme_choice(): accent_name, or
 * the swatch colors of alpha15 and the current families, else Blue. */
static const char *sde_accent_label(const gchar *accent, const gchar *name) {
  static const char *const names[][2] = {
    {"blue", "Blue"}, {"teal", "Teal"}, {"green", "Green"}, {"yellow", "Yellow"}, {"orange", "Orange"},
    {"red", "Red"}, {"pink", "Pink"}, {"purple", "Purple"}, {"graphite", "Graphite"}};
  static const char *const colors[][2] = {
    {"#2672BE", "Blue"}, {"#2F6FDB", "Blue"}, {"#3684DD", "Blue"}, {"#2777FF", "Blue"},
    {"#167D9A", "Teal"}, {"#009688", "Teal"}, {"#0099A1", "Teal"},
    {"#277A4C", "Green"}, {"#4CAF50", "Green"}, {"#06A284", "Green"},
    {"#FBC02D", "Yellow"}, {"#FFD86E", "Yellow"},
    {"#D9682B", "Orange"}, {"#F57C00", "Orange"}, {"#FD7D00", "Orange"},
    {"#E53935", "Red"}, {"#EC0101", "Red"},
    {"#C2407A", "Pink"}, {"#EC407A", "Pink"}, {"#CD0245", "Pink"},
    {"#7A4FD1", "Purple"}, {"#AB47BC", "Purple"}, {"#962AC3", "Purple"},
    {"#5A6472", "Graphite"}, {"#333333", "Graphite"}, {"#737680", "Graphite"}};
  for (guint i = 0; i < G_N_ELEMENTS(names); ++i)
    if (g_strcmp0(name, names[i][0]) == 0) return names[i][1];
  for (guint i = 0; i < G_N_ELEMENTS(colors); ++i)
    if (accent && g_ascii_strcasecmp(accent, colors[i][0]) == 0) return colors[i][1];
  return "Blue";
}

/* SDE themes (SDE-Graphite-Light-Blue, ...) are used when installed. */
static gboolean sde_theme_installed(const char *name) {
  gchar *file = g_build_filename(g_get_user_data_dir(), "themes", name, "gtk-3.0", "gtk.css", NULL);
  gboolean found = g_file_test(file, G_FILE_TEST_EXISTS);
  g_free(file);
  for (const gchar *const *d = g_get_system_data_dirs(); !found && d && *d; ++d) {
    file = g_build_filename(*d, "themes", name, "gtk-3.0", "gtk.css", NULL);
    found = g_file_test(file, G_FILE_TEST_EXISTS);
    g_free(file);
  }
  return found;
}

static void __attribute__((constructor)) sde_native_theme(int argc, char **argv, char **envp) {
  (void)envp;
  if (argc == 2) {
    if (g_str_equal(argv[1], "--version")) {
      g_print("%s %s\n", SDE_NATIVE_NAME, SDE_NATIVE_VERSION);
      exit(0);
    }
  }
  sde_liria_root(argc, argv);
  if (g_getenv("GTK_THEME")) return;
  /* In an SDE session XSETTINGS carries the theme, so windows follow
   * changes made in the Control Center at once; GTK_THEME would pin it. */
  if (g_getenv("SDE_SESSION_VERSION") && g_getenv("DISPLAY")) return;
  gchar *theme = g_strdup("light"), *style = g_strdup("graphite"), *accent = g_strdup("");
  gchar *accent_name = g_strdup("");
  gboolean system = FALSE;
  const gchar *const *dirs = g_get_system_config_dirs();
  guint count = g_strv_length((gchar **)dirs);
  while (count) {
    gchar *file = g_build_filename(dirs[--count], "sde", "sde.conf", NULL);
    sde_read_theme(file, &theme, &system, &style, &accent, &accent_name); g_free(file);
  }
  gchar *file = g_build_filename(g_get_user_config_dir(), "sde", "sde.conf", NULL);
  sde_read_theme(file, &theme, &system, &style, &accent, &accent_name); g_free(file);
  if (!system) {
    /* Same mapping as sde::theme_choice(): sde -> Graphite, aero -> Fluent,
     * liquid-glass -> Glass; "auto" uses the light variant here. */
    const char *family = g_str_equal(style, "fluent") || g_str_equal(style, "aero") ? "Fluent"
                         : g_str_equal(style, "glass") || g_str_equal(style, "liquid-glass") ? "Glass"
                         : "Graphite";
    gboolean dark = g_str_equal(theme, "dark");
    gchar *name = g_strdup_printf("SDE-%s-%s-%s", family, dark ? "Dark" : "Light",
                                  sde_accent_label(accent, accent_name));
    gchar *value = sde_theme_installed(name) ? g_strdup(name) : g_strdup(dark ? "Adwaita:dark" : "Adwaita");
    g_setenv("GTK_THEME", value, FALSE);
    g_free(value);
    g_free(name);
  }
  g_free(style);
  g_free(theme);
  g_free(accent);
  g_free(accent_name);
}
