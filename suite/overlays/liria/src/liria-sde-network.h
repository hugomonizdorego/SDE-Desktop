/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Network places in Liria's windows (0.2.0 Alpha 3): the location bar
 * opens what liria-sde-address.c makes of the text, suggests recent
 * servers and the protocols of a computer, says what is missing when a
 * protocol is not installed, signs in with Liria's own dialog (Remember
 * through the keyring when one runs) and explains failures.
 */
#ifndef LIRIA_SDE_NETWORK_H
#define LIRIA_SDE_NETWORK_H

#include <gtk/gtk.h>

/* GVfs can open |scheme| here (gvfs-backends installed). */
gboolean liria_sde_network_scheme_supported (const char *scheme);
/* What the administrator has to install for |scheme|, or NULL. */
char *liria_sde_network_missing_message (const char *scheme);

/* The location entry: suggestions while typing, and on Enter the text
 * made into a location; TRUE when Liria showed a message instead (the
 * activation is stopped). */
void liria_sde_network_setup_entry (GtkEntry *entry);
gboolean liria_sde_network_entry_activate (GtkEntry *entry, const char *current_directory);

/* Mounting |location| for |parent|: Liria's sign-in dialog. */
GMountOperation *liria_sde_network_mount_operation_new (GtkWindow *parent, GFile *location);

/* A network location was shown: its server goes first in the recent list. */
void liria_sde_network_location_shown (GFile *location);
/* Explains why a network location could not be shown and offers the
 * other protocols of the computer; FALSE for other locations. */
gboolean liria_sde_network_show_failure (GtkWidget *window, GFile *location, GError *error);

#endif
