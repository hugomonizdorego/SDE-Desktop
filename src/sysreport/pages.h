// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The System Reports page, shown by sde-system-reports. (The Driver Manager
// has its own window since 0.1.0~alpha51, see drivers_main.cpp.)
#pragma once

namespace sde {
class Page;
namespace sysreport {

void build_reports_page(Page *page);
// Forget cached scans (the next build scans again).
void rescan_reports();

}  // namespace sysreport
}  // namespace sde
