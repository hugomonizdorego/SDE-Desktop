/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Liria's ribbon (alpha52), an open-source take on a file manager ribbon:
 * full-colour icons of the icon theme (0.2.0 Alpha 2, see "icons" below),
 * theme colours, compact groups.
 *
 *   Home   Clipboard (Pin to Quick access, Copy, Paste, Cut, Copy path,
 *          Paste shortcut), Organize (Move to, Copy to, Delete, Rename),
 *          New (New folder, New document), Open (Properties, Open, Open
 *          with), Select (Select all, Select none, Invert selection)
 *   Share  Send (Email, Compress), Copy to a removable drive
 *   View   Panes (sidebar, extra pane, status bar), Layout (icons, list,
 *          compact, zoom), Sort by, Show (hidden and backup files), Ribbon
 *   Drive  (when the folder is on a drive that can be ejected) Eject,
 *          Unmount, Format, Properties, Disks
 *   Archive (when only archives are selected) Extract here / to, Compress
 *
 * Buttons of existing commands are proxies of the window's GtkActions
 * (gtk_activatable_set_related_action), so they follow the same
 * sensitivity as the menus and are re-bound when the view changes; the
 * other buttons are enabled from the selection by liria_sde_selection_
 * commands () (liria-sde-model.c, unit tested).
 *
 * Three modes for every folder window (liria.conf [ribbon] mode): full,
 * collapsed (tabs only; a tab drops its commands down until one is used)
 * and off (the classic compact header only). They are chosen in the main
 * menu (View > Ribbon), the View tab, with the arrow at the end of the
 * tabs and with Ctrl+F1 (full <-> collapsed).
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include <libliria-private/liria-file.h>
#include <libliria-private/liria-file-operations.h>
#include <libliria-private/liria-view.h>

#include "liria-sde-ribbon.h"
#include "liria-sde-pins.h"
#include "liria-sde-ui.h"
#include "liria-window-private.h"
#include "liria-window-slot.h"

#define RIBBON_KEY "sde-ribbon"

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

typedef struct {
    LiriaNavigationWindow *window;
    GtkUIManager *ui;
    GtkWidget *root;
    GtkWidget *tabs;
    GtkWidget *revealer;
    GtkWidget *stack;
    GtkWidget *collapse;
    GtkWidget *drive_tab;
    GtkWidget *archive_tab;
    GList *tab_buttons;
    GList *bound;        /* buttons bound to a GtkAction by name */
    GList *commands;     /* buttons enabled from the selection */
    GList *view_buttons; /* Icons, List, Compact */
    GList *images;       /* every command's icon (rb_icon_apply ()) */
    gboolean dropped;    /* collapsed mode: the commands are shown */
    gboolean syncing;
    guint refresh_id;
    guint qa_id;         /* LIRIA_SDE_QA_RIBBON: written after the layout */
} Ribbon;

static int sde_ribbon_mode = -1;

static void ribbon_apply (Ribbon *r);

/* --------------------------------------------------------------- icons */

/* 0.2.0 Alpha 2: the ribbon shows the icon theme's full-colour icons, like
 * the folder view and the sidebar, instead of monochrome symbolic glyphs,
 * and follows the icon theme when it changes. Each command names
 * full-colour icons, best first ("base+emblem,emblem" draws an emblem on
 * the base icon, e.g. a folder with a plus). One is used when the theme
 * has it and draws it in colour: many themes, Papirus among them, draw
 * their action icons (edit-copy, edit-paste...) as grey glyphs. Otherwise
 * the theme's symbolic glyph is used, tinted with a colour of the GTK
 * theme (the accent, warning, error or success colour; liria-sde.css).
 * Sizes are 32 px (large) and 16 px (small) at 100 %, grown with the
 * interface scale (liria_sde_ribbon_icon_pixels ()); GTK picks the @2x
 * icons on HiDPI screens itself. */
typedef struct {
    const char *colour[5]; /* full-colour icon names, best first */
    const char *glyph[4];  /* symbolic icon names: the tinted fallback */
    const char *tint;      /* "accent", "amber", "red" or "green" */
} RbIcon;

static const char * const rb_tints[] = { "accent", "amber", "red", "green", NULL };

/* Home */
static const RbIcon rb_icon_pin = { { "view-pin", "pin", NULL },
                                    { "view-pin-symbolic", "pin-symbolic", "starred-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_copy = { { "edit-copy", NULL }, { "edit-copy-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_paste = { { "edit-paste", NULL }, { "edit-paste-symbolic", NULL }, "amber" };
static const RbIcon rb_icon_cut = { { "edit-cut", NULL }, { "edit-cut-symbolic", NULL }, "red" };
static const RbIcon rb_icon_copy_path = { { "edit-copy", NULL }, { "edit-copy-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_paste_link = { { "insert-link", NULL },
                                           { "insert-link-symbolic", "emblem-symbolic-link", NULL }, "accent" };
static const RbIcon rb_icon_move_to = { { "folder-move", NULL },
                                        { "folder-move-symbolic", "go-jump-symbolic", NULL }, "amber" };
static const RbIcon rb_icon_copy_to = { { "folder-copy", NULL },
                                        { "folder-copy-symbolic", "edit-copy-symbolic", NULL }, "amber" };
static const RbIcon rb_icon_delete = { { "user-trash", "edit-delete", NULL },
                                       { "user-trash-symbolic", "edit-delete-symbolic", NULL }, "red" };
static const RbIcon rb_icon_rename = { { "edit-rename", NULL },
                                       { "edit-rename-symbolic", "document-edit-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_new_folder = { { "folder-new", "folder+emblem-new,list-add", NULL },
                                           { "folder-new-symbolic", NULL }, "amber" };
static const RbIcon rb_icon_new_document = { { "document-new", NULL }, { "document-new-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_properties = { { "document-properties", "dialog-information", NULL },
                                           { "document-properties-symbolic", "preferences-system-symbolic", NULL },
                                           "accent" };
static const RbIcon rb_icon_open = { { "document-open", NULL }, { "document-open-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_open_with = { { "system-run", NULL },
                                          { "system-run-symbolic", "application-x-executable-symbolic", NULL },
                                          "green" };
static const RbIcon rb_icon_select_all = { { "edit-select-all", NULL }, { "edit-select-all-symbolic", NULL },
                                           "accent" };
static const RbIcon rb_icon_select_none = { { "edit-select-none", NULL },
                                            { "edit-select-none-symbolic", "edit-clear-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_invert = { { "edit-select-invert", NULL },
                                       { "edit-select-invert-symbolic", "object-flip-horizontal-symbolic", NULL },
                                       "accent" };
/* Share */
static const RbIcon rb_icon_email = { { "mail-send", "internet-mail", "mail-message-new", NULL },
                                      { "mail-send-symbolic", "mail-message-new-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_compress = { { "package-x-generic", "application-x-archive", NULL },
                                         { "package-x-generic-symbolic", "application-x-addon-symbolic", NULL },
                                         "green" };
static const RbIcon rb_icon_to_drive = { { "drive-removable-media-usb", "drive-removable-media", "media-removable",
                                           NULL },
                                         { "media-removable-symbolic", "drive-removable-media-symbolic", NULL },
                                         "accent" };
static const RbIcon rb_icon_make_link = { { "insert-link", NULL }, { "insert-link-symbolic", NULL }, "accent" };
/* View */
static const RbIcon rb_icon_nav_pane = { { "sidebar-show", "view-left-pane", NULL },
                                         { "sidebar-show-symbolic", "view-left-pane-symbolic", "view-dual-symbolic",
                                           NULL }, "accent" };
static const RbIcon rb_icon_extra_pane = { { "view-dual", NULL }, { "view-dual-symbolic", "view-paged-symbolic", NULL },
                                           "accent" };
static const RbIcon rb_icon_status_bar = { { NULL },
                                           { "view-continuous-symbolic", "view-more-horizontal-symbolic", NULL },
                                           "accent" };
static const RbIcon rb_icon_view_icons = { { "view-grid", NULL }, { "view-grid-symbolic", "view-list-symbolic", NULL },
                                           "accent" };
static const RbIcon rb_icon_view_list = { { "view-list", NULL }, { "view-list-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_view_compact = { { "view-compact", NULL },
                                             { "view-compact-symbolic", "view-list-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_zoom_in = { { "zoom-in", NULL }, { "zoom-in-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_zoom_out = { { "zoom-out", NULL }, { "zoom-out-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_zoom_normal = { { "zoom-original", NULL }, { "zoom-original-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_sort = { { "view-sort-ascending", NULL },
                                     { "view-sort-ascending-symbolic", "view-sort-descending-symbolic", NULL },
                                     "accent" };
static const RbIcon rb_icon_hidden = { { NULL }, { "view-reveal-symbolic", "view-conceal-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_backup = { { NULL }, { "document-revert-symbolic", "edit-undo-symbolic", NULL }, "amber" };
static const RbIcon rb_icon_ribbon = { { NULL }, { "view-more-horizontal-symbolic", "open-menu-symbolic", NULL },
                                       "accent" };
/* Drive */
static const RbIcon rb_icon_eject = { { "media-eject", NULL }, { "media-eject-symbolic", NULL }, "accent" };
static const RbIcon rb_icon_format = { { "drive-harddisk", NULL }, { "drive-harddisk-symbolic", NULL }, "red" };
static const RbIcon rb_icon_disks = { { "org.gnome.DiskUtility", "gnome-disks", "drive-multidisk", NULL },
                                      { "drive-multidisk-symbolic", "drive-harddisk-symbolic", NULL }, "accent" };
/* Archive */
static const RbIcon rb_icon_extract_here = { { "extract-archive", "package-x-generic", "application-x-archive", NULL },
                                             { "extract-archive-symbolic", "package-x-generic-symbolic", NULL },
                                             "green" };
static const RbIcon rb_icon_extract_to = { { "folder-download", NULL },
                                           { "folder-download-symbolic", "folder-symbolic", NULL }, "amber" };

/* "name/pixels" -> 1 drawn in grey, 2 in colour; emptied when the icon
 * theme changes. */
static GHashTable *rb_colour_cache = NULL;

static void
rb_colour_cache_reset (GtkIconTheme *theme, gpointer data)
{
    g_hash_table_remove_all (rb_colour_cache);
}

static gboolean
rb_icon_colourful (GtkIconTheme *theme, const char *name, int pixels)
{
    char *key;
    gpointer value;
    GtkIconInfo *info;
    gboolean colourful = FALSE;

    if (rb_colour_cache == NULL)
    {
        rb_colour_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        /* Connected before any ribbon's own handler, so the ribbons find
         * an empty cache when they choose their icons again. */
        g_signal_connect (theme, "changed", G_CALLBACK (rb_colour_cache_reset), NULL);
    }
    key = g_strdup_printf ("%s/%d", name, pixels);
    if (g_hash_table_lookup_extended (rb_colour_cache, key, NULL, &value))
    {
        g_free (key);
        return GPOINTER_TO_INT (value) == 2;
    }
    info = gtk_icon_theme_lookup_icon (theme, name, pixels, GTK_ICON_LOOKUP_FORCE_SIZE);
    if (info != NULL && !gtk_icon_info_is_symbolic (info))
    {
        GdkPixbuf *pixbuf = gtk_icon_info_load_icon (info, NULL);

        if (pixbuf != NULL)
        {
            colourful = liria_sde_pixels_colourful (gdk_pixbuf_read_pixels (pixbuf), gdk_pixbuf_get_width (pixbuf),
                                                    gdk_pixbuf_get_height (pixbuf), gdk_pixbuf_get_rowstride (pixbuf),
                                                    gdk_pixbuf_get_n_channels (pixbuf));
            g_object_unref (pixbuf);
        }
    }
    if (info != NULL)
        g_object_unref (info);
    g_hash_table_insert (rb_colour_cache, key, GINT_TO_POINTER (colourful ? 2 : 1));
    return colourful;
}

/* A GIcon for "name" or "base+emblem,emblem" (the first emblem the theme
 * has, drawn by GTK on the lower right corner). */
static GIcon *
rb_icon_gicon (const char *spec)
{
    const char *plus = strchr (spec, '+');
    char *base;
    char **emblems;
    GIcon *icon, *emblem_icon, *emblemed;
    GEmblem *emblem;

    if (plus == NULL)
        return g_themed_icon_new (spec);
    base = g_strndup (spec, plus - spec);
    emblems = g_strsplit (plus + 1, ",", -1);
    icon = g_themed_icon_new (base);
    emblem_icon = g_themed_icon_new_from_names (emblems, -1);
    emblem = g_emblem_new (emblem_icon);
    emblemed = g_emblemed_icon_new (icon, emblem);
    g_object_unref (emblem);
    g_object_unref (emblem_icon);
    g_object_unref (icon);
    g_strfreev (emblems);
    g_free (base);
    return emblemed;
}

static int
rb_icon_pixels (gboolean large)
{
    GtkSettings *settings = gtk_settings_get_default ();
    gint dpi = -1;

    if (settings != NULL)
        g_object_get (settings, "gtk-xft-dpi", &dpi, NULL);
    return liria_sde_ribbon_icon_pixels (dpi, large);
}

/* Chooses (again) the icon of a command's image, see above. */
static void
rb_icon_apply (GtkWidget *image)
{
    const RbIcon *icon = g_object_get_data (G_OBJECT (image), "sde-icon");
    gboolean large = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (image), "sde-icon-large"));
    GtkIconTheme *theme = gtk_icon_theme_get_default ();
    GtkStyleContext *style = gtk_widget_get_style_context (image);
    int pixels = rb_icon_pixels (large);
    const char *chosen = NULL;
    guint i;

    for (i = 0; icon->colour[i] != NULL && chosen == NULL; i++)
    {
        char *base = g_strndup (icon->colour[i], strcspn (icon->colour[i], "+"));

        if (gtk_icon_theme_has_icon (theme, base) && rb_icon_colourful (theme, base, pixels))
            chosen = icon->colour[i];
        g_free (base);
    }
    for (i = 0; rb_tints[i] != NULL; i++)
    {
        char *tint = g_strconcat ("sde-tint-", rb_tints[i], NULL);

        gtk_style_context_remove_class (style, tint);
        g_free (tint);
    }
    if (chosen != NULL)
    {
        GIcon *gicon = rb_icon_gicon (chosen);

        gtk_image_set_from_gicon (GTK_IMAGE (image), gicon, GTK_ICON_SIZE_BUTTON);
        g_object_unref (gicon);
        g_object_set_data (G_OBJECT (image), "sde-icon-kind", (gpointer) "colour");
    }
    else
    {
        char *tint = g_strconcat ("sde-tint-", icon->tint, NULL);

        chosen = icon->glyph[0];
        for (i = 0; icon->glyph[i] != NULL; i++)
        {
            if (gtk_icon_theme_has_icon (theme, icon->glyph[i]))
            {
                chosen = icon->glyph[i];
                break;
            }
        }
        gtk_image_set_from_icon_name (GTK_IMAGE (image), chosen, GTK_ICON_SIZE_BUTTON);
        gtk_style_context_add_class (style, tint);
        g_free (tint);
        g_object_set_data (G_OBJECT (image), "sde-icon-kind", (gpointer) "glyph");
    }
    gtk_image_set_pixel_size (GTK_IMAGE (image), pixels);
    g_object_set_data (G_OBJECT (image), "sde-icon-chosen", (gpointer) chosen);
}

/* LIRIA_SDE_QA_RIBBON=FILE: the icon of every command (label, icon,
 * "colour" or "glyph", tint, pixels) and the tabs shown ("tab", page,
 * x,y,w,h in the window), one per line, for tests and screenshots. */
static void
rb_qa (Ribbon *r)
{
    const char *path = g_getenv ("LIRIA_SDE_QA_RIBBON");
    GString *text;
    GList *l;

    if (path == NULL || *path == '\0')
        return;
    text = g_string_new (NULL);
    for (l = r->images; l != NULL; l = l->next)
    {
        const RbIcon *icon = g_object_get_data (G_OBJECT (l->data), "sde-icon");
        const char *kind = g_object_get_data (G_OBJECT (l->data), "sde-icon-kind");

        g_string_append_printf (text, "%s\t%s\t%s\t%s\t%d\n",
                                (const char *) g_object_get_data (G_OBJECT (l->data), "sde-icon-label"),
                                (const char *) g_object_get_data (G_OBJECT (l->data), "sde-icon-chosen"),
                                kind, g_strcmp0 (kind, "glyph") == 0 ? icon->tint : "-",
                                gtk_image_get_pixel_size (GTK_IMAGE (l->data)));
    }
    for (l = r->tab_buttons; l != NULL; l = l->next)
    {
        GtkWidget *tab = l->data;
        int x = 0, y = 0;

        if (gtk_widget_get_mapped (tab) &&
            gtk_widget_translate_coordinates (tab, gtk_widget_get_toplevel (tab), 0, 0, &x, &y))
            g_string_append_printf (text, "tab\t%s\t%d,%d,%d,%d\n",
                                    (const char *) g_object_get_data (G_OBJECT (tab), "sde-page"), x, y,
                                    gtk_widget_get_allocated_width (tab), gtk_widget_get_allocated_height (tab));
    }
    g_file_set_contents (path, text->str, -1, NULL);
    g_string_free (text, TRUE);
}

static gboolean
rb_qa_idle (gpointer data)
{
    Ribbon *r = data;

    r->qa_id = 0;
    rb_qa (r);
    return G_SOURCE_REMOVE;
}

/* The QA file again once GTK has placed a tab that just appeared. */
static void
rb_qa_later (Ribbon *r)
{
    const char *path = g_getenv ("LIRIA_SDE_QA_RIBBON");

    if (path != NULL && *path != '\0' && r->qa_id == 0)
        r->qa_id = g_timeout_add_full (G_PRIORITY_LOW, 100, rb_qa_idle, r, NULL);
}

static void
rb_icons_apply_all (Ribbon *r)
{
    GList *l;

    for (l = r->images; l != NULL; l = l->next)
        rb_icon_apply (l->data);
    rb_qa (r);
}

/* ------------------------------------------------------------- helpers */

static GtkAction *
rb_action (Ribbon *r, const char *name)
{
    GList *l;

    for (l = gtk_ui_manager_get_action_groups (r->ui); l != NULL; l = l->next)
    {
        GtkAction *action = gtk_action_group_get_action (l->data, name);

        if (action != NULL)
            return action;
    }
    return NULL;
}

static gboolean
rb_action_usable (Ribbon *r, const char *name)
{
    GtkAction *action = rb_action (r, name);

    return action != NULL && gtk_action_is_visible (action) && gtk_action_is_sensitive (action);
}

static LiriaWindowSlot *
rb_slot (Ribbon *r)
{
    return liria_window_get_active_slot (LIRIA_WINDOW (r->window));
}

/* The selected files of the active view (LiriaFile references, free them
 * with liria_file_list_free ()). The view's own selection is a list of
 * GFile locations; using those as files printed critical warnings on every
 * selection change and gave the commands no URIs (0.2.0). */
static GList *
rb_selection (Ribbon *r)
{
    LiriaWindowSlot *slot = rb_slot (r);
    GList *locations, *l, *files = NULL;

    if (slot == NULL || slot->content_view == NULL)
        return NULL;
    locations = liria_view_get_selection (slot->content_view);
    for (l = locations; l != NULL; l = l->next)
        if (G_IS_FILE (l->data))
            files = g_list_prepend (files, liria_file_get (l->data));
    g_list_free_full (locations, g_object_unref);
    return g_list_reverse (files);
}

static char *
rb_location_uri (Ribbon *r)
{
    LiriaWindowSlot *slot = rb_slot (r);

    return slot != NULL ? liria_window_slot_get_location_uri (slot) : NULL;
}

static GtkWidget *
rb_view_widget (Ribbon *r)
{
    LiriaWindowSlot *slot = rb_slot (r);

    if (slot != NULL && slot->content_view != NULL)
        return liria_view_get_widget (slot->content_view);
    return GTK_WIDGET (r->window);
}

/* URIs of the selection; with nothing selected, the folder itself. */
static GList *
rb_selected_uris (Ribbon *r, gboolean or_folder)
{
    GList *files = rb_selection (r), *uris = NULL, *l;

    for (l = files; l != NULL; l = l->next)
        uris = g_list_prepend (uris, liria_file_get_uri (l->data));
    liria_file_list_free (files);
    if (uris == NULL && or_folder)
    {
        char *uri = rb_location_uri (r);

        if (uri != NULL)
            uris = g_list_prepend (uris, uri);
    }
    return g_list_reverse (uris);
}

static gboolean
rb_is_archive (LiriaFile *file)
{
    static const char *types[] = {
        "application/zip", "application/x-tar", "application/x-compressed-tar",
        "application/x-bzip-compressed-tar", "application/x-xz-compressed-tar", "application/x-7z-compressed",
        "application/vnd.rar", "application/x-rar", "application/gzip", "application/x-zstd-compressed-tar",
        "application/x-bzip", "application/x-xz", "application/x-lzma-compressed-tar", NULL
    };
    int i;

    for (i = 0; types[i] != NULL; i++)
        if (liria_file_is_mime_type (file, types[i]))
            return TRUE;
    return FALSE;
}

static void
rb_spawn (char **argv, const char *dir)
{
    GError *error = NULL;

    if (!g_spawn_async (dir, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error))
    {
        g_warning ("Liria: %s", error->message);
        g_error_free (error);
    }
}

/* Local paths of the selection, for programs that take file names. */
static GPtrArray *
rb_selected_paths (Ribbon *r)
{
    GList *uris = rb_selected_uris (r, FALSE), *l;
    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

    for (l = uris; l != NULL; l = l->next)
    {
        char *path = g_filename_from_uri (l->data, NULL, NULL);

        if (path != NULL)
            g_ptr_array_add (paths, path);
    }
    g_list_free_full (uris, g_free);
    return paths;
}

/* -------------------------------------------------------- the widgets */

static void
rb_close_dropdown (GtkWidget *widget, Ribbon *r)
{
    if (r->dropped)
    {
        r->dropped = FALSE;
        ribbon_apply (r);
    }
}

static GtkWidget *
rb_button_new (Ribbon *r, const char *label, const RbIcon *icon, gboolean large, gboolean toggle)
{
    GtkWidget *button = toggle ? gtk_toggle_button_new () : gtk_button_new ();
    GtkWidget *box = gtk_box_new (large ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, large ? 2 : 6);
    GtkWidget *image = gtk_image_new ();
    GtkWidget *text = gtk_label_new (label);

    g_object_set_data (G_OBJECT (image), "sde-icon", (gpointer) icon);
    g_object_set_data (G_OBJECT (image), "sde-icon-large", GINT_TO_POINTER (large));
    g_object_set_data_full (G_OBJECT (image), "sde-icon-label", g_strdup (label), g_free);
    gtk_style_context_add_class (gtk_widget_get_style_context (image), "sde-ribbon-icon");
    r->images = g_list_append (r->images, image);
    rb_icon_apply (image);
    gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
    if (large)
    {
        /* Two lines at most, like "Pin to Quick / access". */
        gtk_label_set_line_wrap (GTK_LABEL (text), TRUE);
        if (g_utf8_strlen (label, -1) > 12)
            gtk_label_set_width_chars (GTK_LABEL (text), 12);
        gtk_label_set_max_width_chars (GTK_LABEL (text), 13);
    }
    else
        gtk_label_set_xalign (GTK_LABEL (text), 0.0);
    gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), text, FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (button), box);
    gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (button, FALSE);
    gtk_widget_set_tooltip_text (button, label);
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "sde-ribbon-button");
    gtk_style_context_add_class (gtk_widget_get_style_context (button), large ? "sde-ribbon-large" : "sde-ribbon-small");
    if (!large)
        gtk_widget_set_halign (button, GTK_ALIGN_FILL);
    return button;
}

/* A group: its buttons, then its name underneath. */
static GtkWidget *
rb_group (GtkWidget *page, const char *title)
{
    GtkWidget *group = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *label = gtk_label_new (title);

    gtk_style_context_add_class (gtk_widget_get_style_context (group), "sde-ribbon-group");
    gtk_style_context_add_class (gtk_widget_get_style_context (label), "sde-ribbon-group-label");
    gtk_box_pack_start (GTK_BOX (group), content, TRUE, TRUE, 0);
    gtk_box_pack_end (GTK_BOX (group), label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), group, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), gtk_separator_new (GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    return content;
}

/* A column of small buttons in a group. */
static GtkWidget *
rb_column (GtkWidget *group)
{
    GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_valign (column, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (group), column, FALSE, FALSE, 0);
    return column;
}

/* A button for one of the window's actions: it runs the action and
 * follows its sensitivity (and check state); see rb_rebind (). */
static void
rb_action_clicked (GtkButton *button, Ribbon *r)
{
    GtkAction *action;

    if (r->syncing || r->ui == NULL)
        return;
    action = rb_action (r, g_object_get_data (G_OBJECT (button), "sde-action"));
    if (action == NULL)
        return;
    if (GTK_IS_TOGGLE_ACTION (action) && GTK_IS_TOGGLE_BUTTON (button))
        gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (action),
                                      gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)));
    else
        gtk_action_activate (action);
}

static GtkWidget *
rb_action_button (Ribbon *r, GtkWidget *box, const char *action, const char *label, const RbIcon *icon,
                  gboolean large, gboolean toggle)
{
    GtkWidget *button = rb_button_new (r, label, icon, large, toggle);

    g_object_set_data_full (G_OBJECT (button), "sde-action", g_strdup (action), g_free);
    g_signal_connect (button, "clicked", G_CALLBACK (rb_action_clicked), r);
    g_signal_connect_after (button, "clicked", G_CALLBACK (rb_close_dropdown), r);
    r->bound = g_list_prepend (r->bound, button);
    gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
    return button;
}

static GtkWidget *
rb_command_button (Ribbon *r, GtkWidget *box, guint needs, const char *label, const RbIcon *icon,
                   gboolean large, GCallback callback)
{
    GtkWidget *button = rb_button_new (r, label, icon, large, FALSE);

    g_object_set_data (G_OBJECT (button), "sde-needs", GUINT_TO_POINTER (needs));
    g_signal_connect (button, "clicked", callback, r);
    g_signal_connect_after (button, "clicked", G_CALLBACK (rb_close_dropdown), r);
    r->commands = g_list_prepend (r->commands, button);
    gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
    return button;
}

static GtkWidget *
rb_menu_item (GtkWidget *menu, const char *label, GCallback callback, gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic (label);

    if (callback != NULL)
        g_signal_connect (item, "activate", callback, data);
    else
        gtk_widget_set_sensitive (item, FALSE);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    gtk_widget_show (item);
    return item;
}

static gboolean
rb_destroy_menu_idle (gpointer menu)
{
    gtk_widget_destroy (menu);
    g_object_unref (menu);
    return G_SOURCE_REMOVE;
}

static void
rb_menu_deactivated (GtkWidget *menu, Ribbon *r)
{
    g_idle_add (rb_destroy_menu_idle, g_object_ref (menu));
    rb_close_dropdown (menu, r);
}

static void
rb_popup (Ribbon *r, GtkWidget *menu, GtkWidget *button)
{
    gtk_style_context_add_class (gtk_widget_get_style_context (menu), "sde-ribbon-menu");
    gtk_menu_attach_to_widget (GTK_MENU (menu), button, NULL);
    g_signal_connect (menu, "deactivate", G_CALLBACK (rb_menu_deactivated), r);
    gtk_menu_popup_at_widget (GTK_MENU (menu), button, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

/* ------------------------------------------------------ the commands */

static void
rb_pin (GtkButton *button, Ribbon *r)
{
    GList *uris = rb_selected_uris (r, TRUE), *l;
    GPtrArray *list = g_ptr_array_new ();

    for (l = uris; l != NULL; l = l->next)
        g_ptr_array_add (list, l->data);
    g_ptr_array_add (list, NULL);
    liria_sde_pins_add ((const char * const *) list->pdata, -1);
    g_ptr_array_free (list, TRUE);
    g_list_free_full (uris, g_free);
}

static void
rb_copy_path (GtkButton *button, Ribbon *r)
{
    GList *uris = rb_selected_uris (r, TRUE), *l;
    GString *text = g_string_new (NULL);

    for (l = uris; l != NULL; l = l->next)
    {
        GFile *file = g_file_new_for_uri (l->data);
        char *name = g_file_get_parse_name (file);

        if (text->len > 0)
            g_string_append_c (text, '\n');
        g_string_append (text, name);
        g_free (name);
        g_object_unref (file);
    }
    gtk_clipboard_set_text (gtk_clipboard_get (GDK_SELECTION_CLIPBOARD), text->str, -1);
    gtk_clipboard_set_text (gtk_clipboard_get (GDK_SELECTION_PRIMARY), text->str, -1);
    g_string_free (text, TRUE);
    g_list_free_full (uris, g_free);
}

typedef struct {
    GtkWidget *parent;
    char *target;
} PasteLink;

static void
rb_paste_link_received (GtkClipboard *clipboard, gchar **uris, gpointer data)
{
    PasteLink *p = data;
    GList *list = NULL;
    int i;

    for (i = 0; uris != NULL && uris[i] != NULL; i++)
        list = g_list_prepend (list, uris[i]);
    list = g_list_reverse (list);
    if (list != NULL && p->target != NULL)
        liria_file_operations_copy_move (list, NULL, p->target, GDK_ACTION_LINK, p->parent, NULL, NULL);
    g_list_free (list);
    g_object_unref (p->parent);
    g_free (p->target);
    g_free (p);
}

/* Paste shortcut: links to the copied files in this folder. */
static void
rb_paste_link (GtkButton *button, Ribbon *r)
{
    PasteLink *p = g_new0 (PasteLink, 1);

    p->parent = g_object_ref (rb_view_widget (r));
    p->target = rb_location_uri (r);
    gtk_clipboard_request_uris (gtk_clipboard_get (GDK_SELECTION_CLIPBOARD), rb_paste_link_received, p);
}

static void
rb_transfer (Ribbon *r, const char *target, GdkDragAction action)
{
    GList *uris = rb_selected_uris (r, FALSE);

    if (uris != NULL && target != NULL)
        liria_file_operations_copy_move (uris, NULL, target, action, rb_view_widget (r), NULL, NULL);
    g_list_free_full (uris, g_free);
}

static void
rb_transfer_item (GtkMenuItem *item, Ribbon *r)
{
    rb_transfer (r, g_object_get_data (G_OBJECT (item), "sde-target"),
                 GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "sde-drop-action")));
}

static void
rb_transfer_choose (GtkMenuItem *item, Ribbon *r)
{
    GdkDragAction action = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "sde-drop-action"));
    GtkWidget *dialog = gtk_file_chooser_dialog_new (action == GDK_ACTION_MOVE ? _("Move To") : _("Copy To"),
                                                     GTK_WINDOW (r->window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                     _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                     action == GDK_ACTION_MOVE ? _("_Move") : _("_Copy"),
                                                     GTK_RESPONSE_ACCEPT, NULL);

    gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (dialog), FALSE);
    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
        char *target = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (dialog));

        rb_transfer (r, target, action);
        g_free (target);
    }
    gtk_widget_destroy (dialog);
}

/* Move to / Copy to: the personal folders, the pinned folders and any
 * other folder. */
static void
rb_transfer_menu (Ribbon *r, GtkWidget *button, GdkDragAction action)
{
    static const GUserDirectory dirs[] = {
        G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_DOWNLOAD,
        G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_MUSIC, G_USER_DIRECTORY_VIDEOS
    };
    GtkWidget *menu = gtk_menu_new ();
    GPtrArray *pins = liria_sde_pins_load ();
    GPtrArray *seen = g_ptr_array_new_with_free_func (g_free);
    GtkWidget *item;
    gboolean separated = FALSE;
    guint i;

    for (i = 0; i <= G_N_ELEMENTS (dirs); i++)
    {
        const char *path = i == 0 ? g_get_home_dir () : g_get_user_special_dir (dirs[i - 1]);
        char *uri, *name;

        if (path == NULL || (i > 0 && g_strcmp0 (path, g_get_home_dir ()) == 0))
            continue;
        uri = g_filename_to_uri (path, NULL, NULL);
        name = i == 0 ? g_strdup (_("Home")) : g_filename_display_basename (path);
        item = rb_menu_item (menu, name, G_CALLBACK (rb_transfer_item), r);
        g_object_set_data_full (G_OBJECT (item), "sde-target", g_strdup (uri), g_free);
        g_object_set_data (G_OBJECT (item), "sde-drop-action", GINT_TO_POINTER (action));
        g_ptr_array_add (seen, uri);
        g_free (name);
    }
    for (i = 0; i < pins->len; i++)
    {
        LiriaSdePin *pin = pins->pdata[i];
        GFile *file = g_file_new_for_uri (pin->uri);
        char *path = g_file_get_path (file), *name;
        gboolean listed = FALSE;
        guint j;

        for (j = 0; j < seen->len; j++)
            listed = listed || g_strcmp0 (seen->pdata[j], pin->uri) == 0;
        if (!listed && !g_str_has_prefix (pin->uri, "trash:") &&
            (path == NULL || g_file_test (path, G_FILE_TEST_IS_DIR)))
        {
            if (!separated)
                gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
            separated = TRUE;
            name = pin->label != NULL ? g_strdup (pin->label) : g_file_get_basename (file);
            item = rb_menu_item (menu, name, G_CALLBACK (rb_transfer_item), r);
            g_object_set_data_full (G_OBJECT (item), "sde-target", g_strdup (pin->uri), g_free);
            g_object_set_data (G_OBJECT (item), "sde-drop-action", GINT_TO_POINTER (action));
            g_free (name);
        }
        g_free (path);
        g_object_unref (file);
    }
    item = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    item = rb_menu_item (menu, _("_Choose Location\xE2\x80\xA6"), G_CALLBACK (rb_transfer_choose), r);
    g_object_set_data (G_OBJECT (item), "sde-drop-action", GINT_TO_POINTER (action));
    gtk_widget_show_all (menu);
    g_ptr_array_unref (seen);
    g_ptr_array_unref (pins);
    rb_popup (r, menu, button);
}

static void
rb_move_to (GtkButton *button, Ribbon *r)
{
    rb_transfer_menu (r, GTK_WIDGET (button), GDK_ACTION_MOVE);
}

static void
rb_copy_to (GtkButton *button, Ribbon *r)
{
    rb_transfer_menu (r, GTK_WIDGET (button), GDK_ACTION_COPY);
}

static void
rb_select_none (GtkButton *button, Ribbon *r)
{
    LiriaWindowSlot *slot = rb_slot (r);

    if (slot != NULL && slot->content_view != NULL)
        liria_view_set_selection (slot->content_view, NULL);
}

static void
rb_email (GtkButton *button, Ribbon *r)
{
    GPtrArray *paths = rb_selected_paths (r);
    GPtrArray *argv = g_ptr_array_new ();
    guint i;

    g_ptr_array_add (argv, (gpointer) "xdg-email");
    for (i = 0; i < paths->len; i++)
    {
        g_ptr_array_add (argv, (gpointer) "--attach");
        g_ptr_array_add (argv, paths->pdata[i]);
    }
    g_ptr_array_add (argv, NULL);
    if (paths->len > 0)
        rb_spawn ((char **) argv->pdata, NULL);
    g_ptr_array_free (argv, TRUE);
    g_ptr_array_unref (paths);
}

static void
rb_compress (GtkButton *button, Ribbon *r)
{
    GPtrArray *paths = rb_selected_paths (r);
    GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
    char *folder = paths->len > 0 ? g_path_get_dirname (paths->pdata[0]) : NULL;
    guint i;

    g_ptr_array_add (argv, g_strdup ("taca"));
    g_ptr_array_add (argv, g_strdup ("--add"));
    if (folder != NULL)
        g_ptr_array_add (argv, g_strdup_printf ("--default-dir=%s", folder));
    for (i = 0; i < paths->len; i++)
        g_ptr_array_add (argv, g_strdup (paths->pdata[i]));
    g_ptr_array_add (argv, NULL);
    if (paths->len > 0)
        rb_spawn ((char **) argv->pdata, folder);
    g_ptr_array_unref (argv);
    g_ptr_array_unref (paths);
    g_free (folder);
}

static void
rb_extract (Ribbon *r, gboolean here)
{
    GPtrArray *paths = rb_selected_paths (r);
    guint i;

    for (i = 0; i < paths->len; i++)
    {
        char *folder = g_path_get_dirname (paths->pdata[i]);
        char *argv[] = { (char *) "taca", (char *) (here ? "--extract-here" : "--extract"), paths->pdata[i], NULL };

        rb_spawn (argv, folder);
        g_free (folder);
    }
    g_ptr_array_unref (paths);
}

static void
rb_extract_here (GtkButton *button, Ribbon *r)
{
    rb_extract (r, TRUE);
}

static void
rb_extract_to (GtkButton *button, Ribbon *r)
{
    rb_extract (r, FALSE);
}

/* Copy to a removable drive (USB sticks, memory cards) with Liria's safe
 * copy engine. */
static void
rb_drive_menu (GtkButton *button, Ribbon *r)
{
    GVolumeMonitor *monitor = g_volume_monitor_get ();
    GList *mounts = g_volume_monitor_get_mounts (monitor), *l;
    GtkWidget *menu = gtk_menu_new ();
    gboolean any = FALSE;

    for (l = mounts; l != NULL; l = l->next)
    {
        GMount *mount = l->data;
        GDrive *drive = g_mount_get_drive (mount);
        gboolean removable = drive != NULL && (g_drive_is_removable (drive) || g_drive_can_eject (drive));

        if (removable && !g_mount_is_shadowed (mount))
        {
            GFile *root = g_mount_get_root (mount);
            char *name = g_mount_get_name (mount);
            GtkWidget *item = rb_menu_item (menu, name, G_CALLBACK (rb_transfer_item), r);

            g_object_set_data_full (G_OBJECT (item), "sde-target", g_file_get_uri (root), g_free);
            g_object_set_data (G_OBJECT (item), "sde-drop-action", GINT_TO_POINTER (GDK_ACTION_COPY));
            any = TRUE;
            g_free (name);
            g_object_unref (root);
        }
        if (drive != NULL)
            g_object_unref (drive);
    }
    if (!any)
        rb_menu_item (menu, _("No removable drive is connected"), NULL, NULL);
    g_list_free_full (mounts, g_object_unref);
    g_object_unref (monitor);
    rb_popup (r, menu, GTK_WIDGET (button));
}

static void
rb_disks (GtkButton *button, Ribbon *r)
{
    char *argv[] = { (char *) "gnome-disks", NULL };

    rb_spawn (argv, NULL);
}

static void
rb_view_toggled (GtkToggleButton *button, Ribbon *r)
{
    if (!r->syncing && gtk_toggle_button_get_active (button))
        liria_sde_ui_switch_view (r->window, g_object_get_data (G_OBJECT (button), "sde-view-id"));
}

static void
rb_sort_menu (GtkButton *button, Ribbon *r)
{
    GtkWidget *menu = gtk_menu_new ();

    liria_sde_ui_add_sort_items (menu, r->window);
    gtk_widget_show_all (menu);
    rb_popup (r, menu, GTK_WIDGET (button));
}

static void
rb_mode_menu (GtkButton *button, Ribbon *r)
{
    GtkWidget *menu = gtk_menu_new ();

    liria_sde_ribbon_add_mode_items (menu);
    rb_popup (r, menu, GTK_WIDGET (button));
}

/* ------------------------------------------------------------ the pages */

static GtkWidget *
rb_page (Ribbon *r, const char *name, const char *title, GtkWidget **tab)
{
    GtkWidget *page = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *button = gtk_toggle_button_new_with_label (title);

    gtk_style_context_add_class (gtk_widget_get_style_context (page), "sde-ribbon-page");
    gtk_stack_add_named (GTK_STACK (r->stack), page, name);
    gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (button, FALSE);
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "sde-ribbon-tab");
    g_object_set_data_full (G_OBJECT (button), "sde-page", g_strdup (name), g_free);
    gtk_box_pack_start (GTK_BOX (r->tabs), button, FALSE, FALSE, 0);
    r->tab_buttons = g_list_append (r->tab_buttons, button);
    if (tab != NULL)
        *tab = button;
    return page;
}

static void
rb_build_home (Ribbon *r)
{
    GtkWidget *page = rb_page (r, "home", _("Home"), NULL);
    GtkWidget *group, *column;

    group = rb_group (page, _("Clipboard"));
    rb_command_button (r, group, LIRIA_SDE_CMD_PIN, _("Pin to Quick access"),
                       &rb_icon_pin, TRUE, G_CALLBACK (rb_pin));
    rb_action_button (r, group, "Copy", _("Copy"), &rb_icon_copy, TRUE, FALSE);
    rb_action_button (r, group, "Paste", _("Paste"), &rb_icon_paste, TRUE, FALSE);
    column = rb_column (group);
    rb_action_button (r, column, "Cut", _("Cut"), &rb_icon_cut, FALSE, FALSE);
    rb_command_button (r, column, LIRIA_SDE_CMD_PIN, _("Copy path"),
                       &rb_icon_copy_path, FALSE, G_CALLBACK (rb_copy_path));
    rb_command_button (r, column, LIRIA_SDE_CMD_PASTE, _("Paste shortcut"),
                       &rb_icon_paste_link, FALSE, G_CALLBACK (rb_paste_link));

    group = rb_group (page, _("Organize"));
    column = rb_column (group);
    rb_command_button (r, column, LIRIA_SDE_CMD_CUT, _("Move to"),
                       &rb_icon_move_to, FALSE, G_CALLBACK (rb_move_to));
    rb_command_button (r, column, LIRIA_SDE_CMD_COPY, _("Copy to"),
                       &rb_icon_copy_to, FALSE, G_CALLBACK (rb_copy_to));
    column = rb_column (group);
    rb_action_button (r, column, "Trash", _("Delete"), &rb_icon_delete,
                      FALSE, FALSE);
    rb_action_button (r, column, "Rename", _("Rename"), &rb_icon_rename, FALSE, FALSE);

    group = rb_group (page, _("New"));
    rb_action_button (r, group, "New Folder", _("New folder"), &rb_icon_new_folder, TRUE, FALSE);
    column = rb_column (group);
    rb_action_button (r, column, "New Empty File", _("New document"), &rb_icon_new_document,
                      FALSE, FALSE);

    group = rb_group (page, _("Open"));
    rb_action_button (r, group, "Properties", _("Properties"),
                      &rb_icon_properties, TRUE, FALSE);
    column = rb_column (group);
    rb_action_button (r, column, "Open", _("Open"), &rb_icon_open, FALSE, FALSE);
    rb_action_button (r, column, "OtherApplication1", _("Open with\xE2\x80\xA6"),
                      &rb_icon_open_with, FALSE, FALSE);

    group = rb_group (page, _("Select"));
    column = rb_column (group);
    rb_action_button (r, column, "Select All", _("Select all"), &rb_icon_select_all,
                      FALSE, FALSE);
    rb_command_button (r, column, LIRIA_SDE_CMD_COPY, _("Select none"),
                       &rb_icon_select_none, FALSE,
                       G_CALLBACK (rb_select_none));
    rb_action_button (r, column, "Invert Selection", _("Invert selection"),
                      &rb_icon_invert, FALSE, FALSE);
}

static void
rb_build_share (Ribbon *r)
{
    GtkWidget *page = rb_page (r, "share", _("Share"), NULL);
    GtkWidget *group, *column;
    char *program;

    group = rb_group (page, _("Send"));
    program = g_find_program_in_path ("xdg-email");
    if (program != NULL)
        rb_command_button (r, group, LIRIA_SDE_CMD_COPY, _("Email"), &rb_icon_email, TRUE, G_CALLBACK (rb_email));
    g_free (program);
    program = g_find_program_in_path ("taca");
    if (program != NULL)
        rb_command_button (r, group, LIRIA_SDE_CMD_COMPRESS, _("Compress"),
                           &rb_icon_compress, TRUE,
                           G_CALLBACK (rb_compress));
    g_free (program);

    group = rb_group (page, _("Drives"));
    rb_command_button (r, group, LIRIA_SDE_CMD_COPY, _("Copy to drive"),
                       &rb_icon_to_drive, TRUE,
                       G_CALLBACK (rb_drive_menu));
    column = rb_column (group);
    rb_action_button (r, column, "Create Link", _("Make link"), &rb_icon_make_link, FALSE,
                      FALSE);
}

static void
rb_build_view (Ribbon *r)
{
    static const struct { const char *id, *label; const RbIcon *icon; } views[] = {
        { "OAFIID:Liria_File_Manager_Icon_View", N_("Icons"), &rb_icon_view_icons },
        { "OAFIID:Liria_File_Manager_List_View", N_("Details"), &rb_icon_view_list },
        { "OAFIID:Liria_File_Manager_Compact_View", N_("Compact"), &rb_icon_view_compact },
    };
    GtkWidget *page = rb_page (r, "view", _("View"), NULL);
    GtkWidget *group, *column;
    guint i;

    group = rb_group (page, _("Panes"));
    rb_action_button (r, group, "Show Hide Sidebar", _("Navigation pane"),
                      &rb_icon_nav_pane,
                      TRUE, TRUE);
    column = rb_column (group);
    rb_action_button (r, column, "Show Hide Extra Pane", _("Extra pane"),
                      &rb_icon_extra_pane, FALSE, TRUE);
    rb_action_button (r, column, "Show Hide Statusbar", _("Status bar"),
                      &rb_icon_status_bar, FALSE, TRUE);

    group = rb_group (page, _("Layout"));
    column = rb_column (group);
    for (i = 0; i < G_N_ELEMENTS (views); i++)
    {
        GtkWidget *button = rb_button_new (r, _(views[i].label), views[i].icon, FALSE, TRUE);

        g_object_set_data (G_OBJECT (button), "sde-view-id", (gpointer) views[i].id);
        g_signal_connect (button, "toggled", G_CALLBACK (rb_view_toggled), r);
        g_signal_connect_after (button, "clicked", G_CALLBACK (rb_close_dropdown), r);
        gtk_box_pack_start (GTK_BOX (column), button, FALSE, FALSE, 0);
        r->view_buttons = g_list_append (r->view_buttons, button);
    }
    column = rb_column (group);
    rb_action_button (r, column, "Zoom In", _("Larger"), &rb_icon_zoom_in, FALSE, FALSE);
    rb_action_button (r, column, "Zoom Out", _("Smaller"), &rb_icon_zoom_out, FALSE, FALSE);
    rb_action_button (r, column, "Zoom Normal", _("Normal size"), &rb_icon_zoom_normal,
                      FALSE, FALSE);

    group = rb_group (page, _("Current view"));
    rb_command_button (r, group, LIRIA_SDE_CMD_PIN, _("Sort by"),
                       &rb_icon_sort, TRUE,
                       G_CALLBACK (rb_sort_menu));

    group = rb_group (page, _("Show/hide"));
    column = rb_column (group);
    rb_action_button (r, column, "Show Hidden Files", _("Hidden items"),
                      &rb_icon_hidden, FALSE, TRUE);
    rb_action_button (r, column, "Show Backup Files", _("Backup files"),
                      &rb_icon_backup, FALSE, TRUE);
    rb_command_button (r, group, LIRIA_SDE_CMD_PIN, _("Ribbon"),
                       &rb_icon_ribbon, TRUE,
                       G_CALLBACK (rb_mode_menu));
}

static void
rb_build_drive (Ribbon *r)
{
    GtkWidget *page = rb_page (r, "drive", _("Drive"), &r->drive_tab);
    GtkWidget *group, *column;
    char *program;

    gtk_style_context_add_class (gtk_widget_get_style_context (r->drive_tab), "sde-ribbon-contextual");
    group = rb_group (page, _("Drive"));
    rb_action_button (r, group, "Self Eject Volume", _("Eject"), &rb_icon_eject, TRUE, FALSE);
    column = rb_column (group);
    rb_action_button (r, column, "Self Unmount Volume", _("Unmount"), &rb_icon_eject,
                      FALSE, FALSE);
    rb_action_button (r, column, "Self Format Volume", _("Format\xE2\x80\xA6"),
                      &rb_icon_format, FALSE, FALSE);
    group = rb_group (page, _("Manage"));
    rb_action_button (r, group, "LocationProperties", _("Properties"),
                      &rb_icon_properties, TRUE, FALSE);
    program = g_find_program_in_path ("gnome-disks");
    if (program != NULL)
        rb_command_button (r, group, LIRIA_SDE_CMD_PIN, _("Disks"), &rb_icon_disks, TRUE, G_CALLBACK (rb_disks));
    g_free (program);
}

static void
rb_build_archive (Ribbon *r)
{
    GtkWidget *page = rb_page (r, "archive", _("Archive"), &r->archive_tab);
    GtkWidget *group;

    gtk_style_context_add_class (gtk_widget_get_style_context (r->archive_tab), "sde-ribbon-contextual");
    group = rb_group (page, _("Extract"));
    rb_command_button (r, group, LIRIA_SDE_CMD_EXTRACT, _("Extract here"),
                       &rb_icon_extract_here, TRUE,
                       G_CALLBACK (rb_extract_here));
    rb_command_button (r, group, LIRIA_SDE_CMD_EXTRACT, _("Extract to\xE2\x80\xA6"),
                       &rb_icon_extract_to, TRUE,
                       G_CALLBACK (rb_extract_to));
}

/* --------------------------------------------------- state and modes */

static void
rb_sync_tabs (Ribbon *r)
{
    const char *visible = gtk_stack_get_visible_child_name (GTK_STACK (r->stack));
    gboolean open = liria_sde_ribbon_get_mode () == LIRIA_SDE_RIBBON_FULL || r->dropped;
    GList *l;

    r->syncing = TRUE;
    for (l = r->tab_buttons; l != NULL; l = l->next)
        gtk_toggle_button_set_active (l->data, open &&
                                      g_strcmp0 (g_object_get_data (G_OBJECT (l->data), "sde-page"), visible) == 0);
    r->syncing = FALSE;
}

static void
ribbon_apply (Ribbon *r)
{
    LiriaSdeRibbonMode mode = liria_sde_ribbon_get_mode ();
    gboolean full = mode == LIRIA_SDE_RIBBON_FULL;

    gtk_widget_set_visible (r->root, mode != LIRIA_SDE_RIBBON_OFF);
    if (!full && mode != LIRIA_SDE_RIBBON_COLLAPSED)
        r->dropped = FALSE;
    if (full)
        r->dropped = FALSE;
    gtk_revealer_set_reveal_child (GTK_REVEALER (r->revealer), full || r->dropped);
    gtk_button_set_image (GTK_BUTTON (r->collapse),
                          gtk_image_new_from_icon_name (full ? "pan-up-symbolic" : "pan-down-symbolic",
                                                        GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text (r->collapse, full ? _("Collapse the ribbon (Ctrl+F1)")
                                                   : _("Keep the ribbon open (Ctrl+F1)"));
    gtk_style_context_remove_class (gtk_widget_get_style_context (r->root), "sde-ribbon-collapsed");
    if (!full)
        gtk_style_context_add_class (gtk_widget_get_style_context (r->root), "sde-ribbon-collapsed");
    rb_sync_tabs (r);
}

static void
rb_tab_toggled (GtkToggleButton *button, Ribbon *r)
{
    const char *page = g_object_get_data (G_OBJECT (button), "sde-page");
    const char *visible = gtk_stack_get_visible_child_name (GTK_STACK (r->stack));

    if (r->syncing)
        return;
    if (liria_sde_ribbon_get_mode () == LIRIA_SDE_RIBBON_COLLAPSED)
        r->dropped = !(r->dropped && g_strcmp0 (page, visible) == 0);
    gtk_stack_set_visible_child_name (GTK_STACK (r->stack), page);
    if (g_strcmp0 (page, "drive") != 0 && g_strcmp0 (page, "archive") != 0)
        liria_sde_prefs_set_string ("ribbon", "tab", page);
    ribbon_apply (r);
}

static void
rb_collapse_clicked (GtkButton *button, Ribbon *r)
{
    liria_sde_ribbon_set_mode (liria_sde_ribbon_mode_toggle (liria_sde_ribbon_get_mode ()));
}

static void rb_queue_refresh (Ribbon *r);

/* Connected swapped to the ribbon's root widget, so they end with it. */
static void
rb_action_notify (GtkWidget *root, GParamSpec *pspec, GtkAction *action)
{
    Ribbon *r = g_object_get_data (G_OBJECT (root), RIBBON_KEY);

    if (r != NULL)
        rb_queue_refresh (r);
}

static void
rb_action_toggled (GtkWidget *root, GtkToggleAction *action)
{
    Ribbon *r = g_object_get_data (G_OBJECT (root), RIBBON_KEY);

    if (r != NULL)
        rb_queue_refresh (r);
}

/* Buttons stay in place (a hidden action shows as a disabled button) and
 * follow the action's sensitivity and check state. The actions of the
 * view are replaced when the view changes, so they are looked up again. */
static void
rb_rebind (Ribbon *r)
{
    GList *l;

    r->syncing = TRUE;
    for (l = r->bound; l != NULL; l = l->next)
    {
        GtkWidget *button = l->data;
        GtkAction *action = rb_action (r, g_object_get_data (G_OBJECT (button), "sde-action"));

        gtk_widget_set_sensitive (button, action != NULL && gtk_action_is_visible (action) &&
                                          gtk_action_is_sensitive (action));
        if (action == NULL)
            continue;
        if (GTK_IS_TOGGLE_ACTION (action) && GTK_IS_TOGGLE_BUTTON (button))
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button),
                                          gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (action)));
        if (g_object_get_data (G_OBJECT (action), RIBBON_KEY) == NULL)
        {
            /* Follow changes made from menus, shortcuts and the view. */
            g_object_set_data (G_OBJECT (action), RIBBON_KEY, r);
            g_signal_connect_object (action, "notify::sensitive", G_CALLBACK (rb_action_notify), r->root,
                                     G_CONNECT_SWAPPED);
            g_signal_connect_object (action, "notify::visible", G_CALLBACK (rb_action_notify), r->root,
                                     G_CONNECT_SWAPPED);
            if (GTK_IS_TOGGLE_ACTION (action))
                g_signal_connect_object (action, "toggled", G_CALLBACK (rb_action_toggled), r->root,
                                         G_CONNECT_SWAPPED);
        }
    }
    r->syncing = FALSE;
}

static gboolean
rb_refresh_idle (gpointer data)
{
    Ribbon *r = data;
    LiriaSdeSelection selection = { 0, 0, 0, FALSE, FALSE };
    GList *files, *l;
    guint commands;
    LiriaWindowSlot *slot = rb_slot (r);
    const char *view_id = slot != NULL ? liria_window_slot_get_content_view_id (slot) : NULL;
    gboolean drive;

    r->refresh_id = 0;
    rb_rebind (r);
    /* 0.2.0 Alpha 2: the contextual Archive tab needs to know whether
     * every selected item is an archive, so the checks stop at the first
     * one that is not; they took a quarter of the time of Ctrl+A in a
     * folder of 10 000 files. */
    files = slot != NULL && slot->content_view != NULL ? liria_view_get_selection (slot->content_view) : NULL;
    selection.count = g_list_length (files);
    for (l = files; l != NULL; l = l->next)
    {
        LiriaFile *file = G_IS_FILE (l->data) ? liria_file_get_existing (l->data) : NULL;
        gboolean archive = file != NULL && rb_is_archive (file);

        if (file != NULL)
            liria_file_unref (file);
        if (!archive)
            break;
        selection.archives++;
    }
    g_list_free_full (files, g_object_unref);
    /* The folder accepts new files exactly when New Folder works; the
     * clipboard holds files when Paste works. */
    selection.writable = rb_action_usable (r, "New Folder");
    selection.clipboard = rb_action_usable (r, "Paste");
    commands = liria_sde_selection_commands (&selection);
    for (l = r->commands; l != NULL; l = l->next)
    {
        guint needs = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (l->data), "sde-needs"));

        gtk_widget_set_sensitive (l->data, (commands & needs) == needs);
    }

    r->syncing = TRUE;
    for (l = r->view_buttons; l != NULL; l = l->next)
        gtk_toggle_button_set_active (l->data, view_id != NULL &&
                                      g_strcmp0 (g_object_get_data (G_OBJECT (l->data), "sde-view-id"), view_id) == 0);
    r->syncing = FALSE;

    /* Contextual tabs. */
    drive = rb_action_usable (r, "Self Eject Volume") || rb_action_usable (r, "Self Unmount Volume");
    gtk_widget_set_visible (r->drive_tab, drive);
    gtk_widget_set_visible (r->archive_tab, (commands & LIRIA_SDE_CMD_EXTRACT) != 0);
    {
        const char *visible = gtk_stack_get_visible_child_name (GTK_STACK (r->stack));

        if ((g_strcmp0 (visible, "drive") == 0 && !drive) ||
            (g_strcmp0 (visible, "archive") == 0 && !(commands & LIRIA_SDE_CMD_EXTRACT)))
        {
            gtk_stack_set_visible_child_name (GTK_STACK (r->stack), "home");
            rb_sync_tabs (r);
        }
    }
    rb_qa (r);
    rb_qa_later (r);
    return G_SOURCE_REMOVE;
}

static void
rb_queue_refresh (Ribbon *r)
{
    if (r->refresh_id == 0 && r->ui != NULL)
        r->refresh_id = g_timeout_add (60, rb_refresh_idle, r);
}

static void
rb_changed (gpointer instance, Ribbon *r)
{
    rb_queue_refresh (r);
}

static void
rb_loading_uri (gpointer instance, const char *uri, Ribbon *r)
{
    rb_queue_refresh (r);
}

static void
rb_actions_changed (GtkUIManager *ui_manager, GtkWidget *root)
{
    Ribbon *r = g_object_get_data (G_OBJECT (root), RIBBON_KEY);

    if (r != NULL)
        rb_queue_refresh (r);
}

/* Connected swapped to the ribbon's root widget, so they end with it:
 * the icon theme or the interface scale changed. */
static void
rb_icon_theme_changed (GtkWidget *root, GtkIconTheme *theme)
{
    Ribbon *r = g_object_get_data (G_OBJECT (root), RIBBON_KEY);

    if (r != NULL)
        rb_icons_apply_all (r);
}

static void
rb_scale_changed (GtkWidget *root, GParamSpec *pspec, GtkSettings *settings)
{
    rb_icon_theme_changed (root, NULL);
}

static void
rb_window_destroyed (GtkWidget *window, Ribbon *r)
{
    if (r->refresh_id != 0)
    {
        g_source_remove (r->refresh_id);
        r->refresh_id = 0;
    }
    r->ui = NULL;
}

static void
ribbon_free (Ribbon *r)
{
    if (r->refresh_id != 0)
        g_source_remove (r->refresh_id);
    if (r->qa_id != 0)
        g_source_remove (r->qa_id);
    g_list_free (r->tab_buttons);
    g_list_free (r->bound);
    g_list_free (r->commands);
    g_list_free (r->view_buttons);
    g_list_free (r->images);
    g_free (r);
}

static gboolean
rb_accel_toggle (GtkAccelGroup *group, GObject *acceleratable, guint key, GdkModifierType mods, gpointer data)
{
    liria_sde_ribbon_set_mode (liria_sde_ribbon_mode_toggle (liria_sde_ribbon_get_mode ()));
    return TRUE;
}

/* A dropped-down ribbon closes when the files (or anything else outside
 * the ribbon) get the focus, e.g. after a click in the folder. */
static void
rb_focus_moved (GtkWindow *window, GtkWidget *focus, Ribbon *r)
{
    if (r->dropped && focus != NULL && !gtk_widget_is_ancestor (focus, r->root))
        rb_close_dropdown (focus, r);
}

static gboolean
rb_key_pressed (GtkWidget *window, GdkEventKey *event, Ribbon *r)
{
    /* Escape closes a dropped-down ribbon. */
    if (event->keyval == GDK_KEY_Escape && r->dropped)
    {
        rb_close_dropdown (window, r);
        return TRUE;
    }
    return FALSE;
}

LiriaSdeRibbonMode
liria_sde_ribbon_get_mode (void)
{
    if (sde_ribbon_mode < 0)
    {
        char *text = liria_sde_prefs_get_string ("ribbon", "mode", NULL);

        if (text != NULL)
            sde_ribbon_mode = liria_sde_ribbon_mode_parse (text);
        else
        {
            /* No choice yet: small screens (such as 1024x600) start with
             * the tabs only, so the files keep the room. */
            GdkScreen *screen = gdk_screen_get_default ();

            sde_ribbon_mode = screen != NULL && gdk_screen_get_height (screen) < 700
                              ? LIRIA_SDE_RIBBON_COLLAPSED : LIRIA_SDE_RIBBON_FULL;
        }
        g_free (text);
    }
    return sde_ribbon_mode;
}

void
liria_sde_ribbon_set_mode (LiriaSdeRibbonMode mode)
{
    GApplication *app = g_application_get_default ();
    GList *l;

    sde_ribbon_mode = mode;
    liria_sde_ribbon_mode_save (mode);
    if (app == NULL || !GTK_IS_APPLICATION (app))
        return;
    for (l = gtk_application_get_windows (GTK_APPLICATION (app)); l != NULL; l = l->next)
    {
        Ribbon *r = g_object_get_data (G_OBJECT (l->data), RIBBON_KEY);

        if (r != NULL)
            ribbon_apply (r);
    }
}

static void
rb_mode_item_toggled (GtkCheckMenuItem *item, gpointer data)
{
    if (gtk_check_menu_item_get_active (item))
        liria_sde_ribbon_set_mode (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "sde-mode")));
}

void
liria_sde_ribbon_add_mode_items (GtkWidget *menu)
{
    static const struct { LiriaSdeRibbonMode mode; const char *label; } modes[] = {
        { LIRIA_SDE_RIBBON_FULL, N_("_Ribbon") },
        { LIRIA_SDE_RIBBON_COLLAPSED, N_("Ribbon _Tabs Only") },
        { LIRIA_SDE_RIBBON_OFF, N_("_Compact Header (No Ribbon)") },
    };
    LiriaSdeRibbonMode current = liria_sde_ribbon_get_mode ();
    GSList *group = NULL;
    guint i;

    for (i = 0; i < G_N_ELEMENTS (modes); i++)
    {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic (group, _(modes[i].label));

        group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), modes[i].mode == current);
        g_object_set_data (G_OBJECT (item), "sde-mode", GINT_TO_POINTER (modes[i].mode));
        g_signal_connect (item, "toggled", G_CALLBACK (rb_mode_item_toggled), NULL);
        if (modes[i].mode == LIRIA_SDE_RIBBON_COLLAPSED)
        {
            GtkWidget *child = gtk_bin_get_child (GTK_BIN (item));

            if (GTK_IS_ACCEL_LABEL (child))
                gtk_accel_label_set_accel (GTK_ACCEL_LABEL (child), GDK_KEY_F1, GDK_CONTROL_MASK);
        }
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
        gtk_widget_show (item);
    }
}

/* 0.2.0: the mouse wheel scrolls a ribbon wider than the window sideways
 * (GTK only does that with Shift held); on a small screen at 150 % the
 * Open and Select groups were reachable only with the thin overlay
 * scroll bar. */
static gboolean
rb_scroll_sideways (GtkWidget *scroller, GdkEventScroll *event, gpointer data)
{
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (scroller));
    gdouble page = gtk_adjustment_get_page_size (adjustment);
    gdouble delta = 0.0, dx = 0.0, dy = 0.0;

    if (gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_lower (adjustment) <= page)
        return FALSE;
    switch (event->direction)
    {
    case GDK_SCROLL_UP:
    case GDK_SCROLL_LEFT:
        delta = -1.0;
        break;
    case GDK_SCROLL_DOWN:
    case GDK_SCROLL_RIGHT:
        delta = 1.0;
        break;
    case GDK_SCROLL_SMOOTH:
        if (gdk_event_get_scroll_deltas ((GdkEvent *) event, &dx, &dy))
            delta = dx + dy;
        break;
    default:
        break;
    }
    if (delta == 0.0)
        return FALSE;
    gtk_adjustment_set_value (adjustment, gtk_adjustment_get_value (adjustment) + delta * page / 4.0);
    return TRUE;
}

void
liria_sde_ribbon_setup (LiriaNavigationWindow *window, GtkUIManager *ui_manager)
{
    Ribbon *r = g_new0 (Ribbon, 1);
    GtkWidget *strip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkAccelGroup *accel = gtk_accel_group_new ();
    char *tab;

    r->window = window;
    r->ui = ui_manager;
    r->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (r->root), "sde-ribbon");
    gtk_widget_set_hexpand (r->root, TRUE);
    gtk_widget_set_vexpand (r->root, FALSE);

    r->tabs = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class (gtk_widget_get_style_context (strip), "sde-ribbon-tabs");
    gtk_box_pack_start (GTK_BOX (strip), r->tabs, TRUE, TRUE, 0);
    r->collapse = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (r->collapse), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (r->collapse, FALSE);
    gtk_style_context_add_class (gtk_widget_get_style_context (r->collapse), "sde-ribbon-collapse");
    g_signal_connect (r->collapse, "clicked", G_CALLBACK (rb_collapse_clicked), r);
    gtk_box_pack_end (GTK_BOX (strip), r->collapse, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (r->root), strip, FALSE, FALSE, 0);

    r->stack = gtk_stack_new ();
    gtk_stack_set_homogeneous (GTK_STACK (r->stack), FALSE);
    gtk_style_context_add_class (gtk_widget_get_style_context (r->stack), "sde-ribbon-body");
    r->revealer = gtk_revealer_new ();
    gtk_revealer_set_transition_type (GTK_REVEALER (r->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration (GTK_REVEALER (r->revealer), 120);
    {
        /* The groups never make the window wider: on a narrow window or
         * a large interface scale the ribbon scrolls sideways. */
        GtkWidget *scroller = gtk_scrolled_window_new (NULL, NULL);

        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
        gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), TRUE);
        gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroller), GTK_SHADOW_NONE);
        gtk_style_context_add_class (gtk_widget_get_style_context (scroller), "sde-ribbon-scroller");
        gtk_widget_add_events (scroller, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
        g_signal_connect (scroller, "scroll-event", G_CALLBACK (rb_scroll_sideways), NULL);
        gtk_container_add (GTK_CONTAINER (scroller), r->stack);
        gtk_container_add (GTK_CONTAINER (r->revealer), scroller);
    }
    gtk_box_pack_start (GTK_BOX (r->root), r->revealer, FALSE, FALSE, 0);

    rb_build_home (r);
    rb_build_share (r);
    rb_build_view (r);
    rb_build_drive (r);
    rb_build_archive (r);
    {
        GList *l;

        for (l = r->tab_buttons; l != NULL; l = l->next)
            g_signal_connect (l->data, "toggled", G_CALLBACK (rb_tab_toggled), r);
    }

    gtk_widget_show_all (r->root);
    gtk_widget_set_no_show_all (r->root, TRUE);
    gtk_widget_hide (r->drive_tab);
    gtk_widget_hide (r->archive_tab);
    tab = liria_sde_prefs_get_string ("ribbon", "tab", "home");
    gtk_stack_set_visible_child_name (GTK_STACK (r->stack),
                                      g_strcmp0 (tab, "share") == 0 || g_strcmp0 (tab, "view") == 0 ? tab : "home");
    g_free (tab);

    gtk_grid_attach (GTK_GRID (LIRIA_WINDOW (window)->details->grid), r->root, 0, 2, 1, 1);
    g_object_set_data_full (G_OBJECT (window), RIBBON_KEY, r, (GDestroyNotify) ribbon_free);

    g_object_set_data (G_OBJECT (r->root), RIBBON_KEY, r);
    g_signal_connect_object (ui_manager, "actions-changed", G_CALLBACK (rb_actions_changed), r->root, 0);
    /* 0.2.0 Alpha 2: the icons follow the icon theme and the interface
     * scale while the window is open. */
    g_signal_connect_object (gtk_icon_theme_get_default (), "changed", G_CALLBACK (rb_icon_theme_changed), r->root,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (gtk_settings_get_default (), "notify::gtk-xft-dpi", G_CALLBACK (rb_scale_changed),
                             r->root, G_CONNECT_SWAPPED);
    rb_qa (r);
    g_signal_connect (window, "destroy", G_CALLBACK (rb_window_destroyed), r);
    g_signal_connect (window, "selection-changed", G_CALLBACK (rb_changed), r);
    g_signal_connect (window, "loading-uri", G_CALLBACK (rb_loading_uri), r);
    g_signal_connect (window, "key-press-event", G_CALLBACK (rb_key_pressed), r);
    g_signal_connect (window, "set-focus", G_CALLBACK (rb_focus_moved), r);

    /* Ctrl+F1 collapses and expands the ribbon, as in other ribbons. */
    gtk_accel_group_connect (accel, GDK_KEY_F1, GDK_CONTROL_MASK, 0,
                             g_cclosure_new (G_CALLBACK (rb_accel_toggle), NULL, NULL));
    gtk_window_add_accel_group (GTK_WINDOW (window), accel);
    g_object_unref (accel);

    ribbon_apply (r);
    rb_queue_refresh (r);
}

G_GNUC_END_IGNORE_DEPRECATIONS
