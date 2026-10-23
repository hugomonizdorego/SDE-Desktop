// SPDX-License-Identifier: GPL-3.0-or-later
// The user's account in the panel: the picture and the name of the Start
// menu's account button and of the Action Center, and the account popup of
// the Start menu.
//
// The account (AccountsService RealName, the sudo group) and the picture
// (src/common/avatar.h) are looked up on a worker thread when the panel
// starts and again when SDE announces a change (_SDE_AVATAR_SERIAL: the
// Control Center, the popup itself). Opening the menu costs one stat() of
// the picture file at most.
#pragma once

#include <FL/Fl_Window.H>
#include <cairo.h>

#include <string>
#include <vector>

#include "theme.h"

namespace panel_account {

// The account as last looked up.
const std::string &name();  // the real name, else the login name
const std::string &user();
bool admin();
const std::string &picture_path();  // "" without a picture
// The picture at |size| x |size| pixels, or nullptr (draw the initials).
cairo_surface_t *picture(int size);
// Looks the account up again in the background, then repaints the panel's
// windows. Called at start-up and for _SDE_AVATAR_SERIAL.
void reload();
// Re-reads the picture when its file changed (cheap; the menu opening).
void check();

}  // namespace panel_account

// The Start menu's account popup: the picture and the name, the stock
// pictures (a click uses one at once), "Choose a picture..." and "Account
// settings..." (the Control Center). It is drawn inside the menu window next
// to the account button, so the menu keeps its pointer and keyboard grab;
// the menu forwards its events while it is open.
class AccountPopup {
 public:
  AccountPopup() = default;
  ~AccountPopup();
  AccountPopup(const AccountPopup &) = delete;
  AccountPopup &operator=(const AccountPopup &) = delete;

  bool is_open() const { return open_; }
  // Opens next to the account button (|ax|, |ay|, |ah|: its right edge, top
  // and height in |host| coordinates).
  void open(Fl_Window *host, int ax, int ay, int ah);
  void close();
  // An event of the host window while open. Returns true when the popup used
  // it (the host must not handle it then).
  bool handle(int event);
  // Paints the popup on top of the menu: the shapes, then the text.
  void draw_shapes(const sde::Theme &t);
  void draw_text(const sde::Theme &t);
  // Lines for the menu's QA dump ("@account-popup", "account:...").
  std::string qa_lines() const;

 private:
  enum Kind { NONE, AVATAR, CHOOSE, SETTINGS };
  struct Item {
    Kind kind = NONE;
    int index = -1;  // AVATAR: the stock picture
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  void layout();
  int item_at(int px, int py) const;
  void activate(int i);
  void pick(const std::string &path);

  Fl_Window *host_ = nullptr;
  bool open_ = false;
  int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
  int anchor_x_ = 0, anchor_y_ = 0, anchor_h_ = 0;
  std::vector<std::string> stock_;
  std::vector<Item> items_;
  int hover_ = -1, focus_ = -1, pressed_ = -1;
  bool keyboard_ = false;
  bool busy_ = false;
  std::string status_;
  bool status_error_ = false;
};
