// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's window (docs/LIRIA.md), after the File Explorer of Windows 11:
//
//   [|]  [Home x] [Documents x] [+]                          [-] [o] [x]
//   [<] [>] [^] [R]  ( Home > Documents          )  ( Search Documents )
//   [New v] | [cut] [copy] [paste] [rename] [share] [delete] | [Sort v] [View v] | [...]
//   Home          |  ^ Quick access
//   Trash         |  [Desktop *] [Downloads *] [Documents *] ...
//   Pinned        |  ^ Drives
//     Desktop  *  |  [System ====---- 25 GB free of 99 GB] ...
//   Drives        |  ^ Network locations
//   Network       |  ^ Recent files
//   12 items | 2 items selected 4.2 MB                          [details][icons]
//
// The window draws its own title bar (the tabs, as in Windows 11) and
// caption buttons, and is moved and resized by the window manager through
// _NET_WM_MOVERESIZE; [liria] titlebar=system in sde.conf, or
// LIRIA_SYSTEM_TITLEBAR=1, keeps SDE WM's frame instead. Everything is
// drawn with sde::Painter in the active theme and follows its changes; in
// a translucent theme the window follows the transparency of the window
// frames, and with SDE Liquid Glass its tabs, toolbar and navigation pane
// are glass while the files stay on an opaque card (Liria::Look).
#pragma once

#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Widget.H>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "address.h"
#include "buffered_window.h"
#include "drives.h"
#include "file_chooser_model.h"
#include "fileops.h"
#include "openwith.h"
#include "pins.h"
#include "prefs.h"
#include "theme.h"
#include "viewmodel.h"
#include "xdnd.h"

namespace sde {
class ContextMenu;
namespace liria {

class MainWindow;
struct RemoteListing;
struct RemotePoll;
class FileView;
class HomeView;
class Sidebar;
class TabBar;
class AddressBar;
class SearchBox;
class CommandBar;
class NavButton;
class FileClipboard;
class SuggestionList;

// --------------------------------------------------------------- the look

struct Look {
  Color chrome;   // the tab strip (the title bar)
  Color toolbar;  // the active tab, the navigation and command bars, the sidebar
  Color card;     // the files and the Home page
  Color card_border;
  Color text, dim, faint;
  Color hover, press;
  Color select, select_line;  // selected items: fill and outline
  Color accent, accent_text;
  bool liquid = false;  // SDE Liquid Glass: the chrome is glass
};
// The look of the active theme (computed when the theme changes).
const Look &look();
void update_look();

// ------------------------------------------------------------ a background wake-up

// Wakes the FLTK loop from worker threads through a pipe (Fl::add_fd).
class Waker {
 public:
  explicit Waker(std::function<void()> fn);
  ~Waker();
  void wake();

 private:
  static void ready(int fd, void *data);
  int fds_[2] = {-1, -1};
  std::function<void()> fn_;
};

// --------------------------------------------------------------- a tab

struct Tab {
  std::string location;  // location.h
  chooser::History history;
  ViewModel model;
  Layout layout = Layout::Icons;
  int icon_size = 72;
  bool loading = false;
  std::string error;
  uint64_t generation = 0;
  // A search in the folder and below it.
  std::string query;
  bool searching = false;
  uint64_t search_generation = 0;
  std::vector<Item> folder_items;  // the folder itself, while results are shown
  int scroll = 0;
  std::string select_when_loaded;
  std::set<std::string> also_select;
  std::string remote_path;  // the URI a GIO listing reads (remote folders)
  double opened_at = 0, loaded_ms = -1;
  long long free_bytes = -1;
  std::string title() const;
  bool is_folder() const;  // a folder (local or remote), not Home/Network
};

// ------------------------------------------------------------- the window

class MainWindow : public BufferedWindow {
 public:
  MainWindow(int w, int h);
  ~MainWindow() override;

  // ---- tabs
  Tab &tab() { return *tabs_[size_t(current_)]; }
  int tab_count() const { return int(tabs_.size()); }
  int current_tab() const { return current_; }
  const Tab &tab_at(int i) const { return *tabs_[size_t(i)]; }
  void new_tab(const std::string &location, bool activate = true, const std::string &select = "");
  void close_tab(int i);
  void select_tab(int i);
  void move_tab(int from, int to);
  void duplicate_tab(int i);

  // ---- going places
  void go(const std::string &location, bool record = true, const std::string &select = "");
  void go_typed(const std::string &text);
  void back();
  void forward();
  void up();
  void refresh();
  void show_item(const std::string &path);  // opens its folder and selects it
  void open_network_location(const std::string &uri, bool new_tab = false);

  // ---- items
  void open_item(const Item &item, bool new_tab = false);
  void open_paths(const std::vector<std::string> &paths, bool new_tabs = false);
  void open_selection(bool new_tab = false);
  std::vector<std::string> selected_paths() const;
  std::vector<Item> selected_items() const;
  void selection_changed();

  // ---- commands (the command bar, menus and keys)
  void command(const std::string &id);
  void cut();
  void copy();
  void paste();
  void paste_into(const std::string &dir);
  void rename();
  void remove(bool permanently);
  void new_folder();
  void new_document(const std::string &template_path);
  void select_all();
  void select_none();
  void invert_selection();
  void properties();
  void open_with_dialog();
  void open_terminal_here();
  void copy_path();
  void email();
  void empty_trash();
  void restore_selection();
  void undo();
  void compress();
  void extract_here();
  void install_package();
  void run_appimage();
  void set_layout(Layout l);
  void set_icon_size(int px);
  void zoom(int steps);
  void sort_by(chooser::SortKey key);
  void set_sort_descending(bool descending);
  void toggle_hidden();
  void toggle_sidebar();
  void toggle_fullscreen();
  void connect_to_server();
  void open_as_administrator(const std::string &path);
  void about();

  // ---- menus (menus.cpp)
  void item_menu(int x, int y);        // the selected items
  void background_menu(int x, int y);  // the empty part of a folder
  void place_menu(const std::string &location, int x, int y, const std::string &drive_id = "");
  void new_menu(Fl_Widget *anchor);
  void share_menu(Fl_Widget *anchor, int x, int y);
  void sort_menu(int x, int y);
  void view_menu(int x, int y);
  void more_menu(int x, int y);
  void send_to(const std::string &dir);
  // Pops |m| up (below |anchor| when given); the QA description names its
  // items while it is open ("@menu:").
  void show_menu(ContextMenu &m, int x, int y, Fl_Widget *anchor = nullptr);

  // ---- drag and drop
  // Files dropped on |dir| (a folder, or the folder shown) with |action|.
  void drop_files(const std::vector<std::string> &paths, const std::string &dir, desk::DropAction action);
  desk::DropAction drop_action_for(const std::vector<std::string> &paths, const std::string &dir, unsigned mods,
                                   desk::DropAction suggested);
  void begin_drag(const std::vector<std::string> &paths, int count, unsigned long time);
  bool drag_motion(int x_root, int y_root, unsigned mods, unsigned long time);  // true: over this window
  void drag_drop(unsigned long time);
  void cancel_drag();
  bool dragging() const;
  desk::Xdnd *xdnd() { return xdnd_.get(); }
  // While dragging over the window: the folder under the pointer, as drawn.
  std::string drop_target;

  // ---- messages, state
  void say(const std::string &text, bool error = false);
  const std::string &message() const { return message_; }
  bool message_error() const { return message_error_; }
  void update_title();
  void update_commands();
  void layout();
  void retheme();
  void places_changed();   // pins, drives, servers
  void folder_changed(const std::string &dir, const std::vector<std::string> &names);
  std::string status_text() const;
  std::string selection_text() const;
  std::string folder_of_tab() const;  // the local folder shown, or ""
  bool can_paste() const;
  bool can_write_here() const;

  // ---- thumbnails and icons (shared by the views)
  struct Thumb {
    cairo_surface_t *surface = nullptr;
    int w = 0, h = 0;
  };
  Thumb *thumbnail(const Item &e, int size);
  void schedule_thumbnails();
  const std::vector<std::string> &icon_names(const Item &e);
  const std::string &type_text(const std::string &mime);

  // ---- parts
  TabBar *tabbar() { return tabbar_; }
  FileView *fileview() { return fileview_; }
  HomeView *homeview() { return homeview_; }
  Sidebar *sidebar() { return sidebar_; }
  AddressBar *addressbar() { return addressbar_; }
  SearchBox *searchbox() { return searchbox_; }
  CommandBar *commandbar() { return commandbar_; }
  bool csd() const { return csd_; }
  int card_x() const { return card_x_; }
  int card_y() const { return card_y_; }
  int card_w() const { return card_w_; }
  int card_h() const { return card_h_; }
  void start_move(int button);
  void start_resize(int edge, int button);
  int edge_at(int x, int y) const;  // 0-7 (_NET_WM_MOVERESIZE), -1 none
  bool maximized() const;
  void toggle_maximize();
  void minimize();

  // Search typed in the search box (searches below after a moment).
  void set_query(const std::string &q);
  void start_deep_search();

  void qa_dump();
  void schedule_qa();
  std::string window_id() const { return id_; }
  double opened_at = 0, first_frame_ms = -1;

  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void show() override;
  void hide() override;
  bool shortcut(int key);

 private:
  friend class FileView;
  friend class HomeView;
  std::vector<std::unique_ptr<Tab>> tabs_;
  int current_ = 0;
  int bg_next_ = -1;  // where the next tab opened in the background goes
  TabBar *tabbar_ = nullptr;
  NavButton *back_ = nullptr, *forward_ = nullptr, *up_ = nullptr, *refresh_ = nullptr;
  AddressBar *addressbar_ = nullptr;
  SearchBox *searchbox_ = nullptr;
  CommandBar *commandbar_ = nullptr;
  Sidebar *sidebar_ = nullptr;
  FileView *fileview_ = nullptr;
  HomeView *homeview_ = nullptr;
  int card_x_ = 0, card_y_ = 0, card_w_ = 0, card_h_ = 0, status_y_ = 0;
  int view_icons_x_ = 0, view_details_x_ = 0, view_btn_w_ = 0;
  int status_hover_ = -1;
  std::string message_;
  bool message_error_ = false;
  std::string menu_qa_;  // the items of the open menu, for the UI tests
  bool csd_ = true;
  bool sizing_sidebar_ = false;
  std::string id_;
  std::unique_ptr<desk::Xdnd> xdnd_;
  std::vector<std::string> drag_paths_;
  // Thumbnails of the visible tab.
  std::unordered_map<std::string, Thumb> thumbs_;
  std::unordered_map<std::string, std::vector<std::string>> icon_cache_;
  std::unordered_map<std::string, std::string> type_cache_;
  Waker waker_;
  // Readers of the tabs: one for listings, one for searches.
  std::unique_ptr<chooser::FolderReader> reader_, searcher_;
  std::unique_ptr<chooser::Thumbnailer> thumbnailer_;
  void pump();
  void start_listing(Tab &t);
  void apply_items(Tab &t, std::vector<Item> &&items, bool done, const std::string &error);
  void after_load(Tab &t);
  void request_thumbnails();
  static void thumbnails_cb(void *data);
  static void deep_search_cb(void *data);
  static void clear_message_cb(void *data);
  static void qa_cb(void *data);
  void setup_dnd();
  void apply_translucency();
  void save_geometry();
  void remember_view();
  void list_remote(Tab &t, uint64_t generation);
  static void remote_poll_cb(void *data);
  void list_trash(Tab &t);
};

// ---------------------------------------------------------------- the app

class App {
 public:
  static App &get();
  Prefs prefs;
  Pins pins;
  Servers servers;
  Drives drives;
  Apps apps;
  UndoStack undo;
  std::unique_ptr<FileClipboard> clipboard;
  std::vector<MainWindow *> windows;
  MainWindow *last_active = nullptr;

  // A new window with |locations| as tabs (the Home page when empty); the
  // items of |select| are selected in their folders.
  MainWindow *open_window(const std::vector<std::string> &locations, const std::vector<std::string> &select = {});
  void window_closed(MainWindow *w);
  MainWindow *active_window();
  // Everything that shows places changes: pins, drives, servers.
  void places_changed();
  void theme_changed();
  void check_pins();  // another window (or program) changed the pin file
  // Watches |dir| for changes of its items (inotify); the windows that show
  // it are told through MainWindow::folder_changed().
  void watch(const std::string &dir);
  void unwatch_unused();
  // Quits when no window is left (and the clipboard is no one's).
  void maybe_quit();
  // Nothing keeps Liria running: no window, no job, no files on the
  // clipboard that are Liria's, and (started by D-Bus) no call for a while.
  bool should_quit();
  bool service_mode = false;  // started by D-Bus: no window of its own
  double service_until = 0;   // now_ms() until which a D-Bus start waits for calls
  bool quit_requested = false;

  // Jobs on worker threads with the transfer dialog (jobs.cpp).
  void transfer(const std::vector<std::string> &sources, const std::string &dest, Transfer kind, MainWindow *from,
                bool select_results = true);
  void trash(const std::vector<std::string> &paths, MainWindow *from);
  void delete_permanently(const std::vector<std::string> &paths, MainWindow *from);
  void restore(const std::vector<TrashItem> &items, MainWindow *from);
  void empty_trash(MainWindow *from);
  void undo_last(MainWindow *from);
  int running_jobs() const;

  void start_services();
  void init_watch();

 private:
  App() = default;
  int inotify_fd_ = -1;
  std::map<int, std::string> watches_;  // descriptor -> folder
  std::map<std::string, int> watched_;  // folder -> descriptor
  std::map<std::string, std::vector<std::string>> pending_changes_;
  int config_watch_ = -1, data_watch_ = -1;
  bool recent_changed_ = false, config_changed_ = false;
  static void inotify_cb(int fd, void *data);
  static void changes_cb(void *data);
  static void drives_cb(void *data);
};

// ------------------------------------------------------- widgets (widgets.cpp)

// A toolbar button with a line icon (Back, Forward, Up, Refresh).
class NavButton : public Fl_Widget {
 public:
  NavButton(const char *glyph, const char *tip);
  std::string glyph;
  bool hot = false, down = false;
  void draw() override;
  int handle(int e) override;
};

// The tab strip: tabs, "+", the pane toggle and the caption buttons.
class TabBar : public Fl_Widget {
 public:
  explicit TabBar(MainWindow *w);
  struct Slot {
    int x = 0, w = 0, close_x = 0;
  };
  std::vector<Slot> slots;
  int plus_x = 0, pane_x = 0, caption_x = 0, button_w = 0;
  int hover = -1, hover_close = -1, hover_button = -1;  // buttons: 0 pane, 1 plus, 2 min, 3 max, 4 close
  int press = -1, drag_from = -1, drag_x = 0;
  void relayout();
  void draw() override;
  int handle(int e) override;
  int tab_at(int x) const;

 private:
  MainWindow *w_;
};

// The breadcrumbs, and the typed address (Ctrl+L, Alt+D).
class AddressBar : public Fl_Group {
 public:
  explicit AddressBar(MainWindow *w);
  struct Crumb {
    std::string label, location;
    std::vector<std::string> icons;
    int x = 0, w = 0, sep_x = 0;
    bool current = false, overflow = false;
  };
  std::vector<Crumb> crumbs, hidden;
  Fl_Input *entry;
  SuggestionList *list = nullptr;
  bool editing = false;
  int hover = -1, hover_sep = -1;
  struct Suggestion {
    std::string text, label, detail, glyph;
  };
  std::vector<Suggestion> suggestions;
  int suggestion = -1;
  std::string error;  // why the typed text is no location (shown under the bar)
  void rebuild();
  void go_typed(const std::string &text);
  void start_edit(const std::string &text = "");
  void stop_edit();
  void update_suggestions();
  void draw() override;
  int handle(int e) override;
  void resize(int x, int y, int w, int h) override;
  int crumb_at(int x) const;
  int sep_at(int x) const;

 private:
  MainWindow *w_;
  void layout_crumbs();
};

// The address suggestions under the address bar (folders, recent servers):
// a child of the window, drawn over the views.
class SuggestionList : public Fl_Widget {
 public:
  explicit SuggestionList(MainWindow *w);
  int row_h() const;
  int rows() const;
  void place();
  void draw() override;
  int handle(int e) override;
  int hover = -1;

 private:
  MainWindow *w_;
};

class SearchBox : public Fl_Input {
 public:
  explicit SearchBox(MainWindow *w);
  std::string placeholder;
  void draw() override;
  int handle(int e) override;

 private:
  MainWindow *w_;
};

// The command bar: New, the edit commands, Sort, View, the contextual
// commands and More.
class CommandBar : public Fl_Widget {
 public:
  explicit CommandBar(MainWindow *w);
  struct Button {
    std::string id, glyph, label, tip;
    bool enabled = true, dropdown = false, accent = false, separator_before = false;
    int x = 0, w = 0;
  };
  std::vector<Button> buttons;
  int hover = -1, press = -1;
  void rebuild();
  void draw() override;
  int handle(int e) override;
  int button_at(int x, int y) const;
  const Button *find(const std::string &id) const;

 private:
  MainWindow *w_;
};

// --------------------------------------------------------------- the views

// The navigation pane (sidebar.cpp).
class SidebarCanvas;
class Sidebar : public Fl_Scroll {
 public:
  explicit Sidebar(MainWindow *w);
  struct Row {
    enum Kind { Place, Heading, Gap } kind = Place;
    std::string id;         // "home", "trash", "pin:<uri>", "drive:<id>", "net:<id>", "server:<uri>", "network"
    std::string label, location, drive_id;
    std::vector<std::string> icons;
    std::string group;      // headings: "pinned", "drives", "network"
    bool expanded = true, pinned = false, ejectable = false;
    int y = 0, h = 0;
  };
  std::vector<Row> rows;
  int hover = -1, hover_eject = -1, drop_row = -1, drop_gap = -1, focus_row = -1;
  void rebuild();
  int row_at(int y) const;  // window coordinates
  void draw() override;
  void resize(int x, int y, int w, int h) override;
  SidebarCanvas *canvas = nullptr;
  int canvas_y() const;
  MainWindow *win() { return w_; }

 private:
  MainWindow *w_;
};

class FileCanvas;
class FileView : public Fl_Group {
 public:
  explicit FileView(MainWindow *w);
  Fl_Scroll *scroll;
  FileCanvas *canvas;
  // Metrics of the current layout.
  int pad = 0, cols = 1, tile_w = 0, tile_h = 0, row_h = 0, head_h = 0, image = 0;
  void metrics();
  int content_height();
  void sync();  // after the items, layout or size changed
  void item_rect(int k, int &x, int &y, int &w, int &h) const;  // window coordinates
  int item_at(int x, int y) const;
  void visible_range(int &first, int &last) const;
  void ensure_visible(int k);
  void scroll_to_y(int y);
  int scroll_y() const;
  struct Column {
    std::string id, title;
    int x = 0, w = 0;
    bool right = false;
    chooser::SortKey key = chooser::SortKey::Name;
    bool sortable = true;
  };
  std::vector<Column> columns() const;
  int header_hover = -1;
  // Inline rename (F2).
  Fl_Input *editor;
  std::string editing;  // the path being renamed
  void canvas_focus();
  void reset_hover();
  void forget_wraps();
  void start_rename(const std::string &path);
  void finish_rename(bool keep);
  void draw() override;
  int handle(int e) override;
  void resize(int x, int y, int w, int h) override;
  MainWindow *win() { return w_; }
  int thumb_size() const;

 private:
  MainWindow *w_;
};

class HomeCanvas;
class HomeView : public Fl_Scroll {
 public:
  explicit HomeView(MainWindow *w);
  struct Card {
    enum Kind { Pin, Drive, Network, Server, Recent, Header, Empty, Button } kind = Pin;
    std::string id, label, sub, location, path;
    std::vector<std::string> icons;
    std::string section;  // header: "quick", "drives", "network", "recent"
    long long total = -1, free = -1;
    int index = -1;       // pins: the index in the pin list
    std::string mime;
    time_t time = 0;
    int x = 0, y = 0, w = 0, h = 0;
  };
  std::vector<Card> cards;
  int hover = -1, focus = -1, drop_card = -1, drop_gap = -1;
  std::vector<chooser::RecentFile> recent;
  void rebuild();
  void relayout();
  int card_at(int x, int y) const;  // window coordinates
  void draw() override;
  void resize(int x, int y, int w, int h) override;
  HomeCanvas *canvas = nullptr;
  int canvas_x() const;
  int canvas_y() const;
  void focus_canvas();
  MainWindow *win() { return w_; }
  int content_h = 0;

 private:
  MainWindow *w_;
};

// ------------------------------------------------------------ dialogs (dialogs.cpp)

// Asks what to do with items whose destination exists.
Answer ask_clash(MainWindow *parent, const Clash &c, bool &all, Transfer kind);
// "Delete these items permanently?" and friends; true when confirmed.
bool confirm(MainWindow *parent, const std::string &title, const std::string &text, const std::string &yes,
             bool destructive = true);
void show_properties(MainWindow *parent, const std::vector<Item> &items);
void show_open_with(MainWindow *parent, const Item &item);
void show_sign_in(MainWindow *parent, const std::string &message, const std::string &user, const std::string &domain,
                  int flags, bool keyring, const std::string &location, std::function<void(const SignIn &)> reply);
void show_question(MainWindow *parent, const std::string &message, const std::vector<std::string> &choices,
                   std::function<void(int)> reply);
void show_connect(MainWindow *parent);
// Questions of GIO's mount operations, answered with Liria's dialogs.
MountQuestions mount_questions(MainWindow *parent, const std::string &location);

// Small helpers shared by the widgets (widgets.cpp).
std::string thousands(long long n);
std::string display_path(const std::string &path);
void draw_icon(Painter &p, const std::vector<std::string> &names, double x, double y, double size,
               double alpha = 1.0);
bool hovering(Fl_Widget *w);
double now_ms();
// The keyboard modifiers of the last event as desk::MOD_* bits.
unsigned fl_mods();

}  // namespace liria
}  // namespace sde
