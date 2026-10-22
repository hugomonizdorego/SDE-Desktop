// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Test helper (not installed): stock FLTK widgets, menus and dialogs in the
// SDE look, for screenshots and UI smoke tests.
//
//   sde-ui-kit-demo [--theme light|dark] [--accent #RRGGBB]
//                   [--show context|menubar|choice|ask|message|input|about|
//                           tooltip|files]
#include <FL/Fl.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/x.H>

#include <cstdio>
#include <cstring>
#include <string>

#include "about.h"
#include "config.h"
#include "i18n.h"
#include "menu.h"
#include "theme.h"

using namespace sde;

namespace {

Fl_Window *g_win;
Fl_Menu_Bar *g_bar;
Fl_Choice *g_choice;
Fl_Button *g_button;
std::string g_show;

void noop(Fl_Widget *, void *) {}

void about() {
  AppInfo info;
  info.name = "UI Kit Demo";
  info.program = "sde-ui-kit-demo";
  info.version = "0.1.0~alpha51";
  info.icon = "preferences-desktop-theme";
  info.description =
      "Shows FLTK menus, dialogs and widgets drawn in the SDE theme. Used by the SDE test suite.";
  info.credits = {toolkit_credit()};
  show_about(info);
}

void context_menu(int x, int y) {
  ContextMenu menu;
  EditActions edit;
  edit.cut = [] { puts("cut"); };
  edit.copy = [] { puts("copy"); };
  edit.paste = [] { puts("paste"); };
  edit.del = [] { puts("delete"); };
  edit.select_all = [] { puts("select all"); };
  edit.can_cut = false;
  menu.edit_actions(edit);
  menu.quick("Rename", "rename", [] { puts("rename"); });
  menu.quick("Share", "share", [] { puts("share"); });
  MenuItemOptions open;
  open.icon = "open";
  open.shortcut = FL_CTRL + 'o';
  menu.item("Open", [] { puts("open"); }, open);
  menu.item("Open with Niny", [] { puts("open with"); });
  menu.separator();
  menu.check("Show hidden files", true, [] { puts("hidden"); });
  ContextMenu sort;
  sort.radio("Name", true, [] {});
  sort.radio("Date", false, [] {});
  sort.radio("Size", false, [] {});
  menu.submenu("Sort by", std::move(sort));
  menu.separator();
  MenuItemOptions props;
  props.icon = "properties";
  props.shortcut = FL_ALT + FL_Enter;
  menu.item("Properties", [] { puts("properties"); }, props);
  MenuItemOptions off;
  off.enabled = false;
  menu.item("Unavailable", [] {}, off);
  menu.popup(x, y);
}

void show_cb(void *) {
  if (g_show == "context") {
    context_menu(g_win->x() + 220, g_win->y() + 110);
  } else if (g_show == "menubar") {
    const Fl_Menu_Item *edit = g_bar->menu()->next(1);
    g_bar->menu()->pulldown(g_bar->x(), g_bar->y(), g_bar->w(), g_bar->h(), edit, g_bar, 0, 1);
  } else if (g_show == "choice") {
    g_choice->menu()->pulldown(g_choice->x(), g_choice->y(), g_choice->w(), g_choice->h(),
                               g_choice->mvalue(), g_choice);
  } else if (g_show == "ask") {
    fl_choice("Delete \"Holiday 2026\" permanently?", fl_cancel, "Delete", nullptr);
  } else if (g_show == "message") {
    fl_message("The album was exported to Pictures/Exports.");
  } else if (g_show == "input") {
    fl_input("Album name:", "Holiday 2026");
  } else if (g_show == "about") {
    about();
  } else if (g_show == "files") {
    Fl_File_Chooser fc(".", "*", Fl_File_Chooser::SINGLE, "Open File");
    fc.show();
    while (fc.shown()) Fl::wait();
  } else if (g_show == "tooltip") {
    Fl_Tooltip::enter_area(g_button, 0, 0, g_button->w(), g_button->h(), g_button->tooltip());
  }
}

class DemoWindow : public Fl_Window {
 public:
  using Fl_Window::Fl_Window;
  int handle(int e) override {
    if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      context_menu(Fl::event_x(), Fl::event_y());
      return 1;
    }
    return Fl_Window::handle(e);
  }
};

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  std::string theme = "", accent = "";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
    else if (!strcmp(argv[i], "--accent") && i + 1 < argc) accent = argv[++i];
    else if (!strcmp(argv[i], "--show") && i + 1 < argc) g_show = argv[++i];
  }
  fl_open_display();
  Config cfg;
  cfg.load();
  if (!theme.empty()) cfg.set("appearance", "theme", theme);
  if (!accent.empty()) cfg.set("appearance", "accent", accent);
  Theme t = Theme::from_config(cfg);
  t.apply_to_fltk();

  int W = S(560), H = S(360);
  g_win = new DemoWindow(W, H, "UI Kit Demo");
  g_bar = new Fl_Menu_Bar(0, 0, W, S(30));
  g_bar->add("&File/&New", FL_CTRL + 'n', noop);
  g_bar->add("&File/&Open...", FL_CTRL + 'o', noop, nullptr, FL_MENU_DIVIDER);
  g_bar->add("&File/&Quit", FL_CTRL + 'q', [](Fl_Widget *, void *) { g_win->hide(); });
  g_bar->add("&Edit/&Undo", FL_CTRL + 'z', noop);
  g_bar->add("&Edit/&Redo", FL_CTRL + FL_SHIFT + 'z', noop, nullptr, FL_MENU_DIVIDER);
  g_bar->add("&Edit/Cu&t", FL_CTRL + 'x', noop);
  g_bar->add("&Edit/&Copy", FL_CTRL + 'c', noop);
  g_bar->add("&Edit/&Paste", FL_CTRL + 'v', noop, nullptr, FL_MENU_DIVIDER | FL_MENU_INACTIVE);
  g_bar->add("&Edit/Select &All", FL_CTRL + 'a', noop);
  g_bar->add("&View/Show &Sidebar", FL_F + 9, noop, nullptr, FL_MENU_TOGGLE | FL_MENU_VALUE);
  g_bar->add("&View/&Zoom/Zoom &In", FL_CTRL + '+', noop);
  g_bar->add("&View/&Zoom/Zoom &Out", FL_CTRL + '-', noop);
  g_bar->add("&Help/&About", 0, [](Fl_Widget *, void *) { about(); });
  set_menu_icon_for_label("Undo", "undo");
  set_menu_icon_for_label("Redo", "redo");
  set_menu_icon_for_label("Cut", "cut");
  set_menu_icon_for_label("Copy", "copy");
  set_menu_icon_for_label("Paste", "paste");
  set_menu_icon_for_label("Select All", "select-all");

  g_choice = new Fl_Choice(S(130), S(56), S(200), S(32), "Sort photos:");
  g_choice->add("By date taken|By name|By size|By rating");
  g_choice->value(0);

  auto *input = new Fl_Input(S(130), S(100), S(300), S(32), "Search:");
  input->value("Lisbon 2026");

  auto *browser = new Fl_Browser(S(130), S(144), S(300), S(150), "Albums:");
  browser->align(FL_ALIGN_LEFT);
  for (int i = 1; i <= 40; ++i) browser->add(format("Album %d", i).c_str());

  g_button = new Fl_Button(S(130), S(306), S(140), S(32), "Hover me");
  g_button->tooltip("Tooltips use the SDE theme too.");
  g_button->callback([](Fl_Widget *, void *) { fl_alert("Something went wrong."); });
  g_win->end();
  g_win->show();
  if (!g_show.empty()) Fl::add_timeout(0.6, show_cb);
  return Fl::run();
}
