// SPDX-License-Identifier: GPL-3.0-or-later
// Double-buffered FLTK window that always uses an X pixmap as back buffer.
//
// FLTK 1.3's Fl_Double_Window drew into an Xdbe back buffer when the X
// server offered the DOUBLE-BUFFER extension (Xorg does), and cairo cannot
// draw reliably into such a buffer, so SDE windows draw into a plain pixmap
// (fl_create_offscreen) and copy it to the window, which works everywhere.
// FLTK 1.4 (SDE-FLTK 1.4.5) has no Xdbe any more; the class stays for the
// partial repaints below.
//
// Damage handling (alpha 15):
//  * expose events only copy the exposed part of the back buffer;
//  * redraw() of the window repaints everything, as before;
//  * redraw() of a child widget, or damage_rect() on the window, repaints
//    only the damaged rectangle (grown by a few pixels so focus rings and
//    soft edges drawn just outside a widget are refreshed too). draw() still
//    runs completely, but FLTK and sde::Painter clip to that rectangle, so
//    cairo, the X server and the compositor only touch the changed pixels.
// SDE_FULL_REDRAW=1 (or set_partial_redraw(false)) restores full repaints
// for debugging. SDE_PERF_STATS=1 counts repaints (see perf.h).
#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "paint.h"
#include "perf.h"

namespace sde {

class BufferedWindow : public Fl_Window {
 public:
  BufferedWindow(int x, int y, int w, int h, const char *label = nullptr)
      : Fl_Window(x, y, w, h, label) {
    perf::enabled();  // installs the SIGUSR2 handler early when requested
  }
  BufferedWindow(int w, int h, const char *label = nullptr) : Fl_Window(w, h, label) {
    perf::enabled();
  }
  ~BufferedWindow() override { free_buffer(); }

  void hide() override {
    free_buffer();
    Fl_Window::hide();
  }
  void resize(int x, int y, int w, int h) override {
    if (w != this->w() || h != this->h()) free_buffer();
    Fl_Window::resize(x, y, w, h);
  }

  // Repaints only this part of the window at the next flush (window
  // coordinates). Use it instead of redraw() when e.g. only the hovered
  // item changed; several calls before the flush are merged.
  void damage_rect(int x, int y, int w, int h) {
    if (!shown() || !buffer_) {
      redraw();
      return;
    }
    damage(FL_DAMAGE_USER1, x, y, w, h);
  }

  // false: every damage repaints the complete window (pre-alpha-15 behavior).
  void set_partial_redraw(bool on) { partial_ = on; }

  // Implemented in paint.cpp, which is built with FLTK internals (Fl_X).
  void flush() override;

 private:
  bool partial_enabled() const {
    static const bool forced_full = [] {
      const char *v = getenv("SDE_FULL_REDRAW");
      return v && *v && *v != '0';
    }();
    return partial_ && !forced_full;
  }
  void free_buffer() {
    if (buffer_) {
      Painter::forget_target(buffer_);
      fl_delete_offscreen(buffer_);
    }
    buffer_ = 0;
  }
  Fl_Offscreen buffer_ = 0;
  int buffer_w_ = 0, buffer_h_ = 0;
  bool partial_ = true;
};

}  // namespace sde
