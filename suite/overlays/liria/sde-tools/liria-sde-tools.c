/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * SDE Tools for Liria: everyday actions in the context menus - Compress
 * and Extract Here with Taca, Edit with Niny, Edit in Photo, Set as
 * Wallpaper, Send To a removable drive, and under "More Actions" Open
 * Terminal Here, Copy Path, Show Checksum, Set Default Application (the
 * applications for the file's type, and Settings > Default Applications)
 * and Open as Administrator; on the
 * folder background Open Terminal Here and New Text Document. Pin to /
 * Unpin from Quick Access adds files, folders and locations to the Home
 * overview's Quick access cards (src/liria-sde-pins.c). Programs are
 * started without a shell. The Home overview shown above the home folder
 * lives in liria-sde-home.c.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include <libliria-extension/liria-extension-types.h>
#include <libliria-extension/liria-file-info.h>
#include <libliria-extension/liria-location-widget-provider.h>
#include <libliria-extension/liria-menu-provider.h>

#include "../src/liria-sde-pins.h"

#define SDE_TYPE_TOOLS (sde_tools_get_type ())

typedef struct { GObject parent; } SdeTools;
typedef struct { GObjectClass parent_class; } SdeToolsClass;

static GType sde_tools_type = 0;
static GType sde_tools_get_type (void) { return sde_tools_type; }

/* ------------------------------------------------------------------ helpers */

static gchar *
local_path (LiriaFileInfo *file)
{
	GFile *location = liria_file_info_get_location (file);
	gchar *path = location ? g_file_get_path (location) : NULL;

	if (location)
		g_object_unref (location);
	return path;
}

static gboolean
spawn (const gchar *const *argv, const gchar *dir)
{
	GError *error = NULL;
	gboolean ok = g_spawn_async (dir, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);

	if (!ok) {
		g_warning ("SDE Tools: %s", error->message);
		g_error_free (error);
	}
	return ok;
}

/* First installed program of a NULL-terminated list. */
static gchar *
find_any (const gchar *first, ...)
{
	va_list args;
	const gchar *name = first;
	gchar *found = NULL;

	va_start (args, first);
	while (name && !found) {
		found = g_find_program_in_path (name);
		name = va_arg (args, const gchar *);
	}
	va_end (args);
	return found;
}

static void
open_terminal (const gchar *dir)
{
	gchar *terminal = find_any ("x-terminal-emulator", "sde-terminal", "xfce4-terminal", "lxterminal",
	                            "xterm", NULL);
	if (terminal) {
		const gchar *argv[] = { terminal, NULL };
		spawn (argv, dir);
		g_free (terminal);
	}
}

/* Paths (one per line) of the selected local files. */
static gchar *
joined_paths (GList *files)
{
	GString *text = g_string_new (NULL);

	for (GList *l = files; l; l = l->next) {
		gchar *path = local_path (l->data);
		gchar *uri = path ? NULL : liria_file_info_get_uri (l->data);
		if (text->len) g_string_append_c (text, '\n');
		g_string_append (text, path ? path : uri);
		g_free (path);
		g_free (uri);
	}
	return g_string_free (text, FALSE);
}

static GList *
copy_files (GList *files)
{
	return liria_file_info_list_copy (files);
}

static void
free_files (gpointer data, GClosure *closure)
{
	liria_file_info_list_free (data);
}

static gboolean
is_type (LiriaFileInfo *file, const gchar *prefix)
{
	gchar *mime = liria_file_info_get_mime_type (file);
	gboolean match = mime && g_str_has_prefix (mime, prefix);

	g_free (mime);
	return match;
}

static gboolean
is_archive (LiriaFileInfo *file)
{
	static const gchar *types[] = { "application/zip", "application/x-tar", "application/x-compressed-tar",
	                                "application/x-bzip-compressed-tar", "application/x-xz-compressed-tar",
	                                "application/x-7z-compressed", "application/vnd.rar", "application/x-rar",
	                                "application/gzip", "application/x-zstd-compressed-tar", NULL };
	for (int i = 0; types[i]; i++)
		if (liria_file_info_is_mime_type (file, types[i]))
			return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------ actions */

static void
terminal_cb (LiriaMenuItem *item, gpointer data)
{
	LiriaFileInfo *file = g_list_nth_data (data, 0);
	gchar *path = file ? local_path (file) : NULL;

	if (path && !liria_file_info_is_directory (file)) {
		gchar *parent = g_path_get_dirname (path);
		g_free (path);
		path = parent;
	}
	open_terminal (path ? path : g_get_home_dir ());
	g_free (path);
}

static void
copy_path_cb (LiriaMenuItem *item, gpointer data)
{
	gchar *text = joined_paths (data);

	gtk_clipboard_set_text (gtk_clipboard_get (GDK_SELECTION_CLIPBOARD), text, -1);
	gtk_clipboard_set_text (gtk_clipboard_get (GDK_SELECTION_PRIMARY), text, -1);
	g_free (text);
}

/* "sudo liria" in a terminal: sudo asks for the password, and Liria runs
 * on a private D-Bus session with root's own preferences. */
static void
admin_cb (LiriaMenuItem *item, gpointer data)
{
	LiriaFileInfo *file = g_list_nth_data (data, 0);
	gchar *path = file ? local_path (file) : g_strdup (g_get_home_dir ());
	gchar *terminal = find_any ("x-terminal-emulator", "xterm", NULL);

	if (path && terminal) {
		const gchar *argv[] = { terminal, "-e", "sudo", "liria", "--no-desktop", path, NULL };
		spawn (argv, NULL);
	}
	g_free (terminal);
	g_free (path);
}

static void
open_each_with (GList *files, const gchar *program)
{
	GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);

	g_ptr_array_add (argv, g_strdup (program));
	for (GList *l = files; l; l = l->next) {
		gchar *path = local_path (l->data);
		if (path) g_ptr_array_add (argv, path);
	}
	g_ptr_array_add (argv, NULL);
	spawn ((const gchar *const *) argv->pdata, NULL);
	g_ptr_array_free (argv, TRUE);
}

static void
niny_cb (LiriaMenuItem *item, gpointer data)
{
	open_each_with (data, "niny");
}

static void
photo_cb (LiriaMenuItem *item, gpointer data)
{
	open_each_with (data, "sde-view");
}

/* Writes [appearance] wallpaper to ~/.config/sde/sde.conf and asks the
 * running desktop to reload it. */
static void
wallpaper_cb (LiriaMenuItem *item, gpointer data)
{
	gchar *path = local_path (g_list_nth_data (data, 0));
	gchar *dir = g_build_filename (g_get_user_config_dir (), "sde", NULL);
	gchar *conf = g_build_filename (dir, "sde.conf", NULL);
	GKeyFile *key = g_key_file_new ();
	GError *error = NULL;

	if (!path) goto out;
	g_mkdir_with_parents (dir, 0700);
	g_key_file_load_from_file (key, conf, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_key_file_set_string (key, "appearance", "wallpaper", path);
	if (!g_key_file_save_to_file (key, conf, &error)) {
		g_warning ("SDE Tools: %s", error->message);
		g_error_free (error);
	} else {
		gchar *serial = g_strdup_printf ("%u", (unsigned) (g_get_real_time () % 1000000000));
		const gchar *argv[] = { "xprop", "-root", "-f", "_SDE_CONFIG_SERIAL", "32c", "-set",
		                        "_SDE_CONFIG_SERIAL", serial, NULL };
		spawn (argv, NULL);
		g_free (serial);
	}
out:
	g_key_file_unref (key);
	g_free (conf);
	g_free (dir);
	g_free (path);
}

static void
compress_cb (LiriaMenuItem *item, gpointer data)
{
	GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
	LiriaFileInfo *first = g_list_nth_data (data, 0);
	gchar *path = first ? local_path (first) : NULL;
	gchar *folder = path ? g_path_get_dirname (path) : NULL;

	g_ptr_array_add (argv, g_strdup ("taca"));
	g_ptr_array_add (argv, g_strdup ("--add"));
	if (folder) g_ptr_array_add (argv, g_strdup_printf ("--default-dir=%s", folder));
	for (GList *l = data; l; l = l->next) {
		gchar *p = local_path (l->data);
		if (p) g_ptr_array_add (argv, p);
	}
	g_ptr_array_add (argv, NULL);
	spawn ((const gchar *const *) argv->pdata, folder);
	g_ptr_array_free (argv, TRUE);
	g_free (folder);
	g_free (path);
}

static void
extract_cb (LiriaMenuItem *item, gpointer data)
{
	for (GList *l = data; l; l = l->next) {
		gchar *path = local_path (l->data);
		if (path) {
			gchar *folder = g_path_get_dirname (path);
			const gchar *argv[] = { "taca", "--extract-here", path, NULL };
			spawn (argv, folder);
			g_free (folder);
		}
		g_free (path);
	}
}

static void
checksum_cb (LiriaMenuItem *item, gpointer data)
{
	gchar *path = local_path (g_list_nth_data (data, 0));
	gchar *contents = NULL, *sum = NULL, *name;
	gsize length = 0;
	GError *error = NULL;
	GtkWidget *dialog;

	if (!path) return;
	name = g_path_get_basename (path);
	if (g_file_get_contents (path, &contents, &length, &error))
		sum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) contents, length);
	dialog = gtk_message_dialog_new (NULL, 0, sum ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, GTK_BUTTONS_NONE,
	                                 "%s", name);
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "SHA-256:\n%s",
	                                          sum ? sum : error->message);
	gtk_window_set_title (GTK_WINDOW (dialog), _("Checksum"));
	if (sum) gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Copy"), 1);
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Close"), GTK_RESPONSE_CLOSE);
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == 1)
		gtk_clipboard_set_text (gtk_clipboard_get (GDK_SELECTION_CLIPBOARD), sum, -1);
	gtk_widget_destroy (dialog);
	if (error) g_error_free (error);
	g_free (contents);
	g_free (sum);
	g_free (name);
	g_free (path);
}

static void
new_document_cb (LiriaMenuItem *item, gpointer data)
{
	gchar *dir = local_path (g_list_nth_data (data, 0));
	gchar *file = NULL;

	if (!dir) return;
	for (int i = 1; i < 100; i++) {
		gchar *name = i == 1 ? g_strdup (_("New Document.txt"))
		                     : g_strdup_printf (_("New Document %d.txt"), i);
		file = g_build_filename (dir, name, NULL);
		g_free (name);
		if (!g_file_test (file, G_FILE_TEST_EXISTS)) break;
		g_clear_pointer (&file, g_free);
	}
	if (file) g_file_set_contents (file, "", 0, NULL);
	g_free (file);
	g_free (dir);
}

/* Send To: Liria copies with its safe removable-drive engine through the
 * application action "sde-copy-to" (liria-sde-ui.c). */
static void
send_to_cb (LiriaMenuItem *item, gpointer data)
{
	const gchar *target = g_object_get_data (G_OBJECT (item), "sde-target");
	GVariantBuilder uris;
	GApplication *app = g_application_get_default ();

	if (!target || !app)
		return;
	g_variant_builder_init (&uris, G_VARIANT_TYPE ("as"));
	for (GList *l = data; l; l = l->next) {
		gchar *uri = liria_file_info_get_uri (l->data);
		g_variant_builder_add (&uris, "s", uri);
		g_free (uri);
	}
	g_action_group_activate_action (G_ACTION_GROUP (app), "sde-copy-to",
	                                g_variant_new ("(ass)", &uris, target));
}

/* Writable mounts of removable drives (USB sticks, memory cards, phones). */
static GList *
removable_mounts (void)
{
	GVolumeMonitor *monitor = g_volume_monitor_get ();
	GList *mounts = g_volume_monitor_get_mounts (monitor), *result = NULL;

	for (GList *l = mounts; l; l = l->next) {
		GMount *mount = l->data;
		GDrive *drive = g_mount_get_drive (mount);
		gboolean removable = g_mount_can_eject (mount) ||
		                     (drive && (g_drive_is_removable (drive) || g_drive_is_media_removable (drive)));

		if (removable && !g_mount_is_shadowed (mount))
			result = g_list_append (result, g_object_ref (mount));
		if (drive)
			g_object_unref (drive);
	}
	g_list_free_full (mounts, g_object_unref);
	g_object_unref (monitor);
	return result;
}

/* ------------------------------------------------------------ quick access */

static gchar **
file_uris (GList *files)
{
	GPtrArray *uris = g_ptr_array_new ();

	for (GList *l = files; l; l = l->next)
		g_ptr_array_add (uris, liria_file_info_get_uri (l->data));
	g_ptr_array_add (uris, NULL);
	return (gchar **) g_ptr_array_free (uris, FALSE);
}

static void
pin_cb (LiriaMenuItem *item, GList *files)
{
	gchar **uris = file_uris (files);

	liria_sde_pins_add ((const gchar *const *) uris, -1);
	g_strfreev (uris);
}

static void
unpin_cb (LiriaMenuItem *item, GList *files)
{
	gchar **uris = file_uris (files);

	liria_sde_pins_remove ((const gchar *const *) uris);
	g_strfreev (uris);
}

static LiriaMenuItem *item (const gchar *name, const gchar *label, const gchar *tip, const gchar *icon,
                            GCallback cb, GList *files);

/* "Pin to Quick Access", or "Unpin from Quick Access" when every selected
 * item is pinned already. Items inside the Trash are not offered. */
static LiriaMenuItem *
pin_item (const gchar *name, GList *files)
{
	GPtrArray *pins = liria_sde_pins_load ();
	gboolean all_pinned = TRUE, offered = TRUE;
	gchar *pin_name = g_strconcat (name, "Pin", NULL), *unpin_name = g_strconcat (name, "Unpin", NULL);
	LiriaMenuItem *menu_item = NULL;

	for (GList *l = files; l; l = l->next) {
		gchar *uri = liria_file_info_get_uri (l->data);

		if (!uri || (g_str_has_prefix (uri, "trash:") && g_strcmp0 (uri, "trash:///") != 0) ||
		    g_str_has_prefix (uri, "x-liria-desktop:"))
			offered = FALSE;
		else if (liria_sde_pins_find (pins, uri) < 0)
			all_pinned = FALSE;
		g_free (uri);
	}
	if (offered && all_pinned)
		menu_item = item (unpin_name, _("Unpin from Quick Access"), _("Remove from Quick access on the Home overview"),
		                  "view-pin-symbolic", G_CALLBACK (unpin_cb), files);
	else if (offered)
		menu_item = item (pin_name, _("Pin to Quick Access"), _("Show in Quick access on the Home overview"),
		                  "view-pin-symbolic", G_CALLBACK (pin_cb), files);
	g_free (unpin_name);
	g_free (pin_name);
	g_ptr_array_unref (pins);
	return menu_item;
}

/* ------------------------------------------------- default applications */

static void
set_default_cb (LiriaMenuItem *menu_item, gpointer data)
{
	GAppInfo *app = g_object_get_data (G_OBJECT (menu_item), "sde-app");
	const gchar *type = g_object_get_data (G_OBJECT (menu_item), "sde-type");
	GError *error = NULL;

	/* GIO writes [Default Applications] in ~/.config/mimeapps.list, the file
	 * Settings > Default Applications reads too. */
	if (app && type && !g_app_info_set_as_default_for_type (app, type, &error)) {
		g_warning ("SDE Tools: %s", error->message);
		g_error_free (error);
	}
}

static void
default_apps_cb (LiriaMenuItem *menu_item, gpointer data)
{
	const gchar *argv[] = { "sde-settings", "--page", "default-apps", NULL };

	spawn (argv, NULL);
}

/* "Set Default Application" > the applications for the file's type (the
 * current one marked), then "Default Applications..." in the Control Center. */
static LiriaMenuItem *
set_default_item (LiriaFileInfo *file)
{
	gchar *type = liria_file_info_get_mime_type (file);
	GList *apps = type ? g_app_info_get_all_for_type (type) : NULL;
	GAppInfo *current = type ? g_app_info_get_default_for_type (type, FALSE) : NULL;
	LiriaMenuItem *parent, *settings;
	LiriaMenu *menu;
	int n = 0;

	parent = liria_menu_item_new ("SdeTools::SetDefault", _("Set Default Application"),
	                              _("Choose the application that opens files of this type"),
	                              "preferences-desktop-default-applications");
	menu = liria_menu_new ();
	for (GList *l = apps; l && n < 15; l = l->next) {
		GAppInfo *app = l->data;
		gboolean is_current = current && g_app_info_equal (app, current);
		gchar *id = g_strdup_printf ("SdeTools::SetDefault%d", n++);
		gchar *label = is_current ? g_strdup_printf (_("%s (current)"), g_app_info_get_display_name (app))
		                          : g_strdup (g_app_info_get_display_name (app));
		gchar *tip = g_strdup_printf (_("Open every \"%s\" file with %s"), type,
		                              g_app_info_get_display_name (app));
		GIcon *gicon = g_app_info_get_icon (app);
		gchar *icon = gicon ? g_icon_to_string (gicon) : NULL;
		LiriaMenuItem *menu_item = liria_menu_item_new (id, label, tip, icon);

		g_object_set_data_full (G_OBJECT (menu_item), "sde-app", g_object_ref (app), g_object_unref);
		g_object_set_data_full (G_OBJECT (menu_item), "sde-type", g_strdup (type), g_free);
		g_signal_connect (menu_item, "activate", G_CALLBACK (set_default_cb), NULL);
		liria_menu_append_item (menu, menu_item);
		g_object_unref (menu_item);
		g_free (id);
		g_free (label);
		g_free (tip);
		g_free (icon);
	}
	settings = liria_menu_item_new ("SdeTools::DefaultApps", _("Default Applications..."),
	                                _("Open Settings > Default Applications"), "preferences-system");
	g_signal_connect (settings, "activate", G_CALLBACK (default_apps_cb), NULL);
	liria_menu_append_item (menu, settings);
	g_object_unref (settings);
	liria_menu_item_set_submenu (parent, menu);
	g_object_unref (menu);
	g_list_free_full (apps, g_object_unref);
	if (current)
		g_object_unref (current);
	g_free (type);
	return parent;
}

/* ------------------------------------------------------------- menu items */

static LiriaMenuItem *
item (const gchar *name, const gchar *label, const gchar *tip, const gchar *icon, GCallback cb, GList *files)
{
	LiriaMenuItem *menu_item = liria_menu_item_new (name, label, tip, icon);

	g_signal_connect_data (menu_item, "activate", cb, copy_files (files), free_files, 0);
	return menu_item;
}

static void
append_owned (LiriaMenu *menu, LiriaMenuItem *menu_item)
{
	liria_menu_append_item (menu, menu_item);
	g_object_unref (menu_item);
}

static LiriaMenuItem *
send_to_item (GList *files)
{
	GList *mounts = removable_mounts ();
	LiriaMenuItem *parent;
	LiriaMenu *menu;
	int n = 0;

	if (!mounts)
		return NULL;
	parent = liria_menu_item_new ("SdeTools::SendTo", _("Send To"),
	                              _("Copy to a removable drive"), "drive-removable-media");
	menu = liria_menu_new ();
	for (GList *l = mounts; l; l = l->next) {
		GFile *root = g_mount_get_root (l->data);
		gchar *uri = g_file_get_uri (root);
		gchar *name = g_mount_get_name (l->data);
		gchar *id = g_strdup_printf ("SdeTools::SendTo%d", n++);
		gchar *tip = g_strdup_printf (_("Copy the selected items to \"%s\""), name);
		LiriaMenuItem *drive = item (id, name, tip, "drive-removable-media", G_CALLBACK (send_to_cb), files);

		g_object_set_data_full (G_OBJECT (drive), "sde-target", uri, g_free);
		liria_menu_append_item (menu, drive);
		g_object_unref (drive);
		g_object_unref (root);
		g_free (name);
		g_free (id);
		g_free (tip);
	}
	liria_menu_item_set_submenu (parent, menu);
	g_object_unref (menu);
	g_list_free_full (mounts, g_object_unref);
	return parent;
}

static GList *
get_file_items (LiriaMenuProvider *provider, GtkWidget *window, GList *files)
{
	GList *items = NULL;
	LiriaMenuItem *more_item, *send, *pin;
	LiriaMenu *more;
	guint count = g_list_length (files);
	gboolean local = TRUE, all_text = TRUE, all_images = TRUE, all_archives = TRUE, any_dir = FALSE;

	if (count == 0) return NULL;
	for (GList *l = files; l; l = l->next) {
		gchar *path = local_path (l->data);
		if (!path) local = FALSE;
		g_free (path);
		if (liria_file_info_is_directory (l->data)) any_dir = TRUE;
		if (!is_type (l->data, "text/") && !liria_file_info_is_mime_type (l->data, "application/x-shellscript") &&
		    !liria_file_info_is_mime_type (l->data, "application/json") &&
		    !liria_file_info_is_mime_type (l->data, "application/xml"))
			all_text = FALSE;
		if (!is_type (l->data, "image/")) all_images = FALSE;
		if (!is_archive (l->data)) all_archives = FALSE;
	}
	pin = pin_item ("SdeTools::", files);
	if (!local) {
		if (pin)
			items = g_list_append (items, pin);
		return g_list_append (items, item ("SdeTools::CopyPath", _("Copy Location"),
		                                   _("Copy the location to the clipboard"), "edit-copy",
		                                   G_CALLBACK (copy_path_cb), files));
	}
	if (all_archives)
		items = g_list_append (items, item ("SdeTools::Extract", _("Extract Here"),
		                                    _("Extract the archive into this folder"), "package-x-generic",
		                                    G_CALLBACK (extract_cb), files));
	else
		items = g_list_append (items, item ("SdeTools::Compress", _("Compress..."),
		                                    _("Create an archive with Taca"), "package-x-generic",
		                                    G_CALLBACK (compress_cb), files));
	send = send_to_item (files);
	if (send)
		items = g_list_append (items, send);
	if (all_text && !any_dir)
		items = g_list_append (items, item ("SdeTools::Niny", _("Edit with Niny"),
		                                    _("Open in the Niny text editor"), "accessories-text-editor",
		                                    G_CALLBACK (niny_cb), files));
	if (all_images) {
		items = g_list_append (items, item ("SdeTools::Photo", _("Edit in Photo"),
		                                    _("Open in Photo with filters and effects"), "image-x-generic",
		                                    G_CALLBACK (photo_cb), files));
		if (count == 1)
			items = g_list_append (items, item ("SdeTools::Wallpaper", _("Set as Wallpaper"),
			                                    _("Use this picture as the desktop background"),
			                                    "preferences-desktop-wallpaper", G_CALLBACK (wallpaper_cb), files));
	}
	if (pin)
		items = g_list_append (items, pin);
	/* Less frequent tools in one submenu keep the menu short. */
	more_item = liria_menu_item_new ("SdeTools::More", _("More Actions"), _("More actions for the selection"),
	                                 "view-more");
	more = liria_menu_new ();
	if (count == 1)
		append_owned (more, item ("SdeTools::Terminal", _("Open Terminal Here"),
		                                    _("Open a terminal in this folder"), "utilities-terminal",
		                                    G_CALLBACK (terminal_cb), files));
	append_owned (more, item ("SdeTools::CopyPath", _("Copy Path"),
	                                    _("Copy the full path to the clipboard"), "edit-copy",
	                                    G_CALLBACK (copy_path_cb), files));
	if (count == 1 && !any_dir)
		append_owned (more, item ("SdeTools::Checksum", _("Show Checksum"),
		                                    _("Show the SHA-256 checksum"), "dialog-information",
		                                    G_CALLBACK (checksum_cb), files));
	if (count == 1 && !any_dir)
		append_owned (more, set_default_item (files->data));
	if (count == 1 && any_dir)
		append_owned (more, item ("SdeTools::Admin", _("Open as Administrator"),
		                                    _("Open this folder in Liria as root (asks for your password)"),
		                                    "dialog-password", G_CALLBACK (admin_cb), files));
	liria_menu_item_set_submenu (more_item, more);
	g_object_unref (more);
	items = g_list_append (items, more_item);
	return items;
}

static GList *
get_background_items (LiriaMenuProvider *provider, GtkWidget *window, LiriaFileInfo *folder)
{
	GList *files = g_list_append (NULL, folder), *items = NULL;
	gchar *path = local_path (folder);
	LiriaMenuItem *more_item, *pin;
	LiriaMenu *more;

	if (path) {
		items = g_list_append (items, item ("SdeTools::BgTerminal", _("Open Terminal Here"),
		                                    _("Open a terminal in this folder"), "utilities-terminal",
		                                    G_CALLBACK (terminal_cb), files));
		if (liria_file_info_can_write (folder))
			items = g_list_append (items, item ("SdeTools::NewDocument", _("New Text Document"),
			                                    _("Create an empty text document"), "text-x-generic",
			                                    G_CALLBACK (new_document_cb), files));
	}
	pin = pin_item ("SdeTools::Bg", files);
	if (pin)
		items = g_list_append (items, pin);
	more_item = liria_menu_item_new ("SdeTools::BgMore", _("More Actions"), _("More actions for this folder"),
	                                 "view-more");
	more = liria_menu_new ();
	append_owned (more, item ("SdeTools::BgCopyPath", _("Copy Location"),
	                          _("Copy this folder's location to the clipboard"), "edit-copy",
	                          G_CALLBACK (copy_path_cb), files));
	if (path)
		append_owned (more, item ("SdeTools::BgAdmin", _("Open as Administrator"),
		                          _("Open this folder in Liria as root (asks for your password)"),
		                          "dialog-password", G_CALLBACK (admin_cb), files));
	liria_menu_item_set_submenu (more_item, more);
	g_object_unref (more);
	items = g_list_append (items, more_item);
	g_free (path);
	g_list_free (files);
	return items;
}

static void
menu_provider_iface_init (LiriaMenuProviderIface *iface)
{
	iface->get_file_items = get_file_items;
	iface->get_background_items = get_background_items;
}

/* ------------------------------------------------------- home overview */

GtkWidget *sde_home_overview_new (const char *uri, GtkWidget *window);

static GtkWidget *
get_location_widget (LiriaLocationWidgetProvider *provider, const char *uri, GtkWidget *window)
{
	return sde_home_overview_new (uri, window);
}

static void
location_widget_provider_iface_init (LiriaLocationWidgetProviderIface *iface)
{
	iface->get_widget = get_location_widget;
}

static void
sde_tools_class_init (SdeToolsClass *klass)
{
}

static void
sde_tools_instance_init (SdeTools *self)
{
}

static void
sde_tools_register_type (GTypeModule *module)
{
	static const GTypeInfo info = {
		sizeof (SdeToolsClass), NULL, NULL, (GClassInitFunc) sde_tools_class_init, NULL, NULL,
		sizeof (SdeTools), 0, (GInstanceInitFunc) sde_tools_instance_init, NULL
	};
	static const GInterfaceInfo menu_provider = { (GInterfaceInitFunc) menu_provider_iface_init, NULL, NULL };
	static const GInterfaceInfo location_widget_provider = {
		(GInterfaceInitFunc) location_widget_provider_iface_init, NULL, NULL
	};

	sde_tools_type = g_type_module_register_type (module, G_TYPE_OBJECT, "SdeTools", &info, 0);
	g_type_module_add_interface (module, sde_tools_type, LIRIA_TYPE_MENU_PROVIDER, &menu_provider);
	g_type_module_add_interface (module, sde_tools_type, LIRIA_TYPE_LOCATION_WIDGET_PROVIDER,
	                             &location_widget_provider);
}

/* ------------------------------------------------------- module interface */

static GType type_list[1];

void
liria_module_initialize (GTypeModule *module)
{
	sde_tools_register_type (module);
	type_list[0] = SDE_TYPE_TOOLS;
}

void
liria_module_shutdown (void)
{
}

void
liria_module_list_types (const GType **types, int *num_types)
{
	*types = type_list;
	*num_types = G_N_ELEMENTS (type_list);
}
