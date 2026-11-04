// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// 0.2.0 Alpha 4: the taskbar's keyboard shortcuts, as on Windows
// (docs/SHORTCUTS.md). SDE WM runs sde-panel with them; the running panel
// gets them as _SDE_COMMAND:
//
//   Win+1 ... Win+9        "taskbar:N": the app at place N (the visible
//                          pinned apps, then the windows): start it, switch
//                          to its window (several: the next one), or
//                          minimize it when it is the active one
//   Win+Shift+1 ... 9      "taskbar-new:N": another window of that app
//   Win+T                  "focus-taskbar": the taskbar takes the keyboard;
//                          each Win+T, Tab or arrow key goes to the next
//                          button (with its preview), Enter opens it, the
//                          Menu key or Shift+F10 shows its menu, Esc goes
//                          back to the window that was active
//   Win+X                  "quick-link-menu": the Start button's menu (the
//                          Quick Link menu) above the Start button
#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>
#include <functional>

#include "panel.h"
#include "popup.h"
#include "preview.h"
#include "spawn.h"
#include "x11.h"

using namespace sde;

namespace {
// The keyboard mode of Win+T (one panel per process).
bool g_keys = false;
int g_index = 0;
Window g_previous = 0;
bool g_handler = false;

// A shortcut arrives while SDE WM still holds the keyboard from its key
// press (until the key is let go): a menu opened now would not get the
// keys. |fn| runs once the panel can have the keyboard (at most ~1.2 s).
struct Deferred {
  std::function<void()> fn;
  int tries = 60;
};
void when_keyboard_free(void *data) {
  auto *d = static_cast<Deferred *>(data);
  Window w = Panel::instance ? fl_xid(Panel::instance) : 0;
  bool free = w && XGrabKeyboard(fl_display, w, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
  if (free || --d->tries <= 0 || !w) {
    std::function<void()> fn = std::move(d->fn);
    delete d;
    if (fn) fn();
    // FLTK's menu grab took the keyboard over and gave it back; nothing
    // may keep it after the menu.
    XUngrabKeyboard(fl_display, CurrentTime);
    XFlush(fl_display);
    return;
  }
  Fl::repeat_timeout(0.02, when_keyboard_free, d);
}
void with_keyboard(std::function<void()> fn) {
  auto *d = new Deferred;
  d->fn = std::move(fn);
  Fl::add_timeout(0.0, when_keyboard_free, d);
}
}  // namespace

void Panel::taskbar_shortcut(int position, bool new_window) {
  int index = position - 1;
  if (index < 0) return;
  if (index < visible_pins_ && index < int(pinned_.size())) {
    const DesktopEntry entry = pinned_[size_t(index)].entry;
    std::vector<int> windows;
    for (size_t i = 0; i < tasks_.size(); ++i) {
      DesktopEntry e;
      if (find_entry_for(tasks_[i], e) && e.id == entry.id) windows.push_back(int(i));
    }
    if (new_window || windows.empty()) {
      launch_entry(entry);
      return;
    }
    // Its windows in turn: after the active one, or the first.
    auto active = std::find_if(windows.begin(), windows.end(), [&](int i) { return tasks_[size_t(i)].win == active_; });
    if (windows.size() == 1 || active == windows.end()) {
      task_clicked(active == windows.end() ? windows.front() : *active, FL_LEFT_MOUSE);
      return;
    }
    ++active;
    x11::activate_window(tasks_[size_t(active == windows.end() ? windows.front() : *active)].win);
    return;
  }
  index -= visible_pins_;
  if (index >= int(tasks_.size())) return;
  if (new_window) {
    DesktopEntry e;
    if (find_entry_for(tasks_[size_t(index)], e)) launch_entry(e);
    return;
  }
  task_clicked(index, FL_LEFT_MOUSE);
}

void Panel::taskbar_key_focus(int index) {
  if (tasks_.empty()) return;
  g_index = (index % int(tasks_.size()) + int(tasks_.size())) % int(tasks_.size());
  hover_ = Target{TASK, g_index};
  update_tooltip();
  redraw();
  if (previews_enabled()) show_preview(g_index);
}

void Panel::taskbar_keys_end(bool back) {
  if (!g_keys) return;
  g_keys = false;
  hover_ = Target{};
  if (preview_ && preview_->showing()) preview_->hide_now();
  redraw();
  if (back && g_previous) x11::activate_window(g_previous);
}

int Panel::taskbar_key_handler(int event) {
  Panel *p = instance;
  if (!g_keys || !p || (event != FL_SHORTCUT && event != FL_KEYBOARD)) return 0;
  int key = Fl::event_key();
  int index = g_index;
  switch (key) {
    case FL_Escape:
      p->taskbar_keys_end(true);
      return 1;
    case FL_Left:
    case FL_Up:
      p->taskbar_key_focus(index - 1);
      return 1;
    case FL_Right:
    case FL_Down:
      p->taskbar_key_focus(index + 1);
      return 1;
    case FL_Tab:
      p->taskbar_key_focus(Fl::event_shift() ? index - 1 : index + 1);
      return 1;
    case FL_Home:
      p->taskbar_key_focus(0);
      return 1;
    case FL_End:
      p->taskbar_key_focus(int(p->tasks_.size()) - 1);
      return 1;
    case FL_Enter:
    case FL_KP_Enter:
    case ' ':
      p->taskbar_keys_end(false);
      if (index < int(p->tasks_.size())) p->task_clicked(index, FL_LEFT_MOUSE);
      return 1;
    case FL_Menu:
    case FL_F + 10:
      if (key == FL_F + 10 && !Fl::event_shift()) return 0;
      if (index < int(p->tasks_.size())) {
        p->taskbar_keys_end(false);
        // The menu above the button: FLTK takes the pointer position of
        // the last event as the reference.
        Fl::e_x_root = p->x() + p->tasks_[size_t(index)].x;
        Fl::e_x = p->tasks_[size_t(index)].x;
        Fl::e_y_root = p->y();
        Fl::e_y = 0;
        p->task_menu(index);
      }
      return 1;
    default:
      return 0;
  }
}

void Panel::focus_taskbar() {
  if (tasks_.empty()) return;
  if (!g_handler) {
    Fl::add_handler(taskbar_key_handler);
    // The mode ends when another window takes the keyboard (a click on it,
    // or Enter on a button).
    Fl::add_timeout(0.3, [](void *) {
      struct Tick {
        static void run(void *) {
          if (g_keys && Panel::instance) {
            Window focus = 0;
            int revert = 0;
            XGetInputFocus(fl_display, &focus, &revert);
            if (focus != fl_xid(Panel::instance)) Panel::instance->taskbar_keys_end(false);
          }
          Fl::repeat_timeout(0.3, run);
        }
      };
      Tick::run(nullptr);
    });
    g_handler = true;
  }
  if (g_keys) {
    taskbar_key_focus(g_index + 1);
    return;
  }
  if (open_popup_) open_popup_->close();
  set_revealed(true);
  g_previous = active_;
  g_keys = true;
  taskbar_key_focus(0);
  with_keyboard([this] {
    XUngrabKeyboard(fl_display, CurrentTime);
    if (g_keys) XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
  });
}

void Panel::quick_link_menu() {
  if (open_popup_) open_popup_->close();
  set_revealed(true);
  with_keyboard([this] {
    // Above (or below a top panel) the Start button, as a right click on it.
    int x0 = x() + S(4);
    Fl::e_x_root = x0;
    Fl::e_x = S(4);
    Fl::e_y_root = y();
    Fl::e_y = 0;
    start_context_menu();
  });
}
