//
// SDE-FLTK touch gesture recognizer (see FL/Fl_Touch.H).
//
// Copyright 2026 Selviana DE Project and contributors. Same license as FLTK (LGPL-2 with the FLTK
// exceptions, see COPYING).
//
// Plain computation on touch points: no windows, widgets or clocks of its
// own, so the tests feed it event sequences and check what comes out.
//

#include <FL/Fl_Touch.H>

#include <math.h>
#include <string.h>

Fl_Touch_Gesture::Fl_Touch_Gesture() {
  params_.slop = 10;
  params_.long_press_ms = 500;
  params_.multi_tap_ms = 400;
  params_.multi_tap_slop = 40;
  params_.two_tap_ms = 600;
  params_.axis_lock = 2.0;
  params_.velocity_ms = 100;
  params_.fling_min = 0.25;
  params_.fling_max = 8;
  params_.friction_ms = 325;
  params_.stop_speed = 0.02;
  params_.frame_ms = 8;
  params_.boost_ms = 600;
  scale_ = 1;
  state_ = IDLE;
  memset(touch_, 0, sizeof touch_);
  primary_ = secondary_ = -1;
  long_press_at_ = -1;
  lock_ = 0;
  pan_x_ = pan_y_ = 0;
  caught_ = held_moved_ = false;
  nsamples_ = sample_head_ = 0;
  last_move_t_ = 0;
  pinch_cx_ = pinch_cy_ = pinch_d_ = two_t0_ = 0;
  two_moved_ = false;
  last_tap_t_ = -1e9;
  last_tap_x_ = last_tap_y_ = 0;
  last_taps_ = 0;
  fling_ = false;
  fling_t0_ = fling_vx_ = fling_vy_ = fling_px_ = fling_py_ = fling_last_ = 0;
  caught_vx_ = caught_vy_ = 0;
  caught_t_ = -1e9;
  nevents_ = event_head_ = 0;
}

int Fl_Touch_Gesture::touches() const {
  int n = 0;
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_[i].down) n++;
  return n;
}

Fl_Touch_Gesture::Touch *Fl_Touch_Gesture::find(int id) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_[i].down && touch_[i].id == id) return &touch_[i];
  return 0;
}

Fl_Touch_Gesture::Event Fl_Touch_Gesture::make(Type type, double t) const {
  Event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.time = t;
  e.scale = 1;
  e.lock = lock_;
  return e;
}

void Fl_Touch_Gesture::emit(const Event &e) {
  if (nevents_ == MAX_EVENTS) {
    // Nobody reads: drop the oldest, but keep the movement it carried.
    int second = (event_head_ + 1) % MAX_EVENTS;
    Event &old = events_[event_head_];
    if ((old.type == PAN || old.type == MOMENTUM) && events_[second].type == old.type) {
      events_[second].dx += old.dx;
      events_[second].dy += old.dy;
    }
    event_head_ = second;
    nevents_--;
  }
  // Consecutive movements merge, so a slow reader gets one step per frame.
  if (nevents_) {
    Event &last = events_[(event_head_ + nevents_ - 1) % MAX_EVENTS];
    if ((e.type == PAN || e.type == MOMENTUM) && last.type == e.type) {
      last.dx += e.dx;
      last.dy += e.dy;
      last.x = e.x;
      last.y = e.y;
      last.time = e.time;
      return;
    }
    if (e.type == PINCH && last.type == PINCH) {
      last.dx += e.dx;
      last.dy += e.dy;
      last.scale *= e.scale;
      last.x = e.x;
      last.y = e.y;
      last.time = e.time;
      return;
    }
  }
  events_[(event_head_ + nevents_) % MAX_EVENTS] = e;
  nevents_++;
}

bool Fl_Touch_Gesture::next(Event &e) {
  if (!nevents_) return false;
  e = events_[event_head_];
  event_head_ = (event_head_ + 1) % MAX_EVENTS;
  nevents_--;
  return true;
}

void Fl_Touch_Gesture::sample(double t, double x, double y) {
  if (!nsamples_) {
    last_move_t_ = t;
  } else {
    const Sample &prev = samples_[(sample_head_ - 1 + MAX_SAMPLES) % MAX_SAMPLES];
    if (fabs(x - prev.x) >= 1 || fabs(y - prev.y) >= 1) last_move_t_ = t;
  }
  samples_[sample_head_] = Sample{t, x, y};
  sample_head_ = (sample_head_ + 1) % MAX_SAMPLES;
  if (nsamples_ < MAX_SAMPLES) nsamples_++;
}

// Least-squares slope of the samples of the last velocity_ms before |t|.
void Fl_Touch_Gesture::release_velocity(double t, double &vx, double &vy) const {
  vx = vy = 0;
  double st = 0, sx = 0, sy = 0;
  int n = 0;
  for (int k = 0; k < nsamples_; k++) {
    const Sample &s = samples_[(sample_head_ - 1 - k + MAX_SAMPLES) % MAX_SAMPLES];
    if (t - s.t > params_.velocity_ms) break;
    st += s.t; sx += s.x; sy += s.y; n++;
  }
  // A finger that stopped before it lifted does not fling.
  if (n < 2 || t - last_move_t_ > params_.velocity_ms / 2) return;
  double mt = st / n, mx = sx / n, my = sy / n;
  double stt = 0, stx = 0, sty = 0;
  for (int k = 0; k < n; k++) {
    const Sample &s = samples_[(sample_head_ - 1 - k + MAX_SAMPLES) % MAX_SAMPLES];
    stt += (s.t - mt) * (s.t - mt);
    stx += (s.t - mt) * (s.x - mx);
    sty += (s.t - mt) * (s.y - my);
  }
  if (stt > 1e-6) {
    vx = stx / stt;
    vy = sty / stt;
  }
  if (lock_ == 1) vy = 0;
  if (lock_ == 2) vx = 0;
}

void Fl_Touch_Gesture::centre(double &cx, double &cy, double &dist) const {
  const Touch &a = touch_[primary_], &b = touch_[secondary_];
  cx = (a.x + b.x) / 2;
  cy = (a.y + b.y) / 2;
  dist = hypot(a.x - b.x, a.y - b.y);
  if (dist < 1) dist = 1;
}

// The finger |f| starts to pan. from_rest: after a pinch, the finger that
// stayed down goes on from where it is (no slop, no axis lock).
void Fl_Touch_Gesture::start_pan(Touch &f, double t, bool from_rest) {
  double mx = f.x - f.x0, my = f.y - f.y0;
  lock_ = 0;
  double ax = fabs(mx), ay = fabs(my);
  if (!from_rest && params_.axis_lock > 0) {
    if (ay >= params_.axis_lock * ax) lock_ = 2;
    else if (ax >= params_.axis_lock * ay) lock_ = 1;
  }
  Event e = make(PAN_BEGIN, t);
  e.x0 = f.x0;
  e.y0 = f.y0;
  e.x = f.x;
  e.y = f.y;
  e.caught = caught_;
  if (!from_rest) {
    // Only the movement past the slop scrolls, so the content does not jump
    // when the pan begins; the finger and the content then move together.
    double len = hypot(mx, my), keep = len > 0 ? (len - px(params_.slop)) / len : 0;
    if (keep < 0) keep = 0;
    e.dx = lock_ == 2 ? 0 : mx * keep;
    e.dy = lock_ == 1 ? 0 : my * keep;
  }
  emit(e);
  pan_x_ = f.x;
  pan_y_ = f.y;
  nsamples_ = sample_head_ = 0;
  sample(t, f.x, f.y);
  state_ = PANNING;
}

void Fl_Touch_Gesture::begin(int id, double x, double y, double t) {
  if (find(id)) end(id, x, y, t);  // a lost end: start again
  int slot = -1;
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (!touch_[i].down) { slot = i; break; }
  if (slot < 0) return;  // more fingers than anyone has
  Touch &f = touch_[slot];
  f.id = id;
  f.x = f.x0 = x;
  f.y = f.y0 = y;
  f.t0 = t;
  f.down = true;

  switch (state_) {
  case IDLE:
    primary_ = slot;
    secondary_ = -1;
    caught_ = false;
    held_moved_ = false;
    if (fling_) {
      // A finger on moving content stops it, and is not a tap.
      double s = exp(-(t - fling_t0_) / params_.friction_ms);
      caught_vx_ = fling_vx_ * s;
      caught_vy_ = fling_vy_ * s;
      caught_t_ = t;
      fling_ = false;
      emit(make(MOMENTUM_END, t));
      caught_ = true;
    }
    long_press_at_ = t + params_.long_press_ms;
    state_ = POSSIBLE;
    break;
  case POSSIBLE:
  case PANNING:
    if (state_ == PANNING) {
      Event e = make(PAN_END, t);
      e.x = touch_[primary_].x;
      e.y = touch_[primary_].y;
      emit(e);  // no velocity: the second finger takes over
    }
    secondary_ = slot;
    long_press_at_ = -1;
    {
      double cx, cy, d;
      centre(cx, cy, d);
      pinch_cx_ = cx;
      pinch_cy_ = cy;
      pinch_d_ = d;
    }
    two_t0_ = touch_[primary_].t0;
    two_moved_ = state_ == PANNING;
    lock_ = 0;
    state_ = TWO_POSSIBLE;
    break;
  case TWO_POSSIBLE:
  case TWO_ONE_UP:
  case PINCHING:
    if (state_ == PINCHING) {
      // A third finger: a pinch keeps its two.
      break;
    }
    emit(make(CANCEL, t));
    state_ = IGNORE;
    break;
  case HELD:
  case IGNORE:
    break;
  }
}

void Fl_Touch_Gesture::update(int id, double x, double y, double t) {
  Touch *f = find(id);
  if (!f) return;
  f->x = x;
  f->y = y;
  int slot = int(f - touch_);
  switch (state_) {
  case POSSIBLE:
    if (slot == primary_ && hypot(x - f->x0, y - f->y0) > px(params_.slop)) {
      long_press_at_ = -1;
      start_pan(*f, t, false);
    }
    break;
  case HELD:
    if (slot != primary_) break;
    if (!held_moved_ && hypot(x - f->x0, y - f->y0) > px(params_.slop)) {
      held_moved_ = true;
      Event e = make(PAN_BEGIN, t);
      e.x0 = f->x0; e.y0 = f->y0; e.x = x; e.y = y;
      e.dx = x - f->x0; e.dy = y - f->y0;
      lock_ = 0;
      e.lock = 0;
      emit(e);
      pan_x_ = x; pan_y_ = y;
    } else if (held_moved_ && (x != pan_x_ || y != pan_y_)) {
      Event e = make(PAN, t);
      e.x = x; e.y = y; e.dx = x - pan_x_; e.dy = y - pan_y_;
      e.lock = 0;
      emit(e);
      pan_x_ = x; pan_y_ = y;
    }
    break;
  case PANNING:
    if (slot != primary_) break;
    {
      Event e = make(PAN, t);
      e.x = x;
      e.y = y;
      e.dx = lock_ == 2 ? 0 : x - pan_x_;
      e.dy = lock_ == 1 ? 0 : y - pan_y_;
      pan_x_ = x;
      pan_y_ = y;
      sample(t, x, y);
      if (e.dx != 0 || e.dy != 0) emit(e);
    }
    break;
  case TWO_POSSIBLE:
  case PINCHING:
    if (slot != primary_ && slot != secondary_) break;
    {
      double cx, cy, d;
      centre(cx, cy, d);
      if (state_ == TWO_POSSIBLE) {
        bool moved = false;
        for (int s = 0; s < 2; s++) {
          const Touch &g = touch_[s ? secondary_ : primary_];
          if (hypot(g.x - g.x0, g.y - g.y0) > px(params_.slop)) moved = true;
        }
        if (!moved) break;
        Event b = make(PINCH_BEGIN, t);
        b.x0 = pinch_cx_; b.y0 = pinch_cy_; b.x = cx; b.y = cy;
        emit(b);
        state_ = PINCHING;
        two_moved_ = true;
      }
      Event e = make(PINCH, t);
      e.x = cx;
      e.y = cy;
      e.dx = cx - pinch_cx_;
      e.dy = cy - pinch_cy_;
      e.scale = d / pinch_d_;
      pinch_cx_ = cx;
      pinch_cy_ = cy;
      pinch_d_ = d;
      if (e.dx != 0 || e.dy != 0 || e.scale != 1) emit(e);
    }
    break;
  case TWO_ONE_UP:
    if (slot == primary_ && hypot(x - f->x0, y - f->y0) > px(params_.slop)) {
      // The finger left of a two-finger tap moves: it pans instead.
      f->x0 = x;
      f->y0 = y;
      start_pan(*f, t, true);
    }
    break;
  case IDLE:
  case IGNORE:
    break;
  }
}

void Fl_Touch_Gesture::end(int id, double x, double y, double t) {
  Touch *f = find(id);
  if (!f) return;
  update(id, x, y, t);
  if (!f->down) return;  // update() ended it
  int slot = int(f - touch_);
  f->down = false;
  switch (state_) {
  case POSSIBLE:
    if (slot != primary_) break;
    long_press_at_ = -1;
    if (!caught_) {
      Event e = make(TAP, t);
      e.x = e.x0 = f->x0;
      e.y = e.y0 = f->y0;
      bool again = f->t0 - last_tap_t_ <= params_.multi_tap_ms &&
                   hypot(f->x0 - last_tap_x_, f->y0 - last_tap_y_) <= px(params_.multi_tap_slop);
      e.taps = again ? last_taps_ + 1 : 1;
      last_taps_ = e.taps;
      last_tap_t_ = t;
      last_tap_x_ = f->x0;
      last_tap_y_ = f->y0;
      emit(e);
    } else {
      emit(make(CANCEL, t));
    }
    state_ = IDLE;
    break;
  case HELD:
    if (slot != primary_) break;
    {
      Event e = make(LONG_PRESS_END, t);
      e.x = x; e.y = y; e.x0 = f->x0; e.y0 = f->y0;
      e.moved = held_moved_;
      emit(e);
    }
    state_ = touches() ? IGNORE : IDLE;
    break;
  case PANNING:
    if (slot != primary_) break;
    {
      // update() above took the last position as a sample.
      Event e = make(PAN_END, t);
      e.x = x;
      e.y = y;
      release_velocity(t, e.vx, e.vy);
      emit(e);
    }
    state_ = touches() ? IGNORE : IDLE;
    break;
  case TWO_POSSIBLE:
    if (slot != primary_ && slot != secondary_) break;
    // One of two still fingers lifted: a two-finger tap if the other
    // follows soon; it pans if it moves.
    if (slot == primary_) primary_ = secondary_;
    secondary_ = -1;
    state_ = TWO_ONE_UP;
    break;
  case TWO_ONE_UP:
    if (slot != primary_) break;
    if (!two_moved_ && t - two_t0_ <= params_.two_tap_ms) {
      Event e = make(TWO_FINGER_TAP, t);
      e.x = e.x0 = pinch_cx_;
      e.y = e.y0 = pinch_cy_;
      emit(e);
    } else {
      emit(make(CANCEL, t));
    }
    state_ = touches() ? IGNORE : IDLE;
    break;
  case PINCHING:
    if (slot != primary_ && slot != secondary_) break;
    {
      Event e = make(PINCH_END, t);
      e.x = pinch_cx_;
      e.y = pinch_cy_;
      emit(e);
      // The finger that stays goes on as a pan (zoom, then move the picture).
      int rest = slot == primary_ ? secondary_ : primary_;
      primary_ = rest;
      secondary_ = -1;
      Touch &r = touch_[rest];
      r.x0 = r.x;
      r.y0 = r.y;
      start_pan(r, t, true);
    }
    break;
  case IGNORE:
    if (!touches()) state_ = IDLE;
    break;
  case IDLE:
    break;
  }
}

void Fl_Touch_Gesture::cancel(double t) {
  switch (state_) {
  case PANNING: {
    Event e = make(PAN_END, t);
    e.x = pan_x_;
    e.y = pan_y_;
    emit(e);
    break;
  }
  case PINCHING:
    emit(make(PINCH_END, t));
    break;
  case HELD: {
    Event e = make(LONG_PRESS_END, t);
    e.moved = 1;
    emit(e);
    break;
  }
  case POSSIBLE:
  case TWO_POSSIBLE:
  case TWO_ONE_UP:
    emit(make(CANCEL, t));
    break;
  case IDLE:
  case IGNORE:
    break;
  }
  for (int i = 0; i < MAX_TOUCHES; i++) touch_[i].down = false;
  primary_ = secondary_ = -1;
  long_press_at_ = -1;
  state_ = IDLE;
  if (fling_) stop(t);
}

void Fl_Touch_Gesture::tick(double t) {
  if (state_ == POSSIBLE && long_press_at_ >= 0 && t >= long_press_at_) {
    long_press_at_ = -1;
    const Touch &f = touch_[primary_];
    Event e = make(LONG_PRESS, t);
    e.x = e.x0 = f.x0;
    e.y = e.y0 = f.y0;
    emit(e);
    state_ = HELD;
    held_moved_ = false;
  }
  if (fling_ && t > fling_last_) {
    double k = t - fling_t0_;
    double tau = params_.friction_ms;
    double gone = 1 - exp(-k / tau);
    double px_ = fling_vx_ * tau * gone, py_ = fling_vy_ * tau * gone;
    Event e = make(MOMENTUM, t);
    e.dx = px_ - fling_px_;
    e.dy = py_ - fling_py_;
    fling_px_ = px_;
    fling_py_ = py_;
    fling_last_ = t;
    if (e.dx != 0 || e.dy != 0) emit(e);
    double speed = hypot(fling_vx_, fling_vy_) * exp(-k / tau);
    if (speed < px(params_.stop_speed)) {
      fling_ = false;
      emit(make(MOMENTUM_END, t));
    }
  }
}

double Fl_Touch_Gesture::next_tick() const {
  double at = -1;
  if (state_ == POSSIBLE && long_press_at_ >= 0) at = long_press_at_;
  if (fling_) {
    double step = fling_last_ + params_.frame_ms;
    if (at < 0 || step < at) at = step;
  }
  return at;
}

bool Fl_Touch_Gesture::fling(double vx, double vy, double t) {
  double v = hypot(vx, vy), vmax = px(params_.fling_max);
  if (v < px(params_.fling_min)) return false;
  if (v > vmax) {
    vx *= vmax / v;
    vy *= vmax / v;
  }
  // A flick right after catching a fling in the same direction goes on
  // faster: long lists are crossed with a few flicks.
  if (t - caught_t_ <= params_.boost_ms && vx * caught_vx_ + vy * caught_vy_ > 0) {
    vx += caught_vx_;
    vy += caught_vy_;
    v = hypot(vx, vy);
    if (v > vmax) {
      vx *= vmax / v;
      vy *= vmax / v;
    }
  }
  caught_t_ = -1e9;
  fling_ = true;
  fling_t0_ = fling_last_ = t;
  fling_vx_ = vx;
  fling_vy_ = vy;
  fling_px_ = fling_py_ = 0;
  return true;
}

void Fl_Touch_Gesture::stop(double t) {
  if (!fling_) return;
  fling_ = false;
  emit(make(MOMENTUM_END, t));
}
