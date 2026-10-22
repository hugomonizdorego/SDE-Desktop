// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "ui.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "policy.h"
#include "session.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "x11.h"

namespace sde {
namespace lock {

namespace {

double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + ts.tv_nsec / 1e9;
}

void report(int fd, char c) {
  if (fd < 0) return;
  while (write(fd, &c, 1) < 0 && errno == EINTR) {
  }
}

// Box blur of an RGB24/ARGB32 image surface, |passes| times (3 passes come
// close to a gaussian).
void box_blur(cairo_surface_t *s, int radius, int passes) {
  cairo_surface_flush(s);
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s) / 4;
  auto *px = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(s));
  if (!px || w <= 0 || h <= 0) return;
  std::vector<uint32_t> tmp(size_t(std::max(w, h)));
  auto blur_line = [&](uint32_t *start, int n, int step) {
    for (int i = 0; i < n; ++i) tmp[size_t(i)] = start[i * step];
    for (int i = 0; i < n; ++i) {
      unsigned r = 0, g = 0, b = 0, count = 0;
      for (int k = std::max(0, i - radius); k <= std::min(n - 1, i + radius); ++k) {
        uint32_t v = tmp[size_t(k)];
        r += (v >> 16) & 0xFF;
        g += (v >> 8) & 0xFF;
        b += v & 0xFF;
        ++count;
      }
      start[i * step] = 0xFF000000u | ((r / count) << 16) | ((g / count) << 8) | (b / count);
    }
  };
  for (int p = 0; p < passes; ++p) {
    for (int y = 0; y < h; ++y) blur_line(px + y * stride, w, 1);
    for (int x = 0; x < w; ++x) blur_line(px + x, h, stride);
  }
  cairo_surface_mark_dirty(s);
}

// The wallpaper at |w| x |h|, blurred and dimmed.
cairo_surface_t *make_background(const std::string &path, const std::string &mode, int w, int h) {
  cairo_surface_t *full = load_wallpaper(path, w, h, mode, 0.07, 0.09, 0.12);
  int sw = std::max(8, w / 18), sh = std::max(8, h / 18);
  cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
  cairo_t *cr = cairo_create(small);
  cairo_scale(cr, double(sw) / w, double(sh) / h);
  cairo_set_source_surface(cr, full, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(full);
  box_blur(small, 2, 3);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cr = cairo_create(out);
  cairo_scale(cr, double(w) / sw, double(h) / sh);
  cairo_set_source_surface(cr, small, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_identity_matrix(cr);
  cairo_pattern_t *dim = cairo_pattern_create_linear(0, 0, 0, h);
  cairo_pattern_add_color_stop_rgba(dim, 0, 0, 0, 0, 0.30);
  cairo_pattern_add_color_stop_rgba(dim, 1, 0, 0, 0, 0.52);
  cairo_set_source(cr, dim);
  cairo_paint(cr);
  cairo_pattern_destroy(dim);
  cairo_destroy(cr);
  cairo_surface_destroy(small);
  return out;
}

class LockWindow;

// One password check, shared with its worker thread: a PAM stack that never
// answers (a fingerprint reader, a network directory) cannot keep the lock
// screen busy forever; the screen gives up on it after kAuthTimeout and the
// late answer is thrown away.
struct Attempt {
  explicit Attempt(const SecretBuffer &pw) : password(pw.c_str(), pw.c_str() + pw.size()) {
    password.push_back(0);
  }
  ~Attempt() { secure_wipe(password.data(), password.size()); }
  std::vector<char> password;
  AuthResult result;
  unsigned id = 0;
};

const double kAuthTimeout = 30.0;

struct Ui {
  UiOptions opt;
  Config cfg;
  Theme theme;
  std::vector<LockWindow *> windows;  // one per monitor
  LockWindow *primary = nullptr;      // the one with the password form
  bool ready = false;  // content may be drawn (the keyboard is ours, or the waiting message)
  bool done = false;

  // Input. The grabs are checked (taken again) all the time: another
  // client may hold them when the lock starts, and a lost grab must come
  // back without the user noticing.
  bool has_keyboard = false, has_pointer = false;
  bool input_ok = false;      // both grabs held ("L" reported)
  bool waiting = false;       // shown without the grabs ("W" reported)
  double lost_since = -1;     // since when the grabs are missing
  int step = 0;               // InputStep reached in this episode
  double last_wait_log = 0;
  bool raise_pending = false;
  std::string input_note;     // why typing may not work right now

  SecretBuffer password;
  AttemptLimiter limiter;
  std::atomic<bool> busy{false};
  unsigned attempt_id = 0;
  double attempt_started = 0;
  std::string message;
  bool message_error = false;
  bool caps = false;
  double shake_start = -1;
  long last_idle = 0;
  bool blanked = false;
  bool clock_24h = true;
  int last_minute = -1;
  cairo_surface_t *avatar = nullptr;
  std::string wallpaper, wallpaper_mode;
  int switch_x = 0, switch_y = 0, switch_w = 0, switch_h = 0;
  int enter_x = 0, enter_y = 0, enter_r = 0;
};
Ui *g = nullptr;

// SIGUSR1 from the daemon (the computer resumed): take the grabs again,
// raise, look at the monitors and wake them.
std::atomic<bool> g_reassert{false};
void on_sigusr1(int) { g_reassert = true; }

// Timestamped lines on stderr, which sde-session writes to
// ~/.local/state/sde/session.log.
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char *fmt, ...) {
  char when[32];
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(when, sizeof(when), "%H:%M:%S", &tm);
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(stderr, "sde-lock[%d] %s: %s\n", int(getpid()), when, buf);
  fflush(stderr);
}

bool caps_lock_on() {
  XkbStateRec st;
  if (XkbGetState(fl_display, XkbUseCoreKbd, &st) != Success) return false;
  return (st.locked_mods & LockMask) != 0;
}

std::string clock_text() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[64];
  strftime(buf, sizeof(buf), g->clock_24h ? "%H:%M" : "%l:%M", &tm);
  return trim(buf);
}

std::string date_text() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[128];
  strftime(buf, sizeof(buf), _("%A, %e %B"), &tm);
  std::string s = buf;
  // "%e" pads with a space.
  size_t dbl = s.find("  ");
  if (dbl != std::string::npos) s.erase(dbl, 1);
  return s;
}

void submit();

class LockWindow : public BufferedWindow {
 public:
  LockWindow(int x, int y, int w, int h) : BufferedWindow(x, y, w, h, "SDE Screen Lock") {
    end();
    set_override();
    border(0);
    color(FL_BLACK);
  }
  ~LockWindow() override { drop_background(); }

  void prepare_background() {
    if (!bg_) bg_ = make_background(g->wallpaper, g->wallpaper_mode, w(), h());
  }
  void drop_background() {
    if (bg_) cairo_surface_destroy(bg_);
    bg_ = nullptr;
  }

  void draw() override;
  int handle(int e) override;

 private:
  bool primary() const { return this == g->primary; }
  void draw_form(Painter &p, int cx, int top);
  cairo_surface_t *bg_ = nullptr;
};

const Color kWhite = Color::hex(0xFFFFFF);

void LockWindow::draw() {
  int W = w(), H = h();
  if (!g->ready) {
    // Black until the grabs are held: never anything else.
    fl_color(FL_BLACK);
    fl_rectf(0, 0, W, H);
    return;
  }
  double shake = 0;
  if (g->shake_start >= 0) {
    double t = (now_s() - g->shake_start) / 0.45;
    if (t >= 1)
      g->shake_start = -1;
    else
      shake = std::sin(t * 6 * M_PI) * (1 - t) * Sd(18);
  }
  int clock_y = int(H * (primary() ? 0.16 : 0.34));
  int form_top = int(H * 0.44);
  int cx = W / 2;
  {
    Painter p(W, H);
    if (bg_)
      p.surface(bg_, 0, 0);
    else
      p.fill_rect(0, 0, W, H, Color::hex(0x101820));
    if (primary()) draw_form(p, cx + int(shake), form_top);
    // Switch user (LightDM).
    if (primary() && g->opt.switch_user) {
      g->switch_w = S(150);
      g->switch_h = S(40);
      g->switch_x = S(28);
      g->switch_y = H - S(28) - g->switch_h;
      p.fill_rounded(g->switch_x, g->switch_y, g->switch_w, g->switch_h, g->switch_h / 2.0,
                     kWhite.alpha(0.14));
    }
    p.flush();
  }
  // Text (FLTK/Xft) on top of the cairo drawing.
  text_font(S(primary() ? 92 : 110));
  draw_text(clock_text(), 0, clock_y, W, S(primary() ? 110 : 130), kWhite, 1);
  text_font(S(primary() ? 20 : 24));
  draw_text(date_text(), 0, clock_y + S(primary() ? 108 : 128), W, S(30), kWhite.alpha(0.9), 1);
  if (!primary()) return;
  int fx = cx + int(shake);
  int name_y = form_top + S(118);
  text_font(S(20), true);
  draw_text(g->opt.display_name, fx - S(200), name_y, S(400), S(30), kWhite, 1);
  int field_y = name_y + S(44), field_w = S(320), field_h = S(46);
  int field_x = fx - field_w / 2;
  text_font(S(14));
  if (g->password.empty()) {
    std::string hint = g->busy ? _("Checking...") : _("Password");
    draw_text(hint, field_x + S(18), field_y, field_w - S(70), field_h, kWhite.alpha(0.62));
  }
  // Message line: errors, PAM texts, the attempt delay and Caps Lock.
  std::string msg = g->message;
  double wait = g->limiter.wait(now_s());
  if (!g->busy && wait >= 1.5)
    msg = format(_("Too many attempts. Try again in %d seconds."), int(std::ceil(wait)));
  int line_y = field_y + field_h + S(12);
  if (!msg.empty()) {
    text_font(S(14), g->message_error);
    draw_text(msg, fx - S(260), line_y, S(520), S(24),
              g->message_error ? Color::hex(0xFFB4AB) : kWhite.alpha(0.85), 1);
    line_y += S(26);
  }
  if (g->caps) {
    text_font(S(13));
    draw_text(_("Caps Lock is on"), fx - S(200), line_y, S(400), S(22), Color::hex(0xFFE08A), 1);
    line_y += S(24);
  }
  if (!g->input_note.empty()) {
    // The grabs are missing (another program holds them): say so instead
    // of a lock screen that silently ignores the keyboard.
    text_font(S(13), true);
    draw_text(g->input_note, fx - S(300), line_y, S(600), S(22), Color::hex(0xFFE08A), 1);
  }
  if (g->opt.switch_user) {
    text_font(S(14));
    draw_text(_("Switch User"), g->switch_x, g->switch_y, g->switch_w, g->switch_h, kWhite, 1);
  }
  text_font(S(12));
  draw_text(_("The session is locked"), W - S(330), H - S(56), S(300), S(28), kWhite.alpha(0.55), 2);
}

void LockWindow::draw_form(Painter &p, int cx, int top) {
  cairo_t *cr = p.cr();
  int size = S(104);
  double ax = cx - size / 2.0, ay = top;
  p.circle(cx, ay + size / 2.0, size / 2.0 + Sd(3), kWhite.alpha(0.35));
  if (g->avatar) {
    cairo_save(cr);
    cairo_arc(cr, cx, ay + size / 2.0, size / 2.0, 0, 2 * M_PI);
    cairo_clip(cr);
    double iw = cairo_image_surface_get_width(g->avatar);
    cairo_translate(cr, ax, ay);
    cairo_scale(cr, size / iw, size / iw);
    cairo_set_source_surface(cr, g->avatar, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
  } else {
    p.circle(cx, ay + size / 2.0, size / 2.0, g->theme.accent);
    // A simple person silhouette.
    p.circle(cx, ay + size * 0.38, size * 0.17, kWhite.alpha(0.92));
    cairo_save(cr);
    cairo_arc(cr, cx, ay + size / 2.0, size / 2.0, 0, 2 * M_PI);
    cairo_clip(cr);
    cairo_arc(cr, cx, ay + size * 0.98, size * 0.33, M_PI, 2 * M_PI);
    p.set(kWhite.alpha(0.92));
    cairo_fill(cr);
    cairo_restore(cr);
  }
  int name_y = top + S(118);
  int field_y = name_y + S(44), field_w = S(320), field_h = S(46);
  int field_x = cx - field_w / 2;
  p.fill_rounded(field_x, field_y, field_w, field_h, field_h / 2.0, kWhite.alpha(0.16));
  p.stroke_rounded(field_x + 0.5, field_y + 0.5, field_w - 1, field_h - 1, field_h / 2.0,
                   g->busy ? kWhite.alpha(0.35) : g->theme.accent.mix(kWhite, 0.25), Sd(1.6));
  // One dot per character (at most what fits).
  size_t n = g->password.chars();
  int max_dots = (field_w - S(80)) / S(16);
  int dots = int(std::min<size_t>(n, size_t(std::max(1, max_dots))));
  for (int i = 0; i < dots; ++i)
    p.circle(field_x + S(24) + i * S(16), field_y + field_h / 2.0, Sd(4.5), kWhite);
  // Enter button.
  g->enter_r = S(17);
  g->enter_x = field_x + field_w - S(6) - g->enter_r;
  g->enter_y = field_y + field_h / 2;
  p.circle(g->enter_x, g->enter_y, g->enter_r, g->busy ? kWhite.alpha(0.25) : g->theme.accent);
  glyph::chevron(p, g->enter_x - S(8), g->enter_y - S(8), S(16), true, kWhite);
}

void redraw_all() {
  for (auto *w : g->windows) w->redraw();
}

bool ours(Window w) {
  for (auto *lw : g->windows)
    if (lw->shown() && fl_xid(lw) == w) return true;
  return false;
}

bool viewable(Window w) {
  XWindowAttributes a;
  return w && XGetWindowAttributes(fl_display, w, &a) && a.map_state == IsViewable;
}

void raise_all() {
  for (auto *w : g->windows)
    if (w->shown() && fl_xid(w)) XRaiseWindow(fl_display, fl_xid(w));
  XFlush(fl_display);
}

void raise_cb(void *) {
  g->raise_pending = false;
  raise_all();
}

// Raises shortly (coalesced, at most ten times a second: a program that
// also keeps raising itself must not start a raise war at full speed).
void schedule_raise() {
  if (g->raise_pending) return;
  g->raise_pending = true;
  Fl::add_timeout(0.1, raise_cb);
}

void set_primary(LockWindow *w) {
  if (!w || w == g->primary) return;
  LockWindow *old = g->primary;
  g->primary = w;
  if (old) old->redraw();
  w->redraw();
  Fl::focus(w);
}

void become_ready() {
  if (g->ready) return;
  report(g->opt.report_fd, 'H');
  for (auto *w : g->windows) {
    w->prepare_background();
    report(g->opt.report_fd, 'H');  // a big monitor takes a moment
  }
  g->ready = true;
  g->caps = caps_lock_on();
  if (g->primary) Fl::focus(g->primary);
  redraw_all();
}

// Takes (or confirms: taking a grab again is harmless when this client
// holds it) the keyboard and the pointer grabs on the primary window and
// gives it the input focus. The keyboard is kept even when the pointer
// cannot be had: typing the password is what matters.
bool take_grabs() {
  Window w = g->primary && g->primary->shown() ? fl_xid(g->primary) : 0;
  if (!w || !viewable(w)) {
    g->has_keyboard = g->has_pointer = false;
    return false;
  }
  int kb = XGrabKeyboard(fl_display, w, False, GrabModeAsync, GrabModeAsync, CurrentTime);
  int ptr = XGrabPointer(fl_display, w, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
  g->has_keyboard = kb == GrabSuccess;
  g->has_pointer = ptr == GrabSuccess;
  // Focus too: without the keyboard grab the keys still reach the focus
  // window, and the input context follows the focus.
  Window focus = 0;
  int revert = 0;
  XGetInputFocus(fl_display, &focus, &revert);
  if (!ours(focus)) XSetInputFocus(fl_display, w, RevertToPointerRoot, CurrentTime);
  XFlush(fl_display);
  return g->has_keyboard && g->has_pointer;
}

// Popups of other clients that keep a grab (a GTK menu, a panel popup):
// unmapping a window ends the grabs held on it. Logged one by one.
void release_foreign_popups() {
  Window root = RootWindow(fl_display, fl_screen), parent = 0, *kids = nullptr;
  unsigned n = 0;
  if (!XQueryTree(fl_display, root, &root, &parent, &kids, &n)) return;
  XWindowAttributes ra;
  XGetWindowAttributes(fl_display, root, &ra);
  for (unsigned i = 0; i < n; ++i) {
    Window w = kids[i];
    if (ours(w)) continue;
    XWindowAttributes a;
    if (!XGetWindowAttributes(fl_display, w, &a)) continue;
    ForeignWindow f;
    f.override_redirect = a.override_redirect;
    f.viewable = a.map_state == IsViewable;
    f.input_only = a.c_class == InputOnly;
    f.w = a.width;
    f.h = a.height;
    f.screen_w = ra.width;
    f.screen_h = ra.height;
    if (!releasable_popup(f)) continue;  // cheap checks first
    for (Atom t : x11::get_atoms(w, "_NET_WM_WINDOW_TYPE")) {
      char *name = XGetAtomName(fl_display, t);
      if (name) {
        f.types.push_back(name);
        XFree(name);
      }
    }
    f.name = x11::window_title(w);
    if (!releasable_popup(f)) continue;
    log_line("unmapping popup 0x%lx (%s, %dx%d) of another program: it may hold the keyboard grab",
             static_cast<unsigned long>(w), x11::window_class(w).c_str(), a.width, a.height);
    XUnmapWindow(fl_display, w);
  }
  if (kids) XFree(kids);
  XFlush(fl_display);
}

void update_input_note() {
  std::string note;
  if (!g->has_keyboard)
    note = _("Another program is holding the keyboard. The lock screen keeps trying to get it...");
  else if (!g->has_pointer)
    note = _("Another program is holding the mouse. You can type your password.");
  if (note != g->input_note) {
    g->input_note = note;
    if (g->primary) g->primary->redraw();
  }
}

// Every 100 ms while the grabs are missing, every second while they are
// held (a grab can be lost: its window unmapped by someone else).
void input_check(void *) {
  double t = now_s();
  // A window of ours unmapped by another client lost its grabs with it.
  for (auto *w : g->windows)
    if (w->shown() && fl_xid(w) && !viewable(fl_xid(w))) {
      log_line("a lock screen window is not mapped any more; mapping it again");
      XMapRaised(fl_display, fl_xid(w));
      XSync(fl_display, False);
    }
  bool ok = take_grabs();
  if (ok) {
    if (!g->input_ok) {
      if (g->lost_since >= 0 && t - g->lost_since > 0.5)
        log_line("keyboard and pointer grabbed after %.1f s", t - g->lost_since);
      g->input_ok = true;
      report(g->opt.report_fd, 'L');
      session::reset_idle();
    }
    g->lost_since = -1;
    g->step = 0;
    g->waiting = false;
    become_ready();
    update_input_note();
    Fl::repeat_timeout(1.0, input_check);
    return;
  }
  if (g->input_ok) {
    log_line("lost the %s grab; taking it again", !g->has_keyboard ? "keyboard" : "pointer");
    g->input_ok = false;
  }
  if (g->lost_since < 0) g->lost_since = t;
  double waited = t - g->lost_since;
  // With the keyboard the password can be typed: show the form now.
  if (g->has_keyboard) become_ready();
  auto step = int(input_step(waited));
  if (step >= int(InputStep::ClosePopups) && g->step < int(InputStep::ClosePopups)) {
    log_line("another program holds the %s; asking the panel to close its popups",
             !g->has_keyboard ? "keyboard" : "pointer");
    x11::send_command("close-popups");
    raise_all();
  }
  if (step >= int(InputStep::ReleasePopups) && g->step < int(InputStep::ReleasePopups))
    release_foreign_popups();
  if (step >= int(InputStep::ShowWaiting) && !g->waiting) {
    // The safety valve: never a black screen that ignores the user. The
    // lock screen shows itself with the reason and keeps trying.
    g->waiting = true;
    g->last_wait_log = t;
    log_line("cannot get the %s after %.0f s; showing the lock screen and retrying",
             !g->has_keyboard ? "keyboard" : "pointer", waited);
    report(g->opt.report_fd, 'W');
    become_ready();
  } else if (g->waiting && t - g->last_wait_log >= 30) {
    g->last_wait_log = t;
    log_line("still waiting for the %s (%.0f s)", !g->has_keyboard ? "keyboard" : "pointer", waited);
    release_foreign_popups();  // popups opened meanwhile
  }
  g->step = std::max(g->step, step);
  if (g->ready) update_input_note();
  Fl::repeat_timeout(waited < 10 ? 0.1 : 0.25, input_check);
}

void restart_input_check() {
  Fl::remove_timeout(input_check);
  Fl::add_timeout(0, input_check);
}

std::vector<Monitor> current_monitors() {
  std::vector<Monitor> out;
  if (g->opt.monitors) out = g->opt.monitors();
  if (out.empty())
    for (int i = 0; i < std::max(1, Fl::screen_count()); ++i) {
      Monitor m;
      Fl::screen_xywh(m.x, m.y, m.w, m.h, i);
      out.push_back(m);
    }
  return out;
}

// Monitors added, removed or resized (RandR; a resume with another monitor
// layout): one window per monitor again, the form under the pointer, the
// grabs on a window that is still mapped.
void reconfigure_screens() {
  std::vector<Monitor> mons = current_monitors();
  int n = int(mons.size());
  bool same = int(g->windows.size()) == n;
  for (int i = 0; same && i < n; ++i) {
    LockWindow *w = g->windows[size_t(i)];
    same = Monitor{w->x(), w->y(), w->w(), w->h()} == mons[size_t(i)];
  }
  if (same) return;
  int mx = 0, my = 0;
  Fl::get_mouse(mx, my);
  int pointer_screen = monitor_at(mons, mx, my);
  log_line("the monitors changed (%d now); moving the lock screen", n);
  for (int i = 0; i < n; ++i) {
    int sx = mons[size_t(i)].x, sy = mons[size_t(i)].y, sw = mons[size_t(i)].w, sh = mons[size_t(i)].h;
    if (i < int(g->windows.size())) {
      LockWindow *w = g->windows[size_t(i)];
      if (w->x() != sx || w->y() != sy || w->w() != sw || w->h() != sh) {
        w->drop_background();
        w->resize(sx, sy, sw, sh);
      }
    } else {
      auto *w = new LockWindow(sx, sy, sw, sh);
      g->windows.push_back(w);
      w->show();
    }
  }
  std::vector<LockWindow *> extra(g->windows.begin() + n, g->windows.end());
  g->windows.resize(size_t(n));
  LockWindow *want = g->windows[size_t(std::max(0, std::min(n - 1, pointer_screen)))];
  if (std::find(extra.begin(), extra.end(), g->primary) != extra.end()) g->primary = nullptr;
  if (!g->primary) g->primary = want;
  set_primary(want);
  XSync(fl_display, False);
  // Move the grabs before a window that held them goes away.
  take_grabs();
  for (auto *w : extra) {
    w->hide();
    Fl::delete_widget(w);
  }
  if (g->ready)
    for (auto *w : g->windows) w->prepare_background();
  raise_all();
  redraw_all();
  restart_input_check();
}

void reassert() {
  log_line("re-checking the lock screen (resume)");
  Fl::call_screen_init();
  reconfigure_screens();
  raise_all();
  g->blanked = false;
  session::reset_idle();
  restart_input_check();
}

int LockWindow::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_MOVE:
      // The form follows the pointer to the monitor the user looks at.
      if (g->ready) set_primary(this);
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
    case FL_LEAVE:
    case FL_DRAG:
    case FL_RELEASE:
    case FL_MOUSEWHEEL:
    case FL_KEYUP:
      return 1;
    case FL_PUSH: {
      if (!g->ready) return 1;
      if (!primary()) {
        set_primary(this);
        return 1;
      }
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (g->opt.switch_user && ex >= g->switch_x && ex < g->switch_x + g->switch_w &&
          ey >= g->switch_y && ey < g->switch_y + g->switch_h) {
        spawn_detached({"dm-tool", "switch-to-greeter"});
        return 1;
      }
      double dx = ex - g->enter_x, dy = ey - g->enter_y;
      if (dx * dx + dy * dy <= double(g->enter_r) * g->enter_r) submit();
      return 1;
    }
    case FL_KEYBOARD:
    case FL_SHORTCUT: {
      if (!g->ready) return 1;
      g->caps = caps_lock_on();
      if (g->busy) return 1;
      int key = Fl::event_key();
      bool ctrl = Fl::event_state(FL_CTRL) != 0;
      if (key == FL_Escape || (ctrl && (key == 'u' || key == FL_BackSpace))) {
        g->password.clear();
      } else if (key == FL_BackSpace) {
        g->password.pop_char();
      } else if (key == FL_Enter || key == FL_KP_Enter) {
        submit();
      } else if (!ctrl && Fl::event_length() > 0) {
        const char *text = Fl::event_text();
        auto c = static_cast<unsigned char>(text[0]);
        if (c >= 0x20 && c != 0x7F) {
          if (g->message_error) g->message.clear(), g->message_error = false;
          if (!g->password.append(text, size_t(Fl::event_length()))) {
            g->message = _("The password is too long.");
            g->message_error = true;
          }
        }
      }
      if (g->primary) g->primary->redraw();
      return 1;
    }
    default:
      return BufferedWindow::handle(e);
  }
}

void auth_done(void *data) {
  std::unique_ptr<std::shared_ptr<Attempt>> holder(static_cast<std::shared_ptr<Attempt> *>(data));
  std::shared_ptr<Attempt> a = *holder;
  if (!g || a->id != g->attempt_id || !g->busy) return;  // given up on (timeout)
  g->password.clear();
  AuthResult r = std::move(a->result);
  g->busy = false;
  if (r.status == AuthResult::Accepted) {
    g->limiter.success();
    report(g->opt.report_fd, 'U');
    g->done = true;
    return;
  }
  g->limiter.failure(now_s());
  g->message_error = true;
  if (r.status == AuthResult::Unavailable)
    g->message = _("The password cannot be checked (PAM service sde-lock).");
  else if (!r.messages.empty())
    g->message = r.messages.back();
  else
    g->message = _("Wrong password. Try again.");
  g->shake_start = now_s();
  redraw_all();
}

void submit() {
  if (g->busy || g->password.empty()) return;
  if (g->limiter.wait(now_s()) > 0) {
    redraw_all();
    return;
  }
  g->busy = true;
  g->attempt_started = now_s();
  g->message = _("Checking...");
  g->message_error = false;
  if (g->primary) g->primary->redraw();
  Fl::flush();
  auto attempt = std::make_shared<Attempt>(g->password);
  attempt->id = ++g->attempt_id;
  std::function<AuthResult(const char *)> check = g->opt.authenticate;
  std::thread([attempt, check] {
    attempt->result = check(attempt->password.data());
    Fl::awake(auth_done, new std::shared_ptr<Attempt>(attempt));
  }).detach();
}

void auth_watchdog() {
  if (!g->busy || now_s() - g->attempt_started < kAuthTimeout) return;
  log_line("the password check did not answer in %.0f s; giving up on it", kAuthTimeout);
  ++g->attempt_id;  // its answer is ignored
  g->busy = false;
  g->password.clear();
  g->message = _("The password check did not answer. Try again.");
  g->message_error = true;
  redraw_all();
}

void tick(void *) {
  report(g->opt.report_fd, 'H');  // heartbeat for the daemon's watchdog
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  bool caps = g->ready && caps_lock_on();
  bool dirty = tm.tm_min != g->last_minute || caps != g->caps || g->limiter.wait(now_s()) > 0;
  g->last_minute = tm.tm_min;
  g->caps = caps;
  auth_watchdog();
  long idle = session::idle_ms();
  if (idle >= 0) {
    if (idle < g->last_idle) g->blanked = false;
    g->last_idle = idle;
    if (g->opt.blank_after > 0 && !g->blanked && idle >= g->opt.blank_after * 1000L) {
      g->blanked = true;
      session::monitors_off();
    }
    // A half-typed password does not stay around.
    if (!g->busy && !g->password.empty() && idle >= 60000) {
      g->password.clear();
      dirty = true;
    }
  }
  // FLTK's monitor list is refreshed by RRScreenChangeNotify and root
  // ConfigureNotify; polling it would make the X server probe the outputs.
  reconfigure_screens();
  // Stay above everything (notifications, menus opened before the lock).
  raise_all();
  if (dirty) redraw_all();
  Fl::repeat_timeout(1.0, tick);
}

void animate(void *) {
  if (g->shake_start >= 0) {
    if (g->primary) g->primary->redraw();
    Fl::repeat_timeout(1.0 / 60, animate);
  } else {
    Fl::add_timeout(0.05, animate);
  }
}

int x_events(int event) {
  if (!g) return 0;
  if (event == FL_SCREEN_CONFIGURATION_CHANGED) {
    reconfigure_screens();
    return 0;
  }
  if (!fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  Window root = RootWindow(fl_display, fl_screen);
  switch (e.type) {
    case MapNotify:
      if (!ours(e.xmap.window)) schedule_raise();
      break;
    case ConfigureNotify:
      if (e.xconfigure.window == root) {
        // The screen size changed (RandR): the monitors did too.
        Fl::call_screen_init();
        reconfigure_screens();
      } else if (e.xconfigure.event == root && !ours(e.xconfigure.window)) {
        // Another top-level window was restacked (raised above us) or moved.
        schedule_raise();
      }
      break;
    case VisibilityNotify:
      if (ours(e.xvisibility.window) && e.xvisibility.state != VisibilityUnobscured) schedule_raise();
      break;
    case UnmapNotify:
      // One of ours unmapped by another client: its grabs ended with it.
      if (e.xunmap.event == e.xunmap.window && ours(e.xunmap.window) && !g->done) {
        log_line("a lock screen window was unmapped by another program; mapping it again");
        XMapRaised(fl_display, e.xunmap.window);
        restart_input_check();
      }
      break;
    default: break;
  }
  return 0;
}

// Extra events and a name (FLTK names no override-redirect window) for a
// lock window, once.
void select_extra_events(LockWindow *w) {
  if (!w->shown() || !fl_xid(w)) return;
  XWindowAttributes a;
  long want = VisibilityChangeMask | StructureNotifyMask;
  if (XGetWindowAttributes(fl_display, fl_xid(w), &a) && (a.your_event_mask & want) != want) {
    XSelectInput(fl_display, fl_xid(w), a.your_event_mask | want);
    XStoreName(fl_display, fl_xid(w), "SDE Screen Lock");
    x11::set_string(fl_xid(w), "_NET_WM_NAME", "SDE Screen Lock");
  }
}

}  // namespace

void default_options(UiOptions &o) {
  if (struct passwd *pw = getpwuid(getuid())) {
    o.user = pw->pw_name ? pw->pw_name : "";
    std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
    o.display_name = trim(gecos.substr(0, gecos.find(',')));
    std::string home = pw->pw_dir ? pw->pw_dir : home_dir();
    for (const std::string &p : {home + "/.face", home + "/.face.icon",
                                 "/var/lib/AccountsService/icons/" + o.user})
      if (file_exists(p)) {
        o.avatar = p;
        break;
      }
  }
  if (o.display_name.empty()) o.display_name = o.user;
  Config cfg;
  cfg.load();
  o.blank_after = std::max(0, cfg.get_int("lock", "blank_after", 30));
  o.switch_user = cfg.get_bool("lock", "show_switch_user", true) && !env("XDG_SEAT_PATH").empty() &&
                  !find_program("dm-tool").empty();
}

int run_lock_ui(const UiOptions &options) {
  Ui ui;
  g = &ui;
  ui.opt = options;
  signal(SIGUSR1, on_sigusr1);
  Fl::lock();  // enables Fl::awake() from the PAM thread
  // No X input method. An input method (IBus, Fcitx) filters every key
  // before FLTK sees it: when its server is busy, restarting or has not
  // followed the focus to this override-redirect window, the keys never
  // come back and the password cannot be typed. A password does not belong
  // in an input method either (it would see it in clear).
  setenv("XMODIFIERS", "@im=none", 1);
  fl_open_display();
  ui.cfg.load();
  ui.theme = Theme::from_config(ui.cfg);
  ui.clock_24h = ui.cfg.get_bool("panel", "clock_24h", true);
  ui.wallpaper = ui.cfg.get("appearance", "wallpaper", "auto");
  ui.wallpaper_mode = ui.cfg.get("appearance", "wallpaper_mode", "zoom");
  if (ui.wallpaper.empty() || ui.wallpaper == "auto" || !file_exists(ui.wallpaper)) {
    std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
    std::string name = ui.theme.dark ? "selviana-dark.svg" : "selviana-light.svg";
    ui.wallpaper = base + "/backgrounds/sde/" + name;
    if (!file_exists(ui.wallpaper)) ui.wallpaper = "/usr/share/backgrounds/sde/" + name;
  }
  if (!ui.opt.avatar.empty())
    ui.avatar = load_wallpaper(ui.opt.avatar, S(208), S(208), "zoom", 0.2, 0.2, 0.2);

  // The form goes on the monitor with the pointer.
  int mx = 0, my = 0;
  Fl::get_mouse(mx, my);
  std::vector<Monitor> mons = current_monitors();
  int primary_screen = monitor_at(mons, mx, my);
  for (size_t i = 0; i < mons.size(); ++i) {
    auto *w = new LockWindow(mons[i].x, mons[i].y, mons[i].w, mons[i].h);
    ui.windows.push_back(w);
    if (int(i) == primary_screen) ui.primary = w;
  }
  if (!ui.primary) ui.primary = ui.windows.front();
  for (auto *w : ui.windows) w->show();
  // Wait until X has mapped the windows: a grab needs a viewable window.
  double until = now_s() + 3;
  bool mapped = false;
  while (!mapped && now_s() < until) {
    Fl::wait(0.05);
    mapped = true;
    for (auto *w : ui.windows) mapped &= fl_xid(w) && viewable(fl_xid(w));
  }
  raise_all();
  report(ui.opt.report_fd, 'M');
  if (!mapped) {
    log_line("%s", _("cannot map the lock screen windows; the screen is NOT locked"));
    report(ui.opt.report_fd, 'F');
    for (auto *w : ui.windows) w->hide();
    XFlush(fl_display);
    g = nullptr;
    return 3;
  }
  for (auto *w : ui.windows) select_extra_events(w);
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | SubstructureNotifyMask | StructureNotifyMask);
  Fl::add_handler(x_events);
  // Grab at once (typing must work right after the lock), then keep
  // checking. The screen stays black until the keyboard is ours or the
  // waiting message is due.
  ui.lost_since = now_s();
  Fl::add_timeout(0, input_check);
  Fl::add_timeout(1.0, tick);
  Fl::add_timeout(0.05, animate);
  while (!ui.done) {
    Fl::wait(0.25);
    if (g_reassert.exchange(false)) reassert();
    for (auto *w : ui.windows) select_extra_events(w);  // windows of a monitor change
  }
  XUngrabKeyboard(fl_display, CurrentTime);
  XUngrabPointer(fl_display, CurrentTime);
  for (auto *w : ui.windows) w->hide();
  XFlush(fl_display);
  Fl::remove_handler(x_events);
  Fl::remove_timeout(tick);
  Fl::remove_timeout(animate);
  Fl::remove_timeout(input_check);
  Fl::remove_timeout(raise_cb);
  if (ui.avatar) cairo_surface_destroy(ui.avatar);
  g = nullptr;
  return 0;
}

}  // namespace lock
}  // namespace sde
