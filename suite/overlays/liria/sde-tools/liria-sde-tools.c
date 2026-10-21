/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * SDE Tools for Liria: everyday actions in the context menus and the
 * toolbar - Open Terminal Here, Copy Path, Open as Administrator, Edit with
 * Niny, Edit in Photo, Set as Wallpaper, Compress, Extract Here, Checksum
 * and New Text Document. Programs are started without a shell. The Home
 * overview shown above the home folder lives in liria-sde-home.c.
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
	gchar *terminal = find_any ("x-terminal-emulator", "xfce4-terminal", "mate-terminal", "lxterminal",
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

/* ------------------------------------------------------------- menu items */

static LiriaMenuItem *
item (const gchar *name, const gchar *label, const gchar *tip, const gchar *icon, GCallback cb, GList *files)
{
	LiriaMenuItem *menu_item = liria_menu_item_new (name, label, tip, icon);

	g_signal_connect_data (menu_item, "activate", cb, copy_files (files), free_files, 0);
	return menu_item;
}

static GList *
get_file_items (LiriaMenuProvider *provider, GtkWidget *window, GList *files)
{
	GList *items = NULL;
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
	if (!local) {
		return g_list_append (NULL, item ("SdeTools::CopyPath", _("Copy Location"),
		                                  _("Copy the location to the clipboard"), "edit-copy",
		                                  G_CALLBACK (copy_path_cb), files));
	}
	if (count == 1)
		items = g_list_append (items, item ("SdeTools::Terminal", _("Open Terminal Here"),
		                                    _("Open a terminal in this folder"), "utilities-terminal",
		                                    G_CALLBACK (terminal_cb), files));
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
	if (all_archives)
		items = g_list_append (items, item ("SdeTools::Extract", _("Extract Here"),
		                                    _("Extract the archive into this folder"), "package-x-generic",
		                                    G_CALLBACK (extract_cb), files));
	else
		items = g_list_append (items, item ("SdeTools::Compress", _("Compress..."),
		                                    _("Create an archive with Taca"), "package-x-generic",
		                                    G_CALLBACK (compress_cb), files));
	if (count == 1 && !any_dir)
		items = g_list_append (items, item ("SdeTools::Checksum", _("Show Checksum"),
		                                    _("Show the SHA-256 checksum"), "dialog-information",
		                                    G_CALLBACK (checksum_cb), files));
	items = g_list_append (items, item ("SdeTools::CopyPath", _("Copy Path"),
	                                    _("Copy the full path to the clipboard"), "edit-copy",
	                                    G_CALLBACK (copy_path_cb), files));
	if (count == 1 && any_dir)
		items = g_list_append (items, item ("SdeTools::Admin", _("Open as Administrator"),
		                                    _("Open this folder in Liria as root (asks for your password)"),
		                                    "dialog-password", G_CALLBACK (admin_cb), files));
	return items;
}

static GList *
get_background_items (LiriaMenuProvider *provider, GtkWidget *window, LiriaFileInfo *folder)
{
	GList *files = g_list_append (NULL, folder), *items = NULL;
	gchar *path = local_path (folder);

	if (path) {
		items = g_list_append (items, item ("SdeTools::BgTerminal", _("Open Terminal Here"),
		                                    _("Open a terminal in this folder"), "utilities-terminal",
		                                    G_CALLBACK (terminal_cb), files));
		if (liria_file_info_can_write (folder))
			items = g_list_append (items, item ("SdeTools::NewDocument", _("New Text Document"),
			                                    _("Create an empty text document"), "text-x-generic",
			                                    G_CALLBACK (new_document_cb), files));
		items = g_list_append (items, item ("SdeTools::BgAdmin", _("Open as Administrator"),
		                                    _("Open this folder in Liria as root (asks for your password)"),
		                                    "dialog-password", G_CALLBACK (admin_cb), files));
	}
	items = g_list_append (items, item ("SdeTools::BgCopyPath", _("Copy Location"),
	                                    _("Copy this folder's location to the clipboard"), "edit-copy",
	                                    G_CALLBACK (copy_path_cb), files));
	g_free (path);
	g_list_free (files);
	return items;
}

static GList *
get_toolbar_items (LiriaMenuProvider *provider, GtkWidget *window, LiriaFileInfo *folder)
{
	GList *files = g_list_append (NULL, folder), *items = NULL;
	gchar *path = local_path (folder);

	if (path)
		items = g_list_append (items, item ("SdeTools::ToolbarTerminal", _("Terminal"),
		                                    _("Open a terminal in this folder"), "utilities-terminal",
		                                    G_CALLBACK (terminal_cb), files));
	g_free (path);
	g_list_free (files);
	return items;
}

static void
menu_provider_iface_init (LiriaMenuProviderIface *iface)
{
	iface->get_file_items = get_file_items;
	iface->get_background_items = get_background_items;
	iface->get_toolbar_items = get_toolbar_items;
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
