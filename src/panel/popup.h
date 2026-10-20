// SPDX-License-Identifier: GPL-3.0-or-later
// Base class for the panel's pop-up windows: application menu, Action
// Center and the status icon flyouts. Handles placement next to the panel (top or bottom),
// closing on Escape or on a click outside, and keyboard focus.
#pragma once

#include <chrono>

#include <string>

#include "buffered_window.h"

class PanelPopup : public sde::BufferedWindow {
 public:
  // True when root coordinates are inside the popup, or inside where it was
  // just before its last resize (for clicks that arrive late).
  bool contains_root(int rx, int ry) const;
  explicit PanelPopup(const char *title);

  // Opens the popup next to the panel. |anchor_x| is a root x coordinate;
  // with |align_right| the popup's right edge is placed at |anchor_x|.
  void open_at(int anchor_x, bool align_right);
  // Opens the popup centered above (or below) root x |center_x|, kept on
  // the monitor: the flyouts of the panel's status icons.
  void open_centered(int center_x);
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
  int previous_x_ = 0, previous_y_ = 0, previous_w_ = 0, previous_h_ = 0;
  std::chrono::steady_clock::time_point previous_until_{};
  void open(int anchor_x, int align);
  static void refocus(void *data);
  bool closing_ = false;
  int anchor_x_ = 0;
  int align_ = 0;  // 0 left edge, 1 right edge, 2 center at anchor_x_
};

// UI test hook: with SDE_PANEL_QA_DIR set, panel windows write the geometry of
// their named widgets (tooltip or label) to <dir>/<window title>.txt, one
// "name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>active<TAB>scrolled" line each, in window pixels.
// |extra| lines are written first. Does nothing without the variable.
bool qa_dump_enabled();
void qa_dump(Fl_Window *win, const std::string &extra = "");
