/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Liria window chrome for SDE: the compact header bar, the main menu,
 * keyboard shortcuts, the About window and Liria's own style sheet.
 */
#ifndef LIRIA_SDE_UI_H
#define LIRIA_SDE_UI_H

#include <gtk/gtk.h>

#include "liria-navigation-window.h"
#include "liria-navigation-window-pane.h"

void liria_sde_ui_setup_header   (LiriaNavigationWindow *window,
                                  GtkWidget *toolbar,
                                  GtkUIManager *ui_manager,
                                  LiriaNavigationWindowPane *pane);
void liria_sde_ui_tidy_sidebar   (LiriaNavigationWindow *window);
void liria_sde_ui_startup        (GApplication *application);
void liria_sde_ui_show_about     (GtkWindow *parent);
const char *liria_sde_ui_icon_name (void);
void liria_sde_ui_show_shortcuts (GtkWindow *parent);
void liria_sde_ui_show_help      (GtkWindow *parent, const char *section);
void liria_sde_ui_switch_view    (LiriaNavigationWindow *window, const char *id);
void liria_sde_ui_add_sort_items (GtkWidget *menu, LiriaNavigationWindow *window);
GIcon *liria_sde_ui_sidebar_icon (GIcon *icon);
gint liria_sde_ui_sidebar_icon_pixels (void);
gboolean liria_sde_ui_sidebar_collapsed (const char *section);
void liria_sde_ui_sidebar_toggle (const char *section);

#endif
