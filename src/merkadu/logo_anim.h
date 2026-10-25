// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The animated SDE logo that Merkadu shows while it installs, removes or
// updates software: the official SDE mark (data/branding/sde-mark.svg) with
// its three RGB stripes lighting up one after the other, from left to
// right, and the monitor's green LED flickering like a disk light. The
// artwork itself is never redrawn or recoloured: the mark's own layers are
// rendered once per size (the screen and body, each stripe, the LED and the
// ink) and only their opacity changes while the work runs.
//
// It runs at 30 frames per second and repaints only its own box. With
// reduced motion (Control Center > Effects off, or a computer below the
// effects minimum) the stripes stay lit and nothing moves. When the mark is
// not installed it shows the "sde-logo" icon without animation.
#pragma once

#include <FL/Fl_Widget.H>

namespace sde {
namespace merkadu {

class LogoAnimation : public Fl_Widget {
 public:
  LogoAnimation(int x, int y, int w, int h);
  ~LogoAnimation() override;
  // Animates while |on| (and motion is allowed).
  void running(bool on);
  bool running() const { return running_; }
  void draw() override;

  // True when animations are off ([effects] in sde.conf).
  static bool reduced_motion();
  // Animation time in stripes (0-3: filling red, green, blue; 3-3.6: all
  // lit), for the tests and screenshots: MERKADU_LOGO_PHASE freezes it.
  double phase() const { return phase_; }

 private:
  static void tick(void *data);
  double phase_ = 0;
  double led_ = 1;
  bool running_ = false;
  unsigned seed_ = 1;
};

}  // namespace merkadu
}  // namespace sde
