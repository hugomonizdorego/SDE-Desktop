// SPDX-License-Identifier: GPL-3.0-or-later
// Building blocks for sde-settings pages: a vertically scrolling page that
// stacks headings, cards and labelled rows, all sized with the UI scale.
#pragma once

#include <FL/Fl_Choice.H>
#include <FL/Fl_Scroll.H>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "widgets.h"

namespace sde {

class Config;

// Text drawn in the theme colors. |wrap| lines break at the widget width.
class Label : public Fl_Widget {
 public:
  enum Style { NORMAL, BOLD, DIM, TITLE, SUBTITLE };
  Label(int x, int y, int w, int h, const std::string &text, Style style = NORMAL, bool wrap = false);
  void draw() override;
  void set(const std::string &t);
  // Height needed for |text| at width |w|.
  static int measure(const std::string &text, int w, Style style);

 private:
  std::string text_;
  Style style_;
  bool wrap_;
};

// Rounded card behind a group of rows.
class Card : public Fl_Widget {
 public:
  Card(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override;
};

class Page : public Fl_Scroll {
 public:
  Page(int x, int y, int w, int h);

  int left() const { return x() + pad_; }
  int width() const { return w() - 2 * pad_ - scrollbar_size(); }
  int cursor() const { return y_; }
  void advance(int dy) { y_ += dy; }

  void heading(const std::string &title, const std::string &subtitle = "");
  void begin_card(const std::string &title = "");
  void end_card();
  // Free-standing paragraph.
  void note(const std::string &text);

  // A row: label on the left, a control of |control_w| on the right.
  // |make| creates the control at the given box.
  Fl_Widget *row(const std::string &label, int control_w, int control_h,
                 const std::function<Fl_Widget *(int, int, int, int)> &make,
                 const std::string &hint = "");
  Switch *switch_row(const std::string &label, bool value, std::function<void(bool)> changed,
                     const std::string &hint = "");
  Fl_Choice *choice_row(const std::string &label, const std::vector<std::string> &options, int value,
                        std::function<void(int)> changed, const std::string &hint = "");
  Slider *slider_row(const std::string &label, double min, double max, double value,
                     std::function<void(double)> changed, const std::string &hint = "");

  // Ends the page; call once all rows are added.
  void finish();

  // Names the current position (a card about to begin) so that the page can
  // be opened scrolled to it ("--page proxy" shows the Proxy card).
  void anchor(const std::string &id) { anchors_[id] = y_ - y(); }
  // Offset of anchor |id| from the top of the page, or -1.
  int anchor_offset(const std::string &id) const {
    auto it = anchors_.find(id);
    return it == anchors_.end() ? -1 : it->second;
  }

 private:
  std::map<std::string, int> anchors_;
  int pad_, y_;
  Card *card_ = nullptr;
  int card_y_ = 0;
  bool first_row_ = true;
};

// Styles a stock Fl_Choice with the active theme.
void style_choice(Fl_Choice *c);

// Escapes '/', '&', '_' and '\' for FLTK menu labels.
std::string menu_escape(const std::string &s);

// Keeps callback data alive for the lifetime of the program.
template <class T>
T *keep(T *p) {
  static std::vector<std::shared_ptr<void>> store;
  store.emplace_back(std::shared_ptr<T>(p));
  return p;
}

}  // namespace sde
