// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-window-menu: the window menu of SDE (0.2.0 Alpha 4, docs/WINDOWS.md).
//
// SDE WM (patch 0006) runs "sde-window-menu --window XID --at X,Y" where
// Openbox showed its client-menu: a right click on a title bar, the
// window's icon and Alt+Space. The menu is drawn by SDE in its menu style,
// theme and scale, with the items of the taskbar button's menu
// (window_menu.h). SDE WM opens Openbox's own menu when this program cannot
// be started.
//
//   --window XID  the window (default: the active one)
//   --at X,Y      the menu's top-left corner on the screen (default: the
//                 pointer)
//   --above Y     where the menu ends when there is no room below Y: the
//                 top of the window's frame (Alt+Space, the window icon)
//
// SDE_WINDOW_MENU_QA_DIR: the items, and where the menu is, are written to
// <dir>/window-menu.txt for the UI tests.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Window.H>
#include <FL/x.H>
#include <X11/Xatom.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "theme.h"
#include "util.h"
#include "window_menu.h"
#include "x11.h"

using namespace sde;

namespace {

int ignore_x_errors(Display *, XErrorEvent *) { return 0; }

// SDE WM starts this program from a key or button press and still holds
// that grab until the key or button is let go: the menu could not take the
// pointer and keyboard yet. Wait for them (at most |ms|) and keep them;
// FLTK's menu grab then only moves them to its window.
bool take_input(int ms) {
  Window root = RootWindow(fl_display, fl_screen);
  bool pointer = false, keyboard = false;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!(pointer && keyboard) && std::chrono::steady_clock::now() < end) {
    if (!pointer)
      pointer = XGrabPointer(fl_display, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess;
    if (!keyboard)
      keyboard = XGrabKeyboard(fl_display, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
    if (!(pointer && keyboard)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  XSync(fl_display, False);
  return pointer && keyboard;
}

// <qa>/window-menu.txt: "window:<xid>", "at:<x>,<y>", then "item:<label>"
// (":on" when checked, "\t0" when disabled), "sub:<submenu>/<label>".
void qa_dump(const Fl_Menu_Item *items, Window win, int x, int y) {
  const char *dir = getenv("SDE_WINDOW_MENU_QA_DIR");
  if (!dir || !*dir) return;
  char head[96];
  snprintf(head, sizeof head, "window:0x%lx\nat:%d,%d\n", win, x, y);
  std::string text = head;
  int depth = 0;
  std::string prefix;
  for (const Fl_Menu_Item *m = items; depth >= 0; ++m) {
    if (!m->text) {
      if (depth == 0) break;
      --depth;
      prefix.clear();
      continue;
    }
    bool on = (m->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)) && (m->flags & FL_MENU_VALUE);
    text += (depth ? "sub:" + prefix : std::string("item:")) + m->text + (on ? ":on" : "") +
            (m->active() ? "" : "\t0") + "\n";
    if (m->flags & FL_SUBMENU) {
      ++depth;
      prefix = std::string(m->text) + "/";
    }
  }
  std::string path = std::string(dir) + "/window-menu.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

// The height FLTK gives the menu (item heights plus the window border).
int menu_height(const Fl_Menu_Item *items) {
  int total = 0;
  for (const Fl_Menu_Item *m = items; m && m->text; m = m->next()) {
    int hh = 0;
    m->measure(&hh, nullptr);
    total += hh + 2;
  }
  return total + S(8);
}

AppInfo app_info() {
  AppInfo info;
  info.name = _("Window Menu");
  info.program = "sde-window-menu";
  info.version = SDE_VERSION;
  info.icon = "preferences-system-windows";
  info.description = _("The menu of a window: move, size, minimize, maximize, workspaces and close.");
  return info;
}

int usage(int status) {
  printf("%s", _("Usage: sde-window-menu [--window XID] [--at X,Y] [--above Y]\n\n"
                 "Shows the window menu of SDE (Restore, Move, Size, Minimize, Maximize,\n"
                 "Always on Top, Move to Workspace, Close) for a window, by default the\n"
                 "active one, at the pointer or at X,Y.\n"));
  return status;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  Window win = 0;
  int x = INT_MIN, y = INT_MIN, above = INT_MIN;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--window" && i + 1 < argc) {
      win = Window(strtoul(argv[++i], nullptr, 0));
    } else if (a == "--at" && i + 1 < argc) {
      if (sscanf(argv[++i], "%d,%d", &x, &y) != 2) return usage(2);
    } else if (a == "--above" && i + 1 < argc) {
      above = atoi(argv[++i]);
    } else if (a == "--version") {
      return print_version(app_info());
    } else if (a == "--help" || a == "-h") {
      return usage(0);
    } else {
      return usage(2);
    }
  }
  // No input method: a menu has no text to type, and an input method that
  // is busy only delays the first key.
  {
    const char *im = getenv("XMODIFIERS");
    std::string saved = im ? im : "";
    setenv("XMODIFIERS", "@im=none", 1);
    fl_open_display();
    if (im) setenv("XMODIFIERS", saved.c_str(), 1);
    else unsetenv("XMODIFIERS");
  }
  if (!fl_display) {
    fprintf(stderr, "sde-window-menu: cannot open the display\n");
    return 1;
  }
  XSetErrorHandler(ignore_x_errors);
  Window root = RootWindow(fl_display, fl_screen);
  if (!win) win = x11::get_window(root, "_NET_ACTIVE_WINDOW");
  XWindowAttributes attr;
  if (!win || !XGetWindowAttributes(fl_display, win, &attr)) {
    fprintf(stderr, "sde-window-menu: no window\n");
    return 1;
  }
  Config cfg;
  cfg.load();
  Theme theme = Theme::from_config(cfg);
  theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));

  window_menu::State st = window_menu::read(win);
  ContextMenu menu;
  window_menu::add_items(menu, st);

  if (x == INT_MIN || y == INT_MIN) {
    Window r, child;
    int wx, wy;
    unsigned mask;
    XQueryPointer(fl_display, root, &r, &child, &x, &y, &wx, &wy, &mask);
  }
  take_input(3000);
  // Below the title bar, or above the window when the menu does not fit.
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, x, y);
  int mh = menu_height(menu.items());
  if (above != INT_MIN && y + mh > sy + sh && above - mh >= sy) y = above - mh;
  // FLTK places a menu relative to a window of its program: a 1x1 window
  // under the menu's corner, fully transparent for a compositor.
  auto *anchor = new Fl_Window(x, y, 1, 1);
  anchor->border(0);
  anchor->set_override();
  anchor->end();
  anchor->show();
  {
    unsigned long clear = 0;
    XChangeProperty(fl_display, fl_xid(anchor), x11::atom("_NET_WM_WINDOW_OPACITY"), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<unsigned char *>(&clear), 1);
  }
  qa_dump(menu.items(), win, x, y);
  // Root coordinates: no event has made another window the reference yet.
  menu.popup(x, y);
  anchor->hide();
  XUngrabPointer(fl_display, CurrentTime);
  XUngrabKeyboard(fl_display, CurrentTime);
  XSync(fl_display, False);
  return 0;
}
