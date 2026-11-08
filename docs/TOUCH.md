# Touch screens

Since 0.2.0 Alpha 4 SDE is made for fingers as well as for the mouse. This
page describes what a finger does, how it works, how programs (Liria
included) take part, what the session sets up, why SDE felt laggy on new
touch laptops and what was measured, and what still needs a real touch
screen to verify.

## What a finger does in SDE programs

| Gesture | Result |
| --- | --- |
| Tap | A click. Taps in quick succession (400 ms, 40 px at 100 %) are a double click. Nothing stays highlighted after the finger lifts. |
| Drag on something that scrolls | Scrolls with the finger: pages, lists, the file chooser, the Start menu, Merkadu, Niny and Xanara documents, Gallery. A drag that starts mostly along one axis stays on it. |
| Flick | The content goes on (momentum) and slows down; it stops at the end of the content, never beyond. A finger on moving content stops it (and is not a tap); a flick right after one goes faster. |
| Long press (0.5 s) | A right click: the context menu. Sliding to a menu item and lifting picks it; lifting in place leaves the menu open. |
| Two fingers | Pinch to zoom where a program zooms (Photo, Gallery viewer and grid, Xanara); moving both scrolls. A two-finger tap is a right click. |
| Drag on a slider, scroll bar or switch | Moves it along its axis. Across it, the page scrolls. |
| Drag on text | Scrolls what the text is in; a finger never selects text by accident. Tap places the cursor, double tap selects a word, long press opens Cut, Copy, Paste. |
| Drag where nothing scrolls | A mouse drag (desktop icons, drawing, crop frames). |

The mouse and touchpads never produce touch events: they behave exactly as
before.

## How it works

SDE programs are built on SDE-FLTK (`sde-fltk/`), which now speaks XInput
2.2 (`sde-fltk/FL/Fl_Touch.H`, `src/Fl_Touch*.cxx`, `src/Fl_x.cxx`):

* Every window selects XI 2.2 touch events (libXi is loaded at run time).
  The X server then sends that window touches instead of emulated mouse
  events: pointer emulation is off for it. A window that embeds windows of
  other programs keeps the emulation (`Fl_Window::touch_events(false)`): the
  panel bar does, so that tray icons still get clicks; its popups take the
  touches.
* `Fl_Touch_Gesture`, a recognizer without any X or widget code, finds taps,
  long presses, pans (slop, axis lock, release velocity by least squares over
  the last 100 ms), momentum (exponential decay, 325 ms), pinches and
  two-finger taps. Distances and speeds grow with the interface scale.
* `Fl_Touch` dispatches them. A tap is mouse button 1 pressed and released
  (made into the core events a mouse would have produced, so every widget
  and FLTK's menus work unchanged), then `FL_LEAVE`. A pan sends the new
  event `FL_SCROLL_GESTURE` to the widget under the finger and then its
  parents until one can scroll that way; that widget keeps the gesture and
  its momentum. Finger movements are merged per batch of X events: a 240 Hz
  digitizer costs one scroll step per frame.
* A window that holds the pointer grab (the Start menu, panel popups) gets
  emulated mouse events from the X server even when it selected touch
  events. While a grab lasts SDE-FLTK selects raw touch events on the root
  window, recognizes the emulated events of a finger and runs them through
  the same gestures.
* The first `XIQueryVersion` on a connection fixes its version: SDE code
  that uses XInput 2 on `fl_display` must ask for 2.2 too (the X server
  refuses a lower version with BadValue). The force-close watchdog does.

Without XInput 2.2, or with `SDE_TOUCH=0`, or with Touch gestures off in the
Control Center, nothing is selected and the X server's pointer emulation
applies, as before.

## For programs: scrolling, zooming, dragging

Stock FLTK widgets scroll by themselves: `Fl_Scroll`, `Fl_Browser_` (and
`Fl_Hold_Browser` ...) and `Fl_Text_Display`/`Fl_Text_Editor` (by lines).
A widget that scrolls its own drawing handles `FL_SCROLL_GESTURE`:

```cpp
#include <FL/Fl_Touch.H>
#include "touch.h"   // sde-common

case FL_SCROLL_GESTURE: {
  // Fl::event_dy() (and event_dx()): pixels to scroll, positive towards the
  // end like the wheel. Return 1 when the content can move that way, 0 when
  // it cannot (it fits, or it is at its end): 0 hands the scroll to the
  // parent at the start of a gesture and stops the momentum later.
  int old = scroll_, can = sde::touch_scroll_y(scroll_, content_h_ - h());
  if (scroll_ != old) redraw();
  return can;
}
case FL_ZOOM_GESTURE:   // two fingers: zoom by Fl_Touch::zoom_factor() around event_x/y
  set_zoom(zoom_ * Fl_Touch::zoom_factor(), Fl::event_x(), Fl::event_y());
  return 1;
```

* `Fl_Touch::scroll_phase()`: `SCROLL_BEGIN` (is it yours?), `SCROLL_UPDATE`,
  `SCROLL_MOMENTUM`, `SCROLL_END` (dx = dy = 0: show or hide an overlay
  scroll bar, snap to rows).
* Lists that scroll by rows: `sde::touch_scroll_rows(first, max_first,
  row_h)` keeps the pixels between rows for the next event
  (`Fl_Touch::scroll_units_y()` underneath).
* Widgets a finger drags directly: `widget->touch_drag(FL_TOUCH_DRAG_X)`
  (a switch, a handle); sliders and scroll bars do it along their axis by
  default.
* `Fl_Touch::event_is_touch()` (`sde::touch_event()`) is true while an event
  comes from a finger, e.g. to make a press act on release, or a switch that
  follows the finger (`sde::Switch` does).
* Scroll with the finger, at once: a smooth wheel animation (a "glide")
  feels like lag under a finger. Draw scrolling cheaply: `BufferedWindow`
  moves the pixels of an `Fl_Scroll` with an opaque box and draws only the
  strip that comes into view (see Lag below); a widget that draws its own
  list repaints only its own rectangle.

**Liria** (the new file manager on the SDE UI kit): its views take
`FL_SCROLL_GESTURE` as above (`sde::touch_scroll_y` on the view's pixel
offset, or `touch_scroll_rows` for row-based views), `FL_ZOOM_GESTURE` for
icon size, and get taps, double taps (open), long presses (context menu)
and rubber-band drags (a pan the view does not scroll for, e.g. when the
content fits, arrives as a mouse drag) without other code. A view that
scrolls should not start a rubber band from a touch: check
`Fl_Touch::event_is_touch()` in `FL_PUSH`.

## The session

* `startsde` sets `MOZ_USE_XINPUT2=1` (Firefox scrolls with a finger on X11
  only with it; a value the user set stays) and passes it to D-Bus
  activation; it does not pass on `GDK_CORE_DEVICE_EVENTS` and
  `QT_XCB_NO_XI2`, which turn XInput 2 (and touch) off in GTK and Qt. GTK 3/4,
  Qt 5/6 and Chromium take touches themselves.
* **sde-touchscreen** (started at login from XDG autostart, one per display)
  keeps every touch screen on its display. X maps a touch screen to the whole
  desktop, so with an external monitor touches landed beside the finger. The
  helper maps it with `xinput map-to-output` at start, when a monitor comes
  or goes (RandR) and when a touch screen appears (XInput hierarchy:
  plugged in, back after suspend). Automatic: with several monitors, a
  laptop's built-in panel (eDP, LVDS, DSI). It also shows touches: a round,
  click-through window follows each finger (raw touch events, selected only
  while the option is on).
* With a touch screen connected the panel is at least 40 px high, its status
  icons have 40 px slots and the notification button is 40 px; the Start
  menu's rows are 40 px (`[touchscreen] large_targets`).
* On-screen keyboard: when Onboard is installed, a tap that puts the cursor
  in a text field of an SDE program shows it (D-Bus, started if needed).
  GTK programs use Onboard's own auto-show (accessibility).

## Control Center > Touch Screen

`sde-settings --page touch` (also `touchscreen`): the touch screens found;
**Touch gestures** (off: a touch is a mouse click); **Long press** (short,
normal, long); **Show touches**; **Touch screen drives** (Automatic, the
whole desktop, or a connected display; applied at once); **On-screen
keyboard** (when Onboard is installed). Keys in `sde.conf`:

```ini
[touchscreen]
gestures=true          # SDE programs make gestures of touches
long_press=500         # ms (200 to 2000)
show_touches=false
output=auto            # auto, all, or an output name such as eDP-1
keyboard=auto          # auto: Onboard for text fields; off
large_targets=auto     # auto (when a touch screen is connected), true, false
```

## Lag: what was measured and fixed

Measured with `tools/touch-perf.py` on Xvfb at 2880x1800 and 200 % (the
X server draws on the CPU there, like a laptop without a working GPU
driver; the numbers compare builds, not machines):

| Scenario | before | after |
| --- | --- | --- |
| Control Center, four flicks by touch: time in draw() per frame | 12.8 ms | 1.3 ms |
| same: CPU of the X server / of sde-settings | 4.6 s / 1.5 s | 1.3 s / 0.3 s |
| two slow drags (a finger at 120 Hz): frames drawn, time per frame | 90, 21 ms | 234, 1.2 ms |
| mouse wheel, 40 steps: time per frame | 25 ms | 1.7 ms |

* **Cause**: every step of a scrolling page repainted the whole page area
  (1.9 Mpx at 200 %) through cairo. `BufferedWindow` forces a full repaint
  of damaged areas so that translucent widgets get their background, which
  also turned off `Fl_Scroll`'s own scrolling. At 120 Hz finger input the
  page fell behind the finger. Now, when the only change in a window is
  that an `Fl_Scroll` with an opaque box scrolled, the back buffer's pixels
  are moved and only the strip that comes into view is drawn; the screens
  are pixel-identical to a full repaint (checked at 100 % and 200 %).
* **Touch events** are merged per batch of X events (one scroll step per
  frame for 240 Hz digitizers); under pointer emulation every event was an
  `FL_DRAG`.
* **Compositor**: on Xvfb (OpenGL on the CPU) a full-screen frame at
  2880x1800 with SDE's OpenGL profile cost 152 ms, 134 ms without blur and
  41 ms with XRender; blur is about 12 % of an OpenGL frame there. On a
  working GPU this is small; a GPU whose OpenGL falls back to software gets
  the lite profile already (0.2.0 Alpha 3.1). Unredirected fullscreen
  windows and vsync are unchanged; they need measuring on the real laptops
  (see below).
* **Panel**: hovering the taskbar costs 0.3 ms per frame at 200 %; the Start
  menu's touch scrolling 0.2 ms per frame. The panel's raw-event listener
  wakes only for presses and touch begins, not for movement.

Run it: `dbus-run-session -- python3 tools/touch-perf.py --bin-dir build/src
--display :263 [--compositor path/to/sde-compositor] [--json out.json]`.

## Tests

* `sde-touch-gestures` (ctest, `tests/test_touch.cpp`): the recognizer fed
  with touch sequences (taps, double taps, slop, long press, axis lock,
  velocity, momentum distance and end, catching and boosting, pinch,
  two-finger tap, three fingers, cancel, scale, merging) and the helpers
  (scroll positions, rows, the display a touch screen drives, settings).
* `sde-touch-ui` (ctest, `tests/test_touch_ui.py` with `touch_probe.cpp` on
  a private Xvfb): the XI 2.2 selection of windows (and its absence with
  `touch_events(false)` and when turned off), and through `Fl_Touch::inject`
  — the same dispatch as the X server's touch events — taps, scrolling and
  momentum of `Fl_Scroll`, `Fl_Hold_Browser` and `Fl_Text_Editor`, sliders,
  text fields, long press, double tap, the mouse-drag fallback, pinch,
  two-finger tap, the on-screen keyboard hook, and the mouse through XTest
  unchanged; then `sde-touchscreen` (one per display, quiet without a touch
  screen, ends on SIGTERM).
* `SDE_TOUCH_FIFO=<fifo>` makes an SDE program read touches ("b|u|e ID X Y",
  root coordinates) from a FIFO in real time: `tools/touch-perf.py` drives
  the Control Center and the Start menu with it.

XTest cannot send touches and the test machines must not create kernel
input devices, so the X server's side cannot be tested on Xvfb.

## What needs a real touch screen

* That the X server delivers touch events to SDE windows under SDE WM: the
  WM's click-to-focus grab on client windows is replayed (ReplayPointer), and
  the touch must reach the window afterwards; also with a second finger.
* The Start menu and other grabbing popups: raw touch events telling the
  emulated mouse events of a finger apart (scrolling the app list, long
  press on a tile).
* Long-press menus: the touch sequence goes on while the menu grabs the
  pointer; lifting must leave the menu open, sliding must pick.
* Feel: slop (10 px at 100 %), long press (500 ms), momentum (325 ms), fling
  speeds; on 120 Hz panels and at 150 % and 200 %.
* `xinput map-to-output` with an external monitor connected and after
  suspend; rotated displays; Show touches placement (raw values through the
  device's matrix).
* Onboard showing for a tapped text field; the panel closing its popups on
  a tap elsewhere; Firefox with `MOZ_USE_XINPUT2=1`.
* Frame times and compositor costs on the laptops themselves (OpenGL
  profile, vsync, blur at 200 %): `tools/touch-perf.py` runs there as well.
