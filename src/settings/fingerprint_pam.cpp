// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "fingerprint_pam.h"

#include <algorithm>

#include "fprint_pam.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace fingerprint_pam {

const char *const kDebianProfile = "fprintd";
const char *const kSignInProfile = "sde-fingerprint-sign-in";
const char *const kAdminProfile = "sde-fingerprint-admin";

const std::vector<std::string> &sign_in_services() {
  static const std::vector<std::string> list = {"login", "lightdm", "sddm", "lxdm", "xdm"};
  return list;
}

const std::vector<std::string> &admin_services() {
  static const std::vector<std::string> list = {"sudo", "sudo-i", "polkit-1"};
  return list;
}

namespace {

bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

bool fingerprint_profile(const std::string &name) {
  return name == kDebianProfile || name == kSignInProfile || name == kAdminProfile;
}

// |path| (absolute) under |root|.
std::string under(const std::string &root, const std::string &path) {
  if (root.empty() || root == "/") return path;
  std::string r = root;
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  return r + path;
}

}  // namespace

State parse_state(const std::string &common_auth, const std::string &saved_auth, bool module,
                  bool debian_profile, bool sde_profiles) {
  State s;
  s.debian_pam = !trim(common_auth).empty() && !trim(saved_auth).empty();
  s.module = module;
  s.debian_profile = debian_profile;
  s.sde_profiles = sde_profiles;
  // pam-auth-update's record: "Module: NAME" before the lines of each
  // profile it put into common-auth.
  for (const std::string &line : split(saved_auth, '\n')) {
    if (!starts_with(line, "Module:")) continue;
    std::string name = trim(line.substr(7));
    if (fingerprint_profile(name) && !contains(s.enabled, name)) s.enabled.push_back(name);
  }
  std::vector<fprint::PamLine> lines = fprint::pam_auth_lines(common_auth);
  for (const fprint::PamLine &l : lines)
    if (l.module == "pam_fprintd.so") s.in_common_auth = true;
  s.sign_in = fprint::runs_fprintd(lines, "lightdm");
  s.admin = fprint::runs_fprintd(lines, "sudo");
  // What the recorded profiles give; anything else was made by hand.
  bool debian = contains(s.enabled, kDebianProfile);
  bool want_sign_in = debian || contains(s.enabled, kSignInProfile);
  bool want_admin = debian || contains(s.enabled, kAdminProfile);
  s.by_hand = (s.in_common_auth && s.enabled.empty()) || want_sign_in != s.sign_in || want_admin != s.admin;
  return s;
}

bool module_installed(const std::string &root) {
  static const char *const kDirs[] = {"/usr/lib/security", "/lib/security", "/usr/lib64/security",
                                      "/lib64/security"};
  for (const char *d : kDirs)
    if (file_exists(under(root, std::string(d) + "/pam_fprintd.so"))) return true;
  // Multiarch: /usr/lib/x86_64-linux-gnu/security/pam_fprintd.so.
  for (const char *base : {"/usr/lib", "/lib"})
    for (const std::string &triplet : list_dir(under(root, base)))
      if (triplet.find("-linux-") != std::string::npos &&
          file_exists(under(root, std::string(base) + "/" + triplet + "/security/pam_fprintd.so")))
        return true;
  return false;
}

State read_state(const std::string &root) {
  std::string common, saved;
  read_file(under(root, "/etc/pam.d/common-auth"), common);
  read_file(under(root, "/var/lib/pam/auth"), saved);
  std::string configs = under(root, "/usr/share/pam-configs/");
  bool debian = file_exists(configs + kDebianProfile);
  bool sde = file_exists(configs + kSignInProfile) && file_exists(configs + kAdminProfile);
  return parse_state(common, saved, module_installed(root), debian, sde);
}

Plan plan(const State &now, bool sign_in, bool admin) {
  Plan p;
  if (!now.debian_pam) {
    p.error = _("This system does not build /etc/pam.d/common-auth with pam-auth-update (the PAM "
                "configuration of Debian and its derivatives).");
    return p;
  }
  if (now.by_hand) {
    p.error = _("pam_fprintd was put into /etc/pam.d/common-auth by hand: pam-auth-update does not change "
                "hand-made settings. Edit the file, or start over with \"sudo pam-auth-update --force\".");
    return p;
  }
  if ((sign_in || admin) && !now.module) {
    p.error = _("The PAM module of fprintd is not installed (package libpam-fprintd).");
    return p;
  }
  std::vector<std::string> want;
  if (sign_in && admin) {
    if (now.debian_profile) {
      want = {kDebianProfile};
    } else if (now.sde_profiles) {
      want = {kSignInProfile, kAdminProfile};
    } else {
      p.error = _("The pam-auth-update profile of libpam-fprintd (/usr/share/pam-configs/fprintd) is missing.");
      return p;
    }
  } else if (sign_in || admin) {
    if (!now.sde_profiles) {
      p.error = _("SDE's pam-auth-update profiles (/usr/share/pam-configs/sde-fingerprint-*) are not "
                  "installed: the fingerprint can be used for signing in and administrator tasks together "
                  "only.");
      return p;
    }
    want = {sign_in ? kSignInProfile : kAdminProfile};
  }
  for (const char *name : {kDebianProfile, kSignInProfile, kAdminProfile}) {
    bool on = contains(now.enabled, name), wanted = contains(want, name);
    if (wanted && !on) p.enable.push_back(name);
    if (!wanted && on) p.disable.push_back(name);
  }
  return p;
}

std::vector<std::string> pam_auth_update_args(const Plan &p, const std::string &root) {
  std::vector<std::string> out;
  if (!root.empty() && root != "/") out = {"--root", root};
  if (!p.enable.empty()) {
    out.push_back("--enable");
    out.insert(out.end(), p.enable.begin(), p.enable.end());
  }
  if (!p.disable.empty()) {
    out.push_back("--disable");
    out.insert(out.end(), p.disable.begin(), p.disable.end());
  }
  return out;
}

}  // namespace fingerprint_pam
}  // namespace sde
