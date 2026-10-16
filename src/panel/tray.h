// SPDX-License-Identifier: GPL-3.0-or-later
// Freedesktop.org System Tray Protocol host (XEmbed icons), so that
// applications such as nm-applet or volume controls appear in the panel.
#pragma once

#include <X11/Xlib.h>

#include <algorithm>
#include <vector>

class Tray {
 public:
  explicit Tray(Window panel);
  ~Tray();

  // Becomes the _NET_SYSTEM_TRAY_S<screen> selection owner.
  bool start();

  int icon_size() const { return icon_size_; }
  void set_icon_size(int size) {
    icon_size_ = size;
    spacing_ = std::max(4, size / 4);
  }
  int width() const;
  // Moves the icons to start at |x|, vertically centered in |height|.
  void place(int x, int height);

  // Returns true when the event belonged to the tray. |changed| is set when
  // the panel has to re-layout (icon added or removed).
  bool handle(const XEvent &e, bool &changed);

 private:
  void dock(Window icon);
  void undock(Window icon, bool reparent_to_root);

  Window panel_;
  Window owner_ = 0;
  unsigned long selection_ = 0;
  std::vector<Window> icons_;
  int icon_size_ = 22;
  int spacing_ = 6;
};
