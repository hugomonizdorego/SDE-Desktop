/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * The picture of SDE's copy and move dialog (0.2.0 Alpha 3): a penguin
 * takes a paper out of one folder, carries it on its head to the other
 * folder and drops it in, again and again while the operation runs. SDE's
 * own drawing in the style of SDE's icons (flat fills, a dark outline,
 * warm colours), drawn with cairo as vectors, so it is sharp at every size
 * and scale. Shared by the SDE-FLTK dialog (src/common/transfer_dialog.cpp)
 * and Liria's file operations window (suite/redesign.py copies this file
 * into the Liria tree as libliria-private/liria-sde-transfer-scene.h).
 * Plain C99 that also compiles as C++; GPL-2.0-or-later like Liria.
 *
 *   copy       a paper comes out of the full source folder, which stays
 *              full; the destination fills up with the progress;
 *   move       the same, but the source folder empties as it goes;
 *   delete     the destination is a bin, whose lid opens for the paper;
 *   paused     the penguin sits down with its paper (pause bars above);
 *   queued     the same, with a small clock;
 *   finishing  the penguin pushes the last paper into the folder ("Writing
 *              the last data to ...");
 *   done       a little jump with the flippers up, a check mark above;
 *   error      the paper on the ground, the head tilted, a question mark.
 *
 * sde_scene_draw () draws the picture of any moment, a pure function of
 * the time: callers repaint it about 30 times a second while it moves and
 * not at all otherwise (sde_scene_moving ()). Without animation (reduced
 * motion) every state has a still picture.
 */
#ifndef SDE_TRANSFER_SCENE_H
#define SDE_TRANSFER_SCENE_H

#include <cairo.h>
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDE_SCENE_COPY,
    SDE_SCENE_MOVE,
    SDE_SCENE_DELETE /* also moving to the trash */
} SdeSceneKind;

typedef enum {
    SDE_SCENE_RUNNING,
    SDE_SCENE_PAUSED,
    SDE_SCENE_QUEUED,
    SDE_SCENE_FINISHING,
    SDE_SCENE_DONE,
    SDE_SCENE_ERROR
} SdeSceneState;

typedef struct {
    double r, g, b;
} SdeSceneRgb;

typedef struct {
    SdeSceneKind kind;
    SdeSceneState state;
    double progress;      /* 0-1: how full the destination is (empty the source, for a move); < 0 unknown */
    double seconds;       /* the animation's time */
    double state_seconds; /* seconds since the state began (the jump when done) */
    int animate;          /* 0: a still picture (reduced motion) */
    int dark;             /* on a dark background */
    SdeSceneRgb ink;      /* the text colour, for the symbols above the penguin */
} SdeScene;

/* The design: 100 units high, 320 wide, centred in the space given. */
#define SDE_SCENE_HEIGHT 100.0
#define SDE_SCENE_WIDTH 320.0
#define SDE_SCENE_GROUND 92.0
#define SDE_SCENE_FOLDER_W 64.0
#define SDE_SCENE_WALK_SPEED 62.0 /* units per second */
#define SDE_SCENE_STEPS 4.2       /* steps per second */
#define SDE_SCENE_PICK 0.75       /* seconds to take a paper */
#define SDE_SCENE_DROP 0.75       /* seconds to drop it */

/* Where the penguin stands to take and to drop a paper (units). */
#define SDE_SCENE_PICK_X (14.0 + SDE_SCENE_FOLDER_W + 28.0)
#define SDE_SCENE_DROP_X (SDE_SCENE_WIDTH - 14.0 - SDE_SCENE_FOLDER_W - 28.0)

/* One loop of the walk, in seconds. */
static inline double
sde_scene_cycle (void)
{
    return 2.0 * SDE_SCENE_PICK + 2.0 * (SDE_SCENE_DROP_X - SDE_SCENE_PICK_X) / SDE_SCENE_WALK_SPEED;
}

/* TRUE while the picture changes by itself (worth repainting). */
static inline int
sde_scene_moving (const SdeScene *scene)
{
    if (!scene->animate)
        return 0;
    switch (scene->state)
    {
        case SDE_SCENE_RUNNING:
        case SDE_SCENE_FINISHING:
            return 1;
        case SDE_SCENE_DONE:
            return scene->state_seconds < 1.4; /* the jump */
        default:
            return 0;
    }
}

/* ----------------------------------------------------------------- colours */

static inline void
sde_scene_set (cairo_t *cr, double r, double g, double b)
{
    cairo_set_source_rgb (cr, r, g, b);
}

static inline void
sde_scene_hex (cairo_t *cr, unsigned int rgb, double alpha)
{
    cairo_set_source_rgba (cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0,
                           alpha);
}

/* SDE's icons: a dark outline around flat fills. */
#define SDE_SCENE_OUTLINE 0x2B2B2F
#define SDE_SCENE_LINE 2.1

static inline void
sde_scene_fill_outline (cairo_t *cr, unsigned int fill)
{
    sde_scene_hex (cr, fill, 1.0);
    cairo_fill_preserve (cr);
    sde_scene_hex (cr, SDE_SCENE_OUTLINE, 1.0);
    cairo_set_line_width (cr, SDE_SCENE_LINE);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke (cr);
}

/* A vertical gradient from |top| to |bottom| between y0 and y1, outlined. */
static inline void
sde_scene_fill_gradient (cairo_t *cr, unsigned int top, unsigned int bottom, double y0, double y1)
{
    cairo_pattern_t *p = cairo_pattern_create_linear (0, y0, 0, y1);

    cairo_pattern_add_color_stop_rgb (p, 0, ((top >> 16) & 0xff) / 255.0, ((top >> 8) & 0xff) / 255.0,
                                      (top & 0xff) / 255.0);
    cairo_pattern_add_color_stop_rgb (p, 1, ((bottom >> 16) & 0xff) / 255.0, ((bottom >> 8) & 0xff) / 255.0,
                                      (bottom & 0xff) / 255.0);
    cairo_set_source (cr, p);
    cairo_fill_preserve (cr);
    cairo_pattern_destroy (p);
    sde_scene_hex (cr, SDE_SCENE_OUTLINE, 1.0);
    cairo_set_line_width (cr, SDE_SCENE_LINE);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke (cr);
}

/* ------------------------------------------------------------------ shapes */

static inline void
sde_scene_ellipse (cairo_t *cr, double cx, double cy, double rx, double ry)
{
    cairo_save (cr);
    cairo_translate (cr, cx, cy);
    cairo_scale (cr, rx, ry);
    cairo_new_sub_path (cr);
    cairo_arc (cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore (cr);
}

static inline void
sde_scene_rounded (cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc (cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc (cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc (cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path (cr);
}

static inline void
sde_scene_shadow (cairo_t *cr, double cx, double rx, int dark)
{
    sde_scene_ellipse (cr, cx, SDE_SCENE_GROUND, rx, 3.2);
    cairo_set_source_rgba (cr, 0, 0, 0, dark ? 0.30 : 0.10);
    cairo_fill (cr);
}

/* A paper (18 x 22) with a folded corner and lines of text, centred at
 * (cx, cy), turned by |angle|. */
static inline void
sde_scene_paper (cairo_t *cr, double cx, double cy, double angle)
{
    const double w = 18, h = 22, fold = 5.5;
    int i;

    cairo_save (cr);
    cairo_translate (cr, cx, cy);
    cairo_rotate (cr, angle);
    cairo_translate (cr, -w / 2, -h / 2);
    cairo_move_to (cr, 1.5, 0);
    cairo_line_to (cr, w - fold, 0);
    cairo_line_to (cr, w, fold);
    cairo_line_to (cr, w, h - 1.5);
    cairo_arc (cr, w - 1.5, h - 1.5, 1.5, 0, M_PI / 2);
    cairo_line_to (cr, 1.5, h);
    cairo_arc (cr, 1.5, h - 1.5, 1.5, M_PI / 2, M_PI);
    cairo_line_to (cr, 0, 1.5);
    cairo_arc (cr, 1.5, 1.5, 1.5, M_PI, 3 * M_PI / 2);
    cairo_close_path (cr);
    sde_scene_fill_outline (cr, 0xFBFAF7);
    /* The folded corner. */
    cairo_move_to (cr, w - fold, 0);
    cairo_line_to (cr, w - fold, fold);
    cairo_line_to (cr, w, fold);
    cairo_close_path (cr);
    sde_scene_fill_outline (cr, 0xE3DFD4);
    /* Lines of text. */
    sde_scene_hex (cr, 0x9A968C, 1.0);
    cairo_set_line_width (cr, 1.6);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    for (i = 0; i < 3; i++)
    {
        cairo_move_to (cr, 4.0, 9.5 + i * 4.0);
        cairo_line_to (cr, i == 2 ? 10.0 : 14.0, 9.5 + i * 4.0);
    }
    cairo_stroke (cr);
    cairo_restore (cr);
}

/* A folder like SDE's (Liria's) icon: its back with the tab, |papers|
 * papers standing in it (0-3), then (sde_scene_folder_front ()) its front. */
static inline void
sde_scene_folder_back (cairo_t *cr, double x, int papers, double bump)
{
    const double w = SDE_SCENE_FOLDER_W, top = SDE_SCENE_GROUND - 50, bottom = SDE_SCENE_GROUND - 1;
    int i;

    cairo_new_path (cr);
    cairo_move_to (cr, x + 4, top);
    cairo_line_to (cr, x + 21, top);
    cairo_curve_to (cr, x + 24, top, x + 25, top + 1, x + 26.5, top + 3);
    cairo_line_to (cr, x + 28.5, top + 6);
    cairo_line_to (cr, x + w - 4, top + 6);
    cairo_arc (cr, x + w - 4, top + 10, 4, -M_PI / 2, 0);
    cairo_line_to (cr, x + w, bottom - 4);
    cairo_arc (cr, x + w - 4, bottom - 4, 4, 0, M_PI / 2);
    cairo_line_to (cr, x + 4, bottom);
    cairo_arc (cr, x + 4, bottom - 4, 4, M_PI / 2, M_PI);
    cairo_line_to (cr, x, top + 4);
    cairo_arc (cr, x + 4, top + 4, 4, M_PI, 3 * M_PI / 2);
    cairo_close_path (cr);
    sde_scene_fill_gradient (cr, 0xD9A646, 0xC48F33, top, bottom);
    /* Papers standing in the folder, the last one a little higher. */
    for (i = 0; i < papers && i < 3; i++)
    {
        double px = x + 9 + i * 5.0, py = top + 11 - i * 1.5 - (i == papers - 1 ? bump : 0.0);

        sde_scene_rounded (cr, px, py, w - 20 - i * 3.0, 24, 2);
        sde_scene_fill_outline (cr, i % 2 ? 0xF2EFE7 : 0xFBFAF7);
    }
}

static inline void
sde_scene_folder_front (cairo_t *cr, double x, double squash)
{
    const double w = SDE_SCENE_FOLDER_W, bottom = SDE_SCENE_GROUND - 1;
    double top = SDE_SCENE_GROUND - 34 + squash;

    cairo_new_path (cr);
    sde_scene_rounded (cr, x - 0.5, top, w + 1, bottom - top, 4.5);
    sde_scene_fill_gradient (cr, 0xF3CB69, 0xE4AF4A, top, bottom);
}

/* A bin (the destination of a delete) with its lid open by |open| (0-1). */
static inline void
sde_scene_bin (cairo_t *cr, double x, double open, int front)
{
    const double w = SDE_SCENE_FOLDER_W, bottom = SDE_SCENE_GROUND - 1, top = SDE_SCENE_GROUND - 44;
    double l = x + 10, r = x + w - 10;
    int i;

    if (!front)
    {
        /* The lid, hinged at the back (right). */
        cairo_save (cr);
        cairo_translate (cr, r + 3, top - 1);
        cairo_rotate (cr, open * 0.62);
        sde_scene_rounded (cr, -(r - l) / 2 - 9.5, -9.5, 12, 5, 2);
        sde_scene_fill_outline (cr, 0x9EA4B0);
        sde_scene_rounded (cr, -(r - l) - 7, -6, r - l + 7, 6, 2.5);
        sde_scene_fill_gradient (cr, 0xB9BEC8, 0x9EA4B0, -6, 0);
        cairo_restore (cr);
        /* The opening, dark inside. */
        sde_scene_ellipse (cr, (l + r) / 2, top + 1.5, (r - l) / 2 + 1, 2.6);
        sde_scene_fill_outline (cr, 0x55595F);
        return;
    }
    cairo_new_path (cr);
    cairo_move_to (cr, l - 2, top + 1);
    cairo_line_to (cr, r + 2, top + 1);
    cairo_line_to (cr, r - 2, bottom - 3);
    cairo_arc (cr, r - 5, bottom - 3, 3, 0, M_PI / 2);
    cairo_line_to (cr, l + 5, bottom);
    cairo_arc (cr, l + 5, bottom - 3, 3, M_PI / 2, M_PI);
    cairo_close_path (cr);
    sde_scene_fill_gradient (cr, 0xCDD1D8, 0xAEB3BD, top, bottom);
    sde_scene_hex (cr, 0x868B95, 1.0);
    cairo_set_line_width (cr, 1.6);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    for (i = 0; i < 3; i++)
    {
        double fx = l + 9 + i * ((r - l - 18) / 2.0);

        cairo_move_to (cr, fx, top + 9);
        cairo_line_to (cr, fx + (i - 1) * 1.2, bottom - 9);
    }
    cairo_stroke (cr);
}

/* ----------------------------------------------------------------- penguin */

typedef struct {
    double x;        /* the feet, on the ground */
    double facing;   /* 1 right, -1 left */
    double step;     /* the walk's phase in radians; 0 standing */
    double lift;     /* how far the body is lifted (a jump), units */
    double lean;     /* forward lean, radians */
    double flippers; /* 0 down, 1 up holding a paper on the head, 2 up and open (happy) */
    double tilt;     /* the head (puzzled), radians */
    int sitting;
    int carrying;    /* a paper on the head */
} SdePenguin;

/* A flipper hanging from (ax, ay), turned by |angle| (0 straight down;
 * positive turns the tip to the left), |length| times its size. */
static inline void
sde_scene_flipper (cairo_t *cr, double ax, double ay, double angle, double length, unsigned int fill)
{
    cairo_save (cr);
    cairo_translate (cr, ax, ay);
    cairo_rotate (cr, angle);
    cairo_scale (cr, 1.0, length);
    cairo_new_path (cr);
    cairo_move_to (cr, -3.6, 0);
    cairo_curve_to (cr, -4.6, 7, -2.6, 14, 0.4, 17);
    cairo_curve_to (cr, 2.6, 14.5, 4.4, 7, 3.4, 0);
    cairo_curve_to (cr, 2.2, -2.6, -2.4, -2.6, -3.6, 0);
    cairo_close_path (cr);
    cairo_restore (cr);
    sde_scene_fill_outline (cr, fill);
}

static inline void
sde_scene_penguin (cairo_t *cr, const SdePenguin *p, int dark)
{
    unsigned int body = dark ? 0x4A5263 : 0x343A48, flipper = dark ? 0x5A6376 : 0x434A5A;
    unsigned int orange = 0xF2A33A, orange_dark = 0xD9872A;
    double bob = p->step != 0.0 ? fabs (sin (p->step)) * 2.2 : 0.0;
    double sway = p->step != 0.0 ? sin (p->step) * 0.10 : 0.0;
    double lift_a = p->step != 0.0 ? fmax (0.0, sin (p->step)) * 3.0 : 0.0;
    double lift_b = p->step != 0.0 ? fmax (0.0, -sin (p->step)) * 3.0 : 0.0;
    double sit = p->sitting ? 7.0 : 0.0;
    /* The near flipper is on the left of the body (the penguin looks to
     * the right), the far one behind it on the right: hanging and swinging,
     * up along the head to hold the paper, or up and open. */
    double near_angle, far_angle, near_y = -34, far_y = -35, reach = 1.0;

    if (p->flippers >= 1.5)
    {
        near_angle = M_PI - 0.75;
        far_angle = M_PI + 0.75;
        near_y = far_y = -37;
        reach = 1.05;
    }
    else if (p->flippers >= 0.5)
    {
        /* Out to the sides for balance, the paper on the head. */
        near_angle = 0.95 + sway * 1.5;
        far_angle = -0.85 + sway * 1.5;
        near_y = far_y = -36;
    }
    else
    {
        near_angle = 0.28 + sway * 2.5;
        far_angle = -0.22 - sway * 2.5;
    }

    /* The shadow does not jump. */
    sde_scene_shadow (cr, p->x + 1, 15.5 - p->lift * 0.25, dark);

    cairo_save (cr);
    cairo_translate (cr, p->x, SDE_SCENE_GROUND - p->lift);
    cairo_scale (cr, p->facing, 1.0);

    /* Feet: one lifted at a time while walking (sitting: in front). */
    if (!p->sitting)
    {
        sde_scene_ellipse (cr, -5 - lift_a * 0.8, -2.6 - lift_a, 7.0, 3.1);
        sde_scene_fill_outline (cr, orange_dark);
        sde_scene_ellipse (cr, 6 + lift_b * 0.8, -2.6 - lift_b, 7.0, 3.1);
        sde_scene_fill_outline (cr, orange);
    }

    cairo_translate (cr, 0, sit - bob);
    cairo_rotate (cr, sway + p->lean);

    /* The far flipper behind the body. */
    sde_scene_flipper (cr, 10.0, far_y, far_angle, reach, body);

    /* Body. */
    sde_scene_ellipse (cr, 0, -29, 17.5, 27.5);
    sde_scene_fill_outline (cr, body);

    cairo_save (cr);
    cairo_translate (cr, 3, -40);
    cairo_rotate (cr, p->tilt);
    cairo_translate (cr, -3, 40);
    /* Face and belly: one white patch. */
    cairo_new_path (cr);
    cairo_move_to (cr, 3.5, -48.5);
    cairo_curve_to (cr, 9.5, -51, 15.5, -46, 14.5, -38);
    cairo_curve_to (cr, 15.5, -30, 14.5, -18, 10, -9);
    cairo_curve_to (cr, 6, -3.5, -2.5, -3.5, -6, -9);
    cairo_curve_to (cr, -9.5, -17, -9.5, -30, -6.5, -37);
    cairo_curve_to (cr, -7.5, -45.5, -2, -50.5, 3.5, -48.5);
    cairo_close_path (cr);
    sde_scene_hex (cr, 0xFBFAF7, 1.0);
    cairo_fill (cr);
    /* Eyes, with a little light. */
    sde_scene_ellipse (cr, 1.0, -40.5, 2.3, 2.7);
    sde_scene_ellipse (cr, 8.8, -40.5, 2.3, 2.7);
    sde_scene_hex (cr, SDE_SCENE_OUTLINE, 1.0);
    cairo_fill (cr);
    sde_scene_ellipse (cr, 1.7, -41.4, 0.8, 0.8);
    sde_scene_ellipse (cr, 9.5, -41.4, 0.8, 0.8);
    sde_scene_hex (cr, 0xFFFFFF, 1.0);
    cairo_fill (cr);
    /* Beak. */
    cairo_new_path (cr);
    cairo_move_to (cr, 8.5, -36.8);
    cairo_curve_to (cr, 12, -38.8, 17.5, -37.5, 19.5, -35.2);
    cairo_curve_to (cr, 17, -33.2, 11.5, -32.2, 8.5, -34.0);
    cairo_close_path (cr);
    sde_scene_fill_outline (cr, orange);
    /* Cheeks. */
    sde_scene_ellipse (cr, -3.0, -34.5, 2.2, 1.3);
    sde_scene_hex (cr, 0xF4A6A0, 0.55);
    cairo_fill (cr);
    cairo_restore (cr);

    /* The near flipper over the body. */
    sde_scene_flipper (cr, -12.5, near_y, near_angle, reach, flipper);
    if (p->sitting)
    {
        cairo_rotate (cr, -(sway + p->lean));
        sde_scene_ellipse (cr, -1, -2.0, 7.0, 3.0);
        sde_scene_fill_outline (cr, orange_dark);
        sde_scene_ellipse (cr, 10, -2.4, 7.5, 3.2);
        sde_scene_fill_outline (cr, orange);
    }
    cairo_restore (cr);
}

/* ------------------------------------------------------------------ scene */

/* Draws |scene| in (x, y, w, h); the picture keeps its proportions and
 * stands centred. */
static inline void
sde_scene_draw (cairo_t *cr, double x, double y, double w, double h, const SdeScene *scene)
{
    double s = h / SDE_SCENE_HEIGHT;
    double left_x = 14.0, right_x = SDE_SCENE_WIDTH - 14.0 - SDE_SCENE_FOLDER_W;
    double t = scene->animate ? scene->seconds : 0.0;
    double progress = scene->progress < 0.0 ? 0.0 : (scene->progress > 1.0 ? 1.0 : scene->progress);
    int papers_src = 3, papers_dst;
    int bin = scene->kind == SDE_SCENE_DELETE;
    SdePenguin peng;
    /* A paper on its way between the penguin and a folder: from (ax, ay)
     * to (bx, by) by |k| (0-1); |into| the destination. */
    double fly = -1.0, fax = 0, fay = 0, fbx = 0, fby = 0;
    int fly_into = 0;
    double squash = 0.0, lid = 0.0, bump = 0.0;

    if (s * SDE_SCENE_WIDTH > w)
        s = w / SDE_SCENE_WIDTH;
    if (s <= 0.0)
        return;

    papers_dst = (int) floor (progress * 3.0 + 0.5);
    if (scene->kind == SDE_SCENE_MOVE || scene->kind == SDE_SCENE_DELETE)
        papers_src = 3 - (int) floor (progress * 3.0);
    if (scene->state == SDE_SCENE_DONE)
    {
        papers_dst = 3;
        if (scene->kind != SDE_SCENE_COPY)
            papers_src = 0;
    }
    if (papers_src < 0)
        papers_src = 0;
    if (papers_src > 3)
        papers_src = 3;
    if (papers_dst > 3)
        papers_dst = 3;

    memset (&peng, 0, sizeof (peng));
    peng.facing = 1.0;

    switch (scene->state)
    {
        case SDE_SCENE_RUNNING:
            if (!scene->animate)
            {
                /* Still: on the way, the paper on its head. */
                peng.x = (SDE_SCENE_PICK_X + SDE_SCENE_DROP_X) / 2.0;
                peng.carrying = 1;
                peng.flippers = 1.0;
            }
            else
            {
                double walk = (SDE_SCENE_DROP_X - SDE_SCENE_PICK_X) / SDE_SCENE_WALK_SPEED;
                double u = fmod (t, sde_scene_cycle ());

                if (u < SDE_SCENE_PICK)
                {
                    /* At the source, facing it: a paper rises onto the head. */
                    double k = u / SDE_SCENE_PICK;

                    peng.x = SDE_SCENE_PICK_X;
                    peng.facing = -1.0;
                    peng.flippers = k > 0.55 ? 1.0 : 0.0;
                    fly = k;
                    fax = left_x + SDE_SCENE_FOLDER_W / 2.0;
                    fay = SDE_SCENE_GROUND - 40;
                    fbx = peng.x - 2;
                    fby = SDE_SCENE_GROUND - 68;
                    bump = sin (k * M_PI) * 3.0;
                }
                else if (u < SDE_SCENE_PICK + walk)
                {
                    double k = (u - SDE_SCENE_PICK) / walk;

                    peng.x = SDE_SCENE_PICK_X + k * (SDE_SCENE_DROP_X - SDE_SCENE_PICK_X);
                    peng.step = (u - SDE_SCENE_PICK) * SDE_SCENE_STEPS * M_PI;
                    peng.carrying = 1;
                    peng.flippers = 1.0;
                }
                else if (u < SDE_SCENE_PICK + walk + SDE_SCENE_DROP)
                {
                    /* At the destination: the paper goes in. */
                    double k = (u - SDE_SCENE_PICK - walk) / SDE_SCENE_DROP;

                    peng.x = SDE_SCENE_DROP_X;
                    peng.flippers = k < 0.45 ? 1.0 : 0.0;
                    peng.lean = sin (k * M_PI) * 0.12;
                    fly = k;
                    fax = peng.x + 2;
                    fay = SDE_SCENE_GROUND - 68;
                    fbx = right_x + SDE_SCENE_FOLDER_W / 2.0;
                    fby = SDE_SCENE_GROUND - (bin ? 30 : 36);
                    fly_into = 1;
                    if (k > 0.75)
                        squash = sin ((k - 0.75) / 0.25 * M_PI) * 1.8;
                    lid = sin (k * M_PI);
                }
                else
                {
                    double k = (u - SDE_SCENE_PICK - walk - SDE_SCENE_DROP) / walk;

                    peng.x = SDE_SCENE_DROP_X - k * (SDE_SCENE_DROP_X - SDE_SCENE_PICK_X);
                    peng.facing = -1.0;
                    peng.step = (u - SDE_SCENE_PICK - walk - SDE_SCENE_DROP) * SDE_SCENE_STEPS * M_PI;
                }
            }
            break;
        case SDE_SCENE_PAUSED:
        case SDE_SCENE_QUEUED:
            peng.x = (SDE_SCENE_PICK_X + SDE_SCENE_DROP_X) / 2.0;
            peng.sitting = 1;
            peng.carrying = 1;
            peng.flippers = 1.0;
            break;
        case SDE_SCENE_FINISHING:
        {
            /* Pushing the last paper in, gently, again and again. */
            double k = scene->animate ? 0.5 - 0.5 * cos (t * 2.0 * M_PI / 1.2) : 0.5;

            peng.x = SDE_SCENE_DROP_X + 2;
            peng.lean = 0.10 + k * 0.10;
            peng.flippers = 0.0;
            fly = 0.78 + k * 0.12;
            fax = peng.x + 2;
            fay = SDE_SCENE_GROUND - 68;
            fbx = right_x + SDE_SCENE_FOLDER_W / 2.0;
            fby = SDE_SCENE_GROUND - (bin ? 30 : 36);
            fly_into = 1;
            lid = 0.6;
            break;
        }
        case SDE_SCENE_DONE:
        {
            double k = scene->animate ? scene->state_seconds : 2.0;

            peng.x = SDE_SCENE_DROP_X - 4;
            peng.flippers = 2.0;
            /* Two little jumps, then still. */
            if (k < 1.2)
                peng.lift = fabs (sin (k * 2.0 * M_PI / 1.2)) * 7.0;
            break;
        }
        case SDE_SCENE_ERROR:
            peng.x = (SDE_SCENE_PICK_X + SDE_SCENE_DROP_X) / 2.0 - 6;
            peng.tilt = -0.22;
            break;
    }

    cairo_save (cr);
    cairo_translate (cr, x + (w - SDE_SCENE_WIDTH * s) / 2.0, y + (h - SDE_SCENE_HEIGHT * s) / 2.0);
    cairo_scale (cr, s, s);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

    /* The ground under the folders. */
    sde_scene_shadow (cr, left_x + SDE_SCENE_FOLDER_W / 2.0, 36, scene->dark);
    sde_scene_shadow (cr, right_x + SDE_SCENE_FOLDER_W / 2.0, bin ? 26 : 36, scene->dark);

    /* The source folder, and a paper coming out of it. */
    sde_scene_folder_back (cr, left_x, papers_src, 0.0);
    if (fly >= 0.0 && !fly_into)
    {
        double k = fly * fly * (3.0 - 2.0 * fly);
        double px = fax + (fbx - fax) * k, py = fay + (fby - fay) * k - sin (fly * M_PI) * 14.0;

        sde_scene_paper (cr, px, py, (1.0 - k) * -0.5);
    }
    sde_scene_folder_front (cr, left_x, 0.0);

    /* The destination, and a paper going in. */
    if (bin)
        sde_scene_bin (cr, right_x, lid, 0);
    else
        sde_scene_folder_back (cr, right_x, papers_dst, bump * 0.0);
    if (fly >= 0.0 && fly_into)
    {
        double k = fly * fly;
        double px = fax + (fbx - fax) * k, py = fay + (fby - fay) * k - sin (fly * M_PI) * 12.0;

        sde_scene_paper (cr, px, py, k * 0.45);
    }
    if (bin)
        sde_scene_bin (cr, right_x, lid, 1);
    else
        sde_scene_folder_front (cr, right_x, squash);

    /* The penguin, and what it has. */
    if (scene->state == SDE_SCENE_ERROR)
        sde_scene_paper (cr, peng.x + 26, SDE_SCENE_GROUND - 9, 1.30);
    sde_scene_penguin (cr, &peng, scene->dark);
    if (peng.carrying)
    {
        double bob = peng.step != 0.0 ? fabs (sin (peng.step)) * 2.2 : 0.0;
        double sway = peng.step != 0.0 ? sin (peng.step) * 0.10 : 0.0;
        double sit = peng.sitting ? 7.0 : 0.0;

        sde_scene_paper (cr, peng.x + peng.facing * 1.5 + sway * 22.0, SDE_SCENE_GROUND - 69 - peng.lift + sit - bob,
                         sway * 0.8);
    }

    /* Signs above the penguin, in the text colour. */
    if (scene->state == SDE_SCENE_PAUSED || scene->state == SDE_SCENE_QUEUED || scene->state == SDE_SCENE_ERROR ||
        scene->state == SDE_SCENE_DONE)
    {
        double sx = peng.x + 24, sy = SDE_SCENE_GROUND - 78 + (peng.sitting ? 7 : 0);

        cairo_set_source_rgba (cr, scene->ink.r, scene->ink.g, scene->ink.b, 0.85);
        cairo_set_line_width (cr, 2.6);
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        if (scene->state == SDE_SCENE_PAUSED)
        {
            cairo_move_to (cr, sx - 2.5, sy - 5);
            cairo_line_to (cr, sx - 2.5, sy + 5);
            cairo_move_to (cr, sx + 2.5, sy - 5);
            cairo_line_to (cr, sx + 2.5, sy + 5);
            cairo_stroke (cr);
        }
        else if (scene->state == SDE_SCENE_QUEUED)
        {
            cairo_set_line_width (cr, 2.0);
            cairo_arc (cr, sx, sy, 6.0, 0, 2 * M_PI);
            cairo_move_to (cr, sx, sy - 3.5);
            cairo_line_to (cr, sx, sy);
            cairo_line_to (cr, sx + 2.8, sy + 1.6);
            cairo_stroke (cr);
        }
        else if (scene->state == SDE_SCENE_ERROR)
        {
            cairo_set_line_width (cr, 2.6);
            cairo_arc (cr, sx, sy - 3, 4.2, M_PI * 1.05, M_PI * 2.35);
            cairo_line_to (cr, sx, sy + 3.0);
            cairo_stroke (cr);
            cairo_arc (cr, sx, sy + 7.5, 1.6, 0, 2 * M_PI);
            cairo_fill (cr);
        }
        else
        {
            /* Done: a check mark in green. */
            cairo_set_source_rgb (cr, 0.20, 0.66, 0.33);
            cairo_set_line_width (cr, 3.0);
            cairo_move_to (cr, sx - 6, sy);
            cairo_line_to (cr, sx - 1.5, sy + 4.5);
            cairo_line_to (cr, sx + 7, sy - 5);
            cairo_stroke (cr);
        }
    }
    cairo_restore (cr);
}

#ifdef __cplusplus
}
#endif

#endif /* SDE_TRANSFER_SCENE_H */
