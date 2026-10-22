// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Calculator user interface: themed keys, the expression field and the main
// window (Standard, Scientific, Programmer and Converter modes, History).
#pragma once

#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "converter.h"
#include "engine.h"
#include "paint.h"
#include "state.h"

namespace calc {

// Line icons drawn with cairo in a size x size box.
enum class Glyph { Backspace, History, More, Menu, Swap, Chevron, Trash };
void draw_glyph(sde::Painter &p, Glyph g, double x, double y, double size, const sde::Color &c);

// A keypad key: rounded, themed, with hover and press feedback.
class Key : public Fl_Widget {
 public:
  enum Kind { Digit, Operator, Function, Equals, Memory };

  Key(int x, int y, int w, int h, std::string id, std::string label, Kind kind);
  const std::string &id() const { return id_; }
  void set_label(const std::string &l) {
    label_ = l;
    redraw();
  }
  const std::string &text() const { return label_; }
  void set_toggled(bool on) {
    if (on != toggled_) redraw();
    toggled_ = on;
  }
  void set_glyph(bool on) { glyph_ = on; }
  // Shows the pressed look briefly (the key was typed on the keyboard).
  void flash();
  std::function<void(const std::string &)> on_press;
  std::string tooltip_text;

  void draw() override;
  int handle(int event) override;

 private:
  static void unflash(void *data);
  std::string id_, label_;
  Kind kind_;
  bool down_ = false, toggled_ = false, glyph_ = false;
};

// A borderless icon button of the title row.
class IconButton : public Fl_Widget {
 public:
  IconButton(int x, int y, int w, int h, Glyph g, std::string text = "");
  std::function<void()> on_press;
  void set_text(const std::string &t) {
    text_ = t;
    redraw();
  }
  void set_toggled(bool on) {
    if (on != toggled_) redraw();
    toggled_ = on;
  }
  void draw() override;
  int handle(int event) override;

 private:
  Glyph glyph_;
  std::string text_;
  bool toggled_ = false, down_ = false;
};

class CalcWindow;

// The expression line; keys, pastes and edits go through the window.
class ExprInput : public Fl_Input {
 public:
  ExprInput(int x, int y, int w, int h, CalcWindow *owner);
  int handle(int event) override;
  void draw() override;

 private:
  CalcWindow *owner_;
};

class CalcWindow : public sde::BufferedWindow {
 public:
  CalcWindow(int w, int h, const Settings &settings);
  ~CalcWindow() override;

  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

  // Keyboard input from the expression field (1: used).
  int key_event();
  // Inserts pasted text (sanitized) at the cursor.
  void paste_text(const std::string &text);
  // The user edited the field directly (typing, cut, undo).
  void input_changed();

  void set_mode(Mode m);
  void apply_theme();
  void show_about();
  static int x_event_handler(int);

 private:
  friend class ExprInput;
  // Keypad and actions.
  void build_keys();
  void press(const std::string &id);
  void insert(const std::string &text, bool continues = false);
  void set_expression(const std::string &text);
  void equals();
  void clear_all();
  void clear_entry();
  void backspace();
  void negate();
  void apply_function(const std::string &name);
  void memory(const std::string &op);
  bool current_value(Number &out);
  void flash_key_for(const std::string &id);

  // Display.
  void layout();
  void update();
  void write_qa() const;
  void draw_title(sde::Painter &p);
  void draw_display(sde::Painter &p);
  void draw_display_text();
  void draw_programmer(sde::Painter &p);
  void draw_programmer_text();
  void draw_converter(sde::Painter &p);
  void draw_converter_text();
  void draw_history(sde::Painter &p);
  void draw_history_text();
  int history_hit(int x, int y) const;
  void history_menu(int index);
  void reuse_history(size_t index, bool result_only);

  // Menus and clipboard.
  void mode_menu();
  void more_menu();
  void display_menu();
  void copy_result();
  void copy_expression();
  void paste();
  void toggle_history();
  void show_shortcuts();

  // Programmer and converter helpers.
  void set_base(int base);
  void set_bits(int bits);
  void toggle_bit(int bit);
  int programmer_bit_at(int x, int y) const;
  int base_row_at(int x, int y) const;
  void fill_unit_choices();
  void convert_now();
  void swap_units();

  bool history_docked() const;
  int calc_width() const;

  Settings settings_;
  Mode mode_ = Mode::Standard;
  Context ctx_;
  History history_;
  sde::Theme theme_;

  ExprInput *input_ = nullptr;
  IconButton *mode_button_ = nullptr, *history_button_ = nullptr, *more_button_ = nullptr;
  std::vector<Key *> keys_;
  Fl_Choice *category_ = nullptr, *from_ = nullptr, *to_ = nullptr;
  Key *swap_ = nullptr;

  // Display state.
  std::string result_;      // shown result (display formatted)
  std::string raw_result_;  // the same, plain (copy, QA, continuing)
  std::string error_;
  bool evaluated_ = false;  // the result of "=" is shown
  bool preview_ = false;    // result_ is a live preview
  bool second_ = false, hyp_ = false;
  uint64_t pvalue_ = 0;
  bool pvalid_ = true;
  Number conv_value_ = 0;
  bool conv_ok_ = false;

  // Geometry (window coordinates).
  int title_h_ = 0, display_y_ = 0, display_h_ = 0, chips_y_ = 0;
  int bases_y_ = 0, bits_y_ = 0, bits_h_ = 0, keypad_y_ = 0;
  int conv_from_y_ = 0, conv_to_y_ = 0;
  int angle_x_ = 0, angle_w_ = 0, word_x_ = 0, word_w_ = 0;
  int history_x_ = 0, history_w_ = 0;
  bool history_overlay_ = false;
  int history_scroll_ = 0;
  int history_hover_ = -1;
  int history_clear_x_ = 0, history_clear_y_ = 0, history_clear_s_ = 0;
  std::vector<std::pair<int, int>> history_rows_;  // y, h of each visible entry (newest first)
};

}  // namespace calc
