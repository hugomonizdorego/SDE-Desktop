// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's window (ui.h): its layout and drawing, the tabs, going places,
// reading folders in the background, the commands, the keys of Windows
// Explorer, drag and drop and the QA description for the tests.
#define FL_INTERNALS 1
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include "about.h"
#include "clipboard.h"
#include "config.h"
#include "desktop_model.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "location.h"
#include "menu.h"
#include "spawn.h"
#include "trash.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "x11.h"

namespace sde {
namespace liria {

namespace {

int g_window_serial = 0;

bool system_titlebar() {
  const char *e = getenv("LIRIA_SYSTEM_TITLEBAR");
  if (e && *e && *e != '0') return true;
  Config cfg;
  cfg.load();
  return cfg.get("liria", "titlebar", "own") == "system";
}

Item item_from(chooser::Entry &&e) {
  Item it;
  static_cast<chooser::Entry &>(it) = std::move(e);
  return it;
}

}  // namespace

std::string Tab::title() const {
  if (!query.empty()) return format(_("Search results in %s"), location_title(location).c_str());
  return location_title(location);
}

bool Tab::is_folder() const { return is_local(location) || is_remote(location); }

// ------------------------------------------------------------------ construction

MainWindow::MainWindow(int W, int H) : BufferedWindow(W, H), waker_([this] { pump(); }) {
  id_ = std::to_string(++g_window_serial);
  csd_ = !system_titlebar();
  if (csd_) border(0);
  update_look();
  color(look().toolbar.fl());
  begin();
  tabbar_ = new TabBar(this);
  back_ = new NavButton("back", _("Back (Alt+Left)"));
  back_->callback([](Fl_Widget *, void *d) { static_cast<MainWindow *>(d)->back(); }, this);
  forward_ = new NavButton("forward", _("Forward (Alt+Right)"));
  forward_->callback([](Fl_Widget *, void *d) { static_cast<MainWindow *>(d)->forward(); }, this);
  up_ = new NavButton("up", _("Up to the folder above (Alt+Up)"));
  up_->callback([](Fl_Widget *, void *d) { static_cast<MainWindow *>(d)->up(); }, this);
  refresh_ = new NavButton("refresh", _("Refresh (F5)"));
  refresh_->callback([](Fl_Widget *, void *d) { static_cast<MainWindow *>(d)->refresh(); }, this);
  addressbar_ = new AddressBar(this);
  searchbox_ = new SearchBox(this);
  commandbar_ = new CommandBar(this);
  sidebar_ = new Sidebar(this);
  homeview_ = new HomeView(this);
  fileview_ = new FileView(this);
  addressbar_->list = new SuggestionList(this);
  end();
  resizable(nullptr);
  size_range(S(480), S(360));
  reader_.reset(new chooser::FolderReader([this] { waker_.wake(); }));
  searcher_.reset(new chooser::FolderReader([this] { waker_.wake(); }));
  thumbnailer_.reset(new chooser::Thumbnailer([this] { waker_.wake(); }, 2));
  xclass("liria");
  callback([](Fl_Widget *w, void *) { static_cast<MainWindow *>(w)->hide(); });
  tabs_.push_back(std::make_unique<Tab>());
  current_ = 0;
  App::get().windows.push_back(this);
  opened_at = now_ms();
}

MainWindow::~MainWindow() {
  Fl::remove_timeout(thumbnails_cb, this);
  Fl::remove_timeout(deep_search_cb, this);
  Fl::remove_timeout(clear_message_cb, this);
  Fl::remove_timeout(qa_cb, this);
  // Threads stop before the thumbnails go.
  thumbnailer_.reset();
  reader_.reset();
  searcher_.reset();
  for (auto &kv : thumbs_)
    if (kv.second.surface) cairo_surface_destroy(kv.second.surface);
  thumbs_.clear();
}

void MainWindow::show() {
  bool first = !shown();
  BufferedWindow::show();
  if (!first) return;
  XClassHint hint{const_cast<char *>("liria"), const_cast<char *>("Liria")};
  XSetClassHint(fl_display, fl_xid(this), &hint);
  if (csd_) {
    // No frame of the window manager: Liria draws its own title bar.
    struct {
      unsigned long flags, functions, decorations;
      long input_mode;
      unsigned long status;
    } hints = {2, 0, 0, 0, 0};
    Atom motif = x11::atom("_MOTIF_WM_HINTS");
    XChangeProperty(fl_display, fl_xid(this), motif, motif, 32, PropModeReplace,
                    reinterpret_cast<unsigned char *>(&hints), 5);
    // FLTK keeps borderless windows (popups) out of the taskbar with
    // _NET_WM_STATE_SKIP_TASKBAR, set before the window is mapped: Liria
    // is an application window and belongs there (and in Alt+Tab). Once
    // mapped, the window manager owns the state: ask it (EWMH).
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.window = fl_xid(this);
    e.xclient.message_type = x11::atom("_NET_WM_STATE");
    e.xclient.format = 32;
    e.xclient.data.l[0] = 0;  // _NET_WM_STATE_REMOVE
    e.xclient.data.l[1] = long(x11::atom("_NET_WM_STATE_SKIP_TASKBAR"));
    e.xclient.data.l[3] = 1;  // an application
    XSendEvent(fl_display, RootWindow(fl_display, fl_screen), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(fl_display);
  }
  setup_dnd();
  apply_translucency();
  App::get().last_active = this;
}

void MainWindow::hide() {
  save_geometry();
  BufferedWindow::hide();
  if (xdnd_) xdnd_.reset();
  App::get().window_closed(this);
}

void MainWindow::save_geometry() {
  if (!shown()) return;
  Prefs &pr = App::get().prefs;
  bool maxed = maximized();
  pr.maximized = maxed;
  if (!maxed && !fullscreen_active()) {
    double sc = ui_scale() > 0 ? ui_scale() : 1.0;
    pr.width = int(w() / sc);
    pr.height = int(h() / sc);
  }
  pr.save();
}

void MainWindow::apply_translucency() {
  if (!shown()) return;
  const Theme &t = active_theme();
  ::Window xid = fl_xid(this);
  if (t.liquid) {
    // SDE Liquid Glass: the chrome (drawn in the surface color) is glass,
    // the files stay on their opaque card.
    x11::set_surface(xid, "window");
    x11::set_opacity(xid, t.opacity);
  } else {
    XDeleteProperty(fl_display, xid, x11::atom("_SDE_SURFACE"));
    // Translucent themes: the window follows the transparency of the
    // window frames (only over a blur; frame_opacity is 1 otherwise).
    x11::set_opacity(xid, t.translucent ? t.frame_opacity : 1.0);
  }
}

void MainWindow::retheme() {
  update_look();
  color(look().toolbar.fl());
  icon_cache_.clear();
  type_cache_.clear();
  apply_translucency();
  layout();
  redraw();
}

// FLTK follows _NET_WM_STATE: no round trip to the X server.
bool MainWindow::maximized() const { return maximize_active() != 0; }

void MainWindow::toggle_maximize() {
  if (!shown()) return;
  if (maximize_active()) un_maximize();
  else maximize();
}

void MainWindow::minimize() {
  if (shown()) x11::minimize_window(fl_xid(this));
}

void MainWindow::start_move(int button) { start_resize(8, button); }

void MainWindow::start_resize(int edge, int button) {
  if (!shown()) return;
  // The window manager moves or resizes the window (_NET_WM_MOVERESIZE).
  XUngrabPointer(fl_display, CurrentTime);
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = fl_xid(this);
  e.xclient.message_type = x11::atom("_NET_WM_MOVERESIZE");
  e.xclient.format = 32;
  e.xclient.data.l[0] = Fl::event_x_root();
  e.xclient.data.l[1] = Fl::event_y_root();
  e.xclient.data.l[2] = edge;
  e.xclient.data.l[3] = button;
  e.xclient.data.l[4] = 1;
  XSendEvent(fl_display, RootWindow(fl_display, fl_screen), False, SubstructureRedirectMask | SubstructureNotifyMask,
             &e);
  XFlush(fl_display);
}

int MainWindow::edge_at(int mx, int my) const {
  if (!csd_ || const_cast<MainWindow *>(this)->maximized() || fullscreen_active()) return -1;
  int b = std::max(4, S(6)), c = S(16);
  bool left = mx < b, right = mx >= w() - b, top = my < b, bottom = my >= h() - b;
  if (top && mx < c) return 0;
  if (top && mx >= w() - c) return 2;
  if (bottom && mx >= w() - c) return 4;
  if (bottom && mx < c) return 6;
  if (top) return 1;
  if (right) return 3;
  if (bottom) return 5;
  if (left) return 7;
  return -1;
}

// ------------------------------------------------------------------ layout

void MainWindow::layout() {
  const Theme &t = active_theme();
  App &app = App::get();
  int W = w(), H = h();
  int tab_h = S(42);
  tabbar_->resize(0, 0, W, tab_h);
  tabbar_->relayout();
  int nav_y = tab_h, nav_h = S(50);
  int bs = S(36), by = nav_y + (nav_h - bs) / 2;
  int bx = S(8);
  for (NavButton *b : {back_, forward_, up_, refresh_}) {
    b->resize(bx, by, bs, bs);
    bx += bs + S(4);
  }
  int sw = std::max(S(160), std::min(S(320), W / 4));
  int fh = S(34), fy = nav_y + (nav_h - fh) / 2;
  int ax = bx + S(6);
  searchbox_->resize(W - S(10) - sw, fy, sw, fh);
  addressbar_->resize(ax, fy, std::max(S(120), W - S(10) - sw - S(8) - ax), fh);
  int cmd_y = nav_y + nav_h, cmd_h = S(46);
  commandbar_->resize(0, cmd_y, W, cmd_h);
  int body_y = cmd_y + cmd_h + S(6), status_h = S(30);
  status_y_ = H - status_h;
  int side_w = app.prefs.sidebar_shown && W >= S(560) ? std::min(S(app.prefs.sidebar_width), W / 2) : 0;
  if (side_w) {
    sidebar_->show();
    sidebar_->resize(0, body_y, side_w, status_y_ - body_y);
  } else {
    sidebar_->hide();
  }
  card_x_ = side_w ? side_w + S(4) : S(8);
  card_y_ = body_y;
  card_w_ = W - card_x_ - S(8);
  card_h_ = status_y_ - body_y;
  int in = 1;
  homeview_->resize(card_x_ + in, card_y_ + in, card_w_ - 2 * in, card_h_ - 2 * in);
  fileview_->resize(card_x_ + in, card_y_ + in, card_w_ - 2 * in, card_h_ - 2 * in);
  bool home = tab().location == kHome;
  if (home) {
    homeview_->show();
    fileview_->hide();
  } else {
    fileview_->show();
    homeview_->hide();
  }
  fileview_->sync();
  homeview_->relayout();
  if (addressbar_->list->visible()) addressbar_->list->place();
  view_btn_w_ = S(30);
  view_icons_x_ = W - S(10) - view_btn_w_;
  view_details_x_ = view_icons_x_ - view_btn_w_ - S(2);
  commandbar_->rebuild();
  schedule_qa();
  (void)t;
}

void MainWindow::resize(int X, int Y, int W, int H) {
  bool sized = W != w() || H != h();
  BufferedWindow::resize(X, Y, W, H);
  if (sized) {
    layout();
    schedule_thumbnails();
  }
}

// ------------------------------------------------------------------ drawing

void MainWindow::draw() {
  if (first_frame_ms < 0) {
    first_frame_ms = now_ms() - opened_at;
    if (getenv("LIRIA_TIMING")) fprintf(stderr, "liria: first frame after %.1f ms\n", first_frame_ms);
    schedule_qa();
  }
  const Look &l = look();
  const Theme &t = active_theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), l.toolbar);
  }
  Fl_Widget *list = addressbar_->list;
  for (int i = 0; i < children(); ++i) {
    Fl_Widget *c = child(i);
    if (c == list) continue;
    draw_child(*c);
  }
  {
    // The card's rounded corners and its edge, over the views.
    Painter p(w(), h());
    cairo_t *cr = p.cr();
    cairo_new_path(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, card_x_, card_y_, card_w_, card_h_);
    p.rounded_rect(card_x_, card_y_, card_w_, card_h_, t.radius);
    p.set(l.toolbar);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    p.stroke_rounded(card_x_ + 0.5, card_y_ + 0.5, card_w_ - 1, card_h_ - 1, t.radius, l.card_border);
    // The status bar's view buttons.
    Layout lay = tab().layout;
    for (int b = 0; b < 2; ++b) {
      int bx = b == 0 ? view_details_x_ : view_icons_x_;
      bool on = tab().is_folder() || tab().location == kTrash || tab().location == kNetwork
                    ? (b == 0 ? lay == Layout::Details : lay == Layout::Icons)
                    : false;
      int byy = status_y_ + (h() - status_y_ - S(24)) / 2;
      if (on) p.fill_rounded(bx, byy, view_btn_w_, S(24), Sd(5), l.select);
      else if (status_hover_ == b) p.fill_rounded(bx, byy, view_btn_w_, S(24), Sd(5), l.hover);
      liria::glyph(p, b == 0 ? "details" : "grid", bx + (view_btn_w_ - S(16)) / 2.0, byy + S(4), S(16),
                   on ? l.accent_text : l.dim);
    }
    if (csd_ && !maximized() && !fullscreen_active())
      p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, 0, t.border.mix(l.chrome, 0.2));
  }
  // Status text.
  text_font(t.font_size - 1);
  std::string left = message_.empty() ? status_text() : message_;
  draw_text(left, card_x_ + S(8), status_y_, view_details_x_ - card_x_ - S(16), h() - status_y_,
            !message_.empty() && message_error_ ? t.error : l.dim);
  if (list->visible()) draw_child(*list);
}

std::string MainWindow::status_text() const {
  const Tab &t = const_cast<MainWindow *>(this)->tab();
  if (t.location == kHome) return "";
  if (t.loading && t.model.count() == 0) return _("Loading…");
  if (!t.error.empty()) return t.error;
  std::string s = format(ngettext("%s item", "%s items", t.model.count()), thousands(t.model.count()).c_str());
  if (t.searching) s = format(_("Searching… %s"), s.c_str());
  std::string sel = selection_text();
  if (!sel.empty()) s += "   |   " + sel;
  if (t.free_bytes >= 0 && sel.empty() && is_local(t.location))
    s += "   |   " + format(_("%s free"), chooser::size_text(t.free_bytes).c_str());
  return s;
}

std::string MainWindow::selection_text() const {
  const Tab &t = const_cast<MainWindow *>(this)->tab();
  size_t n = t.model.selected().size();
  if (n == 0) return "";
  long files = 0, folders = 0;
  long long bytes = 0;
  t.model.selection_size(files, folders, bytes);
  std::string s = format(ngettext("%d item selected", "%d items selected", int(n)), int(n));
  if (files > 0 && bytes > 0) s += "  " + chooser::size_text(bytes);
  return s;
}

void MainWindow::update_title() {
  std::string title = tab().title();
  copy_label(title.c_str());
  tabbar_->redraw();
}

void MainWindow::update_commands() {
  Tab &t = tab();
  back_->activate();
  if (!t.history.can_back()) back_->deactivate();
  forward_->activate();
  if (!t.history.can_forward()) forward_->deactivate();
  up_->activate();
  if (parent_location(t.location).empty()) up_->deactivate();
  searchbox_->placeholder = t.is_folder() || t.location == kTrash
                                ? format(_("Search %s"), location_title(t.location).c_str())
                                : std::string(_("Search"));
  commandbar_->rebuild();
  back_->redraw();
  forward_->redraw();
  up_->redraw();
  searchbox_->redraw();
  damage_rect(0, status_y_, w(), h() - status_y_);
  schedule_qa();
}

void MainWindow::say(const std::string &text, bool error) {
  message_ = text;
  message_error_ = error;
  damage_rect(0, status_y_, w(), h() - status_y_);
  Fl::remove_timeout(clear_message_cb, this);
  Fl::add_timeout(error ? 8.0 : 4.0, clear_message_cb, this);
  schedule_qa();
}

void MainWindow::clear_message_cb(void *d) {
  auto *self = static_cast<MainWindow *>(d);
  self->message_.clear();
  self->damage_rect(0, self->status_y_, self->w(), self->h() - self->status_y_);
}

// ------------------------------------------------------------------ tabs

void MainWindow::new_tab(const std::string &location, bool activate, const std::string &select) {
  auto t = std::make_unique<Tab>();
  t->location = normalize_location(location);
  if (t->location.empty()) t->location = kHome;
  t->history.visit(t->location);
  t->select_when_loaded = select;
  // A tab opened in the background goes right of the current one and of
  // the tabs opened from it before (as in web browsers): locations given
  // together keep their order.
  int at = int(tabs_.size());
  if (!activate) {
    at = current_ + 1;
    if (bg_next_ > at && bg_next_ <= int(tabs_.size())) at = bg_next_;
    bg_next_ = at + 1;
  }
  tabs_.insert(tabs_.begin() + at, std::move(t));
  if (!activate && at <= current_) ++current_;
  if (activate) {
    current_ = at;
    std::string loc = tabs_[size_t(at)]->location;
    tabs_[size_t(at)]->location.clear();
    tabs_[size_t(at)]->history = chooser::History();
    go(loc, true, select);
  } else {
    tabbar_->redraw();
  }
  schedule_qa();
}

void MainWindow::close_tab(int i) {
  if (i < 0 || i >= int(tabs_.size())) return;
  if (tabs_.size() == 1) {
    do_callback();  // the last tab: the window closes
    return;
  }
  bool was_current = i == current_;
  if (fileview_->editing.size()) fileview_->finish_rename(false);
  bg_next_ = -1;
  tabs_.erase(tabs_.begin() + i);
  if (current_ >= i && current_ > 0) --current_;
  if (current_ >= int(tabs_.size())) current_ = int(tabs_.size()) - 1;
  if (was_current) select_tab(current_);
  else tabbar_->redraw();
  App::get().unwatch_unused();
  schedule_qa();
}

void MainWindow::select_tab(int i) {
  if (i < 0 || i >= int(tabs_.size())) return;
  if (i != current_) bg_next_ = -1;
  if (fileview_->editing.size()) fileview_->finish_rename(true);
  addressbar_->stop_edit();
  if (i != current_ && tab().loading && is_local(tab().location)) {
    // The window reads one folder at a time: this one is read again when shown.
    reader_->cancel();
    tab().loading = false;
    tab().model.reset();
  }
  current_ = i;
  Tab &t = tab();
  // A tab opened in the background is read when it is first shown.
  bool fresh = t.location.empty() || (t.is_folder() && t.model.total() == 0 && !t.loading && t.error.empty());
  if (fresh) {
    std::string loc = t.location.empty() ? kHome : t.location;
    t.location.clear();
    t.history = chooser::History();
    go(loc, true, t.select_when_loaded);
    return;
  }
  searchbox_->value(t.query.c_str());
  layout();
  fileview_->scroll_to_y(t.scroll);
  addressbar_->rebuild();
  update_title();
  update_commands();
  if (t.location == kHome) homeview_->rebuild();
  sidebar_->redraw();
  schedule_thumbnails();
  redraw();
}

void MainWindow::move_tab(int from, int to) {
  if (from < 0 || to < 0 || from >= int(tabs_.size()) || to >= int(tabs_.size()) || from == to) return;
  auto t = std::move(tabs_[size_t(from)]);
  tabs_.erase(tabs_.begin() + from);
  tabs_.insert(tabs_.begin() + to, std::move(t));
  if (current_ == from) current_ = to;
  else if (from < current_ && to >= current_) --current_;
  else if (from > current_ && to <= current_) ++current_;
  tabbar_->redraw();
  schedule_qa();
}

void MainWindow::duplicate_tab(int i) {
  if (i < 0 || i >= int(tabs_.size())) return;
  std::string loc = tabs_[size_t(i)]->location;
  current_ = i;
  new_tab(loc, true);
}

// ------------------------------------------------------------------ going places

void MainWindow::go(const std::string &loc_in, bool record, const std::string &select) {
  std::string loc = normalize_location(loc_in);
  if (loc.empty()) {
    say(format(_("“%s” is not a place Liria can open."), loc_in.c_str()), true);
    return;
  }
  if (is_remote(loc)) {
    std::string local = local_path_of_uri(loc);
    if (!local.empty() && dir_exists(local)) loc = local;
  }
  if (is_local(loc) && !dir_exists(loc)) {
    struct stat st {};
    if (stat(loc.c_str(), &st) == 0) {
      show_item(loc);
      return;
    }
    say(format(_("“%s” cannot be found."), display_path(loc).c_str()), true);
    return;
  }
  if (fileview_->editing.size()) fileview_->finish_rename(true);
  addressbar_->stop_edit();
  Tab &t = tab();
  remember_view();
  if (record) t.history.visit(loc);
  t.location = loc;
  t.query.clear();
  t.folder_items.clear();
  t.searching = false;
  searcher_->cancel();
  Fl::remove_timeout(deep_search_cb, this);
  searchbox_->value("");
  t.model.reset();
  t.model.set_query("");
  t.error.clear();
  t.select_when_loaded = select;
  t.also_select.clear();
  t.scroll = 0;
  t.free_bytes = -1;
  t.opened_at = now_ms();
  t.loaded_ms = -1;
  App &app = App::get();
  FolderView v = app.prefs.view_of(is_local(loc) ? loc : std::string());
  t.layout = v.layout;
  t.icon_size = v.icon_size ? v.icon_size : app.prefs.icon_size;
  t.model.order = v.sort;
  t.model.show_hidden = app.prefs.show_hidden;
  if (loc == kTrash && !app.prefs.has_view(loc)) t.layout = Layout::Details;
  if (thumbs_.size() > 1500) {
    for (auto &kv : thumbs_)
      if (kv.second.surface) cairo_surface_destroy(kv.second.surface);
    thumbs_.clear();
  }
  fileview_->reset_hover();
  start_listing(t);
  if (is_local(loc)) {
    app.watch(loc);
    struct statvfs vfs {};
    if (statvfs(loc.c_str(), &vfs) == 0) t.free_bytes = (long long)vfs.f_bavail * (long long)vfs.f_frsize;
  }
  app.unwatch_unused();
  layout();
  fileview_->scroll_to_y(0);
  homeview_->scroll_to(0, 0);
  if (loc == kHome) homeview_->rebuild();
  addressbar_->rebuild();
  update_title();
  update_commands();
  sidebar_->redraw();
  redraw();
  if (loc != kHome) fileview_->canvas_focus();
  else homeview_->focus_canvas();
}

void MainWindow::go_typed(const std::string &text) {
  Address a = parse_address(text);
  App &app = App::get();
  switch (a.kind) {
    case Address::Empty:
      return;
    case Address::Error:
      say(a.message, true);
      addressbar_->start_edit(text);
      addressbar_->error = a.message;
      addressbar_->list->place();
      return;
    case Address::Path: {
      std::string cwd = is_local(tab().location) ? tab().location : home_dir();
      std::string p = chooser::expand_location(a.uri, cwd);
      if (!p.empty() && desk::path_exists(p)) {
        go(p);
        return;
      }
      say(format(_("“%s” cannot be found."), text.c_str()), true);
      addressbar_->start_edit(text);
      return;
    }
    case Address::Host:
      // A plain name that is also an item of this folder opens that item.
      if (a.ambiguous && is_local(tab().location)) {
        std::string p = tab().location + (tab().location == "/" ? "" : "/") + text;
        if (desk::path_exists(p)) {
          go(p);
          return;
        }
      }
      open_network_location(a.uri);
      return;
    case Address::Location:
      if (a.scheme == "liria" || a.scheme == "trash" || a.scheme == "network" || a.scheme == "file") {
        go(a.uri);
        return;
      }
      if (is_network_scheme(a.scheme) || is_remote(a.uri)) {
        open_network_location(a.uri);
        return;
      }
      go(a.uri);
      return;
  }
  (void)app;
}

void MainWindow::open_network_location(const std::string &uri_in, bool new_tab_) {
  std::string uri = uri_in;
  std::string scheme = lower(uri.substr(0, uri.find(':')));
  if (!Drives::scheme_supported(scheme) && scheme != "file") {
    say(format(_("This computer cannot open %s locations: they need the package %s. Ask your administrator to "
                 "install it."),
               scheme.c_str(), Drives::package_for(scheme).c_str()),
        true);
    return;
  }
  Address a = parse_address(uri);
  if (!a.password.empty()) uri = a.uri;  // never kept in the address
  say(format(_("Connecting to %s…"), a.host.empty() ? uri.c_str() : a.host.c_str()));
  MainWindow *self = this;
  App::get().drives.mount_location(
      uri, mount_questions(this, a.host.empty() ? uri : a.host),
      [self, uri, new_tab_](bool ok, const std::string &error, const std::string &path) {
        auto &ws = App::get().windows;
        if (std::find(ws.begin(), ws.end(), self) == ws.end()) return;
        if (!ok) {
          if (!error.empty()) self->say(error, true);
          else self->say(_("Cancelled."));
          return;
        }
        App::get().servers.remember(uri);
        App::get().places_changed();
        std::string target = !path.empty() && dir_exists(path) ? path : uri;
        self->message_.clear();
        if (new_tab_) self->new_tab(target, true);
        else self->go(target);
      });
}

void MainWindow::back() {
  Tab &t = tab();
  if (!t.history.can_back()) return;
  std::string from = t.location;
  std::string loc = t.history.back();
  if (loc.empty()) return;
  std::string select;
  if (is_local(loc) && starts_with(from, loc == "/" ? "/" : loc + "/")) {
    size_t start = loc == "/" ? 1 : loc.size() + 1;
    size_t slash = from.find('/', start);
    select = slash == std::string::npos ? from : from.substr(0, slash);
  }
  go(loc, false, select);
}

void MainWindow::forward() {
  Tab &t = tab();
  if (!t.history.can_forward()) return;
  std::string loc = t.history.forward();
  if (!loc.empty()) go(loc, false);
}

void MainWindow::up() {
  std::string from = tab().location;
  std::string up = parent_location(from);
  if (up.empty()) return;
  go(up, true, from);
}

void MainWindow::refresh() {
  Tab &t = tab();
  if (t.location == kHome) {
    App::get().drives.refresh();
    homeview_->rebuild();
    return;
  }
  std::string keep = t.model.focus;
  std::set<std::string> sel = t.model.selected();
  int scroll = fileview_->scroll_y();
  std::string q = t.query;
  go(t.location, false, keep);
  t.also_select = sel;
  fileview_->scroll_to_y(scroll);
  if (!q.empty()) {
    searchbox_->value(q.c_str());
    set_query(q);
  }
}

void MainWindow::show_item(const std::string &path) {
  std::string dir = chooser::parent_dir(path);
  if (dir.empty()) return;
  if (tab().location == dir) {
    Tab &t = tab();
    if (t.model.index(path) >= 0) {
      t.model.select_only(path);
      fileview_->ensure_visible(t.model.index(path));
      selection_changed();
      return;
    }
    t.select_when_loaded = path;
    return;
  }
  go(dir, true, path);
}

void MainWindow::remember_view() {
  // The view of a folder is remembered only when the user changed it
  // (set_layout(), sort_by() ...).
}

// ------------------------------------------------------------------ loading

void MainWindow::start_listing(Tab &t) {
  t.loading = false;
  if (t.location == kHome) return;
  if (t.location == kTrash) {
    list_trash(t);
    return;
  }
  if (t.location == kNetwork) {
    // The network page: mounted shares and recent servers.
    App &app = App::get();
    std::vector<Item> items;
    std::set<std::string> seen;
    for (const auto &d : app.drives.network()) {
      Item it;
      it.name = d.name;
      it.display = d.name;
      it.path = d.path.empty() ? d.uri : d.path;
      it.dir = true;
      it.mime = "inode/directory";
      it.remote = d.path.empty();
      it.fold = chooser::fold_text(d.name);
      gchar *key = g_utf8_collate_key_for_filename(it.display.c_str(), -1);
      it.key = key ? key : it.display;
      g_free(key);
      it.folder = d.uri;
      seen.insert(server_root(d.uri));
      items.push_back(std::move(it));
    }
    for (const auto &s : app.servers.items()) {
      if (seen.count(s)) continue;
      Item it;
      it.name = s;
      it.display = uri_basename(s);
      it.path = s;
      it.dir = true;
      it.remote = true;
      it.mime = "inode/directory";
      it.fold = chooser::fold_text(it.display);
      it.key = it.fold;
      it.folder = s;
      items.push_back(std::move(it));
    }
    t.model.add(std::move(items));
    t.model.refresh();
    return;
  }
  t.loading = true;
  if (is_remote(t.location)) {
    static uint64_t serial = 1u << 30;
    t.generation = ++serial;
    list_remote(t, t.generation);
    return;
  }
  if (&t == &tab()) t.generation = reader_->start(t.location);
  else t.loading = false;  // read when shown
}

void MainWindow::list_trash(Tab &t) {
  std::vector<Item> items;
  for (const auto &ti : liria::list_trash()) {
    chooser::Entry e;
    if (!chooser::entry_for_path(ti.files_path(), e)) continue;
    Item it = item_from(std::move(e));
    it.display = ti.original.empty() ? ti.name : basename_of(ti.original);
    gchar *d = g_filename_display_name(it.display.c_str());
    if (d) {
      it.display = d;
      g_free(d);
    }
    it.fold = chooser::fold_text(it.display);
    gchar *key = g_utf8_collate_key_for_filename(it.display.c_str(), -1);
    it.key = key ? key : it.display;
    g_free(key);
    it.original = ti.original;
    it.folder = ti.original.empty() ? std::string() : chooser::parent_dir(ti.original);
    it.mtime = ti.deleted;
    it.hidden = false;
    items.push_back(std::move(it));
  }
  t.model.add(std::move(items));
  t.model.refresh();
}

struct RemoteListing {
  std::atomic<bool> done{false};
  std::vector<Item> items;
  std::string error;
};

struct RemotePoll {
  std::shared_ptr<RemoteListing> job;
  MainWindow *w;
  uint64_t generation;
  std::string uri;
};

void MainWindow::list_remote(Tab &t, uint64_t generation) {
  auto job = std::make_shared<RemoteListing>();
  std::string uri = t.location;
  MainWindow *self = this;
  std::thread([job, uri] {
    GFile *dir = g_file_new_for_uri(uri.c_str());
    GError *err = nullptr;
    GFileEnumerator *en = g_file_enumerate_children(
        dir, "standard::name,standard::display-name,standard::type,standard::size,standard::content-type,"
             "standard::fast-content-type,standard::is-hidden,standard::is-symlink,time::modified",
        G_FILE_QUERY_INFO_NONE, nullptr, &err);
    if (!en) {
      job->error = err && err->message ? err->message : "error";
      if (err) g_error_free(err);
    } else {
      while (GFileInfo *info = g_file_enumerator_next_file(en, nullptr, nullptr)) {
        Item it;
        it.name = g_file_info_get_name(info) ? g_file_info_get_name(info) : "";
        it.display = g_file_info_get_display_name(info) ? g_file_info_get_display_name(info) : it.name;
        GFile *child = g_file_get_child(dir, it.name.c_str());
        gchar *cu = g_file_get_uri(child);
        it.path = cu ? cu : uri + it.name;
        g_free(cu);
        g_object_unref(child);
        it.dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        it.remote = true;
        if (it.dir) it.path += it.path.empty() || it.path.back() == '/' ? "" : "/";
        it.size = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ? g_file_info_get_size(info) : 0;
        const char *ct = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE)
                             ? g_file_info_get_content_type(info)
                             : nullptr;
        it.mime = it.dir ? "inode/directory" : ct ? ct : chooser::guess_mime(it.name, false);
        it.hidden = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) && g_file_info_get_is_hidden(info);
        it.link = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) && g_file_info_get_is_symlink(info);
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
          it.mtime = time_t(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED));
        it.fold = chooser::fold_text(it.display);
        gchar *folded = g_utf8_casefold(it.display.c_str(), -1);
        gchar *key = g_utf8_collate_key_for_filename(folded ? folded : it.display.c_str(), -1);
        it.key = key ? key : it.display;
        g_free(key);
        g_free(folded);
        job->items.push_back(std::move(it));
        g_object_unref(info);
      }
      g_object_unref(en);
    }
    g_object_unref(dir);
    job->done = true;
  }).detach();
  auto *poll = new RemotePoll{job, self, generation, uri};
  Fl::add_timeout(0.05, remote_poll_cb, poll);
}

void MainWindow::remote_poll_cb(void *d) {
  auto *p = static_cast<RemotePoll *>(d);
  auto &ws = App::get().windows;
  if (std::find(ws.begin(), ws.end(), p->w) == ws.end()) {
    delete p;
    return;
  }
  if (!p->job->done) {
    Fl::repeat_timeout(0.05, remote_poll_cb, d);
    return;
  }
  for (auto &t : p->w->tabs_)
    if (t->location == p->uri && t->generation == p->generation) {
      std::string error = p->job->error;
      p->w->apply_items(*t, std::move(p->job->items), true, error);
    }
  delete p;
}

void MainWindow::apply_items(Tab &t, std::vector<Item> &&items, bool done, const std::string &error) {
  t.model.add(std::move(items));
  if (done) {
    t.loading = false;
    t.error = error;
  }
  bool sel_changed = t.model.refresh();
  if (!t.select_when_loaded.empty()) {
    int k = t.model.index(t.select_when_loaded);
    if (k >= 0) {
      t.model.select_only(t.select_when_loaded);
      for (const auto &p : t.also_select)
        if (t.model.index(p) >= 0 && !t.model.is_selected(p)) t.model.toggle(p);
      t.model.focus = t.model.anchor = t.select_when_loaded;
      if (&t == &tab()) {
        fileview_->sync();
        fileview_->ensure_visible(k);
      }
      t.select_when_loaded.clear();
      t.also_select.clear();
      sel_changed = true;
    } else if (done) {
      t.select_when_loaded.clear();
      t.also_select.clear();
    }
  }
  if (&t == &tab()) {
    fileview_->sync();
    if (sel_changed) selection_changed();
    else update_commands();
    schedule_thumbnails();
  }
  if (done) after_load(t);
}

void MainWindow::after_load(Tab &t) {
  if (t.loaded_ms < 0) {
    t.loaded_ms = now_ms() - t.opened_at;
    if (getenv("LIRIA_TIMING"))
      fprintf(stderr, "liria: %s: %zu items in %.1f ms\n", t.location.c_str(), t.model.total(), t.loaded_ms);
  }
  schedule_qa();
}

void MainWindow::pump() {
  Tab &t = tab();
  chooser::FolderReader::Batch b;
  std::vector<Item> items;
  bool done = false;
  std::string error;
  bool any = false;
  while (reader_->take(b)) {
    if (b.generation != t.generation) continue;
    for (auto &e : b.entries) items.push_back(item_from(std::move(e)));
    if (b.done) {
      done = true;
      error = b.error;
    }
    any = true;
  }
  if (any) apply_items(t, std::move(items), done, error);
  items.clear();
  bool found_any = false, found_done = false;
  while (searcher_->take(b)) {
    if (b.generation != t.search_generation) continue;
    for (auto &e : b.entries) {
      if (e.folder == t.location) continue;  // already listed
      items.push_back(item_from(std::move(e)));
    }
    if (b.done) found_done = true;
    found_any = true;
  }
  if (found_any) {
    if (found_done) t.searching = false;
    apply_items(t, std::move(items), false, "");
    if (found_done) update_commands();
  }
  bool view_dirty = false;
  for (auto &r : thumbnailer_->take()) {
    std::string key = std::to_string(r.job.size) + "\n" + r.job.path;
    Thumb &th = thumbs_[key];
    if (th.surface) cairo_surface_destroy(th.surface);
    th.surface = r.surface;
    th.w = r.width;
    th.h = r.height;
    view_dirty = true;
  }
  if (view_dirty) fileview_->scroll->redraw();
}

// ------------------------------------------------------------------ search

void MainWindow::set_query(const std::string &q) {
  Tab &t = tab();
  if (!t.is_folder() && t.location != kTrash) {
    if (!q.empty() && t.location == kHome) {
      // Searching from Home searches the home folder.
      std::string query = q;
      go(home_dir());
      searchbox_->value(query.c_str());
      searchbox_->insert_position(searchbox_->size());
      searchbox_->take_focus();
      set_query(query);
    }
    return;
  }
  if (q == t.query) return;
  if (t.query.empty() && !q.empty()) t.folder_items = t.model.items();
  t.query = q;
  searcher_->cancel();
  t.searching = false;
  Fl::remove_timeout(deep_search_cb, this);
  if (q.empty()) {
    t.model.reset();
    t.model.add(std::move(t.folder_items));
    t.folder_items.clear();
  }
  t.model.set_query(q);
  t.model.refresh();
  fileview_->scroll_to_y(0);
  fileview_->sync();
  addressbar_->rebuild();
  update_title();
  update_commands();
  if (chooser::fold_text(q).size() >= 2 && is_local(t.location)) Fl::add_timeout(0.35, deep_search_cb, this);
}

void MainWindow::start_deep_search() {
  Fl::remove_timeout(deep_search_cb, this);
  deep_search_cb(this);
}

void MainWindow::deep_search_cb(void *d) {
  auto *self = static_cast<MainWindow *>(d);
  Tab &t = self->tab();
  if (t.query.empty() || !is_local(t.location)) return;
  // The results of an earlier query go; the folder's own items stay.
  t.model.reset();
  std::vector<Item> copy = t.folder_items;
  t.model.add(std::move(copy));
  t.model.set_query(t.query);
  t.model.refresh();
  t.searching = true;
  t.search_generation = self->searcher_->start_search(t.location, t.query, App::get().prefs.show_hidden);
  self->fileview_->sync();
  self->update_commands();
}

// ------------------------------------------------------------------ thumbnails, icons

MainWindow::Thumb *MainWindow::thumbnail(const Item &e, int size) {
  auto it = thumbs_.find(std::to_string(size) + "\n" + e.path);
  return it == thumbs_.end() ? nullptr : &it->second;
}

void MainWindow::schedule_thumbnails() {
  Fl::remove_timeout(thumbnails_cb, this);
  Fl::add_timeout(0.03, thumbnails_cb, this);
}

void MainWindow::thumbnails_cb(void *d) { static_cast<MainWindow *>(d)->request_thumbnails(); }

void MainWindow::request_thumbnails() {
  Tab &t = tab();
  if (!t.is_folder() && t.location != kTrash) return;
  std::vector<chooser::Thumbnailer::Job> jobs;
  fileview_->metrics();
  int first, last;
  fileview_->visible_range(first, last);
  int n = t.model.count(), size = fileview_->thumb_size();
  auto want = [&](int k) {
    if (k < 0 || k >= n) return;
    const Item &e = t.model.at(k);
    if (e.dir || e.broken || e.remote || starts_with(e.mime, "inode/")) return;
    if (thumbs_.count(std::to_string(size) + "\n" + e.path)) return;
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
  thumbnailer_->request(std::move(jobs));
}

const std::vector<std::string> &MainWindow::icon_names(const Item &e) {
  std::string key = e.dir ? "dir:" + e.path : e.mime;
  auto it = icon_cache_.find(key);
  if (it != icon_cache_.end()) return it->second;
  std::vector<std::string> names;
  if (e.dir) {
    names = is_local(e.path) ? location_icons(e.path) : std::vector<std::string>{"folder-remote", "folder"};
    if (tab().location == kNetwork) names = {"folder-remote", "network-server", "folder"};
  } else {
    names = chooser::mime_icon_names(e.mime);
  }
  if (icon_cache_.size() > 4000) icon_cache_.clear();
  return icon_cache_.emplace(key, names).first->second;
}

const std::string &MainWindow::type_text(const std::string &mime) {
  auto it = type_cache_.find(mime);
  if (it == type_cache_.end()) it = type_cache_.emplace(mime, chooser::mime_description(mime)).first;
  return it->second;
}

// ------------------------------------------------------------------ changes on disk

void MainWindow::folder_changed(const std::string &dir, const std::vector<std::string> &names) {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    Tab &t = *tabs_[i];
    if (t.location != dir) continue;
    if (dir == kTrash) {
      t.model.reset();
      list_trash(t);
      if (int(i) == current_) {
        fileview_->sync();
        update_commands();
      }
      continue;
    }
    if (!is_local(dir)) continue;
    if (names.empty()) {
      // Everything again, keeping the selection and the place.
      if (int(i) == current_) {
        std::set<std::string> sel = t.model.selected();
        std::string focus = t.model.focus;
        int scroll = fileview_->scroll_y();
        std::vector<chooser::Entry> entries;
        std::string error;
        if (chooser::list_directory(dir, entries, &error)) {
          std::set<std::string> hidden = chooser::hidden_names(dir);
          std::vector<Item> items;
          for (auto &e : entries) {
            if (hidden.count(e.name)) e.hidden = true;
            items.push_back(item_from(std::move(e)));
          }
          std::string keep_select = t.select_when_loaded;
          std::set<std::string> keep_also = t.also_select;
          t.model.reset();
          t.model.add(std::move(items));
          t.model.select_set(sel);
          t.model.focus = focus;
          t.select_when_loaded = keep_select;
          t.also_select = keep_also;
          apply_items(t, {}, true, "");
          fileview_->scroll_to_y(scroll);
        }
      } else {
        t.model.reset();  // read again when shown
      }
      continue;
    }
    std::set<std::string> hidden = chooser::hidden_names(dir);
    for (const auto &name : names) {
      std::string path = dir == "/" ? "/" + name : dir + "/" + name;
      chooser::Entry e;
      if (chooser::entry_for_path(path, e)) {
        if (hidden.count(e.name)) e.hidden = true;
        t.model.upsert(item_from(std::move(e)));
        // A changed picture gets a new thumbnail.
        for (int size : {128, 256}) {
          auto it = thumbs_.find(std::to_string(size) + "\n" + path);
          if (it != thumbs_.end()) {
            if (it->second.surface) cairo_surface_destroy(it->second.surface);
            thumbs_.erase(it);
          }
        }
      } else {
        t.model.remove(path);
      }
    }
    if (int(i) == current_) {
      fileview_->forget_wraps();
      apply_items(t, {}, !t.loading, t.error);
    }
  }
}

void MainWindow::places_changed() {
  sidebar_->rebuild();
  if (tab().location == kHome) homeview_->rebuild();
  if (tab().location == kNetwork) {
    tab().model.reset();
    start_listing(tab());
    fileview_->sync();
  }
  addressbar_->rebuild();
  update_commands();
}

// ------------------------------------------------------------------ selection

std::vector<std::string> MainWindow::selected_paths() const {
  std::vector<std::string> out;
  for (const Item *e : const_cast<MainWindow *>(this)->tab().model.selection()) out.push_back(e->path);
  return out;
}

std::vector<Item> MainWindow::selected_items() const {
  std::vector<Item> out;
  for (const Item *e : const_cast<MainWindow *>(this)->tab().model.selection()) out.push_back(*e);
  return out;
}

void MainWindow::selection_changed() {
  fileview_->scroll->redraw();
  update_commands();
}

std::string MainWindow::folder_of_tab() const {
  const Tab &t = const_cast<MainWindow *>(this)->tab();
  return is_local(t.location) ? t.location : std::string();
}

bool MainWindow::can_write_here() const {
  std::string dir = folder_of_tab();
  return !dir.empty() && access(dir.c_str(), W_OK) == 0;
}

bool MainWindow::can_paste() const {
  FileClipboard *c = App::get().clipboard.get();
  return c && c->has_files();
}

// ------------------------------------------------------------------ opening

void MainWindow::open_item(const Item &e, bool in_new_tab) {
  App &app = App::get();
  if (tab().location == kTrash) {
    // Items in the Trash are not opened: their properties say where they came from.
    show_properties(this, {e});
    return;
  }
  if (e.dir) {
    if (e.remote) open_network_location(e.path, in_new_tab);
    else if (in_new_tab) new_tab(e.path, false);
    else go(e.path);
    return;
  }
  if (e.remote) {
    // A file on a server without a FUSE folder: its default application gets the URI.
    std::string id = app.apps.default_for(e.mime);
    if (!id.empty() && app.apps.launch(id, {e.path})) return;
    open_uri(e.path);
    return;
  }
  std::string path = e.path;
  const std::string &m = e.mime;
  if (m == "application/x-desktop") {
    DesktopEntry de;
    if (de.load(path) && (de.type == "Application" || de.type == "Link")) {
      if (!launch_entry(de, {})) say(format(_("“%s” could not be started."), e.display.c_str()), true);
      return;
    }
  }
  if (m == "application/vnd.appimage" || m == "application/x-iso9660-appimage") {
    spawn_detached({"sde-appimage", path});
    return;
  }
  std::string id = app.apps.default_for(m);
  if (!id.empty() && app.apps.launch(id, {path})) {
    chooser::add_recent(path, m, "liria");
    return;
  }
  // Nothing knows the type: choose an application.
  show_open_with(this, e);
}

void MainWindow::open_paths(const std::vector<std::string> &paths, bool new_tabs) {
  for (const auto &p : paths) {
    Item e;
    if (chooser::entry_for_path(p, e)) open_item(e, new_tabs);
  }
}

void MainWindow::open_selection(bool in_new_tab) {
  std::vector<Item> sel = selected_items();
  if (sel.empty()) return;
  int dirs = 0;
  for (const auto &e : sel) dirs += e.dir;
  if (sel.size() == 1) {
    open_item(sel[0], in_new_tab);
    return;
  }
  // Several folders open in tabs; files with their applications.
  for (const auto &e : sel) {
    if (e.dir) {
      if (dirs > 1 || in_new_tab) new_tab(e.path, false);
      else open_item(e, false);
    } else {
      open_item(e, false);
    }
  }
}

// ------------------------------------------------------------------ commands

void MainWindow::command(const std::string &id) {
  if (id == "cut") cut();
  else if (id == "copy") copy();
  else if (id == "paste") paste();
  else if (id == "rename") rename();
  else if (id == "delete") remove(tab().location == kTrash);
  else if (id == "restore") restore_selection();
  else if (id == "empty-trash") empty_trash();
  else if (id == "extract") extract_here();
  else if (id == "install") install_package();
  else if (id == "run-appimage") run_appimage();
  else if (id == "connect") connect_to_server();
  else if (id == "eject") {
    const Drive *d = App::get().drives.drive_of(tab().location);
    if (d) {
      std::string drive = d->id, path = d->path;
      go(kHome);
      App::get().drives.eject(drive, [this](bool ok, const std::string &error, const std::string &) {
        if (!ok && !error.empty()) say(error, true);
        else if (ok) say(_("The drive can be removed now."));
      });
    }
  } else if (id == "new") new_folder();
  else if (id == "share") copy_path();
}

void MainWindow::cut() {
  std::vector<std::string> paths = selected_paths();
  if (paths.empty() || tab().location == kTrash) return;
  App::get().clipboard->set(paths, true, fl_event_time);
  say(format(ngettext("%d item cut: paste it where it should go (Ctrl+V).",
                      "%d items cut: paste them where they should go (Ctrl+V).", int(paths.size())),
             int(paths.size())));
  update_commands();
}

void MainWindow::copy() {
  std::vector<std::string> paths = selected_paths();
  if (paths.empty()) return;
  App::get().clipboard->set(paths, false, fl_event_time);
  say(format(ngettext("%d item copied.", "%d items copied.", int(paths.size())), int(paths.size())));
  update_commands();
}

void MainWindow::paste() {
  std::string dir = folder_of_tab();
  if (dir.empty()) return;
  paste_into(dir);
}

void MainWindow::paste_into(const std::string &dir) {
  App &app = App::get();
  FileClipboard *clip = app.clipboard.get();
  if (clip->owned()) {
    std::vector<std::string> paths;
    for (const auto &p : clip->paths())
      if (is_local(p)) paths.push_back(p);
    app.transfer(paths, dir, clip->cut() ? Transfer::Move : Transfer::Copy, this);
    return;
  }
  if (!xdnd_) return;
  MainWindow *self = this;
  xdnd_->request_clipboard([self, dir](const std::string &uris, bool cut) {
    auto &ws = App::get().windows;
    if (std::find(ws.begin(), ws.end(), self) == ws.end()) return;
    std::vector<std::string> paths;
    for (auto &uri : desk::parse_uri_list(uris)) {
      std::string p = desk::path_from_uri(uri);
      if (!p.empty()) paths.push_back(p);
    }
    if (paths.empty()) {
      self->say(_("There are no files on the clipboard."));
      return;
    }
    App::get().transfer(paths, dir, cut ? Transfer::Move : Transfer::Copy, self);
  }, fl_event_time);
}

void MainWindow::rename() {
  std::vector<std::string> paths = selected_paths();
  if (paths.size() != 1 || tab().location == kTrash || tab().location == kHome) return;
  fileview_->start_rename(paths[0]);
}

void MainWindow::remove(bool permanently) {
  std::vector<std::string> paths = selected_paths();
  if (paths.empty()) return;
  App &app = App::get();
  if (tab().location == kTrash || permanently) app.delete_permanently(paths, this);
  else app.trash(paths, this);
}

void MainWindow::new_folder() {
  std::string dir = folder_of_tab();
  if (dir.empty() || !can_write_here()) return;
  std::string error;
  std::string path = create_folder(dir, "", &error);
  if (path.empty()) {
    say(error, true);
    return;
  }
  UndoStep step;
  step.kind = UndoStep::Create;
  step.pairs.push_back({path, ""});
  App::get().undo.push(step);
  Tab &t = tab();
  folder_changed(dir, {basename_of(path)});
  if (t.model.index(path) >= 0) {
    t.model.select_only(path);
    selection_changed();
    fileview_->start_rename(path);
  }
}

void MainWindow::new_document(const std::string &template_path) {
  std::string dir = folder_of_tab();
  if (dir.empty() || !can_write_here()) return;
  std::string error;
  std::string path = create_document(dir, "", template_path, &error);
  if (path.empty()) {
    say(error, true);
    return;
  }
  UndoStep step;
  step.kind = UndoStep::Create;
  step.pairs.push_back({path, ""});
  App::get().undo.push(step);
  Tab &t = tab();
  folder_changed(dir, {basename_of(path)});
  if (t.model.index(path) >= 0) {
    t.model.select_only(path);
    selection_changed();
    fileview_->start_rename(path);
  }
}

void MainWindow::select_all() {
  Tab &t = tab();
  if (t.location == kHome) return;
  t.model.select_all();
  selection_changed();
}

void MainWindow::select_none() {
  tab().model.clear_selection();
  selection_changed();
}

void MainWindow::invert_selection() {
  tab().model.invert();
  selection_changed();
}

void MainWindow::properties() {
  std::vector<Item> sel = selected_items();
  if (sel.empty()) {
    Tab &t = tab();
    Item self;
    if (is_local(t.location) && chooser::entry_for_path(t.location, self)) sel.push_back(self);
  }
  if (!sel.empty()) show_properties(this, sel);
}

void MainWindow::open_with_dialog() {
  std::vector<Item> sel = selected_items();
  if (sel.size() == 1 && !sel[0].dir) show_open_with(this, sel[0]);
}

void MainWindow::open_terminal_here() {
  std::string dir = folder_of_tab();
  std::vector<Item> sel = selected_items();
  if (sel.size() == 1 && sel[0].dir && is_local(sel[0].path)) dir = sel[0].path;
  if (dir.empty()) dir = home_dir();
  if (!sde::open_terminal({}, dir)) say(_("No terminal is installed."), true);
}

void MainWindow::copy_path() {
  std::vector<std::string> paths = selected_paths();
  if (paths.empty() && is_local(tab().location)) paths.push_back(tab().location);
  std::string text;
  for (size_t i = 0; i < paths.size(); ++i) text += (i ? "\n" : "") + paths[i];
  if (text.empty()) return;
  Fl::copy(text.c_str(), int(text.size()), 1);
  Fl::copy(text.c_str(), int(text.size()), 0);
  say(paths.size() == 1 ? format(_("Copied the path of “%s”."), basename_of(paths[0]).c_str())
                        : format(_("Copied %d paths."), int(paths.size())));
}

void MainWindow::email() {
  std::vector<std::string> paths = selected_paths();
  std::vector<std::string> files;
  for (const auto &p : paths)
    if (is_local(p) && !dir_exists(p)) files.push_back(p);
  if (files.empty()) {
    say(_("Folders cannot be attached to an email; compress them first."), true);
    return;
  }
  // xdg-email attaches the files in the user's mail program.
  std::vector<std::string> argv{"xdg-email"};
  for (const auto &f : files) {
    argv.push_back("--attach");
    argv.push_back(f);
  }
  if (!spawn_detached(argv)) say(_("No mail program is set up (xdg-email, package xdg-utils)."), true);
}

void MainWindow::empty_trash() { App::get().empty_trash(this); }

void MainWindow::restore_selection() {
  std::vector<TrashItem> items;
  for (const auto &p : selected_paths()) {
    TrashItem it;
    if (trash_item_of(p, it)) items.push_back(it);
  }
  App::get().restore(items, this);
}

void MainWindow::undo() { App::get().undo_last(this); }

void MainWindow::compress() {
  std::vector<std::string> paths = selected_paths();
  if (paths.empty()) return;
  std::vector<std::string> argv{"taca", "--add", "--default-dir=" + chooser::parent_dir(paths[0])};
  for (const auto &p : paths)
    if (is_local(p)) argv.push_back(p);
  if (!spawn_detached(argv, chooser::parent_dir(paths[0]))) say(_("Taca, the archive manager, is not installed."), true);
}

void MainWindow::extract_here() {
  for (const auto &p : selected_paths()) {
    if (!is_local(p)) continue;
    if (!spawn_detached({"taca", "--extract-here", p}, chooser::parent_dir(p))) {
      say(_("Taca, the archive manager, is not installed."), true);
      return;
    }
  }
}

void MainWindow::install_package() {
  std::vector<std::string> paths = selected_paths();
  if (paths.size() == 1 && !spawn_detached({"debrun", paths[0]}))
    say(_("Debrun, the package installer, is not installed."), true);
}

void MainWindow::run_appimage() {
  std::vector<std::string> paths = selected_paths();
  if (paths.size() == 1 && !spawn_detached({"sde-appimage", paths[0]}))
    say(_("sde-appimage is not installed."), true);
}

void MainWindow::set_layout(Layout l) {
  Tab &t = tab();
  t.layout = l;
  App &app = App::get();
  app.prefs.layout = l;
  if (is_local(t.location) || t.location == kTrash) {
    FolderView v = app.prefs.view_of(t.location);
    v.layout = l;
    v.sort = t.model.order;
    v.icon_size = t.icon_size;
    app.prefs.set_view(t.location, v);
  }
  app.prefs.save();
  fileview_->forget_wraps();
  layout();
  fileview_->ensure_visible(t.model.index(t.model.focus));
  schedule_thumbnails();
  redraw();
}

void MainWindow::set_icon_size(int px) {
  Tab &t = tab();
  if (t.layout != Layout::Icons) t.layout = Layout::Icons;
  t.icon_size = Prefs::clamp_icon_size(px);
  App &app = App::get();
  app.prefs.icon_size = t.icon_size;
  set_layout(Layout::Icons);
}

void MainWindow::zoom(int steps) {
  static const int sizes[] = {32, 48, 64, 72, 96, 128, 160, 192, 256};
  Tab &t = tab();
  if (t.layout == Layout::Details && steps > 0) {
    set_layout(Layout::Compact);
    return;
  }
  if (t.layout == Layout::Compact) {
    if (steps > 0) set_icon_size(48);
    else set_layout(Layout::Details);
    return;
  }
  int i = 0;
  while (i + 1 < int(sizeof sizes / sizeof sizes[0]) && sizes[i] < t.icon_size) ++i;
  int n = int(sizeof sizes / sizeof sizes[0]);
  int next = std::max(0, std::min(n - 1, i + steps));
  if (steps < 0 && t.icon_size <= sizes[0]) {
    set_layout(Layout::Compact);
    return;
  }
  set_icon_size(sizes[next]);
}

void MainWindow::sort_by(chooser::SortKey key) {
  Tab &t = tab();
  t.model.order.key = key;
  // Dates newest first, sizes largest first, as Windows does.
  t.model.order.descending = key == chooser::SortKey::Modified || key == chooser::SortKey::Size;
  App &app = App::get();
  app.prefs.sort = t.model.order;
  if (is_local(t.location)) {
    FolderView v = app.prefs.view_of(t.location);
    v.layout = t.layout;
    v.sort = t.model.order;
    app.prefs.set_view(t.location, v);
  }
  app.prefs.save();
  t.model.refresh();
  fileview_->sync();
  schedule_thumbnails();
  schedule_qa();
}

void MainWindow::set_sort_descending(bool descending) {
  Tab &t = tab();
  t.model.order.descending = descending;
  App &app = App::get();
  app.prefs.sort = t.model.order;
  if (is_local(t.location)) {
    FolderView v = app.prefs.view_of(t.location);
    v.layout = t.layout;
    v.sort = t.model.order;
    app.prefs.set_view(t.location, v);
  }
  app.prefs.save();
  t.model.refresh();
  fileview_->sync();
  schedule_thumbnails();
  schedule_qa();
}

void MainWindow::toggle_hidden() {
  App &app = App::get();
  app.prefs.show_hidden = !app.prefs.show_hidden;
  app.prefs.save();
  for (auto *w : app.windows) {
    for (auto &t : w->tabs_) {
      t->model.show_hidden = app.prefs.show_hidden;
      t->model.refresh();
    }
    w->fileview_->sync();
    w->update_commands();
  }
  say(app.prefs.show_hidden ? _("Hidden items are shown (Ctrl+H hides them).") : _("Hidden items are hidden."));
}

void MainWindow::toggle_sidebar() {
  App &app = App::get();
  app.prefs.sidebar_shown = !app.prefs.sidebar_shown;
  app.prefs.save();
  for (auto *w : app.windows) {
    w->layout();
    w->redraw();
  }
}

void MainWindow::toggle_fullscreen() {
  if (fullscreen_active()) fullscreen_off();
  else fullscreen();
}

void MainWindow::open_as_administrator(const std::string &path) {
  // sudo asks for the password in a terminal; Liria then runs with root's
  // own preferences on a private D-Bus session (main.cpp).
  std::string self = "/usr/bin/liria";
  if (!file_exists(self)) self = "liria";
  if (!sde::open_terminal({"sudo", self, "--no-desktop", path}, home_dir()))
    say(_("No terminal is installed."), true);
}

void MainWindow::about() {
  AppInfo info;
  info.name = _("Liria");
  info.program = "liria";
#ifdef SDE_APP_VERSION
  info.version = SDE_APP_VERSION;
#endif
  info.icon = "liria";
  info.icon_fallbacks = {"system-file-manager", "folder"};
  info.description = _("Browse, organize and share your files, drives and network places.");
  info.credits = {toolkit_credit()};
  show_about(info);
}

// ------------------------------------------------------------------ keys and events

bool MainWindow::shortcut(int key) {
  int st = Fl::event_state();
  bool ctrl = st & FL_CTRL, alt = st & FL_ALT, shift = st & FL_SHIFT;
  bool in_text = dynamic_cast<Fl_Input *>(Fl::focus()) != nullptr;
  if (alt && !ctrl) {
    switch (key) {
      case FL_Left: back(); return true;
      case FL_Right: forward(); return true;
      case FL_Up: up(); return true;
      case FL_Home: go(kHome); return true;
      case FL_Enter:
      case FL_KP_Enter: properties(); return true;
      case 'd': addressbar_->start_edit(); return true;
    }
    return false;
  }
  if (ctrl && !alt) {
    switch (key) {
      case 'n':
        if (shift) new_folder();
        else App::get().open_window({tab().location});
        return true;
      case 't': new_tab(kHome); return true;
      case 'w': close_tab(current_); return true;
      case FL_Tab:
      case FL_Iso_Key:
        if (tab_count() > 1) select_tab((current_ + (shift ? tab_count() - 1 : 1)) % tab_count());
        return true;
      case FL_Page_Down: if (tab_count() > 1) select_tab((current_ + 1) % tab_count()); return true;
      case FL_Page_Up: if (tab_count() > 1) select_tab((current_ + tab_count() - 1) % tab_count()); return true;
      case 'l': addressbar_->start_edit(); return true;
      case 'e':
      case 'f':
        searchbox_->take_focus();
        searchbox_->insert_position(searchbox_->size(), 0);
        return true;
      case 'h': toggle_hidden(); return true;
      case 'r': refresh(); return true;
      case 'z':
        if (in_text) return false;
        undo();
        return true;
      case '=':
      case '+':
      case FL_KP + '+': zoom(1); return true;
      case '-':
      case FL_KP + '-': zoom(-1); return true;
      case '0': set_icon_size(72); return true;
    }
    if (shift && key >= '1' && key <= '6') {
      // Ctrl+Shift+1..6: the views of Windows Explorer.
      static const int sizes[] = {256, 128, 72, 48};
      int n = key - '1';
      if (n < 4) set_icon_size(sizes[n]);
      else if (n == 4) set_layout(Layout::Compact);
      else set_layout(Layout::Details);
      return true;
    }
    if (in_text) return false;
    switch (key) {
      case 'a': select_all(); return true;
      case 'c': copy(); return true;
      case 'x': cut(); return true;
      case 'v': paste(); return true;
      case 'i': invert_selection(); return true;
    }
    return false;
  }
  switch (key) {
    case FL_F + 5: refresh(); return true;
    case FL_F + 11: toggle_fullscreen(); return true;
    case FL_F + 3:
      searchbox_->take_focus();
      return true;
    case FL_F + 4:
      addressbar_->start_edit();
      return true;
  }
  if (in_text) return false;
  switch (key) {
    case FL_F + 2: rename(); return true;
    case FL_Delete:
      remove(shift);
      return true;
    case FL_F + 10:
      if (shift) {
        Tab &t = tab();
        if (t.model.selected().empty()) background_menu(card_x_ + S(60), card_y_ + S(60));
        else item_menu(card_x_ + S(60), card_y_ + S(60));
        return true;
      }
      more_menu(commandbar_->x() + commandbar_->w() - S(200), commandbar_->y() + commandbar_->h());
      return true;
  }
  return false;
}

int MainWindow::handle(int e) {
  switch (e) {
    case FL_FOCUS:
      App::get().last_active = this;
      break;
    case FL_KEYBOARD:
    case FL_SHORTCUT: {
      int key = Fl::event_key();
      if (shortcut(key)) return 1;
      if (key == FL_Escape) {
        if (addressbar_->editing) {
          addressbar_->stop_edit();
          return 1;
        }
        if (!tab().query.empty()) {
          searchbox_->value("");
          set_query("");
          return 1;
        }
        if (fullscreen_active()) {
          fullscreen_off();
          return 1;
        }
        if (!tab().model.selected().empty()) {
          select_none();
          return 1;
        }
        return 1;  // Escape never closes Liria
      }
      if (key == FL_BackSpace && !dynamic_cast<Fl_Input *>(Fl::focus())) {
        back();
        return 1;
      }
      break;
    }
    case FL_PUSH: {
      App::get().last_active = this;
      int b = Fl::event_button();
      int mx = Fl::event_x(), my = Fl::event_y();
      if (b == FL_BACK_MOUSE) {
        back();
        return 1;
      }
      if (b == FL_FORWARD_MOUSE) {
        forward();
        return 1;
      }
      int edge = edge_at(mx, my);
      if (edge >= 0 && b == FL_LEFT_MOUSE) {
        start_resize(edge, b);
        return 1;
      }
      // The splitter between the sidebar and the files.
      if (sidebar_->visible() && my >= card_y_ && my < status_y_ && mx >= sidebar_->w() - S(2) && mx < card_x_ + S(2)) {
        sizing_sidebar_ = true;
        return 1;
      }
      if (my >= status_y_) {
        for (int k = 0; k < 2; ++k) {
          int bx = k == 0 ? view_details_x_ : view_icons_x_;
          if (mx >= bx && mx < bx + view_btn_w_) {
            set_layout(k == 0 ? Layout::Details : Layout::Icons);
            return 1;
          }
        }
      }
      break;
    }
    case FL_DRAG:
      if (sizing_sidebar_) {
        int wpx = std::max(S(150), std::min(Fl::event_x(), w() / 2));
        App::get().prefs.sidebar_width = int(wpx / ui_scale());
        layout();
        redraw();
        return 1;
      }
      break;
    case FL_RELEASE:
      if (sizing_sidebar_) {
        sizing_sidebar_ = false;
        App::get().prefs.save();
        return 1;
      }
      break;
    case FL_MOVE:
    case FL_ENTER: {
      int mx = Fl::event_x(), my = Fl::event_y();
      int edge = edge_at(mx, my);
      static const Fl_Cursor cursors[8] = {FL_CURSOR_NW, FL_CURSOR_N, FL_CURSOR_NE, FL_CURSOR_E,
                                           FL_CURSOR_SE, FL_CURSOR_S, FL_CURSOR_SW, FL_CURSOR_W};
      bool splitter = sidebar_->visible() && my >= card_y_ && my < status_y_ && mx >= sidebar_->w() - S(2) &&
                      mx < card_x_ + S(2);
      cursor(edge >= 0 ? cursors[edge] : splitter ? FL_CURSOR_WE : FL_CURSOR_DEFAULT);
      int sh = -1;
      if (my >= status_y_)
        for (int k = 0; k < 2; ++k) {
          int bx = k == 0 ? view_details_x_ : view_icons_x_;
          if (mx >= bx && mx < bx + view_btn_w_) sh = k;
        }
      if (sh != status_hover_) {
        status_hover_ = sh;
        damage_rect(0, status_y_, w(), h() - status_y_);
      }
      if (edge >= 0 || splitter) return 1;
      break;
    }
    case FL_LEAVE:
      cursor(FL_CURSOR_DEFAULT);
      break;
  }
  return BufferedWindow::handle(e);
}

// ------------------------------------------------------------------ drag and drop

namespace {

// What lies under (x, y) of the window for a drop: a folder, the Trash, or
// a place in Quick access. |pin_at| >= 0: the drop pins there.
struct DropPlace {
  std::string dir;
  int pin_at = -1;
  bool valid() const { return !dir.empty() || pin_at >= 0; }
};

}  // namespace

static DropPlace drop_place(MainWindow *w, int mx, int my, const std::vector<std::string> &dragged) {
  DropPlace d;
  FileView *fv = w->fileview();
  Sidebar *sb = w->sidebar();
  HomeView *hv = w->homeview();
  TabBar *tb = w->tabbar();
  auto inside = [&](Fl_Widget *wd) {
    return wd->visible() && mx >= wd->x() && mx < wd->x() + wd->w() && my >= wd->y() && my < wd->y() + wd->h();
  };
  if (inside(fv)) {
    Tab &t = w->tab();
    int k = fv->item_at(mx, my);
    if (k >= 0 && t.model.at(k).dir &&
        std::find(dragged.begin(), dragged.end(), t.model.at(k).path) == dragged.end() && is_local(t.model.at(k).path))
      d.dir = t.model.at(k).path;
    else if (is_local(t.location) || t.location == kTrash)
      d.dir = t.location;
    return d;
  }
  if (inside(sb)) {
    int r = sb->row_at(my);
    if (r < 0) return d;
    const auto &row = sb->rows[size_t(r)];
    if (row.kind == Sidebar::Row::Heading) {
      if (row.group == "pinned") d.pin_at = int(App::get().pins.items().size());
      return d;
    }
    if (row.location == kTrash || is_local(row.location)) d.dir = row.location;
    return d;
  }
  if (inside(hv)) {
    int c = hv->card_at(mx, my);
    if (c >= 0) {
      const auto &card = hv->cards[size_t(c)];
      if ((card.kind == HomeView::Card::Pin || card.kind == HomeView::Card::Drive) && is_local(card.location) &&
          dir_exists(card.location))
        d.dir = card.location;
      else if (card.kind == HomeView::Card::Pin && card.location == kTrash)
        d.dir = kTrash;
      else if (card.section == "quick")
        d.pin_at = card.kind == HomeView::Card::Pin ? card.index : int(App::get().pins.items().size());
    }
    return d;
  }
  if (inside(tb)) {
    int ti = tb->tab_at(mx);
    if (ti >= 0) {
      const std::string &loc = w->tab_at(ti).location;
      if (is_local(loc) || loc == kTrash) d.dir = loc;
    }
  }
  return d;
}

void MainWindow::setup_dnd() {
  desk::Xdnd::Hooks h;
  h.over = [this](int x, int y, unsigned mods, desk::DropAction suggested, const desk::ActionSet &allowed, bool files) {
    DropPlace d = drop_place(this, x - x_root(), y - y_root(), drag_paths_);
    std::string target = d.dir.empty() && d.pin_at >= 0 ? "pin" : d.dir;
    if (target != drop_target) {
      drop_target = target;
      fileview_->scroll->redraw();
      sidebar_->redraw();
      homeview_->redraw();
    }
    if (!d.valid()) return desk::DropAction::NoDrop;
    if (!files) return d.dir.empty() ? desk::DropAction::NoDrop : allowed.copy ? desk::DropAction::Copy : desk::DropAction::NoDrop;
    if (d.pin_at >= 0) return allowed.link ? desk::DropAction::Link : desk::DropAction::Copy;
    return desk::choose_drop_action(d.dir == kTrash ? desk::TargetKind::Trash : desk::TargetKind::Folder, mods, true,
                                    suggested, allowed);
  };
  h.leave = [this] {
    if (!drop_target.empty()) {
      drop_target.clear();
      fileview_->scroll->redraw();
      sidebar_->redraw();
    }
  };
  h.drop = [this](int x, int y, desk::DropAction action, const std::string &data, bool uri_list) {
    desk::DropPayload payload = desk::classify_drop(data, uri_list);
    DropPlace d = drop_place(this, x - x_root(), y - y_root(), {});
    drop_target.clear();
    if (payload.files.empty()) return desk::DropAction::NoDrop;
    if (d.pin_at >= 0 && d.dir.empty()) {
      App &app = App::get();
      int at = d.pin_at;
      for (const auto &f : payload.files)
        if (app.pins.add(f, at)) ++at;
      app.places_changed();
      return desk::DropAction::Link;
    }
    if (d.dir.empty()) return desk::DropAction::NoDrop;
    Window r, c;
    int rx, ry, wx, wy;
    unsigned mask = 0;
    XQueryPointer(fl_display, RootWindow(fl_display, fl_screen), &r, &c, &rx, &ry, &wx, &wy, &mask);
    unsigned mods = desk::Xdnd::modifiers_from_state(mask);
    desk::DropAction a = drop_action_for(payload.files, d.dir, mods,
                                         action == desk::DropAction::Ask ? desk::DropAction::Ask : desk::DropAction::Copy);
    if (action == desk::DropAction::Ask) a = desk::DropAction::Ask;
    drop_files(payload.files, d.dir, a);
    return a == desk::DropAction::Ask ? desk::DropAction::Copy : a;
  };
  h.finished = [this](bool accepted, desk::DropAction action) {
    if (accepted && action == desk::DropAction::Move) Fl::add_timeout(0.5, [](void *d) {
        auto *w = static_cast<MainWindow *>(d);
        auto &ws = App::get().windows;
        if (std::find(ws.begin(), ws.end(), w) != ws.end()) w->folder_changed(w->tab().location, {});
      }, this);
    drag_paths_.clear();
  };
  xdnd_.reset(new desk::Xdnd(fl_display, fl_xid(this), h));
}

desk::DropAction MainWindow::drop_action_for(const std::vector<std::string> &paths, const std::string &dir,
                                             unsigned mods, desk::DropAction suggested) {
  if (dir == kTrash) return desk::DropAction::Move;
  bool same = !paths.empty() && desk::same_filesystem(paths[0], dir);
  return desk::choose_drop_action(desk::TargetKind::Folder, mods, same, suggested, desk::ActionSet());
}

void MainWindow::drop_files(const std::vector<std::string> &paths, const std::string &dir, desk::DropAction action) {
  if (paths.empty() || dir.empty()) return;
  App &app = App::get();
  if (dir == kTrash) {
    app.trash(paths, this);
    return;
  }
  // Dropped on the folder they are in: nothing to do for a move.
  if (action == desk::DropAction::Move &&
      std::all_of(paths.begin(), paths.end(), [&](const std::string &p) { return chooser::parent_dir(p) == dir; }))
    return;
  if (action == desk::DropAction::Ask) {
    ContextMenu m;
    std::vector<std::string> copy = paths;
    m.item(_("Move here"), [this, copy, dir] { App::get().transfer(copy, dir, Transfer::Move, this); }, {"cut"});
    m.item(_("Copy here"), [this, copy, dir] { App::get().transfer(copy, dir, Transfer::Copy, this); }, {"copy"});
    m.item(_("Link here"), [this, copy, dir] { App::get().transfer(copy, dir, Transfer::Link, this); }, {"link"});
    m.separator();
    m.item(_("Cancel"), [] {});
    m.popup();
    return;
  }
  Transfer kind = action == desk::DropAction::Move ? Transfer::Move
                  : action == desk::DropAction::Link ? Transfer::Link
                                                     : Transfer::Copy;
  app.transfer(paths, dir, kind, this);
}

void MainWindow::begin_drag(const std::vector<std::string> &paths, int count, unsigned long time) {
  if (!xdnd_ || paths.empty()) return;
  drag_paths_ = paths;
  cairo_surface_t *icon = nullptr;
  Item e;
  if (is_local(paths[0]) && chooser::entry_for_path(paths[0], e)) icon = IconLoader::get(icon_names(e), S(48));
  xdnd_->begin_drag(paths, icon, S(48), count, time);
}

bool MainWindow::drag_motion(int xr, int yr, unsigned mods, unsigned long time) {
  if (!xdnd_ || !xdnd_->dragging()) return false;
  bool own = xdnd_->motion(xr, yr, mods, time);
  std::string target;
  if (own) {
    DropPlace d = drop_place(this, xr - x_root(), yr - y_root(), drag_paths_);
    target = d.dir.empty() && d.pin_at >= 0 ? "pin" : d.dir;
    if (target == tab().location && fileview_->visible()) target.clear();
  }
  if (target != drop_target) {
    drop_target = target;
    fileview_->scroll->redraw();
    sidebar_->redraw();
    homeview_->redraw();
  }
  return own;
}

void MainWindow::drag_drop(unsigned long time) {
  if (!xdnd_) return;
  if (xdnd_->dragging() && xdnd_->over_remote()) {
    xdnd_->drop(time);
    drop_target.clear();
    return;
  }
  // Inside this window.
  std::vector<std::string> paths = drag_paths_;
  xdnd_->cancel();
  int mx = Fl::event_x_root() - x_root(), my = Fl::event_y_root() - y_root();
  DropPlace d = drop_place(this, mx, my, paths);
  drop_target.clear();
  fileview_->scroll->redraw();
  sidebar_->redraw();
  homeview_->redraw();
  if (d.pin_at >= 0 && d.dir.empty()) {
    App &app = App::get();
    int at = d.pin_at;
    for (const auto &p : paths)
      if (dir_exists(p) && app.pins.add(p, at)) ++at;
    app.places_changed();
    drag_paths_.clear();
    return;
  }
  if (!d.dir.empty()) drop_files(paths, d.dir, drop_action_for(paths, d.dir, fl_mods(), desk::DropAction::Move));
  drag_paths_.clear();
}

void MainWindow::cancel_drag() {
  if (xdnd_) xdnd_->cancel();
  drag_paths_.clear();
}

bool MainWindow::dragging() const { return xdnd_ && xdnd_->dragging(); }

// ------------------------------------------------------------------ QA

void MainWindow::schedule_qa() {
  if (!getenv("SDE_PANEL_QA_DIR")) return;
  Fl::remove_timeout(qa_cb, this);
  Fl::add_timeout(0.05, qa_cb, this);
}

void MainWindow::qa_cb(void *d) { static_cast<MainWindow *>(d)->qa_dump(); }

// UI test hook, as the Control Center's and the file chooser's: with
// SDE_PANEL_QA_DIR set, <dir>/Liria.txt describes the active window
// ("@location:", "@selected:" ... lines) and its named parts, one
// "name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>active<TAB>0" line each (window pixels).
void MainWindow::qa_dump() {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  if (!dir || !*dir || !shown()) return;
  App &app = App::get();
  if (app.last_active && app.last_active != this) {
    auto &ws = app.windows;
    if (std::find(ws.begin(), ws.end(), app.last_active) != ws.end()) return;
  }
  std::string out;
  auto state = [&](const std::string &k, std::string v) {
    for (char &c : v)
      if (c == '\t' || c == '\n') c = ' ';
    out += "@" + k + ":" + v + "\t0\t0\t0\t0\t1\t0\n";
  };
  auto control = [&](std::string name, int x, int y, int w, int h, bool active) {
    for (char &c : name)
      if (c == '\t' || c == '\n') c = ' ';
    out += name + "\t" + std::to_string(x) + "\t" + std::to_string(y) + "\t" + std::to_string(w) + "\t" +
           std::to_string(h) + "\t" + (active ? "1" : "0") + "\t0\n";
  };
  auto widget = [&](const std::string &name, Fl_Widget *wd) {
    if (wd && wd->visible_r()) control(name, wd->x(), wd->y(), wd->w(), wd->h(), wd->active_r());
  };
  Tab &t = tab();
  state("window", id_);
  state("xid", std::to_string((unsigned long)fl_xid(this)));
  state("title", label() ? label() : "");
  state("location", t.location);
  state("tabs", std::to_string(tab_count()));
  state("tab", std::to_string(current_));
  state("layout", layout_id(t.layout));
  state("icon-size", std::to_string(t.icon_size));
  state("loading", t.loading ? "1" : "0");
  state("items", std::to_string(t.model.count()));
  state("total", std::to_string(t.model.total()));
  std::string sel;
  for (const Item *e : t.model.selection()) sel += (sel.empty() ? "" : "|") + e->name;
  state("selected", sel);
  state("focus", t.model.focus);
  state("query", t.query);
  state("searching", t.searching ? "1" : "0");
  state("editing", addressbar_->editing ? "1" : "0");
  state("renaming", fileview_->editing);
  state("message", message_);
  state("hidden", app.prefs.show_hidden ? "1" : "0");
  state("sort", chooser::sort_key_id(t.model.order.key) + (t.model.order.descending ? ":desc" : ":asc"));
  state("clipboard", app.clipboard && app.clipboard->owned()
                         ? std::string(app.clipboard->cut() ? "cut:" : "copy:") + std::to_string(app.clipboard->paths().size())
                         : "");
  state("pins", std::to_string(app.pins.items().size()));
  state("csd", csd_ ? "1" : "0");
  state("first-frame-ms", std::to_string(int(std::lround(first_frame_ms))));
  state("loaded-ms", std::to_string(int(std::lround(t.loaded_ms))));
  state("jobs", std::to_string(app.running_jobs()));
  state("undo", std::to_string(app.undo.size()));
  state("status", status_text());
  state("menu", menu_qa_);
  for (int i = 0; i < tab_count(); ++i) {
    if (i >= int(tabbar_->slots.size())) break;
    control("tab:" + std::to_string(i) + ":" + tab_at(i).title(), tabbar_->slots[size_t(i)].x, tabbar_->y(),
            tabbar_->slots[size_t(i)].w, tabbar_->h(), true);
  }
  control("new-tab", tabbar_->plus_x, tabbar_->y(), S(32), tabbar_->h(), true);
  if (csd_) {
    control("caption:minimize", tabbar_->caption_x, 0, tabbar_->button_w, tabbar_->h(), true);
    control("caption:maximize", tabbar_->caption_x + tabbar_->button_w, 0, tabbar_->button_w, tabbar_->h(), true);
    control("caption:close", tabbar_->caption_x + 2 * tabbar_->button_w, 0, tabbar_->button_w, tabbar_->h(), true);
  }
  widget("back", back_);
  widget("forward", forward_);
  widget("up", up_);
  widget("refresh", refresh_);
  widget("address", addressbar_);
  widget("address-entry", addressbar_->editing ? addressbar_->entry : nullptr);
  if (!addressbar_->editing)
    for (size_t i = 0; i < addressbar_->crumbs.size(); ++i) {
      const auto &c = addressbar_->crumbs[i];
      control("crumb:" + std::to_string(i) + ":" + c.label, c.x, addressbar_->y(), c.w, addressbar_->h(), true);
    }
  for (size_t i = 0; i < addressbar_->suggestions.size(); ++i)
    if (addressbar_->list->visible())
      control("suggestion:" + addressbar_->suggestions[i].text, addressbar_->list->x(),
              addressbar_->list->y() + S(4) + int(i + (addressbar_->error.empty() ? 0 : 1)) * addressbar_->list->row_h(),
              addressbar_->list->w(), addressbar_->list->row_h(), true);
  state("address-error", addressbar_->error);
  widget("search", searchbox_);
  for (const auto &b : commandbar_->buttons)
    control("command:" + b.id, b.x, commandbar_->y(), b.w, commandbar_->h(), b.enabled);
  if (sidebar_->visible())
    for (const auto &r : sidebar_->rows) {
      int ry = sidebar_->canvas_y() + r.y;
      bool in = ry >= sidebar_->y() && ry + r.h <= sidebar_->y() + sidebar_->h();
      control((r.kind == Sidebar::Row::Heading ? "heading:" + r.group : "place:" + r.id), sidebar_->x() + S(8), ry,
              sidebar_->w() - S(16), r.h, in);
    }
  if (homeview_->visible())
    for (const auto &c : homeview_->cards) {
      int cy = homeview_->canvas_y() + c.y;
      bool in = cy >= homeview_->y() && cy + c.h <= homeview_->y() + homeview_->h();
      std::string kind = c.kind == HomeView::Card::Header  ? "header"
                         : c.kind == HomeView::Card::Pin   ? "pin"
                         : c.kind == HomeView::Card::Drive ? "drive"
                         : c.kind == HomeView::Card::Recent ? "recent"
                         : c.kind == HomeView::Card::Empty ? "note"
                         : c.kind == HomeView::Card::Button ? "button"
                                                            : "network";
      control("card:" + c.section + ":" + kind + ":" + c.label, homeview_->canvas_x() + c.x, cy, c.w, c.h, in);
    }
  if (fileview_->visible()) {
    widget("files", fileview_->scroll);
    int first, last;
    fileview_->metrics();
    fileview_->visible_range(first, last);
    for (int k = first; k <= last && k < t.model.count(); ++k) {
      int X, Y, W, H;
      fileview_->item_rect(k, X, Y, W, H);
      bool in = Y >= fileview_->scroll->y() && Y + H <= fileview_->scroll->y() + fileview_->scroll->h();
      control("item:" + t.model.at(k).name, X, Y, W, H, in);
    }
    if (t.layout == Layout::Details)
      for (const auto &c : fileview_->columns()) control("column:" + c.id, c.x, fileview_->y(), c.w, fileview_->head_h, true);
    widget("rename-entry", fileview_->editing.empty() ? nullptr : fileview_->editor);
  }
  control("status:details", view_details_x_, status_y_, view_btn_w_, h() - status_y_, true);
  control("status:icons", view_icons_x_, status_y_, view_btn_w_, h() - status_y_, true);
  std::string path = std::string(dir) + "/Liria.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    ::rename(tmp.c_str(), path.c_str());
  }
}

}  // namespace liria
}  // namespace sde
