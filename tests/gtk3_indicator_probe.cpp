// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// QA helper (not installed): the check and radio indicators of GTK 3 in the
// current theme (GTK_THEME), unchecked and checked, in a window and in a
// popup menu, each written as an RGBA PAM image for
// tests/test_theme_motion.py: OUTDIR/{window,menu}-{check,radio}-{off,on}.pam
//
//   sde-gtk3-indicator-probe OUTDIR
#include <gtk/gtk.h>
#include <cstdio>

static void save(GtkWidget *w, const char *dir, const char *name) {
  int width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(s);
  gtk_widget_draw(w, cr);
  cairo_destroy(cr);
  cairo_surface_flush(s);
  char path[4096];
  snprintf(path, sizeof path, "%s/%s.pam", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return;
  }
  fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", width, height);
  const unsigned char *d = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      unsigned int p = *(const unsigned int *)(d + y * stride + x * 4);
      unsigned char a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
      // Premultiplied: back to straight colors.
      if (a) {
        r = (unsigned char)(r * 255 / a);
        g = (unsigned char)(g * 255 / a);
        b = (unsigned char)(b * 255 / a);
      }
      unsigned char px[4] = {r, g, b, a};
      fwrite(px, 1, 4, f);
    }
  fclose(f);
  cairo_surface_destroy(s);
}

static void settle(void) {
  for (int i = 0; i < 200; ++i) {
    while (gtk_events_pending()) gtk_main_iteration();
    g_usleep(5000);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
    return 2;
  }
  gtk_init(&argc, &argv);
  const char *dir = argv[1];
  GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_container_add(GTK_CONTAINER(win), box);
  GtkWidget *check_off = gtk_check_button_new(), *check_on = gtk_check_button_new();
  GtkWidget *radio_off = gtk_radio_button_new(NULL);
  GtkWidget *radio_on = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(radio_off));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_on), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_on), TRUE);
  GtkWidget *buttons[] = {check_off, check_on, radio_off, radio_on};
  for (int i = 0; i < 4; ++i) {
    gtk_widget_set_can_focus(buttons[i], FALSE);
    gtk_box_pack_start(GTK_BOX(box), buttons[i], FALSE, FALSE, 0);
  }
  gtk_widget_show_all(win);
  settle();
  save(check_off, dir, "window-check-off");
  save(check_on, dir, "window-check-on");
  save(radio_off, dir, "window-radio-off");
  save(radio_on, dir, "window-radio-on");
  // The same in a popup menu ("Show Hidden Files" in Liria's menu).
  GtkWidget *menu = gtk_menu_new();
  GtkWidget *m_check_off = gtk_check_menu_item_new(), *m_check_on = gtk_check_menu_item_new();
  GtkWidget *m_radio_off = gtk_radio_menu_item_new(NULL);
  GtkWidget *m_radio_on = gtk_radio_menu_item_new_from_widget(GTK_RADIO_MENU_ITEM(m_radio_off));
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(m_check_on), TRUE);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(m_radio_on), TRUE);
  GtkWidget *items[] = {m_check_off, m_check_on, m_radio_off, m_radio_on};
  for (int i = 0; i < 4; ++i) gtk_menu_shell_append(GTK_MENU_SHELL(menu), items[i]);
  gtk_widget_show_all(menu);
  GdkRectangle at = {10, 10, 1, 1};
  gtk_menu_popup_at_rect(GTK_MENU(menu), gtk_widget_get_window(win), &at, GDK_GRAVITY_NORTH_WEST,
                         GDK_GRAVITY_NORTH_WEST, NULL);
  settle();
  if (!gtk_widget_get_mapped(menu)) {
    fprintf(stderr, "the menu did not show\n");
    return 1;
  }
  save(m_check_off, dir, "menu-check-off");
  save(m_check_on, dir, "menu-check-on");
  save(m_radio_off, dir, "menu-radio-off");
  save(m_radio_on, dir, "menu-radio-on");
  return 0;
}
