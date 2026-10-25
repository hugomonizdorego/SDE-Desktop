/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Liria's file operations window in the design of SDE's transfer dialog
 * (0.2.0 Alpha 2, see liria-sde-progress.h and suite/redesign.py). Every
 * operation of the window is a box of its own:
 *
 *   [SDE mark]  Copying 1,234 items from Documents to Backup
 *               42% complete                    [Pause] [Queue] [Cancel]
 *   [progress bar]              or, with More details,
 *   [speed graph: the part done, the speed along the progress, a line at
 *    the current speed with "Speed: 85.3 MB/s"]
 *   Name: holiday.mov
 *   Time remaining: About 2 minutes
 *   Items remaining: 120 (1.2 GB)
 *   (v) More details / Fewer details
 *
 * Copies and moves report what they do with liria_sde_progress_transfer_*
 * () from their thread; the window reads it four times a second and feeds
 * SDE's model (liria-sde-transfer-model.h), which gives the speed, the
 * time remaining and the graph exactly like the SDE desktop's dialog.
 * Other operations (delete, trash, links) keep their own texts under the
 * bar. While an operation runs, a pulse runs along the red, green and blue
 * stripes of the SDE mark at about 30 frames per second (only the mark is
 * redrawn); the mark stands still while the operation is paused, queued or
 * finishing and with reduced motion (gtk-enable-animations off,
 * SDE_REDUCED_MOTION=1, or [aero] animations=false or [appearance]
 * low_resource=true in sde.conf). With one operation the window title is
 * its progress ("42% complete").
 *
 * LIRIA_SDE_QA_PROGRESS=<file> writes the texts, the state and the button
 * positions to that file for tests (tools/liria-progress-smoke.py).
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "liria-sde-progress.h"
#include "liria-sde-transfer-model.h"

#define PROGRESS_KEY "sde-progress"
#define REFRESH_MSEC 250
#define FRAME_MSEC 33

/* ------------------------------------------------------ what jobs report */

typedef struct {
    LiriaSdeTransferKind kind;
    char *source;      /* display name of the folder the items come from, or NULL */
    char *destination; /* display name of the destination folder, or NULL */
    char *item;        /* name of the item being copied */
    gint64 bytes_done;
    gint64 bytes_total;
    int files_done;
    int files_total;
    gboolean finishing;
} SdeReport;

static GMutex report_lock;

static GQuark
report_quark (void)
{
    static GQuark quark = 0;

    if (G_UNLIKELY (quark == 0))
        quark = g_quark_from_static_string ("liria-sde-transfer");
    return quark;
}

static void
report_free (gpointer data)
{
    SdeReport *report = data;

    g_free (report->source);
    g_free (report->destination);
    g_free (report->item);
    g_free (report);
}

/* Called with report_lock held. */
static SdeReport *
report_get (LiriaProgressInfo *info, gboolean create)
{
    SdeReport *report = g_object_get_qdata (G_OBJECT (info), report_quark ());

    if (report == NULL && create)
    {
        report = g_new0 (SdeReport, 1);
        g_object_set_qdata_full (G_OBJECT (info), report_quark (), report, report_free);
    }
    return report;
}

/* "Documents", "Home" (like the SDE desktop); may block. */
static char *
folder_name (GFile *folder)
{
    GFileInfo *file_info;
    char *name = NULL, *path = g_file_get_path (folder);

    if (path != NULL && g_strcmp0 (path, g_get_home_dir ()) == 0)
        name = g_strdup (_("Home"));
    g_free (path);
    if (name == NULL)
    {
        file_info = g_file_query_info (folder, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, G_FILE_QUERY_INFO_NONE,
                                       NULL, NULL);
        if (file_info != NULL)
        {
            name = g_strdup (g_file_info_get_attribute_string (file_info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME));
            g_object_unref (file_info);
        }
    }
    if (name == NULL)
    {
        char *base = g_file_get_basename (folder);

        name = base != NULL ? g_filename_display_name (base) : g_file_get_parse_name (folder);
        g_free (base);
    }
    return name;
}

void
liria_sde_progress_transfer_begin (LiriaProgressInfo *info, LiriaSdeTransferKind kind, GList *files,
                                   GFile *destination)
{
    GFile *parent = NULL;
    gboolean same = files != NULL;
    char *source = NULL, *target = NULL;
    SdeReport *report;
    GList *l;

    /* The source folder when every item comes from the same one. */
    for (l = files; l != NULL && same; l = l->next)
    {
        GFile *folder = g_file_get_parent (l->data);

        if (folder == NULL || (parent != NULL && !g_file_equal (folder, parent)))
            same = FALSE;
        else if (parent == NULL)
            parent = g_object_ref (folder);
        g_clear_object (&folder);
    }
    if (same && parent != NULL)
        source = folder_name (parent);
    if (destination != NULL)
        target = folder_name (destination);
    g_clear_object (&parent);

    g_mutex_lock (&report_lock);
    report = report_get (info, TRUE);
    report->kind = kind;
    g_free (report->source);
    report->source = source;
    g_free (report->destination);
    report->destination = target;
    g_mutex_unlock (&report_lock);
}

void
liria_sde_progress_transfer_counts (LiriaProgressInfo *info, goffset bytes_done, goffset bytes_total,
                                    int files_done, int files_total)
{
    SdeReport *report;

    g_mutex_lock (&report_lock);
    report = report_get (info, TRUE);
    report->bytes_done = bytes_done;
    report->bytes_total = bytes_total;
    report->files_done = files_done;
    report->files_total = files_total;
    g_mutex_unlock (&report_lock);
}

void
liria_sde_progress_transfer_item (LiriaProgressInfo *info, GFile *file)
{
    char *base = g_file_get_basename (file);
    char *name = base != NULL ? g_filename_display_name (base) : NULL;
    SdeReport *report;

    g_free (base);
    g_mutex_lock (&report_lock);
    report = report_get (info, TRUE);
    g_free (report->item);
    report->item = name;
    g_mutex_unlock (&report_lock);
}

void
liria_sde_progress_transfer_finishing (LiriaProgressInfo *info)
{
    SdeReport *report;

    g_mutex_lock (&report_lock);
    report = report_get (info, FALSE);
    if (report != NULL)
        report->finishing = TRUE;
    g_mutex_unlock (&report_lock);
}

void
liria_sde_progress_transfer_throttle (goffset bytes)
{
    static gsize once = 0;
    static double rate = 0.0;
    double t, current;

    if (g_once_init_enter (&once))
    {
        const char *text = g_getenv ("LIRIA_SDE_QA_COPY_RATE");

        rate = text != NULL ? g_ascii_strtod (text, NULL) : 0.0;
        g_once_init_leave (&once, 1);
    }
    if (rate <= 0.0 || bytes <= 0)
        return;
    /* Around the rate like a real drive: a slow wave and a faster ripple. */
    t = g_get_monotonic_time () / (double) G_USEC_PER_SEC;
    current = rate * (1.0 + 0.30 * sin (t * G_PI / 3.0) + 0.08 * sin (t * 7.3));
    g_usleep ((gulong) (bytes / current * G_USEC_PER_SEC));
}

/* ------------------------------------------------------------- helpers */

/* Pixels at 100 %, grown with SDE's interface scale (Xft/DPI). */
static int
sde_px (int px)
{
    gint dpi = -1;

    g_object_get (gtk_settings_get_default (), "gtk-xft-dpi", &dpi, NULL);
    return dpi > 96 * 1024 ? (int) (px * (dpi / 1024.0) / 96.0 + 0.5) : px;
}

/* A boolean of sde.conf: the user's file, then the system's. */
static gboolean
sde_conf_bool (const char *group, const char *key, gboolean fallback)
{
    const gchar *const *system = g_get_system_config_dirs ();
    gboolean value = fallback, found = FALSE;
    int i;

    for (i = -1; !found && (i < 0 || system[i] != NULL); i++)
    {
        char *path = g_build_filename (i < 0 ? g_get_user_config_dir () : system[i], "sde", "sde.conf", NULL);
        GKeyFile *file = g_key_file_new ();

        if (g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL) &&
            g_key_file_has_key (file, group, key, NULL))
        {
            GError *error = NULL;
            gboolean v = g_key_file_get_boolean (file, group, key, &error);

            if (error == NULL)
            {
                value = v;
                found = TRUE;
            }
            g_clear_error (&error);
        }
        g_key_file_unref (file);
        g_free (path);
    }
    return value;
}

static gboolean
reduced_motion (void)
{
    const char *forced = g_getenv ("SDE_REDUCED_MOTION");
    gboolean animations = TRUE;

    if (forced != NULL && *forced != '\0')
        return strcmp (forced, "0") != 0;
    g_object_get (gtk_settings_get_default (), "gtk-enable-animations", &animations, NULL);
    return !animations || !sde_conf_bool ("aero", "animations", TRUE) ||
           sde_conf_bool ("appearance", "low_resource", FALSE);
}

static SdeTransferRgb
theme_rgb (GtkWidget *widget, const char *name, double r, double g, double b)
{
    GdkRGBA rgba;
    SdeTransferRgb out = { r, g, b };

    if (gtk_style_context_lookup_color (gtk_widget_get_style_context (widget), name, &rgba))
    {
        out.r = rgba.red;
        out.g = rgba.green;
        out.b = rgba.blue;
    }
    return out;
}

static SdeTransferRgb
text_rgb (GtkWidget *widget)
{
    GtkStyleContext *context = gtk_widget_get_style_context (widget);
    GdkRGBA rgba;
    SdeTransferRgb out;

    gtk_style_context_get_color (context, gtk_style_context_get_state (context), &rgba);
    out.r = rgba.red;
    out.g = rgba.green;
    out.b = rgba.blue;
    return out;
}

static void
set_text (GtkWidget *label, const char *text)
{
    if (g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), text) != 0)
        gtk_label_set_text (GTK_LABEL (label), text);
}

/* A one-line label that takes the width it gets and ellipsizes the rest,
 * so long names never widen the window. */
static GtkWidget *
line_label (PangoEllipsizeMode ellipsize)
{
    GtkWidget *label = gtk_label_new (NULL);

    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (label), ellipsize);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 1);
    gtk_widget_set_hexpand (label, TRUE);
    return label;
}

static void
label_scale (GtkWidget *label, double scale, gboolean bold)
{
    PangoAttrList *attrs = pango_attr_list_new ();

    pango_attr_list_insert (attrs, pango_attr_scale_new (scale));
    if (bold)
        pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (label), attrs);
    pango_attr_list_unref (attrs);
}

/* ---------------------------------------------------------------- texts */

static char *
format_size (gint64 bytes)
{
    return g_format_size ((guint64) MAX (bytes, 0));
}

/* "85.3 MB/s" */
static char *
format_speed (double bytes_per_second)
{
    char *size = format_size ((gint64) (bytes_per_second + 0.5));
    char *text = g_strdup_printf ("%s/s", size);

    g_free (size);
    return text;
}

/* "About 2 minutes", "About 35 seconds", "A few seconds", "Calculating..." */
static char *
format_remaining (double seconds)
{
    int value = 0, minutes = 0;

    switch (sde_transfer_eta_parts (seconds, &value, &minutes))
    {
        case SDE_TRANSFER_ETA_UNKNOWN:
            return g_strdup (_("Calculating\xE2\x80\xA6"));
        case SDE_TRANSFER_ETA_FEW_SECONDS:
            return g_strdup (_("A few seconds"));
        case SDE_TRANSFER_ETA_SECONDS:
            return g_strdup_printf (ngettext ("About %d second", "About %d seconds", value), value);
        case SDE_TRANSFER_ETA_MINUTES:
            return g_strdup_printf (ngettext ("About %d minute", "About %d minutes", value), value);
        case SDE_TRANSFER_ETA_HOURS:
        default:
            if (minutes == 0)
                return g_strdup_printf (ngettext ("About %d hour", "About %d hours", value), value);
            return g_strdup_printf (ngettext ("About %d hour and %d minutes", "About %d hours and %d minutes",
                                              value), value, minutes);
    }
}

/* ------------------------------------------------------------ the mark */

/* The SDE mark in layers (liria-sde-transfer-model.h), shared by every
 * operation and loaded again when the size or the scale changes. A mark
 * without stripes, a PNG or an icon is drawn still. */
static SdeTransferLogo mark_layers;
static cairo_surface_t *mark_still;
static int mark_size, mark_scale;
static gboolean mark_loaded;

static char *
mark_path (void)
{
    const gchar *const *system = g_get_system_data_dirs ();
    const char *forced = g_getenv ("SDE_BRANDING_DIR");
    GPtrArray *folders = g_ptr_array_new_with_free_func (g_free);
    char *found = NULL;
    guint i;
    int j;

    if (forced != NULL && *forced != '\0')
        g_ptr_array_add (folders, g_strdup (forced));
    g_ptr_array_add (folders, g_build_filename (g_get_user_data_dir (), "sde", "branding", NULL));
    for (i = 0; system[i] != NULL; i++)
        g_ptr_array_add (folders, g_build_filename (system[i], "sde", "branding", NULL));
    g_ptr_array_add (folders, g_strdup ("/usr/share/sde/branding"));
    for (i = 0; i < folders->len && found == NULL; i++)
    {
        for (j = 0; j < 2 && found == NULL; j++)
        {
            char *path = g_build_filename (folders->pdata[i], j == 0 ? "sde-mark.svg" : "sde-mark.png", NULL);

            if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
                found = path;
            else
                g_free (path);
        }
    }
    g_ptr_array_free (folders, TRUE);
    return found;
}

static cairo_surface_t *
render_svg (const char *svg, int size, int scale)
{
    cairo_surface_t *surface = NULL;
    int attempt;

    /* The loader named "svg" first, then by MIME type and by sniffing: not
     * every gdk-pixbuf build (glycin) has a loader of that name. On Debian
     * testing the mark fell back to a generic icon (0.2 Alpha 2); when no
     * loader reads SVG at all, mark_load () takes the PNG beside it. */
    for (attempt = 0; attempt < 3 && surface == NULL; attempt++)
    {
        GdkPixbufLoader *loader = attempt == 0 ? gdk_pixbuf_loader_new_with_type ("svg", NULL)
                                  : attempt == 1 ? gdk_pixbuf_loader_new_with_mime_type ("image/svg+xml", NULL)
                                                 : gdk_pixbuf_loader_new ();
        gboolean ok;

        if (loader == NULL)
            continue;
        gdk_pixbuf_loader_set_size (loader, size * scale, size * scale);
        ok = gdk_pixbuf_loader_write (loader, (const guchar *) svg, strlen (svg), NULL);
        ok = gdk_pixbuf_loader_close (loader, NULL) && ok;
        if (ok && gdk_pixbuf_loader_get_pixbuf (loader) != NULL)
            surface = gdk_cairo_surface_create_from_pixbuf (gdk_pixbuf_loader_get_pixbuf (loader), scale, NULL);
        g_object_unref (loader);
    }
    return surface;
}

static void
mark_load (int size, int scale)
{
    char *path, *svg = NULL;
    int i;

    if (mark_loaded && size == mark_size && scale == mark_scale)
        return;
    g_clear_pointer (&mark_layers.under, cairo_surface_destroy);
    for (i = 0; i < 3; i++)
        g_clear_pointer (&mark_layers.stripe[i], cairo_surface_destroy);
    g_clear_pointer (&mark_layers.over, cairo_surface_destroy);
    g_clear_pointer (&mark_still, cairo_surface_destroy);
    mark_loaded = TRUE;
    mark_size = size;
    mark_scale = scale;
    mark_layers.size = size;

    path = mark_path ();
    if (path != NULL && g_str_has_suffix (path, ".svg") && g_file_get_contents (path, &svg, NULL, NULL))
    {
        cairo_surface_t *layers[5] = { NULL, NULL, NULL, NULL, NULL };
        gboolean ok = TRUE;

        for (i = 0; i < 5 && ok; i++)
        {
            char *part = sde_transfer_logo_layer_svg (svg, i);

            layers[i] = part != NULL ? render_svg (part, size, scale) : NULL;
            ok = layers[i] != NULL;
            free (part);
        }
        if (ok)
        {
            mark_layers.under = layers[0];
            for (i = 0; i < 3; i++)
                mark_layers.stripe[i] = layers[i + 1];
            mark_layers.over = layers[4];
        }
        else
        {
            for (i = 0; i < 5; i++)
                if (layers[i] != NULL)
                    cairo_surface_destroy (layers[i]);
        }
    }
    if (mark_layers.under == NULL && path != NULL)
    {
        /* The SVG as one picture, else the PNG next to it. */
        char *png = g_str_has_suffix (path, ".svg") ? g_strdup (path) : NULL;
        const char *files[2] = { path, NULL };

        if (png != NULL)
        {
            strcpy (png + strlen (png) - 3, "png");
            files[1] = png;
        }
        for (i = 0; i < 2 && mark_still == NULL; i++)
        {
            GdkPixbuf *pixbuf = files[i] != NULL
                                    ? gdk_pixbuf_new_from_file_at_size (files[i], size * scale, size * scale, NULL)
                                    : NULL;

            if (pixbuf != NULL)
            {
                mark_still = gdk_cairo_surface_create_from_pixbuf (pixbuf, scale, NULL);
                g_object_unref (pixbuf);
            }
        }
        g_free (png);
    }
    if (mark_layers.under == NULL && mark_still == NULL)
    {
        static const char *const icons[] = { "sde-logo", "system-file-manager" };

        for (i = 0; i < 2 && mark_still == NULL; i++)
            mark_still = gtk_icon_theme_load_surface (gtk_icon_theme_get_default (), icons[i], size, scale, NULL,
                                                      GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
    g_free (svg);
    g_free (path);
}

/* -------------------------------------------------------- the operation */

typedef struct {
    GtkWidget *box;
    LiriaProgressInfo *info;
    GtkWidget *mark;
    GtkWidget *status;   /* the operation's own status (other operations) */
    GtkWidget *headline; /* "Copying 3 items from Downloads to Desktop" */
    GtkWidget *percent;
    GtkWidget *pause_button, *queue_button, *cancel_button;
    GtkWidget *own_bar;  /* the operation's own bar: pulses while it prepares and finishes */
    GtkWidget *bar;      /* a copy's bar, from the bytes */
    GtkWidget *graph;
    GtkWidget *rows;
    GtkWidget *values[3]; /* name, time remaining, items remaining */
    GtkWidget *details;  /* the operation's own details */
    GtkWidget *toggle, *toggle_arrow, *toggle_label;
    SdeTransfer model;
    LiriaSdeTransferKind kind;
    char *source, *destination;
    int state;
    gboolean begun, counted, finishing, details_shown, arrow_up, motion;
    gint64 started;
    guint refresh_id, frame_id;
} SdeProgress;

/* More details stay open for the next operations of the session. */
static gboolean details_default = FALSE;

static double
monotonic_seconds (void)
{
    return g_get_monotonic_time () / (double) G_USEC_PER_SEC;
}

static char *
format_headline (SdeProgress *p)
{
    long n = p->model.files_total;
    gboolean move = p->kind == LIRIA_SDE_TRANSFER_MOVE;
    char *count, *text;

    if (p->kind == LIRIA_SDE_TRANSFER_DUPLICATE || p->destination == NULL)
    {
        if (n <= 0)
            return g_strdup (_("Preparing"));
        count = g_strdup_printf ("%'ld", n);
        text = p->source != NULL ?
               g_strdup_printf (ngettext ("Duplicating %s item in %s", "Duplicating %s items in %s", n), count,
                                p->source) :
               g_strdup_printf (ngettext ("Duplicating %s item", "Duplicating %s items", n), count);
        g_free (count);
        return text;
    }
    if (n <= 0)
        return g_strdup_printf (move ? _("Moving to %s") : _("Copying to %s"), p->destination);
    count = g_strdup_printf ("%'ld", n);
    if (p->source == NULL)
        text = g_strdup_printf (move ? ngettext ("Moving %s item to %s", "Moving %s items to %s", n) :
                                       ngettext ("Copying %s item to %s", "Copying %s items to %s", n),
                                count, p->destination);
    else
        text = g_strdup_printf (move ? ngettext ("Moving %s item from %s to %s", "Moving %s items from %s to %s", n) :
                                       ngettext ("Copying %s item from %s to %s", "Copying %s items from %s to %s", n),
                                count, p->source, p->destination);
    g_free (count);
    return text;
}

/* "42% complete", "Paused - 42% complete", "Finishing..."; an operation
 * that only pulses has no percentage. */
static char *
format_percent (SdeProgress *p, double progress)
{
    int percent = progress < 0 ? 0 : (int) floor (progress * 100.0);

    if (p->finishing)
        return g_strdup (_("Finishing\xE2\x80\xA6"));
    if (progress < 0 && !p->begun)
        return g_strdup (p->state == 1 ? _("Paused") : p->state == 2 ? _("Queued") : "");
    if (p->state == 1)
        return g_strdup_printf (_("Paused - %d%% complete"), percent);
    if (p->state == 2)
        return g_strdup_printf (_("Queued - %d%% complete"), percent);
    return g_strdup_printf (_("%d%% complete"), percent);
}

static char *
format_speed_text (SdeProgress *p)
{
    char *speed, *text;

    if (p->model.speed <= 0)
        return g_strdup ("");
    speed = format_speed (p->model.speed);
    text = g_strdup_printf (_("Speed: %s"), speed);
    g_free (speed);
    return text;
}

static char *
format_time_text (SdeProgress *p)
{
    if (p->state == 1)
        return g_strdup (_("Paused"));
    if (p->state == 2)
        return g_strdup (_("Queued"));
    return format_remaining (sde_transfer_remaining (&p->model));
}

static char *
format_items_text (SdeProgress *p)
{
    long items = MAX (p->model.files_total - p->model.files_done, 0);
    char *count, *size, *text;

    if (p->model.files_total <= 0)
        return g_strdup (_("Calculating\xE2\x80\xA6"));
    count = g_strdup_printf ("%'ld", items);
    size = format_size ((gint64) (p->model.bytes_total - p->model.bytes_done));
    text = g_strdup_printf ("%s (%s)", count, size);
    g_free (count);
    g_free (size);
    return text;
}

static gboolean
frame_tick (gpointer data)
{
    SdeProgress *p = data;

    gtk_widget_queue_draw (p->mark);
    return G_SOURCE_CONTINUE;
}

static void
animation_update (SdeProgress *p)
{
    gboolean want = p->motion && p->state == 0 && !p->finishing && gtk_widget_get_mapped (p->mark) &&
                    mark_layers.under != NULL;

    if (want && p->frame_id == 0)
    {
        p->frame_id = g_timeout_add (FRAME_MSEC, frame_tick, p);
    }
    else if (!want && p->frame_id != 0)
    {
        g_source_remove (p->frame_id);
        p->frame_id = 0;
        gtk_widget_queue_draw (p->mark);
    }
}

static void
mark_mapped (SdeProgress *p)
{
    mark_load (sde_px (72), gtk_widget_get_scale_factor (p->mark));
    animation_update (p);
}

static gboolean
mark_draw (GtkWidget *widget, cairo_t *cr, SdeProgress *p)
{
    mark_load (sde_px (72), gtk_widget_get_scale_factor (widget));
    if (mark_layers.under != NULL)
        sde_transfer_draw_logo (cr, &mark_layers, 0, 0, (g_get_monotonic_time () - p->started) / 1e6,
                                p->frame_id != 0);
    else if (mark_still != NULL)
    {
        cairo_set_source_surface (cr, mark_still, 0, 0);
        cairo_paint (cr);
    }
    return FALSE;
}

static gboolean
graph_draw (GtkWidget *widget, cairo_t *cr, SdeProgress *p)
{
    int w = gtk_widget_get_allocated_width (widget), h = gtk_widget_get_allocated_height (widget);
    SdeTransferRgb accent = p->state != 0 ? theme_rgb (widget, "warning_color", 0.96, 0.62, 0.04) :
                                            theme_rgb (widget, "theme_selected_bg_color", 0.21, 0.52, 0.89);
    SdeTransferRgb fg = text_rgb (widget);
    double line_y = sde_transfer_draw_graph (cr, 0, 0, w, h, &p->model, accent, fg);
    char *text = format_speed_text (p);

    if (line_y >= 0 && *text != '\0')
    {
        /* Over the current speed line, at the right, inside the graph. */
        PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);
        PangoAttrList *attrs = pango_attr_list_new ();
        int tw, th, y;

        pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        pango_attr_list_insert (attrs, pango_attr_scale_new (0.92));
        pango_layout_set_attributes (layout, attrs);
        pango_attr_list_unref (attrs);
        pango_layout_get_pixel_size (layout, &tw, &th);
        y = (int) line_y - th - sde_px (2);
        if (y < sde_px (2))
            y = (int) line_y + sde_px (3);
        cairo_set_source_rgba (cr, fg.r, fg.g, fg.b, 0.92);
        cairo_move_to (cr, w - tw - sde_px (8), y);
        pango_cairo_show_layout (cr, layout);
        g_object_unref (layout);
    }
    g_free (text);
    return FALSE;
}

static void
sync_layout (SdeProgress *p)
{
    gboolean expanded = p->counted && p->details_shown;

    gtk_widget_set_visible (p->headline, p->begun);
    gtk_widget_set_visible (p->status, !p->begun);
    gtk_widget_set_visible (p->bar, p->counted && !p->finishing && !expanded);
    gtk_widget_set_visible (p->own_bar, !p->counted || (p->finishing && !expanded));
    gtk_widget_set_visible (p->graph, expanded);
    gtk_widget_set_visible (p->rows, expanded);
    gtk_widget_set_visible (p->details, !p->counted || p->finishing);
    gtk_widget_set_visible (p->toggle, p->counted);
    if (p->arrow_up != p->details_shown)
    {
        p->arrow_up = p->details_shown;
        gtk_image_set_from_icon_name (GTK_IMAGE (p->toggle_arrow),
                                      p->details_shown ? "pan-up-symbolic" : "pan-down-symbolic",
                                      GTK_ICON_SIZE_BUTTON);
    }
    set_text (p->toggle_label, p->details_shown ? _("Fewer details") : _("More details"));
}

static void
qa_rect (GString *out, const char *key, GtkWidget *widget)
{
    GtkWidget *top = gtk_widget_get_toplevel (widget);
    int x = 0, y = 0;

    if (gtk_widget_get_mapped (widget) && gtk_widget_translate_coordinates (widget, top, 0, 0, &x, &y))
        g_string_append_printf (out, "%s=%d,%d,%d,%d\n", key, x, y, gtk_widget_get_allocated_width (widget),
                                gtk_widget_get_allocated_height (widget));
}

/* LIRIA_SDE_QA_PROGRESS: what the window shows, for tests. */
static void
qa_write (SdeProgress *p, const char *percent)
{
    const char *path = g_getenv ("LIRIA_SDE_QA_PROGRESS");
    GtkWidget *top = gtk_widget_get_toplevel (p->box);
    char *speed, *remaining, *items;
    GString *out;

    if (path == NULL || *path == '\0')
        return;
    speed = format_speed_text (p);
    remaining = format_time_text (p);
    items = format_items_text (p);
    out = g_string_new (NULL);
    g_string_append_printf (out, "headline=%s\n",
                            gtk_label_get_text (GTK_LABEL (p->begun ? p->headline : p->status)));
    g_string_append_printf (out, "percent=%s\nspeed=%s\nremaining=%s\nitems=%s\nname=%s\n", percent, speed,
                            remaining, items, gtk_label_get_text (GTK_LABEL (p->values[0])));
    g_string_append_printf (out, "state=%d\npaused=%d\nanimating=%d\ndetails=%d\nfinishing=%d\nreduced=%d\n",
                            p->state, p->state != 0, p->frame_id != 0, p->counted && p->details_shown,
                            p->finishing, !p->motion);
    g_string_append_printf (out, "logo=%s\nbytes=%.0f/%.0f\nitems_done=%ld/%ld\n",
                            mark_layers.under != NULL ? "layers" : mark_still != NULL ? "still" : "none",
                            p->model.bytes_done, p->model.bytes_total, p->model.files_done, p->model.files_total);
    if (GTK_IS_WINDOW (top))
    {
        g_string_append_printf (out, "title=%s\n", gtk_window_get_title (GTK_WINDOW (top)));
#ifdef GDK_WINDOWING_X11
        if (gtk_widget_get_realized (top) && GDK_IS_X11_WINDOW (gtk_widget_get_window (top)))
            g_string_append_printf (out, "window=%lu\n",
                                    (unsigned long) GDK_WINDOW_XID (gtk_widget_get_window (top)));
#endif
    }
    qa_rect (out, "pause", p->pause_button);
    qa_rect (out, "queue", p->queue_button);
    qa_rect (out, "cancel", p->cancel_button);
    qa_rect (out, "toggle", p->toggle);
    qa_rect (out, "mark", p->mark);
    qa_rect (out, "graph", p->graph);
    g_file_set_contents (path, out->str, out->len, NULL);
    g_string_free (out, TRUE);
    g_free (speed);
    g_free (remaining);
    g_free (items);
}

static void
update_title (SdeProgress *p, const char *percent)
{
    GtkWidget *top = gtk_widget_get_toplevel (p->box), *parent = gtk_widget_get_parent (p->box);
    const char *title = _("File Operations");
    GList *children;

    if (!GTK_IS_WINDOW (top) || parent == NULL)
        return;
    children = gtk_container_get_children (GTK_CONTAINER (parent));
    if (children != NULL && children->next == NULL && *percent != '\0')
        title = percent;
    g_list_free (children);
    if (g_strcmp0 (gtk_window_get_title (GTK_WINDOW (top)), title) != 0)
        gtk_window_set_title (GTK_WINDOW (top), title);
}

static void
progress_refresh (SdeProgress *p)
{
    SdeReport *report;
    SdeReport now_report = { 0 };
    gboolean begun = FALSE;
    double progress;
    char *text;

    g_mutex_lock (&report_lock);
    report = report_get (p->info, FALSE);
    if (report != NULL)
    {
        begun = TRUE;
        now_report = *report;
        now_report.source = g_strdup (report->source);
        now_report.destination = g_strdup (report->destination);
        now_report.item = g_strdup (report->item);
    }
    g_mutex_unlock (&report_lock);

    if (begun)
    {
        if (!p->begun)
        {
            p->begun = TRUE;
            p->kind = now_report.kind;
            p->source = g_strdup (now_report.source);
            p->destination = g_strdup (now_report.destination);
        }
        if (now_report.files_total > 0 && !p->counted)
        {
            /* The speed and the time count from the first counted item,
             * not from the start of the scan. */
            sde_transfer_init (&p->model, monotonic_seconds ());
            if (p->state != 0)
                sde_transfer_set_paused (&p->model, p->model.now, 1);
        }
        sde_transfer_update (&p->model, MAX (monotonic_seconds (), p->model.now), (double) now_report.bytes_done,
                             (double) now_report.bytes_total, now_report.files_done, now_report.files_total);
        p->counted = now_report.files_total > 0;
        if (now_report.finishing && !p->finishing)
        {
            p->finishing = TRUE;
            animation_update (p);
        }
        text = format_headline (p);
        set_text (p->headline, text);
        g_free (text);
        progress = sde_transfer_progress (&p->model);
        if (p->counted && !p->finishing && progress >= 0)
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->bar), progress);
    }
    else
    {
        progress = liria_progress_info_get_progress (p->info);
    }

    if (p->counted && p->details_shown)
    {
        set_text (p->values[0], now_report.item != NULL ? now_report.item : "\xE2\x80\x94");
        text = format_time_text (p);
        set_text (p->values[1], text);
        g_free (text);
        text = format_items_text (p);
        set_text (p->values[2], text);
        g_free (text);
        if (p->state == 0)
            gtk_widget_queue_draw (p->graph);
    }
    else if (now_report.item != NULL)
    {
        set_text (p->values[0], now_report.item);
    }

    text = format_percent (p, progress);
    set_text (p->percent, text);
    sync_layout (p);
    update_title (p, text);
    qa_write (p, text);
    g_free (text);
    g_free (now_report.source);
    g_free (now_report.destination);
    g_free (now_report.item);
}

static gboolean
refresh_tick (gpointer data)
{
    progress_refresh (data);
    return G_SOURCE_CONTINUE;
}

static void
toggle_clicked (GtkButton *button, SdeProgress *p)
{
    p->details_shown = !p->details_shown;
    details_default = p->details_shown;
    progress_refresh (p);
}

/* A line between two operations, as wide as their content. */
static gboolean
box_draw (GtkWidget *box, cairo_t *cr, SdeProgress *p)
{
    GtkWidget *parent = gtk_widget_get_parent (box);
    GList *children;
    gboolean first;

    if (parent == NULL || !GTK_IS_CONTAINER (parent))
        return FALSE;
    children = gtk_container_get_children (GTK_CONTAINER (parent));
    first = children == NULL || children->data == box;
    g_list_free (children);
    if (!first)
    {
        SdeTransferRgb fg = text_rgb (box);
        int inset = sde_px (18);

        cairo_set_source_rgba (cr, fg.r, fg.g, fg.b, 0.14);
        cairo_rectangle (cr, inset, 0, gtk_widget_get_allocated_width (box) - 2 * inset, 1);
        cairo_fill (cr);
    }
    return FALSE;
}

static void
progress_destroyed (GtkWidget *box, SdeProgress *p)
{
    const char *qa = g_getenv ("LIRIA_SDE_QA_PROGRESS");

    if (p->refresh_id != 0)
    {
        g_source_remove (p->refresh_id);
        if (qa != NULL && *qa != '\0')
            g_file_set_contents (qa, "done=1\n", -1, NULL);
    }
    if (p->frame_id != 0)
        g_source_remove (p->frame_id);
    p->refresh_id = 0;
    p->frame_id = 0;
}

static void
progress_free (gpointer data)
{
    SdeProgress *p = data;

    progress_destroyed (NULL, p);
    g_free (p->source);
    g_free (p->destination);
    g_free (p);
}

/* Pause, Queue and Cancel: round flat buttons with the theme's symbolic
 * icons, at the size of the interface. */
static void
action_button (GtkWidget *button, GtkWidget *box)
{
    GtkStyleContext *context = gtk_widget_get_style_context (button);
    GtkWidget *child = gtk_bin_get_child (GTK_BIN (button));
    static const char *const keys[] = { "pauseimg", "resumeimg" };
    int i;

    gtk_style_context_add_class (context, "flat");
    gtk_style_context_add_class (context, "circular");
    gtk_style_context_add_class (context, "sde-progress-action");
    gtk_widget_set_size_request (button, sde_px (32), sde_px (32));
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    if (GTK_IS_IMAGE (child))
        gtk_image_set_pixel_size (GTK_IMAGE (child), sde_px (16));
    for (i = 0; i < 2; i++)
    {
        GtkWidget *image = g_object_get_data (G_OBJECT (button), keys[i]);

        if (GTK_IS_IMAGE (image))
            gtk_image_set_pixel_size (GTK_IMAGE (image), sde_px (16));
    }
    gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
}

static GtkWidget *
managed (GtkWidget *widget)
{
    /* The window's gtk_widget_show_all () must not show it: sync_layout ()
     * does. */
    gtk_widget_set_no_show_all (widget, TRUE);
    return widget;
}

void
liria_sde_progress_build (GtkWidget *box, LiriaProgressInfo *info, GtkWidget *start_button,
                          GtkWidget *queue_button, GtkWidget *cancel_button, GtkWidget **status,
                          GtkWidget **progress_bar, GtkWidget **details)
{
    static const char *const row_names[] = { N_("Name:"), N_("Time remaining:"), N_("Items remaining:") };
    SdeProgress *p = g_new0 (SdeProgress, 1);
    GtkWidget *inner, *header, *line, *buttons, *content;
    int i;

    p->box = box;
    p->info = info;
    p->started = g_get_monotonic_time ();
    p->motion = !reduced_motion ();
    p->details_shown = details_default;
    sde_transfer_init (&p->model, monotonic_seconds ());
    g_object_set_data_full (G_OBJECT (box), PROGRESS_KEY, p, progress_free);
    g_signal_connect (box, "destroy", G_CALLBACK (progress_destroyed), p);
    g_signal_connect (box, "draw", G_CALLBACK (box_draw), p);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "sde-progress");
    gtk_widget_set_size_request (box, sde_px (480), -1);
    gtk_box_set_spacing (GTK_BOX (box), 0);
    /* The content, inside a margin; the line between operations is drawn
     * on the operation's box (box_draw ()). */
    inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, sde_px (10));
    gtk_container_set_border_width (GTK_CONTAINER (inner), sde_px (18));
    gtk_box_pack_start (GTK_BOX (box), inner, TRUE, TRUE, 0);

    /* The mark, the headline, the percentage and the buttons. */
    header = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (header), sde_px (14));
    p->mark = gtk_drawing_area_new ();
    gtk_widget_set_size_request (p->mark, sde_px (72), sde_px (72));
    gtk_widget_set_valign (p->mark, GTK_ALIGN_START);
    g_signal_connect (p->mark, "draw", G_CALLBACK (mark_draw), p);
    g_signal_connect_swapped (p->mark, "map", G_CALLBACK (mark_mapped), p);
    g_signal_connect_swapped (p->mark, "unmap", G_CALLBACK (animation_update), p);
    gtk_grid_attach (GTK_GRID (header), p->mark, 0, 0, 1, 2);

    line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (line, sde_px (4));
    p->status = managed (line_label (PANGO_ELLIPSIZE_END));
    p->headline = managed (line_label (PANGO_ELLIPSIZE_END));
    gtk_style_context_add_class (gtk_widget_get_style_context (p->status), "dim-label");
    gtk_style_context_add_class (gtk_widget_get_style_context (p->headline), "dim-label");
    gtk_box_pack_start (GTK_BOX (line), p->status, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (line), p->headline, TRUE, TRUE, 0);
    gtk_grid_attach (GTK_GRID (header), line, 1, 0, 2, 1);

    p->percent = line_label (PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class (gtk_widget_get_style_context (p->percent), "sde-progress-percent");
    label_scale (p->percent, 1.45, FALSE);
    gtk_widget_set_vexpand (p->percent, TRUE);
    gtk_widget_set_valign (p->percent, GTK_ALIGN_CENTER);
    gtk_grid_attach (GTK_GRID (header), p->percent, 1, 1, 1, 1);

    buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, sde_px (6));
    gtk_widget_set_valign (buttons, GTK_ALIGN_CENTER);
    p->pause_button = start_button;
    p->queue_button = queue_button;
    p->cancel_button = cancel_button;
    action_button (start_button, buttons);
    action_button (queue_button, buttons);
    action_button (cancel_button, buttons);
    gtk_grid_attach (GTK_GRID (header), buttons, 2, 1, 1, 1);
    gtk_box_pack_start (GTK_BOX (inner), header, FALSE, FALSE, 0);

    /* The bars: the operation's own one and a copy's. */
    p->own_bar = managed (gtk_progress_bar_new ());
    p->bar = managed (gtk_progress_bar_new ());
    gtk_style_context_add_class (gtk_widget_get_style_context (p->own_bar), "sde-progress-bar");
    gtk_style_context_add_class (gtk_widget_get_style_context (p->bar), "sde-progress-bar");
    gtk_box_pack_start (GTK_BOX (inner), p->own_bar, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (inner), p->bar, FALSE, FALSE, 0);

    /* More details: the speed graph and the rows. */
    p->graph = managed (gtk_drawing_area_new ());
    gtk_widget_set_size_request (p->graph, -1, sde_px (104));
    gtk_style_context_add_class (gtk_widget_get_style_context (p->graph), "sde-progress-graph");
    g_signal_connect (p->graph, "draw", G_CALLBACK (graph_draw), p);
    gtk_box_pack_start (GTK_BOX (inner), p->graph, FALSE, FALSE, 0);

    p->rows = managed (gtk_grid_new ());
    gtk_grid_set_column_spacing (GTK_GRID (p->rows), sde_px (12));
    gtk_grid_set_row_spacing (GTK_GRID (p->rows), sde_px (4));
    for (i = 0; i < 3; i++)
    {
        GtkWidget *name = gtk_label_new (_(row_names[i]));

        gtk_label_set_xalign (GTK_LABEL (name), 0.0);
        gtk_style_context_add_class (gtk_widget_get_style_context (name), "dim-label");
        p->values[i] = line_label (i == 0 ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_END);
        gtk_label_set_text (GTK_LABEL (p->values[i]), "\xE2\x80\x94");
        gtk_grid_attach (GTK_GRID (p->rows), name, 0, i, 1, 1);
        gtk_grid_attach (GTK_GRID (p->rows), p->values[i], 1, i, 1, 1);
        gtk_widget_show (name);
        gtk_widget_show (p->values[i]);
    }
    gtk_box_pack_start (GTK_BOX (inner), p->rows, FALSE, FALSE, 0);

    /* The operation's own details ("Preparing to copy 12 files"). */
    p->details = managed (gtk_label_new (NULL));
    gtk_label_set_xalign (GTK_LABEL (p->details), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (p->details), TRUE);
    gtk_label_set_line_wrap_mode (GTK_LABEL (p->details), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars (GTK_LABEL (p->details), 1);
    gtk_style_context_add_class (gtk_widget_get_style_context (p->details), "dim-label");
    gtk_box_pack_start (GTK_BOX (inner), p->details, FALSE, FALSE, 0);

    /* More details / Fewer details. */
    p->toggle = managed (gtk_button_new ());
    gtk_style_context_add_class (gtk_widget_get_style_context (p->toggle), "flat");
    gtk_style_context_add_class (gtk_widget_get_style_context (p->toggle), "sde-progress-toggle");
    gtk_widget_set_halign (p->toggle, GTK_ALIGN_START);
    content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, sde_px (6));
    p->toggle_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic", GTK_ICON_SIZE_BUTTON);
    p->toggle_label = gtk_label_new (_("More details"));
    gtk_box_pack_start (GTK_BOX (content), p->toggle_arrow, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content), p->toggle_label, FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (p->toggle), content);
    gtk_widget_show_all (content);
    g_signal_connect (p->toggle, "clicked", G_CALLBACK (toggle_clicked), p);
    gtk_box_pack_start (GTK_BOX (inner), p->toggle, FALSE, FALSE, 0);

    *status = p->status;
    *progress_bar = p->own_bar;
    *details = p->details;
    sync_layout (p);
    p->refresh_id = g_timeout_add (REFRESH_MSEC, refresh_tick, p);
}

void
liria_sde_progress_set_state (GtkWidget *box, int state)
{
    SdeProgress *p = g_object_get_data (G_OBJECT (box), PROGRESS_KEY);
    GtkStyleContext *context;

    if (p == NULL || p->state == state)
        return;
    sde_transfer_set_paused (&p->model, MAX (monotonic_seconds (), p->model.now), state != 0);
    p->state = state;
    context = gtk_widget_get_style_context (box);
    if (state != 0)
        gtk_style_context_add_class (context, "sde-paused");
    else
        gtk_style_context_remove_class (context, "sde-paused");
    animation_update (p);
    gtk_widget_queue_draw (p->graph);
    progress_refresh (p);
}

void
liria_sde_progress_window_setup (GtkWidget *window)
{
    GtkWidget *child = gtk_bin_get_child (GTK_BIN (window));

    gtk_container_set_border_width (GTK_CONTAINER (window), 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (window), "sde-progress-window");
    if (GTK_IS_BOX (child))
        gtk_box_set_spacing (GTK_BOX (child), 0);
}
