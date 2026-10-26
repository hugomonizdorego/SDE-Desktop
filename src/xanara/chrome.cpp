// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The window chrome of Xanara: the tab strip (Home and one tab per
// document), the main toolbar (side panel, pointer, zoom, the mode tabs,
// undo/redo, export and the properties panel) and the context toolbar with
// the tools of the active mode. Everything is laid out from the window
// width, so narrow or scaled windows move mode tabs and tools into
// overflow menus instead of clipping them.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#include "fonts.h"
#include "i18n.h"
#include "ocr.h"
#include "spawn.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

std::string zoom_text(double z) { return format("%d%%", int(std::lround(z * 100))); }

Color strip_color(const Theme &t) {
  return t.dark ? t.background.mix(Color::hex(0x000000), 0.22) : t.surface_alt.mix(t.border, 0.25);
}

int mode_font(const Theme &t) { return t.font_size + S(1); }

}  // namespace

// ----------------------------------------------------------------- layout

void XanaraWindow::layout() {
  DocView *v = view();
  bool presenting = v && v->presentation();
  seps_.clear();
  if (presenting) {
    tabs_h_ = bar_h_ = ctx_h_ = 0;
    hots_.clear();
    home_->hide();
    sidebar_->hide();
    props_->hide();
    page_input_->hide();
    find_input_->hide();
    v->resize(0, 0, w(), h());
    redraw();
    return;
  }
  tabs_h_ = S(40);
  bar_h_ = v ? S(50) : 0;
  ctx_h_ = v ? S(46) : 0;
  int top = tabs_h_ + bar_h_ + ctx_h_;
  if (!v) {
    home_->resize(0, tabs_h_, w(), h() - tabs_h_);
    home_->show();
    sidebar_->hide();
    props_->hide();
    page_input_->hide();
    find_input_->hide();
  } else {
    home_->hide();
    int rail = sidebar_->rail_w();
    // The panels give way to the pages on narrow windows.
    int min_canvas = S(300);
    bool panel = sidebar_on_ && w() - rail - std::min(side_w_, std::max(S(180), (w() - rail) / 3)) >= min_canvas;
    int panel_w = std::min(side_w_, std::max(S(180), (w() - rail) / 3));
    int sw = rail + (panel ? panel_w : 0);
    props_w_ = props_on_ && w() - sw - S(264) >= min_canvas ? S(264) : 0;
    sidebar_->set_panel_open(panel);
    sidebar_->resize(0, top, sw, h() - top);
    sidebar_->show();
    if (props_w_) {
      props_->resize(w() - props_w_, top, props_w_, h() - top);
      props_->show();
    } else {
      props_->hide();
    }
    int cw = w() - sw - props_w_;
    int other = compare_id_ >= 0 ? tablist_.index_of_id(compare_id_) : -1;
    if (other < 0 || other == tablist_.active()) compare_id_ = -1;
    if (compare_id_ >= 0) {
      // Compare: the other document beside this one.
      DocView *ov = tabs_[size_t(other)]->view;
      v->resize(sw, top, cw / 2, h() - top);
      ov->resize(sw + cw / 2 + 1, top, cw - cw / 2 - 1, h() - top);
      ov->show();
    } else {
      v->resize(sw, top, cw, h() - top);
    }
    int px, py, pw, ph;
    sidebar_->page_box(px, py, pw, ph);
    page_input_->resize(px, py, pw, ph);
    page_input_->show();
    if (panel && sidebar_->panel() == Sidebar::Search) {
      sidebar_->search_box(px, py, pw, ph);
      find_input_->resize(px, py, pw, ph);
      find_input_->show();
    } else {
      find_input_->hide();
    }
  }
  build_chrome();
  redraw();
}

void XanaraWindow::build_chrome() {
  hots_.clear();
  seps_.clear();
  ctx_hint_w_ = 0;
  if (tabs_h_) layout_tabs(0, tabs_h_);
  if (bar_h_) layout_toolbar(tabs_h_, bar_h_);
  if (ctx_h_) layout_context(tabs_h_ + bar_h_, ctx_h_);
  hover_hot_ = last_mx_ >= 0 ? hot_at(last_mx_, last_my_) : -1;
  if (press_hot_ >= int(hots_.size())) press_hot_ = -1;
}

int XanaraWindow::tab_x(int index) const { return tabs_x0_ + index * tab_w_; }

void XanaraWindow::layout_tabs(int y0, int hh) {
  int x = S(8);
  Hot home;
  home.kind = Hot::TabHome;
  home.glyph = icon::Home;
  home.text = _("Home");
  home.tip = _("Home: recent documents and quick actions");
  home.x = x;
  home.y = y0 + S(5);
  home.w = w() < S(760) ? S(44) : S(116);
  home.h = hh - S(5);
  home.toggled = tablist_.active() < 0;
  home.action = [this] { activate_tab(-1); };
  hots_.push_back(home);
  x += home.w + S(4);
  int n = tablist_.count();
  int plus_w = S(34);
  int avail = w() - x - plus_w - S(16);
  tabs_x0_ = x;
  tab_w_ = n ? std::clamp(avail / n, S(92), S(236)) : 0;
  for (int i = 0; i < n; ++i) {
    const DocTab &t = *tabs_[size_t(i)];
    Hot h;
    h.kind = Hot::Tab;
    h.tab = i;
    h.text = display_name(t.target);
    h.tip = t.target;
    h.x = tab_x(i);
    h.y = y0 + S(5);
    h.w = tab_w_ - S(2);
    h.h = hh - S(5);
    h.toggled = i == tablist_.active();
    h.enabled = true;
    h.action = [this, i] { activate_tab(i); };
    hots_.push_back(h);
    Hot c;
    c.kind = Hot::TabClose;
    c.glyph = icon::Close;
    c.tab = i;
    c.tip = _("Close (Ctrl+W)");
    c.w = c.h = S(22);
    c.x = h.x + h.w - c.w - S(8);
    c.y = h.y + (h.h - c.h) / 2;
    c.action = [this, i] { close_tab(i, true); };
    hots_.push_back(c);
  }
  Hot plus;
  plus.kind = Hot::Icon;
  plus.glyph = icon::Plus;
  plus.tip = _("Open a document in a new tab (Ctrl+O)");
  plus.x = std::min(tabs_x0_ + n * tab_w_ + S(4), w() - plus_w - S(8));
  plus.y = y0 + S(8);
  plus.w = plus_w;
  plus.h = hh - S(14);
  plus.action = [this] { open_dialog(); };
  hots_.push_back(plus);
}

void XanaraWindow::layout_toolbar(int y0, int hh) {
  DocView *v = view();
  int bs = S(34), by = y0 + (hh - bs) / 2;
  auto icon_hot = [&](icon::Kind g, const std::string &tip, std::function<void()> fn, int x) {
    Hot h;
    h.kind = Hot::Icon;
    h.glyph = g;
    h.tip = tip;
    h.action = std::move(fn);
    h.x = x;
    h.y = by;
    h.w = h.h = bs;
    h.row = 1;
    hots_.push_back(h);
    return &hots_.back();
  };
  auto sep = [&](int x) { seps_.push_back({x, by + S(7), bs - S(14)}); };
  int x = S(10);
  icon_hot(icon::Sidebar, _("Side panel (F9)"), [this] { toggle_sidebar(); }, x)->toggled = sidebar_on_;
  x += bs + S(2);
  {
    Hot h;
    h.kind = Hot::Pointer;
    h.glyph = v && v->tool() == Tool::Hand ? icon::Hand : icon::Select;
    h.chevron = true;
    h.tip = _("Pointer: select or hand (V)");
    h.x = x;
    h.y = by;
    h.w = S(52);
    h.h = bs;
    h.row = 1;
    h.action = [this] {
      for (const auto &hh2 : hots_)
        if (hh2.kind == Hot::Pointer) pointer_menu(hh2.x, hh2.y + hh2.h + S(4));
    };
    hots_.push_back(h);
    x += h.w + S(8);
  }
  bool compact = w() < S(980);
  sep(x);
  x += S(8);
  icon_hot(icon::ZoomOut, _("Zoom out (Ctrl+−)"), [this] { if (view()) view()->zoom_step(-1); }, x);
  x += bs;
  {
    Hot h;
    h.kind = Hot::Zoom;
    h.chevron = true;
    h.tip = _("Zoom");
    h.x = x;
    h.y = by;
    h.w = compact ? S(66) : S(84);
    h.h = bs;
    h.row = 1;
    h.action = [this] {
      for (const auto &hh2 : hots_)
        if (hh2.kind == Hot::Zoom) zoom_menu(hh2.x, hh2.y + hh2.h + S(4));
    };
    hots_.push_back(h);
    x += h.w;
  }
  icon_hot(icon::ZoomIn, _("Zoom in (Ctrl++)"), [this] { if (view()) view()->zoom_step(1); }, x);
  x += bs;
  int left_end = x;
  // Right side, from the window edge inwards.
  int rx = w() - S(10) - bs;
  icon_hot(icon::More, _("Menu"), [this] {
    for (const auto &hh2 : hots_)
      if (hh2.glyph == icon::More && hh2.row == 1) show_menu(hh2.x + hh2.w - S(240), hh2.y + hh2.h + S(4));
  }, rx);
  rx -= bs + S(2);
  icon_hot(icon::Properties, _("Properties panel (F10)"), [this] { toggle_props(); }, rx)->toggled = props_on_;
  rx -= S(8);
  sep(rx);
  rx -= S(8) + bs;
  icon_hot(icon::Share, _("Export and share"), [this] {
    for (const auto &hh2 : hots_)
      if (hh2.glyph == icon::Share) export_menu(hh2.x + hh2.w - S(220), hh2.y + hh2.h + S(4));
  }, rx);
  rx -= S(8);
  sep(rx);
  rx -= S(8) + bs;
  icon_hot(icon::Redo, _("Redo (Ctrl+Shift+Z)"), [this] { if (view()) view()->redo(); }, rx)->enabled =
      v && v->can_redo();
  rx -= bs;
  icon_hot(icon::Undo, _("Undo (Ctrl+Z)"), [this] { if (view()) view()->undo(); }, rx)->enabled = v && v->can_undo();
  int right_start = rx;
  // The mode tabs, centred in the window when they fit.
  text_font(mode_font(theme_), true);
  int pad = compact ? S(20) : S(30);
  std::vector<int> widths(kModeCount);
  int total = 0;
  for (int m = 0; m < kModeCount; ++m) {
    widths[size_t(m)] = text_width(mode_label(Mode(m))) + pad;
    total += widths[size_t(m)];
  }
  int a0 = left_end + S(10), a1 = right_start - S(10);
  std::vector<int> shown;
  int more_w = S(70);
  if (total <= a1 - a0) {
    for (int m = 0; m < kModeCount; ++m) shown.push_back(m);
  } else {
    int used = 0;
    for (int m = 0; m < kModeCount; ++m) {
      if (used + widths[size_t(m)] > a1 - a0 - more_w) break;
      shown.push_back(m);
      used += widths[size_t(m)];
    }
    int active = int(mode_);
    if (std::find(shown.begin(), shown.end(), active) == shown.end()) {
      while (!shown.empty() && used + widths[size_t(active)] > a1 - a0 - more_w) {
        used -= widths[size_t(shown.back())];
        shown.pop_back();
      }
      if (used + widths[size_t(active)] <= a1 - a0 - more_w) shown.push_back(active);
    }
  }
  int sum = 0;
  for (int m : shown) sum += widths[size_t(m)];
  bool overflow = int(shown.size()) < kModeCount;
  if (overflow) sum += more_w;
  int mx = std::clamp((w() - sum) / 2, a0, std::max(a0, a1 - sum));
  for (int m : shown) {
    Hot h;
    h.kind = Hot::ModeTab;
    h.text = mode_label(Mode(m));
    h.tip = format(_("%s tools (Ctrl+Alt+%d)"), mode_label(Mode(m)), m + 1);
    h.x = mx;
    h.y = y0;
    h.w = widths[size_t(m)];
    h.h = hh;
    h.row = 1;
    h.toggled = int(mode_) == m;
    h.action = [this, m] { set_mode(Mode(m)); };
    hots_.push_back(h);
    mx += h.w;
  }
  if (overflow) {
    std::vector<int> hidden;
    for (int m = 0; m < kModeCount; ++m)
      if (std::find(shown.begin(), shown.end(), m) == shown.end()) hidden.push_back(m);
    Hot h;
    h.kind = Hot::Overflow;
    h.text = shown.empty() ? std::string(mode_label(mode_)) : std::string(_("More"));
    h.chevron = true;
    h.tip = _("More modes");
    h.x = mx;
    h.y = by;
    h.w = more_w;
    h.h = bs;
    h.row = 1;
    h.action = [this, hidden] {
      for (const auto &hh2 : hots_)
        if (hh2.kind == Hot::Overflow && hh2.row == 1) mode_menu(hh2.x, hh2.y + hh2.h + S(4), hidden);
    };
    hots_.push_back(h);
  }
}

bool XanaraWindow::tool_enabled(const ToolSpec &s, std::string *why) {
  DocView *v = view();
  if (!v) return false;
  if (s.act == Act::Ocr) {
    // Whether the language data is installed does not change while Xanara runs.
    static int ocr = -1;
    static std::string reason;
    if (ocr < 0) ocr = ocr_available(reason) ? 1 : 0;
    if (!ocr) {
      if (why) *why = reason;
      return false;
    }
  }
  if (s.act == Act::ApplyRedactions && v->redactions().empty()) {
    if (why) *why = _("Mark areas with Mark Redaction first");
    return false;
  }
  return true;
}

bool XanaraWindow::tool_toggled(const ToolSpec &s) const {
  DocView *v = view();
  if (!v) return false;
  if (s.act == Act::ToggleAnnots) return !v->show_annots();
  if (s.act == Act::ShowFields) return v->highlight_fields();
  if (s.act == Act::Compare) return compare_id_ >= 0;
  if (!s.canvas) return false;
  FieldKind k = FieldKind::Text;
  Tool t = tool_for(s.act, &k);
  if (t != v->tool()) return false;
  if (t == Tool::Field) return k == v->field_kind();
  if (t == Tool::Signature) return (s.act == Act::FillSign) == (mode_ == Mode::Fill);
  return true;
}

// The Edit mode while text is edited or a picture is picked: the tools for
// it (font, size, bold, italic, color, alignment; replace, delete) take the
// place of the less urgent tools of the mode, which go to the More menu.
bool XanaraWindow::layout_edit_bar(int y0, int hh) {
  DocView *v = view();
  if (!v || mode_ != Mode::Edit) return false;
  bool text = v->editing_text() || v->tool() == Tool::NewText;
  bool image = !text && v->has_selected_image();
  if (!text && !image) return false;
  int bs = S(36), sep = S(17), by = y0 + (hh - bs) / 2;
  auto tools = tools_for(mode_);
  std::vector<const ToolSpec *> main_tools, rest;
  for (const ToolSpec *t : tools)
    (t->act == Act::EditContent || t->act == Act::AddText || t->act == Act::AddImage ? main_tools : rest).push_back(t);
  TextStyle st = v->text_style();
  int align = v->align();
  static const icon::Kind align_icons[] = {icon::AlignLeft, icon::AlignCenter, icon::AlignRight, icon::AlignJustify};
  static const char *align_tips[] = {N_("Align left (Ctrl+L)"), N_("Center (Ctrl+E)"), N_("Align right (Ctrl+R)"),
                                     N_("Justify (Ctrl+J)")};
  struct Item {
    Hot h;
    int group;
  };
  text_font(theme_.font_size - S(1));
  // Builds the bar for a level of compactness; returns its width.
  std::vector<Item> items;
  auto build = [&](int level) {
    items.clear();
    auto icon_item = [&](icon::Kind g, const std::string &tip, std::function<void()> fn, int group, bool toggled) {
      Hot h;
      h.kind = Hot::Icon;
      h.glyph = g;
      h.tip = tip;
      h.action = std::move(fn);
      h.w = h.h = bs;
      h.toggled = toggled;
      items.push_back({h, group});
    };
    auto label_item = [&](icon::Kind g, const std::string &label, const std::string &tip, std::function<void()> fn,
                          int group) {
      Hot h;
      h.kind = level >= 1 ? Hot::Icon : Hot::Label;
      h.glyph = g;
      h.text = label;
      h.tip = tip;
      h.action = std::move(fn);
      h.w = level >= 1 ? bs : S(36) + text_width(label) + S(12);
      h.h = bs;
      items.push_back({h, group});
    };
    if (level < 2)
      for (const ToolSpec *t : main_tools) {
        Act a = t->act;
        icon_item(t->glyph, _(t->tip), [this, a] { run(a); }, 0, tool_toggled(*t));
      }
    if (text) {
      Hot f;
      f.kind = Hot::Dropdown;
      f.text = v->text_family();
      f.tip = _("Font");
      f.w = level >= 4 ? S(96) : S(150);
      f.h = bs;
      f.action = [this] {
        for (const auto &x : hots_)
          if (x.kind == Hot::Dropdown && x.tip == _("Font")) font_menu(x.x, x.y + x.h + S(4));
      };
      items.push_back({f, 1});
      Hot z;
      z.kind = Hot::Dropdown;
      z.text = format("%g", std::round(st.size * 10) / 10);
      z.tip = _("Font size");
      z.w = S(66);
      z.h = bs;
      z.action = [this] {
        for (const auto &x : hots_)
          if (x.kind == Hot::Dropdown && x.tip == _("Font size")) size_menu(x.x, x.y + x.h + S(4));
      };
      items.push_back({z, 1});
      icon_item(icon::Bold, _("Bold (Ctrl+B)"), [this] {
        if (DocView *dv = view()) {
          bool on = !dv->text_style().bold;
          dv->restyle_text([on](TextStyle &s2) { s2.bold = on; });
        }
      }, 2, st.bold);
      icon_item(icon::Italic, _("Italic (Ctrl+I)"), [this] {
        if (DocView *dv = view()) {
          bool on = !dv->text_style().italic;
          dv->restyle_text([on](TextStyle &s2) { s2.italic = on; });
        }
      }, 2, st.italic);
      Hot c;
      c.kind = Hot::Swatch;
      c.tip = _("Text color");
      c.color = st.paint.rgb();
      c.w = S(50);
      c.h = bs;
      c.action = [this] {
        for (const auto &x : hots_)
          if (x.kind == Hot::Swatch) color_menu(x.x, x.y + x.h + S(4));
      };
      items.push_back({c, 2});
      if (level >= 3) {
        Hot a;
        a.kind = Hot::Dropdown;
        a.glyph = align_icons[std::clamp(align, 0, 3)];
        a.tip = _("Alignment");
        a.w = S(56);
        a.h = bs;
        a.action = [this] {
          for (const auto &x : hots_)
            if (x.kind == Hot::Dropdown && x.tip == _("Alignment")) align_menu(x.x, x.y + x.h + S(4));
        };
        items.push_back({a, 3});
      } else {
        for (int k = 0; k < 4; ++k)
          icon_item(align_icons[k], _(align_tips[k]), [this, k] {
            if (DocView *dv = view()) dv->set_align(k);
            tool_changed();
          }, 3, align == k);
      }
      label_item(icon::Check, _("Done"), _("Done: write the text into the page (Esc)"), [this] {
        if (DocView *dv = view()) {
          dv->finish_text_edit();
          if (dv->tool() == Tool::NewText) dv->set_tool(Tool::Edit);
        }
      }, 4);
    } else {
      label_item(icon::Replace, _("Replace"), _("Replace the picture with an image file"), [this] {
        if (DocView *dv = view()) dv->replace_selected_image();
      }, 1);
      label_item(icon::Trash, _("Delete"), _("Delete the picture (Delete)"), [this] {
        if (DocView *dv = view()) dv->delete_selected_image();
      }, 1);
    }
    int total = 0;
    for (size_t k = 0; k < items.size(); ++k) {
      if (k && items[k].group != items[k - 1].group) total += sep;
      total += items[k].h.w;
    }
    return total + S(8) + bs;  // the More button
  };
  int avail = w() - S(24), level = 0, total = 0;
  for (level = 0; level <= 4; ++level) {
    total = build(level);
    if (total <= avail) break;
  }
  if (level > 4) level = 4;
  if (level >= 2) rest.insert(rest.begin(), main_tools.begin(), main_tools.end());
  int x = std::max(S(12), (w() - total) / 2);
  ctx_hint_x_ = S(16);
  ctx_hint_w_ = x - S(32);
  for (size_t k = 0; k < items.size(); ++k) {
    if (k && items[k].group != items[k - 1].group) {
      seps_.push_back({x + sep / 2, by + S(8), bs - S(16)});
      x += sep;
    }
    Hot h = items[k].h;
    h.x = x;
    h.y = by;
    h.row = 2;
    hots_.push_back(h);
    x += h.w;
  }
  x += S(8);
  Hot more;
  more.kind = Hot::Icon;
  more.glyph = icon::More;
  more.tip = _("More tools");
  more.x = x;
  more.y = by;
  more.w = more.h = bs;
  more.row = 2;
  more.action = [this, rest] {
    for (const auto &hh2 : hots_)
      if (hh2.row == 2 && hh2.glyph == icon::More) overflow_menu(hh2.x, hh2.y + hh2.h + S(4), rest);
  };
  hots_.push_back(more);
  return true;
}

void XanaraWindow::layout_context(int y0, int hh) {
  if (layout_edit_bar(y0, hh)) return;
  auto tools = tools_for(mode_);
  int bs = S(36), sep = S(17), by = y0 + (hh - bs) / 2;
  text_font(theme_.font_size - S(1));
  auto label_w = [&](const ToolSpec *t) { return S(36) + text_width(_(t->label)) + S(12); };
  int avail = w() - S(24);
  int with_labels = 0;
  for (size_t k = 0; k < tools.size(); ++k) {
    if (k && tools[k]->group != tools[k - 1]->group) with_labels += sep;
    with_labels += label_w(tools[k]);
  }
  bool labels = mode_ != Mode::Annotate && with_labels <= avail - S(160);
  int n = labels ? int(tools.size()) : tools_that_fit(tools, avail, bs, sep, bs + S(8));
  int total = 0;
  for (int k = 0; k < n; ++k) {
    if (k && tools[size_t(k)]->group != tools[size_t(k - 1)]->group) total += sep;
    total += labels ? label_w(tools[size_t(k)]) : bs;
  }
  bool overflow = n < int(tools.size());
  if (overflow) total += S(8) + bs;
  int x = std::max(S(12), (w() - total) / 2);
  ctx_hint_x_ = S(16);
  ctx_hint_w_ = x - S(32);
  for (int k = 0; k < n; ++k) {
    const ToolSpec *t = tools[size_t(k)];
    if (k && t->group != tools[size_t(k - 1)]->group) {
      seps_.push_back({x + sep / 2, by + S(8), bs - S(16)});
      x += sep;
    }
    Hot h;
    h.kind = labels ? Hot::Label : Hot::Icon;
    h.glyph = t->glyph;
    if (t->act == Act::ToggleAnnots && view() && !view()->show_annots()) h.glyph = icon::EyeOff;
    h.text = _(t->label);
    std::string why;
    h.enabled = tool_enabled(*t, &why);
    h.tip = h.enabled ? std::string(_(t->tip)) : format("%s — %s", _(t->label), why.c_str());
    h.toggled = tool_toggled(*t);
    h.x = x;
    h.y = by;
    h.w = labels ? label_w(t) : bs;
    h.h = bs;
    h.row = 2;
    Act a = t->act;
    h.action = [this, a] { run(a); };
    hots_.push_back(h);
    x += h.w;
  }
  if (overflow) {
    std::vector<const ToolSpec *> rest(tools.begin() + n, tools.end());
    x += S(8);
    Hot h;
    h.kind = Hot::Icon;
    h.glyph = icon::More;
    h.tip = _("More tools");
    h.x = x;
    h.y = by;
    h.w = bs;
    h.h = bs;
    h.row = 2;
    h.action = [this, rest] {
      for (const auto &hh2 : hots_)
        if (hh2.row == 2 && hh2.glyph == icon::More) overflow_menu(hh2.x, hh2.y + hh2.h + S(4), rest);
    };
    hots_.push_back(h);
  }
}

int XanaraWindow::hot_at(int mx, int my) const {
  for (int k = int(hots_.size()) - 1; k >= 0; --k) {
    const Hot &b = hots_[size_t(k)];
    if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) return k;
  }
  return -1;
}

void XanaraWindow::update_chrome() {
  DocView *v = view();
  DocTab *t = tab();
  if (v && Fl::focus() != page_input_) page_input_->value(std::to_string(v->current_page() + 1).c_str());
  std::string label_text = "Xanara";
  if (t) {
    std::string title = display_name(t->target);
    if (t->dirty()) title = "• " + title;
    label_text = title + " — Xanara";
  }
  static std::string stored;
  if (stored != label_text) {
    stored = label_text;
    label(stored.c_str());
  }
  build_chrome();
  redraw();
}

// ------------------------------------------------------------------ menus

namespace {

struct Menu {
  std::vector<Fl_Menu_Item> items;
  std::deque<std::string> keep;  // the labels; a deque keeps them in place
  const Theme &t;
  explicit Menu(const Theme &th) : t(th) {}
  void end_sub() {
    Fl_Menu_Item it;
    memset(&it, 0, sizeof it);
    items.push_back(it);
  }
  void add(const std::string &text, long id, int flags = 0) {
    keep.push_back(text);
    Fl_Menu_Item it;
    memset(&it, 0, sizeof it);
    it.text = keep.back().c_str();
    it.user_data_ = reinterpret_cast<void *>(id);
    it.flags = flags;
    it.labelsize(t.font_size);
    it.labelcolor((flags & FL_MENU_INACTIVE) ? t.text_dim.fl() : t.text.fl());
    items.push_back(it);
  }
  long popup(int x, int y) {
    Fl_Menu_Item end;
    memset(&end, 0, sizeof end);
    items.push_back(end);
    const Fl_Menu_Item *m = items.data()->popup(x, y);
    return m ? reinterpret_cast<long>(m->user_data()) : -1;
  }
};

}  // namespace

void XanaraWindow::zoom_menu(int x, int y) {
  DocView *v = view();
  if (!v) return;
  struct P {
    const char *label;
    double z;
  } presets[] = {{_("Fit Width"), -1}, {_("Fit Page"), -2}, {"50%", 0.5},   {"75%", 0.75},
                 {"100%", 1.0},       {"125%", 1.25},     {"150%", 1.5},   {"200%", 2.0},
                 {"300%", 3.0},       {"400%", 4.0},      {"800%", 8.0}};
  Menu m(theme_);
  for (int k = 0; k < 11; ++k) {
    bool cur = (presets[k].z == -1 && v->fit() == Fit::Width) || (presets[k].z == -2 && v->fit() == Fit::Page) ||
               (v->fit() == Fit::Free && std::fabs(presets[k].z - v->zoom()) < 0.005);
    m.add(presets[k].label, k + 1, FL_MENU_RADIO | (cur ? FL_MENU_VALUE : 0) | (k == 1 ? FL_MENU_DIVIDER : 0));
  }
  m.add(_("Single Page"), 100, FL_MENU_RADIO | (v->layout_mode() == Layout::Single ? FL_MENU_VALUE : 0));
  m.add(_("Continuous"), 101, FL_MENU_RADIO | (v->layout_mode() == Layout::Continuous ? FL_MENU_VALUE : 0));
  m.add(_("Two Pages"), 102, FL_MENU_RADIO | (v->layout_mode() == Layout::Book ? FL_MENU_VALUE : 0));
  long id = m.popup(x, y);
  if (id >= 1 && id <= 11) {
    double z = presets[id - 1].z;
    if (z == -1) v->set_fit(Fit::Width);
    else if (z == -2) v->set_fit(Fit::Page);
    else v->set_zoom(z);
  } else if (id >= 100) {
    v->set_layout(id == 100 ? Layout::Single : id == 101 ? Layout::Continuous : Layout::Book);
    settings_changed();
  }
}

void XanaraWindow::pointer_menu(int x, int y) {
  DocView *v = view();
  if (!v) return;
  Menu m(theme_);
  m.add(_("Select Text and Annotations (V)"), 1, FL_MENU_RADIO | (v->tool() != Tool::Hand ? FL_MENU_VALUE : 0));
  m.add(_("Hand: Drag the Pages"), 2, FL_MENU_RADIO | (v->tool() == Tool::Hand ? FL_MENU_VALUE : 0));
  long id = m.popup(x, y);
  if (id == 1) v->set_tool(Tool::Select);
  else if (id == 2) v->set_tool(Tool::Hand);
}

void XanaraWindow::export_menu(int x, int y) {
  if (!view()) return;
  Menu m(theme_);
  auto add_act = [&](Act a, const char *label) {
    const ToolSpec *s = tool_spec(a);
    m.add(label, long(a) + 1, tool_enabled(*s) ? 0 : FL_MENU_INACTIVE);
  };
  m.add(_("Save a Copy As…"), 1000);
  add_act(Act::Print, _("Print…"));
  m.items.back().flags |= FL_MENU_DIVIDER;
  add_act(Act::ToPng, _("Export Pages as PNG…"));
  add_act(Act::ToJpeg, _("Export Pages as JPEG…"));
  add_act(Act::ToText, _("Export as Text…"));
  add_act(Act::ToHtml, _("Export as HTML…"));
  add_act(Act::ToMarkdown, _("Export as Markdown…"));
  m.items.back().flags |= FL_MENU_DIVIDER;
  add_act(Act::Compress, _("Save a Smaller Copy…"));
  add_act(Act::Encrypt, _("Protect with a Password…"));
  m.add(_("Open the Folder"), 1001);
  long id = m.popup(std::max(0, x), y);
  if (id == 1000) save_as();
  else if (id == 1001) {
    if (DocTab *t = tab()) {
      std::string dir = t->target.substr(0, t->target.rfind('/'));
      if (!open_uri(file_uri(dir))) toast(_("No file manager is available"));
    }
  } else if (id > 0) {
    run(Act(id - 1));
  }
}

void XanaraWindow::overflow_menu(int x, int y, const std::vector<const ToolSpec *> &tools) {
  Menu m(theme_);
  for (size_t k = 0; k < tools.size(); ++k) {
    std::string why;
    bool en = tool_enabled(*tools[k], &why);
    int flags = (en ? 0 : FL_MENU_INACTIVE) | (tool_toggled(*tools[k]) ? FL_MENU_TOGGLE | FL_MENU_VALUE : 0);
    if (k + 1 < tools.size() && tools[k + 1]->group != tools[k]->group) flags |= FL_MENU_DIVIDER;
    m.add(en ? std::string(_(tools[k]->label)) : format("%s  (%s)", _(tools[k]->label), why.c_str()), long(k) + 1,
          flags);
  }
  long id = m.popup(std::max(0, std::min(x, w() - S(260))), y);
  if (id > 0) run(tools[size_t(id - 1)]->act);
}

void XanaraWindow::font_menu(int x, int y) {
  DocView *v = view();
  if (!v) return;
  TextStyle st = v->text_style();
  std::string current = v->text_family();
  std::vector<std::string> families = SystemFont::families();
  auto has = [&](const std::string &f) { return std::find(families.begin(), families.end(), f) != families.end(); };
  Menu m(theme_);
  std::vector<std::string> ids;
  auto add = [&](const std::string &family, const std::string &label, int flags) {
    ids.push_back(family);
    m.add(label, long(ids.size()), flags | FL_MENU_RADIO | (family == current || (family.empty() && st.family.empty() && st.font >= 0) ? FL_MENU_VALUE : 0));
  };
  if (st.font >= 0 && st.family.empty()) add("", format(_("%s (the document's font)"), current.c_str()), FL_MENU_DIVIDER);
  static const char *common[] = {"Liberation Sans", "Liberation Serif", "Liberation Mono", "DejaVu Sans", "DejaVu Serif",
                                 "DejaVu Sans Mono", "Noto Sans", "Noto Serif", "Noto Sans Mono", "Carlito", "Caladea",
                                 "Cantarell", "Ubuntu", "Open Sans", "Roboto", "Source Sans 3", "Inter"};
  int shown = 0;
  for (const char *c : common)
    if (has(c)) {
      add(c, c, 0);
      ++shown;
    }
  if (shown) m.items.back().flags |= FL_MENU_DIVIDER;
  m.add(_("All Fonts"), 0, FL_SUBMENU);
  for (const auto &f : families) {
    std::string label;
    for (char ch : f) {
      if (ch == '/' || ch == '\\' || ch == '&' || ch == '_') label += '\\';
      label += ch;
    }
    add(f, label, 0);
  }
  m.end_sub();
  long id = m.popup(std::max(0, x), y);
  if (id <= 0 || id > long(ids.size())) return;
  std::string family = ids[size_t(id - 1)];
  if (family.empty()) return;  // the document's font: as it is
  v->restyle_text([family](TextStyle &s2) { s2.family = family; });
  tool_changed();
}

void XanaraWindow::size_menu(int x, int y) {
  DocView *v = view();
  if (!v) return;
  double cur = v->text_style().size;
  static const double sizes[] = {6, 7, 8, 9, 10, 10.5, 11, 12, 13, 14, 16, 18, 20, 24, 28, 32, 36, 48, 60, 72};
  Menu m(theme_);
  int n = int(sizeof sizes / sizeof sizes[0]);
  for (int k = 0; k < n; ++k)
    m.add(format("%g", sizes[k]), k + 1, FL_MENU_RADIO | (std::fabs(sizes[k] - cur) < 0.05 ? FL_MENU_VALUE : 0) |
                                             (k == n - 1 ? FL_MENU_DIVIDER : 0));
  m.add(_("Other Size…"), 100);
  long id = m.popup(std::max(0, x), y);
  double size = 0;
  if (id >= 1 && id <= n) {
    size = sizes[id - 1];
  } else if (id == 100) {
    std::string t = format("%g", std::round(cur * 10) / 10);
    if (!ask_text(_("Font Size (points)"), t, false)) return;
    size = atof(t.c_str());
  }
  if (size >= 1 && size <= 999) v->set_font_size(size);
  tool_changed();
}

void XanaraWindow::color_menu(int x, int y) {
  DocView *v = view();
  if (!v) return;
  Rgb cur = v->text_style().paint.rgb();
  struct C {
    const char *name;
    Rgb c;
  };
  std::vector<C> colors = {{N_("Black"), {0, 0, 0}}, {N_("Dark Gray"), {0.3, 0.3, 0.3}}, {N_("Gray"), {0.55, 0.55, 0.55}},
                           {N_("White"), {1, 1, 1}}};
  for (int k = 0; k < kSwatchCount; ++k)
    if (std::string(kSwatches[k].name) != "Black") colors.push_back({kSwatches[k].name, kSwatches[k].color});
  Menu m(theme_);
  auto near = [](Rgb a, Rgb b) { return std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b) < 0.03; };
  for (size_t k = 0; k < colors.size(); ++k)
    m.add(_(colors[k].name), long(k) + 1, FL_MENU_RADIO | (near(colors[k].c, cur) ? FL_MENU_VALUE : 0) |
                                              (k + 1 == colors.size() ? FL_MENU_DIVIDER : 0));
  m.add(_("Other Color…"), 100);
  long id = m.popup(std::max(0, x), y);
  Rgb c;
  if (id >= 1 && id <= long(colors.size())) {
    c = colors[size_t(id - 1)].c;
  } else if (id == 100) {
    std::string t = format("#%02X%02X%02X", int(std::lround(cur.r * 255)), int(std::lround(cur.g * 255)),
                           int(std::lround(cur.b * 255)));
    if (!ask_text(_("Text Color (#RRGGBB)"), t, false)) return;
    t = trim(t);
    if (!t.empty() && t[0] == '#') t = t.substr(1);
    if (t.size() != 6 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
      toast(_("Write the color as #RRGGBB, for example #1A73E8"));
      return;
    }
    unsigned long hex = strtoul(t.c_str(), nullptr, 16);
    c = Rgb{((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0, (hex & 255) / 255.0};
  } else {
    return;
  }
  v->set_ink(c);
  tool_changed();
}

void XanaraWindow::align_menu(int x, int y) {
  DocView *v = view();
  if (!v) return;
  Menu m(theme_);
  const char *labels[] = {_("Align Left"), _("Center"), _("Align Right"), _("Justify")};
  for (int k = 0; k < 4; ++k) m.add(labels[k], k + 1, FL_MENU_RADIO | (v->align() == k ? FL_MENU_VALUE : 0));
  long id = m.popup(std::max(0, x), y);
  if (id >= 1 && id <= 4) v->set_align(int(id - 1));
  tool_changed();
}

void XanaraWindow::mode_menu(int x, int y, const std::vector<int> &modes) {
  Menu m(theme_);
  for (int k = 0; k < kModeCount; ++k)
    m.add(mode_label(Mode(k)), k + 1, FL_MENU_RADIO | (int(mode_) == k ? FL_MENU_VALUE : 0));
  (void)modes;
  long id = m.popup(x, y);
  if (id > 0) set_mode(Mode(id - 1));
}

// ---------------------------------------------------------------- drawing

void XanaraWindow::draw_hot(Painter &p, const Hot &b, int k) {
  const Theme &t = theme_;
  bool hov = k == hover_hot_ && b.enabled, down = k == press_hot_;
  Color fg = !b.enabled ? t.text_dim.alpha(0.42) : b.toggled ? t.accent_text : t.text;
  switch (b.kind) {
    case Hot::TabHome:
    case Hot::Tab: {
      bool active = b.toggled;
      bool dragging = b.kind == Hot::Tab && drag_tab_ == b.tab && drag_tab_to_ != drag_tab_;
      double r = S(9);
      if (active || hov || dragging) {
        Color bg = active ? t.surface : t.hover;
        cairo_t *cr = p.cr();
        p.set(bg);
        double x0 = b.x, y0 = b.y, x1 = b.x + b.w, y1 = b.y + b.h;
        cairo_new_sub_path(cr);
        cairo_move_to(cr, x0, y1);
        cairo_arc(cr, x0 + r, y0 + r, r, M_PI, 3 * M_PI / 2);
        cairo_arc(cr, x1 - r, y0 + r, r, -M_PI / 2, 0);
        cairo_line_to(cr, x1, y1);
        cairo_close_path(cr);
        cairo_fill(cr);
      } else if (b.kind == Hot::Tab) {
        // A thin divider between inactive tabs.
        p.line(b.x + b.w + 0.5, b.y + S(9), b.x + b.w + 0.5, b.y + b.h - S(9), t.border);
      }
      if (b.kind == Hot::TabHome) {
        double ix = b.w < S(60) ? b.x + (b.w - S(20)) / 2.0 : b.x + S(14);
        icon::draw(p, icon::Home, ix, b.y + (b.h - S(20)) / 2.0, S(20), active ? t.text : t.text_dim);
      } else {
        const DocTab &dt = *tabs_[size_t(b.tab)];
        if (dt.dirty()) p.circle(b.x + S(14), b.y + b.h / 2.0, S(3.5), t.accent);
      }
      break;
    }
    case Hot::TabClose: {
      bool show = b.tab == tablist_.active() || (hover_hot_ >= 0 && hots_[size_t(hover_hot_)].tab == b.tab &&
                                                 (hots_[size_t(hover_hot_)].kind == Hot::Tab ||
                                                  hots_[size_t(hover_hot_)].kind == Hot::TabClose));
      if (!show) break;
      if (hov) p.fill_rounded(b.x, b.y, b.w, b.h, S(5), t.hover.mix(t.text, 0.08));
      icon::draw(p, icon::Close, b.x + S(3), b.y + S(3), b.w - S(6), t.text_dim);
      break;
    }
    case Hot::ModeTab:
      if (hov && !b.toggled) p.fill_rounded(b.x + S(4), b.y + S(8), b.w - S(8), b.h - S(16), S(8), t.hover);
      if (b.toggled) {
        text_font(mode_font(t), true);
        int tw = text_width(b.text);
        p.fill_rounded(b.x + (b.w - tw) / 2.0, b.y + b.h - S(7), tw, S(3), S(1.5), t.accent);
      }
      break;
    case Hot::Zoom:
    case Hot::Overflow:
      if (hov || down) p.fill_rounded(b.x, b.y, b.w, b.h, S(8), t.hover);
      icon::draw(p, icon::ChevronDown, b.x + b.w - S(20), b.y + (b.h - S(14)) / 2.0, S(14), t.text_dim);
      break;
    case Hot::Pointer:
      if (hov || down) p.fill_rounded(b.x, b.y, b.w, b.h, S(8), t.hover);
      icon::draw(p, b.glyph, b.x + S(6), b.y + (b.h - S(20)) / 2.0, S(20), t.text);
      icon::draw(p, icon::ChevronDown, b.x + b.w - S(18), b.y + (b.h - S(14)) / 2.0, S(14), t.text_dim);
      break;
    case Hot::Dropdown:
      p.fill_rounded(b.x + S(2), b.y + S(3), b.w - S(4), b.h - S(6), S(7), hov || down ? t.hover : t.background);
      p.stroke_rounded(b.x + S(2) + 0.5, b.y + S(3) + 0.5, b.w - S(4) - 1, b.h - S(6) - 1, S(7), t.border);
      if (b.text.empty()) icon::draw(p, b.glyph, b.x + S(9), b.y + (b.h - S(20)) / 2.0, S(20), fg);
      icon::draw(p, icon::ChevronDown, b.x + b.w - S(20), b.y + (b.h - S(14)) / 2.0, S(14), t.text_dim);
      break;
    case Hot::Swatch: {
      if (hov || down) p.fill_rounded(b.x + S(1), b.y + S(1), b.w - S(2), b.h - S(2), S(8), t.hover);
      double r = S(9), cx = b.x + S(16), cy = b.y + b.h / 2.0;
      p.circle(cx, cy, r + S(1.5), t.border);
      p.circle(cx, cy, r, Color{b.color.r, b.color.g, b.color.b, 1});
      icon::draw(p, icon::ChevronDown, b.x + b.w - S(19), b.y + (b.h - S(14)) / 2.0, S(14), t.text_dim);
      break;
    }
    case Hot::Icon:
    case Hot::Label: {
      if (b.toggled) p.fill_rounded(b.x + S(1), b.y + S(1), b.w - S(2), b.h - S(2), S(8), t.accent.alpha(0.16));
      else if (hov || down) p.fill_rounded(b.x + S(1), b.y + S(1), b.w - S(2), b.h - S(2), S(8), t.hover);
      double is = b.row == 2 ? S(22) : S(20);
      double ix = b.kind == Hot::Label ? b.x + S(9) : b.x + (b.w - is) / 2.0;
      icon::draw(p, b.glyph, ix, b.y + (b.h - is) / 2.0, is, fg);
      break;
    }
  }
}

void XanaraWindow::draw_chrome(Painter &p) {
  const Theme &t = theme_;
  if (tabs_h_) {
    p.fill_rect(0, 0, w(), tabs_h_, strip_color(t));
    p.line(0, tabs_h_ - 0.5, w(), tabs_h_ - 0.5, t.border.alpha(0.6));
  }
  if (bar_h_) {
    p.fill_rect(0, tabs_h_, w(), bar_h_ + ctx_h_, t.surface);
    p.line(0, tabs_h_ + bar_h_ - 0.5, w(), tabs_h_ + bar_h_ - 0.5, t.border.alpha(0.55));
    p.line(0, tabs_h_ + bar_h_ + ctx_h_ - 0.5, w(), tabs_h_ + bar_h_ + ctx_h_ - 0.5, t.border);
  }
  for (const auto &s : seps_) p.line(s[0] + 0.5, s[1], s[0] + 0.5, s[1] + s[2], t.border);
  for (int k = 0; k < int(hots_.size()); ++k) draw_hot(p, hots_[size_t(k)], k);
  // Where a dragged tab will land.
  if (drag_tab_ >= 0 && drag_tab_to_ >= 0 && drag_tab_to_ != drag_tab_) {
    int x = drag_tab_to_ > drag_tab_ ? tab_x(drag_tab_to_) + tab_w_ : tab_x(drag_tab_to_);
    p.fill_rounded(x - S(1.5), S(8), S(3), tabs_h_ - S(14), S(1.5), t.accent);
  }
}

void XanaraWindow::draw_chrome_text() {
  const Theme &t = theme_;
  DocView *v = view();
  for (int k = 0; k < int(hots_.size()); ++k) {
    const Hot &b = hots_[size_t(k)];
    Color fg = !b.enabled ? t.text_dim.alpha(0.5) : b.toggled ? t.accent_text : t.text;
    switch (b.kind) {
      case Hot::TabHome:
        if (b.w >= S(60)) {
          text_font(t.font_size, b.toggled);
          draw_text(b.text, b.x + S(42), b.y, b.w - S(48), b.h, b.toggled ? t.text : t.text_dim);
        }
        break;
      case Hot::Tab: {
        text_font(t.font_size, b.toggled);
        int left = b.x + S(24);
        draw_text(b.text, left, b.y, b.w - S(24) - S(34), b.h, b.toggled ? t.text : t.text_dim, 0);
        break;
      }
      case Hot::ModeTab:
        text_font(mode_font(t), b.toggled);
        draw_text(b.text, b.x, b.y, b.w, b.h - S(2), b.toggled ? t.accent_text : t.text, 1);
        break;
      case Hot::Zoom:
        if (v) {
          text_font(t.font_size);
          draw_text(zoom_text(v->zoom()), b.x + S(6), b.y, b.w - S(26), b.h, t.text, 1);
        }
        break;
      case Hot::Overflow:
        text_font(t.font_size, true);
        draw_text(b.text, b.x + S(8), b.y, b.w - S(28), b.h, t.text, 0);
        break;
      case Hot::Label:
        text_font(t.font_size - S(1));
        draw_text(b.text, b.x + S(36), b.y, b.w - S(40), b.h, fg);
        break;
      case Hot::Dropdown:
        if (!b.text.empty()) {
          text_font(t.font_size - S(1));
          draw_text(b.text, b.x + S(11), b.y, b.w - S(34), b.h, t.text, 0);
        }
        break;
      default: break;
    }
  }
  // The armed tool's hint left of the context tools.
  if (v && ctx_h_ && ctx_hint_w_ > S(120)) {
    std::string hint = tool_hint(v->tool());
    if (v->editing_text()) hint = _("Type to change the text; Esc or a click outside ends");
    else if (v->has_selected_image()) hint = _("Drag to move, corners to resize");
    if (!v->redactions().empty() && mode_ == Mode::Protect)
      hint = format(_("%zu area(s) marked for redaction"), v->redactions().size());
    if (!hint.empty()) {
      text_font(t.font_size - S(2));
      draw_text(hint, ctx_hint_x_, tabs_h_ + bar_h_, ctx_hint_w_, ctx_h_, t.text_dim);
    }
  }
}

void XanaraWindow::draw() {
  DocView *v = view();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    draw_chrome(p);
  }
  draw_chrome_text();
  if (v) {
    draw_child(*v);
    if (compare_id_ >= 0) {
      int other = tablist_.index_of_id(compare_id_);
      if (other >= 0) {
        DocView *ov = tabs_[size_t(other)]->view;
        draw_child(*ov);
        Painter p(w(), h());
        p.fill_rect(ov->x() - 1, ov->y(), 1, ov->h(), theme_.accent);
      }
    }
    if (sidebar_->visible()) draw_child(*sidebar_);
    if (props_->visible()) draw_child(*props_);
  } else {
    draw_child(*home_);
  }
  if (page_input_->visible()) draw_child(*page_input_);
  if (find_input_->visible()) draw_child(*find_input_);
  if (field_input_->visible()) draw_child(*field_input_);
  if (find_input_->visible() && find_input_->size() == 0) {
    text_font(theme_.font_size - S(1));
    draw_text(_("Search the document"), find_input_->x() + S(4), find_input_->y(), find_input_->w(),
              find_input_->h(), theme_.text_dim);
  }
  // Tooltip.
  if (!tip_.empty()) {
    text_font(theme_.font_size - S(1));
    int tw = std::min(text_width(tip_) + S(20), w() - S(8)), th = S(28);
    int tx = std::clamp(tip_x_ - tw / 2, S(4), w() - tw - S(4)), ty = tip_y_;
    {
      Painter p(w(), h());
      p.shadow(tx, ty, tw, th, S(6), theme_.shadow, S(5));
      p.fill_rounded(tx, ty, tw, th, S(6), theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.1) : theme_.text.mix(theme_.background, 0.08));
    }
    draw_text(tip_, tx + S(10), ty, tw - S(20), th, theme_.dark ? theme_.text : theme_.background);
  }
  if (!toast_.empty()) {
    text_font(theme_.font_size);
    int tw = std::min(text_width(toast_) + S(40), w() - S(40)), th = S(40);
    int cx = v ? v->x() + v->w() / 2 : w() / 2;
    int tx = std::clamp(cx - tw / 2, S(8), w() - tw - S(8)), ty = h() - th - S(28);
    Color bg = theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.12) : theme_.text.mix(theme_.background, 0.08);
    Color fg = theme_.dark ? theme_.text : theme_.background;
    {
      Painter p(w(), h());
      p.shadow(tx, ty, tw, th, th / 2.0, theme_.shadow, S(8));
      p.fill_rounded(tx, ty, tw, th, th / 2.0, bg);
    }
    draw_text(toast_, tx + S(20), ty, tw - S(40), th, fg, 1);
  }
}

}  // namespace xanara
