// SPDX-License-Identifier: GPL-2.0-or-later
#define SDE_UPSTREAM_NAME "pluma"
#define SDE_UPSTREAM_URL "https://github.com/mate-desktop/pluma"
#define SDE_ABOUT_NAME "Niny"
#define SDE_ABOUT_VERSION "1.0.0~alpha2"
#define SDE_ORIGINAL_AUTHORS "Original author"
#include "../suite/sde-about.h"
#include <cassert>
#include <cstring>

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  const char *authors[] = {"Original developer", nullptr};
  gtk_show_about_dialog(nullptr, "program-name", "Niny", "authors", authors,
                        "copyright", "Original copyright", nullptr);
  GList *windows = gtk_window_list_toplevels();
  int found = 0;
  for (GList *p = windows; p; p = p->next) {
    if (!GTK_IS_ABOUT_DIALOG(p->data)) continue;
    ++found;
    auto *about = GTK_ABOUT_DIALOG(p->data);
    assert(strcmp(gtk_about_dialog_get_authors(about)[0], "Original developer") == 0);
    assert(strcmp(gtk_about_dialog_get_copyright(about), "Original copyright") == 0);
    assert(strcmp(gtk_about_dialog_get_website(about), SDE_UPSTREAM_URL) == 0);
    assert(strcmp(gtk_about_dialog_get_website_label(about), "pluma") == 0);
    sde_native_about_apply(GTK_WIDGET(about), nullptr);  // Safe on repeated opens.
    gtk_widget_destroy(GTK_WIDGET(about));
  }
  g_list_free(windows);
  assert(found == 1);
  auto *about = gtk_about_dialog_new();
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about), authors);
  gtk_widget_show(about);
  assert(g_object_get_data(G_OBJECT(about), "sde-about-credited"));
  assert(strcmp(gtk_about_dialog_get_authors(GTK_ABOUT_DIALOG(about))[0], "Original developer") == 0);
  gtk_widget_destroy(about);
}
