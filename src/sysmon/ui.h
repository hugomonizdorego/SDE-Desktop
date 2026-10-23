// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE System Monitor: widgets and pages shared by the user interface files.
#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "paint.h"
#include "sampler.h"
#include "theme.h"

namespace sysmon {

using sde::Color;
using sde::Painter;
using sde::S;
using sde::Sd;

// Fixed hues for the resource kinds; they read well on light and dark.
struct Palette {
  Color cpu, memory, swap, disk, disk_write, net, net_send, gpu, battery, danger, warning;
};
const Palette &palette();

// Draws |c| at |x|,|y| filling |size| for the navigation and page icons.
enum class Icon { App, Processes, Performance, Disks, Startup, Search, Drive, Folder, Chip, Network, Battery, Memory, Gpu };
void draw_icon(Painter &p, Icon icon, double x, double y, double size, const Color &c);

// Last 60 seconds of a value (61 points, newest last).
struct Series {
  std::deque<double> values;
  void push(double v) {
    values.push_back(v);
    while (values.size() > 61) values.pop_front();
  }
  double last() const { return values.empty() ? 0 : values.back(); }
  double max() const;
};

// Smooth line graph with a soft fill. |top| is the value at the top edge.
// Draws |a| filled and the optional |b| as a second line.
void draw_graph(Painter &p, double x, double y, double w, double h, const Series &a,
                const Color &ca, const Series *b, const Color &cb, double top, bool grid = true);

// --------------------------------------------------------------- widgets

// Window that keeps an X pixmap back buffer and, unlike a full redraw on
// every change, only repaints the children that asked for it. A live graph
// updating once a second then costs one card, not the whole window.
class MonitorWindow : public Fl_Window {
 public:
  MonitorWindow(int w, int h, const char *label);
  ~MonitorWindow() override;
  void flush() override;
  void hide() override;
  void resize(int x, int y, int w, int h) override;
  void draw() override;

 private:
  void free_buffer();
  Fl_Offscreen buffer_ = 0;
  int buffer_w_ = 0, buffer_h_ = 0;
};

// Opaque single line of text (e.g. live page summaries).
class TextLine : public Fl_Widget {
 public:
  TextLine(int x, int y, int w, int h, int size_delta = 0, bool bold = false, bool dim = false);
  void text(const std::string &t);
  void draw() override;
  Color bg;

 private:
  std::string text_;
  int size_delta_;
  bool bold_, dim_;
};

// Pill shaped on/off button.
class Chip : public Fl_Button {
 public:
  Chip(int x, int y, int w, int h, const std::string &text);
  void draw() override;
  int handle(int event) override;
  Color bg;

 private:
  std::string text_;
};

// Text field with a rounded frame, a search glyph and a placeholder.
class SearchField : public Fl_Group {
 public:
  SearchField(int x, int y, int w, int h, const std::string &placeholder);
  Fl_Input *input() { return input_; }
  std::string value() const;
  void draw() override;
  Color bg;

 private:
  Fl_Input *input_;
  std::string placeholder_;
};

// --------------------------------------------------------------- pages

class Page : public Fl_Group {
 public:
  Page(int x, int y, int w, int h, const std::string &title);
  // A new sample is available. |visible| is false while another page shows.
  virtual void update(const std::shared_ptr<const Snapshot> &snap, bool visible) = 0;
  virtual void activated() {}
  // Keyboard shortcut for "find" (Ctrl+F).
  virtual void focus_search() {}
  void draw() override;
  const std::string &title() const { return title_; }

 protected:
  // Title drawn by draw(); subtitle_ is a TextLine children may update.
  int header_height() const { return S(76); }
  std::string title_;
  TextLine *subtitle_ = nullptr;
};

Page *make_processes_page(int x, int y, int w, int h, Sampler &sampler);
Page *make_performance_page(int x, int y, int w, int h);
Page *make_filesystems_page(int x, int y, int w, int h);
Page *make_startup_page(int x, int y, int w, int h);

// Opens |path| in the Liria file manager (or the preferred one).
void open_folder(const std::string &path);

}  // namespace sysmon
