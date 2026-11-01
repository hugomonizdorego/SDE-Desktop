// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// QA helper (not installed): the check and radio indicators of GTK 4 in the
// current theme (GTK_THEME), unchecked and checked, in a window and in a
// popover menu, each written as an RGBA PAM image for
// tests/test_theme_motion.py: OUTDIR/{window,menu}-{check,radio}-{off,on}.pam
//
//   sde-gtk4-indicator-probe OUTDIR
#include <gtk/gtk.h>

#include <cstdio>
#include <string>

namespace {

const char *g_dir;
int g_written = 0;

void save(GtkWidget *w, const std::string &name) {
  int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
  if (width <= 0 || height <= 0) return;
  GdkPaintable *paintable = gtk_widget_paintable_new(w);
  GtkSnapshot *snap = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snap, width, height);
  GskRenderNode *node = gtk_snapshot_free_to_node(snap);
  g_object_unref(paintable);
  if (!node) return;
  GskRenderer *renderer = gsk_cairo_renderer_new();
  if (!gsk_renderer_realize_for_display(renderer, gdk_display_get_default(), nullptr)) return;
  graphene_rect_t view = GRAPHENE_RECT_INIT(0, 0, float(width), float(height));
  GdkTexture *t = gsk_renderer_render_texture(renderer, node, &view);
  int tw = gdk_texture_get_width(t), th = gdk_texture_get_height(t);
  std::string data(size_t(tw) * th * 4, '\0');
  gdk_texture_download(t, reinterpret_cast<guchar *>(&data[0]), size_t(tw) * 4);
  std::string path = std::string(g_dir) + "/" + name + ".pam";
  if (FILE *f = fopen(path.c_str(), "wb")) {
    fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", tw, th);
    for (int i = 0; i < tw * th; ++i) {
      // Premultiplied BGRA (cairo's ARGB32 on little endian): straight RGBA.
      unsigned char b = data[i * 4], g = data[i * 4 + 1], r = data[i * 4 + 2], a = data[i * 4 + 3];
      if (a) r = (unsigned char)(r * 255 / a), g = (unsigned char)(g * 255 / a), b = (unsigned char)(b * 255 / a);
      unsigned char px[4] = {r, g, b, a};
      fwrite(px, 1, 4, f);
    }
    fclose(f);
    ++g_written;
  }
  g_object_unref(t);
  gsk_renderer_unrealize(renderer);
  g_object_unref(renderer);
  gsk_render_node_unref(node);
}

// Every check and radio indicator under |w|, named by where it is.
void collect(GtkWidget *w) {
  std::string css = gtk_widget_get_css_name(w);
  if (css == "check" || css == "radio") {
    bool menu = gtk_widget_get_ancestor(w, GTK_TYPE_POPOVER) != nullptr;
    bool on = (gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_CHECKED) != 0;
    save(w, std::string(menu ? "menu-" : "window-") + css + (on ? "-on" : "-off"));
  }
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) collect(c);
}

void settle() {
  for (int i = 0; i < 200; ++i) {
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    g_usleep(5000);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
    return 2;
  }
  g_dir = argv[1];
  gtk_init();
  GtkWidget *win = gtk_window_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_window_set_child(GTK_WINDOW(win), box);
  GtkWidget *check_off = gtk_check_button_new(), *check_on = gtk_check_button_new();
  GtkWidget *radio_off = gtk_check_button_new(), *radio_on = gtk_check_button_new();
  gtk_check_button_set_group(GTK_CHECK_BUTTON(radio_on), GTK_CHECK_BUTTON(radio_off));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(check_on), TRUE);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(radio_on), TRUE);
  for (GtkWidget *b : {check_off, check_on, radio_off, radio_on}) gtk_box_append(GTK_BOX(box), b);
  // A popover menu with a check item (a boolean action) and radio items (a
  // string action), like an application menu.
  GSimpleActionGroup *group = g_simple_action_group_new();
  GSimpleAction *hidden = g_simple_action_new_stateful("hidden", nullptr, g_variant_new_boolean(FALSE));
  GSimpleAction *shown = g_simple_action_new_stateful("shown", nullptr, g_variant_new_boolean(TRUE));
  GSimpleAction *sort = g_simple_action_new_stateful("sort", G_VARIANT_TYPE_STRING, g_variant_new_string("name"));
  for (GSimpleAction *a : {hidden, shown, sort}) g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
  gtk_widget_insert_action_group(win, "probe", G_ACTION_GROUP(group));
  GMenu *model = g_menu_new();
  g_menu_append(model, "Show Hidden Files", "probe.hidden");
  g_menu_append(model, "Show Sidebar", "probe.shown");
  g_menu_append(model, "By Name", "probe.sort::name");
  g_menu_append(model, "By Date", "probe.sort::date");
  GtkWidget *button = gtk_menu_button_new();
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(model));
  gtk_box_append(GTK_BOX(box), button);
  gtk_window_present(GTK_WINDOW(win));
  settle();
  gtk_menu_button_popup(GTK_MENU_BUTTON(button));
  settle();
  collect(win);
  // The popover is a surface of its own under the menu button.
  if (GtkPopover *pop = gtk_menu_button_get_popover(GTK_MENU_BUTTON(button))) collect(GTK_WIDGET(pop));
  fprintf(stderr, "%d indicators written\n", g_written);
  return g_written >= 8 ? 0 : 1;
}
