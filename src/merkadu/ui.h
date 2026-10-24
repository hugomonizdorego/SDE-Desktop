// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's widgets in the SDE UI kit look (docs/UI-KIT.md): cards and
// tiles like the Control Center's, source badges, app rows with their
// action button, filter chips and the screenshot strip. They follow
// active_theme() and the UI scale; widgets that show an app look it up by
// id in the Store whenever they draw, so they always show its current state.
#pragma once

#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>

#include <functional>
#include <string>
#include <vector>

#include "model.h"
#include "paint.h"

namespace sde {
namespace merkadu {

// The color of a source's badge (calm red, blue and orange).
Color source_color(Source s);
// Text readable on |bg| in |c|'s hue.
Color readable(const Color &c, const Color &bg);
// A pill with the source's name; returns its width. |x|, |y| top left.
int badge_width(const std::string &text);
void paint_badge(Painter &p, double x, double y, double w, double h, const Color &c);
void draw_badge_text(const std::string &text, int x, int y, int w, int h, const Color &c);
// The badges of an app's sources from |x|; returns where they end. The
// cairo part and the text part are separate calls (paint first).
int paint_source_badges(Painter &p, const App &a, int x, int y, int h, int max_w);
void draw_source_badges(const App &a, int x, int y, int h, int max_w);
void paint_progress(Painter &p, double x, double y, double w, double h, int percent, double phase);
// A rounded card like the Control Center's tiles.
void paint_card(Painter &p, double x, double y, double w, double h, bool hot, bool focus);
// Small line symbols for the sidebar and the history.
enum class Symbol {
  Explore, Installed, Updates, History, Progress, Sources, Search, Back, Check, Plus, Minus, Refresh, Warning,
  Download, Trash, Shield, Folder, Star, Clock, Close
};
void paint_symbol(Painter &p, Symbol s, double x, double y, double size, const Color &c);
// The app's icon at |size|.
cairo_surface_t *app_icon(const App &a, int size);

// A push button. Primary: the accent; Danger: the theme's error color;
// Ghost: no background until hovered.
class Button : public Fl_Button {
 public:
  enum Kind { Normal, Primary, Danger, Ghost };
  Button(int x, int y, int w, int h, const std::string &text, Kind kind = Normal);
  void text(const std::string &t);
  const std::string &text() const { return text_; }
  void kind(Kind k) {
    kind_ = k;
    redraw();
  }
  // A width that fits the text.
  static int fit(const std::string &text);
  std::function<void()> on_click;
  void draw() override;
  int handle(int e) override;

 private:
  std::string text_;
  Kind kind_;
};

// A toggle in a row of filters ("All", "Debian", ...).
class Chip : public Fl_Widget {
 public:
  Chip(int x, int y, int w, int h, const std::string &text, bool on);
  static int fit(const std::string &text);
  std::function<void()> on_click;
  Color dot;  // a colored dot before the text (alpha 0: none)
  void draw() override;
  int handle(int e) override;

 private:
  std::string text_;
  bool on_;
};

// A card with an app: icon, name, summary and source badges.
class AppTile : public Fl_Widget {
 public:
  AppTile(int x, int y, int w, int h, const std::string &app_id);
  std::function<void(const std::string &)> on_open;
  void draw() override;
  int handle(int e) override;
  const std::string &app_id() const { return id_; }

 private:
  std::string id_;
  bool hot_ = false;
};

// A list row with an app and its action: Install, Open, Remove, Update,
// or the progress of its change.
class AppRow : public Fl_Widget {
 public:
  enum Mode { Browse, Installed, Updates };
  AppRow(int x, int y, int w, int h, const std::string &app_id, Mode mode);
  std::function<void(const std::string &)> on_open;
  std::function<void(const std::string &)> on_action;
  void draw() override;
  int handle(int e) override;
  // The action button's box, for the QA dump.
  void action_box(int *bx, int *by, int *bw, int *bh) const;
  std::string action_text() const;

 private:
  std::string id_;
  Mode mode_;
  bool hot_ = false, hot_button_ = false;
};

// The featured app: a large card with its icon, name, summary and a
// screenshot when there is one.
class Banner : public Fl_Widget {
 public:
  Banner(int x, int y, int w, int h, const std::string &app_id);
  std::function<void(const std::string &)> on_open;
  void draw() override;
  int handle(int e) override;

 private:
  std::string id_;
  bool hot_ = false;
};

// A department: its icon, name and number of apps.
class CategoryTile : public Fl_Widget {
 public:
  CategoryTile(int x, int y, int w, int h, const std::string &title, const std::string &icon, int count,
               bool compact);
  std::function<void()> on_open;
  void draw() override;
  int handle(int e) override;

 private:
  std::string title_, icon_;
  int count_;
  bool compact_, hot_ = false;
};

// Screenshots side by side; a click shows one large (on_open).
class ScreenshotStrip : public Fl_Widget {
 public:
  ScreenshotStrip(int x, int y, int w, int h, const std::vector<Screenshot> &shots);
  std::function<void(int index)> on_open;
  void draw() override;
  int handle(int e) override;
  static int preferred_height(int w);

 private:
  int shot_at(int ex) const;
  std::vector<Screenshot> shots_;
  int hover_ = -1;
};

// A themed text input with a placeholder, inside a rounded field with a
// magnifier (make_search_field() creates both).
class SearchField : public Fl_Input {
 public:
  SearchField(int x, int y, int w, int h, const std::string &placeholder);
  void restyle();
  void draw() override;
  int handle(int e) override;
  std::function<void()> on_enter, on_escape;
  Fl_Widget *frame = nullptr;

 private:
  std::string placeholder_;
};
// The field (x, y, w, h) and its input, in the current group.
SearchField *make_search_field(int x, int y, int w, int h, const std::string &placeholder);

// Wrapped text of several paragraphs; its height comes from measure().
class TextBlock : public Fl_Widget {
 public:
  TextBlock(int x, int y, int w, int h, const std::string &text, bool dim = false, int size_delta = 0);
  static int measure(const std::string &text, int w, int size_delta = 0);
  void draw() override;

 private:
  std::string text_;
  bool dim_;
  int delta_;
};

// Shows one screenshot in a window of its own.
void show_screenshot(const std::vector<Screenshot> &shots, int index, const std::string &title);

}  // namespace merkadu
}  // namespace sde
