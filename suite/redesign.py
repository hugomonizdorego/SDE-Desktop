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


RECIPES = {"sde-view": photo, "sde-system-monitor": system_monitor, "liria": liria}


def apply(record, staged):
    overlay(record["id"], staged)
    recipe = RECIPES.get(record["id"])
    if recipe:
        recipe(staged)
