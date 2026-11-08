// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "security.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xutil.h>
#include <cairo.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "buffered_window.h"
#include "config.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "paint.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"

namespace sde {

namespace {

Theme g_theme;

struct Choice {
  std::string label;
  std::vector<std::string> command;
};

// What Ctrl+Alt+Delete offers, in the order of Windows.
std::vector<Choice> choices() {
  std::vector<Choice> list;
  list.push_back({_("Lock"), {"sde-lock"}});
  // Switch User needs a display manager that keeps both sessions (LightDM).
  if (!find_program("dm-tool").empty() && !env("XDG_SEAT_PATH").empty())
    list.push_back({_("Switch User"), {"dm-tool", "switch-to-greeter"}});
  list.push_back({_("Log Out"), {"sde-session", "--logout"}});
  list.push_back({_("Change a Password"), {"sde-settings", "--page", "users"}});
  list.push_back({_("Task Manager"), {"sde-system-monitor"}});
  return list;
}

void run(const std::vector<std::string> &command) {
  if (!env("SDE_LOGOUT_DRY_RUN").empty()) {
    std::string line;
    for (const auto &a : command) line += (line.empty() ? "" : " ") + a;
    printf("%s\n", line.c_str());
    fflush(stdout);
    return;
  }
  spawn_detached(command);
}

// The screen as it was, soft and dark, behind the options.
cairo_surface_t *backdrop(int w, int h) {
  Window root = RootWindow(fl_display, fl_screen);
  XImage *img = XGetImage(fl_display, root, 0, 0, unsigned(w), unsigned(h), AllPlanes, ZPixmap);
  if (!img) return nullptr;
  int tw = std::max(1, w / 24), th = std::max(1, h / 24);
  cairo_surface_t *tiny = cairo_image_surface_create(CAIRO_FORMAT_RGB24, tw, th);
  unsigned char *data = cairo_image_surface_get_data(tiny);
  int stride = cairo_image_surface_get_stride(tiny);
  // Averages of 24x24 blocks: a cheap blur.
  for (int y = 0; y < th; ++y)
    for (int x = 0; x < tw; ++x) {
      unsigned long r = 0, g = 0, b = 0, n = 0;
      for (int yy = y * 24; yy < std::min(h, y * 24 + 24); yy += 4)
        for (int xx = x * 24; xx < std::min(w, x * 24 + 24); xx += 4) {
          unsigned long p = XGetPixel(img, xx, yy);
          r += (p >> 16) & 0xFF;
          g += (p >> 8) & 0xFF;
          b += p & 0xFF;
          ++n;
        }
      n = std::max(1ul, n);
      reinterpret_cast<uint32_t *>(data + y * stride)[x] = uint32_t(((r / n) << 16) | ((g / n) << 8) | (b / n));
    }
  XDestroyImage(img);
  cairo_surface_mark_dirty(tiny);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cairo_t *cr = cairo_create(out);
  cairo_scale(cr, double(w) / tw, double(h) / th);
  cairo_set_source_surface(cr, tiny, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(tiny);
  return out;
}

class SecurityScreen : public BufferedWindow {
 public:
  SecurityScreen(int x, int y, int w, int h) : BufferedWindow(x, y, w, h, "Security options"), list_(choices()) {}
  cairo_surface_t *back = nullptr;

  void draw() override {
    const Color ink = Color::hex(0xFFFFFF);
    {
      Painter p(w(), h());
      if (back) {
        cairo_set_source_surface(p.cr(), back, 0, 0);
        cairo_paint(p.cr());
      }
      p.fill_rect(0, 0, w(), h(), Color::hex(0x0B1220, back ? 0.72 : 1.0));
      for (int i = 0; i < count(); ++i) {
        Box b = item_box(i);
        if (i == hover_ || i == focus_)
          p.fill_rounded(b.x, b.y, b.w, b.h, g_theme.radius_small, Color::hex(0xFFFFFF, i == focus_ ? 0.16 : 0.1));
        if (i == focus_ && keyboard_)
          p.stroke_rounded(b.x + 1, b.y + 1, b.w - 2, b.h - 2, g_theme.radius_small, Color::hex(0xFFFFFF, 0.8), Sd(1.5));
      }
      Box c = cancel_box();
      p.fill_rounded(c.x, c.y, c.w, c.h, g_theme.radius_small, Color::hex(0xFFFFFF, hover_ == kCancel ? 0.28 : 0.18));
      if (focus_ == kCancel && keyboard_)
        p.stroke_rounded(c.x + 1, c.y + 1, c.w - 2, c.h - 2, g_theme.radius_small, Color::hex(0xFFFFFF, 0.8), Sd(1.5));
      Box pw = power_box();
      if (hover_ == kPower || focus_ == kPower)
        p.fill_rounded(pw.x, pw.y, pw.w, pw.h, g_theme.radius_small, Color::hex(0xFFFFFF, 0.16));
      glyph::power(p, pw.x + S(10), pw.y + S(10), pw.w - S(20), ink);
    }
    for (int i = 0; i < count(); ++i) {
      Box b = item_box(i);
      text_font(g_theme.font_size + 6, i == focus_ || i == hover_);
      draw_text(list_[size_t(i)].label, b.x + S(18), b.y, b.w - S(36), b.h, ink, 0);
    }
    Box c = cancel_box();
    text_font(g_theme.font_size + 2, true);
    draw_text(_("Cancel"), c.x, c.y, c.w, c.h, ink, 1);
  }

  int handle(int e) override {
    switch (e) {
      case FL_FOCUS:
      case FL_UNFOCUS:
        return 1;
      case FL_MOVE: {
        int i = hit(Fl::event_x(), Fl::event_y());
        if (i != hover_) {
          hover_ = i;
          keyboard_ = false;
          redraw();
        }
        return 1;
      }
      case FL_PUSH:
        return 1;
      case FL_RELEASE:
        if (Fl::event_button() == FL_LEFT_MOUSE) choose(hit(Fl::event_x(), Fl::event_y()));
        return 1;
      case FL_KEYBOARD:
      case FL_SHORTCUT: {
        int k = Fl::event_key(), n = count() + 2;  // the items, Cancel, power
        // Focus order: the items, Cancel, power.
        auto order = [&](int f) { return f == kCancel ? count() : f == kPower ? count() + 1 : f; };
        auto from = [&](int o) { return o == count() ? kCancel : o == count() + 1 ? kPower : o; };
        keyboard_ = true;
        if (k == FL_Escape) {
          finish();
        } else if (k == FL_Down || (k == FL_Tab && !Fl::event_shift())) {
          focus_ = from((order(focus_) + 1) % n);
        } else if (k == FL_Up || (k == FL_Tab && Fl::event_shift())) {
          focus_ = from((order(focus_) + n - 1) % n);
        } else if (k == FL_Enter || k == FL_KP_Enter || k == ' ') {
          choose(focus_);
          return 1;
        } else {
          return 1;
        }
        redraw();
        return 1;
      }
      default:
        return BufferedWindow::handle(e);
    }
  }

  void take_input() {
    // Ctrl+Alt+Delete: SDE WM holds the keyboard until Delete is let go.
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool kb = false, ptr = false;
    while (!(kb && ptr) && std::chrono::steady_clock::now() < end) {
      if (!kb) kb = XGrabKeyboard(fl_display, fl_xid(this), True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
      if (!ptr)
        ptr = XGrabPointer(fl_display, fl_xid(this), True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                           GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess;
      if (!(kb && ptr)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
    XSync(fl_display, False);
    Fl::focus(this);
    qa();
  }

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool in(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  static constexpr int kCancel = 100, kPower = 101;
  std::vector<Choice> list_;
  int hover_ = -1, focus_ = 0;
  bool keyboard_ = true;

  int count() const { return int(list_.size()); }
  Box item_box(int i) const {
    int iw = S(300), ih = S(46), total = count() * ih;
    return {(w() - iw) / 2, (h() - total) / 2 - S(40) + i * ih, iw, ih - S(4)};
  }
  Box cancel_box() const {
    Box last = item_box(count() - 1);
    return {(w() - S(160)) / 2, last.y + last.h + S(48), S(160), S(40)};
  }
  Box power_box() const { return {w() - S(84), h() - S(84), S(52), S(52)}; }
  int hit(int x, int y) const {
    for (int i = 0; i < count(); ++i)
      if (item_box(i).in(x, y)) return i;
    if (cancel_box().in(x, y)) return kCancel;
    if (power_box().in(x, y)) return kPower;
    return -1;
  }

  void choose(int i) {
    if (i == kCancel) {
      finish();
    } else if (i == kPower) {
      power_menu();
    } else if (i >= 0 && i < count()) {
      std::vector<std::string> cmd = list_[size_t(i)].command;
      finish(false);
      run(cmd);
      exit(0);
    }
  }

  void power_menu() {
    ContextMenu m;
    m.item(_("Sleep"), [this] { leave_with({"systemctl", "suspend"}); }, {"system-suspend"});
    m.item(_("Restart"), [this] { leave_with({"systemctl", "reboot"}); }, {"system-reboot"});
    m.item(_("Shut Down"), [this] { leave_with({"systemctl", "poweroff"}); }, {"system-shutdown"});
    Box pw = power_box();
    m.popup(pw.x - S(120), pw.y - S(130));
    take_input();
  }

  void leave_with(const std::vector<std::string> &cmd) {
    finish(false);
    run(cmd);
    exit(0);
  }

  void finish(bool quit = true) {
    XUngrabKeyboard(fl_display, CurrentTime);
    XUngrabPointer(fl_display, CurrentTime);
    XSync(fl_display, False);
    hide();
    if (quit) exit(0);
  }

  void qa() const {
    const char *dir = getenv("SDE_LOGOUT_QA_DIR");
    if (!dir || !*dir) return;
    std::string text;
    auto line = [&](const std::string &name, const Box &b) {
      text += name + "\t" + std::to_string(x() + b.x) + "\t" + std::to_string(y() + b.y) + "\t" + std::to_string(b.w) +
              "\t" + std::to_string(b.h) + "\n";
    };
    for (int i = 0; i < count(); ++i) line("item:" + list_[size_t(i)].label, item_box(i));
    line("cancel", cancel_box());
    line("power", power_box());
    std::string path = std::string(dir) + "/security.txt", tmp = path + ".tmp";
    if (FILE *f = fopen(tmp.c_str(), "w")) {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
      rename(tmp.c_str(), path.c_str());
    }
  }
};

}  // namespace

int run_security_screen() {
  {
    const char *im = getenv("XMODIFIERS");
    std::string saved = im ? im : "";
    setenv("XMODIFIERS", "@im=none", 1);
    fl_open_display();
    if (im) setenv("XMODIFIERS", saved.c_str(), 1);
    else unsetenv("XMODIFIERS");
  }
  if (!fl_display) return 1;
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  int w = DisplayWidth(fl_display, fl_screen), h = DisplayHeight(fl_display, fl_screen);
  auto *win = new SecurityScreen(0, 0, w, h);
  win->end();
  win->border(0);
  win->set_override();
  win->back = backdrop(w, h);
  win->show();
  win->take_input();
  return Fl::run();
}

}  // namespace sde
