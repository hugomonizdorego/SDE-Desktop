// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The "auth" lines of a PAM file such as /etc/pam.d/common-auth, and
// whether they run pam_fprintd for a PAM service (docs/FINGERPRINT.md).
// Used by SDE Screen Lock (does its password stack ask fprintd too?), the
// Control Center and its root helper (settings/fingerprint_pam.h). Plain
// text work only: no D-Bus, no display.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace fprint {

struct PamLine {
  std::string control;            // "required", "[success=2 default=ignore]"
  std::string module;             // file name: "pam_fprintd.so"
  std::vector<std::string> args;  // "max-tries=1", "timeout=10"
};
// Comments ('#' to the end of the line, as PAM reads them) are dropped.
std::vector<PamLine> pam_auth_lines(const std::string &text);

// Do |lines| run pam_fprintd for PAM service |service|? A pam_fprintd line
// runs for every service unless the line before it is the guard of SDE's
// pam-auth-update profiles, "[success=1 ...] pam_succeed_if.so ... service
// notin LIST" (or "service in LIST"), which skips it for the other services.
bool runs_fprintd(const std::vector<PamLine> &lines, const std::string &service);
// The same for the file at |path| (false when it cannot be read).
bool file_runs_fprintd(const std::string &path, const std::string &service);

}  // namespace fprint
}  // namespace sde
