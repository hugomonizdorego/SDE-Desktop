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

static void sde_read_theme(const gchar *file, gchar **theme, gboolean *system) {
  GKeyFile *key = g_key_file_new();
  if (g_key_file_load_from_file(key, file, G_KEY_FILE_NONE, NULL)) {
    gchar *value = g_key_file_get_string(key, "appearance", "theme", NULL);
    if (value) { g_free(*theme); *theme = value; }
    value = g_key_file_get_string(key, "suite", "gtk_theme", NULL);
    if (value) { *system = g_str_equal(value, "system"); g_free(value); }
  }
  g_key_file_unref(key);
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
  gchar *theme = g_strdup("light");
  gboolean system = FALSE;
  const gchar *const *dirs = g_get_system_config_dirs();
  guint count = g_strv_length((gchar **)dirs);
  while (count) {
    gchar *file = g_build_filename(dirs[--count], "sde", "sde.conf", NULL);
    sde_read_theme(file, &theme, &system); g_free(file);
  }
  gchar *file = g_build_filename(g_get_user_config_dir(), "sde", "sde.conf", NULL);
  sde_read_theme(file, &theme, &system); g_free(file);
  if (!system) g_setenv("GTK_THEME", g_str_equal(theme, "dark") ? "Adwaita:dark" : "Adwaita", FALSE);
  g_free(theme);
}
