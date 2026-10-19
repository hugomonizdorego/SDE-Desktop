/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hugo Moniz do Rego.
 * Extend the upstream About dialog without replacing its authors or license.
 */
#pragma once
#include <gtk/gtk.h>

static void sde_native_about_apply(GtkWidget *widget, gpointer unused) {
  (void)unused;
  if (g_object_get_data(G_OBJECT(widget), "sde-about-credited")) return;
  g_object_set_data(G_OBJECT(widget), "sde-about-credited", GINT_TO_POINTER(1));
  const gchar *developers[] = {
      "Hugo Moniz do Rego https://github.com/hugomonizdorego",
      "SDE contributors https://github.com/hugomonizdorego/SDE-Desktop", NULL};
  const gchar *source[] = {SDE_UPSTREAM_NAME " " SDE_UPSTREAM_URL, NULL};
  gtk_about_dialog_add_credit_section(GTK_ABOUT_DIALOG(widget), "SDE development", developers);
  gtk_about_dialog_add_credit_section(GTK_ABOUT_DIALOG(widget), "Original source", source);
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(widget), SDE_UPSTREAM_URL);
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(widget), SDE_UPSTREAM_NAME);
}
static void sde_native_about_all(void) {
  GList *windows = gtk_window_list_toplevels();
  for (GList *p = windows; p; p = p->next)
    if (GTK_IS_ABOUT_DIALOG(p->data)) sde_native_about_apply(GTK_WIDGET(p->data), NULL);
  g_list_free(windows);
}
static GtkWidget *sde_native_about_new(void) {
  GtkWidget *dialog = gtk_about_dialog_new();
  g_signal_connect(dialog, "map", G_CALLBACK(sde_native_about_apply), NULL);
  return dialog;
}
#define gtk_show_about_dialog(...) do { gtk_show_about_dialog(__VA_ARGS__); sde_native_about_all(); } while (0)
#define gtk_about_dialog_new() sde_native_about_new()

static void sde_native_about_button(GtkWidget *button, gpointer parent) {
  (void)button;
  const gchar *authors[] = {SDE_ORIGINAL_AUTHORS, NULL};
  gtk_show_about_dialog(GTK_WINDOW(parent), "program-name", SDE_ABOUT_NAME,
      "version", SDE_ABOUT_VERSION, "authors", authors,
      "license-type", GTK_LICENSE_GPL_2_0, "comments", "Native SDE build. Original authors and license notices are retained.", NULL);
}
static void sde_native_add_about(GtkWidget *box, GtkWindow *parent) {
  GtkWidget *button = gtk_button_new_with_label("About");
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  g_signal_connect(button, "clicked", G_CALLBACK(sde_native_about_button), parent);
  gtk_widget_show(button);
}
