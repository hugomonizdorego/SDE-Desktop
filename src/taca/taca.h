// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Taca, the SDE archive manager: user interface declarations.
#pragma once

#include <FL/Fl_Input.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Widget.H>

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "buffered_window.h"
#include "engine.h"
#include "model.h"
#include "paint.h"
#include "theme.h"
#include "widgets.h"

namespace taca {

// --------------------------------------------------------------- icons
namespace icon {
enum Kind {
  Box, Open, New, Extract, Add, AddFolder, Trash, Rename, Shield, Info, Search, List, Grid, Home,
  Chevron, Lock, Close, Folder, Check, Warning, Up, Plus
};
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);
// A file card with a tinted corner and an extension label (label drawn by
// the caller with FLTK after the painter is gone, see file_label_box()).
void file(sde::Painter &p, const std::string &name, double x, double y, double size, bool dir,
          const sde::Color &accent, bool dark);
void file_label_box(const std::string &name, double x, double y, double size, int &lx, int &ly, int &lw,
                    int &lh);
sde::Color category_color(int category, const sde::Color &accent);
// The format "badge": a rounded square in the accent gradient.
void badge(sde::Painter &p, double x, double y, double size, const sde::Color &accent);
}  // namespace icon

// Applies the theme to a text field (drawn inside a rounded pill by its
// parent, see draw_field()).
void style_input(Fl_Input *in, const sde::Theme &t);
void draw_field(sde::Painter &p, Fl_Widget *in, const sde::Theme &t, int pad_left = 0);

// A row of options; one is chosen. Options can be disabled.
class Chips : public Fl_Widget {
 public:
  Chips(int x, int y, int w, int h, std::vector<std::string> labels);
  void draw() override;
  int handle(int event) override;
  int value() const { return value_; }
  void value(int v) {
    value_ = v;
    redraw();
  }
  void enable(int i, bool on);
  bool enabled(int i) const { return i >= 0 && i < int(labels_.size()) && !off_.count(i); }
  void set_labels(std::vector<std::string> l) {
    labels_ = std::move(l);
    redraw();
  }
  int rows_needed(int width) const;

 private:
  struct Cell {
    int x, y, w, h;
  };
  std::vector<Cell> layout() const;
  std::vector<std::string> labels_;
  std::set<int> off_;
  int value_ = 0, hover_ = -1, focus_ = 0;
};

// ------------------------------------------------------------------ jobs

// Runs |work| on a worker thread; |done| runs on the main loop afterwards.
// The interface never waits for archive work.
class Job {
 public:
  using Work = std::function<Result(Progress &)>;
  using Done = std::function<void(const Result &)>;
  Job(const std::string &title, Work work, Done done);
  ~Job();
  const std::string &title() const { return title_; }
  Progress &progress() { return progress_; }
  bool finished() const { return finished_; }
  void cancel() { progress_.cancel = true; }

 private:
  static void awake_cb(void *p);
  std::string title_;
  Work work_;
  Done done_;
  Progress progress_;
  Result result_;
  std::thread thread_;
  std::atomic<bool> finished_{false};
};

class TacaWindow;

// ------------------------------------------------------------ file view

class FileView : public Fl_Widget {
 public:
  FileView(TacaWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;

  void set_items(std::vector<int> items, bool keep_selection);
  const std::vector<int> &items() const { return items_; }
  std::vector<int> selection() const;
  void select_all();
  void clear_selection();
  bool grid() const { return grid_; }
  void set_grid(bool g);
  int focus_item() const { return focus_ >= 0 && focus_ < int(items_.size()) ? items_[size_t(focus_)] : -1; }
  void scroll_to_top() { scroll_ = 0; }

 private:
  int header_h() const;
  int row_h() const;
  int tile_w() const;
  int tile_h() const;
  int columns() const;
  int content_h() const;
  int item_at(int mx, int my) const;
  void item_rect(int i, int &ix, int &iy, int &iw, int &ih) const;
  void ensure_visible(int i);
  void clamp_scroll();
  void click(int index, int state);
  bool keyboard(int key, int state);
  void draw_list(sde::Painter *p, bool text_pass);
  void draw_grid(sde::Painter *p, bool text_pass);
  void column_x(int &name, int &size, int &packed, int &type, int &modified, int &end) const;
  int header_column_at(int mx) const;

  TacaWindow *app_;
  std::vector<int> items_;
  std::vector<char> selected_;
  bool grid_ = false;
  int scroll_ = 0, hover_ = -1, focus_ = -1, anchor_ = -1;
  int press_x_ = 0, press_y_ = 0, press_item_ = -1;
  bool dragging_bar_ = false, drag_started_ = false;
  int bar_offset_ = 0;
};

// ------------------------------------------------------------ dialogs

// A themed modal window. run() returns the index of the button pressed
// (0 = cancel / closed).
class Dialog : public sde::BufferedWindow {
 public:
  Dialog(int w, int h, const std::string &title, const std::string &subtitle = "");
  void draw() override;
  int handle(int event) override;
  int run(Fl_Window *parent);
  void finish(int code);
  // Buttons along the bottom edge, right aligned. The last is the default.
  void buttons(const std::vector<std::string> &labels, int primary);
  // Text labels drawn by the dialog at fixed positions.
  void label(int x, int y, int w, const std::string &text, bool dim = false, bool bold = false);
  void field(Fl_Widget *w) { fields_.push_back(w); }
  const sde::Theme &theme() const { return theme_; }
  std::function<void()> on_change;
  std::function<void(sde::Painter &)> extra_paint;
  std::function<void()> extra_text;
  std::function<bool(int)> accept;  // validates before closing with a code

 protected:
  struct Label {
    int x, y, w;
    std::string text;
    bool dim, bold;
  };
  sde::Theme theme_;
  std::string title_, subtitle_;
  std::vector<Label> labels_;
  std::vector<Fl_Widget *> fields_;
  std::vector<sde::FlatButton *> buttons_;
  int code_ = 0;
  bool done_ = false;
};

// Asks for a password. Returns false when cancelled.
bool ask_password(Fl_Window *parent, const std::string &archive, bool wrong, std::string &out);
// Message with up to three buttons; returns the button index (0 = first).
int message(Fl_Window *parent, const std::string &title, const std::string &text,
            const std::vector<std::string> &buttons, int primary = -1, bool warning = false);
// Asks for a new name. Returns false when cancelled.
bool ask_name(Fl_Window *parent, const std::string &title, const std::string &current, std::string &out);

struct ExtractChoice {
  std::string dest;
  bool selected_only = false;
  bool keep_structure = true;
  Overwrite overwrite = Overwrite::Rename;
  bool open_after = true;
};
bool ask_extract(Fl_Window *parent, const std::string &archive, size_t selected, ExtractChoice &c);

struct CreateChoice {
  std::string path;  // full path of the new archive
  CreateOptions options;
};
bool ask_create(Fl_Window *parent, const std::vector<std::string> &files, CreateChoice &c);
void show_properties(Fl_Window *parent, const ArchiveInfo &info, const std::string &password_note);

// Directory and file choosers.
std::string choose_folder(const std::string &title, const std::string &start);
std::vector<std::string> choose_files(const std::string &title, const std::string &start, bool folders,
                                      bool archives);

// ------------------------------------------------------------ main window

class TacaWindow : public sde::BufferedWindow {
 public:
  TacaWindow(int w, int h);
  ~TacaWindow() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

  const sde::Theme &theme() const { return theme_; }
  const Tree &tree() const { return tree_; }
  const ArchiveInfo &info() const { return info_; }
  bool has_archive() const { return !info_.path.empty(); }
  bool busy() const { return job_ != nullptr; }
  SortKey sort_key() const { return sort_; }
  bool sort_descending() const { return descending_; }
  void set_sort(SortKey key);
  bool searching() const;

  void open_archive(const std::string &path);
  void close_archive();
  void enter(int node);  // folder: browse into it; file: open it
  void go_up();
  void refresh_view(bool keep_selection = false);
  void selection_changed();
  void context_menu(int node);
  void start_drag();

  // Actions.
  void action_open();
  void action_new(const std::vector<std::string> &files = {});
  void action_extract(bool selected_only);
  void action_add(bool folders);
  void add_paths(const std::vector<std::string> &paths);
  void action_delete();
  void action_rename();
  void action_test();
  void action_properties();
  void open_member(int node);

  void toast(const std::string &message);
  void apply_theme();

  // Extraction from the command line (no archive view).
  static std::vector<std::string> recent();
  static void remember(const std::string &path);

 private:
  void layout();
  void run_job(const std::string &title, Job::Work work, Job::Done done);
  void job_finished();
  // Retries |again| with a password when |r| asks for one; true if handled.
  bool password_retry(const Result &r, std::function<void()> again);
  void show_result(const std::string &what, const Result &r);
  void reload();
  std::vector<std::string> selected_paths() const;
  std::string current_folder() const;
  void draw_welcome(sde::Painter &p);
  void draw_welcome_text();
  void draw_sidebar(sde::Painter &p);
  void draw_sidebar_text();
  void draw_header(sde::Painter &p);
  void draw_header_text();
  void draw_progress(sde::Painter &p);
  void draw_progress_text();
  int sidebar_action_at(int mx, int my) const;
  int crumb_at(int mx, int my) const;
  int recent_at(int mx, int my) const;
  void run_sidebar_action(int a);
  static void tick_cb(void *p);
  static void toast_cb(void *p);
  static int x_event_handler(int event);
  static int system_handler(void *event, void *data);

  sde::Theme theme_;
  ArchiveInfo info_;
  Tree tree_;
  int folder_ = 0;
  std::string password_;
  SortKey sort_ = SortKey::Name;
  bool descending_ = false;
  std::unique_ptr<Job> job_;
  std::string toast_;
  std::vector<std::string> recent_;

  FileView *view_ = nullptr;
  Fl_Input *search_ = nullptr;
  Chips *mode_ = nullptr;
  sde::FlatButton *cancel_ = nullptr;
  sde::FlatButton *open_btn_ = nullptr, *new_btn_ = nullptr;
  int sidebar_w_ = 0, header_h_ = 0;
  int hover_action_ = -1, hover_crumb_ = -1, hover_recent_ = -1;
  bool drop_hover_ = false;
  bool dragging_out_ = false;
  std::string drag_uris_;
  static TacaWindow *instance_;
};

// The small window used by "taca --extract-here" and friends.
int run_cli_job(const std::string &title, Job::Work work, bool quiet_success,
                std::function<void(const Result &)> done = nullptr);

std::string uri_to_path(const std::string &uri);
std::string path_to_uri(const std::string &path);
std::vector<std::string> parse_uri_list(const std::string &text);

}  // namespace taca
