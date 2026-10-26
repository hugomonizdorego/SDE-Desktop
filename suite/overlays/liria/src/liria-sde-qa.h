/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * QA recorder for tools/liria-perf.py (LIRIA_SDE_QA_FRAMES=FILE): frame
 * and pointer event timings of Liria's windows and the places of the parts
 * that its hover scenarios move the pointer over.
 */
#ifndef LIRIA_SDE_QA_H
#define LIRIA_SDE_QA_H

#include <gtk/gtk.h>

void liria_sde_qa_startup (void);
void liria_sde_qa_window  (GtkWidget *window);

#endif
