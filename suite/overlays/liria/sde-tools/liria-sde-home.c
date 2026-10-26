/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * SDE Home overview for Liria: when a window shows the home folder, a
 * collapsible overview sits above the files with Quick access cards,
 * drives with a usage bar and recently used files. Everything is styled by
 * the SDE GTK themes through the .sde-home-* / .sde-*-card style classes.
 * Recent files can be removed one by one (right click or the Menu key) or
 * cleared at once; both go through GtkRecentManager, so the shared
 * ~/.local/share/recently-used.xbel list of every application changes.
 *
 * Quick access shows the user's pinned folders, files and locations
 * (src/liria-sde-pins.c, ~/.config/sde/liria-quick-access). Pins are added
 * with "Pin to Quick Access" in the file and sidebar menus, by dropping
 * files on the cards or with the "+" card; a card's menu (right click,
 * Menu key or Shift+F10) opens, renames, moves, unpins and resets them,
 * cards can be dragged to a new place, Delete unpins the focused card and
 * Ctrl+Shift+Left/Right moves it. Every window watches the pin file.
 *
 * Folders open in the same Liria window through the "sde-go-to-uri" hook
 * that suite/redesign.py adds to the navigation window; without it they are
 * opened with the default handler. Slow file systems are queried
 * asynchronously and every pending request is cancelled with the widget.
 *
 * Since alpha52 the overview is the Home view (as in Windows 11): while it
 * is shown, the window's "sde-home-replaces-view" hook hides the folder
 * grid below it and the overview scrolls on its own; "Hide overview" brings
 * the plain folder view back. Quick access lists the pins first (with a
 * pin marker), then folders suggested from recently used files (marked
 * "Frequent", with a Pin button on hover). Quick access, Drives and Recent
 * files have collapsible headers (remembered in ~/.config/sde/liria.conf)
 * and the drives are a grid of cards. Every card is a drag source
 * (text/uri-list) for Liria, the desktop and other applications; files
 * dropped on a folder or drive card are copied, moved or linked into it
 * (Shift moves, Ctrl copies, Ctrl+Shift links, Alt or the middle button
 * ask) through the window's "sde-drop" action, and locations dropped on
 * the Quick access area, a gap or the "+" card are pinned there.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include "../src/liria-sde-pins.h"
#include "../src/liria-sde-model.h"

GtkWidget *sde_home_overview_new (const char *uri, GtkWidget *window);

typedef void (*SdeGoToUri) (GtkWidget *window, const char *uri);
/* Window hook (liria-sde-ui.c): the overview replaces the folder view. */
typedef void (*SdeReplacesView) (GtkWidget *overview, gboolean replace);

typedef struct {
	GtkWidget *window;
	GtkWidget *root;
	GtkWidget *scroller;
	GtkWidget *toggle_label;
	GtkWidget *toggle_icon;
	GtkWidget *drives;
	GtkWidget *recent;
	GtkWidget *recent_clear;
	GtkWidget *quick;
	GtkWidget *drop_card;   /* the card highlighted by a drag over Quick access */
	GCancellable *quick_cancellable;
	GFileMonitor *pins_monitor;
	guint reload_id;
	gchar *focus_uri;
	GVolumeMonitor *monitor;
	GCancellable *cancellable;
	gboolean expanded;
	gboolean destroyed;
} Overview;

/* ------------------------------------------------------------ settings */

static gboolean
overview_expanded (void)
{
	return liria_sde_prefs_get_bool ("home", "show-overview", TRUE);
}

static void
save_expanded (gboolean expanded)
{
	liria_sde_prefs_set_bool ("home", "show-overview", expanded);
}

/* ----------------------------------------------------------- navigation */

typedef struct {
	GtkWidget *window;
	gchar *uri;
	gboolean open_file;
} Activation;

static gboolean
activate_idle (gpointer data)
{
	Activation *a = data;
	SdeGoToUri go = a->window ? g_object_get_data (G_OBJECT (a->window), "sde-go-to-uri") : NULL;

	if (go && !a->open_file) {
		go (a->window, a->uri);
	} else {
		GdkAppLaunchContext *context = gdk_display_get_app_launch_context (gdk_display_get_default ());
		GError *error = NULL;

		if (!g_app_info_launch_default_for_uri (a->uri, G_APP_LAUNCH_CONTEXT (context), &error)) {
			g_warning ("SDE Home: %s", error->message);
			g_error_free (error);
		}
		g_object_unref (context);
	}
	if (a->window)
		g_object_unref (a->window);
	g_free (a->uri);
	g_free (a);
	return G_SOURCE_REMOVE;
}

/* Runs after the click: opening a folder replaces this overview. */
static void
activate_uri (GtkWidget *window, const gchar *uri, gboolean open_file)
{
	Activation *a = g_new0 (Activation, 1);

	a->window = window ? g_object_ref (window) : NULL;
	a->uri = g_strdup (uri);
	a->open_file = open_file;
	g_idle_add (activate_idle, a);
}

static gboolean pin_card_clicked (GtkWidget *card, Overview *o);
static gboolean pin_card_popup (GtkWidget *card, Overview *o, const GdkEvent *event);
static gboolean pin_card_key (GtkWidget *card, GdkEventKey *event, Overview *o);

static void
card_clicked (GtkButton *button, Overview *o)
{
	const gchar *uri = g_object_get_data (G_OBJECT (button), "sde-uri");

	if (pin_card_clicked (GTK_WIDGET (button), o))
		return;
	if (uri)
		activate_uri (o->window, uri, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "sde-open-file")));
}

static void fill_recent (Overview *o);

/* ---------------------------------------------------- recent: clearing */

static void
recent_remove_uri (Overview *o, const gchar *uri)
{
	GError *error = NULL;

	if (!gtk_recent_manager_remove_item (gtk_recent_manager_get_default (), uri, &error)) {
		g_warning ("SDE Home: %s", error->message);
		g_error_free (error);
	}
	if (!o->destroyed)
		fill_recent (o);
}

static void
recent_clear_all (Overview *o)
{
	GtkWidget *toplevel = o->window ? gtk_widget_get_toplevel (o->window) : NULL;
	GtkWidget *dialog = gtk_message_dialog_new (GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL,
	                                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	                                            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
	                                            _("Clear the list of recent files?"));
	GError *error = NULL;
	gint response;

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
	                                          _("The files themselves are not deleted. The list is "
	                                            "cleared for all applications."));
	gtk_dialog_add_buttons (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL,
	                        _("C_lear"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
	response = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	if (response != GTK_RESPONSE_ACCEPT)
		return;
	if (gtk_recent_manager_purge_items (gtk_recent_manager_get_default (), &error) < 0 || error) {
		if (error) {
			g_warning ("SDE Home: %s", error->message);
			g_error_free (error);
		}
	}
	if (!o->destroyed)
		fill_recent (o);
}

static void
clear_clicked (GtkButton *button, Overview *o)
{
	recent_clear_all (o);
}

typedef struct {
	Overview *o;
	gchar *uri;
	gchar *parent_uri;
} RecentAction;

static void
recent_action_free (gpointer data, GClosure *closure)
{
	RecentAction *a = data;

	g_free (a->uri);
	g_free (a->parent_uri);
	g_free (a);
}

static void
menu_open (GtkMenuItem *item, RecentAction *a)
{
	activate_uri (a->o->window, a->uri, TRUE);
}

static void
menu_open_folder (GtkMenuItem *item, RecentAction *a)
{
	activate_uri (a->o->window, a->parent_uri, FALSE);
}

/* The menu is closing: change the list after the menu has gone. */
static gboolean
remove_idle (gpointer data)
{
	RecentAction *a = data;

	if (!a->o->destroyed)
		recent_remove_uri (a->o, a->uri);
	g_object_unref (a->o->root);
	recent_action_free (a, NULL);
	return G_SOURCE_REMOVE;
}

static void
menu_remove (GtkMenuItem *item, RecentAction *a)
{
	RecentAction *copy = g_new0 (RecentAction, 1);

	copy->o = a->o;
	copy->uri = g_strdup (a->uri);
	g_object_ref (a->o->root);
	g_idle_add (remove_idle, copy);
}

static gboolean
clear_idle (gpointer data)
{
	GtkWidget *root = data;
	Overview *o = g_object_get_data (G_OBJECT (root), "sde-overview");

	if (o && !o->destroyed)
		recent_clear_all (o);
	g_object_unref (root);
	return G_SOURCE_REMOVE;
}

static void
menu_clear (GtkMenuItem *item, RecentAction *a)
{
	g_idle_add (clear_idle, g_object_ref (a->o->root));
}

static GtkWidget *
menu_item (GtkWidget *menu, const gchar *label, GCallback callback, RecentAction *a, gboolean owns)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic (label);

	if (owns)
		g_signal_connect_data (item, "activate", callback, a, recent_action_free, 0);
	else
		g_signal_connect (item, "activate", callback, a);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	return item;
}

static gboolean
destroy_idle (gpointer menu)
{
	gtk_widget_destroy (menu);
	g_object_unref (menu);
	return G_SOURCE_REMOVE;
}

/* The chosen item is activated after "deactivate": destroy later. */
static void
menu_deactivated (GtkWidget *menu)
{
	g_idle_add (destroy_idle, g_object_ref (menu));
}

static void
recent_popup (GtkWidget *card, Overview *o, const GdkEvent *event)
{
	RecentAction *a = g_new0 (RecentAction, 1);
	GtkWidget *menu = gtk_menu_new ();

	a->o = o;
	a->uri = g_strdup (g_object_get_data (G_OBJECT (card), "sde-uri"));
	a->parent_uri = g_strdup (g_object_get_data (G_OBJECT (card), "sde-parent-uri"));
	gtk_style_context_add_class (gtk_widget_get_style_context (menu), "sde-recent-menu");
	/* The first item owns the shared data and frees it with the menu. */
	menu_item (menu, _("_Open"), G_CALLBACK (menu_open), a, TRUE);
	if (a->parent_uri)
		menu_item (menu, _("Open _Containing Folder"), G_CALLBACK (menu_open_folder), a, FALSE);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
	menu_item (menu, _("_Remove from Recent"), G_CALLBACK (menu_remove), a, FALSE);
	menu_item (menu, _("C_lear Recent Files\xE2\x80\xA6"), G_CALLBACK (menu_clear), a, FALSE);
	gtk_widget_show_all (menu);
	gtk_menu_attach_to_widget (GTK_MENU (menu), card, NULL);
	g_signal_connect (menu, "deactivate", G_CALLBACK (menu_deactivated), NULL);
	if (event)
		gtk_menu_popup_at_pointer (GTK_MENU (menu), event);
	else
		gtk_menu_popup_at_widget (GTK_MENU (menu), card, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

/* Right click on a recent file: Open, Open Containing Folder, Remove
 * from Recent and Clear Recent Files. */
static gboolean
card_pressed (GtkWidget *button, GdkEventButton *event, Overview *o)
{
	if (event->type == GDK_BUTTON_PRESS && event->button == 3 &&
	    pin_card_popup (button, o, (GdkEvent *) event))
		return TRUE;
	if (event->type == GDK_BUTTON_PRESS && event->button == 3 &&
	    g_object_get_data (G_OBJECT (button), "sde-open-file")) {
		recent_popup (button, o, (GdkEvent *) event);
		return TRUE;
	}
	return FALSE;
}

static gboolean
card_popup_menu (GtkWidget *button, Overview *o)
{
	if (pin_card_popup (button, o, NULL))
		return TRUE;
	if (!g_object_get_data (G_OBJECT (button), "sde-open-file"))
		return FALSE;
	recent_popup (button, o, NULL);
	return TRUE;
}

/* Delete removes the focused recent file from the list. */
static gboolean
card_key_pressed (GtkWidget *button, GdkEventKey *event, Overview *o)
{
	if (pin_card_key (button, event, o))
		return TRUE;
	if ((event->keyval == GDK_KEY_Delete || event->keyval == GDK_KEY_KP_Delete) &&
	    g_object_get_data (G_OBJECT (button), "sde-open-file")) {
		RecentAction a = { o, g_object_get_data (G_OBJECT (button), "sde-uri"), NULL };

		menu_remove (NULL, &a);
		return TRUE;
	}
	return FALSE;
}

/* -------------------------------------------------------------- widgets */

static GtkWidget *
label_new (const gchar *text, const gchar *style_class, gboolean ellipsize)
{
	GtkWidget *label = gtk_label_new (text);

	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	if (ellipsize) {
		gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars (GTK_LABEL (label), 1);
		gtk_widget_set_hexpand (label, TRUE);
	}
	if (style_class)
		gtk_style_context_add_class (gtk_widget_get_style_context (label), style_class);
	return label;
}

static GtkWidget *
card_new (Overview *o, const gchar *style_class, const gchar *uri, const gchar *tooltip)
{
	GtkWidget *button = gtk_button_new ();

	gtk_style_context_add_class (gtk_widget_get_style_context (button), "sde-card");
	gtk_style_context_add_class (gtk_widget_get_style_context (button), style_class);
	g_object_set_data_full (G_OBJECT (button), "sde-uri", g_strdup (uri), g_free);
	if (tooltip)
		gtk_widget_set_tooltip_text (button, tooltip);
	g_signal_connect (button, "clicked", G_CALLBACK (card_clicked), o);
	g_signal_connect (button, "button-press-event", G_CALLBACK (card_pressed), o);
	g_signal_connect (button, "popup-menu", G_CALLBACK (card_popup_menu), o);
	g_signal_connect (button, "key-press-event", G_CALLBACK (card_key_pressed), o);
	return button;
}

static GtkWidget *
flow_new (guint min_per_line, guint max_per_line, const gchar *style_class)
{
	GtkWidget *flow = gtk_flow_box_new ();

	gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
	gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), min_per_line);
	gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), max_per_line);
	gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 6);
	gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 8);
	gtk_style_context_add_class (gtk_widget_get_style_context (flow), style_class);
	return flow;
}

static void
flow_add (GtkWidget *flow, GtkWidget *child)
{
	gtk_container_add (GTK_CONTAINER (flow), child);
	/* The card is the focusable button, not the flow box child around it. */
	if (GTK_IS_FLOW_BOX_CHILD (gtk_widget_get_parent (child)))
		gtk_widget_set_can_focus (gtk_widget_get_parent (child), FALSE);
}

/* Drive and recent file cards: 220 px at 100 %, grown with the interface
 * scale (Xft/DPI) so that "12.3 GB free of 45.6 GB" fits. */
static gint
card_min_width (void)
{
	gint dpi = -1;

	g_object_get (gtk_settings_get_default (), "gtk-xft-dpi", &dpi, NULL);
	return dpi > 96 * 1024 ? 220 * (dpi / 1024) / 96 : 220;
}

static void
clear_container (GtkWidget *container)
{
	GList *children = gtk_container_get_children (GTK_CONTAINER (container));

	for (GList *l = children; l; l = l->next)
		gtk_widget_destroy (l->data);
	g_list_free (children);
}

/* ---------------------------------------------------------- quick access */

/* Item count of a folder, counted in the background. */
typedef struct {
	GtkWidget *label;
	GCancellable *cancellable;
	guint count;
} Count;

static void
count_free (Count *c)
{
	g_object_unref (c->label);
	g_object_unref (c->cancellable);
	g_free (c);
}

static void
count_show (Count *c)
{
	gchar *text = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u item", "%u items", c->count), c->count);

	gtk_label_set_text (GTK_LABEL (c->label), text);
	g_free (text);
}

static void
count_next (GObject *source, GAsyncResult *result, gpointer data)
{
	Count *c = data;
	GFileEnumerator *enumerator = G_FILE_ENUMERATOR (source);
	GList *files = g_file_enumerator_next_files_finish (enumerator, result, NULL);

	if (g_cancellable_is_cancelled (c->cancellable)) {
		g_list_free_full (files, g_object_unref);
		count_free (c);
		return;
	}
	if (!files) {
		count_show (c);
		count_free (c);
		return;
	}
	for (GList *l = files; l; l = l->next)
		if (!g_file_info_get_attribute_boolean (l->data, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) &&
		    !g_file_info_get_attribute_boolean (l->data, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP))
			c->count++;
	g_list_free_full (files, g_object_unref);
	if (c->count >= 10000) {
		count_show (c);
		count_free (c);
		return;
	}
	g_file_enumerator_next_files_async (enumerator, 200, G_PRIORITY_LOW, c->cancellable, count_next, c);
}

static void
count_enumerated (GObject *source, GAsyncResult *result, gpointer data)
{
	Count *c = data;
	GFileEnumerator *enumerator = g_file_enumerate_children_finish (G_FILE (source), result, NULL);

	if (!enumerator || g_cancellable_is_cancelled (c->cancellable)) {
		if (enumerator)
			g_object_unref (enumerator);
		count_free (c);
		return;
	}
	g_file_enumerator_next_files_async (enumerator, 200, G_PRIORITY_LOW, c->cancellable, count_next, c);
	g_object_unref (enumerator);
}

static void
count_items (GCancellable *cancellable, GFile *folder, GtkWidget *label)
{
	Count *c = g_new0 (Count, 1);

	c->label = g_object_ref (label);
	c->cancellable = g_object_ref (cancellable);
	g_file_enumerate_children_async (folder, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
	                                 G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP,
	                                 G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW, cancellable, count_enumerated, c);
}

/* What a pinned card points to, known once its information has arrived. */
enum {
	PIN_UNKNOWN,
	PIN_FOLDER,   /* folders, Trash, mountable locations: open in Liria */
	PIN_FILE,     /* documents, images, archives...: the default application */
	PIN_OFFLINE,  /* not mounted (remote or a removable drive): Liria mounts it */
	PIN_MISSING   /* deleted or moved: offer to remove the pin */
};

#define PIN_TARGET "application/x-liria-quick-access-pin"

enum { DROP_PIN, DROP_URIS };

/* Pin cards are dragged to a new place (the pin target) or, like every
 * other card, into Liria, the desktop and other applications. */
static const GtkTargetEntry pin_source_targets[] = {
	{ (gchar *) PIN_TARGET, GTK_TARGET_SAME_APP, DROP_PIN },
	{ (gchar *) "text/uri-list", 0, DROP_URIS },
};

static const GtkTargetEntry card_source_targets[] = {
	{ (gchar *) "text/uri-list", 0, DROP_URIS },
};

static const GtkTargetEntry file_drop_targets[] = {
	{ (gchar *) "text/uri-list", 0, DROP_URIS },
};

static const GtkTargetEntry pin_drop_targets[] = {
	{ (gchar *) PIN_TARGET, GTK_TARGET_SAME_APP, DROP_PIN },
	{ (gchar *) "text/uri-list", 0, DROP_URIS },
};

static void fill_quick_access (Overview *o);
static void set_drop_card (Overview *o, GtkWidget *card);
static gint recent_newer_first (gconstpointer a, gconstpointer b);

static gint
pin_kind (GtkWidget *card)
{
	return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (card), "sde-pin-kind"));
}

static void
set_pin_kind (GtkWidget *card, gint kind)
{
	g_object_set_data (G_OBJECT (card), "sde-pin-kind", GINT_TO_POINTER (kind));
	g_object_set_data (G_OBJECT (card), "sde-open-file", GINT_TO_POINTER (kind == PIN_FILE));
}

static gboolean
reload_timeout (gpointer data)
{
	Overview *o = data;

	o->reload_id = 0;
	if (!o->destroyed)
		fill_quick_access (o);
	return G_SOURCE_REMOVE;
}

/* Several changes in a row (our own save and the file monitor) rebuild once. */
static void
schedule_reload (Overview *o)
{
	if (!o->destroyed && o->reload_id == 0)
		o->reload_id = g_timeout_add (80, reload_timeout, o);
}

static void
pins_file_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, GtkWidget *root)
{
	Overview *o = g_object_get_data (G_OBJECT (root), "sde-overview");

	if (o && !o->destroyed && event != G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
		schedule_reload (o);
}

/* -- card contents */

typedef struct {
	GtkWidget *card;
	GtkWidget *image;
	GtkWidget *title;
	GtkWidget *subtitle;
	GCancellable *cancellable;
	gboolean keep_title;
	gboolean is_trash;
} PinQuery;

static void
pin_query_free (PinQuery *q)
{
	g_object_unref (q->card);
	g_object_unref (q->image);
	g_object_unref (q->title);
	g_object_unref (q->subtitle);
	g_object_unref (q->cancellable);
	g_free (q);
}

typedef struct {
	GtkWidget *image;
	GCancellable *cancellable;
} Thumb;

static void
thumb_free (Thumb *t)
{
	g_object_unref (t->image);
	g_object_unref (t->cancellable);
	g_free (t);
}

/* Previews fill the 36 px icon square, cropped to the middle. */
static void
set_square_preview (GtkWidget *image, GdkPixbuf *pixbuf)
{
	gint width = gdk_pixbuf_get_width (pixbuf), height = gdk_pixbuf_get_height (pixbuf);
	gint side = MIN (width, height);
	GdkPixbuf *square = gdk_pixbuf_new_subpixbuf (pixbuf, (width - side) / 2, (height - side) / 2, side, side);
	GdkPixbuf *scaled = gdk_pixbuf_scale_simple (square, 36, 36, GDK_INTERP_BILINEAR);

	gtk_image_set_from_pixbuf (GTK_IMAGE (image), scaled);
	gtk_style_context_add_class (gtk_widget_get_style_context (image), "sde-card-preview");
	g_object_unref (scaled);
	g_object_unref (square);
}

static void
thumb_loaded (GObject *source, GAsyncResult *result, gpointer data)
{
	Thumb *t = data;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_finish (result, NULL);

	if (pixbuf && !g_cancellable_is_cancelled (t->cancellable))
		set_square_preview (t->image, pixbuf);
	if (pixbuf)
		g_object_unref (pixbuf);
	g_input_stream_close (G_INPUT_STREAM (source), NULL, NULL);
	thumb_free (t);
}

static void
thumb_opened (GObject *source, GAsyncResult *result, gpointer data)
{
	Thumb *t = data;
	GFileInputStream *stream = g_file_read_finish (G_FILE (source), result, NULL);

	if (!stream || g_cancellable_is_cancelled (t->cancellable)) {
		if (stream)
			g_object_unref (stream);
		thumb_free (t);
		return;
	}
	gdk_pixbuf_new_from_stream_at_scale_async (G_INPUT_STREAM (stream), 96, 96, TRUE, t->cancellable,
	                                           thumb_loaded, t);
	g_object_unref (stream);
}

/* Pictures show a small preview: the thumbnail made by the file manager
 * when there is one, otherwise local pictures up to 16 MB are scaled. */
static void
pin_thumbnail (PinQuery *q, GFile *file, GFileInfo *info)
{
	const gchar *thumb = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
	const gchar *type = g_file_info_get_content_type (info);
	Thumb *t;

	if (thumb && *thumb) {
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_size (thumb, 96, 96, NULL);

		if (pixbuf) {
			set_square_preview (q->image, pixbuf);
			g_object_unref (pixbuf);
			return;
		}
	}
	if (!type || !g_str_has_prefix (type, "image/") || !g_file_is_native (file) ||
	    g_file_info_get_size (info) > 16 * 1024 * 1024)
		return;
	t = g_new0 (Thumb, 1);
	t->image = g_object_ref (q->image);
	t->cancellable = g_object_ref (q->cancellable);
	g_file_read_async (file, G_PRIORITY_LOW, t->cancellable, thumb_opened, t);
}

static void
pin_info_done (GObject *source, GAsyncResult *result, gpointer data)
{
	PinQuery *q = data;
	GFile *file = G_FILE (source);
	GError *error = NULL;
	GFileInfo *info = g_file_query_info_finish (file, result, &error);

	if (g_cancellable_is_cancelled (q->cancellable)) {
		/* the cards were rebuilt */
	} else if (!info && q->is_trash) {
		set_pin_kind (q->card, PIN_FOLDER);
		gtk_label_set_text (GTK_LABEL (q->subtitle), _("Deleted files"));
	} else if (!info) {
		gboolean offline = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED);
		gchar *where = g_file_get_parse_name (file);
		gchar *tip = g_strdup_printf (offline ? _("%s\nNot connected: open it to connect")
		                                      : _("%s\nNot available: it was moved, renamed or deleted"),
		                              where);

		set_pin_kind (q->card, offline ? PIN_OFFLINE : PIN_MISSING);
		gtk_label_set_text (GTK_LABEL (q->subtitle), _("Not available"));
		gtk_style_context_add_class (gtk_widget_get_style_context (q->card), "sde-card-missing");
		gtk_widget_set_tooltip_text (q->card, tip);
		g_free (tip);
		g_free (where);
	} else {
		GFileType type = g_file_info_get_file_type (info);
		GIcon *icon = g_file_info_get_icon (info);

		if (!q->keep_title && g_file_info_get_display_name (info))
			gtk_label_set_text (GTK_LABEL (q->title), g_file_info_get_display_name (info));
		if (icon)
			gtk_image_set_from_gicon (GTK_IMAGE (q->image), icon, GTK_ICON_SIZE_DIALOG);
		gtk_image_set_pixel_size (GTK_IMAGE (q->image), 36);
		if (q->is_trash) {
			set_pin_kind (q->card, PIN_FOLDER);
			gtk_label_set_text (GTK_LABEL (q->subtitle), _("Deleted files"));
		} else if (type == G_FILE_TYPE_DIRECTORY || type == G_FILE_TYPE_MOUNTABLE ||
		           type == G_FILE_TYPE_SHORTCUT) {
			set_pin_kind (q->card, PIN_FOLDER);
			if (type == G_FILE_TYPE_DIRECTORY)
				count_items (q->cancellable, file, q->subtitle);
			else
				gtk_label_set_text (GTK_LABEL (q->subtitle), _("Location"));
		} else {
			const gchar *content = g_file_info_get_content_type (info);
			gchar *kind = content ? g_content_type_get_description (content) : g_strdup (_("File"));
			gchar *size = g_format_size (g_file_info_get_size (info));
			/* Translators: a pinned file's size and type, "12.4 kB · PDF document" */
			gchar *text = g_strdup_printf (_("%s \xC2\xB7 %s"), size, kind);

			set_pin_kind (q->card, PIN_FILE);
			gtk_label_set_text (GTK_LABEL (q->subtitle), text);
			pin_thumbnail (q, file, info);
			g_free (text);
			g_free (size);
			g_free (kind);
		}
	}
	if (info)
		g_object_unref (info);
	g_clear_error (&error);
	pin_query_free (q);
}

/* -- drag and drop */

static void
card_drag_begin (GtkWidget *card, GdkDragContext *context, gpointer data)
{
	GtkAllocation allocation;
	cairo_surface_t *surface;
	cairo_t *cr;

	gtk_widget_get_allocation (card, &allocation);
	surface = gdk_window_create_similar_surface (gtk_widget_get_window (card), CAIRO_CONTENT_COLOR_ALPHA,
	                                             allocation.width, allocation.height);
	cr = cairo_create (surface);
	gtk_widget_draw (card, cr);
	cairo_destroy (cr);
	cairo_surface_set_device_offset (surface, -allocation.width / 2.0, -allocation.height / 2.0);
	gtk_drag_set_icon_surface (context, surface);
	cairo_surface_destroy (surface);
}

/* The pin itself (for a new place) or its location as a text/uri-list. */
static void
card_drag_data_get (GtkWidget *card, GdkDragContext *context, GtkSelectionData *data, guint info, guint time,
                    gpointer user_data)
{
	const gchar *uri = g_object_get_data (G_OBJECT (card), "sde-pin-uri");

	if (info == DROP_PIN && uri) {
		gtk_selection_data_set (data, gtk_selection_data_get_target (data), 8, (const guchar *) uri, strlen (uri));
		return;
	}
	uri = g_object_get_data (G_OBJECT (card), "sde-uri");
	if (uri) {
		gchar *uris[] = { (gchar *) uri, NULL };

		gtk_selection_data_set_uris (data, uris);
	}
}

/* Every card with a location can be dragged into Liria, onto the desktop or
 * into another application. Liria copies, moves or links what it receives
 * itself, so nothing here is ever deleted after the drop. */
static void
card_drag_source (GtkWidget *card, gboolean pin)
{
	if (pin)
		gtk_drag_source_set (card, GDK_BUTTON1_MASK | GDK_BUTTON2_MASK, pin_source_targets,
		                     G_N_ELEMENTS (pin_source_targets), GDK_ACTION_COPY | GDK_ACTION_LINK | GDK_ACTION_ASK);
	else
		gtk_drag_source_set (card, GDK_BUTTON1_MASK | GDK_BUTTON2_MASK, card_source_targets,
		                     G_N_ELEMENTS (card_source_targets), GDK_ACTION_COPY | GDK_ACTION_LINK | GDK_ACTION_ASK);
	g_signal_connect (card, "drag-begin", G_CALLBACK (card_drag_begin), NULL);
	g_signal_connect (card, "drag-data-get", G_CALLBACK (card_drag_data_get), NULL);
}

static gboolean
same_file_system (const gchar *a, const gchar *b)
{
	const gchar *uris[] = { a, b };
	gchar *ids[2] = { NULL, NULL };
	gboolean same;

	for (guint i = 0; i < 2; i++) {
		GFile *file = g_file_new_for_uri (uris[i]);
		GFileInfo *info = g_file_query_info (file, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NONE, NULL, NULL);

		if (info) {
			ids[i] = g_strdup (g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILESYSTEM));
			g_object_unref (info);
		}
		g_object_unref (file);
	}
	same = ids[0] && ids[1] && g_strcmp0 (ids[0], ids[1]) == 0;
	g_free (ids[0]);
	g_free (ids[1]);
	return same;
}

/* What a file drop does, as in Liria's folder views: Shift moves, Ctrl
 * copies, Ctrl+Shift links, Alt or a middle-button drag asks; otherwise
 * files on the same file system are moved and others copied. */
static GdkDragAction
file_drop_action (GtkWidget *widget, GdkDragContext *context, const gchar *first_uri, const gchar *target)
{
	GdkModifierType mask = 0;
	GdkDevice *device = gdk_drag_context_get_device (context);
	GdkWindow *window = gtk_widget_get_window (widget);

	/* GTK suggests "ask" for middle-button drags; Liria marks its
	 * right-button drags on the display. */
	if (gdk_drag_context_get_suggested_action (context) == GDK_ACTION_ASK ||
	    g_object_get_data (G_OBJECT (gdk_display_get_default ()), "sde-drag-ask"))
		return GDK_ACTION_ASK;
	if (device && window)
		gdk_window_get_device_position (window, device, NULL, NULL, &mask);
	if (mask & GDK_MOD1_MASK)
		return GDK_ACTION_ASK;
	if ((mask & GDK_CONTROL_MASK) && (mask & GDK_SHIFT_MASK))
		return GDK_ACTION_LINK;
	if (mask & GDK_CONTROL_MASK)
		return GDK_ACTION_COPY;
	if (mask & GDK_SHIFT_MASK)
		return GDK_ACTION_MOVE;
	if (first_uri && target && same_file_system (first_uri, target))
		return GDK_ACTION_MOVE;
	return GDK_ACTION_COPY;
}

/* Hands the files to the window's "sde-drop" action (liria-sde-ui.c),
 * which runs Liria's own copy, move or link with progress and undo, and
 * asks with the Move Here / Copy Here / Link Here menu for "ask". */
static void
drop_files (gchar **uris, const gchar *target, GdkDragAction action)
{
	GApplication *app = g_application_get_default ();
	GVariantBuilder list;
	const gchar *name = action == GDK_ACTION_ASK ? "ask"
	                    : action == GDK_ACTION_LINK ? "link"
	                    : action == GDK_ACTION_COPY ? "copy" : "move";

	if (!app || !uris || !uris[0] || !target)
		return;
	g_variant_builder_init (&list, G_VARIANT_TYPE ("as"));
	for (guint i = 0; uris[i]; i++)
		g_variant_builder_add (&list, "s", uris[i]);
	if (g_action_group_has_action (G_ACTION_GROUP (app), "sde-drop"))
		g_action_group_activate_action (G_ACTION_GROUP (app), "sde-drop",
		                                g_variant_new ("(asss)", &list, target, name));
	else
		g_action_group_activate_action (G_ACTION_GROUP (app), "sde-copy-to", g_variant_new ("(ass)", &list, target));
}

/* The card under (x, y) of the Quick access area and where it lies. */
static GtkWidget *
quick_card_at (Overview *o, gint x, gint y, LiriaSdeDropTarget *where, gint *index)
{
	GtkFlowBoxChild *child = gtk_flow_box_get_child_at_pos (GTK_FLOW_BOX (o->quick), x, y);
	GtkWidget *card = child ? gtk_bin_get_child (GTK_BIN (child)) : NULL;
	GtkAllocation allocation;
	gint edge;

	*where = LIRIA_SDE_DROP_AREA;
	*index = -1;
	if (!card)
		return NULL;
	gtk_widget_get_allocation (GTK_WIDGET (child), &allocation);
	if (g_object_get_data (G_OBJECT (card), "sde-pin-uri"))
		*index = liria_sde_drop_pin_index (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (card), "sde-pin-index")),
		                                   x, allocation.x, allocation.width);
	if (!g_object_get_data (G_OBJECT (card), "sde-uri"))
		return card; /* the "+" card */
	/* The outer fifth of a card is a gap between two cards. */
	edge = allocation.width / 5;
	if (x < allocation.x + edge || x > allocation.x + allocation.width - edge)
		return card;
	if (pin_kind (card) == PIN_FOLDER || g_object_get_data (G_OBJECT (card), "sde-frequent"))
		*where = LIRIA_SDE_DROP_FOLDER_CARD;
	else if (g_object_get_data (G_OBJECT (card), "sde-pin-uri"))
		*where = LIRIA_SDE_DROP_FILE_CARD;
	return card;
}

static void
set_drop_card (Overview *o, GtkWidget *card)
{
	if (o->drop_card == card)
		return;
	if (o->drop_card) {
		gtk_style_context_remove_class (gtk_widget_get_style_context (o->drop_card), "sde-drop-target");
		g_object_remove_weak_pointer (G_OBJECT (o->drop_card), (gpointer *) &o->drop_card);
	}
	o->drop_card = card;
	if (card) {
		gtk_style_context_add_class (gtk_widget_get_style_context (card), "sde-drop-target");
		g_object_add_weak_pointer (G_OBJECT (card), (gpointer *) &o->drop_card);
	}
}

static gboolean
quick_drag_motion (GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint time, Overview *o)
{
	GdkAtom target = gtk_drag_dest_find_target (widget, context, NULL);
	gboolean from_pin = target == gdk_atom_intern_static_string (PIN_TARGET);
	LiriaSdeDropTarget where;
	gint index;
	GtkWidget *card;

	if (target == GDK_NONE) {
		gdk_drag_status (context, 0, time);
		return FALSE;
	}
	card = quick_card_at (o, x, y, &where, &index);
	switch (liria_sde_drop_decide (where, from_pin, 1)) {
	case LIRIA_SDE_DROP_FILES: {
		GdkDragAction action = gdk_drag_context_get_suggested_action (context);

		set_drop_card (o, card);
		gtk_drag_unhighlight (widget);
		/* Shown as the source suggests; the real action is chosen on drop. */
		gdk_drag_status (context, action ? action : GDK_ACTION_COPY, time);
		return TRUE;
	}
	case LIRIA_SDE_DROP_PIN:
	case LIRIA_SDE_DROP_REORDER:
		set_drop_card (o, NULL);
		gtk_drag_highlight (widget);
		gdk_drag_status (context, gdk_drag_context_get_actions (context) & GDK_ACTION_LINK ? GDK_ACTION_LINK
		                          : GDK_ACTION_COPY, time);
		return TRUE;
	default:
		gdk_drag_status (context, 0, time);
		return TRUE;
	}
}

static void
quick_drag_leave (GtkWidget *widget, GdkDragContext *context, guint time, Overview *o)
{
	set_drop_card (o, NULL);
	gtk_drag_unhighlight (widget);
}

static gboolean
quick_drag_drop (GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint time, Overview *o)
{
	GdkAtom target = gtk_drag_dest_find_target (widget, context, NULL);

	if (target == GDK_NONE)
		return FALSE;
	gtk_drag_get_data (widget, context, target, time);
	return TRUE;
}

static void
quick_drag_data_received (GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *data,
                          guint info, guint time, Overview *o)
{
	LiriaSdeDropTarget where;
	gint index;
	GtkWidget *card = quick_card_at (o, x, y, &where, &index);
	gboolean done = FALSE;
	gchar **uris = info == DROP_URIS ? gtk_selection_data_get_uris (data) : NULL;
	guint n = uris ? g_strv_length (uris) : (gtk_selection_data_get_length (data) > 0 ? 1 : 0);

	set_drop_card (o, NULL);
	gtk_drag_unhighlight (widget);
	switch (liria_sde_drop_decide (where, info == DROP_PIN, n)) {
	case LIRIA_SDE_DROP_REORDER: {
		gchar *uri = g_strndup ((const gchar *) gtk_selection_data_get_data (data),
		                        gtk_selection_data_get_length (data));
		GPtrArray *pins = liria_sde_pins_load ();
		gint from = liria_sde_pins_find (pins, uri);

		if (from >= 0) {
			if (index < 0)
				index = pins->len;
			if (from < index)
				index--;
			g_free (o->focus_uri);
			o->focus_uri = g_strdup (uri);
			done = liria_sde_pins_move (uri, index);
		}
		g_ptr_array_unref (pins);
		g_free (uri);
		break;
	}
	case LIRIA_SDE_DROP_PIN:
		done = liria_sde_pins_add ((const gchar *const *) uris, index);
		break;
	case LIRIA_SDE_DROP_FILES: {
		const gchar *dest = card ? g_object_get_data (G_OBJECT (card), "sde-uri") : NULL;

		drop_files (uris, dest, file_drop_action (widget, context, uris[0], dest));
		break;
	}
	default:
		break;
	}
	/* Never a move for the source: Liria itself moves what it was given. */
	gtk_drag_finish (context, n > 0, FALSE, time);
	g_strfreev (uris);
	if (done)
		schedule_reload (o);
}

/* Drive cards: dropped files go into the drive. */
static gboolean
drive_drag_motion (GtkWidget *card, GdkDragContext *context, gint x, gint y, guint time, gpointer data)
{
	if (gtk_drag_dest_find_target (card, context, NULL) == GDK_NONE) {
		gdk_drag_status (context, 0, time);
		return FALSE;
	}
	gtk_style_context_add_class (gtk_widget_get_style_context (card), "sde-drop-target");
	gdk_drag_status (context, gdk_drag_context_get_suggested_action (context), time);
	return TRUE;
}

static void
drive_drag_leave (GtkWidget *card, GdkDragContext *context, guint time, gpointer data)
{
	gtk_style_context_remove_class (gtk_widget_get_style_context (card), "sde-drop-target");
}

static gboolean
drive_drag_drop (GtkWidget *card, GdkDragContext *context, gint x, gint y, guint time, gpointer data)
{
	GdkAtom target = gtk_drag_dest_find_target (card, context, NULL);

	if (target == GDK_NONE)
		return FALSE;
	gtk_drag_get_data (card, context, target, time);
	return TRUE;
}

static void
drive_drag_data_received (GtkWidget *card, GdkDragContext *context, gint x, gint y, GtkSelectionData *data,
                          guint info, guint time, gpointer user_data)
{
	gchar **uris = gtk_selection_data_get_uris (data);
	const gchar *dest = g_object_get_data (G_OBJECT (card), "sde-uri");

	gtk_style_context_remove_class (gtk_widget_get_style_context (card), "sde-drop-target");
	if (uris && uris[0] && dest &&
	    liria_sde_drop_decide (LIRIA_SDE_DROP_DRIVE_CARD, FALSE, g_strv_length (uris)) == LIRIA_SDE_DROP_FILES)
		drop_files (uris, dest, file_drop_action (card, context, uris[0], dest));
	gtk_drag_finish (context, uris && uris[0], FALSE, time);
	g_strfreev (uris);
}

static void
drive_drag_dest (GtkWidget *card)
{
	gtk_drag_dest_set (card, 0, file_drop_targets, G_N_ELEMENTS (file_drop_targets),
	                   GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK);
	g_signal_connect (card, "drag-motion", G_CALLBACK (drive_drag_motion), NULL);
	g_signal_connect (card, "drag-leave", G_CALLBACK (drive_drag_leave), NULL);
	g_signal_connect (card, "drag-drop", G_CALLBACK (drive_drag_drop), NULL);
	g_signal_connect (card, "drag-data-received", G_CALLBACK (drive_drag_data_received), NULL);
}

/* -- actions */

static void
pin_open (Overview *o, GtkWidget *card)
{
	const gchar *uri = g_object_get_data (G_OBJECT (card), "sde-pin-uri");

	activate_uri (o->window, uri, pin_kind (card) == PIN_FILE);
}

static GtkWindow *
parent_window (Overview *o)
{
	GtkWidget *toplevel = o->window ? gtk_widget_get_toplevel (o->window) : NULL;

	return GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL;
}

static void
choose_and_pin (Overview *o)
{
	GtkWidget *dialog = gtk_file_chooser_dialog_new (_("Pin to Quick Access"), parent_window (o),
	                                                 GTK_FILE_CHOOSER_ACTION_OPEN,
	                                                 _("_Cancel"), GTK_RESPONSE_CANCEL, NULL);
	/* Not an "accept" response: the chooser then returns a selected
	 * folder instead of opening it. */
	GtkWidget *pin = gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Pin"), 1);

	gtk_style_context_add_class (gtk_widget_get_style_context (pin), "suggested-action");
	gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER (dialog), TRUE);
	gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (dialog), FALSE);
	gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), g_get_home_dir ());
	g_signal_connect_swapped (dialog, "file-activated", G_CALLBACK (gtk_dialog_response), GINT_TO_POINTER (1));
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == 1) {
		GSList *uris = gtk_file_chooser_get_uris (GTK_FILE_CHOOSER (dialog));
		GPtrArray *list = g_ptr_array_new_with_free_func (g_free);

		if (!uris)
			uris = g_slist_prepend (NULL, gtk_file_chooser_get_current_folder_uri (GTK_FILE_CHOOSER (dialog)));
		for (GSList *l = uris; l; l = l->next)
			if (l->data)
				g_ptr_array_add (list, l->data);
		g_ptr_array_add (list, NULL);
		if (liria_sde_pins_add ((const gchar *const *) list->pdata, -1) && !o->destroyed)
			schedule_reload (o);
		g_slist_free (uris);
		g_ptr_array_unref (list);
	}
	gtk_widget_destroy (dialog);
}

static void
rename_pin (Overview *o, const gchar *uri, const gchar *title)
{
	GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Rename Card"), parent_window (o),
	                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	                                                 _("_Cancel"), GTK_RESPONSE_CANCEL,
	                                                 _("_Rename"), GTK_RESPONSE_ACCEPT, NULL);
	GtkWidget *area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *entry = gtk_entry_new ();
	GtkWidget *label = gtk_label_new_with_mnemonic (_("_Name shown in Quick access:"));
	GtkWidget *hint = gtk_label_new (_("Leave it empty to use the name of the file or folder."));

	gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
	gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
	gtk_entry_set_text (GTK_ENTRY (entry), title ? title : "");
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	gtk_container_set_border_width (GTK_CONTAINER (box), 12);
	gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), hint, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (area), box, TRUE, TRUE, 0);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
	gtk_window_set_default_size (GTK_WINDOW (dialog), 360, -1);
	gtk_widget_show_all (box);
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
		g_free (o->focus_uri);
		o->focus_uri = g_strdup (uri);
		if (liria_sde_pins_set_label (uri, gtk_entry_get_text (GTK_ENTRY (entry))) && !o->destroyed)
			schedule_reload (o);
	}
	gtk_widget_destroy (dialog);
}

static void
reset_pins (Overview *o)
{
	GtkWidget *dialog = gtk_message_dialog_new (parent_window (o), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	                                            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
	                                            _("Reset Quick access to the default folders?"));

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
	                                          _("Your pins are removed and Desktop, Documents, Downloads, "
	                                            "Pictures, Music, Videos and Trash are shown again. No files "
	                                            "are deleted."));
	gtk_dialog_add_buttons (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL,
	                        _("_Reset"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT && liria_sde_pins_reset () && !o->destroyed)
		schedule_reload (o);
	gtk_widget_destroy (dialog);
}

typedef enum {
	TASK_OPEN, TASK_OPEN_FOLDER, TASK_RENAME, TASK_LEFT, TASK_RIGHT, TASK_UNPIN, TASK_RESET, TASK_ADD, TASK_PIN
} TaskKind;

typedef struct {
	GtkWidget *root;
	TaskKind kind;
	gchar *uri;
	gchar *title;
	gchar *neighbour;
	gint index;
	gint pin_kind;
} PinTask;

static void
pin_task_free (PinTask *t)
{
	g_object_unref (t->root);
	g_free (t->uri);
	g_free (t->title);
	g_free (t->neighbour);
	g_free (t);
}

/* Runs after the menu has closed: the cards may be rebuilt. */
static gboolean
pin_task_idle (gpointer data)
{
	PinTask *t = data;
	Overview *o = g_object_get_data (G_OBJECT (t->root), "sde-overview");

	if (o && !o->destroyed) {
		switch (t->kind) {
		case TASK_OPEN:
			activate_uri (o->window, t->uri, t->pin_kind == PIN_FILE);
			break;
		case TASK_OPEN_FOLDER: {
			GFile *file = g_file_new_for_uri (t->uri), *parent = g_file_get_parent (file);

			if (parent) {
				gchar *parent_uri = g_file_get_uri (parent);

				activate_uri (o->window, parent_uri, FALSE);
				g_free (parent_uri);
				g_object_unref (parent);
			}
			g_object_unref (file);
			break;
		}
		case TASK_RENAME:
			rename_pin (o, t->uri, t->title);
			break;
		case TASK_LEFT:
		case TASK_RIGHT:
			g_free (o->focus_uri);
			o->focus_uri = g_strdup (t->uri);
			if (liria_sde_pins_move (t->uri, t->index + (t->kind == TASK_LEFT ? -1 : 1)))
				schedule_reload (o);
			break;
		case TASK_UNPIN: {
			const gchar *uris[] = { t->uri, NULL };

			g_free (o->focus_uri);
			o->focus_uri = g_strdup (t->neighbour ? t->neighbour : "sde:add");
			if (liria_sde_pins_remove (uris))
				schedule_reload (o);
			break;
		}
		case TASK_RESET:
			reset_pins (o);
			break;
		case TASK_ADD:
			choose_and_pin (o);
			break;
		case TASK_PIN: {
			const gchar *uris[] = { t->uri, NULL };

			g_free (o->focus_uri);
			o->focus_uri = g_strdup (t->uri);
			if (liria_sde_pins_add (uris, -1))
				schedule_reload (o);
			break;
		}
		}
	}
	pin_task_free (t);
	return G_SOURCE_REMOVE;
}

static PinTask *
pin_task_new (Overview *o, GtkWidget *card, TaskKind kind)
{
	PinTask *t = g_new0 (PinTask, 1);

	t->root = g_object_ref (o->root);
	t->kind = kind;
	if (card) {
		t->uri = g_strdup (g_object_get_data (G_OBJECT (card), "sde-pin-uri"));
		if (!t->uri)
			t->uri = g_strdup (g_object_get_data (G_OBJECT (card), "sde-uri"));
		t->title = g_strdup (g_object_get_data (G_OBJECT (card), "sde-pin-label"));
		t->neighbour = g_strdup (g_object_get_data (G_OBJECT (card), "sde-pin-neighbour"));
		t->index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (card), "sde-pin-index"));
		t->pin_kind = pin_kind (card);
	}
	return t;
}

static void
run_task (Overview *o, GtkWidget *card, TaskKind kind)
{
	g_idle_add (pin_task_idle, pin_task_new (o, card, kind));
}

static void
task_item_activated (GtkMenuItem *item, PinTask *t)
{
	PinTask *copy = g_new0 (PinTask, 1);

	*copy = *t;
	copy->root = g_object_ref (t->root);
	copy->uri = g_strdup (t->uri);
	copy->title = g_strdup (t->title);
	copy->neighbour = g_strdup (t->neighbour);
	g_idle_add (pin_task_idle, copy);
}

static GtkWidget *
task_item (GtkWidget *menu, const gchar *label, Overview *o, GtkWidget *card, TaskKind kind, gboolean sensitive)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic (label);

	g_signal_connect_data (item, "activate", G_CALLBACK (task_item_activated), pin_task_new (o, card, kind),
	                       (GClosureNotify) pin_task_free, 0);
	gtk_widget_set_sensitive (item, sensitive);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	return item;
}

static void
popup_menu (GtkWidget *menu, GtkWidget *widget, const GdkEvent *event)
{
	gtk_style_context_add_class (gtk_widget_get_style_context (menu), "sde-quick-menu");
	gtk_widget_show_all (menu);
	gtk_menu_attach_to_widget (GTK_MENU (menu), widget, NULL);
	g_signal_connect (menu, "deactivate", G_CALLBACK (menu_deactivated), NULL);
	if (event)
		gtk_menu_popup_at_pointer (GTK_MENU (menu), event);
	else
		gtk_menu_popup_at_widget (GTK_MENU (menu), widget, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

/* The "+" card and the empty card area: add a pin or reset the list. */
static void
add_popup (Overview *o, GtkWidget *widget, const GdkEvent *event)
{
	GtkWidget *menu = gtk_menu_new ();

	task_item (menu, _("_Pin a Folder or File\xE2\x80\xA6"), o, NULL, TASK_ADD, TRUE);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
	task_item (menu, _("Reset to _Defaults\xE2\x80\xA6"), o, NULL, TASK_RESET, TRUE);
	popup_menu (menu, widget, event);
}

static gboolean
pin_card_popup (GtkWidget *card, Overview *o, const GdkEvent *event)
{
	GtkWidget *menu;
	gint kind = pin_kind (card), index, count;
	gboolean missing = kind == PIN_MISSING;

	if (g_object_get_data (G_OBJECT (card), "sde-add")) {
		add_popup (o, card, event);
		return TRUE;
	}
	if (g_object_get_data (G_OBJECT (card), "sde-frequent")) {
		menu = gtk_menu_new ();
		task_item (menu, _("_Open"), o, card, TASK_OPEN, TRUE);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
		task_item (menu, _("_Pin to Quick Access"), o, card, TASK_PIN, TRUE);
		popup_menu (menu, card, event);
		return TRUE;
	}
	if (!g_object_get_data (G_OBJECT (card), "sde-pin-uri"))
		return FALSE;
	index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (card), "sde-pin-index"));
	count = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (card), "sde-pin-count"));
	menu = gtk_menu_new ();
	if (!missing) {
		task_item (menu, _("_Open"), o, card, TASK_OPEN, TRUE);
		if (kind == PIN_FILE)
			task_item (menu, _("Open _Containing Folder"), o, card, TASK_OPEN_FOLDER, TRUE);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
	}
	task_item (menu, _("Re_name\xE2\x80\xA6"), o, card, TASK_RENAME, TRUE);
	task_item (menu, _("Move _Left"), o, card, TASK_LEFT, index > 0);
	task_item (menu, _("Move _Right"), o, card, TASK_RIGHT, index + 1 < count);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
	task_item (menu, missing ? _("_Remove from Quick Access") : _("_Unpin from Quick Access"), o, card, TASK_UNPIN,
	           TRUE);
	task_item (menu, _("_Pin a Folder or File\xE2\x80\xA6"), o, NULL, TASK_ADD, TRUE);
	task_item (menu, _("Reset to _Defaults\xE2\x80\xA6"), o, NULL, TASK_RESET, TRUE);
	popup_menu (menu, card, event);
	return TRUE;
}

static gboolean
pin_card_clicked (GtkWidget *card, Overview *o)
{
	if (g_object_get_data (G_OBJECT (card), "sde-add")) {
		run_task (o, NULL, TASK_ADD);
		return TRUE;
	}
	if (!g_object_get_data (G_OBJECT (card), "sde-pin-uri"))
		return FALSE;
	if (pin_kind (card) == PIN_MISSING)
		pin_card_popup (card, o, NULL);
	else
		pin_open (o, card);
	return TRUE;
}

/* Delete unpins, F2 renames, Ctrl+Shift+Left/Right moves the card. */
static gboolean
pin_card_key (GtkWidget *card, GdkEventKey *event, Overview *o)
{
	GdkModifierType mods = event->state & gtk_accelerator_get_default_mod_mask ();
	gint index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (card), "sde-pin-index"));
	gint count = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (card), "sde-pin-count"));

	if (!g_object_get_data (G_OBJECT (card), "sde-pin-uri"))
		return FALSE;
	if ((event->keyval == GDK_KEY_Delete || event->keyval == GDK_KEY_KP_Delete) && mods == 0) {
		run_task (o, card, TASK_UNPIN);
		return TRUE;
	}
	if (event->keyval == GDK_KEY_F2 && mods == 0) {
		run_task (o, card, TASK_RENAME);
		return TRUE;
	}
	if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
	    (event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)) {
		if (index > 0)
			run_task (o, card, TASK_LEFT);
		return TRUE;
	}
	if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
	    (event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)) {
		if (index + 1 < count)
			run_task (o, card, TASK_RIGHT);
		return TRUE;
	}
	return FALSE;
}

static gboolean
quick_area_pressed (GtkWidget *area, GdkEventButton *event, Overview *o)
{
	if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
		add_popup (o, area, (GdkEvent *) event);
		return TRUE;
	}
	return FALSE;
}

/* -- cards */

static GtkWidget *
card_box (GtkWidget *card, GtkWidget *image, GtkWidget *title, GtkWidget *subtitle)
{
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *overlay = gtk_overlay_new ();

	gtk_image_set_pixel_size (GTK_IMAGE (image), 32);
	gtk_widget_set_halign (image, GTK_ALIGN_START);
	gtk_style_context_add_class (gtk_widget_get_style_context (image), "sde-card-icon");
	gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), title, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), subtitle, FALSE, FALSE, 0);
	gtk_container_add (GTK_CONTAINER (overlay), box);
	gtk_container_add (GTK_CONTAINER (card), overlay);
	g_object_set_data (G_OBJECT (card), "sde-overlay", overlay);
	return box;
}

/* A small marker in the card's top right corner: the pin of a pinned
 * card, or the Pin button of a suggested folder (shown on hover). */
static void
card_marker (GtkWidget *card, GtkWidget *marker, const gchar *style_class)
{
	GtkWidget *overlay = g_object_get_data (G_OBJECT (card), "sde-overlay");

	gtk_widget_set_halign (marker, GTK_ALIGN_END);
	gtk_widget_set_valign (marker, GTK_ALIGN_START);
	gtk_style_context_add_class (gtk_widget_get_style_context (marker), style_class);
	gtk_overlay_add_overlay (GTK_OVERLAY (overlay), marker);
}

static GtkWidget *
pin_marker_new (void)
{
	GtkWidget *image = gtk_image_new_from_icon_name ("view-pin-symbolic", GTK_ICON_SIZE_MENU);

	gtk_image_set_pixel_size (GTK_IMAGE (image), 12);
	gtk_widget_set_tooltip_text (image, _("Pinned"));
	return image;
}

static GtkWidget *
pin_card (Overview *o, GPtrArray *pins, guint index)
{
	LiriaSdePin *pin = pins->pdata[index];
	GFile *file = g_file_new_for_uri (pin->uri), *home = g_file_new_for_path (g_get_home_dir ());
	gchar *where = g_file_get_parse_name (file), *base = g_file_get_basename (file), *name;
	gboolean is_trash = g_str_has_prefix (pin->uri, "trash:"), is_home = g_file_equal (file, home);
	GtkWidget *card = card_new (o, "sde-quick-card", pin->uri, is_trash ? _("Trash") : where);
	GtkWidget *image, *title, *subtitle;
	PinQuery *q;

	if (pin->label)
		name = g_strdup (pin->label);
	else if (is_trash)
		name = g_strdup (_("Trash"));
	else if (is_home)
		name = g_strdup (_("Home"));
	else if (base && g_strcmp0 (base, "/") != 0)
		name = g_filename_display_name (base);
	else
		name = g_strdup (where);
	if (is_trash)
		image = gtk_image_new_from_icon_name ("user-trash", GTK_ICON_SIZE_DIALOG);
	else if (base && strchr (base, '.')) {
		gchar *type = g_content_type_guess (base, NULL, 0, NULL);
		GIcon *icon = g_content_type_get_icon (type);

		image = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DIALOG);
		g_object_unref (icon);
		g_free (type);
	} else
		image = gtk_image_new_from_icon_name (is_home ? "user-home" : "folder", GTK_ICON_SIZE_DIALOG);
	title = label_new (name, "sde-card-title", TRUE);
	subtitle = label_new (" ", "sde-card-subtitle", TRUE);
	card_box (card, image, title, subtitle);
	atk_object_set_name (gtk_widget_get_accessible (card), name);

	g_object_set_data_full (G_OBJECT (card), "sde-pin-uri", g_strdup (pin->uri), g_free);
	g_object_set_data_full (G_OBJECT (card), "sde-pin-label", g_strdup (name), g_free);
	g_object_set_data (G_OBJECT (card), "sde-pin-index", GINT_TO_POINTER (index));
	g_object_set_data (G_OBJECT (card), "sde-pin-count", GINT_TO_POINTER (pins->len));
	if (index + 1 < pins->len || index > 0)
		g_object_set_data_full (G_OBJECT (card), "sde-pin-neighbour",
		                        g_strdup (((LiriaSdePin *) pins->pdata[index + 1 < pins->len ? index + 1 : index - 1])->uri),
		                        g_free);
	set_pin_kind (card, PIN_UNKNOWN);
	card_marker (card, pin_marker_new (), "sde-card-pin");
	card_drag_source (card, TRUE);

	q = g_new0 (PinQuery, 1);
	q->card = g_object_ref (card);
	q->image = g_object_ref (image);
	q->title = g_object_ref (title);
	q->subtitle = g_object_ref (subtitle);
	q->cancellable = g_object_ref (o->quick_cancellable);
	q->keep_title = pin->label || is_trash || is_home;
	q->is_trash = is_trash;
	g_file_query_info_async (file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
	                         G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
	                         G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_THUMBNAIL_PATH,
	                         G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW, q->cancellable, pin_info_done, q);
	g_free (name);
	g_free (base);
	g_free (where);
	g_object_unref (home);
	g_object_unref (file);
	return card;
}

static GtkWidget *
add_card (Overview *o, gboolean empty)
{
	GtkWidget *card = card_new (o, "sde-quick-card", NULL,
	                            _("Pin a folder, file or location to Quick access. You can also drop "
	                              "files here or use \"Pin to Quick Access\" in their menu."));
	GtkWidget *image = gtk_image_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_DIALOG);

	gtk_style_context_add_class (gtk_widget_get_style_context (card), "sde-quick-add");
	g_object_set_data (G_OBJECT (card), "sde-add", GINT_TO_POINTER (TRUE));
	card_box (card, image, label_new (_("Add"), "sde-card-title", TRUE),
	          label_new (empty ? _("Pin a folder") : _("Pin more"), "sde-card-subtitle", TRUE));
	atk_object_set_name (gtk_widget_get_accessible (card), _("Pin a folder or file to Quick access"));
	return card;
}

static void
frequent_pin_clicked (GtkButton *button, Overview *o)
{
	GtkWidget *card = g_object_get_data (G_OBJECT (button), "sde-card");

	if (card)
		run_task (o, card, TASK_PIN);
}

/* A folder suggested from recently used files: marked "Frequent", with a
 * Pin button on hover and "Pin to Quick Access" in its menu. */
static GtkWidget *
frequent_card (Overview *o, const gchar *uri)
{
	GFile *file = g_file_new_for_uri (uri);
	gchar *where = g_file_get_parse_name (file), *base = g_file_get_basename (file);
	gchar *name = base ? g_filename_display_name (base) : g_strdup (where);
	gchar *tip = g_strdup_printf (_("%s\nOften used: pin it to keep it in Quick access"), where);
	GtkWidget *card = card_new (o, "sde-quick-card", uri, tip);
	GtkWidget *image = gtk_image_new_from_icon_name ("folder", GTK_ICON_SIZE_DIALOG);
	GtkWidget *pin = gtk_button_new_from_icon_name ("view-pin-symbolic", GTK_ICON_SIZE_MENU);

	gtk_style_context_add_class (gtk_widget_get_style_context (card), "sde-quick-frequent");
	card_box (card, image, label_new (name, "sde-card-title", TRUE),
	          label_new (_("Frequent folder"), "sde-card-subtitle", TRUE));
	g_object_set_data (G_OBJECT (card), "sde-frequent", GINT_TO_POINTER (TRUE));
	gtk_button_set_relief (GTK_BUTTON (pin), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click (pin, FALSE);
	gtk_widget_set_tooltip_text (pin, _("Pin to Quick Access"));
	g_object_set_data (G_OBJECT (pin), "sde-card", card);
	g_signal_connect (pin, "clicked", G_CALLBACK (frequent_pin_clicked), o);
	card_marker (card, pin, "sde-card-pin-button");
	card_drag_source (card, FALSE);
	atk_object_set_name (gtk_widget_get_accessible (card), name);
	g_free (tip);
	g_free (name);
	g_free (base);
	g_free (where);
	g_object_unref (file);
	return card;
}

/* Parent folders of the recently used files, without the pins. */
static GPtrArray *
frequent_folders (GPtrArray *pins)
{
	GList *items = g_list_sort (gtk_recent_manager_get_items (gtk_recent_manager_get_default ()),
	                            recent_newer_first);
	GPtrArray *recent = g_ptr_array_new (), *pinned = g_ptr_array_new (), *folders;
	gchar *home = g_filename_to_uri (g_get_home_dir (), NULL, NULL);

	for (GList *l = items; l; l = l->next)
		g_ptr_array_add (recent, (gpointer) gtk_recent_info_get_uri (l->data));
	g_ptr_array_add (recent, NULL);
	for (guint i = 0; i < pins->len; i++)
		g_ptr_array_add (pinned, ((LiriaSdePin *) pins->pdata[i])->uri);
	g_ptr_array_add (pinned, NULL);
	folders = liria_sde_home_frequent_folders ((const gchar *const *) recent->pdata,
	                                           (const gchar *const *) pinned->pdata, home, 4);
	/* Only folders that still exist. */
	for (guint i = folders->len; i > 0; i--) {
		gchar *path = g_filename_from_uri (folders->pdata[i - 1], NULL, NULL);

		if (!path || !g_file_test (path, G_FILE_TEST_IS_DIR))
			g_ptr_array_remove_index (folders, i - 1);
		g_free (path);
	}
	g_ptr_array_unref (recent);
	g_ptr_array_unref (pinned);
	g_list_free_full (items, (GDestroyNotify) gtk_recent_info_unref);
	g_free (home);
	return folders;
}

static void
fill_quick_access (Overview *o)
{
	GPtrArray *pins = liria_sde_pins_load (), *frequent;
	GtkWidget *focus = NULL, *plus;

	if (o->quick_cancellable) {
		g_cancellable_cancel (o->quick_cancellable);
		g_object_unref (o->quick_cancellable);
	}
	o->quick_cancellable = g_cancellable_new ();
	set_drop_card (o, NULL);
	clear_container (o->quick);
	for (guint i = 0; i < pins->len; i++) {
		GtkWidget *card = pin_card (o, pins, i);

		flow_add (o->quick, card);
		if (o->focus_uri && g_strcmp0 (o->focus_uri, ((LiriaSdePin *) pins->pdata[i])->uri) == 0)
			focus = card;
	}
	frequent = frequent_folders (pins);
	for (guint i = 0; i < frequent->len; i++)
		flow_add (o->quick, frequent_card (o, frequent->pdata[i]));
	g_ptr_array_unref (frequent);
	plus = add_card (o, pins->len == 0);
	flow_add (o->quick, plus);
	if (g_strcmp0 (o->focus_uri, "sde:add") == 0)
		focus = plus;
	gtk_widget_show_all (o->quick);
	if (focus)
		gtk_widget_grab_focus (focus);
	g_clear_pointer (&o->focus_uri, g_free);
	g_ptr_array_unref (pins);
}

static GtkWidget *
quick_access_new (Overview *o)
{
	GtkWidget *area = gtk_event_box_new ();
	GFile *file;
	gchar *path;

	o->quick = flow_new (2, 8, "sde-quick-access");
	gtk_event_box_set_visible_window (GTK_EVENT_BOX (area), FALSE);
	gtk_container_add (GTK_CONTAINER (area), o->quick);
	g_signal_connect (area, "button-press-event", G_CALLBACK (quick_area_pressed), o);
	/* Pins are moved, dropped locations pinned and files dropped on a
	 * folder card copied or moved into it (quick_drag_*). */
	gtk_drag_dest_set (o->quick, 0, pin_drop_targets, G_N_ELEMENTS (pin_drop_targets),
	                   GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK);
	g_signal_connect (o->quick, "drag-motion", G_CALLBACK (quick_drag_motion), o);
	g_signal_connect (o->quick, "drag-leave", G_CALLBACK (quick_drag_leave), o);
	g_signal_connect (o->quick, "drag-drop", G_CALLBACK (quick_drag_drop), o);
	g_signal_connect (o->quick, "drag-data-received", G_CALLBACK (quick_drag_data_received), o);
	fill_quick_access (o);

	/* The folder must exist for the change notification to be immediate. */
	path = g_build_filename (g_get_user_config_dir (), "sde", NULL);
	g_mkdir_with_parents (path, 0700);
	g_free (path);
	path = liria_sde_pins_path ();
	file = g_file_new_for_path (path);
	o->pins_monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, NULL);
	if (o->pins_monitor)
		g_signal_connect_object (o->pins_monitor, "changed", G_CALLBACK (pins_file_changed), o->root, 0);
	g_object_unref (file);
	g_free (path);
	return area;
}

/* --------------------------------------------------------------- drives */

typedef struct {
	GtkWidget *bar;
	GtkWidget *label;
	GCancellable *cancellable;
} Usage;

static void
usage_done (GObject *source, GAsyncResult *result, gpointer data)
{
	Usage *u = data;
	GFileInfo *info = g_file_query_filesystem_info_finish (G_FILE (source), result, NULL);

	if (info && !g_cancellable_is_cancelled (u->cancellable) &&
	    g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
		guint64 size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
		guint64 free_space = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

		if (size > 0) {
			gchar *free_text = g_format_size (free_space), *size_text = g_format_size (size);
			/* Translators: "102.4 GB free of 237.0 GB" */
			gchar *text = g_strdup_printf (_("%s free of %s"), free_text, size_text);

			gtk_level_bar_set_value (GTK_LEVEL_BAR (u->bar), (gdouble) (size - MIN (free_space, size)) / size);
			gtk_widget_show (u->bar);
			gtk_label_set_text (GTK_LABEL (u->label), text);
			g_free (text);
			g_free (size_text);
			g_free (free_text);
		}
	}
	if (info)
		g_object_unref (info);
	g_object_unref (u->bar);
	g_object_unref (u->label);
	g_object_unref (u->cancellable);
	g_free (u);
}

static GtkWidget *
drive_card (Overview *o, GFile *root, const gchar *name, GIcon *icon)
{
	gchar *uri = g_file_get_uri (root), *where = g_file_get_parse_name (root);
	GtkWidget *card = card_new (o, "sde-drive-card", uri, where);
	GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
	GtkWidget *image = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DND);
	GtkWidget *bar = gtk_level_bar_new_for_interval (0.0, 1.0);
	GtkWidget *detail = label_new (_("Drive"), "sde-card-subtitle", TRUE);
	Usage *u = g_new0 (Usage, 1);

	gtk_image_set_pixel_size (GTK_IMAGE (image), 36);
	gtk_style_context_add_class (gtk_widget_get_style_context (image), "sde-card-icon");
	/* Usage colours come from the theme: normal until 90 % full. */
	gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (bar), GTK_LEVEL_BAR_OFFSET_LOW);
	gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (bar), GTK_LEVEL_BAR_OFFSET_HIGH);
	gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (bar), GTK_LEVEL_BAR_OFFSET_FULL);
	gtk_level_bar_add_offset_value (GTK_LEVEL_BAR (bar), "sde-normal", 0.90);
	gtk_level_bar_add_offset_value (GTK_LEVEL_BAR (bar), "sde-critical", 1.0);
	gtk_style_context_add_class (gtk_widget_get_style_context (bar), "sde-usage-bar");
	gtk_widget_set_no_show_all (bar, TRUE);
	gtk_widget_set_valign (row, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
	gtk_box_pack_start (GTK_BOX (text), label_new (name, "sde-card-title", TRUE), FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (text), bar, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (text), detail, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (row), image, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (row), text, TRUE, TRUE, 0);
	gtk_container_add (GTK_CONTAINER (card), row);
	gtk_widget_set_size_request (card, card_min_width (), -1);
	card_drag_source (card, FALSE);
	drive_drag_dest (card);

	u->bar = g_object_ref (bar);
	u->label = g_object_ref (detail);
	u->cancellable = g_object_ref (o->cancellable);
	g_file_query_filesystem_info_async (root, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
	                                    G_PRIORITY_LOW, o->cancellable, usage_done, u);
	g_free (where);
	g_free (uri);
	return card;
}

static void
fill_drives (Overview *o)
{
	GList *mounts;
	GFile *root = g_file_new_for_path ("/");
	GIcon *icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk-system");

	clear_container (o->drives);
	flow_add (o->drives, drive_card (o, root, _("System Disk"), icon));
	g_object_unref (icon);
	g_object_unref (root);

	mounts = g_volume_monitor_get_mounts (o->monitor);
	for (GList *l = mounts; l; l = l->next) {
		GMount *mount = l->data;
		gchar *name;

		if (g_mount_is_shadowed (mount))
			continue;
		root = g_mount_get_default_location (mount);
		name = g_mount_get_name (mount);
		icon = g_mount_get_icon (mount);
		flow_add (o->drives, drive_card (o, root, name, icon));
		g_object_unref (icon);
		g_free (name);
		g_object_unref (root);
	}
	g_list_free_full (mounts, g_object_unref);
	gtk_widget_show_all (o->drives);
}

static void
mounts_changed (GtkWidget *root)
{
	Overview *o = g_object_get_data (G_OBJECT (root), "sde-overview");

	if (o && !o->destroyed)
		fill_drives (o);
}

/* --------------------------------------------------------- recent files */

static gint
recent_newer_first (gconstpointer a, gconstpointer b)
{
	time_t ta = gtk_recent_info_get_modified ((GtkRecentInfo *) a);
	time_t tb = gtk_recent_info_get_modified ((GtkRecentInfo *) b);

	return ta < tb ? 1 : (ta > tb ? -1 : 0);
}

static gchar *
when_text (time_t when)
{
	GDateTime *then = g_date_time_new_from_unix_local (when);
	GDateTime *now = g_date_time_new_now_local ();
	gint days = (g_date_time_get_day_of_year (now) - g_date_time_get_day_of_year (then)) +
	            365 * (g_date_time_get_year (now) - g_date_time_get_year (then));
	gchar *text;

	if (days <= 0)
		text = g_date_time_format (then, _("Today, %H:%M"));
	else if (days == 1)
		text = g_strdup (_("Yesterday"));
	else if (days < 7)
		text = g_date_time_format (then, "%A");
	else
		text = g_date_time_format (then, "%x");
	g_date_time_unref (now);
	g_date_time_unref (then);
	return text;
}

static void
fill_recent (Overview *o)
{
	GtkRecentManager *manager = gtk_recent_manager_get_default ();
	GList *items = g_list_sort (gtk_recent_manager_get_items (manager), recent_newer_first);
	guint shown = 0;

	clear_container (o->recent);
	for (GList *l = items; l && shown < 6; l = l->next) {
		GtkRecentInfo *info = l->data;
		const gchar *uri = gtk_recent_info_get_uri (info);
		GFile *file, *parent;
		GtkWidget *card, *row, *text, *image;
		gchar *name, *parent_uri, *when, *where;
		GIcon *icon;

		if (!g_str_has_prefix (uri, "file://") || !gtk_recent_info_exists (info))
			continue;
		file = g_file_new_for_uri (uri);
		parent = g_file_get_parent (file);
		where = g_file_get_parse_name (file);
		card = card_new (o, "sde-recent-card", uri, where);
		g_object_set_data (G_OBJECT (card), "sde-open-file", GINT_TO_POINTER (TRUE));
		if (parent) {
			parent_uri = g_file_get_uri (parent);
			g_object_set_data_full (G_OBJECT (card), "sde-parent-uri", parent_uri, g_free);
		}
		row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
		text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		icon = gtk_recent_info_get_gicon (info);
		image = icon ? gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DND)
		             : gtk_image_new_from_icon_name ("text-x-generic", GTK_ICON_SIZE_DND);
		gtk_image_set_pixel_size (GTK_IMAGE (image), 28);
		name = g_strdup (gtk_recent_info_get_display_name (info));
		when = when_text (gtk_recent_info_get_modified (info));
		gtk_box_pack_start (GTK_BOX (text), label_new (name, "sde-card-title", TRUE), FALSE, FALSE, 0);
		gtk_box_pack_start (GTK_BOX (text), label_new (when, "sde-card-subtitle", TRUE), FALSE, FALSE, 0);
		gtk_box_pack_start (GTK_BOX (row), image, FALSE, FALSE, 0);
		gtk_box_pack_start (GTK_BOX (row), text, TRUE, TRUE, 0);
		gtk_container_add (GTK_CONTAINER (card), row);
		card_drag_source (card, FALSE);
		flow_add (o->recent, card);
		shown++;
		if (icon)
			g_object_unref (icon);
		g_free (when);
		g_free (name);
		g_free (where);
		if (parent)
			g_object_unref (parent);
		g_object_unref (file);
	}
	if (o->recent_clear)
		gtk_widget_set_visible (o->recent_clear, shown > 0);
	if (shown == 0) {
		GtkWidget *empty = label_new (_("Files you open will appear here."), "sde-home-empty", FALSE);
		flow_add (o->recent, empty);
	}
	g_list_free_full (items, (GDestroyNotify) gtk_recent_info_unref);
	gtk_widget_show_all (o->recent);
}

static void
recent_changed (GtkWidget *root)
{
	Overview *o = g_object_get_data (G_OBJECT (root), "sde-overview");

	if (o && !o->destroyed)
		fill_recent (o);
}

/* ------------------------------------------------------------- overview */

/* The overview is the Home view while it is shown: the window hides the
 * folder grid below it (no second copy of Desktop, Documents...). */
static void
apply_expanded (Overview *o)
{
	SdeReplacesView replaces = o->window ? g_object_get_data (G_OBJECT (o->window), "sde-home-replaces-view") : NULL;

	gtk_widget_set_visible (o->scroller, o->expanded);
	gtk_widget_set_vexpand (o->root, o->expanded);
	gtk_label_set_text (GTK_LABEL (o->toggle_label), o->expanded ? _("Hide overview") : _("Show overview"));
	gtk_image_set_from_icon_name (GTK_IMAGE (o->toggle_icon), o->expanded ? "pan-up-symbolic" : "pan-down-symbolic",
	                              GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text (gtk_widget_get_parent (gtk_widget_get_parent (o->toggle_label)),
	                             o->expanded ? _("Show the files of the home folder instead")
	                                         : _("Show Quick access, drives and recent files"));
	if (replaces && gtk_widget_get_parent (o->root))
		replaces (o->root, o->expanded);
}

static void
toggle_clicked (GtkButton *button, Overview *o)
{
	o->expanded = !o->expanded;
	apply_expanded (o);
	save_expanded (o->expanded);
}

static void
overview_mapped (GtkWidget *root, Overview *o)
{
	apply_expanded (o);
}

static void
overview_destroyed (GtkWidget *root, Overview *o)
{
	o->destroyed = TRUE;
	set_drop_card (o, NULL);
	g_cancellable_cancel (o->cancellable);
	if (o->quick_cancellable)
		g_cancellable_cancel (o->quick_cancellable);
	if (o->pins_monitor)
		g_file_monitor_cancel (o->pins_monitor);
	if (o->reload_id) {
		g_source_remove (o->reload_id);
		o->reload_id = 0;
	}
}

static void
overview_free (Overview *o)
{
	g_cancellable_cancel (o->cancellable);
	g_object_unref (o->cancellable);
	if (o->quick_cancellable) {
		g_cancellable_cancel (o->quick_cancellable);
		g_object_unref (o->quick_cancellable);
	}
	if (o->pins_monitor) {
		g_file_monitor_cancel (o->pins_monitor);
		g_object_unref (o->pins_monitor);
	}
	if (o->reload_id)
		g_source_remove (o->reload_id);
	g_free (o->focus_uri);
	g_object_unref (o->monitor);
	g_free (o);
}

static const gchar *
greeting (void)
{
	GDateTime *now = g_date_time_new_now_local ();
	gint hour = g_date_time_get_hour (now);

	g_date_time_unref (now);
	if (hour >= 5 && hour < 12)
		return _("Good morning");
	if (hour >= 12 && hour < 18)
		return _("Good afternoon");
	return _("Good evening");
}

/* -- collapsible sections */

static void
section_update (GtkWidget *header)
{
	GtkWidget *revealer = g_object_get_data (G_OBJECT (header), "sde-revealer");
	GtkWidget *arrow = g_object_get_data (G_OBJECT (header), "sde-arrow");
	gboolean open = gtk_revealer_get_reveal_child (GTK_REVEALER (revealer));

	gtk_image_set_from_icon_name (GTK_IMAGE (arrow), open ? "pan-down-symbolic" : "pan-end-symbolic",
	                              GTK_ICON_SIZE_MENU);
	gtk_widget_set_tooltip_text (header, open ? _("Collapse this section") : _("Expand this section"));
	atk_object_set_role (gtk_widget_get_accessible (header), ATK_ROLE_TOGGLE_BUTTON);
}

static void
section_toggled (GtkButton *header, gpointer data)
{
	GtkWidget *revealer = g_object_get_data (G_OBJECT (header), "sde-revealer");
	const gchar *name = g_object_get_data (G_OBJECT (header), "sde-section");
	gboolean open = !gtk_revealer_get_reveal_child (GTK_REVEALER (revealer));

	gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), open);
	section_update (GTK_WIDGET (header));
	liria_sde_prefs_set_flag ("home", "collapsed", name, !open);
}

/* A section with a header button (arrow and title) that collapses it, as
 * in Windows 11; end is an optional widget at the end of the header row. */
static GtkWidget *
section (const gchar *name, const gchar *title, GtkWidget *content, GtkWidget *end)
{
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *header = gtk_button_new ();
	GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *arrow = gtk_image_new ();
	GtkWidget *revealer = gtk_revealer_new ();

	gtk_button_set_relief (GTK_BUTTON (header), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click (header, FALSE);
	gtk_style_context_add_class (gtk_widget_get_style_context (header), "sde-home-section-button");
	gtk_box_pack_start (GTK_BOX (inner), arrow, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (inner), label_new (title, "sde-home-section", FALSE), FALSE, FALSE, 0);
	gtk_container_add (GTK_CONTAINER (header), inner);
	gtk_box_pack_start (GTK_BOX (row), header, FALSE, FALSE, 0);
	if (end)
		gtk_box_pack_end (GTK_BOX (row), end, FALSE, FALSE, 0);
	gtk_revealer_set_transition_type (GTK_REVEALER (revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration (GTK_REVEALER (revealer), 150);
	gtk_container_add (GTK_CONTAINER (revealer), content);
	gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), !liria_sde_prefs_has_flag ("home", "collapsed", name));
	g_object_set_data (G_OBJECT (header), "sde-revealer", revealer);
	g_object_set_data (G_OBJECT (header), "sde-arrow", arrow);
	g_object_set_data_full (G_OBJECT (header), "sde-section", g_strdup (name), g_free);
	g_signal_connect (header, "clicked", G_CALLBACK (section_toggled), NULL);
	section_update (header);
	gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), revealer, FALSE, FALSE, 0);
	gtk_style_context_add_class (gtk_widget_get_style_context (box), "sde-home-block");
	return box;
}

/* Drive and recent file cards: as many per row as fit, at most three. */
static gboolean
grid_columns_idle (gpointer data)
{
	GtkWidget *flow = data;
	gint width = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (flow), "sde-width"));
	guint columns = liria_sde_home_columns (width, card_min_width (), 8, 3);

	g_object_set_data (G_OBJECT (flow), "sde-idle", NULL);
	if (!gtk_widget_in_destruction (flow) &&
	    gtk_flow_box_get_max_children_per_line (GTK_FLOW_BOX (flow)) != columns) {
		gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), columns);
		gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), columns);
	}
	g_object_unref (flow);
	return G_SOURCE_REMOVE;
}

static void
grid_allocated (GtkWidget *flow, GtkAllocation *allocation, gpointer data)
{
	g_object_set_data (G_OBJECT (flow), "sde-width", GINT_TO_POINTER (allocation->width));
	if (!g_object_get_data (G_OBJECT (flow), "sde-idle")) {
		g_object_set_data (G_OBJECT (flow), "sde-idle", GINT_TO_POINTER (1));
		g_idle_add (grid_columns_idle, g_object_ref (flow));
	}
}

GtkWidget *
sde_home_overview_new (const char *uri, GtkWidget *window)
{
	Overview *o;
	GtkWidget *root, *header, *title, *toggle, *toggle_box, *body, *viewport;
	gchar *home_uri = g_filename_to_uri (g_get_home_dir (), NULL, NULL);
	gchar *hello;
	const gchar *real_name;
	gboolean is_home = home_uri && uri && (g_strcmp0 (uri, home_uri) == 0 ||
	                   (g_str_has_prefix (uri, home_uri) && g_strcmp0 (uri + strlen (home_uri), "/") == 0));

	g_free (home_uri);
	if (!is_home || g_getenv ("SDE_LIRIA_ROOT_BUS"))
		return NULL;

	o = g_new0 (Overview, 1);
	o->window = window;
	o->cancellable = g_cancellable_new ();
	o->monitor = g_volume_monitor_get ();
	o->expanded = overview_expanded ();

	root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	o->root = root;
	gtk_style_context_add_class (gtk_widget_get_style_context (root), "sde-home-overview");
	g_object_set_data_full (G_OBJECT (root), "sde-overview", o, (GDestroyNotify) overview_free);
	g_signal_connect (root, "destroy", G_CALLBACK (overview_destroyed), o);
	g_signal_connect (root, "map", G_CALLBACK (overview_mapped), o);

	/* Header: a one-line greeting and the overview switch. */
	header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
	real_name = g_get_real_name ();
	if (!real_name || !*real_name || g_strcmp0 (real_name, "Unknown") == 0)
		real_name = g_get_user_name ();
	hello = g_strdup_printf ("%s, %s", greeting (), real_name);
	title = label_new (hello, "sde-home-title", TRUE);
	g_free (hello);

	toggle = gtk_button_new ();
	gtk_style_context_add_class (gtk_widget_get_style_context (toggle), "sde-home-toggle");
	toggle_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
	o->toggle_label = gtk_label_new (NULL);
	o->toggle_icon = gtk_image_new ();
	gtk_box_pack_start (GTK_BOX (toggle_box), o->toggle_label, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (toggle_box), o->toggle_icon, FALSE, FALSE, 0);
	gtk_container_add (GTK_CONTAINER (toggle), toggle_box);
	gtk_widget_set_valign (toggle, GTK_ALIGN_CENTER);
	g_signal_connect (toggle, "clicked", G_CALLBACK (toggle_clicked), o);
	gtk_box_pack_start (GTK_BOX (header), title, TRUE, TRUE, 0);
	gtk_box_pack_end (GTK_BOX (header), toggle, FALSE, FALSE, 0);
	gtk_style_context_add_class (gtk_widget_get_style_context (header), "sde-home-header");
	gtk_box_pack_start (GTK_BOX (root), header, FALSE, FALSE, 0);

	/* Body: Quick access, Drives and Recent files, scrolled together. */
	body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
	gtk_style_context_add_class (gtk_widget_get_style_context (body), "sde-home-body");
	gtk_box_pack_start (GTK_BOX (body), section ("quick", _("Quick access"), quick_access_new (o), NULL),
	                    FALSE, FALSE, 0);

	o->drives = flow_new (1, 3, "sde-drives");
	g_signal_connect (o->drives, "size-allocate", G_CALLBACK (grid_allocated), NULL);
	fill_drives (o);
	gtk_box_pack_start (GTK_BOX (body), section ("drives", _("Drives"), o->drives, NULL), FALSE, FALSE, 0);

	o->recent = flow_new (1, 3, "sde-recent-files");
	g_signal_connect (o->recent, "size-allocate", G_CALLBACK (grid_allocated), NULL);
	o->recent_clear = gtk_button_new_with_mnemonic (_("C_lear"));
	gtk_widget_set_tooltip_text (o->recent_clear, _("Clear the list of recent files"));
	gtk_style_context_add_class (gtk_widget_get_style_context (o->recent_clear), "sde-home-clear");
	gtk_widget_set_no_show_all (o->recent_clear, TRUE);
	gtk_widget_set_valign (o->recent_clear, GTK_ALIGN_CENTER);
	g_signal_connect (o->recent_clear, "clicked", G_CALLBACK (clear_clicked), o);
	fill_recent (o);
	gtk_box_pack_start (GTK_BOX (body), section ("recent", _("Recent files"), o->recent, o->recent_clear),
	                    FALSE, FALSE, 0);

	o->scroller = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (o->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (o->scroller), FALSE);
	gtk_widget_set_vexpand (o->scroller, TRUE);
	gtk_style_context_add_class (gtk_widget_get_style_context (o->scroller), "sde-home-scroller");
	viewport = gtk_viewport_new (NULL, NULL);
	gtk_viewport_set_shadow_type (GTK_VIEWPORT (viewport), GTK_SHADOW_NONE);
	gtk_container_add (GTK_CONTAINER (viewport), body);
	gtk_container_add (GTK_CONTAINER (o->scroller), viewport);
	gtk_widget_show_all (o->scroller);
	gtk_widget_set_no_show_all (o->scroller, TRUE);
	gtk_box_pack_start (GTK_BOX (root), o->scroller, TRUE, TRUE, 0);

	g_signal_connect_object (o->monitor, "mount-added", G_CALLBACK (mounts_changed), root, G_CONNECT_SWAPPED);
	g_signal_connect_object (o->monitor, "mount-removed", G_CALLBACK (mounts_changed), root, G_CONNECT_SWAPPED);
	g_signal_connect_object (gtk_recent_manager_get_default (), "changed", G_CALLBACK (recent_changed), root,
	                         G_CONNECT_SWAPPED);

	gtk_widget_show_all (root);
	apply_expanded (o);
	return root;
}
