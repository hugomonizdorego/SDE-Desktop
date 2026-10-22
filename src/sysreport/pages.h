// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Driver Manager and System Reports pages, shown by sde-drivers and
// sde-system-reports and inside SDE Welcome.
#pragma once

namespace sde {
class Page;
namespace sysreport {

void build_drivers_page(Page *page);
void build_reports_page(Page *page);
// Forget cached scans (the next build scans again).
void rescan_drivers();
void rescan_reports();

}  // namespace sysreport
}  // namespace sde
