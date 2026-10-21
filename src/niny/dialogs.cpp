// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny dialogs: the in-window question sheet and the Settings window.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <cmath>
#include <set>

#include "i18n.h"
#include "niny.h"
#include "process.h"
#include "util.h"

using namespace sde;

namespace niny {

// ------------------------------------------------------------------ Sheet

Sheet::Sheet(App *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  box(FL_NO_BOX);
  end();
  hide();
}

void Sheet::layout_card() {
  cw_ = std::min<double>(S(460), w() - S(40));
  text_font(look().theme.font_size);
  int tw = 0, th = 0;
  fl_measure(message_.c_str(), tw = int(cw_ - S(48)), th, 0);
  ch_ = S(64) + th + S(72);
  cx_ = x() + (w() - cw_) / 2;
  cy_ = y() + std::max<double>(S(60), (h() - ch_) / 2.6);
  int bx = int(cx_ + cw_ - S(20));
  for (int i = int(buttons_.size()) - 1; i >= 0; --i) {
    text_font(look().theme.font_size, true);
    int bw = std::max<int>(S(90), text_width(buttons_[size_t(i)]->label() ? buttons_[size_t(i)]->label() : "") + S(32));
    bw = std::max(bw, S(96));
    buttons_[size_t(i)]->resize(bx - bw, int(cy_ + ch_ - S(52)), bw, S(34));
    bx -= bw + S(8);
  }
}

int Sheet::ask(const std::string &title, const std::string &message, const std::vector<std::string> &buttons,
               int default_button, int cancel_button) {
  for (auto *b : buttons_) {
    remove(b);
    Fl::delete_widget(b);
  }
  buttons_.clear();
  title_ = title;
  message_ = message;
  default_ = default_button;
  cancel_ = cancel_button;
  begin();
  static std::vector<std::string> labels;
  labels = buttons;
  for (size_t i = 0; i < buttons.size(); ++i) {
    auto *b = new FlatButton(0, 0, S(100), S(34), buttons[i], int(i) == default_button);
    b->copy_label(buttons[i].c_str());
    b->labeltype(FL_NO_LABEL);
    b->callback([](Fl_Widget *w, void *p) {
      auto *self = static_cast<Sheet *>(p);
      for (size_t k = 0; k < self->buttons_.size(); ++k)
        if (self->buttons_[k] == w) self->result_ = int(k);
      self->active_ = false;
    }, this);
    buttons_.push_back(b);
  }
  end();
  resize(0, 0, window()->w(), window()->h());
  layout_card();
  result_ = cancel_button;
  active_ = true;
  show();
  Fl_Widget *old_focus = Fl::focus();
  if (default_button >= 0 && default_button < int(buttons_.size())) buttons_[size_t(default_button)]->take_focus();
  window()->redraw();
  while (active_ && window()->shown()) Fl::wait();
  hide();
  window()->redraw();
  if (old_focus && old_focus->visible_r()) old_focus->take_focus();
  return result_;
}

void Sheet::draw() {
  const Look &L = look();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), Color::hex(0x000000, L.dark ? 0.45 : 0.28));
    double r = Sd(L.radius + 6);
    p.shadow(cx_, cy_, cw_, ch_, r, L.shadow, S(20));
    p.fill_rounded(cx_, cy_, cw_, ch_, r, L.card);
    if (L.glass) p.glass_sheen(cx_, cy_, cw_, ch_ * 0.6, r, L.dark);
    p.stroke_rounded(cx_, cy_, cw_, ch_, r, L.card_border);
  }
  text_font(L.theme.font_size + S(3), true);
  draw_text(title_, int(cx_ + S(24)), int(cy_ + S(18)), int(cw_ - S(48)), S(28), L.text);
  text_font(L.theme.font_size);
  fl_color(L.dim.fl());
  fl_draw(message_.c_str(), int(cx_ + S(24)), int(cy_ + S(54)), int(cw_ - S(48)), int(ch_ - S(120)),
          FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
  draw_children();
}

int Sheet::handle(int event) {
  if (!active_) return 0;
  if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
    if (Fl::event_key() == FL_Escape) {
      result_ = cancel_;
      active_ = false;
      return 1;
    }
    if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
      for (size_t k = 0; k < buttons_.size(); ++k)
        if (Fl::focus() == buttons_[k]) result_ = int(k);
      if (Fl::focus() != nullptr && std::find(buttons_.begin(), buttons_.end(), Fl::focus()) == buttons_.end())
        result_ = default_;
      active_ = false;
      return 1;
    }
    if (Fl::event_key() == FL_Left || Fl::event_key() == FL_Right || Fl::event_key() == FL_Tab) {
      int cur = 0;
      for (size_t k = 0; k < buttons_.size(); ++k)
        if (Fl::focus() == buttons_[k]) cur = int(k);
      int n = int(buttons_.size());
      bool back = Fl::event_key() == FL_Left || (Fl::event_state() & FL_SHIFT);
      if (n) buttons_[size_t((cur + (back ? n - 1 : 1)) % n)]->take_focus();
      redraw();
      return 1;
    }
  }
  int r = Fl_Group::handle(event);
  if (event == FL_PUSH || event == FL_RELEASE || event == FL_DRAG || event == FL_MOVE || event == FL_MOUSEWHEEL ||
      event == FL_KEYBOARD || event == FL_SHORTCUT)
    return 1;
  return r;
}

// --------------------------------------------------------------- Settings

namespace {

std::vector<std::string> monospace_fonts() {
  static std::vector<std::string> cache;
  if (!cache.empty()) return cache;
  std::set<std::string> names;
  for (const char *spacing : {":spacing=100", ":spacing=90"}) {
    int status = 0;
    std::string out = run_sync({"fc-list", spacing, "family"}, &status, 3000);
    for (auto &line : split(out, '\n')) {
      std::string f = trim(line.substr(0, line.find(',')));
      if (!f.empty()) names.insert(f);
    }
  }
  cache.push_back("Monospace");
  for (auto &n : names)
    if (n != "Monospace") cache.push_back(n);
  return cache;
}

class SettingsWindow;
SettingsWindow *g_settings = nullptr;

class SettingsWindow : public BufferedWindow {
 public:
  explicit SettingsWindow(App *app) : BufferedWindow(S(780), S(600), _("Niny Settings")), app_(app) {
    build();
    end();
    xclass("Niny");
    set_non_modal();
    callback([](Fl_Widget *w, void *) {
      w->hide();
      g_settings = nullptr;
      Fl::delete_widget(w);
    });
  }

  void draw() override {
    const Look &L = look();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), L.window);
      if (L.glass) p.fill_gradient(0, 0, w(), S(84), 0, L.header_top, L.window);
      icon::logo(p, S(24), S(22), S(40));
      for (auto &c : cards_) {
        p.fill_rounded(c.x, c.y, c.w, c.h, Sd(L.radius + 4), L.card);
        p.stroke_rounded(c.x, c.y, c.w, c.h, Sd(L.radius + 4), L.border);
      }
    }
    text_font(L.theme.font_size + S(6), true);
    draw_text(_("Settings"), S(76), S(20), w() - S(100), S(28), L.text);
    text_font(L.theme.font_size - S(1));
    draw_text(home_relative(settings_path()), S(76), S(46), w() - S(100), S(20), L.dim);
    for (auto &c : cards_) {
      text_font(L.theme.font_size - S(1), true);
      draw_text(c.title, int(c.x + S(16)), int(c.y + S(10)), int(c.w - S(32)), S(22), L.accent_text);
    }
    for (auto &lbl : labels_) {
      text_font(L.theme.font_size + S(1));
      draw_text(lbl.text, lbl.x, lbl.y, lbl.w, lbl.h, L.text);
    }
    draw_children();
  }

 private:
  struct Card {
    double x, y, w, h;
    std::string title;
  };
  struct Label {
    std::string text;
    int x, y, w, h;
  };

  Settings &s() { return app_->settings(); }
  void changed() { app_->apply_settings(true); }

  void sw(int col_x, int &y, int cw, const char *label, bool Settings::*field) {
    auto *w = new Switch(col_x + S(16), y, cw - S(32), S(36), label);
    w->value(s().*field);
    struct Ctx {
      SettingsWindow *self;
      bool Settings::*field;
    };
    auto *ctx = new Ctx{this, field};
    w->callback([](Fl_Widget *wd, void *p) {
      auto *c = static_cast<Ctx *>(p);
      c->self->s().*(c->field) = static_cast<Switch *>(wd)->value() != 0;
      c->self->changed();
    }, ctx);
    y += S(38);
  }

  void choice(int col_x, int &y, int cw, const char *label, std::function<std::string()> current,
              std::function<std::vector<std::string>()> options, std::function<void(const std::string &)> set) {
    labels_.push_back({label, col_x + S(16), y, cw / 2, S(36)});
    auto *b = new FlatButton(col_x + cw / 2, y + S(3), cw / 2 - S(16), S(30), current());
    struct Ctx {
      SettingsWindow *self;
      FlatButton *b;
      std::function<std::string()> current;
      std::function<std::vector<std::string>()> options;
      std::function<void(const std::string &)> set;
    };
    auto *ctx = new Ctx{this, b, current, options, set};
    b->callback([](Fl_Widget *, void *p) {
      auto *c = static_cast<Ctx *>(p);
      auto opts = c->options();
      std::vector<Fl_Menu_Item> items(opts.size() + 1);
      for (size_t i = 0; i < opts.size(); ++i) {
        items[i] = Fl_Menu_Item{opts[i].c_str(), 0, nullptr, nullptr, 0, FL_NORMAL_LABEL, 0, 0, 0};
        if (opts[i] == c->current()) items[i].flags |= FL_MENU_VALUE;
      }
      items[opts.size()] = Fl_Menu_Item{nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0};
      const Fl_Menu_Item *m = items[0].popup(c->b->x(), c->b->y() + c->b->h());
      if (m && m->text) {
        c->set(m->text);
        c->b->text(c->current());
        c->self->changed();
      }
    }, ctx);
    y += S(38);
  }

  void build() {
    int pad = S(20), top = S(84);
    int cw = (w() - 3 * pad) / 2;
    int lx = pad, rx = 2 * pad + cw;
    begin();
    // Editor.
    int y = top + S(38);
    int start = top;
    choice(lx, y, cw, _("Font"), [this] { return s().font; }, [] { return monospace_fonts(); },
           [this](const std::string &v) { s().font = v; });
    choice(lx, y, cw, _("Font size"), [this] { return std::to_string(s().font_size); },
           [] {
             std::vector<std::string> v;
             for (int i : {8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24}) v.push_back(std::to_string(i));
             return v;
           },
           [this](const std::string &v) {
             s().font_size = atoi(v.c_str());
             s().zoom = 0;
           });
    choice(lx, y, cw, _("Tab width"), [this] { return std::to_string(s().tab_width); },
           [] { return std::vector<std::string>{"2", "3", "4", "8"}; },
           [this](const std::string &v) { s().tab_width = atoi(v.c_str()); });
    choice(lx, y, cw, _("Margin line"),
           [this] { return s().right_margin ? std::to_string(s().right_margin) : std::string(_("Off")); },
           [] { return std::vector<std::string>{_("Off"), "72", "80", "100", "120"}; },
           [this](const std::string &v) { s().right_margin = atoi(v.c_str()); });
    sw(lx, y, cw, _("Insert spaces for Tab"), &Settings::insert_spaces);
    sw(lx, y, cw, _("Detect indentation of files"), &Settings::detect_indent);
    sw(lx, y, cw, _("Automatic indentation"), &Settings::auto_indent);
    sw(lx, y, cw, _("Close brackets and quotes"), &Settings::auto_close);
    sw(lx, y, cw, _("Wrap long lines"), &Settings::word_wrap);
    cards_.push_back({double(lx), double(start), double(cw), double(y - start + S(6)), _("EDITOR")});
    // View.
    y = top + S(38);
    start = top;
    choice(rx, y, cw, _("Colors"),
           [this] {
             return std::string(s().color_scheme == "dark" ? _("Dark") : s().color_scheme == "light" ? _("Light")
                                                                                                      : _("Follow SDE"));
           },
           [] { return std::vector<std::string>{_("Follow SDE"), _("Light"), _("Dark")}; },
           [this](const std::string &v) {
             s().color_scheme = v == _("Dark") ? "dark" : v == _("Light") ? "light" : "auto";
           });
    sw(rx, y, cw, _("Line numbers"), &Settings::line_numbers);
    sw(rx, y, cw, _("Highlight current line"), &Settings::highlight_line);
    sw(rx, y, cw, _("Highlight matching brackets"), &Settings::bracket_match);
    sw(rx, y, cw, _("Status bar"), &Settings::status_bar);
    cards_.push_back({double(rx), double(start), double(cw), double(y - start + S(6)), _("VIEW")});
    // Files.
    y += S(18);
    start = y;
    y += S(38);
    sw(rx, y, cw, _("Reopen files from last time"), &Settings::restore_session);
    sw(rx, y, cw, _("Back up unsaved changes"), &Settings::backups);
    sw(rx, y, cw, _("Keep unsaved changes on quit"), &Settings::hot_exit);
    sw(rx, y, cw, _("Watch files for changes"), &Settings::watch_files);
    sw(rx, y, cw, _("Trim trailing spaces on save"), &Settings::trim_trailing);
    sw(rx, y, cw, _("End files with a newline"), &Settings::final_newline);
    cards_.push_back({double(rx), double(start), double(cw), double(y - start + S(6)), _("FILES")});
    int bottom = 0;
    for (auto &c : cards_) bottom = std::max(bottom, int(c.y + c.h));
    auto *done = new FlatButton(w() - pad - S(110), bottom + S(14), S(110), S(34), _("Done"), true);
    done->callback([](Fl_Widget *w, void *) { w->window()->do_callback(); });
    size(w(), bottom + S(64));
  }

  App *app_;
  std::vector<Card> cards_;
  std::vector<Label> labels_;
};

}  // namespace

void show_settings(App *app) {
  if (g_settings) {
    g_settings->show();
    return;
  }
  g_settings = new SettingsWindow(app);
  g_settings->position(app->x() + (app->w() - g_settings->w()) / 2, app->y() + S(40));
  g_settings->show();
}

}  // namespace niny
