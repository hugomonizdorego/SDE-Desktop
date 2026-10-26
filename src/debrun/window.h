// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Debrun's window: one package at a time. The header shows the application
// (its icon and name from the package's launcher when it has one), the
// page below shows what the package is, what it changes beyond its files,
// whether its dependencies can be met and its files, and the bar at the
// bottom says what will happen and installs (with the animated SDE logo
// while it works).
#pragma once

#include <string>

#include "about.h"

namespace sde {
namespace debrun {

AppInfo about_info();
// Shows the window for |path| (or the "choose a package" page when empty)
// and runs the event loop.
int run_window(const std::string &path);

}  // namespace debrun
}  // namespace sde
