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

static void sde_read_theme(const gchar *file, gchar **theme, gboolean *system, gchar **style, gboolean *glass) {
  GKeyFile *key = g_key_file_new();
  if (g_key_file_load_from_file(key, file, G_KEY_FILE_NONE, NULL)) {
    gchar *value = g_key_file_get_string(key, "appearance", "theme", NULL);
    if (value) { g_free(*theme); *theme = value; }
    value = g_key_file_get_string(key, "appearance", "style", NULL);
    if (value) { g_free(*style); *style = value; }
    value = g_key_file_get_string(key, "suite", "gtk_theme", NULL);
    if (value) { *system = g_str_equal(value, "system"); g_free(value); }
    value = g_key_file_get_string(key, "aero", "enabled", NULL);
    if (value) { *glass = !g_str_equal(value, "false") && !g_str_equal(value, "0"); g_free(value); }
  }
  g_key_file_unref(key);
}

/* SDE themes (SDE, SDE-Aero, SDE-Liquid-Glass) are used when installed. */
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
  gchar *theme = g_strdup("light"), *style = g_strdup("");
  gboolean system = FALSE, glass = TRUE;
  const gchar *const *dirs = g_get_system_config_dirs();
  guint count = g_strv_length((gchar **)dirs);
  while (count) {
    gchar *file = g_build_filename(dirs[--count], "sde", "sde.conf", NULL);
    sde_read_theme(file, &theme, &system, &style, &glass); g_free(file);
  }
  gchar *file = g_build_filename(g_get_user_config_dir(), "sde", "sde.conf", NULL);
  sde_read_theme(file, &theme, &system, &style, &glass); g_free(file);
  if (!system) {
    /* Same mapping as theme_style() in SDE: an alpha10 file without
     * [appearance] style and with [aero] enabled=false means SDE Light/Dark. */
    const char *name = g_str_equal(style, "sde") ? "SDE"
                       : g_str_equal(style, "liquid-glass") ? "SDE-Liquid-Glass"
                       : g_str_equal(style, "aero") ? "SDE-Aero"
                       : glass ? "SDE-Aero" : "SDE";
    if (!sde_theme_installed(name)) name = "Adwaita";
    gchar *value = g_strconcat(name, g_str_equal(theme, "dark") ? ":dark" : "", NULL);
    g_setenv("GTK_THEME", value, FALSE);
    g_free(value);
  }
  g_free(style);
  g_free(theme);
}
