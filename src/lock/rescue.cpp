// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "rescue.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "i18n.h"
#include "policy.h"
#include "util.h"

namespace sde {
namespace lock {

namespace {

double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + ts.tv_nsec / 1e9;
}

struct Check {
  std::vector<char> password;
  AuthResult result;
  unsigned id = 0;
  ~Check() { secure_wipe(password.data(), password.size()); }
};

class Prompt : public Fl_Window {
 public:
  Prompt(int x, int y, int w, int h) : Fl_Window(x, y, w, h, "SDE Screen Lock") {
    end();
    set_override();
    border(0);
    color(FL_BLACK);
  }

  RescueOptions opt;
  SecretBuffer password;
  AttemptLimiter limiter;
  bool busy = false;
  unsigned check_id = 0;
  std::string message;

  void draw() override {
    fl_color(FL_BLACK);
    fl_rectf(0, 0, w(), h());
    // The monitor with the pointer.
    int mx = 0, my = 0, sx = 0, sy = 0, sw = w(), sh = h();
    Fl::get_mouse(mx, my);
    Fl::screen_xywh(sx, sy, sw, sh, Fl::screen_num(mx, my));
    sx -= x();
    sy -= y();
    int cx = sx + sw / 2, top = sy + sh * 2 / 5;
    fl_color(FL_WHITE);
    fl_font(FL_HELVETICA_BOLD, 26);
    fl_draw(_("SDE Screen Lock"), sx, top, sw, 34, FL_ALIGN_CENTER);
    fl_font(FL_HELVETICA, 16);
    fl_color(fl_rgb_color(200, 200, 200));
    fl_draw(_("The lock screen could not start. Type your password and press Enter."), sx,
            top + 44, sw, 24, FL_ALIGN_CENTER);
    // The field.
    int fw = 340, fh = 44, fx = cx - fw / 2, fy = top + 84;
    fl_color(fl_rgb_color(40, 44, 52));
    fl_rectf(fx, fy, fw, fh);
    fl_color(busy ? fl_rgb_color(120, 120, 120) : fl_rgb_color(90, 150, 230));
    fl_rect(fx, fy, fw, fh);
    fl_color(FL_WHITE);
    size_t n = std::min<size_t>(password.chars(), size_t((fw - 30) / 16));
    for (size_t i = 0; i < n; ++i) fl_pie(fx + 14 + int(i) * 16, fy + fh / 2 - 5, 10, 10, 0, 360);
    if (!n) {
      fl_color(fl_rgb_color(150, 150, 150));
      fl_font(FL_HELVETICA, 15);
      fl_draw(busy ? _("Checking...") : _("Password"), fx + 14, fy, fw - 28, fh, FL_ALIGN_LEFT);
    }
    std::string msg = message;
    double wait = limiter.wait(now_s());
    if (!busy && wait >= 1.5) msg = format(_("Too many attempts. Try again in %d seconds."), int(wait + 0.99));
    if (!msg.empty()) {
      fl_font(FL_HELVETICA, 15);
      fl_color(fl_rgb_color(255, 180, 171));
      fl_draw(msg.c_str(), sx, fy + fh + 12, sw, 24, FL_ALIGN_CENTER);
    }
  }

  int handle(int e) override {
    switch (e) {
      case FL_KEYBOARD:
      case FL_SHORTCUT: {
        if (busy) return 1;
        int key = Fl::event_key();
        bool ctrl = Fl::event_state(FL_CTRL) != 0;
        if (key == FL_Escape || (ctrl && (key == 'u' || key == FL_BackSpace))) {
          password.clear();
        } else if (key == FL_BackSpace) {
          password.pop_char();
        } else if (key == FL_Enter || key == FL_KP_Enter) {
          submit();
        } else if (!ctrl && Fl::event_length() > 0) {
          auto c = static_cast<unsigned char>(Fl::event_text()[0]);
          if (c >= 0x20 && c != 0x7F) {
            message.clear();
            password.append(Fl::event_text(), size_t(Fl::event_length()));
          }
        }
        redraw();
        return 1;
      }
      case FL_PUSH:
      case FL_RELEASE:
      case FL_FOCUS:
      case FL_UNFOCUS:
      case FL_ENTER:
      case FL_LEAVE:
      case FL_MOVE:
      case FL_KEYUP:
        return 1;
      default:
        return Fl_Window::handle(e);
    }
  }

  void submit();
};

Prompt *g_prompt = nullptr;

void check_done(void *data) {
  std::unique_ptr<std::shared_ptr<Check>> holder(static_cast<std::shared_ptr<Check> *>(data));
  std::shared_ptr<Check> c = *holder;
  Prompt *p = g_prompt;
  if (!p || !p->busy || c->id != p->check_id) return;
  p->busy = false;
  p->password.clear();
  if (c->result.status == AuthResult::Accepted) {
    p->limiter.success();
    auto unlocked = p->opt.unlocked;
    if (unlocked) unlocked();
    return;
  }
  p->limiter.failure(now_s());
  p->message = c->result.status == AuthResult::Unavailable
                   ? _("The password cannot be checked (PAM service sde-lock).")
                   : _("Wrong password. Try again.");
  p->redraw();
}

void Prompt::submit() {
  if (busy || password.empty() || limiter.wait(now_s()) > 0) {
    redraw();
    return;
  }
  busy = true;
  message.clear();
  auto c = std::make_shared<Check>();
  c->password.assign(password.c_str(), password.c_str() + password.size());
  c->password.push_back(0);
  c->id = ++check_id;
  auto check = opt.authenticate;
  std::thread([c, check] {
    if (check) c->result = check(c->password.data());
    Fl::awake(check_done, new std::shared_ptr<Check>(c));
  }).detach();
  redraw();
}

}  // namespace

void show_rescue_prompt(const RescueOptions &options) {
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes ra;
  XGetWindowAttributes(fl_display, root, &ra);
  if (g_prompt && (g_prompt->w() != ra.width || g_prompt->h() != ra.height))
    g_prompt->resize(0, 0, ra.width, ra.height);
  if (!g_prompt) g_prompt = new Prompt(0, 0, std::max(1, ra.width), std::max(1, ra.height));
  g_prompt->opt = options;
  if (!g_prompt->shown()) g_prompt->show();
  // Mapped before the caller grabs on it.
  for (int i = 0; i < 40; ++i) {
    XWindowAttributes a;
    if (fl_xid(g_prompt) && XGetWindowAttributes(fl_display, fl_xid(g_prompt), &a) &&
        a.map_state == IsViewable)
      break;
    Fl::wait(0.025);
  }
  if (fl_xid(g_prompt)) {
    XStoreName(fl_display, fl_xid(g_prompt), "SDE Screen Lock");
    XRaiseWindow(fl_display, fl_xid(g_prompt));
  }
  Fl::focus(g_prompt);
  g_prompt->redraw();
  XFlush(fl_display);
}

void hide_rescue_prompt() {
  if (!g_prompt) return;
  g_prompt->password.clear();
  g_prompt->busy = false;
  ++g_prompt->check_id;
  g_prompt->hide();
  XFlush(fl_display);
}

unsigned long rescue_prompt_window() {
  return g_prompt && g_prompt->shown() && g_prompt->visible() ? fl_xid(g_prompt) : 0;
}

}  // namespace lock
}  // namespace sde
