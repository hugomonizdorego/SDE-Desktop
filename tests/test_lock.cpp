// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Unit tests for SDE Screen Lock and SDE Power: the PAM conversation (with
// a fake PAM stack, built only here), the password buffer, attempt delays,
// inhibitor tracking, the crash watchdog, idle stages, battery levels and
// brightness steps.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/lock/auth.h"
#include "../src/lock/policy.h"
#include "power.h"
#include "../src/power/policy.h"

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

// ------------------------------------------------------------ fake PAM

namespace fake {

struct Handle {
  std::string service, user;
  struct pam_conv conv;
  bool authenticated = false;
  int ended_with = -1;
};

// Behaviour of the fake stack for the next authentication.
std::string g_secret = "correct horse";
bool g_send_info = false;    // a PAM_TEXT_INFO before the prompt
bool g_two_prompts = false;  // e.g. password + one-time code
int g_acct = PAM_SUCCESS;
int g_setcred_calls = 0;
int g_start_rc = PAM_SUCCESS;
Handle *g_last = nullptr;

int start(const char *service, const char *user, const struct pam_conv *conv, pam_handle_t **pamh) {
  if (g_start_rc != PAM_SUCCESS) return g_start_rc;
  auto *h = new Handle;
  h->service = service;
  h->user = user;
  h->conv = *conv;
  g_last = h;
  *pamh = reinterpret_cast<pam_handle_t *>(h);
  return PAM_SUCCESS;
}

int ask(Handle *h, int style, const char *text, std::string *answer) {
  struct pam_message m = {style, text};
  const struct pam_message *list[] = {&m};
  struct pam_response *resp = nullptr;
  int rc = h->conv.conv(1, list, &resp, h->conv.appdata_ptr);
  if (rc != PAM_SUCCESS) return rc;
  if (resp) {
    if (resp[0].resp) {
      if (answer) *answer = resp[0].resp;
      free(resp[0].resp);
    }
    free(resp);
  }
  return PAM_SUCCESS;
}

int authenticate(pam_handle_t *pamh, int) {
  auto *h = reinterpret_cast<Handle *>(pamh);
  if (g_send_info && ask(h, PAM_TEXT_INFO, "Touch the security key", nullptr) != PAM_SUCCESS)
    return PAM_CONV_ERR;
  std::string answer;
  if (ask(h, PAM_PROMPT_ECHO_OFF, "Password: ", &answer) != PAM_SUCCESS) return PAM_CONV_ERR;
  if (g_two_prompts) {
    std::string second;
    if (ask(h, PAM_PROMPT_ECHO_ON, "Code: ", &second) != PAM_SUCCESS) return PAM_CONV_ERR;
    if (second != answer) return PAM_AUTH_ERR;
  }
  if (answer != g_secret) {
    ask(h, PAM_ERROR_MSG, "Authentication failure", nullptr);
    return PAM_AUTH_ERR;
  }
  h->authenticated = true;
  return PAM_SUCCESS;
}

int acct_mgmt(pam_handle_t *, int) { return g_acct; }
int setcred(pam_handle_t *, int flags) {
  if (flags & PAM_REFRESH_CRED) ++g_setcred_calls;
  return PAM_SUCCESS;
}
int end(pam_handle_t *pamh, int status) {
  auto *h = reinterpret_cast<Handle *>(pamh);
  h->ended_with = status;
  return PAM_SUCCESS;
}

const lock::PamApi api = {start, authenticate, acct_mgmt, setcred, end};

}  // namespace fake

static void test_conversation() {
  using namespace lock;
  std::vector<std::string> messages;
  Conversation data;
  data.password = "pw";
  data.messages = &messages;
  struct pam_message a = {PAM_TEXT_INFO, "hello"}, b = {PAM_PROMPT_ECHO_OFF, "Password:"},
                     c = {PAM_ERROR_MSG, "bad"};
  const struct pam_message *list[] = {&a, &b, &c};
  struct pam_response *resp = nullptr;
  CHECK(conversation(3, list, &resp, &data) == PAM_SUCCESS);
  CHECK(resp != nullptr);
  if (resp) {
    CHECK(resp[0].resp == nullptr);
    CHECK(resp[1].resp && std::string(resp[1].resp) == "pw");
    CHECK(resp[2].resp == nullptr);
    for (int i = 0; i < 3; ++i) free(resp[i].resp);
    free(resp);
  }
  CHECK(messages.size() == 2 && messages[0] == "hello" && messages[1] == "bad");
  CHECK(data.prompts == 1);
  // Invalid input.
  CHECK(conversation(0, list, &resp, &data) == PAM_CONV_ERR);
  CHECK(conversation(1, list, &resp, nullptr) == PAM_CONV_ERR);
  struct pam_message weird = {12345, "?"};
  const struct pam_message *wl[] = {&b, &weird};
  resp = reinterpret_cast<struct pam_response *>(0x1);
  CHECK(conversation(2, wl, &resp, &data) == PAM_CONV_ERR);
  CHECK(resp == nullptr);  // nothing leaks to PAM on failure
  // No password available: a prompt is a conversation error.
  Conversation none;
  CHECK(conversation(1, list + 1, &resp, &none) == PAM_CONV_ERR);
}

static void test_authenticate() {
  using namespace lock;
  fake::g_secret = "correct horse";
  AuthResult r = authenticate(fake::api, "sde-lock", "selviana", "correct horse");
  CHECK(r.status == AuthResult::Accepted);
  CHECK(fake::g_last && fake::g_last->service == "sde-lock" && fake::g_last->user == "selviana");
  CHECK(fake::g_last && fake::g_last->ended_with == PAM_SUCCESS);
  CHECK(fake::g_setcred_calls == 1);
  delete fake::g_last;

  r = authenticate(fake::api, "sde-lock", "selviana", "wrong");
  CHECK(r.status == AuthResult::Rejected);
  CHECK(!r.messages.empty() && r.messages.back() == "Authentication failure");
  CHECK(fake::g_setcred_calls == 1);  // no credentials refreshed on failure
  delete fake::g_last;

  fake::g_send_info = true;
  r = authenticate(fake::api, "sde-lock", "selviana", "correct horse");
  CHECK(r.status == AuthResult::Accepted);
  CHECK(r.messages.size() == 1 && r.messages[0] == "Touch the security key");
  fake::g_send_info = false;
  delete fake::g_last;

  fake::g_two_prompts = true;
  r = authenticate(fake::api, "sde-lock", "selviana", "correct horse");
  CHECK(r.status == AuthResult::Accepted);
  fake::g_two_prompts = false;
  delete fake::g_last;

  // An expired password unlocks; a locked account does not.
  fake::g_acct = PAM_NEW_AUTHTOK_REQD;
  CHECK(authenticate(fake::api, "sde-lock", "selviana", "correct horse").status == AuthResult::Accepted);
  delete fake::g_last;
  fake::g_acct = PAM_ACCT_EXPIRED;
  CHECK(authenticate(fake::api, "sde-lock", "selviana", "correct horse").status == AuthResult::Rejected);
  delete fake::g_last;
  fake::g_acct = PAM_SUCCESS;

  // PAM unavailable, missing user or password.
  fake::g_start_rc = PAM_ABORT;
  CHECK(authenticate(fake::api, "sde-lock", "selviana", "correct horse").status == AuthResult::Unavailable);
  fake::g_start_rc = PAM_SUCCESS;
  CHECK(authenticate(fake::api, "sde-lock", "", "x").status == AuthResult::Unavailable);
  CHECK(authenticate(fake::api, "sde-lock", "selviana", nullptr).status == AuthResult::Unavailable);
  // An empty password is passed to PAM (and rejected by it).
  CHECK(authenticate(fake::api, "sde-lock", "selviana", "").status == AuthResult::Rejected);
  delete fake::g_last;
}

static void test_secret_buffer() {
  using namespace lock;
  SecretBuffer b;
  CHECK(b.empty());
  CHECK(b.append("ab", 2));
  CHECK(b.append("\xC3\xA9", 2));  // é
  CHECK(b.append("\xE2\x82\xAC", 3));  // €
  CHECK(b.size() == 7 && b.chars() == 4);
  CHECK(std::string(b.c_str()) == "ab\xC3\xA9\xE2\x82\xAC");
  b.pop_char();
  CHECK(std::string(b.c_str()) == "ab\xC3\xA9" && b.chars() == 3);
  b.pop_char();
  CHECK(std::string(b.c_str()) == "ab");
  b.clear();
  CHECK(b.empty() && b.c_str()[0] == 0);
  b.pop_char();  // harmless when empty
  std::string big(SecretBuffer::kCapacity, 'x');
  CHECK(b.append(big.data(), big.size()));
  CHECK(!b.append("y", 1));  // full: refused, not truncated
  CHECK(b.size() == SecretBuffer::kCapacity);
  b.clear();
  char raw[8] = "secret";
  secure_wipe(raw, sizeof(raw));
  bool zero = true;
  for (char c : raw) zero &= c == 0;
  CHECK(zero);
}

static void test_limiter() {
  using namespace lock;
  CHECK(AttemptLimiter::delay_after(0) == 0);
  CHECK(AttemptLimiter::delay_after(1) == 1 && AttemptLimiter::delay_after(3) == 1);
  CHECK(AttemptLimiter::delay_after(4) == 2 && AttemptLimiter::delay_after(5) == 4);
  CHECK(AttemptLimiter::delay_after(30) == 300);
  AttemptLimiter l;
  CHECK(l.wait(100) == 0);
  l.failure(100);
  CHECK(l.wait(100) == 1 && l.wait(100.5) == 0.5 && l.wait(101) == 0);
  for (int i = 0; i < 4; ++i) l.failure(200);
  CHECK(l.failures() == 5);
  CHECK(l.wait(200) == 4);
  l.success();
  CHECK(l.wait(200) == 0 && l.failures() == 0);
}

static void test_inhibitors() {
  using namespace lock;
  InhibitorTracker t;
  CHECK(!t.active());
  uint32_t a = t.add(":1.10", "Videos", "Playing a film");
  uint32_t b = t.add(":1.11", "Slides", "Presenting");
  CHECK(a != 0 && b != 0 && a != b);
  CHECK(t.active() && t.size() == 2);
  // Only the owner may release its inhibitor.
  CHECK(!t.remove(a, ":1.11"));
  CHECK(t.remove(a, ":1.10"));
  CHECK(!t.remove(a, ":1.10"));  // already gone
  CHECK(t.size() == 1);
  // A vanished client loses its inhibitors.
  t.add(":1.11", "Slides", "second");
  CHECK(t.drop_owner(":1.11") == 2);
  CHECK(!t.active());
  // Per-owner limit.
  for (size_t i = 0; i < InhibitorTracker::kMaxPerOwner; ++i) CHECK(t.add(":1.20", "x", "y") != 0);
  CHECK(t.add(":1.20", "x", "y") == 0);
  CHECK(t.add(":1.21", "x", "y") != 0);
  auto list = t.list();
  CHECK(list.size() == InhibitorTracker::kMaxPerOwner + 1);
  // Long strings are cut.
  InhibitorTracker u;
  uint32_t c = u.add(":1.30", std::string(1000, 'a'), std::string(1000, 'b'));
  CHECK(u.list()[0].application.size() == 128 && u.list()[0].reason.size() == 256 && c);
}

static void test_idle_lock() {
  using namespace lock;
  CHECK(!should_idle_lock(299999, 300, false, false));
  CHECK(should_idle_lock(300000, 300, false, false));
  CHECK(!should_idle_lock(900000, 300, true, false));   // a video plays
  CHECK(!should_idle_lock(900000, 300, false, true));   // already locked
  CHECK(!should_idle_lock(900000, 0, false, false));    // disabled
  CHECK(!should_idle_lock(-1, 300, false, false));      // no idle counter
}

static void test_backoff() {
  using namespace lock;
  CrashBackoff b;
  CHECK(b.crashed(0) < 0.5);
  CHECK(b.crashed(1) < 0.5);
  CHECK(b.crashed(2) < 0.5);
  CHECK(b.crashed(3) == 2);
  CHECK(b.crashed(4) == 4);
  for (int i = 0; i < 10; ++i) b.crashed(5);
  CHECK(b.crashed(6) == 30);
  CHECK(b.crashed(200) < 0.5);  // old crashes are forgotten
}

// Alpha 51: the lock screen never gives up on the grabs, and only unmaps
// small override-redirect popups of other programs to end their grabs.
static void test_input_recovery() {
  using namespace lock;
  CHECK(input_step(0) == InputStep::Retry);
  CHECK(input_step(0.9) == InputStep::Retry);
  CHECK(input_step(1.0) == InputStep::ClosePopups);
  CHECK(input_step(2.5) == InputStep::ReleasePopups);
  CHECK(input_step(3.0) == InputStep::ShowWaiting);
  CHECK(input_step(3600) == InputStep::ShowWaiting);

  ForeignWindow menu;
  menu.override_redirect = menu.viewable = true;
  menu.w = 220;
  menu.h = 300;
  menu.screen_w = 1280;
  menu.screen_h = 800;
  menu.types = {"_NET_WM_WINDOW_TYPE_POPUP_MENU"};
  CHECK(releasable_popup(menu));
  ForeignWindow f = menu;
  f.types.clear();  // an FLTK or panel popup has no type
  CHECK(releasable_popup(f));
  f = menu;
  f.override_redirect = false;  // managed by the window manager
  CHECK(!releasable_popup(f));
  f = menu;
  f.viewable = false;
  CHECK(!releasable_popup(f));
  f = menu;
  f.input_only = true;
  CHECK(!releasable_popup(f));
  f = menu;
  f.name = "SDE Screen Lock";  // the daemon's curtain
  CHECK(!releasable_popup(f));
  f = menu;
  f.w = 1280;  // full screen: desktop, compositor overlay
  f.h = 800;
  CHECK(!releasable_popup(f));
  for (const char *type : {"_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP",
                           "_NET_WM_WINDOW_TYPE_NOTIFICATION"}) {
    f = menu;
    f.types = {type};
    CHECK(!releasable_popup(f));
  }

  CHECK(!ui_unresponsive(100, 0));  // never heard yet
  CHECK(!ui_unresponsive(100, 95));
  CHECK(ui_unresponsive(100, 100 - kUiHeartbeatTimeout - 0.5));

  std::vector<Monitor> mons = {{0, 0, 1280, 800}, {1280, 0, 1920, 1080}};
  CHECK(monitor_at(mons, 10, 10) == 0);
  CHECK(monitor_at(mons, 1290, 10) == 1);
  CHECK(monitor_at(mons, 1500, 1000) == 1);
  CHECK(monitor_at(mons, -50, 400) == 0);  // off every monitor: the nearest
  CHECK(monitor_at({}, 5, 5) == 0);
}

static void test_power_policy() {
  using namespace power;
  IdleTimeouts t{60, 300, 900};
  CHECK(idle_stage(0, t) == IdleStage::Active);
  CHECK(idle_stage(59999, t) == IdleStage::Active);
  CHECK(idle_stage(60000, t) == IdleStage::Dim);
  CHECK(idle_stage(300000, t) == IdleStage::Blank);
  CHECK(idle_stage(900000, t) == IdleStage::Suspend);
  CHECK(next_stage_in(0, t) == 60000);
  CHECK(next_stage_in(61000, t) == 239000);
  CHECK(next_stage_in(900000, t) == -1);
  IdleTimeouts no_dim{0, 120, 0};
  CHECK(idle_stage(100000, no_dim) == IdleStage::Active);
  CHECK(idle_stage(120000, no_dim) == IdleStage::Blank);
  CHECK(next_stage_in(130000, no_dim) == -1);
  IdleTimeouts off{0, 0, 0};
  CHECK(idle_stage(1L << 40, off) == IdleStage::Active && next_stage_in(0, off) == -1);

  // Battery levels with hysteresis.
  auto L = BatteryLevel::Low, N = BatteryLevel::Normal, C = BatteryLevel::Critical;
  CHECK(battery_level(50, true, N, 10, 5) == N);
  CHECK(battery_level(10, true, N, 10, 5) == L);
  CHECK(battery_level(11, true, L, 10, 5) == L);   // no flapping at the edge
  CHECK(battery_level(12, true, L, 10, 5) == N);
  CHECK(battery_level(5, true, L, 10, 5) == C);
  CHECK(battery_level(6, true, C, 10, 5) == C);
  CHECK(battery_level(7, true, C, 10, 5) == L);
  CHECK(battery_level(3, false, C, 10, 5) == N);   // charging
  CHECK(battery_level(-1, true, N, 10, 5) == N);   // unknown

  CHECK(parse_action("hibernate", Action::Nothing) == Action::Hibernate);
  CHECK(parse_action("bogus", Action::Suspend) == Action::Suspend);
  CHECK(std::string(action_name(Action::PowerOff)) == "poweroff");
  Settings s;
  s.lid_battery = Action::Suspend;
  s.lid_ac = Action::Lock;
  CHECK(lid_action(s, true, false) == Action::Suspend);
  CHECK(lid_action(s, false, false) == Action::Lock);
  CHECK(lid_action(s, true, true) == Action::Nothing);  // docked
}

static void test_brightness() {
  using namespace power;
  CHECK(percent_to_raw(0, 255) == 1);
  CHECK(percent_to_raw(100, 255) == 255);
  CHECK(percent_to_raw(50, 1000) == 500);
  CHECK(percent_to_raw(50, 0) == 0);
  CHECK(raw_to_percent(255, 255) == 100 && raw_to_percent(0, 255) == 0);
  CHECK(raw_to_percent(10, 0) == -1);
  CHECK(step_percent(50, 1) == 55 && step_percent(52, 1) == 55);
  CHECK(step_percent(50, -1) == 45 && step_percent(52, -1) == 50);
  CHECK(step_percent(100, 3) == 100);
  CHECK(step_percent(10, -1) == 8);   // finer steps when dark
  CHECK(step_percent(2, -1) == 1 && step_percent(1, -5) == 1);
  CHECK(step_percent(1, 1) == 2);
  CHECK(step_percent(40, -2, 10) == 20);

  // A fake /sys/class/backlight.
  char tmpl[] = "/tmp/sde-backlight-XXXXXX";
  char *dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (!dir) return;
  std::string root = dir;
  auto make = [&](const std::string &name, const std::string &type, int value, int max) {
    std::string d = root + "/" + name;
    std::string cmd = "mkdir -p '" + d + "'";
    CHECK(system(cmd.c_str()) == 0);
    FILE *f = fopen((d + "/type").c_str(), "w");
    fprintf(f, "%s\n", type.c_str());
    fclose(f);
    f = fopen((d + "/brightness").c_str(), "w");
    fprintf(f, "%d\n", value);
    fclose(f);
    f = fopen((d + "/max_brightness").c_str(), "w");
    fprintf(f, "%d\n", max);
    fclose(f);
  };
  make("acpi_video0", "firmware", 5, 10);
  make("intel_backlight", "raw", 48000, 96000);
  make("broken", "raw", 1, 0);
  auto all = backlights(root);
  CHECK(all.size() == 2);
  if (all.size() == 2) {
    CHECK(all[0].name == "acpi_video0" && all[0].percent() == 50);
    CHECK(all[1].name == "intel_backlight" && all[1].percent() == 50);
  }
  Backlight best;
  CHECK(primary_backlight(best, root) && best.type == "firmware");
  std::string cleanup = "rm -rf '" + root + "'";
  CHECK(system(cleanup.c_str()) == 0);
  CHECK(!primary_backlight(best, root));
}

int main() {
  test_conversation();
  test_authenticate();
  test_secret_buffer();
  test_limiter();
  test_inhibitors();
  test_idle_lock();
  test_backoff();
  test_input_recovery();
  test_power_policy();
  test_brightness();
  if (g_failures) {
    fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  printf("lock/power: all %d checks passed\n", g_checks);
  return 0;
}
