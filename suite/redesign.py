# SPDX-License-Identifier: GPL-3.0-or-later
"""SDE alpha10 redesign of the native suite forks.

Applied by tools/prepare-native-suite.py after the namespace rename. Every
change is a small, checked text edit or an added file from suite/overlays;
upstream copyright notices, authors and About credits are kept unchanged.
A missing anchor stops preparation instead of silently skipping a change.
"""
import re
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
OVERLAYS = HERE / "overlays"


def inputs():
    """Files whose content changes the prepared sources (for the lock hash)."""
    return [Path(__file__)] + sorted(p for p in OVERLAYS.rglob("*") if p.is_file())


def edit(path, old, new, count=1):
    text = path.read_text()
    if text.count(old) < 1:
        raise ValueError("Redesign anchor not found in %s: %r" % (path, old[:60]))
    path.write_text(text.replace(old, new, count))


def overlay(record_id, staged):
    source = OVERLAYS / record_id
    if not source.is_dir():
        return
    for path in source.rglob("*"):
        if path.is_file():
            target = staged / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def photo(staged):
    """SDE View becomes Photo, with a live Effects side pane plugin."""
    window = staged / "src/sdeview-window.c"
    edit(window, '"comments",_("Photo is a simple graphics viewer for the MATE Desktop Environment."),',
         '"comments",_("Photo shows, browses and lightly edits pictures in SDE.\\n'
         'Based on Eye of MATE by the MATE developers."),')
    plugins = staged / "plugins/Makefile.am"
    edit(plugins, "plugins_LTLIBRARIES = \\\n\tfullscreen/libfullscreen.la  \\\n",
         "plugins_LTLIBRARIES = \\\n\teffects/libeffects.la  \\\n\tfullscreen/libfullscreen.la  \\\n")
    edit(plugins, "plugins_in_files = \\\n\tfullscreen/fullscreen.plugin.desktop.in  \\\n",
         "plugins_in_files = \\\n\teffects/effects.plugin.desktop.in  \\\n\tfullscreen/fullscreen.plugin.desktop.in  \\\n")
    edit(plugins, "# Fullscreen plugin\n",
         "# Effects plugin (SDE)\n"
         "effects_libeffects_la_SOURCES = \\\n"
         "\teffects/sdeview-effects-plugin.h  \\\n"
         "\teffects/sdeview-effects-plugin.c\n\n"
         "effects_libeffects_la_LIBADD = $(SDEVIEW_LIBS) -lm\n\n"
         "# Fullscreen plugin\n")
    edit(staged / "configure.ac", "plugins/fullscreen/fullscreen.plugin.desktop.in\n",
         "plugins/effects/effects.plugin.desktop.in\nplugins/fullscreen/fullscreen.plugin.desktop.in\n")


def liria(staged):
    """Compact icon-only Back/Forward buttons (tooltips keep the names), so
    the command bar reads as one glass strip instead of Caja's labels; and
    the built-in SDE Tools extension (suite/overlays/liria/sde-tools)."""
    edit(staged / "Makefile.am", "\tsrc \\\n", "\tsrc \\\n\tsde-tools \\\n")
    edit(staged / "configure.ac", "src/file-manager/Makefile\n", "src/file-manager/Makefile\nsde-tools/Makefile\n")
    menus = staged / "src/liria-navigation-window-menus.c"
    for direction in ("BACK", "FORWARD"):
        edit(menus, '"direction", LIRIA_NAVIGATION_DIRECTION_%s,\n                           "is_important", TRUE,' % direction,
             '"direction", LIRIA_NAVIGATION_DIRECTION_%s,\n                           "is_important", FALSE,' % direction)
    liria_command_bar(staged)
    liria_home(staged)
    liria_sidebar(staged)


def liria_command_bar(staged):
    """alpha14: one command row. Back, Forward, Up and Reload, then the
    breadcrumb path (with its search field) stretched across the window,
    then View As, Search, extension buttons and a single main-menu button
    that holds the classic File/Edit/View/Go/Bookmarks/Help menus. Every
    action, shortcut and menu stays available; only the layout changes."""
    edit(staged / "src/liria-navigation-window-ui.xml",
         '\t<toolitem name="Up" action="Up"/>\n'
         '\t<toolitem name="Stop" action="Stop"/>\n'
         '\t<toolitem name="Reload" action="Reload"/>\n'
         '\t<separator/>\n'
         '\t<toolitem name="Home" action="Home"/>\n'
         '\t<toolitem name="Computer" action="Go to Computer"/>\n'
         '\t<separator/>\n'
         '\t<toolitem name="Zoom" action="Zoom"/>\n'
         '\t<toolitem name="ViewAs" action="ViewAs"/>\n',
         '\t<toolitem name="Up" action="Up"/>\n'
         '\t<toolitem name="Reload" action="Reload"/>\n'
         '\t<toolitem name="ViewAs" action="ViewAs"/>\n')
    window = staged / "src/liria-navigation-window.c"
    edit(window, "static void\nliria_navigation_window_init (LiriaNavigationWindow *window)\n{",
         """/* SDE: opens a location in this window's active tab. The SDE Home
 * overview (an extension) finds it as the "sde-go-to-uri" object data. */
static void
sde_go_to_uri (GtkWidget *widget, const char *uri)
{
    GFile *location = g_file_new_for_uri (uri);

    liria_window_go_to (LIRIA_WINDOW (widget), location);
    g_object_unref (location);
}

/* SDE: the first pane's path bar row moves into the command bar, right
 * after Reload; it is destroyed together with its pane. */
static void
sde_merge_location_row (GtkWidget *toolbar, GtkUIManager *ui_manager, LiriaNavigationWindowPane *pane)
{
    GtkWidget *reload = gtk_ui_manager_get_widget (ui_manager, "/Toolbar/Reload");
    GtkToolItem *item = gtk_tool_item_new ();
    GtkWidget *row = pane->location_bar;
    gint index = -1;

    if (reload != NULL && gtk_widget_get_parent (reload) == toolbar)
        index = gtk_toolbar_get_item_index (GTK_TOOLBAR (toolbar), GTK_TOOL_ITEM (reload)) + 1;
    g_object_ref (row);
    gtk_container_remove (GTK_CONTAINER (gtk_widget_get_parent (row)), row);
    gtk_container_add (GTK_CONTAINER (item), row);
    g_object_unref (row);
    gtk_container_set_border_width (GTK_CONTAINER (row), 0);
    gtk_widget_set_margin_start (row, 8);
    gtk_widget_set_margin_end (row, 8);
    gtk_tool_item_set_expand (item, TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (item)), "sde-location-item");
    gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, index);
    gtk_widget_show (GTK_WIDGET (item));
    g_signal_connect_object (pane->widget, "destroy", G_CALLBACK (gtk_widget_destroy), item, G_CONNECT_SWAPPED);
}

/* SDE: the menu bar's menus move into one main-menu button at the end of
 * the command bar. F10 opens it, like the menu bar before. */
static void
sde_merge_menu_bar (GtkWidget *toolbar, GtkWidget *menubar)
{
    GtkToolItem *item = gtk_tool_item_new ();
    GtkWidget *bar = gtk_menu_bar_new ();
    GtkWidget *button = gtk_menu_item_new ();
    GtkWidget *menu = gtk_menu_new ();
    GtkWidget *image;
    GList *children, *l;
    const char *icon = "open-menu-symbolic";

    if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), icon))
        icon = "open-menu";
    if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), icon))
        icon = "emblem-system";
    image = gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_container_add (GTK_CONTAINER (button), image);
    gtk_widget_set_tooltip_text (button, _("Menu"));
    children = gtk_container_get_children (GTK_CONTAINER (menubar));
    for (l = children; l != NULL; l = l->next)
    {
        GtkWidget *child = l->data;

        g_object_ref (child);
        gtk_container_remove (GTK_CONTAINER (menubar), child);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), child);
        g_object_unref (child);
    }
    g_list_free (children);
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (button), menu);
    gtk_menu_shell_append (GTK_MENU_SHELL (bar), button);
    gtk_style_context_add_class (gtk_widget_get_style_context (bar), "sde-main-menu");
    gtk_container_add (GTK_CONTAINER (item), bar);
    gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);
    gtk_widget_show_all (GTK_WIDGET (item));
    gtk_widget_hide (menubar);
    gtk_widget_set_no_show_all (menubar, TRUE);
}

static void
liria_navigation_window_init (LiriaNavigationWindow *window)
{""")
    edit(window, """    gtk_widget_show (toolbar);

    liria_navigation_window_initialize_toolbars (window);
""", """    gtk_widget_show (toolbar);

    /* SDE: one command row with icon buttons, the path and the menu. */
    gtk_toolbar_set_style (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_show_arrow (GTK_TOOLBAR (toolbar), FALSE);
    sde_merge_location_row (toolbar, ui_manager, pane);
    sde_merge_menu_bar (toolbar, win->details->menubar);
    g_object_set_data (G_OBJECT (window), "sde-go-to-uri", sde_go_to_uri);

    liria_navigation_window_initialize_toolbars (window);
""")


def liria_home(staged):
    """The view's extension strip (where the SDE Home overview appears)
    becomes a flat, themable area instead of an etched frame."""
    edit(staged / "src/liria-window-slot.c",
         "    frame = gtk_frame_new (NULL);\n    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_ETCHED_IN);\n",
         "    frame = gtk_frame_new (NULL);\n    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_NONE);\n"
         "    gtk_style_context_add_class (gtk_widget_get_style_context (frame), \"liria-extra-location\");\n")


def liria_sidebar(staged):
    """Sidebar sections read Favorites (Home and the personal folders),
    Drives (the file system and devices), Bookmarks and Network."""
    sidebar = staged / "src/liria-places-sidebar.c"
    edit(sidebar, '    last_iter = add_heading (sidebar, SECTION_COMPUTER,\n                             _("Computer"));',
         '    last_iter = add_heading (sidebar, SECTION_COMPUTER,\n                             _("Favorites"));')
    edit(sidebar, '            add_heading (sidebar, SECTION_DEVICES,\n                     _("Devices"));',
         '            add_heading (sidebar, SECTION_DEVICES,\n                     _("Drives"));')
    edit(sidebar, "    display_name = g_filename_display_basename (g_get_home_dir ());\n",
         "    display_name = g_strdup (_(\"Home\"));\n")
    root = """    /* file system root */
    mount_uri = "file:///"; /* No need to strdup */
    icon = g_themed_icon_new (LIRIA_ICON_FILESYSTEM);
    last_iter = add_place (sidebar, PLACES_BUILT_IN,
                           SECTION_COMPUTER,
                           _("File System"), icon,
                           mount_uri, NULL, NULL, NULL, 0,
                           _("Open the contents of the File System"));
    g_object_unref (icon);
    compare_for_selection (sidebar,
                           location, mount_uri, last_uri,
                           &last_iter, &select_path);
"""
    edit(sidebar, root, "")
    edit(sidebar, "    /* first go through all connected drives */\n",
         root.replace("SECTION_COMPUTER", "SECTION_DEVICES") + "\n    /* first go through all connected drives */\n")


RECIPES = {"sde-view": photo, "liria": liria}


def apply(record, staged):
    overlay(record["id"], staged)
    recipe = RECIPES.get(record["id"])
    if recipe:
        recipe(staged)
