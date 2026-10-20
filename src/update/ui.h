// SPDX-License-Identifier: GPL-3.0-or-later
// SDE Update widgets: cards, badges, vector icons, a virtual list view, a
// search field and a themed text view. They follow active_theme(), the SDE
// theme family (SDE, Aero, Liquid Glass) and the UI scale.
#pragma once

#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Widget.H>

#include <functional>
#include <string>

#include "model.h"
#include "paint.h"

namespace sde {
class Config;
namespace update {

// The SDE theme family member in use, from [appearance] style (falling back
// to [aero] enabled for older configuration files).
struct Look {
  std::string style = "aero";  // sde, aero or liquid-glass
  bool aero() const { return style == "aero"; }
  bool liquid() const { return style == "liquid-glass"; }
  double card_radius() const;
  double control_radius(double h) const;
  static Look from_config(const Config &cfg);
};
const Look &look();
void set_look(const Look &l);

// Colors for update kinds; text() variants are readable on the surface.
Color kind_color(Kind kind);
Color kind_text(Kind kind);
Color readable_on(const Color &c, const Color &bg);
Color success_color();
Color warning_color();
Color danger_color();

// Backgrounds.
void paint_window_background(Painter &p, int w, int h);
void paint_card(Painter &p, double x, double y, double w, double h, bool hero = false);
void paint_progress(Painter &p, double x, double y, double w, double h, int percent, double phase);
void paint_checkbox(Painter &p, double x, double y, double size, int state, bool hover);
void paint_spinner(Painter &p, double cx, double cy, double r, double phase, const Color &c);

// Badges: measure, then paint the pill (cairo) and the label (FLTK) separately.
int badge_width(const std::string &text);
void paint_badge(Painter &p, double x, double y, double w, double h, const Color &c);
void draw_badge_text(const std::string &text, int x, int y, int w, int h, const Color &c);

enum class Icon {
  Updates, History, Sources, Maintenance, Settings, About, Shield, Download, Warning,
  Search, Restart, Trash, Broom, Wrench, Terminal, Package, Clock, Close, Info, Check
};
void paint_icon(Painter &p, Icon icon, double x, double y, double size, const Color &c);

// Push button in the SDE style. Liquid Glass makes it a pill.
class Button : public Fl_Button {
 public:
  enum Kind { Normal, Primary, Danger, Ghost };
  Button(int x, int y, int w, int h, const std::string &text, Kind kind = Normal);
  void text(const std::string &t);
  const std::string &text() const { return text_; }
  void icon(Icon i) {
    icon_ = i;
    has_icon_ = true;
  }
  void kind(Kind k) {
    kind_ = k;
    redraw();
  }
  // Width that fits the label (and icon).
  int preferred_width() const;
  void draw() override;
  int handle(int event) override;

 private:
  std::string text_;
  Kind kind_;
  Icon icon_ = Icon::Info;
  bool has_icon_ = false;
};

// Text input with a rounded background, a search icon and a placeholder.
class SearchField : public Fl_Input {
 public:
  SearchField(int x, int y, int w, int h, const std::string &placeholder);
  void draw() override;
  int handle(int event) override;

 private:
  std::string placeholder_;
};

// Scrollable list with rows of variable height drawn by callbacks.
class ListView : public Fl_Widget {
 public:
  ListView(int x, int y, int w, int h);
  std::function<int()> count;
  std::function<int(int)> row_height;
  std::function<bool(int)> selectable;  // headers are not
  // Cairo layer and FLTK text layer of one row.
  std::function<void(Painter &, int, int, int, int, int, bool, bool)> paint_row;
  std::function<void(int, int, int, int, int, bool, bool)> text_row;
  // A click on |row| at |x| relative to the row; |clicks| is 1 or 2.
  std::function<void(int row, int x, int clicks)> clicked;
  std::function<void(int row)> selection_changed;
  std::function<void(int row)> toggled;  // space bar
  // Shown when count() is 0.
  std::string empty_title, empty_text;
  Icon empty_icon = Icon::Info;
  bool loading = false;
  double phase = 0;  // spinner animation
  bool framed = true;

  int selected() const { return selected_; }
  void select(int row, bool notify = true);
  void reset_scroll() { scroll_ = 0; }
  void ensure_visible(int row);
  void draw() override;
  int handle(int event) override;

 private:
  int content_height() const;
  int row_top(int row) const;
  int row_at(int y) const;
  void clamp_scroll();
  void move_selection(int delta);
  int scroll_ = 0, selected_ = -1, hover_ = -1;
  bool dragging_bar_ = false;
  int drag_offset_ = 0;
};

// Read-only text with themed colors (monospace for changelogs and logs).
class TextView : public Fl_Text_Display {
 public:
  TextView(int x, int y, int w, int h, bool mono = true);
  ~TextView() override;
  void set(const std::string &text);
  void append(const std::string &text);
  void scroll_to_end();

 private:
  Fl_Text_Buffer *buffer_;
};

void style_choice(Fl_Choice *c);
std::string menu_escape(const std::string &s);

// A modal question in the SDE style. |details| is shown in a text box when
// not empty. Returns true when the user chose |accept|.
bool confirm(Fl_Window *parent, const std::string &title, const std::string &message,
             const std::string &details, const std::string &accept, bool danger = false);
void inform(Fl_Window *parent, const std::string &title, const std::string &message);
// A non-modal window with a transaction log.
void show_log(Fl_Window *parent, const std::string &title, const std::string &text);

}  // namespace update
}  // namespace sde
