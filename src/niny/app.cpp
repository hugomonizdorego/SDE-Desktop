// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Niny window: documents, layout, files, session, backups, search and
// the command registry.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"
#include "i18n.h"
#include "niny.h"
#include "spawn.h"
#include "textops.h"
#include "util.h"
#include "x11.h"

using namespace sde;

namespace niny {

App *App::instance_ = nullptr;

namespace {

std::string absolute(const std::string &path, const std::string &cwd) {
  if (path.empty() || path[0] == '/') return path;
  std::string base = cwd;
  if (base.empty()) {
    char buf[PATH_MAX];
    base = getcwd(buf, sizeof buf) ? buf : home_dir();
  }
  std::string p = base + "/" + path;
  char resolved[PATH_MAX];
  if (realpath(p.c_str(), resolved)) return resolved;
  // Not existing yet: normalize the folder part.
  size_t s = p.rfind('/');
  if (s != std::string::npos && realpath(p.substr(0, s).c_str(), resolved))
    return std::string(resolved) + p.substr(s);
  return p;
}

std::string recent_path() { return state_dir() + "/recent"; }
std::string session_path() { return state_dir() + "/session.conf"; }

bool process_is_niny(int pid) {
  if (pid <= 0) return false;
  if (kill(pid, 0) != 0 && errno == ESRCH) return false;
  std::string comm;
  if (!read_file("/proc/" + std::to_string(pid) + "/comm", comm)) return false;
  return trim(comm) == "niny";
}

}  // namespace

App::App(int w, int h) : BufferedWindow(w, h, "Niny") {
  instance_ = this;
  settings_.load(settings_path());
  std::string rec;
  if (read_file(recent_path(), rec)) recent_ = parse_list(rec);
  {
    Config cfg;
    cfg.load();
    set_look(make_look(cfg, settings_.color_scheme));
  }
  begin();
  editor_ = new EditorView(this, 0, 0, w, h);
  mdview_ = new MarkdownView(this, 0, 0, w, h);
  mdview_->hide();
  info_ = new InfoBar(this, 0, 0, w, S(44));
  info_->hide();
  sidebar_ = new Sidebar(this, 0, 0, S(230), h);
  tabs_ = new TabBar(this, 0, 0, w, S(42));
  toolbar_ = new Toolbar(this, 0, 0, w, S(40));
  status_ = new StatusBar(this, 0, 0, w, S(30));
  settings_page_ = new SettingsPage(this, 0, 0, w, h);
  settings_page_->hide();
  find_ = new FindBar(this, 0, 0, S(560), S(52));
  palette_ = new Palette(this, 0, 0, S(600), S(300));
  sheet_ = new Sheet(this, 0, 0, w, h);
  end();
  resizable(nullptr);
  size_range(S(480), S(320));
  xclass("Niny");
  callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->quit(); }, this);
  build_commands();
  apply_theme();
  sidebar_->set_folder(home_dir());
  layout();
  Fl::event_dispatch(dispatch);
  Fl::add_handler(x_event);
  Fl::add_timeout(5.0, backup_cb, this);
  Fl::add_timeout(2.0, watch_cb, this);
  listen_for_instances(this);
}

App::~App() {
  stop_listening();
  Fl::event_dispatch(nullptr);
  Fl::remove_handler(x_event);
  Fl::remove_timeout(backup_cb, this);
  Fl::remove_timeout(watch_cb, this);
  Fl::remove_timeout(ui_cb, this);
  Fl::remove_timeout(toast_cb, this);
  Fl::remove_timeout(match_cb, this);
  Fl::remove_idle(idle_cb, this);
  editor_->buffer(nullptr);
  instance_ = nullptr;
}

void App::hide() {
  stop_listening();
  BufferedWindow::hide();
}

bool App::modal() const { return sheet_ && sheet_->active(); }

int App::ask(const std::string &title, const std::string &message, const std::vector<std::string> &buttons,
             int default_button, int cancel_button) {
  if (!shown()) return cancel_button;
  return sheet_->ask(title, message, buttons, default_button, cancel_button);
}

std::string App::cwd_folder() const {
  if (Document *d = active())
    if (!d->path.empty()) return d->folder();
  return sidebar_->folder().empty() ? home_dir() : sidebar_->folder();
}

void App::raise_window() {
  show();
  if (shown()) x11::activate_window(fl_xid(this));
}

// ------------------------------------------------------------- appearance

void App::apply_theme() {
  Config cfg;
  cfg.load();
  double scale = ui_scale();
  Theme t = Theme::from_config(cfg);
  set_ui_scale(scale);
  t.scale = scale;
  t.apply_to_fltk();
  set_look(make_look(cfg, settings_.color_scheme));
  const Look &L = look();
  // Popup menus and tooltips follow the editor colors.
  auto u = [](double v) { return uchar(std::lround(std::fmin(1, std::fmax(0, v)) * 255)); };
  Fl::background(u(L.card.r), u(L.card.g), u(L.card.b));
  Fl::background2(u(L.field.r), u(L.field.g), u(L.field.b));
  Fl::foreground(u(L.text.r), u(L.text.g), u(L.text.b));
  Fl::set_color(FL_SELECTION_COLOR, L.accent.fl());
  Fl_Tooltip::color(L.card.fl());
  Fl_Tooltip::textcolor(L.text.fl());
  color(L.window.fl());
  editor_->apply_look();
  editor_->apply_wrap(settings_.word_wrap);
  find_->relayout();
  palette_->relayout();
  redraw();
}

void App::apply_settings(bool save) {
  int cur = editor_->insert_position(), top = editor_->top_line();
  apply_theme();
  for (auto &d : docs_) {
    if (!d->indent_manual) d->apply_indent_detection(settings_.detect_indent, settings_.tab_width, settings_.insert_spaces);
  }
  if (editor_->doc()) {
    editor_->insert_position(std::min(cur, editor_->doc()->length()));
    editor_->scroll(top, 0);
  }
  editor_->update_gutter();
  layout();
  if (save) settings_.save(settings_path());
  update_ui();
}

void App::editor_rect(int &X, int &Y, int &W, int &H) const {
  int sw = settings_.show_sidebar ? std::min(S(settings_.sidebar_width), w() / 2) : 0;
  X = sw;
  Y = header_h_;
  W = w() - sw;
  H = h() - header_h_ - status_h_;
}

void App::layout() {
  tabs_h_ = S(42);
  int bar_h = S(40);
  header_h_ = tabs_h_ + bar_h;
  bool page = settings_visible();
  status_h_ = settings_.status_bar && !page ? S(30) : 0;
  int W = w(), H = h();
  tabs_->resize(S(46), 0, std::max(S(80), W - S(46) - S(6)), tabs_h_);
  toolbar_->resize(0, tabs_h_, W, bar_h);
  int ex, ey, ew, eh;
  editor_rect(ex, ey, ew, eh);
  int body_y = ey, body_h = eh;
  settings_page_->resize(0, header_h_, W, H - header_h_ - status_h_);
  if (ex && !page) {
    sidebar_->resize(0, body_y, ex, body_h);
    sidebar_->show();
  } else {
    sidebar_->hide();
  }
  Document *d = active();
  if (d && d->notice != Document::Notice::Clear && !page) {
    info_->resize(ex, ey, ew, S(44));
    info_->show_for(d);
    ey += S(44);
    eh -= S(44);
  } else {
    info_->show_for(nullptr);
  }
  // A little air above the first line, like a sheet of paper.
  int top_pad = S(6);
  editor_->resize(ex, ey + top_pad, ew, std::max(S(40), eh - top_pad));
  mdview_->resize(ex, ey, ew, std::max(S(40), eh));
  bool preview = d && d->preview && is_markdown(d);
  if (page) {
    editor_->hide();
    mdview_->hide();
    settings_page_->show();
  } else {
    settings_page_->hide();
    if (preview) {
      editor_->hide();
      mdview_->show_doc(d);
      mdview_->show();
    } else {
      mdview_->hide();
      editor_->show();
    }
  }
  if (find_->visible()) {
    int fw = std::min(S(600), ew - S(24));
    find_->resize(ex + ew - fw - S(18), ey + S(4), fw, find_->wanted_height());
    find_->relayout();
  }
  int pw = std::min(S(640), W - S(40));
  palette_->resize((W - pw) / 2, header_h_ + S(6), pw, palette_->h());
  palette_->relayout();
  sheet_->resize(0, 0, W, H);
  if (status_h_) {
    status_->resize(0, H - status_h_, W, status_h_);
    status_->show();
  } else {
    status_->hide();
  }
}

void App::resize(int X, int Y, int W, int H) {
  BufferedWindow::resize(X, Y, W, H);
  layout();
}

void App::draw_header(Painter &p) {
  const Look &L = look();
  int hh = tabs_h_;
  if (L.glass) {
    p.fill_gradient(0, 0, w(), hh, 0, L.strip, L.strip_bottom);
    p.glass_sheen(0, 0, w(), hh, 0, L.dark);
  } else {
    p.fill_rect(0, 0, w(), hh, L.strip);
  }
  // The app mark at the start of the tab strip.
  double s = S(22);
  icon::logo(p, (S(46) - s) / 2.0 + S(2), S(6) + (hh - S(6) - s) / 2.0, s);
}

void App::draw() {
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), look().editor);
    draw_header(p);
  }
  draw_children();
}

// --------------------------------------------------------------- commands

void App::build_commands() {
  commands_.clear();
  auto add = [&](const char *id, const std::string &label, const std::string &group, int shortcut,
                 const char *sc_text, std::function<void()> fn, bool global = true) {
    Command c;
    c.id = id;
    c.label = label;
    c.group = group;
    c.shortcut = shortcut;
    c.shortcut_text = sc_text ? sc_text : "";
    c.run = std::move(fn);
    c.global = global;
    commands_.push_back(std::move(c));
  };
  std::string F = _("File"), E = _("Edit"), S_ = _("Search"), V = _("View"), D = _("Document"), T = _("Tools");
  add("new", _("New Tab"), F, FL_CTRL + 'n', "Ctrl+N", [this] { new_document(); });
  add("new_window", _("New Window"), F, FL_CTRL + FL_SHIFT + 'n', "Ctrl+Shift+N", [this] { new_window(); });
  add("open", _("Open…"), F, FL_CTRL + 'o', "Ctrl+O", [this] { open_dialog(); });
  add("quick_open", _("Quick Open…"), F, FL_CTRL + 'p', "Ctrl+P", [this] { show_palette(""); });
  add("save", _("Save"), F, FL_CTRL + 's', "Ctrl+S", [this] { if (active()) save(active()); });
  add("save_as", _("Save As…"), F, FL_CTRL + FL_SHIFT + 's', "Ctrl+Shift+S", [this] { if (active()) save_as(active()); });
  add("save_all", _("Save All"), F, FL_CTRL + FL_ALT + 's', "Ctrl+Alt+S", [this] { save_all(); });
  add("reload", _("Reload from Disk"), F, 0, nullptr, [this] { if (active()) reload(active()); });
  add("close", _("Close Tab"), F, FL_CTRL + 'w', "Ctrl+W", [this] { if (active()) close_document(active_); });
  add("close_others", _("Close Other Tabs"), F, 0, nullptr, [this] {
    Document *keep = active();
    for (int i = int(docs_.size()) - 1; i >= 0; --i)
      if (docs_[size_t(i)].get() != keep && !close_document(i)) break;
  });
  add("close_all", _("Close All Tabs"), F, FL_CTRL + FL_SHIFT + 'w', "Ctrl+Shift+W", [this] {
    for (int i = int(docs_.size()) - 1; i >= 0; --i)
      if (!close_document(i)) break;
  });
  add("reopen", _("Reopen Closed Tab"), F, FL_CTRL + FL_SHIFT + 't', "Ctrl+Shift+T", [this] { reopen_closed(); });
  add("copy_path", _("Copy File Path"), F, 0, nullptr, [this] {
    if (active() && !active()->path.empty()) {
      Fl::copy(active()->path.c_str(), int(active()->path.size()), 1);
      toast(_("Path copied"));
    }
  });
  add("show_folder", _("Show File in Side Panel"), F, 0, nullptr, [this] {
    if (!active() || active()->path.empty()) return;
    sidebar_->set_folder(active()->folder());
    sidebar_->set_mode(1);
    if (!settings_.show_sidebar) toggle_sidebar();
  });
  add("quit", _("Exit"), F, FL_CTRL + 'q', "Ctrl+Q", [this] { quit(); });

  add("undo", _("Undo"), E, FL_CTRL + 'z', "Ctrl+Z", [this] { undo(); }, false);
  add("redo", _("Redo"), E, FL_CTRL + FL_SHIFT + 'z', "Ctrl+Shift+Z", [this] { redo(); }, false);
  add("cut", _("Cut"), E, FL_CTRL + 'x', "Ctrl+X", [this] { if (active()) Fl_Text_Editor::kf_cut(0, editor_); }, false);
  add("copy", _("Copy"), E, FL_CTRL + 'c', "Ctrl+C", [this] { if (active()) Fl_Text_Editor::kf_copy(0, editor_); }, false);
  add("paste", _("Paste"), E, FL_CTRL + 'v', "Ctrl+V", [this] { if (active()) Fl_Text_Editor::kf_paste(0, editor_); }, false);
  add("delete", _("Delete"), E, 0, "Del", [this] { delete_selection(); }, false);
  add("select_all", _("Select All"), E, FL_CTRL + 'a', "Ctrl+A", [this] {
    if (active()) active()->buf->select(0, active()->buf->length());
    editor_->redraw();
    cursor_moved();
  }, false);
  add("date_time", _("Time/Date"), E, FL_F + 5, "F5", [this] { insert_date_time(); });
  add("search_web", _("Search the Web"), E, FL_CTRL + 'e', "Ctrl+E", [this] { search_web(); });
  add("duplicate", _("Duplicate Line or Selection"), E, FL_CTRL + 'd', "Ctrl+D", [this] { duplicate_lines(); }, false);
  add("delete_line", _("Delete Line"), E, FL_CTRL + FL_SHIFT + 'k', "Ctrl+Shift+K", [this] { delete_lines(); }, false);
  add("move_up", _("Move Line Up"), E, FL_ALT + FL_Up, "Alt+Up", [this] { move_lines(-1); }, false);
  add("move_down", _("Move Line Down"), E, FL_ALT + FL_Down, "Alt+Down", [this] { move_lines(1); }, false);
  add("comment", _("Toggle Comment"), E, FL_CTRL + '/', "Ctrl+/", [this] { toggle_comment(); }, false);
  add("select_line", _("Select Line"), E, FL_CTRL + 'l', "Ctrl+L", [this] { select_line(); }, false);
  add("line_below", _("Insert Line Below"), E, FL_CTRL + FL_Enter, "Ctrl+Enter", [this] { insert_line(false); }, false);
  add("line_above", _("Insert Line Above"), E, FL_CTRL + FL_SHIFT + FL_Enter, "Ctrl+Shift+Enter",
      [this] { insert_line(true); }, false);
  add("indent", _("Indent"), E, 0, "Tab", [this] { indent(true); }, false);
  add("unindent", _("Unindent"), E, 0, "Shift+Tab", [this] { indent(false); }, false);
  add("bracket", _("Go to Matching Bracket"), E, FL_CTRL + 'm', "Ctrl+M", [this] { goto_matching_bracket(); }, false);
  add("upper", _("Convert to Upper Case"), E, FL_CTRL + 'u', "Ctrl+U", [this] { transform(upper_case, false); }, false);
  add("lower", _("Convert to Lower Case"), E, FL_CTRL + FL_SHIFT + 'u', "Ctrl+Shift+U",
      [this] { transform(lower_case, false); }, false);
  add("title", _("Convert to Title Case"), E, 0, nullptr, [this] { transform(title_case, false); });
  add("sort", _("Sort Lines"), E, 0, nullptr, [this] {
    transform([](const std::string &s) { return sort_lines(s); }, true);
  });
  add("sort_unique", _("Sort Lines and Remove Duplicates"), E, 0, nullptr, [this] {
    transform([](const std::string &s) { return sort_lines(s, false, true); }, true);
  });
  add("reverse", _("Sort Lines in Reverse"), E, 0, nullptr, [this] {
    transform([](const std::string &s) { return sort_lines(s, true); }, true);
  });
  add("trim", _("Trim Trailing Whitespace"), E, 0, nullptr, [this] {
    Document *d = active();
    if (!d) return;
    std::string t = trim_trailing_whitespace(d->text());
    if (t != d->text()) replace_range(0, d->length(), t, false);
  });

  std::string M = _("Format");
  add("md_bold", _("Bold"), M, FL_CTRL + 'b', "Ctrl+B", [this] { md_wrap("**"); });
  add("md_italic", _("Italic"), M, FL_CTRL + 'i', "Ctrl+I", [this] { md_wrap("*"); });
  add("md_strike", _("Strikethrough"), M, 0, nullptr, [this] { md_wrap("~~"); });
  add("md_code", _("Inline Code"), M, 0, nullptr, [this] { md_wrap("`"); });
  add("md_link", _("Link"), M, FL_CTRL + 'k', "Ctrl+K", [this] { md_link(); });
  add("md_clear", _("Clear Formatting"), M, FL_CTRL + FL_SHIFT + 'x', "Ctrl+Shift+X", [this] { md_clear(); });
  for (int lv = 1; lv <= 6; ++lv) {
    static const char *ids[] = {"", "md_h1", "md_h2", "md_h3", "md_h4", "md_h5", "md_h6"};
    static const char *keys[] = {"", "Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5", "Ctrl+6"};
    add(ids[lv], format(_("Heading %d"), lv), M, FL_CTRL + '0' + lv, keys[lv], [this, lv] { md_heading(lv); });
  }
  add("md_body", _("Body Text"), M, 0, nullptr, [this] { md_heading(0); });
  add("md_bullet", _("Bulleted List"), M, 0, nullptr, [this] { md_list(md::ListKind::Bullet); });
  add("md_numbered", _("Numbered List"), M, 0, nullptr, [this] { md_list(md::ListKind::Numbered); });
  add("md_task", _("Task List"), M, 0, nullptr, [this] { md_list(md::ListKind::Task); });
  add("md_quote", _("Quote"), M, 0, nullptr, [this] { md_list(md::ListKind::Quote); });
  add("preview", _("Formatted Markdown View"), M, FL_CTRL + FL_SHIFT + 'm', "Ctrl+Shift+M", [this] { toggle_preview(); });

  add("find", _("Find…"), S_, FL_CTRL + 'f', "Ctrl+F", [this] { show_find(false); });
  add("replace", _("Find and Replace…"), S_, FL_CTRL + 'h', "Ctrl+H", [this] { show_find(true); });
  add("find_next", _("Find Next"), S_, FL_F + 3, "F3", [this] { find_next(true); });
  add("find_prev", _("Find Previous"), S_, FL_SHIFT + FL_F + 3, "Shift+F3", [this] { find_next(false); });
  add("goto", _("Go To…"), S_, FL_CTRL + 'g', "Ctrl+G", [this] { show_palette(":"); });

  add("palette", _("Command Palette"), V, FL_CTRL + FL_SHIFT + 'p', "Ctrl+Shift+P", [this] { show_palette(">"); });
  add("sidebar", _("Side Panel"), V, FL_F + 9, "F9", [this] { toggle_sidebar(); });
  add("documents", _("Show Open Documents"), V, 0, nullptr, [this] {
    sidebar_->set_mode(0);
    if (!settings_.show_sidebar) toggle_sidebar();
  });
  add("wrap", _("Word Wrap"), V, FL_ALT + 'z', "Alt+Z", [this] { set_wrap(!settings_.word_wrap); });
  add("whitespace", _("Show Whitespace"), V, 0, nullptr, [this] { toggle_whitespace(); });
  add("line_numbers", _("Line Numbers"), V, 0, nullptr, [this] {
    settings_.line_numbers = !settings_.line_numbers;
    apply_settings(true);
  });
  add("status_bar", _("Status Bar"), V, 0, nullptr, [this] {
    settings_.status_bar = !settings_.status_bar;
    apply_settings(true);
  });
  add("zoom_in", _("Zoom In"), V, FL_CTRL + '=', "Ctrl++", [this] { zoom(1); });
  add("zoom_out", _("Zoom Out"), V, FL_CTRL + '-', "Ctrl+-", [this] { zoom(-1); });
  add("zoom_reset", _("Restore Default Zoom"), V, FL_CTRL + '0', "Ctrl+0", [this] { zoom(0); });
  add("next_tab", _("Next Tab"), V, FL_CTRL + FL_Page_Down, "Ctrl+Page Down", [this] {
    if (!docs_.empty()) activate((active_ + 1) % int(docs_.size()));
  });
  add("prev_tab", _("Previous Tab"), V, FL_CTRL + FL_Page_Up, "Ctrl+Page Up", [this] {
    if (!docs_.empty()) activate((active_ + int(docs_.size()) - 1) % int(docs_.size()));
  });
  add("fullscreen", _("Full Screen"), V, FL_F + 11, "F11", [this] {
    if (fullscreen_active()) fullscreen_off();
    else fullscreen();
  });
  add("scheme_light", _("Theme: Light"), V, 0, nullptr, [this] {
    settings_.color_scheme = "light";
    apply_settings(true);
  });
  add("scheme_dark", _("Theme: Dark"), V, 0, nullptr, [this] {
    settings_.color_scheme = "dark";
    apply_settings(true);
  });
  add("scheme_auto", _("Theme: Follow SDE"), V, 0, nullptr, [this] {
    settings_.color_scheme = "auto";
    apply_settings(true);
  });

  for (const Language *l : all_languages()) {
    std::string id = "lang:" + language_id(l);
    Command c;
    c.id = id;
    c.label = format(_("Language: %s"), language_name(l).c_str());
    c.group = D;
    c.run = [this, l] { set_language(l); };
    commands_.push_back(c);
  }
  add("eol_lf", _("Line Endings: LF (Unix)"), D, 0, nullptr, [this] { set_eol(Eol::LF); });
  add("eol_crlf", _("Line Endings: CRLF (Windows)"), D, 0, nullptr, [this] { set_eol(Eol::CRLF); });
  add("eol_cr", _("Line Endings: CR (classic Mac)"), D, 0, nullptr, [this] { set_eol(Eol::CR); });
  for (Encoding e : all_encodings()) {
    Command c;
    c.id = std::string("enc:") + encoding_name(e);
    c.label = format(_("Save with Encoding: %s"), encoding_name(e));
    c.group = D;
    c.run = [this, e] { set_encoding_for_save(e); };
    commands_.push_back(c);
    Command r;
    r.id = std::string("reopen_enc:") + encoding_name(e);
    r.label = format(_("Reopen with Encoding: %s"), encoding_name(e));
    r.group = D;
    r.run = [this, e] { if (active() && !active()->path.empty()) reload(active(), &e); };
    commands_.push_back(r);
  }
  add("indent_spaces", _("Indent Using Spaces"), D, 0, nullptr, [this] {
    if (active()) set_indent(false, active()->indent_width);
  });
  add("indent_tabs", _("Indent Using Tabs"), D, 0, nullptr, [this] {
    if (active()) set_indent(true, active()->buf->tab_distance());
  });
  for (int wdt : {2, 4, 8}) {
    Command c;
    c.id = "indent_width:" + std::to_string(wdt);
    c.label = format(_("Indent Width: %d"), wdt);
    c.group = D;
    c.run = [this, wdt] { if (active()) set_indent(active()->use_tabs, wdt); };
    commands_.push_back(c);
  }
  add("settings", _("Settings"), T, FL_CTRL + ',', "Ctrl+,", [this] { open_settings(); });
  add("about", _("About Niny"), T, 0, nullptr, [this] { about(); });
}

void App::run_command(const std::string &id) {
  for (auto &c : commands_)
    if (c.id == id) {
      c.run();
      return;
    }
}

bool App::global_shortcut(int key, int state) {
  state &= FL_SHIFT | FL_CTRL | FL_ALT | FL_META;
  if (key >= 'A' && key <= 'Z') key += 32;
  // Ctrl+Tab, Alt+1..9, keypad zoom and Escape.
  if ((state & FL_CTRL) && key == FL_Tab) {
    run_command(state & FL_SHIFT ? "prev_tab" : "next_tab");
    return true;
  }
  if (state == FL_ALT && (key == 'f' || key == 'e' || key == 'v')) {
    toolbar_->open(key == 'f' ? Toolbar::File : key == 'e' ? Toolbar::Edit : Toolbar::View);
    return true;
  }
  if (key == FL_Escape && state == 0 && settings_visible()) {
    show_settings_page(false);
    return true;
  }
  if (key == FL_Menu && state == 0 && active() && !settings_visible()) {
    int px = 0, py = 0;
    editor_->position_to_xy(editor_->insert_position(), &px, &py);
    context_menu(x_root() + px, y_root() + py + editor_->line_height());
    return true;
  }
  if (state == FL_ALT && key >= '1' && key <= '9') {
    int i = key == '9' ? int(docs_.size()) - 1 : key - '1';
    if (i >= 0 && i < int(docs_.size())) activate(i);
    return true;
  }
  if (state & FL_CTRL) {
    if (key == FL_KP + '+' || (key == '=' && (state & FL_SHIFT)) || key == '+') {
      zoom(1);
      return true;
    }
    if (key == FL_KP + '-') {
      zoom(-1);
      return true;
    }
    if (key == FL_KP + '0') {
      zoom(0);
      return true;
    }
  }
  if (key == FL_Escape && state == 0) {
    if (palette_->visible()) {
      palette_->close(true);
      return true;
    }
    if (find_->visible()) {
      hide_find();
      return true;
    }
    return false;
  }
  if (key == FL_Tab && (state & FL_CTRL) == 0) return false;
  for (auto &c : commands_) {
    if (!c.global || !c.shortcut) continue;
    int ck = c.shortcut & 0xFFFF, cs = c.shortcut & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META);
    if (ck == key && cs == state) {
      c.run();
      return true;
    }
  }
  // Ctrl+Shift+/ arrives as Ctrl+? on some layouts; the editor handles it.
  return false;
}

int App::dispatch(int event, Fl_Window *w) {
  App *self = instance_;
  if (self && w == self && !self->modal()) {
    if (event == FL_KEYBOARD && self->global_shortcut(Fl::event_key(), Fl::event_state())) return 1;
    if (event == FL_PUSH && self->palette_->visible() && !Fl::event_inside(self->palette_)) {
      self->palette_->close(true);
    }
  }
  return Fl::handle_(event, w);
}

void App::popup_status(int id, int x, int y) {
  Document *d = active();
  if (!d) return;
  if (id == StatusBar::Position) {
    show_palette(":");
    return;
  }
  MenuSpec spec;
  auto radio = [](const std::string &label, bool on, std::function<void()> fn) {
    MenuItem m;
    m.label = label;
    m.radio = true;
    m.checked = on;
    m.run = std::move(fn);
    return m;
  };
  if (id == StatusBar::FileType) {
    for (const Language *l : all_languages())
      spec.items.push_back(radio(is_plain(l) ? std::string(_("Plain text")) : language_name(l), d->lang == l,
                                 [this, l] { set_language(l); }));
  } else if (id == StatusBar::Zoom) {
    spec.items.push_back(command_item("zoom_in"));
    spec.items.push_back(command_item("zoom_out"));
    spec.items.push_back(command_item("zoom_reset"));
  } else if (id == StatusBar::LineEnding) {
    spec.items.push_back(radio(_("Windows (CRLF)"), d->eol == Eol::CRLF && !d->mixed_eol, [this] { set_eol(Eol::CRLF); }));
    spec.items.push_back(radio(_("Unix (LF)"), d->eol == Eol::LF && !d->mixed_eol, [this] { set_eol(Eol::LF); }));
    spec.items.push_back(radio(_("Macintosh (CR)"), d->eol == Eol::CR && !d->mixed_eol, [this] { set_eol(Eol::CR); }));
  } else if (id == StatusBar::Encoding) {
    for (Encoding e : all_encodings())
      spec.items.push_back(radio(encoding_name(e), d->encoding == e, [this, e] { set_encoding_for_save(e); }));
    if (!d->path.empty()) {
      spec.items.push_back(MenuItem::sep());
      MenuItem re;
      re.label = _("Reopen with Encoding");
      for (Encoding e : all_encodings()) {
        MenuItem m;
        m.label = encoding_name(e);
        m.run = [this, d, e] { reload(d, &e); };
        re.sub.push_back(m);
      }
      spec.items.push_back(re);
    }
  } else {
    return;
  }
  spec.min_width = 160;
  popup_menu(spec, x, y, -1);
}

void App::about() {
  ask(std::string("Niny ") + NINY_VERSION,
      format(_("A simple text editor for the Selviana Desktop Environment.\n\n"
               "Developer: DebION Dev-Team\n"
               "Copyright (C) 2026 DebION Dev-Team. Niny is free software under the GNU General Public "
               "License, version 3 or later.\n\nSettings: %s"),
             home_relative(settings_path()).c_str()),
      {_("Close")}, 0, 0);
}

void App::new_window() {
  char exe[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  std::string prog = n > 0 ? std::string(exe, size_t(n)) : std::string("niny");
  if (!spawn_detached({prog, "--new-window"})) toast(_("Could not open a new window"));
}

void App::toggle_whitespace() {
  settings_.show_whitespace = !settings_.show_whitespace;
  settings_.save(settings_path());
  editor_->redraw();
}

void App::toggle_preview() {
  Document *d = active();
  if (!d) return;
  if (!is_markdown(d)) {
    toast(_("The formatted view is for Markdown documents"));
    return;
  }
  if (!d->preview) {
    d->cursor = editor_->insert_position();
    d->top_line = editor_->top_line();
  }
  d->preview = !d->preview;
  if (d->preview) hide_find();
  layout();
  if (!d->preview) editor_->take_focus();
  else mdview_->take_focus();
  update_ui();
  redraw();
}

bool App::settings_visible() const { return settings_page_ && settings_page_->visible(); }

void App::show_settings_page(bool on) {
  if (on == settings_visible()) return;
  if (on) {
    palette_->close(false);
    hide_find();
    settings_page_->rebuild();
    settings_page_->show();
  } else {
    settings_page_->hide();
  }
  layout();
  if (on) settings_page_->take_focus();
  else if (active()) editor_->take_focus();
  update_ui();
  redraw();
}

// -------------------------------------------------------------- documents

int App::find_document(const std::string &path) const {
  if (path.empty()) return -1;
  for (size_t i = 0; i < docs_.size(); ++i)
    if (docs_[i]->path == path) return int(i);
  return -1;
}

Document *App::new_document() {
  auto d = std::make_unique<Document>(this);
  d->untitled = ++untitled_counter_;
  d->indent_width = settings_.tab_width;
  d->use_tabs = !settings_.insert_spaces;
  d->set_tab_width(settings_.tab_width);
  Encoding e;
  if (encoding_from_name(settings_.default_encoding, e)) d->encoding = e;
  Eol eol;
  if (eol_from_name(settings_.default_eol, eol)) d->eol = eol;
  Document *raw = d.get();
  int at = active_ < 0 ? int(docs_.size()) : active_ + 1;
  docs_.insert(docs_.begin() + at, std::move(d));
  activate(at);
  return raw;
}

Document *App::open_file(const std::string &input, int line, int column, bool activate_it) {
  std::string path = absolute(input, "");
  int existing = find_document(path);
  if (existing >= 0) {
    if (activate_it) activate(existing);
    if (line > 0) goto_line(line, column);
    return docs_[size_t(existing)].get();
  }
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    sidebar_->set_folder(path);
    sidebar_->set_mode(1);
    if (!settings_.show_sidebar) toggle_sidebar();
    return nullptr;
  }
  auto d = std::make_unique<Document>(this);
  if (file_exists(path)) {
    std::string err;
    if (!d->load(path, err)) {
      if (shown()) ask(_("Cannot open the file"), basename_of(path) + ": " + err, {_("OK")}, 0, 0);
      else fprintf(stderr, "niny: %s: %s\n", path.c_str(), err.c_str());
      return nullptr;
    }
  } else {
    // A new file: created on the first save.
    d->path = path;
    d->lang = detect_language(path);
    Encoding e;
    if (encoding_from_name(settings_.default_encoding, e)) d->encoding = e;
  }
  d->apply_indent_detection(settings_.detect_indent, settings_.tab_width, settings_.insert_spaces);
  if (settings_.remember_recent) push_recent(recent_, path);
  save_recent();
  // Replace an untouched empty untitled document.
  Document *cur = active();
  int at = active_ < 0 ? int(docs_.size()) : active_ + 1;
  bool replace = cur && cur->path.empty() && !cur->modified() && cur->length() == 0 && docs_.size() == 1;
  Document *raw = d.get();
  docs_.insert(docs_.begin() + at, std::move(d));
  if (replace) {
    remove_backup(cur);
    docs_.erase(docs_.begin() + (at - 1));
    at -= 1;
    active_ = -1;
    editor_->attach(nullptr);
  }
  if (activate_it || active_ < 0) activate(at);
  if (line > 0) goto_line(line, column);
  if (Fl::has_idle(idle_cb, this) == 0 && raw->highlight_pending()) Fl::add_idle(idle_cb, this);
  return raw;
}

void App::open_files(const std::vector<std::string> &args, const std::string &cwd) {
  int line = 0;
  Document *last = nullptr;
  for (auto &a : args) {
    int l;
    if (parse_line_arg(a, l)) {
      line = l;
      continue;
    }
    std::string p = uri_to_path(a);
    if (p.empty()) {
      toast(format(_("Only local files can be opened: %s"), a.c_str()));
      continue;
    }
    p = absolute(p, cwd);
    int col = 0;
    std::string stripped;
    int sl = 0, sc = 0;
    if (!file_exists(p) && split_position_suffix(p, stripped, sl, sc) && file_exists(stripped)) {
      p = stripped;
      line = sl;
      col = sc;
    }
    if (Document *d = open_file(p, line, col, true)) last = d;
    line = 0;
  }
  if (last) {
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i].get() == last) activate(int(i));
  }
}

void App::activate(int index) {
  if (index < 0 || index >= int(docs_.size())) return;
  if (active_ != index || editor_->doc() != docs_[size_t(index)].get()) {
    active_ = index;
    Document *d = docs_[size_t(index)].get();
    editor_->attach(d);
    if (!d->path.empty()) sidebar_->set_folder(d->folder());
    match_rev_ = ~0u;
    if (find_->visible()) refresh_matches();
  }
  layout();
  tabs_->ensure_visible(index);
  if (!palette_->visible() && !find_->visible()) editor_->take_focus();
  update_ui();
  redraw();
}

void App::move_document(int from, int to) {
  if (from < 0 || to < 0 || from >= int(docs_.size()) || to >= int(docs_.size()) || from == to) return;
  Document *act = active();
  auto d = std::move(docs_[size_t(from)]);
  docs_.erase(docs_.begin() + from);
  docs_.insert(docs_.begin() + to, std::move(d));
  for (size_t i = 0; i < docs_.size(); ++i)
    if (docs_[i].get() == act) active_ = int(i);
  update_ui();
}

bool App::close_document(int index, bool ask_user) {
  if (index < 0 || index >= int(docs_.size())) return false;
  Document *d = docs_[size_t(index)].get();
  if (ask_user && d->modified()) {
    activate(index);
    int r = ask(format(_("Save changes to “%s”?"), d->display_name().c_str()),
                _("Your changes will be lost if you close it without saving."),
                {_("Close Without Saving"), _("Cancel"), _("Save")}, 2, 1);
    if (r == 1) return false;
    if (r == 2 && !save(d)) return false;
  }
  remove_backup(d);
  if (!d->path.empty()) {
    closed_.erase(std::remove(closed_.begin(), closed_.end(), d->path), closed_.end());
    closed_.push_back(d->path);
  }
  if (editor_->doc() == d) editor_->attach(nullptr);
  if (match_doc_ == d) match_doc_ = nullptr;
  docs_.erase(docs_.begin() + index);
  if (docs_.empty()) {
    // Like Notepad's last tab: start over with an empty one.
    active_ = -1;
    editor_->buffer(nullptr);
    hide_find();
    new_document();
    return true;
  }
  int next = std::min(index, int(docs_.size()) - 1);
  if (active_ > index) --active_;
  active_ = -1;
  activate(next);
  return true;
}

void App::reopen_closed() {
  while (!closed_.empty()) {
    std::string p = closed_.back();
    closed_.pop_back();
    if (file_exists(p)) {
      open_file(p);
      return;
    }
  }
  toast(_("No closed documents to reopen"));
}

void App::open_dialog() {
  Fl_Native_File_Chooser fc;
  fc.title(_("Open Files"));
  fc.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
  fc.directory(cwd_folder().c_str());
  if (fc.show() != 0) return;
  std::vector<std::string> files;
  for (int i = 0; i < fc.count(); ++i) files.push_back(fc.filename(i));
  open_files(files, "");
}

bool App::save_as(Document *doc) {
  Fl_Native_File_Chooser fc;
  fc.title(_("Save As"));
  fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
  std::string dir = doc->path.empty() ? cwd_folder() : doc->folder();
  fc.directory(dir.c_str());
  fc.preset_file(doc->path.empty() ? (doc->display_name() + ".txt").c_str() : basename_of(doc->path).c_str());
  if (fc.show() != 0) return false;
  std::string path = fc.filename();
  if (path.empty()) return false;
  int other = find_document(path);
  if (other >= 0 && docs_[size_t(other)].get() != doc) {
    ask(_("The file is open in another tab"), _("Close that tab first, or choose another name."), {_("OK")}, 0, 0);
    return false;
  }
  std::string old = doc->path;
  doc->path = path;
  if (!save(doc)) {
    doc->path = old;
    return false;
  }
  return true;
}

bool App::save(Document *doc) {
  if (doc->path.empty()) return save_as(doc);
  // Optional clean-ups as one undo step.
  if (settings_.trim_trailing || settings_.final_newline) {
    std::string t = doc->text();
    std::string n = settings_.trim_trailing ? trim_trailing_whitespace(t) : t;
    if (settings_.final_newline && !n.empty() && n.back() != '\n') n += '\n';
    if (n != t) {
      Document *was = active();
      if (was == doc) {
        int c = editor_->insert_position();
        doc->buf->replace(0, doc->length(), n.c_str());
        editor_->insert_position(std::min(c, doc->length()));
      } else {
        doc->buf->replace(0, doc->length(), n.c_str());
      }
    }
  }
  std::string err;
  if (!doc->save_to(doc->path, err)) {
    ask(format(_("Could not save “%s”"), doc->display_name().c_str()), err, {_("OK")}, 0, 0);
    return false;
  }
  remove_backup(doc);
  if (settings_.remember_recent) push_recent(recent_, doc->path);
  save_recent();
  if (doc == active()) {
    sidebar_->set_folder(doc->folder());
    sidebar_->refresh();
  }
  toast(format(_("Saved %s"), doc->display_name().c_str()));
  layout();
  update_ui();
  return true;
}

void App::save_all() {
  for (auto &d : docs_)
    if (d->modified() && !save(d.get())) return;
}

void App::reload(Document *doc, const Encoding *forced) {
  if (doc->path.empty() || !file_exists(doc->path)) return;
  if (doc->modified()) {
    int r = ask(_("Reload and lose your changes?"), _("The document has changes that are not saved."),
                {_("Cancel"), _("Reload")}, 1, 0);
    if (r != 1) return;
  }
  if (doc == editor_->doc()) doc->cursor = editor_->insert_position();
  std::string err;
  if (!doc->reload(err, forced)) {
    ask(_("Cannot reload the file"), err, {_("OK")}, 0, 0);
    return;
  }
  remove_backup(doc);
  if (doc == editor_->doc()) {
    editor_->attach(nullptr);
    editor_->attach(doc);
  }
  if (!Fl::has_idle(idle_cb, this) && doc->highlight_pending()) Fl::add_idle(idle_cb, this);
  layout();
  update_ui();
}

void App::quit() {
  if (modal()) return;
  bool keep = settings_.hot_exit && settings_.backups;
  if (!keep) {
    std::vector<int> dirty;
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i]->modified()) dirty.push_back(int(i));
    if (dirty.size() == 1) {
      activate(dirty[0]);
      Document *d = docs_[size_t(dirty[0])].get();
      int r = ask(format(_("Save changes to “%s” before quitting?"), d->display_name().c_str()),
                  _("Your changes will be lost if you quit without saving."),
                  {_("Quit Without Saving"), _("Cancel"), _("Save")}, 2, 1);
      if (r == 1) return;
      if (r == 2 && !save(d)) return;
    } else if (dirty.size() > 1) {
      int r = ask(format(_("%d documents have unsaved changes"), int(dirty.size())),
                  _("Save them before quitting? Changes that are not saved will be lost."),
                  {_("Quit Without Saving"), _("Cancel"), _("Save All")}, 2, 1);
      if (r == 1) return;
      if (r == 2)
        for (int i : dirty)
          if (!save(docs_[size_t(i)].get())) return;
    }
    for (auto &d : docs_) remove_backup(d.get());
  } else {
    write_backups(true);
  }
  quitting_ = true;
  save_session();
  hide();
}

// ---------------------------------------------------------------- session

void App::save_recent() {
  make_dirs(state_dir());
  write_file_atomic(recent_path(), serialize_list(recent_));
}

void App::save_session() {
  if (secondary_) return;
  Session s;
  if (editor_->doc()) {
    editor_->doc()->cursor = editor_->insert_position();
    editor_->doc()->top_line = editor_->top_line();
  }
  if (settings_.restore_session || (settings_.hot_exit && settings_.backups)) {
    for (auto &d : docs_) {
      SessionEntry e;
      e.path = d->path;
      if (d->modified() && d->backup_written && settings_.backups && settings_.hot_exit) e.backup = d->backup_id;
      if (e.path.empty() && e.backup.empty()) continue;
      if (!settings_.restore_session && e.backup.empty()) continue;
      e.cursor = d->cursor;
      e.top_line = d->top_line;
      if (d->lang_manual) e.language = language_id(d->lang);
      s.entries.push_back(e);
      if (d.get() == active()) s.active = int(s.entries.size()) - 1;
    }
  }
  s.folder = sidebar_->folder();
  make_dirs(state_dir());
  write_file_atomic(session_path(), s.serialize());
}

Document *App::recover_from(const std::string &id, const std::string &path_hint) {
  std::string dir = backup_dir();
  std::string text, meta_text;
  if (!read_file(dir + "/" + id + ".txt", text)) return nullptr;
  read_file(dir + "/" + id + ".meta", meta_text);
  Ini meta;
  meta.parse(meta_text);
  std::string path = meta.get("backup", "path", path_hint);
  int existing = find_document(path);
  if (existing >= 0) {
    // Already open (restored from the session): give it the unsaved text.
    Document *open = docs_[size_t(existing)].get();
    open->set_content(text);
    open->undo.mark_unsaved();
    open->notice = Document::Notice::Recovered;
    open->notice_text = _("Niny closed unexpectedly. Your unsaved changes were recovered.");
    unlink((dir + "/" + id + ".txt").c_str());
    unlink((dir + "/" + id + ".meta").c_str());
    return open;
  }
  auto d = std::make_unique<Document>(this);
  std::string err;
  if (!path.empty() && file_exists(path) && d->load(path, err)) {
    d->set_content(text);
  } else {
    d->path = path;
    d->set_content(text);
    d->lang = path.empty() ? plain_language() : detect_language(path, text.substr(0, text.find('\n')));
    d->rehighlight_all();
  }
  Encoding e;
  if (encoding_from_name(meta.get("backup", "encoding", "UTF-8"), e)) d->encoding = e;
  Eol eol;
  if (eol_from_name(meta.get("backup", "eol", "LF"), eol)) d->eol = eol;
  if (const Language *l = find_language(meta.get("backup", "language", ""))) {
    d->lang = l;
    d->lang_manual = meta.get_bool("backup", "language_manual", false);
    d->rehighlight_all();
  }
  d->undo.mark_unsaved();
  if (path.empty()) d->untitled = ++untitled_counter_;
  d->apply_indent_detection(settings_.detect_indent, settings_.tab_width, settings_.insert_spaces);
  d->notice = Document::Notice::Recovered;
  d->notice_text = meta.get_int("backup", "pid", 0, 0, INT_MAX) == 0 || meta.get_bool("backup", "clean_exit", false)
                       ? _("Unsaved changes from your last session were restored.")
                       : _("Niny closed unexpectedly. Your unsaved changes were recovered.");
  unlink((dir + "/" + id + ".txt").c_str());
  unlink((dir + "/" + id + ".meta").c_str());
  Document *raw = d.get();
  docs_.push_back(std::move(d));
  return raw;
}

void App::restore_session() {
  std::string text;
  Session s;
  if (read_file(session_path(), text)) s.parse(text);
  if (!s.folder.empty() && dir_exists(s.folder)) sidebar_->set_folder(s.folder);
  int want_active = -1;
  for (size_t i = 0; i < s.entries.size(); ++i) {
    const SessionEntry &e = s.entries[i];
    Document *d = nullptr;
    if (!e.backup.empty()) d = recover_from(e.backup, e.path);
    if (!d && settings_.restore_session && !e.path.empty() && file_exists(e.path)) {
      d = open_file(e.path, 0, 0, false);
    }
    if (!d) continue;
    if (d->notice == Document::Notice::Recovered && e.backup.size()) {
      d->notice_text = _("Unsaved changes from your last session were restored.");
      // Quietly: the user chose to keep them on quit.
      d->notice = Document::Notice::Clear;
    }
    d->cursor = std::min(e.cursor, d->length());
    d->top_line = e.top_line;
    if (const Language *l = find_language(e.language)) d->set_language(l, true);
    if (int(i) == s.active)
      for (size_t k = 0; k < docs_.size(); ++k)
        if (docs_[k].get() == d) want_active = int(k);
  }
  if (!docs_.empty()) {
    active_ = -1;
    activate(want_active >= 0 ? want_active : int(docs_.size()) - 1);
  }
  if (!Fl::has_idle(idle_cb, this)) Fl::add_idle(idle_cb, this);
}

void App::recover_backups() {
  std::string dir = backup_dir();
  for (auto &name : list_dir(dir)) {
    if (!ends_with(name, ".meta")) continue;
    std::string id = name.substr(0, name.size() - 5);
    std::string meta_text;
    read_file(dir + "/" + name, meta_text);
    Ini meta;
    meta.parse(meta_text);
    int pid = meta.get_int("backup", "pid", 0, 0, INT_MAX);
    if (pid == getpid()) continue;
    if (process_is_niny(pid)) continue;  // another Niny window owns it
    Document *d = recover_from(id, "");
    for (size_t k = 0; d && k < docs_.size(); ++k)
      if (docs_[k].get() == d) {
        active_ = -1;
        activate(int(k));
      }
  }
  write_backups(true);
}

void App::write_backups(bool all) {
  if (!settings_.backups) return;
  std::string dir = backup_dir();
  for (auto &dp : docs_) {
    Document *d = dp.get();
    if (!d->modified()) {
      if (d->backup_written) remove_backup(d);
      continue;
    }
    if (d->backup_written && d->backup_state == d->undo.state() + (d->format_dirty ? 1u << 31 : 0)) continue;
    if (!all && now_seconds() - d->last_edit < 1.5) continue;  // still typing
    if (d->backup_id.empty()) {
      static int counter = 0;
      d->backup_id = format("%d-%d-%ld", int(getpid()), ++counter, long(time(nullptr)));
    }
    make_dirs(dir);
    Ini meta;
    meta.set("backup", "pid", std::to_string(getpid()));
    meta.set("backup", "path", d->path);
    meta.set("backup", "encoding", encoding_name(d->encoding));
    meta.set("backup", "eol", eol_name(d->eol));
    meta.set("backup", "language", language_id(d->lang));
    meta.set("backup", "language_manual", d->lang_manual ? "true" : "false");
    meta.set("backup", "clean_exit", quitting_ || all ? "true" : "false");
    meta.set("backup", "saved", std::to_string(time(nullptr)));
    std::string base = dir + "/" + d->backup_id;
    if (write_file_atomic(base + ".txt", d->text()) && write_file_atomic(base + ".meta", meta.serialize())) {
      chmod((base + ".txt").c_str(), 0600);
      d->backup_written = true;
      d->backup_state = d->undo.state() + (d->format_dirty ? 1u << 31 : 0);
    }
  }
}

void App::remove_backup(Document *doc) {
  if (!doc || doc->backup_id.empty()) return;
  std::string base = backup_dir() + "/" + doc->backup_id;
  unlink((base + ".txt").c_str());
  unlink((base + ".meta").c_str());
  doc->backup_written = false;
}

void App::backup_cb(void *p) {
  auto *self = static_cast<App *>(p);
  self->write_backups(false);
  Fl::repeat_timeout(5.0, backup_cb, p);
}

// ------------------------------------------------------- external changes

void App::check_disk(bool all) {
  if (!settings_.watch_files || modal()) return;
  bool changed = false;
  for (auto &dp : docs_) {
    Document *d = dp.get();
    if (!all && d != active()) continue;
    int st = d->disk_status();
    if (st == 0) continue;
    if (st == 2) {
      d->on_disk = false;
      d->notice = Document::Notice::Deleted;
      changed = true;
      continue;
    }
    if (!d->modified() && d->notice != Document::Notice::Changed) {
      int cur = d == editor_->doc() ? editor_->insert_position() : d->cursor;
      int top = d == editor_->doc() ? editor_->top_line() : d->top_line;
      std::string err;
      if (d->reload(err)) {
        d->cursor = std::min(cur, d->length());
        d->top_line = top;
        if (d == editor_->doc()) {
          editor_->attach(nullptr);
          editor_->attach(d);
        }
        if (!Fl::has_idle(idle_cb, this)) Fl::add_idle(idle_cb, this);
        toast(format(_("%s changed on disk and was reloaded"), d->display_name().c_str()));
      }
    } else {
      d->notice = Document::Notice::Changed;
      d->stat_disk();  // report this change once
    }
    changed = true;
  }
  if (changed) {
    layout();
    update_ui();
    redraw();
  }
}

void App::watch_cb(void *p) {
  auto *self = static_cast<App *>(p);
  self->check_disk(false);
  Fl::repeat_timeout(2.0, watch_cb, p);
}

void App::handle_notice(Document *doc, int button) {
  if (!doc) return;
  switch (doc->notice) {
    case Document::Notice::Changed:
      if (button == 0) {
        doc->undo.mark_saved();  // reload without asking again
        doc->format_dirty = false;
        std::string err;
        int cur = editor_->insert_position();
        if (doc->reload(err)) {
          doc->cursor = std::min(cur, doc->length());
          editor_->attach(nullptr);
          editor_->attach(doc);
          remove_backup(doc);
        }
      }
      doc->notice = Document::Notice::Clear;
      break;
    case Document::Notice::Deleted:
    case Document::Notice::Recovered:
      doc->notice = Document::Notice::Clear;
      if (button == 0) save(doc);
      break;
    default:
      doc->notice = Document::Notice::Clear;
      break;
  }
  layout();
  update_ui();
  redraw();
}

// ----------------------------------------------------------------- events

int App::x_event(int) {
  if (!instance_ || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    instance_->apply_theme();
    instance_->layout();
  } else if (e.type == FocusIn && instance_->shown() && e.xfocus.window == fl_xid(instance_)) {
    instance_->check_disk(true);
  }
  return 0;
}

int App::handle(int event) {
  switch (event) {
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      if (BufferedWindow::handle(event)) return 1;
      return 1;
    case FL_PASTE: {
      if (BufferedWindow::handle(event)) return 1;
      const char *t = Fl::event_text();
      if (!t) return 0;
      std::vector<std::string> files;
      for (auto &line : split(t, '\n')) {
        std::string p = uri_to_path(trim(line));
        if (!p.empty() && p[0] == '/') files.push_back(p);
      }
      if (!files.empty()) open_files(files, "");
      return 1;
    }
  }
  return BufferedWindow::handle(event);
}

void App::toast(const std::string &message) {
  toast_ = message;
  Fl::remove_timeout(toast_cb, this);
  Fl::add_timeout(3.0, toast_cb, this);
  if (status_) status_->redraw();
}

void App::toast_cb(void *p) {
  auto *self = static_cast<App *>(p);
  self->toast_.clear();
  self->status_->redraw();
}

void App::schedule_ui() {
  if (ui_pending_) return;
  ui_pending_ = true;
  Fl::add_timeout(0.0, ui_cb, this);
}

void App::ui_cb(void *p) {
  auto *self = static_cast<App *>(p);
  self->ui_pending_ = false;
  self->update_ui();
}

void App::update_ui() {
  Document *d = active();
  std::string title = "Niny";
  if (d) title = (d->modified() ? "• " : "") + d->display_name() + " — Niny";
  if (!label() || title != label()) copy_label(title.c_str());
  editor_->update_gutter();
  tabs_->redraw();
  toolbar_->redraw();
  status_->redraw();
  if (mdview_->visible()) mdview_->redraw();
  sidebar_->redraw();
  editor_->redraw();
}

void App::doc_changed(Document *doc) {
  if (doc == active()) {
    mdview_->invalidate();
    if (settings_.spell_check) schedule_spell_check();
  }
  if (doc == active() && find_->visible()) {
    Fl::remove_timeout(match_cb, this);
    Fl::add_timeout(doc->length() > (2 << 20) ? 0.4 : 0.1, match_cb, this);
  }
  if (doc->highlight_pending() && !Fl::has_idle(idle_cb, this)) Fl::add_idle(idle_cb, this);
  schedule_ui();
}

void App::doc_restyled(Document *doc, int start, int end) {
  if (doc == editor_->doc() && editor_->buffer() == doc->buf) editor_->redisplay_range(start, end);
}

void App::cursor_moved() { schedule_ui(); }

void App::idle_cb(void *p) {
  auto *self = static_cast<App *>(p);
  Document *a = self->active();
  if (a && a->highlight_pending()) {
    a->highlight_step(256 * 1024);
    return;
  }
  for (auto &d : self->docs_)
    if (d->highlight_pending()) {
      d->highlight_step(256 * 1024);
      return;
    }
  Fl::remove_idle(idle_cb, p);
}

// ----------------------------------------------------------------- search

bool App::find_visible() const { return find_ && find_->visible(); }

void App::show_find(bool replace) {
  if (!active()) return;
  palette_->close(false);
  std::string seed = selection_text();
  if (seed.size() > 200) seed.clear();
  find_origin_ = editor_->insert_position();
  int a, b;
  if (active()->buf->selection_position(&a, &b)) find_origin_ = a;
  find_->open(replace, seed);
  find_changed();
}

void App::hide_find() {
  if (!find_->visible()) return;
  find_->close();
  matches_.clear();
  match_lines_.clear();
  layout();
  if (active()) editor_->take_focus();
  redraw();
}

void App::find_changed() {
  Document *d = active();
  if (!d) return;
  std::string err;
  if (!searcher_.set(find_->query(), find_->options(), &err)) {
    matches_.clear();
    match_lines_.clear();
    find_->set_status(find_->query().empty() ? "" : _("Invalid"), !find_->query().empty());
    if (!err.empty()) find_->tooltip(nullptr);
    redraw();
    return;
  }
  Match m;
  if (searcher_.find(d->text(), size_t(std::max(0, find_origin_)), true, true, m))
    editor_->goto_pos(int(m.start), true, int(m.end));
  else
    editor_->goto_pos(std::min(std::max(0, find_origin_), d->length()), false);
  match_rev_ = ~0u;
  Fl::remove_timeout(match_cb, this);
  if (d->length() > (2 << 20)) Fl::add_timeout(0.15, match_cb, this);
  else refresh_matches();
}

void App::match_cb(void *p) { static_cast<App *>(p)->refresh_matches(); }

void App::refresh_matches() {
  Document *d = active();
  if (!d || !find_->visible() || !searcher_.valid()) {
    matches_.clear();
    match_lines_.clear();
    if (find_->visible()) find_->set_status("", false);
    return;
  }
  if (match_rev_ != d->revision || match_doc_ != d) {
    const std::string &text = d->text();
    const size_t limit = 20000;
    matches_ = searcher_.find_all(text, limit);
    match_lines_.clear();
    int line = 1;
    size_t pos = 0;
    for (size_t i = 0; i < matches_.size() && match_lines_.size() < 5000; ++i) {
      const char *s = text.data() + pos, *e = text.data() + matches_[i].start;
      while (s < e) {
        const void *nl = memchr(s, '\n', size_t(e - s));
        if (!nl) break;
        ++line;
        s = static_cast<const char *>(nl) + 1;
      }
      pos = matches_[i].start;
      if (match_lines_.empty() || match_lines_.back() != line) match_lines_.push_back(line);
    }
    match_rev_ = d->revision;
    match_doc_ = d;
  }
  int a = 0, b = 0, index = 0;
  if (d->buf->selection_position(&a, &b))
    for (size_t i = 0; i < matches_.size(); ++i)
      if (int(matches_[i].start) == a && int(matches_[i].end) == b) index = int(i) + 1;
  std::string s;
  if (matches_.empty()) s = _("No results");
  else if (index) s = format(_("%d of %d%s"), index, int(matches_.size()), matches_.size() >= 20000 ? "+" : "");
  else s = format(matches_.size() == 1 ? _("%d match") : _("%d matches"), int(matches_.size()));
  find_->set_status(s, matches_.empty());
  editor_->redraw();
}

void App::find_next(bool forward) {
  Document *d = active();
  if (!d) return;
  if (!searcher_.valid()) {
    show_find(false);
    return;
  }
  int a = 0, b = 0;
  int cur = editor_->insert_position();
  bool sel = d->buf->selection_position(&a, &b);
  size_t from = forward ? size_t(sel ? b : cur) : size_t(sel ? a : cur);
  Match m;
  if (searcher_.find(d->text(), from, forward, true, m)) {
    bool wrapped = forward ? m.start < from : m.end > from;
    editor_->goto_pos(int(m.start), true, int(m.end));
    find_origin_ = int(m.start);
    if (wrapped) toast(forward ? _("Search wrapped to the top") : _("Search wrapped to the bottom"));
  } else {
    toast(_("No results"));
  }
  refresh_matches();
}

void App::replace_one() {
  Document *d = active();
  if (!d || !searcher_.valid()) return;
  int a = 0, b = 0;
  if (d->buf->selection_position(&a, &b) && searcher_.matches_at(d->text(), size_t(a), size_t(b))) {
    std::string rep = searcher_.expand(d->text(), Match{size_t(a), size_t(b)}, find_->replacement());
    d->buf->replace(a, b, rep.c_str());
    editor_->insert_position(a + int(rep.size()));
    d->buf->unselect();
  }
  find_next(true);
}

void App::replace_all() {
  Document *d = active();
  if (!d || !searcher_.valid()) return;
  std::string out;
  size_t n = searcher_.replace_all(d->text(), find_->replacement(), out);
  if (n == 0) {
    toast(_("No results"));
    return;
  }
  int cur = editor_->insert_position();
  int top = editor_->top_line();
  d->undo.break_merge();
  d->buf->replace(0, d->length(), out.c_str());
  d->undo.break_merge();
  editor_->insert_position(std::min(cur, d->length()));
  editor_->scroll(top, 0);
  toast(format(n == 1 ? _("Replaced %d occurrence") : _("Replaced %d occurrences"), int(n)));
  refresh_matches();
}

// ------------------------------------------------------------------- view

void App::toggle_sidebar() {
  settings_.show_sidebar = !settings_.show_sidebar;
  layout();
  settings_.save(settings_path());
  redraw();
}

void App::set_wrap(bool on) {
  settings_.word_wrap = on;
  editor_->apply_wrap(on);
  settings_.save(settings_path());
  toast(on ? _("Word wrap on") : _("Word wrap off"));
  redraw();
}

void App::zoom(int delta) {
  settings_.zoom = delta == 0 ? 0 : std::max(-8, std::min(40, settings_.zoom + delta * S(1) * 2));
  editor_->apply_look();
  settings_.save(settings_path());
  int base = std::max(7, int(std::lround(settings_.font_size * 4.0 / 3.0 * ui_scale())));
  toast(format(_("Zoom %d%%"), int(std::lround(100.0 * (base + settings_.zoom) / base))));
  update_ui();
}

void App::show_palette(const std::string &prefix) {
  if (find_->visible() && prefix != ":") hide_find();
  palette_->open(prefix);
}

void App::goto_line(int line, int column) {
  Document *d = active();
  if (!d) return;
  int pos = d->buf->skip_lines(0, std::max(0, line - 1));
  if (column > 1) {
    int end = d->buf->line_end(pos);
    int col = 0, p = pos;
    while (p < end && col < column - 1) {
      p = d->buf->next_char(p);
      ++col;
    }
    pos = p;
  }
  editor_->goto_pos(pos, true);
}

void App::set_language(const Language *l) {
  Document *d = active();
  if (!d) return;
  d->set_language(l, true);
  if (!Fl::has_idle(idle_cb, this)) Fl::add_idle(idle_cb, this);
  toast(format(_("Language: %s"), language_name(l).c_str()));
  update_ui();
}

void App::set_encoding_for_save(Encoding e) {
  Document *d = active();
  if (!d || d->encoding == e) return;
  d->encoding = e;
  d->format_dirty = true;
  toast(format(_("%s will be used when saving"), encoding_name(e)));
  update_ui();
}

void App::set_eol(Eol e) {
  Document *d = active();
  if (!d || (d->eol == e && !d->mixed_eol)) return;
  d->eol = e;
  d->mixed_eol = false;
  d->format_dirty = true;
  toast(format(_("Line endings: %s"), eol_name(e)));
  update_ui();
}

void App::set_indent(bool tabs, int width) {
  Document *d = active();
  if (!d) return;
  d->use_tabs = tabs;
  d->indent_width = width;
  d->indent_manual = true;
  int cur = editor_->insert_position(), top = editor_->top_line();
  d->set_tab_width(width);
  editor_->insert_position(std::min(cur, d->length()));
  editor_->scroll(top, 0);
  editor_->redraw();
  update_ui();
}

}  // namespace niny
