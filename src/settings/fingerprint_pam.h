// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Control Center > Fingerprint, "Use for": where the system's PAM stack asks
// for a finger (docs/FINGERPRINT.md). Debian builds /etc/pam.d/common-auth
// with pam-auth-update from the profiles in /usr/share/pam-configs; the
// login screen (LightDM), sudo and polkit include it. Three profiles put
// pam_fprintd there:
//
//   fprintd                  Debian's (libpam-fprintd): every service that
//                            includes common-auth
//   sde-fingerprint-sign-in  SDE's: only the services that sign in (login,
//                            lightdm, sddm, lxdm, xdm)
//   sde-fingerprint-admin    SDE's: only administrator authentication
//                            (sudo, sudo-i, polkit-1)
//
// Both switches on is Debian's own profile; one of them is SDE's profile
// for it. The state is read from what PAM uses (common-auth) and from
// pam-auth-update's record (/var/lib/pam/auth); sde-fingerprint-pam
// (fingerprint_pam_main.cpp, through pkexec) makes the changes with
// pam-auth-update. Nothing here needs a display: unit tested.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace fingerprint_pam {

extern const char *const kDebianProfile;  // "fprintd"
extern const char *const kSignInProfile;  // "sde-fingerprint-sign-in"
extern const char *const kAdminProfile;   // "sde-fingerprint-admin"

// The PAM services of each SDE profile (and of each switch).
const std::vector<std::string> &sign_in_services();
const std::vector<std::string> &admin_services();

struct State {
  bool debian_pam = false;      // common-auth and pam-auth-update's record exist
  bool module = false;          // pam_fprintd.so is installed (libpam-fprintd)
  bool debian_profile = false;  // /usr/share/pam-configs/fprintd exists
  bool sde_profiles = false;    // SDE's two profiles exist
  std::vector<std::string> enabled;  // pam-auth-update's profiles in common-auth
  bool in_common_auth = false;  // common-auth has pam_fprintd
  // pam_fprintd is in common-auth but not from a profile, or a profile is
  // recorded but its lines are gone: pam-auth-update will not touch it.
  bool by_hand = false;
  bool sign_in = false, admin = false;  // common-auth runs pam_fprintd for them
};

// From the texts of common-auth and /var/lib/pam/auth and what is installed.
State parse_state(const std::string &common_auth, const std::string &saved_auth, bool module,
                  bool debian_profile, bool sde_profiles);
// The system under |root| ("/", or a fake root in the tests).
State read_state(const std::string &root);
// Whether pam_fprintd.so is installed under |root| (the module directories
// of Debian, with or without a multiarch triplet).
bool module_installed(const std::string &root);

// The change that makes the switches |sign_in| and |admin| so.
struct Plan {
  std::vector<std::string> enable, disable;
  std::string error;  // why it cannot be made (translated), or ""
  bool nothing() const { return enable.empty() && disable.empty(); }
};
Plan plan(const State &now, bool sign_in, bool admin);
// pam-auth-update's arguments for |p| (with "--root ROOT" unless "/").
std::vector<std::string> pam_auth_update_args(const Plan &p, const std::string &root);

}  // namespace fingerprint_pam
}  // namespace sde
