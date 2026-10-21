// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny Text Editor: user interface declarations.
#pragma once

#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Widget.H>
#include <sys/stat.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "codec.h"
#include "paint.h"
#include "search.h"
#include "settings.h"
#include "syntax.h"
#include "theme.h"
#include "undo.h"
#include "widgets.h"

namespace sde {
class Config;
}

namespace niny {

class App;

// ------------------------------------------------------------------ look

// Colors for the editor chrome and the syntax palette, derived from the SDE
// theme (light/dark), its style (SDE flat, Aero glass, Liquid Glass) and the
// accent color.
struct Look {
  sde::Theme theme;
  std::string style = "aero";  // sde, aero, liquid-glass
  bool dark = false, glass = false, liquid = false;
  sde::Color accent, on_accent, accent_text;
  sde::Color window, header_top, header_bottom, header_line;
  sde::Color tab_active, tab_hover, tab_text, tab_text_dim;
  sde::Color editor, gutter, gutter_text, gutter_current, current_line, selection, margin;
  sde::Color match, match_current, bracket;
  sde::Color text, dim, border, hover;
  sde::Color sidebar, sidebar_active, status, card, card_border, field, shadow;
  sde::Color syntax[kStyleCount];
  bool bold[kStyleCount] = {};
  bool italic[kStyleCount] = {};
  double radius = 8;  // chrome corner radius (unscaled)
};
Look make_look(const sde::Config &cfg, const std::string &scheme);
const Look &look();
void set_look(const Look &l);
// Language badge: short label and color for tabs and lists.
const char *language_badge(const Language *lang);
sde::Color language_color(const Language *lang);
void draw_badge(sde::Painter &p, const Language *lang, double x, double y, double size);
void draw_badge_text(const Language *lang, double x, double y, double size);
void style_input(Fl_Input *in, bool large = false);

// ----------------------------------------------------------------- icons

namespace icon {
enum Kind {
  Menu, Sidebar, Plus, Close, Search, Command, ChevronUp, ChevronDown, ChevronLeft, ChevronRight,
  Folder, File, Up, Gear, Save, Warning, Info, Replace, ReplaceAll, Refresh, Wrap, Dot, Open, Recent,
  Documents, Undo, Redo
};
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);
void logo(sde::Painter &p, double x, double y, double s);
}  // namespace icon

// -------------------------------------------------------------- document

class Document {
 public:
  explicit Document(App *app);
  ~Document();
  Document(const Document &) = delete;
  Document &operator=(const Document &) = delete;

  App *app;
  Fl_Text_Buffer *buf = nullptr, *style = nullptr;
  std::string path;  // empty for untitled documents
  int untitled = 0;
  Encoding encoding = Encoding::Utf8;
  Eol eol = Eol::LF;
  bool mixed_eol = false;
  const Language *lang = nullptr;
  bool lang_manual = false;
  bool use_tabs = false;
  int indent_width = 4;
  bool indent_manual = false;
  UndoStack undo;
  bool format_dirty = false;  // encoding or line endings changed

  // Disk state at load/save.
  bool on_disk = false;
  struct timespec mtime {};
  long long disk_size = -1;
  bool readonly = false;

  enum class Notice { None, Changed, Deleted, Recovered, Error };
  Notice notice = Notice::None;
  std::string notice_text;

  // View state while the document is not shown.
  int cursor = 0, top_line = 1, horiz = 0;
  int sel_start = 0, sel_end = 0;

  // Crash recovery.
  std::string backup_id;
  unsigned backup_state = 0;
  bool backup_written = false;

  unsigned revision = 0;  // increases on every change
  double last_edit = 0;

  bool modified() const { return !undo.at_saved() || format_dirty; }
  std::string display_name() const;
  std::string folder() const;
  // UTF-8 copy of the whole text, cached until the next change.
  const std::string &text();
  int length() const { return buf->length(); }
  std::string indent_unit() const;
  int line_of(int pos);  // 1-based, cached incrementally
  int line_count() const;

  bool load(const std::string &file, std::string &error, const Encoding *forced = nullptr);
  bool reload(std::string &error, const Encoding *forced = nullptr);
  bool save_to(const std::string &file, std::string &error);
  // Replaces the content without recording undo (loads, recovery).
  void set_content(const std::string &utf8);
  void stat_disk();
  // 1: changed on disk, 2: deleted, 0: unchanged.
  int disk_status() const;

  void set_language(const Language *l, bool manual);
  void apply_indent_detection(bool detect, int default_width, bool default_spaces);

  // Undo/redo; returns the new cursor position or -1.
  int do_undo();
  int do_redo();
  void begin_group(int cursor = -1) { undo.begin_group(cursor); }
  void end_group() { undo.end_group(); }

  // Background highlighting; returns true while work remains.
  bool highlight_pending() const { return hl_enabled_ && valid_end_ < buf->length(); }
  void highlight_step(int budget);
  void rehighlight_all();
  bool highlighting() const { return hl_enabled_; }

 private:
  static void modify_cb(int pos, int ins, int del, int restyled, const char *deleted, void *arg);
  void on_modify(int pos, int ins, int del, const char *deleted);
  // Re-highlights from line start |from| until the state matches the stored
  // one beyond |must_reach|, or |budget| bytes; returns the position reached.
  int rehighlight(int from, int must_reach, int budget, bool &stable);
  void apply_history(const UndoStep *step, bool undo_dir, int &cursor);

  std::string snapshot_;
  bool snapshot_ok_ = false;
  bool recording_ = true;
  bool hl_enabled_ = true;
  int valid_end_ = 0;
  int cache_pos_ = 0, cache_line_ = 1;
};

// ------------------------------------------------------------ editor view

class EditorView : public Fl_Text_Editor {
 public:
  EditorView(App *app, int x, int y, int w, int h);
  void attach(Document *doc);
  Document *doc() const { return doc_; }
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void apply_look();      // fonts, colors, style table
  void update_gutter();   // line number width
  void apply_wrap(bool wrap);
  int line_height() const { return mMaxsize; }
  int top_line() const { return mTopLineNum; }
  int first_char() const { return mFirstChar; }
  int last_char() const { return mLastChar; }
  int visible_lines() const { return mNVisibleLines; }
  void goto_pos(int pos, bool center = true, int sel_to = -1);
  int char_width() const { return char_w_; }
  int font_px() const { return font_px_; }

 private:
  void draw_overlay();
  void draw_gutter(sde::Painter &p);
  void draw_scrollbars(sde::Painter &p);
  void notify_cursor();
  App *app_;
  Document *doc_ = nullptr;
  Style_Table_Entry table_[kStateBase - 'A' + kMaxStates];
  int gutter_digits_ = 0;
  int char_w_ = 8, font_px_ = 14;
  bool dnd_ = false;
  int last_cursor_ = -1;
  // Bracket match cache.
  unsigned br_rev_ = ~0u;
  int br_cursor_ = -1;
  long br_a_ = -1, br_b_ = -1;
};

// ---------------------------------------------------------------- chrome

// Fl_Input with a key hook (Enter, Escape, arrows) for bars and palettes.
class Field : public Fl_Input {
 public:
  Field(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {}
  std::function<int(int key, int state)> on_key;
  int handle(int event) override;
};

class TabBar : public Fl_Widget {
 public:
  TabBar(App *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void ensure_visible(int index);

 private:
  struct Tab {
    double x = 0, w = 0;
  };
  std::vector<Tab> layout() const;
  int tab_at(int mx) const;
  bool on_close(int index, int mx, int my) const;
  double plus_x() const;
  App *app_;
  int hover_ = -1, press_ = -1, drag_from_ = -1;
  bool hover_close_ = false, hover_plus_ = false, dragging_ = false;
  int press_x_ = 0;
  double scroll_ = 0;
};

class StatusBar : public Fl_Widget {
 public:
  StatusBar(App *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;

 private:
  struct Pill {
    std::string text;
    int id = 0;
    double x = 0, w = 0;
  };
  std::vector<Pill> pills() const;
  App *app_;
  int hover_ = -1;
};

class InfoBar : public Fl_Group {
 public:
  InfoBar(App *app, int x, int y, int w, int h);
  void draw() override;
  void resize(int x, int y, int w, int h) override;
  void show_for(Document *doc);

 private:
  void layout_buttons();
  App *app_;
  Document *doc_ = nullptr;
  sde::FlatButton *primary_, *secondary_;
  std::string text_;
  bool warning_ = false;
};

class StartPage : public Fl_Widget {
 public:
  StartPage(App *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;

 private:
  struct Hit {
    double x, y, w, h;
    int id;  // 0 new, 1 open, 100 + recent index
  };
  std::vector<Hit> hits() const;
  App *app_;
  int hover_ = -1;
};

class Sidebar : public Fl_Widget {
 public:
  Sidebar(App *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void set_folder(const std::string &dir);
  const std::string &folder() const { return folder_; }
  void refresh();
  void set_mode(int m);
  int mode() const { return mode_; }

 private:
  struct Entry {
    std::string name, path;
    bool dir = false;
  };
  int rows_top() const;
  int row_h() const;
  int row_count() const;
  int row_at(int my) const;
  void activate_row(int row, bool middle);
  App *app_;
  int mode_ = 0;  // 0 documents, 1 files
  std::string folder_;
  std::vector<Entry> entries_;
  int hover_ = -1, hover_seg_ = -1;
  bool hover_up_ = false, hover_close_ = false;
  double scroll_ = 0;
  bool resizing_ = false;
};

class FindBar : public Fl_Group {
 public:
  FindBar(App *app, int x, int y, int w, int h);
  void draw() override;
  void open(bool replace, const std::string &seed);
  void close();
  bool replace_mode() const { return replace_mode_; }
  std::string query() const { return find_->value(); }
  std::string replacement() const { return replace_->value(); }
  SearchOptions options() const { return opts_; }
  void set_status(const std::string &s, bool error);
  int wanted_height() const;
  void relayout();
  Field *find_field() { return find_; }

 private:
  sde::DrawnButton *chip(const char *label, bool *flag);
  sde::DrawnButton *icon_button(icon::Kind kind, std::function<void()> fn, const char *tip);
  App *app_;
  Field *find_, *replace_;
  sde::DrawnButton *toggle_, *case_, *word_, *regex_, *prev_, *next_, *close_;
  sde::FlatButton *rep_one_, *rep_all_;
  SearchOptions opts_;
  bool replace_mode_ = false;
  std::string status_;
  bool error_ = false;
};

struct PaletteItem {
  std::string label, detail, hint;
  std::function<void()> run;
  const Language *badge = nullptr;
  int score = 0;
};

class Palette : public Fl_Group {
 public:
  Palette(App *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void open(const std::string &prefix);
  void close(bool revert = true);
  void relayout();

 private:
  void rebuild();
  void run_selected();
  int list_top() const;
  int item_h() const;
  App *app_;
  Field *input_;
  std::vector<PaletteItem> items_;
  int sel_ = 0, scroll_ = 0, hover_ = -1;
  std::string mode_;  // ">", ":", "@lang", ""
  int origin_cursor_ = -1, origin_top_ = 1;
};

// In-window modal question with buttons.
class Sheet : public Fl_Group {
 public:
  Sheet(App *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  int ask(const std::string &title, const std::string &message, const std::vector<std::string> &buttons,
          int default_button, int cancel_button);
  bool active() const { return active_; }

 private:
  void layout_card();
  App *app_;
  std::string title_, message_;
  std::vector<sde::FlatButton *> buttons_;
  int result_ = -1, cancel_ = 0, default_ = 0;
  bool active_ = false;
  double cx_ = 0, cy_ = 0, cw_ = 0, ch_ = 0;
};

void show_settings(App *app);

// ------------------------------------------------------------- commands

struct Command {
  std::string id, label, group;
  int shortcut = 0;           // FLTK shortcut value (FL_CTRL + 's')
  std::string shortcut_text;  // "Ctrl+S"
  std::function<void()> run;
};

// ------------------------------------------------------------------ app

class App : public sde::BufferedWindow {
 public:
  App(int w, int h);
  ~App() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

  // Documents.
  const std::vector<std::unique_ptr<Document>> &docs() const { return docs_; }
  Document *active() const { return active_ < 0 || active_ >= int(docs_.size()) ? nullptr : docs_[size_t(active_)].get(); }
  int active_index() const { return active_; }
  void activate(int index);
  Document *new_document();
  Document *open_file(const std::string &path, int line = 0, int column = 0, bool activate_it = true);
  void open_files(const std::vector<std::string> &args, const std::string &cwd);
  bool close_document(int index, bool ask = true);
  void move_document(int from, int to);
  int find_document(const std::string &path) const;
  void reopen_closed();

  // File actions.
  void open_dialog();
  bool save(Document *doc);
  bool save_as(Document *doc);
  void save_all();
  void reload(Document *doc, const Encoding *forced = nullptr);
  void quit();

  // Editing actions (actions.cpp).
  int editor_key(int key, int state, const char *text, int len);
  void undo();
  void redo();
  void duplicate_lines();
  void delete_lines();
  void move_lines(int dir);
  void toggle_comment();
  void indent(bool in);
  void transform(const std::function<std::string(const std::string &)> &fn, bool whole_lines);
  void select_line();
  void goto_matching_bracket();
  void insert_line(bool above);
  void delete_word(bool forward);
  void replace_range(int start, int end, const std::string &text, bool select);
  std::string selection_text();

  // Search.
  void show_find(bool replace);
  void hide_find();
  void find_changed();
  void find_next(bool forward);
  void replace_one();
  void replace_all();
  void refresh_matches();
  const std::vector<Match> &matches() const { return matches_; }
  const std::vector<int> &match_lines() const { return match_lines_; }
  bool find_visible() const;

  // View.
  void toggle_sidebar();
  void set_wrap(bool on);
  void zoom(int delta);  // 0 resets
  void show_palette(const std::string &prefix);
  void goto_line(int line, int column);
  void set_language(const Language *l);
  void set_encoding_for_save(Encoding e);
  void set_eol(Eol e);
  void set_indent(bool tabs, int width);
  void popup_menu(int x, int y);
  void popup_status(int id, int x, int y);
  void open_settings() { show_settings(this); }
  void about();

  // State.
  Settings &settings() { return settings_; }
  void apply_settings(bool save = true);
  const std::vector<std::string> &recent() const { return recent_; }
  const std::vector<Command> &commands() const { return commands_; }
  void run_command(const std::string &id);
  EditorView *editor() const { return editor_; }
  Sidebar *sidebar() const { return sidebar_; }
  TabBar *tabs() const { return tabs_; }
  void doc_changed(Document *doc);    // text modified
  void doc_restyled(Document *doc, int start, int end);
  void cursor_moved();
  void update_ui();                   // tabs, status, title
  void toast(const std::string &message);
  const std::string &toast_text() const { return toast_; }
  int ask(const std::string &title, const std::string &message, const std::vector<std::string> &buttons,
          int default_button, int cancel_button);
  bool modal() const;
  void layout();
  void apply_theme();
  void restore_session();
  void save_session();
  void recover_backups();
  void write_backups(bool all);
  void remove_backup(Document *doc);
  void check_disk(bool all);
  void handle_notice(Document *doc, int button);
  int header_h() const { return header_h_; }
  void raise_window();
  std::string cwd_folder() const;

 private:
  void build_commands();
  bool global_shortcut(int key, int state);
  static int dispatch(int event, Fl_Window *w);
  static void idle_cb(void *p);
  static void ui_cb(void *p);
  static void backup_cb(void *p);
  static void watch_cb(void *p);
  static void toast_cb(void *p);
  static void match_cb(void *p);
  static int x_event(int event);
  void schedule_ui();
  void draw_header(sde::Painter &p);
  void save_recent();
  Document *recover_from(const std::string &id, const std::string &path);

  Settings settings_;
  std::vector<std::unique_ptr<Document>> docs_;
  int active_ = -1;
  int untitled_counter_ = 0;
  std::vector<std::string> recent_, closed_;
  std::vector<Command> commands_;
  std::vector<Match> matches_;
  std::vector<int> match_lines_;
  unsigned match_rev_ = ~0u;
  Document *match_doc_ = nullptr;
  Searcher searcher_;
  int find_origin_ = -1;

  EditorView *editor_ = nullptr;
  TabBar *tabs_ = nullptr;
  StatusBar *status_ = nullptr;
  InfoBar *info_ = nullptr;
  StartPage *start_ = nullptr;
  Sidebar *sidebar_ = nullptr;
  FindBar *find_ = nullptr;
  Palette *palette_ = nullptr;
  Sheet *sheet_ = nullptr;
  sde::DrawnButton *menu_btn_ = nullptr, *side_btn_ = nullptr, *cmd_btn_ = nullptr;
  int header_h_ = 0, status_h_ = 0;
  std::string toast_;
  bool ui_pending_ = false, quitting_ = false;
  std::string instance_tag_;
  static App *instance_;
  friend class Palette;
};

// instance.cpp: single-window handoff through a UNIX socket.
// Returns true when another Niny took the files (this process should exit).
bool hand_off_to_running(const std::vector<std::string> &args, const std::string &cwd);
void listen_for_instances(App *app);
void stop_listening();

// Atomic save that keeps permissions and follows symbolic links.
bool write_file_safely(const std::string &path, const std::string &data, std::string &error);
double now_seconds();
std::string home_relative(const std::string &path);

}  // namespace niny
