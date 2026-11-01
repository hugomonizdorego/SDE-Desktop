// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The SDE file chooser's window (file_chooser.h). Everything is drawn with
// sde::Painter in the active theme; the stock parts (the filter menu, the
// hidden files check box, the text fields' context menus, the scroll bar)
// come from the SDE UI kit. Folders are read and thumbnails made on worker
// threads (file_chooser_model.h), which wake the FLTK loop through a pipe.
#define FL_INTERNALS 1
#include "file_chooser.h"

#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "buffered_window.h"
#include "config.h"
#include "file_chooser_model.h"
#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "paint.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"
#include "x11.h"

namespace sde {

using chooser::Entry;
using chooser::Filter;
using chooser::Place;
using chooser::SortKey;

struct FileChooser::Options {
  Mode mode = Mode::Open;
  std::string title, folder, default_folder, file_name, accept_label, program;
  std::vector<Filter> filters;
  int all_files = -1;  // -1: by mode
  int filter = -1;     // -1: the one that fits the proposed name, else the first
  int preview = -1;  // -1: the user's last choice
  bool confirm_overwrite = true, new_folder = true, recent = true;
  Fl_Window *parent = nullptr;
  unsigned long parent_xid = 0;
  void *dialog = nullptr;  // while run() waits
};

namespace {

using Mode = FileChooser::Mode;

double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool is_open(Mode m) { return m == Mode::Open || m == Mode::OpenMultiple; }
bool is_folder_mode(Mode m) { return m == Mode::Folder || m == Mode::Folders; }
bool is_multi(Mode m) { return m == Mode::OpenMultiple || m == Mode::Folders; }
bool real_folder(const std::string &location) { return !location.empty() && location[0] == '/'; }

std::string take_gstr(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

// "1,234" in the locale's way.
std::string thousands(long n) { return take_gstr(g_strdup_printf("%'ld", n)); }

// "~/Documents" for folders below the home folder.
std::string home_relative(const std::string &path) {
  std::string home = home_dir();
  if (path == home) return "~";
  if (!home.empty() && home != "/" && starts_with(path, home + "/")) return "~" + path.substr(home.size());
  return path;
}

std::string display_path(const std::string &path) {
  return take_gstr(g_filename_display_name(home_relative(path).c_str()));
}

size_t next_char(const std::string &s, size_t i) {
  if (i >= s.size()) return s.size();
  ++i;
  while (i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80) ++i;
  return i;
}

// A name in at most two lines of |width| pixels (the font must be set):
// broken after a space when it can be (else after a dash, an underscore or
// a dot), the second line shortened with an ellipsis.
std::vector<std::string> wrap_name(const std::string &s, int width) {
  if (text_width(s) <= width) return {s};
  size_t fit = 0, space = std::string::npos, dash = std::string::npos, dot = std::string::npos;
  for (size_t i = 0; i < s.size();) {
    size_t n = next_char(s, i);
    if (text_width(s.substr(0, n)) > width) break;
    fit = n;
    char c = s[i];
    if (c == ' ') space = n;
    else if (c == '-' || c == '_') dash = n;
    else if (c == '.') dot = n;
    i = n;
  }
  size_t cut = fit;
  if (space != std::string::npos && space > fit / 3) cut = space;
  else if (dash != std::string::npos && dash > fit / 3) cut = dash;
  else if (dot != std::string::npos && dot > fit / 2) cut = dot;
  if (cut == 0) cut = next_char(s, 0);
  std::string first = s.substr(0, cut), rest = s.substr(cut);
  while (!first.empty() && first.back() == ' ') first.pop_back();
  while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
  if (rest.empty()) return {first};
  return {first, ellipsize(rest, width)};
}

// ------------------------------------------------------------------ glyphs

// Line icons on a 24 x 24 grid, drawn |s| pixels wide at (x, y). |weight|
// is the line width in grid units.
void glyph(Painter &p, const std::string &name, double x, double y, double s, const Color &c, double weight = 1.7) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s / 24.0, s / 24.0);
  p.set(c);
  cairo_set_line_width(cr, weight);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_new_path(cr);
  auto M = [&](double a, double b) { cairo_move_to(cr, a, b); };
  auto L = [&](double a, double b) { cairo_line_to(cr, a, b); };
  auto C = [&](double a, double b, double c2, double d, double e, double f) { cairo_curve_to(cr, a, b, c2, d, e, f); };
  auto rrect = [&](double a, double b, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, a + w - r, b + r, r, -M_PI / 2, 0);
    cairo_arc(cr, a + w - r, b + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, a + r, b + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, a + r, b + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
  };
  auto circle = [&](double cx, double cy, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  };
  auto folder = [&] {
    M(3, 7.5);
    C(3, 6.1, 3.9, 5, 5.3, 5);
    L(9.2, 5);
    L(11.2, 7);
    L(18.7, 7);
    C(20.1, 7, 21, 8, 21, 9.3);
    L(21, 16.7);
    C(21, 18.1, 20.1, 19, 18.7, 19);
    L(5.3, 19);
    C(3.9, 19, 3, 18.1, 3, 16.7);
    cairo_close_path(cr);
  };
  auto stroke = [&] { cairo_stroke(cr); };
  auto fill = [&] { cairo_fill(cr); };
  if (name == "back") {
    M(14.5, 5.5), L(8, 12), L(14.5, 18.5), stroke();
  } else if (name == "forward") {
    M(9.5, 5.5), L(16, 12), L(9.5, 18.5), stroke();
  } else if (name == "up") {
    M(12, 19), L(12, 5.5), stroke();
    M(6, 11), L(12, 5), L(18, 11), stroke();
  } else if (name == "search") {
    circle(10.5, 10.5, 6.3), stroke();
    M(15.2, 15.2), L(20, 20), stroke();
  } else if (name == "grid") {
    rrect(4, 4, 7, 7, 1.6), rrect(13, 4, 7, 7, 1.6), rrect(4, 13, 7, 7, 1.6), rrect(13, 13, 7, 7, 1.6), stroke();
  } else if (name == "list") {
    M(9, 6.5), L(20, 6.5), M(9, 12), L(20, 12), M(9, 17.5), L(20, 17.5), stroke();
    circle(4.8, 6.5, 1.25), circle(4.8, 12, 1.25), circle(4.8, 17.5, 1.25), fill();
  } else if (name == "pane") {
    rrect(3, 4.5, 18, 15, 2.5), stroke();
    M(14.5, 4.5), L(14.5, 19.5), stroke();
  } else if (name == "menu") {
    circle(12, 5.5, 1.7), circle(12, 12, 1.7), circle(12, 18.5, 1.7), fill();
  } else if (name == "edit") {
    M(4.5, 19.5), L(5.3, 15.6), L(15.6, 5.3), C(16.4, 4.5, 17.6, 4.5, 18.4, 5.3), L(18.7, 5.6),
        C(19.5, 6.4, 19.5, 7.6, 18.7, 8.4), L(8.4, 18.7), cairo_close_path(cr), stroke();
    M(14, 7), L(17, 10), stroke();
  } else if (name == "close") {
    M(7, 7), L(17, 17), M(17, 7), L(7, 17), stroke();
  } else if (name == "home") {
    M(3.5, 11.2), L(12, 4), L(20.5, 11.2), stroke();
    M(6, 9.5), L(6, 19.5), L(18, 19.5), L(18, 9.5), stroke();
    M(10, 19.5), L(10, 14.5), L(14, 14.5), L(14, 19.5), stroke();
  } else if (name == "desktop") {
    rrect(3, 4, 18, 12.5, 2), stroke();
    M(8.5, 20), L(15.5, 20), M(12, 16.5), L(12, 20), stroke();
  } else if (name == "documents" || name == "file") {
    M(6, 3), L(14, 3), L(19, 8), L(19, 21), L(6, 21), cairo_close_path(cr), stroke();
    M(14, 3), L(14, 8), L(19, 8), stroke();
    if (name == "documents") M(9, 13), L(16, 13), M(9, 17), L(16, 17), stroke();
  } else if (name == "downloads") {
    M(12, 4), L(12, 15), stroke();
    M(7, 10.5), L(12, 15.5), L(17, 10.5), stroke();
    M(5, 20), L(19, 20), stroke();
  } else if (name == "music") {
    M(9.5, 17.5), L(9.5, 5.5), L(19, 3.8), L(19, 15.5), stroke();
    circle(7.2, 17.6, 2.4), circle(16.7, 15.6, 2.4), stroke();
  } else if (name == "pictures") {
    rrect(3, 4.5, 18, 15, 2.5), stroke();
    M(3.5, 17), L(9, 11.5), L(13.5, 16), L(16.5, 13), L(20.5, 17), stroke();
    circle(15.5, 9, 1.6), fill();
  } else if (name == "videos") {
    rrect(3, 6, 13, 12, 2.2), stroke();
    M(16, 10.2), L(21, 7.3), L(21, 16.7), L(16, 13.8), stroke();
  } else if (name == "recent") {
    circle(12, 12, 8.5), stroke();
    M(12, 7), L(12, 12), L(15.5, 14.2), stroke();
  } else if (name == "bookmark") {
    M(7, 4), L(17, 4), L(17, 20), L(12, 15.8), L(7, 20), cairo_close_path(cr), stroke();
  } else if (name == "drive" || name == "root") {
    rrect(3, 6.5, 18, 11, 2.5), stroke();
    M(3, 13.5), L(21, 13.5), stroke();
    circle(17.3, 15.7, 0.9), fill();
  } else if (name == "usb") {
    rrect(7, 9, 10, 12, 2), stroke();
    M(9, 9), L(9, 3.5), L(15, 3.5), L(15, 9), stroke();
    M(11, 6), L(11, 6.3), M(13, 6), L(13, 6.3), stroke();
  } else if (name == "network") {
    circle(12, 12, 8.5), stroke();
    cairo_save(cr);
    cairo_translate(cr, 12, 12);
    cairo_scale(cr, 0.45, 1);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0, 0, 8.5, 0, 2 * M_PI);
    cairo_restore(cr);
    stroke();
    M(3.5, 12), L(20.5, 12), stroke();
  } else if (name == "folder") {
    folder(), stroke();
  } else if (name == "new-folder") {
    folder(), stroke();
    M(12, 10), L(12, 16), M(9, 13), L(15, 13), stroke();
  } else if (name == "crumb") {
    M(10, 7), L(15, 12), L(10, 17), stroke();
  } else if (name == "sort-up") {
    M(7, 14.5), L(12, 9.5), L(17, 14.5), stroke();
  } else if (name == "sort-down") {
    M(7, 9.5), L(12, 14.5), L(17, 9.5), stroke();
  } else if (name == "check") {
    M(5, 12.5), L(10, 17.5), L(19.5, 7), stroke();
  } else if (name == "warning") {
    M(12, 3.5), L(21.5, 20), L(2.5, 20), cairo_close_path(cr), stroke();
    M(12, 10), L(12, 14), stroke();
    circle(12, 17, 1), fill();
  } else if (name == "go") {
    M(5, 12), L(19, 12), M(13.5, 6.5), L(19, 12), L(13.5, 17.5), stroke();
  } else if (name == "stack") {
    rrect(6, 3, 13, 15, 2), stroke();
    M(4, 7), L(4, 19), C(4, 20.1, 4.9, 21, 6, 21), L(15, 21), stroke();
  }
  cairo_restore(cr);
}

// ------------------------------------------------------------------ waker

// Wakes the FLTK loop from the worker threads: a byte written into a pipe
// that Fl::add_fd() watches (works whether or not the program called
// Fl::lock()).
class Waker {
 public:
  explicit Waker(std::function<void()> fn) : fn_(std::move(fn)) {
    if (pipe2(fds_, O_NONBLOCK | O_CLOEXEC) == 0) Fl::add_fd(fds_[0], FL_READ, ready, this);
    else fds_[0] = fds_[1] = -1;
  }
  ~Waker() {
    if (fds_[0] >= 0) {
      Fl::remove_fd(fds_[0]);
      close(fds_[0]);
      close(fds_[1]);
    }
  }
  void wake() {
    if (fds_[1] < 0) return;
    char c = 1;
    ssize_t r = write(fds_[1], &c, 1);
    (void)r;
  }

 private:
  static void ready(FL_SOCKET fd, void *data) {
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0) {
    }
    static_cast<Waker *>(data)->fn_();
  }
  int fds_[2] = {-1, -1};
  std::function<void()> fn_;
};

// ------------------------------------------------------------------ text fields

// The rounded text fields (path bar, search, file name, new folder): a box
// type that keeps FLTK's text and mouse positions inside the padding.
const Fl_Boxtype kFieldBox = Fl_Boxtype(FL_FREE_BOXTYPE + 60);

void field_box(int, int, int, int, Fl_Color) {}

void register_field_box() {
  int dx = std::max(4, S(10)), dy = std::max(2, S(3));
  Fl::set_boxtype(kFieldBox, field_box, uchar(dx), uchar(dy), uchar(2 * dx), uchar(2 * dy));
}

class Dialog;

class Field : public Fl_Input {
 public:
  Field(Dialog *d, int x, int y, int w, int h, const char *name)
      : Fl_Input(x, y, w, h), d_(d) {
    box(kFieldBox);
    (void)name;
  }
  std::string placeholder;
  bool frameless = false;  // inside the path bar, which draws the frame
  std::function<bool(int key)> keys;  // keys before the field: true when used
  void draw() override;
  int handle(int e) override;

 private:
  Dialog *d_;
};

// ------------------------------------------------------------------ dialog

class IconButton;
class PathBar;
class PlacesView;
class FileView;
class PreviewPane;
class CompletionList;
class NewFolderBox;

class Dialog : public BufferedWindow {
 public:
  explicit Dialog(FileChooser::Options &opts);
  ~Dialog() override;
  bool run(std::vector<std::string> &out);

  FileChooser::Options &o;
  Mode mode;
  chooser::State state;
  bool preview_on = true, preview_forced = false, preview_toggled = false, sorted_by_user = false;
  std::string location, crumb_tail;
  chooser::History history;
  std::vector<Entry> entries, found;
  std::vector<int> listed;  // >= 0: entries[i]; < 0: found[-1 - i]
  std::unordered_map<std::string, int> index_of;
  std::set<std::string> selected;
  std::string focus_path, anchor_path;
  bool loading = false, searching_deep = false;
  std::string error;
  uint64_t generation = 0, search_generation = 0;
  std::string query, folded_query;
  bool search_open = false, editing = false;
  int filter = 0;
  std::vector<Filter> filters;
  std::string select_when_loaded;
  std::string message;
  bool message_error = false;
  double opened_at = 0, first_draw_ms = -1, loaded_ms = -1;
  std::vector<Place> places;
  std::string mounts_text, bookmarks_text;
  long long free_bytes = -1;
  long preview_count = -1;  // items of the selected folder
  bool done = false, accepted = false;
  std::vector<std::string> result;
  int card_x = 0, card_y = 0, card_w = 0, card_h = 0, footer_y = 0, seg_x = 0;

  IconButton *back_btn = nullptr, *forward_btn = nullptr, *up_btn = nullptr, *search_btn = nullptr,
             *grid_btn = nullptr, *list_btn = nullptr, *pane_btn = nullptr, *menu_btn = nullptr;
  PathBar *pathbar = nullptr;
  PlacesView *placesv = nullptr;
  FileView *view = nullptr;
  PreviewPane *preview = nullptr;
  Field *name_input = nullptr;
  Fl_Choice *filter_choice = nullptr;
  Fl_Check_Button *hidden_check = nullptr;
  FlatButton *newfolder_btn = nullptr, *cancel_btn = nullptr, *accept_btn = nullptr;
  CompletionList *completion = nullptr;
  NewFolderBox *newfolder_box = nullptr;
  std::vector<Fl_Menu_Item> filter_items;
  std::vector<std::string> filter_labels;

  struct Thumb {
    cairo_surface_t *surface = nullptr;
    int w = 0, h = 0;
  };
  std::unordered_map<std::string, Thumb> thumbs;  // "<size>\n<path>" ("p<size>" for pictures)

  // Members that start threads come last: they stop first.
  Waker waker;
  chooser::FolderReader reader, searcher;
  chooser::Thumbnailer thumbnailer;

  const Entry &at(int k) const { return listed[size_t(k)] >= 0 ? entries[size_t(listed[size_t(k)])] : found[size_t(-1 - listed[size_t(k)])]; }
  int count() const { return int(listed.size()); }
  int index(const std::string &path) const {
    auto it = index_of.find(path);
    return it == index_of.end() ? -1 : it->second;
  }
  bool selectable(const Entry &e) const { return !is_folder_mode(mode) || e.dir; }
  // The order shown: Recent lists the newest first until a column is chosen.
  chooser::SortOrder order() const {
    chooser::SortOrder o2 = state.sort;
    if (location == "recent:" && !sorted_by_user) {
      o2.key = SortKey::Modified;
      o2.descending = true;
      o2.folders_first = false;
    }
    return o2;
  }
  const Filter *current_filter() const {
    return filter >= 0 && filter < int(filters.size()) ? &filters[size_t(filter)] : nullptr;
  }
  std::vector<const Entry *> selection() const;

  void open_location(const std::string &loc, bool record = true);
  void go_back();
  void go_forward();
  void go_up();
  void reload();
  void pump();
  void refilter();
  void set_query(const std::string &q);
  static void deep_search_cb(void *d);
  void select_only(const std::string &path);
  void selection_changed();
  void activate(int k);
  void accept();
  void cancel();
  bool choose_typed(const std::string &typed);
  void update_buttons();
  void layout();
  void set_list_view(bool list);
  void set_hidden(bool on);
  void set_preview(bool on);
  void sort_by(SortKey key);
  void options_menu(int x, int y);
  void item_menu(int k);
  void start_edit(const std::string &text);
  void stop_edit(bool refocus = true);
  void open_search();
  void close_search();
  void completions_changed();
  void new_folder();
  bool create_folder(const std::string &name, std::string *error);
  void filter_changed();
  void drop(const std::string &text);
  void request_thumbnails();
  static void request_thumbnails_cb(void *d);
  void schedule_thumbnails();
  Thumb *thumbnail(const Entry &e, int size);
  Thumb *picture(const std::string &path, int size);
  int preview_size() const;
  void refresh_places(bool force = false);
  static void poll_places(void *d);
  void say(const std::string &text, bool is_error = false);
  static void clear_message(void *d);
  bool shortcut(int key);
  bool escape();
  void remember();
  void qa_dump();
  static void qa_dump_cb(void *d);
  void place_window();
  void finish(const std::vector<std::string> &paths);
  const std::vector<std::string> &dir_icon_names(const Entry &e);
  const std::vector<std::string> &folder_icon_names(const std::string &path);
  const std::string &type_text(const std::string &mime);
  std::string status_text() const;
  // "Nature", "Home", "File System": a folder's name as places show it.
  std::string folder_label(const std::string &path) const;
  std::unordered_map<std::string, std::vector<std::string>> icon_cache, folder_icons;
  std::unordered_map<std::string, std::string> type_cache;
  std::set<std::string> also_select;  // more files to select once the folder is read (a drop)
  bool preview_shown = false;         // the pane fits and is on

  void draw() override;
  int handle(int e) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

};

void Field::draw() {
  const Theme &t = active_theme();
  Color fill = frameless ? t.surface : t.surface;
  {
    Painter p(window()->w(), window()->h());
    if (!frameless) {
      double r = t.radius_for(h());
      bool focused = Fl::focus() == this;
      p.fill_rounded(x(), y(), w(), h(), r, fill);
      if (focused) p.stroke_rounded(x(), y(), w(), h(), r, t.accent, Sd(2));
      else p.stroke_rounded(x(), y(), w(), h(), r, t.border);
    } else {
      p.fill_rect(x(), y(), w(), h(), fill);
    }
  }
  color(fill.fl());
  textcolor(t.text.fl());
  selection_color(t.accent.fl());
  cursor_color(t.accent.fl());
  textfont(FL_HELVETICA);
  textsize(t.font_size);
  int dx = Fl::box_dx(box()), dy = Fl::box_dy(box());
  drawtext(x() + dx, y() + dy, w() - Fl::box_dw(box()), h() - Fl::box_dh(box()));
  if (size() == 0 && !placeholder.empty() && Fl::focus() != this) {
    text_font(t.font_size);
    draw_text(placeholder, x() + dx + S(2), y(), w() - 2 * dx, h(), t.text_dim);
  }
}

// ------------------------------------------------------------------ icon buttons

class IconButton : public Fl_Button {
 public:
  IconButton(int x, int y, int w, int h, const char *g, const char *tip) : Fl_Button(x, y, w, h), glyph_(g) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    tooltip(tip);
  }
  bool on = false;
  int segment = 0;  // 0 alone, 1 and 2: the halves of the view switch
  void set_glyph(const char *g) {
    glyph_ = g;
    redraw();
  }
  void draw() override {
    const Theme &t = active_theme();
    bool hot = Fl::belowmouse() == this && active_r(), down = value() != 0;
    Painter p(window()->w(), window()->h());
    double r = t.radius_for(h());
    if (on && segment) {
      // The chosen half of the view switch: a raised tile, as in Liria.
      double in = Sd(3);
      Color tile = t.dark ? t.surface_alt.mix(Color::hex(0xFFFFFF), 0.12) : t.surface;
      p.fill_rounded(x() + in, y() + in, w() - 2 * in, h() - 2 * in, std::max(2.0, r - in), tile);
      p.stroke_rounded(x() + in, y() + in, w() - 2 * in, h() - 2 * in, std::max(2.0, r - in),
                       t.border.mix(t.text_dim, 0.15));
    } else if (on) {
      p.fill_rounded(x(), y(), w(), h(), r, t.accent.alpha(t.dark ? 0.28 : 0.16));
    } else if (down || hot) {
      double in = segment ? Sd(3) : 0;
      p.fill_rounded(x() + in, y() + in, w() - 2 * in, h() - 2 * in, std::max(2.0, r - in),
                     down ? t.hover.mix(t.text, 0.08) : t.hover);
    }
    Color c = !active_r() ? t.text_dim.alpha(0.45) : on && !segment ? t.accent_text : t.text;
    double gs = std::round(std::min(w(), h()) * (segment ? 0.52 : 0.56));
    glyph(p, glyph_, x() + std::round((w() - gs) / 2), y() + std::round((h() - gs) / 2), gs, c);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  int handle(int e) override {
    switch (e) {
      case FL_ENTER:
      case FL_LEAVE:
        redraw();
        Fl_Button::handle(e);
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::focus() == this && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
          do_callback();
          return 1;
        }
        break;
      case FL_PUSH: {
        // A click leaves the keyboard where it was (the files keep their
        // type-ahead), as GTK's tool buttons do; Tab still reaches it.
        Fl_Widget *before = Fl::focus();
        int r = Fl_Button::handle(e);
        if (before && before != this && Fl::focus() == this && before->window() == window() && before->visible_r()) {
          Fl::focus(before);
          before->redraw();
          redraw();
        }
        return r;
      }
    }
    return Fl_Button::handle(e);
  }

 private:
  std::string glyph_;
};

// ------------------------------------------------------------------ path bar

class PathBar : public Fl_Group {
 public:
  PathBar(Dialog *d, int x, int y, int w, int h) : Fl_Group(x, y, w, h), d_(d) {
    entry = new Field(d, x, y, w, h, _("Location"));
    entry->frameless = true;
    entry->hide();
    entry->when(FL_WHEN_CHANGED);
    entry->callback([](Fl_Widget *, void *v) { static_cast<Dialog *>(v)->completions_changed(); }, d);
    search = new Field(d, x, y, w, h, _("Search"));
    search->frameless = true;
    search->hide();
    search->when(FL_WHEN_CHANGED);
    search->callback([](Fl_Widget *w, void *v) { static_cast<Dialog *>(v)->set_query(static_cast<Field *>(w)->value()); },
                     d);
    end();
  }
  Field *entry, *search;

  struct Crumb {
    std::string label, path, icon;
    int x = 0, w = 0;
    bool current = false, overflow = false;
  };
  std::vector<Crumb> crumbs, hidden;

  int button_x() const { return x() + w() - h(); }
  void rebuild() {
    crumbs.clear();
    hidden.clear();
    const std::string &loc = d_->location;
    if (loc == "recent:") {
      Crumb c;
      c.label = _("Recent");
      c.icon = "recent";
      c.current = true;
      crumbs.push_back(c);
    } else {
      std::string tail = d_->crumb_tail;
      if (tail.empty() || !(tail == loc || starts_with(tail, loc == "/" ? "/" : loc + "/"))) tail = loc;
      // The chain starts at the home folder, a drive, or the file system.
      std::string anchor = "/", anchor_label = _("File System"), anchor_icon = "root";
      std::string home = home_dir();
      if (tail == home || starts_with(tail, home + "/")) {
        anchor = home;
        anchor_label = _("Home");
        anchor_icon = "home";
      }
      for (const auto &p : d_->places)
        if (p.kind == Place::Kind::Drive || p.kind == Place::Kind::Network)
          if ((tail == p.path || starts_with(tail, p.path + "/")) && p.path.size() > anchor.size()) {
            anchor = p.path;
            anchor_label = p.label;
            anchor_icon = p.icon;
          }
      for (const auto &path : chooser::path_chain(tail)) {
        if (path.size() < anchor.size()) continue;
        Crumb c;
        c.path = path;
        if (path == anchor) {
          c.label = anchor_label;
          c.icon = anchor_icon;
        } else {
          c.label = take_gstr(g_filename_display_basename(path.c_str()));
        }
        c.current = path == loc;
        crumbs.push_back(c);
      }
    }
    layout_crumbs();
    redraw();
  }
  void layout_crumbs() {
    const Theme &t = active_theme();
    text_font(t.font_size);
    int sep = S(16), pad = S(8);
    int avail = button_x() - x() - S(6);
    for (auto &c : crumbs) {
      text_font(t.font_size, c.current);
      c.w = std::min(S(220), text_width(c.label)) + 2 * pad + (c.icon.empty() ? 0 : S(22));
      c.overflow = false;
    }
    // Too long: the first folders give way to "…", which lists them.
    int total = 0;
    for (auto &c : crumbs) total += c.w + sep;
    hidden.clear();
    while (crumbs.size() > 1 && total > avail) {
      hidden.push_back(crumbs.front());
      total -= crumbs.front().w + sep;
      crumbs.erase(crumbs.begin());
      if (hidden.size() == 1) total += S(34) + sep;
    }
    if (!hidden.empty()) {
      Crumb more;
      more.label = "\xE2\x80\xA6";
      more.overflow = true;
      more.w = S(34);
      crumbs.insert(crumbs.begin(), more);
    }
    int cx = x() + S(4);
    for (auto &c : crumbs) {
      c.x = cx;
      cx += c.w + sep;
    }
    // A last crumb that still does not fit is shortened.
    if (!crumbs.empty() && crumbs.back().x + crumbs.back().w > button_x() - S(2))
      crumbs.back().w = std::max(S(40), button_x() - S(2) - crumbs.back().x);
  }
  int crumb_at(int mx) const {
    for (size_t i = 0; i < crumbs.size(); ++i)
      if (mx >= crumbs[i].x && mx < crumbs[i].x + crumbs[i].w) return int(i);
    return -1;
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    entry->resize(X + S(6), Y + 1, W - H - S(6), H - 2);
    search->resize(X + S(32), Y + 1, W - H - S(32), H - 2);
    layout_crumbs();
  }
  void draw() override {
    const Theme &t = active_theme();
    bool field = d_->editing || d_->search_open;
    Field *active = d_->editing ? entry : search;
    bool focused = field && Fl::focus() == active;
    {
      Painter p(window()->w(), window()->h());
      double r = t.radius_for(h());
      p.fill_rounded(x(), y(), w(), h(), r, t.surface);
      if (focused) p.stroke_rounded(x(), y(), w(), h(), r, t.accent, Sd(2));
      else p.stroke_rounded(x(), y(), w(), h(), r, t.border);
      int bx = button_x();
      if (hover_button_) p.fill_rounded(bx + S(3), y() + S(3), h() - S(6), h() - S(6), t.radius_for(h() - S(6)), t.hover);
      double gs = S(16);
      if (d_->search_open) {
        glyph(p, "search", x() + S(10), y() + (h() - gs) / 2, gs, t.text_dim);
        if (!d_->query.empty()) glyph(p, "close", bx + (h() - gs) / 2, y() + (h() - gs) / 2, gs, t.text_dim);
      } else if (d_->editing) {
        glyph(p, "go", bx + (h() - gs) / 2, y() + (h() - gs) / 2, gs, t.accent_text);
      } else {
        glyph(p, "edit", bx + (h() - gs) / 2, y() + (h() - gs) / 2, gs, t.text_dim);
        for (size_t i = 0; i < crumbs.size(); ++i) {
          const Crumb &c = crumbs[i];
          int cy = y() + S(4), ch = h() - S(8);
          if (c.current) p.fill_rounded(c.x, cy, c.w, ch, t.radius_for(ch), t.accent.alpha(t.dark ? 0.24 : 0.13));
          else if (int(i) == hover_) p.fill_rounded(c.x, cy, c.w, ch, t.radius_for(ch), t.hover);
          if (!c.icon.empty())
            glyph(p, c.icon, c.x + S(8), y() + (h() - S(16)) / 2.0, S(16), c.current ? t.accent_text : t.text_dim);
          if (i + 1 < crumbs.size())
            glyph(p, "crumb", c.x + c.w + S(1), y() + (h() - S(14)) / 2.0, S(14), t.text_dim.alpha(0.8));
        }
      }
    }
    if (field) {
      draw_child(*active);
    } else {
      for (const Crumb &c : crumbs) {
        text_font(t.font_size, c.current);
        int tx = c.x + S(8) + (c.icon.empty() ? 0 : S(22));
        draw_text(c.label, tx, y(), c.w - (tx - c.x) - S(8), h(), c.current ? t.accent_text : t.text,
                  c.icon.empty() && c.overflow ? 1 : 0);
      }
    }
  }
  int handle(int e) override;

  int hover_ = -1;
  bool hover_button_ = false;

 private:
  Dialog *d_;
};

// ------------------------------------------------------------------ places

class PlacesView : public Fl_Widget {
 public:
  PlacesView(Dialog *d, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), d_(d) {}
  struct Row {
    int place = -1;  // index into d->places, -1 for a caption
    std::string caption;
    int y = 0, h = 0;
  };
  std::vector<Row> rows;
  int scroll = 0, hover = -1, focus_row = -1;

  void rebuild() {
    rows.clear();
    int y = S(6);
    Place::Kind last = Place::Kind::Home;
    for (size_t i = 0; i < d_->places.size(); ++i) {
      const Place &p = d_->places[i];
      std::string caption;
      if (p.kind == Place::Kind::Bookmark && last != Place::Kind::Bookmark) caption = _("Bookmarks");
      if ((p.kind == Place::Kind::Drive || p.kind == Place::Kind::Root) && last != Place::Kind::Drive &&
          last != Place::Kind::Root)
        caption = _("Drives");
      if (p.kind == Place::Kind::Network && last != Place::Kind::Network) caption = _("Network");
      if (!caption.empty()) {
        Row c;
        c.caption = caption;
        c.y = y + S(6);
        c.h = S(22);
        rows.push_back(c);
        y += S(28);
      }
      Row r;
      r.place = int(i);
      r.y = y;
      r.h = S(32);
      rows.push_back(r);
      y += S(34);
      last = p.kind;
    }
    content_h_ = y + S(6);
    clamp_scroll();
    redraw();
  }
  int current_row() const {
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].place >= 0) {
        const Place &p = d_->places[size_t(rows[i].place)];
        if ((p.kind == Place::Kind::Recent && d_->location == "recent:") ||
            (p.kind != Place::Kind::Recent && p.path == d_->location))
          return int(i);
      }
    return -1;
  }
  void clamp_scroll() { scroll = std::max(0, std::min(scroll, content_h_ - h())); }
  int row_at(int my) const {
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].place >= 0 && my >= y() + rows[i].y - scroll && my < y() + rows[i].y + rows[i].h - scroll)
        return int(i);
    return -1;
  }
  void reveal(int row) {
    if (row < 0) return;
    int top = rows[size_t(row)].y, bottom = top + rows[size_t(row)].h;
    if (top - scroll < 0) scroll = top - S(6);
    if (bottom - scroll > h()) scroll = bottom - h() + S(6);
    clamp_scroll();
  }
  void draw() override {
    const Theme &t = active_theme();
    int cur = current_row();
    fl_push_clip(x(), y(), w(), h());
    {
      Painter p(window()->w(), window()->h());
      for (size_t i = 0; i < rows.size(); ++i) {
        const Row &r = rows[i];
        if (r.place < 0) continue;
        int ry = y() + r.y - scroll;
        if (ry + r.h < y() || ry > y() + h()) continue;
        double rx = x() + S(8), rw = w() - S(14);
        if (int(i) == cur) {
          p.fill_rounded(rx, ry, rw, r.h, t.radius_for(r.h), t.accent.alpha(t.dark ? 0.24 : 0.15));
          p.fill_rounded(rx, ry + S(9), Sd(3), r.h - S(18), Sd(1.5), t.accent);
        } else if (int(i) == hover) {
          p.fill_rounded(rx, ry, rw, r.h, t.radius_for(r.h), t.hover);
        }
        if (Fl::focus() == this && int(i) == (focus_row >= 0 ? focus_row : cur))
          focus_ring(p, rx, ry, rw, r.h, t.radius_for(r.h));
        const Place &pl = d_->places[size_t(r.place)];
        glyph(p, pl.icon, x() + S(20), ry + (r.h - S(18)) / 2.0, S(18), t.accent_text);
      }
      if (content_h_ > h() + S(8)) {
        double track = h() - S(8), th = std::max<double>(S(24), track * h() / content_h_);
        double ty = y() + S(4) + (track - th) * scroll / std::max(1, content_h_ - h());
        p.fill_rounded(x() + w() - S(5), ty, Sd(3), th, Sd(1.5), t.text_dim.alpha(0.35));
      }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row &r = rows[i];
      int ry = y() + r.y - scroll;
      if (ry + r.h < y() || ry > y() + h()) continue;
      if (r.place < 0) {
        text_font(t.font_size - S(2), true);
        draw_text(r.caption, x() + S(20), ry, w() - S(30), r.h, t.text_dim);
        continue;
      }
      bool cur_row = int(i) == cur;
      text_font(t.font_size, cur_row);
      draw_text(d_->places[size_t(r.place)].label, x() + S(48), ry, w() - S(58), r.h,
                cur_row ? t.accent_text : t.text);
    }
    fl_pop_clip();
  }
  int handle(int e) override;

 private:
  Dialog *d_;
  int content_h_ = 0;
};

// ------------------------------------------------------------------ the files

class FileView : public Fl_Group {
 public:
  FileView(Dialog *d, int x, int y, int w, int h) : Fl_Group(x, y, w, h), d_(d) {
    bar = new Fl_Scrollbar(x + w - S(12), y, S(12), h);
    bar->type(FL_VERTICAL);
    bar->callback([](Fl_Widget *w, void *v) {
      static_cast<FileView *>(v)->set_scroll(static_cast<Fl_Scrollbar *>(w)->value());
    }, this);
    bar->hide();
    end();
  }
  Fl_Scrollbar *bar;
  int scroll = 0, hover = -1, press = -1;
  bool drop_hover = false, header_hover = false;
  int header_col_hover = -1;
  std::string typed;
  double typed_at = 0;
  // Metrics, set by metrics().
  int pad = 0, cols = 1, tile_w = 0, tile_h = 0, row_h = 0, head_h = 0, status_h = 0, image_h = 0;
  std::unordered_map<std::string, std::vector<std::string>> wraps;
  int wraps_w = 0;

  bool list() const { return d_->state.list_view; }
  void metrics() {
    pad = S(10);
    status_h = S(30);
    head_h = list() ? S(32) : 0;
    row_h = S(32);
    int iw = w() - S(12) - 2 * pad;
    int min_tile = S(116);
    cols = std::max(1, iw / min_tile);
    tile_w = std::max(S(80), iw / cols);
    image_h = S(84);
    tile_h = image_h + S(52);
    if (tile_w != wraps_w) {
      wraps.clear();
      wraps_w = tile_w;
    }
  }
  int items_y() const { return y() + head_h; }
  int items_h() const { return std::max(0, h() - head_h - status_h); }
  int content_h() const {
    int n = d_->count();
    if (list()) return n * row_h + S(8);
    int rows = (n + cols - 1) / cols;
    return rows * tile_h + 2 * pad;
  }
  void item_rect(int k, int &X, int &Y, int &W, int &H) const {
    if (list()) {
      X = x() + S(6);
      Y = items_y() + S(4) + k * row_h - scroll;
      W = w() - S(12) - S(10);
      H = row_h;
    } else {
      X = x() + pad + (k % cols) * tile_w;
      Y = items_y() + pad + (k / cols) * tile_h - scroll;
      W = tile_w;
      H = tile_h;
    }
  }
  int item_at(int mx, int my) const {
    if (my < items_y() || my >= items_y() + items_h() || mx < x() || mx >= x() + w() - S(12)) return -1;
    int n = d_->count();
    if (list()) {
      int k = (my - items_y() - S(4) + scroll) / row_h;
      return my - items_y() - S(4) + scroll >= 0 && k < n ? k : -1;
    }
    int cx = mx - x() - pad, cy = my - items_y() - pad + scroll;
    if (cx < 0 || cy < 0) return -1;
    int col = cx / tile_w, row = cy / tile_h;
    if (col >= cols) return -1;
    int k = row * cols + col;
    return k < n ? k : -1;
  }
  void visible_range(int &first, int &last) const {
    int n = d_->count();
    if (n == 0) {
      first = 0;
      last = -1;
      return;
    }
    if (list()) {
      first = std::max(0, (scroll - S(4)) / row_h);
      last = std::min(n - 1, (scroll + items_h()) / row_h);
    } else {
      first = std::max(0, (scroll - pad) / tile_h * cols);
      last = std::min(n - 1, ((scroll + items_h()) / tile_h + 1) * cols - 1);
    }
  }
  void update_bar() {
    metrics();
    int total = content_h(), vis = items_h();
    int max_scroll = std::max(0, total - vis);
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
    bar->resize(x() + w() - S(12), items_y(), S(12), vis);
    if (total <= vis) {
      bar->hide();
    } else {
      bar->show();
      bar->value(scroll, vis, 0, total);
      bar->linesize(list() ? row_h : tile_h / 2);
    }
  }
  void set_scroll(int s) {
    int max_scroll = std::max(0, content_h() - items_h());
    s = std::max(0, std::min(s, max_scroll));
    if (s == scroll) return;
    scroll = s;
    hover = -1;
    if (bar->visible()) bar->value(scroll, items_h(), 0, content_h());
    redraw();
    d_->schedule_thumbnails();
  }
  void ensure_visible(int k) {
    if (k < 0) return;
    metrics();
    int X, Y, W, H;
    item_rect(k, X, Y, W, H);
    int top = Y + scroll - items_y(), bottom = top + H;
    int s = scroll;
    if (top - S(4) < s) s = std::max(0, top - S(4));
    if (bottom + S(4) > s + items_h()) s = bottom + S(4) - items_h();
    set_scroll(s);
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    update_bar();
  }
  // The pixel size of the thumbnails that the grid shows.
  int thumb_size() const { return list() ? 128 : (image_h > 128 ? 256 : 128); }

  struct Column {
    SortKey key;
    std::string title;
    int x, w;
    bool right;
  };
  std::vector<Column> columns() const {
    std::vector<Column> out;
    int left = x() + S(6), right = x() + w() - S(12) - S(10);
    int size_w = S(86), date_w = S(150), type_w = S(150);
    int avail = right - left;
    bool show_type = avail >= S(560), show_date = avail >= S(380), show_size = avail >= S(260);
    bool where = d_->location == "recent:" || !d_->found.empty();
    int cx = right;
    std::vector<Column> tail;
    if (show_type) {
      cx -= type_w;
      tail.push_back({SortKey::Type, where ? _("Location") : _("Type"), cx, type_w, false});
    }
    if (show_date) {
      cx -= date_w;
      tail.push_back({SortKey::Modified, _("Modified"), cx, date_w, false});
    }
    if (show_size) {
      cx -= size_w;
      tail.push_back({SortKey::Size, _("Size"), cx, size_w, true});
    }
    out.push_back({SortKey::Name, _("Name"), left, cx - left, false});
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) out.push_back(*it);
    return out;
  }
  int column_at(int mx) const {
    auto cols_ = columns();
    for (size_t i = 0; i < cols_.size(); ++i)
      if (mx >= cols_[i].x && mx < cols_[i].x + cols_[i].w) return int(i);
    return -1;
  }

  void draw() override;
  int handle(int e) override;

 private:
  Dialog *d_;
  void draw_empty(const Theme &t);
};

// ------------------------------------------------------------------ preview

class PreviewPane : public Fl_Widget {
 public:
  PreviewPane(Dialog *d, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), d_(d) {}
  // The picture box: as wide as the pane, at most this high.
  int box_w() const { return std::max(S(60), w() - S(32)); }
  int box_h() const { return std::min(box_w(), std::max(S(80), h() / 2)); }
  void draw() override;

 private:
  Dialog *d_;
};

// ------------------------------------------------------------------ completion

class CompletionList : public Fl_Widget {
 public:
  explicit CompletionList(Dialog *d) : Fl_Widget(0, 0, 10, 10), d_(d) { hide(); }
  std::vector<std::string> items;
  int current = -1, hover = -1, first = 0;
  int row_h() const { return S(32); }
  int rows() const { return std::min(int(items.size()), 8); }
  void set(std::vector<std::string> v);
  void move_current(int delta) {
    if (items.empty()) return;
    int n = int(items.size());
    current = current < 0 ? (delta > 0 ? 0 : n - 1) : (current + delta + n) % n;
    if (current < first) first = current;
    if (current >= first + rows()) first = current - rows() + 1;
    redraw();
  }
  void draw() override {
    const Theme &t = active_theme();
    {
      Painter p(window()->w(), window()->h());
      p.shadow(x(), y(), w(), h(), t.radius, t.shadow, S(10));
      p.fill_rounded(x(), y(), w(), h(), t.radius, t.menu_bg);
      p.stroke_rounded(x(), y(), w(), h(), t.radius, t.menu_border);
      for (int i = 0; i < rows(); ++i) {
        int k = first + i, ry = y() + S(4) + i * row_h();
        if (k == current || k == hover)
          p.fill_rounded(x() + S(4), ry, w() - S(8), row_h(), t.radius_for(row_h()),
                         k == current ? t.menu_hover : t.hover);
        bool dir = !items[size_t(k)].empty() && items[size_t(k)].back() == '/';
        glyph(p, dir ? "folder" : "file", x() + S(14), ry + (row_h() - S(16)) / 2.0, S(16),
              k == current ? t.menu_hover_text : t.text_dim);
      }
    }
    for (int i = 0; i < rows(); ++i) {
      int k = first + i, ry = y() + S(4) + i * row_h();
      std::string s = items[size_t(k)];
      bool dir = !s.empty() && s.back() == '/';
      if (dir) s.pop_back();
      size_t slash = s.rfind('/');
      std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
      text_font(t.font_size);
      draw_text(take_gstr(g_filename_display_name(name.c_str())), x() + S(40), ry, w() - S(52), row_h(),
                k == current ? t.menu_hover_text : t.menu_text);
    }
  }
  int handle(int e) override;

 private:
  Dialog *d_;
};

// ------------------------------------------------------------------ new folder

class NewFolderBox : public Fl_Group {
 public:
  explicit NewFolderBox(Dialog *d) : Fl_Group(0, 0, S(340), S(150)), d_(d) {
    input = new Field(d, 0, 0, 10, 10, _("Folder name"));
    input->keys = [this](int key) {
      if (key == FL_Enter || key == FL_KP_Enter) {
        create_cb(nullptr, this);
        return true;
      }
      if (key == FL_Escape) {
        close();
        return true;
      }
      return false;
    };
    input->when(FL_WHEN_CHANGED);
    input->callback([](Fl_Widget *, void *v) {
      auto *self = static_cast<NewFolderBox *>(v);
      self->error.clear();
      self->redraw();
    }, this);
    create = new FlatButton(0, 0, 10, 10, _("Create"), true);
    create->copy_label(_("Create"));
    create->callback(create_cb, this);
    cancel = new FlatButton(0, 0, 10, 10, _("Cancel"));
    cancel->copy_label(_("Cancel folder"));
    cancel->callback([](Fl_Widget *, void *v) { static_cast<NewFolderBox *>(v)->close(); }, this);
    end();
    hide();
  }
  Field *input;
  FlatButton *create, *cancel;
  std::string error;

  void place(int X, int Y) {
    int W = S(340), H = S(152);
    resize(X, Y - H, W, H);
    input->resize(X + S(16), Y - H + S(44), W - S(32), S(34));
    cancel->resize(X + W - S(16) - S(96) * 2 - S(8), Y - S(16) - S(32), S(96), S(32));
    create->resize(X + W - S(16) - S(96), Y - S(16) - S(32), S(96), S(32));
  }
  void open(int X, int Y) {
    place(X, Y);
    error.clear();
    input->value(_("New Folder"));
    input->insert_position(input->size(), 0);
    show();
    input->take_focus();
    window()->redraw();
  }
  void close();
  static void create_cb(Fl_Widget *, void *v);
  void draw() override {
    const Theme &t = active_theme();
    {
      Painter p(window()->w(), window()->h());
      p.shadow(x(), y(), w(), h(), t.radius, t.shadow, S(12));
      p.fill_rounded(x(), y(), w(), h(), t.radius, t.surface);
      p.stroke_rounded(x(), y(), w(), h(), t.radius, t.border);
    }
    text_font(t.font_size + 1, true);
    draw_text(_("New Folder"), x() + S(16), y() + S(10), w() - S(32), S(26), t.text);
    if (!error.empty()) {
      text_font(t.font_size - 1);
      draw_text(error, x() + S(16), y() + S(80), w() - S(32), S(20), t.error);
    }
    draw_children();
  }

 private:
  Dialog *d_;
};

// ================================================================== drawing

void FileView::draw_empty(const Theme &t) {
  std::string title, detail;
  const char *icon = "folder";
  if (!d_->error.empty()) {
    title = _("This folder cannot be opened");
    detail = d_->error;
    icon = "warning";
  } else if (d_->loading) {
    return;
  } else if (!d_->query.empty()) {
    title = d_->searching_deep ? _("Searching…") : _("No files found");
    detail = format(_("Nothing here is called “%s”."), d_->query.c_str());
    icon = "search";
  } else if (d_->location == "recent:") {
    title = _("No recent files");
    detail = _("Files you open or save appear here.");
    icon = "recent";
  } else if (!d_->entries.empty()) {
    const Filter *f = d_->current_filter();
    title = f ? format(_("No %s here"), f->name.c_str()) : _("Nothing to show");
    detail = _("Other files of this folder are hidden by the filter.");
  } else {
    title = _("This folder is empty");
  }
  int cy = items_y() + items_h() / 2 - S(50);
  {
    Painter p(window()->w(), window()->h());
    double gs = S(56);
    glyph(p, icon, x() + (w() - gs) / 2, cy, gs, t.text_dim.alpha(0.55), 1.1);
  }
  text_font(t.font_size + 2, true);
  draw_text(title, x() + S(20), cy + S(64), w() - S(40), S(26), t.text, 1);
  if (!detail.empty()) {
    text_font(t.font_size);
    draw_text(detail, x() + S(20), cy + S(90), w() - S(40), S(22), t.text_dim, 1);
  }
}

void FileView::draw() {
  const Theme &t = active_theme();
  metrics();
  int n = d_->count();
  int first, last;
  visible_range(first, last);
  bool focused = Fl::focus() == this;
  std::vector<Column> cols_ = list() ? columns() : std::vector<Column>();
  int iy = items_y(), ih = items_h();
  // Pass 1: shapes and pictures.
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.surface);
    if (list()) {
      p.fill_rect(x(), y(), w(), head_h, t.surface);
      p.line(x() + S(8), y() + head_h - 0.5, x() + w() - S(8), y() + head_h - 0.5, t.border);
      for (size_t i = 0; i < cols_.size(); ++i) {
        const Column &c = cols_[i];
        if (int(i) == header_col_hover)
          p.fill_rounded(c.x, y() + S(4), c.w, head_h - S(8), t.radius_for(head_h - S(8)), t.hover);
        if (c.key == d_->order().key) {
          double gx = c.right ? c.x + S(2) : c.x + c.w - S(18);
          glyph(p, d_->order().descending ? "sort-down" : "sort-up", gx, y() + (head_h - S(14)) / 2.0, S(14),
                t.accent_text);
        }
      }
    }
    // The status line.
    p.line(x() + S(8), y() + h() - status_h + 0.5, x() + w() - S(8), y() + h() - status_h + 0.5, t.border);
  }
  fl_push_clip(x(), iy, w(), ih);
  {
    Painter p(window()->w(), window()->h());
    for (int k = first; k <= last && k < n; ++k) {
      const Entry &e = d_->at(k);
      int X, Y, W, H;
      item_rect(k, X, Y, W, H);
      if (!fl_not_clipped(X, Y, W, H)) continue;
      bool sel = d_->selected.count(e.path) != 0, hot = k == hover, foc = focused && d_->focus_path == e.path;
      double alpha = d_->selectable(e) ? 1.0 : 0.42;
      if (list()) {
        double r = t.radius_for(H - S(2));
        if (sel) p.fill_rounded(X, Y + 1, W, H - 2, r, t.accent.alpha(t.dark ? 0.30 : 0.17));
        else if (hot) p.fill_rounded(X, Y + 1, W, H - 2, r, t.hover);
        if (foc) focus_ring(p, X + S(2), Y + S(2), W - S(4), H - S(4), r);
        int isz = S(22);
        Dialog::Thumb *th = e.dir ? nullptr : d_->thumbnail(e, 128);
        if (th && th->surface) {
          p.image(th->surface, X + S(8), Y + (H - isz) / 2.0, isz, alpha);
        } else {
          cairo_surface_t *icon = IconLoader::get(d_->dir_icon_names(e), isz);
          p.image(icon, X + S(8), Y + (H - isz) / 2.0, isz, alpha);
        }
      } else {
        double r = t.radius;
        double bx = X + S(4), by = Y + S(2), bw = W - S(8), bh = H - S(4);
        if (sel) p.fill_rounded(bx, by, bw, bh, r, t.accent.alpha(t.dark ? 0.30 : 0.17));
        else if (hot) p.fill_rounded(bx, by, bw, bh, r, t.hover);
        if (foc) focus_ring(p, bx + S(2), by + S(2), bw - S(4), bh - S(4), r);
        double box_w = W - S(24), box_h = image_h;
        double cx = X + W / 2.0, top = Y + S(8);
        Dialog::Thumb *th = e.dir ? nullptr : d_->thumbnail(e, thumb_size());
        if (th && th->surface) {
          double iw = cairo_image_surface_get_width(th->surface), ihh = cairo_image_surface_get_height(th->surface);
          double k2 = std::min(box_w / iw, box_h / ihh);
          if (k2 > 1 && iw >= S(48)) k2 = std::min(k2, 2.0);  // small pictures are not blown up much
          double dw = std::round(iw * k2), dh = std::round(ihh * k2);
          double dx = std::round(cx - dw / 2), dy = std::round(top + (box_h - dh) / 2);
          cairo_t *cr = p.cr();
          cairo_save(cr);
          p.rounded_rect(dx, dy, dw, dh, Sd(5));
          cairo_clip(cr);
          cairo_translate(cr, dx, dy);
          cairo_scale(cr, dw / iw, dh / ihh);
          cairo_set_source_surface(cr, p.server_copy(th->surface), 0, 0);
          cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
          cairo_paint_with_alpha(cr, alpha);
          cairo_restore(cr);
          p.stroke_rounded(dx, dy, dw, dh, Sd(5), t.text.alpha(t.dark ? 0.18 : 0.12));
        } else {
          int isz = S(64);
          cairo_surface_t *icon = IconLoader::get(d_->dir_icon_names(e), isz);
          p.image(icon, cx - isz / 2.0, top + (box_h - isz) / 2.0, isz, alpha);
        }
      }
    }
    if (drop_hover) p.stroke_rounded(x() + S(4), iy + S(4), w() - S(8), ih - S(8), t.radius, t.accent, Sd(2));
  }
  // Pass 2: text.
  for (int k = first; k <= last && k < n; ++k) {
    const Entry &e = d_->at(k);
    int X, Y, W, H;
    item_rect(k, X, Y, W, H);
    if (!fl_not_clipped(X, Y, W, H)) continue;
    bool sel = d_->selected.count(e.path) != 0;
    Color fg = d_->selectable(e) ? t.text : t.text_dim.alpha(0.6);
    if (list()) {
      for (const Column &c : cols_) {
        std::string s;
        Color col = fg;
        text_font(t.font_size);
        switch (c.key) {
          case SortKey::Name:
            s = e.display;
            draw_text(s, X + S(40), Y, c.w - S(40) - S(8), H, fg);
            continue;
          case SortKey::Size:
            s = e.dir ? std::string() : chooser::size_text(e.size);
            col = t.text_dim;
            break;
          case SortKey::Modified:
            s = chooser::time_text(e.mtime, time(nullptr));
            col = t.text_dim;
            break;
          case SortKey::Type:
            s = !e.folder.empty() ? display_path(e.folder) : d_->type_text(e.mime);
            col = t.text_dim;
            break;
        }
        draw_text(s, c.x + S(8), Y, c.w - S(16), H, col, c.right ? 2 : 0);
      }
    } else {
      text_font(t.font_size);
      int lw = W - S(14);
      int line_h = fl_height() + S(1);
      int ty = Y + S(8) + image_h + S(6);
      if (!e.folder.empty()) {
        // Recent files and search results: the name, and where it is.
        draw_text(e.display, X + S(7), ty, lw, line_h, fg, 1);
        text_font(t.font_size - S(1));
        draw_text(d_->folder_label(e.folder), X + S(7), ty + line_h, lw, line_h, t.text_dim, 1);
        continue;
      }
      auto it = wraps.find(e.path);
      if (it == wraps.end()) it = wraps.emplace(e.path, wrap_name(e.display, lw)).first;
      for (const auto &line : it->second) {
        draw_text(line, X + S(7), ty, lw, line_h, sel ? t.text : fg, 1);
        ty += line_h;
      }
    }
  }
  if (n == 0) draw_empty(t);
  fl_pop_clip();
  if (list()) {
    for (size_t i = 0; i < cols_.size(); ++i) {
      const Column &c = cols_[i];
      text_font(t.font_size - 1, c.key == d_->order().key);
      int tx = c.x + S(8), tw = c.w - S(16) - (c.key == d_->order().key ? S(18) : 0);
      if (c.right) tx += c.key == d_->order().key ? S(18) : 0;
      draw_text(c.title, tx, y(), tw, head_h, c.key == d_->order().key ? t.accent_text : t.text_dim,
                c.right ? 2 : 0);
    }
  }
  // Status: what is shown, or a message.
  {
    text_font(t.font_size - 1);
    std::string left = d_->status_text(), right;
    if (!d_->message.empty()) left = d_->message;
    if (d_->free_bytes >= 0 && real_folder(d_->location))
      right = format(_("%s free"), chooser::size_text(d_->free_bytes).c_str());
    int rw = right.empty() ? 0 : text_width(right) + S(16);
    draw_text(left, x() + S(14), y() + h() - status_h, w() - S(28) - rw, status_h,
              !d_->message.empty() && d_->message_error ? t.error : t.text_dim);
    if (!right.empty()) draw_text(right, x() + w() - rw - S(10), y() + h() - status_h, rw, status_h, t.text_dim, 2);
  }
  draw_child(*bar);
}

void PreviewPane::draw() {
  const Theme &t = active_theme();
  std::vector<const Entry *> sel = d_->selection();
  const Entry *e = sel.size() == 1 ? sel[0] : nullptr;
  int bw = box_w(), bh = box_h();
  int bx = x() + (w() - bw) / 2, by = y() + S(18);
  int text_y = by + bh + S(14);
  std::string title, line1, line2, line3, line4;
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.surface);
    p.line(x() + 0.5, y() + S(10), x() + 0.5, y() + h() - S(10), t.border);
    cairo_surface_t *pic = nullptr;
    int pw = 0, ph = 0;
    if (e && !e->dir) {
      Dialog::Thumb *big = d_->picture(e->path, d_->preview_size());
      if (big && big->surface) {
        pic = big->surface;
        pw = big->w;
        ph = big->h;
      } else if (Dialog::Thumb *th = d_->thumbnail(*e, 256)) {
        pic = th->surface;
        pw = th->w;
        ph = th->h;
      }
    }
    if (pic) {
      double iw = cairo_image_surface_get_width(pic), ih = cairo_image_surface_get_height(pic);
      double k = std::min(bw / iw, bh / ih);
      if (k > 1) k = std::min(k, iw < S(64) ? 1.0 : 3.0);
      double dw = std::round(iw * k), dh = std::round(ih * k);
      double dx = std::round(bx + (bw - dw) / 2), dy = std::round(by + (bh - dh) / 2);
      p.shadow(dx, dy, dw, dh, Sd(8), t.shadow.alpha(t.shadow.a * 0.6), S(6));
      cairo_t *cr = p.cr();
      cairo_save(cr);
      p.rounded_rect(dx, dy, dw, dh, Sd(8));
      cairo_clip(cr);
      cairo_translate(cr, dx, dy);
      cairo_scale(cr, dw / iw, dh / ih);
      cairo_set_source_surface(cr, p.server_copy(pic), 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
      cairo_paint(cr);
      cairo_restore(cr);
      p.stroke_rounded(dx, dy, dw, dh, Sd(8), t.text.alpha(t.dark ? 0.16 : 0.10));
      (void)pw;
      (void)ph;
    } else {
      int isz = std::min(S(96), bh);
      if (sel.size() > 1) {
        glyph(p, "stack", bx + (bw - isz) / 2.0, by + (bh - isz) / 2.0, isz, t.text_dim.alpha(0.6), 1.2);
      } else {
        std::vector<std::string> names = e ? d_->dir_icon_names(*e) : d_->folder_icon_names(d_->location);
        if (d_->location == "recent:" && !e) {
          glyph(p, "recent", bx + (bw - isz) / 2.0, by + (bh - isz) / 2.0, isz, t.text_dim.alpha(0.6), 1.2);
        } else {
          p.image(IconLoader::get(names, isz), bx + (bw - isz) / 2.0, by + (bh - isz) / 2.0, isz);
        }
      }
    }
  }
  // The text under the picture.
  std::vector<std::pair<std::string, std::string>> rows;  // label, value
  if (sel.size() > 1) {
    long long total = 0;
    int files = 0;
    for (const Entry *s : sel)
      if (!s->dir) {
        total += s->size;
        ++files;
      }
    title = format(ngettext("%d item selected", "%d items selected", int(sel.size())), int(sel.size()));
    if (files) rows.push_back({_("Size"), chooser::size_text(total)});
  } else if (e) {
    title = e->display;
    rows.push_back({_("Type"), d_->type_text(e->mime)});
    if (e->dir) {
      if (d_->preview_count >= 0)
        rows.push_back({_("Contents"), d_->preview_count >= 5000 ? std::string(_("5,000 items or more"))
                                                                  : format(ngettext("%ld item", "%ld items",
                                                                                    d_->preview_count),
                                                                           d_->preview_count)});
    } else {
      Dialog::Thumb *big = d_->picture(e->path, d_->preview_size());
      Dialog::Thumb *th = big && big->w > 0 ? big : d_->thumbnail(*e, 256);
      if (th && th->w > 0 && th->h > 0) rows.push_back({_("Dimensions"), format("%d × %d", th->w, th->h)});
      rows.push_back({_("Size"), chooser::size_text(e->size)});
    }
    rows.push_back({_("Modified"), chooser::time_text(e->mtime, time(nullptr))});
    if (!e->folder.empty()) rows.push_back({_("Location"), display_path(e->folder)});
  } else if (d_->location == "recent:") {
    title = _("Recent");
    rows.push_back({_("Contents"), format(ngettext("%d file", "%d files", d_->count()), d_->count())});
  } else {
    title = take_gstr(g_filename_display_basename(d_->location.c_str()));
    if (d_->location == home_dir()) title = _("Home");
    rows.push_back({_("Type"), _("Folder")});
    rows.push_back({_("Contents"), format(ngettext("%d item", "%d items", int(d_->entries.size())),
                                          int(d_->entries.size()))});
  }
  fl_push_clip(x(), y(), w(), h());
  int tx = x() + S(16), tw = w() - S(32);
  text_font(t.font_size + 1, true);
  std::vector<std::string> lines = wrap_name(title, tw);
  for (const auto &l : lines) {
    draw_text(l, tx, text_y, tw, S(22), t.text, 1);
    text_y += S(22);
  }
  text_y += S(10);
  for (const auto &r : rows) {
    if (r.second.empty()) continue;
    if (text_y + S(40) > y() + h()) break;
    text_font(t.font_size - 2, true);
    draw_text(r.first, tx, text_y, tw, S(18), t.text_dim);
    text_font(t.font_size);
    draw_text(r.second, tx, text_y + S(17), tw, S(20), t.text);
    text_y += S(44);
  }
  fl_pop_clip();
}

// ================================================================== events

int Field::handle(int e) {
  if (e == FL_KEYBOARD) {
    int key = Fl::event_key();
    if (keys && keys(key)) return 1;
    if (d_->shortcut(key)) return 1;
    if (key == FL_Escape && d_->escape()) return 1;
  }
  if (e == FL_FOCUS || e == FL_UNFOCUS) {
    if (parent()) parent()->redraw();
    redraw();
  }
  return Fl_Input::handle(e);
}

int PathBar::handle(int e) {
  if ((e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE) && Fl_Group::handle(e)) return 1;
  int mx = Fl::event_x();
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int h2 = d_->editing || d_->search_open ? -1 : crumb_at(mx);
      bool hb = mx >= button_x();
      if (h2 != hover_ || hb != hover_button_) {
        hover_ = h2;
        hover_button_ = hb;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      hover_button_ = false;
      redraw();
      return 1;
    case FL_PUSH: {
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      if (mx >= button_x()) {
        if (d_->search_open) {
          if (!d_->query.empty()) {
            search->value("");
            d_->set_query("");
            search->take_focus();
          } else {
            d_->close_search();
          }
        } else if (d_->editing) {
          d_->choose_typed(entry->value());
        } else {
          d_->start_edit(real_folder(d_->location) ? d_->location : std::string());
        }
        return 1;
      }
      if (d_->editing || d_->search_open) return 1;
      int k = crumb_at(mx);
      if (k < 0) {
        d_->start_edit(real_folder(d_->location) ? d_->location : std::string());
        return 1;
      }
      if (crumbs[size_t(k)].overflow) {
        ContextMenu menu;
        for (auto it = hidden.rbegin(); it != hidden.rend(); ++it) {
          std::string path = it->path;
          menu.item(it->label, [this, path] { d_->open_location(path); }, {it->icon.empty() ? "folder" : "", 0});
        }
        menu.popup(crumbs[size_t(k)].x - window()->x() + window()->x() - x() + x(), y() + h() + S(2));
        return 1;
      }
      if (!crumbs[size_t(k)].path.empty() && crumbs[size_t(k)].path != d_->location)
        d_->open_location(crumbs[size_t(k)].path);
      return 1;
    }
  }
  return Fl_Group::handle(e);
}

int PlacesView::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int r = row_at(Fl::event_y());
      if (r != hover) {
        hover = r;
        redraw();
        if (r >= 0) {
          const Place &p = d_->places[size_t(rows[size_t(r)].place)];
          static std::string tip;
          tip = p.path.empty() ? p.label : display_path(p.path);
          Fl_Tooltip::enter_area(this, x(), y() + rows[size_t(r)].y - scroll, w(), rows[size_t(r)].h, tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      int r = row_at(Fl::event_y());
      if (r < 0) return 1;
      if (Fl::event_button() == FL_LEFT_MOUSE) {
        focus_row = r;
        const Place &p = d_->places[size_t(rows[size_t(r)].place)];
        d_->open_location(p.kind == Place::Kind::Recent ? "recent:" : p.path);
      }
      return 1;
    }
    case FL_MOUSEWHEEL:
      if (content_h_ > h()) {
        scroll += Fl::event_dy() * S(36);
        clamp_scroll();
        hover = -1;
        redraw();
        return 1;
      }
      return 0;
    case FL_FOCUS:
      focus_row = current_row();
      if (focus_row < 0)
        for (size_t i = 0; i < rows.size(); ++i)
          if (rows[i].place >= 0) {
            focus_row = int(i);
            break;
          }
      redraw();
      return 1;
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      if (d_->shortcut(key)) return 1;
      if (key == FL_Up || key == FL_Down) {
        int r = focus_row;
        for (int i = 0; i < int(rows.size()); ++i) {
          r += key == FL_Down ? 1 : -1;
          if (r < 0 || r >= int(rows.size())) return 1;
          if (rows[size_t(r)].place >= 0) break;
        }
        focus_row = r;
        reveal(r);
        const Place &p = d_->places[size_t(rows[size_t(r)].place)];
        d_->open_location(p.kind == Place::Kind::Recent ? "recent:" : p.path);
        take_focus();
        redraw();
        return 1;
      }
      if (key == FL_Enter || key == FL_KP_Enter || key == ' ') {
        if (focus_row >= 0 && rows[size_t(focus_row)].place >= 0) {
          const Place &p = d_->places[size_t(rows[size_t(focus_row)].place)];
          d_->open_location(p.kind == Place::Kind::Recent ? "recent:" : p.path);
        }
        return 1;
      }
      if (key == FL_BackSpace) {
        d_->go_up();
        return 1;
      }
      return 0;
    }
  }
  return Fl_Widget::handle(e);
}

int FileView::handle(int e) {
  int mx = Fl::event_x(), my = Fl::event_y();
  if (e != FL_PUSH || !Fl::event_inside(bar) || !bar->visible()) {
    // fall through to our own handling
  } else {
    return Fl_Group::handle(e);
  }
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      if (bar->visible() && Fl::event_inside(bar)) {
        Fl_Group::handle(e);
        if (hover != -1) {
          hover = -1;
          redraw();
        }
        return 1;
      }
      int k = item_at(mx, my);
      int hc = list() && my >= y() && my < y() + head_h ? column_at(mx) : -1;
      if (k != hover || hc != header_col_hover) {
        hover = k;
        header_col_hover = hc;
        redraw();
        if (k >= 0) {
          // The whole name, when it was shortened, and where a found file is.
          const Entry &en = d_->at(k);
          static std::string tip;
          tip = en.display;
          if (!en.folder.empty()) tip += "\n" + display_path(en.folder);
          int X, Y, W, H;
          item_rect(k, X, Y, W, H);
          Fl_Tooltip::enter_area(this, X, Y, W, H, tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      if (hover != -1 || header_col_hover != -1) {
        hover = -1;
        header_col_hover = -1;
        redraw();
      }
      return 1;
    case FL_PUSH: {
      take_focus();
      int b = Fl::event_button();
      if (b == FL_BACK_MOUSE || b == FL_FORWARD_MOUSE) return 0;
      if (list() && my >= y() && my < y() + head_h && b == FL_LEFT_MOUSE) {
        int c = column_at(mx);
        if (c >= 0) d_->sort_by(columns()[size_t(c)].key);
        return 1;
      }
      int k = item_at(mx, my);
      if (b == FL_RIGHT_MOUSE) {
        if (k >= 0 && !d_->selected.count(d_->at(k).path)) d_->select_only(d_->at(k).path);
        d_->item_menu(k);
        return 1;
      }
      if (b != FL_LEFT_MOUSE) return 1;
      if (k < 0) {
        if (!(Fl::event_state() & (FL_CTRL | FL_SHIFT))) {
          d_->selected.clear();
          d_->selection_changed();
        }
        return 1;
      }
      const Entry &en = d_->at(k);
      if (Fl::event_clicks() && d_->focus_path == en.path) {
        Fl::event_clicks(0);
        d_->activate(k);
        return 1;
      }
      if (!d_->selectable(en)) return 1;
      bool multi = is_multi(d_->mode);
      if (multi && (Fl::event_state() & FL_CTRL)) {
        if (d_->selected.count(en.path)) d_->selected.erase(en.path);
        else d_->selected.insert(en.path);
        d_->focus_path = d_->anchor_path = en.path;
      } else if (multi && (Fl::event_state() & FL_SHIFT) && d_->index(d_->anchor_path) >= 0) {
        int a = d_->index(d_->anchor_path);
        d_->selected.clear();
        for (int i = std::min(a, k); i <= std::max(a, k); ++i)
          if (d_->selectable(d_->at(i))) d_->selected.insert(d_->at(i).path);
        d_->focus_path = en.path;
      } else {
        d_->select_only(en.path);
        return 1;
      }
      d_->selection_changed();
      return 1;
    }
    case FL_MOUSEWHEEL:
      if (Fl::event_dy()) {
        set_scroll(scroll + Fl::event_dy() * (list() ? row_h * 3 : tile_h / 2));
        return 1;
      }
      return 0;
    case FL_FOCUS:
      if (d_->focus_path.empty() && d_->count() > 0) {
        int k = 0;
        while (k < d_->count() && !d_->selectable(d_->at(k))) ++k;
        if (k < d_->count()) d_->focus_path = d_->at(k).path;
      }
      redraw();
      return 1;
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      if (d_->shortcut(key)) return 1;
      int n = d_->count();
      int k = d_->index(d_->focus_path);
      bool shift = Fl::event_state() & FL_SHIFT, ctrl = Fl::event_state() & FL_CTRL;
      int step = 0;
      metrics();
      int page = std::max(1, items_h() / (list() ? row_h : tile_h)) * (list() ? 1 : cols);
      switch (key) {
        case FL_Left: step = list() ? 0 : -1; break;
        case FL_Right: step = list() ? 0 : 1; break;
        case FL_Up: step = list() ? -1 : -cols; break;
        case FL_Down: step = list() ? 1 : cols; break;
        case FL_Page_Up: step = -page; break;
        case FL_Page_Down: step = page; break;
        case FL_Home: step = -n; break;
        case FL_End: step = n; break;
        case FL_Enter:
        case FL_KP_Enter:
          if (k >= 0) d_->activate(k);
          else d_->accept();
          return 1;
        case FL_BackSpace:
          if (!typed.empty() && now_ms() - typed_at < 1200) {
            typed.erase(typed.size() - 1);
            typed_at = now_ms();
          } else {
            d_->go_up();
          }
          return 1;
        default:
          break;
      }
      if (step && n > 0) {
        int target = k < 0 ? (step > 0 ? 0 : n - 1) : std::max(0, std::min(n - 1, k + step));
        const Entry &en = d_->at(target);
        if (shift && is_multi(d_->mode)) {
          int a = d_->index(d_->anchor_path);
          if (a < 0) a = k < 0 ? target : k;
          d_->selected.clear();
          for (int i = std::min(a, target); i <= std::max(a, target); ++i)
            if (d_->selectable(d_->at(i))) d_->selected.insert(d_->at(i).path);
          d_->anchor_path = d_->at(a).path;
          d_->focus_path = en.path;
          d_->selection_changed();
        } else if (ctrl && is_multi(d_->mode)) {
          d_->focus_path = en.path;
          redraw();
        } else if (d_->selectable(en)) {
          d_->select_only(en.path);
        } else {
          d_->focus_path = en.path;
          redraw();
        }
        ensure_visible(target);
        return 1;
      }
      if (key == ' ' && is_multi(d_->mode) && typed.empty() && k >= 0) {
        const Entry &en = d_->at(k);
        if (d_->selectable(en)) {
          if (d_->selected.count(en.path)) d_->selected.erase(en.path);
          else d_->selected.insert(en.path);
          d_->selection_changed();
        }
        return 1;
      }
      if (ctrl && (key == 'a' || key == 'A') && is_multi(d_->mode)) {
        d_->selected.clear();
        for (int i = 0; i < n; ++i)
          if (d_->selectable(d_->at(i)) && (!d_->at(i).dir || is_folder_mode(d_->mode)))
            d_->selected.insert(d_->at(i).path);
        d_->selection_changed();
        return 1;
      }
      // Type-ahead: the first name that starts with what was typed.
      const char *text = Fl::event_text();
      if (!ctrl && !(Fl::event_state() & (FL_ALT | FL_META)) && text && *text && uint8_t(text[0]) >= 0x20 &&
          text[0] != 0x7f) {
        if (typed.empty() && (text[0] == '/' || text[0] == '~')) {
          d_->start_edit(text);
          return 1;
        }
        if (now_ms() - typed_at > 1200) typed.clear();
        typed += text;
        typed_at = now_ms();
        std::string want = chooser::fold_text(typed);
        int start = typed.size() == strlen(text) && k >= 0 ? k + 1 : std::max(0, k);
        for (int i = 0; i < n; ++i) {
          int c = (start + i) % n;
          if (d_->at(c).fold.compare(0, want.size(), want) == 0) {
            if (d_->selectable(d_->at(c))) d_->select_only(d_->at(c).path);
            else d_->focus_path = d_->at(c).path;
            ensure_visible(c);
            redraw();
            break;
          }
        }
        return 1;
      }
      return 0;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
      if (!drop_hover) {
        drop_hover = true;
        redraw();
      }
      return 1;
    case FL_DND_LEAVE:
      drop_hover = false;
      redraw();
      return 1;
    case FL_DND_RELEASE:
      drop_hover = false;
      redraw();
      return 1;
    case FL_PASTE:
      if (Fl::event_text()) d_->drop(Fl::event_text());
      return 1;
  }
  return Fl_Group::handle(e);
}

void CompletionList::set(std::vector<std::string> v) {
  items = std::move(v);
  current = -1;
  hover = -1;
  first = 0;
  if (items.empty() || !d_->editing) {
    if (visible()) {
      hide();
      window()->redraw();
    }
    return;
  }
  resize(d_->pathbar->x(), d_->pathbar->y() + d_->pathbar->h() + S(4), d_->pathbar->w(), rows() * row_h() + S(8));
  show();
  window()->redraw();
}

int CompletionList::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = first + (Fl::event_y() - y() - S(4)) / row_h();
      if (k < first || k >= first + rows()) k = -1;
      if (k != hover) {
        hover = k;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      int k = first + (Fl::event_y() - y() - S(4)) / row_h();
      if (k >= first && k < first + rows()) {
        Field *entry = d_->pathbar->entry;
        entry->value(items[size_t(k)].c_str());
        entry->insert_position(entry->size());
        entry->take_focus();
        d_->completions_changed();
      }
      return 1;
    }
    case FL_MOUSEWHEEL:
      first = std::max(0, std::min(first + Fl::event_dy(), int(items.size()) - rows()));
      redraw();
      return 1;
  }
  return Fl_Widget::handle(e);
}

void NewFolderBox::close() {
  hide();
  error.clear();
  if (window()) window()->redraw();
  d_->view->take_focus();
}

void NewFolderBox::create_cb(Fl_Widget *, void *v) {
  auto *self = static_cast<NewFolderBox *>(v);
  std::string err;
  std::string name = trim(self->input->value());
  if (self->d_->create_folder(name, &err)) {
    self->close();
  } else {
    self->error = err;
    self->redraw();
  }
}

}  // namespace


// ================================================================== the dialog

namespace {

void (*g_prev_before_map)(Fl_Window *, Window) = nullptr;
Fl_Window *g_mapping = nullptr;
Window g_transient_for = 0;

// Before the dialog is mapped: a dialog window, transient for its parent
// (also a window of another program, for the portal).
void dialog_before_map(Fl_Window *win, Window xid) {
  if (g_prev_before_map) g_prev_before_map(win, xid);
  if (win != g_mapping) return;
  if (g_transient_for) XSetTransientForHint(fl_display, xid, g_transient_for);
  Atom type = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE", False);
  Atom dialog = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  XChangeProperty(fl_display, xid, type, XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char *>(&dialog), 1);
}

std::string join_path_of(const std::string &dir, const std::string &name) {
  if (dir.empty()) return name;
  return dir.back() == '/' ? dir + name : dir + "/" + name;
}

void select_stem(Fl_Input *in) {
  std::string v = in->value();
  auto r = chooser::stem_range(v);
  in->insert_position(int(r.second), int(r.first));
}

Dialog::Dialog(FileChooser::Options &opts)
    : BufferedWindow(S(860), S(560)),
      o(opts),
      mode(opts.mode),
      waker([this] { pump(); }),
      reader([this] { waker.wake(); }),
      searcher([this] { waker.wake(); }),
      thumbnailer([this] { waker.wake(); }, 2) {
  if (active_theme().font.empty()) {
    Config cfg;
    cfg.load();
    Theme::from_config(cfg).apply_to_fltk();
  }
  register_field_box();
  const Theme &t = active_theme();
  state = chooser::State::load();
  preview_forced = o.preview >= 0;
  preview_on = preview_forced ? o.preview != 0 : state.show_preview;
  filters = o.filters;
  bool add_all = o.all_files >= 0 ? o.all_files != 0 : is_open(mode) && !filters.empty();
  if (add_all && std::none_of(filters.begin(), filters.end(), [](const Filter &f) { return f.all(); })) {
    Filter all;
    all.name = _("All files");
    all.patterns = {"*"};
    filters.push_back(all);
  }
  int first = o.filter;
  if (first < 0 && mode == Mode::Save && !o.file_name.empty())
    for (size_t i = 0; i < filters.size() && first < 0; ++i)
      if (!filters[i].all() && filters[i].matches_file(basename_of(o.file_name), "")) first = int(i);
  filter = filters.empty() ? -1 : std::max(0, std::min(first, int(filters.size()) - 1));

  color(t.background.fl());
  begin();
  auto button = [this](const char *g, const char *tip, void (*cb)(Fl_Widget *, void *)) {
    auto *b = new IconButton(0, 0, 10, 10, g, tip);
    b->callback(cb, this);
    return b;
  };
  back_btn = button("back", _("Back (Alt+Left)"), [](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->go_back(); });
  forward_btn =
      button("forward", _("Forward (Alt+Right)"), [](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->go_forward(); });
  up_btn = button("up", _("Enclosing folder (Alt+Up)"), [](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->go_up(); });
  pathbar = new PathBar(this, 0, 0, 100, 34);
  pathbar->entry->keys = [this](int key) {
    switch (key) {
      case FL_Tab:
        if (completion->visible()) {
          std::string pick = completion->current >= 0 ? completion->items[size_t(completion->current)]
                                                      : chooser::common_prefix(completion->items);
          if (pick.size() > strlen(pathbar->entry->value()) || completion->current >= 0) {
            pathbar->entry->value(pick.c_str());
            pathbar->entry->insert_position(pathbar->entry->size());
            completions_changed();
          }
          return true;
        }
        return false;
      case FL_Down:
      case FL_Up:
        if (completion->visible()) {
          completion->move_current(key == FL_Down ? 1 : -1);
          return true;
        }
        return false;
      case FL_Enter:
      case FL_KP_Enter: {
        std::string typed = pathbar->entry->value();
        if (completion->visible() && completion->current >= 0) typed = completion->items[size_t(completion->current)];
        choose_typed(typed);
        return true;
      }
      case FL_Escape:
        if (completion->visible()) completion->set({});
        else stop_edit();
        return true;
    }
    return false;
  };
  pathbar->search->placeholder = _("Search");
  pathbar->search->keys = [this](int key) {
    if (key == FL_Escape) {
      if (!query.empty()) {
        pathbar->search->value("");
        set_query("");
      } else {
        close_search();
      }
      return true;
    }
    if (key == FL_Enter || key == FL_KP_Enter || key == FL_Down) {
      if (count() > 0) {
        int k = 0;
        while (k < count() && !selectable(at(k))) ++k;
        if (k < count()) select_only(at(k).path);
        view->take_focus();
      }
      return true;
    }
    return false;
  };
  search_btn = button("search", _("Search (Ctrl+F)"), [](Fl_Widget *, void *d) {
    auto *self = static_cast<Dialog *>(d);
    if (self->search_open) self->close_search();
    else self->open_search();
  });
  grid_btn = button("grid", _("Grid (Ctrl+1)"), [](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->set_list_view(false); });
  grid_btn->segment = 1;
  list_btn = button("list", _("List (Ctrl+2)"), [](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->set_list_view(true); });
  list_btn->segment = 2;
  pane_btn = button("pane", _("Preview"), [](Fl_Widget *, void *d) {
    auto *self = static_cast<Dialog *>(d);
    self->set_preview(!self->preview_on);
  });
  menu_btn = button("menu", _("View options"), [](Fl_Widget *w, void *d) {
    static_cast<Dialog *>(d)->options_menu(w->x(), w->y() + w->h() + S(2));
  });
  placesv = new PlacesView(this, 0, 0, 100, 100);
  view = new FileView(this, 0, 0, 100, 100);
  preview = new PreviewPane(this, 0, 0, 100, 100);
  if (mode == Mode::Save) {
    name_input = new Field(this, 0, 0, 100, 34, _("File name"));
    name_input->placeholder = _("File name");
    name_input->when(FL_WHEN_CHANGED);
    name_input->callback([](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->update_buttons(); }, this);
    name_input->keys = [this](int key) {
      if (key == FL_Enter || key == FL_KP_Enter) {
        accept();
        return true;
      }
      return false;
    };
  }
  if (!filters.empty() && !is_folder_mode(mode)) {
    filter_choice = new Fl_Choice(0, 0, 100, 34);
    filter_choice->tooltip(_("Show only these files"));
    filter_labels.reserve(filters.size());
    for (const auto &f : filters) {
      std::string label;
      for (char c : f.name) label += c == '&' ? std::string("&&") : std::string(1, c);
      filter_labels.push_back(label);
    }
    for (const auto &l : filter_labels) {
      Fl_Menu_Item it{};
      it.text = l.c_str();
      it.labelsize_ = uchar(std::min(255, t.font_size));
      filter_items.push_back(it);
    }
    filter_items.push_back(Fl_Menu_Item{});
    filter_choice->menu(filter_items.data());
    filter_choice->textsize(t.font_size);
    filter_choice->value(std::max(0, filter));
    filter_choice->callback([](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->filter_changed(); }, this);
  }
  hidden_check = new Fl_Check_Button(0, 0, 100, 30, _("Show hidden files"));
  hidden_check->labelsize(t.font_size);
  hidden_check->tooltip(_("Show hidden files (Ctrl+H)"));
  hidden_check->callback([](Fl_Widget *w, void *d) {
    static_cast<Dialog *>(d)->set_hidden(static_cast<Fl_Check_Button *>(w)->value() != 0);
  }, this);
  newfolder_btn = new FlatButton(0, 0, 100, 34, _("New Folder"));
  newfolder_btn->copy_label(_("New Folder"));
  newfolder_btn->tooltip(_("New Folder (Ctrl+Shift+N)"));
  newfolder_btn->callback([](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->new_folder(); }, this);
  if (!o.new_folder) newfolder_btn->hide();
  cancel_btn = new FlatButton(0, 0, 100, 34, _("Cancel"));
  cancel_btn->copy_label(_("Cancel"));
  cancel_btn->callback([](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->cancel(); }, this);
  std::string accept_text = o.accept_label;
  if (accept_text.empty()) {
    switch (mode) {
      case Mode::Save: accept_text = _("Save"); break;
      case Mode::Folder: accept_text = _("Select Folder"); break;
      case Mode::Folders: accept_text = _("Select Folders"); break;
      default: accept_text = _("Open"); break;
    }
  }
  accept_btn = new FlatButton(0, 0, 100, 34, accept_text, true);
  accept_btn->copy_label(accept_text.c_str());
  accept_btn->callback([](Fl_Widget *, void *d) { static_cast<Dialog *>(d)->accept(); }, this);
  completion = new CompletionList(this);
  newfolder_box = new NewFolderBox(this);
  end();
  if (filter_choice) apply_stock_style(filter_choice);
  apply_stock_style(hidden_check);
  hidden_check->labelcolor(t.text.fl());
  hidden_check->value(state.show_hidden);
  std::string title = o.title;
  if (title.empty()) {
    switch (mode) {
      case Mode::OpenMultiple: title = _("Open Files"); break;
      case Mode::Save: title = _("Save File"); break;
      case Mode::Folder: title = _("Select a Folder"); break;
      case Mode::Folders: title = _("Select Folders"); break;
      default: title = _("Open a File"); break;
    }
  }
  copy_label(title.c_str());
  xclass("sde-file-chooser");
  callback([](Fl_Widget *w, void *) { static_cast<Dialog *>(w)->cancel(); });
  refresh_places(true);
  layout();
}

Dialog::~Dialog() {
  Fl::remove_timeout(deep_search_cb, this);
  Fl::remove_timeout(request_thumbnails_cb, this);
  Fl::remove_timeout(poll_places, this);
  Fl::remove_timeout(clear_message, this);
  Fl::remove_timeout(qa_dump_cb, this);
  for (auto &kv : thumbs)
    if (kv.second.surface) cairo_surface_destroy(kv.second.surface);
  thumbs.clear();
}

std::vector<const Entry *> Dialog::selection() const {
  std::vector<const Entry *> out;
  for (const auto &p : selected) {
    int k = index(p);
    if (k >= 0) out.push_back(&at(k));
  }
  std::sort(out.begin(), out.end(), [this](const Entry *a, const Entry *b) { return index(a->path) < index(b->path); });
  return out;
}

const std::vector<std::string> &Dialog::folder_icon_names(const std::string &path) {
  auto it = folder_icons.find(path);
  if (it != folder_icons.end()) return it->second;
  if (folder_icons.empty()) {
    static const struct {
      const char *key, *icon;
    } dirs[] = {{"DESKTOP", "user-desktop"},       {"DOCUMENTS", "folder-documents"}, {"DOWNLOAD", "folder-download"},
                {"MUSIC", "folder-music"},         {"PICTURES", "folder-pictures"},   {"VIDEOS", "folder-videos"},
                {"TEMPLATES", "folder-templates"}, {"PUBLICSHARE", "folder-publicshare"}};
    std::string home = home_dir();
    for (const auto &d : dirs) {
      std::string p = chooser::normalize_path(user_dir(d.key));
      if (!p.empty() && p != home) folder_icons[p] = {d.icon, "folder"};
    }
    folder_icons[home] = {"user-home", "folder"};
    it = folder_icons.find(path);
    if (it != folder_icons.end()) return it->second;
  }
  static const std::vector<std::string> plain{"folder"};
  return plain;
}

const std::vector<std::string> &Dialog::dir_icon_names(const Entry &e) {
  if (e.dir) return folder_icon_names(e.path);
  auto it = icon_cache.find(e.mime);
  if (it == icon_cache.end()) it = icon_cache.emplace(e.mime, chooser::mime_icon_names(e.mime)).first;
  return it->second;
}

const std::string &Dialog::type_text(const std::string &mime) {
  auto it = type_cache.find(mime);
  if (it == type_cache.end()) it = type_cache.emplace(mime, chooser::mime_description(mime)).first;
  return it->second;
}

std::string Dialog::folder_label(const std::string &path) const {
  if (path == home_dir()) return _("Home");
  if (path == "/") return _("File System");
  return take_gstr(g_filename_display_basename(path.c_str()));
}

std::string Dialog::status_text() const {
  if (loading) return format(_("Loading… %s items"), thousands(long(entries.size())).c_str());
  if (!error.empty()) return error;
  int folders = 0, files = 0;
  for (int k = 0; k < count(); ++k) at(k).dir ? ++folders : ++files;
  std::string s;
  if (!query.empty()) {
    s = format(ngettext("%s result", "%s results", count()), thousands(count()).c_str());
    if (searching_deep) s = format(_("Searching… %s"), s.c_str());
  } else if (folders && files) {
    s = format(ngettext("%d folder", "%d folders", folders), folders) + ", " +
        format(ngettext("%d file", "%d files", files), files);
  } else if (folders) {
    s = format(ngettext("%d folder", "%d folders", folders), folders);
  } else {
    s = format(ngettext("%d file", "%d files", files), files);
  }
  if (selected.size() > 1) {
    long long total = 0;
    for (const auto &p : selected) {
      int k = index(p);
      if (k >= 0 && !at(k).dir) total += at(k).size;
    }
    s += " · " + format(ngettext("%d selected", "%d selected", int(selected.size())), int(selected.size()));
    if (total > 0) s += " (" + chooser::size_text(total) + ")";
  }
  return s;
}

// ---------------------------------------------------------------- places

void Dialog::refresh_places(bool force) {
  std::string mounts, marks;
  read_file("/proc/self/mountinfo", mounts);
  if (!read_file(config_home() + "/gtk-3.0/bookmarks", marks)) read_file(home_dir() + "/.gtk-bookmarks", marks);
  if (!force && mounts == mounts_text && marks == bookmarks_text) return;
  mounts_text = mounts;
  bookmarks_text = marks;
  places = chooser::standard_places(!is_folder_mode(mode));
  std::set<std::string> seen;
  for (const auto &p : places)
    if (!p.path.empty()) seen.insert(p.path);
  for (auto &b : chooser::read_bookmarks())
    if (seen.insert(b.path).second) places.push_back(b);
  for (auto &d : chooser::parse_mounts(mounts, user_name(), home_dir()))
    if (seen.insert(d.path).second && dir_exists(d.path)) places.push_back(d);
  Place root;
  root.kind = Place::Kind::Root;
  root.id = "root";
  root.label = _("File System");
  root.path = "/";
  root.icon = "root";
  places.push_back(root);
  for (auto &n : chooser::network_places()) places.push_back(n);
  if (placesv) placesv->rebuild();
  if (pathbar) pathbar->rebuild();
}

void Dialog::poll_places(void *d) {
  auto *self = static_cast<Dialog *>(d);
  self->refresh_places();
  Fl::repeat_timeout(2.0, poll_places, d);
}

// ---------------------------------------------------------------- moving around

void Dialog::open_location(const std::string &loc_in, bool record) {
  std::string loc = loc_in == "recent:" ? loc_in : chooser::normalize_path(loc_in);
  if (loc.empty()) return;
  if (loc != "recent:" && !dir_exists(loc)) {
    say(format(_("“%s” cannot be found"), display_path(loc).c_str()), true);
    return;
  }
  if (editing) stop_edit(false);
  if (search_open) {
    search_open = false;
    pathbar->search->hide();
    query.clear();
    folded_query.clear();
    pathbar->search->value("");
    searcher.cancel();
    searching_deep = false;
    Fl::remove_timeout(deep_search_cb, this);
  }
  newfolder_box->hide();
  if (record) history.visit(loc);
  if (!(crumb_tail == loc || (loc != "recent:" && starts_with(crumb_tail, loc == "/" ? "/" : loc + "/"))))
    crumb_tail = loc;
  location = loc;
  entries.clear();
  found.clear();
  listed.clear();
  index_of.clear();
  selected.clear();
  focus_path.clear();
  anchor_path.clear();
  error.clear();
  message.clear();
  preview_count = -1;
  loading = true;
  view->scroll = 0;
  view->hover = -1;
  view->wraps.clear();
  if (thumbs.size() > 1200) {
    for (auto &kv : thumbs)
      if (kv.second.surface) cairo_surface_destroy(kv.second.surface);
    thumbs.clear();
  }
  generation = reader.start(loc);
  free_bytes = -1;
  struct statvfs vfs {};
  if (real_folder(loc) && statvfs(loc.c_str(), &vfs) == 0) free_bytes = (long long)vfs.f_bavail * (long long)vfs.f_frsize;
  pathbar->rebuild();
  placesv->reveal(placesv->current_row());
  placesv->redraw();
  view->update_bar();
  update_buttons();
  redraw();
}

void Dialog::go_back() {
  if (!history.can_back()) return;
  std::string from = location;
  std::string loc = history.back();
  if (loc.empty()) return;
  open_location(loc, false);
  // Back from a subfolder: that subfolder is selected.
  if (real_folder(loc) && starts_with(from, loc == "/" ? "/" : loc + "/")) {
    size_t start = loc == "/" ? 1 : loc.size() + 1;
    size_t slash = from.find('/', start);
    select_when_loaded = slash == std::string::npos ? from : from.substr(0, slash);
  }
}

void Dialog::go_forward() {
  if (!history.can_forward()) return;
  std::string loc = history.forward();
  if (!loc.empty()) open_location(loc, false);
}

void Dialog::go_up() {
  if (!real_folder(location) || location == "/") return;
  std::string from = location, up = chooser::parent_dir(location);
  if (up.empty()) return;
  open_location(up);
  select_when_loaded = from;
}

void Dialog::reload() {
  std::string keep = focus_path;
  std::set<std::string> keep_all = selected;
  open_location(location, false);
  select_when_loaded = keep;
  also_select = keep_all;
}

// ---------------------------------------------------------------- loading

void Dialog::pump() {
  bool changed = false;
  chooser::FolderReader::Batch b;
  while (reader.take(b)) {
    if (b.generation != generation) continue;
    for (auto &e : b.entries) entries.push_back(std::move(e));
    if (b.done) {
      loading = false;
      error = b.error;
      if (loaded_ms < 0) loaded_ms = now_ms() - opened_at;
    }
    changed = true;
  }
  while (searcher.take(b)) {
    if (b.generation != search_generation) continue;
    for (auto &e : b.entries) found.push_back(std::move(e));
    if (b.done) searching_deep = false;
    changed = true;
  }
  if (changed) refilter();
  bool view_dirty = false, preview_dirty = false;
  for (auto &r : thumbnailer.take()) {
    std::string key = (r.job.picture ? "p" : "") + std::to_string(r.job.size) + "\n" + r.job.path;
    Thumb &th = thumbs[key];
    if (th.surface) cairo_surface_destroy(th.surface);
    th.surface = r.surface;
    th.w = r.width;
    th.h = r.height;
    if (r.job.picture || selected.count(r.job.path)) preview_dirty = true;
    if (!r.job.picture) view_dirty = true;
  }
  if (view_dirty) view->redraw();
  if (preview_dirty && preview_shown) preview->redraw();
  if (changed || view_dirty || preview_dirty) {
    Fl::remove_timeout(qa_dump_cb, this);
    Fl::add_timeout(0.05, qa_dump_cb, this);
  }
}

void Dialog::refilter() {
  listed.clear();
  const Filter *f = is_folder_mode(mode) ? nullptr : current_filter();
  bool searching = !folded_query.empty();
  auto keep = [&](const Entry &e) {
    if (e.hidden && !state.show_hidden) return false;
    if (f && !f->matches(e)) return false;
    if (searching && e.fold.find(folded_query) == std::string::npos) return false;
    return true;
  };
  for (size_t i = 0; i < entries.size(); ++i)
    if (keep(entries[i])) listed.push_back(int(i));
  if (searching)
    for (size_t i = 0; i < found.size(); ++i)
      if (found[i].folder != location && keep(found[i])) listed.push_back(-1 - int(i));
  chooser::SortOrder by = order();
  std::stable_sort(listed.begin(), listed.end(), [&](int a, int b) {
    const Entry &x = a >= 0 ? entries[size_t(a)] : found[size_t(-1 - a)];
    const Entry &y = b >= 0 ? entries[size_t(b)] : found[size_t(-1 - b)];
    return chooser::entry_less(x, y, by);
  });
  index_of.clear();
  index_of.reserve(listed.size() * 2);
  for (int k = 0; k < count(); ++k) index_of.emplace(at(k).path, k);
  bool sel_changed = false;
  for (auto it = selected.begin(); it != selected.end();) {
    if (!index_of.count(*it)) {
      it = selected.erase(it);
      sel_changed = true;
    } else {
      ++it;
    }
  }
  if (!focus_path.empty() && !index_of.count(focus_path)) focus_path.clear();
  if (!select_when_loaded.empty()) {
    int k = index(select_when_loaded);
    if (k >= 0) {
      if (selectable(at(k))) {
        selected.clear();
        selected.insert(at(k).path);
        if (is_multi(mode))
          for (const auto &p : also_select)
            if (index(p) >= 0 && selectable(at(index(p)))) selected.insert(p);
        anchor_path = at(k).path;
      }
      focus_path = at(k).path;
      view->update_bar();
      view->ensure_visible(k);
      select_when_loaded.clear();
      also_select.clear();
      sel_changed = true;
    } else if (!loading) {
      select_when_loaded.clear();
      also_select.clear();
    }
  }
  view->update_bar();
  view->redraw();
  if (sel_changed) selection_changed();
  else update_buttons();
  if (preview_shown) preview->redraw();
  schedule_thumbnails();
}

void Dialog::set_query(const std::string &q) {
  if (q == query) return;
  query = q;
  folded_query = chooser::fold_text(q);
  found.clear();
  searcher.cancel();
  searching_deep = false;
  Fl::remove_timeout(deep_search_cb, this);
  view->scroll = 0;
  refilter();
  if (folded_query.size() >= 2 && real_folder(location)) Fl::add_timeout(0.35, deep_search_cb, this);
  pathbar->redraw();
}

void Dialog::deep_search_cb(void *d) {
  auto *self = static_cast<Dialog *>(d);
  if (self->folded_query.empty() || !real_folder(self->location)) return;
  self->found.clear();
  self->searching_deep = true;
  self->search_generation = self->searcher.start_search(self->location, self->query, self->state.show_hidden);
  self->view->redraw();
}

// ---------------------------------------------------------------- selection

void Dialog::select_only(const std::string &path) {
  selected.clear();
  int k = index(path);
  if (k >= 0 && selectable(at(k))) selected.insert(path);
  focus_path = anchor_path = path;
  if (k >= 0) view->ensure_visible(k);
  selection_changed();
}

void Dialog::selection_changed() {
  auto sel = selection();
  if (mode == Mode::Save && sel.size() == 1 && !sel[0]->dir && name_input) {
    name_input->value(sel[0]->display.c_str());
    select_stem(name_input);
  }
  preview_count = -1;
  if (sel.size() == 1 && sel[0]->dir) {
    // How many items the folder has (up to 5000 are counted).
    long n = 0;
    if (DIR *d = opendir(sel[0]->path.c_str())) {
      while (struct dirent *de = readdir(d)) {
        if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))) continue;
        if (de->d_name[0] == '.' && !state.show_hidden) continue;
        if (++n >= 5000) break;
      }
      closedir(d);
      preview_count = n;
    }
  }
  update_buttons();
  view->redraw();
  if (preview_shown) preview->redraw();
  schedule_thumbnails();
}

void Dialog::activate(int k) {
  if (k < 0 || k >= count()) return;
  const Entry &e = at(k);
  if (e.dir) {
    if (!e.folder.empty() || location == "recent:") crumb_tail.clear();
    std::string path = e.path;
    open_location(path);
    view->take_focus();
    return;
  }
  if (!selectable(e)) return;
  if (is_open(mode)) {
    if (!selected.count(e.path)) select_only(e.path);
    accept();
  } else if (mode == Mode::Save && name_input) {
    std::string name = e.display, folder = e.folder;
    if (!folder.empty() && folder != location) open_location(folder);
    name_input->value(name.c_str());
    accept();
  }
}

// ---------------------------------------------------------------- choosing

void Dialog::update_buttons() {
  auto set = [](Fl_Widget *w, bool on) {
    if (on) w->activate();
    else w->deactivate();
    w->redraw();
  };
  set(back_btn, history.can_back());
  set(forward_btn, history.can_forward());
  set(up_btn, real_folder(location) && location != "/");
  set(newfolder_btn, real_folder(location));
  bool ok = false;
  auto sel = selection();
  switch (mode) {
    case Mode::Open:
    case Mode::OpenMultiple:
      ok = std::any_of(sel.begin(), sel.end(), [](const Entry *e) { return !e->dir; }) || (sel.size() == 1 && sel[0]->dir);
      break;
    case Mode::Save: {
      std::string name = name_input ? name_input->value() : "";
      ok = !name.empty() && (real_folder(location) || name.find('/') != std::string::npos || name[0] == '~');
      break;
    }
    case Mode::Folder:
    case Mode::Folders:
      ok = real_folder(location) || std::any_of(sel.begin(), sel.end(), [](const Entry *e) { return e->dir; });
      break;
  }
  set(accept_btn, ok);
  grid_btn->on = !state.list_view;
  list_btn->on = state.list_view;
  pane_btn->on = preview_on;
  search_btn->on = search_open;
  hidden_check->value(state.show_hidden);
  for (Fl_Widget *w : {static_cast<Fl_Widget *>(grid_btn), static_cast<Fl_Widget *>(list_btn),
                       static_cast<Fl_Widget *>(pane_btn), static_cast<Fl_Widget *>(search_btn),
                       static_cast<Fl_Widget *>(hidden_check)})
    w->redraw();
  Fl::remove_timeout(qa_dump_cb, this);
  Fl::add_timeout(0.05, qa_dump_cb, this);
}

void Dialog::finish(const std::vector<std::string> &paths) {
  result = paths;
  accepted = true;
  done = true;
  if (o.recent)
    for (const auto &p : paths)
      if (!dir_exists(p)) chooser::add_recent(p, chooser::guess_mime(basename_of(p), false), o.program);
  remember();
  BufferedWindow::hide();
}

void Dialog::accept() {
  if (editing) {
    choose_typed(pathbar->entry->value());
    return;
  }
  if (!accept_btn->active()) return;
  auto sel = selection();
  switch (mode) {
    case Mode::Open:
    case Mode::OpenMultiple: {
      std::vector<std::string> files;
      for (const Entry *e : sel)
        if (!e->dir) files.push_back(e->path);
      if (files.empty()) {
        if (sel.size() == 1 && sel[0]->dir) {
          std::string path = sel[0]->path;
          open_location(path);
          view->take_focus();
        }
        return;
      }
      if (mode == Mode::Open) files.resize(1);
      finish(files);
      return;
    }
    case Mode::Save: {
      std::string name = name_input->value();
      while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
      if (name.empty()) return;
      std::string cwd = real_folder(location) ? location : std::string();
      std::string path;
      if (name.find('/') != std::string::npos || name[0] == '~') {
        path = chooser::expand_location(name, cwd.empty() ? home_dir() : cwd);
      } else if (cwd.empty()) {
        say(_("Choose the folder to save in first."), true);
        return;
      } else {
        path = join_path_of(cwd, name);
      }
      if (path.empty()) return;
      if (dir_exists(path)) {
        // The name of a folder: go into it, as GTK does.
        open_location(path);
        name_input->value("");
        update_buttons();
        name_input->take_focus();
        return;
      }
      std::string dir = chooser::parent_dir(path), base = basename_of(path);
      if (base.empty() || base == "." || base == "..") return;
      if (!dir_exists(dir)) {
        say(format(_("The folder “%s” does not exist."), display_path(dir).c_str()), true);
        return;
      }
      if (const Filter *f = current_filter()) {
        std::string fixed = chooser::name_for_filter(base, *f, filters);
        if (fixed != base) {
          base = fixed;
          path = join_path_of(dir, base);
        }
      }
      if (access(dir.c_str(), W_OK) != 0) {
        say(_("You cannot save files in this folder."), true);
        return;
      }
      struct stat st {};
      if (lstat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return;
        if (o.confirm_overwrite) {
          std::string shown_name = take_gstr(g_filename_display_name(base.c_str()));
          std::string folder_name = take_gstr(g_filename_display_basename(dir.c_str()));
          fl_message_title(_("Replace File?"));
          int r = fl_choice(_("A file named “%s” already exists in “%s”.\nDo you want to replace it?"), _("Cancel"),
                            _("Replace"), nullptr, shown_name.c_str(), folder_name.c_str());
          if (r != 1) {
            name_input->take_focus();
            return;
          }
        }
      }
      finish({path});
      return;
    }
    case Mode::Folder:
    case Mode::Folders: {
      std::vector<std::string> dirs;
      for (const Entry *e : sel)
        if (e->dir) dirs.push_back(e->path);
      if (dirs.empty() && real_folder(location)) dirs.push_back(location);
      if (dirs.empty()) return;
      if (mode == Mode::Folder) dirs.resize(1);
      finish(dirs);
      return;
    }
  }
}

void Dialog::cancel() {
  if (done) return;
  done = true;
  accepted = false;
  remember();
  BufferedWindow::hide();
}

bool Dialog::choose_typed(const std::string &typed) {
  std::string path = chooser::expand_location(typed, real_folder(location) ? location : home_dir());
  if (path.empty()) return false;
  struct stat st {};
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    crumb_tail.clear();
    open_location(path);
    view->take_focus();
    return true;
  }
  std::string dir = chooser::parent_dir(path);
  if (mode == Mode::Save && dir_exists(dir)) {
    crumb_tail.clear();
    open_location(dir);
    name_input->value(basename_of(path).c_str());
    select_stem(name_input);
    name_input->take_focus();
    update_buttons();
    return true;
  }
  if (is_open(mode) && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
    // A typed file is chosen at once, as in GTK.
    finish({path});
    return true;
  }
  say(format(_("“%s” cannot be found"), typed.c_str()), true);
  return false;
}

void Dialog::remember() {
  double sc = ui_scale() > 0 ? ui_scale() : 1.0;
  if (shown()) {
    state.width = int(std::lround(w() / sc));
    state.height = int(std::lround(h() / sc));
  }
  if (!preview_forced || preview_toggled) state.show_preview = preview_on;
  std::string where;
  if (accepted && !result.empty()) {
    if (is_folder_mode(mode) && result[0] == location) where = location;
    else where = chooser::parent_dir(result[0]);
  }
  // Another dialog may have saved meanwhile: only this one's choices change.
  chooser::State saved = chooser::State::load();
  saved.width = state.width;
  saved.height = state.height;
  saved.list_view = state.list_view;
  saved.show_hidden = state.show_hidden;
  saved.show_preview = state.show_preview;
  saved.sort = state.sort;
  if (!where.empty()) saved.folders[o.program] = where;
  saved.save();
}

// ---------------------------------------------------------------- view options

void Dialog::set_list_view(bool list) {
  if (state.list_view == list) return;
  state.list_view = list;
  view->wraps.clear();
  view->update_bar();
  view->ensure_visible(index(focus_path));
  update_buttons();
  view->redraw();
  schedule_thumbnails();
}

void Dialog::set_hidden(bool on) {
  state.show_hidden = on;
  refilter();
  if (editing) completions_changed();
  update_buttons();
}

void Dialog::set_preview(bool on) {
  preview_on = on;
  preview_toggled = true;
  layout();
  update_buttons();
  redraw();
  schedule_thumbnails();
}

void Dialog::sort_by(SortKey key) {
  if (!sorted_by_user && location == "recent:") state.sort = order();
  if (state.sort.key == key) {
    state.sort.descending = !state.sort.descending;
  } else {
    state.sort.key = key;
    state.sort.descending = key == SortKey::Modified || key == SortKey::Size;
  }
  sorted_by_user = true;
  refilter();
}

void Dialog::options_menu(int x, int y) {
  ContextMenu menu;
  menu.radio(_("Grid"), !state.list_view, [this] { set_list_view(false); });
  menu.radio(_("List"), state.list_view, [this] { set_list_view(true); });
  menu.separator();
  ContextMenu sort;
  static const struct {
    SortKey key;
    const char *label;
  } keys[] = {{SortKey::Name, N_("Name")}, {SortKey::Size, N_("Size")}, {SortKey::Modified, N_("Modified")},
              {SortKey::Type, N_("Type")}};
  for (const auto &k : keys) {
    SortKey key = k.key;
    sort.radio(_(k.label), state.sort.key == key, [this, key] {
      if (state.sort.key != key) sort_by(key);
    });
  }
  sort.separator();
  sort.check(_("Descending"), state.sort.descending, [this] {
    state.sort.descending = !state.sort.descending;
    sorted_by_user = true;
    refilter();
  });
  sort.check(_("Folders First"), state.sort.folders_first, [this] {
    state.sort.folders_first = !state.sort.folders_first;
    refilter();
  });
  menu.submenu(_("Sort By"), std::move(sort));
  menu.check(_("Show Hidden Files"), state.show_hidden, [this] { set_hidden(!state.show_hidden); });
  menu.check(_("Show Preview"), preview_on, [this] { set_preview(!preview_on); });
  menu.separator();
  if (o.new_folder && real_folder(location))
    menu.item(_("New Folder…"), [this] { new_folder(); }, {"folder-new", FL_CTRL + FL_SHIFT + 'n'});
  menu.item(_("Reload"), [this] { reload(); }, {"refresh", FL_F + 5});
  if (real_folder(location)) {
    std::string loc = location;
    menu.item(_("Copy Location"), [loc] { Fl::copy(loc.c_str(), int(loc.size()), 1); }, {"copy", 0});
  }
  menu.popup(x, y);
}

void Dialog::item_menu(int k) {
  if (k < 0) {
    options_menu(Fl::event_x(), Fl::event_y());
    return;
  }
  const Entry &e = at(k);
  std::string path = e.path;
  ContextMenu menu;
  if (e.dir) {
    menu.item(_("Open"), [this, path] {
      int i = index(path);
      if (i >= 0) activate(i);
    }, {"folder-open", 0});
  } else if (selectable(e) && mode != Mode::Save) {
    menu.item(o.accept_label.empty() ? std::string(_("Choose")) : o.accept_label, [this, path] {
      int i = index(path);
      if (i >= 0) activate(i);
    }, {"object-select", 0});
  }
  if (!e.folder.empty()) {
    std::string folder = e.folder;
    menu.item(_("Open Containing Folder"), [this, folder, path] {
      crumb_tail.clear();
      open_location(folder);
      select_when_loaded = path;
    }, {"folder", 0});
  }
  menu.item(_("Copy Location"), [path] { Fl::copy(path.c_str(), int(path.size()), 1); }, {"copy", 0});
  menu.separator();
  menu.check(_("Show Hidden Files"), state.show_hidden, [this] { set_hidden(!state.show_hidden); });
  menu.check(_("Show Preview"), preview_on, [this] { set_preview(!preview_on); });
  menu.item(_("Reload"), [this] { reload(); }, {"refresh", FL_F + 5});
  menu.popup();
}

// ---------------------------------------------------------------- location entry, search

void Dialog::start_edit(const std::string &text) {
  if (search_open) {
    search_open = false;
    pathbar->search->hide();
    if (!query.empty()) {
      pathbar->search->value("");
      set_query("");
    }
  }
  editing = true;
  Field *e = pathbar->entry;
  e->show();
  e->value(text.c_str());
  if (text.size() > 1 && text == location) e->insert_position(e->size(), 0);
  else e->insert_position(e->size());
  e->take_focus();
  completions_changed();
  update_buttons();
  redraw();
}

void Dialog::stop_edit(bool refocus) {
  if (!editing) return;
  editing = false;
  pathbar->entry->hide();
  completion->set({});
  pathbar->rebuild();
  if (refocus) view->take_focus();
  redraw();
}

void Dialog::completions_changed() {
  if (!editing) {
    completion->set({});
    return;
  }
  std::string typed = pathbar->entry->value();
  std::vector<std::string> items;
  if (!typed.empty())
    items = chooser::complete_location(typed, real_folder(location) ? location : home_dir(), is_folder_mode(mode),
                                       state.show_hidden, 60);
  if (items.size() == 1 && items[0] == typed) items.clear();
  completion->set(items);
  Fl::remove_timeout(qa_dump_cb, this);
  Fl::add_timeout(0.05, qa_dump_cb, this);
}

void Dialog::open_search() {
  if (editing) stop_edit(false);
  search_open = true;
  Field *s = pathbar->search;
  s->show();
  s->value(query.c_str());
  s->insert_position(s->size(), 0);
  s->take_focus();
  update_buttons();
  redraw();
}

void Dialog::close_search() {
  search_open = false;
  pathbar->search->hide();
  if (!query.empty()) {
    pathbar->search->value("");
    set_query("");
  }
  view->take_focus();
  update_buttons();
  redraw();
}

// ---------------------------------------------------------------- new folder, filters, drops

void Dialog::new_folder() {
  if (!o.new_folder || !real_folder(location)) return;
  completion->set({});
  newfolder_box->open(newfolder_btn->x(), newfolder_btn->y() - S(8));
}

bool Dialog::create_folder(const std::string &name, std::string *err) {
  if (name.empty()) {
    *err = _("Type a name for the folder.");
    return false;
  }
  if (name.find('/') != std::string::npos) {
    *err = _("A folder name cannot contain “/”.");
    return false;
  }
  if (name == "." || name == "..") {
    *err = _("This name cannot be used.");
    return false;
  }
  std::string path = join_path_of(location, name);
  struct stat st {};
  if (lstat(path.c_str(), &st) == 0) {
    *err = _("There is already something with this name here.");
    return false;
  }
  if (mkdir(path.c_str(), 0777) != 0) {
    *err = strerror(errno);
    return false;
  }
  if (mode == Mode::Save) {
    open_location(path);
    if (name_input) name_input->take_focus();
  } else {
    reload();
    select_when_loaded = path;
    also_select.clear();
  }
  say(format(_("Folder “%s” created"), take_gstr(g_filename_display_name(name.c_str())).c_str()));
  return true;
}

void Dialog::filter_changed() {
  if (!filter_choice) return;
  filter = filter_choice->value();
  if (mode == Mode::Save && name_input && current_filter()) {
    std::string name = name_input->value();
    std::string fixed = chooser::name_for_filter(name, *current_filter(), filters);
    if (fixed != name) name_input->value(fixed.c_str());
    // Back to the name, ready to be typed over.
    select_stem(name_input);
    name_input->take_focus();
  }
  refilter();
}

void Dialog::drop(const std::string &text) {
  std::vector<std::string> paths;
  for (auto line : split(text, '\n')) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::string p = starts_with(line, "file://") ? chooser::expand_location(line, "") : line;
    if (!p.empty() && p[0] == '/') paths.push_back(chooser::normalize_path(p));
  }
  if (paths.empty()) return;
  std::string first = paths[0];
  struct stat st {};
  if (stat(first.c_str(), &st) != 0) return;
  std::string dir = chooser::parent_dir(first);
  crumb_tail.clear();
  if (S_ISDIR(st.st_mode)) {
    if (is_folder_mode(mode) && !dir.empty()) {
      open_location(dir);
      select_when_loaded = first;
      also_select.insert(paths.begin(), paths.end());
    } else {
      open_location(first);
    }
    return;
  }
  if (dir.empty()) return;
  if (mode == Mode::Save) {
    open_location(dir);
    name_input->value(basename_of(first).c_str());
    select_stem(name_input);
    name_input->take_focus();
    update_buttons();
    return;
  }
  if (!is_open(mode)) return;
  Entry e;
  if (const Filter *f = current_filter())
    if (chooser::entry_for_path(first, e) && !f->matches(e)) {
      say(format(_("“%s” is not one of the files that can be chosen here."), e.display.c_str()), true);
      return;
    }
  open_location(dir);
  select_when_loaded = first;
  also_select.insert(paths.begin(), paths.end());
}

// ---------------------------------------------------------------- thumbnails

Dialog::Thumb *Dialog::thumbnail(const Entry &e, int size) {
  auto it = thumbs.find(std::to_string(size) + "\n" + e.path);
  return it == thumbs.end() ? nullptr : &it->second;
}

Dialog::Thumb *Dialog::picture(const std::string &path, int size) {
  auto it = thumbs.find("p" + std::to_string(size) + "\n" + path);
  return it == thumbs.end() ? nullptr : &it->second;
}

int Dialog::preview_size() const {
  int px = std::max(preview->box_w(), preview->box_h());
  return px <= 256 ? 256 : px <= 512 ? 512 : 1024;
}

void Dialog::schedule_thumbnails() {
  Fl::remove_timeout(request_thumbnails_cb, this);
  Fl::add_timeout(0.02, request_thumbnails_cb, this);
}

void Dialog::request_thumbnails_cb(void *d) { static_cast<Dialog *>(d)->request_thumbnails(); }

void Dialog::request_thumbnails() {
  std::vector<chooser::Thumbnailer::Job> jobs;
  auto sel = selection();
  if (preview_shown && sel.size() == 1 && !sel[0]->dir) {
    if (chooser::can_thumbnail(sel[0]->mime) && !picture(sel[0]->path, preview_size())) {
      chooser::Thumbnailer::Job j;
      j.path = sel[0]->path;
      j.mtime = sel[0]->mtime;
      j.size = preview_size();
      j.picture = true;
      jobs.push_back(j);
    }
    if (!thumbnail(*sel[0], 256)) {
      chooser::Thumbnailer::Job j;
      j.path = sel[0]->path;
      j.mtime = sel[0]->mtime;
      j.size = 256;
      j.generate = chooser::can_thumbnail(sel[0]->mime) && sel[0]->size < (200LL << 20);
      jobs.push_back(j);
    }
  }
  view->metrics();
  int first, last;
  view->visible_range(first, last);
  int n = count(), size = view->thumb_size();
  auto want = [&](int k) {
    if (k < 0 || k >= n) return;
    const Entry &e = at(k);
    if (e.dir || e.broken || starts_with(e.mime, "inode/")) return;
    if (thumbs.count(std::to_string(size) + "\n" + e.path)) return;
    chooser::Thumbnailer::Job j;
    j.path = e.path;
    j.mtime = e.mtime;
    j.size = size;
    j.generate = chooser::can_thumbnail(e.mime) && e.size < (200LL << 20);
    jobs.push_back(std::move(j));
  };
  for (int k = first; k <= last; ++k) want(k);
  int span = std::max(1, last - first + 1);
  for (int d = 1; d <= span; ++d) {
    want(last + d);
    want(first - d);
  }
  thumbnailer.request(std::move(jobs));
  // Many thumbnails: the ones far from the view are forgotten.
  if (thumbs.size() > 1500) {
    std::set<std::string> keep;
    for (int k = std::max(0, first - 2 * span); k <= std::min(n - 1, last + 2 * span); ++k) keep.insert(at(k).path);
    for (const Entry *e : sel) keep.insert(e->path);
    for (auto it = thumbs.begin(); it != thumbs.end();) {
      std::string p = it->first.substr(it->first.find('\n') + 1);
      if (!keep.count(p)) {
        if (it->second.surface) cairo_surface_destroy(it->second.surface);
        it = thumbs.erase(it);
      } else {
        ++it;
      }
    }
  }
}

// ---------------------------------------------------------------- messages, keys

void Dialog::say(const std::string &text, bool is_error) {
  message = text;
  message_error = is_error;
  view->redraw();
  Fl::remove_timeout(clear_message, this);
  Fl::add_timeout(is_error ? 6.0 : 3.0, clear_message, this);
  Fl::remove_timeout(qa_dump_cb, this);
  Fl::add_timeout(0.05, qa_dump_cb, this);
}

void Dialog::clear_message(void *d) {
  auto *self = static_cast<Dialog *>(d);
  self->message.clear();
  self->view->redraw();
}

bool Dialog::shortcut(int key) {
  int st = Fl::event_state();
  bool ctrl = st & FL_CTRL, alt = st & FL_ALT, shift = st & FL_SHIFT;
  if (alt && !ctrl) {
    if (key == FL_Left) {
      go_back();
      return true;
    }
    if (key == FL_Right) {
      go_forward();
      return true;
    }
    if (key == FL_Up) {
      go_up();
      return true;
    }
    if (key == FL_Home) {
      open_location(home_dir());
      return true;
    }
  }
  if (ctrl && !alt) {
    switch (key) {
      case 'f':
        open_search();
        return true;
      case 'l':
        start_edit(real_folder(location) ? location : std::string());
        return true;
      case 'h':
        set_hidden(!state.show_hidden);
        return true;
      case 'r':
        reload();
        return true;
      case '1':
        set_list_view(false);
        return true;
      case '2':
        set_list_view(true);
        return true;
      case 'n':
        if (shift && o.new_folder) {
          new_folder();
          return true;
        }
        break;
    }
  }
  if (key == FL_F + 5) {
    reload();
    return true;
  }
  return false;
}

bool Dialog::escape() {
  if (completion->visible()) {
    completion->set({});
    return true;
  }
  if (newfolder_box->visible()) {
    newfolder_box->close();
    return true;
  }
  if (editing) {
    stop_edit();
    return true;
  }
  if (search_open) {
    if (!query.empty()) {
      pathbar->search->value("");
      set_query("");
    } else {
      close_search();
    }
    return true;
  }
  cancel();
  return true;
}

int Dialog::handle(int e) {
  switch (e) {
    case FL_PUSH: {
      int b = Fl::event_button();
      if (b == FL_BACK_MOUSE) {
        go_back();
        return 1;
      }
      if (b == FL_FORWARD_MOUSE) {
        go_forward();
        return 1;
      }
      if (completion->visible() && !Fl::event_inside(completion) && !Fl::event_inside(pathbar)) completion->set({});
      if (newfolder_box->visible() && !Fl::event_inside(newfolder_box) && !Fl::event_inside(newfolder_btn))
        newfolder_box->close();
      break;
    }
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      if (shortcut(key)) return 1;
      if (key == FL_Escape) {
        escape();
        return 1;
      }
      if (key == FL_Enter || key == FL_KP_Enter) {
        accept();
        return 1;
      }
      if (key == FL_BackSpace && !dynamic_cast<Fl_Input *>(Fl::focus())) {
        go_up();
        return 1;
      }
      break;
    }
    case FL_SHORTCUT:
      if (Fl::event_key() == FL_Escape) {
        escape();
        return 1;
      }
      break;
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      BufferedWindow::handle(e);
      return 1;
    case FL_PASTE:
      if (BufferedWindow::handle(e)) return 1;
      if (Fl::event_text()) drop(Fl::event_text());
      return 1;
  }
  return BufferedWindow::handle(e);
}

// ---------------------------------------------------------------- layout and drawing

void Dialog::layout() {
  const Theme &t = active_theme();
  int W = w(), H = h();
  int pad = S(10), bh = S(34), hh = S(56);
  bool two_rows = mode == Mode::Save;
  int fh = two_rows ? S(104) : S(60);
  int by = (hh - bh) / 2;
  back_btn->resize(pad, by, bh, bh);
  forward_btn->resize(pad + bh + S(2), by, bh, bh);
  up_btn->resize(pad + 2 * (bh + S(2)), by, bh, bh);
  int right = W - pad;
  menu_btn->resize(right - bh, by, bh, bh);
  right -= bh + S(6);
  pane_btn->resize(right - bh, by, bh, bh);
  right -= bh + S(8);
  seg_x = right - 2 * bh;
  list_btn->resize(right - bh, by, bh, bh);
  grid_btn->resize(right - 2 * bh, by, bh, bh);
  right -= 2 * bh + S(8);
  search_btn->resize(right - bh, by, bh, bh);
  right -= bh + S(8);
  int px = pad + 3 * (bh + S(2)) + S(8);
  pathbar->resize(px, by, std::max(S(80), right - px), bh);

  int top = hh, bottom = H - fh;
  int sw = W >= S(900) ? S(204) : W >= S(660) ? S(176) : W >= S(520) ? S(150) : 0;
  if (sw) {
    placesv->show();
    placesv->resize(0, top, sw, bottom - top);
  } else {
    placesv->hide();
  }
  card_x = sw ? sw + S(2) : pad;
  card_y = top;
  card_w = W - card_x - pad;
  card_h = bottom - top;
  int pw = std::max(S(190), std::min(S(270), card_w * 30 / 100));
  int min_files = preview_forced && preview_on ? S(220) : S(300);
  preview_shown = preview_on && card_w - pw >= min_files;
  if (preview_shown) {
    preview->show();
    preview->resize(card_x + card_w - pw - 1, card_y + 1, pw, card_h - 2);
    view->resize(card_x + 1, card_y + 1, card_w - pw - 2, card_h - 2);
  } else {
    preview->hide();
    view->resize(card_x + 1, card_y + 1, card_w - 2, card_h - 2);
  }
  footer_y = bottom;
  // The footer: the name and the filter (saving), then the buttons.
  int row = bottom + S(12);
  int fw = filter_choice ? std::min(S(220), std::max(S(130), W / 4)) : 0;
  text_font(t.font_size);
  int bw = std::max(S(96), text_width(_("Cancel")) + S(40));
  text_font(t.font_size, true);
  int aw = std::max(S(110), text_width(accept_btn->label() ? accept_btn->label() : "") + S(44));
  if (two_rows) {
    int lx = pad + S(52);
    name_input->resize(lx, row, W - lx - pad - (fw ? fw + S(10) : 0), bh);
    if (filter_choice) filter_choice->resize(W - pad - fw, row, fw, bh);
    row += bh + S(10);
  }
  accept_btn->resize(W - pad - aw, row, aw, bh);
  cancel_btn->resize(W - pad - aw - S(8) - bw, row, bw, bh);
  int lx = pad;
  text_font(t.font_size);
  int nfw = std::max(S(110), text_width(_("New Folder")) + S(36));
  if (o.new_folder) {
    newfolder_btn->resize(lx, row, nfw, bh);
    lx += nfw + S(14);
  }
  int rx = cancel_btn->x() - S(12);
  if (filter_choice && !two_rows) {
    filter_choice->resize(rx - fw, row, fw, bh);
    rx -= fw + S(12);
  }
  int cw = text_width(_("Show hidden files")) + S(34);
  if (rx - lx >= cw) {
    hidden_check->show();
    hidden_check->resize(lx, row + (bh - S(28)) / 2, cw, S(28));
  } else {
    hidden_check->hide();
  }
  if (newfolder_box->visible()) newfolder_box->place(newfolder_btn->x(), newfolder_btn->y() - S(8));
  if (completion->visible()) completion->set(completion->items);
  view->update_bar();
}

void Dialog::resize(int X, int Y, int W, int H) {
  bool sized = W != w() || H != h();
  BufferedWindow::resize(X, Y, W, H);
  if (sized) {
    layout();
    schedule_thumbnails();
  }
}

void Dialog::hide() {
  // Closed by the window manager: a cancel.
  if (!done) {
    done = true;
    remember();
  }
  BufferedWindow::hide();
}

void Dialog::draw() {
  const Theme &t = active_theme();
  if (first_draw_ms < 0) {
    first_draw_ms = now_ms() - opened_at;
    if (getenv("SDE_FILE_CHOOSER_TIMING")) fprintf(stderr, "sde file chooser: first frame after %.1f ms\n", first_draw_ms);
  }
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.background);
    // The track of the view switch.
    double sh = grid_btn->h();
    p.fill_rounded(seg_x, grid_btn->y(), 2 * grid_btn->w(), sh, t.radius_for(sh),
                   t.dark ? t.surface_alt : t.surface_alt.mix(t.text, 0.03));
    p.stroke_rounded(seg_x, grid_btn->y(), 2 * grid_btn->w(), sh, t.radius_for(sh), t.border);
  }
  if (name_input) {
    text_font(t.font_size);
    draw_text(_("Name"), S(10), name_input->y(), S(48), name_input->h(), t.text);
  }
  for (int i = 0; i < children(); ++i) {
    Fl_Widget *c = child(i);
    if (c == completion || c == newfolder_box) continue;
    draw_child(*c);
  }
  {
    // The card's rounded corners and edge, over the files and the preview.
    Painter p(w(), h());
    cairo_t *cr = p.cr();
    cairo_new_path(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, card_x, card_y, card_w, card_h);
    p.rounded_rect(card_x, card_y, card_w, card_h, t.radius);
    p.set(t.background);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    p.stroke_rounded(card_x, card_y, card_w, card_h, t.radius, t.border);
  }
  draw_child(*completion);
  draw_child(*newfolder_box);
  if (getenv("SDE_PANEL_QA_DIR")) {
    Fl::remove_timeout(qa_dump_cb, this);
    Fl::add_timeout(0.05, qa_dump_cb, this);
  }
}

// ---------------------------------------------------------------- QA

void Dialog::qa_dump_cb(void *d) { static_cast<Dialog *>(d)->qa_dump(); }

// UI test hook, as the Control Center's: with SDE_PANEL_QA_DIR set,
// <dir>/File-Chooser.txt describes the dialog ("@mode:", "@location:",
// "@selected:" ... lines) and its named controls, one
// "name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>active<TAB>0" line each, in window pixels.
void Dialog::qa_dump() {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  if (!dir || !*dir || !shown()) return;
  static const char *modes[] = {"open", "multi", "save", "folder", "folders"};
  std::string out;
  auto state_line = [&](const std::string &k, const std::string &v) {
    std::string value = v;
    for (char &c : value)
      if (c == '\t' || c == '\n') c = ' ';
    out += "@" + k + ":" + value + "\t0\t0\t0\t0\t1\t0\n";
  };
  auto control = [&](const std::string &name, int x, int y, int w, int h, bool active) {
    std::string n = name;
    for (char &c : n)
      if (c == '\t' || c == '\n') c = ' ';
    out += n + "\t" + std::to_string(x) + "\t" + std::to_string(y) + "\t" + std::to_string(w) + "\t" +
           std::to_string(h) + "\t" + (active ? "1" : "0") + "\t0\n";
  };
  auto widget = [&](const std::string &name, Fl_Widget *wd) {
    if (wd && wd->visible_r()) control(name, wd->x(), wd->y(), wd->w(), wd->h(), wd->active_r());
  };
  state_line("mode", modes[int(mode)]);
  state_line("title", label() ? label() : "");
  state_line("location", location);
  state_line("view", state.list_view ? "list" : "grid");
  state_line("preview", preview_shown ? "1" : "0");
  state_line("hidden", state.show_hidden ? "1" : "0");
  state_line("loading", loading ? "1" : "0");
  state_line("items", std::to_string(count()));
  state_line("total", std::to_string(entries.size()));
  std::string sel;
  for (const Entry *e : selection()) sel += (sel.empty() ? "" : "|") + e->name;
  state_line("selected", sel);
  state_line("focus", focus_path);
  state_line("filter", std::to_string(filter) + ":" + (current_filter() ? current_filter()->name : ""));
  state_line("name", name_input ? name_input->value() : "");
  if (name_input) {
    int a = name_input->insert_position(), b = name_input->mark();
    state_line("name-selection", std::to_string(std::min(a, b)) + "-" + std::to_string(std::max(a, b)));
  }
  state_line("query", query);
  state_line("searching", searching_deep ? "1" : "0");
  state_line("editing", editing ? "1" : "0");
  state_line("entry", editing ? pathbar->entry->value() : "");
  state_line("completions", std::to_string(completion->visible() ? completion->items.size() : 0));
  state_line("sort", chooser::sort_key_id(state.sort.key) + (state.sort.descending ? ":desc" : ":asc"));
  int done_thumbs = 0;
  for (const auto &kv : thumbs)
    if (kv.second.surface) ++done_thumbs;
  state_line("thumbnails", std::to_string(done_thumbs));
  state_line("thumbnailing", thumbnailer.busy() ? "1" : "0");
  state_line("accept", accept_btn->active() ? "1" : "0");
  state_line("message", message);
  state_line("opened-ms", std::to_string(int(std::lround(first_draw_ms))));
  state_line("loaded-ms", std::to_string(int(std::lround(loaded_ms))));
  state_line("newfolder-box", newfolder_box->visible() ? "1" : "0");
  state_line("focus-widget", Fl::focus() == view ? "files" : Fl::focus() == name_input && name_input ? "name"
                             : Fl::focus() == pathbar->entry ? "location" : Fl::focus() == pathbar->search ? "search"
                             : Fl::focus() == placesv ? "places" : "other");
  widget("back", back_btn);
  widget("forward", forward_btn);
  widget("up", up_btn);
  widget("pathbar", pathbar);
  if (!editing && !search_open)
    for (size_t i = 0; i < pathbar->crumbs.size(); ++i) {
      const auto &c = pathbar->crumbs[i];
      control("crumb:" + std::to_string(i) + ":" + c.label, c.x, pathbar->y(), c.w, pathbar->h(), true);
    }
  control("edit", pathbar->button_x(), pathbar->y(), pathbar->h(), pathbar->h(), true);
  widget("location", editing ? pathbar->entry : nullptr);
  widget("search-entry", search_open ? pathbar->search : nullptr);
  widget("search", search_btn);
  widget("view:grid", grid_btn);
  widget("view:list", list_btn);
  widget("preview", pane_btn);
  widget("menu", menu_btn);
  if (placesv->visible())
    for (const auto &r : placesv->rows)
      if (r.place >= 0) {
        int ry = placesv->y() + r.y - placesv->scroll;
        bool in = ry >= placesv->y() && ry + r.h <= placesv->y() + placesv->h();
        control("place:" + places[size_t(r.place)].id, placesv->x() + S(8), ry, placesv->w() - S(14), r.h, in);
      }
  widget("files", view);
  if (preview_shown) widget("preview-pane", preview);
  int first, last;
  view->metrics();
  view->visible_range(first, last);
  for (int k = first; k <= last && k < count(); ++k) {
    int X, Y, W, H;
    view->item_rect(k, X, Y, W, H);
    bool in = Y >= view->items_y() && Y + H <= view->items_y() + view->items_h();
    control("item:" + at(k).name, X, Y, W, H, in && selectable(at(k)));
  }
  if (state.list_view)
    for (const auto &c : view->columns())
      control("column:" + chooser::sort_key_id(c.key), c.x, view->y(), c.w, view->head_h, true);
  if (completion->visible())
    for (int i = 0; i < completion->rows(); ++i)
      control("completion:" + completion->items[size_t(completion->first + i)], completion->x(),
              completion->y() + S(4) + i * completion->row_h(), completion->w(), completion->row_h(), true);
  widget("name", name_input);
  widget("filter", filter_choice);
  widget("hidden", hidden_check);
  widget("new-folder", newfolder_btn);
  widget("cancel", cancel_btn);
  widget("accept", accept_btn);
  if (newfolder_box->visible()) {
    widget("folder-name", newfolder_box->input);
    widget("create", newfolder_box->create);
  }
  std::string path = std::string(dir) + "/File-Chooser.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

// ---------------------------------------------------------------- showing

void Dialog::place_window() {
  // The window the dialog belongs to.
  Fl_Window *pw = o.parent && o.parent->shown() ? o.parent->top_window() : nullptr;
  if (!pw && Fl::focus() && Fl::focus()->window()) pw = Fl::focus()->top_window();
  if (!pw)
    for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w))
      if (w != this && w->shown() && w->visible() && !w->menu_window() && !w->tooltip_window() && !w->parent()) {
        pw = w;
        break;
      }
  int px = 0, py = 0, pww = 0, phh = 0;
  g_transient_for = 0;
  if (o.parent_xid && fl_display) {
    XWindowAttributes attr;
    Window child;
    if (XGetWindowAttributes(fl_display, Window(o.parent_xid), &attr)) {
      XTranslateCoordinates(fl_display, Window(o.parent_xid), RootWindow(fl_display, fl_screen), 0, 0, &px, &py,
                            &child);
      pww = attr.width;
      phh = attr.height;
      g_transient_for = Window(o.parent_xid);
    }
  } else if (pw && pw->shown()) {
    px = pw->x();
    py = pw->y();
    pww = pw->w();
    phh = pw->h();
    g_transient_for = fl_xid(pw);
  }
  int cx, cy;
  int sx, sy, sw, sh;
  if (pww > 0) {
    cx = px + pww / 2;
    cy = py + phh / 2;
    Fl::screen_work_area(sx, sy, sw, sh, cx, cy);
  } else {
    Fl::get_mouse(cx, cy);
    Fl::screen_work_area(sx, sy, sw, sh, cx, cy);
    cx = sx + sw / 2;
    cy = sy + sh / 2;
  }
  double sc = ui_scale() > 0 ? ui_scale() : 1.0;
  int W = state.width > 0 ? int(state.width * sc) : S(880);
  int H = state.height > 0 ? int(state.height * sc) : S(600);
  int frame = S(34);  // the title bar
  W = std::max(std::min(W, sw - S(16)), std::min(S(460), sw));
  H = std::max(std::min(H, sh - frame - S(16)), std::min(S(340), sh - frame));
  int X = cx - W / 2, Y = cy - H / 2;
  X = std::max(sx + S(8), std::min(X, sx + sw - W - S(8)));
  Y = std::max(sy + frame, std::min(Y, sy + sh - H - S(8)));
  resize(X, Y, W, H);
  size_range(std::min(S(460), W), std::min(S(340), H));
}

bool Dialog::run(std::vector<std::string> &out) {
  opened_at = now_ms();
  std::string home = home_dir();
  std::string start, name = o.file_name;
  if (name.find('/') != std::string::npos) {
    std::string full = chooser::expand_location(name, home);
    if (o.folder.empty()) o.folder = chooser::parent_dir(full);
    name = basename_of(full);
  }
  if (!o.folder.empty()) {
    start = o.folder;
  } else {
    auto it = state.folders.find(o.program);
    if (it != state.folders.end() && dir_exists(it->second)) start = it->second;
    else if (!o.default_folder.empty()) start = o.default_folder;
  }
  if (start.empty()) start = home;
  start = chooser::expand_location(start, home);
  while (!start.empty() && !dir_exists(start)) start = chooser::parent_dir(start);
  if (start.empty()) start = home;
  place_window();
  open_location(start);
  if (mode == Mode::Save) {
    name_input->value(name.c_str());
    select_stem(name_input);
  } else if (!name.empty()) {
    select_when_loaded = join_path_of(start, name);
  }
  set_modal();
  g_prev_before_map = Fl_X::before_map;
  Fl_X::before_map = dialog_before_map;
  g_mapping = this;
  show();
  Fl_X::before_map = g_prev_before_map;
  g_mapping = nullptr;
  if (mode == Mode::Save) name_input->take_focus();
  else view->take_focus();
  update_buttons();
  Fl::add_timeout(2.0, poll_places, this);
  while (!done && shown()) Fl::wait();
  if (shown()) BufferedWindow::hide();
  Fl::remove_timeout(poll_places, this);
  out = result;
  return accepted && !result.empty();
}

}  // namespace

// ================================================================== FileChooser

FileChooser::FileChooser(Mode mode) : o_(new Options) { o_->mode = mode; }
FileChooser::~FileChooser() = default;
FileChooser::Mode FileChooser::mode() const { return o_->mode; }
void FileChooser::title(const std::string &title) { o_->title = title; }
void FileChooser::folder(const std::string &dir) { o_->folder = dir; }
void FileChooser::default_folder(const std::string &dir) { o_->default_folder = dir; }
void FileChooser::file_name(const std::string &name) { o_->file_name = name; }
void FileChooser::filters(const std::string &spec) {
  for (auto &f : chooser::parse_filters(spec)) o_->filters.push_back(std::move(f));
}
void FileChooser::add_filter(const std::string &name, const std::string &patterns,
                             const std::vector<std::string> &mime_types) {
  Filter f;
  if (!patterns.empty()) {
    auto parsed = chooser::parse_filters(patterns);
    if (!parsed.empty()) f.patterns = parsed[0].patterns;
  }
  f.name = name;
  f.mime_types = mime_types;
  o_->filters.push_back(std::move(f));
}
void FileChooser::all_files(bool on) { o_->all_files = on ? 1 : 0; }
void FileChooser::filter_index(int index) { o_->filter = index; }
int FileChooser::filter_index() const { return o_->filter; }
void FileChooser::preview(bool show) { o_->preview = show ? 1 : 0; }
void FileChooser::accept_label(const std::string &label) { o_->accept_label = label; }
void FileChooser::confirm_overwrite(bool on) { o_->confirm_overwrite = on; }
void FileChooser::new_folder(bool on) { o_->new_folder = on; }
void FileChooser::program(const std::string &id) { o_->program = id; }
void FileChooser::parent(Fl_Window *win) { o_->parent = win; }
void FileChooser::parent_xid(unsigned long xid) { o_->parent_xid = xid; }
void FileChooser::add_to_recent(bool on) { o_->recent = on; }
const std::vector<std::string> &FileChooser::paths() const { return paths_; }
std::string FileChooser::path() const { return paths_.empty() ? std::string() : paths_[0]; }

bool FileChooser::run() {
  paths_.clear();
  if (o_->program.empty()) o_->program = chooser::program_id();
  Fl_Group *current = Fl_Group::current();
  Fl_Group::current(nullptr);
  auto *dialog = new Dialog(*o_);
  Fl_Group::current(current);
  o_->dialog = dialog;
  bool ok = dialog->run(paths_);
  o_->dialog = nullptr;
  o_->filter = dialog->filter;
  delete dialog;
  return ok;
}

void FileChooser::close() {
  if (o_->dialog) static_cast<Dialog *>(o_->dialog)->cancel();
}

bool FileChooser::running() const { return o_->dialog != nullptr; }

}  // namespace sde
