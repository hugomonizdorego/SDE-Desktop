/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 DebION Dev-Team
 *
 * Liria's ribbon (alpha52): Home, Share and View tabs with the file
 * commands under the header row, plus contextual Drive and Archive tabs.
 */
#ifndef LIRIA_SDE_RIBBON_H
#define LIRIA_SDE_RIBBON_H

#include <gtk/gtk.h>

#include "liria-navigation-window.h"
#include "liria-sde-model.h"

void liria_sde_ribbon_setup (LiriaNavigationWindow *window, GtkUIManager *ui_manager);
/* Changes the mode of every Liria window and remembers it. */
void liria_sde_ribbon_set_mode (LiriaSdeRibbonMode mode);
LiriaSdeRibbonMode liria_sde_ribbon_get_mode (void);
/* Full, collapsed and off as radio items (main menu, View tab). */
void liria_sde_ribbon_add_mode_items (GtkWidget *menu);

#endif
