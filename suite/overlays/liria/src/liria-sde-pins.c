/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Liria Quick access pins.
 *
 * Storage: ~/.config/sde/liria-quick-access (XDG_CONFIG_HOME), a plain UTF-8
 * text file with one pinned location per line, in card order:
 *
 *     # comment
 *     file:///home/ana/Documents
 *     file:///home/ana/Notes/plan.odt<TAB>Plan
 *     sftp://server/srv/share
 *     trash:///
 *
 * A line is a URI (an absolute path or ~/path is accepted too), optionally
 * followed by a TAB and a custom card label. Blank lines and lines starting
 * with '#' are ignored; a URI listed twice keeps its first place. When the
 * file does not exist the defaults are used (Desktop, Documents, Downloads,
 * Pictures, Music, Videos and Trash); "Reset to Defaults" deletes the file.
 * An existing file without entries means the user unpinned everything.
 * Every open Liria window watches the file, so a change shows everywhere.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "liria-sde-pins.h"

LiriaSdePin *
liria_sde_pin_new (const gchar *uri, const gchar *label)
{
	LiriaSdePin *pin = g_new0 (LiriaSdePin, 1);

	pin->uri = g_strdup (uri);
	pin->label = label && *label ? g_strdup (label) : NULL;
	return pin;
}

void
liria_sde_pin_free (LiriaSdePin *pin)
{
	if (!pin)
		return;
	g_free (pin->uri);
	g_free (pin->label);
	g_free (pin);
}

static GPtrArray *
pins_new (void)
{
	return g_ptr_array_new_with_free_func ((GDestroyNotify) liria_sde_pin_free);
}

/* One spelling per location: file:///a/b/ and /a/b are the same pin. */
gchar *
liria_sde_pins_canonical_uri (const gchar *uri_or_path)
{
	GFile *file;
	gchar *uri;

	if (!uri_or_path || !*uri_or_path)
		return NULL;
	if (uri_or_path[0] == '/')
		file = g_file_new_for_path (uri_or_path);
	else if (g_str_has_prefix (uri_or_path, "~/")) {
		gchar *path = g_build_filename (g_get_home_dir (), uri_or_path + 2, NULL);

		file = g_file_new_for_path (path);
		g_free (path);
	} else {
		gchar *scheme = g_uri_parse_scheme (uri_or_path);

		if (!scheme)
			return NULL;
		g_free (scheme);
		file = g_file_new_for_uri (uri_or_path);
	}
	uri = g_file_get_uri (file);
	g_object_unref (file);
	return uri;
}

gint
liria_sde_pins_find (GPtrArray *pins, const gchar *uri)
{
	gchar *wanted = liria_sde_pins_canonical_uri (uri);
	gint found = -1;

	for (guint i = 0; wanted && i < pins->len && found < 0; i++)
		if (g_strcmp0 (((LiriaSdePin *) pins->pdata[i])->uri, wanted) == 0)
			found = (gint) i;
	g_free (wanted);
	return found;
}

GPtrArray *
liria_sde_pins_parse (const gchar *text)
{
	GPtrArray *pins = pins_new ();
	gchar **lines;

	if (!text)
		return pins;
	lines = g_strsplit (text, "\n", -1);
	for (guint i = 0; lines[i]; i++) {
		gchar *line = g_strchomp (lines[i]); /* also drops \r */
		gchar *tab, *uri, *label = NULL;

		while (*line == ' ')
			line++;
		if (!*line || *line == '#')
			continue;
		tab = strchr (line, '\t');
		if (tab) {
			*tab = '\0';
			label = g_strstrip (tab + 1);
		}
		uri = liria_sde_pins_canonical_uri (g_strstrip (line));
		if (uri && liria_sde_pins_find (pins, uri) < 0 && g_utf8_validate (line, -1, NULL))
			g_ptr_array_add (pins, liria_sde_pin_new (uri, label && g_utf8_validate (label, -1, NULL) ? label : NULL));
		g_free (uri);
	}
	g_strfreev (lines);
	return pins;
}

gchar *
liria_sde_pins_serialize (GPtrArray *pins)
{
	GString *text = g_string_new ("# Liria Quick access: one pinned location per line, in card order.\n"
	                              "# <URI>, optionally followed by a TAB and a custom label.\n");

	for (guint i = 0; i < pins->len; i++) {
		LiriaSdePin *pin = pins->pdata[i];

		g_string_append (text, pin->uri);
		if (pin->label && *pin->label) {
			gchar *label = g_strdup (pin->label);

			g_strdelimit (label, "\t\r\n", ' ');
			g_string_append_c (text, '\t');
			g_string_append (text, g_strstrip (label));
			g_free (label);
		}
		g_string_append_c (text, '\n');
	}
	return g_string_free (text, FALSE);
}

gchar *
liria_sde_pins_path (void)
{
	return g_build_filename (g_get_user_config_dir (), "sde", "liria-quick-access", NULL);
}

GPtrArray *
liria_sde_pins_defaults (void)
{
	static const GUserDirectory dirs[] = {
		G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_DOWNLOAD,
		G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_MUSIC, G_USER_DIRECTORY_VIDEOS,
	};
	GPtrArray *pins = pins_new ();

	for (guint i = 0; i < G_N_ELEMENTS (dirs); i++) {
		const gchar *path = g_get_user_special_dir (dirs[i]);
		gchar *uri;

		if (!path || g_strcmp0 (path, g_get_home_dir ()) == 0 || !g_file_test (path, G_FILE_TEST_IS_DIR))
			continue;
		uri = liria_sde_pins_canonical_uri (path);
		if (uri && liria_sde_pins_find (pins, uri) < 0)
			g_ptr_array_add (pins, liria_sde_pin_new (uri, NULL));
		g_free (uri);
	}
	g_ptr_array_add (pins, liria_sde_pin_new ("trash:///", NULL));
	return pins;
}

GPtrArray *
liria_sde_pins_load (void)
{
	gchar *path = liria_sde_pins_path (), *text = NULL;
	GPtrArray *pins;

	if (g_file_get_contents (path, &text, NULL, NULL))
		pins = liria_sde_pins_parse (text);
	else
		pins = liria_sde_pins_defaults ();
	g_free (text);
	g_free (path);
	return pins;
}

gboolean
liria_sde_pins_save (GPtrArray *pins)
{
	gchar *path = liria_sde_pins_path (), *dir = g_path_get_dirname (path);
	gchar *text = liria_sde_pins_serialize (pins);
	GError *error = NULL;
	gboolean ok;

	g_mkdir_with_parents (dir, 0700);
	ok = g_file_set_contents (path, text, -1, &error);
	if (!ok) {
		g_warning ("Liria Quick access: %s", error->message);
		g_error_free (error);
	}
	g_free (text);
	g_free (dir);
	g_free (path);
	return ok;
}

gboolean
liria_sde_pins_is_pinned (const gchar *uri)
{
	GPtrArray *pins = liria_sde_pins_load ();
	gboolean pinned = liria_sde_pins_find (pins, uri) >= 0;

	g_ptr_array_unref (pins);
	return pinned;
}

/* Adds the URIs not pinned yet at position (-1: at the end). */
gboolean
liria_sde_pins_add (const gchar *const *uris, gint position)
{
	GPtrArray *pins = liria_sde_pins_load ();
	gboolean changed = FALSE, ok = TRUE;

	if (position < 0 || position > (gint) pins->len)
		position = pins->len;
	for (guint i = 0; uris && uris[i]; i++) {
		gchar *uri = liria_sde_pins_canonical_uri (uris[i]);

		if (uri && liria_sde_pins_find (pins, uri) < 0) {
			g_ptr_array_insert (pins, position++, liria_sde_pin_new (uri, NULL));
			changed = TRUE;
		}
		g_free (uri);
	}
	if (changed)
		ok = liria_sde_pins_save (pins);
	g_ptr_array_unref (pins);
	return changed && ok;
}

gboolean
liria_sde_pins_remove (const gchar *const *uris)
{
	GPtrArray *pins = liria_sde_pins_load ();
	gboolean changed = FALSE, ok = TRUE;

	for (guint i = 0; uris && uris[i]; i++) {
		gint index = liria_sde_pins_find (pins, uris[i]);

		if (index >= 0) {
			g_ptr_array_remove_index (pins, index);
			changed = TRUE;
		}
	}
	if (changed)
		ok = liria_sde_pins_save (pins);
	g_ptr_array_unref (pins);
	return changed && ok;
}

/* Moves a pin so that it ends up at position (clamped to the list). */
gboolean
liria_sde_pins_move (const gchar *uri, gint position)
{
	GPtrArray *pins = liria_sde_pins_load ();
	gint index = liria_sde_pins_find (pins, uri);
	gboolean ok = FALSE;

	if (index >= 0) {
		LiriaSdePin *pin = g_ptr_array_steal_index (pins, index);

		position = CLAMP (position, 0, (gint) pins->len);
		g_ptr_array_insert (pins, position, pin);
		ok = position != index && liria_sde_pins_save (pins);
	}
	g_ptr_array_unref (pins);
	return ok;
}

gboolean
liria_sde_pins_set_label (const gchar *uri, const gchar *label)
{
	GPtrArray *pins = liria_sde_pins_load ();
	gint index = liria_sde_pins_find (pins, uri);
	gboolean ok = FALSE;

	if (index >= 0) {
		LiriaSdePin *pin = pins->pdata[index];
		gchar *clean = label ? g_strstrip (g_strdup (label)) : NULL;

		g_free (pin->label);
		pin->label = clean && *clean ? clean : NULL;
		if (!pin->label)
			g_free (clean);
		ok = liria_sde_pins_save (pins);
	}
	g_ptr_array_unref (pins);
	return ok;
}

gboolean
liria_sde_pins_reset (void)
{
	gchar *path = liria_sde_pins_path ();
	gboolean ok = g_unlink (path) == 0 || !g_file_test (path, G_FILE_TEST_EXISTS);

	g_free (path);
	return ok;
}
