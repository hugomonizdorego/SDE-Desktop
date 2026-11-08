// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-logout --security: the security options of Ctrl+Alt+Delete (0.2.0
// Alpha 4, docs/WINDOWS.md), as on Windows: Lock, Switch User, Log Out,
// Change a Password and Task Manager (System Monitor) over the dimmed
// screen, Cancel, and the power options (Sleep, Restart, Shut Down) at the
// bottom right. SDE_LOGOUT_QA_DIR: its buttons are written to
// <dir>/security.txt; SDE_LOGOUT_DRY_RUN=1 prints the command of a choice
// instead of running it (the UI tests).
#pragma once

namespace sde {

int run_security_screen();

}  // namespace sde
