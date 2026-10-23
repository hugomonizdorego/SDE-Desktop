// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's side panel: the photo information, and the editing tools
// (adjustments, filters, crop/rotate/resize).
#include <FL/Fl.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "photo.h"
#include "util.h"

using namespace sde;
namespace icon = gallery::icon;

namespace photo {

namespace {

struct SliderSpec {
  const char *label;
  double lo, hi;
  double Adjust::*field;
};

const SliderSpec kSliders[] = {{N_("Exposure"), -100, 100, &Adjust::exposure},
                               {N_("Brightness"), -100, 100, &Adjust::brightness},
                               {N_("Contrast"), -100, 100, &Adjust::contrast},
                               {N_("Saturation"), -100, 100, &Adjust::saturation},
                               {N_("Warmth"), -100, 100, &Adjust::warmth},
                               {N_("Fade"), 0, 100, &Adjust::fade},
                               {N_("Vignette"), 0, 100, &Adjust::vignette}};
constexpr int kSliderCount = int(sizeof kSliders / sizeof kSliders[0]);

struct AspectSpec {
  const char *label;
  double ratio;  // 0 free, -1 original
};
const AspectSpec kAspects[] = {{N_("Free"), 0}, {N_("Original"), -1}, {"1:1", 1},
                               {"4:3", 4.0 / 3}, {"3:2", 1.5},     {"16:9", 16.0 / 9}};

std::vector<std::string> wrap(const std::string &text, int width, size_t max_lines) {
  std::vector<std::string> lines;
  std::string line;
  size_t i = 0;
  while (i < text.size()) {
    size_t n = 1;
    while (i + n < text.size() && (uchar(text[i + n]) & 0xC0) == 0x80) ++n;
    std::string cp = text.substr(i, n);
    if (text_width(line + cp) > width && !line.empty()) {
      size_t cut = line.find_last_of(" /_-");
      std::string rest;
      if (cut != std::string::npos && cut > line.size() / 2) {
        rest = line.substr(cut + 1);
        line = line.substr(0, cut + 1);
      }
      lines.push_back(line);
      line = rest;
      if (lines.size() + 1 == max_lines) {
        lines.push_back(ellipsize(line + text.substr(i), width));
        return lines;
      }
    }
    line += cp;
    i += n;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

std::string type_name(const std::string &path) {
  Format f = sniff_format(path);
  if (f != Format::Unknown) return format(_("%s image"), format_name(f));
  std::string e = gallery::extension_of(path);
  for (auto &c : e) c = char(toupper(uchar(c)));
  return format(_("%s image"), e.c_str());
}

// The Draw tab's tools, in the order shown.
struct ToolSpec {
  Tool tool;
  const char *name;
  const char *key;
  double lo, hi;  // width range (screen pixels)
};
const ToolSpec kTools[kToolCount] = {
    {Tool::Select, N_("Select and move"), "S", 1, 40},   {Tool::Pen, N_("Pen"), "P", 1, 40},
    {Tool::Highlighter, N_("Highlighter"), "M", 6, 60},  {Tool::Eraser, N_("Eraser"), "X", 6, 90},
    {Tool::Line, N_("Line"), "L", 1, 40},                {Tool::Arrow, N_("Arrow"), "A", 1, 40},
    {Tool::Rect, N_("Rectangle"), "U", 1, 40},           {Tool::Ellipse, N_("Ellipse"), "O", 1, 40},
    {Tool::Text, N_("Text"), "T", 8, 160},               {Tool::Blur, N_("Blur region"), "", 2, 40},
    {Tool::Pixelate, N_("Pixelate region"), "", 4, 64},  {Tool::Picker, N_("Pick a colour"), "K", 1, 40}};

enum DrawId { D_TOOL = 0, D_SWATCH = 100, D_CUSTOM = 150, D_FILL = 200, D_BOX, D_FONT = 210, D_DELETE = 300, D_CLEAR };

// Line glyphs for the tools, on a 24-unit grid.
void tool_glyph(Painter &p, Tool t, double x, double y, double s, const Color &c, uint32_t ink) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s / 24, s / 24);
  cairo_set_line_width(cr, 1.8);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  p.set(c);
  Color inkc = Color::hex(ink & 0xFFFFFF);
  switch (t) {
    case Tool::Select:
      cairo_move_to(cr, 6, 3);
      cairo_line_to(cr, 6, 19);
      cairo_line_to(cr, 10.5, 15);
      cairo_line_to(cr, 13.5, 21);
      cairo_line_to(cr, 16, 20);
      cairo_line_to(cr, 13, 14);
      cairo_line_to(cr, 18.5, 13.5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Tool::Pen:
      cairo_move_to(cr, 4, 20);
      cairo_line_to(cr, 5, 16);
      cairo_line_to(cr, 16, 5);
      cairo_line_to(cr, 19, 8);
      cairo_line_to(cr, 8, 19);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 14, 7);
      cairo_line_to(cr, 17, 10);
      cairo_stroke(cr);
      break;
    case Tool::Highlighter:
      cairo_move_to(cr, 8, 15);
      cairo_line_to(cr, 15, 4);
      cairo_line_to(cr, 20, 7);
      cairo_line_to(cr, 13, 18);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 8, 15);
      cairo_line_to(cr, 6, 19);
      cairo_line_to(cr, 10, 20);
      cairo_line_to(cr, 13, 18);
      cairo_stroke(cr);
      p.set(inkc.alpha(0.55));
      cairo_set_line_width(cr, 3);
      cairo_move_to(cr, 3, 22);
      cairo_line_to(cr, 21, 22);
      cairo_stroke(cr);
      break;
    case Tool::Eraser:
      cairo_move_to(cr, 9, 20);
      cairo_line_to(cr, 3.5, 14.5);
      cairo_line_to(cr, 13, 5);
      cairo_line_to(cr, 20.5, 12.5);
      cairo_line_to(cr, 13, 20);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 8, 10);
      cairo_line_to(cr, 15.5, 17.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 13, 20);
      cairo_line_to(cr, 21, 20);
      cairo_stroke(cr);
      break;
    case Tool::Line:
      cairo_move_to(cr, 4, 20);
      cairo_line_to(cr, 20, 4);
      cairo_stroke(cr);
      break;
    case Tool::Arrow:
      cairo_move_to(cr, 4, 20);
      cairo_line_to(cr, 19, 5);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 5);
      cairo_line_to(cr, 19, 5);
      cairo_line_to(cr, 19, 14);
      cairo_stroke(cr);
      break;
    case Tool::Rect:
      cairo_rectangle(cr, 4, 6, 16, 12);
      cairo_stroke(cr);
      break;
    case Tool::Ellipse:
      cairo_save(cr);
      cairo_translate(cr, 12, 12);
      cairo_scale(cr, 8.5, 6.5);
      cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
      cairo_restore(cr);
      cairo_stroke(cr);
      break;
    case Tool::Text:
      cairo_move_to(cr, 5, 6);
      cairo_line_to(cr, 5, 4);
      cairo_line_to(cr, 19, 4);
      cairo_line_to(cr, 19, 6);
      cairo_move_to(cr, 12, 4);
      cairo_line_to(cr, 12, 20);
      cairo_move_to(cr, 9, 20);
      cairo_line_to(cr, 15, 20);
      cairo_stroke(cr);
      break;
    case Tool::Blur:
      for (int i = 0; i < 3; ++i) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, 12, 12, 3 + i * 3.5, 0, 2 * M_PI);
        p.set(c.alpha(c.a * (1 - i * 0.3)));
        cairo_stroke(cr);
      }
      break;
    case Tool::Pixelate:
      for (int yy = 0; yy < 3; ++yy)
        for (int xx = 0; xx < 3; ++xx) {
          cairo_rectangle(cr, 4 + xx * 5.6, 4 + yy * 5.6, 4.6, 4.6);
          p.set(c.alpha(c.a * ((xx + yy) % 2 ? 0.35 : 1)));
          cairo_fill(cr);
        }
      break;
    case Tool::Picker:
      cairo_move_to(cr, 4, 20);
      cairo_line_to(cr, 5, 16);
      cairo_line_to(cr, 13, 8);
      cairo_line_to(cr, 16, 11);
      cairo_line_to(cr, 8, 19);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 11, 6);
      cairo_line_to(cr, 18, 13);
      cairo_stroke(cr);
      cairo_arc(cr, 17, 7, 3.2, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
  }
  cairo_restore(cr);
}

std::string date_text(time_t t) {
  char buf[128];
  struct tm tm {};
  localtime_r(&t, &tm);
  if (!strftime(buf, sizeof buf, "%A, %e %B %Y, %H:%M", &tm)) return "";
  return trim(buf);
}

}  // namespace

SidePanel::SidePanel(PhotoWindow *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  begin();
  for (int i = 0; i < kSliderCount; ++i) {
    auto *s = new Slider(0, 0, 10, S(26));
    s->bounds(kSliders[i].lo, kSliders[i].hi);
    s->step(1);
    s->value(0);
    s->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    s->callback([](Fl_Widget *w, void *p) {
      auto *panel = static_cast<SidePanel *>(p);
      for (size_t k = 0; k < panel->sliders_.size(); ++k)
        if (panel->sliders_[k] == w) panel->slider_changed(int(k));
    }, this);
    sliders_.push_back(s);
  }
  strength_ = new Slider(0, 0, 10, S(26));
  strength_->bounds(0, 100);
  strength_->step(1);
  strength_->value(100);
  strength_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
  strength_->callback([](Fl_Widget *, void *p) { static_cast<SidePanel *>(p)->slider_changed(-1); }, this);
  struct B {
    const char *label;
    std::function<void(PhotoWindow *)> fn;
  };
  std::vector<B> crop = {{N_("Rotate Left"), [](PhotoWindow *a) { a->rotate(-1); }},
                         {N_("Rotate Right"), [](PhotoWindow *a) { a->rotate(1); }},
                         {N_("Flip Horizontally"), [](PhotoWindow *a) { a->flip(true); }},
                         {N_("Flip Vertically"), [](PhotoWindow *a) { a->flip(false); }},
                         {N_("Crop…"), [](PhotoWindow *a) { a->start_crop(); }},
                         {N_("Apply Crop"), [](PhotoWindow *a) { a->apply_crop(); }},
                         {N_("Cancel"), [](PhotoWindow *a) { a->cancel_crop(); }}};
  for (size_t i = 0; i < crop.size(); ++i) {
    auto *b = new FlatButton(0, 0, 10, S(34), _(crop[i].label), i == 5);
    crop_fns_.push_back(std::make_unique<std::function<void(PhotoWindow *)>>(crop[i].fn));
    b->callback([](Fl_Widget *w, void *f) {
      auto *panel = static_cast<SidePanel *>(w->parent());
      (*static_cast<std::function<void(PhotoWindow *)> *>(f))(panel->app_);
    }, crop_fns_.back().get());
    crop_buttons_.push_back(b);
  }
  for (size_t i = 0; i < sizeof kAspects / sizeof kAspects[0]; ++i) {
    auto *b = new FlatButton(0, 0, 10, S(30), _(kAspects[i].label));
    b->callback([](Fl_Widget *w, void *data) {
      auto *panel = static_cast<SidePanel *>(w->parent());
      const AspectSpec &a = kAspects[reinterpret_cast<size_t>(data)];
      double r = a.ratio;
      if (r < 0) r = panel->app_->image_h() > 0 ? double(panel->app_->image_w()) / panel->app_->image_h() : 0;
      // Portrait photos get portrait crops.
      if (r > 0 && a.ratio > 0 && panel->app_->canvas()->crop_rect().h * panel->app_->image_h() >
                                       panel->app_->canvas()->crop_rect().w * panel->app_->image_w())
        r = 1 / r;
      panel->app_->set_crop_aspect(r);
    }, reinterpret_cast<void *>(i));
    aspect_buttons_.push_back(b);
  }
  res_w_ = new Fl_Int_Input(0, 0, 10, S(30));
  res_h_ = new Fl_Int_Input(0, 0, 10, S(30));
  for (auto *in : {res_w_, res_h_}) {
    in->when(FL_WHEN_CHANGED);
    in->callback([](Fl_Widget *w, void *p) {
      auto *panel = static_cast<SidePanel *>(p);
      if (!panel->res_lock_) return;
      int ow, oh;
      Edits e = panel->app_->edits();
      e.resize_w = e.resize_h = 0;
      result_size(e, panel->app_->image_w(), panel->app_->image_h(), ow, oh);
      if (ow <= 0 || oh <= 0) return;
      int v = atoi(static_cast<Fl_Int_Input *>(w)->value());
      if (v <= 0) return;
      std::string other = std::to_string(std::max(1, int(std::lround(w == panel->res_w_ ? v * double(oh) / ow : v * double(ow) / oh))));
      (w == panel->res_w_ ? panel->res_h_ : panel->res_w_)->value(other.c_str());
    }, this);
  }
  res_apply_ = new FlatButton(0, 0, 10, S(32), _("Resize"));
  res_apply_->callback([](Fl_Widget *, void *p) {
    auto *panel = static_cast<SidePanel *>(p);
    int w = atoi(panel->res_w_->value()), h = atoi(panel->res_h_->value());
    if (w <= 0 || h <= 0 || w > 30000 || h > 30000) {
      panel->app_->toast(_("Enter a width and height between 1 and 30000 pixels"));
      return;
    }
    Edits e = panel->app_->edits();
    int ow, oh;
    Edits base = e;
    base.resize_w = base.resize_h = 0;
    result_size(base, panel->app_->image_w(), panel->app_->image_h(), ow, oh);
    if (w == ow && h == oh) w = h = 0;
    e.resize_w = w;
    e.resize_h = h;
    panel->app_->apply(e);
  }, this);
  for (int i = 0; i < 2; ++i) {
    auto *sl = new Slider(0, 0, 10, S(26));
    sl->step(1);
    sl->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    sl->callback([](Fl_Widget *w, void *p) {
      auto *panel = static_cast<SidePanel *>(p);
      panel->draw_slider_changed(w == panel->draw_width_ ? 0 : 1);
    }, this);
    (i ? draw_opacity_ : draw_width_) = sl;
  }
  draw_opacity_->bounds(10, 100);
  reset_ = new FlatButton(0, 0, 10, S(34), _("Reset"));
  reset_->callback([](Fl_Widget *, void *p) { static_cast<SidePanel *>(p)->app_->reset_edits(); }, this);
  save_as_ = new FlatButton(0, 0, 10, S(34), _("Save As…"));
  save_as_->callback([](Fl_Widget *, void *p) { static_cast<SidePanel *>(p)->app_->save_as(); }, this);
  save_ = new FlatButton(0, 0, 10, S(34), _("Save"), true);
  save_->callback([](Fl_Widget *, void *p) { static_cast<SidePanel *>(p)->app_->save(); }, this);
  end();
  resizable(nullptr);
  layout();
}

void SidePanel::resize(int x, int y, int w, int h) {
  Fl_Widget::resize(x, y, w, h);  // children are placed by layout()
  layout();
}

void SidePanel::set_mode(Mode m) {
  mode_ = m;
  info_scroll_ = 0;
  layout();
  sync();
  redraw();
}

void SidePanel::set_tab(Tab t) {
  if (tab_ == Draw && t != Draw) app_->canvas()->cancel_drawing();
  tab_ = t;
  draw_scroll_ = 0;
  if (t != Crop && app_->canvas()->cropping()) app_->cancel_crop();
  layout();
  sync();
  redraw();
}

void SidePanel::layout() {
  for (int i = 0; i < children(); ++i) child(i)->hide();
  if (mode_ != Edit) return;
  int px = x() + S(20), pw = w() - S(40);
  int top = y() + S(64);
  if (tab_ == Adjust) {
    int yy = top + S(22);
    for (auto *s : sliders_) {
      s->resize(px, yy, pw, S(26));
      s->show();
      yy += S(58);
    }
  } else if (tab_ == Draw) {
    layout_draw(px, pw, top);
  } else if (tab_ == Filters) {
    int tile = (pw - 2 * S(8)) / 3;
    int rows = int((filters().size() + 2) / 3);
    int yy = top + rows * (tile + S(26)) + S(30);
    strength_->resize(px, yy, pw, S(26));
    strength_->show();
  } else {
    bool cropping = app_->canvas()->cropping();
    int yy = top;
    int half = (pw - S(8)) / 2;
    if (!cropping) {
      for (int i = 0; i < 4; ++i) {
        crop_buttons_[size_t(i)]->resize(px + (i % 2) * (half + S(8)), yy + (i / 2) * S(42), half, S(34));
        crop_buttons_[size_t(i)]->show();
      }
      yy += 2 * S(42) + S(14);
      crop_buttons_[4]->resize(px, yy, pw, S(34));
      crop_buttons_[4]->show();
      yy += S(34) + S(40);
      int iw = (pw - S(24)) / 2;
      res_w_->resize(px, yy, iw, S(30));
      res_h_->resize(px + iw + S(24), yy, iw, S(30));
      res_w_->show();
      res_h_->show();
      yy += S(40);
      res_apply_->resize(px, yy, pw, S(32));
      res_apply_->show();
    } else {
      yy += S(24);
      int third = (pw - 2 * S(6)) / 3;
      for (size_t i = 0; i < aspect_buttons_.size(); ++i) {
        aspect_buttons_[i]->resize(px + int(i % 3) * (third + S(6)), yy + int(i / 3) * S(38), third, S(30));
        aspect_buttons_[i]->show();
      }
      yy += 2 * S(38) + S(16);
      crop_buttons_[6]->resize(px, yy, half, S(34));
      crop_buttons_[5]->resize(px + half + S(8), yy, half, S(34));
      crop_buttons_[5]->show();
      crop_buttons_[6]->show();
    }
  }
  int by = y() + h() - S(52);
  int third = (pw - 2 * S(8)) / 3;
  reset_->resize(px, by, third, S(34));
  save_as_->resize(px + third + S(8), by, third, S(34));
  save_->resize(px + 2 * (third + S(8)), by, third, S(34));
  reset_->show();
  save_as_->show();
  save_->show();
  const Theme &t = app_->theme();
  for (auto *in : {res_w_, res_h_}) {
    in->box(FL_FLAT_BOX);
    in->color(t.surface_alt.fl());
    in->textcolor(t.text.fl());
    in->selection_color(t.accent.fl());
    in->cursor_color(t.accent.fl());
    in->textsize(t.font_size);
  }
}

// ------------------------------------------------------------- Draw tab

namespace {

// The tool whose options the panel shows: the selected mark's, else the
// current tool's.
Tool panel_tool(PhotoWindow *app) {
  int i = app->canvas()->selected_mark();
  const auto &items = app->edits().marks.items;
  if (i < 0 || i >= int(items.size())) return app->draw_settings().tool;
  switch (items[size_t(i)].kind) {
    case MarkKind::Pen: return Tool::Pen;
    case MarkKind::Highlighter: return Tool::Highlighter;
    case MarkKind::Line: return Tool::Line;
    case MarkKind::Arrow: return Tool::Arrow;
    case MarkKind::Rect: return Tool::Rect;
    case MarkKind::Ellipse: return Tool::Ellipse;
    case MarkKind::Text: return Tool::Text;
    case MarkKind::Blur: return Tool::Blur;
    case MarkKind::Pixelate: return Tool::Pixelate;
  }
  return Tool::Pen;
}

bool has_opacity(Tool t) {
  return t == Tool::Pen || t == Tool::Highlighter || t == Tool::Line || t == Tool::Arrow || t == Tool::Rect ||
         t == Tool::Ellipse || t == Tool::Text;
}

const char *width_label(Tool t) {
  switch (t) {
    case Tool::Eraser: return _("Eraser size");
    case Tool::Text: return _("Text size");
    case Tool::Blur: return _("Blur strength");
    case Tool::Pixelate: return _("Block size");
    default: break;
  }
  return _("Stroke width");
}

}  // namespace

void SidePanel::layout_draw(int px, int pw, int top) {
  draw_boxes_.clear();
  DrawSettings &d = app_->draw_settings();
  Tool t = panel_tool(app_);
  bool selection = app_->canvas()->selected_mark() >= 0;
  bool compact = draw_compact();
  double cell_h = compact ? S(34) : S(40), label = compact ? S(20) : S(22);
  if (compact) top -= S(4);
  double yy = top - draw_scroll_;
  draw_tool_label_y_ = yy;
  yy += label;
  double gap = compact ? S(4) : S(6), cell = (pw - 5 * gap) / 6;
  for (int i = 0; i < kToolCount; ++i)
    draw_boxes_.push_back({px + (i % 6) * (cell + gap), yy + (i / 6) * (cell_h + gap), cell, cell_h, D_TOOL + i});
  yy += 2 * (cell_h + gap) + (compact ? S(6) : S(12));
  draw_colour_label_y_ = yy;
  yy += label;
  const auto &pal = palette();
  double sw = double(pw) / double(pal.size() + 1);
  for (size_t i = 0; i < pal.size(); ++i) draw_boxes_.push_back({px + i * sw, yy, sw, double(S(30)), D_SWATCH + int(i)});
  draw_boxes_.push_back({px + pal.size() * sw, yy, sw, double(S(30)), D_CUSTOM});
  yy += S(30) + (compact ? S(8) : S(14));
  int content_top = draw_top(), content_bottom = draw_bottom();
  auto place = [&](Slider *sl, bool on) {
    if (!on) return;
    yy += S(20);
    sl->resize(px, int(yy), pw, S(26));
    if (sl->y() + sl->h() > content_top && sl->y() < content_bottom) sl->show();
    yy += S(26) + (compact ? S(6) : S(12));
  };
  bool width = t != Tool::Picker && (t != Tool::Select || selection);
  int ti = int(t);
  draw_width_->bounds(kTools[ti].lo, kTools[ti].hi);
  if (t == Tool::Text) draw_width_->value(d.text_size);
  else draw_width_->value(std::max(kTools[ti].lo, std::min(kTools[ti].hi, d.width[ti])));
  draw_opacity_->value(d.opacity[ti]);
  place(draw_width_, width);
  place(draw_opacity_, has_opacity(t));
  if (t == Tool::Rect || t == Tool::Ellipse) {
    draw_boxes_.push_back({double(px), yy, double(pw), double(S(32)), D_FILL});
    yy += S(42);
  } else if (t == Tool::Text) {
    double fw = (pw - 3 * gap) / 4;
    for (int i = 0; i < 3; ++i) draw_boxes_.push_back({px + i * (fw + gap), yy, fw, double(S(32)), D_FONT + i});
    draw_boxes_.push_back({px + 3 * (fw + gap), yy, fw, double(S(32)), D_BOX});
    yy += S(42);
  }
  if (!app_->edits().marks.empty() || selection) {
    double half = (pw - S(8)) / 2.0;
    draw_boxes_.push_back({double(px), yy, half, double(S(34)), D_DELETE});
    draw_boxes_.push_back({px + half + S(8), yy, half, double(S(34)), D_CLEAR});
    yy += S(44);
  }
  draw_h_ = yy + draw_scroll_ - top;
}

void SidePanel::draw_slider_changed(int which) {
  DrawSettings &d = app_->draw_settings();
  Tool t = panel_tool(app_);
  if (which == 0) {
    if (t == Tool::Text) d.text_size = draw_width_->value();
    else d.width[int(t)] = draw_width_->value();
  } else {
    d.opacity[int(t)] = draw_opacity_->value();
  }
  bool release = Fl::event() == FL_RELEASE;
  bool drag = Fl::event() == FL_DRAG || release;
  if (app_->canvas()->selected_mark() >= 0) app_->style_changed(gesture_ && drag);
  gesture_ = drag && !release;
  app_->canvas()->redraw();
  redraw();
}

int SidePanel::draw_tab_click(int mx, int my) {
  if (my < draw_top() || my >= draw_bottom()) return 0;
  DrawSettings &d = app_->draw_settings();
  for (const auto &b : draw_boxes_) {
    if (!b.in(mx, my)) continue;
    bool restyle = false;
    if (b.id >= D_TOOL && b.id < D_TOOL + kToolCount) {
      app_->set_tool(kTools[b.id - D_TOOL].tool);
    } else if (b.id >= D_SWATCH && b.id < D_SWATCH + int(palette().size())) {
      d.color = palette()[size_t(b.id - D_SWATCH)];
      restyle = true;
    } else if (b.id == D_CUSTOM) {
      uchar r = (d.color >> 16) & 0xFF, g = (d.color >> 8) & 0xFF, bl = d.color & 0xFF;
      if (fl_color_chooser(_("Drawing Colour"), r, g, bl)) {
        d.color = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | bl;
        restyle = true;
      }
    } else if (b.id == D_FILL) {
      d.fill = !d.fill;
      restyle = true;
    } else if (b.id == D_BOX) {
      d.text_box = !d.text_box;
      restyle = true;
    } else if (b.id >= D_FONT && b.id < D_FONT + 3) {
      d.font = FontFace(b.id - D_FONT);
      restyle = true;
    } else if (b.id == D_DELETE) {
      app_->delete_selected_mark();
    } else if (b.id == D_CLEAR) {
      Edits e = app_->edits();
      if (!e.marks.empty()) {
        e.marks.items.clear();
        app_->canvas()->cancel_drawing();
        app_->apply(e);
        app_->toast(_("All drawings removed · Ctrl+Z brings them back"));
      }
    }
    if (restyle) app_->style_changed(false);
    layout();
    app_->canvas()->redraw();
    redraw();
    return 1;
  }
  return 0;
}

void SidePanel::draw_draw_tab(Painter *p) {
  const Theme &t = app_->theme();
  DrawSettings &d = app_->draw_settings();
  Tool shown = panel_tool(app_);
  int px = x() + S(20), pw = w() - S(40);
  bool selection = app_->canvas()->selected_mark() >= 0;
  bool any = !app_->edits().marks.empty();
  for (const auto &b : draw_boxes_) {
    bool hot = b.id == draw_hover_;
    if (b.id >= D_TOOL && b.id < D_TOOL + kToolCount) {
      const ToolSpec &spec = kTools[b.id - D_TOOL];
      bool on = spec.tool == d.tool;
      if (p) {
        if (on) p->fill_rounded(b.x, b.y, b.w, b.h, Sd(9), t.accent);
        else p->fill_rounded(b.x, b.y, b.w, b.h, Sd(9), hot ? t.hover : t.surface_alt);
        double gs = S(22);
        tool_glyph(*p, spec.tool, b.x + (b.w - gs) / 2, b.y + (b.h - gs) / 2, gs, on ? t.on_accent : t.text, d.color);
      }
    } else if (b.id >= D_SWATCH && b.id < D_SWATCH + int(palette().size())) {
      if (!p) continue;
      uint32_t c = palette()[size_t(b.id - D_SWATCH)];
      double r = std::min(b.w, b.h) / 2 - Sd(3), cx = b.x + b.w / 2, cy = b.y + b.h / 2;
      bool on = (d.color & 0xFFFFFF) == (c & 0xFFFFFF);
      if (on) p->circle(cx, cy, r + Sd(2.5), t.accent);
      else if (hot) p->circle(cx, cy, r + Sd(2), t.border);
      p->circle(cx, cy, r, t.surface);
      p->circle(cx, cy, r - Sd(1.5), Color::hex(c & 0xFFFFFF));
      if ((c & 0xFFFFFF) == 0xFFFFFF || (c & 0xFFFFFF) == 0x111111) {
        cairo_new_path(p->cr());
        cairo_arc(p->cr(), cx, cy, r - Sd(1.5), 0, 2 * M_PI);
        p->set(t.border);
        cairo_set_line_width(p->cr(), 1);
        cairo_stroke(p->cr());
      }
    } else if (b.id == D_CUSTOM) {
      if (!p) continue;
      double r = std::min(b.w, b.h) / 2 - Sd(3), cx = b.x + b.w / 2, cy = b.y + b.h / 2;
      bool custom = true;
      for (uint32_t c : palette())
        if ((c & 0xFFFFFF) == (d.color & 0xFFFFFF)) custom = false;
      if (custom) p->circle(cx, cy, r + Sd(2.5), t.accent);
      // A colour wheel.
      for (int i = 0; i < 12; ++i) {
        double a0 = i * M_PI / 6, a1 = (i + 1) * M_PI / 6;
        double h = i / 12.0;
        auto f = [h](double off) {
          double k = std::fmod(h * 6 + off, 6);
          return std::max(0.0, std::min(1.0, std::min(k, 4 - k)));
        };
        cairo_move_to(p->cr(), cx, cy);
        cairo_arc(p->cr(), cx, cy, r - Sd(1.5), a0, a1 + 0.02);
        cairo_close_path(p->cr());
        cairo_set_source_rgb(p->cr(), f(5), f(3), f(1));
        cairo_fill(p->cr());
      }
      p->circle(cx, cy, r * 0.45, custom ? Color::hex(d.color & 0xFFFFFF) : t.surface);
    } else {
      bool on = (b.id == D_FILL && d.fill) || (b.id == D_BOX && d.text_box) ||
                (b.id >= D_FONT && b.id < D_FONT + 3 && int(d.font) == b.id - D_FONT);
      bool enabled = b.id == D_DELETE ? selection : b.id == D_CLEAR ? any : true;
      std::string label = b.id == D_FILL       ? _("Fill the shape")
                          : b.id == D_BOX      ? _("Box")
                          : b.id == D_FONT     ? _("Sans")
                          : b.id == D_FONT + 1 ? _("Serif")
                          : b.id == D_FONT + 2 ? _("Mono")
                          : b.id == D_DELETE   ? _("Delete")
                                               : _("Remove All");
      if (p) {
        Color bg = on ? t.accent.alpha(t.dark ? 0.32 : 0.16) : hot && enabled ? t.hover : t.surface_alt;
        p->fill_rounded(b.x, b.y, b.w, b.h, Sd(9), bg);
        if (on) p->stroke_rounded(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, Sd(9), t.accent, Sd(1.2));
        if (b.id == D_FILL) {
          double bx = b.x + S(10), by = b.y + (b.h - S(16)) / 2;
          p->stroke_rounded(bx, by, S(16), S(16), Sd(4), on ? t.accent : t.text_dim, Sd(1.5));
          if (on) p->fill_rounded(bx + S(3), by + S(3), S(10), S(10), Sd(2), t.accent);
        }
      } else {
        text_font(t.font_size - S(1), on);
        Color fg = !enabled ? t.text_dim.alpha(0.6) : on ? t.accent_text : t.text;
        if (b.id == D_FILL) draw_text(label, int(b.x + S(34)), int(b.y), int(b.w - S(40)), int(b.h), fg);
        else draw_text(label, int(b.x), int(b.y), int(b.w), int(b.h), fg, 1);
      }
    }
  }
  if (p) return;
  // Labels.
  text_font(t.font_size - S(1), true);
  double ly = draw_tool_label_y_;
  const ToolSpec &cur = kTools[int(d.tool)];
  std::string tool_line = _(cur.name);
  if (*cur.key) tool_line += format("  (%s)", cur.key);
  draw_text(tool_line, px, int(ly), pw, S(18), t.accent_text);
  double cy = draw_colour_label_y_;
  draw_text(_("COLOUR"), px, int(cy), pw, S(18), t.accent_text);
  text_font(t.font_size - S(1));
  char hex[16];
  snprintf(hex, sizeof hex, "#%06X", unsigned(d.color & 0xFFFFFF));
  draw_text(hex, px, int(cy), pw, S(18), t.text_dim, 2);
  for (Slider *sl : {draw_width_, draw_opacity_}) {
    if (!sl->visible()) continue;
    text_font(t.font_size - S(1));
    std::string label = sl == draw_width_ ? width_label(shown) : _("Opacity");
    draw_text(label, px, sl->y() - S(20), pw, S(18), t.text);
    int v = int(std::lround(sl->value()));
    draw_text(sl == draw_width_ ? format(_("%d px"), v) : format("%d%%", v), px, sl->y() - S(20), pw, S(18),
              t.text_dim, 2);
  }
}

void SidePanel::sync() {
  const Edits &e = app_->edits();
  for (int i = 0; i < kSliderCount; ++i) sliders_[size_t(i)]->value(e.adjust.*kSliders[i].field);
  strength_->value(e.strength);
  int ow, oh;
  result_size(e, app_->image_w(), app_->image_h(), ow, oh);
  if (Fl::focus() != res_w_ && Fl::focus() != res_h_) {
    res_w_->value(std::to_string(ow).c_str());
    res_h_->value(std::to_string(oh).c_str());
  }
  if (app_->modified()) {
    reset_->activate();
    save_->activate();
  } else {
    reset_->deactivate();
    save_->deactivate();
  }
  if (mode_ == Edit && tab_ == Filters) filter_previews();
  if (mode_ == Edit && tab_ == Draw) layout();
  redraw();
}

void SidePanel::slider_changed(int index) {
  Edits e = app_->edits();
  if (index >= 0) e.adjust.*kSliders[index].field = sliders_[size_t(index)]->value();
  else e.strength = int(strength_->value());
  bool release = Fl::event() == FL_RELEASE;
  bool drag = Fl::event() == FL_DRAG || release;
  // One undo step per drag.
  app_->apply(e, gesture_ && drag);
  gesture_ = drag && !release;
  redraw();
}

void SidePanel::filter_previews() {
  cairo_surface_t *base = app_->preview_base();
  if (!base) return;
  if (!preview_source_) preview_source_ = downscale(base, S(110));
  Edits e = app_->edits();
  e.orient = {};  // previews show the tone only
  e.crop = Rect();
  e.resize_w = e.resize_h = 0;
  e.marks = Marks();
  for (auto *s : previews_) cairo_surface_destroy(s);
  previews_.clear();
  for (const auto &f : filters()) {
    Edits fe = e;
    fe.filter = f.id;
    fe.strength = 100;
    previews_.push_back(render(preview_source_, fe));
  }
}

int SidePanel::filter_at(int mx, int my) const {
  if (mode_ != Edit || tab_ != Filters) return -1;
  int px = x() + S(20), pw = w() - S(40);
  int tile = (pw - 2 * S(8)) / 3, top = y() + S(64);
  for (size_t i = 0; i < filters().size(); ++i) {
    int tx = px + int(i % 3) * (tile + S(8)), ty = top + int(i / 3) * (tile + S(26));
    if (mx >= tx && mx < tx + tile && my >= ty && my < ty + tile + S(22)) return int(i);
  }
  return -1;
}

void SidePanel::draw_filters(Painter &p) {
  const Theme &t = app_->theme();
  int px = x() + S(20), pw = w() - S(40);
  int tile = (pw - 2 * S(8)) / 3, top = y() + S(64);
  cairo_t *cr = p.cr();
  for (size_t i = 0; i < filters().size(); ++i) {
    double tx = px + int(i % 3) * (tile + S(8)), ty = top + int(i / 3) * (tile + S(26));
    bool active = filters()[i].id == app_->edits().filter;
    cairo_save(cr);
    p.rounded_rect(tx, ty, tile, tile, Sd(8));
    cairo_clip(cr);
    p.fill_rect(tx, ty, tile, tile, t.surface_alt);
    if (i < previews_.size() && previews_[i]) {
      cairo_surface_t *s = previews_[i];
      double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
      double sc = std::max(tile / sw, tile / sh);
      cairo_translate(cr, tx + (tile - sw * sc) / 2, ty + (tile - sh * sc) / 2);
      cairo_scale(cr, sc, sc);
      cairo_set_source_surface(cr, s, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
      cairo_paint(cr);
    }
    cairo_restore(cr);
    if (active) p.stroke_rounded(tx - Sd(1.5), ty - Sd(1.5), tile + Sd(3), tile + Sd(3), Sd(9), t.accent, Sd(2.5));
    else if (int(i) == filter_hover_) p.stroke_rounded(tx + 0.5, ty + 0.5, tile - 1, tile - 1, Sd(8), t.border, Sd(1.5));
  }
}

void SidePanel::draw_info(Painter *p) {
  // Called twice: with a painter for shapes, then without for text.
  const Theme &t = app_->theme();
  const gallery::Photo *ph = app_->current();
  int px = x() + S(20), pw = w() - S(40);
  double yy = y() + S(18) - info_scroll_;
  if (!ph) return;
  const gallery::PhotoInfo &in = app_->info();
  auto section = [&](const char *title) {
    yy += S(6);
    if (!p) {
      text_font(t.font_size - S(1), true);
      draw_text(title, px, int(yy), pw, S(18), t.accent_text);
    }
    yy += S(26);
  };
  auto field = [&](const std::string &label, const std::string &value, size_t lines = 1) {
    if (value.empty()) return;
    if (!p) {
      text_font(t.font_size - S(1));
      draw_text(label, px, int(yy), pw, S(18), t.text_dim);
    }
    yy += S(19);
    text_font(t.font_size);
    for (const auto &line : wrap(value, pw, lines)) {
      if (!p) draw_text(line, px, int(yy), pw, S(20), t.text);
      yy += S(20);
    }
    yy += S(8);
  };
  if (!p) {
    text_font(t.font_size + S(2), true);
    for (const auto &line : wrap(ph->name, pw, 3)) {
      draw_text(line, px, int(yy), pw, S(24), t.text);
      yy += S(24);
    }
  } else {
    text_font(t.font_size + S(2), true);
    yy += S(24) * double(wrap(ph->name, pw, 3).size());
  }
  yy += S(6);
  section(_("FILE"));
  if (app_->image_w() > 0) {
    double mp = app_->image_w() * double(app_->image_h()) / 1e6;
    field(_("Dimensions"), format("%d × %d", app_->image_w(), app_->image_h()) +
                               (mp >= 0.1 ? format(_("  ·  %.1f MP"), mp) : std::string()));
  } else {
    field(_("Dimensions"), app_->load_failed() ? _("Unknown") : _("Loading…"));
  }
  field(_("Size"), gallery::human_size(ph->size));
  field(_("Type"), type_name(ph->path));
  field(_("Modified"), date_text(ph->mtime));
  field(_("Folder"), ph->dir, 3);
  if (!in.color_profile.empty()) field(_("Colour profile"), in.color_profile);
  bool camera = !in.model.empty() || !in.make.empty() || in.exposure_time > 0 || in.f_number > 0 || in.iso > 0 ||
                in.taken;
  section(_("CAMERA"));
  if (camera) {
    if (in.taken) field(_("Taken"), date_text(in.taken));
    std::string model = in.model;
    if (!in.make.empty()) {
      // "NIKON CORPORATION" + "NIKON D750" -> "NIKON D750"; "Canon" + "EOS R6" -> "Canon EOS R6".
      std::string first = in.make.substr(0, in.make.find(' '));
      if (model.empty()) model = in.make;
      else if (lower(model).find(lower(first)) == std::string::npos) model = first + " " + model;
    }
    field(_("Camera"), model, 2);
    field(_("Lens"), in.lens, 2);
    std::vector<std::string> parts;
    if (in.exposure_time > 0) parts.push_back(gallery::format_exposure(in.exposure_time));
    if (in.f_number > 0) parts.push_back(gallery::format_aperture(in.f_number));
    if (in.iso > 0) parts.push_back("ISO " + std::to_string(in.iso));
    std::string exp;
    for (const auto &s : parts) exp += (exp.empty() ? "" : "  ·  ") + s;
    field(_("Exposure"), exp, 2);
    if (in.has_bias && std::fabs(in.exposure_bias) > 0.01) field(_("Exposure bias"), format("%+.1f EV", in.exposure_bias));
    field(_("Focal length"), gallery::format_focal(in.focal_length, in.focal_35mm));
    field(_("Flash"), gallery::format_flash(in.flash));
    field(_("Software"), in.software);
    field(_("Artist"), in.artist, 2);
    field(_("Copyright"), in.copyright, 2);
  } else {
    if (!p) {
      text_font(t.font_size);
      draw_text(_("No camera information"), px, int(yy), pw, S(20), t.text_dim);
    }
    yy += S(30);
  }
  if (in.has_gps) {
    section(_("LOCATION"));
    field(_("Coordinates"), gallery::format_coordinates(in.latitude, in.longitude), 2);
    if (in.has_altitude) field(_("Altitude"), format(_("%.0f m"), in.altitude));
    link_x_ = px;
    link_y_ = yy;
    link_w_ = pw;
    link_h_ = S(36);
    if (p) {
      p->fill_rounded(px, yy, pw, S(36), Sd(9), t.accent.alpha(t.dark ? 0.25 : 0.12));
      icon::draw(*p, icon::MapPin, px + S(10), yy + S(8), S(20), t.accent_text);
    } else {
      text_font(t.font_size, true);
      draw_text(_("Open in Map (OpenStreetMap)"), px + S(38), int(yy), pw - S(46), S(36), t.accent_text);
    }
    yy += S(46);
  } else {
    link_w_ = 0;
  }
  info_h_ = yy + info_scroll_ - y();
}

void SidePanel::draw() {
  const Theme &t = app_->theme();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.surface);
    p.line(x() + 0.5, y(), x() + 0.5, y() + h(), t.border);
    if (mode_ == Edit) {
      // Tabs.
      double tx = x() + S(20), tw = w() - S(40), th = S(34), ty = y() + S(14);
      p.fill_rounded(tx, ty, tw, th, th / 2, t.surface_alt);
      double seg = (tw - S(6)) / 4;
      p.fill_rounded(tx + S(3) + seg * tab_, ty + S(3), seg, th - S(6), (th - S(6)) / 2, t.accent);
      if (tab_ == Filters) draw_filters(p);
      if (tab_ == Draw) {
        cairo_save(p.cr());
        cairo_rectangle(p.cr(), x(), draw_top(), w(), draw_bottom() - draw_top());
        cairo_clip(p.cr());
        draw_draw_tab(&p);
        cairo_restore(p.cr());
        double room = draw_bottom() - draw_top() - S(8);
        if (draw_h_ > room) {
          // A scroll bar, and a fade where more options continue.
          double track = draw_bottom() - draw_top() - S(8);
          double bar = std::max(double(S(24)), track * room / draw_h_);
          double by = draw_top() + S(4) + (track - bar) * draw_scroll_ / std::max(1.0, draw_h_ - room);
          p.fill_rounded(x() + w() - S(7), by, S(4), bar, S(2), t.text_dim.alpha(0.45));
          if (draw_scroll_ < draw_h_ - room - 1)
            p.fill_gradient(x() + 1, draw_bottom() - S(18), w() - S(10), S(18), 0, t.surface.alpha(0), t.surface);
        }
      }
      p.line(x() + S(20), y() + h() - S(64), x() + w() - S(20), y() + h() - S(64), t.border);
    } else {
      cairo_save(p.cr());
      cairo_rectangle(p.cr(), x(), y(), w(), h());
      cairo_clip(p.cr());
      draw_info(&p);
      cairo_restore(p.cr());
    }
  }
  if (mode_ == Edit) {
    double tx = x() + S(20), tw = w() - S(40), th = S(34), ty = y() + S(14);
    double seg = (tw - S(6)) / 4;
    const char *names[] = {_("Adjust"), _("Filters"), _("Crop"), _("Draw")};
    for (int i = 0; i < 4; ++i) {
      text_font(t.font_size - S(1), i == tab_);
      draw_text(names[i], int(tx + S(3) + seg * i), int(ty), int(seg), int(th), i == tab_ ? t.on_accent : t.text, 1);
    }
    int px = x() + S(20), pw = w() - S(40);
    if (tab_ == Adjust) {
      for (int i = 0; i < kSliderCount; ++i) {
        Slider *s = sliders_[size_t(i)];
        text_font(t.font_size - S(1));
        draw_text(_(kSliders[i].label), px, s->y() - S(20), pw, S(18), t.text);
        int v = int(std::lround(s->value()));
        draw_text(v > 0 && kSliders[i].lo < 0 ? format("+%d", v) : std::to_string(v), px, s->y() - S(20), pw, S(18),
                  v ? t.accent_text : t.text_dim, 2);
      }
    } else if (tab_ == Draw) {
      fl_push_clip(x(), draw_top(), w(), draw_bottom() - draw_top());
      draw_draw_tab(nullptr);
      fl_pop_clip();
    } else if (tab_ == Filters) {
      int tile = (pw - 2 * S(8)) / 3, top = y() + S(64);
      for (size_t i = 0; i < filters().size(); ++i) {
        int fx = px + int(i % 3) * (tile + S(8)), fy = top + int(i / 3) * (tile + S(26));
        bool active = filters()[i].id == app_->edits().filter;
        text_font(t.font_size - S(1), active);
        draw_text(_(filters()[i].name), fx, fy + tile + S(2), tile, S(20), active ? t.accent_text : t.text, 1);
      }
      text_font(t.font_size - S(1));
      draw_text(_("Strength"), px, strength_->y() - S(20), pw, S(18), t.text);
      draw_text(format("%d%%", int(strength_->value())), px, strength_->y() - S(20), pw, S(18), t.text_dim, 2);
      if (app_->edits().filter == Filter::Original) strength_->deactivate();
      else strength_->activate();
    } else {
      text_font(t.font_size - S(1), true);
      if (!app_->canvas()->cropping()) {
        draw_text(_("RESIZE"), px, res_w_->y() - S(26), pw, S(18), t.accent_text);
        text_font(t.font_size);
        draw_text("×", res_w_->x() + res_w_->w(), res_w_->y(), S(24), res_w_->h(), t.text_dim, 1);
        text_font(t.font_size - S(1));
        draw_text(_("Size in pixels; proportions are kept."), px, res_apply_->y() + S(38), pw,
                  S(18), t.text_dim);
      } else {
        draw_text(_("ASPECT RATIO"), px, y() + S(64), pw, S(18), t.accent_text);
        text_font(t.font_size - S(1));
        draw_text(_("Drag the corners, edges or the frame."), px,
                  crop_buttons_[5]->y() + S(44), pw, S(18), t.text_dim);
      }
    }
    if (tab_ == Draw && draw_compact()) {
      // No room for a note.
    } else if (tab_ == Draw && !app_->edits().marks.empty()) {
      text_font(t.font_size - S(1));
      draw_text(_("Drawings stay editable until you save."), px, y() + h() - S(88), pw, S(18), t.text_dim);
    } else if (!app_->modified()) {
      text_font(t.font_size - S(1));
      draw_text(_("The file changes only when you save."), px, y() + h() - S(88), pw, S(18),
                t.text_dim);
    } else if (save_->active()) {
      text_font(t.font_size - S(1));
      bool jpeg = app_->current() && sniff_format(app_->current()->path) == Format::Jpeg;
      std::string note = jpeg && app_->edits().geometry_only()
                             ? _("Rotation is saved without loss of quality.")
                             : _("Saving encodes the photo again.");
      draw_text(note, px, y() + h() - S(88), pw, S(18), t.text_dim);
    }
  } else {
    fl_push_clip(x(), y(), w(), h());
    draw_info(nullptr);
    fl_pop_clip();
  }
  if (mode_ == Edit && tab_ == Draw) {
    // The sliders scroll with the tab: clip them to its area.
    for (Slider *sl : {draw_width_, draw_opacity_}) {
      if (!sl->visible()) continue;
      fl_push_clip(x(), draw_top(), w(), draw_bottom() - draw_top());
      draw_child(*sl);
      fl_pop_clip();
    }
    for (int i = 0; i < children(); ++i) {
      Fl_Widget *c = child(i);
      if (c != draw_width_ && c != draw_opacity_ && c->visible()) draw_child(*c);
    }
    return;
  }
  draw_children();
}

int SidePanel::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (event) {
    case FL_PUSH:
      if (mode_ == Edit && my >= y() + S(14) && my < y() + S(48) && mx >= x() + S(20) && mx < x() + w() - S(20)) {
        int seg = (mx - x() - S(23)) * 4 / std::max(1, w() - S(46));
        Tab nt = Tab(std::max(0, std::min(3, seg)));
        if (nt == Draw) app_->open_draw();
        else set_tab(nt);
        return 1;
      }
      if (mode_ == Edit && tab_ == Draw && draw_tab_click(mx, my)) return 1;
      if (mode_ == Edit && tab_ == Filters) {
        int f = filter_at(mx, my);
        if (f >= 0) {
          Edits e = app_->edits();
          e.filter = filters()[size_t(f)].id;
          if (e.filter != Filter::Original && e.strength == 0) e.strength = 100;
          app_->apply(e);
          redraw();
          return 1;
        }
      }
      if (mode_ == Info && link_w_ > 0 && mx >= link_x_ && mx < link_x_ + link_w_ && my >= link_y_ &&
          my < link_y_ + link_h_) {
        app_->open_map();
        return 1;
      }
      break;
    case FL_MOVE:
    case FL_ENTER: {
      int f = filter_at(mx, my);
      if (f != filter_hover_) {
        filter_hover_ = f;
        redraw();
      }
      int dh = -1;
      if (mode_ == Edit && tab_ == Draw)
        for (const auto &b : draw_boxes_)
          if (b.in(mx, my) && my >= draw_top() && my < draw_bottom()) dh = b.id;
      if (dh != draw_hover_) {
        draw_hover_ = dh;
        redraw();
      }
      if (dh >= 0) f = 0;  // hand cursor
      bool on_link = mode_ == Info && link_w_ > 0 && mx >= link_x_ && mx < link_x_ + link_w_ && my >= link_y_ &&
                     my < link_y_ + link_h_;
      window()->cursor(on_link || f >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
      break;
    }
    case FL_LEAVE:
      window()->cursor(FL_CURSOR_DEFAULT);
      filter_hover_ = -1;
      redraw();
      break;
    case FL_MOUSEWHEEL:
      if (mode_ == Edit && tab_ == Draw) {
        double room = draw_bottom() - draw_top() - S(8);
        draw_scroll_ = std::max(0.0, std::min(std::max(0.0, draw_h_ - room), draw_scroll_ + Fl::event_dy() * S(40)));
        layout();
        redraw();
        return 1;
      }
      if (mode_ == Info) {
        info_scroll_ = std::max(0.0, std::min(std::max(0.0, info_h_ - h() + S(20)),
                                              info_scroll_ + Fl::event_dy() * S(40)));
        redraw();
        return 1;
      }
      break;
  }
  int r = Fl_Group::handle(event);
  if (event == FL_PUSH || event == FL_ENTER || event == FL_MOVE) return 1;
  return r;
}

}  // namespace photo
