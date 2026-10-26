/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * The copy and move progress of SDE (0.2.0 Alpha 2, 3): the model shared
 * by the SDE-FLTK dialog (src/common/transfer_dialog.*, used by the desktop
 * and other SDE programs) and Liria's file operations window
 * (suite/redesign.py copies this file into the Liria tree as
 * libliria-private/liria-sde-transfer-model.h), so both estimate the same.
 * The picture of the dialog is transfer_scene.h. Plain C99 that also
 * compiles as C++; licensed GPL-2.0-or-later like Liria, so the file can
 * be part of both.
 *
 *   SdeTransfer   bytes and items done and in total over time: the current
 *                 speed (smoothed over about 2 s) and the time remaining
 *                 (the average and the current speed together, so it
 *                 neither jumps nor lags). Paused time does not count.
 *   sde_transfer_eta_parts ()
 *                 the remaining time in the words a person uses: "a few
 *                 seconds", about N seconds (5 s steps), minutes, hours.
 *
 * 0.2.0 Alpha 3: the speed graph and the SDE mark's pulse of Alpha 2 are
 * gone with the dialog's new design.
 */
#ifndef SDE_TRANSFER_MODEL_H
#define SDE_TRANSFER_MODEL_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDE_TRANSFER_RATE_SAMPLES 32  /* recent (time, bytes) pairs for the speed */
#define SDE_TRANSFER_RATE_SPAN 2.0    /* seconds the current speed looks back */

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
} SdeTransfer;

static inline void
sde_transfer_init (SdeTransfer *t, double now)
{
    memset (t, 0, sizeof (*t));
    t->start = t->now = now;
    t->paused_since = -1.0;
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
    double active;
    int i, oldest;

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

#ifdef __cplusplus
}
#endif

#endif /* SDE_TRANSFER_MODEL_H */
