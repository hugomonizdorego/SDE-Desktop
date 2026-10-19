// SPDX-License-Identifier: GPL-3.0-or-later
// Base class for the panel's pop-up windows: application menu, quick
// settings and calendar. Handles placement next to the panel (top or bottom),
// closing on Escape or on a click outside, and keyboard focus.
#pragma once

#include "buffered_window.h"

class PanelPopup : public sde::BufferedWindow {
 public:
  explicit PanelPopup(const char *title);

  // Opens the popup next to the panel. |anchor_x| is a root x coordinate;
  // with |align_right| the popup's right edge is placed at |anchor_x|.
  void open_at(int anchor_x, bool align_right);
  virtual void close();
  void reposition();
  bool is_open() const { return !closing_ && const_cast<PanelPopup *>(this)->shown(); }

  int handle(int event) override;

 protected:
  // Called before the window is placed; set the popup's size here.
  virtual void prepare() {}
  virtual bool corner_popup() const { return false; }
  virtual bool native_children() const { return false; }
  // Called after the popup closed.
  virtual void closed() {}

 private:
  static void refocus(void *data);
  bool closing_ = false;
  int anchor_x_ = 0;
  bool align_right_ = false;
};
