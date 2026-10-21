// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The properties panel: colour, opacity, line width, font size and
// alignment of the armed tool or the selected annotation, and the
// document's information when nothing is selected.
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

const double kWidths[] = {1, 2, 3, 5};
const double kSizes[] = {8, 10, 12, 14, 18, 24};
const char *kStamps[] = {N_("APPROVED"), N_("DRAFT"), N_("CONFIDENTIAL"), N_("REJECTED"), N_("FINAL")};
enum ActionId { EditNote = 0, Delete = 1, Stamp0 = 2, Bookmark = 20 };

bool same(Rgb a, Rgb b) { return std::fabs(a.r - b.r) < 0.02 && std::fabs(a.g - b.g) < 0.02 && std::fabs(a.b - b.b) < 0.02; }

std::string annot_title(PopplerAnnotType t) {
  switch (t) {
    case POPPLER_ANNOT_TEXT: return _("Note");
    case POPPLER_ANNOT_FREE_TEXT: return _("Text Box");
    case POPPLER_ANNOT_HIGHLIGHT: return _("Highlight");
    case POPPLER_ANNOT_UNDERLINE: return _("Underline");
    case POPPLER_ANNOT_STRIKE_OUT: return _("Strike-out");
    case POPPLER_ANNOT_SQUIGGLY: return _("Squiggly");
    case POPPLER_ANNOT_INK: return _("Drawing");
    case POPPLER_ANNOT_SQUARE: return _("Rectangle");
    case POPPLER_ANNOT_CIRCLE: return _("Ellipse");
    case POPPLER_ANNOT_LINE: return _("Line");
    case POPPLER_ANNOT_STAMP: return _("Stamp");
    default: return _("Annotation");
  }
}

// Sections and their label rows, rebuilt with the items.
struct Label {
  int y;
  std::string text;
  bool heading;
  std::string value;
};
std::vector<Label> g_labels;

}  // namespace

PropsPanel::PropsPanel(XanaraWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

void PropsPanel::build() {
  items_.clear();
  g_labels.clear();
  DocView *v = app_->view();
  Document *d = app_->document();
  if (!v || !d) return;
  int px = x() + S(18), pw = w() - S(36);
  int yy = y() + S(60) - int(scroll_);
  auto heading = [&](const std::string &t) {
    g_labels.push_back({yy, t, true, ""});
    yy += S(28);
  };
  auto row = [&](const std::string &label, const std::string &value) {
    g_labels.push_back({yy, label, false, value});
    yy += S(40);
  };
  const Annotation *a = v->tool() == Tool::Select ? v->selected_annot() : nullptr;
  Tool t = v->tool();
  bool color = a ? a->has_color : tool_has_color(t);
  if (color) {
    heading(_("Color"));
    int sz = S(26), gap = std::max(S(4), (pw - 8 * sz) / 7);
    for (int k = 0; k < kSwatchCount; ++k) items_.push_back({Item::Swatch, px + k * (sz + gap), yy, sz, sz, k});
    yy += sz + S(18);
    heading(_("Opacity"));
    items_.push_back({Item::Opacity, px, yy, pw - S(52), S(22), 0});
    yy += S(40);
  }
  if (!a && tool_has_width(t)) {
    heading(_("Line Width"));
    int bw = (pw - 3 * S(6)) / 4;
    for (int k = 0; k < 4; ++k) items_.push_back({Item::Width, px + k * (bw + S(6)), yy, bw, S(32), k});
    yy += S(48);
  }
  if (!a && tool_has_font(t)) {
    heading(_("Font Size"));
    int bw = (pw - 5 * S(5)) / 6;
    for (int k = 0; k < 6; ++k) items_.push_back({Item::FontSize, px + k * (bw + S(5)), yy, bw, S(32), k});
    yy += S(48);
    heading(_("Alignment"));
    int aw = (pw - 2 * S(6)) / 3;
    for (int k = 0; k < 3; ++k) items_.push_back({Item::Align, px + k * (aw + S(6)), yy, aw, S(32), k});
    yy += S(48);
  }
  if (!a && t == Tool::Stamp) {
    heading(_("Stamp"));
    for (int k = 0; k < 5; ++k) {
      items_.push_back({Item::Action, px, yy, pw, S(34), Stamp0 + k});
      yy += S(38);
    }
    yy += S(10);
  }
  if (a) {
    heading(_("Details"));
    row(_("Page"), std::to_string(v->selected_annot_page() + 1));
    if (!a->author.empty()) row(_("Author"), a->author);
    if (!a->contents.empty()) row(_("Text"), a->contents);
    if (a->type == POPPLER_ANNOT_TEXT || a->type == POPPLER_ANNOT_FREE_TEXT) {
      items_.push_back({Item::Action, px, yy, pw, S(34), EditNote});
      yy += S(40);
    }
    items_.push_back({Item::Action, px, yy, pw, S(34), Delete});
    yy += S(40);
  }
  if (!a && t == Tool::Select) {
    heading(_("Document"));
    DocTab *tab = app_->tab();
    row(_("File"), display_name(tab->target));
    for (const auto &kv : d->info()) row(_(kv.first.c_str()), kv.second);
    row(_("Pages"), std::to_string(d->count()));
    int cur = v->current_page();
    row(_("Page size"), format("%.0f × %.0f mm", d->width(cur) / 72 * 25.4, d->height(cur) / 72 * 25.4));
    struct stat st;
    if (stat(tab->target.c_str(), &st) == 0)
      row(_("File size"), st.st_size > (1 << 20) ? format("%.1f MB", st.st_size / 1048576.0)
                                                 : format("%.0f KB", st.st_size / 1024.0));
    items_.push_back({Item::Action, px, yy, pw, S(34), Bookmark});
    yy += S(40);
  }
}

void PropsPanel::draw() {
  const Theme &t = app_->theme();
  build();
  DocView *v = app_->view();
  const Annotation *a = v && v->tool() == Tool::Select ? v->selected_annot() : nullptr;
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), t.surface);
    p.line(x() + 0.5, y(), x() + 0.5, y() + h(), t.border);
    p.line(x(), y() + S(48) - 0.5, x() + w(), y() + S(48) - 0.5, t.border.alpha(0.6));
    Rgb ink = v ? v->ink() : Rgb{};
    double op = a ? a->opacity : v ? v->opacity() : 1;
    for (int k = 0; k < int(items_.size()); ++k) {
      const Item &it = items_[size_t(k)];
      bool hov = k == hover_;
      switch (it.kind) {
        case Item::Swatch: {
          const Rgb &c = kSwatches[it.index].color;
          double cx = it.x + it.w / 2.0, cy = it.y + it.h / 2.0;
          if (same(ink, c)) p.circle(cx, cy, it.w / 2.0 + S(2), t.accent);
          p.circle(cx, cy, it.w / 2.0 - (same(ink, c) ? S(1) : 0), t.surface);
          p.circle(cx, cy, it.w / 2.0 - S(3) + (hov ? S(1) : 0), Color{c.r, c.g, c.b, 1});
          break;
        }
        case Item::Opacity: {
          double cy = it.y + it.h / 2.0;
          p.fill_rounded(it.x, cy - S(2), it.w, S(4), S(2), t.border);
          p.fill_rounded(it.x, cy - S(2), it.w * op, S(4), S(2), t.accent);
          p.circle(it.x + it.w * op, cy, S(8), t.surface);
          p.circle(it.x + it.w * op, cy, S(6.5), t.accent);
          break;
        }
        case Item::Width:
        case Item::FontSize:
        case Item::Align:
        case Item::Action: {
          bool on = (it.kind == Item::Width && v && std::fabs(v->stroke() - kWidths[it.index]) < 0.01) ||
                    (it.kind == Item::FontSize && v && std::fabs(v->font_size() - kSizes[it.index]) < 0.01) ||
                    (it.kind == Item::Align && v && v->align() == it.index) ||
                    (it.kind == Item::Action && it.index >= Stamp0 && it.index < Stamp0 + 5 && v &&
                     v->stamp_text() == kStamps[it.index - Stamp0]);
          Color bg = on ? t.accent.alpha(0.16) : hov ? t.hover : t.background;
          p.fill_rounded(it.x, it.y, it.w, it.h, S(8), bg);
          p.stroke_rounded(it.x + 0.5, it.y + 0.5, it.w - 1, it.h - 1, S(8), on ? t.accent : t.border);
          if (it.kind == Item::Width)
            p.line(it.x + S(10), it.y + it.h / 2.0, it.x + it.w - S(10), it.y + it.h / 2.0, Color{ink.r, ink.g, ink.b, 1},
                   Sd(kWidths[it.index]));
          if (it.kind == Item::Align) {
            for (int l = 0; l < 3; ++l) {
              double lw = l == 1 ? S(12) : S(18);
              double lx = it.index == 0 ? it.x + it.w / 2.0 - S(9) : it.index == 1 ? it.x + (it.w - lw) / 2.0
                                                                                     : it.x + it.w / 2.0 + S(9) - lw;
              p.line(lx, it.y + S(10) + l * S(6), lx + lw, it.y + S(10) + l * S(6), t.text, Sd(1.6));
            }
          }
          break;
        }
      }
    }
    cairo_restore(cr);
  }
  fl_push_clip(x(), y(), w(), h());
  text_font(t.font_size + S(2), true);
  std::string title = !v ? std::string(_("Properties"))
                      : a ? annot_title(a->type)
                      : v->tool() == Tool::Select ? std::string(_("Properties"))
                                                  : std::string(tool_name(v->tool()));
  draw_text(title, x() + S(18), y(), w() - S(36), S(48), t.text);
  for (const auto &l : g_labels) {
    if (l.heading) {
      text_font(t.font_size - S(1), true);
      draw_text(l.text, x() + S(18), l.y, w() - S(36), S(22), t.text_dim);
    } else {
      text_font(t.font_size - S(2));
      draw_text(l.text, x() + S(18), l.y - S(2), w() - S(36), S(18), t.text_dim);
      text_font(t.font_size - S(1));
      draw_text(l.value, x() + S(18), l.y + S(16), w() - S(36), S(20), t.text);
    }
  }
  double op = a ? a->opacity : v ? v->opacity() : 1;
  for (const auto &it : items_) {
    text_font(t.font_size - S(1));
    switch (it.kind) {
      case Item::Opacity:
        draw_text(format("%d%%", int(std::lround(op * 100))), it.x + it.w + S(8), it.y, S(44), it.h, t.text, 2);
        break;
      case Item::FontSize: draw_text(format("%g", kSizes[it.index]), it.x, it.y, it.w, it.h, t.text, 1); break;
      case Item::Action: {
        std::string s = it.index == EditNote ? _("Edit Text…")
                        : it.index == Delete ? _("Delete Annotation")
                        : it.index == Bookmark ? _("Bookmark This Page")
                                               : _(kStamps[it.index - Stamp0]);
        draw_text(s, it.x, it.y, it.w, it.h, it.index == Delete ? Color::hex(0xE03131) : t.text, 1);
        break;
      }
      default: break;
    }
  }
  if (!v) {
    text_font(t.font_size - S(1));
    draw_text(_("Open a document"), x() + S(18), y() + S(60), w() - S(36), S(24), t.text_dim);
  }
  fl_pop_clip();
}

int PropsPanel::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  DocView *v = app_->view();
  auto at = [&]() {
    for (int k = 0; k < int(items_.size()); ++k) {
      const Item &it = items_[size_t(k)];
      int pad = it.kind == Item::Opacity ? S(10) : 0;
      if (mx >= it.x - pad && mx < it.x + it.w + pad && my >= it.y - pad && my < it.y + it.h + pad) return k;
    }
    return -1;
  };
  auto set_opacity = [&](const Item &it, bool commit) {
    double o = std::clamp(double(mx - it.x) / std::max(1, it.w), 0.05, 1.0);
    o = std::round(o * 20) / 20;
    if (!v) return;
    if (v->tool() == Tool::Select && v->has_selected_annot()) {
      if (commit) v->set_selected_opacity(o);
    } else {
      v->set_opacity(o);
    }
    redraw();
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = at();
      if (k != hover_) {
        hover_ = k;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_MOUSEWHEEL:
      scroll_ = std::max(0.0, scroll_ + Fl::event_dy() * S(40));
      redraw();
      return 1;
    case FL_PUSH: {
      int k = at();
      if (k < 0 || !v) return 1;
      const Item it = items_[size_t(k)];
      switch (it.kind) {
        case Item::Swatch: v->set_ink(kSwatches[it.index].color); break;
        case Item::Opacity:
          dragging_opacity_ = true;
          set_opacity(it, false);
          break;
        case Item::Width:
          v->set_stroke(kWidths[it.index]);
          app_->tool_changed();
          break;
        case Item::FontSize:
          v->set_font_size(kSizes[it.index]);
          app_->tool_changed();
          break;
        case Item::Align:
          v->set_align(it.index);
          app_->tool_changed();
          break;
        case Item::Action:
          if (it.index == EditNote) v->edit_selected_note();
          else if (it.index == Delete) v->delete_selected_annot();
          else if (it.index == Bookmark) app_->toggle_bookmark();
          else {
            v->set_stamp_text(kStamps[it.index - Stamp0]);
            app_->tool_changed();
          }
          break;
      }
      redraw();
      return 1;
    }
    case FL_DRAG:
      if (dragging_opacity_)
        for (const auto &it : items_)
          if (it.kind == Item::Opacity) set_opacity(it, false);
      return 1;
    case FL_RELEASE:
      if (dragging_opacity_) {
        dragging_opacity_ = false;
        for (const auto &it : items_)
          if (it.kind == Item::Opacity) set_opacity(it, true);
      }
      return 1;
  }
  return Fl_Widget::handle(event);
}

}  // namespace xanara
