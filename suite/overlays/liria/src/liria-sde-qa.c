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
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <glib-unix.h>
#include <gtk/gtk.h>

#include "liria-sde-qa.h"

typedef struct
{
    gint64 begin;
    gint64 layout;
    gint64 area;
} SdeQaFrame;

static FILE *sde_qa_file = NULL;
static char *sde_qa_geometry = NULL;
static GSList *sde_qa_windows = NULL;

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

static void
sde_qa_event (GdkEvent *event, gpointer data)
{
    gint64 begin;

    if (event->type != GDK_MOTION_NOTIFY && event->type != GDK_ENTER_NOTIFY &&
        event->type != GDK_LEAVE_NOTIFY)
    {
        gtk_main_do_event (event);
        return;
    }
    begin = g_get_monotonic_time ();
    gtk_main_do_event (event);
    fprintf (sde_qa_file, "event\t%" G_GINT64_FORMAT "\t%d\t%" G_GINT64_FORMAT "\n",
             begin, (int) event->type, g_get_monotonic_time () - begin);
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
