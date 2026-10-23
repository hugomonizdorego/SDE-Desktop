// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Taca, the SDE archive manager: user interface declarations.
//
// The main window follows the classic archiver layout: a menu bar, a
// toolbar of large labelled buttons, an address bar with an Up button, a
// details list that browses the file system as well as the inside of
// archives, and a status bar with totals.
#pragma once

#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Widget.H>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "about.h"
#include "buffered_window.h"
#include "engine.h"
#include "model.h"
#include "paint.h"
#include "theme.h"
#include "tools.h"
#include "widgets.h"

namespace taca {

// --------------------------------------------------------------- icons
namespace icon {
// Small line icons (dialogs, menus, status bar).
enum Kind {
  Box, Open, New, Extract, Add, AddFolder, Trash, Rename, Shield, Info, Search, List, Grid, Home,
  Chevron, Lock, Close, Folder, Check, Warning, Up, Plus, Drive, Star, Down
};
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);

// The big colourful toolbar pictures, drawn on a 32-unit grid.
enum Tool { TAdd, TExtract, TTest, TView, TDelete, TFind, TWizard, TInfo, TRepair, ToolCount };
void tool(sde::Painter &p, Tool kind, double x, double y, double size, bool dark, bool enabled);

// A file card with a tinted corner and an extension label (label drawn by
// the caller with FLTK after the painter is gone, see file_label_box()).
void file(sde::Painter &p, const std::string &name, double x, double y, double size, bool dir,
          const sde::Color &accent, bool dark);
void file_label_box(const std::string &name, double x, double y, double size, int &lx, int &ly, int &lw,
                    int &lh);
// An archive file: a colourful parcel with a zip band.
void archive(sde::Painter &p, double x, double y, double size, const sde::Color &accent, bool dark);
// The ".." entry: a folder with an up arrow.
void parent_folder(sde::Painter &p, double x, double y, double size, const sde::Color &accent, bool dark);
sde::Color category_color(int category, const sde::Color &accent);
// The format "badge": a rounded square in the accent gradient.
void badge(sde::Painter &p, double x, double y, double size, const sde::Color &accent);
}  // namespace icon

// Applies the theme to a text field (drawn inside a rounded box by its
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

// ------------------------------------------------------------- menu bar

// Fl_Menu_Bar drawn in the SDE theme (its pull-down menus are FLTK's and
// follow the colours applied by Theme::apply_to_fltk()).
class MenuBar : public Fl_Menu_Bar {
 public:
  MenuBar(int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void apply_theme();

 private:
  int title_at(int mx) const;
  int hover_ = -1;
};

// -------------------------------------------------------------- toolbar

class Toolbar : public Fl_Widget {
 public:
  Toolbar(TacaWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  // Height wanted for the window height |wh| (compact on short windows).
  static int wanted_height(int wh);
  std::function<void(int)> on_click;  // icon::Tool index
  std::function<bool(int)> is_enabled;

 private:
  struct Button {
    int tool, x, w;
  };
  std::vector<Button> layout() const;
  bool compact() const;
  int at(int mx, int my) const;
  TacaWindow *app_;
  int hover_ = -1, down_ = -1;
};

// ------------------------------------------------------------ file view

// Item value of the ".." row.
constexpr int kParentItem = -1;

class FileView : public Fl_Widget {
 public:
  FileView(TacaWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;

  // |parent_row| adds ".." at the top.
  void set_items(std::vector<int> items, bool parent_row, bool keep_selection);
  const std::vector<int> &items() const { return items_; }
  std::vector<int> selection() const;  // never contains kParentItem
  void select_all();
  void invert_selection();
  void clear_selection();
  void select_node(int node);  // selects and shows one item
  bool grid() const { return grid_; }
  void set_grid(bool g);
  int focus_item() const { return focus_ >= 0 && focus_ < int(items_.size()) ? items_[size_t(focus_)] : -2; }

 private:
  struct Column {
    std::string label;
    int x, w, align;
    SortKey key;
  };
  std::vector<Column> columns_layout() const;
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
  void draw_icon(sde::Painter &p, int item, double x, double y, double size);

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

// Shrinks the user interface scale while a dialog that would not fit on
// the screen is open (a 600 pixel tall screen at 150 %), so every dialog
// stays reachable. Construct it before the Dialog; it restores on exit.
class FitScale {
 public:
  explicit FitScale(int logical_h, int logical_w = 0);
  ~FitScale();

 private:
  double old_ = 0;
  sde::Theme saved_;
  bool active_ = false;
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
  std::string folder;  // default location ("" = next to the first file)
};
bool ask_create(Fl_Window *parent, const std::vector<std::string> &files, CreateChoice &c);
void show_properties(Fl_Window *parent, const ArchiveInfo &info, const std::string &password_note);

struct FindChoice {
  std::string pattern = "*";
  bool recursive = true;
  bool inside_archives = false;
  bool in_archive = false;  // searching the open archive (no file system options)
};
bool ask_find(Fl_Window *parent, const std::string &where, FindChoice &c);

// Repair: shows what will be done and asks where to save the copy.
bool ask_repair(Fl_Window *parent, const std::string &archive, const RepairPlan &plan, std::string &dest_dir);

// The wizard's first page. Returns 0 cancelled, 1 create, 2 extract, 3 convert.
int ask_wizard_action(Fl_Window *parent);
// Converting: the archive's new format and name.
bool ask_convert(Fl_Window *parent, const std::string &archive, std::string &dest, CreateOptions &o);

struct Settings {
  Format format = Format::Zip;
  int level = 6;
  bool icons = false;    // icon grid instead of details
  bool hidden = false;   // show hidden files
  std::string theme = "follow";  // follow, light or dark
  bool open_after = true;        // show extracted files
  static Settings load();
  void save() const;
};
bool ask_settings(Fl_Window *parent, Settings &s);

// Favorites: lets the user remove and reorder entries. Returns true if changed.
bool organize_favorites(Fl_Window *parent, std::vector<std::string> &list);

// A read-only text window (preview of a text file inside an archive).
void show_text(Fl_Window *parent, const std::string &title, const std::string &text);
void show_shortcuts(Fl_Window *parent);
void show_about(Fl_Window *parent);
// Taca's entry for the common SDE About window and --version.
sde::AppInfo about_info();

// Directory and file choosers.
std::string choose_folder(const std::string &title, const std::string &start);
std::vector<std::string> choose_files(const std::string &title, const std::string &start, bool folders,
                                      bool archives);

// Moves files to the freedesktop trash. Returns the paths that could not
// be moved (another file system), with |error| set for real failures.
std::vector<std::string> move_to_trash(const std::vector<std::string> &paths, std::string &error);

// ------------------------------------------------------------ main window

enum Command {
  CmdOpen = 1, CmdBrowse, CmdNew, CmdClose, CmdSelectAll, CmdInvert, CmdDeselect, CmdCopyPath, CmdShowFolder,
  CmdQuit, CmdAdd, CmdExtract, CmdExtractHere, CmdTest, CmdView, CmdRename, CmdDelete, CmdRepair, CmdInfo,
  CmdUp, CmdWizard, CmdFind, CmdConvert, CmdFavAdd, CmdFavOrganize, CmdSettings, CmdHidden, CmdViewDetails,
  CmdViewIcons, CmdThemeFollow, CmdThemeLight, CmdThemeDark, CmdShortcuts, CmdAbout, CmdRefresh,
  CmdFavorite = 1000  // + index
};

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
  bool results() const { return results_; }
  bool busy() const { return job_ != nullptr; }
  SortKey sort_key() const { return sort_; }
  bool sort_descending() const { return descending_; }
  void set_sort(SortKey key);
  // A flat list of matches (find results) with their folder shown.
  bool flat_view() const { return results_; }
  // The type column text for a node.
  std::string type_of(int node) const;
  bool node_is_archive(int node) const;
  // Full path on disk of a file system node ("" inside an archive).
  std::string disk_path(int node) const;
  // The folder shown in the address bar for a find hit.
  std::string hit_folder(int node) const;

  void browse(const std::string &dir, const std::string &select = "");
  void open_archive(const std::string &path, bool keep_password = false, const std::string &inner = "",
                    const std::string &select = "");
  void close_archive();
  void enter(int node);  // folder: browse into it; archive: open; file: open it
  void go_up();
  void refresh_view(bool keep_selection = false);
  void selection_changed();
  void context_menu(int node);
  void start_drag();
  void command(int id);
  bool command_enabled(int id) const;

  void toast(const std::string &message);
  void apply_theme();

  static std::vector<std::string> recent();
  static void remember(const std::string &path);
  void action_new(const std::vector<std::string> &files = {});

 private:
  void layout();
  void build_menu();
  void run_job(const std::string &title, Job::Work work, Job::Done done);
  // Retries |again| with a password when |r| asks for one; true if handled.
  bool password_retry(const Result &r, std::function<void()> again, const std::string &name = "");
  void show_result(const std::string &what, const Result &r);
  void reload();
  void list_folder(bool keep_selection);
  std::vector<std::string> selected_paths() const;      // member paths (archive mode)
  std::vector<std::string> selected_disk_paths() const;  // full paths (file system mode)
  std::vector<std::string> selected_archives() const;
  std::string current_folder() const;
  std::string location() const;
  void navigate_to(const std::string &typed);
  void address_menu();
  void open_hit(int node);

  // Actions.
  void action_open();
  void action_browse();
  void action_add();
  void add_paths(const std::vector<std::string> &paths);
  void action_extract(bool here);
  void run_extract(const std::string &path, ExtractOptions o, bool here, bool open_after, std::function<void()> next);
  void action_delete();
  void action_rename();
  void action_test();
  void test_archives(const std::vector<std::string> &archives, size_t index, std::vector<std::string> damaged);
  void action_view();
  void action_find();
  void action_wizard();
  void action_convert(const std::string &archive);
  void action_info();
  void disk_info(const std::vector<std::string> &paths);
  void action_repair();
  void action_settings();
  void open_member(int node, bool preview);
  void preview_file(const std::string &path, const std::string &title);

  void draw_address(sde::Painter &p);
  void draw_address_text();
  void draw_status(sde::Painter &p);
  void draw_status_text();
  void draw_progress(sde::Painter &p);
  void draw_progress_text();
  static void tick_cb(void *p);
  static void toast_cb(void *p);
  static void menu_cb(Fl_Widget *w, void *data);
  static int x_event_handler(int event);
  static int system_handler(void *event, void *data);

  sde::Theme theme_;
  Settings settings_;
  ArchiveInfo info_;
  Tree tree_;
  int folder_ = 0;
  std::string fs_dir_;
  std::string password_;
  SortKey sort_ = SortKey::Name;
  bool descending_ = false;
  std::unique_ptr<Job> job_;
  std::string toast_;
  std::vector<std::string> favorites_;
  // Find results.
  bool results_ = false;
  std::vector<FindHit> hits_;
  std::unordered_map<int, size_t> hit_of_node_;
  std::string results_title_;
  std::string back_archive_, back_inner_, back_dir_;

  MenuBar *menubar_ = nullptr;
  Toolbar *toolbar_ = nullptr;
  Fl_Input *address_ = nullptr;
  FileView *view_ = nullptr;
  sde::FlatButton *cancel_ = nullptr;
  std::vector<Fl_Menu_Item> menu_;
  std::deque<std::string> menu_labels_;
  int address_y_ = 0, address_h_ = 0, status_h_ = 0;
  int hover_part_ = 0;  // 1 up button, 2 dropdown
  bool drop_hover_ = false;
  bool dragging_out_ = false;
  std::string drag_uris_;
  static TacaWindow *instance_;
};

// The small window used by "taca --extract-here" and friends.
int run_cli_job(const std::string &title, Job::Work work, bool quiet_success,
                std::function<void(const Result &)> done = nullptr);

// SIGTERM, SIGINT and SIGHUP cancel running work cleanly before exiting.
void install_signal_handlers();

std::string uri_to_path(const std::string &uri);
std::string path_to_uri(const std::string &path);
std::vector<std::string> parse_uri_list(const std::string &text);
std::string favorites_file();

}  // namespace taca
