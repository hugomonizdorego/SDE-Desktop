// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Xanara window: tabs and files (open, save, print), settings, the
// keyboard, dialogs and the application menu. The chrome (tab strip,
// toolbar and context toolbar) is laid out and drawn in chrome.cpp, the
// commands of the modes live in commands.cpp.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "process.h"
#include "spawn.h"
#include "util.h"
#include "x11.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

const Swatch kSwatches[] = {
    {N_("Yellow"), {1.0, 0.83, 0.23}}, {N_("Orange"), {1.0, 0.57, 0.17}}, {N_("Red"), {0.88, 0.19, 0.19}},
    {N_("Pink"), {0.90, 0.29, 0.50}},  {N_("Violet"), {0.47, 0.31, 0.95}}, {N_("Blue"), {0.11, 0.49, 0.84}},
    {N_("Green"), {0.18, 0.62, 0.27}}, {N_("Black"), {0.13, 0.15, 0.16}},
};
const int kSwatchCount = int(sizeof(kSwatches) / sizeof(kSwatches[0]));

XanaraWindow *XanaraWindow::instance_ = nullptr;

std::string display_name(const std::string &path) { return basename_of(path); }

namespace {

std::string session_file() { return state_home() + "/sde/xanara-session"; }
std::string bookmarks_file() { return state_home() + "/sde/xanara-bookmarks"; }

void style_input(Fl_Input *in, const Theme &t, bool paper) {
  in->box(FL_FLAT_BOX);
  in->color(paper ? Color::hex(0xFFFFFF).fl() : t.background.fl());
  in->textcolor(paper ? Color::hex(0x111111).fl() : t.text.fl());
  in->selection_color(t.accent.fl());
  in->cursor_color(t.accent.fl());
  in->textsize(t.font_size);
  in->textfont(FL_HELVETICA);
}

}  // namespace

// --------------------------------------------------------------- window

XanaraWindow::XanaraWindow(int w, int h) : BufferedWindow(w, h, "Xanara") {
  instance_ = this;
  theme_ = active_theme();
  history_.load();
  begin();
  home_ = new HomeView(this, 0, 0, w, h);
  sidebar_ = new Sidebar(this, 0, 0, S(300), h);
  props_ = new PropsPanel(this, 0, 0, S(260), h);
  page_input_ = new Fl_Input(0, 0, S(40), S(26));
  page_input_->when(FL_WHEN_ENTER_KEY_ALWAYS);
  page_input_->callback([](Fl_Widget *wd, void *p) {
    auto *self = static_cast<XanaraWindow *>(p);
    int n = atoi(static_cast<Fl_Input *>(wd)->value());
    if (self->view() && n >= 1) self->view()->go_to(std::min(n, self->document()->count()) - 1);
    if (self->view()) Fl::focus(self->view());
    self->update_chrome();
  }, this);
  find_input_ = new Fl_Input(0, 0, S(200), S(28));
  find_input_->when(FL_WHEN_ENTER_KEY_ALWAYS);
  find_input_->callback([](Fl_Widget *, void *p) { static_cast<XanaraWindow *>(p)->find_input_changed(); }, this);
  field_input_ = new Fl_Input(0, 0, S(100), S(24));
  field_input_->when(FL_WHEN_ENTER_KEY);
  field_input_->callback([](Fl_Widget *, void *p) { static_cast<XanaraWindow *>(p)->commit_field(); }, this);
  field_input_->hide();
  end();
  resizable(nullptr);
  size_range(S(560), S(380));
  xclass("Xanara");
  load_settings();
  apply_theme();
  layout();
  update_chrome();
  Fl::add_handler(x_event_handler);
  callback([](Fl_Widget *, void *p) { static_cast<XanaraWindow *>(p)->quit(); }, this);
}

XanaraWindow::~XanaraWindow() {
  remember_position();
  Fl::remove_handler(x_event_handler);
  Fl::remove_timeout(toast_cb, this);
  Fl::remove_timeout(remember_cb, this);
  if (Fl::has_timeout(settings_cb, this)) save_settings();
  Fl::remove_timeout(settings_cb, this);
  for (auto &t : tabs_) {
    t->view->set_document(nullptr);
    t->doc.reset();
    for (const auto &f : t->temps) unlink(f.c_str());
  }
  instance_ = nullptr;
}

void XanaraWindow::hide() {
  // FLTK also hides and re-shows the window to enter full screen without a
  // window manager, so this must not close documents; quit() does.
  BufferedWindow::hide();
}

void XanaraWindow::quit() {
  save_session();
  for (int i = tablist_.count() - 1; i >= 0; --i) {
    if (tabs_[size_t(i)]->dirty()) {
      activate_tab(i);
      if (!close_tab(i, true)) return;
    }
  }
  remember_position();
  hide();
}

DocView *XanaraWindow::view() const {
  int a = tablist_.active();
  return a >= 0 ? tabs_[size_t(a)]->view : nullptr;
}

Document *XanaraWindow::document() const {
  int a = tablist_.active();
  return a >= 0 ? tabs_[size_t(a)]->doc.get() : nullptr;
}

DocTab *XanaraWindow::tab() const {
  int a = tablist_.active();
  return a >= 0 ? tabs_[size_t(a)].get() : nullptr;
}

bool XanaraWindow::have(const std::string &program) {
  auto it = programs_.find(program);
  if (it != programs_.end()) return it->second;
  bool ok = !find_program(program).empty();
  programs_[program] = ok;
  return ok;
}

void XanaraWindow::set_mode(Mode m) {
  mode_ = m;
  if (DocView *v = view()) {
    // A tool of another mode is put down.
    v->set_tool(Tool::Select);
    if (m == Mode::Form) v->set_highlight_fields(true);
  }
  layout();
  update_chrome();
  settings_changed();
}

// ------------------------------------------------------------- settings

void XanaraWindow::load_settings() {
  Config cfg;
  cfg.load();
  sidebar_on_ = cfg.get_bool("xanara", "sidebar", true);
  sidebar_->set_panel_open(sidebar_on_);
  std::string panel = cfg.get("xanara", "sidebar_panel", "thumbnails");
  for (int k = 0; k < Sidebar::PanelCount; ++k)
    if (panel == Sidebar::panel_id(Sidebar::Panel(k))) sidebar_->set_panel(Sidebar::Panel(k));
  side_w_ = S(std::clamp(cfg.get_int("xanara", "sidebar_width", 260), 180, 480));
  props_on_ = cfg.get_bool("xanara", "properties", false);
  Mode m;
  if (mode_from_id(cfg.get("xanara", "mode", "annotate"), m)) mode_ = m;
  initials_ = cfg.get("xanara", "initials", "");
}

void XanaraWindow::save_settings() {
  Config cfg;
  cfg.load();
  if (DocView *v = view()) {
    Layout l = v->presentation() ? Layout::Continuous : v->layout_mode();
    cfg.set("xanara", "layout", l == Layout::Single ? "single" : l == Layout::Book ? "book" : "continuous");
    ColorMode m = v->color_mode();
    cfg.set("xanara", "reading_mode", m == ColorMode::Night ? "night" : m == ColorMode::Sepia ? "sepia" : "normal");
    cfg.set("xanara", "highlight_fields", v->highlight_fields() ? "true" : "false");
  }
  cfg.set("xanara", "sidebar", sidebar_on_ ? "true" : "false");
  cfg.set("xanara", "sidebar_panel", Sidebar::panel_id(sidebar_->panel()));
  cfg.set("xanara", "sidebar_width", std::to_string(int(std::lround(side_w_ / ui_scale()))));
  cfg.set("xanara", "properties", props_on_ ? "true" : "false");
  cfg.set("xanara", "mode", mode_id(mode_));
  if (!initials_.empty()) cfg.set("xanara", "initials", initials_);
  cfg.save();
}

void XanaraWindow::settings_changed() {
  Fl::remove_timeout(settings_cb, this);
  Fl::add_timeout(1.0, settings_cb, this);
}

void XanaraWindow::settings_cb(void *p) { static_cast<XanaraWindow *>(p)->save_settings(); }

void XanaraWindow::apply_theme() {
  Config cfg;
  cfg.load();
  double scale = ui_scale();
  theme_ = Theme::from_config(cfg);
  set_ui_scale(scale);
  theme_.scale = scale;
  theme_.apply_to_fltk();
  style_input(page_input_, theme_, false);
  style_input(find_input_, theme_, false);
  style_input(field_input_, theme_, true);
  page_input_->textsize(theme_.font_size - S(1));
  color(theme_.background.fl());
  redraw();
}

int XanaraWindow::x_event_handler(int) {
  if (!instance_ || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    instance_->apply_theme();
    instance_->layout();
  }
  return 0;
}

void XanaraWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void XanaraWindow::set_side_w(int w) {
  side_w_ = std::clamp(w, S(180), std::max(S(180), this->w() / 2));
  layout();
  settings_changed();
}

// ----------------------------------------------------------- notifications

void XanaraWindow::toast(const std::string &message) {
  toast_ = message;
  Fl::remove_timeout(toast_cb, this);
  Fl::add_timeout(3.2, toast_cb, this);
  redraw();
}

void XanaraWindow::toast_cb(void *p) {
  auto *self = static_cast<XanaraWindow *>(p);
  self->toast_.clear();
  self->redraw();
}

void XanaraWindow::page_changed() {
  if (DocView *v = view()) {
    sidebar_->page_changed(v->current_page());
    // Compare: the other document follows.
    int other = compare_id_ >= 0 ? tablist_.index_of_id(compare_id_) : -1;
    if (other >= 0 && other != tablist_.active()) {
      DocView *ov = tabs_[size_t(other)]->view;
      if (ov->current_page() != v->current_page() && v->current_page() < ov->document()->count())
        ov->go_to(v->current_page());
    }
  }
  update_chrome();
  Fl::remove_timeout(remember_cb, this);
  Fl::add_timeout(2.0, remember_cb, this);
}

void XanaraWindow::remember_cb(void *p) { static_cast<XanaraWindow *>(p)->remember_position(); }

void XanaraWindow::remember_position() {
  DocTab *t = tab();
  if (!t || t->target.empty()) return;
  HistoryEntry e;
  e.path = t->target;
  e.page = t->view->current_page();
  Fit f = t->view->fit();
  e.zoom = f == Fit::Width ? "width" : f == Fit::Page ? "page" : format("%.4f", t->view->zoom());
  history_.load();
  history_.remember(e);
  history_.save();
}

std::vector<HistoryEntry> XanaraWindow::recent() const { return history_.recent(24, true); }

void XanaraWindow::forget_recent(const std::string &path) {
  history_.load();
  history_.forget(path);
  history_.save();
  home_->refresh();
}

void XanaraWindow::tool_changed() {
  update_chrome();
  props_->redraw();
}

void XanaraWindow::document_edited() {
  sidebar_->notes_changed();
  update_chrome();
  props_->redraw();
}

void XanaraWindow::toggle_sidebar() {
  sidebar_on_ = !sidebar_on_;
  sidebar_->set_panel_open(sidebar_on_);
  layout();
  update_chrome();
  settings_changed();
}

void XanaraWindow::toggle_props() {
  props_on_ = !props_on_;
  layout();
  update_chrome();
  settings_changed();
}

void XanaraWindow::toggle_fullscreen() {
  if (!fullscreen_) {
    saved_x_ = x();
    saved_y_ = y();
    saved_w_ = w();
    saved_h_ = h();
    fullscreen();
  } else {
    fullscreen_off(saved_x_, saved_y_, saved_w_, saved_h_);
  }
  fullscreen_ = !fullscreen_;
}

void XanaraWindow::toggle_presentation() {
  DocView *v = view();
  if (!v) return;
  bool on = !v->presentation();
  commit_field();
  v->set_tool(Tool::Select);
  v->set_presentation(on);
  if (on != fullscreen_) toggle_fullscreen();
  layout();
  Fl::focus(v);
  if (on) toast(_("Presentation — arrows or click to advance, Esc to leave"));
  update_chrome();
}

void XanaraWindow::show_find(bool show) {
  if (!view()) return;
  if (show) {
    if (!sidebar_on_) {
      sidebar_on_ = true;
      sidebar_->set_panel_open(true);
    }
    sidebar_->set_panel(Sidebar::Search);
    layout();
    if (view()->has_selection()) {
      std::string s = view()->selection_text();
      if (!s.empty() && s.size() < 80 && s.find('\n') == std::string::npos) find_input_->value(s.c_str());
    }
    Fl::focus(find_input_);
    find_input_->position(0, find_input_->size());
  } else {
    view()->find_clear();
    Fl::focus(view());
  }
  update_chrome();
}

void XanaraWindow::find_input_changed() {
  DocView *v = view();
  if (!v) return;
  if (Fl::event_state() & FL_SHIFT) v->find_next(-1);
  else v->find(find_input_->value(), find_case_);
  sidebar_->search_changed();
  redraw();
}

void XanaraWindow::toggle_find_case() {
  find_case_ = !find_case_;
  if (DocView *v = view()) {
    v->find_clear();
    if (find_input_->size()) v->find(find_input_->value(), find_case_);
  }
  sidebar_->search_changed();
}

void XanaraWindow::cycle_color_mode() {
  DocView *v = view();
  if (!v) return;
  ColorMode m = v->color_mode();
  ColorMode next = m == ColorMode::Normal ? ColorMode::Night : m == ColorMode::Night ? ColorMode::Sepia : ColorMode::Normal;
  for (auto &t : tabs_) t->view->set_color_mode(next);
  toast(next == ColorMode::Night ? _("Night reading") : next == ColorMode::Sepia ? _("Sepia reading") : _("Normal colors"));
  settings_changed();
}

// ---------------------------------------------------------------- dialogs

bool XanaraWindow::ask_text(const std::string &title, std::string &text, bool multiline) {
  int dw = S(420), dh = multiline ? S(250) : S(150);
  BufferedWindow dlg(dw, dh, title.c_str());
  dlg.set_modal();
  dlg.color(theme_.surface.fl());
  dlg.begin();
  Fl_Input *in = new Fl_Input(S(16), S(46), dw - S(32), dh - S(46) - S(64));
  in->type(multiline ? FL_MULTILINE_INPUT : FL_NORMAL_INPUT);
  in->value(text.c_str());
  style_input(in, theme_, false);
  in->wrap(1);
  int result = 0;
  auto *cancel = new FlatButton(dw - S(16) - S(200) - S(8), dh - S(48), S(100), S(34), _("Cancel"));
  auto *ok = new FlatButton(dw - S(16) - S(100), dh - S(48), S(100), S(34), _("OK"), true);
  struct Ctx {
    int *result;
    Fl_Window *w;
  } ctx{&result, &dlg};
  cancel->callback([](Fl_Widget *, void *p) {
    auto *c = static_cast<Ctx *>(p);
    *c->result = 0;
    c->w->hide();
  }, &ctx);
  ok->callback([](Fl_Widget *, void *p) {
    auto *c = static_cast<Ctx *>(p);
    *c->result = 1;
    c->w->hide();
  }, &ctx);
  ok->shortcut(multiline ? FL_CTRL + FL_Enter : FL_Enter);
  dlg.end();
  struct Header : Fl_Widget {
    std::string t;
    const Theme *th;
    Header(int x, int y, int w, int h, std::string s, const Theme *theme) : Fl_Widget(x, y, w, h), t(s), th(theme) {}
    void draw() override {
      {
        Painter p(window()->w(), window()->h());
        p.fill_rect(0, 0, window()->w(), window()->h(), th->surface);
        p.stroke_rounded(S(15) + 0.5, S(45) + 0.5, window()->w() - S(30) - 1, window()->h() - S(108) - 1, S(4), th->border);
      }
      text_font(th->font_size + S(1), true);
      draw_text(t, x(), y(), w(), h(), th->text);
    }
  };
  dlg.insert(*new Header(S(16), S(10), dw - S(32), S(30), title, &theme_), 0);
  if (shown()) dlg.position(x() + (w() - dw) / 2, y() + (h() - dh) / 3);
  dlg.show();
  Fl::focus(in);
  while (dlg.shown()) Fl::wait();
  if (result) text = in->value();
  return result == 1;
}

bool XanaraWindow::ask_choice(const std::string &title, const std::string &message,
                              const std::vector<std::string> &options, int &choice) {
  // A popup menu of choices under a short explanation.
  std::vector<Fl_Menu_Item> items(options.size() + 2);
  memset(items.data(), 0, sizeof(Fl_Menu_Item) * items.size());
  items[0].text = message.c_str();
  items[0].flags = FL_MENU_INACTIVE | FL_MENU_DIVIDER;
  items[0].labelsize(theme_.font_size - S(1));
  for (size_t k = 0; k < options.size(); ++k) {
    items[k + 1].text = options[k].c_str();
    items[k + 1].labelsize(theme_.font_size);
    items[k + 1].labelcolor(theme_.text.fl());
  }
  (void)title;
  int mx = last_mx_ >= 0 ? last_mx_ : w() / 2, my = last_my_ >= 0 ? last_my_ : h() / 3;
  const Fl_Menu_Item *m = items.data()->popup(mx, my);
  if (!m || m == items.data()) return false;
  choice = int(m - items.data()) - 1;
  return true;
}

void XanaraWindow::about() {
  // The native UI kit's sde::show_about is not available yet.
  fl_message_title(_("About Xanara"));
  fl_message("%s", format(_("Xanara %s\nDocument viewer and PDF editor for SDE.\n\n"
                            "Developer: DebION Dev-Team\n"
                            "License: GPL-3.0-or-later.\nRendering by poppler %s; page tools by qpdf."),
                          SDE_APP_VERSION, poppler_get_version())
                       .c_str());
}

void XanaraWindow::show_menu(int mx, int my) {
  DocView *v = view();
  bool have_doc = v != nullptr;
  int dis = have_doc ? 0 : FL_MENU_INACTIVE;
  Layout lay = have_doc ? v->layout_mode() : Layout::Continuous;
  ColorMode cm = have_doc ? v->color_mode() : ColorMode::Normal;
  auto rec = history_.recent(10, true);
  std::vector<std::string> recent_labels;
  for (auto &e : rec) {
    std::string s = basename_of(e.path), esc;
    for (char c : s) {
      if (c == '/' || c == '\\' || c == '&' || c == '_') esc += '\\';
      esc += c;
    }
    recent_labels.push_back(esc);
  }
  std::vector<Fl_Menu_Item> items;
  auto item = [&](const char *text, long id, int flags = 0) {
    Fl_Menu_Item it;
    memset(&it, 0, sizeof it);
    it.text = text;
    it.user_data_ = reinterpret_cast<void *>(id);
    it.flags = flags;
    it.labelsize(theme_.font_size);
    it.labelcolor(theme_.text.fl());
    items.push_back(it);
  };
  auto end_sub = [&]() {
    Fl_Menu_Item it;
    memset(&it, 0, sizeof it);
    items.push_back(it);
  };
  enum {
    Open = 1, Save, SaveAs, Print, Close, Quit, Continuous, Single, Book, RotL, RotR, Normal, Night, Sepia,
    Present, Full, Side, Props, Fields, Undo, Redo, Sign, DrawSign, About, Home, Find, Recent0 = 1000
  };
  item(_("Open…"), Open);
  item(_("Open Recent"), 0, FL_SUBMENU | (rec.empty() ? FL_MENU_INACTIVE : 0));
  for (size_t k = 0; k < rec.size(); ++k) item(recent_labels[k].c_str(), Recent0 + long(k));
  end_sub();
  item(_("Save"), Save, dis);
  item(_("Save As…"), SaveAs, dis);
  item(_("Print…"), Print, dis | FL_MENU_DIVIDER);
  item(_("Find…"), Find, dis);
  item(_("View"), 0, FL_SUBMENU | dis);
  item(_("Continuous"), Continuous, FL_MENU_RADIO | (lay == Layout::Continuous ? FL_MENU_VALUE : 0));
  item(_("Single Page"), Single, FL_MENU_RADIO | (lay == Layout::Single ? FL_MENU_VALUE : 0));
  item(_("Two Pages (Book)"), Book, FL_MENU_RADIO | (lay == Layout::Book ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  item(_("Rotate View Left"), RotL);
  item(_("Rotate View Right"), RotR, FL_MENU_DIVIDER);
  item(_("Normal Colors"), Normal, FL_MENU_RADIO | (cm == ColorMode::Normal ? FL_MENU_VALUE : 0));
  item(_("Night Reading"), Night, FL_MENU_RADIO | (cm == ColorMode::Night ? FL_MENU_VALUE : 0));
  item(_("Sepia Reading"), Sepia, FL_MENU_RADIO | (cm == ColorMode::Sepia ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  item(_("Side Panel"), Side, FL_MENU_TOGGLE | (sidebar_on_ ? FL_MENU_VALUE : 0));
  item(_("Properties Panel"), Props, FL_MENU_TOGGLE | (props_on_ ? FL_MENU_VALUE : 0));
  item(_("Highlight Form Fields"), Fields,
       FL_MENU_TOGGLE | (have_doc && v->highlight_fields() ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  item(_("Presentation"), Present);
  item(_("Full Screen"), Full, FL_MENU_TOGGLE | (fullscreen_ ? FL_MENU_VALUE : 0));
  end_sub();
  item(_("Undo"), Undo, have_doc && v->can_undo() ? 0 : FL_MENU_INACTIVE);
  item(_("Redo"), Redo, (have_doc && v->can_redo() ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER);
  item(_("Choose Signature Image…"), Sign);
  item(_("Draw Signature…"), DrawSign, FL_MENU_DIVIDER);
  item(_("Home"), Home);
  item(_("Close Tab"), Close, dis);
  item(_("About Xanara"), About);
  item(_("Quit"), Quit);
  end_sub();
  const Fl_Menu_Item *m = items.data()->popup(std::max(0, mx), my);
  if (!m) return;
  long id = reinterpret_cast<long>(m->user_data());
  if (id >= Recent0 && id < Recent0 + 100) {
    open_path(rec[size_t(id - Recent0)].path);
    return;
  }
  switch (id) {
    case Open: open_dialog(); break;
    case Save: save(); break;
    case SaveAs: save_as(); break;
    case Print: print(); break;
    case Find: show_find(true); break;
    case Close: close_tab(tablist_.active(), true); break;
    case Home: activate_tab(-1); break;
    case Quit: quit(); break;
    case Continuous: v->set_layout(Layout::Continuous); break;
    case Single: v->set_layout(Layout::Single); break;
    case Book: v->set_layout(Layout::Book); break;
    case RotL: v->rotate(-90); break;
    case RotR: v->rotate(90); break;
    case Normal: case Night: case Sepia:
      for (auto &t : tabs_)
        t->view->set_color_mode(id == Normal ? ColorMode::Normal : id == Night ? ColorMode::Night : ColorMode::Sepia);
      break;
    case Side: toggle_sidebar(); break;
    case Props: toggle_props(); break;
    case Fields: v->set_highlight_fields(!v->highlight_fields()); break;
    case Present: toggle_presentation(); break;
    case Full: toggle_fullscreen(); break;
    case Undo: v->undo(); break;
    case Redo: v->redo(); break;
    case Sign: choose_signature(); break;
    case DrawSign: draw_signature(); break;
    case About: about(); break;
  }
  settings_changed();
}

std::string XanaraWindow::signatures_dir() const { return data_home() + "/sde/xanara/signatures"; }

void XanaraWindow::choose_signature() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.title(_("Choose a Signature Image"));
  chooser.filter(_("Images\t*.{png,PNG,jpg,jpeg,JPG,svg}"));
  chooser.directory(user_dir("PICTURES").c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  cairo_surface_t *img = load_image(chooser.filename(), 900, 300);
  if (!img) {
    toast(_("This image could not be read"));
    return;
  }
  // Keep a copy so it shows up in the Signatures panel next time.
  make_dirs(signatures_dir());
  std::string keep = signatures_dir() + "/" + format("signature-%ld.png", long(time(nullptr)));
  cairo_surface_write_to_png(img, keep.c_str());
  sidebar_->notes_changed();
  if (DocView *v = view()) {
    v->set_signature(img);
    v->set_tool(Tool::Signature);
    toast(_("Click on the page to place the signature"));
  } else {
    cairo_surface_destroy(img);
  }
}

namespace {

// A pad to draw a signature with the mouse or a pen tablet.
class SignaturePad : public Fl_Widget {
 public:
  SignaturePad(int x, int y, int w, int h, const Theme *t) : Fl_Widget(x, y, w, h), th_(t) {}
  std::vector<std::vector<Point>> strokes;
  void draw() override {
    Painter p(window()->w(), window()->h());
    p.fill_rect(0, 0, window()->w(), window()->h(), th_->surface);
    p.fill_rounded(x(), y(), w(), h(), S(8), Color::hex(0xFFFFFF));
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(8), th_->border);
    p.line(x() + S(20), y() + h() - S(36), x() + w() - S(20), y() + h() - S(36), Color::hex(0xC5CAD3));
    cairo_t *cr = p.cr();
    cairo_set_source_rgb(cr, 0.08, 0.12, 0.25);
    cairo_set_line_width(cr, Sd(2.4));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (auto &s : strokes) {
      for (size_t i = 0; i < s.size(); ++i) {
        if (i == 0) cairo_move_to(cr, x() + s[i].x, y() + s[i].y);
        else cairo_line_to(cr, x() + s[i].x, y() + s[i].y);
      }
      if (s.size() == 1) cairo_line_to(cr, x() + s[0].x + 0.1, y() + s[0].y);
      cairo_stroke(cr);
    }
  }
  int handle(int e) override {
    if (e == FL_PUSH) {
      strokes.push_back({Point{double(Fl::event_x() - x()), double(Fl::event_y() - y())}});
      redraw();
      return 1;
    }
    if (e == FL_DRAG && !strokes.empty()) {
      strokes.back().push_back(Point{double(Fl::event_x() - x()), double(Fl::event_y() - y())});
      redraw();
      return 1;
    }
    return e == FL_RELEASE ? 1 : Fl_Widget::handle(e);
  }

 private:
  const Theme *th_;
};

}  // namespace

void XanaraWindow::draw_signature() {
  int dw = S(520), dh = S(300);
  BufferedWindow dlg(dw, dh, _("Draw Your Signature"));
  dlg.set_modal();
  dlg.begin();
  auto *pad = new SignaturePad(S(16), S(16), dw - S(32), dh - S(84), &theme_);
  int result = 0;
  auto *clear = new FlatButton(S(16), dh - S(52), S(100), S(36), _("Clear"));
  auto *cancel = new FlatButton(dw - S(16) - S(216), dh - S(52), S(100), S(36), _("Cancel"));
  auto *ok = new FlatButton(dw - S(16) - S(108), dh - S(52), S(108), S(36), _("Use"), true);
  struct Ctx {
    int *result;
    Fl_Window *w;
    SignaturePad *pad;
  } ctx{&result, &dlg, pad};
  clear->callback([](Fl_Widget *, void *p) {
    auto *c = static_cast<Ctx *>(p);
    c->pad->strokes.clear();
    c->pad->redraw();
  }, &ctx);
  cancel->callback([](Fl_Widget *, void *p) { static_cast<Ctx *>(p)->w->hide(); }, &ctx);
  ok->callback([](Fl_Widget *, void *p) {
    auto *c = static_cast<Ctx *>(p);
    *c->result = 1;
    c->w->hide();
  }, &ctx);
  dlg.end();
  if (shown()) dlg.position(x() + (w() - dw) / 2, y() + (h() - dh) / 3);
  dlg.show();
  while (dlg.shown()) Fl::wait();
  if (!result || pad->strokes.empty()) return;
  // Crop to the ink and render at 3x for a crisp stamp.
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (auto &s : pad->strokes)
    for (auto &pt : s) {
      x0 = std::min(x0, pt.x);
      y0 = std::min(y0, pt.y);
      x1 = std::max(x1, pt.x);
      y1 = std::max(y1, pt.y);
    }
  double pad_px = 6, k = 3.0 / ui_scale();
  int iw = int((x1 - x0 + 2 * pad_px) * k) + 1, ih = int((y1 - y0 + 2 * pad_px) * k) + 1;
  cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
  cairo_t *cr = cairo_create(img);
  cairo_scale(cr, k, k);
  cairo_translate(cr, -x0 + pad_px, -y0 + pad_px);
  cairo_set_source_rgb(cr, 0.08, 0.12, 0.25);
  cairo_set_line_width(cr, Sd(2.4));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  for (auto &s : pad->strokes) {
    for (size_t i = 0; i < s.size(); ++i) {
      if (i == 0) cairo_move_to(cr, s[i].x, s[i].y);
      else cairo_line_to(cr, s[i].x, s[i].y);
    }
    cairo_stroke(cr);
  }
  cairo_destroy(cr);
  cairo_surface_flush(img);
  make_dirs(signatures_dir());
  std::string keep = signatures_dir() + "/" + format("signature-%ld.png", long(time(nullptr)));
  cairo_surface_write_to_png(img, keep.c_str());
  sidebar_->notes_changed();
  if (DocView *v = view()) {
    v->set_signature(img);
    v->set_tool(Tool::Signature);
    toast(_("Click on the page to place the signature"));
  } else {
    cairo_surface_destroy(img);
  }
}

std::string XanaraWindow::initials() {
  if (initials_.empty()) {
    std::string t;
    if (!ask_text(_("Your Initials"), t, false) || trim(t).empty()) return "";
    initials_ = trim(t);
    settings_changed();
  }
  return initials_;
}

// -------------------------------------------------------------- bookmarks

std::vector<int> XanaraWindow::bookmarks(const std::string &path) {
  std::vector<int> out;
  std::string text;
  if (!read_file(bookmarks_file(), text)) return out;
  for (const auto &line : split(text, '\n')) {
    size_t tab = line.rfind('\t');
    if (tab == std::string::npos || line.substr(0, tab) != path) continue;
    out.push_back(atoi(line.c_str() + tab + 1));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void XanaraWindow::toggle_bookmark() {
  DocTab *t = tab();
  if (!t) return;
  int page = t->view->current_page();
  std::string text, out;
  read_file(bookmarks_file(), text);
  bool removed = false;
  for (const auto &line : split(text, '\n')) {
    if (line == t->target + "\t" + std::to_string(page)) {
      removed = true;
      continue;
    }
    out += line + "\n";
  }
  if (!removed) out += t->target + "\t" + std::to_string(page) + "\n";
  make_dirs(state_home() + "/sde");
  write_file_atomic(bookmarks_file(), out);
  toast(removed ? format(_("Bookmark on page %d removed"), page + 1) : format(_("Page %d bookmarked"), page + 1));
  sidebar_->notes_changed();
  update_chrome();
}

// ------------------------------------------------------------------- forms

void XanaraWindow::edit_field(int page, const FormField &f, int fx, int fy, int fw, int fh) {
  commit_field();
  Document *d = document();
  if (!d) return;
  field_ = POPPLER_FORM_FIELD(g_object_ref(f.field));
  field_page_ = page;
  field_view_ = view();
  std::string value;
  bool multiline = false;
  {
    std::lock_guard<std::mutex> lock(d->mutex());
    gchar *t = poppler_form_field_text_get_text(field_);
    value = t ? t : "";
    g_free(t);
    multiline = poppler_form_field_text_get_text_type(field_) == POPPLER_FORM_TEXT_MULTILINE;
  }
  field_input_->type(multiline ? FL_MULTILINE_INPUT : FL_NORMAL_INPUT);
  field_input_->when(multiline ? FL_WHEN_NEVER : FL_WHEN_ENTER_KEY);
  field_input_->resize(fx, fy, fw, fh);
  field_input_->textsize(std::max(S(10), std::min(fh - S(6), theme_.font_size + S(2))));
  field_input_->value(value.c_str());
  field_input_->show();
  Fl::focus(field_input_);
  field_input_->position(field_input_->size(), 0);
  redraw();
}

void XanaraWindow::commit_field() {
  if (!field_input_->visible() || !field_) return;
  std::string v = field_input_->value();
  field_input_->hide();
  for (auto &t : tabs_)
    if (t->view == field_view_ && t->doc->set_field_text(field_page_, field_, v)) t->view->edited(field_page_);
  g_object_unref(field_);
  field_ = nullptr;
  field_view_ = nullptr;
  redraw();
}

// ---------------------------------------------------------------- files

bool XanaraWindow::replace_document(DocTab &t, const std::string &path, const std::string &password, bool working,
                                    int page, bool keep_view) {
  std::string error;
  bool encrypted = false;
  auto d = Document::open(path, password, error, encrypted);
  if (!d) {
    fl_alert("%s", format(_("Xanara could not open “%s”:\n%s"), display_name(path).c_str(), error.c_str()).c_str());
    return false;
  }
  DocView *v = t.view;
  Fit fit = v->fit();
  double zoom = v->zoom(), ssx, ssy;
  v->scroll_pos(ssx, ssy);
  v->set_document(nullptr);
  t.doc = std::move(d);
  t.working_copy = working;
  v->set_document(t.doc.get());
  if (keep_view) {
    if (fit != Fit::Free) v->set_fit(fit);
    else v->set_zoom(zoom);
  }
  if (v == view()) sidebar_->document_changed();
  layout();
  v->go_to(std::clamp(page, 0, t.doc->count() - 1));
  // After saving, the pages are the same: stay exactly where the reader was.
  if (keep_view && !working) v->set_scroll_pos(ssx, ssy);
  update_chrome();
  return true;
}

bool XanaraWindow::open_path(const std::string &path0, int page) {
  std::string path = path0;
  int existing = tablist_.find(path);
  if (existing >= 0) {
    activate_tab(existing);
    if (page >= 0) view()->go_to(page);
    return true;
  }
  if (!file_exists(path)) {
    fl_alert("%s", format(_("“%s” does not exist."), path.c_str()).c_str());
    forget_recent(path);
    return false;
  }
  std::string password, error;
  bool encrypted = false;
  auto d = Document::open(path, "", error, encrypted);
  for (int tries = 0; !d && encrypted && tries < 3; ++tries) {
    fl_message_title(_("Password Required"));
    const char *pw = fl_password("%s", "",
                                 format(tries ? _("The password for “%s” was not right. Try again:")
                                              : _("“%s” is protected. Enter its password:"),
                                        display_name(path).c_str())
                                     .c_str());
    if (!pw) return false;
    password = pw;
    d = Document::open(path, password, error, encrypted);
  }
  if (!d) {
    if (!encrypted)
      fl_alert("%s", format(_("Xanara could not open “%s”:\n%s"), display_name(path).c_str(), error.c_str()).c_str());
    return false;
  }
  commit_field();
  auto t = std::make_unique<DocTab>();
  t->doc = std::move(d);
  t->target = path;
  begin();
  t->view = new DocView(this, 0, 0, w(), h());
  end();
  // New tabs read like the last one.
  if (DocView *prev = view()) {
    t->view->set_color_mode(prev->color_mode());
    t->view->set_layout(prev->layout_mode());
    t->view->set_highlight_fields(prev->highlight_fields());
  } else {
    Config cfg;
    cfg.load();
    std::string l = cfg.get("xanara", "layout", "continuous");
    t->view->set_layout(l == "single" ? Layout::Single : l == "book" ? Layout::Book : Layout::Continuous);
    std::string m = cfg.get("xanara", "reading_mode", "normal");
    t->view->set_color_mode(m == "night" ? ColorMode::Night : m == "sepia" ? ColorMode::Sepia : ColorMode::Normal);
    t->view->set_highlight_fields(cfg.get_bool("xanara", "highlight_fields", true));
    int mb = cfg.get_int("xanara", "cache_mb", 224);
    t->view->set_cache_budget(size_t(std::clamp(mb, 32, 2048)) << 20);
  }
  t->view->hide();
  t->view->set_document(t->doc.get());
  int at = tablist_.open(path);
  tabs_.insert(tabs_.begin() + at, std::move(t));
  activate_tab(at);
  DocView *v = view();
  // Where the reader left off.
  int start = 0;
  history_.load();
  if (const HistoryEntry *e = history_.find(path)) {
    start = e->page;
    if (e->zoom == "page") v->set_fit(Fit::Page);
    else if (e->zoom == "width") v->set_fit(Fit::Width);
    else if (atof(e->zoom.c_str()) > 0) v->set_zoom(atof(e->zoom.c_str()));
  } else {
    v->set_fit(Fit::Width);
  }
  if (page >= 0) start = page;
  v->go_to(std::clamp(start, 0, document()->count() - 1));
  remember_position();
  home_->refresh();
  save_session();
  Fl::focus(v);
  update_chrome();
  return true;
}

void XanaraWindow::activate_tab(int index) {
  commit_field();
  if (DocView *old = view()) {
    remember_position();
    old->hide();
  }
  tablist_.activate(index);
  for (auto &t : tabs_)
    if (t->view != view()) t->view->hide();
  if (DocView *v = view()) {
    v->show();
    Fl::focus(v);
    find_input_->value(v->find_query().c_str());
  }
  sidebar_->document_changed();
  if (DocView *v = view()) sidebar_->page_changed(v->current_page());
  home_->refresh();
  layout();
  update_chrome();
}

void XanaraWindow::open_dialog() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
  chooser.title(_("Open Documents"));
  chooser.filter(_("PDF Documents\t*.{pdf,PDF}"));
  DocTab *t = tab();
  std::string dir = !t ? user_dir("DOCUMENTS") : t->target.substr(0, t->target.rfind('/'));
  if (dir.empty() || !dir_exists(dir)) dir = home_dir();
  chooser.directory(dir.c_str());
  if (chooser.show() != 0) return;
  for (int k = 0; k < chooser.count(); ++k) open_path(path_from_argument(chooser.filename(k)));
}

bool XanaraWindow::close_tab(int index, bool ask) {
  if (index < 0 || index >= tablist_.count()) return true;
  DocTab &t = *tabs_[size_t(index)];
  commit_field();
  if (ask && t.dirty()) {
    if (index != tablist_.active()) activate_tab(index);
    fl_message_title(_("Unsaved Changes"));
    int c = fl_choice("%s", _("Cancel"), _("Don't Save"), _("Save"),
                      format(_("Save the changes to “%s” before closing?\n"
                               "Without saving, your annotations and edits are lost."),
                             display_name(t.target).c_str())
                          .c_str());
    if (c == 0) return false;
    if (c == 2 && !save()) return false;
  }
  if (index == tablist_.active()) remember_position();
  DocView *v = t.view;
  v->set_document(nullptr);
  t.doc.reset();
  for (const auto &f : t.temps) unlink(f.c_str());
  remove(v);
  Fl::delete_widget(v);
  tabs_.erase(tabs_.begin() + index);
  int was_active = tablist_.active();
  int now = tablist_.close(index);
  // tablist_.close() already moved the active index; make it visible.
  tablist_.activate(-1);
  (void)was_active;
  activate_tab(now);
  save_session();
  return true;
}

bool XanaraWindow::save() {
  DocTab *t = tab();
  if (!t) return false;
  commit_field();
  if (t->target.empty()) return save_as();
  if (!t->dirty()) {
    toast(_("No changes to save"));
    return true;
  }
  std::string error;
  if (!t->doc->save_to(t->target, error)) {
    fl_alert("%s", format(_("“%s” could not be saved:\n%s\n\nThe original file was not changed."),
                          display_name(t->target).c_str(), error.c_str())
                       .c_str());
    return false;
  }
  int page = t->view->current_page();
  std::string pw = t->doc->password();
  for (const auto &f : t->temps) unlink(f.c_str());
  t->temps.clear();
  t->view->forget_history();
  replace_document(*t, t->target, pw, false, page, true);
  remember_position();
  toast(format(_("Saved “%s”"), display_name(t->target).c_str()));
  return true;
}

bool XanaraWindow::save_as() {
  DocTab *t = tab();
  if (!t) return false;
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
  chooser.title(_("Save a Copy As"));
  chooser.filter(_("PDF Documents\t*.pdf"));
  chooser.directory(t->target.substr(0, t->target.rfind('/')).c_str());
  chooser.preset_file(display_name(t->target).c_str());
  if (chooser.show() != 0 || !chooser.filename()) return false;
  std::string path = chooser.filename();
  if (!ends_with(lower(path), ".pdf")) path += ".pdf";
  std::string error;
  commit_field();
  if (!t->doc->save_to(path, error)) {
    fl_alert("%s", format(_("“%s” could not be saved:\n%s"), display_name(path).c_str(), error.c_str()).c_str());
    return false;
  }
  int page = t->view->current_page();
  std::string pw = t->doc->password();
  t->target = path;
  tablist_.rename(tablist_.active(), path);
  for (const auto &f : t->temps) unlink(f.c_str());
  t->temps.clear();
  t->view->forget_history();
  replace_document(*t, t->target, pw, false, page, true);
  remember_position();
  save_session();
  toast(format(_("Saved “%s”"), display_name(t->target).c_str()));
  return true;
}

std::string XanaraWindow::temp_path(const std::string &tag) {
  static int counter = 0;
  std::string dir = runtime_dir();
  make_dirs(dir);
  std::string p = format("%s/xanara-%d-%d-%s", dir.c_str(), int(getpid()), ++counter, tag.c_str());
  if (tag.find('.') == std::string::npos) p += ".pdf";
  if (DocTab *t = tab()) t->temps.push_back(p);
  return p;
}

void XanaraWindow::print() {
  DocTab *t = tab();
  if (!t) return;
  if (!have("lp")) {
    fl_alert("%s", _("Printing needs the CUPS client (the lp command).\nInstall it with: sudo apt install cups-client"));
    return;
  }
  std::string file = t->doc->path();
  if (t->doc->modified()) {
    std::string error;
    file = temp_path("print");
    if (!t->doc->save_to(file, error)) {
      fl_alert("%s", error.c_str());
      return;
    }
  }
  fl_message_title(_("Print"));
  if (fl_choice("%s", _("Cancel"), _("Print"), nullptr,
                format(_("Print “%s” (%d pages) on the default printer?"), display_name(t->target).c_str(),
                       t->doc->count())
                    .c_str()) != 1)
    return;
  run_async(lp_args(file, display_name(t->target)), [this](const std::string &, int status) {
    if (instance_ != this) return;
    toast(status == 0 ? std::string(_("Sent to the printer")) : std::string(_("Printing failed — is a printer set up?")));
  });
}

void XanaraWindow::save_session() {
  make_dirs(state_home() + "/sde");
  write_file_atomic(session_file(), tablist_.serialize());
}

// ------------------------------------------------------------------ events

int XanaraWindow::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  if (event == FL_PUSH || event == FL_MOVE) {
    last_mx_ = mx;
    last_my_ = my;
  }
  DocView *v = view();
  switch (event) {
    case FL_MOVE:
    case FL_ENTER: {
      int k = hot_at(mx, my);
      if (k != hover_hot_) {
        hover_hot_ = k;
        tip_.clear();
        if (k >= 0) {
          const Hot &b = hots_[size_t(k)];
          tip_ = b.tip;
          tip_x_ = b.x + b.w / 2;
          tip_y_ = b.y + b.h + S(6);
          if (tip_y_ + S(30) > h()) tip_y_ = b.y - S(34);
        }
        redraw();
      }
      if (k >= 0 || my < tabs_h_ + bar_h_ + ctx_h_) {
        cursor(FL_CURSOR_DEFAULT);
        return 1;
      }
      break;
    }
    case FL_LEAVE:
      if (hover_hot_ >= 0 || !tip_.empty()) {
        hover_hot_ = -1;
        tip_.clear();
        redraw();
      }
      break;
    case FL_PUSH: {
      int k = hot_at(mx, my);
      if (k >= 0) {
        const Hot &b = hots_[size_t(k)];
        press_hot_ = k;
        tip_.clear();
        if (b.kind == Hot::Tab && Fl::event_button() == FL_LEFT_MOUSE) {
          drag_tab_ = b.tab;
          drag_tab_x_ = mx;
          drag_tab_to_ = b.tab;
        }
        if (b.kind == Hot::Tab && Fl::event_button() == FL_MIDDLE_MOUSE) {
          press_hot_ = -1;
          close_tab(b.tab, true);
          return 1;
        }
        redraw();
        return 1;
      }
      if (my < tabs_h_ + bar_h_ + ctx_h_) {
        // A double click on the empty tab strip opens a file.
        if (my < tabs_h_ && Fl::event_clicks() > 0) open_dialog();
        return 1;
      }
      break;
    }
    case FL_DRAG:
      if (drag_tab_ >= 0) {
        if (std::abs(mx - drag_tab_x_) > S(6)) {
          drag_tab_to_ = tab_drop_index(mx, tabs_x0_, tab_w_, tablist_.count(), drag_tab_);
          redraw();
        }
        return 1;
      }
      if (press_hot_ >= 0) return 1;
      break;
    case FL_RELEASE:
      if (drag_tab_ >= 0) {
        int from = drag_tab_, to = drag_tab_to_;
        bool moved = std::abs(mx - drag_tab_x_) > S(6);
        drag_tab_ = -1;
        if (moved && to >= 0 && to != from) {
          // Keep the tab objects parallel to the list.
          auto obj = std::move(tabs_[size_t(from)]);
          tabs_.erase(tabs_.begin() + from);
          tabs_.insert(tabs_.begin() + to, std::move(obj));
          tablist_.move(from, to);
          tablist_.activate(-1);
          activate_tab(to);
          save_session();
          press_hot_ = -1;
          return 1;
        }
      }
      if (press_hot_ >= 0) {
        int k = press_hot_;
        press_hot_ = -1;
        if (hot_at(mx, my) == k && k < int(hots_.size()) && hots_[size_t(k)].enabled && hots_[size_t(k)].action) {
          auto fn = hots_[size_t(k)].action;
          fn();
        }
        redraw();
        return 1;
      }
      break;
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      int st = Fl::event_state();
      bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT;
      Fl_Widget *f = Fl::focus();
      bool typing = f == page_input_ || f == find_input_ || f == field_input_;
      if (key == FL_Escape) {
        if (f == field_input_) {
          field_input_->hide();
          if (field_) g_object_unref(field_);
          field_ = nullptr;
          if (v) Fl::focus(v);
          redraw();
          return 1;
        }
        if (v && v->presentation()) {
          toggle_presentation();
          return 1;
        }
        if (f == find_input_ || f == page_input_) {
          if (v) Fl::focus(v);
          update_chrome();
          return 1;
        }
        if (fullscreen_ && v && !v->has_selection() && v->tool() == Tool::Select && !v->has_selected_annot()) {
          toggle_fullscreen();
          return 1;
        }
        if (v) v->handle(FL_KEYBOARD);
        return 1;  // never close the window on Escape
      }
      if (ctrl && (st & FL_ALT) && key >= '1' && key <= '8') {
        set_mode(Mode(key - '1'));
        return 1;
      }
      if (ctrl) {
        switch (key) {
          case 'o': open_dialog(); return 1;
          case 's':
            if (shift) save_as();
            else save();
            return 1;
          case 'p': print(); return 1;
          case 'f': show_find(true); return 1;
          case 'd': toggle_bookmark(); return 1;
          case 'g':
            if (v) v->find_next(shift ? -1 : 1);
            return 1;
          case 'w': close_tab(tablist_.active(), true); return 1;
          case 't': open_dialog(); return 1;
          case 'q': quit(); return 1;
          case FL_Tab:
          case FL_Page_Down:
          case FL_Page_Up: {
            int n = tablist_.count() + 1;  // with Home
            int cur = tablist_.active() + 1;
            int dir = (key == FL_Page_Up || (key == FL_Tab && shift)) ? -1 : 1;
            activate_tab((cur + dir + n) % n - 1);
            return 1;
          }
          case 'z':
            if (!typing && v) {
              if (shift) v->redo();
              else v->undo();
              return 1;
            }
            break;
          case 'y':
            if (!typing && v) {
              v->redo();
              return 1;
            }
            break;
          case '0': if (v) v->set_zoom(1.0); return 1;
          case '1': if (v) v->set_fit(Fit::Page); return 1;
          case '2': if (v) v->set_fit(Fit::Width); return 1;
          case '+': case '=': case FL_KP + '+': if (v) v->zoom_step(1); return 1;
          case '-': case FL_KP + '-': if (v) v->zoom_step(-1); return 1;
          case 'l':
            if (v && page_input_->visible()) {
              Fl::focus(page_input_);
              page_input_->position(0, page_input_->size());
              redraw();
            }
            return 1;
          case 'r': if (v) v->rotate(shift ? -90 : 90); return 1;
          case 'm': cycle_color_mode(); return 1;
        }
      }
      if (key == FL_F + 9) {
        toggle_sidebar();
        return 1;
      }
      if (key == FL_F + 10 && !ctrl) {
        toggle_props();
        return 1;
      }
      if (key == FL_F + 5) {
        toggle_presentation();
        return 1;
      }
      if (key == FL_F + 11) {
        toggle_fullscreen();
        return 1;
      }
      if (key == FL_F + 3) {
        if (v) v->find_next(shift ? -1 : 1);
        return 1;
      }
      if (typing) break;
      if (!ctrl && !(st & FL_ALT) && v && !v->presentation()) {
        Act a = Act::Count;
        Tool t = Tool::Count;
        switch (key) {
          case 'v': t = Tool::Select; break;
          case ' ':
            break;
          case 'h': a = Act::Highlight; break;
          case 'u': a = Act::Underline; break;
          case 'q': a = Act::Squiggly; break;
          case 'k': a = Act::Strike; break;
          case 'n': a = Act::Note; break;
          case 't': a = Act::TextBox; break;
          case 'p': a = Act::Pen; break;
          case 'e': a = Act::Eraser; break;
          case 'r': a = Act::Rect; break;
          case 'o': a = Act::Ellipse; break;
          case 'a': a = Act::Arrow; break;
          case 'l': a = Act::Line; break;
          case 'm': a = Act::Measure; break;
          case 'g': a = Act::Signature; break;
        }
        if (a != Act::Count) {
          if (mode_ != Mode::Annotate) set_mode(Mode::Annotate);
          run(a);
          return 1;
        }
        if (t == Tool::Select) {
          v->set_tool(t);
          return 1;
        }
      }
      if (event == FL_SHORTCUT && v && f != v && v->handle(FL_KEYBOARD)) return 1;
      break;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      return 1;
    case FL_PASTE: {
      std::string text(Fl::event_text(), size_t(Fl::event_length()));
      for (auto &line : split(text, '\n')) {
        std::string fl = trim(line);
        if (!fl.empty()) open_path(path_from_argument(fl));
      }
      return 1;
    }
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_FOCUS || event == FL_UNFOCUS || event == FL_PUSH) redraw();
  return r;
}

}  // namespace xanara
