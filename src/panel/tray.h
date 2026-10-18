// SPDX-License-Identifier: GPL-3.0-or-later
// One persistent XEmbed host moves between panel and Action Center.
#pragma once
#include <X11/Xlib.h>

#include <algorithm>
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
  void set_background(unsigned long pixel);
  bool handle(const XEvent &e, bool &changed);

 private:
  void dock(Window icon);
  void undock(Window icon, bool reparent_to_root);
  Window panel_, owner_ = 0, host_ = 0, parent_ = 0;
  unsigned long selection_ = 0;
  std::vector<Window> icons_;
  int icon_size_ = 22, spacing_ = 6;
};
