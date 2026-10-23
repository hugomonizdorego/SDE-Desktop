/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Liria SDE preferences and the small decisions behind the ribbon, the
 * Home view and drag and drop, kept free of widgets so that a unit test
 * (sde-tools/test-liria-sde-model.c) can check them. Shared by the window
 * and the SDE Tools extension like liria-sde-pins.c.
 */
#ifndef LIRIA_SDE_MODEL_H
#define LIRIA_SDE_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------ preferences
 * ~/.config/sde/liria.conf (XDG_CONFIG_HOME), a GKeyFile:
 *   [ribbon] mode=full|collapsed|off  tab=home|share|view
 *   [home]   show-overview=true  collapsed=quick;drives;recent
 *   [sidebar] collapsed=pinned;drives;network;bookmarks
 */
G_GNUC_INTERNAL gchar *liria_sde_prefs_path (void);
G_GNUC_INTERNAL gchar *liria_sde_prefs_get_string (const gchar *group, const gchar *key, const gchar *fallback);
G_GNUC_INTERNAL gboolean liria_sde_prefs_set_string (const gchar *group, const gchar *key, const gchar *value);
G_GNUC_INTERNAL gboolean liria_sde_prefs_get_bool (const gchar *group, const gchar *key, gboolean fallback);
G_GNUC_INTERNAL gboolean liria_sde_prefs_set_bool (const gchar *group, const gchar *key, gboolean value);
/* A ';'-separated set of names, e.g. the collapsed sections. */
G_GNUC_INTERNAL gboolean liria_sde_prefs_has_flag (const gchar *group, const gchar *key, const gchar *name);
G_GNUC_INTERNAL gboolean liria_sde_prefs_set_flag (const gchar *group, const gchar *key, const gchar *name,
                                                   gboolean on);
/* Pure helpers of the flag list, used by the two above. */
G_GNUC_INTERNAL gboolean liria_sde_flags_contain (const gchar *list, const gchar *name);
G_GNUC_INTERNAL gchar *liria_sde_flags_with (const gchar *list, const gchar *name, gboolean on);

/* ----------------------------------------------------------------- ribbon */
typedef enum {
	LIRIA_SDE_RIBBON_FULL,      /* tabs and the command groups */
	LIRIA_SDE_RIBBON_COLLAPSED, /* tabs only; a tab drops its commands down */
	LIRIA_SDE_RIBBON_OFF        /* the classic compact header only */
} LiriaSdeRibbonMode;

G_GNUC_INTERNAL LiriaSdeRibbonMode liria_sde_ribbon_mode_parse (const gchar *text);
G_GNUC_INTERNAL const gchar *liria_sde_ribbon_mode_name (LiriaSdeRibbonMode mode);
/* Ctrl+F1: full <-> collapsed; an "off" ribbon comes back full. */
G_GNUC_INTERNAL LiriaSdeRibbonMode liria_sde_ribbon_mode_toggle (LiriaSdeRibbonMode mode);
G_GNUC_INTERNAL LiriaSdeRibbonMode liria_sde_ribbon_mode_load (void);
G_GNUC_INTERNAL gboolean liria_sde_ribbon_mode_save (LiriaSdeRibbonMode mode);

/* Which commands a selection allows (the ribbon enables its buttons from it). */
typedef struct {
	guint count;        /* selected items */
	guint folders;      /* of which folders */
	guint archives;     /* of which archives */
	gboolean writable;  /* the current folder accepts new files */
	gboolean clipboard; /* the clipboard holds files */
} LiriaSdeSelection;

typedef enum {
	LIRIA_SDE_CMD_PIN = 1 << 0,        /* pin the selection, or the folder itself */
	LIRIA_SDE_CMD_COPY = 1 << 1,       /* copy, copy path, copy to, send by email */
	LIRIA_SDE_CMD_CUT = 1 << 2,        /* cut, move to, delete, rename */
	LIRIA_SDE_CMD_PASTE = 1 << 3,      /* paste, paste shortcut */
	LIRIA_SDE_CMD_NEW = 1 << 4,        /* new folder, new document */
	LIRIA_SDE_CMD_RENAME = 1 << 5,     /* exactly one item */
	LIRIA_SDE_CMD_OPEN = 1 << 6,       /* open, open with */
	LIRIA_SDE_CMD_COMPRESS = 1 << 7,
	LIRIA_SDE_CMD_EXTRACT = 1 << 8     /* only archives selected */
} LiriaSdeCommands;

G_GNUC_INTERNAL guint liria_sde_selection_commands (const LiriaSdeSelection *selection);

/* --------------------------------------------------------------- the Home view */

/* Folders suggested in Quick access: the parent folders of recently used
 * files (newest first in recent_uris), most used first, without the home
 * folder, the file system root and what is pinned already. */
G_GNUC_INTERNAL GPtrArray *liria_sde_home_frequent_folders (const gchar *const *recent_uris,
                                                            const gchar *const *pinned_uris,
                                                            const gchar *home_uri, guint max);
/* Cards per row of a responsive grid: as many min_width cards as fit
 * (spacing between them), at least 1 and at most max. */
G_GNUC_INTERNAL guint liria_sde_home_columns (gint width, gint min_width, gint spacing, guint max);

/* ----------------------------------------------------------- drag and drop */
typedef enum {
	LIRIA_SDE_DROP_AREA,        /* the Quick access area, the "+" card, a gap */
	LIRIA_SDE_DROP_FOLDER_CARD, /* a pinned folder or location */
	LIRIA_SDE_DROP_FILE_CARD,   /* a pinned file */
	LIRIA_SDE_DROP_DRIVE_CARD,  /* a drive */
	LIRIA_SDE_DROP_PINNED_HEADING /* the sidebar's Pinned heading or a gap in it */
} LiriaSdeDropTarget;

typedef enum {
	LIRIA_SDE_DROP_NONE,
	LIRIA_SDE_DROP_PIN,     /* add the dropped locations to Quick access */
	LIRIA_SDE_DROP_REORDER, /* a dragged pin card changes place */
	LIRIA_SDE_DROP_FILES    /* copy, move or link the files into the target */
} LiriaSdeDropResult;

G_GNUC_INTERNAL LiriaSdeDropResult liria_sde_drop_decide (LiriaSdeDropTarget target, gboolean from_pin_card,
                                                          guint n_uris);
/* The pin position a drop at x on a card [card_x, card_x + card_width) of
 * pin index means: before it on its left half, after it on the right. */
G_GNUC_INTERNAL gint liria_sde_drop_pin_index (gint index, gint x, gint card_x, gint card_width);

G_END_DECLS

#endif
