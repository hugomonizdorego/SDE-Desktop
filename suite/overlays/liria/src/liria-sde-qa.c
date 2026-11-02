/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * QA (0.2.0 Alpha 3): LIRIA_SDE_QA_FRAMES=FILE records how Liria answers
 * the pointer, for the hover scenarios of tools/liria-perf.py. Off unless
 * the variable is set. FILE gets one line per frame of every folder window
 *
 *   frame <begin> <update and layout> <paint> <painted pixels>
 *
 * (the frame clock's begin time, the time spent in style updates and
 * layout and in painting, in microseconds, and the area the window
 * repainted, the clip of its "draw") and one per pointer motion,
 * enter or leave event (event <begin> <GdkEventType> <dispatch time>).
 * SIGUSR2 writes FILE.geometry: the parts of every window that the
 * scenarios move over (sidebar, view, ribbon buttons and tabs,
 * breadcrumbs, tabs, Home cards, status bar, header buttons) as
 * "kind x y width height label" in screen coordinates, clipped to the
 * scrolled area they are in, then "end". (SIGUSR1 is Liria's debug log.)
 *
 * Menus (0.2.0 Alpha 3.1, tools/liria-menu-smoke.py) add
 *
 *   input <begin> <GdkEventType> <dispatch time> <X time> <button or key>
 *   menu-map <time> <menu> <items> <width> <height> <first label>
 *   menu-frame <begin> <update and layout> <paint> <painted pixels> <menu>
 *   menu-select <time> <menu> <X time> <label>
 *
 * a button press or release or a key press (the X server's time of the
 * event is in milliseconds of the same monotonic clock as the other
 * times, which are in microseconds), every menu that is mapped (a number
 * for each menu, its visible items, its size), every frame of a menu
 * window and every pointer or key event that moved the highlight of a
 * menu to another item.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <glib-unix.h>
#include <gtk/gtk.h>

#include <libliria-private/liria-file.h>
#include <libliria-private/liria-icon-private.h>

#include "file-manager/fm-list-model.h"
#include "liria-sde-qa.h"

typedef struct
{
    gint64 begin;
    gint64 layout;
    gint64 area;
    int menu; /* the menu shown in this window, 0 for folder windows */
} SdeQaFrame;

static FILE *sde_qa_file = NULL;
static char *sde_qa_geometry = NULL;
static GSList *sde_qa_windows = NULL;
static int sde_qa_menus = 0;

static void
sde_qa_before_paint (GdkFrameClock *clock, SdeQaFrame *frame)
{
    frame->begin = g_get_monotonic_time ();
    frame->layout = 0;
    frame->area = 0;
}

static void
sde_qa_layout (GdkFrameClock *clock, SdeQaFrame *frame)
{
    frame->layout = g_get_monotonic_time ();
}

static void
sde_qa_after_paint (GdkFrameClock *clock, SdeQaFrame *frame)
{
    gint64 end = g_get_monotonic_time ();
    gint64 layout = frame->layout != 0 ? frame->layout : frame->begin;

    if (frame->begin == 0)
        return;
    if (frame->menu != 0)
        fprintf (sde_qa_file, "menu-frame\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT
                 "\t%" G_GINT64_FORMAT "\t%d\n", frame->begin, layout - frame->begin, end - layout, frame->area,
                 frame->menu);
    else
        fprintf (sde_qa_file, "frame\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT
                 "\t%" G_GINT64_FORMAT "\n", frame->begin, layout - frame->begin, end - layout, frame->area);
    frame->begin = 0;
}

static gboolean
sde_qa_draw (GtkWidget *window, cairo_t *cr, SdeQaFrame *frame)
{
    cairo_rectangle_list_t *list = cairo_copy_clip_rectangle_list (cr);
    int i;

    if (list->status == CAIRO_STATUS_SUCCESS)
        for (i = 0; i < list->num_rectangles; i++)
            frame->area += (gint64) (list->rectangles[i].width * list->rectangles[i].height);
    cairo_rectangle_list_destroy (list);
    return FALSE;
}

/* The menu that an event goes to: the menu, one of its items (each has an
 * input window of its own) or the popup window that holds the menu. */
static GtkMenuShell *
sde_qa_event_menu (GdkEvent *event)
{
    GtkWidget *widget = gtk_get_event_widget (event);

    if (widget != NULL && GTK_IS_WINDOW (widget) && GTK_IS_MENU (gtk_bin_get_child (GTK_BIN (widget))))
        widget = gtk_bin_get_child (GTK_BIN (widget));
    widget = widget != NULL ? gtk_widget_get_ancestor (widget, GTK_TYPE_MENU) : NULL;
    return widget != NULL ? GTK_MENU_SHELL (widget) : NULL;
}

static void
sde_qa_event (GdkEvent *event, gpointer data)
{
    GtkMenuShell *menu;
    GtkWidget *selected = NULL, *now;
    gint64 begin, end;

    switch (event->type)
    {
    case GDK_MOTION_NOTIFY:
    case GDK_ENTER_NOTIFY:
    case GDK_LEAVE_NOTIFY:
    case GDK_BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
    case GDK_KEY_PRESS:
        break;
    default:
        gtk_main_do_event (event);
        return;
    }
    menu = sde_qa_event_menu (event);
    if (menu != NULL)
    {
        g_object_ref (menu);
        selected = gtk_menu_shell_get_selected_item (menu);
    }
    begin = g_get_monotonic_time ();
    gtk_main_do_event (event);
    end = g_get_monotonic_time ();
    if (event->type == GDK_BUTTON_PRESS || event->type == GDK_BUTTON_RELEASE || event->type == GDK_KEY_PRESS)
        fprintf (sde_qa_file, "input\t%" G_GINT64_FORMAT "\t%d\t%" G_GINT64_FORMAT "\t%u\t%u\n", begin,
                 (int) event->type, end - begin, gdk_event_get_time (event),
                 event->type == GDK_KEY_PRESS ? event->key.keyval : event->button.button);
    else
        fprintf (sde_qa_file, "event\t%" G_GINT64_FORMAT "\t%d\t%" G_GINT64_FORMAT "\n",
                 begin, (int) event->type, end - begin);
    if (menu == NULL)
        return;
    now = gtk_menu_shell_get_selected_item (menu);
    if (now != NULL && now != selected)
    {
        const char *label = GTK_IS_MENU_ITEM (now) ? gtk_menu_item_get_label (GTK_MENU_ITEM (now)) : NULL;

        fprintf (sde_qa_file, "menu-select\t%" G_GINT64_FORMAT "\t%d\t%u\t%s\n", end,
                 GPOINTER_TO_INT (g_object_get_data (G_OBJECT (menu), "sde-qa-menu")),
                 gdk_event_get_time (event), label != NULL && *label != '\0' ? label : "-");
    }
    g_object_unref (menu);
}

/* Every menu that is mapped: its frames are recorded from now on. */
static gboolean
sde_qa_menu_mapped (GSignalInvocationHint *hint, guint n_values, const GValue *values, gpointer data)
{
    GtkWidget *menu = g_value_get_object (&values[0]);
    GtkWidget *toplevel;
    GdkWindow *window;
    GdkFrameClock *clock;
    GList *children, *l;
    const char *first = NULL;
    int id, items = 0;

    if (!GTK_IS_MENU (menu))
        return TRUE;
    id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (menu), "sde-qa-menu"));
    if (id == 0)
    {
        id = ++sde_qa_menus;
        g_object_set_data (G_OBJECT (menu), "sde-qa-menu", GINT_TO_POINTER (id));
    }
    toplevel = gtk_widget_get_toplevel (menu);
    window = toplevel != NULL ? gtk_widget_get_window (toplevel) : NULL;
    clock = window != NULL ? gdk_window_get_frame_clock (window) : NULL;
    if (clock != NULL && g_object_get_data (G_OBJECT (clock), "sde-qa-frame") == NULL)
    {
        SdeQaFrame *frame = g_new0 (SdeQaFrame, 1);

        g_object_set_data_full (G_OBJECT (clock), "sde-qa-frame", frame, g_free);
        g_signal_connect (clock, "before-paint", G_CALLBACK (sde_qa_before_paint), frame);
        g_signal_connect_after (clock, "layout", G_CALLBACK (sde_qa_layout), frame);
        g_signal_connect_after (clock, "after-paint", G_CALLBACK (sde_qa_after_paint), frame);
        g_signal_connect (toplevel, "draw", G_CALLBACK (sde_qa_draw), frame);
    }
    if (clock != NULL)
        ((SdeQaFrame *) g_object_get_data (G_OBJECT (clock), "sde-qa-frame"))->menu = id;
    children = gtk_container_get_children (GTK_CONTAINER (menu));
    for (l = children; l != NULL; l = l->next)
    {
        if (!gtk_widget_get_visible (l->data) || GTK_IS_SEPARATOR_MENU_ITEM (l->data))
            continue;
        items++;
        if (first == NULL && GTK_IS_MENU_ITEM (l->data))
            first = gtk_menu_item_get_label (GTK_MENU_ITEM (l->data));
    }
    g_list_free (children);
    fprintf (sde_qa_file, "menu-map\t%" G_GINT64_FORMAT "\t%d\t%d\t%d\t%d\t%s\n", g_get_monotonic_time (), id, items,
             gtk_widget_get_allocated_width (menu), gtk_widget_get_allocated_height (menu),
             first != NULL && *first != '\0' ? first : "-");
    return TRUE;
}

/* ------------------------------------------------------------ geometry */

static gboolean
sde_qa_has_class (GtkWidget *widget, const char *name)
{
    return gtk_style_context_has_class (gtk_widget_get_style_context (widget), name);
}

static gboolean
sde_qa_inside (GtkWidget *widget, const char *type_name)
{
    GType type = g_type_from_name (type_name);

    for (; widget != NULL; widget = gtk_widget_get_parent (widget))
        if (type != 0 && g_type_is_a (G_OBJECT_TYPE (widget), type))
            return TRUE;
    return FALSE;
}

static const char *
sde_qa_kind (GtkWidget *widget)
{
    GType icons = g_type_from_name ("LiriaIconContainer");

    if (icons != 0 && g_type_is_a (G_OBJECT_TYPE (widget), icons))
        return "icon-view";
    if (GTK_IS_TREE_VIEW (widget))
        return sde_qa_inside (widget, "LiriaPlacesSidebar") ? "sidebar"
               : sde_qa_inside (widget, "FMListView") ? "list-view" : NULL;
    if (GTK_IS_STATUSBAR (widget))
        return "status";
    if (!GTK_IS_BUTTON (widget))
        return NULL;
    if (sde_qa_has_class (widget, "sde-ribbon-button"))
        return "ribbon-button";
    if (sde_qa_has_class (widget, "sde-ribbon-tab"))
        return "ribbon-tab";
    if (sde_qa_has_class (widget, "sde-card"))
        return "card";
    if (sde_qa_inside (widget, "LiriaPathBar"))
        return "crumb";
    if (sde_qa_inside (widget, "GtkToolbar") && !sde_qa_inside (widget, "LiriaLocationBar"))
        return "header-button";
    return NULL;
}

/* The widget's rectangle on the screen, clipped to the scrolled windows
 * around it; FALSE when nothing of it is visible. */
static gboolean
sde_qa_rectangle (GtkWidget *widget, GdkRectangle *out)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel (widget);
    GtkWidget *p;
    int ox = 0, oy = 0, x = 0, y = 0;

    if (!gtk_widget_translate_coordinates (widget, toplevel, 0, 0, &x, &y))
        return FALSE;
    gdk_window_get_origin (gtk_widget_get_window (toplevel), &ox, &oy);
    out->x = x + ox;
    out->y = y + oy;
    out->width = gtk_widget_get_allocated_width (widget);
    out->height = gtk_widget_get_allocated_height (widget);
    for (p = gtk_widget_get_parent (widget); p != NULL; p = gtk_widget_get_parent (p))
    {
        GdkRectangle clip;

        if (!GTK_IS_SCROLLED_WINDOW (p) ||
            !gtk_widget_translate_coordinates (p, toplevel, 0, 0, &x, &y))
            continue;
        clip.x = x + ox;
        clip.y = y + oy;
        clip.width = gtk_widget_get_allocated_width (p);
        clip.height = gtk_widget_get_allocated_height (p);
        if (!gdk_rectangle_intersect (out, &clip, out))
            return FALSE;
    }
    return out->width > 0 && out->height > 0;
}

static void
sde_qa_write (GString *text, const char *kind, GtkWidget *widget, const char *label)
{
    GdkRectangle r;
    char *clean;

    if (!sde_qa_rectangle (widget, &r))
        return;
    clean = g_strdup (label != NULL && *label != '\0' ? label : "-");
    g_strdelimit (clean, "\t\n\r", ' ');
    g_string_append_printf (text, "%s\t%d\t%d\t%d\t%d\t%s\n", kind, r.x, r.y, r.width, r.height, clean);
    g_free (clean);
}

/* "file x y width height name": a visible file of a folder view (the
 * picture of an icon, the name cell of a list row), clipped to the view. */
static void
sde_qa_write_file (GString *text, const GdkRectangle *view, int x, int y, int width, int height, LiriaFile *file)
{
    GdkRectangle r = { x, y, width, height };
    char *name;

    if (file == NULL || !gdk_rectangle_intersect (&r, view, &r))
        return;
    name = liria_file_get_display_name (file);
    g_strdelimit (name, "\t\n\r", ' ');
    g_string_append_printf (text, "file\t%d\t%d\t%d\t%d\t%s\n", r.x, r.y, r.width, r.height, name);
    g_free (name);
}

static void
sde_qa_write_files (GtkWidget *widget, const char *kind, GString *text)
{
    GdkRectangle view;
    int ox = 0, oy = 0;

    if (!sde_qa_rectangle (widget, &view))
        return;
    if (g_strcmp0 (kind, "icon-view") == 0)
    {
        GList *l;

        gdk_window_get_origin (gtk_layout_get_bin_window (GTK_LAYOUT (widget)), &ox, &oy);
        for (l = LIRIA_ICON_CONTAINER (widget)->details->icons; l != NULL; l = l->next)
        {
            LiriaIcon *icon = l->data;
            EelDRect world;
            int x0, y0, x1, y1;

            if (icon->item == NULL || !LIRIA_IS_FILE (icon->data))
                continue;
            world = liria_icon_canvas_item_get_icon_rectangle (icon->item);
            eel_canvas_w2c (EEL_CANVAS (widget), world.x0, world.y0, &x0, &y0);
            eel_canvas_w2c (EEL_CANVAS (widget), world.x1, world.y1, &x1, &y1);
            sde_qa_write_file (text, &view, ox + x0, oy + y0, x1 - x0, y1 - y0, LIRIA_FILE (icon->data));
        }
    }
    else if (g_strcmp0 (kind, "list-view") == 0)
    {
        GtkTreeView *tree = GTK_TREE_VIEW (widget);
        GtkTreeModel *model = gtk_tree_view_get_model (tree);
        GtkTreeViewColumn *column = gtk_tree_view_get_column (tree, 0);
        GtkTreePath *path = NULL, *last = NULL;

        if (model == NULL || column == NULL || !gtk_tree_view_get_visible_range (tree, &path, &last))
            return;
        gdk_window_get_origin (gtk_tree_view_get_bin_window (tree), &ox, &oy);
        while (gtk_tree_path_compare (path, last) <= 0)
        {
            GtkTreeIter iter;
            GdkRectangle cell;
            LiriaFile *file = NULL;

            if (!gtk_tree_model_get_iter (model, &iter, path))
                break;
            gtk_tree_model_get (model, &iter, FM_LIST_MODEL_FILE_COLUMN, &file, -1);
            gtk_tree_view_get_cell_area (tree, path, column, &cell);
            sde_qa_write_file (text, &view, ox + cell.x, oy + cell.y, cell.width, cell.height, file);
            if (file != NULL)
                liria_file_unref (file);
            gtk_tree_path_next (path);
        }
        gtk_tree_path_free (path);
        gtk_tree_path_free (last);
    }
    else if (g_strcmp0 (kind, "sidebar") == 0)
    {
        /* "place x y width height name": the rows of the places sidebar
         * (its model keeps the name in column 5, PLACES_SIDEBAR_COLUMN_NAME). */
        GtkTreeView *tree = GTK_TREE_VIEW (widget);
        GtkTreeModel *model = gtk_tree_view_get_model (tree);
        GtkTreeViewColumn *column = gtk_tree_view_get_column (tree, 0);
        GtkTreePath *path = NULL, *last = NULL;

        if (model == NULL || column == NULL || gtk_tree_model_get_n_columns (model) <= 5 ||
            gtk_tree_model_get_column_type (model, 5) != G_TYPE_STRING ||
            !gtk_tree_view_get_visible_range (tree, &path, &last))
            return;
        gdk_window_get_origin (gtk_tree_view_get_bin_window (tree), &ox, &oy);
        while (gtk_tree_path_compare (path, last) <= 0)
        {
            GtkTreeIter iter;
            GdkRectangle cell, r;
            char *name = NULL;

            if (!gtk_tree_model_get_iter (model, &iter, path))
                break;
            gtk_tree_model_get (model, &iter, 5, &name, -1);
            gtk_tree_view_get_background_area (tree, path, column, &cell);
            r.x = ox + cell.x;
            r.y = oy + cell.y;
            r.width = cell.width;
            r.height = cell.height;
            if (name != NULL && gdk_rectangle_intersect (&r, &view, &r))
            {
                g_strdelimit (name, "\t\n\r", ' ');
                g_string_append_printf (text, "place\t%d\t%d\t%d\t%d\t%s\n", r.x, r.y, r.width, r.height, name);
            }
            g_free (name);
            gtk_tree_path_next (path);
        }
        gtk_tree_path_free (path);
        gtk_tree_path_free (last);
    }
}

static void
sde_qa_walk (GtkWidget *widget, GString *text)
{
    const char *kind;

    if (!gtk_widget_get_mapped (widget))
        return;
    kind = sde_qa_kind (widget);
    if (kind != NULL)
    {
        char *tip = gtk_widget_get_tooltip_text (widget);

        sde_qa_write (text, kind, widget, tip);
        g_free (tip);
        sde_qa_write_files (widget, kind, text);
    }
    if (GTK_IS_NOTEBOOK (widget))
    {
        GtkNotebook *notebook = GTK_NOTEBOOK (widget);
        int i;

        for (i = 0; i < gtk_notebook_get_n_pages (notebook); i++)
        {
            GtkWidget *tab = gtk_notebook_get_tab_label (notebook, gtk_notebook_get_nth_page (notebook, i));

            if (tab != NULL && gtk_widget_get_mapped (tab))
                sde_qa_write (text, "tab", tab, NULL);
        }
    }
    if (GTK_IS_CONTAINER (widget))
        gtk_container_forall (GTK_CONTAINER (widget), (GtkCallback) sde_qa_walk, text);
}

/* The items of the open menus: "menu-item x y width height label menu". */
static void
sde_qa_walk_menus (GString *text)
{
    GList *toplevels = gtk_window_list_toplevels (), *l;

    for (l = toplevels; l != NULL; l = l->next)
    {
        GtkWidget *menu = gtk_bin_get_child (GTK_BIN (l->data));
        GList *items, *i;

        if (menu == NULL || !GTK_IS_MENU (menu) || !gtk_widget_get_mapped (menu))
            continue;
        items = gtk_container_get_children (GTK_CONTAINER (menu));
        for (i = items; i != NULL; i = i->next)
        {
            const char *label;
            GdkRectangle r;
            char *clean;

            if (!GTK_IS_MENU_ITEM (i->data) || GTK_IS_SEPARATOR_MENU_ITEM (i->data) ||
                !gtk_widget_get_mapped (i->data) || !sde_qa_rectangle (i->data, &r))
                continue;
            label = gtk_menu_item_get_label (i->data);
            clean = g_strdup (label != NULL && *label != '\0' ? label : "-");
            g_strdelimit (clean, "\t\n\r", ' ');
            g_string_append_printf (text, "menu-item\t%d\t%d\t%d\t%d\t%s\t%d\n", r.x, r.y, r.width, r.height, clean,
                                    GPOINTER_TO_INT (g_object_get_data (G_OBJECT (menu), "sde-qa-menu")));
            g_free (clean);
        }
        g_list_free (items);
    }
    g_list_free (toplevels);
}

static gboolean
sde_qa_dump (gpointer data)
{
    GString *text = g_string_new (NULL);
    GSList *l;

    for (l = sde_qa_windows; l != NULL; l = l->next)
    {
        GtkWidget *window = l->data;
        GdkRectangle r;

        if (!gtk_widget_get_mapped (window))
            continue;
        if (sde_qa_rectangle (window, &r))
            g_string_append_printf (text, "window\t%d\t%d\t%d\t%d\t%s\n", r.x, r.y, r.width, r.height,
                                    gtk_window_get_title (GTK_WINDOW (window)));
        sde_qa_walk (window, text);
    }
    sde_qa_walk_menus (text);
    g_string_append (text, "end\n");
    g_file_set_contents (sde_qa_geometry, text->str, -1, NULL);
    g_string_free (text, TRUE);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------- setup */

static void
sde_qa_realized (GtkWidget *window, gpointer data)
{
    GdkFrameClock *clock = gdk_window_get_frame_clock (gtk_widget_get_window (window));
    SdeQaFrame *frame;

    if (clock == NULL || g_object_get_data (G_OBJECT (clock), "sde-qa-frame") != NULL)
        return;
    frame = g_new0 (SdeQaFrame, 1);
    g_object_set_data_full (G_OBJECT (clock), "sde-qa-frame", frame, g_free);
    g_signal_connect (clock, "before-paint", G_CALLBACK (sde_qa_before_paint), frame);
    g_signal_connect_after (clock, "layout", G_CALLBACK (sde_qa_layout), frame);
    g_signal_connect_after (clock, "after-paint", G_CALLBACK (sde_qa_after_paint), frame);
    g_signal_connect (window, "draw", G_CALLBACK (sde_qa_draw), frame);
}

static void
sde_qa_destroyed (GtkWidget *window, gpointer data)
{
    sde_qa_windows = g_slist_remove (sde_qa_windows, window);
}

void
liria_sde_qa_startup (void)
{
    const char *path = g_getenv ("LIRIA_SDE_QA_FRAMES");

    if (path == NULL || *path == '\0' || sde_qa_file != NULL)
        return;
    sde_qa_file = fopen (path, "a");
    if (sde_qa_file == NULL)
        return;
    setvbuf (sde_qa_file, NULL, _IOLBF, 0);
    sde_qa_geometry = g_strconcat (path, ".geometry", NULL);
    gdk_event_handler_set (sde_qa_event, NULL, NULL);
    g_unix_signal_add (SIGUSR2, sde_qa_dump, NULL);
    /* Menus: their maps and frames. The signal exists once the widget
     * class is initialized; the reference keeps it for the hook. */
    g_type_class_ref (GTK_TYPE_WIDGET);
    g_signal_add_emission_hook (g_signal_lookup ("map", GTK_TYPE_WIDGET), 0, sde_qa_menu_mapped, NULL, NULL);
}

void
liria_sde_qa_window (GtkWidget *window)
{
    if (sde_qa_file == NULL)
        return;
    sde_qa_windows = g_slist_prepend (sde_qa_windows, window);
    g_signal_connect (window, "destroy", G_CALLBACK (sde_qa_destroyed), NULL);
    g_signal_connect (window, "realize", G_CALLBACK (sde_qa_realized), NULL);
    if (gtk_widget_get_realized (window))
        sde_qa_realized (window, NULL);
}
