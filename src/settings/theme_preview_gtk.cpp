// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-theme-preview-gtk: the GTK part of Control Center > Appearance >
// Preview (0.2.0 Alpha 3.1, src/settings/theme_preview.h). Real GTK 3
// widgets in a GtkPlug that the preview window embeds (XEmbed), drawn with
// the chosen GTK theme, icon theme and the CSS block SDE would write for it.
// Nothing of the user's settings changes: the theme is set for this process
// only (GtkSettings), above what XSETTINGS says.
//
//   sde-theme-preview-gtk --socket XID --theme NAME [--icons NAME]
//                         [--css FILE] [--dark | --light] [--labels L1|L2|...]
//
// --labels: the texts of the widgets, translated by the preview window
// (SDE's message catalog), in the order of kLabels below.
//
// Lines on standard input change what is shown: "theme NAME dark|light",
// "icons NAME", "css FILE"; "quit" or the end of the input ends it.
#include <gtk/gtk.h>
#include <gtk/gtkx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vector>

namespace {

GtkCssProvider *g_css = nullptr;

// The English texts; --labels replaces them in this order.
std::vector<std::string> kLabels = {"Files",       "Recent",       "Shared",     "Search files", "Documents",
                                    "Pictures",    "Music",        "Downloads",  "Show hidden files",
                                    "Thumbnails",  "Zoom",         "Open With...", "Rename...", "Properties",
                                    "More",        "Cancel",       "Save"};
const char *L(size_t i) { return kLabels[i].c_str(); }

void set_theme(const std::string &name, bool dark) {
  g_object_set(gtk_settings_get_default(), "gtk-theme-name", name.c_str(), "gtk-application-prefer-dark-theme",
               dark ? TRUE : FALSE, nullptr);
}

void set_icons(const std::string &name) {
  g_object_set(gtk_settings_get_default(), "gtk-icon-theme-name", name.c_str(), nullptr);
}

void load_css(const std::string &path) {
  // Above the user's own gtk.css, which holds the block of the look in use.
  if (!g_css) {
    g_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(g_css),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
  }
  gtk_css_provider_load_from_path(g_css, path.c_str(), nullptr);
}

gboolean on_input(GIOChannel *channel, GIOCondition, gpointer) {
  gchar *line = nullptr;
  GIOStatus status = g_io_channel_read_line(channel, &line, nullptr, nullptr, nullptr);
  if (status == G_IO_STATUS_EOF || status == G_IO_STATUS_ERROR) {
    gtk_main_quit();
    return FALSE;
  }
  if (!line) return TRUE;
  std::string text = g_strchomp(line);
  g_free(line);
  if (text.rfind("theme ", 0) == 0) {
    std::string rest = text.substr(6);
    size_t sp = rest.find(' ');
    set_theme(rest.substr(0, sp), sp != std::string::npos && rest.substr(sp + 1) == "dark");
  } else if (text.rfind("icons ", 0) == 0) {
    set_icons(text.substr(6));
  } else if (text.rfind("css ", 0) == 0) {
    load_css(text.substr(4));
  } else if (text == "quit") {
    gtk_main_quit();
    return FALSE;
  }
  return TRUE;
}

GtkWidget *labelled(const char *text, GtkWidget *control) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *label = gtk_label_new(text);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(row), control, FALSE, FALSE, 0);
  return row;
}

// Typical widgets of a GTK application: view switcher, search, a list with
// a selected row, a check button, a switch, a slider, buttons (one
// suggested) and a menu button with a popover menu.
GtkWidget *build() {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  g_object_set(box, "margin", 12, nullptr);

  GtkWidget *views = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(views), "linked");
  GtkWidget *first = nullptr;
  for (const char *name : {L(0), L(1), L(2)}) {
    GtkWidget *b = first ? gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(first), name)
                         : gtk_radio_button_new_with_label(nullptr, name);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
    if (!first) first = b;
    gtk_box_pack_start(GTK_BOX(views), b, TRUE, TRUE, 0);
  }
  gtk_box_pack_start(GTK_BOX(box), views, FALSE, FALSE, 0);

  GtkWidget *search = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search), L(3));
  gtk_box_pack_start(GTK_BOX(box), search, FALSE, FALSE, 0);

  GtkWidget *frame = gtk_frame_new(nullptr);
  GtkWidget *list = gtk_list_box_new();
  for (const char *name : {L(4), L(5), L(6), L(7)}) {
    GtkWidget *label = gtk_label_new(name);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    g_object_set(label, "margin", 5, nullptr);
    gtk_list_box_insert(GTK_LIST_BOX(list), label, -1);
  }
  gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 1));
  gtk_container_add(GTK_CONTAINER(frame), list);
  gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);

  GtkWidget *check = gtk_check_button_new_with_label(L(8));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), TRUE);
  gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
  GtkWidget *sw = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(sw), TRUE);
  gtk_box_pack_start(GTK_BOX(box), labelled(L(9), sw), FALSE, FALSE, 0);
  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_range_set_value(GTK_RANGE(scale), 40);
  gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
  gtk_widget_set_size_request(scale, 140, -1);
  gtk_box_pack_start(GTK_BOX(box), labelled(L(10), scale), FALSE, FALSE, 0);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GMenu *model = g_menu_new();
  g_menu_append(model, L(11), "app.none");
  g_menu_append(model, L(12), "app.none");
  g_menu_append(model, L(13), "app.none");
  GtkWidget *more = gtk_menu_button_new();
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), G_MENU_MODEL(model));
  gtk_button_set_label(GTK_BUTTON(more), L(14));
  GtkWidget *cancel = gtk_button_new_with_label(L(15));
  GtkWidget *save = gtk_button_new_with_label(L(16));
  gtk_style_context_add_class(gtk_widget_get_style_context(save), "suggested-action");
  gtk_box_pack_start(GTK_BOX(buttons), more, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(buttons), save, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(buttons), cancel, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
  return box;
}

}  // namespace

int main(int argc, char **argv) {
  unsigned long socket = 0;
  std::string theme, icons, css;
  bool dark = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--socket" && i + 1 < argc) socket = strtoul(argv[++i], nullptr, 0);
    else if (a == "--theme" && i + 1 < argc) theme = argv[++i];
    else if (a == "--icons" && i + 1 < argc) icons = argv[++i];
    else if (a == "--css" && i + 1 < argc) css = argv[++i];
    else if (a == "--dark") dark = true;
    else if (a == "--light") dark = false;
    else if (a == "--labels" && i + 1 < argc) {
      std::string all = argv[++i];
      size_t k = 0, start = 0;
      while (k < kLabels.size() && start <= all.size()) {
        size_t bar = all.find('|', start);
        std::string one = all.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (!one.empty()) kLabels[k] = one;
        ++k;
        if (bar == std::string::npos) break;
        start = bar + 1;
      }
    }
  }
  // The preview is an X window: GtkPlug needs the X11 backend.
  gdk_set_allowed_backends("x11");
  if (!gtk_init_check(&argc, &argv) || !socket) {
    fprintf(stderr, "sde-theme-preview-gtk: needs an X display and --socket\n");
    return 1;
  }
  if (!theme.empty()) set_theme(theme, dark);
  if (!icons.empty()) set_icons(icons);
  if (!css.empty()) load_css(css);
  GtkWidget *plug = gtk_plug_new(Window(socket));
  g_signal_connect(plug, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_container_add(GTK_CONTAINER(plug), build());
  gtk_widget_show_all(plug);
  GIOChannel *in = g_io_channel_unix_new(0);
  g_io_add_watch(in, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), on_input, nullptr);
  gtk_main();
  return 0;
}
