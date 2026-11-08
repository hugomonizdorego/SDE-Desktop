//
// SDE-FLTK touch screen support.
//
// Copyright 2026 Selviana DE Project and contributors. Same license as FLTK (LGPL-2 with the FLTK
// exceptions, see COPYING).
//
// On X11 with XInput 2.2, SDE-FLTK takes the touches of its windows itself
// instead of the X server's emulated mouse events (Fl_x.cxx), and turns them
// into what a finger means:
//
//  * a tap is a click (FL_PUSH and FL_RELEASE with mouse button 1); taps in
//    quick succession are a double click (Fl::event_clicks());
//  * a finger that moves scrolls: FL_SCROLL_GESTURE goes to the widget under
//    the finger and then to its parents until one takes it; the content
//    keeps moving after a flick (momentum) until it slows down or reaches
//    its end;
//  * a finger held still (long press) is a right click: a context menu;
//  * two fingers pinch (FL_ZOOM_GESTURE) where a widget zooms, and a
//    two-finger tap is a right click too;
//  * sliders, scroll bars, switches and other widgets that a finger drags
//    (Fl_Widget::touch_drag()) get the drag as mouse events, and a drag that
//    no widget scrolls for is sent as a mouse drag too, except on text,
//    which a finger never selects by accident.
//
// The mouse and touchpads are not affected: they never produce touch events.
//
// Fl_Touch_Gesture is the recognizer: it knows nothing about windows or
// widgets and can be tested with plain event sequences. Fl_Touch is the
// public interface for programs.
//

#ifndef Fl_Touch_H
#define Fl_Touch_H

#include "Fl_Export.H"

class Fl_Widget;
class Fl_Window;

/// Recognizes taps, long presses, pans with momentum, pinches and two-finger
/// taps in a sequence of touch points. Positions are in pixels (any origin),
/// times in milliseconds of any monotonic clock.
///
/// Feed begin()/update()/end() and call tick() when next_tick() says so,
/// then take the recognized gestures with next(). fling() starts momentum
/// after a PAN_END; whoever scrolls decides whether to.
class FL_EXPORT Fl_Touch_Gesture {
public:
  struct Params {
    double slop;           ///< px a finger may move and still tap or long press (10)
    double long_press_ms;  ///< a finger held still this long is a long press (500)
    double multi_tap_ms;   ///< a tap this soon after the previous one counts on (400)
    double multi_tap_slop; ///< ... when it is this close to it (px, 40)
    double two_tap_ms;     ///< two fingers lifted this soon are a two-finger tap (600)
    double axis_lock;      ///< a pan this many times longer along one axis than the other
                           ///< stays on that axis (2.0: within ~27 degrees; 0: never)
    double velocity_ms;    ///< the release velocity is measured over the last ms (100)
    double fling_min;      ///< px/ms: slower releases do not fling (0.25)
    double fling_max;      ///< px/ms: the fastest fling (8)
    double friction_ms;    ///< momentum slows down as exp(-t / friction_ms) (325)
    double stop_speed;     ///< px/ms: momentum ends below this speed (0.02)
    double frame_ms;       ///< ms between momentum steps (8)
    double boost_ms;       ///< a fling this soon after catching one adds its speed (600)
  };

  enum Type {
    NONE = 0,
    TAP,            ///< x, y: where the finger was; taps: 1, 2 (double tap), ...
    LONG_PRESS,     ///< x, y: a finger held still for Params::long_press_ms
    LONG_PRESS_END, ///< the held finger lifted at x, y; moved: it moved after the long press
    PAN_BEGIN,      ///< a finger moved: x0, y0 where it began, x, y now, dx, dy the
                    ///< movement past the slop (axis locked); lock: 0 free, 1 x, 2 y
    PAN,            ///< dx, dy: movement since the last event (axis locked); x, y now
    PAN_END,        ///< the finger lifted (or a second one came): vx, vy in px/ms
    MOMENTUM,       ///< after fling(): dx, dy for this step
    MOMENTUM_END,   ///< the momentum ran out or was stopped
    PINCH_BEGIN,    ///< two fingers moved; x, y: their centre
    PINCH,          ///< scale: distance ratio since the last event; dx, dy: centre
                    ///< movement; x, y: centre now
    PINCH_END,      ///< one of the two fingers lifted
    TWO_FINGER_TAP, ///< two fingers touched and lifted without moving; x, y: centre
    CANCEL          ///< the touches ended without a gesture (three fingers, cancel())
  };

  struct Event {
    Type type;
    double x, y;    ///< position now (the touch point, or the centre of two)
    double x0, y0;  ///< where the gesture began
    double dx, dy;  ///< movement, see Type
    double vx, vy;  ///< PAN_END: release velocity, px/ms
    double scale;   ///< PINCH: distance ratio since the last event
    double time;    ///< ms
    int taps;       ///< TAP: 1 single, 2 double, ...
    int lock;       ///< PAN_BEGIN, PAN, PAN_END: 0 free, 1 horizontal, 2 vertical
    int moved;      ///< LONG_PRESS_END: the finger moved after the long press
    int caught;     ///< PAN_BEGIN, TAP: the touch stopped a running momentum
  };

  Fl_Touch_Gesture();

  /// The thresholds; distances are for scale 1 (see scale()).
  Params &params() { return params_; }
  const Params &params() const { return params_; }
  /// Distances (slop, multi_tap_slop) and speeds grow with this factor:
  /// the interface scale, so that a finger moves as far at 200 %.
  void scale(double s) { scale_ = s > 0.1 ? s : 0.1; }
  double scale() const { return scale_; }

  void begin(int id, double x, double y, double t);
  void update(int id, double x, double y, double t);
  void end(int id, double x, double y, double t);
  /// Forgets every touch (the window went away); emits PAN_END, PINCH_END or
  /// CANCEL as needed.
  void cancel(double t);
  /// Runs the long press timer and the momentum.
  void tick(double t);
  /// When tick() has something to do next (ms, same clock), or -1.
  double next_tick() const;
  /// Takes the next recognized gesture; false when there is none.
  bool next(Event &e);

  /// Starts momentum with release velocity vx, vy (px/ms, as in PAN_END).
  /// Returns false when it is too slow to fling.
  bool fling(double vx, double vy, double t);
  /// Ends a running momentum (the content reached its end): MOMENTUM_END.
  void stop(double t);
  bool flinging() const { return fling_; }
  /// Number of fingers down.
  int touches() const;

private:
  enum State { IDLE, POSSIBLE, HELD, PANNING, TWO_POSSIBLE, TWO_ONE_UP, PINCHING, IGNORE };
  enum { MAX_TOUCHES = 10, MAX_SAMPLES = 32, MAX_EVENTS = 16 };
  struct Touch { int id; double x, y, x0, y0, t0; bool down; };
  struct Sample { double t, x, y; };

  Touch *find(int id);
  void emit(const Event &e);
  Event make(Type type, double t) const;
  void start_pan(Touch &f, double t, bool from_rest);
  void sample(double t, double x, double y);
  void release_velocity(double t, double &vx, double &vy) const;
  void centre(double &cx, double &cy, double &dist) const;
  double px(double v) const { return v * scale_; }

  Params params_;
  double scale_;
  State state_;
  Touch touch_[MAX_TOUCHES];
  int primary_, secondary_;   // indexes in touch_, -1 when none
  double long_press_at_;
  // pan
  int lock_;
  double pan_x_, pan_y_;      // last position sent
  bool caught_, held_moved_;
  Sample samples_[MAX_SAMPLES];
  int nsamples_, sample_head_;
  double last_move_t_;        // when the panning finger last moved
  // pinch
  double pinch_cx_, pinch_cy_, pinch_d_, two_t0_;
  bool two_moved_;
  // taps
  double last_tap_t_, last_tap_x_, last_tap_y_;
  int last_taps_;
  // momentum
  bool fling_;
  double fling_t0_, fling_vx_, fling_vy_, fling_px_, fling_py_, fling_last_;
  double caught_vx_, caught_vy_, caught_t_;
  // output queue
  Event events_[MAX_EVENTS];
  int nevents_, event_head_;
};

/// Touch screen support of SDE-FLTK for programs: settings, the state of
/// the current touch event and FL_SCROLL_GESTURE / FL_ZOOM_GESTURE helpers.
///
/// Widgets that scroll handle FL_SCROLL_GESTURE: Fl::event_dx() and
/// Fl::event_dy() are the pixels to scroll (positive: towards the end, as
/// the mouse wheel), Fl::event_x() and Fl::event_y() the point where the
/// finger touched. Return 1 when you can scroll in that direction (even if
/// less than a pixel or a line moved this time), 0 when you cannot (the
/// content fits, or it is at its end). The first widget to return 1 keeps
/// the gesture, including the momentum after a flick; momentum stops when
/// it returns 0. Fl_Scroll, Fl_Browser_ and Fl_Text_Display do this.
///
/// Widgets that zoom handle FL_ZOOM_GESTURE: zoom by Fl_Touch::zoom_factor()
/// around Fl::event_x(), Fl::event_y() and return 1.
class FL_EXPORT Fl_Touch {
public:
  /// Phase of an FL_SCROLL_GESTURE (scroll_phase()).
  enum Phase {
    SCROLL_BEGIN = 0,  ///< the first event of a scroll: is it yours?
    SCROLL_UPDATE,     ///< the finger moved
    SCROLL_MOMENTUM,   ///< the content moves on after a flick
    SCROLL_END         ///< the scroll is over (dx = dy = 0)
  };

  /// Touch events for the windows shown from now on, and for those shown
  /// already (default: on when the X server has XInput 2.2). Off: touches
  /// arrive as the X server's emulated mouse events, as without SDE-FLTK.
  static void enable(bool on);
  static bool enabled();
  /// The X server delivers touch events (XInput 2.2 or later).
  static bool available();
  /// The interface scale: the gesture distances grow with it (1 = 100 %).
  static void scale(double s);
  static double scale();
  /// How long a finger is held for a long press (seconds, default 0.5).
  static void long_press_delay(double seconds);
  static double long_press_delay();

  /// True while the event being handled comes from a touch screen (a tap,
  /// a long press, a drag sent as mouse events, FL_SCROLL_GESTURE,
  /// FL_ZOOM_GESTURE).
  static bool event_is_touch();
  /// FL_SCROLL_GESTURE: SCROLL_BEGIN, SCROLL_UPDATE, SCROLL_MOMENTUM or SCROLL_END.
  static int scroll_phase();
  /// FL_SCROLL_GESTURE for widgets that scroll by rows or lines: the
  /// whole |unit_px| units of this event's Fl::event_dy() (or event_dx())
  /// plus what was left over by the earlier events of the gesture; the rest
  /// is kept for the next one.
  static int scroll_units_y(int unit_px);
  static int scroll_units_x(int unit_px);
  /// FL_ZOOM_GESTURE: the zoom since the previous FL_ZOOM_GESTURE (1.05 =
  /// 5 % larger). Fl::event_dy() is the same as (factor - 1) * 1000.
  static double zoom_factor();

  /// The touch point of the current event (window coordinates, fractional).
  static double x();
  static double y();

  /// Hooks for the desktop (SDE): called when a tap gives the keyboard
  /// focus to a text field, so that an on-screen keyboard can be shown.
  typedef void (*Keyboard_Hook)(Fl_Widget *focus);
  static void keyboard_hook(Keyboard_Hook hook);

  /// Feeds a touch as if the X server had sent it (tests and demos):
  /// type 0 begin, 1 update, 2 end; x, y in the window's coordinates;
  /// time in ms (0: now).
  static void inject(Fl_Window *window, int type, int id, double x, double y, double time = 0);

  /// Called by the platform code (Fl_x.cxx) for each touch event and at the
  /// end of each batch of events. Not for programs.
  static void platform_event(Fl_Window *window, int type, int id, double x, double y,
                             double time, unsigned state);
  static void platform_flush();
  /// Forgets the touches without sending anything (their events stopped).
  static void platform_cancel();
  /// The first window is shown: SDE_TOUCH_FIFO=<path> (tests and
  /// measurements) reads touches from that FIFO, one per line: "b|u|e ID X
  /// Y" (begin, update, end; root coordinates), as they arrive.
  static void platform_start();
};

#endif // !Fl_Touch_H
