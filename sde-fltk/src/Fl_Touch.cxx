//
// SDE-FLTK touch screen support: turns recognized gestures into FLTK events
// (see FL/Fl_Touch.H). The X11 part is in Fl_x.cxx.
//
// Copyright 2026 Selviana DE Project and contributors. Same license as FLTK (LGPL-2 with the FLTK
// exceptions, see COPYING).
//
// What a gesture does depends on the widget under the finger:
//
//  * TAP: mouse button 1 pressed and released there (a click; taps in quick
//    succession are a double click), then FL_LEAVE so that nothing stays
//    highlighted under a finger that is gone.
//  * PAN: a widget that a finger drags (Fl_Widget::touch_drag(), sliders and
//    scroll bars along their axis) gets mouse button 1 pressed where the
//    finger touched and FL_DRAG events. Otherwise FL_SCROLL_GESTURE goes to
//    the widget under the finger and its parents until one takes it; that
//    one keeps the gesture and its momentum. When none does, text widgets
//    ignore the drag (a finger does not select text) and other widgets get
//    it as a mouse drag (icons, canvases).
//  * LONG_PRESS: mouse button 3 pressed (a context menu); moving and lifting
//    the finger drags and releases button 3, so an item of the menu that
//    opened can be chosen by sliding to it.
//  * PINCH: FL_ZOOM_GESTURE to the widget under the fingers and its parents;
//    the movement of the two fingers scrolls as a pan does.
//  * TWO_FINGER_TAP: a right click.
//

#include <FL/Fl.H>
#include <FL/Fl_Touch.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Valuator.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Platform glue (Fl_x.cxx).
// what: 0 motion, 1 press, 2 release; button: the button pressed, released
// or held (0: none). clicks: Fl::event_clicks() for a press; keep_click: a
// release that is still a click (Fl::event_is_click()).
extern void fl_touch_pointer(Fl_Window *win, int what, int button, int x, int y,
                             unsigned long server_time, unsigned state, int clicks, int keep_click);
extern void fl_touch_select_all(bool on);
extern bool fl_touch_platform_available();

namespace {

enum Mode { NONE, PENDING, POINTER, SCROLL, DROP, HELD };

struct Touch_Dispatch {
  Fl_Touch_Gesture rec;
  Fl_Widget *window;     // the Fl_Window where the gesture began (watched)
  Fl_Widget *hit;        // the deepest widget under the first finger (watched)
  Fl_Widget *scroll;     // took FL_SCROLL_GESTURE (watched)
  Fl_Widget *zoom;       // took FL_ZOOM_GESTURE (watched)
  int scroll_ox, scroll_oy;  // offset of |scroll|'s window from |window|
  int zoom_ox, zoom_oy;
  bool zoom_tried, scroll_tried;
  Mode mode;
  int button;            // the mouse button held in POINTER and HELD mode
  double x0, y0;         // where the scroll began (window coordinates)
  double acc_x, acc_y;   // scroll pixels not sent yet (fractions)
  double unit_x, unit_y; // scroll_units_*: pixels left over
  unsigned state;        // keyboard modifiers of the last touch event
  unsigned long server_time;  // X server time of the last touch event (0: none)
  double clock_offset;   // local ms - server ms (smallest seen)
  bool have_offset;
  double last_event;     // local ms of the last touch event
  bool timer;            // tick() is scheduled
  bool pending;          // movements wait for platform_flush()
  bool enabled;
  double scale;
  // the event being handled
  bool touch_event;
  int phase;
  double zoom_factor;
  double ex, ey;
  Fl_Touch::Keyboard_Hook keyboard;
  // fingers of this gesture (touches of other windows are ignored)
  int ids[16];
  int nids;
};

Touch_Dispatch &D() {
  static Touch_Dispatch *d = 0;
  if (!d) {
    d = new Touch_Dispatch();
    d->window = d->hit = d->scroll = d->zoom = 0;
    d->scroll_ox = d->scroll_oy = d->zoom_ox = d->zoom_oy = 0;
    d->zoom_tried = d->scroll_tried = false;
    d->mode = NONE;
    d->button = 1;
    d->x0 = d->y0 = d->acc_x = d->acc_y = d->unit_x = d->unit_y = 0;
    d->state = 0;
    d->server_time = 0;
    d->clock_offset = 0;
    d->have_offset = false;
    d->last_event = 0;
    d->timer = d->pending = d->touch_event = false;
    d->enabled = true;
    d->scale = 1;
    d->phase = Fl_Touch::SCROLL_END;
    d->zoom_factor = 1;
    d->ex = d->ey = 0;
    d->keyboard = 0;
    d->nids = 0;
    Fl::watch_widget_pointer(d->window);
    Fl::watch_widget_pointer(d->hit);
    Fl::watch_widget_pointer(d->scroll);
    Fl::watch_widget_pointer(d->zoom);
    // SDE_TOUCH=0 leaves touches to the X server's pointer emulation.
    const char *env = getenv("SDE_TOUCH");
    if (env && (!strcmp(env, "0") || !strcmp(env, "off") || !strcmp(env, "false"))) d->enabled = false;
  }
  return *d;
}

Fl_Window *gesture_window() { return D().window ? D().window->as_window() : 0; }

double now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// The deepest visible widget at (x, y) in window coordinates.
Fl_Widget *widget_at(Fl_Group *g, int x, int y) {
  for (int i = g->children(); i--;) {
    Fl_Widget *o = g->child(i);
    if (!o->visible() || o->as_window()) continue;
    if (x < o->x() || y < o->y() || x >= o->x() + o->w() || y >= o->y() + o->h()) continue;
    Fl_Group *sub = o->as_group();
    return sub ? widget_at(sub, x, y) : o;
  }
  return g;
}

bool is_text(Fl_Widget *w) {
  for (; w && !w->as_window(); w = w->parent())
    if (dynamic_cast<Fl_Input_ *>(w) || dynamic_cast<Fl_Text_Display *>(w)) return true;
  return false;
}

bool wants_keyboard(Fl_Widget *w) {
  if (!w) return false;
  if (w->needs_keyboard()) return true;
  if (Fl_Input_ *in = dynamic_cast<Fl_Input_ *>(w)) return !in->readonly();
  return dynamic_cast<Fl_Text_Editor *>(w) != 0;
}

// Directions in which a finger drags |w| (FL_TOUCH_DRAG_X, FL_TOUCH_DRAG_Y).
int drag_axes(Fl_Widget *w) {
  if (!w) return 0;
  if (int set = w->touch_drag()) return set & (FL_TOUCH_DRAG_X | FL_TOUCH_DRAG_Y);
  if (Fl_Slider *s = dynamic_cast<Fl_Slider *>(w))  // also scroll bars
    return (s->type() & FL_HOR_SLIDER) ? FL_TOUCH_DRAG_X : FL_TOUCH_DRAG_Y;
  if (dynamic_cast<Fl_Valuator *>(w)) return FL_TOUCH_DRAG_X | FL_TOUCH_DRAG_Y;
  return 0;
}

bool blocked_by_modal(Fl_Window *w) {
  Fl_Window *m = Fl::modal();
  if (!m || !w) return false;
  Fl_Window *top = w;
  while (top->window()) top = top->window();
  return top != m && !m->contains(w);
}

struct Saved_Event {
  int e_number, e_x, e_y, e_x_root, e_y_root, e_dx, e_dy;
  Saved_Event()
    : e_number(Fl::e_number), e_x(Fl::e_x), e_y(Fl::e_y), e_x_root(Fl::e_x_root),
      e_y_root(Fl::e_y_root), e_dx(Fl::e_dx), e_dy(Fl::e_dy) {}
  ~Saved_Event() {
    Fl::e_number = e_number; Fl::e_x = e_x; Fl::e_y = e_y; Fl::e_x_root = e_x_root;
    Fl::e_y_root = e_y_root; Fl::e_dx = e_dx; Fl::e_dy = e_dy;
  }
};

struct Touch_Flag {
  bool old;
  Touch_Flag() : old(D().touch_event) { D().touch_event = true; }
  ~Touch_Flag() { D().touch_event = old; }
};

// Sends |event| to |w| itself; (x, y) in the coordinates of w's window.
int send_direct(Fl_Widget *w, int event, int x, int y, int dx, int dy) {
  Fl_Window *win = w->as_window() ? w->as_window() : w->window();
  Saved_Event saved;
  Touch_Flag flag;
  Fl::e_number = event;
  Fl::e_x = x;
  Fl::e_y = y;
  Fl::e_x_root = (win ? win->x_root() : 0) + x;
  Fl::e_y_root = (win ? win->y_root() : 0) + y;
  Fl::e_dx = dx;
  Fl::e_dy = dy;
  return w->handle(event);
}

// |event| to the widget under the finger, then to its parents until one
// takes it. (x, y) in the coordinates of the gesture's window; ox, oy: the
// offset of the window of the widget that took it.
Fl_Widget *offer_chain(Fl_Widget *from, int event, double x, double y, int dx, int dy,
                       int &ox, int &oy) {
  ox = oy = 0;
  for (Fl_Widget *w = from; w; w = w->parent()) {
    Fl_Widget_Tracker t(w);
    if (send_direct(w, event, int(lround(x)) + ox, int(lround(y)) + oy, dx, dy))
      return t.exists() ? w : 0;
    if (!t.exists()) return 0;
    if (w->as_window()) {
      if (!w->parent()) break;
      // A subwindow: its parent sees the point moved by its position.
      ox += w->x();
      oy += w->y();
    }
  }
  return 0;
}

void schedule();

void pointer(int what, int button, double x, double y, int clicks = 0, int keep_click = 0) {
  Touch_Dispatch &d = D();
  Fl_Window *win = gesture_window();
  if (!win || blocked_by_modal(win)) return;
  Touch_Flag flag;
  d.ex = x;
  d.ey = y;
  fl_touch_pointer(win, what, button, int(lround(x)), int(lround(y)), d.server_time, d.state,
                   clicks, keep_click);
}

void leave() {
  Fl_Window *win = gesture_window();
  if (!win || Fl::grab() || Fl::pushed()) return;
  Touch_Flag flag;
  Fl::handle(FL_LEAVE, win);
}

// Sends the whole pixels of the finger movement fdx, fdy to the scroll
// target; returns what it returned.
int scroll_by(double fdx, double fdy, int phase) {
  Touch_Dispatch &d = D();
  if (!d.scroll) return 0;
  // The content follows the finger: a finger moving up scrolls down.
  d.acc_x -= fdx;
  d.acc_y -= fdy;
  int sx = int(d.acc_x), sy = int(d.acc_y);  // towards zero
  if (!sx && !sy) return 1;  // less than a pixel: nothing to say yet
  d.acc_x -= sx;
  d.acc_y -= sy;
  d.phase = phase;
  return send_direct(d.scroll, FL_SCROLL_GESTURE, int(lround(d.x0)) + d.scroll_ox,
                     int(lround(d.y0)) + d.scroll_oy, sx, sy);
}

void end_scroll() {
  Touch_Dispatch &d = D();
  if (d.scroll) {
    d.phase = Fl_Touch::SCROLL_END;
    send_direct(d.scroll, FL_SCROLL_GESTURE, int(lround(d.x0)) + d.scroll_ox,
                int(lround(d.y0)) + d.scroll_oy, 0, 0);
  }
  d.scroll = 0;
  d.scroll_tried = false;
  d.acc_x = d.acc_y = 0;
}

// The first FL_SCROLL_GESTURE of a pan, at (x0, y0): finds the widget that
// takes a scroll in the direction of the finger movement fdx, fdy.
bool begin_scroll(Fl_Widget *from, double x0, double y0, double fdx, double fdy) {
  Touch_Dispatch &d = D();
  d.scroll_tried = true;
  d.x0 = x0;
  d.y0 = y0;
  d.acc_x = -fdx;
  d.acc_y = -fdy;
  d.unit_x = d.unit_y = 0;
  int sx = int(d.acc_x), sy = int(d.acc_y);
  // At least the direction, for a movement of less than a pixel.
  int qx = sx ? sx : (d.acc_x > 0 ? 1 : d.acc_x < 0 ? -1 : 0);
  int qy = sy ? sy : (d.acc_y > 0 ? 1 : d.acc_y < 0 ? -1 : 0);
  if (!qx && !qy) return false;
  d.acc_x -= qx;
  d.acc_y -= qy;
  d.phase = Fl_Touch::SCROLL_BEGIN;
  int ox, oy;
  Fl_Widget *w = offer_chain(from, FL_SCROLL_GESTURE, x0, y0, qx, qy, ox, oy);
  if (!w) {
    d.acc_x = d.acc_y = 0;
    return false;
  }
  d.scroll = w;
  d.scroll_ox = ox;
  d.scroll_oy = oy;
  return true;
}

void zoom_by(double factor, double cx, double cy) {
  Touch_Dispatch &d = D();
  Fl_Window *win = gesture_window();
  if (factor == 1 || !win || blocked_by_modal(win)) return;
  d.zoom_factor = factor;
  int dy = int(lround((factor - 1) * 1000));
  if (!d.zoom && !d.zoom_tried) {
    d.zoom_tried = true;
    Fl_Widget *from = widget_at(win, int(lround(cx)), int(lround(cy)));
    d.zoom = offer_chain(from, FL_ZOOM_GESTURE, cx, cy, 0, dy, d.zoom_ox, d.zoom_oy);
    return;
  }
  if (d.zoom)
    send_direct(d.zoom, FL_ZOOM_GESTURE, int(lround(cx)) + d.zoom_ox, int(lround(cy)) + d.zoom_oy, 0, dy);
}

// A finger began to move from (x0, y0) to (x, y); fdx, fdy: the movement to
// scroll by. Picks a drag, a scroll or nothing.
void start_moving(double x0, double y0, double x, double y, double fdx, double fdy, int lock) {
  Touch_Dispatch &d = D();
  Fl_Window *win = gesture_window();
  if (!win) return;
  double mx = x - x0, my = y - y0;
  int dir = lock == 1 ? FL_TOUCH_DRAG_X : lock == 2 ? FL_TOUCH_DRAG_Y
          : (fabs(mx) >= fabs(my) ? FL_TOUCH_DRAG_X : FL_TOUCH_DRAG_Y);
  bool modal = blocked_by_modal(win);
  if (modal) {
    d.mode = DROP;
    return;
  }
  if (!(drag_axes(d.hit) & dir)) {
    if (d.hit && begin_scroll(d.hit, x0, y0, fdx, fdy)) {
      d.mode = SCROLL;
      return;
    }
    if (is_text(d.hit)) {
      d.mode = DROP;  // a finger does not select text
      return;
    }
  }
  d.mode = POINTER;
  d.button = 1;
  pointer(0, 0, x0, y0);
  pointer(1, 1, x0, y0);
  pointer(0, 1, x, y);
}

void handle_gesture(const Fl_Touch_Gesture::Event &e) {
  Touch_Dispatch &d = D();
  typedef Fl_Touch_Gesture G;
  switch (e.type) {
  case G::TAP: {
    if (!gesture_window()) break;
    // Hover first: many widgets act on what the pointer moved over.
    pointer(0, 0, e.x, e.y);
    Fl_Widget *before = Fl::focus();
    pointer(1, 1, e.x, e.y, e.taps - 1);
    pointer(2, 1, e.x, e.y, 0, 1);
    leave();
    Fl_Widget *now = Fl::focus();
    if (d.keyboard && now && now != before && wants_keyboard(now)) d.keyboard(now);
    d.mode = NONE;
    break;
  }
  case G::LONG_PRESS:
    if (!gesture_window()) break;
    d.mode = HELD;
    d.button = 3;
    pointer(0, 0, e.x, e.y);
    pointer(1, 3, e.x, e.y);  // a context menu may run its own loop in here
    break;
  case G::LONG_PRESS_END:
    if (d.mode == HELD) {
      d.mode = NONE;
      // Lifted where it was held: still a click, so a menu that opened stays.
      pointer(2, 3, e.moved ? e.x : e.x0, e.moved ? e.y : e.y0, 0, !e.moved);
      leave();
    }
    break;
  case G::PAN_BEGIN:
    if (d.mode == HELD) {
      pointer(0, 3, e.x, e.y);  // FL_DRAG with button 3: slide to a menu item
    } else if (e.dx == 0 && e.dy == 0 && e.x == e.x0 && e.y == e.y0) {
      // The finger left of a pinch: go on with the scroll of the two, or
      // decide when it moves.
      if (d.scroll) {
        d.mode = SCROLL;
        d.x0 = e.x0;
        d.y0 = e.y0;
      } else {
        d.mode = PENDING;
      }
    } else {
      start_moving(e.x0, e.y0, e.x, e.y, e.dx, e.dy, e.lock);
    }
    break;
  case G::PAN:
    switch (d.mode) {
    case HELD: pointer(0, 3, e.x, e.y); break;
    case POINTER: pointer(0, 1, e.x, e.y); break;
    case SCROLL: scroll_by(e.dx, e.dy, Fl_Touch::SCROLL_UPDATE); break;
    case PENDING: start_moving(e.x - e.dx, e.y - e.dy, e.x, e.y, e.dx, e.dy, 0); break;
    default: break;
    }
    break;
  case G::PAN_END:
    if (d.mode == POINTER) {
      pointer(2, 1, e.x, e.y);
      leave();
      d.mode = NONE;
    } else if (d.mode == SCROLL) {
      if (d.scroll && (e.vx != 0 || e.vy != 0) && d.rec.fling(e.vx, e.vy, e.time)) break;
      end_scroll();
      d.mode = NONE;
    } else if (d.mode != HELD) {
      d.mode = NONE;
    }
    break;
  case G::MOMENTUM:
    if (d.mode == SCROLL && d.scroll) {
      if (!scroll_by(e.dx, e.dy, Fl_Touch::SCROLL_MOMENTUM)) d.rec.stop(e.time);
    } else {
      d.rec.stop(e.time);
    }
    break;
  case G::MOMENTUM_END:
    if (d.mode == SCROLL) {
      end_scroll();
      d.mode = NONE;
    }
    break;
  case G::PINCH_BEGIN:
    d.zoom = 0;
    d.zoom_tried = false;
    if (d.mode == HELD || d.mode == POINTER) {
      pointer(2, d.button, d.ex, d.ey);
      leave();
    }
    if (d.scroll) end_scroll();
    d.mode = NONE;
    d.x0 = e.x;
    d.y0 = e.y;
    break;
  case G::PINCH:
    zoom_by(e.scale, e.x, e.y);
    // Two fingers that move together scroll.
    if (e.dx != 0 || e.dy != 0) {
      if (d.scroll) {
        scroll_by(e.dx, e.dy, Fl_Touch::SCROLL_UPDATE);
      } else if (!d.scroll_tried && gesture_window()) {
        Fl_Widget *from = widget_at(gesture_window(), int(lround(d.x0)), int(lround(d.y0)));
        if (begin_scroll(from, d.x0, d.y0, e.dx, e.dy)) d.mode = SCROLL;
      }
    }
    break;
  case G::PINCH_END:
    d.zoom = 0;
    d.zoom_tried = false;
    // The finger that stays goes on with this scroll (PAN_BEGIN follows).
    d.mode = NONE;
    break;
  case G::TWO_FINGER_TAP:
    if (!gesture_window()) break;
    pointer(0, 0, e.x, e.y);
    pointer(1, 3, e.x, e.y);
    pointer(2, 3, e.x, e.y, 0, 1);
    leave();
    break;
  case G::CANCEL:
    if (d.mode == POINTER || d.mode == HELD) {
      pointer(2, d.button, d.ex, d.ey);
      leave();
    }
    if (d.scroll && !d.rec.flinging()) end_scroll();
    d.mode = NONE;
    break;
  case G::NONE:
    break;
  }
}

void process() {
  Touch_Dispatch &d = D();
  d.pending = false;
  Fl_Touch_Gesture::Event e;
  while (d.rec.next(e)) handle_gesture(e);
  if (d.mode == PENDING && !d.rec.touches()) d.mode = NONE;
  if (!d.rec.touches() && !d.rec.flinging() && d.scroll && d.mode != SCROLL) end_scroll();
  schedule();
}

void tick_cb(void *) {
  Touch_Dispatch &d = D();
  d.timer = false;
  d.rec.tick(now_ms());
  process();
}

void schedule() {
  Touch_Dispatch &d = D();
  double at = d.rec.next_tick();
  if (d.timer) Fl::remove_timeout(tick_cb);
  d.timer = false;
  if (at < 0) return;
  double wait = (at - now_ms()) / 1000.0;
  if (wait < 0) wait = 0;
  Fl::add_timeout(wait, tick_cb);
  d.timer = true;
}

bool known_id(int id) {
  Touch_Dispatch &d = D();
  for (int i = 0; i < d.nids; i++)
    if (d.ids[i] == id) return true;
  return false;
}

void forget_id(int id) {
  Touch_Dispatch &d = D();
  for (int i = 0; i < d.nids; i++)
    if (d.ids[i] == id) {
      d.ids[i] = d.ids[--d.nids];
      return;
    }
}

// A touch event at local time t (ms); server_time for the events sent on.
void feed(Fl_Window *window, int type, int id, double x, double y, double t,
          unsigned long server_time, unsigned state) {
  Touch_Dispatch &d = D();
  if (server_time) d.server_time = server_time;
  d.state = state;
  if (type == 0 && d.rec.touches() && (!d.window || t - d.last_event > 10000)) {
    // The window of the running gesture went away, or its fingers were
    // never seen to lift: start over.
    d.rec.cancel(t);
    Fl_Touch_Gesture::Event e;
    while (d.rec.next(e)) {}
    d.mode = NONE;
    d.nids = 0;
  }
  d.last_event = t;
  if (type == 0) {
    if (d.rec.touches() == 0) {
      d.nids = 0;
      d.window = window;
      d.hit = window ? widget_at(window, int(lround(x)), int(lround(y))) : 0;
      d.zoom = 0;
      d.zoom_tried = false;
      if (!d.rec.flinging()) {
        if (d.scroll) end_scroll();
        d.mode = NONE;
      }
    } else if (window != d.window) {
      return;  // a finger in another window while a gesture runs
    }
    if (d.nids < 16) d.ids[d.nids++] = id;
    d.rec.begin(id, x, y, t);
    process();
  } else if (type == 1) {
    if (!known_id(id)) return;
    d.rec.update(id, x, y, t);
    d.pending = true;  // sent at the end of the batch: one scroll step per batch
  } else {
    if (!known_id(id)) return;
    forget_id(id);
    d.rec.end(id, x, y, t);
    process();
  }
}

} // namespace

void Fl_Touch::platform_event(Fl_Window *window, int type, int id, double x, double y,
                              double time, unsigned state) {
  if (!enabled()) return;
  Touch_Dispatch &d = D();
  double now = now_ms();
  double t = now;
  if (time > 0) {
    // The server's clock moved onto ours: the smallest difference seen is
    // the closest to the true one (events arrive after they happen).
    double off = now - time;
    if (!d.have_offset || off < d.clock_offset || off > d.clock_offset + 5000) d.clock_offset = off;
    d.have_offset = true;
    t = time + d.clock_offset;
    if (t > now) t = now;
  }
  feed(window, type, id, x, y, t, (unsigned long)time, state);
}

void Fl_Touch::platform_flush() {
  if (D().pending) process();
}

void Fl_Touch::platform_cancel() {
  Touch_Dispatch &d = D();
  // A button that was pressed for the finger is released.
  if (d.mode == POINTER || d.mode == HELD) pointer(2, d.button, d.ex, d.ey);
  d.rec.cancel(now_ms());
  Fl_Touch_Gesture::Event e;
  while (d.rec.next(e)) {}
  if (d.scroll) end_scroll();
  d.mode = NONE;
  d.nids = 0;
  schedule();
}

void Fl_Touch::inject(Fl_Window *window, int type, int id, double x, double y, double time) {
  feed(window, type, id, x, y, time > 0 ? time : now_ms(), 0, D().state);
  platform_flush();
}

void Fl_Touch::enable(bool on) {
  Touch_Dispatch &d = D();
  if (d.enabled == on) return;
  d.enabled = on;
  if (!on) {
    d.rec.cancel(now_ms());
    Fl_Touch_Gesture::Event e;
    while (d.rec.next(e)) {}
    if (d.scroll) end_scroll();
    d.mode = NONE;
    d.nids = 0;
  }
  fl_touch_select_all(on);
}

bool Fl_Touch::enabled() { return D().enabled; }

bool Fl_Touch::available() { return fl_touch_platform_available(); }

void Fl_Touch::scale(double s) {
  Touch_Dispatch &d = D();
  d.scale = s > 0.1 ? s : 1;
  d.rec.scale(d.scale);
}

double Fl_Touch::scale() { return D().scale; }

void Fl_Touch::long_press_delay(double seconds) {
  if (seconds < 0.1) seconds = 0.1;
  D().rec.params().long_press_ms = seconds * 1000;
}

double Fl_Touch::long_press_delay() { return D().rec.params().long_press_ms / 1000; }

bool Fl_Touch::event_is_touch() { return D().touch_event; }

int Fl_Touch::scroll_phase() { return D().phase; }

static int units(double &left, int delta, int unit) {
  if (unit < 1) unit = 1;
  left += delta;
  int n = int(left / unit);  // towards zero
  left -= double(n) * unit;
  return n;
}

int Fl_Touch::scroll_units_y(int unit_px) {
  Touch_Dispatch &d = D();
  if (d.phase == SCROLL_BEGIN) d.unit_y = 0;
  return units(d.unit_y, Fl::event_dy(), unit_px);
}

int Fl_Touch::scroll_units_x(int unit_px) {
  Touch_Dispatch &d = D();
  if (d.phase == SCROLL_BEGIN) d.unit_x = 0;
  return units(d.unit_x, Fl::event_dx(), unit_px);
}

double Fl_Touch::zoom_factor() { return D().zoom_factor; }

double Fl_Touch::x() { return D().ex; }

double Fl_Touch::y() { return D().ey; }

void Fl_Touch::keyboard_hook(Keyboard_Hook hook) { D().keyboard = hook; }
