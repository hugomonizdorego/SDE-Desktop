// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "osd.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "buffered_window.h"
#include "config.h"
#include "glyphs.h"
#include "i18n.h"
#include "paint.h"
#include "theme.h"
#include "x11.h"

namespace sde {
namespace power {

namespace {

Theme g_theme;
bool g_theme_loaded = false;

const Theme &theme() {
  if (!g_theme_loaded) osd_reload_theme();
  return g_theme;
}

double kRadius() { return Sd(14); }

// Rounded window corners with the SHAPE extension (works without a
// compositor): one rectangle per row near the top and bottom edges.
void round_corners(Window win, int w, int h, int r) {
  int ev, err;
  if (!XShapeQueryExtension(fl_display, &ev, &err)) return;
  std::vector<XRectangle> rows;
  for (int y = 0; y < h; ++y) {
    int dy = y < r ? r - y : (y >= h - r ? y - (h - r - 1) : 0);
    int inset = 0;
    if (dy > 0) inset = int(std::ceil(r - std::sqrt(double(r) * r - double(dy - 0.5) * (dy - 0.5))));
    rows.push_back(XRectangle{short(inset), short(y), static_cast<unsigned short>(std::max(0, w - 2 * inset)), 1});
  }
  XShapeCombineRectangles(fl_display, win, ShapeBounding, 0, 0, rows.data(), int(rows.size()), ShapeSet,
                          YXBanded);
}

class Osd : public BufferedWindow {
 public:
  Osd() : BufferedWindow(0, 0, S(280), S(76), "SDE OSD") {
    end();
    set_override();
    border(0);
    clear_visible_focus();
  }

  void set(OsdKind kind, int percent, const std::string &label) {
    kind_ = kind;
    percent_ = std::max(0, std::min(100, percent));
    label_ = label;
    redraw();
  }

  int handle(int) override { return 0; }  // never takes input

  void draw() override {
    const Theme &t = theme();
    int W = w(), H = h(), pad = S(16), icon = S(30);
    {
      Painter p(W, H);
      p.fill_rect(0, 0, W, H, t.surface);
      if (t.glass) p.glass_sheen(0, 0, W, H, kRadius(), t.dark);
      p.stroke_rounded(0.5, 0.5, W - 1, H - 1, kRadius(), t.border);
      double gy = (H - icon) / 2.0;
      if (kind_ == OsdKind::Brightness)
        glyph::sun(p, pad, gy, icon, t.text);
      else
        glyph::battery(p, pad, gy, icon, percent_, false, t.text, t.accent);
      int bx = pad + icon + S(14), bw = W - bx - pad - S(46), bh = S(6);
      int by = H / 2 + S(6);
      p.fill_rounded(bx, by, bw, bh, bh / 2.0, t.text.alpha(0.18));
      p.fill_rounded(bx, by, std::max<double>(bh, bw * percent_ / 100.0), bh, bh / 2.0, t.accent);
      p.flush();
    }
    int bx = pad + icon + S(14);
    std::string title = label_.empty() ? (kind_ == OsdKind::Brightness ? _("Brightness") : _("Battery"))
                                       : label_;
    text_font(t.font_size, true);
    draw_text(title, bx, S(10), W - bx - pad - S(46), H / 2 - S(6), t.text);
    text_font(t.font_size);
    draw_text(std::to_string(percent_) + "%", W - pad - S(44), 0, S(44), H, t.text, 2);
  }

 private:
  OsdKind kind_ = OsdKind::Brightness;
  int percent_ = 0;
  std::string label_;
};

std::unique_ptr<Osd> g_osd;

void hide_cb(void *) {
  if (g_osd) g_osd->hide();
}

}  // namespace

void osd_reload_theme() {
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme_loaded = true;
  if (g_osd) {
    g_osd->hide();
    g_osd.reset();
  }
}

void show_osd(OsdKind kind, int percent, const std::string &label, double seconds) {
  theme();
  if (!g_osd) g_osd.reset(new Osd());
  g_osd->set(kind, percent, label);
  // The screen with the pointer.
  int mx = 0, my = 0;
  Fl::get_mouse(mx, my);
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, Fl::screen_num(mx, my));
  int W = g_osd->w(), H = g_osd->h();
  g_osd->position(sx + (sw - W) / 2, sy + sh - H - std::max(S(96), sh / 7));
  if (!g_osd->shown()) {
    g_osd->show();
    if (fl_xid(g_osd.get())) {
      x11::set_window_type(fl_xid(g_osd.get()), "_NET_WM_WINDOW_TYPE_NOTIFICATION");
      x11::set_opacity(fl_xid(g_osd.get()), theme().glass ? theme().popup_opacity : 1.0);
      round_corners(fl_xid(g_osd.get()), g_osd->w(), g_osd->h(), int(kRadius()));
    }
  }
  if (fl_xid(g_osd.get())) XRaiseWindow(fl_display, fl_xid(g_osd.get()));
  Fl::remove_timeout(hide_cb);
  Fl::add_timeout(seconds, hide_cb);
}

}  // namespace power
}  // namespace sde
