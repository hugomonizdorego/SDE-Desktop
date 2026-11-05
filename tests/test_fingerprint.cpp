// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for the fingerprint support (docs/FINGERPRINT.md): fprintd's
// statuses and errors in words (src/common/fprint.h), PAM's auth lines and
// pam_fprintd's services (fprint_pam.h), the Control Center's PAM model and
// pam-auth-update plans (settings/fingerprint_pam.h), SDE's two
// pam-auth-update profiles as shipped (data/pam-configs), and the lock
// screen's account check after a recognized finger (with a fake PAM).
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../src/lock/auth.h"
#include "fingerprint_pam.h"
#include "fprint.h"
#include "fprint_pam.h"
#include "util.h"

using namespace sde;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static bool has(const std::vector<std::string> &v, const std::string &s) {
  for (const std::string &x : v)
    if (x == s) return true;
  return false;
}

static void test_fingers() {
  const std::vector<std::string> &f = fprint::fingers();
  CHECK(f.size() == 10);
  CHECK(f.front() == "left-little-finger" && f.back() == "right-little-finger");
  CHECK(f[4] == "left-thumb" && f[5] == "right-thumb");
  std::set<std::string> labels;
  for (const std::string &name : f) {
    CHECK(fprint::valid_finger(name));
    CHECK(fprint::left_hand(name) == starts_with(name, "left-"));
    std::string label = fprint::finger_label(name);
    CHECK(!label.empty() && label != name);
    labels.insert(label);
  }
  CHECK(labels.size() == 10);
  CHECK(!fprint::valid_finger("any"));
  CHECK(!fprint::valid_finger("right-toe"));
  CHECK(fprint::finger_label("right-toe") == "right-toe");
  CHECK(fprint::scan_type("swipe") == fprint::ScanType::swipe);
  CHECK(fprint::scan_type("press") == fprint::ScanType::press);
  CHECK(fprint::scan_type("") == fprint::ScanType::press);
  CHECK(fprint::touch_prompt(fprint::ScanType::press) != fprint::touch_prompt(fprint::ScanType::swipe));
}

static void test_enroll_statuses() {
  using K = fprint::EnrollStep;
  struct Case {
    const char *result;
    bool done;
    K::Kind kind;
    bool over;
  } cases[] = {
      {"enroll-completed", true, K::completed, true},
      {"enroll-failed", true, K::failed, true},
      {"enroll-stage-passed", false, K::stage, false},
      {"enroll-retry-scan", false, K::retry, false},
      {"enroll-swipe-too-short", false, K::retry, false},
      {"enroll-finger-not-centered", false, K::retry, false},
      {"enroll-remove-and-retry", false, K::retry, false},
      {"enroll-data-full", true, K::failed, true},
      {"enroll-duplicate", true, K::failed, true},
      {"enroll-disconnected", true, K::failed, true},
      {"enroll-unknown-error", true, K::failed, true},
  };
  std::set<std::string> texts;
  for (const Case &c : cases) {
    for (fprint::ScanType scan : {fprint::ScanType::press, fprint::ScanType::swipe}) {
      K s = fprint::enroll_step(c.result, c.done, scan);
      CHECK(s.kind == c.kind);
      CHECK(s.done == c.over);
      CHECK(!s.message.empty());
      CHECK(s.message.find("enroll-") == std::string::npos);  // words, not the code
      if (scan == fprint::ScanType::press) texts.insert(s.message);
    }
  }
  // Every status says something of its own.
  CHECK(texts.size() == sizeof(cases) / sizeof(cases[0]));
  // A final status even when fprintd forgot "done", and unknown statuses.
  CHECK(fprint::enroll_step("enroll-completed", false, fprint::ScanType::press).done);
  K unknown_final = fprint::enroll_step("enroll-new-thing", true, fprint::ScanType::press);
  CHECK(unknown_final.kind == K::failed && unknown_final.done);
  CHECK(unknown_final.message.find("enroll-new-thing") != std::string::npos);
  K unknown_retry = fprint::enroll_step("enroll-new-thing", false, fprint::ScanType::press);
  CHECK(unknown_retry.kind == K::retry && !unknown_retry.done);
  // Swipe readers are told to swipe.
  CHECK(fprint::enroll_step("enroll-retry-scan", false, fprint::ScanType::swipe).message !=
        fprint::enroll_step("enroll-retry-scan", false, fprint::ScanType::press).message);
}

static void test_verify_statuses() {
  using K = fprint::VerifyStep;
  struct Case {
    const char *result;
    bool done;
    K::Kind kind;
    bool over;
  } cases[] = {
      {"verify-match", true, K::match, true},
      {"verify-no-match", true, K::no_match, true},
      {"verify-retry-scan", false, K::retry, false},
      {"verify-swipe-too-short", false, K::retry, false},
      {"verify-finger-not-centered", false, K::retry, false},
      {"verify-remove-and-retry", false, K::retry, false},
      {"verify-disconnected", true, K::error, true},
      {"verify-unknown-error", true, K::error, true},
  };
  std::set<std::string> texts;
  for (const Case &c : cases) {
    K s = fprint::verify_step(c.result, c.done, fprint::ScanType::press);
    CHECK(s.kind == c.kind);
    CHECK(s.done == c.over);
    CHECK(!s.message.empty() && s.message.find("verify-") == std::string::npos);
    texts.insert(s.message);
  }
  CHECK(texts.size() == sizeof(cases) / sizeof(cases[0]));
  CHECK(fprint::verify_step("verify-disconnected", true, fprint::ScanType::press).device_lost);
  CHECK(!fprint::verify_step("verify-unknown-error", true, fprint::ScanType::press).device_lost);
  CHECK(fprint::verify_step("verify-match", false, fprint::ScanType::press).done);
  CHECK(fprint::verify_step("verify-other", true, fprint::ScanType::press).kind == K::error);
  CHECK(fprint::verify_step("verify-other", false, fprint::ScanType::press).kind == K::retry);
}

static void test_errors() {
  const char *names[] = {"net.reactivated.Fprint.Error.PermissionDenied", "net.reactivated.Fprint.Error.AlreadyInUse",
                         "net.reactivated.Fprint.Error.NoEnrolledPrints",
                         "net.reactivated.Fprint.Error.FingerAlreadyEnrolled",
                         "net.reactivated.Fprint.Error.InvalidFingername", "net.reactivated.Fprint.Error.NoSuchDevice",
                         "net.reactivated.Fprint.Error.PrintsNotDeleted",
                         "net.reactivated.Fprint.Error.PrintsNotDeletedFromDevice",
                         "org.freedesktop.DBus.Error.ServiceUnknown", "org.freedesktop.DBus.Error.NoReply",
                         "org.freedesktop.DBus.Error.UnknownMethod"};
  std::set<std::string> texts;
  for (const char *n : names) {
    std::string t = fprint::error_text(n, "raw text");
    CHECK(!t.empty() && t != n && t != "raw text");
    texts.insert(t);
  }
  CHECK(texts.size() == sizeof(names) / sizeof(names[0]));
  CHECK(fprint::error_text("net.reactivated.Fprint.Error.Internal", "driver said no").find("driver said no") !=
        std::string::npos);
  CHECK(fprint::error_text("com.example.Other", "something") == "something");
  CHECK(fprint::service_missing("org.freedesktop.DBus.Error.ServiceUnknown"));
  CHECK(fprint::service_missing("org.freedesktop.DBus.Error.NameHasNoOwner"));
  CHECK(!fprint::service_missing("org.freedesktop.DBus.Error.NoReply"));
  CHECK(std::string(fprint::kService) == "net.reactivated.Fprint");
}

// common-auth as pam-auth-update writes it, with |primary| lines.
static std::string common_auth(const std::string &primary) {
  return "#\n# /etc/pam.d/common-auth - authentication settings common to all services\n#\n"
         "# here are the per-package modules (the \"Primary\" block)\n" +
         primary +
         "auth\trequisite\t\t\tpam_deny.so\n"
         "# here's the fallback if no module succeeds\n"
         "auth\trequired\t\t\tpam_permit.so\n"
         "# and here are more per-package modules (the \"Additional\" block)\n"
         "# end of pam-auth-update config\n";
}

const char *kUnix = "auth\t[success=1 default=ignore]\tpam_unix.so nullok try_first_pass\n";
const char *kDebian = "auth\t[success=2 default=ignore]\tpam_fprintd.so max-tries=1 timeout=10 # debug\n";
const char *kSignIn =
    "auth\t[success=1 default=ignore]\tpam_succeed_if.so quiet service notin login:lightdm:sddm:lxdm:xdm\n"
    "auth\t[success=2 default=ignore]\tpam_fprintd.so max-tries=1 timeout=10\n";
const char *kAdmin =
    "auth\t[success=1 default=ignore]\tpam_succeed_if.so quiet service notin sudo:sudo-i:polkit-1\n"
    "auth\t[success=2 default=ignore]\tpam_fprintd.so max-tries=1 timeout=10\n";

static void test_pam_lines() {
  std::vector<fprint::PamLine> l = fprint::pam_auth_lines(common_auth(std::string(kDebian) + kUnix));
  CHECK(l.size() == 4);
  CHECK(l[0].module == "pam_fprintd.so" && l[0].control == "[success=2 default=ignore]");
  CHECK(l[0].args.size() == 2 && l[0].args[0] == "max-tries=1" && l[0].args[1] == "timeout=10");  // "# debug" dropped
  CHECK(l[1].module == "pam_unix.so" && l[1].args.size() == 2);
  // Full paths, "-auth", other types and @include are handled.
  l = fprint::pam_auth_lines("-auth optional /lib/x86_64-linux-gnu/security/pam_fprintd.so\n"
                             "account required pam_fprintd.so\n@include common-auth\n# auth required pam_x.so\n");
  CHECK(l.size() == 1 && l[0].module == "pam_fprintd.so" && l[0].control == "optional");

  // Debian's profile: every service.
  l = fprint::pam_auth_lines(common_auth(std::string(kDebian) + kUnix));
  for (const char *s : {"lightdm", "sudo", "polkit-1", "sde-lock", "su", "login"}) CHECK(fprint::runs_fprintd(l, s));
  // None.
  l = fprint::pam_auth_lines(common_auth(kUnix));
  CHECK(!fprint::runs_fprintd(l, "sudo") && !fprint::runs_fprintd(l, "sde-lock"));
  // SDE's administrator profile.
  l = fprint::pam_auth_lines(common_auth(std::string(kAdmin) + kUnix));
  CHECK(fprint::runs_fprintd(l, "sudo") && fprint::runs_fprintd(l, "sudo-i") && fprint::runs_fprintd(l, "polkit-1"));
  CHECK(!fprint::runs_fprintd(l, "lightdm") && !fprint::runs_fprintd(l, "sde-lock") && !fprint::runs_fprintd(l, "su"));
  // SDE's sign-in profile.
  l = fprint::pam_auth_lines(common_auth(std::string(kSignIn) + kUnix));
  CHECK(fprint::runs_fprintd(l, "lightdm") && fprint::runs_fprintd(l, "login"));
  CHECK(!fprint::runs_fprintd(l, "sudo") && !fprint::runs_fprintd(l, "polkit-1") && !fprint::runs_fprintd(l, "sde-lock"));
  // Both of SDE's.
  l = fprint::pam_auth_lines(common_auth(std::string(kSignIn) + kAdmin + kUnix));
  CHECK(fprint::runs_fprintd(l, "lightdm") && fprint::runs_fprintd(l, "sudo"));
  CHECK(!fprint::runs_fprintd(l, "sde-lock"));
  // "service in LIST" guards skip the listed services.
  l = fprint::pam_auth_lines("auth [success=1 default=ignore] pam_succeed_if.so service in sshd:cron\n"
                             "auth sufficient pam_fprintd.so\n");
  CHECK(!fprint::runs_fprintd(l, "sshd") && fprint::runs_fprintd(l, "sudo"));
  // Another guard (not a service list) does not limit it.
  l = fprint::pam_auth_lines("auth [success=1 default=ignore] pam_succeed_if.so user ingroup nofinger\n"
                             "auth sufficient pam_fprintd.so\n");
  CHECK(fprint::runs_fprintd(l, "sudo"));
}

static void test_pam_state() {
  using namespace fingerprint_pam;
  std::string saved_unix = "Module: unix\n[success=end default=ignore]\tpam_unix.so nullok\n";
  // Nothing enabled.
  State s = parse_state(common_auth(kUnix), saved_unix, true, true, true);
  CHECK(s.debian_pam && s.module && !s.in_common_auth && !s.by_hand && !s.sign_in && !s.admin && s.enabled.empty());
  // Debian's profile.
  s = parse_state(common_auth(std::string(kDebian) + kUnix), "Module: fprintd\nx\nModule: unix\ny\n", true, true, true);
  CHECK(s.sign_in && s.admin && !s.by_hand && s.enabled.size() == 1 && s.enabled[0] == kDebianProfile);
  // SDE's two, alone and together.
  s = parse_state(common_auth(std::string(kAdmin) + kUnix), "Module: sde-fingerprint-admin\nModule: unix\n", true, true,
                  true);
  CHECK(!s.sign_in && s.admin && !s.by_hand);
  s = parse_state(common_auth(std::string(kSignIn) + kUnix), "Module: sde-fingerprint-sign-in\nModule: unix\n", true,
                  true, true);
  CHECK(s.sign_in && !s.admin && !s.by_hand);
  s = parse_state(common_auth(std::string(kSignIn) + kAdmin + kUnix),
                  "Module: sde-fingerprint-sign-in\nModule: sde-fingerprint-admin\nModule: unix\n", true, true, true);
  CHECK(s.sign_in && s.admin && !s.by_hand && s.enabled.size() == 2);
  // pam_fprintd written by hand (no profile recorded), and a profile whose
  // lines were removed by hand.
  s = parse_state(common_auth(std::string(kDebian) + kUnix), saved_unix, true, true, true);
  CHECK(s.by_hand && s.in_common_auth && s.sign_in);
  s = parse_state(common_auth(kUnix), "Module: fprintd\nModule: unix\n", true, true, true);
  CHECK(s.by_hand && !s.in_common_auth);
  // Not Debian's PAM.
  s = parse_state("", "", false, false, false);
  CHECK(!s.debian_pam);
}

static void test_plans() {
  using namespace fingerprint_pam;
  std::string saved_unix = "Module: unix\n";
  State none = parse_state(common_auth(kUnix), saved_unix, true, true, true);
  Plan p = plan(none, true, true);
  CHECK(p.error.empty() && p.enable == std::vector<std::string>{kDebianProfile} && p.disable.empty());
  p = plan(none, false, true);
  CHECK(p.error.empty() && p.enable == std::vector<std::string>{kAdminProfile} && p.disable.empty());
  p = plan(none, true, false);
  CHECK(p.error.empty() && p.enable == std::vector<std::string>{kSignInProfile});
  p = plan(none, false, false);
  CHECK(p.error.empty() && p.nothing());

  State debian = parse_state(common_auth(std::string(kDebian) + kUnix), "Module: fprintd\nModule: unix\n", true, true, true);
  p = plan(debian, false, true);
  CHECK(p.enable == std::vector<std::string>{kAdminProfile} && p.disable == std::vector<std::string>{kDebianProfile});
  p = plan(debian, false, false);
  CHECK(p.enable.empty() && p.disable == std::vector<std::string>{kDebianProfile});
  p = plan(debian, true, true);
  CHECK(p.nothing() && p.error.empty());

  State admin = parse_state(common_auth(std::string(kAdmin) + kUnix), "Module: sde-fingerprint-admin\n", true, true, true);
  p = plan(admin, true, true);
  CHECK(p.enable == std::vector<std::string>{kDebianProfile} && p.disable == std::vector<std::string>{kAdminProfile});
  p = plan(admin, true, false);
  CHECK(p.enable == std::vector<std::string>{kSignInProfile} && p.disable == std::vector<std::string>{kAdminProfile});

  // Without Debian's profile both switches use SDE's two.
  State no_debian = parse_state(common_auth(kUnix), saved_unix, true, false, true);
  p = plan(no_debian, true, true);
  CHECK(p.error.empty() && p.enable.size() == 2 && has(p.enable, kSignInProfile) && has(p.enable, kAdminProfile));
  // Without SDE's profiles a single switch cannot be had.
  State no_sde = parse_state(common_auth(kUnix), saved_unix, true, true, false);
  CHECK(!plan(no_sde, false, true).error.empty());
  CHECK(plan(no_sde, true, true).error.empty());
  // Turning off always works, even without the module.
  State no_module = parse_state(common_auth(std::string(kDebian) + kUnix), "Module: fprintd\n", false, true, true);
  CHECK(!plan(no_module, true, true).error.empty());
  p = plan(no_module, false, false);
  CHECK(p.error.empty() && p.disable == std::vector<std::string>{kDebianProfile});
  CHECK(!plan(parse_state(common_auth(kUnix), saved_unix, false, true, true), true, false).error.empty());
  // Hand-made settings and other systems are refused.
  State hand = parse_state(common_auth(std::string(kDebian) + kUnix), saved_unix, true, true, true);
  CHECK(!plan(hand, false, false).error.empty());
  CHECK(!plan(parse_state("", "", true, true, true), true, true).error.empty());

  // pam-auth-update's arguments.
  Plan q;
  q.enable = {kAdminProfile};
  q.disable = {kDebianProfile, kSignInProfile};
  std::vector<std::string> a = pam_auth_update_args(q, "/");
  CHECK((a == std::vector<std::string>{"--enable", kAdminProfile, "--disable", kDebianProfile, kSignInProfile}));
  a = pam_auth_update_args(q, "/tmp/root");
  CHECK(a.size() == 7 && a[0] == "--root" && a[1] == "/tmp/root" && a[2] == "--enable");
  q.enable.clear();
  a = pam_auth_update_args(q, "/");
  CHECK(a.size() == 3 && a[0] == "--disable");
}

// The profiles as shipped: the services of the guards are the switches'
// lists, the module line is Debian's, and the lines pam-auth-update writes
// from them run pam_fprintd for exactly those services.
static void test_profiles(const std::string &source) {
  using namespace fingerprint_pam;
  struct P {
    const char *file;
    const std::vector<std::string> &services;
  } profiles[] = {{kSignInProfile, sign_in_services()}, {kAdminProfile, admin_services()}};
  for (const P &p : profiles) {
    std::string text;
    bool read = read_file(source + "/data/pam-configs/" + p.file, text);
    CHECK(read);
    if (!read) continue;
    CHECK(text.find("Default: no\n") != std::string::npos);
    CHECK(text.find("Conflicts: fprintd, fprint\n") != std::string::npos);
    CHECK(text.find("Auth-Type: Primary\n") != std::string::npos);
    CHECK(text.find('#') == std::string::npos);  // pam-auth-update reads no comments
    std::string list;
    for (const std::string &s : p.services) list += (list.empty() ? "" : ":") + s;
    std::string guard = "\t[success=1 default=ignore]\tpam_succeed_if.so quiet service notin " + list + "\n";
    std::string module = "\t[success=end default=ignore]\tpam_fprintd.so max-tries=1 timeout=10\n";
    CHECK(text.find("Auth:\n" + guard + module) != std::string::npos);
    // What pam-auth-update makes of it ("end": past pam_deny).
    std::string lines = "auth" + guard + "auth" + module;
    size_t end = lines.find("success=end");
    if (end != std::string::npos) lines.replace(end, 11, "success=2");
    std::vector<fprint::PamLine> l = fprint::pam_auth_lines(common_auth(lines + kUnix));
    for (const std::string &s : p.services) CHECK(fprint::runs_fprintd(l, s));
    for (const char *other : {"sde-lock", "su", "cron", "chsh"}) CHECK(!fprint::runs_fprintd(l, other));
    State st = parse_state(common_auth(lines + kUnix), std::string("Module: ") + p.file + "\nModule: unix\n", true,
                           true, true);
    CHECK(!st.by_hand);
    CHECK(st.sign_in == (std::string(p.file) == kSignInProfile));
    CHECK(st.admin == (std::string(p.file) == kAdminProfile));
  }
  // The two lists do not overlap, and the lock's own service is in none.
  for (const std::string &s : sign_in_services()) CHECK(!has(admin_services(), s));
  CHECK(!has(sign_in_services(), "sde-lock") && !has(admin_services(), "sde-lock"));
  // read_state() on the source tree's data as a root: no common-auth.
  CHECK(!read_state(source + "/data").debian_pam);
}

// --------------------------------------------- the lock's account check

namespace fakepam {
int acct_rc = PAM_SUCCESS;
int start_rc = PAM_SUCCESS;
std::string service, user;
bool authenticated = false, prompted = false;
struct pam_conv conv;

int start(const char *svc, const char *u, const struct pam_conv *c, pam_handle_t **pamh) {
  if (start_rc != PAM_SUCCESS) return start_rc;
  service = svc;
  user = u;
  conv = *c;
  static int handle;
  *pamh = reinterpret_cast<pam_handle_t *>(&handle);
  return PAM_SUCCESS;
}
int authenticate(pam_handle_t *, int) {
  authenticated = true;
  return PAM_SUCCESS;
}
int acct_mgmt(pam_handle_t *, int) {
  // A module that asks something: the check has no password to give.
  if (acct_rc == -1) {
    struct pam_message m = {PAM_PROMPT_ECHO_OFF, "Password: "};
    const struct pam_message *mp = &m;
    struct pam_response *resp = nullptr;
    prompted = true;
    int rc = conv.conv(1, &mp, &resp, conv.appdata_ptr);
    return rc == PAM_SUCCESS ? PAM_SUCCESS : PAM_AUTH_ERR;
  }
  return acct_rc;
}
int setcred(pam_handle_t *, int) { return PAM_SUCCESS; }
int end(pam_handle_t *, int) { return PAM_SUCCESS; }
const lock::PamApi api = {start, authenticate, acct_mgmt, setcred, end};
}  // namespace fakepam

static void test_account_check() {
  using lock::AuthResult;
  fakepam::acct_rc = PAM_SUCCESS;
  AuthResult r = lock::check_account(fakepam::api, "sde-lock-fingerprint", "sam");
  CHECK(r.status == AuthResult::Accepted);
  CHECK(fakepam::service == "sde-lock-fingerprint" && fakepam::user == "sam");
  CHECK(!fakepam::authenticated);  // the finger was the authentication
  fakepam::acct_rc = PAM_NEW_AUTHTOK_REQD;  // an expired password, as with the password
  CHECK(lock::check_account(fakepam::api, "sde-lock-fingerprint", "sam").status == AuthResult::Accepted);
  for (int rc : {PAM_ACCT_EXPIRED, PAM_PERM_DENIED, PAM_USER_UNKNOWN, PAM_AUTH_ERR}) {
    fakepam::acct_rc = rc;
    CHECK(lock::check_account(fakepam::api, "sde-lock-fingerprint", "sam").status == AuthResult::Rejected);
  }
  fakepam::acct_rc = PAM_SYSTEM_ERR;
  CHECK(lock::check_account(fakepam::api, "sde-lock-fingerprint", "sam").status == AuthResult::Unavailable);
  fakepam::acct_rc = -1;  // a prompt is refused (no password here)
  CHECK(lock::check_account(fakepam::api, "sde-lock-fingerprint", "sam").status == AuthResult::Rejected);
  CHECK(fakepam::prompted);
  fakepam::acct_rc = PAM_SUCCESS;
  fakepam::start_rc = PAM_SYSTEM_ERR;
  CHECK(lock::check_account(fakepam::api, "sde-lock-fingerprint", "sam").status == AuthResult::Unavailable);
  fakepam::start_rc = PAM_SUCCESS;
  CHECK(lock::check_account(fakepam::api, "sde-lock-fingerprint", "").status == AuthResult::Unavailable);
}

int main(int argc, char **argv) {
  std::string source = argc > 1 ? argv[1] : SDE_TEST_SOURCE_DIR;
  test_fingers();
  test_enroll_statuses();
  test_verify_statuses();
  test_errors();
  test_pam_lines();
  test_pam_state();
  test_plans();
  test_profiles(source);
  test_account_check();
  if (g_failures) {
    fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  printf("fingerprint: all %d checks passed\n", g_checks);
  return 0;
}
