// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "about.h"

#include <FL/Fl.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <langinfo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "buffered_window.h"
#include "build_config.h"
// The toolkit credit names SDE-FLTK's own version ("1.3.11-sde2"), not the
// FLTK release that fltk-config reports.
#undef SDE_FLTK_VERSION
#include <FL/sde_fltk.h>
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "legal.h"
#include "legal_data.h"
#include "menu.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

namespace sde {

// Constant-initialized from the generated header: AppInfo objects with
// static storage (e.g. sysreport's options) read them before main().
const char *const kDesktopName = legal_data::kProjectName;
const char *const kDeveloper = legal_data::kProjectDeveloper;
const char *const kDeveloperId = legal_data::kProjectDeveloperId;
const char *const kCopyright = legal_data::kProjectCopyright;
const char *const kProjectWebsite = legal_data::kProjectWebsite;

std::string developer_line() { return format(_("Developer: %s"), kDeveloper); }

std::string part_of_line() { return format(_("Part of %s"), kDesktopName); }

std::string license_name(const std::string &spdx) { return legal::license_short_name(spdx); }

std::string toolkit_credit() {
  return format(_("Built with SDE-FLTK %s, based on FLTK by Bill Spitzak and others"),
                SDE_FLTK_VERSION);
}

namespace {

// ------------------------------------------------------------ helpers

const Theme &about_theme() {
  if (!active_theme().font.empty()) return active_theme();
  // Programs that never applied a theme (daemons, tests): the configured one.
  static Theme t;
  static bool init = false;
  if (!init) {
    Config cfg;
    cfg.load();
    double s = ui_scale();
    t = Theme::from_config(cfg);
    set_ui_scale(s);
    init = true;
  }
  return t;
}

void qa_log(const std::string &line) {
  std::string path = env("SDE_ABOUT_QA_FILE");
  if (path.empty()) return;
  if (FILE *f = fopen(path.c_str(), "a")) {
    fprintf(f, "%s\n", line.c_str());
    fclose(f);
  }
}

std::vector<std::string> wrap(const std::string &text, int width) {
  std::vector<std::string> lines;
  std::string line, word;
  auto flush_word = [&] {
    if (word.empty()) return;
    std::string cand = line.empty() ? word : line + " " + word;
    if (!line.empty() && text_width(cand) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = cand;
    }
    word.clear();
  };
  for (char c : text) {
    if (c == '\n') {
      flush_word();
      lines.push_back(line);
      line.clear();
    } else if (c == ' ') {
      flush_word();
    } else {
      word += c;
    }
  }
  flush_word();
  if (!line.empty()) lines.push_back(line);
  return lines;
}

std::string strip_scheme(std::string url) {
  for (const char *pre : {"https://", "http://"})
    if (url.rfind(pre, 0) == 0) url = url.substr(strlen(pre));
  return url;
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// The window that owns a dialog: the first visible normal window.
Fl_Window *owner_window(Fl_Window *self) {
  for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w))
    if (w != self && !w->menu_window() && !w->tooltip_window() && w->visible()) return w;
  return nullptr;
}

// Work area of the screen the dialog opens on.
void work_area(Fl_Window *parent, int &x, int &y, int &w, int &h) {
  int n = parent ? Fl::screen_num(parent->x() + parent->w() / 2, parent->y() + parent->h() / 2) : 0;
  Fl::screen_work_area(x, y, w, h, n);
}

void place(Fl_Window *win, Fl_Window *parent) {
  int sx, sy, sw, sh;
  work_area(parent, sx, sy, sw, sh);
  int x = parent ? parent->x() + (parent->w() - win->w()) / 2 : sx + (sw - win->w()) / 2;
  int y = parent ? parent->y() + (parent->h() - win->h()) / 3 : sy + (sh - win->h()) / 3;
  x = std::max(sx, std::min(x, sx + sw - win->w()));
  y = std::max(sy, std::min(y, sy + sh - win->h()));
  win->position(x, y);
}

void run_modal(Fl_Window *win) {
  win->set_modal();
  win->show();
  while (win->shown()) Fl::wait();
}

// AppInfo with the legal data filled in.
struct Resolved {
  AppInfo info;
  const legal::Component *component = nullptr;
  std::string license;
  std::vector<std::string> copyright, trademarks;
  // The program's own icons (before |info.logo|), then generic ones.
  std::vector<std::string> icons, generic_icons;
  std::vector<UpstreamCredit> upstream;
};

Resolved resolve(const AppInfo &info) {
  Resolved r;
  r.info = info;
  std::string id = info.component.empty() ? info.program : info.component;
  r.component = legal::find(id);
  if (!r.component && !info.program.empty()) r.component = legal::find(info.program);
  r.license = !info.license.empty() ? info.license
              : r.component     ? r.component->license
                                : std::string(legal::kLicense);
  r.copyright = info.copyright;
  if (r.copyright.empty() && r.component) r.copyright = r.component->copyright;
  if (r.copyright.empty()) r.copyright = {kCopyright};
  r.upstream = info.upstream;
  if (r.upstream.empty() && r.component)
    for (const auto &u : r.component->upstream)
      r.upstream.push_back({u.project, u.url, u.license, u.copyright, u.relation});
  // Upstream copyright lines belong to the copyright notices too.
  for (const auto &u : r.upstream)
    for (const auto &part : split(u.copyright, ';')) {
      std::string line = trim(part);
      bool known = line.empty();
      for (const auto &c : r.copyright) known = known || starts_with(c, line);
      if (!known) r.copyright.push_back(line + " \xC2\xB7 " + u.project);
    }
  // Trademark notices: SDE's, the component's, the program's extra ones and
  // those its texts need (Linux, Debian).
  std::vector<std::string> keys = info.trademarks;
  std::string text = info.description;
  for (const auto &c : info.credits) text += "\n" + c;
  for (const auto &u : r.upstream) text += "\n" + u.project;
  for (const auto &k : legal::trademark_keys_for_text(text)) keys.push_back(k);
  r.trademarks = legal::trademarks_for(r.component, keys);
  if (!info.icon.empty()) r.icons.push_back(info.icon);
  for (const auto &n : info.icon_fallbacks) r.icons.push_back(n);
  if (r.icons.empty() && r.component && !r.component->icons.empty()) r.icons.push_back(r.component->icons[0]);
  if (r.component)
    for (const auto &n : r.component->icons)
      if (!contains(r.icons, n)) r.generic_icons.push_back(n);
  if (!info.program.empty() && !contains(r.icons, info.program)) r.generic_icons.push_back(info.program);
  r.generic_icons.push_back("application-x-executable");
  return r;
}

// ------------------------------------------------------- content model
//
// A section is a list of items laid out top to bottom in the content area;
// it scrolls when it is taller than the window allows.

enum Style { TITLE, HEADING, BODY, DIM, SMALL };

struct Item {
  enum Kind { TEXT, ICON, MARK, ROW, SPACE, CARD_BEGIN, CARD_END } kind = TEXT;
  std::string text, value;
  Style style = BODY;
  bool center = false;
  int size = 0;  // ICON: icon size; SPACE: height
  // Laid out:
  int y = 0, h = 0;
  std::vector<std::string> lines, value_lines;
};

void set_style_font(const Theme &t, Style s) {
  switch (s) {
    case TITLE: text_font(t.font_size + S(7), true); break;
    case HEADING: text_font(t.font_size, true); break;
    case BODY: text_font(t.font_size); break;
    case DIM: text_font(t.font_size - S(1)); break;
    case SMALL: text_font(std::max(S(10), t.font_size - S(2))); break;
  }
}

int line_height(const Theme &t, Style s) {
  switch (s) {
    case TITLE: return t.font_size + S(16);
    case HEADING: return t.font_size + S(10);
    case BODY: return t.font_size + S(8);
    case DIM: return t.font_size + S(6);
    case SMALL: return t.font_size + S(4);
  }
  return t.font_size + S(8);
}

struct Section {
  std::string id, title;
  std::vector<Item> items;
  int height = 0;
};

Item text_item(const std::string &text, Style style, bool center = false) {
  Item i;
  i.kind = Item::TEXT;
  i.text = text;
  i.style = style;
  i.center = center;
  return i;
}

Item space(int h) {
  Item i;
  i.kind = Item::SPACE;
  i.size = h;
  return i;
}

Item row(const std::string &label, const std::string &value) {
  Item i;
  i.kind = Item::ROW;
  i.text = label;
  i.value = value;
  return i;
}

Item card(bool begin) {
  Item i;
  i.kind = begin ? Item::CARD_BEGIN : Item::CARD_END;
  return i;
}

// Paragraphs of a notice ("\n\n" separated) as body text items.
void add_paragraphs(std::vector<Item> &items, const std::string &text, Style style) {
  size_t start = 0;
  bool first = true;
  while (start <= text.size()) {
    size_t p = text.find("\n\n", start);
    std::string para = text.substr(start, p == std::string::npos ? std::string::npos : p - start);
    if (!first) items.push_back(space(S(8)));
    items.push_back(text_item(para, style));
    first = false;
    if (p == std::string::npos) break;
    start = p + 2;
  }
}

int layout(const Theme &t, Section &s, int width) {
  int y = 0, card_pad = S(10);
  bool in_card = false;
  int kw = std::min(S(118), width / 3);
  for (auto &it : s.items) {
    it.y = y;
    int inner = in_card ? width - 2 * card_pad : width;
    switch (it.kind) {
      case Item::TEXT:
        set_style_font(t, it.style);
        it.lines = wrap(it.text, inner);
        if (it.lines.empty()) it.lines.push_back("");
        it.h = line_height(t, it.style) * int(it.lines.size());
        break;
      case Item::ICON:
        it.h = it.size + S(8);
        break;
      case Item::MARK:
        set_style_font(t, BODY);
        it.lines = wrap(it.text, inner - S(28));
        it.h = std::max(S(26), line_height(t, BODY) * int(it.lines.size()));
        break;
      case Item::ROW:
        set_style_font(t, DIM);
        it.value_lines = wrap(it.value, inner - kw - S(8));
        if (it.value_lines.empty()) it.value_lines.push_back("");
        it.h = std::max(S(28), line_height(t, DIM) * int(it.value_lines.size()) + S(8));
        break;
      case Item::SPACE:
        it.h = it.size;
        break;
      case Item::CARD_BEGIN:
        in_card = true;
        it.h = S(6);
        break;
      case Item::CARD_END:
        in_card = false;
        it.h = S(6);
        break;
    }
    y += it.h;
  }
  s.height = y;
  return y;
}

// ------------------------------------------------------------ widgets

class AboutWindow;

// One button of the section bar (a segmented control).
class TabButton : public Fl_Button {
 public:
  TabButton(int x, int y, int w, int h, AboutWindow *owner, int index, std::string label)
      : Fl_Button(x, y, w, h), owner_(owner), index_(index), label_(std::move(label)) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    copy_tooltip(label_.c_str());
  }
  void draw() override;
  int handle(int event) override;
  const std::string &text() const { return label_; }

 private:
  AboutWindow *owner_;
  int index_;
  std::string label_;
};

class AboutWindow : public BufferedWindow {
 public:
  AboutWindow(int w, int h, const char *title, Resolved r)
      : BufferedWindow(0, 0, w, h, title), r_(std::move(r)) {}

  Resolved r_;
  std::vector<Section> sections_;
  std::vector<TabButton *> tabs_;
  FlatButton *close_ = nullptr, *website_ = nullptr, *view_license_ = nullptr;
  int current_ = 0, scroll_ = 0;
  int content_x = 0, content_y = 0, content_w = 0, content_h = 0;
  bool compact_ = false;

  int current() const { return current_; }
  int max_scroll() const {
    return std::max(0, sections_[size_t(current_)].height - content_h);
  }

  void select(int index, bool focus_tab) {
    int n = int(sections_.size());
    index = (index % n + n) % n;
    bool changed = index != current_;
    current_ = index;
    scroll_ = 0;
    if (website_) {
      if (current_ == 0) website_->show(); else website_->hide();
    }
    if (view_license_) {
      if (current_ == 2) view_license_->show(); else view_license_->hide();
    }
    if (focus_tab) tabs_[size_t(current_)]->take_focus();
    if (changed || focus_tab) qa_log("section " + sections_[size_t(current_)].id);
    redraw();
  }

  void scroll_by(int dy) {
    int s = std::max(0, std::min(max_scroll(), scroll_ + dy));
    if (s != scroll_) {
      scroll_ = s;
      redraw();
    }
  }

  void draw() override {
    const Theme &t = about_theme();
    const Section &s = sections_[size_t(current_)];
    int W = w();
    tile_size_ = 0;
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      if (t.glass) p.glass_sheen(0, 0, w(), S(150), 0, t.dark);
      // Section bar background.
      if (!tabs_.empty()) {
        int bx = tabs_.front()->x() - S(3), by = tabs_.front()->y() - S(3);
        int bw = tabs_.back()->x() + tabs_.back()->w() - bx + S(3), bh = tabs_.front()->h() + S(6);
        p.fill_rounded(bx, by, bw, bh, std::min<double>(t.radius, bh / 2.0), t.surface_alt);
        p.stroke_rounded(bx + 0.5, by + 0.5, bw - 1, bh - 1, std::min<double>(t.radius, bh / 2.0),
                         t.border.alpha(0.6));
      }
      cairo_t *cr = p.cr();
      cairo_save(cr);
      cairo_rectangle(cr, content_x - S(4), content_y, content_w + S(8), content_h);
      cairo_clip(cr);
      int card_top = -1;
      for (const auto &it : s.items) {
        int y = content_y + it.y - scroll_;
        if (it.kind == Item::CARD_BEGIN) card_top = y;
        if (it.kind == Item::CARD_END && card_top >= 0) {
          p.fill_rounded(content_x, card_top, content_w, y + it.h - card_top, t.radius, t.surface_alt);
          card_top = -1;
        }
      }
      for (const auto &it : s.items) {
        int y = content_y + it.y - scroll_;
        if (y + it.h < content_y || y > content_y + content_h) continue;
        if (it.kind == Item::ICON) draw_icon(p, t, (W - it.size) / 2.0, y, it.size);
        if (it.kind == Item::MARK) {
          set_style_font(t, BODY);
          int tw = 0;
          for (const auto &l : it.lines) tw = std::max(tw, text_width(l));
          int ms = S(20), total = ms + S(8) + tw;
          int mx = it.center ? (W - total) / 2 : content_x;
          draw_mark(p, t, mx, y + (it.h - ms) / 2, ms);
        }
      }
      // Rows inside cards get a hairline between them.
      bool in_card = false;
      int last_row_end = -1;
      for (const auto &it : s.items) {
        int y = content_y + it.y - scroll_;
        if (it.kind == Item::CARD_BEGIN) {
          in_card = true;
          last_row_end = -1;
        } else if (it.kind == Item::CARD_END) {
          in_card = false;
        } else if (it.kind == Item::ROW && in_card) {
          if (last_row_end >= 0)
            p.line(content_x + S(12), y + 0.5, content_x + content_w - S(12), y + 0.5, t.border.alpha(0.7));
          last_row_end = y + it.h;
        }
      }
      cairo_restore(cr);
      // Scroll position.
      if (max_scroll() > 0) {
        double track = content_h - S(8);
        double thumb = std::max<double>(S(24), track * content_h / double(s.height));
        double ty = content_y + S(4) + (track - thumb) * scroll_ / double(max_scroll());
        p.fill_rounded(W - S(9), ty, S(4), thumb, S(2), t.text_dim.alpha(0.45));
      }
      // Footer line.
      int fy = content_y + content_h + S(8);
      p.line(S(20), fy + 0.5, W - S(20), fy + 0.5, t.border.alpha(0.8));
    }
    fl_push_clip(content_x - S(4), content_y, content_w + S(8), content_h);
    if (tile_size_ > 0) {
      text_font(int(tile_size_ * 0.5), true);
      draw_text(r_.info.name.substr(0, 1), tile_x_, tile_y_, tile_size_, tile_size_, t.on_accent, 1);
    }
    int kw = std::min(S(118), content_w / 3);
    bool in_card = false;
    for (const auto &it : s.items) {
      int y = content_y + it.y - scroll_;
      if (it.kind == Item::CARD_BEGIN) in_card = true;
      if (it.kind == Item::CARD_END) in_card = false;
      if (y + it.h < content_y || y > content_y + content_h) continue;
      int x = content_x + (in_card ? S(10) : 0), cw = content_w - (in_card ? S(20) : 0);
      if (it.kind == Item::TEXT) {
        set_style_font(t, it.style);
        int lh = line_height(t, it.style);
        Color c = it.style == DIM || it.style == SMALL ? t.text_dim : t.text;
        for (const auto &l : it.lines) {
          draw_text(l, x, y, cw, lh, c, it.center ? 1 : 0);
          y += lh;
        }
      } else if (it.kind == Item::MARK) {
        set_style_font(t, BODY);
        int tw = 0;
        for (const auto &l : it.lines) tw = std::max(tw, text_width(l));
        int ms = S(20), total = ms + S(8) + tw;
        int mx = it.center ? (w() - total) / 2 : content_x;
        int lh = line_height(t, BODY), ly = y + (it.h - lh * int(it.lines.size())) / 2;
        for (const auto &l : it.lines) {
          draw_text(l, mx + ms + S(8), ly, tw + S(4), lh, t.text);
          ly += lh;
        }
      } else if (it.kind == Item::ROW) {
        set_style_font(t, DIM);
        int lh = line_height(t, DIM);
        draw_text(it.text, x, y + S(4), kw, lh, t.text_dim);
        set_style_font(t, BODY);
        int vy = y + S(4);
        for (const auto &l : it.value_lines) {
          draw_text(l, x + kw + S(8), vy, cw - kw - S(8), lh, t.text);
          vy += lh;
        }
      }
    }
    fl_pop_clip();
    draw_children();
  }

  cairo_surface_t *find_icon(const std::vector<std::string> &names, int size) {
    std::string path;
    for (const auto &n : names)
      if (!(path = IconLoader::lookup(n, size)).empty()) break;
    return path.empty() ? nullptr : IconLoader::get({path}, size);
  }

  void draw_icon(Painter &p, const Theme &t, double x, double y, double size) {
    // The program's icon from the icon theme, else its own drawing, else a
    // generic icon.
    cairo_surface_t *img = find_icon(r_.icons, int(size));
    if (!img && r_.info.logo) {
      r_.info.logo(p, x, y, size);
      return;
    }
    if (!img) img = find_icon(r_.generic_icons, int(size));
    if (img) {
      p.image(img, x, y, size);
      return;
    }
    // No icon theme: a tile with the initial.
    p.fill_gradient(x, y, size, size, S(14), t.accent.mix(t.on_accent, 0.15), t.accent);
    tile_x_ = int(x);
    tile_y_ = int(y);
    tile_size_ = int(size);
  }

  void draw_mark(Painter &p, const Theme &t, double x, double y, int size) {
    if (!mark_ || mark_size_ != size || mark_dark_ != t.dark) {
      if (mark_) cairo_surface_destroy(mark_);
      // The mark alone, else the square sde-logo icon; the full logo (which
      // may carry the name) only when neither is installed.
      mark_ = legal::load_branding({"sde-mark"}, size, size, {"sde-logo"});
      if (!mark_) mark_ = legal::load_branding({legal::logo_name(t.dark)}, size, size);
      mark_size_ = size;
      mark_dark_ = t.dark;
    }
    if (mark_) {
      double iw = cairo_image_surface_get_width(mark_), ih = cairo_image_surface_get_height(mark_);
      p.surface(mark_, std::round(x + (size - iw) / 2), std::round(y + (size - ih) / 2));
    } else {
      // Without the logo files: a small accent badge.
      p.fill_rounded(x, y, size, size, size * 0.28, t.accent);
      p.fill_rounded(x + size * 0.22, y + size * 0.26, size * 0.56, size * 0.36, size * 0.06, t.on_accent);
      p.fill_rect(x + size * 0.42, y + size * 0.62, size * 0.16, size * 0.1, t.on_accent);
    }
  }

  void resize(int X, int Y, int W, int H) override {
    BufferedWindow::resize(X, Y, W, H);
    relayout();
  }

  void relayout();

  int handle(int event) override {
    if (event == FL_MOUSEWHEEL) {
      scroll_by(Fl::event_dy() * S(40));
      return 1;
    }
    if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
      int k = Fl::event_key();
      int st = Fl::event_state();
      bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT, alt = st & FL_ALT;
      if (k == FL_Escape) {
        hide();
        return 1;
      }
      if (ctrl && (k == FL_Tab || k == FL_Page_Down || k == FL_Page_Up)) {
        bool back = k == FL_Page_Up || (k == FL_Tab && shift);
        select(current_ + (back ? -1 : 1), false);
        return 1;
      }
      if (alt && k >= '1' && k < '1' + int(sections_.size())) {
        select(k - '1', false);
        return 1;
      }
      if (event == FL_KEYBOARD && !ctrl && !alt) {
        int page = std::max(S(40), content_h - S(40));
        switch (k) {
          case FL_Up: scroll_by(-S(40)); return 1;
          case FL_Down: scroll_by(S(40)); return 1;
          case FL_Page_Up: scroll_by(-page); return 1;
          case FL_Page_Down: scroll_by(page); return 1;
          case FL_Home: scroll_by(-scroll_); return 1;
          case FL_End: scroll_by(max_scroll()); return 1;
          case FL_Enter:
          case FL_KP_Enter:
            hide();  // focus is not on a button (they take Enter themselves)
            return 1;
          default: break;
        }
      }
    }
    int r = BufferedWindow::handle(event);
    if (event == FL_PUSH && !r) take_focus();
    return r;
  }

  ~AboutWindow() override {
    if (mark_) cairo_surface_destroy(mark_);
  }

 private:
  cairo_surface_t *mark_ = nullptr;
  int mark_size_ = 0;
  bool mark_dark_ = false;
  int tile_x_ = 0, tile_y_ = 0, tile_size_ = 0;  // letter tile drawn instead of an icon
};

void TabButton::draw() {
  const Theme &t = about_theme();
  bool on = owner_->current() == index_;
  bool hot = Fl::belowmouse() == this;
  {
    Painter p(window()->w(), window()->h());
    double r = std::min<double>(t.radius_small, h() / 2.0);
    if (on) {
      p.fill_rounded(x(), y(), w(), h(), r, t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border);
    } else if (hot) {
      p.fill_rounded(x(), y(), w(), h(), r, t.hover);
    }
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(t.font_size, on);
  draw_text(label_, x() + S(4), y(), w() - S(8), h(), on ? t.accent_text : t.text, 1);
}

int TabButton::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_LEAVE:
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      owner_->select(index_, true);
      return 1;
    case FL_RELEASE:
      return 1;
    case FL_KEYBOARD:
      if (Fl::focus() != this) return 0;
      if (Fl::event_key() == FL_Left) {
        owner_->select(index_ - 1, true);
        return 1;
      }
      if (Fl::event_key() == FL_Right) {
        owner_->select(index_ + 1, true);
        return 1;
      }
      if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
        owner_->select(index_, true);
        return 1;
      }
      return 0;
    default:
      return Fl_Button::handle(event);
  }
}

// ------------------------------------------------------------ sections

Section about_section(const Resolved &r, bool compact) {
  const AppInfo &info = r.info;
  Section s{"about", _("About"), {}, 0};
  Item icon;
  icon.kind = Item::ICON;
  icon.size = compact ? S(48) : S(64);
  s.items.push_back(icon);
  s.items.push_back(text_item(info.name, TITLE, true));
  s.items.push_back(text_item(info.version.empty() ? info.program : format(_("Version %s"), info.version.c_str()),
                              DIM, true));
  if (!info.description.empty()) {
    s.items.push_back(space(S(6)));
    s.items.push_back(text_item(info.description, BODY, true));
  }
  s.items.push_back(space(compact ? S(8) : S(12)));
  Item mark;
  mark.kind = Item::MARK;
  mark.text = part_of_line();
  mark.center = true;
  s.items.push_back(mark);
  if (!info.version.empty() && info.version != SDE_VERSION && !compact)
    s.items.push_back(text_item(format(_("SDE %s"), SDE_VERSION_LABEL), SMALL, true));
  s.items.push_back(space(compact ? S(6) : S(10)));
  s.items.push_back(card(true));
  s.items.push_back(row(_("Developer"), kDeveloper));
  if (!info.website.empty()) s.items.push_back(row(_("Website"), strip_scheme(info.website)));
  s.items.push_back(card(false));
  return s;
}

Section credits_section(const Resolved &r) {
  const AppInfo &info = r.info;
  Section s{"credits", _("Credits"), {}, 0};
  s.items.push_back(text_item(_("Developed by"), HEADING));
  s.items.push_back(text_item(format(_("%s and contributors"), kDeveloper), BODY));
  if (!r.upstream.empty()) {
    s.items.push_back(space(S(12)));
    s.items.push_back(text_item(_("Based on"), HEADING));
    for (const auto &u : r.upstream) {
      s.items.push_back(space(S(4)));
      s.items.push_back(text_item(legal::relation_label(u.relation) + " " + u.project, BODY));
      for (const auto &part : split(u.copyright, ';')) s.items.push_back(text_item(trim(part), DIM));
      s.items.push_back(text_item(format(_("License: %s"), legal::license_short_name(u.license).c_str()), DIM));
      s.items.push_back(text_item(strip_scheme(u.url), DIM));
    }
  }
  // Credits are translated by the program; the legal data's English credits
  // are shown in the open-source license list.
  const std::vector<std::string> &credits = info.credits;
  if (!credits.empty()) {
    s.items.push_back(space(S(12)));
    s.items.push_back(text_item(_("Acknowledgements"), HEADING));
    for (const auto &c : credits) s.items.push_back(text_item(c, DIM));
  }
  s.items.push_back(space(S(12)));
  s.items.push_back(text_item(_("Inspiration"), HEADING));
  s.items.push_back(text_item(
      _("The Selviana Desktop Environment is inspired by the Equinox Desktop Environment (EDE) and "
        "contains no EDE code."),
      DIM));
  return s;
}

Section license_section(const Resolved &r) {
  Section s{"license", _("License"), {}, 0};
  std::vector<std::string> parts = legal::license_parts(r.license);
  std::string main = parts.empty() ? r.license : parts[0];
  const legal::License *l = legal::license(main);
  s.items.push_back(text_item(l ? _(l->name.c_str()) : main, HEADING));
  s.items.push_back(text_item("SPDX: " + r.license, SMALL));
  s.items.push_back(space(S(8)));
  add_paragraphs(s.items, r.info.license_notice.empty() ? legal::license_notice(main) : r.info.license_notice, BODY);
  if (parts.size() > 1) {
    s.items.push_back(space(S(12)));
    s.items.push_back(text_item(_("Also included"), HEADING));
    for (size_t i = 1; i < parts.size(); ++i) {
      std::string who;
      for (const auto &u : r.upstream)
        if (contains(legal::license_parts(u.license), parts[i])) who += (who.empty() ? "" : ", ") + u.project;
      std::string line = legal::license_name(parts[i]);
      if (!who.empty()) line += "  ·  " + who;
      s.items.push_back(text_item(line, DIM));
    }
  }
  s.items.push_back(space(S(12)));
  std::string path = legal::license_text_path(main);
  s.items.push_back(text_item(path.empty() ? std::string(_("The full text is not installed on this computer; "
                                                           "View License shows where to read it."))
                                           : format(_("Full text: %s"), path.c_str()),
                              SMALL));
  return s;
}

Section legal_section(const Resolved &r) {
  Section s{"legal", _("Legal"), {}, 0};
  s.items.push_back(text_item(_("Copyright"), HEADING));
  for (const auto &c : r.copyright) s.items.push_back(text_item(c, BODY));
  s.items.push_back(space(S(12)));
  s.items.push_back(text_item(_("Trademarks"), HEADING));
  for (size_t i = 0; i < r.trademarks.size(); ++i) {
    if (i) s.items.push_back(space(S(4)));
    s.items.push_back(text_item(r.trademarks[i], DIM));
  }
  return s;
}

void AboutWindow::relayout() {
  if (sections_.empty()) return;
  const Theme &t = about_theme();
  int pad = S(20);
  // Section bar.
  int n = int(tabs_.size()), bar_h = S(30);
  int tab_w = 0;
  text_font(t.font_size, true);
  for (auto *b : tabs_) tab_w = std::max(tab_w, text_width(b->text()) + S(24));
  int total = std::min(w() - 2 * pad - S(6), tab_w * n);
  tab_w = total / std::max(1, n);
  int tx = (w() - tab_w * n) / 2;
  for (int i = 0; i < n; ++i) tabs_[size_t(i)]->resize(tx + i * tab_w, S(14), tab_w, bar_h);
  content_x = pad + S(4);
  content_y = S(14) + bar_h + S(14);
  content_w = w() - 2 * content_x;
  int footer = S(34) + S(16) + S(16);
  content_h = h() - content_y - footer - S(6);
  int by = h() - S(34) - S(16);
  if (close_) close_->resize(w() - pad - close_->w(), by, close_->w(), S(34));
  if (website_) website_->resize(pad, by, website_->w(), S(34));
  if (view_license_) view_license_->resize(pad, by, view_license_->w(), S(34));
  for (auto &s : sections_) layout(t, s, content_w);
  scroll_ = std::min(scroll_, max_scroll());
}

int button_width(const std::string &label) {
  text_font(about_theme().font_size);
  return std::max(S(100), text_width(label) + S(36));
}

// ------------------------------------------------------------ viewer

class ViewerWindow : public BufferedWindow {
 public:
  ViewerWindow(int w, int h, const char *title) : BufferedWindow(0, 0, w, h, title) {}
  Fl_Text_Display *display = nullptr;
  FlatButton *close = nullptr;
  void draw() override {
    const Theme &t = about_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      if (display) {
        // The text sits on a rounded panel with a margin around it.
        int m = S(8);
        p.fill_rounded(display->x() - m, display->y() - m, display->w() + 2 * m, display->h() + 2 * m,
                       t.radius_small, t.surface_alt);
        p.stroke_rounded(display->x() - m + 0.5, display->y() - m + 0.5, display->w() + 2 * m - 1,
                         display->h() + 2 * m - 1, t.radius_small, t.border);
      }
    }
    draw_children();
  }
  int handle(int event) override {
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
      hide();
      return 1;
    }
    return BufferedWindow::handle(event);
  }
};

}  // namespace

void show_license_viewer(const std::string &title, const std::string &text) {
  fl_open_display();
  const Theme &t = about_theme();
  Fl_Window *parent = owner_window(nullptr);
  int sx, sy, sw, sh;
  work_area(parent, sx, sy, sw, sh);
  int W = std::min(S(680), sw - S(16)), H = std::min(S(560), sh - S(16));
  auto *win = new ViewerWindow(W, H, title.c_str());
  win->copy_label(title.c_str());
  win->begin();
  int pad = S(16), bh = S(34);
  auto *buffer = new Fl_Text_Buffer();
  buffer->text(text.c_str());
  int m = S(8);
  auto *display = new Fl_Text_Display(pad + m, pad + m, W - 2 * pad - 2 * m, H - 2 * pad - bh - S(12) - 2 * m);
  display->buffer(buffer);
  display->box(FL_FLAT_BOX);
  display->color(t.surface_alt.fl());
  display->textcolor(t.text.fl());
  display->selection_color(t.selection_bg.fl());
  display->textfont(FL_COURIER);
  display->textsize(std::max(S(10), t.font_size - S(1)));
  display->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
  display->scrollbar_width(S(10));
  win->display = display;
  int cw = button_width(_("Close"));
  auto *close = new FlatButton(W - pad - cw, H - pad - bh, cw, bh, _("Close"), true);
  close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  win->close = close;
  win->end();
  win->resizable(display);
  win->size_range(S(320), S(240));
  win->color(t.surface.fl());
  place(win, parent);
  qa_log("viewer " + title);
  run_modal(win);
  display->buffer(nullptr);
  Fl::delete_widget(win);
  delete buffer;
}

void show_license(const AppInfo &info) {
  Resolved r = resolve(info);
  legal::Component c;
  if (r.component) c = *r.component;
  // component_license_text() shows the component's name: use the program's.
  c.name = info.name.empty() ? c.name : info.name;
  c.license = r.license;
  c.copyright = r.copyright;
  c.upstream.clear();
  for (const auto &u : r.upstream) c.upstream.push_back({u.project, u.url, u.license, u.copyright, u.relation});
  show_license_viewer(format(_("%s \xE2\x80\x94 License"), info.name.c_str()), legal::component_license_text(c));
}

void show_about(const AppInfo &info) {
  fl_open_display();
  // Programs that draw their own icons never pick an icon theme; without one
  // the About logo would fall back to the letter tile.
  if (!info.logo && IconLoader::theme() == "hicolor") {
    Config cfg;
    cfg.load();
    IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  }
  const Theme &t = about_theme();
  Resolved r = resolve(info);
  Fl_Window *parent = owner_window(nullptr);
  int sx, sy, sw, sh;
  work_area(parent, sx, sy, sw, sh);
  int W = std::min(S(480), sw - S(16));
  int max_h = sh - S(16);
  bool compact = max_h < S(440);
  std::string title = format(_("About %s"), info.name.c_str());
  auto *win = new AboutWindow(W, std::min(S(470), max_h), title.c_str(), r);
  win->copy_label(title.c_str());
  win->compact_ = compact;
  win->sections_ = {about_section(r, compact), credits_section(r), license_section(r), legal_section(r)};
  win->begin();
  for (size_t i = 0; i < win->sections_.size(); ++i)
    win->tabs_.push_back(new TabButton(0, 0, 10, 10, win, int(i), win->sections_[i].title));
  std::string site_label = _("Website"), view_label = _("View License"), close_label = _("Close");
  if (!info.website.empty()) {
    win->website_ = new FlatButton(0, 0, button_width(site_label), S(34), site_label);
    static std::string url;
    url = info.website;
    win->website_->callback([](Fl_Widget *, void *) { open_uri(url); });
    win->website_->tooltip(info.website.c_str());
  }
  win->view_license_ = new FlatButton(0, 0, button_width(view_label), S(34), view_label);
  static AppInfo shown;
  shown = info;
  win->view_license_->callback([](Fl_Widget *, void *) { show_license(shown); });
  win->view_license_->shortcut(FL_ALT + 'v');
  win->view_license_->tooltip(_("Shows the full license text (Alt+V)"));
  win->view_license_->hide();
  win->close_ = new FlatButton(0, 0, button_width(close_label), S(34), close_label, true);
  win->close_->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  win->end();
  win->color(t.surface.fl());
  // Height: the tallest section when it fits, else the work area (sections
  // scroll).
  win->relayout();
  int tallest = 0;
  for (const auto &s : win->sections_) tallest = std::max(tallest, s.height);
  int H = std::max(S(300), std::min(max_h, win->content_y + tallest + S(34) + S(38)));
  H = std::min(H, std::max(max_h, S(240)));
  win->size(W, H);
  win->relayout();
  win->size_range(S(360), S(240));
  place(win, parent);
  qa_log("about " + info.name);
  qa_log("section about");
  win->select(0, false);
  win->set_modal();
  win->show();
  win->close_->take_focus();
  while (win->shown()) Fl::wait();
  Fl::delete_widget(win);
}

// ------------------------------------------------- open-source licenses

namespace {

class LicenseList : public Fl_Widget {
 public:
  LicenseList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    for (const auto &c : legal::components())
      if (c.listed) entries_.push_back(&c);
  }
  const legal::Component *selected() const {
    return sel_ >= 0 && sel_ < int(entries_.size()) ? entries_[size_t(sel_)] : nullptr;
  }
  void open() {
    if (const legal::Component *c = selected()) {
      qa_log("viewer " + c->id);
      show_license_viewer(format(_("%s \xE2\x80\x94 License"), legal::display_name(*c).c_str()),
                          legal::component_license_text(*c));
    }
  }
  void draw() override {
    const Theme &t = about_theme();
    layout();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), t.radius, t.surface_alt);
      cairo_t *cr = p.cr();
      cairo_save(cr);
      cairo_rectangle(cr, x(), y(), w(), h());
      cairo_clip(cr);
      for (size_t i = 0; i < rows_.size(); ++i) {
        int ry = y() + rows_[i].y - scroll_;
        if (ry + rows_[i].h < y() || ry > y() + h()) continue;
        if (int(i) == sel_)
          p.fill_rounded(x() + S(4), ry + S(2), w() - S(8), rows_[i].h - S(4), t.radius_small,
                         Fl::focus() == this ? t.selection_bg : t.hover);
        else if (i)
          p.line(x() + S(14), ry + 0.5, x() + w() - S(14), ry + 0.5, t.border.alpha(0.6));
      }
      cairo_restore(cr);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), t.radius);
      if (total_ > h()) {
        double track = h() - S(8), thumb = std::max<double>(S(24), track * h() / double(total_));
        double ty = y() + S(4) + (track - thumb) * scroll_ / double(total_ - h());
        p.fill_rounded(x() + w() - S(8), ty, S(4), thumb, S(2), t.text_dim.alpha(0.45));
      }
    }
    fl_push_clip(x(), y(), w(), h());
    for (size_t i = 0; i < rows_.size(); ++i) {
      int ry = y() + rows_[i].y - scroll_;
      if (ry + rows_[i].h < y() || ry > y() + h()) continue;
      bool sel = int(i) == sel_ && Fl::focus() == this;
      const legal::Component &c = *entries_[i];
      int tx = x() + S(16), tw = w() - S(32);
      text_font(t.font_size, true);
      int ly = ry + S(8);
      draw_text(legal::display_name(c), tx, ly, tw, S(22), sel ? t.selection_text : t.text);
      ly += S(22);
      text_font(t.font_size - S(1));
      Color dim = sel ? t.selection_text : t.text_dim;
      for (const auto &l : rows_[i].lines) {
        draw_text(l, tx, ly, tw, S(19), dim);
        ly += S(19);
      }
    }
    fl_pop_clip();
  }
  int handle(int e) override {
    switch (e) {
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_PUSH: {
        take_focus();
        int my = Fl::event_y() - y() + scroll_;
        for (size_t i = 0; i < rows_.size(); ++i)
          if (my >= rows_[i].y && my < rows_[i].y + rows_[i].h) {
            sel_ = int(i);
            redraw();
            if (Fl::event_clicks()) open();
            break;
          }
        return 1;
      }
      case FL_MOUSEWHEEL:
        scroll_to(scroll_ + Fl::event_dy() * S(40));
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key(), n = int(entries_.size());
        if (k == FL_Up || k == FL_Down || k == FL_Home || k == FL_End || k == FL_Page_Up || k == FL_Page_Down) {
          int step = k == FL_Page_Up || k == FL_Page_Down ? 5 : 1;
          if (k == FL_Up || k == FL_Page_Up) sel_ = std::max(0, sel_ - step);
          if (k == FL_Down || k == FL_Page_Down) sel_ = std::min(n - 1, sel_ + step);
          if (k == FL_Home) sel_ = 0;
          if (k == FL_End) sel_ = n - 1;
          reveal();
          redraw();
          return 1;
        }
        if (k == FL_Enter || k == FL_KP_Enter || k == ' ') {
          open();
          return 1;
        }
        return 0;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  struct Row {
    int y = 0, h = 0;
    std::vector<std::string> lines;
  };
  std::vector<const legal::Component *> entries_;
  std::vector<Row> rows_;
  int sel_ = 0, scroll_ = 0, total_ = 0, laid_w_ = -1;

  void layout() {
    if (laid_w_ == w()) return;
    laid_w_ = w();
    const Theme &t = about_theme();
    text_font(t.font_size - S(1));
    rows_.clear();
    int y = 0, tw = w() - S(32);
    for (const auto *c : entries_) {
      Row r;
      r.y = y;
      for (const auto &l : wrap(legal::display_summary(*c), tw)) r.lines.push_back(l);
      for (const auto &l : wrap(format(_("License: %s"), legal::license_name(c->license).c_str()), tw))
        r.lines.push_back(l);
      for (const auto &cr : c->copyright)
        for (const auto &l : wrap(cr, tw)) r.lines.push_back(l);
      for (const auto &u : c->upstream)
        for (const auto &l : wrap(legal::relation_label(u.relation) + " " + u.project + " (" +
                                      legal::license_short_name(u.license) + ")",
                                  tw))
          r.lines.push_back(l);
      r.h = S(8) + S(22) + S(19) * int(r.lines.size()) + S(10);
      y += r.h;
      rows_.push_back(std::move(r));
    }
    total_ = y;
  }
  void scroll_to(int s) {
    s = std::max(0, std::min(s, std::max(0, total_ - h())));
    if (s != scroll_) {
      scroll_ = s;
      redraw();
    }
  }
  void reveal() {
    layout();
    if (sel_ < 0 || sel_ >= int(rows_.size())) return;
    const Row &r = rows_[size_t(sel_)];
    if (r.y < scroll_) scroll_to(r.y);
    else if (r.y + r.h > scroll_ + h()) scroll_to(r.y + r.h - h());
  }
};

class LicensesWindow : public BufferedWindow {
 public:
  LicensesWindow(int w, int h, const char *title) : BufferedWindow(0, 0, w, h, title) {}
  LicenseList *list = nullptr;
  std::string intro;
  void draw() override {
    const Theme &t = about_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
    }
    text_font(t.font_size + S(3), true);
    draw_text(_("Open-Source Licenses"), S(20), S(14), w() - S(40), S(30), t.text);
    text_font(t.font_size - S(1));
    int y = S(44);
    for (const auto &l : wrap(intro, w() - S(40))) {
      draw_text(l, S(20), y, w() - S(40), S(19), t.text_dim);
      y += S(19);
    }
    draw_children();
  }
  int handle(int event) override {
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
      hide();
      return 1;
    }
    return BufferedWindow::handle(event);
  }
};

}  // namespace

void show_open_source_licenses() {
  fl_open_display();
  const Theme &t = about_theme();
  Fl_Window *parent = owner_window(nullptr);
  int sx, sy, sw, sh;
  work_area(parent, sx, sy, sw, sh);
  int W = std::min(S(600), sw - S(16)), H = std::min(S(560), sh - S(16));
  std::string title = _("Open-Source Licenses");
  auto *win = new LicensesWindow(W, H, title.c_str());
  win->copy_label(title.c_str());
  win->intro = _("The Selviana Desktop Environment and the components it includes, with their licenses "
                 "and copyright holders. Select one and press Enter or View License for the full text.");
  text_font(t.font_size - S(1));
  int intro_h = S(19) * int(wrap(win->intro, W - S(40)).size());
  int pad = S(20), bh = S(34), top = S(44) + intro_h + S(12);
  win->begin();
  win->list = new LicenseList(pad, top, W - 2 * pad, H - top - bh - S(32));
  std::string view_label = _("View License"), close_label = _("Close");
  auto *view = new FlatButton(pad, H - bh - S(16), button_width(view_label), bh, view_label);
  view->callback([](Fl_Widget *b, void *) {
    static_cast<LicensesWindow *>(b->window())->list->open();
  });
  int cw = button_width(close_label);
  auto *close = new FlatButton(W - pad - cw, H - bh - S(16), cw, bh, close_label, true);
  close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  win->end();
  win->resizable(win->list);
  win->size_range(S(360), S(300));
  win->color(t.surface.fl());
  place(win, parent);
  qa_log("licenses");
  win->set_modal();
  win->show();
  win->list->take_focus();
  while (win->shown()) Fl::wait();
  Fl::delete_widget(win);
}

int handle_about_event(int event, const AppInfo &info) {
  if ((event == FL_SHORTCUT || event == FL_KEYBOARD) && Fl::event_key() == FL_F + 1) {
    show_about(info);
    return 1;
  }
  if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
    ContextMenu menu;
    MenuItemOptions o;
    o.icon = "info";
    o.shortcut = FL_F + 1;
    AppInfo app = info;
    menu.item(format(_("About %s"), info.name.c_str()), [app] { show_about(app); }, o);
    menu.popup();
    return 1;
  }
  return 0;
}

std::string version_line(const AppInfo &info) {
  Resolved r = resolve(info);
  // "©" on UTF-8 terminals, "(C)" elsewhere. Programs may print the version
  // before setlocale(), so the locale variables count too.
  bool utf8 = false;
  if (const char *cs = nl_langinfo(CODESET)) utf8 = !strcmp(cs, "UTF-8") || !strcmp(cs, "utf8");
  for (const char *var : {"LC_ALL", "LC_CTYPE", "LANG"}) {
    std::string v = lower(env(var));
    if (v.empty()) continue;
    utf8 = utf8 || v.find("utf-8") != std::string::npos || v.find("utf8") != std::string::npos;
    break;
  }
  std::string out = info.program + " (" + kDesktopName + ") " + info.version + "\n";
  if (!info.version.empty() && info.version != SDE_VERSION)
    out += std::string("SDE ") + SDE_VERSION_LABEL + " (" + SDE_VERSION + ")\n";
  for (std::string line : r.copyright) {
    if (!utf8) {
      for (size_t p; (p = line.find("\xC2\xA9")) != std::string::npos;) line.replace(p, 2, "(C)");
      for (size_t p; (p = line.find("\xE2\x80\x93")) != std::string::npos;) line.replace(p, 3, "-");
    }
    out += line + "\n";
  }
  std::vector<std::string> parts = legal::license_parts(r.license);
  std::string urls;
  for (const auto &p : parts)
    if (const legal::License *l = legal::license(p)) urls += (urls.empty() ? "" : " ") + ("<" + l->url + ">");
  out += "License " + r.license + ": " + legal::license_short_name(r.license) + (urls.empty() ? "" : " " + urls) +
         ".\n";
  out += "This is free software: you are free to change and redistribute it.\n";
  out += "There is NO WARRANTY, to the extent permitted by law.\n";
  return out;
}

std::string version_line(const std::string &program, const std::string &version) {
  AppInfo info;
  info.program = program;
  info.version = version;
  return version_line(info);
}

int print_version(const AppInfo &info) {
  fputs(version_line(info).c_str(), stdout);
  return 0;
}

int print_version(const std::string &program, const std::string &version) {
  fputs(version_line(program, version).c_str(), stdout);
  return 0;
}

}  // namespace sde
