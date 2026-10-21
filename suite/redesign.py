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


def system_monitor(staged):
    """Card-style graphs: theme-aware background, soft dashed grid, thicker
    curves with a translucent fill instead of thin lines on white; and a CSS
    class on the window so SDE themes can style it."""
    edit(staged / "src/interface.cpp",
         'app = GTK_WIDGET (gtk_builder_get_object (builder, "main_window"));',
         'app = GTK_WIDGET (gtk_builder_get_object (builder, "main_window"));\n'
         '    gtk_style_context_add_class (gtk_widget_get_style_context (app), "sde-system-monitor");')
    graph = staged / "src/load-graph.cpp"
    edit(graph, """    /* Draw background rectangle */
    cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
    cairo_rectangle (cr, graph->rmargin + graph->indent, 0,
                     graph->draw_width - graph->rmargin - graph->indent, graph->real_draw_height);
    cairo_fill(cr);
""", """    /* Draw background rectangle: a rounded card tinted from the theme. */
    {
        double bx = graph->rmargin + graph->indent, bw = graph->draw_width - graph->rmargin - graph->indent;
        double bh = graph->real_draw_height, r = 8.0;
        cairo_new_sub_path (cr);
        cairo_arc (cr, bx + bw - r, r, r, -G_PI / 2, 0);
        cairo_arc (cr, bx + bw - r, bh - r, r, 0, G_PI / 2);
        cairo_arc (cr, bx + r, bh - r, r, G_PI / 2, G_PI);
        cairo_arc (cr, bx + r, r, r, G_PI, 3 * G_PI / 2);
        cairo_close_path (cr);
        cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.045);
        cairo_fill (cr);
    }
""")
    text = graph.read_text()
    old_major = "cairo_set_source_rgba (cr, 0.70, 0.71, 0.70, 1.0);"
    old_minor = "cairo_set_source_rgba (cr, 0.89, 0.89, 0.89, 1.0);"
    if text.count(old_major) < 2 or text.count(old_minor) < 2:
        raise ValueError("Redesign anchor not found in load-graph.cpp grid colors")
    text = text.replace(old_major, "cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.20);")
    text = text.replace(old_minor, "cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.09);")
    graph.write_text(text)
    edit(graph, """    cairo_set_line_width (cr, 1);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);""", """    cairo_set_line_width (cr, 2);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);""")
    edit(graph, """    for (j = 0; j < graph->n; ++j) {
        cairo_move_to (cr, x_offset, (1.0f - graph->data[0][j]) * graph->real_draw_height);
        gdk_cairo_set_source_rgba (cr, &(graph->colors [j]));

        for (i = 1; i < LoadGraph::NUM_POINTS; ++i) {
            if (graph->data[i][j] == -1.0f)
                continue;
            cairo_curve_to (cr,
                       x_offset - ((i - 0.5f) * graph->graph_delx),
                       (1.0f - graph->data[i-1][j]) * graph->real_draw_height + 3.5f,
                       x_offset - ((i - 0.5f) * graph->graph_delx),
                       (1.0f - graph->data[i][j]) * graph->real_draw_height + 3.5f,
                       x_offset - (i * graph->graph_delx),
                       (1.0f - graph->data[i][j]) * graph->real_draw_height + 3.5f);
        }
        cairo_stroke (cr);

    }""", """    for (j = 0; j < graph->n; ++j) {
        double last_x = x_offset, base = graph->real_draw_height + 3.5f;
        cairo_move_to (cr, x_offset, (1.0f - graph->data[0][j]) * graph->real_draw_height);
        gdk_cairo_set_source_rgba (cr, &(graph->colors [j]));

        for (i = 1; i < LoadGraph::NUM_POINTS; ++i) {
            if (graph->data[i][j] == -1.0f)
                continue;
            cairo_curve_to (cr,
                       x_offset - ((i - 0.5f) * graph->graph_delx),
                       (1.0f - graph->data[i-1][j]) * graph->real_draw_height + 3.5f,
                       x_offset - ((i - 0.5f) * graph->graph_delx),
                       (1.0f - graph->data[i][j]) * graph->real_draw_height + 3.5f,
                       x_offset - (i * graph->graph_delx),
                       (1.0f - graph->data[i][j]) * graph->real_draw_height + 3.5f);
            last_x = x_offset - (i * graph->graph_delx);
        }
        cairo_stroke_preserve (cr);
        /* SDE: soft area under each curve. */
        cairo_line_to (cr, last_x, base);
        cairo_line_to (cr, x_offset, base);
        cairo_close_path (cr);
        cairo_set_source_rgba (cr, graph->colors[j].red, graph->colors[j].green, graph->colors[j].blue,
                               graph->n > 4 ? 0.05 : 0.14);
        cairo_fill (cr);
    }""")


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


RECIPES = {"sde-view": photo, "sde-system-monitor": system_monitor, "liria": liria}


def apply(record, staged):
    overlay(record["id"], staged)
    recipe = RECIPES.get(record["id"])
    if recipe:
        recipe(staged)
