// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's main window: a sidebar like the Control Center's (Explore,
// Installed, Updates, History, In Progress, Sources and the departments), a
// bar with Back, the page title and the search, and the page itself. The
// pages are built by pages.cpp from the Store and rebuilt when it changes.
#pragma once

#include <string>

#include "about.h"
#include "model.h"

namespace sde {

class Page;

namespace merkadu {

struct View {
  enum Kind { Explore, Category, Search, Details, Installed, Updates, History, Progress, Sources };
  Kind kind = Explore;
  std::string arg;  // category id, search text or app id
  bool operator==(const View &o) const { return kind == o.kind && arg == o.arg; }
};

const char *view_id(View::Kind k);  // "explore", "category", ... (QA dump, --page)
bool view_from_id(const std::string &id, View::Kind *out);

// Opens the main window on |start| and runs the event loop.
int run_window(const View &start);

// Navigation, for the pages.
void navigate(const View &v);
void go_back();
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

}  // namespace merkadu
}  // namespace sde
