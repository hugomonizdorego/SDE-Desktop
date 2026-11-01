// SPDX-License-Identifier: GPL-3.0-or-later
// Control Center > Appearance > Preview (0.2.0 Alpha 3.1): the chosen look
// shown for real before it is applied.
//
// "sde-settings --theme-preview LOOK" (started by the Control Center) opens a
// window over the desktop: a picture of the screen behind it (taken as it
// opens and following the window when it is moved) with the panel, a quick
// settings popup and a window frame drawn in the chosen look, with its
// transparency, blur or Liquid Glass over the user's own wallpaper and
// windows; real SDE widgets (switches, slider, buttons, a menu) drawn by
// SDE-FLTK with the chosen theme; and real GTK widgets from a GTK 3 helper
// (sde-theme-preview-gtk) embedded with XEmbed and themed with the chosen
// GTK theme and CSS, so nothing in the user's settings changes. Its
// Transparency slider, Cancel and Apply talk to the Control Center over a
// pipe; the Control Center sends every change of the chosen look back.
#pragma once

#include <cairo.h>

#include <functional>
#include <string>

#include "appearance_draft.h"
#include "paint.h"
#include "theme.h"

namespace sde {

// "SDE Fluent · Dark · Teal · Transparency 50 %".
std::string look_summary(const AppearanceLook &look);

// A miniature desktop in theme |t| over |backdrop| (a wallpaper or a picture
// of the screen, scaled to cover; a dusky sky without one): a window with
// its frame, a menu, a quick settings popup and the panel, each with the
// opacity, blur or Liquid Glass |t| gives it.
void draw_look_scene(Painter &p, const Theme &t, double x, double y, double w, double h, double radius,
                     cairo_surface_t *backdrop, const Color &border);

// The preview window (sde-settings --theme-preview LOOK); returns the exit
// status.
int run_theme_preview(const std::string &look);

// The Control Center's side: starts the preview window and keeps it showing
// the chosen look.
class ThemePreview {
 public:
  // "apply" or "cancel" (the window closed) and "look" (its Transparency
  // slider moved), with the look the window shows.
  using Handler = std::function<void(const std::string &what, const AppearanceLook &look)>;
  static ThemePreview &get();
  bool open(const AppearanceLook &look, Handler handler);
  bool is_open() const { return pid_ > 0; }
  void update(const AppearanceLook &look);
  void close();

 private:
  static void readable(int fd, void *self);
  void finished();
  int pid_ = -1, to_ = -1, from_ = -1;
  std::string buffer_, last_sent_;
  Handler handler_;
};

}  // namespace sde
