// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Session services shared by sde-power and sde-lock: the X idle counter
// (MIT-SCREEN-SAVER), DPMS, desktop notifications and systemd-logind.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sde {
namespace session {

// Milliseconds since the last keyboard or pointer input, or -1 when the X
// server has no MIT-SCREEN-SAVER extension.
long idle_ms();
// Resets the X idle counter (what SimulateUserActivity does) and turns the
// monitors back on.
void reset_idle();
// Turns the monitors off now (DPMS). Returns false without DPMS.
bool monitors_off();
// Stops the X server's own screen saver and DPMS timers: SDE decides.
void disable_x_timers();

// org.freedesktop.Notifications.Notify. |replaces| and the returned id let
// a later message replace an earlier one. Asynchronous; |done| receives the id.
void notify(const std::string &app, const std::string &icon, const std::string &summary,
            const std::string &body, int urgency, uint32_t replaces,
            std::function<void(uint32_t)> done = nullptr, int timeout_ms = -1);

// org.freedesktop.login1.Manager.Inhibit(what, who, why, mode). Returns the
// inhibitor file descriptor (close it to release), or -1.
int logind_inhibit(const std::string &what, const std::string &who, const std::string &why,
                   const std::string &mode);
// Suspend, Hibernate, PowerOff, Reboot (asynchronous, non-interactive).
void logind_action(const std::string &method);
// "yes", "no", "challenge", "na" for CanSuspend, CanHibernate, ...
std::string logind_can(const std::string &method);

}  // namespace session
}  // namespace sde
