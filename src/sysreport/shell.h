// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Window shell shared by SDE Welcome, SDE Driver Manager and SDE System
// Reports: an optional sidebar of pages on the left and a scrolling
// sde-settings style Page (src/settings/ui.h) on the right, plus helpers for
// the action rows of these tools (buttons, privileged commands).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "about.h"
#include "config.h"
#include "theme.h"
#include "ui.h"

namespace sde {
namespace shell {

struct PageDef {
  std::string id;
  std::string title;
  std::vector<std::string> icons;  // freedesktop icon names for the sidebar
  std::function<void(Page *)> build;
};

struct Options {
  std::string title;    // window title
  std::string xclass;   // WM_CLASS
  AppInfo about;
  std::vector<PageDef> pages;  // more than one: a sidebar is shown
  std::string start;           // page id
  int width = 940, height = 680;  // unscaled
  // Bottom of the sidebar: a switch with this label (empty: none).
  std::string sidebar_switch;
  bool sidebar_switch_value = false;
  std::function<void(bool)> sidebar_switch_changed;
};

// Loads the configuration and theme (call before creating widgets).
void init();
Config &config();
const Theme &theme();

// Creates the window, shows it and runs the event loop.
int run(Options opts);

// Rebuilds the visible page (keeps the scroll position).
void refresh();
// Switches to another page.
void show_page(const std::string &id);
// Saves sde.conf and tells every SDE program (GTK settings, window frames,
// compositor, _SDE_CONFIG_SERIAL). |retheme|: re-reads the theme and
// rebuilds this window in it.
void apply_config(bool retheme = false);

// ---- page helpers

// A Mint-style step: icon, title, explanation and buttons on the right.
struct Action {
  std::string label;
  std::function<void()> run;
  bool primary = false;
  bool enabled = true;
};
void step(Page *page, const std::vector<std::string> &icons, const std::string &title,
          const std::string &text, const std::vector<Action> &actions,
          const std::string &status = "", const Color *status_color = nullptr);

// A row with one button on the right.
FlatButton *button_row(Page *page, const std::string &label, const std::string &button,
                       std::function<void()> run, const std::string &hint = "", bool primary = false);

// Large banner at the top of a page.
void hero(Page *page, const std::vector<std::string> &icons, const std::string &title,
          const std::string &subtitle);

// Starts a program; tells the user when it is not installed.
bool launch(const std::vector<std::string> &argv);
// Opens a URL or file.
void open_url(const std::string &url);
// Copies text to the clipboard (both selections).
void copy_text(const std::string &text);

// Runs |argv| as root through pkexec without blocking the window. |what|
// is shown while it runs ("Installing timeshift..."); |done| gets the
// output and exit status (126/127: authorization refused or failed).
void run_admin(const std::vector<std::string> &argv, const std::string &what,
               std::function<void(const std::string &output, int status)> done);
// "pkexec env DEBIAN_FRONTEND=noninteractive apt-get install -y PKGS",
// then refresh() and a message on failure.
void install_packages(const std::vector<std::string> &packages, const std::string &what);
// True while run_admin() is busy; the text describes it.
bool busy(std::string *what = nullptr);

// Package installed (dpkg-query), cached until refresh_package_cache().
bool package_installed(const std::string &package);
void refresh_package_cache();

}  // namespace shell
}  // namespace sde
