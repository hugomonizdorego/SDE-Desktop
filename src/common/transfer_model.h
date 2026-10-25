/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * The copy and move progress of SDE (0.2.0 Alpha 2): the model and the
 * cairo drawing shared by the SDE-FLTK dialog (src/common/transfer_dialog.*,
 * used by the desktop and other SDE programs) and Liria's file operations
 * window (suite/redesign.py copies this file into the Liria tree as
 * libliria-private/liria-sde-transfer-model.h), so both look and estimate
 * the same. Plain C99 that also compiles as C++; licensed GPL-2.0-or-later
 * like Liria, so the file can be part of both.
 *
 *   SdeTransfer   bytes and items done and in total over time: the current
 *                 speed (smoothed over about 2 s), the time remaining (the
 *                 average and the current speed together, so it neither
 *                 jumps nor lags), and the speed at each 1 % of the
 *                 progress for the graph. Paused time does not count.
 *   sde_transfer_eta_parts ()
 *                 the remaining time in the words a person uses: "a few
 *                 seconds", about N seconds (5 s steps), minutes, hours.
 *   sde_transfer_draw_graph ()
 *                 the speed graph: the part done tinted, the speed as a
 *                 filled area along the progress, a line at the current
 *                 speed (the caller writes "Speed: ..." over it).
 *   sde_transfer_draw_logo ()
 *                 the SDE mark while it works: a bright pulse runs along
 *                 its red, green and blue stripes from left to right, one
 *                 after the other, like data on its way. At rest (reduced
 *                 motion, paused) the mark is drawn as it is.
 */
#ifndef SDE_TRANSFER_MODEL_H
#define SDE_TRANSFER_MODEL_H

#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDE_TRANSFER_GRAPH_POINTS 100 /* one speed sample per 1 % of the progress */
#define SDE_TRANSFER_RATE_SAMPLES 32  /* recent (time, bytes) pairs for the speed */
#define SDE_TRANSFER_RATE_SPAN 2.0    /* seconds the current speed looks back */
#define SDE_TRANSFER_PULSE_PERIOD 1.6 /* seconds for a pulse to cross the mark */

typedef struct {
    double start;          /* time of the first update (seconds, monotonic) */
    double now;            /* time of the last update */
    double paused_since;   /* < 0 while running */
    double paused_total;   /* seconds spent paused */
    double bytes_done, bytes_total;
    long files_done, files_total;
    double speed;          /* bytes per second, smoothed; 0 while unknown */
    double ring_time[SDE_TRANSFER_RATE_SAMPLES], ring_bytes[SDE_TRANSFER_RATE_SAMPLES];
    int ring_count, ring_next;
    float graph[SDE_TRANSFER_GRAPH_POINTS]; /* speed at each 1 %, < 0 not reached */
    int graph_last;        /* last point filled, -1 for none */
    double peak;           /* highest speed in the graph */
} SdeTransfer;

static inline void
sde_transfer_init (SdeTransfer *t, double now)
{
    int i;

    memset (t, 0, sizeof (*t));
    t->start = t->now = now;
    t->paused_since = -1.0;
    t->graph_last = -1;
    for (i = 0; i < SDE_TRANSFER_GRAPH_POINTS; i++)
        t->graph[i] = -1.0f;
}

/* Seconds of work so far: wall time without the pauses. */
static inline double
sde_transfer_active_time (const SdeTransfer *t)
{
    double paused = t->paused_total + (t->paused_since >= 0.0 ? t->now - t->paused_since : 0.0);
    double active = t->now - t->start - paused;

    return active > 0.0 ? active : 0.0;
}

/* Progress from 0 to 1, or -1 while the total is not known. */
static inline double
sde_transfer_progress (const SdeTransfer *t)
{
    double p;

    if (t->bytes_total > 0.0)
        p = t->bytes_done / t->bytes_total;
    else if (t->files_total > 0)
        p = (double) t->files_done / (double) t->files_total;
    else
        return -1.0;
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}

static inline void
sde_transfer_set_paused (SdeTransfer *t, double now, int paused)
{
    t->now = now;
    if (paused && t->paused_since < 0.0)
    {
        t->paused_since = now;
    }
    else if (!paused && t->paused_since >= 0.0)
    {
        t->paused_total += now - t->paused_since;
        t->paused_since = -1.0;
        /* The speed starts again from the next samples. */
        t->ring_count = 0;
    }
}

/* Records the state at |now|; call it a few times a second. */
static inline void
sde_transfer_update (SdeTransfer *t, double now, double bytes_done, double bytes_total, long files_done,
                     long files_total)
{
    double active, progress;
    int i, oldest, point;

    t->now = now;
    t->bytes_done = bytes_done > 0.0 ? bytes_done : 0.0;
    t->bytes_total = bytes_total > 0.0 ? bytes_total : 0.0;
    if (t->bytes_total > 0.0 && t->bytes_done > t->bytes_total)
        t->bytes_total = t->bytes_done;
    t->files_done = files_done;
    t->files_total = files_total;
    if (t->paused_since >= 0.0)
        return;

    /* The current speed: bytes over the last SDE_TRANSFER_RATE_SPAN
     * seconds of work, smoothed so the number can be read. */
    active = sde_transfer_active_time (t);
    t->ring_time[t->ring_next] = active;
    t->ring_bytes[t->ring_next] = t->bytes_done;
    t->ring_next = (t->ring_next + 1) % SDE_TRANSFER_RATE_SAMPLES;
    if (t->ring_count < SDE_TRANSFER_RATE_SAMPLES)
        t->ring_count++;
    oldest = (t->ring_next - t->ring_count + SDE_TRANSFER_RATE_SAMPLES) % SDE_TRANSFER_RATE_SAMPLES;
    for (i = 0; i < t->ring_count - 1; i++)
    {
        int next = (oldest + 1) % SDE_TRANSFER_RATE_SAMPLES;

        if (active - t->ring_time[next] < SDE_TRANSFER_RATE_SPAN)
            break;
        oldest = next;
    }
    if (active - t->ring_time[oldest] >= 0.25)
    {
        double rate = (t->bytes_done - t->ring_bytes[oldest]) / (active - t->ring_time[oldest]);

        if (rate < 0.0)
            rate = 0.0;
        t->speed = t->speed > 0.0 ? t->speed + (rate - t->speed) * 0.35 : rate;
    }

    /* The graph: the current speed at every 1 % reached since the last
     * update. */
    progress = sde_transfer_progress (t);
    if (progress >= 0.0 && t->speed > 0.0)
    {
        point = (int) (progress * SDE_TRANSFER_GRAPH_POINTS);
        if (point >= SDE_TRANSFER_GRAPH_POINTS)
            point = SDE_TRANSFER_GRAPH_POINTS - 1;
        for (i = t->graph_last + 1; i <= point; i++)
            t->graph[i] = (float) t->speed;
        if (point >= 0)
            t->graph[point] = (float) t->speed;
        if (point > t->graph_last)
            t->graph_last = point;
        if (t->speed > t->peak)
            t->peak = t->speed;
    }
}

/* Seconds left, or -1 while they cannot be known yet ("Calculating..."):
 * the first second of work, or nothing copied yet. The average speed so
 * far and the current speed weigh the same, so a short stall neither makes
 * the estimate jump nor hides a lasting change. */
static inline double
sde_transfer_remaining (const SdeTransfer *t)
{
    double active = sde_transfer_active_time (t);
    double average, rate;

    if (t->bytes_total <= 0.0 || active < 1.0 || t->bytes_done <= 0.0)
        return -1.0;
    if (t->bytes_done >= t->bytes_total)
        return 0.0;
    average = t->bytes_done / active;
    rate = t->speed > 0.0 ? (average + t->speed) / 2.0 : average;
    if (rate <= 0.0)
        return -1.0;
    return (t->bytes_total - t->bytes_done) / rate;
}

typedef enum {
    SDE_TRANSFER_ETA_UNKNOWN,     /* "Calculating..." */
    SDE_TRANSFER_ETA_FEW_SECONDS, /* under 5 s */
    SDE_TRANSFER_ETA_SECONDS,     /* value seconds, a multiple of 5 */
    SDE_TRANSFER_ETA_MINUTES,     /* value minutes */
    SDE_TRANSFER_ETA_HOURS        /* value hours and minutes minutes (multiple of 5) */
} SdeTransferEta;

static inline SdeTransferEta
sde_transfer_eta_parts (double seconds, int *value, int *minutes)
{
    *value = 0;
    *minutes = 0;
    if (seconds < 0.0)
        return SDE_TRANSFER_ETA_UNKNOWN;
    if (seconds < 5.0)
        return SDE_TRANSFER_ETA_FEW_SECONDS;
    if (seconds < 55.0)
    {
        *value = (int) (ceil (seconds / 5.0) * 5.0);
        return SDE_TRANSFER_ETA_SECONDS;
    }
    if (seconds < 59.5 * 60.0)
    {
        *value = (int) floor (seconds / 60.0 + 0.5);
        if (*value < 1)
            *value = 1;
        return SDE_TRANSFER_ETA_MINUTES;
    }
    {
        long rounded = (long) floor (seconds / 300.0 + 0.5) * 5; /* minutes, 5 minute steps */

        *value = (int) (rounded / 60);
        *minutes = (int) (rounded % 60);
        return SDE_TRANSFER_ETA_HOURS;
    }
}

/* ----------------------------------------------------------------- drawing */

typedef struct {
    double r, g, b;
} SdeTransferRgb;

/* The speed graph in (x, y, w, h). Returns the y of the current speed
 * line (for its label), or -1 when there is no speed yet. */
static inline double
sde_transfer_draw_graph (cairo_t *cr, double x, double y, double w, double h, const SdeTransfer *t,
                         SdeTransferRgb accent, SdeTransferRgb fg)
{
    double progress = sde_transfer_progress (t);
    double top = t->peak > 0.0 ? t->peak * 1.25 : 1.0;
    double line_y = -1.0;
    int i, first = -1, last = -1;

    cairo_save (cr);
    /* Frame and the part done. */
    cairo_rectangle (cr, x, y, w, h);
    cairo_set_source_rgba (cr, fg.r, fg.g, fg.b, 0.035);
    cairo_fill (cr);
    if (progress > 0.0)
    {
        cairo_rectangle (cr, x, y, w * progress, h);
        cairo_set_source_rgba (cr, accent.r, accent.g, accent.b, 0.10);
        cairo_fill (cr);
    }
    /* Faint grid lines every quarter. */
    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, fg.r, fg.g, fg.b, 0.07);
    for (i = 1; i < 4; i++)
    {
        cairo_move_to (cr, floor (x + w * i / 4.0) + 0.5, y);
        cairo_line_to (cr, floor (x + w * i / 4.0) + 0.5, y + h);
    }
    cairo_stroke (cr);
    /* The speed along the progress. */
    for (i = 0; i < SDE_TRANSFER_GRAPH_POINTS; i++)
    {
        if (t->graph[i] >= 0.0f)
        {
            if (first < 0)
                first = i;
            last = i;
        }
    }
    if (first >= 0)
    {
        double step = w / SDE_TRANSFER_GRAPH_POINTS;
        double end = progress > 0.0 ? x + w * progress : x + step * (last + 1);

        cairo_move_to (cr, x + step * first, y + h);
        for (i = first; i <= last; i++)
        {
            double v = t->graph[i] >= 0.0f ? t->graph[i] : 0.0;
            double px = x + step * (i + 0.5);

            if (i == first)
                cairo_line_to (cr, x + step * first, y + h - h * v / top);
            cairo_line_to (cr, px > end ? end : px, y + h - h * v / top);
        }
        cairo_line_to (cr, end, y + h - h * (t->graph[last] >= 0.0f ? t->graph[last] : 0.0) / top);
        cairo_line_to (cr, end, y + h);
        cairo_close_path (cr);
        cairo_set_source_rgba (cr, accent.r, accent.g, accent.b, 0.38);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, accent.r, accent.g, accent.b, 0.95);
        cairo_set_line_width (cr, 1.5);
        cairo_stroke (cr);
    }
    /* A line across at the current speed. */
    if (t->speed > 0.0)
    {
        line_y = floor (y + h - h * t->speed / top) + 0.5;
        cairo_set_source_rgba (cr, fg.r, fg.g, fg.b, 0.65);
        cairo_set_line_width (cr, 1.0);
        cairo_move_to (cr, x, line_y);
        cairo_line_to (cr, x + w, line_y);
        cairo_stroke (cr);
    }
    cairo_rectangle (cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
    cairo_set_source_rgba (cr, fg.r, fg.g, fg.b, 0.18);
    cairo_stroke (cr);
    cairo_restore (cr);
    return line_y;
}

/* The mark in its layers, all |size| x |size| image surfaces: what lies
 * under the stripes (the monitor and its screen), each stripe alone (red,
 * green, blue) and what lies over them (the portrait's ink, the LED). */
typedef struct {
    cairo_surface_t *under;
    cairo_surface_t *stripe[3];
    cairo_surface_t *over;
    int size;
} SdeTransferLogo;

/* Where the pulse of |stripe| is at |seconds|, from 0 (left edge of the
 * mark) to 1 (right edge); it starts and ends just outside. */
static inline double
sde_transfer_pulse_position (double seconds, int stripe)
{
    double phase = fmod (seconds / SDE_TRANSFER_PULSE_PERIOD - stripe * 0.16, 1.0);

    if (phase < 0.0)
        phase += 1.0;
    /* Ease in and out a little: the pulse leaves slowly and arrives fast. */
    phase = phase * phase * (3.0 - 2.0 * phase);
    return -0.25 + phase * 1.5;
}

/* Draws the mark at (x, y). |animate| false draws it at rest. */
static inline void
sde_transfer_draw_logo (cairo_t *cr, const SdeTransferLogo *logo, double x, double y, double seconds, int animate)
{
    int i;

    if (logo == NULL || logo->under == NULL)
        return;
    cairo_save (cr);
    cairo_set_source_surface (cr, logo->under, x, y);
    cairo_paint (cr);
    for (i = 0; i < 3; i++)
    {
        if (logo->stripe[i] == NULL)
            continue;
        if (!animate)
        {
            cairo_set_source_surface (cr, logo->stripe[i], x, y);
            cairo_paint (cr);
            continue;
        }
        {
            double size = logo->size;
            double centre = x + size * sde_transfer_pulse_position (seconds, i);
            double half = size * 0.30;
            cairo_pattern_t *mask = cairo_pattern_create_linear (centre - half, 0, centre + half, 0);
            cairo_pattern_t *core = cairo_pattern_create_linear (centre - half * 0.45, 0, centre + half * 0.45, 0);

            /* The stripe stays, dimmed; the pulse brings it to full colour
             * and adds a little white light at its core. */
            cairo_set_source_surface (cr, logo->stripe[i], x, y);
            cairo_paint_with_alpha (cr, 0.30);
            cairo_pattern_add_color_stop_rgba (mask, 0.0, 0, 0, 0, 0.0);
            cairo_pattern_add_color_stop_rgba (mask, 0.5, 0, 0, 0, 1.0);
            cairo_pattern_add_color_stop_rgba (mask, 1.0, 0, 0, 0, 0.0);
            cairo_set_source_surface (cr, logo->stripe[i], x, y);
            cairo_mask (cr, mask);
            cairo_push_group (cr);
            cairo_set_source_rgba (cr, 1, 1, 1, 0.55);
            cairo_mask_surface (cr, logo->stripe[i], x, y);
            cairo_pop_group_to_source (cr);
            cairo_pattern_add_color_stop_rgba (core, 0.0, 0, 0, 0, 0.0);
            cairo_pattern_add_color_stop_rgba (core, 0.5, 0, 0, 0, 1.0);
            cairo_pattern_add_color_stop_rgba (core, 1.0, 0, 0, 0, 0.0);
            cairo_mask (cr, core);
            cairo_pattern_destroy (core);
            cairo_pattern_destroy (mask);
        }
    }
    if (logo->over != NULL)
    {
        cairo_set_source_surface (cr, logo->over, x, y);
        cairo_paint (cr);
    }
    cairo_restore (cr);
}

/* The SVG of the mark (data/branding/sde-mark.svg) split into the layers
 * above: |layer| 0 keeps what is under the stripes, 1 to 3 one stripe
 * (red, green, blue), 4 what is over them. Returns a newly allocated
 * string (free ()), or NULL when the SVG has no stripes. The mark lists
 * its paths bottom to top with the ids "stripe-red", "stripe-green" and
 * "stripe-blue" (tools/make-sde-logo.py). */
static inline char *
sde_transfer_logo_layer_svg (const char *svg, int layer)
{
    static const char *const ids[] = { "<path id=\"stripe-red\"", "<path id=\"stripe-green\"",
                                       "<path id=\"stripe-blue\"" };
    const char *starts[3], *ends[3];
    const char *close_tag;
    size_t length = strlen (svg), header;
    char *out, *p;
    int i;

    for (i = 0; i < 3; i++)
    {
        starts[i] = strstr (svg, ids[i]);
        if (starts[i] == NULL)
            return NULL;
        ends[i] = strstr (starts[i], "/>");
        if (ends[i] == NULL)
            return NULL;
        ends[i] += 2;
    }
    if (!(starts[0] < starts[1] && starts[1] < starts[2]))
        return NULL;
    close_tag = strstr (ends[2], "</svg>");
    if (close_tag == NULL)
        return NULL;
    /* The header: everything before the first <path (svg element, title). */
    {
        const char *first = strstr (svg, "<path");

        header = first != NULL ? (size_t) (first - svg) : 0;
    }
    out = (char *) malloc (length + 16);
    if (out == NULL)
        return NULL;
    p = out;
    memcpy (p, svg, header);
    p += header;
    if (layer == 0)
    {
        size_t n = (size_t) (starts[0] - (svg + header));

        memcpy (p, svg + header, n);
        p += n;
    }
    else if (layer >= 1 && layer <= 3)
    {
        size_t n = (size_t) (ends[layer - 1] - starts[layer - 1]);

        memcpy (p, starts[layer - 1], n);
        p += n;
    }
    else
    {
        size_t n = (size_t) (close_tag - ends[2]);

        memcpy (p, ends[2], n);
        p += n;
    }
    memcpy (p, "\n</svg>\n", 9);
    return out;
}

#ifdef __cplusplus
}
#endif

#endif /* SDE_TRANSFER_MODEL_H */
