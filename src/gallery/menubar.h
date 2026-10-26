// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
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
  std::string icon;         // "sde-gallery"; |logo| draws it when not installed
  std::vector<std::string> icon_fallbacks;
  std::string version;
  std::string description;  // one or two sentences
  // Draws the application icon in a size x size box.
  std::function<void(sde::Painter &, double x, double y, double size)> logo;
  std::vector<std::string> credits;  // extra lines (libraries used ...)
};
// Modal About window (sde::show_about): developer, license, copyright,
// trademarks and credits from the SDE legal data.
void show_about(const AboutInfo &info);

// Modal list of keyboard shortcuts: sections of (keys, description) rows;
// a row with empty keys starts a section titled by the description.
void show_shortcuts(const std::string &title,
                    const std::vector<std::pair<std::string, std::string>> &rows);

// Modal question in the SDE theme. |buttons| run left to right; the first
// is the cancel choice (also Escape), the last the default one. With
// |quality|, a 30-100 quality slider is shown and its value returned.
// Returns the index of the chosen button.
int ask(const std::string &title, const std::string &message, const std::vector<std::string> &buttons,
        int *quality = nullptr);

// A row of the Plugins window (plugins_ui.cpp).
struct PluginRow {
  std::string id;
  std::string name, description;
  int glyph = -1;        // a line icon (icon::Kind) for a built-in plugin
  std::string icon;      // a theme icon for an action plugin
  std::string detail;    // the plugin's file, or why it cannot run
  bool action = false;   // an action plugin (else built in)
  bool on = true;
  bool usable = true;    // false: it cannot run (its program is missing)
  std::function<void()> settings;  // a "Settings…" button when set
};
// The Plugins window of Photo or Gallery: a switch for every plugin
// (|rows| are changed in place) and a button that opens |folder|, the
// user's folder for action plugins. Returns true when a switch changed.
bool show_plugins_dialog(const std::string &title, const std::string &intro, std::vector<PluginRow> &rows,
                         const std::string &folder);

}  // namespace gallery
