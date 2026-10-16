// SPDX-License-Identifier: GPL-3.0-or-later
// Themed widgets shared by SDE programs. They follow active_theme(), scale
// with the UI scale and show a focus ring for keyboard users.
#pragma once

#include <FL/Fl_Button.H>
#include <FL/Fl_Slider.H>

#include <functional>
#include <string>

#include "paint.h"

namespace sde {

// Draws the keyboard focus ring around a rounded box.
void focus_ring(Painter &p, double x, double y, double w, double h, double r);

class Slider : public Fl_Slider {
 public:
  Slider(int x, int y, int w, int h, const char *label = nullptr);
  void draw() override;
  int handle(int event) override;
};

// On/off switch; value() is the state.
class Switch : public Fl_Button {
 public:
  Switch(int x, int y, int w, int h, const char *label = nullptr);
  void draw() override;
  int handle(int event) override;
};

// Rounded push button. primary uses the accent color.
class FlatButton : public Fl_Button {
 public:
  FlatButton(int x, int y, int w, int h, const std::string &text, bool primary = false);
  void text(const std::string &t) {
    text_ = t;
    redraw();
  }
  void draw() override;
  int handle(int event) override;

 private:
  std::string text_;
  bool primary_;
};

// Button whose look is drawn by a callback (tiles, list rows, icon buttons).
class DrawnButton : public Fl_Button {
 public:
  using DrawFn = std::function<void(Painter &p, bool hover, bool down)>;
  // Text drawn after the cairo layer.
  using TextFn = std::function<void(bool hover)>;
  DrawnButton(int x, int y, int w, int h, DrawFn draw, TextFn text = nullptr, double radius = -1);
  void draw() override;
  int handle(int event) override;

 private:
  DrawFn draw_fn_;
  TextFn text_fn_;
  double radius_;
};

}  // namespace sde
