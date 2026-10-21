// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Photo's side panel: the photo information, and the editing tools
// (adjustments, filters, crop/rotate/resize).
#include <FL/Fl.H>
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
    auto *fn = new std::function<void(PhotoWindow *)>(crop[i].fn);  // lives as long as the button
    b->callback([](Fl_Widget *w, void *f) {
      auto *panel = static_cast<SidePanel *>(w->parent());
      (*static_cast<std::function<void(PhotoWindow *)> *>(f))(panel->app_);
    }, fn);
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
  tab_ = t;
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
      double seg = (tw - S(6)) / 3;
      p.fill_rounded(tx + S(3) + seg * tab_, ty + S(3), seg, th - S(6), (th - S(6)) / 2, t.accent);
      if (tab_ == Filters) draw_filters(p);
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
    double seg = (tw - S(6)) / 3;
    const char *names[] = {_("Adjust"), _("Filters"), _("Crop")};
    for (int i = 0; i < 3; ++i) {
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
        draw_text(_("Width and height in pixels; the proportions are kept."), px, res_apply_->y() + S(38), pw,
                  S(18), t.text_dim);
      } else {
        draw_text(_("ASPECT RATIO"), px, y() + S(64), pw, S(18), t.accent_text);
        text_font(t.font_size - S(1));
        draw_text(_("Drag the corners or the edges; drag inside to move."), px,
                  crop_buttons_[5]->y() + S(44), pw, S(18), t.text_dim);
      }
    }
    if (!app_->modified()) {
      text_font(t.font_size - S(1));
      draw_text(_("Edits are previewed; the file changes only when you save."), px, y() + h() - S(88), pw, S(18),
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
  draw_children();
}

int SidePanel::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (event) {
    case FL_PUSH:
      if (mode_ == Edit && my >= y() + S(14) && my < y() + S(48) && mx >= x() + S(20) && mx < x() + w() - S(20)) {
        int seg = (mx - x() - S(23)) * 3 / std::max(1, w() - S(46));
        set_tab(Tab(std::max(0, std::min(2, seg))));
        return 1;
      }
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
