/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Liria's file operations window in the design of SDE's transfer dialog
 * (0.2.0 Alpha 3, see liria-sde-progress.h and suite/redesign.py). Every
 * operation of the window is a box of its own; the first one is drawn in
 * full, the others under it as the queue:
 *
 *   [a penguin takes a paper out of one folder, carries it on its head to
 *    the other folder and drops it in; a bin for Delete and the Trash]
 *   Copying 3 items to Backup                     [Pause] [Queue] [Cancel]
 *   [==================>                         ]
 *   780 MB of 1.2 GB · 22 MB/s · About 40 seconds left
 *   holiday.mov
 *   (v) Details: From Videos, To Backup on SANDISK, Items 2 of 3
 *   ----
 *   1 more operation
 *   Moving 12 items to Documents                  [Pause] [Queue] [Cancel]
 *   [=========>             ] 35 %
 *
 * Copies and moves report what they do with liria_sde_progress_transfer_*
 * () from their thread; the window reads it four times a second and feeds
 * SDE's model (liria-sde-transfer-model.h): the speed and the time left
 * like the SDE desktop's dialog. Delete and the Trash show the bin with
 * their own texts; other operations keep their own texts without a
 * picture. The picture (liria-sde-transfer-scene.h) moves at about 30
 * frames per second while it is visible (only it is redrawn); it stands
 * still while paused, queued, minimized and with reduced motion
 * (gtk-enable-animations off, SDE_REDUCED_MOTION=1, or [aero]
 * animations=false or [appearance] low_resource=true in sde.conf). A copy
 * to a removable drive says at the end that the files are on the drive and
 * that it can be removed; the window title is the first operation's
 * progress ("42% · Copying 3 items to Backup").
 *
 * LIRIA_SDE_QA_PROGRESS=<file> writes the texts, the state and the
 * positions of the buttons and the picture to that file for tests
 * (tools/liria-progress-smoke.py, tools/safe-copy-check.py).
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
#include "liria-sde-transfer-scene.h"

#define PROGRESS_KEY "sde-progress"
#define REFRESH_MSEC 250
#define FRAME_MSEC 33
#define DONE_MSEC 3000      /* a finished operation stays this long */
#define DONE_DRIVE_MSEC 6000 /* ... when it says the drive may be removed */

/* ------------------------------------------------------ what jobs report */

typedef struct {
    LiriaSdeTransferKind kind;
    char *source;      /* display name of the folder the items come from, or NULL */
    char *destination; /* display name of the destination folder, or NULL */
    char *item;        /* name of the item being copied */
    char *drive;       /* the destination drive written safely, or NULL */
    char *failure;     /* what went wrong with the drive, or NULL */
    gint64 bytes_done;
    gint64 bytes_total;
    int files_done;
    int files_total;
    gboolean finishing, removable, fat;
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
    g_free (report->drive);
    g_free (report->failure);
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
    if (kind == LIRIA_SDE_TRANSFER_DELETE || kind == LIRIA_SDE_TRANSFER_TRASH)
        report->files_total = (int) g_list_length (files);
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
liria_sde_progress_transfer_drive (LiriaProgressInfo *info, const char *name, gboolean removable, gboolean fat)
{
    SdeReport *report;

    g_mutex_lock (&report_lock);
    report = report_get (info, TRUE);
    g_free (report->drive);
    report->drive = g_strdup (name);
    report->removable = removable;
    report->fat = fat;
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
liria_sde_progress_transfer_failed (LiriaProgressInfo *info, const char *message)
{
    SdeReport *report;

    g_mutex_lock (&report_lock);
    report = report_get (info, TRUE);
    g_free (report->failure);
    report->failure = g_strdup (message);
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

static SdeSceneRgb
text_rgb (GtkWidget *widget)
{
    GtkStyleContext *context = gtk_widget_get_style_context (widget);
    GdkRGBA rgba;
    SdeSceneRgb out;

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

/* "About 2 minutes left", "About 35 seconds left", "A few seconds left",
 * "Calculating..." */
static char *
format_left (double seconds)
{
    int value = 0, minutes = 0;

    switch (sde_transfer_eta_parts (seconds, &value, &minutes))
    {
        case SDE_TRANSFER_ETA_UNKNOWN:
            return g_strdup (_("Calculating\xE2\x80\xA6"));
        case SDE_TRANSFER_ETA_FEW_SECONDS:
            return g_strdup (_("A few seconds left"));
        case SDE_TRANSFER_ETA_SECONDS:
            return g_strdup_printf (ngettext ("About %d second left", "About %d seconds left", value), value);
        case SDE_TRANSFER_ETA_MINUTES:
            return g_strdup_printf (ngettext ("About %d minute left", "About %d minutes left", value), value);
        case SDE_TRANSFER_ETA_HOURS:
        default:
            if (minutes == 0)
                return g_strdup_printf (ngettext ("About %d hour left", "About %d hours left", value), value);
            return g_strdup_printf (
                ngettext ("About %d hour and %d minutes left", "About %d hours and %d minutes left", value), value,
                minutes);
    }
}

/* -------------------------------------------------------- the operation */

typedef struct {
    GtkWidget *box;
    LiriaProgressInfo *info;
    GtkWidget *scene;
    GtkWidget *queue_heading; /* "2 more operations", in the second box */
    GtkWidget *status;   /* the operation's own status (other operations) */
    GtkWidget *headline; /* "Copying 3 items to Backup" */
    GtkWidget *pause_button, *queue_button, *cancel_button;
    GtkWidget *own_bar;  /* the operation's own bar: pulses while it prepares and finishes */
    GtkWidget *bar;      /* a copy's bar, from the bytes */
    GtkWidget *line;     /* "780 MB of 1.2 GB · 22 MB/s · About 40 seconds left" */
    GtkWidget *details;  /* the operation's own details (other operations) */
    GtkWidget *note;     /* the item being copied, a hint */
    GtkWidget *toggle, *toggle_arrow;
    GtkWidget *rows;
    GtkWidget *values[3]; /* from, to, items */
    SdeTransfer model;
    LiriaSdeTransferKind kind;
    char *source, *destination, *drive, *failure;
    int state;
    gboolean begun, counted, finishing, removable, fat, arrow_up, motion, main, details_shown;
    gboolean finished, failed;
    gint64 started, state_changed;
    guint refresh_id, frame_id, done_id;
    GSourceFunc remove;
    gpointer remove_data;
} SdeProgress;

/* Details stay open for the next operations of the session. */
static gboolean details_default = FALSE;
/* The window is minimized: no picture moves. */
static gboolean window_iconified = FALSE;
static GList *all_progress = NULL;

static double
monotonic_seconds (void)
{
    return g_get_monotonic_time () / (double) G_USEC_PER_SEC;
}

static gboolean
deleting (SdeProgress *p)
{
    return p->kind == LIRIA_SDE_TRANSFER_DELETE || p->kind == LIRIA_SDE_TRANSFER_TRASH;
}

/* A copy or a move, whose texts are SDE's (not a deletion's own). */
static gboolean
transfer (SdeProgress *p)
{
    return p->begun && !deleting (p);
}

static double
progress_of (SdeProgress *p)
{
    if (transfer (p))
        return sde_transfer_progress (&p->model);
    return liria_progress_info_get_progress (p->info);
}

static char *
format_headline (SdeProgress *p)
{
    long n = p->model.files_total;
    gboolean move = p->kind == LIRIA_SDE_TRANSFER_MOVE;
    char *count, *text;

    if (p->failed)
    {
        if (p->kind == LIRIA_SDE_TRANSFER_DUPLICATE || p->destination == NULL)
            return g_strdup (_("Not everything could be duplicated"));
        return g_strdup_printf (move ? _("Not everything could be moved to %s") :
                                       _("Not everything could be copied to %s"), p->destination);
    }
    count = g_strdup_printf ("%'ld", n);
    if (p->finished)
    {
        if (p->kind == LIRIA_SDE_TRANSFER_DUPLICATE || p->destination == NULL)
            text = g_strdup_printf (ngettext ("Duplicated %s item", "Duplicated %s items", n), count);
        else
            text = g_strdup_printf (move ? ngettext ("Moved %s item to %s", "Moved %s items to %s", n) :
                                           ngettext ("Copied %s item to %s", "Copied %s items to %s", n),
                                    count, p->destination);
    }
    else if (p->kind == LIRIA_SDE_TRANSFER_DUPLICATE || p->destination == NULL)
    {
        text = n <= 0 ? g_strdup (_("Preparing")) :
                        g_strdup_printf (ngettext ("Duplicating %s item", "Duplicating %s items", n), count);
    }
    else if (n <= 0)
    {
        text = g_strdup_printf (move ? _("Moving to %s") : _("Copying to %s"), p->destination);
    }
    else
    {
        text = g_strdup_printf (move ? ngettext ("Moving %s item to %s", "Moving %s items to %s", n) :
                                       ngettext ("Copying %s item to %s", "Copying %s items to %s", n),
                                count, p->destination);
    }
    g_free (count);
    return text;
}

/* The line under the bar of a copy or a move. */
static char *
format_line (SdeProgress *p)
{
    char *done, *total, *text;

    if (p->failed)
        return g_strdup (p->failure != NULL ? p->failure : _("Something went wrong."));
    if (p->finished)
    {
        if (p->drive != NULL && p->removable)
            return g_strdup_printf (_("The files are on %s \xE2\x80\x94 you can remove it."), p->drive);
        if (p->drive != NULL)
            return g_strdup_printf (_("The files are on %s."), p->drive);
        return g_strdup (_("Done"));
    }
    if (p->finishing)
        return p->drive != NULL ? g_strdup_printf (_("Writing the last data to %s\xE2\x80\xA6"), p->drive)
                                : g_strdup (_("Writing the last data\xE2\x80\xA6"));
    if (p->model.bytes_total <= 0)
        return g_strdup (p->model.files_total > 0 ? "" : _("Preparing\xE2\x80\xA6"));
    done = format_size ((gint64) p->model.bytes_done);
    total = format_size ((gint64) p->model.bytes_total);
    text = g_strdup_printf (_("%s of %s"), done, total);
    g_free (done);
    g_free (total);
    if (p->state != 0)
    {
        char *with = g_strconcat (text, " \xC2\xB7 ", p->state == 2 ? _("Waiting") : _("Paused"), NULL);

        g_free (text);
        return with;
    }
    if (p->model.speed > 0)
    {
        char *speed = format_speed (p->model.speed), *with = g_strconcat (text, " \xC2\xB7 ", speed, NULL);

        g_free (speed);
        g_free (text);
        text = with;
    }
    {
        char *left = format_left (sde_transfer_remaining (&p->model)), *with = g_strconcat (text, " \xC2\xB7 ", left, NULL);

        g_free (left);
        g_free (text);
        text = with;
    }
    return text;
}

static char *
format_note (SdeProgress *p, const char *item)
{
    if (p->finished && !p->failed)
        return g_strdup (p->fat && p->removable ?
                         _("Windows may offer to scan it as it was not ejected; the files are complete.") : "");
    if (p->failed)
        return g_strdup ("");
    if (p->finishing)
        return g_strdup (_("Do not remove the drive yet."));
    return g_strdup (item != NULL ? item : "");
}

static SdeScene
scene_of (SdeProgress *p)
{
    SdeScene scene;
    double now = monotonic_seconds ();

    memset (&scene, 0, sizeof scene);
    scene.kind = deleting (p) ? SDE_SCENE_DELETE : p->kind == LIRIA_SDE_TRANSFER_MOVE ? SDE_SCENE_MOVE
                                                                                         : SDE_SCENE_COPY;
    if (p->failed)
        scene.state = SDE_SCENE_ERROR;
    else if (p->finished)
        scene.state = SDE_SCENE_DONE;
    else if (p->state == 1)
        scene.state = SDE_SCENE_PAUSED;
    else if (p->state == 2)
        scene.state = SDE_SCENE_QUEUED;
    else if (p->finishing)
        scene.state = SDE_SCENE_FINISHING;
    else
        scene.state = SDE_SCENE_RUNNING;
    scene.progress = progress_of (p);
    scene.seconds = now - p->started / (double) G_USEC_PER_SEC;
    scene.state_seconds = now - p->state_changed / (double) G_USEC_PER_SEC;
    scene.animate = p->motion;
    scene.ink = text_rgb (p->scene);
    {
        /* Dark when the text is light. */
        double l = 0.2126 * scene.ink.r + 0.7152 * scene.ink.g + 0.0722 * scene.ink.b;

        scene.dark = l > 0.5;
    }
    return scene;
}

static gboolean
frame_tick (gpointer data)
{
    SdeProgress *p = data;

    gtk_widget_queue_draw (p->scene);
    return G_SOURCE_CONTINUE;
}

static void
animation_update (SdeProgress *p)
{
    SdeScene scene = scene_of (p);
    gboolean want = p->main && p->begun && sde_scene_moving (&scene) && gtk_widget_get_mapped (p->scene) &&
                    !window_iconified;

    if (want && p->frame_id == 0)
    {
        p->frame_id = g_timeout_add (FRAME_MSEC, frame_tick, p);
    }
    else if (!want && p->frame_id != 0)
    {
        g_source_remove (p->frame_id);
        p->frame_id = 0;
        gtk_widget_queue_draw (p->scene);
    }
}

static gboolean
scene_draw (GtkWidget *widget, cairo_t *cr, SdeProgress *p)
{
    SdeScene scene = scene_of (p);

    sde_scene_draw (cr, 0, 0, gtk_widget_get_allocated_width (widget), gtk_widget_get_allocated_height (widget),
                    &scene);
    return FALSE;
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

/* The first box of the window is drawn in full, the others as the queue;
 * |others|: the operations after the first, for the heading of the second
 * box (-1 for the third and later). */
static gboolean
is_first (SdeProgress *p, int *others)
{
    GtkWidget *parent = gtk_widget_get_parent (p->box);
    GList *children;
    gboolean first;

    *others = 0;
    if (parent == NULL || !GTK_IS_CONTAINER (parent))
        return TRUE;
    children = gtk_container_get_children (GTK_CONTAINER (parent));
    first = children == NULL || children->data == p->box;
    if (!first)
        *others = children->next != NULL && children->next->data == p->box ? (int) g_list_length (children) - 1 : -1;
    g_list_free (children);
    return first;
}

static void
sync_layout (SdeProgress *p)
{
    int others = 0;
    gboolean first = is_first (p, &others);
    gboolean ours = transfer (p);
    gboolean expanded = first && ours && p->details_shown;

    if (first != p->main)
    {
        p->main = first;
        animation_update (p);
    }
    /* The picture for copies, moves and deletions, in the first box only. */
    gtk_widget_set_visible (p->scene, first && p->begun);
    gtk_widget_set_visible (p->queue_heading, others > 0);
    if (others > 0)
    {
        char *text = g_strdup_printf (ngettext ("%d more operation", "%d more operations", others), others);

        set_text (p->queue_heading, text);
        g_free (text);
    }
    /* A copy's own texts, or the operation's. */
    gtk_widget_set_visible (p->headline, ours);
    gtk_widget_set_visible (p->status, !ours);
    gtk_widget_set_visible (p->bar, ours && p->counted && !p->finishing);
    gtk_widget_set_visible (p->own_bar, !ours || !p->counted || p->finishing);
    gtk_widget_set_visible (p->line, ours);
    gtk_widget_set_visible (p->details, !ours);
    gtk_widget_set_visible (p->note, first && (ours || p->finished));
    gtk_widget_set_visible (p->toggle, first && ours && p->counted);
    gtk_widget_set_visible (p->rows, expanded);
    gtk_widget_set_visible (p->pause_button, !p->finished);
    gtk_widget_set_visible (p->queue_button, !p->finished);
    if (p->arrow_up != p->details_shown)
    {
        p->arrow_up = p->details_shown;
        gtk_image_set_from_icon_name (GTK_IMAGE (p->toggle_arrow),
                                      p->details_shown ? "pan-up-symbolic" : "pan-down-symbolic",
                                      GTK_ICON_SIZE_BUTTON);
    }
}

/* LIRIA_SDE_QA_PROGRESS: what the window shows, for tests. */
static void
qa_write (SdeProgress *p, const char *percent)
{
    const char *path = g_getenv ("LIRIA_SDE_QA_PROGRESS");
    GtkWidget *top = gtk_widget_get_toplevel (p->box);
    GString *out;

    if (path == NULL || *path == '\0' || !p->main)
        return;
    out = g_string_new (NULL);
    g_string_append_printf (out, "headline=%s\n",
                            gtk_label_get_text (GTK_LABEL (transfer (p) ? p->headline : p->status)));
    g_string_append_printf (out, "status=%s\nnote=%s\npercent=%s\n",
                            gtk_label_get_text (GTK_LABEL (transfer (p) ? p->line : p->details)),
                            gtk_label_get_text (GTK_LABEL (p->note)), percent);
    g_string_append_printf (out, "state=%d\npaused=%d\nanimating=%d\ndetails=%d\nfinishing=%d\nreduced=%d\n",
                            p->state, p->state != 0, p->frame_id != 0, gtk_widget_get_visible (p->rows),
                            p->finishing, !p->motion);
    g_string_append_printf (out, "finished=%d\nfailed=%d\ndrive=%s\nkind=%d\n", p->finished, p->failed,
                            p->drive != NULL ? p->drive : "", (int) p->kind);
    g_string_append_printf (out, "bytes=%.0f/%.0f\nitems_done=%ld/%ld\n", p->model.bytes_done, p->model.bytes_total,
                            p->model.files_done, p->model.files_total);
    if (GTK_IS_WINDOW (top))
    {
        GtkWidget *parent = gtk_widget_get_parent (p->box);
        GList *children = parent != NULL ? gtk_container_get_children (GTK_CONTAINER (parent)) : NULL;

        g_string_append_printf (out, "title=%s\ncount=%u\n", gtk_window_get_title (GTK_WINDOW (top)),
                                g_list_length (children));
        g_list_free (children);
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
    qa_rect (out, "scene", p->scene);
    g_file_set_contents (path, out->str, out->len, NULL);
    g_string_free (out, TRUE);
}

static void
update_title (SdeProgress *p, const char *percent)
{
    GtkWidget *top = gtk_widget_get_toplevel (p->box);
    const char *headline;
    char *title;

    if (!GTK_IS_WINDOW (top) || !p->main)
        return;
    headline = gtk_label_get_text (GTK_LABEL (transfer (p) ? p->headline : p->status));
    if (headline == NULL || *headline == '\0')
        title = g_strdup (_("File Operations"));
    else if (*percent != '\0' && !p->finished)
        title = g_strdup_printf ("%s \xC2\xB7 %s", percent, headline);
    else
        title = g_strdup (headline);
    if (g_strcmp0 (gtk_window_get_title (GTK_WINDOW (top)), title) != 0)
        gtk_window_set_title (GTK_WINDOW (top), title);
    g_free (title);
}

static void
progress_refresh (SdeProgress *p)
{
    SdeReport *report;
    SdeReport now_report = { 0 };
    gboolean begun = FALSE;
    double progress;
    char *text, *percent;

    g_mutex_lock (&report_lock);
    report = report_get (p->info, FALSE);
    if (report != NULL)
    {
        begun = TRUE;
        now_report = *report;
        now_report.source = g_strdup (report->source);
        now_report.destination = g_strdup (report->destination);
        now_report.item = g_strdup (report->item);
        now_report.drive = g_strdup (report->drive);
        now_report.failure = g_strdup (report->failure);
    }
    g_mutex_unlock (&report_lock);

    if (begun && !p->finished)
    {
        if (!p->begun)
        {
            p->begun = TRUE;
            p->kind = now_report.kind;
            p->source = g_strdup (now_report.source);
            p->destination = g_strdup (now_report.destination);
        }
        if (now_report.drive != NULL && p->drive == NULL)
        {
            p->drive = g_strdup (now_report.drive);
            p->removable = now_report.removable;
            p->fat = now_report.fat;
        }
        if (now_report.failure != NULL && p->failure == NULL)
            p->failure = g_strdup (now_report.failure);
        if (transfer (p))
        {
            if (now_report.files_total > 0 && !p->counted)
            {
                /* The speed and the time count from the first counted item,
                 * not from the start of the scan. */
                sde_transfer_init (&p->model, monotonic_seconds ());
                if (p->state != 0)
                    sde_transfer_set_paused (&p->model, p->model.now, 1);
            }
            sde_transfer_update (&p->model, MAX (monotonic_seconds (), p->model.now),
                                 (double) now_report.bytes_done, (double) now_report.bytes_total,
                                 now_report.files_done, now_report.files_total);
            p->counted = now_report.files_total > 0;
        }
        else
        {
            p->model.files_total = now_report.files_total;
        }
        if (now_report.finishing && !p->finishing)
        {
            p->finishing = TRUE;
            p->state_changed = g_get_monotonic_time ();
            /* The last data must reach the drive: nothing to pause. */
            gtk_widget_set_sensitive (p->pause_button, FALSE);
            gtk_widget_set_sensitive (p->queue_button, FALSE);
        }
    }
    progress = progress_of (p);
    if (transfer (p))
    {
        text = format_headline (p);
        set_text (p->headline, text);
        g_free (text);
        text = format_line (p);
        set_text (p->line, text);
        g_free (text);
        if (p->counted && progress >= 0)
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->bar), p->finished ? 1.0 : progress);
    }
    text = format_note (p, now_report.item);
    set_text (p->note, text);
    g_free (text);
    if (p->details_shown)
    {
        char *done = g_strdup_printf ("%'ld", p->model.files_done), *total = g_strdup_printf ("%'ld", p->model.files_total);

        set_text (p->values[0], p->source != NULL ? p->source : "\xE2\x80\x94");
        if (p->drive != NULL && p->destination != NULL)
            text = g_strdup_printf (_("%s on %s"), p->destination, p->drive);
        else
            text = g_strdup (p->destination != NULL ? p->destination : "\xE2\x80\x94");
        set_text (p->values[1], text);
        g_free (text);
        text = p->model.files_total > 0 ? g_strdup_printf (_("%s of %s"), done, total) : g_strdup ("\xE2\x80\x94");
        set_text (p->values[2], text);
        g_free (text);
        g_free (done);
        g_free (total);
    }

    percent = progress >= 0 && (p->begun || progress > 0) ? g_strdup_printf ("%d%%", (int) floor (progress * 100.0))
                                                          : g_strdup ("");
    sync_layout (p);
    if (p->main && p->begun && p->frame_id == 0)
        gtk_widget_queue_draw (p->scene); /* a still picture follows the progress too */
    animation_update (p);
    update_title (p, percent);
    qa_write (p, percent);
    g_free (percent);
    g_free (now_report.source);
    g_free (now_report.destination);
    g_free (now_report.item);
    g_free (now_report.drive);
    g_free (now_report.failure);
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
    int others = 0;

    if (!is_first (p, &others))
    {
        SdeSceneRgb fg = text_rgb (box);
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
        if (qa != NULL && *qa != '\0' && p->main)
            g_file_set_contents (qa, "done=1\nfinished=1\n", -1, NULL);
    }
    if (p->frame_id != 0)
        g_source_remove (p->frame_id);
    if (p->done_id != 0)
        g_source_remove (p->done_id);
    p->refresh_id = 0;
    p->frame_id = 0;
    p->done_id = 0;
    all_progress = g_list_remove (all_progress, p);
}

static void
progress_free (gpointer data)
{
    SdeProgress *p = data;

    progress_destroyed (NULL, p);
    g_free (p->source);
    g_free (p->destination);
    g_free (p->drive);
    g_free (p->failure);
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

/* On a small screen (1024x600 at 150 %) the picture is smaller, so the
 * window fits. */
static void
mapped (SdeProgress *p)
{
    GdkDisplay *display = gtk_widget_get_display (p->scene);
    GdkWindow *window = gtk_widget_get_window (p->scene);
    GdkMonitor *monitor = window != NULL ? gdk_display_get_monitor_at_window (display, window) : NULL;
    int height = sde_px (104);

    if (monitor != NULL)
    {
        GdkRectangle area;

        gdk_monitor_get_workarea (monitor, &area);
        if (area.height < sde_px (560))
            height = sde_px (72);
    }
    gtk_widget_set_size_request (p->scene, -1, height);
    animation_update (p);
}

void
liria_sde_progress_build (GtkWidget *box, LiriaProgressInfo *info, GtkWidget *start_button,
                          GtkWidget *queue_button, GtkWidget *cancel_button, GtkWidget **status,
                          GtkWidget **progress_bar, GtkWidget **details)
{
    static const char *const row_names[] = { N_("From"), N_("To"), N_("Items") };
    SdeProgress *p = g_new0 (SdeProgress, 1);
    GtkWidget *inner, *header, *buttons, *content, *texts;
    int i;

    p->box = box;
    p->info = info;
    p->started = p->state_changed = g_get_monotonic_time ();
    p->motion = !reduced_motion ();
    p->details_shown = details_default;
    p->main = TRUE;
    sde_transfer_init (&p->model, monotonic_seconds ());
    all_progress = g_list_prepend (all_progress, p);
    g_object_set_data_full (G_OBJECT (box), PROGRESS_KEY, p, progress_free);
    g_signal_connect (box, "destroy", G_CALLBACK (progress_destroyed), p);
    g_signal_connect (box, "draw", G_CALLBACK (box_draw), p);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "sde-progress");
    gtk_widget_set_size_request (box, sde_px (500), -1);
    gtk_box_set_spacing (GTK_BOX (box), 0);
    /* The content, inside a margin; the line between operations is drawn
     * on the operation's box (box_draw ()). */
    inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, sde_px (6));
    gtk_container_set_border_width (GTK_CONTAINER (inner), sde_px (16));
    gtk_box_pack_start (GTK_BOX (box), inner, TRUE, TRUE, 0);

    /* "2 more operations" over the queue. */
    p->queue_heading = managed (line_label (PANGO_ELLIPSIZE_END));
    gtk_style_context_add_class (gtk_widget_get_style_context (p->queue_heading), "dim-label");
    label_scale (p->queue_heading, 0.92, TRUE);
    gtk_box_pack_start (GTK_BOX (inner), p->queue_heading, FALSE, FALSE, 0);

    /* The picture. */
    p->scene = managed (gtk_drawing_area_new ());
    gtk_widget_set_size_request (p->scene, -1, sde_px (104));
    gtk_style_context_add_class (gtk_widget_get_style_context (p->scene), "sde-progress-scene");
    g_signal_connect (p->scene, "draw", G_CALLBACK (scene_draw), p);
    g_signal_connect_swapped (p->scene, "map", G_CALLBACK (mapped), p);
    g_signal_connect_swapped (p->scene, "unmap", G_CALLBACK (animation_update), p);
    gtk_box_pack_start (GTK_BOX (inner), p->scene, FALSE, FALSE, 0);

    /* The headline and the buttons. */
    header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, sde_px (12));
    texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
    p->status = managed (line_label (PANGO_ELLIPSIZE_END));
    p->headline = managed (line_label (PANGO_ELLIPSIZE_END));
    label_scale (p->status, 1.08, TRUE);
    label_scale (p->headline, 1.08, TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (p->headline), "sde-progress-headline");
    gtk_box_pack_start (GTK_BOX (texts), p->status, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (texts), p->headline, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (header), texts, TRUE, TRUE, 0);
    buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, sde_px (4));
    gtk_widget_set_valign (buttons, GTK_ALIGN_CENTER);
    p->pause_button = start_button;
    p->queue_button = queue_button;
    p->cancel_button = cancel_button;
    action_button (start_button, buttons);
    action_button (queue_button, buttons);
    action_button (cancel_button, buttons);
    gtk_widget_show_all (queue_button);
    gtk_box_pack_start (GTK_BOX (header), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (inner), header, FALSE, FALSE, 0);

    /* The bars: the operation's own one and a copy's. */
    p->own_bar = managed (gtk_progress_bar_new ());
    p->bar = managed (gtk_progress_bar_new ());
    gtk_style_context_add_class (gtk_widget_get_style_context (p->own_bar), "sde-progress-bar");
    gtk_style_context_add_class (gtk_widget_get_style_context (p->bar), "sde-progress-bar");
    gtk_box_pack_start (GTK_BOX (inner), p->own_bar, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (inner), p->bar, FALSE, FALSE, 0);

    /* "780 MB of 1.2 GB · 22 MB/s · About 40 seconds left", or the
     * operation's own details; the item being copied. */
    p->line = managed (line_label (PANGO_ELLIPSIZE_END));
    gtk_box_pack_start (GTK_BOX (inner), p->line, FALSE, FALSE, 0);
    p->details = managed (gtk_label_new (NULL));
    gtk_label_set_xalign (GTK_LABEL (p->details), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (p->details), TRUE);
    gtk_label_set_line_wrap_mode (GTK_LABEL (p->details), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars (GTK_LABEL (p->details), 1);
    gtk_box_pack_start (GTK_BOX (inner), p->details, FALSE, FALSE, 0);
    p->note = managed (line_label (PANGO_ELLIPSIZE_MIDDLE));
    gtk_style_context_add_class (gtk_widget_get_style_context (p->note), "dim-label");
    label_scale (p->note, 0.92, FALSE);
    gtk_box_pack_start (GTK_BOX (inner), p->note, FALSE, FALSE, 0);

    /* Details: from, to, items. */
    p->toggle = managed (gtk_button_new ());
    gtk_style_context_add_class (gtk_widget_get_style_context (p->toggle), "flat");
    gtk_style_context_add_class (gtk_widget_get_style_context (p->toggle), "sde-progress-toggle");
    gtk_widget_set_halign (p->toggle, GTK_ALIGN_START);
    content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, sde_px (6));
    p->toggle_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_box_pack_start (GTK_BOX (content), p->toggle_arrow, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content), gtk_label_new (_("Details")), FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (p->toggle), content);
    gtk_widget_show_all (content);
    g_signal_connect (p->toggle, "clicked", G_CALLBACK (toggle_clicked), p);
    gtk_box_pack_start (GTK_BOX (inner), p->toggle, FALSE, FALSE, 0);

    p->rows = managed (gtk_grid_new ());
    gtk_grid_set_column_spacing (GTK_GRID (p->rows), sde_px (14));
    gtk_grid_set_row_spacing (GTK_GRID (p->rows), sde_px (4));
    gtk_widget_set_margin_start (p->rows, sde_px (4));
    for (i = 0; i < 3; i++)
    {
        GtkWidget *name = gtk_label_new (_(row_names[i]));

        gtk_label_set_xalign (GTK_LABEL (name), 0.0);
        gtk_style_context_add_class (gtk_widget_get_style_context (name), "dim-label");
        p->values[i] = line_label (PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_text (GTK_LABEL (p->values[i]), "\xE2\x80\x94");
        gtk_grid_attach (GTK_GRID (p->rows), name, 0, i, 1, 1);
        gtk_grid_attach (GTK_GRID (p->rows), p->values[i], 1, i, 1, 1);
        gtk_widget_show (name);
        gtk_widget_show (p->values[i]);
    }
    gtk_box_pack_start (GTK_BOX (inner), p->rows, FALSE, FALSE, 0);

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

    if (p == NULL || p->state == state || p->finished)
        return;
    sde_transfer_set_paused (&p->model, MAX (monotonic_seconds (), p->model.now), state != 0);
    p->state = state;
    p->state_changed = g_get_monotonic_time ();
    context = gtk_widget_get_style_context (box);
    if (state != 0)
        gtk_style_context_add_class (context, "sde-paused");
    else
        gtk_style_context_remove_class (context, "sde-paused");
    progress_refresh (p);
}

static gboolean
done_timeout (gpointer data)
{
    SdeProgress *p = data;

    p->done_id = 0;
    if (p->remove != NULL)
        p->remove (p->remove_data);
    return G_SOURCE_REMOVE;
}

gboolean
liria_sde_progress_finish (GtkWidget *box, GSourceFunc remove, gpointer data)
{
    SdeProgress *p = g_object_get_data (G_OBJECT (box), PROGRESS_KEY);
    GtkWidget *top = gtk_widget_get_toplevel (box);
    GCancellable *cancellable;
    gboolean cancelled;

    if (p == NULL)
        return FALSE;
    progress_refresh (p);
    cancellable = liria_progress_info_get_cancellable (p->info);
    cancelled = g_cancellable_is_cancelled (cancellable);
    g_object_unref (cancellable);
    /* Only what the user could watch says that it is over. */
    if (cancelled || !transfer (p) || !GTK_IS_WINDOW (top) || !gtk_widget_get_visible (top))
        return FALSE;
    p->finished = TRUE;
    p->failed = p->failure != NULL;
    p->finishing = FALSE;
    p->state = 0;
    p->state_changed = g_get_monotonic_time ();
    p->remove = remove;
    p->remove_data = data;
    gtk_style_context_remove_class (gtk_widget_get_style_context (box), "sde-paused");
    gtk_style_context_add_class (gtk_widget_get_style_context (box), p->failed ? "sde-failed" : "sde-done");
    gtk_widget_set_sensitive (p->cancel_button, TRUE);
    gtk_widget_set_tooltip_text (p->cancel_button, _("Close"));
    if (!p->failed)
        p->done_id = g_timeout_add (p->drive != NULL ? DONE_DRIVE_MSEC : DONE_MSEC, done_timeout, p);
    progress_refresh (p);
    return TRUE;
}

gboolean
liria_sde_progress_close (GtkWidget *box)
{
    SdeProgress *p = g_object_get_data (G_OBJECT (box), PROGRESS_KEY);

    if (p == NULL || !p->finished || p->remove == NULL)
        return FALSE;
    if (p->done_id != 0)
        g_source_remove (p->done_id);
    p->done_id = g_idle_add (done_timeout, p);
    return TRUE;
}

gboolean
liria_sde_progress_is_finished (GtkWidget *box)
{
    SdeProgress *p = g_object_get_data (G_OBJECT (box), PROGRESS_KEY);

    return p != NULL && p->finished;
}

/* Minimized: no picture moves. */
static gboolean
window_state_changed (GtkWidget *window, GdkEventWindowState *event, gpointer data)
{
    GList *l;

    window_iconified = (event->new_window_state & GDK_WINDOW_STATE_ICONIFIED) != 0;
    for (l = all_progress; l != NULL; l = l->next)
        animation_update (l->data);
    return FALSE;
}

void
liria_sde_progress_window_setup (GtkWidget *window)
{
    GtkWidget *child = gtk_bin_get_child (GTK_BIN (window));

    gtk_container_set_border_width (GTK_CONTAINER (window), 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (window), "sde-progress-window");
    if (GTK_IS_BOX (child))
        gtk_box_set_spacing (GTK_BOX (child), 0);
    g_signal_connect (window, "window-state-event", G_CALLBACK (window_state_changed), NULL);
}
