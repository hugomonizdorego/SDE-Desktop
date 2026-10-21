// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Xanara window: command bar, tool strip, find bar, files (open, save,
// print), page operations through qpdf, dialogs and settings.
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

enum Group { GFile = 1, GPage, GZoom, GTools, GEnd, GStrip = 10, GSwatch, GWidth, GStripEnd, GFind = 20 };

const char *tool_name(Tool t) {
  switch (t) {
    case Tool::Select: return _("Select");
    case Tool::Highlight: return _("Highlight");
    case Tool::Underline: return _("Underline");
    case Tool::Strike: return _("Strike Out");
    case Tool::Note: return _("Sticky Note");
    case Tool::Text: return _("Text Box");
    case Tool::Pen: return _("Pen");
    case Tool::Rect: return _("Rectangle");
    case Tool::Line: return _("Line");
    case Tool::Signature: return _("Signature");
  }
  return "";
}

const char *tool_hint(Tool t) {
  switch (t) {
    case Tool::Highlight:
    case Tool::Underline:
    case Tool::Strike: return _("Drag across text");
    case Tool::Note: return _("Click where the note belongs");
    case Tool::Text: return _("Drag a box, then type");
    case Tool::Pen: return _("Draw on the page");
    case Tool::Rect: return _("Drag to draw a rectangle");
    case Tool::Line: return _("Drag to draw a line");
    case Tool::Signature: return _("Click to place your signature image");
    default: return "";
  }
}

bool same_rgb(Rgb a, Rgb b) {
  return std::fabs(a.r - b.r) < 0.02 && std::fabs(a.g - b.g) < 0.02 && std::fabs(a.b - b.b) < 0.02;
}

std::string zoom_text(double z) { return format("%d%%", int(std::lround(z * 100))); }

}  // namespace

// --------------------------------------------------------------- window

XanaraWindow::XanaraWindow(int w, int h) : BufferedWindow(w, h, "Xanara") {
  instance_ = this;
  theme_ = active_theme();
  history_.load();
  begin();
  view_ = new DocView(this, 0, 0, w, h);
  sidebar_ = new Sidebar(this, 0, 0, S(220), h);
  page_input_ = new Fl_Input(0, 0, S(44), S(26));
  page_input_->when(FL_WHEN_ENTER_KEY_ALWAYS);
  page_input_->callback([](Fl_Widget *wd, void *p) {
    auto *self = static_cast<XanaraWindow *>(p);
    int n = atoi(static_cast<Fl_Input *>(wd)->value());
    if (self->doc_ && n >= 1) self->view_->go_to(std::min(n, self->doc_->count()) - 1);
    Fl::focus(self->view_);
    self->update_chrome();
  }, this);
  find_input_ = new Fl_Input(0, 0, S(200), S(26));
  find_input_->when(FL_WHEN_ENTER_KEY_ALWAYS);
  find_input_->callback([](Fl_Widget *wd, void *p) {
    auto *self = static_cast<XanaraWindow *>(p);
    bool back = Fl::event_state() & FL_SHIFT;
    if (back) self->view_->find_next(-1);
    else self->view_->find(static_cast<Fl_Input *>(wd)->value(), self->find_case_);
    self->redraw();
  }, this);
  find_input_->hide();
  field_input_ = new Fl_Input(0, 0, S(100), S(24));
  field_input_->when(FL_WHEN_ENTER_KEY);
  field_input_->callback([](Fl_Widget *, void *p) { static_cast<XanaraWindow *>(p)->commit_field(); }, this);
  field_input_->hide();
  end();
  resizable(nullptr);
  size_range(S(520), S(360));
  xclass("Xanara");
  load_settings();
  apply_theme();
  build_buttons();
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
  view_->set_document(nullptr);
  doc_.reset();
  for (const auto &t : temps_) unlink(t.c_str());
  instance_ = nullptr;
}

void XanaraWindow::hide() {
  // FLTK also hides and re-shows the window to enter full screen without a
  // window manager, so this must not close the document; quit() does.
  BufferedWindow::hide();
}

void XanaraWindow::quit() {
  if (close_document(true)) hide();
}

// ------------------------------------------------------------- settings

void XanaraWindow::load_settings() {
  Config cfg;
  cfg.load();
  std::string l = cfg.get("xanara", "layout", "continuous");
  view_->set_layout(l == "single" ? Layout::Single : l == "book" ? Layout::Book : Layout::Continuous);
  std::string m = cfg.get("xanara", "reading_mode", "normal");
  view_->set_color_mode(m == "night" ? ColorMode::Night : m == "sepia" ? ColorMode::Sepia : ColorMode::Normal);
  sidebar_on_ = cfg.get_bool("xanara", "sidebar", true);
  std::string tab = cfg.get("xanara", "sidebar_tab", "pages");
  sidebar_->set_tab(tab == "outline" ? Sidebar::Outline : tab == "notes" ? Sidebar::Notes : Sidebar::Pages);
  view_->set_highlight_fields(cfg.get_bool("xanara", "highlight_fields", true));
  int mb = cfg.get_int("xanara", "cache_mb", 224);
  view_->set_cache_budget(size_t(std::clamp(mb, 32, 2048)) << 20);
}

void XanaraWindow::save_settings() {
  Config cfg;
  cfg.load();
  Layout l = view_->presentation() ? Layout::Continuous : view_->layout_mode();
  cfg.set("xanara", "layout", l == Layout::Single ? "single" : l == Layout::Book ? "book" : "continuous");
  ColorMode m = view_->color_mode();
  cfg.set("xanara", "reading_mode", m == ColorMode::Night ? "night" : m == ColorMode::Sepia ? "sepia" : "normal");
  cfg.set("xanara", "sidebar", sidebar_on_ ? "true" : "false");
  Sidebar::Tab t = sidebar_->tab();
  cfg.set("xanara", "sidebar_tab", t == Sidebar::Outline ? "outline" : t == Sidebar::Notes ? "notes" : "pages");
  cfg.set("xanara", "highlight_fields", view_->highlight_fields() ? "true" : "false");
  cfg.save();
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
  for (Fl_Input *in : {page_input_, find_input_, field_input_}) {
    in->box(FL_FLAT_BOX);
    in->color(in == field_input_ ? Color::hex(0xFFFFFF).fl() : theme_.surface.fl());
    in->textcolor(in == field_input_ ? Color::hex(0x111111).fl() : theme_.text.fl());
    in->selection_color(theme_.accent.fl());
    in->cursor_color(theme_.accent.fl());
    in->textsize(theme_.font_size);
    in->textfont(FL_HELVETICA);
  }
  page_input_->color(theme_.background.fl());
  color(theme_.background.fl());
  redraw();
}

int XanaraWindow::x_event_handler(int) {
  if (!instance_ || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    instance_->apply_theme();
    instance_->sidebar_->redraw();
    instance_->view_->redraw();
  }
  return 0;
}

// ---------------------------------------------------------- command bar

void XanaraWindow::build_buttons() {
  buttons_.clear();
  auto add = [&](int group, icon::Kind g, const char *tip, std::function<void()> fn) {
    Button b;
    b.group = group;
    b.glyph = g;
    b.tip = tip;
    b.action = std::move(fn);
    buttons_.push_back(b);
  };
  add(GFile, icon::Sidebar, _("Sidebar (F9)"), [this] { toggle_sidebar(); });
  add(GFile, icon::Open, _("Open (Ctrl+O)"), [this] { open_dialog(); });
  add(GFile, icon::Save, _("Save (Ctrl+S)"), [this] { save(); });
  add(GPage, icon::Prev, _("Previous page"), [this] { view_->prev_page(); });
  add(GPage, icon::Next, _("Next page"), [this] { view_->next_page(); });
  add(GZoom, icon::ZoomOut, _("Zoom out (Ctrl+−)"), [this] { view_->zoom_step(-1); });
  add(GZoom, icon::Menu, _("Zoom"), nullptr);  // the percentage; opens presets
  buttons_.back().text = "zoom";
  buttons_.back().action = [this] {
    const Button *zb = nullptr;
    for (auto &b : buttons_)
      if (b.text == "zoom") zb = &b;
    struct P {
      const char *label;
      double z;
    } presets[] = {{_("Fit Width"), -1}, {_("Fit Page"), -2}, {"50%", 0.5}, {"75%", 0.75}, {"100%", 1.0},
                   {"125%", 1.25}, {"150%", 1.5}, {"200%", 2.0}, {"300%", 3.0}, {"400%", 4.0}, {"800%", 8.0}};
    Fl_Menu_Item items[12];
    memset(items, 0, sizeof items);
    for (int k = 0; k < 11; ++k) {
      items[k].text = presets[k].label;
      items[k].labelsize(theme_.font_size);
      items[k].labelcolor(theme_.text.fl());
      if (k == 1) items[k].flags = FL_MENU_DIVIDER;
    }
    const Fl_Menu_Item *m = items->popup(zb ? zb->x : Fl::event_x(), zb ? zb->y + zb->h + S(4) : Fl::event_y());
    if (!m) return;
    double z = presets[m - items].z;
    if (z == -1) view_->set_fit(Fit::Width);
    else if (z == -2) view_->set_fit(Fit::Page);
    else view_->set_zoom(z);
  };
  add(GZoom, icon::ZoomIn, _("Zoom in (Ctrl++)"), [this] { view_->zoom_step(1); });
  add(GZoom, icon::FitWidth, _("Fit width (Ctrl+2)"), [this] { view_->set_fit(Fit::Width); });
  add(GZoom, icon::FitPage, _("Fit page (Ctrl+1)"), [this] { view_->set_fit(Fit::Page); });
  struct T {
    Tool t;
    icon::Kind g;
    const char *tip;
  } tools[] = {{Tool::Select, icon::Select, _("Select and edit (V)")},
               {Tool::Highlight, icon::Highlight, _("Highlight text (H)")},
               {Tool::Underline, icon::Underline, _("Underline text (U)")},
               {Tool::Strike, icon::Strike, _("Strike out text (K)")},
               {Tool::Note, icon::Note, _("Sticky note (N)")},
               {Tool::Text, icon::TextBox, _("Text box (T)")},
               {Tool::Pen, icon::Pen, _("Pen (P)")},
               {Tool::Rect, icon::Rect, _("Rectangle (R)")},
               {Tool::Line, icon::Line, _("Line (L)")},
               {Tool::Signature, icon::Signature, _("Signature (G)")}};
  for (auto &t : tools) {
    Tool tool = t.t;
    add(GTools, t.g, t.tip, [this, tool] { view_->set_tool(view_->tool() == tool ? Tool::Select : tool); });
    buttons_.back().text = "tool:" + std::to_string(int(tool));
  }
  add(GEnd, icon::Find, _("Find (Ctrl+F)"), [this] { show_find(!find_on_); });
  add(GEnd, icon::Moon, _("Reading mode: normal, night, sepia (Ctrl+M)"), [this] { cycle_color_mode(); });
  buttons_.back().text = "mode";
  add(GEnd, icon::Menu, _("Menu"), [this] {
    for (auto &b : buttons_)
      if (b.group == GEnd && b.glyph == icon::Menu) show_menu(b.x + b.w - S(220), b.y + b.h + S(4));
  });
  // The tool strip.
  add(GStrip, icon::Undo, _("Undo (Ctrl+Z)"), [this] { view_->undo(); });
  add(GStrip, icon::Trash, _("Delete annotation (Delete)"), [this] { view_->delete_selected_annot(); });
  buttons_.back().text = "delete";
  add(GStrip, icon::Note, _("Edit note text"), [this] { view_->edit_selected_note(); });
  buttons_.back().text = "editnote";
  for (int k = 0; k < kSwatchCount; ++k) {
    add(GSwatch, icon::Check, _(kSwatches[k].name), [this, k] { view_->set_ink(kSwatches[k].color); });
    buttons_.back().text = "swatch:" + std::to_string(k);
  }
  for (double wd : {1.0, 2.0, 4.0}) {
    add(GWidth, icon::Line, format(_("%g pt"), wd).c_str(), [this, wd] {
      view_->set_stroke(wd);
      tool_changed();
    });
    buttons_.back().text = "width:" + format("%g", wd);
  }
  add(GStripEnd, icon::Close, _("Done (Esc)"), [this] {
    view_->set_tool(Tool::Select);
    view_->clear_selection();
    Fl::focus(view_);
  });
  // The find bar.
  add(GFind, icon::Up, _("Previous match (Shift+Enter)"), [this] { view_->find_next(-1); });
  add(GFind, icon::Down, _("Next match (Enter)"), [this] {
    view_->find(find_input_->value(), find_case_);
  });
  add(GFind, icon::Check, _("Match case"), [this] {
    find_case_ = !find_case_;
    view_->find_clear();
    if (find_input_->size()) view_->find(find_input_->value(), find_case_);
    update_chrome();
  });
  buttons_.back().text = "Aa";
  add(GFind, icon::Close, _("Close (Esc)"), [this] { show_find(false); });
}

void XanaraWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void XanaraWindow::layout() {
  bool presenting = view_->presentation();
  bool have = doc_ != nullptr;
  bar_h_ = presenting ? 0 : S(54);
  bool strip = have && !presenting && (view_->tool() != Tool::Select || view_->has_selected_annot());
  strip_h_ = strip ? S(44) : 0;
  sidebar_w_ = have && sidebar_on_ && !presenting && w() >= S(640) ? S(224) : 0;
  // The tool strip floats over the pages, so choosing a tool never moves
  // the text under the pointer.
  int top = bar_h_;
  if (sidebar_w_) {
    sidebar_->resize(0, top, sidebar_w_, h() - top);
    sidebar_->show();
  } else {
    sidebar_->hide();
  }
  view_->resize(sidebar_w_, top, w() - sidebar_w_, h() - top);
  // Command bar: file group on the left, then page navigation; tools,
  // zoom and the menu on the right.
  int bs = S(34), by = (bar_h_ - bs) / 2, gap = S(10);
  for (auto &b : buttons_) {
    b.visible = !presenting;
    b.w = b.h = bs;
    b.y = by;
  }
  auto place = [&](int group, int x0) {
    int x = x0;
    for (auto &b : buttons_)
      if (b.group == group && b.visible) {
        if (b.text == "zoom") b.w = S(64);
        b.x = x;
        x += b.w;
      }
    return x;
  };
  auto width_of = [&](int group) {
    int wsum = 0;
    for (auto &b : buttons_)
      if (b.group == group && b.visible) wsum += b.text == "zoom" ? S(64) : bs;
    return wsum;
  };
  bool narrow = w() < S(1060), tiny = w() < S(760);
  for (auto &b : buttons_) {
    if (b.group == GTools && narrow) b.visible = false;
    if (b.group == GZoom && tiny && (b.glyph == icon::FitWidth || b.glyph == icon::FitPage)) b.visible = false;
    if (!have && (b.group == GPage || b.group == GZoom || b.group == GTools || (b.group == GFile && b.glyph != icon::Open)) )
      b.visible = false;
    if (!have && b.group == GEnd && b.glyph != icon::Menu) b.visible = false;
  }
  int x = place(GFile, S(12));
  if (have && !presenting) {
    x += gap;
    for (auto &b : buttons_)
      if (b.group == GPage && b.glyph == icon::Prev) b.x = x;
    page_input_->resize(x + bs + S(6), by + (bs - S(26)) / 2, S(46), S(26));
    page_input_->show();
    for (auto &b : buttons_)
      if (b.group == GPage && b.glyph == icon::Next) b.x = x + bs + S(6) + S(46) + S(52);
    x += 2 * bs + S(110);
  } else {
    page_input_->hide();
  }
  int right = w() - S(12);
  right -= width_of(GEnd);
  place(GEnd, right);
  right -= gap;
  right -= width_of(GTools);
  place(GTools, right);
  if (width_of(GTools)) right -= gap;
  right -= width_of(GZoom);
  place(GZoom, right);
  (void)x;
  // Strip.
  int sy = bar_h_ + (strip_h_ - S(30)) / 2;
  int sx = S(12) + S(150);
  for (auto &b : buttons_) {
    if (b.group < GStrip || b.group > GStripEnd) continue;
    b.visible = strip;
    b.y = sy;
    b.h = S(30);
    b.w = S(30);
  }
  bool annot = view_->has_selected_annot() && view_->tool() == Tool::Select;
  Tool t = view_->tool();
  bool shape = t == Tool::Pen || t == Tool::Rect || t == Tool::Line;
  bool colours = t != Tool::Signature;
  for (auto &b : buttons_) {
    if (!b.visible) continue;
    if (b.group == GSwatch) {
      if (!colours) b.visible = false;
      else {
        b.x = sx;
        sx += S(32);
      }
    }
  }
  sx += S(14);
  for (auto &b : buttons_) {
    if (!b.visible || b.group != GWidth) continue;
    if (!shape) b.visible = false;
    else {
      b.x = sx;
      b.w = S(36);
      sx += S(38);
    }
  }
  int ex = w() - S(12) - S(30);
  for (auto &b : buttons_) {
    if (!b.visible) continue;
    if (b.group == GStripEnd) b.x = ex;
    if (b.group == GStrip) {
      if ((b.text == "delete" && !annot) || (b.text == "editnote" && !annot)) {
        b.visible = false;
        continue;
      }
      if (b.text == "editnote") {
        const Annotation *a = view_->selected_annot();
        if (!a || (a->type != POPPLER_ANNOT_TEXT && a->type != POPPLER_ANNOT_FREE_TEXT)) {
          b.visible = false;
          continue;
        }
      }
    }
  }
  int rx = ex - S(8);
  for (int k = int(buttons_.size()) - 1; k >= 0; --k) {
    Button &b = buttons_[size_t(k)];
    if (b.visible && b.group == GStrip) {
      rx -= b.w;
      b.x = rx;
      rx -= S(4);
    }
  }
  // Find bar.
  int fw = std::min(S(430), w() - S(24)), fh = S(46);
  int fx = w() - fw - S(16), fy = bar_h_ + strip_h_ + S(10);
  for (auto &b : buttons_)
    if (b.group == GFind) {
      b.visible = find_on_ && have && !presenting;
      b.w = b.h = S(30);
      b.y = fy + (fh - S(30)) / 2;
    }
  int bx = fx + fw - S(8);
  for (int k = int(buttons_.size()) - 1; k >= 0; --k) {
    Button &b = buttons_[size_t(k)];
    if (b.group != GFind) continue;
    bx -= b.w + S(2);
    b.x = bx;
  }
  if (find_on_ && have && !presenting) {
    find_input_->resize(fx + S(40), fy + (fh - S(26)) / 2, std::max(S(60), bx - fx - S(40) - S(78)), S(26));
    find_input_->show();
  } else {
    find_input_->hide();
  }
  redraw();
}

int XanaraWindow::button_at(int mx, int my) const {
  for (int k = 0; k < int(buttons_.size()); ++k) {
    const Button &b = buttons_[size_t(k)];
    if (b.visible && b.active && mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) return k;
  }
  return -1;
}

void XanaraWindow::update_chrome() {
  bool have = doc_ != nullptr;
  for (auto &b : buttons_) {
    b.toggled = false;
    b.active = true;
    if (b.group == GTools && starts_with(b.text, "tool:")) b.toggled = int(view_->tool()) == atoi(b.text.c_str() + 5);
    if (b.group == GFile && b.glyph == icon::Sidebar) b.toggled = sidebar_on_ && have;
    if (b.group == GFile && b.glyph == icon::Save) b.active = have && (doc_->modified() || working_copy_);
    if (b.group == GZoom && b.glyph == icon::FitWidth) b.toggled = view_->fit() == Fit::Width;
    if (b.group == GZoom && b.glyph == icon::FitPage) b.toggled = view_->fit() == Fit::Page;
    if (b.group == GEnd && b.glyph == icon::Find) b.toggled = find_on_;
    if (b.text == "mode") {
      ColorMode m = view_->color_mode();
      b.glyph = m == ColorMode::Night ? icon::Moon : m == ColorMode::Sepia ? icon::Sepia : icon::Sun;
      b.toggled = m != ColorMode::Normal;
    }
    if (b.group == GPage && have) {
      if (b.glyph == icon::Prev) b.active = view_->current_page() > 0;
      if (b.glyph == icon::Next) b.active = view_->current_page() + 1 < doc_->count();
    }
    if (b.group == GStrip && b.glyph == icon::Undo) b.active = view_->can_undo();
    if (b.group == GSwatch) {
      int k = atoi(b.text.c_str() + 7);
      b.toggled = same_rgb(view_->ink(), kSwatches[k].color);
    }
    if (b.group == GWidth) b.toggled = std::fabs(view_->stroke() - atof(b.text.c_str() + 6)) < 0.01;
    if (b.text == "Aa") b.toggled = find_case_;
  }
  if (have && Fl::focus() != page_input_) page_input_->value(std::to_string(view_->current_page() + 1).c_str());
  std::string title = have ? display_name(target_) : std::string();
  if (have && (doc_->modified() || working_copy_)) title = "• " + title;
  std::string label_text = have ? title + " — Xanara" : std::string("Xanara");
  static std::string stored;
  if (stored != label_text) {
    stored = label_text;
    label(stored.c_str());
  }
  redraw();
}

void XanaraWindow::draw_bar(Painter &p) {
  if (!bar_h_) return;
  p.fill_rect(0, 0, w(), bar_h_, theme_.surface);
  p.line(0, bar_h_ - 0.5, w(), bar_h_ - 0.5, theme_.border);
  // Group pills.
  for (int g : {int(GFile), int(GPage), int(GZoom), int(GTools), int(GEnd)}) {
    int x0 = 1 << 30, x1 = -1, y0 = 0, hh = 0;
    for (auto &b : buttons_)
      if (b.group == g && b.visible) {
        x0 = std::min(x0, b.x);
        x1 = std::max(x1, b.x + b.w);
        y0 = b.y;
        hh = b.h;
      }
    if (x1 < 0) continue;
    if (g == GPage) {
      x0 -= 0;
    }
    p.fill_rounded(x0 - S(3), y0 - S(3), x1 - x0 + S(6), hh + S(6), (hh + S(6)) / 2.0, theme_.surface_alt);
  }
  if (page_input_->visible()) {
    int px = page_input_->x() - S(4), py = page_input_->y() - S(1);
    p.fill_rounded(px, py, page_input_->w() + S(8), page_input_->h() + S(2), S(6), theme_.background);
    p.stroke_rounded(px + 0.5, py + 0.5, page_input_->w() + S(7), page_input_->h() + S(1), S(6),
                     Fl::focus() == page_input_ ? theme_.accent : theme_.border);
  }
  for (int k = 0; k < int(buttons_.size()); ++k) {
    const Button &b = buttons_[size_t(k)];
    if (!b.visible || b.group >= GStrip) continue;
    bool hov = k == hover_button_ && b.active, down = k == press_button_;
    if (b.toggled) p.fill_rounded(b.x + S(1), b.y + S(1), b.w - S(2), b.h - S(2), (b.h - S(2)) / 2.0, theme_.accent.alpha(0.18));
    else if (hov || down) p.fill_rounded(b.x + S(1), b.y + S(1), b.w - S(2), b.h - S(2), (b.h - S(2)) / 2.0, theme_.hover);
    if (b.text == "zoom") continue;
    Color c = !b.active ? theme_.text_dim.alpha(0.45) : b.toggled ? theme_.accent_text : theme_.text;
    icon::draw(p, b.glyph, b.x + (b.w - S(20)) / 2.0, b.y + (b.h - S(20)) / 2.0, S(20), c);
    // A dot on Save while there are unsaved changes.
    if (b.group == GFile && b.glyph == icon::Save && b.active)
      p.circle(b.x + b.w - S(8), b.y + S(8), S(3.5), theme_.accent);
  }
}

void XanaraWindow::draw_bar_text() {
  if (!bar_h_) return;
  for (const auto &b : buttons_)
    if (b.visible && b.text == "zoom") {
      text_font(theme_.font_size, true);
      draw_text(view_->fit() == Fit::Width ? std::string(_("Width")) : view_->fit() == Fit::Page ? std::string(_("Page"))
                                                                                                 : zoom_text(view_->zoom()),
                b.x, b.y, b.w, b.h, theme_.text, 1);
      if (view_->fit() != Fit::Free) {
        text_font(theme_.font_size - S(3));
        draw_text(zoom_text(view_->zoom()), b.x, b.y + b.h - S(12), b.w, S(12), theme_.text_dim, 1);
      }
    }
  if (page_input_->visible() && doc_) {
    text_font(theme_.font_size);
    draw_text(format(_("of %d"), doc_->count()), page_input_->x() + page_input_->w() + S(8), page_input_->y(), S(48),
              page_input_->h(), theme_.text_dim);
    // The document name, when there is room.
    int left = 0;
    for (const auto &b : buttons_)
      if (b.group == GPage && b.glyph == icon::Next) left = b.x + b.w + S(18);
    int right = w();
    for (const auto &b : buttons_)
      if (b.visible && (b.group == GZoom || b.group == GTools || b.group == GEnd)) right = std::min(right, b.x - S(18));
    if (right - left > S(120)) {
      text_font(theme_.font_size, true);
      std::string name = display_name(target_);
      draw_text(name, left, S(8), right - left, S(22), theme_.text);
      text_font(theme_.font_size - S(2));
      std::string sub = doc_->modified() || working_copy_ ? std::string(_("Edited — not saved yet")) : std::string();
      if (sub.empty()) {
        std::string title = doc_->title();
        sub = title.empty() || title == name ? format(doc_->count() == 1 ? _("%d page") : _("%d pages"), doc_->count())
                                             : title;
      }
      draw_text(sub, left, S(28), right - left, S(18),
                doc_->modified() || working_copy_ ? theme_.accent_text : theme_.text_dim);
    }
  } else if (!doc_ && bar_h_) {
    text_font(theme_.font_size, true);
    draw_text("Xanara", S(60), 0, S(200), bar_h_, theme_.text);
  }
}

void XanaraWindow::draw_strip(Painter &p) {
  if (!strip_h_) return;
  p.fill_gradient(0, bar_h_ + strip_h_, w(), S(8), 0, theme_.shadow.alpha(theme_.dark ? 0.35 : 0.14),
                  theme_.shadow.alpha(0));
  p.fill_rect(0, bar_h_, w(), strip_h_, theme_.surface_alt);
  p.line(0, bar_h_ + strip_h_ - 0.5, w(), bar_h_ + strip_h_ - 0.5, theme_.border);
  Rgb ink = view_->ink();
  for (int k = 0; k < int(buttons_.size()); ++k) {
    const Button &b = buttons_[size_t(k)];
    if (!b.visible || b.group < GStrip || b.group > GStripEnd) continue;
    bool hov = k == hover_button_ && b.active;
    if (b.group == GSwatch) {
      const Rgb &c = kSwatches[atoi(b.text.c_str() + 7)].color;
      double cx = b.x + b.w / 2.0, cy = b.y + b.h / 2.0;
      if (b.toggled) p.circle(cx, cy, S(12), theme_.accent);
      p.circle(cx, cy, b.toggled ? S(9.5) : hov ? S(10) : S(9), theme_.surface);
      p.circle(cx, cy, S(8), Color{c.r, c.g, c.b, 1});
      continue;
    }
    if (b.toggled) p.fill_rounded(b.x, b.y, b.w, b.h, S(8), theme_.accent.alpha(0.18));
    else if (hov) p.fill_rounded(b.x, b.y, b.w, b.h, S(8), theme_.hover);
    if (b.group == GWidth) {
      double wd = atof(b.text.c_str() + 6);
      p.line(b.x + S(8), b.y + b.h / 2.0, b.x + b.w - S(8), b.y + b.h / 2.0, Color{ink.r, ink.g, ink.b, 1}, Sd(wd * 1.4));
      continue;
    }
    Color c = !b.active ? theme_.text_dim.alpha(0.45) : theme_.text;
    icon::draw(p, b.glyph, b.x + (b.w - S(18)) / 2.0, b.y + (b.h - S(18)) / 2.0, S(18), c);
  }
}

void XanaraWindow::draw_find(Painter &p) {
  if (!find_input_->visible()) return;
  int fw = std::min(S(430), w() - S(24)), fh = S(46);
  int fx = w() - fw - S(16), fy = bar_h_ + strip_h_ + S(10);
  p.shadow(fx, fy, fw, fh, S(12), theme_.shadow, S(8));
  p.fill_rounded(fx, fy, fw, fh, S(12), theme_.surface);
  p.stroke_rounded(fx + 0.5, fy + 0.5, fw - 1, fh - 1, S(12), theme_.border);
  icon::draw(p, icon::Find, fx + S(12), fy + (fh - S(18)) / 2.0, S(18), theme_.text_dim);
  for (int k = 0; k < int(buttons_.size()); ++k) {
    const Button &b = buttons_[size_t(k)];
    if (!b.visible || b.group != GFind) continue;
    if (b.toggled) p.fill_rounded(b.x, b.y, b.w, b.h, S(8), theme_.accent.alpha(0.18));
    else if (k == hover_button_) p.fill_rounded(b.x, b.y, b.w, b.h, S(8), theme_.hover);
    if (b.text == "Aa") continue;
    icon::draw(p, b.glyph, b.x + S(6), b.y + S(6), S(18), theme_.text);
  }
}

void XanaraWindow::draw_find_text() {
  if (!find_input_->visible()) return;
  for (const auto &b : buttons_)
    if (b.visible && b.text == "Aa") {
      text_font(theme_.font_size - S(1), true);
      draw_text("Aa", b.x, b.y, b.w, b.h, b.toggled ? theme_.accent_text : theme_.text, 1);
    }
  int n = view_->find_count();
  std::string s;
  if (view_->find_running()) s = n ? format("%d/%d…", view_->find_index() + 1, n) : std::string("…");
  else if (find_input_->size()) s = n ? format("%d/%d", view_->find_index() + 1, n) : std::string(_("none"));
  text_font(theme_.font_size - S(2));
  draw_text(s, find_input_->x() + find_input_->w() + S(4), find_input_->y(), S(72), find_input_->h(), theme_.text_dim, 1);
  if (find_input_->size() == 0) {
    text_font(theme_.font_size);
    draw_text(_("Find in document"), find_input_->x() + S(3), find_input_->y(), find_input_->w(), find_input_->h(),
              theme_.text_dim);
  }
}

void XanaraWindow::draw() {
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    draw_bar(p);
  }
  draw_bar_text();
  draw_child(*view_);
  if (sidebar_->visible()) draw_child(*sidebar_);
  if (strip_h_) {
    {
      Painter p(w(), h());
      draw_strip(p);
    }
    Tool t = view_->tool();
    text_font(theme_.font_size, true);
    std::string name = view_->has_selected_annot() && t == Tool::Select ? std::string(_("Annotation")) : tool_name(t);
    draw_text(name, S(16), bar_h_, S(140), strip_h_, theme_.text);
    int hint_x = S(12) + S(150);
    for (const auto &b : buttons_)
      if (b.visible && (b.group == GSwatch || b.group == GWidth)) hint_x = std::max(hint_x, b.x + b.w + S(18));
    int hint_r = w() - S(60);
    for (const auto &b : buttons_)
      if (b.visible && b.group == GStrip) hint_r = std::min(hint_r, b.x - S(12));
    text_font(theme_.font_size - S(1));
    draw_text(tool_hint(t), hint_x, bar_h_, hint_r - hint_x, strip_h_, theme_.text_dim);
  }
  {
    Painter p(w(), h());
    draw_find(p);
  }
  if (page_input_->visible()) draw_child(*page_input_);
  if (find_input_->visible()) draw_child(*find_input_);
  if (field_input_->visible()) draw_child(*field_input_);
  draw_find_text();
  // Button tooltip.
  if (!tip_.empty()) {
    text_font(theme_.font_size - S(1));
    int tw = text_width(tip_) + S(20), th = S(28);
    int tx = std::clamp(tip_x_ - tw / 2, S(4), w() - tw - S(4)), ty = tip_y_;
    {
      Painter p(w(), h());
      p.shadow(tx, ty, tw, th, S(6), theme_.shadow, S(5));
      p.fill_rounded(tx, ty, tw, th, S(6), theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.1) : Color::hex(0x1F2630));
    }
    draw_text(tip_, tx + S(10), ty, tw - S(20), th, theme_.dark ? theme_.text : Color::hex(0xF5F7FA));
  }
  if (!toast_.empty()) {
    text_font(theme_.font_size);
    int tw = std::min(text_width(toast_) + S(40), w() - S(40)), th = S(40);
    int tx = view_->x() + (view_->w() - tw) / 2, ty = h() - th - S(28);
    Color bg = theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.12) : Color::hex(0x1F2630);
    Color fg = theme_.dark ? theme_.text : Color::hex(0xF5F7FA);
    {
      Painter p(w(), h());
      p.shadow(tx, ty, tw, th, th / 2.0, theme_.shadow, S(8));
      p.fill_rounded(tx, ty, tw, th, th / 2.0, bg);
    }
    draw_text(toast_, tx + S(20), ty, tw - S(40), th, fg, 1);
  }
}

// ------------------------------------------------------------------ events

int XanaraWindow::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (event) {
    case FL_MOVE:
    case FL_ENTER: {
      int k = button_at(mx, my);
      if (k != hover_button_) {
        hover_button_ = k;
        tip_.clear();
        if (k >= 0) {
          const Button &b = buttons_[size_t(k)];
          tip_ = b.tip;
          tip_x_ = b.x + b.w / 2;
          tip_y_ = b.y + b.h + S(8);
          if (tip_y_ + S(30) > h()) tip_y_ = b.y - S(34);
        }
        redraw();
      }
      if (k >= 0) {
        cursor(FL_CURSOR_DEFAULT);
        return 1;
      }
      break;
    }
    case FL_LEAVE:
      if (hover_button_ >= 0 || !tip_.empty()) {
        hover_button_ = -1;
        tip_.clear();
        redraw();
      }
      break;
    case FL_PUSH: {
      int k = button_at(mx, my);
      if (k >= 0) {
        press_button_ = k;
        tip_.clear();
        redraw();
        return 1;
      }
      if (strip_h_ && my >= bar_h_ && my < bar_h_ + strip_h_) return 1;  // strip background
      break;
    }
    case FL_DRAG:
      if (press_button_ >= 0) return 1;
      break;
    case FL_RELEASE:
      if (press_button_ >= 0) {
        int k = press_button_;
        press_button_ = -1;
        if (button_at(mx, my) == k && buttons_[size_t(k)].action) {
          auto fn = buttons_[size_t(k)].action;
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
          Fl::focus(view_);
          redraw();
          return 1;
        }
        if (view_->presentation()) {
          toggle_presentation();
          return 1;
        }
        if (f == find_input_ || (find_on_ && !view_->has_selection() && view_->tool() == Tool::Select &&
                                 !view_->has_selected_annot())) {
          show_find(false);
          return 1;
        }
        if (f == page_input_) {
          Fl::focus(view_);
          update_chrome();
          return 1;
        }
        if (fullscreen_ && !view_->has_selection() && view_->tool() == Tool::Select && !view_->has_selected_annot()) {
          toggle_fullscreen();
          return 1;
        }
        view_->handle(FL_KEYBOARD);
        return 1;  // never close the window on Escape
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
          case 'g':
            if (shift) view_->find_next(-1);
            else view_->find_next(1);
            return 1;
          case 'w': close_document(true); update_chrome(); layout(); return 1;
          case 'q': quit(); return 1;
          case 'z':
            if (!typing) {
              view_->undo();
              return 1;
            }
            break;
          case '0': view_->set_zoom(1.0); return 1;
          case '1': view_->set_fit(Fit::Page); return 1;
          case '2': view_->set_fit(Fit::Width); return 1;
          case '+': case '=': case FL_KP + '+': view_->zoom_step(1); return 1;
          case '-': case FL_KP + '-': view_->zoom_step(-1); return 1;
          case 'l':
            if (doc_) {
              Fl::focus(page_input_);
              page_input_->position(0, page_input_->size());
              redraw();
            }
            return 1;
          case 'r': view_->rotate(shift ? -90 : 90); return 1;
          case FL_Left: view_->rotate(-90); return 1;
          case FL_Right: view_->rotate(90); return 1;
          case 'm': cycle_color_mode(); return 1;
        }
      }
      if (key == FL_F + 9) {
        toggle_sidebar();
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
        view_->find_next(shift ? -1 : 1);
        return 1;
      }
      if (typing) break;
      if (!ctrl && !(st & FL_ALT) && doc_ && !view_->presentation()) {
        Tool t = Tool::Select;
        bool tool_key = true;
        switch (key) {
          case 'v': t = Tool::Select; break;
          case 'h': t = Tool::Highlight; break;
          case 'u': t = Tool::Underline; break;
          case 'k': t = Tool::Strike; break;
          case 'n': t = Tool::Note; break;
          case 't': t = Tool::Text; break;
          case 'p': t = Tool::Pen; break;
          case 'r': t = Tool::Rect; break;
          case 'l': t = Tool::Line; break;
          case 'g': t = Tool::Signature; break;
          default: tool_key = false;
        }
        if (tool_key) {
          view_->set_tool(t);
          return 1;
        }
      }
      if (event == FL_SHORTCUT && f != view_ && view_->handle(FL_KEYBOARD)) return 1;
      break;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      return 1;
    case FL_PASTE:
      return view_->handle(FL_PASTE);
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_FOCUS || event == FL_UNFOCUS || event == FL_PUSH) redraw();
  return r;
}

// ----------------------------------------------------------- notifications

void XanaraWindow::toast(const std::string &message) {
  toast_ = message;
  Fl::remove_timeout(toast_cb, this);
  Fl::add_timeout(2.8, toast_cb, this);
  redraw();
}

void XanaraWindow::toast_cb(void *p) {
  auto *self = static_cast<XanaraWindow *>(p);
  self->toast_.clear();
  self->redraw();
}

void XanaraWindow::page_changed() {
  sidebar_->page_changed(view_->current_page());
  update_chrome();
  Fl::remove_timeout(remember_cb, this);
  Fl::add_timeout(2.0, remember_cb, this);
}

void XanaraWindow::remember_cb(void *p) { static_cast<XanaraWindow *>(p)->remember_position(); }

void XanaraWindow::remember_position() {
  if (!doc_ || target_.empty()) return;
  HistoryEntry e;
  e.path = target_;
  e.page = view_->current_page();
  e.zoom = view_->fit() == Fit::Width ? "width" : view_->fit() == Fit::Page ? "page" : format("%.4f", view_->zoom());
  history_.load();
  history_.remember(e);
  history_.save();
}

std::vector<HistoryEntry> XanaraWindow::recent() const { return history_.recent(8, true); }

void XanaraWindow::tool_changed() {
  layout();
  update_chrome();
}

void XanaraWindow::document_edited() {
  sidebar_->notes_changed();
  sidebar_->redraw();
  update_chrome();
}

void XanaraWindow::toggle_sidebar() {
  if (!doc_) return;
  sidebar_on_ = !sidebar_on_;
  layout();
  update_chrome();
  Fl::remove_timeout(settings_cb, this);
  Fl::add_timeout(1.0, settings_cb, this);
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
  if (!doc_) return;
  bool on = !view_->presentation();
  commit_field();
  view_->set_tool(Tool::Select);
  view_->set_presentation(on);
  if (on != fullscreen_) toggle_fullscreen();
  layout();
  Fl::focus(view_);
  if (on) toast(_("Presentation — arrows or click to advance, Esc to leave"));
  update_chrome();
}

void XanaraWindow::show_find(bool show) {
  if (show && !doc_) return;
  find_on_ = show;
  layout();
  if (show) {
    Fl::focus(find_input_);
    find_input_->position(find_input_->size(), 0);
    if (view_->has_selection()) {
      std::string s = view_->selection_text();
      if (!s.empty() && s.size() < 80 && s.find('\n') == std::string::npos) {
        find_input_->value(s.c_str());
        find_input_->position(0, find_input_->size());
      }
    }
  } else {
    view_->find_clear();
    Fl::focus(view_);
  }
  update_chrome();
}

void XanaraWindow::cycle_color_mode() {
  ColorMode m = view_->color_mode();
  ColorMode next = m == ColorMode::Normal ? ColorMode::Night : m == ColorMode::Night ? ColorMode::Sepia : ColorMode::Normal;
  view_->set_color_mode(next);
  toast(next == ColorMode::Night ? _("Night reading") : next == ColorMode::Sepia ? _("Sepia reading") : _("Normal colors"));
  Fl::remove_timeout(settings_cb, this);
  Fl::add_timeout(1.0, settings_cb, this);
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
  in->box(FL_FLAT_BOX);
  in->color(theme_.background.fl());
  in->textcolor(theme_.text.fl());
  in->selection_color(theme_.accent.fl());
  in->cursor_color(theme_.accent.fl());
  in->textsize(theme_.font_size);
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

void XanaraWindow::about() {
  fl_message_title(_("About Xanara"));
  fl_message("%s", format(_("Xanara %s\nDocument viewer and PDF editor for SDE.\n\n"
                            "Written from scratch by the DebION Dev-Team.\n"
                            "License: GPL-3.0-or-later. Rendering by poppler %s."),
                          SDE_VERSION, poppler_get_version())
                       .c_str());
}

void XanaraWindow::show_menu(int mx, int my) {
  bool have = doc_ != nullptr;
  int dis = have ? 0 : FL_MENU_INACTIVE;
  Layout lay = view_->layout_mode();
  ColorMode cm = view_->color_mode();
  auto recent = history_.recent(10, true);
  std::vector<std::string> recent_labels;
  for (auto &e : recent) {
    std::string s = basename_of(e.path);
    std::string esc;
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
    Present, Full, Side, Fields, PRotL, PRotR, PDel, PUp, PDown, PIns, PExt, Undo, Sign, About, Recent0 = 1000
  };
  item(_("Open…"), Open);
  item(_("Open Recent"), 0, FL_SUBMENU | (recent.empty() ? FL_MENU_INACTIVE : 0));
  for (size_t k = 0; k < recent.size(); ++k) item(recent_labels[k].c_str(), Recent0 + long(k));
  end_sub();
  item(_("Save"), Save, dis);
  item(_("Save As…"), SaveAs, dis);
  item(_("Print…"), Print, dis | FL_MENU_DIVIDER);
  item(_("View"), 0, FL_SUBMENU | dis);
  item(_("Continuous"), Continuous, FL_MENU_RADIO | (lay == Layout::Continuous ? FL_MENU_VALUE : 0));
  item(_("Single Page"), Single, FL_MENU_RADIO | (lay == Layout::Single ? FL_MENU_VALUE : 0));
  item(_("Two Pages (Book)"), Book, FL_MENU_RADIO | (lay == Layout::Book ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  item(_("Rotate Left"), RotL);
  item(_("Rotate Right"), RotR, FL_MENU_DIVIDER);
  item(_("Normal Colors"), Normal, FL_MENU_RADIO | (cm == ColorMode::Normal ? FL_MENU_VALUE : 0));
  item(_("Night Reading"), Night, FL_MENU_RADIO | (cm == ColorMode::Night ? FL_MENU_VALUE : 0));
  item(_("Sepia Reading"), Sepia, FL_MENU_RADIO | (cm == ColorMode::Sepia ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  item(_("Sidebar"), Side, FL_MENU_TOGGLE | (sidebar_on_ ? FL_MENU_VALUE : 0));
  item(_("Highlight Form Fields"), Fields, FL_MENU_TOGGLE | (view_->highlight_fields() ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  item(_("Presentation"), Present);
  item(_("Full Screen"), Full, FL_MENU_TOGGLE | (fullscreen_ ? FL_MENU_VALUE : 0));
  end_sub();
  item(_("Pages"), 0, FL_SUBMENU | dis);
  item(_("Rotate Page Left"), PRotL);
  item(_("Rotate Page Right"), PRotR, FL_MENU_DIVIDER);
  item(_("Move Page Up"), PUp);
  item(_("Move Page Down"), PDown, FL_MENU_DIVIDER);
  item(_("Insert Pages from File…"), PIns);
  item(_("Delete Pages…"), PDel);
  item(_("Extract Pages…"), PExt);
  end_sub();
  item(_("Annotate"), 0, FL_SUBMENU | dis);
  item(_("Undo"), Undo, view_->can_undo() ? 0 : FL_MENU_INACTIVE);
  item(_("Choose Signature Image…"), Sign);
  struct T {
    Tool t;
  } tools[] = {{Tool::Highlight}, {Tool::Underline}, {Tool::Strike}, {Tool::Note}, {Tool::Text},
               {Tool::Pen}, {Tool::Rect}, {Tool::Line}, {Tool::Signature}};
  items.back().flags |= FL_MENU_DIVIDER;
  for (auto &t : tools) item(tool_name(t.t), 2000 + int(t.t), FL_MENU_RADIO | (view_->tool() == t.t ? FL_MENU_VALUE : 0));
  end_sub();
  item(_("Close Document"), Close, dis);
  item(_("About Xanara"), About);
  item(_("Quit"), Quit);
  end_sub();
  const Fl_Menu_Item *m = items.data()->popup(std::max(0, mx), my);
  if (!m) return;
  long id = reinterpret_cast<long>(m->user_data());
  if (id >= Recent0 && id < Recent0 + 100) {
    open_path(recent[size_t(id - Recent0)].path);
    return;
  }
  if (id >= 2000 && id < 2100) {
    view_->set_tool(Tool(id - 2000));
    return;
  }
  switch (id) {
    case Open: open_dialog(); break;
    case Save: save(); break;
    case SaveAs: save_as(); break;
    case Print: print(); break;
    case Close: close_document(true); layout(); update_chrome(); break;
    case Quit: quit(); break;
    case Continuous: view_->set_layout(Layout::Continuous); break;
    case Single: view_->set_layout(Layout::Single); break;
    case Book: view_->set_layout(Layout::Book); break;
    case RotL: view_->rotate(-90); break;
    case RotR: view_->rotate(90); break;
    case Normal: view_->set_color_mode(ColorMode::Normal); break;
    case Night: view_->set_color_mode(ColorMode::Night); break;
    case Sepia: view_->set_color_mode(ColorMode::Sepia); break;
    case Side: toggle_sidebar(); break;
    case Fields: view_->set_highlight_fields(!view_->highlight_fields()); break;
    case Present: toggle_presentation(); break;
    case Full: toggle_fullscreen(); break;
    case PRotL: page_operation("rotate-left"); break;
    case PRotR: page_operation("rotate-right"); break;
    case PUp: page_operation("move-up"); break;
    case PDown: page_operation("move-down"); break;
    case PIns: page_operation("insert"); break;
    case PDel: page_operation("delete"); break;
    case PExt: page_operation("extract"); break;
    case Undo: view_->undo(); break;
    case Sign: choose_signature(); break;
    case About: about(); break;
  }
  Fl::remove_timeout(settings_cb, this);
  Fl::add_timeout(1.0, settings_cb, this);
}

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
  view_->set_signature(img);
  view_->set_tool(Tool::Signature);
  toast(_("Click on the page to place the signature"));
}

// ------------------------------------------------------------------- forms

void XanaraWindow::edit_field(int page, const FormField &f, int fx, int fy, int fw, int fh) {
  commit_field();
  if (!doc_) return;
  field_ = POPPLER_FORM_FIELD(g_object_ref(f.field));
  field_page_ = page;
  std::string value;
  bool multiline = false;
  {
    std::lock_guard<std::mutex> lock(doc_->mutex());
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
  if (doc_ && doc_->set_field_text(field_page_, field_, v)) view_->edited(field_page_);
  g_object_unref(field_);
  field_ = nullptr;
  redraw();
}

// ---------------------------------------------------------------- files

bool XanaraWindow::replace_document(const std::string &path, const std::string &password, bool working, int page,
                                    bool keep_view) {
  std::string error;
  bool encrypted = false;
  auto d = Document::open(path, password, error, encrypted);
  if (!d) {
    fl_alert("%s", format(_("Xanara could not open “%s”:\n%s"), display_name(path).c_str(), error.c_str()).c_str());
    return false;
  }
  Fit fit = view_->fit();
  double zoom = view_->zoom(), ssx, ssy;
  view_->scroll_pos(ssx, ssy);
  view_->set_document(nullptr);
  doc_ = std::move(d);
  working_copy_ = working;
  view_->set_document(doc_.get());
  if (keep_view) {
    if (fit != Fit::Free) view_->set_fit(fit);
    else view_->set_zoom(zoom);
  }
  sidebar_->document_changed();
  layout();
  view_->go_to(std::clamp(page, 0, doc_->count() - 1));
  // After saving, the pages are the same: stay exactly where the reader was.
  if (keep_view && !working) view_->set_scroll_pos(ssx, ssy);
  update_chrome();
  return true;
}

bool XanaraWindow::open_path(const std::string &path0, int page) {
  std::string path = path0;
  if (doc_ && path == target_) {
    if (page >= 0) view_->go_to(page);
    return true;
  }
  if (!file_exists(path)) {
    fl_alert("%s", format(_("“%s” does not exist."), path.c_str()).c_str());
    history_.forget(path);
    history_.save();
    redraw();
    return false;
  }
  if (!close_document(true)) return false;
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
  doc_ = std::move(d);
  target_ = path;
  working_copy_ = false;
  view_->set_document(doc_.get());
  view_->set_tool(Tool::Select);
  sidebar_->document_changed();
  layout();
  // Where the reader left off.
  int start = 0;
  history_.load();
  if (const HistoryEntry *e = history_.find(path)) {
    start = e->page;
    if (e->zoom == "page") view_->set_fit(Fit::Page);
    else if (e->zoom == "width") view_->set_fit(Fit::Width);
    else if (atof(e->zoom.c_str()) > 0) view_->set_zoom(atof(e->zoom.c_str()));
  } else {
    view_->set_fit(Fit::Width);
  }
  if (page >= 0) start = page;
  view_->go_to(std::clamp(start, 0, doc_->count() - 1));
  remember_position();
  Fl::focus(view_);
  update_chrome();
  return true;
}

void XanaraWindow::open_dialog() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.title(_("Open Document"));
  chooser.filter(_("PDF Documents\t*.{pdf,PDF}"));
  std::string dir = target_.empty() ? user_dir("DOCUMENTS") : target_.substr(0, target_.rfind('/'));
  if (dir.empty() || !dir_exists(dir)) dir = home_dir();
  chooser.directory(dir.c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  open_path(path_from_argument(chooser.filename()));
}

bool XanaraWindow::close_document(bool ask) {
  if (!doc_) return true;
  commit_field();
  if (ask && (doc_->modified() || working_copy_)) {
    fl_message_title(_("Unsaved Changes"));
    int c = fl_choice("%s", _("Cancel"), _("Don't Save"), _("Save"),
                      format(_("Save the changes to “%s” before closing?\n"
                               "Without saving, your annotations and edits are lost."),
                             display_name(target_).c_str())
                          .c_str());
    if (c == 0) return false;
    if (c == 2 && !save()) return false;
  }
  remember_position();
  view_->set_document(nullptr);
  doc_.reset();
  target_.clear();
  working_copy_ = false;
  find_on_ = false;
  sidebar_->document_changed();
  for (const auto &t : temps_) unlink(t.c_str());
  temps_.clear();
  layout();
  update_chrome();
  return true;
}

bool XanaraWindow::save() {
  if (!doc_) return false;
  commit_field();
  if (target_.empty()) return save_as();
  if (!doc_->modified() && !working_copy_) {
    toast(_("No changes to save"));
    return true;
  }
  std::string error;
  if (!doc_->save_to(target_, error)) {
    fl_alert("%s", format(_("“%s” could not be saved:\n%s\n\nThe original file was not changed."),
                          display_name(target_).c_str(), error.c_str())
                       .c_str());
    return false;
  }
  int page = view_->current_page();
  std::string pw = doc_->password();
  for (const auto &t : temps_) unlink(t.c_str());
  temps_.clear();
  replace_document(target_, pw, false, page, true);
  remember_position();
  toast(format(_("Saved “%s”"), display_name(target_).c_str()));
  return true;
}

bool XanaraWindow::save_as() {
  if (!doc_) return false;
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
  chooser.title(_("Save a Copy As"));
  chooser.filter(_("PDF Documents\t*.pdf"));
  chooser.directory(target_.substr(0, target_.rfind('/')).c_str());
  chooser.preset_file(display_name(target_).c_str());
  if (chooser.show() != 0 || !chooser.filename()) return false;
  std::string path = chooser.filename();
  if (!ends_with(lower(path), ".pdf")) path += ".pdf";
  std::string error;
  commit_field();
  if (!doc_->save_to(path, error)) {
    fl_alert("%s", format(_("“%s” could not be saved:\n%s"), display_name(path).c_str(), error.c_str()).c_str());
    return false;
  }
  int page = view_->current_page();
  std::string pw = doc_->password();
  target_ = path;
  for (const auto &t : temps_) unlink(t.c_str());
  temps_.clear();
  replace_document(target_, pw, false, page, true);
  remember_position();
  toast(format(_("Saved “%s”"), display_name(target_).c_str()));
  return true;
}

std::string XanaraWindow::temp_path(const std::string &tag) {
  static int counter = 0;
  std::string dir = runtime_dir();
  make_dirs(dir);
  std::string p = format("%s/xanara-%d-%d-%s.pdf", dir.c_str(), int(getpid()), ++counter, tag.c_str());
  temps_.push_back(p);
  return p;
}

void XanaraWindow::print() {
  if (!doc_) return;
  if (find_program("lp").empty()) {
    fl_alert("%s", _("Printing needs the CUPS client (the lp command).\nInstall it with: sudo apt install cups-client"));
    return;
  }
  std::string file = doc_->path();
  if (doc_->modified()) {
    std::string error;
    file = temp_path("print");
    if (!doc_->save_to(file, error)) {
      fl_alert("%s", error.c_str());
      return;
    }
  }
  fl_message_title(_("Print"));
  if (fl_choice("%s", _("Cancel"), _("Print"), nullptr,
                format(_("Print “%s” (%d pages) on the default printer?"), display_name(target_).c_str(), doc_->count())
                    .c_str()) != 1)
    return;
  run_async({"lp", "-t", display_name(target_), file}, [this](const std::string &out, int status) {
    if (instance_ != this) return;
    toast(status == 0 ? std::string(_("Sent to the printer")) : std::string(_("Printing failed — is a printer set up?")));
    (void)out;
  });
}

std::string XanaraWindow::current_source(bool &temp) {
  temp = false;
  if (!doc_->modified()) return doc_->path();
  std::string error, p = temp_path("edit");
  if (!doc_->save_to(p, error)) {
    fl_alert("%s", error.c_str());
    return "";
  }
  temp = true;
  return p;
}

bool XanaraWindow::run_qpdf(std::vector<std::string> args, const std::string &out) {
  std::vector<std::string> argv{"qpdf", "--warning-exit-0"};
  if (!doc_->password().empty()) argv.push_back("--password=" + doc_->password());
  for (auto &a : args) argv.push_back(a);
  int status = -1;
  std::string output = run_sync(argv, &status, 120000);
  if ((status != 0 && status != 3) || !file_exists(out)) {
    fl_alert("%s", format(_("qpdf could not change the pages (exit %d).\n%s"), status, output.c_str()).c_str());
    return false;
  }
  return true;
}

void XanaraWindow::page_operation(const std::string &op) {
  if (!doc_) return;
  commit_field();
  if (find_program("qpdf").empty()) {
    fl_message_title(_("qpdf Is Needed"));
    fl_alert("%s", _("Rotating, moving, inserting, deleting and extracting pages uses qpdf.\n"
                     "Install it with:  sudo apt install qpdf"));
    return;
  }
  int n = doc_->count(), cur = view_->current_page();
  std::vector<int> order;
  for (int i = 0; i < n; ++i) order.push_back(i);
  std::string extra_file;
  std::vector<int> pages;
  if (op == "delete" || op == "extract") {
    std::string ranges = std::to_string(cur + 1);
    if (!ask_text(op == "delete" ? _("Delete Pages (for example 3 or 2-5,9)") : _("Extract Pages (for example 1-3,7)"),
                  ranges, false))
      return;
    if (!parse_page_ranges(ranges, n, pages)) {
      fl_alert("%s", format(_("“%s” is not a valid page selection for pages 1 to %d."), ranges.c_str(), n).c_str());
      return;
    }
    if (op == "delete") {
      std::vector<int> keep;
      for (int i = 0; i < n; ++i)
        if (std::find(pages.begin(), pages.end(), i) == pages.end()) keep.push_back(i);
      if (keep.empty()) {
        fl_alert("%s", _("A document needs at least one page."));
        return;
      }
      fl_message_title(_("Delete Pages"));
      if (fl_choice("%s", _("Cancel"), _("Delete"), nullptr,
                    format(_("Delete %zu page(s)? You can still close without saving to keep them."), n - keep.size())
                        .c_str()) != 1)
        return;
      order = keep;
    }
  } else if (op == "move-up" || op == "move-down") {
    int to = op == "move-up" ? cur - 1 : cur + 1;
    if (to < 0 || to >= n) return;
    std::swap(order[size_t(cur)], order[size_t(to)]);
    cur = to;
  } else if (op == "insert") {
    Fl_Native_File_Chooser chooser;
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.title(_("Insert Pages from a PDF"));
    chooser.filter(_("PDF Documents\t*.{pdf,PDF}"));
    chooser.directory(target_.substr(0, target_.rfind('/')).c_str());
    if (chooser.show() != 0 || !chooser.filename()) return;
    extra_file = path_from_argument(chooser.filename());
  }
  bool temp = false;
  std::string src = current_source(temp);
  if (src.empty()) return;
  if (op == "extract") {
    Fl_Native_File_Chooser chooser;
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    chooser.title(_("Save Extracted Pages As"));
    chooser.filter(_("PDF Documents\t*.pdf"));
    std::string base = display_name(target_);
    if (ends_with(lower(base), ".pdf")) base = base.substr(0, base.size() - 4);
    chooser.directory(target_.substr(0, target_.rfind('/')).c_str());
    chooser.preset_file((base + " (" + format_page_ranges(pages) + ").pdf").c_str());
    if (chooser.show() != 0 || !chooser.filename()) return;
    std::string out = chooser.filename();
    if (!ends_with(lower(out), ".pdf")) out += ".pdf";
    std::string tmp = out + ".xanara-part";
    if (!run_qpdf({src, "--pages", ".", format_page_ranges(pages), "--", tmp}, tmp)) {
      unlink(tmp.c_str());
      return;
    }
    if (rename(tmp.c_str(), out.c_str()) != 0) {
      unlink(tmp.c_str());
      fl_alert("%s", strerror(errno));
      return;
    }
    toast(format(_("Saved %zu page(s) to “%s”"), pages.size(), display_name(out).c_str()));
    return;
  }
  std::string out = temp_path("pages");
  bool ok = false;
  if (op == "rotate-left" || op == "rotate-right") {
    ok = run_qpdf({src, out, format("--rotate=%s90:%d", op == "rotate-left" ? "-" : "+", cur + 1)}, out);
  } else if (op == "insert") {
    std::vector<std::string> args{src, "--pages"};
    if (cur + 1 >= 1) {
      args.push_back(".");
      args.push_back(format("1-%d", cur + 1));
    }
    args.push_back(extra_file);
    args.push_back("1-z");
    if (cur + 2 <= n) {
      args.push_back(".");
      args.push_back(format("%d-z", cur + 2));
    }
    args.push_back("--");
    args.push_back(out);
    ok = run_qpdf(args, out);
    cur += 1;
  } else {
    ok = run_qpdf({src, "--pages", ".", format_page_ranges(order), "--", out}, out);
    if (op == "delete") cur = std::min(cur, int(order.size()) - 1);
  }
  if (!ok) return;
  std::string pw = doc_->password();
  if (replace_document(out, pw, true, cur, true)) {
    doc_->set_modified(true);
    update_chrome();
    toast(_("Pages changed — save to keep the result"));
  }
  (void)temp;
}

}  // namespace xanara
