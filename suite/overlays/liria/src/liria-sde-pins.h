/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Liria Quick access pins: the user-editable list of folders, files and
 * locations shown as cards on the Home overview. Shared by the window
 * (sidebar menu) and the SDE Tools extension (file menu, Home overview).
 */
#ifndef LIRIA_SDE_PINS_H
#define LIRIA_SDE_PINS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
	gchar *uri;   /* the pinned location */
	gchar *label; /* custom card title, or NULL for the file's name */
} LiriaSdePin;

/* Pure text format: no I/O, used by the unit test. */
G_GNUC_INTERNAL GPtrArray *liria_sde_pins_parse (const gchar *text);
G_GNUC_INTERNAL gchar *liria_sde_pins_serialize (GPtrArray *pins);
G_GNUC_INTERNAL LiriaSdePin *liria_sde_pin_new (const gchar *uri, const gchar *label);
G_GNUC_INTERNAL void liria_sde_pin_free (LiriaSdePin *pin);
G_GNUC_INTERNAL gchar *liria_sde_pins_canonical_uri (const gchar *uri_or_path);
G_GNUC_INTERNAL gint liria_sde_pins_find (GPtrArray *pins, const gchar *uri);

/* ~/.config/sde/liria-quick-access */
G_GNUC_INTERNAL gchar *liria_sde_pins_path (void);
G_GNUC_INTERNAL GPtrArray *liria_sde_pins_defaults (void);
G_GNUC_INTERNAL GPtrArray *liria_sde_pins_load (void);
G_GNUC_INTERNAL gboolean liria_sde_pins_save (GPtrArray *pins);

/* Load, change and save in one step. */
G_GNUC_INTERNAL gboolean liria_sde_pins_is_pinned (const gchar *uri);
G_GNUC_INTERNAL gboolean liria_sde_pins_add (const gchar *const *uris, gint position);
G_GNUC_INTERNAL gboolean liria_sde_pins_remove (const gchar *const *uris);
G_GNUC_INTERNAL gboolean liria_sde_pins_move (const gchar *uri, gint position);
G_GNUC_INTERNAL gboolean liria_sde_pins_set_label (const gchar *uri, const gchar *label);
G_GNUC_INTERNAL gboolean liria_sde_pins_reset (void);

G_END_DECLS

#endif
