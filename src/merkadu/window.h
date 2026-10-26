// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's main window: a sidebar like the Control Center's (Home,
// Installed, Updates, All Software, History, Sources, In Progress and the
// departments of the applications), a bar with Back, Forward and Home, the
// page title, the search and Refresh, and the page itself. The pages are
// built by pages.cpp (packages_ui.cpp for All Software) from the Store and
// rebuilt when it changes.
#pragma once

#include <string>
#include <vector>

#include "about.h"
#include "model.h"

namespace sde {

class Page;

namespace merkadu {

struct View {
  // Explore is the Home page. Packages is All Software (|arg|: its
  // filter, see packages_view_arg()), Package the page of one package,
  // Changes the summary of the marked changes.
  enum Kind { Explore, Category, Search, Details, Installed, Updates, History, Progress, Sources, Packages, Package,
              Changes };
  Kind kind = Explore;
  std::string arg;  // category id, search text, app id, package filter or package name
  bool operator==(const View &o) const { return kind == o.kind && arg == o.arg; }
};

const char *view_id(View::Kind k);  // "explore", "category", ... (QA dump, --page)
bool view_from_id(const std::string &id, View::Kind *out);

// Opens the main window on |start| and runs the event loop.
int run_window(const View &start);

// Navigation, for the pages.
void navigate(const View &v);
// Replaces the page shown without a step in the history (filters).
void replace_view(const View &v);
void go_back();
void go_forward();
void go_home();
const View &current_view();
// Rebuilds the page shown (keeping its scroll position).
void rebuild_page();
// The search text of the bar.
void set_search_text(const std::string &text);
// The source the details page acts on for an app (the user's choice, else
// the app's main origin).
Source chosen_source(const std::string &app_id);
void choose_source(const std::string &app_id, Source s);
// Starts a change, asking first where needed (removals, classic snaps).
void request_install(const std::string &app_id, Source s);
void request_remove(const std::string &app_id, Source s);
// UI test hook: an extra line of the QA dump (Merkadu.txt), window pixels.
void qa_line(const std::string &name, int x, int y, int w, int h);
// The About information of Merkadu.
AppInfo about_info();

// Builds |v| into |page| (pages.cpp).
void build_page(Page *page, const View &v);
// The page title of |v| for the bar.
std::string view_title(const View &v);

// The uninstall dialog of `merkadu --uninstall FILE` (uninstall.cpp).
// Returns the exit status: 0 removed, 1 failed or refused, 2 cancelled.
int run_uninstall(const std::string &desktop_file);

// `merkadu --self-test [--expect REF,...] [--expect-flatpak N] [--list]`
// (selftest.cpp): the catalogs and package lists without a display.
int run_self_test(const std::vector<std::string> &args);

}  // namespace merkadu
}  // namespace sde
