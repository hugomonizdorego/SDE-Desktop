// SPDX-License-Identifier: GPL-3.0-or-later
// Control Center > Appearance > Preview (0.2.0 Alpha 3.1); see theme_preview.h.
#include "theme_preview.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "integration.h"
#include "menu.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "wm.h"

namespace sde {

std::string look_summary(const AppearanceLook &l) {
  std::string family = l.family;
  for (const auto &f : theme_families())
    if (f.id == l.family) family = std::string("SDE ") + _(f.label.c_str());
  const char *mode = l.mode == "dark" ? _("Dark") : l.mode == "auto" ? _("Auto") : _("Light");
  std::string accent = l.custom_accent.empty() ? theme_accent_label(l.accent) : l.custom_accent;
  std::string glass = !l.translucent || l.transparency <= 0
                          ? std::string(_("Opaque"))
                          : format(_("Transparency %d %%"), l.transparency);
  return family + " · " + mode + " · " + accent + " · " + glass;
}

// ------------------------------------------------------------ backdrop
//
// What is behind the drawn surfaces: |sharp| at (ox, oy) in window pixels,
// and a blurred copy at a quarter of its size for translucent surfaces
// (three box blurs: close to the compositor's Gaussian and dual Kawase).

namespace {

struct Backdrop {
  cairo_surface_t *sharp = nullptr;
  double ox = 0, oy = 0;
};

constexpr int kBlurDown = 4;

// A box blur of an RGB24 image in place, radius |r|, along x then y.
void box_blur(unsigned char *data, int w, int h, int stride, int r) {
  if (r < 1 || w < 2 || h < 2) return;
  std::vector<int> tmp(size_t(std::max(w, h)) * 3);
  for (int y = 0; y < h; ++y) {
    unsigned char *row = data + size_t(y) * stride;
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) {
        int sum = 0, n = 0;
        for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k, ++n) sum += row[k * 4 + c];
        tmp[size_t(x) * 3 + size_t(c)] = sum / n;
      }
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) row[x * 4 + c] = (unsigned char)tmp[size_t(x) * 3 + size_t(c)];
  }
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y)
      for (int c = 0; c < 3; ++c) {
        int sum = 0, n = 0;
        for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k, ++n) sum += data[size_t(k) * stride + x * 4 + c];
        tmp[size_t(y) * 3 + size_t(c)] = sum / n;
      }
    for (int y = 0; y < h; ++y)
      for (int c = 0; c < 3; ++c) data[size_t(y) * stride + x * 4 + c] = (unsigned char)tmp[size_t(y) * 3 + size_t(c)];
  }
}

// The blurred copy of |sharp| for blur |strength| (1-12) and, for Liquid
// Glass, the material's saturation and brightness. Cached: the slider of
// the preview changes only the tint.
cairo_surface_t *blurred(cairo_surface_t *sharp, int strength, double saturation, double brightness) {
  struct Key {
    cairo_surface_t *s;
    int strength;
    int sat, bright;
    bool operator<(const Key &o) const {
      return std::tie(s, strength, sat, bright) < std::tie(o.s, o.strength, o.sat, o.bright);
    }
  };
  static std::map<Key, cairo_surface_t *> cache;
  Key key{sharp, strength, int(std::lround(saturation * 100)), int(std::lround(brightness * 100))};
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  if (cache.size() > 6) {
    for (auto &e : cache) cairo_surface_destroy(e.second);
    cache.clear();
  }
  int sw = cairo_image_surface_get_width(sharp), sh = cairo_image_surface_get_height(sharp);
  int w = std::max(1, sw / kBlurDown), h = std::max(1, sh / kBlurDown);
  cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cairo_t *cr = cairo_create(small);
  cairo_scale(cr, double(w) / sw, double(h) / sh);
  cairo_set_source_surface(cr, sharp, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(small);
  unsigned char *data = cairo_image_surface_get_data(small);
  int stride = cairo_image_surface_get_stride(small);
  int r = std::max(1, (strength + 1) / 3);
  for (int pass = 0; pass < 3; ++pass) box_blur(data, w, h, stride, r);
  if (std::fabs(saturation - 1) > 0.01 || std::fabs(brightness - 1) > 0.01)
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        unsigned char *px = data + size_t(y) * stride + x * 4;  // B G R x
        double b = px[0] / 255.0, g = px[1] / 255.0, rr = px[2] / 255.0;
        double l = 0.2126 * rr + 0.7152 * g + 0.0722 * b;
        auto adj = [&](double c) {
          return (unsigned char)std::lround(std::max(0.0, std::min(1.0, (l + (c - l) * saturation) * brightness)) * 255);
        };
        px[0] = adj(b);
        px[1] = adj(g);
        px[2] = adj(rr);
      }
  cairo_surface_mark_dirty(small);
  cache[key] = small;
  return small;
}

// A dusky sky over sand with stripes and a sun (as the Transparency cards):
// a busy backdrop shows how much comes through when there is no wallpaper.
cairo_surface_t *sample_backdrop(int w, int h) {
  static std::map<std::pair<int, int>, cairo_surface_t *> cache;
  auto it = cache.find({w, h});
  if (it != cache.end()) return it->second;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, std::max(1, w), std::max(1, h));
  cairo_t *cr = cairo_create(s);
  cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, w, h);
  cairo_pattern_add_color_stop_rgb(g, 0, 0x8F / 255.0, 0xA9 / 255.0, 0xBF / 255.0);
  cairo_pattern_add_color_stop_rgb(g, 1, 0xCD / 255.0, 0xB7 / 255.0, 0x9A / 255.0);
  cairo_set_source(cr, g);
  cairo_paint(cr);
  cairo_pattern_destroy(g);
  for (int i = 0; i < 8; ++i) {
    cairo_rectangle(cr, i * w / 8.0, 0, w / 16.0, h);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
    cairo_fill(cr);
  }
  cairo_arc(cr, w * 0.70, h * 0.32, h * 0.22, 0, 2 * M_PI);
  cairo_set_source_rgb(cr, 0xE9 / 255.0, 0xD3 / 255.0, 0xA0 / 255.0);
  cairo_fill(cr);
  cairo_destroy(cr);
  cache[{w, h}] = s;
  return s;
}

// A surface of the panel, a popup or a window frame: what is behind it
// (blurred when the theme blurs), tinted with |fill| at |alpha|; Liquid
// Glass adds its light rim. Opaque themes simply fill.
void glass_layer(Painter &p, const Theme &t, const Backdrop &b, double x, double y, double w, double h, double r,
                 const Color &fill, double alpha, int corners = 15) {
  cairo_t *cr = p.cr();
  bool see_through = t.translucent && alpha < 0.999 && b.sharp;
  if (see_through && t.blur) {
    LiquidMaterial m = theme_liquid_material(t.family, t.dark);
    int strength = t.liquid ? std::max(1, std::min(12, m.blur)) : 6;
    cairo_surface_t *soft = blurred(b.sharp, strength, t.liquid ? m.saturation : 1.0, t.liquid ? m.brightness : 1.0);
    cairo_save(cr);
    p.rounded_corners(x, y, w, h, r, corners);
    cairo_clip(cr);
    cairo_translate(cr, b.ox, b.oy);
    cairo_scale(cr, kBlurDown, kBlurDown);
    cairo_set_source_surface(cr, soft, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);
  }
  cairo_save(cr);
  p.rounded_corners(x, y, w, h, r, corners);
  p.set(fill.alpha(see_through ? alpha : 1.0));
  cairo_fill(cr);
  cairo_restore(cr);
  if (t.liquid && see_through)
    p.glass_surface(x, y, w, h, r, t.border.alpha(0.5), t.dark, corners);
  else
    p.surface_edge(x, y, w, h, r, (t.family == "glass" ? t.border.alpha(0.6) : t.border), t.family == "glass" ? 0.6 : 0.0,
                   t.dark, corners);
}

void paint_backdrop(Painter &p, const Backdrop &b, double x, double y, double w, double h) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_rectangle(cr, x, y, w, h);
  cairo_clip(cr);
  cairo_set_source_surface(cr, b.sharp, b.ox, b.oy);
  cairo_paint(cr);
  cairo_restore(cr);
}

// The panel along the bottom of a scene (a floating pill in Liquid Glass).
void draw_panel(Painter &p, const Theme &t, const Backdrop &b, double x, double y, double w, double h) {
  bool pill = t.family == "glass";
  double px = pill ? x + w * 0.14 : x, pw = pill ? w * 0.72 : w, py = pill ? y - Sd(4) : y;
  glass_layer(p, t, b, px, py, pw, h, pill ? h / 2 : 0, t.surface, t.opacity, pill ? 15 : 0);
  double icon = h * 0.56, ix = px + (pill ? h * 0.5 : Sd(8)), iy = py + (h - icon) / 2;
  p.fill_rounded(ix, iy, icon * 1.6, icon, std::min(icon / 2, double(t.radius_small)), t.accent);
  ix += icon * 1.6 + Sd(8);
  p.fill_rounded(ix, iy, pw * 0.22, icon, icon / 2, t.surface_alt.alpha(0.9));
  ix += pw * 0.22 + Sd(10);
  for (int i = 0; i < 4; ++i) {
    p.fill_rounded(ix, iy, icon, icon, Sd(4), i == 1 ? t.accent.alpha(0.85) : t.text_dim.alpha(0.55));
    if (i == 1) p.fill_rounded(ix + icon * 0.25, py + h - Sd(3), icon * 0.5, Sd(2), Sd(1), t.accent);
    ix += icon + Sd(8);
  }
}

}  // namespace

void draw_look_scene(Painter &p, const Theme &t, double x, double y, double w, double h, double radius,
                     cairo_surface_t *backdrop, const Color &border) {
  cairo_t *cr = p.cr();
  Backdrop b;
  b.sharp = backdrop ? backdrop : sample_backdrop(int(w), int(h));
  int bw = cairo_image_surface_get_width(b.sharp), bh = cairo_image_surface_get_height(b.sharp);
  double scale = std::max(w / std::max(1, bw), h / std::max(1, bh));
  cairo_save(cr);
  p.rounded_rect(x, y, w, h, radius);
  cairo_clip(cr);
  // The wallpaper covers the scene; the blurred copy is drawn in the same
  // place (glass_layer() expects the backdrop at its own scale).
  cairo_save(cr);
  cairo_translate(cr, x + (w - bw * scale) / 2, y + (h - bh * scale) / 2);
  cairo_scale(cr, scale, scale);
  // From here on the units are backdrop pixels.
  paint_backdrop(p, b, 0, 0, bw, bh);
  double ph = std::max(Sd(9), h * 0.10) / scale;  // the panel's height
  // A window with its frame, the theme's corners and caption buttons.
  double ww = bw * 0.46, wh = bh * 0.64, wx = bw * 0.06, wy = bh * 0.08;
  double wr = std::max(2.0, t.radius * 0.6 / scale), th = std::max(ph * 1.2, bh * 0.12);
  p.shadow(wx, wy, ww, wh, wr, t.shadow.alpha(std::min(0.3, t.shadow.a)), int(std::max(2.0, Sd(5) / scale)));
  cairo_save(cr);
  p.rounded_rect(wx, wy, ww, wh, wr);
  cairo_clip(cr);
  p.fill_rect(wx, wy + th, ww, wh - th, t.background);
  p.fill_rect(wx, wy + th, ww * 0.3, wh - th, t.surface_alt);
  cairo_restore(cr);
  glass_layer(p, t, b, wx, wy, ww, th, wr, t.titlebar_bg, t.frame_opacity, 3);
  double cy = wy + th / 2, cr_ = th * 0.2;
  p.circle(wx + ww - th * 0.55, cy, cr_, t.caption_disc);
  p.stroke_rounded(wx + ww - th * 1.25 - cr_ * 0.6, cy - cr_ * 0.6, cr_ * 1.2, cr_ * 1.2, cr_ * 0.2, t.caption_glyph,
                   std::max(0.6, th * 0.04));
  p.line(wx + ww - th * 1.95 - cr_ * 0.6, cy, wx + ww - th * 1.95 + cr_ * 0.6, cy, t.caption_glyph,
         std::max(0.6, th * 0.04));
  for (int i = 0; i < 3; ++i)
    p.fill_rounded(wx + ww * 0.04, wy + th + wh * 0.10 + i * wh * 0.12, ww * 0.2, wh * 0.05, wh * 0.025,
                   i == 0 ? t.accent : t.text_dim.alpha(0.5));
  for (int i = 0; i < 3; ++i)
    p.fill_rounded(wx + ww * 0.36, wy + th + wh * 0.10 + i * wh * 0.11, ww * (i == 0 ? 0.42 : 0.52), wh * 0.045,
                   wh * 0.02, (i == 0 ? t.text : t.text_dim).alpha(0.7));
  p.fill_rounded(wx + ww * 0.36, wy + wh - wh * 0.18, ww * 0.22, wh * 0.10, std::min(wh * 0.05, double(t.radius_small)),
                 t.accent);
  // A menu over the window.
  double mx = wx + ww * 0.62, my = wy + wh * 0.40, mw = bw * 0.24, mh = bh * 0.36;
  p.shadow(mx, my, mw, mh, wr, t.shadow.alpha(std::min(0.3, t.shadow.a)), int(std::max(2.0, Sd(4) / scale)));
  glass_layer(p, t, b, mx, my, mw, mh, wr, t.menu_bg, std::min(t.popup_opacity, t.gtk_menu_alpha));
  p.fill_rounded(mx + mw * 0.05, my + mh * 0.38, mw * 0.9, mh * 0.22, std::min(mh * 0.11, double(t.radius_small)),
                 t.menu_hover);
  for (int i = 0; i < 3; ++i)
    p.fill_rounded(mx + mw * 0.12, my + mh * (0.17 + i * 0.32), mw * (i == 1 ? 0.5 : 0.62), mh * 0.07, mh * 0.035,
                   i == 1 ? t.menu_hover_text : t.menu_text.alpha(0.75));
  // A quick settings popup above the panel, on the right.
  double qx = bw * 0.62, qy = bh * 0.12, qw = bw * 0.33, qh = bh * 0.62;
  double qr = std::max(2.0, t.radius_large * 0.6 / scale);
  p.shadow(qx, qy, qw, qh, qr, t.shadow.alpha(std::min(0.3, t.shadow.a)), int(std::max(2.0, Sd(5) / scale)));
  glass_layer(p, t, b, qx, qy, qw, qh, qr, t.surface, t.popup_opacity);
  for (int r = 0; r < 2; ++r)
    for (int c = 0; c < 3; ++c) {
      double tw = (qw - qw * 0.16) / 3, tx = qx + qw * 0.06 + c * (tw + qw * 0.02), ty = qy + qh * (0.10 + r * 0.26);
      p.fill_rounded(tx, ty, tw, qh * 0.2, std::min(qh * 0.06, double(t.radius_small)),
                     (r == 0 && c < 2) ? t.accent : t.surface_alt.alpha(0.9));
    }
  double sy = qy + qh * 0.70;
  p.fill_rounded(qx + qw * 0.08, sy, qw * 0.84, qh * 0.04, qh * 0.02, t.text_dim.alpha(0.35));
  p.fill_rounded(qx + qw * 0.08, sy, qw * 0.5, qh * 0.04, qh * 0.02, t.accent);
  p.circle(qx + qw * 0.58, sy + qh * 0.02, qh * 0.06, Color::hex(0xFFFFFF));
  // The panel.
  draw_panel(p, t, b, 0, bh - ph, bw, ph);
  cairo_restore(cr);
  cairo_restore(cr);
  p.stroke_rounded(x + 0.5, y + 0.5, w - 1, h - 1, radius, border);
}

// ===================================================== the preview window

namespace {

constexpr long XEMBED_EMBEDDED_NOTIFY = 0, XEMBED_WINDOW_ACTIVATE = 1;

struct PreviewState {
  Config base;              // the user's configuration (never written)
  AppearanceLook look;      // what is shown
  Theme theme;
  cairo_surface_t *screen = nullptr;  // the screen as the window opened
  Window socket = None, plug = None;
  int helper_in = -1;       // the GTK helper's stdin
  pid_t helper = -1;
  std::string helper_theme, helper_css, helper_icons, css_path;
  std::string from_cc;      // input from the Control Center
} g_pv;

class PreviewWindow;
PreviewWindow *g_pv_window = nullptr;
Fl_Window *g_pv_fl_window = nullptr;  // the same window

Config look_config(const AppearanceLook &look) {
  Config c = g_pv.base;
  look.write(c);
  // Without a compositor nothing is translucent.
  if (lower(c.get("compositor", "mode", "auto")) == "off") c.set("aero", "enabled", "false");
  return c;
}

void say(const std::string &line) {
  std::string text = line + "\n";
  if (write(1, text.data(), text.size()) < 0) {
  }
}

// The picture of the screen (before the window is mapped).
cairo_surface_t *grab_screen() {
  Window root = RootWindow(fl_display, fl_screen);
  int w = DisplayWidth(fl_display, fl_screen), h = DisplayHeight(fl_display, fl_screen);
  XImage *img = XGetImage(fl_display, root, 0, 0, unsigned(w), unsigned(h), AllPlanes, ZPixmap);
  if (!img) return nullptr;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  bool direct = img->bits_per_pixel == 32 && img->red_mask == 0xFF0000 && img->green_mask == 0xFF00 &&
                img->blue_mask == 0xFF && img->byte_order == LSBFirst;
  for (int y = 0; y < h; ++y) {
    auto *row = reinterpret_cast<uint32_t *>(data + size_t(y) * stride);
    if (direct) {
      memcpy(row, img->data + size_t(y) * img->bytes_per_line, size_t(w) * 4);
      continue;
    }
    for (int x = 0; x < w; ++x) {
      unsigned long px = XGetPixel(img, x, y);
      auto ch = [&](unsigned long mask) {
        if (!mask) return 0u;
        int shift = __builtin_ctzl(mask);
        unsigned long max = mask >> shift;
        return unsigned(((px & mask) >> shift) * 255 / max);
      };
      row[x] = (ch(img->red_mask) << 16) | (ch(img->green_mask) << 8) | ch(img->blue_mask);
    }
  }
  XDestroyImage(img);
  cairo_surface_mark_dirty(s);
  return s;
}

// The GTK part: a child window that the helper's GtkPlug lives in (XEmbed).
class GtkArea : public Fl_Window {
 public:
  GtkArea(int x, int y, int w, int h) : Fl_Window(x, y, w, h) { box(FL_NO_BOX); }
  void draw() override {
    fl_color(g_pv.theme.background.fl());
    fl_rectf(0, 0, w(), h());
    if (g_pv.helper <= 0) {
      fl_color(g_pv.theme.text_dim.fl());
      fl_font(FL_HELVETICA, g_pv.theme.font_size);
      fl_draw(_("GTK applications follow the same theme (the GTK preview is not installed)."), S(12), 0, w() - S(24),
              h(), FL_ALIGN_CENTER | FL_ALIGN_WRAP);
    }
  }
  void resize(int x, int y, int w, int h) override {
    Fl_Window::resize(x, y, w, h);
    if (g_pv.plug != None) XResizeWindow(fl_display, g_pv.plug, unsigned(w), unsigned(h));
  }
};

void send_xembed(Window to, long message, long detail, long data1) {
  XEvent e{};
  e.xclient.type = ClientMessage;
  e.xclient.window = to;
  e.xclient.message_type = XInternAtom(fl_display, "_XEMBED", False);
  e.xclient.format = 32;
  e.xclient.data.l[0] = CurrentTime;
  e.xclient.data.l[1] = message;
  e.xclient.data.l[2] = detail;
  e.xclient.data.l[3] = data1;
  e.xclient.data.l[4] = 0;
  XSendEvent(fl_display, to, False, NoEventMask, &e);
}

// The XEmbed embedder's part: the plug is resized to the area, told that it
// is embedded and active (GTK draws its widgets as in a focused window) and
// mapped.
int xembed_events(void *event, void *) {
  auto *e = static_cast<XEvent *>(event);
  if (g_pv.socket == None) return 0;
  Window w = None;
  if (e->type == CreateNotify && e->xcreatewindow.parent == g_pv.socket) w = e->xcreatewindow.window;
  if (e->type == ReparentNotify && e->xreparent.parent == g_pv.socket) w = e->xreparent.window;
  if (w != None && w != g_pv.plug) {
    g_pv.plug = w;
    XWindowAttributes a;
    XGetWindowAttributes(fl_display, g_pv.socket, &a);
    XSelectInput(fl_display, w, StructureNotifyMask | PropertyChangeMask);
    XResizeWindow(fl_display, w, unsigned(a.width), unsigned(a.height));
    send_xembed(w, XEMBED_EMBEDDED_NOTIFY, 0, long(g_pv.socket));
    send_xembed(w, XEMBED_WINDOW_ACTIVATE, 0, 0);
    XMapWindow(fl_display, w);
    XFlush(fl_display);
    if (g_pv_fl_window) g_pv_fl_window->redraw();
  }
  if (e->type == DestroyNotify && e->xdestroywindow.window == g_pv.plug) g_pv.plug = None;
  return 0;
}

std::string helper_path() {
  std::string forced = env("SDE_THEME_PREVIEW_GTK");
  if (!forced.empty()) return forced;
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n > 0) {
    self[n] = 0;
    std::string dir = std::string(self).substr(0, std::string(self).rfind('/'));
    if (is_executable(dir + "/sde-theme-preview-gtk")) return dir + "/sde-theme-preview-gtk";
  }
  return std::string(SDE_LIBEXECDIR) + "/sde/sde-theme-preview-gtk";
}

void helper_say(const std::string &line) {
  if (g_pv.helper_in < 0) return;
  std::string text = line + "\n";
  if (write(g_pv.helper_in, text.data(), text.size()) < 0) {
  }
}

// The GTK theme, icon theme and CSS of the shown look, for the helper.
void update_helper() {
  Config c = look_config(g_pv.look);
  std::string theme = gtk_theme_name(c), icons = gtk_icon_theme_name(c, g_pv.theme.dark);
  std::string css = gtk_user_css(c, 3);
  if (theme != g_pv.helper_theme) helper_say("theme " + theme + (g_pv.theme.dark ? " dark" : " light"));
  if (icons != g_pv.helper_icons) helper_say("icons " + icons);
  if (css != g_pv.helper_css) {
    write_file_atomic(g_pv.css_path, css);
    helper_say("css " + g_pv.css_path);
  }
  g_pv.helper_theme = theme;
  g_pv.helper_icons = icons;
  g_pv.helper_css = css;
}

void start_helper() {
  std::string path = helper_path();
  if (!is_executable(path) || g_pv.socket == None) return;
  g_pv.css_path = runtime_dir() + "/sde-theme-preview-" + std::to_string(getpid()) + ".css";
  Config c = look_config(g_pv.look);
  g_pv.helper_theme = gtk_theme_name(c);
  g_pv.helper_icons = gtk_icon_theme_name(c, g_pv.theme.dark);
  g_pv.helper_css = gtk_user_css(c, 3);
  write_file_atomic(g_pv.css_path, g_pv.helper_css);
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return;
  std::string socket = std::to_string(g_pv.socket);
  // The texts of the GTK widgets, from SDE's catalog (see the helper).
  std::string labels;
  for (const char *l : {_("Files"), _("Recent"), _("Shared"), _("Search files"), _("Documents"), _("Pictures"),
                        _("Music"), _("Downloads"), _("Show hidden files"), _("Thumbnails"), _("Zoom"),
                        _("Open With..."), _("Rename..."), _("Properties"), _("More"), _("Cancel"), _("Save")})
    labels += std::string(labels.empty() ? "" : "|") + l;
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[0], 0);
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) dup2(null, 1);
    execl(path.c_str(), path.c_str(), "--socket", socket.c_str(), "--theme", g_pv.helper_theme.c_str(),
          "--icons", g_pv.helper_icons.c_str(), "--css", g_pv.css_path.c_str(),
          g_pv.theme.dark ? "--dark" : "--light", "--labels", labels.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  close(fds[0]);
  if (pid < 0) {
    close(fds[1]);
    return;
  }
  g_pv.helper = pid;
  g_pv.helper_in = fds[1];
  fcntl(g_pv.helper_in, F_SETFL, O_NONBLOCK);
}

void stop_helper() {
  if (g_pv.helper > 0) {
    helper_say("quit");
    close(g_pv.helper_in);
    g_pv.helper_in = -1;
    for (int i = 0; i < 20 && waitpid(g_pv.helper, nullptr, WNOHANG) == 0; ++i) usleep(20000);
    if (waitpid(g_pv.helper, nullptr, WNOHANG) == 0) {
      kill(g_pv.helper, SIGTERM);
      waitpid(g_pv.helper, nullptr, 0);
    }
    g_pv.helper = -1;
  }
  if (!g_pv.css_path.empty()) unlink(g_pv.css_path.c_str());
}

void close_preview(const char *what) {
  stop_helper();
  if (g_pv_fl_window) {
    // Gone from the screen before the Control Center applies the look (the
    // session takes its picture of the screen then).
    g_pv_fl_window->hide();
    XSync(fl_display, False);
  }
  if (std::string(what) == "apply")
    say("apply " + g_pv.look.encode());
  else
    say(what);
  exit(0);
}

class PreviewWindow : public BufferedWindow {
 public:
  PreviewWindow(int w, int h) : BufferedWindow(w, h, _("Appearance Preview")) {
    int bar = S(64), pad = S(18);
    scene_h_ = h - bar;
    // The GTK application window: the frame is drawn, the content is GTK.
    int aw = int(w * 0.52), ah = scene_h_ - S(44) - pad - S(12);
    app_ = {pad, pad, aw, ah};
    gtk_ = new GtkArea(app_.x, app_.y + title_h(), app_.w, app_.h - title_h());
    gtk_->end();
    // Quick settings with real SDE widgets.
    int qw = std::min(S(290), w - aw - 3 * pad), qx = w - pad - qw, qy = pad + S(14);
    quick_ = {qx, qy, qw, S(232)};
    int lx = qx + S(16), lw = qw - S(32);
    auto *wifi = new Switch(qx + qw - S(16) - S(46), qy + S(54), S(46), S(26));
    wifi->value(1);
    wifi->tooltip(_("Wi-Fi switch (preview)"));
    auto *night = new Switch(qx + qw - S(16) - S(46), qy + S(90), S(46), S(26));
    night->tooltip(_("Night light switch (preview)"));
    auto *volume = new Slider(lx, qy + S(132), lw, S(28));
    volume->bounds(0, 100);
    volume->value(64);
    volume->tooltip(_("Volume slider (preview)"));
    auto *primary = new FlatButton(lx, qy + S(178), (lw - S(8)) / 2, S(34), _("Connect"), true);
    primary->tooltip(_("Primary button (preview)"));
    auto *menu = new FlatButton(lx + (lw + S(8)) / 2, qy + S(178), (lw - S(8)) / 2, S(34), _("Menu..."));
    menu->tooltip(_("Open a menu (preview)"));
    menu->callback([](Fl_Widget *wg, void *) {
      ContextMenu m;
      m.item(_("Open"), [] {}, {"open"});
      m.item(_("Rename..."), [] {}, {"rename"});
      m.check(_("Show Hidden Files"), true, [] {});
      m.separator();
      m.item(_("Properties"), [] {}, {"properties"});
      m.popup_below(wg);
    });
    // The bar: what is shown, the Transparency slider, Cancel and Apply.
    int by = h - bar + (bar - S(36)) / 2;
    apply_ = new FlatButton(w - pad - S(110), by, S(110), S(36), _("Apply"), true);
    apply_->tooltip(_("Apply this look"));
    apply_->shortcut(FL_Enter);
    apply_->callback([](Fl_Widget *, void *) { close_preview("apply"); });
    cancel_ = new FlatButton(w - pad - S(110) - S(8) - S(100), by, S(100), S(36), _("Cancel"));
    cancel_->tooltip(_("Close the preview without applying"));
    cancel_->callback([](Fl_Widget *, void *) { close_preview("cancel"); });
    int sw = std::min(S(220), w / 4), sx = w - pad - S(230) - S(16) - sw;
    text_w_ = sx - pad - S(16);
    slider_ = new Slider(sx, by + S(4), sw, S(28));
    slider_->bounds(0, 100);
    slider_->when(FL_WHEN_CHANGED);
    slider_->tooltip(_("Transparency level (preview)"));
    slider_->callback([](Fl_Widget *wg, void *) {
      AppearanceLook l = g_pv.look;
      l.transparency = int(std::lround(static_cast<Slider *>(wg)->value()));
      if (l.transparency > 0) l.translucent = true;
      show_look(l);
      say("look " + l.encode());
    });
    end();
    callback([](Fl_Widget *, void *) { close_preview("cancel"); });  // Escape, the close button
    sync_controls();
  }
  int title_h() const { return S(34); }
  static void show_look(const AppearanceLook &look) {
    g_pv.look = look;
    Config c = look_config(look);
    g_pv.theme = Theme::from_config(c);
    g_pv.theme.apply_to_fltk();
    if (g_pv_window) {
      g_pv_window->sync_controls();
      g_pv_window->redraw();
    }
    update_helper();
  }
  void sync_controls() {
    if (Fl::pushed() != slider_) slider_->value(g_pv.look.translucent ? g_pv.look.transparency : 0);
  }
  void resize(int x, int y, int w, int h) override {
    bool moved = x != this->x() || y != this->y();
    BufferedWindow::resize(x, y, w, h);
    if (moved) redraw();  // the screen behind it is another part
  }
  void draw() override {
    const Theme &t = g_pv.theme;
    {
      Painter p(w(), h());
      Backdrop b;
      b.sharp = g_pv.screen ? g_pv.screen : sample_backdrop(w(), scene_h_);
      if (g_pv.screen && shown()) {
        // Where the window's client area is on the screen.
        Window child;
        int rx = 0, ry = 0;
        XTranslateCoordinates(fl_display, fl_xid(this), RootWindow(fl_display, fl_screen), 0, 0, &rx, &ry, &child);
        b.ox = -rx;
        b.oy = -ry;
      }
      paint_backdrop(p, b, 0, 0, w(), scene_h_);
      // The application window: its frame in the theme (title bar with the
      // frame opacity), its content from GTK.
      p.shadow(app_.x, app_.y, app_.w, app_.h, t.radius, t.shadow.alpha(std::min(0.35, t.shadow.a)), int(Sd(10)));
      glass_layer(p, t, b, app_.x, app_.y, app_.w, title_h(), t.radius, t.titlebar_bg, t.frame_opacity, 3);
      double cy = app_.y + title_h() / 2.0, c = app_.x + app_.w - Sd(18);
      p.circle(c, cy, Sd(9), t.caption_disc);
      p.line(c - Sd(3.2), cy - Sd(3.2), c + Sd(3.2), cy + Sd(3.2), t.titlebar_bg, Sd(1.4));
      p.line(c - Sd(3.2), cy + Sd(3.2), c + Sd(3.2), cy - Sd(3.2), t.titlebar_bg, Sd(1.4));
      c -= Sd(30);
      p.stroke_rounded(c - Sd(4.5), cy - Sd(4.5), Sd(9), Sd(9), Sd(1.5), t.caption_glyph, Sd(1.3));
      c -= Sd(30);
      p.line(c - Sd(4.5), cy, c + Sd(4.5), cy, t.caption_glyph, Sd(1.3));
      // Quick settings: the popup surface of the panel.
      p.shadow(quick_.x, quick_.y, quick_.w, quick_.h, t.radius_large, t.shadow.alpha(std::min(0.35, t.shadow.a)),
               int(Sd(10)));
      glass_layer(p, t, b, quick_.x, quick_.y, quick_.w, quick_.h, t.radius_large, t.surface, t.popup_opacity);
      // The panel along the bottom of the scene.
      draw_panel(p, t, b, 0, scene_h_ - S(44), w(), S(44));
      // The bar.
      p.fill_rect(0, scene_h_, w(), h() - scene_h_, t.background);
      p.line(0, scene_h_ + 0.5, w(), scene_h_ + 0.5, t.border);
    }
    // Texts on the drawn surfaces.
    text_font(t.font_size, true);
    draw_text(_("Files"), app_.x + S(14), app_.y, app_.w - S(120), title_h(), t.titlebar_text);
    draw_text(_("Quick settings"), quick_.x + S(16), quick_.y + S(12), quick_.w - S(32), S(26), t.text);
    text_font(t.font_size);
    draw_text(_("Wi-Fi"), quick_.x + S(16), quick_.y + S(54), quick_.w - S(90), S(26), t.text);
    draw_text(_("Night light"), quick_.x + S(16), quick_.y + S(90), quick_.w - S(90), S(26), t.text);
    draw_text("10:30", w() - S(70), scene_h_ - S(44), S(60), S(44), t.text, 2);
    text_font(t.font_size, true);
    draw_text(_("Preview of the chosen look"), S(18), scene_h_ + S(8), text_w_, S(24), t.text);
    text_font(std::max(S(9), t.font_size - S(1)));
    draw_text(look_summary(g_pv.look), S(18), scene_h_ + S(30), text_w_, S(22), t.text_dim);
    draw_children();
    qa_dump();
  }
  // UI test hook, as the Control Center's: with SDE_PANEL_QA_DIR set,
  // <dir>/Theme-Preview.txt lists the named controls ("name x y w h
  // active"), whether the GTK part is embedded and the look shown.
  void qa_dump() {
    const char *dir = getenv("SDE_PANEL_QA_DIR");
    if (!dir || !*dir) return;
    std::string text = "@look:" + g_pv.look.encode() + "\t0\t0\t0\t0\t1\n";
    text += std::string("@gtk:") + (g_pv.plug != None ? "embedded" : g_pv.helper > 0 ? "starting" : "none") +
            "\t" + std::to_string(gtk_->x()) + "\t" + std::to_string(gtk_->y()) + "\t" + std::to_string(gtk_->w()) +
            "\t" + std::to_string(gtk_->h()) + "\t1\n";
    for (int i = 0; i < children(); ++i) {
      Fl_Widget *c = child(i);
      if (!c->visible() || !c->tooltip()) continue;
      text += std::string(c->tooltip()) + "\t" + std::to_string(c->x()) + "\t" + std::to_string(c->y()) + "\t" +
              std::to_string(c->w()) + "\t" + std::to_string(c->h()) + "\t" + (c->active_r() ? "1" : "0") + "\n";
    }
    std::string path = std::string(dir) + "/Theme-Preview.txt";
    write_file_atomic(path, text);
  }
  int handle(int e) override {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
      close_preview("cancel");
      return 1;
    }
    return BufferedWindow::handle(e);
  }
  GtkArea *gtk_area() { return gtk_; }

 private:
  struct Box {
    int x, y, w, h;
  } app_{}, quick_{};
  int scene_h_ = 0, text_w_ = 0;
  GtkArea *gtk_ = nullptr;
  FlatButton *apply_ = nullptr, *cancel_ = nullptr;
  Slider *slider_ = nullptr;
};

// Lines from the Control Center: "look ENCODED" (the chosen look changed),
// "raise" (Preview pressed again), "quit".
void from_control_center(FL_SOCKET fd, void *) {
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n <= 0) {
    close_preview("cancel");  // the Control Center went away
    return;
  }
  g_pv.from_cc.append(buf, size_t(n));
  std::string last_look;
  size_t nl;
  while ((nl = g_pv.from_cc.find('\n')) != std::string::npos) {
    std::string line = g_pv.from_cc.substr(0, nl);
    g_pv.from_cc.erase(0, nl + 1);
    if (starts_with(line, "look ")) last_look = line.substr(5);  // only the newest counts
    else if (line == "raise" && g_pv_window) g_pv_window->show();
    else if (line == "quit") close_preview("closed");
  }
  AppearanceLook look;
  if (!last_look.empty() && AppearanceLook::decode(last_look, look) && look != g_pv.look)
    PreviewWindow::show_look(look);
}

}  // namespace

int run_theme_preview(const std::string &encoded) {
  signal(SIGPIPE, SIG_IGN);
  fl_open_display();
  g_pv.base.load();
  if (!AppearanceLook::decode(encoded, g_pv.look)) g_pv.look = AppearanceLook::from_config(g_pv.base);
  g_pv.theme = Theme::from_config(look_config(g_pv.look));
  g_pv.theme.apply_to_fltk();
  IconLoader::set_theme(g_pv.base.get("appearance", "icon_theme", "auto"), g_pv.theme.dark);
  Fl::visible_focus(1);
  // The screen as it is now, before the window covers part of it.
  g_pv.screen = grab_screen();
  int sw = DisplayWidth(fl_display, fl_screen), sh = DisplayHeight(fl_display, fl_screen);
  int W = std::min(S(820), sw - S(60)), H = std::min(S(600), sh - S(90));
  int x = 0, y = 0;
  fit_to_work_area(x, y, W, H);
  auto *win = new PreviewWindow(W, H);
  g_pv_window = win;
  g_pv_fl_window = win;
  win->xclass("sde-settings");
  win->position(x, y);
  Fl::add_system_handler(xembed_events, nullptr);
  Fl::add_fd(0, FL_READ, from_control_center);
  win->show();
  while (!win->shown() || !fl_xid(win->gtk_area())) Fl::wait(0.05);
  g_pv.socket = fl_xid(win->gtk_area());
  XWindowAttributes a;
  XGetWindowAttributes(fl_display, g_pv.socket, &a);
  XSelectInput(fl_display, g_pv.socket, a.your_event_mask | SubstructureNotifyMask);
  start_helper();
  win->gtk_area()->redraw();
  say("ready");
  return Fl::run();
}

// ====================================================== the Control Center

ThemePreview &ThemePreview::get() {
  static ThemePreview preview;
  return preview;
}

bool ThemePreview::open(const AppearanceLook &look, Handler handler) {
  handler_ = std::move(handler);
  if (is_open()) {
    update(look);
    std::string raise = "raise\n";
    if (write(to_, raise.data(), raise.size()) < 0) {
    }
    return true;
  }
  int in[2], out[2];
  if (pipe2(in, O_CLOEXEC) != 0) return false;
  if (pipe2(out, O_CLOEXEC) != 0) {
    ::close(in[0]);
    ::close(in[1]);
    return false;
  }
  std::string text = look.encode();
  pid_t pid = fork();
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(out[1], 1);
    execl("/proc/self/exe", "sde-settings", "--theme-preview", text.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  ::close(in[0]);
  ::close(out[1]);
  if (pid < 0) {
    ::close(in[1]);
    ::close(out[0]);
    return false;
  }
  pid_ = pid;
  to_ = in[1];
  from_ = out[0];
  fcntl(to_, F_SETFL, O_NONBLOCK);
  last_sent_ = text;
  buffer_.clear();
  Fl::add_fd(from_, FL_READ, readable, this);
  return true;
}

void ThemePreview::update(const AppearanceLook &look) {
  if (!is_open()) return;
  std::string text = look.encode();
  if (text == last_sent_) return;
  last_sent_ = text;
  std::string line = "look " + text + "\n";
  if (write(to_, line.data(), line.size()) < 0) {
  }
}

void ThemePreview::close() {
  if (!is_open()) return;
  std::string line = "quit\n";
  if (write(to_, line.data(), line.size()) < 0) {
  }
}

void ThemePreview::readable(int fd, void *data) {
  auto *self = static_cast<ThemePreview *>(data);
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n <= 0) {
    self->finished();
    return;
  }
  self->buffer_.append(buf, size_t(n));
  size_t nl;
  while ((nl = self->buffer_.find('\n')) != std::string::npos) {
    std::string line = self->buffer_.substr(0, nl);
    self->buffer_.erase(0, nl + 1);
    std::string what = line.substr(0, line.find(' '));
    AppearanceLook look;
    if (line.size() > what.size() + 1) AppearanceLook::decode(line.substr(what.size() + 1), look);
    if (what == "look") self->last_sent_ = look.encode();
    if (self->handler_ && (what == "look" || what == "apply" || what == "cancel" || what == "closed"))
      self->handler_(what, look);
  }
}

void ThemePreview::finished() {
  Fl::remove_fd(from_);
  ::close(from_);
  ::close(to_);
  from_ = to_ = -1;
  if (pid_ > 0) waitpid(pid_, nullptr, 0);
  pid_ = -1;
  last_sent_.clear();
  if (handler_) handler_("gone", AppearanceLook());
}

}  // namespace sde
