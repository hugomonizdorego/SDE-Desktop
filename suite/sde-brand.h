/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 * GTK startup integration for SDE's native upstream-derived applications.
 * Kept GPLv2+ so it is compatible with the original application licenses.
 */
#include <glib.h>

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

static void __attribute__((constructor)) sde_native_theme(void) {
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
