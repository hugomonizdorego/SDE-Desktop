/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 DebION Dev-Team
 *
 * Liria SDE preferences and widget-free decisions (see liria-sde-model.h).
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gio/gio.h>

#include "liria-sde-model.h"

/* ------------------------------------------------------------ preferences */

gchar *
liria_sde_prefs_path (void)
{
	return g_build_filename (g_get_user_config_dir (), "sde", "liria.conf", NULL);
}

static GKeyFile *
prefs_load (void)
{
	GKeyFile *key = g_key_file_new ();
	gchar *path = liria_sde_prefs_path ();

	g_key_file_load_from_file (key, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_free (path);
	return key;
}

static gboolean
prefs_save (GKeyFile *key)
{
	gchar *path = liria_sde_prefs_path (), *dir = g_path_get_dirname (path);
	GError *error = NULL;
	gboolean ok;

	g_mkdir_with_parents (dir, 0700);
	ok = g_key_file_save_to_file (key, path, &error);
	if (!ok) {
		g_warning ("Liria: %s", error->message);
		g_error_free (error);
	}
	g_free (dir);
	g_free (path);
	return ok;
}

gchar *
liria_sde_prefs_get_string (const gchar *group, const gchar *key, const gchar *fallback)
{
	GKeyFile *file = prefs_load ();
	gchar *value = g_key_file_get_string (file, group, key, NULL);

	g_key_file_unref (file);
	if (value)
		return g_strstrip (value);
	return g_strdup (fallback);
}

gboolean
liria_sde_prefs_set_string (const gchar *group, const gchar *key, const gchar *value)
{
	GKeyFile *file = prefs_load ();
	gboolean ok;

	g_key_file_set_string (file, group, key, value ? value : "");
	ok = prefs_save (file);
	g_key_file_unref (file);
	return ok;
}

gboolean
liria_sde_prefs_get_bool (const gchar *group, const gchar *key, gboolean fallback)
{
	GKeyFile *file = prefs_load ();
	GError *error = NULL;
	gboolean value = g_key_file_get_boolean (file, group, key, &error);

	g_key_file_unref (file);
	if (error) {
		g_error_free (error);
		return fallback;
	}
	return value;
}

gboolean
liria_sde_prefs_set_bool (const gchar *group, const gchar *key, gboolean value)
{
	GKeyFile *file = prefs_load ();
	gboolean ok;

	g_key_file_set_boolean (file, group, key, value);
	ok = prefs_save (file);
	g_key_file_unref (file);
	return ok;
}

gboolean
liria_sde_flags_contain (const gchar *list, const gchar *name)
{
	gchar **names;
	gboolean found = FALSE;

	if (!list || !name || !*name)
		return FALSE;
	names = g_strsplit (list, ";", -1);
	for (guint i = 0; names[i] && !found; i++)
		found = g_strcmp0 (g_strstrip (names[i]), name) == 0;
	g_strfreev (names);
	return found;
}

/* The list with name added (on) or removed, without duplicates or empty names. */
gchar *
liria_sde_flags_with (const gchar *list, const gchar *name, gboolean on)
{
	gchar **names = g_strsplit (list ? list : "", ";", -1);
	GString *out = g_string_new (NULL);

	for (guint i = 0; names[i]; i++) {
		gchar *item = g_strstrip (names[i]);

		if (!*item || g_strcmp0 (item, name) == 0 || liria_sde_flags_contain (out->str, item))
			continue;
		if (out->len)
			g_string_append_c (out, ';');
		g_string_append (out, item);
	}
	if (on && name && *name) {
		if (out->len)
			g_string_append_c (out, ';');
		g_string_append (out, name);
	}
	g_strfreev (names);
	return g_string_free (out, FALSE);
}

gboolean
liria_sde_prefs_has_flag (const gchar *group, const gchar *key, const gchar *name)
{
	gchar *list = liria_sde_prefs_get_string (group, key, "");
	gboolean found = liria_sde_flags_contain (list, name);

	g_free (list);
	return found;
}

gboolean
liria_sde_prefs_set_flag (const gchar *group, const gchar *key, const gchar *name, gboolean on)
{
	gchar *list = liria_sde_prefs_get_string (group, key, "");
	gchar *changed = liria_sde_flags_with (list, name, on);
	gboolean ok = TRUE;

	if (g_strcmp0 (list, changed) != 0)
		ok = liria_sde_prefs_set_string (group, key, changed);
	g_free (changed);
	g_free (list);
	return ok;
}

/* ----------------------------------------------------------------- ribbon */

LiriaSdeRibbonMode
liria_sde_ribbon_mode_parse (const gchar *text)
{
	if (g_strcmp0 (text, "collapsed") == 0 || g_strcmp0 (text, "minimized") == 0)
		return LIRIA_SDE_RIBBON_COLLAPSED;
	if (g_strcmp0 (text, "off") == 0 || g_strcmp0 (text, "false") == 0 || g_strcmp0 (text, "classic") == 0)
		return LIRIA_SDE_RIBBON_OFF;
	return LIRIA_SDE_RIBBON_FULL;
}

const gchar *
liria_sde_ribbon_mode_name (LiriaSdeRibbonMode mode)
{
	switch (mode) {
	case LIRIA_SDE_RIBBON_COLLAPSED:
		return "collapsed";
	case LIRIA_SDE_RIBBON_OFF:
		return "off";
	default:
		return "full";
	}
}

LiriaSdeRibbonMode
liria_sde_ribbon_mode_toggle (LiriaSdeRibbonMode mode)
{
	return mode == LIRIA_SDE_RIBBON_FULL ? LIRIA_SDE_RIBBON_COLLAPSED : LIRIA_SDE_RIBBON_FULL;
}

LiriaSdeRibbonMode
liria_sde_ribbon_mode_load (void)
{
	gchar *text = liria_sde_prefs_get_string ("ribbon", "mode", "full");
	LiriaSdeRibbonMode mode = liria_sde_ribbon_mode_parse (text);

	g_free (text);
	return mode;
}

gboolean
liria_sde_ribbon_mode_save (LiriaSdeRibbonMode mode)
{
	return liria_sde_prefs_set_string ("ribbon", "mode", liria_sde_ribbon_mode_name (mode));
}

guint
liria_sde_selection_commands (const LiriaSdeSelection *s)
{
	guint commands = LIRIA_SDE_CMD_PIN;

	if (s->count > 0)
		commands |= LIRIA_SDE_CMD_COPY | LIRIA_SDE_CMD_OPEN | LIRIA_SDE_CMD_COMPRESS;
	if (s->count > 0 && s->writable)
		commands |= LIRIA_SDE_CMD_CUT;
	if (s->count == 1 && s->writable)
		commands |= LIRIA_SDE_CMD_RENAME;
	if (s->writable)
		commands |= LIRIA_SDE_CMD_NEW;
	if (s->writable && s->clipboard)
		commands |= LIRIA_SDE_CMD_PASTE;
	if (s->count > 0 && s->archives == s->count)
		commands |= LIRIA_SDE_CMD_EXTRACT;
	return commands;
}

/* --------------------------------------------------------------- the Home view */

typedef struct {
	gchar *uri;
	guint count;
	guint first;
} Folder;

static gint
folder_order (gconstpointer a, gconstpointer b)
{
	const Folder *fa = *(Folder * const *) a, *fb = *(Folder * const *) b;

	if (fa->count != fb->count)
		return fa->count > fb->count ? -1 : 1;
	return fa->first < fb->first ? -1 : (fa->first > fb->first ? 1 : 0);
}

static void
folder_free (Folder *f)
{
	g_free (f->uri);
	g_free (f);
}

static gboolean
uri_listed (const gchar *const *uris, const gchar *uri)
{
	for (guint i = 0; uris && uris[i]; i++) {
		GFile *a = g_file_new_for_uri (uris[i]), *b = g_file_new_for_uri (uri);
		gboolean same = g_file_equal (a, b);

		g_object_unref (a);
		g_object_unref (b);
		if (same)
			return TRUE;
	}
	return FALSE;
}

GPtrArray *
liria_sde_home_frequent_folders (const gchar *const *recent_uris, const gchar *const *pinned_uris,
                                 const gchar *home_uri, guint max)
{
	GPtrArray *folders = g_ptr_array_new_with_free_func ((GDestroyNotify) folder_free);
	GPtrArray *result = g_ptr_array_new_with_free_func (g_free);
	GHashTable *index = g_hash_table_new (g_str_hash, g_str_equal);
	GFile *home = home_uri ? g_file_new_for_uri (home_uri) : NULL;

	for (guint i = 0; recent_uris && recent_uris[i]; i++) {
		GFile *file, *parent;
		gchar *uri;
		Folder *folder;

		if (!g_str_has_prefix (recent_uris[i], "file://"))
			continue;
		file = g_file_new_for_uri (recent_uris[i]);
		parent = g_file_get_parent (file);
		g_object_unref (file);
		if (!parent)
			continue;
		if ((home && g_file_equal (parent, home)) || !g_file_has_parent (parent, NULL)) {
			g_object_unref (parent);
			continue;
		}
		uri = g_file_get_uri (parent);
		g_object_unref (parent);
		folder = g_hash_table_lookup (index, uri);
		if (folder) {
			folder->count++;
			g_free (uri);
			continue;
		}
		if (uri_listed (pinned_uris, uri)) {
			g_free (uri);
			continue;
		}
		folder = g_new0 (Folder, 1);
		folder->uri = uri;
		folder->count = 1;
		folder->first = i;
		g_ptr_array_add (folders, folder);
		g_hash_table_insert (index, folder->uri, folder);
	}
	g_ptr_array_sort (folders, folder_order);
	for (guint i = 0; i < folders->len && i < max; i++)
		g_ptr_array_add (result, g_strdup (((Folder *) folders->pdata[i])->uri));
	g_hash_table_unref (index);
	g_ptr_array_unref (folders);
	if (home)
		g_object_unref (home);
	return result;
}

guint
liria_sde_home_columns (gint width, gint min_width, gint spacing, guint max)
{
	guint columns;

	if (min_width <= 0 || width <= 0)
		return 1;
	columns = (guint) ((width + MAX (spacing, 0)) / (min_width + MAX (spacing, 0)));
	return CLAMP (columns, 1, MAX (max, 1));
}

/* ----------------------------------------------------------- drag and drop */

LiriaSdeDropResult
liria_sde_drop_decide (LiriaSdeDropTarget target, gboolean from_pin_card, guint n_uris)
{
	if (from_pin_card) {
		/* A pin card dragged within Quick access changes its place;
		 * dropped on a folder or drive it is copied there like a file. */
		if (target == LIRIA_SDE_DROP_DRIVE_CARD)
			return LIRIA_SDE_DROP_FILES;
		return target == LIRIA_SDE_DROP_PINNED_HEADING ? LIRIA_SDE_DROP_NONE : LIRIA_SDE_DROP_REORDER;
	}
	if (n_uris == 0)
		return LIRIA_SDE_DROP_NONE;
	switch (target) {
	case LIRIA_SDE_DROP_FOLDER_CARD:
	case LIRIA_SDE_DROP_DRIVE_CARD:
		return LIRIA_SDE_DROP_FILES;
	case LIRIA_SDE_DROP_AREA:
	case LIRIA_SDE_DROP_FILE_CARD:
	case LIRIA_SDE_DROP_PINNED_HEADING:
		return LIRIA_SDE_DROP_PIN;
	}
	return LIRIA_SDE_DROP_NONE;
}

gint
liria_sde_drop_pin_index (gint index, gint x, gint card_x, gint card_width)
{
	if (index < 0)
		return -1;
	return x > card_x + card_width / 2 ? index + 1 : index;
}
