/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 DebION Dev-Team
 *
 * Liria window chrome for SDE.
 *
 * One compact header row: Back, Forward and Parent, the breadcrumb path
 * (with its edit toggle and the search field), a linked view switcher, the
 * search button and one main menu. All buttons use 16 px symbolic icons.
 * The classic menu bar stays in the window, hidden, so every action keeps
 * its keyboard shortcut; the main menu is built from the same GtkActions
 * each time it opens, so check marks and sensitivity are always current.
 *
 * Also here: the keyboard shortcuts window, the About window, local help,
 * Liria's style sheet (standard GTK named colours only, so it follows any
 * light, dark or accent theme) and the "sde-copy-to" application action
 * used by the SDE Tools "Send To" menu.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include <libliria-private/liria-bookmark.h>
#include <libliria-private/liria-file-operations.h>
#include <libliria-private/liria-sidebar.h>
#include <libliria-private/liria-view-factory.h>

#include "liria-sde-ui.h"
#include "liria-bookmark-list.h"
#include "liria-navigation-window.h"
#include "liria-navigation-window-pane.h"
#include "liria-side-pane.h"
#include "liria-window-private.h"
#include "liria-window-slot.h"

#define SDE_ICON_VIEW_ID    "OAFIID:Liria_File_Manager_Icon_View"
#define SDE_LIST_VIEW_ID    "OAFIID:Liria_File_Manager_List_View"
#define SDE_COMPACT_VIEW_ID "OAFIID:Liria_File_Manager_Compact_View"
#define SDE_HELP_FILE       LIRIA_DATADIR "/help/index.html"
#define SDE_PROJECT_URL     "https://github.com/hugomonizdorego/SDE-Desktop"

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* ------------------------------------------------------------ helpers */

static const char *
sde_icon (const char *first, ...)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default ();
    const char *name = first, *last = first;
    va_list args;

    va_start (args, first);
    while (name != NULL)
    {
        last = name;
        if (gtk_icon_theme_has_icon (theme, name))
            break;
        name = va_arg (args, const char *);
    }
    va_end (args);
    return name != NULL ? name : last;
}

/* A themed icon with the -symbolic variant of each name first, so places
 * and drives use small monochrome icons that follow the text colour. */
GIcon *
liria_sde_ui_symbolic_icon (GIcon *icon)
{
    const char * const *names;
    GPtrArray *list;
    GIcon *result;
    guint i;

    if (icon == NULL || !G_IS_THEMED_ICON (icon))
        return icon != NULL ? g_object_ref (icon) : NULL;
    names = g_themed_icon_get_names (G_THEMED_ICON (icon));
    list = g_ptr_array_new_with_free_func (g_free);
    for (i = 0; names != NULL && names[i] != NULL; i++)
    {
        if (g_str_has_suffix (names[i], "-symbolic"))
            g_ptr_array_add (list, g_strdup (names[i]));
        else
            g_ptr_array_add (list, g_strconcat (names[i], "-symbolic", NULL));
    }
    for (i = 0; names != NULL && names[i] != NULL; i++)
        g_ptr_array_add (list, g_strdup (names[i]));
    g_ptr_array_add (list, NULL);
    result = g_themed_icon_new_from_names ((char **) list->pdata, -1);
    g_ptr_array_free (list, TRUE);
    return result;
}

static GtkAction *
sde_find_action (GtkUIManager *ui_manager, const char *name)
{
    GList *l;

    for (l = gtk_ui_manager_get_action_groups (ui_manager); l != NULL; l = l->next)
    {
        GtkAction *action = gtk_action_group_get_action (l->data, name);

        if (action != NULL)
            return action;
    }
    return NULL;
}

static void
sde_set_symbolic (GtkUIManager *ui_manager, const char *name, const char *icon)
{
    GtkAction *action = sde_find_action (ui_manager, name);

    if (action != NULL)
        g_object_set (action, "icon-name", icon, "gicon", NULL, NULL);
}

static GtkWidget *
sde_append (GtkWidget *menu, GtkWidget *item)
{
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    gtk_widget_show (item);
    return item;
}

static void
sde_separator (GtkWidget *menu)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (menu));
    GList *last = g_list_last (children);

    /* No leading or doubled separators. */
    if (last != NULL && !GTK_IS_SEPARATOR_MENU_ITEM (last->data))
        sde_append (menu, gtk_separator_menu_item_new ());
    g_list_free (children);
}

/* A menu item proxy for an existing action: same label, shortcut,
 * sensitivity and check state as the hidden menu bar item. */
static GtkWidget *
sde_action_item (GtkWidget *menu, GtkUIManager *ui_manager, const char *name, const char *label)
{
    GtkAction *action = sde_find_action (ui_manager, name);
    GtkWidget *item;

    if (action == NULL || !gtk_action_is_visible (action))
        return NULL;
    item = gtk_action_create_menu_item (action);
    if (label != NULL)
    {
        gtk_activatable_set_use_action_appearance (GTK_ACTIVATABLE (item), FALSE);
        gtk_menu_item_set_use_underline (GTK_MENU_ITEM (item), TRUE);
        gtk_menu_item_set_label (GTK_MENU_ITEM (item), label);
    }
    return sde_append (menu, item);
}

static GtkWidget *
sde_submenu (GtkWidget *menu, const char *label)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic (label);
    GtkWidget *submenu = gtk_menu_new ();

    gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);
    sde_append (menu, item);
    return submenu;
}

static void
sde_drop_if_empty (GtkWidget *submenu)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (submenu));

    if (children == NULL)
        gtk_widget_destroy (gtk_menu_get_attach_widget (GTK_MENU (submenu)));
    g_list_free (children);
}

static LiriaWindowSlot *
sde_active_slot (LiriaNavigationWindow *window)
{
    return liria_window_get_active_slot (LIRIA_WINDOW (window));
}

static char *
sde_current_path (LiriaNavigationWindow *window)
{
    LiriaWindowSlot *slot = sde_active_slot (window);
    GFile *location = slot != NULL ? liria_window_slot_get_location (slot) : NULL;
    char *path = location != NULL ? g_file_get_path (location) : NULL;

    if (location != NULL)
        g_object_unref (location);
    return path;
}

/* ------------------------------------------------------- open terminal */

static void
sde_open_terminal (GtkMenuItem *item, LiriaNavigationWindow *window)
{
    const char *programs[] = { "x-terminal-emulator", "sde-terminal", "xfce4-terminal",
                               "lxterminal", "xterm", NULL };
    char *path = sde_current_path (window);
    char *program = NULL;
    GError *error = NULL;
    int i;

    for (i = 0; programs[i] != NULL && program == NULL; i++)
        program = g_find_program_in_path (programs[i]);
    if (program != NULL)
    {
        char *argv[] = { program, NULL };

        if (!g_spawn_async (path != NULL ? path : g_get_home_dir (), argv, NULL,
                            G_SPAWN_DEFAULT, NULL, NULL, NULL, &error))
        {
            g_warning ("Liria: %s", error->message);
            g_error_free (error);
        }
    }
    g_free (program);
    g_free (path);
}

/* -------------------------------------------------------------- views */

static void
sde_switch_view (LiriaNavigationWindow *window, const char *id)
{
    LiriaWindowSlot *slot = sde_active_slot (window);
    GList *l;

    if (slot == NULL || liria_window_slot_content_view_matches_iid (slot, id))
        return;
    for (l = LIRIA_WINDOW (window)->details->short_list_viewers; l != NULL; l = l->next)
    {
        if (g_strcmp0 (l->data, id) == 0)
        {
            liria_window_slot_set_content_view (slot, id);
            return;
        }
    }
}

static void
sde_view_item_toggled (GtkCheckMenuItem *item, LiriaNavigationWindow *window)
{
    if (gtk_check_menu_item_get_active (item))
        sde_switch_view (window, g_object_get_data (G_OBJECT (item), "sde-view-id"));
}

static void
sde_add_view_items (GtkWidget *menu, LiriaNavigationWindow *window)
{
    LiriaWindowSlot *slot = sde_active_slot (window);
    GSList *group = NULL;
    GList *l;

    for (l = LIRIA_WINDOW (window)->details->short_list_viewers; l != NULL; l = l->next)
    {
        const LiriaViewInfo *info = liria_view_factory_lookup (l->data);
        GtkWidget *item;

        if (info == NULL)
            continue;
        item = gtk_radio_menu_item_new_with_mnemonic (group, _(info->view_menu_label_with_mnemonic));
        group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item),
                                        slot != NULL && liria_window_slot_content_view_matches_iid (slot, l->data));
        g_object_set_data_full (G_OBJECT (item), "sde-view-id", g_strdup (l->data), g_free);
        g_signal_connect (item, "toggled", G_CALLBACK (sde_view_item_toggled), window);
        sde_append (menu, item);
    }
}

/* --------------------------------------------------------------- sort */

static GtkTreeView *
sde_list_tree_view (LiriaNavigationWindow *window)
{
    LiriaWindowSlot *slot = sde_active_slot (window);
    GtkWidget *view, *child;

    if (slot == NULL || slot->content_view == NULL || !GTK_IS_BIN (slot->content_view))
        return NULL;
    view = GTK_WIDGET (slot->content_view);
    child = gtk_bin_get_child (GTK_BIN (view));
    return GTK_IS_TREE_VIEW (child) ? GTK_TREE_VIEW (child) : NULL;
}

static void
sde_sort_column_toggled (GtkCheckMenuItem *item, GtkTreeView *tree)
{
    GtkTreeViewColumn *column = g_object_get_data (G_OBJECT (item), "sde-column");
    GtkTreeModel *model = gtk_tree_view_get_model (tree);
    int current;
    GtkSortType order;

    if (!gtk_check_menu_item_get_active (item) || !GTK_IS_TREE_SORTABLE (model))
        return;
    if (!gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), &current, &order))
        order = GTK_SORT_ASCENDING;
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (model),
                                          gtk_tree_view_column_get_sort_column_id (column), order);
}

static void
sde_sort_reversed_toggled (GtkCheckMenuItem *item, GtkTreeView *tree)
{
    GtkTreeModel *model = gtk_tree_view_get_model (tree);
    int current;
    GtkSortType order;

    if (!GTK_IS_TREE_SORTABLE (model) ||
        !gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), &current, &order))
        return;
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (model), current,
                                          gtk_check_menu_item_get_active (item)
                                          ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

/* The list view sorts by its columns; offer the visible ones. */
static void
sde_add_list_sort_items (GtkWidget *menu, GtkTreeView *tree)
{
    GtkTreeModel *model = gtk_tree_view_get_model (tree);
    GList *columns, *l;
    GSList *group = NULL;
    GtkWidget *item;
    int current = -1;
    GtkSortType order = GTK_SORT_ASCENDING;

    if (!GTK_IS_TREE_SORTABLE (model))
        return;
    gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), &current, &order);
    columns = gtk_tree_view_get_columns (tree);
    for (l = columns; l != NULL; l = l->next)
    {
        GtkTreeViewColumn *column = l->data;
        const char *title = gtk_tree_view_column_get_title (column);
        int id = gtk_tree_view_column_get_sort_column_id (column);

        if (!gtk_tree_view_column_get_visible (column) || id < 0 || title == NULL || *title == '\0')
            continue;
        item = gtk_radio_menu_item_new_with_label (group, title);
        group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), id == current);
        g_object_set_data (G_OBJECT (item), "sde-column", column);
        g_signal_connect (item, "toggled", G_CALLBACK (sde_sort_column_toggled), tree);
        sde_append (menu, item);
    }
    g_list_free (columns);
    sde_separator (menu);
    item = gtk_check_menu_item_new_with_mnemonic (_("_Reversed Order"));
    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), order == GTK_SORT_DESCENDING);
    g_signal_connect (item, "toggled", G_CALLBACK (sde_sort_reversed_toggled), tree);
    sde_append (menu, item);
}

static void
sde_add_sort_menu (GtkWidget *menu, LiriaNavigationWindow *window, GtkUIManager *ui_manager)
{
    static const char *icon_actions[] = {
        "Sort by Name", "Sort by Size", "Sort by Type", "Sort by Modification Date",
        "Sort by Extension", "Sort by Trash Time", NULL
    };
    GtkWidget *sort = sde_submenu (menu, _("_Sort"));
    GtkTreeView *tree = sde_list_tree_view (window);
    int i;

    if (tree != NULL)
    {
        sde_add_list_sort_items (sort, tree);
    }
    else
    {
        for (i = 0; icon_actions[i] != NULL; i++)
            sde_action_item (sort, ui_manager, icon_actions[i], NULL);
        sde_separator (sort);
        sde_action_item (sort, ui_manager, "Reversed Order", NULL);
        sde_action_item (sort, ui_manager, "Keep Aligned", NULL);
    }
    sde_drop_if_empty (sort);
}

/* ------------------------------------------------------------ sidebar */

static void
sde_sidebar_item_toggled (GtkCheckMenuItem *item, LiriaNavigationWindow *window)
{
    GtkWidget *panel = g_object_get_data (G_OBJECT (item), "sde-panel");

    if (gtk_check_menu_item_get_active (item) && window->sidebar != NULL &&
        g_list_find (window->sidebar_panels, panel) != NULL)
        liria_side_pane_show_panel (window->sidebar, panel);
}

static void
sde_add_sidebar_items (GtkWidget *menu, LiriaNavigationWindow *window, GtkUIManager *ui_manager)
{
    GtkWidget *current = window->sidebar != NULL ? liria_side_pane_get_current_panel (window->sidebar) : NULL;
    GSList *group = NULL;
    GList *l, *panels;

    sde_action_item (menu, ui_manager, "Show Hide Sidebar", _("Show _Sidebar"));
    if (window->sidebar == NULL)
        return;
    sde_separator (menu);
    panels = g_list_reverse (g_list_copy (window->sidebar_panels));
    for (l = panels; l != NULL; l = l->next)
    {
        char *label = liria_sidebar_get_tab_label (LIRIA_SIDEBAR (l->data));
        GtkWidget *item = gtk_radio_menu_item_new_with_label (group, label != NULL ? label : "");

        group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), l->data == (gpointer) current);
        g_object_set_data (G_OBJECT (item), "sde-panel", l->data);
        g_signal_connect (item, "toggled", G_CALLBACK (sde_sidebar_item_toggled), window);
        sde_append (menu, item);
        g_free (label);
    }
    g_list_free (panels);
}

/* ---------------------------------------------------------- bookmarks */

static void
sde_bookmark_activate (GtkMenuItem *item, LiriaNavigationWindow *window)
{
    GFile *location = g_object_get_data (G_OBJECT (item), "sde-location");

    if (location != NULL)
        liria_window_go_to (LIRIA_WINDOW (window), location);
}

static void
sde_add_bookmark_items (GtkWidget *menu, LiriaNavigationWindow *window, GtkUIManager *ui_manager)
{
    LiriaBookmarkList *list = LIRIA_WINDOW (window)->details->bookmark_list;
    guint i, n;

    sde_action_item (menu, ui_manager, "Add Bookmark", NULL);
    sde_action_item (menu, ui_manager, "Edit Bookmarks", NULL);
    if (list == NULL)
        return;
    n = liria_bookmark_list_length (list);
    if (n > 0)
        sde_separator (menu);
    for (i = 0; i < n && i < 30; i++)
    {
        LiriaBookmark *bookmark = liria_bookmark_list_item_at (list, i);
        GtkWidget *item = liria_bookmark_menu_item_new (bookmark);

        if (item == NULL)
            continue;
        g_object_set_data_full (G_OBJECT (item), "sde-location",
                                liria_bookmark_get_location (bookmark), g_object_unref);
        g_signal_connect (item, "activate", G_CALLBACK (sde_bookmark_activate), window);
        sde_append (menu, item);
    }
}

/* ------------------------------------------------------- the main menu */

static void
sde_menu_item_activate (GtkMenuItem *item, GCallback callback)
{
    GtkWidget *toplevel = GTK_WIDGET (g_object_get_data (G_OBJECT (item), "sde-window"));

    ((void (*) (GtkWindow *)) callback) (GTK_WINDOW (toplevel));
}

static void
sde_window_item (GtkWidget *menu, LiriaNavigationWindow *window, const char *label,
                 const char *accel, void (*callback) (GtkWindow *))
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic (label);

    if (accel != NULL)
    {
        guint key;
        GdkModifierType mods;
        GtkWidget *child = gtk_bin_get_child (GTK_BIN (item));

        gtk_accelerator_parse (accel, &key, &mods);
        if (GTK_IS_ACCEL_LABEL (child))
            gtk_accel_label_set_accel (GTK_ACCEL_LABEL (child), key, mods);
    }
    g_object_set_data (G_OBJECT (item), "sde-window", window);
    g_signal_connect (item, "activate", G_CALLBACK (sde_menu_item_activate), (gpointer) callback);
    sde_append (menu, item);
}

static void
sde_show_help_main (GtkWindow *parent)
{
    liria_sde_ui_show_help (parent, NULL);
}

static GtkWidget *
sde_build_main_menu (LiriaNavigationWindow *window)
{
    GtkUIManager *ui_manager = liria_window_get_ui_manager (LIRIA_WINDOW (window));
    GtkWidget *menu = gtk_menu_new ();
    GtkWidget *item, *sub;
    char *path;

    gtk_style_context_add_class (gtk_widget_get_style_context (menu), "sde-main-menu");
    sde_action_item (menu, ui_manager, "New Window", NULL);
    sde_action_item (menu, ui_manager, "New Tab", NULL);
    sde_separator (menu);

    item = sde_append (menu, gtk_menu_item_new_with_mnemonic (_("Open _Terminal Here")));
    path = sde_current_path (window);
    gtk_widget_set_sensitive (item, path != NULL);
    g_free (path);
    g_signal_connect (item, "activate", G_CALLBACK (sde_open_terminal), window);
    sde_action_item (menu, ui_manager, "Connect to Server", NULL);
    sde_separator (menu);

    sde_action_item (menu, ui_manager, "Show Hidden Files", NULL);
    sde_add_sort_menu (menu, window, ui_manager);

    sub = sde_submenu (menu, _("_View"));
    sde_add_view_items (sub, window);
    sde_separator (sub);
    sde_action_item (sub, ui_manager, "Zoom In", NULL);
    sde_action_item (sub, ui_manager, "Zoom Out", NULL);
    sde_action_item (sub, ui_manager, "Zoom Normal", NULL);
    sde_separator (sub);
    sde_add_sidebar_items (sub, window, ui_manager);
    sde_separator (sub);
    sde_action_item (sub, ui_manager, "Show Hide Statusbar", _("Status _Bar"));
    sde_action_item (sub, ui_manager, "Show Hide Extra Pane", _("_Extra Pane"));
    sde_action_item (sub, ui_manager, "Show Backup Files", NULL);
    sde_separator (sub);
    sde_action_item (sub, ui_manager, "Reload", NULL);

    sub = sde_submenu (menu, _("_Edit"));
    sde_action_item (sub, ui_manager, "Undo", NULL);
    sde_action_item (sub, ui_manager, "Redo", NULL);
    sde_separator (sub);
    sde_action_item (sub, ui_manager, "Select All", NULL);
    sde_action_item (sub, ui_manager, "Select Pattern", NULL);
    sde_action_item (sub, ui_manager, "Invert Selection", NULL);
    sde_separator (sub);
    sde_action_item (sub, ui_manager, "Go to Location", _("_Enter Location..."));
    sde_action_item (sub, ui_manager, "Empty Trash", NULL);
    sde_drop_if_empty (sub);

    sub = sde_submenu (menu, _("_Bookmarks"));
    sde_add_bookmark_items (sub, window, ui_manager);
    sde_separator (menu);

    sde_action_item (menu, ui_manager, "Preferences", NULL);
    sde_window_item (menu, window, _("_Keyboard Shortcuts"), "<Control>question", liria_sde_ui_show_shortcuts);
    sde_window_item (menu, window, _("_Help"), "F1", sde_show_help_main);
    sde_window_item (menu, window, _("_About Liria"), NULL, liria_sde_ui_show_about);
    sde_separator (menu);
    sde_action_item (menu, ui_manager, "Close", _("_Close Window"));
    return menu;
}

static gboolean
sde_destroy_menu_idle (gpointer menu)
{
    gtk_widget_destroy (menu);
    return G_SOURCE_REMOVE;
}

static void
sde_menu_deactivate (GtkMenuShell *menu, GtkToggleButton *button)
{
    gtk_toggle_button_set_active (button, FALSE);
    g_idle_add (sde_destroy_menu_idle, menu);
}

static void
sde_popup_main_menu (GtkToggleButton *button, LiriaNavigationWindow *window)
{
    GtkWidget *menu;

    if (!gtk_toggle_button_get_active (button) || g_object_get_data (G_OBJECT (button), "sde-open"))
        return;
    menu = sde_build_main_menu (window);
    gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (button), NULL);
    g_signal_connect (menu, "deactivate", G_CALLBACK (sde_menu_deactivate), button);
    gtk_menu_popup_at_widget (GTK_MENU (menu), GTK_WIDGET (button),
                              GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
    gtk_menu_shell_select_first (GTK_MENU_SHELL (menu), FALSE);
    gtk_menu_shell_deselect (GTK_MENU_SHELL (menu));
}

static gboolean
sde_accel_main_menu (GtkAccelGroup *group, GObject *acceleratable, guint key,
                     GdkModifierType mods, gpointer button)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
    return TRUE;
}

static gboolean
sde_accel_window_call (GtkAccelGroup *group, GObject *acceleratable, guint key,
                       GdkModifierType mods, gpointer callback)
{
    ((void (*) (GtkWindow *)) callback) (GTK_WINDOW (acceleratable));
    return TRUE;
}

static gboolean
sde_accel_view (GtkAccelGroup *group, GObject *acceleratable, guint key,
                GdkModifierType mods, gpointer id)
{
    sde_switch_view (LIRIA_NAVIGATION_WINDOW (acceleratable), id);
    return TRUE;
}

static void
sde_connect_accel (GtkAccelGroup *group, const char *accel, GCallback callback, gpointer data)
{
    guint key;
    GdkModifierType mods;

    gtk_accelerator_parse (accel, &key, &mods);
    gtk_accel_group_connect (group, key, mods, 0, g_cclosure_new (callback, data, NULL));
}

static GtkToolItem *
sde_main_menu_item (LiriaNavigationWindow *window)
{
    GtkToolItem *item = gtk_tool_item_new ();
    GtkWidget *button = gtk_toggle_button_new ();
    GtkWidget *image = gtk_image_new_from_icon_name (sde_icon ("open-menu-symbolic", "open-menu",
                                                               "view-more-symbolic", NULL),
                                                     GTK_ICON_SIZE_MENU);
    GtkAccelGroup *accel = gtk_accel_group_new ();

    gtk_container_add (GTK_CONTAINER (button), image);
    gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (button, FALSE);
    gtk_widget_set_tooltip_text (button, _("Main Menu"));
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "sde-main-menu-button");
    g_signal_connect (button, "toggled", G_CALLBACK (sde_popup_main_menu), window);
    gtk_container_add (GTK_CONTAINER (item), button);
    gtk_widget_show_all (GTK_WIDGET (item));

    /* F10 opens the main menu, as it opened the menu bar before. */
    sde_connect_accel (accel, "F10", G_CALLBACK (sde_accel_main_menu), button);
    sde_connect_accel (accel, "<Control>question", G_CALLBACK (sde_accel_window_call), liria_sde_ui_show_shortcuts);
    sde_connect_accel (accel, "<Control>F1", G_CALLBACK (sde_accel_window_call), liria_sde_ui_show_shortcuts);
    sde_connect_accel (accel, "<Control>1", G_CALLBACK (sde_accel_view), (gpointer) SDE_ICON_VIEW_ID);
    sde_connect_accel (accel, "<Control>2", G_CALLBACK (sde_accel_view), (gpointer) SDE_LIST_VIEW_ID);
    sde_connect_accel (accel, "<Control>3", G_CALLBACK (sde_accel_view), (gpointer) SDE_COMPACT_VIEW_ID);
    gtk_window_add_accel_group (GTK_WINDOW (window), accel);
    g_object_unref (accel);
    return item;
}

/* ------------------------------------------------------- header layout */

static gint
sde_toolbar_index (GtkWidget *toolbar, GtkUIManager *ui_manager, const char *path)
{
    GtkWidget *widget = gtk_ui_manager_get_widget (ui_manager, path);

    if (widget == NULL || gtk_widget_get_parent (widget) != toolbar)
        return -1;
    return gtk_toolbar_get_item_index (GTK_TOOLBAR (toolbar), GTK_TOOL_ITEM (widget));
}

/* The first pane's location row (path bar, edit toggle and search field)
 * moves into the header after the Parent button. */
static void
sde_merge_location_row (GtkWidget *toolbar, GtkUIManager *ui_manager, LiriaNavigationWindowPane *pane)
{
    GtkToolItem *item = gtk_tool_item_new ();
    GtkWidget *row = pane->location_bar;
    gint index = sde_toolbar_index (toolbar, ui_manager, "/Toolbar/Up");

    g_object_ref (row);
    gtk_container_remove (GTK_CONTAINER (gtk_widget_get_parent (row)), row);
    gtk_container_add (GTK_CONTAINER (item), row);
    g_object_unref (row);
    gtk_container_set_border_width (GTK_CONTAINER (row), 0);
    gtk_box_set_spacing (GTK_BOX (row), 4);
    gtk_widget_set_margin_start (row, 6);
    gtk_widget_set_margin_end (row, 6);
    gtk_widget_set_valign (row, GTK_ALIGN_CENTER);
    gtk_tool_item_set_expand (item, TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (item)), "sde-location-item");
    gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, index >= 0 ? index + 1 : -1);
    gtk_widget_show (GTK_WIDGET (item));
    g_signal_connect_object (pane->widget, "destroy", G_CALLBACK (gtk_widget_destroy), item, G_CONNECT_SWAPPED);
}

/* SDE: opens a location in this window's active tab. The SDE Home
 * overview (an extension) finds it as the "sde-go-to-uri" object data. */
static void
sde_go_to_uri (GtkWidget *widget, const char *uri)
{
    GFile *location = g_file_new_for_uri (uri);

    liria_window_go_to (LIRIA_WINDOW (widget), location);
    g_object_unref (location);
}

void
liria_sde_ui_setup_header (LiriaNavigationWindow *window,
                           GtkWidget *toolbar,
                           GtkUIManager *ui_manager,
                           LiriaNavigationWindowPane *pane)
{
    LiriaWindow *win = LIRIA_WINDOW (window);
    GtkToolItem *menu_item;

    sde_set_symbolic (ui_manager, "Back", sde_icon ("go-previous-symbolic", "go-previous", NULL));
    sde_set_symbolic (ui_manager, "Forward", sde_icon ("go-next-symbolic", "go-next", NULL));
    sde_set_symbolic (ui_manager, "Up", sde_icon ("go-up-symbolic", "go-up", NULL));
    sde_set_symbolic (ui_manager, "Reload", sde_icon ("view-refresh-symbolic", "view-refresh", NULL));
    sde_set_symbolic (ui_manager, "Search", sde_icon ("edit-find-symbolic", "system-search-symbolic",
                                                      "edit-find", NULL));

    gtk_toolbar_set_style (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size (GTK_TOOLBAR (toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_toolbar_set_show_arrow (GTK_TOOLBAR (toolbar), FALSE);
    gtk_style_context_add_class (gtk_widget_get_style_context (toolbar), "sde-header");

    sde_merge_location_row (toolbar, ui_manager, pane);

    menu_item = sde_main_menu_item (window);
    gtk_toolbar_insert (GTK_TOOLBAR (toolbar), menu_item, -1);

    /* The menu bar stays for its accelerators, but is never shown. */
    gtk_widget_hide (win->details->menubar);
    gtk_widget_set_no_show_all (win->details->menubar, TRUE);

    g_object_set_data (G_OBJECT (window), "sde-go-to-uri", sde_go_to_uri);
}

/* The sidebar header ("Places" chooser and close button) is replaced by
 * View > Sidebar in the main menu. */
void
liria_sde_ui_tidy_sidebar (LiriaNavigationWindow *window)
{
    GtkWidget *title;

    if (window->sidebar == NULL)
        return;
    title = liria_side_pane_get_title (window->sidebar);
    if (title != NULL)
    {
        gtk_widget_hide (title);
        gtk_widget_set_no_show_all (title, TRUE);
    }
}

/* ---------------------------------------------------- shortcuts window */

static const char sde_shortcuts_ui[] =
"<interface>"
" <object class='GtkShortcutsWindow' id='window'>"
"  <property name='modal'>1</property>"
"  <child><object class='GtkShortcutsSection'>"
"   <property name='visible'>1</property><property name='section-name'>liria</property>"
"   <property name='max-height'>12</property>"
"   %s"
"  </object></child>"
" </object>"
"</interface>";

typedef struct { const char *accel; const char *title; } SdeShortcut;

static void
sde_shortcut_group (GString *ui, const char *title, const SdeShortcut *list)
{
    char *escaped = g_markup_escape_text (title, -1);

    g_string_append_printf (ui, "<child><object class='GtkShortcutsGroup'><property name='visible'>1</property>"
                                "<property name='title'>%s</property>", escaped);
    g_free (escaped);
    for (; list->accel != NULL; list++)
    {
        char *accel = g_markup_escape_text (list->accel, -1);
        char *name = g_markup_escape_text (_(list->title), -1);

        g_string_append_printf (ui, "<child><object class='GtkShortcutsShortcut'><property name='visible'>1</property>"
                                    "<property name='accelerator'>%s</property><property name='title'>%s</property>"
                                    "</object></child>", accel, name);
        g_free (accel);
        g_free (name);
    }
    g_string_append (ui, "</object></child>");
}

void
liria_sde_ui_show_shortcuts (GtkWindow *parent)
{
    static const SdeShortcut navigation[] = {
        { "<Alt>Left", N_("Go back") }, { "<Alt>Right", N_("Go forward") },
        { "<Alt>Up", N_("Open the parent folder") }, { "<Alt>Home", N_("Open the home folder") },
        { "<Control>L", N_("Enter a location") }, { "<Control>F", N_("Search for files") },
        { "<Control>R", N_("Reload") }, { NULL, NULL }
    };
    static const SdeShortcut windows[] = {
        { "<Control>N", N_("New window") }, { "<Control>T", N_("New tab") },
        { "<Control>Page_Up", N_("Previous tab") }, { "<Control>Page_Down", N_("Next tab") },
        { "<Control>W", N_("Close the tab or window") }, { "<Control>Q", N_("Close all windows") },
        { "F3", N_("Extra pane") }, { "F6", N_("Switch to the other pane") }, { NULL, NULL }
    };
    static const SdeShortcut view[] = {
        { "<Control>1", N_("Icons") }, { "<Control>2", N_("List") }, { "<Control>3", N_("Compact") },
        { "<Control>plus", N_("Zoom in") }, { "<Control>minus", N_("Zoom out") },
        { "<Control>0", N_("Normal size") }, { "<Control>H", N_("Show hidden files") },
        { "F9", N_("Show or hide the sidebar") }, { "F10", N_("Main menu") }, { NULL, NULL }
    };
    static const SdeShortcut files[] = {
        { "<Control>O", N_("Open") }, { "<Control>C", N_("Copy") }, { "<Control>X", N_("Cut") },
        { "<Control>V", N_("Paste") }, { "F2", N_("Rename") }, { "Delete", N_("Move to Trash") },
        { "<Shift>Delete", N_("Delete permanently") }, { "<Control><Shift>N", N_("New folder") },
        { "<Control>M", N_("Make link") }, { "<Alt>Return", N_("Properties") }, { NULL, NULL }
    };
    static const SdeShortcut selection[] = {
        { "<Control>A", N_("Select all") }, { "<Control>S", N_("Select items matching") },
        { "<Control><Shift>I", N_("Invert selection") }, { "<Control>Z", N_("Undo") },
        { "<Control>Y", N_("Redo") }, { "<Control>D", N_("Bookmark this location") },
        { "<Control>B", N_("Edit bookmarks") }, { NULL, NULL }
    };
    GString *groups = g_string_new (NULL);
    GtkBuilder *builder;
    GtkWidget *shortcuts;
    char *ui;
    GError *error = NULL;

    sde_shortcut_group (groups, _("Navigation"), navigation);
    sde_shortcut_group (groups, _("Windows and Tabs"), windows);
    sde_shortcut_group (groups, _("View"), view);
    sde_shortcut_group (groups, _("Files"), files);
    sde_shortcut_group (groups, _("Selection and Bookmarks"), selection);
    ui = g_strdup_printf (sde_shortcuts_ui, groups->str);
    g_string_free (groups, TRUE);

    builder = gtk_builder_new ();
    if (!gtk_builder_add_from_string (builder, ui, -1, &error))
    {
        g_warning ("Liria: %s", error->message);
        g_error_free (error);
        g_object_unref (builder);
        g_free (ui);
        return;
    }
    shortcuts = GTK_WIDGET (gtk_builder_get_object (builder, "window"));
    gtk_window_set_title (GTK_WINDOW (shortcuts), _("Keyboard Shortcuts"));
    if (parent != NULL)
        gtk_window_set_transient_for (GTK_WINDOW (shortcuts), parent);
    gtk_widget_show (shortcuts);
    g_object_unref (builder);
    g_free (ui);
}

/* ------------------------------------------------------------- help */

void
liria_sde_ui_show_help (GtkWindow *parent, const char *section)
{
    char *uri = g_strconcat ("file://" SDE_HELP_FILE, section != NULL ? "#" : "", section, NULL);
    GError *error = NULL;

    if (!g_file_test (SDE_HELP_FILE, G_FILE_TEST_EXISTS) ||
        !gtk_show_uri_on_window (parent, uri, gtk_get_current_event_time (), &error))
    {
        GtkWidget *dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                                    GTK_BUTTONS_CLOSE, "%s", _("Liria Help"));

        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
            _("Help could not be opened. Press Ctrl+? for keyboard shortcuts.\n%s"),
            error != NULL ? error->message : SDE_HELP_FILE);
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
    }
    g_clear_error (&error);
    g_free (uri);
}

/* ------------------------------------------------------------- about */

#define SDE_ABOUT_GROUP "About"

void
liria_sde_ui_show_about (GtkWindow *parent)
{
    const char *developers[] = { "DebION Dev-Team", NULL };
    char **authors = NULL;
    gsize n_authors = 0, i;
    GKeyFile *key_file = g_key_file_new ();
    GtkWidget *about;
    char *license, *comments;

    /* The original authors, kept as the upstream credits require. */
    if (g_key_file_load_from_file (key_file, LIRIA_DATADIR G_DIR_SEPARATOR_S "liria.about", 0, NULL))
        authors = g_key_file_get_string_list (key_file, SDE_ABOUT_GROUP, "Authors", &n_authors, NULL);
    g_key_file_free (key_file);
    for (i = 0; i < n_authors; i++)
        g_strdelimit (authors[i], "%", '@');

    comments = g_strdup_printf ("%s\n\n%s", _("Browse and organize the files and folders on your "
                                              "computer, removable drives and network."),
                                _("Developer: DebION Dev-Team"));
    license = g_strjoin ("\n\n",
        _("Liria is free software; you can redistribute it and/or modify it under the terms of the "
          "GNU General Public License as published by the Free Software Foundation; either version 2 "
          "of the License, or (at your option) any later version."),
        _("Liria is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without "
          "even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the "
          "GNU General Public License for more details."),
        _("You should have received a copy of the GNU General Public License along with Liria; if not, "
          "write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, "
          "MA 02110-1301 USA"),
        /* Legal notice of the original work: not translated, not rebranded. */
        "Liria is a modified version of the Caja file manager 1.28.\n"
        "Copyright \xC2\xA9 1999-2009 The Nautilus authors\n"
        "Copyright \xC2\xA9 2011-2021 The Caja authors\n"
        "The original copyright and license notices are installed in "
        "/usr/share/doc/sde/upstream/caja/.",
        NULL);

    about = gtk_about_dialog_new ();
    g_object_set (about,
                  "program-name", _("Liria"),
                  "title", _("About Liria"),
                  "version", "@SDE_BUILD_VERSION@",
                  "comments", comments,
                  "copyright", "Copyright \xC2\xA9 2026 DebION Dev-Team",
                  "license", license,
                  "wrap-license", TRUE,
                  "authors", developers,
                  "translator-credits", _("translator-credits"),
                  "logo-icon-name", "system-file-manager",
                  "website", SDE_PROJECT_URL,
                  "website-label", _("SDE website"),
                  NULL);
    gtk_about_dialog_add_credit_section (GTK_ABOUT_DIALOG (about), _("Developer"), developers);
    if (authors != NULL && n_authors > 0)
        gtk_about_dialog_add_credit_section (GTK_ABOUT_DIALOG (about), _("Original authors"),
                                             (const char **) authors);
    if (parent != NULL)
    {
        gtk_window_set_transient_for (GTK_WINDOW (about), parent);
        gtk_window_set_modal (GTK_WINDOW (about), TRUE);
    }
    g_signal_connect (about, "response", G_CALLBACK (gtk_widget_destroy), NULL);
    gtk_widget_show (about);

    g_strfreev (authors);
    g_free (license);
    g_free (comments);
}

/* --------------------------------------------------- application level */

/* "sde-copy-to" (as, s): copies files to a folder with Liria's own file
 * operations, so removable drives get the safe copy engine and progress. */
static void
sde_copy_to_activate (GSimpleAction *action, GVariant *parameter, gpointer data)
{
    GVariantIter *iter;
    const char *uri, *target;
    GList *uris = NULL;

    g_variant_get (parameter, "(as&s)", &iter, &target);
    while (g_variant_iter_next (iter, "&s", &uri))
        uris = g_list_prepend (uris, g_strdup (uri));
    g_variant_iter_free (iter);
    uris = g_list_reverse (uris);
    if (uris != NULL && target != NULL && *target != '\0')
        liria_file_operations_copy_move (uris, NULL, target, GDK_ACTION_COPY, NULL, NULL, NULL);
    g_list_free_full (uris, g_free);
}

void
liria_sde_ui_startup (GApplication *application)
{
    GSimpleAction *copy_to = g_simple_action_new ("sde-copy-to", G_VARIANT_TYPE ("(ass)"));
    GtkCssProvider *provider = gtk_css_provider_new ();

    g_signal_connect (copy_to, "activate", G_CALLBACK (sde_copy_to_activate), NULL);
    g_action_map_add_action (G_ACTION_MAP (application), G_ACTION (copy_to));
    g_object_unref (copy_to);

    /* Liria's own look: only standard theme colours, so it matches every
     * SDE theme (light, dark, accent) and other GTK themes. */
    gtk_css_provider_load_from_resource (provider, "/org/debion/liria/liria-sde.css");
    gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                               GTK_STYLE_PROVIDER (provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (provider);
}

G_GNUC_END_IGNORE_DEPRECATIONS
