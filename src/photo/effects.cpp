// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's editing plugins in the window: the Effects tab (Background Blur,
// Red-Eye Fix, Frames & Borders, Watermark), Auto Enhance (Adjust tab),
// sizes for the web and social media (Crop tab) and Metadata & Privacy
// (the info view). The image work is in finish.cpp and draw.cpp, the file
// work in gallery/metadata.cpp.
#include <FL/Fl.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "config.h"
#include "forms.h"
#include "i18n.h"
#include "menu.h"
#include "menubar.h"
#include "metadata.h"
#include "photo.h"
#include "util.h"

using namespace sde;
using gallery::ask;

namespace photo {

namespace {

enum FxId {
  FX_FOCUS = 1000,  // + FocusShape
  FX_REDEYE = 1010,
  FX_REDEYE_CLEAR,
  FX_FRAME = 1020,  // + FrameStyle
  FX_COLOR = 1040,  // + index into kFrameColors
  FX_COLOR_CUSTOM = 1060,
  FX_CORNER = 1070,  // + corner
  FX_WM_LAST = 1080,
};

const uint32_t kFrameColors[] = {0xFFFFFFFF, 0xFFF3EEE4, 0xFFD5D7DB, 0xFF3A3F47, 0xFF111111, 0xFF8C6A4F};
const char *const kFrameNames[] = {N_("None"), N_("Border"), N_("Mat"), N_("Rounded"), N_("Print"), N_("Shadow")};
const char *const kFocusNames[] = {N_("Off"), N_("Circle"), N_("Band")};

struct SizePreset {
  const char *name;
  int w, h;  // h == 0: w is the long side and the shape stays
};
const SizePreset kSizes[] = {
    {N_("4K"), 3840, 0},           {N_("Full HD"), 1920, 0},       {N_("Web"), 1600, 0},
    {N_("Email"), 1024, 0},        {N_("Square post"), 1080, 1080}, {N_("Portrait post"), 1080, 1350},
    {N_("Story"), 1080, 1920},     {N_("Widescreen"), 1920, 1080},
};

std::string size_text(const SizePreset &sp) {
  return sp.h ? format("%d × %d", sp.w, sp.h) : format(_("%d px on the long side"), sp.w);
}

int red_eye_count(const Edits &e) {
  int n = 0;
  for (const auto &m : e.marks.items)
    if (m.kind == MarkKind::RedEye) ++n;
  return n;
}

std::string default_signature() {
  std::string name;
  if (struct passwd *pw = getpwuid(getuid())) {
    if (pw->pw_gecos) name = trim(std::string(pw->pw_gecos).substr(0, std::string(pw->pw_gecos).find(',')));
    if (name.empty() && pw->pw_name) name = pw->pw_name;
  }
  time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  return format("© %s %d", name.c_str(), tm.tm_year + 1900);
}

// Writes |bytes| to |path| (atomically) and keeps its modification time:
// only the metadata changed, not the photo.
bool rewrite_keeping_time(const std::string &path, const std::string &bytes) {
  struct stat st {};
  bool have = stat(path.c_str(), &st) == 0;
  if (!write_file_atomic(path, bytes)) return false;
  if (have) {
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    utimensat(AT_FDCWD, path.c_str(), times, 0);
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------- the panel

bool SidePanel::effects_available() const {
  return app_->plugin_on("focusblur") || app_->plugin_on("redeye") || app_->plugin_on("frames") ||
         app_->plugin_on("watermark");
}

void SidePanel::layout_effects(int px, int pw, int top) {
  fx_boxes_.clear();
  fx_labels_.clear();
  if (!fx_blur_) {
    // The controls are made the first time the tab is shown.
    begin();
    auto slider = [this](double lo, double hi) {
      auto *s = new Slider(0, 0, 10, S(26));
      s->bounds(lo, hi);
      s->step(1);
      s->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
      s->callback([](Fl_Widget *, void *p) { static_cast<SidePanel *>(p)->effects_slider_changed(); }, this);
      s->hide();
      return s;
    };
    fx_blur_ = slider(5, 100);
    fx_focus_ = slider(5, 100);
    fx_frame_ = slider(1, 20);
    fx_wm_size_ = slider(2, 15);
    fx_wm_alpha_ = slider(10, 100);
    fx_text_ = new Fl_Input(0, 0, 10, S(32));
    fx_text_->when(FL_WHEN_CHANGED);
    fx_text_->callback([](Fl_Widget *, void *p) { static_cast<SidePanel *>(p)->watermark_typed(); }, this);
    fx_text_->hide();
    end();
  }
  const Theme &t = app_->theme();
  fx_text_->box(FL_FLAT_BOX);
  fx_text_->color(t.surface_alt.fl());
  fx_text_->textcolor(t.text.fl());
  fx_text_->selection_color(t.accent.fl());
  fx_text_->cursor_color(t.accent.fl());
  fx_text_->textsize(t.font_size);
  const Finish &f = app_->edits().finish;
  bool compact = draw_compact();
  if (compact) top -= S(4);
  int ct = draw_top(), cb = draw_bottom();
  double yy = top - fx_scroll_, gap = S(6), chip_h = compact ? S(30) : S(32);
  auto visible = [&](Fl_Widget *w) { return w->y() >= ct && w->y() + w->h() <= cb; };
  auto title = [&](const char *text) {
    fx_labels_.push_back({yy, text, 0, nullptr});
    yy += S(24);
  };
  auto place = [&](Slider *sl, const char *label, double value) {
    fx_labels_.push_back({yy, label, 1, sl});
    yy += S(20);
    sl->value(value);
    sl->resize(px, int(yy), pw, S(26));
    if (visible(sl)) sl->show();
    yy += S(26) + (compact ? S(6) : S(10));
  };
  auto chips = [&](int id0, int n, int cols) {
    double cw = (pw - (cols - 1) * gap) / cols;
    for (int i = 0; i < n; ++i)
      fx_boxes_.push_back({px + (i % cols) * (cw + gap), yy + (i / cols) * (chip_h + gap), cw, chip_h, id0 + i});
    yy += ((n + cols - 1) / cols) * (chip_h + gap) + S(4);
  };
  auto note = [&](const char *text) {
    fx_labels_.push_back({yy, text, 2, nullptr});
    yy += S(24);
  };
  if (app_->plugin_on("focusblur")) {
    title(N_("BACKGROUND BLUR"));
    chips(FX_FOCUS, 3, 3);
    if (f.focus != FocusShape::Off) {
      place(fx_blur_, N_("Blur"), f.blur);
      place(fx_focus_, f.focus == FocusShape::Band ? N_("Sharp band") : N_("Sharp area"), f.focus_size);
      if (!app_->red_eye_mode()) note(N_("Click the photo where it stays sharp."));
    }
    yy += S(8);
  }
  if (app_->plugin_on("redeye")) {
    title(N_("RED EYES"));
    int n = red_eye_count(app_->edits());
    if (n > 0) {
      double half = (pw - gap) / 2;
      fx_boxes_.push_back({double(px), yy, half, chip_h, FX_REDEYE});
      fx_boxes_.push_back({px + half + gap, yy, half, chip_h, FX_REDEYE_CLEAR});
    } else {
      fx_boxes_.push_back({double(px), yy, double(pw), chip_h, FX_REDEYE});
    }
    yy += chip_h + gap + S(2);
    if (app_->red_eye_mode()) note(N_("Click each red eye in the photo."));
    yy += S(8);
  }
  if (app_->plugin_on("frames")) {
    title(N_("FRAME"));
    chips(FX_FRAME, 6, 3);
    if (f.frame != FrameStyle::NoFrame) {
      place(fx_frame_, N_("Width"), f.frame_width);
      double sw = double(pw) / 7;
      for (int i = 0; i < 6; ++i) fx_boxes_.push_back({px + i * sw, yy, sw, double(S(30)), FX_COLOR + i});
      fx_boxes_.push_back({px + 6 * sw, yy, sw, double(S(30)), FX_COLOR_CUSTOM});
      yy += S(30) + S(10);
    }
    yy += S(8);
  }
  if (app_->plugin_on("watermark")) {
    title(N_("WATERMARK"));
    if (Fl::focus() != fx_text_) fx_text_->value(f.watermark.c_str());
    fx_text_->resize(px, int(yy), pw, S(32));
    if (visible(fx_text_)) fx_text_->show();
    yy += S(32) + gap + S(2);
    if (trim(f.watermark).empty()) {
      fx_boxes_.push_back({double(px), yy, double(pw), chip_h, FX_WM_LAST});
      yy += chip_h + gap;
    } else {
      chips(FX_CORNER, 5, 5);
      place(fx_wm_size_, N_("Size"), f.watermark_size);
      place(fx_wm_alpha_, N_("Opacity"), f.watermark_opacity);
    }
  }
  fx_h_ = yy + fx_scroll_ - top;
}

void SidePanel::draw_effects_tab(Painter *p) {
  const Theme &t = app_->theme();
  const Finish &f = app_->edits().finish;
  int px = x() + S(20), pw = w() - S(40);
  for (const auto &b : fx_boxes_) {
    bool hot = b.id == fx_hover_;
    if (b.id >= FX_COLOR && b.id <= FX_COLOR_CUSTOM) {
      if (!p) continue;
      double r = std::min(b.w, b.h) / 2 - Sd(3), cx = b.x + b.w / 2, cy = b.y + b.h / 2;
      bool custom = b.id == FX_COLOR_CUSTOM;
      bool known = false;
      for (uint32_t c : kFrameColors) known = known || (c & 0xFFFFFF) == (f.frame_color & 0xFFFFFF);
      bool on = custom ? !known : (kFrameColors[b.id - FX_COLOR] & 0xFFFFFF) == (f.frame_color & 0xFFFFFF);
      if (on) p->circle(cx, cy, r + Sd(2.5), t.accent);
      else if (hot) p->circle(cx, cy, r + Sd(2), t.border);
      p->circle(cx, cy, r, t.surface);
      if (custom) {
        for (int i = 0; i < 12; ++i) {
          double a0 = i * M_PI / 6, a1 = (i + 1) * M_PI / 6, h = i / 12.0;
          auto ch = [h](double off) {
            double k = std::fmod(h * 6 + off, 6);
            return std::max(0.0, std::min(1.0, std::min(k, 4 - k)));
          };
          cairo_move_to(p->cr(), cx, cy);
          cairo_arc(p->cr(), cx, cy, r - Sd(1.5), a0, a1 + 0.02);
          cairo_close_path(p->cr());
          cairo_set_source_rgb(p->cr(), ch(5), ch(3), ch(1));
          cairo_fill(p->cr());
        }
        p->circle(cx, cy, r * 0.45, !known ? Color::hex(f.frame_color & 0xFFFFFF) : t.surface);
      } else {
        uint32_t c = kFrameColors[b.id - FX_COLOR];
        p->circle(cx, cy, r - Sd(1.5), Color::hex(c & 0xFFFFFF));
        cairo_new_path(p->cr());
        cairo_arc(p->cr(), cx, cy, r - Sd(1.5), 0, 2 * M_PI);
        p->set(t.border);
        cairo_set_line_width(p->cr(), 1);
        cairo_stroke(p->cr());
      }
      continue;
    }
    bool on = false;
    std::string label;
    if (b.id >= FX_FOCUS && b.id < FX_FOCUS + 3) {
      on = int(f.focus) == b.id - FX_FOCUS;
      label = _(kFocusNames[b.id - FX_FOCUS]);
    } else if (b.id >= FX_FRAME && b.id < FX_FRAME + 6) {
      on = int(f.frame) == b.id - FX_FRAME;
      label = _(kFrameNames[b.id - FX_FRAME]);
    } else if (b.id == FX_REDEYE) {
      on = app_->red_eye_mode();
      label = on ? _("Done") : _("Fix Red Eyes…");
    } else if (b.id == FX_REDEYE_CLEAR) {
      label = format(_("Undo %d"), red_eye_count(app_->edits()));
    } else if (b.id == FX_WM_LAST) {
      label = format(_("Use “%s”"), app_->watermark_text().c_str());
    } else if (b.id >= FX_CORNER && b.id < FX_CORNER + 5) {
      on = f.watermark_corner == b.id - FX_CORNER;
    }
    if (p) {
      Color bg = on ? t.accent.alpha(t.dark ? 0.32 : 0.16) : hot ? t.hover : t.surface_alt;
      p->fill_rounded(b.x, b.y, b.w, b.h, Sd(9), bg);
      if (on) p->stroke_rounded(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, Sd(9), t.accent, Sd(1.2));
      if (b.id >= FX_CORNER && b.id < FX_CORNER + 5) {
        // A little picture with the text's place in it.
        double gw = std::min(b.w - S(12), double(S(26))), gh = gw * 0.7;
        double gx = b.x + (b.w - gw) / 2, gy = b.y + (b.h - gh) / 2;
        p->stroke_rounded(gx, gy, gw, gh, Sd(2.5), on ? t.accent : t.text_dim, Sd(1.3));
        int c = b.id - FX_CORNER;
        double dw = gw * 0.42, dh = Sd(3), m = Sd(3);
        double dx = c == 1 || c == 3 ? gx + gw - m - dw : c == 4 ? gx + (gw - dw) / 2 : gx + m;
        double dy = c == 2 || c == 3 ? gy + gh - m - dh : c == 4 ? gy + (gh - dh) / 2 : gy + m;
        p->fill_rounded(dx, dy, dw, dh, dh / 2, on ? t.accent : t.text);
      }
    } else if (!label.empty()) {
      text_font(t.font_size - S(1), on);
      draw_text(ellipsize(label, int(b.w) - S(12)), int(b.x), int(b.y), int(b.w), int(b.h),
                on ? t.accent_text : t.text, 1);
    }
  }
  if (p) return;
  for (const auto &l : fx_labels_) {
    switch (l.kind) {
      case 0:
        text_font(t.font_size - S(1), true);
        draw_text(_(l.text), px, int(l.y), pw, S(18), t.accent_text);
        break;
      case 1: {
        text_font(t.font_size - S(1));
        draw_text(_(l.text), px, int(l.y), pw, S(18), t.text);
        int v = int(std::lround(l.slider->value()));
        draw_text(l.slider == fx_blur_ ? std::to_string(v) : format("%d%%", v), px, int(l.y), pw, S(18), t.text_dim, 2);
        break;
      }
      default:
        text_font(t.font_size - S(1));
        draw_text(ellipsize(_(l.text), pw), px, int(l.y), pw, S(18), t.text_dim);
        break;
    }
  }
}

void SidePanel::draw_effects_hint() {
  // Over the empty text field (drawn after it): what Watermark would sign.
  if (!fx_text_ || !fx_text_->visible() || fx_text_->size() != 0 || Fl::focus() == fx_text_) return;
  const Theme &t = app_->theme();
  fl_push_clip(x(), draw_top(), w(), draw_bottom() - draw_top());
  text_font(t.font_size);
  draw_text(format(_("e.g. %s"), app_->watermark_text().c_str()), fx_text_->x() + S(6), fx_text_->y(),
            fx_text_->w() - S(12), fx_text_->h(), t.text_dim);
  fl_pop_clip();
}

int SidePanel::effects_click(int mx, int my) {
  if (my < draw_top() || my >= draw_bottom()) return 0;
  for (const auto &b : fx_boxes_) {
    if (!b.in(mx, my)) continue;
    Edits e = app_->edits();
    Finish &f = e.finish;
    bool changed = true;
    if (b.id >= FX_FOCUS && b.id < FX_FOCUS + 3) {
      f.focus = FocusShape(b.id - FX_FOCUS);
    } else if (b.id >= FX_FRAME && b.id < FX_FRAME + 6) {
      f.frame = FrameStyle(b.id - FX_FRAME);
    } else if (b.id >= FX_COLOR && b.id < FX_COLOR + 6) {
      f.frame_color = kFrameColors[b.id - FX_COLOR];
    } else if (b.id == FX_COLOR_CUSTOM) {
      uchar r = (f.frame_color >> 16) & 0xFF, g = (f.frame_color >> 8) & 0xFF, bl = f.frame_color & 0xFF;
      changed = fl_color_chooser(_("Frame Colour"), r, g, bl) != 0;
      f.frame_color = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | bl;
    } else if (b.id >= FX_CORNER && b.id < FX_CORNER + 5) {
      f.watermark_corner = b.id - FX_CORNER;
    } else if (b.id == FX_WM_LAST) {
      f.watermark = app_->watermark_text();
      fx_text_->value(f.watermark.c_str());
    } else if (b.id == FX_REDEYE) {
      app_->set_red_eye_mode(!app_->red_eye_mode());
      changed = false;
    } else if (b.id == FX_REDEYE_CLEAR) {
      int n = red_eye_count(e);
      e.marks.items.erase(std::remove_if(e.marks.items.begin(), e.marks.items.end(),
                                         [](const Mark &m) { return m.kind == MarkKind::RedEye; }),
                          e.marks.items.end());
      app_->toast(format(n == 1 ? _("%d red eye undone · Ctrl+Z brings it back")
                                : _("%d red eyes undone · Ctrl+Z brings them back"), n));
    }
    if (changed && e != app_->edits()) app_->apply(e);
    layout();
    app_->canvas()->redraw();
    redraw();
    return 1;
  }
  return 0;
}

void SidePanel::effects_slider_changed() {
  Edits e = app_->edits();
  Finish &f = e.finish;
  f.blur = fx_blur_->value();
  f.focus_size = fx_focus_->value();
  f.frame_width = fx_frame_->value();
  f.watermark_size = fx_wm_size_->value();
  f.watermark_opacity = fx_wm_alpha_->value();
  bool release = Fl::event() == FL_RELEASE;
  bool drag = Fl::event() == FL_DRAG || release;
  app_->apply(e, gesture_ && drag);  // one undo step per drag
  gesture_ = drag && !release;
  fx_typing_ = false;
  redraw();
}

void SidePanel::watermark_typed() {
  Edits e = app_->edits();
  std::string text = fx_text_->value();
  bool was_empty = trim(e.finish.watermark).empty();
  e.finish.watermark = text;
  // The letters of one word are one undo step.
  app_->apply(e, fx_typing_ && !was_empty);
  fx_typing_ = true;
  if (!trim(text).empty()) app_->remember_watermark(text);
  if (was_empty != trim(text).empty()) layout();
  redraw();
}

// ---------------------------------------------------------------- canvas

bool Canvas::content_rect(double &x, double &y, double &w, double &h) const {
  if (!image_) return false;
  image_rect(x, y, w, h);
  const Edits &e = app_->edits();
  if (e.has_frame() && !cropping_) {
    int cw, ch;
    content_size(e, app_->image_w(), app_->image_h(), cw, ch);
    double l, t, r, b;
    frame_insets(e.finish, cw, ch, l, t, r, b);
    double fw = w / (1 + l + r), fh = h / (1 + t + b);
    x += fw * l;
    y += fh * t;
    w = fw;
    h = fh;
  }
  return w > 1 && h > 1;
}

void Canvas::set_focus_point(int mx, int my, bool amend) {
  double cx, cy, cw, ch;
  if (!content_rect(cx, cy, cw, ch)) return;
  Edits e = app_->edits();
  e.finish.focus_x = std::max(0.0, std::min(1.0, (mx - cx) / cw));
  e.finish.focus_y = std::max(0.0, std::min(1.0, (my - cy) / ch));
  app_->apply(e, amend);
}

void Canvas::add_red_eye(int mx, int my) {
  double ix, iy, iw, ih, cx, cy, cw, ch;
  image_rect(ix, iy, iw, ih);
  if (!content_rect(cx, cy, cw, ch)) return;
  int sw = cairo_image_surface_get_width(image_), sh = cairo_image_surface_get_height(image_);
  double kx = sw / iw, ky = sh / ih;  // screen -> pixels of the picture shown
  // Pupils are small: look around the click, a few percent of the photo.
  double radius = std::max(6.0, std::min(cw * kx, ch * ky) * 0.05);
  Box red;
  if (!find_red_eye(image_, (mx - ix) * kx, (my - iy) * ky, radius, red)) {
    app_->toast(_("No red eye here · click the middle of a red pupil"));
    return;
  }
  // A circle a little larger than the red part, on the photo's pixels.
  double rx = (red.x0 + red.x1) / 2 / kx + ix, ry = (red.y0 + red.y1) / 2 / ky + iy;
  double rr = std::max(red.w() / kx, red.h() / ky) / 2 * 1.35 + 1;
  ViewMap v = screen_map();
  Pt a = v.to_image({rx - rr, ry - rr}), b = v.to_image({rx + rr, ry + rr});
  Edits e = app_->edits_for_marks();
  Mark m;
  m.kind = MarkKind::RedEye;
  m.pts = {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}};
  m.width = 1;
  e.marks.items.push_back(m);
  app_->apply(e);
  app_->toast(_("Red eye fixed · Ctrl+Z undoes it"));
}

int Canvas::effects_event(int event, int mx, int my) {
  if (!app_->effects_open() || !image_ || cropping_) return 0;
  const Edits &e = app_->edits();
  bool red = app_->red_eye_mode();
  bool focus = !red && e.finish.focus != FocusShape::Off && app_->plugin_on("focusblur");
  if (!red && !focus) return 0;
  double cx, cy, cw, ch;
  if (!content_rect(cx, cy, cw, ch)) return 0;
  bool inside = mx >= cx && mx < cx + cw && my >= cy && my < cy + ch;
  switch (event) {
    case FL_ENTER:
    case FL_MOVE:
      if (!inside) return 0;
      window()->cursor(FL_CURSOR_CROSS);
      return 1;
    case FL_PUSH:
      if (!inside || Fl::event_button() != FL_LEFT_MOUSE) return 0;
      if (red) {
        add_red_eye(mx, my);
        return 1;
      }
      focus_drag_ = true;
      set_focus_point(mx, my, false);
      return 1;
    case FL_DRAG:
      if (!focus_drag_) return 0;
      set_focus_point(mx, my, true);
      return 1;
    case FL_RELEASE:
      if (!focus_drag_) return 0;
      focus_drag_ = false;
      return 1;
  }
  return 0;
}

void Canvas::draw_effects_overlay(Painter &p) {
  if (!app_->effects_open() || !image_ || cropping_) return;
  const Edits &e = app_->edits();
  double cx, cy, cw, ch;
  if (!content_rect(cx, cy, cw, ch)) return;
  cairo_t *cr = p.cr();
  auto outline = [&](auto path) {
    // White dashes on a dark line: visible on any photo.
    cairo_set_line_width(cr, Sd(3));
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_set_dash(cr, nullptr, 0, 0);
    path();
    cairo_stroke(cr);
    double dash[] = {Sd(6), Sd(5)};
    cairo_set_dash(cr, dash, 2, 0);
    cairo_set_line_width(cr, Sd(1.5));
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    path();
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
  };
  cairo_save(cr);
  cairo_rectangle(cr, cx, cy, cw, ch);
  cairo_clip(cr);
  if (app_->red_eye_mode()) {
    ViewMap v = screen_map();
    for (const auto &m : e.marks.items) {
      if (m.kind != MarkKind::RedEye || m.pts.size() < 2) continue;
      Pt a = v.to_out(m.pts[0]), b = v.to_out(m.pts[1]);
      double r = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y)) / 2;
      outline([&] {
        cairo_new_path(cr);
        cairo_arc(cr, (a.x + b.x) / 2, (a.y + b.y) / 2, std::max(2.0, r), 0, 2 * M_PI);
      });
    }
  } else if (e.finish.focus != FocusShape::Off && app_->plugin_on("focusblur")) {
    double fx = cx + e.finish.focus_x * cw, fy = cy + e.finish.focus_y * ch;
    double size = std::max(5.0, std::min(100.0, e.finish.focus_size)) / 100;
    if (e.finish.focus == FocusShape::Circle) {
      double r = size * std::min(cw, ch) / 2;
      outline([&] {
        cairo_new_path(cr);
        cairo_arc(cr, fx, fy, r, 0, 2 * M_PI);
      });
    } else {
      double half = size * ch / 2;
      outline([&] {
        cairo_new_path(cr);
        cairo_move_to(cr, cx, fy - half);
        cairo_line_to(cr, cx + cw, fy - half);
        cairo_move_to(cr, cx, fy + half);
        cairo_line_to(cr, cx + cw, fy + half);
      });
    }
    // The point itself.
    p.circle(fx, fy, Sd(7), Color::hex(0x000000, 0.35));
    p.circle(fx, fy, Sd(5), Color::hex(0xFFFFFF));
  }
  cairo_restore(cr);
}

// ---------------------------------------------------------------- window

bool PhotoWindow::effects_open() const {
  return panel_->visible() && panel_->mode() == SidePanel::Edit && panel_->tab() == SidePanel::Effects &&
         source_ != nullptr;
}

void PhotoWindow::open_effects() {
  if (!source_ || !panel_->effects_available()) return;
  if (canvas_->cropping()) apply_crop();
  if (!panel_->visible() || panel_->mode() != SidePanel::Edit) toggle_panel(SidePanel::Edit);
  panel_->set_tab(SidePanel::Effects);
  canvas_->stop_animation();
  layout();
  redraw();
}

void PhotoWindow::set_red_eye_mode(bool on) {
  red_eye_mode_ = on && plugin_on("redeye");
  if (red_eye_mode_ && !effects_open()) open_effects();
  panel_->layout();
  panel_->redraw();
  canvas_->redraw();
  if (red_eye_mode_) toast(_("Click each red eye in the photo"));
}

void PhotoWindow::auto_enhance() {
  if (!source_ || !preview_base_) return;
  // The picture as it is framed, before any tone change.
  Edits g = edits();
  g.adjust = Adjust();
  g.filter = Filter::Original;
  g.marks = Marks();
  g.finish = Finish();
  double scale = double(cairo_image_surface_get_width(preview_base_)) / std::max(1, full_w_);
  cairo_surface_t *img = render(preview_base_, g, scale, kNoMarks);
  Adjust a = auto_adjust(img);
  cairo_surface_destroy(img);
  Edits e = edits();
  Adjust before = e.adjust;
  e.adjust.exposure = a.exposure;
  e.adjust.brightness = 0;
  e.adjust.contrast = a.contrast;
  e.adjust.saturation = a.saturation;
  e.adjust.warmth = a.warmth;
  if (e.adjust == before) {
    toast(a.identity() ? _("This photo looks good as it is") : _("Auto Enhance is already applied"));
    return;
  }
  apply(e);
  toast(_("Enhanced · the sliders show what changed, Ctrl+Z undoes it"));
}

void PhotoWindow::show_size_presets(Fl_Widget *anchor) {
  ContextMenu m;
  for (size_t i = 0; i < sizeof kSizes / sizeof kSizes[0]; ++i) {
    if (i == 4) m.separator();
    m.item(format("%s (%s)", _(kSizes[i].name), size_text(kSizes[i]).c_str()), [this, i] { apply_size_preset(int(i)); });
  }
  if (anchor) m.popup_below(anchor);
  else m.popup();
}

void PhotoWindow::apply_size_preset(int index) {
  if (!source_ || index < 0 || index >= int(sizeof kSizes / sizeof kSizes[0])) return;
  if (canvas_->cropping()) apply_crop();
  const SizePreset &sp = kSizes[index];
  Edits e = edits();
  e.resize_w = e.resize_h = 0;
  if (sp.h == 0) {
    // The long side, the same shape.
    int cw, ch;
    content_size(e, full_w_, full_h_, cw, ch);
    int longest = std::max(cw, ch);
    if (longest <= sp.w) {
      toast(format(_("The photo is %d × %d already: smaller than %d px"), cw, ch, sp.w));
      return;
    }
    double k = double(sp.w) / longest;
    e.resize_w = std::max(1, int(std::lround(cw * k)));
    e.resize_h = std::max(1, int(std::lround(ch * k)));
  } else {
    // The largest centred crop of the shape, then the size.
    int ow, oh;
    Edits whole = e;
    whole.crop = Rect();
    content_size(whole, full_w_, full_h_, ow, oh);
    double target = double(sp.w) / sp.h, have = double(ow) / std::max(1, oh);
    Rect r;
    if (have > target) {
      r.w = target / have;
      r.x = (1 - r.w) / 2;
    } else {
      r.h = have / target;
      r.y = (1 - r.h) / 2;
    }
    e.crop = r;
    int cw, ch;
    content_size(e, full_w_, full_h_, cw, ch);
    if (cw > sp.w || ch > sp.h) {
      e.resize_w = sp.w;
      e.resize_h = sp.h;
    }
  }
  apply(e);
  int ow, oh;
  content_size(e, full_w_, full_h_, ow, oh);
  if (sp.h && (ow != sp.w || oh != sp.h))
    toast(format(_("%s shape: %d × %d (the photo is smaller than %d × %d) · Crop… moves the frame"), _(sp.name), ow,
                 oh, sp.w, sp.h));
  else if (sp.h)
    toast(format(_("%s: %d × %d · Crop… moves the frame, Ctrl+Z undoes it"), _(sp.name), ow, oh));
  else
    toast(format(_("%s: %d × %d · Ctrl+Z undoes it"), _(sp.name), ow, oh));
}

std::string PhotoWindow::watermark_text() const { return watermark_.empty() ? default_signature() : watermark_; }

void PhotoWindow::remember_watermark(const std::string &text) {
  watermark_ = text;
  // Saved a moment after the typing stops.
  Fl::remove_timeout(save_watermark_cb, this);
  Fl::add_timeout(1.5, save_watermark_cb, this);
}

void PhotoWindow::save_watermark_cb(void *p) {
  auto *self = static_cast<PhotoWindow *>(p);
  if (instance_ != self) return;
  Config cfg;
  cfg.load();
  cfg.set("photo", "watermark", self->watermark_);
  cfg.save();
}

void PhotoWindow::remove_location() {
  const gallery::Photo *p = current();
  if (!p) return;
  std::string bytes;
  if (!read_file(p->path, bytes)) {
    toast(_("The photo could not be read"));
    return;
  }
  bool changed = false;
  Format f = sniff_format(p->path);
  bool ok = f == Format::Jpeg ? gallery::jpeg_remove_location(bytes, &changed)
            : f == Format::Png ? gallery::png_remove_location(bytes, &changed)
                               : false;
  if (!ok) {
    toast(_("Only JPEG and PNG photos can lose their location here"));
    return;
  }
  if (!changed) {
    toast(_("This photo has no location"));
    return;
  }
  if (ask(_("Remove Location"),
          format(_("Remove the location from “%s”?\nThe picture and the other camera details stay as they are."),
                 p->name.c_str()),
          {_("Cancel"), _("Remove")}) != 1)
    return;
  if (!rewrite_keeping_time(p->path, bytes)) {
    toast(_("The photo could not be written"));
    return;
  }
  info_ = gallery::PhotoInfo();
  gallery::read_photo_info(p->path, info_);
  panel_->info_changed();
  notify_gallery("changed", p->path);
  toast(format(_("The location was removed from “%s”"), p->name.c_str()));
}

void PhotoWindow::save_without_details() {
  const gallery::Photo *p = current();
  if (!p) return;
  if (modified()) {
    int r = ask(_("Save a Copy Without Details"),
                _("The copy is made from the saved photo.\nSave your edits first?"),
                {_("Cancel"), _("Without the Edits"), _("Save")});
    if (r == 0) return;
    if (r == 2 && !save(false)) return;
  }
  std::string bytes;
  if (!read_file(p->path, bytes)) {
    toast(_("The photo could not be read"));
    return;
  }
  Format f = sniff_format(p->path);
  bool ok = f == Format::Jpeg ? gallery::jpeg_strip_metadata(bytes) : f == Format::Png ? gallery::png_strip_metadata(bytes)
                                                                                       : false;
  if (!ok) {
    toast(_("Only JPEG and PNG photos can be copied without details here"));
    return;
  }
  std::string stem = p->name, ext;
  size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    ext = stem.substr(dot);
    stem = stem.substr(0, dot);
  }
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  chooser.title(_("Save a Copy Without Details"));
  chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
  chooser.directory(p->dir.c_str());
  chooser.preset_file((stem + " " + _("(no details)") + ext).c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  std::string dest = chooser.filename();
  if (dest == p->path) {
    // Replacing the photo itself: its time stays.
    if (!rewrite_keeping_time(dest, bytes)) {
      toast(_("The copy could not be written"));
      return;
    }
    info_ = gallery::PhotoInfo();
    gallery::read_photo_info(dest, info_);
    panel_->info_changed();
  } else if (!write_file_atomic(dest, bytes)) {
    toast(_("The copy could not be written"));
    return;
  }
  notify_gallery(dest == p->path ? "changed" : "added", dest);
  toast(format(_("Saved “%s” without camera details or location"), basename_of(dest).c_str()));
}

bool PhotoWindow::has_removable_details() {
  const gallery::Photo *p = current();
  if (!p) return false;
  struct stat st {};
  if (stat(p->path.c_str(), &st) != 0) return false;
  if (p->path != details_path_ || st.st_mtime != details_mtime_) {
    details_path_ = p->path;
    details_mtime_ = st.st_mtime;
    std::string bytes;
    details_removable_ = read_file(p->path, bytes) && gallery::has_removable_metadata(bytes);
  }
  return details_removable_;
}

void PhotoWindow::show_all_details() {
  const gallery::Photo *p = current();
  if (!p) return;
  std::string bytes;
  read_file(p->path, bytes);
  auto fields = gallery::list_metadata(bytes);
  std::vector<std::pair<std::string, std::string>> rows;
  std::string group;
  for (const auto &f : fields) {
    if (f.group != group) {
      group = f.group;
      rows.push_back({group, ""});
    }
    rows.push_back({f.name, f.value});
  }
  if (rows.empty()) rows.push_back({_("This photo carries no details besides its picture."), ""});
  gallery::show_fields(_("All Details"), p->name, rows);
}

}  // namespace photo
