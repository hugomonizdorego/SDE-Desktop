// SPDX-License-Identifier: GPL-3.0-or-later
// One persistent XEmbed host moves between panel and Action Center.
#pragma once
#include <X11/Xlib.h>

#include <algorithm>
#include <string>
#include <vector>
class Tray {
 public:
  explicit Tray(Window panel);
  ~Tray();
  bool start();
  int icon_size() const { return icon_size_; }
  void set_icon_size(int size) {
    icon_size_ = size;
    spacing_ = std::max(4, size / 4);
  }
  int count() const { return int(icons_.size()); }
  int width() const;
  int grid_height(int columns) const;
  void place(int x, int height);
  void place_in(Window parent, int x, int y, int columns, int clip_top = 0,
                int clip_height = 100000);
  void hide();
  // Hidden inside the panel window (behind the overflow button): the host
  // must not stay in a popup that is about to be destroyed.
  void park();
  void set_background(unsigned long pixel);
  bool handle(const XEvent &e, bool &changed);
  // Lower-case WM_CLASS names whose icons are not docked because an SDE
  // indicator already shows the same state (nm-applet, blueman...).
  void set_hidden(std::vector<std::string> names) { hidden_ = std::move(names); }
  // XKB layout applets (gxkb, xxkb...) are not docked while SDE's keyboard
  // indicator is shown; an input method's icon (IBus, Fcitx) is.
  void set_hide_layout_applets(bool hide) { hide_layout_applets_ = hide; }
  bool has_input_method() const;

 private:
  bool hidden(Window icon) const;
  void dock(Window icon);
  void undock(Window icon, bool reparent_to_root);
  Window panel_, owner_ = 0, host_ = 0, parent_ = 0;
  unsigned long selection_ = 0;
  std::vector<Window> icons_;
  std::vector<std::string> hidden_;
  std::vector<std::string> classes_;  // WM_CLASS (lower case) of icons_
  bool hide_layout_applets_ = false;
  int icon_size_ = 22, spacing_ = 6;
};
