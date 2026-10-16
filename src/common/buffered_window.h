// SPDX-License-Identifier: GPL-3.0-or-later
// Double-buffered FLTK window that always uses an X pixmap as back buffer.
//
// FLTK 1.3's Fl_Double_Window draws into an Xdbe back buffer when the X
// server offers the DOUBLE-BUFFER extension (Xorg does). cairo cannot draw
// reliably into such a buffer, so SDE windows draw into a plain pixmap
// (fl_create_offscreen) and copy it to the window, which works everywhere.
#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

namespace sde {

class BufferedWindow : public Fl_Window {
 public:
  BufferedWindow(int x, int y, int w, int h, const char *label = nullptr)
      : Fl_Window(x, y, w, h, label) {}
  BufferedWindow(int w, int h, const char *label = nullptr) : Fl_Window(w, h, label) {}
  ~BufferedWindow() override { free_buffer(); }

  void hide() override {
    free_buffer();
    Fl_Window::hide();
  }
  void resize(int x, int y, int w, int h) override {
    if (w != this->w() || h != this->h()) free_buffer();
    Fl_Window::resize(x, y, w, h);
  }

  void flush() override {
    if (!shown()) return;
    make_current();
    bool fresh = false;
    if (!buffer_ || buffer_w_ != w() || buffer_h_ != h()) {
      free_buffer();
      buffer_ = fl_create_offscreen(w(), h());
      buffer_w_ = w();
      buffer_h_ = h();
      fresh = true;
    }
    // Pure expose events only need the buffer copied again. Anything else
    // redraws the whole window: SDE windows are small, and a full redraw
    // keeps custom backgrounds and child widgets consistent.
    if (fresh || (damage() & ~FL_DAMAGE_EXPOSE)) {
      damage(FL_DAMAGE_ALL);
      fl_begin_offscreen(buffer_);
      fl_push_no_clip();
      draw();
      fl_pop_clip();
      fl_end_offscreen();
    }
    fl_copy_offscreen(0, 0, w(), h(), buffer_, 0, 0);
  }

 private:
  void free_buffer() {
    if (buffer_) fl_delete_offscreen(buffer_);
    buffer_ = 0;
  }
  Fl_Offscreen buffer_ = 0;
  int buffer_w_ = 0, buffer_h_ = 0;
};

}  // namespace sde
