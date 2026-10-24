// SPDX-License-Identifier: GPL-3.0-or-later
// Base class for the panel's pop-up windows: application menu, Action
// Center and the status icon flyouts. Handles placement next to the panel (top or bottom),
// closing on Escape or on a click outside, and keyboard focus.
#pragma once

#include <chrono>

#include <string>

#include "buffered_window.h"

namespace sde {
class Painter;
struct Color;
}  // namespace sde
struct Fl_Menu_Item;

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
  // The X window is really on screen (not unmapped behind FLTK's back).
  bool viewable() const;
  // The keyboard reaches this popup (focus and, with a grab, the grabs).
  bool has_input() const;

  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  // alpha51: soft rounded corners (popup_style::radius()); a corner popup
  // keeps square corners on the side that touches the panel.
  int corner_radius() const;
  // Draws the popup's border along the rounded outline.
  void stroke_outline(sde::Painter &p, const sde::Color &c) const;
  // A context menu or dropdown of this popup at window coordinates |x|, |y|
  // (Fl_Menu_Item::popup()). 0.2 Alpha 2: typing a character there closes
  // it (nothing is chosen) and puts the text in |typed|, so that the caller
  // can search for it; afterwards this popup has the pointer and keyboard
  // grabs and the focus back (FLTK releases every grab when a popup menu
  // closes: clicks outside went to the desktop, keys could follow them).
  const Fl_Menu_Item *popup_menu(const Fl_Menu_Item *items, int x, int y,
                                 std::string *typed = nullptr);

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
  // alpha50: a screenshot tool (scrot -s, xfce4-screenshooter, flameshot)
  // that still holds the pointer and keyboard grabs when a popup opens made
  // FLTK's grab and the input focus fail silently: the search then took no
  // typing. ensure_input() checks the grabs and the focus and retries until
  // they hold (or the popup closes).
  static void ensure_input(void *data);
  bool grabbing_ = false;
  int input_tries_ = 0;
  bool closing_ = false;
  void apply_shape();
  int shaped_w_ = 0, shaped_h_ = 0;
  int anchor_x_ = 0;
  int align_ = 0;  // 0 left edge, 1 right edge, 2 center at anchor_x_
};

// The text a key press types: printable characters without Ctrl, Alt or
// Super (shortcuts type nothing), else "". When an input method keeps the
// text back, a plain Latin-1 key still types its character.
std::string typed_key_text();

// UI test hook: with SDE_PANEL_QA_DIR set, panel windows write the geometry of
// their named widgets (tooltip or label) to <dir>/<window title>.txt, one
// "name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>active<TAB>scrolled" line each, in window pixels.
// |extra| lines are written first. Does nothing without the variable.
bool qa_dump_enabled();
void qa_dump(Fl_Window *win, const std::string &extra = "");
// UI test hook: <dir>/<window title>.input exists while a popup holds the
// keyboard (its grabs and the input focus), so that tests type only then.
void qa_mark_input(Fl_Window *win, bool has_input);

// Shapes X window |xid| (|w| x |h|) with rounded corners of radius |r|;
// |corners|: 1 top-left, 2 top-right, 4 bottom-right, 8 bottom-left.
void apply_rounded_shape(unsigned long xid, int w, int h, int r, int corners = 15);
