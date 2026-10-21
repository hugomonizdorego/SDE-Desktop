// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Pieces of user interface shared by Gallery and Photo: a menu bar drawn in
// the SDE theme, and the About and Keyboard Shortcuts windows.
#pragma once

#include <FL/Fl_Menu_Bar.H>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "paint.h"

namespace gallery {

class MenuBar : public Fl_Menu_Bar {
 public:
  MenuBar(int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  // Applies the colours of the active SDE theme to the bar and its menus.
  void apply_theme();

 private:
  int title_at(int mx) const;
  int hover_ = -1;
};

struct AboutInfo {
  std::string name;         // "Gallery"
  std::string program;      // "sde-gallery"
  std::string version;
  std::string description;  // one or two sentences
  // Draws the application icon in a size x size box.
  std::function<void(sde::Painter &, double x, double y, double size)> logo;
  std::vector<std::string> credits;  // extra lines (libraries used ...)
};
// Modal About window with the DebION Dev-Team credits and the licence.
void show_about(const AboutInfo &info);

// Modal list of keyboard shortcuts: sections of (keys, description) rows;
// a row with empty keys starts a section titled by the description.
void show_shortcuts(const std::string &title,
                    const std::vector<std::pair<std::string, std::string>> &rows);

}  // namespace gallery
