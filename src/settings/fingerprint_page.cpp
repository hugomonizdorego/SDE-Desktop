// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// Control Center > Fingerprint (sde-settings --page fingerprint,
// docs/FINGERPRINT.md).
//
// The fingerprint reader (fprintd's Manager.GetDevices and the device's
// name and scan type), your enrolled fingers on a picture of both hands,
// and what can be done with the finger chosen there: Enroll (Claim,
// EnrollStart and its EnrollStatus signals: the stages, the retries and a
// message for every status), Test (VerifyStart), Delete and Delete All.
// fprintd asks polkit before it enrolls or deletes (your own password,
// net.reactivated.fprint.device.enroll); the calls wait for that.
//
// "Use for": Sign in and Administrator authentication (sudo, polkit) are
// the system's PAM configuration, changed the Debian way with
// pam-auth-update by sde-fingerprint-pam through pkexec (fingerprint_pam.h,
// polkit action org.debion.sde.fingerprint.configure). Unlock the screen
// is SDE Screen Lock's own setting ([lock] fingerprint), which needs no
// administrator and no PAM module (the lock asks fprintd itself).
//
// Without fprintd or libpam-fprintd the page says so and offers to install
// them (PackageKit's pkcon, else Merkadu). Everything runs from the event
// loop: the D-Bus calls and signals (src/common/fprint.h), the helper and
// the installer (run_async).
//
// QA hooks (tests/test_fingerprint_ui.py): SDE_FINGERPRINT_PAM_ROOT reads
// the PAM state of a fake root; SDE_FINGERPRINT_HELPER runs that helper
// directly (no pkexec) on it with --root.
#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <cairo.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "fingerprint_pam.h"
#include "fprint.h"
#include "i18n.h"
#include "paint.h"
#include "process.h"
#include "settings.h"
#include "spawn.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

// Enrolling and deleting wait for polkit (your password): minutes.
const int kAuthTimeout = 300000;
const char *kPage = "fingerprint";

double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + ts.tv_nsec / 1e9;
}

struct Reader {
  std::string path, name;
  fprint::ScanType scan = fprint::ScanType::press;
  int stages = 0;  // num-enroll-stages (0: not said)
};

enum class Service { unknown, missing, no_answer, ready };
enum class Op { none, enroll, verify, remove, remove_all };
enum class Kind { info, working, done, error };

struct State {
  Service service = Service::unknown;
  std::string service_error;
  bool loading = false;
  int generation = 0;  // answers to an older load are dropped
  double loaded_at = -100;
  std::vector<Reader> readers;
  size_t reader = 0;
  std::vector<std::string> enrolled;  // of the reader shown, for you
  bool enrolled_known = false;
  std::string selected = "right-index-finger";

  // The operation on the reader.
  Op op = Op::none;
  bool claimed = false;
  bool running = false;  // EnrollStart/VerifyStart answered
  std::string owner;     // the unique name of the fprintd that answered Claim
  std::string op_path, op_finger;
  int stage = 0;
  std::string message;
  Kind kind = Kind::info;
  bool confirm_remove_all = false;

  // "Use for" and the installer.
  bool pam_busy = false;
  std::string pam_message;
  Kind pam_kind = Kind::info;
  bool installing = false;
  std::string install_message;
  Kind install_kind = Kind::info;
};
State g;

dbus::Bus *bus() { return dbus::Bus::system(); }

const Reader *reader() { return g.reader < g.readers.size() ? &g.readers[g.reader] : nullptr; }

bool enrolled(const std::string &finger) {
  return std::find(g.enrolled.begin(), g.enrolled.end(), finger) != g.enrolled.end();
}

// The page is built again from the event loop: never inside the callback
// of one of its own widgets, and once for many changes.
void refresh_now(void *) { refresh_if_shown(kPage); }
void changed() {
  Fl::remove_timeout(refresh_now);
  Fl::add_timeout(0, refresh_now);
}

void say(const std::string &text, Kind kind) {
  g.message = text;
  g.kind = kind;
}

std::string pam_root() { return env("SDE_FINGERPRINT_PAM_ROOT", "/"); }

// ------------------------------------------------------------ fprintd

void load_enrolled();
void on_signal(const fprint::Signal &s);

void watch() {
  static bool watching = false;
  if (watching || !bus()) return;
  watching = true;
  fprint::watch_signals(bus(), on_signal);
  // fprintd was installed, started or restarted: look again (it exits
  // when idle; that is not a change worth a look).
  fprint::watch_owner(bus(), [](const std::string &, const std::string &now_owner) {
    if (!now_owner.empty() && g.op == Op::none && g.service != Service::ready) {
      g.loaded_at = -100;
      changed();
    }
  });
}

void load() {
  if (g.loading || g.op != Op::none) return;
  watch();
  g.loading = true;
  int gen = ++g.generation;
  fprint::call(bus(), fprint::kManagerPath, fprint::kManagerIface, "GetDevices", {}, [gen](const fprint::Reply &r) {
    if (gen != g.generation) return;
    if (!r.ok) {
      g.loading = false;
      g.loaded_at = now_s();
      g.service = fprint::service_missing(r.error) ? Service::missing : Service::no_answer;
      g.service_error = fprint::error_text(r.error, r.message);
      g.readers.clear();
      g.enrolled.clear();
      changed();
      return;
    }
    std::vector<std::string> paths;
    for (const std::string &p : fprint::strings_of(r))
      if (dbus::valid_path(p)) paths.push_back(p);
    if (paths.empty() || !bus()) {
      g.loading = false;
      g.loaded_at = now_s();
      g.service = Service::ready;
      g.readers.clear();
      g.enrolled.clear();
      g.enrolled_known = true;
      changed();
      return;
    }
    auto readers = std::make_shared<std::vector<Reader>>(paths.size());
    auto pending = std::make_shared<size_t>(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
      (*readers)[i].path = paths[i];
      dbus::get_all_properties(bus(), fprint::kService, paths[i], fprint::kDeviceIface,
                               [gen, readers, pending, i](const dbus::Value &props) {
                                 if (gen != g.generation) return;
                                 Reader &rd = (*readers)[i];
                                 if (props.valid()) {
                                   if (const dbus::Value *v = props.get("name"))
                                     rd.name = dbus::sanitize_utf8(v->str()).substr(0, 120);
                                   if (const dbus::Value *v = props.get("scan-type")) rd.scan = fprint::scan_type(v->str());
                                   if (const dbus::Value *v = props.get("num-enroll-stages"))
                                     rd.stages = int(std::max<int64_t>(0, std::min<int64_t>(100, v->num())));
                                 }
                                 if (rd.name.empty()) rd.name = _("Fingerprint reader");
                                 if (--*pending) return;
                                 std::string keep = reader() ? reader()->path : "";
                                 g.readers = *readers;
                                 g.reader = 0;
                                 for (size_t k = 0; k < g.readers.size(); ++k)
                                   if (g.readers[k].path == keep) g.reader = k;
                                 g.service = Service::ready;
                                 load_enrolled();
                               });
    }
  });
}

void load_enrolled() {
  const Reader *rd = reader();
  if (!rd) {
    g.loading = false;
    g.loaded_at = now_s();
    changed();
    return;
  }
  int gen = g.generation;
  fprint::call(bus(), rd->path, fprint::kDeviceIface, "ListEnrolledFingers", {""}, [gen](const fprint::Reply &r) {
    if (gen != g.generation) return;
    g.loading = false;
    g.loaded_at = now_s();
    g.enrolled.clear();
    if (r.ok) {
      for (const std::string &f : fprint::strings_of(r))
        if (fprint::valid_finger(f) && !enrolled(f)) g.enrolled.push_back(f);
    } else if (r.error != fprint::kErrorNoEnrolledPrints) {
      say(fprint::error_text(r.error, r.message), Kind::error);
    }
    g.enrolled_known = true;
    // Show an enrolled finger when the one chosen has none.
    if (!enrolled(g.selected) && !g.enrolled.empty() && g.message.empty()) g.selected = g.enrolled.front();
    changed();
  });
}

void reload() {
  g.loaded_at = -100;
  load();
}

void finish(const std::string &message, Kind kind) {
  g.op = Op::none;
  g.claimed = g.running = false;
  g.owner.clear();
  g.op_path.clear();
  say(message, kind);
  changed();
  reload();
}

// Releases the reader (when it was claimed), then |next|.
void release_then(std::function<void()> next) {
  if (!g.claimed) {
    next();
    return;
  }
  g.claimed = false;
  fprint::call(bus(), g.op_path, fprint::kDeviceIface, "Release", {}, [next](const fprint::Reply &) { next(); });
}

// Stops the enrollment or verification (|member|), releases, then |next|.
void stop_then(const char *member, std::function<void()> next) {
  g.running = false;
  fprint::call(bus(), g.op_path, fprint::kDeviceIface, member, {},
               [next](const fprint::Reply &) { release_then(next); });
}

// Claims the reader shown for you, then |next|; on failure the operation ends.
void claim_then(std::function<void()> next) {
  const Reader *rd = reader();
  if (!rd) {
    finish(_("No fingerprint reader was found."), Kind::error);
    return;
  }
  g.op_path = rd->path;
  fprint::call(bus(), g.op_path, fprint::kDeviceIface, "Claim", {""},
               [next](const fprint::Reply &r) {
                 if (g.op == Op::none) return;
                 if (!r.ok) {
                   finish(fprint::error_text(r.error, r.message), Kind::error);
                   return;
                 }
                 g.claimed = true;
                 g.owner = r.sender;
                 next();
               },
               60000);
}

// Ends the operation after a failed call (releasing the reader first).
void failed(const fprint::Reply &r) {
  std::string why = fprint::error_text(r.error, r.message);
  release_then([why] { finish(why, Kind::error); });
}

void start(Op op, const std::string &finger, const std::string &working) {
  if (g.op != Op::none || !reader()) return;
  g.op = op;
  g.op_finger = finger;
  g.stage = 0;
  g.running = false;
  g.confirm_remove_all = false;
  say(working, Kind::working);
  changed();
}

void enroll(const std::string &finger) {
  start(Op::enroll, finger, _("Starting... A window may ask for your password first."));
  claim_then([finger] {
    fprint::call(bus(), g.op_path, fprint::kDeviceIface, "EnrollStart", {finger},
                 [](const fprint::Reply &r) {
                   if (g.op != Op::enroll) return;
                   if (!r.ok) {
                     failed(r);
                     return;
                   }
                   g.running = true;
                   say(fprint::enroll_prompt(reader() ? reader()->scan : fprint::ScanType::press), Kind::working);
                   changed();
                 },
                 kAuthTimeout);
  });
}

void verify() {
  start(Op::verify, "", _("Starting..."));
  claim_then([] {
    fprint::call(bus(), g.op_path, fprint::kDeviceIface, "VerifyStart", {"any"}, [](const fprint::Reply &r) {
      if (g.op != Op::verify) return;
      if (!r.ok) {
        failed(r);
        return;
      }
      g.running = true;
      say(format(_("%s with one of your enrolled fingers."),
                 fprint::touch_prompt(reader() ? reader()->scan : fprint::ScanType::press).c_str()),
          Kind::working);
      changed();
    });
  });
}

void delete_finger(const std::string &finger) {
  start(Op::remove, finger, _("Deleting... A window may ask for your password first."));
  claim_then([finger] {
    fprint::call(bus(), g.op_path, fprint::kDeviceIface, "DeleteEnrolledFinger", {finger},
                 [finger](const fprint::Reply &r) {
                   if (g.op != Op::remove) return;
                   if (!r.ok && r.error == DBUS_ERROR_UNKNOWN_METHOD) {
                     release_then([] {
                       finish(_("This version of fprintd deletes all fingerprints at once only: use Delete All."),
                              Kind::error);
                     });
                     return;
                   }
                   if (!r.ok) {
                     failed(r);
                     return;
                   }
                   std::string done = format(_("%s: the fingerprint was deleted."), fprint::finger_label(finger).c_str());
                   release_then([done] { finish(done, Kind::done); });
                 },
                 kAuthTimeout);
  });
}

void delete_all() {
  start(Op::remove_all, "", _("Deleting... A window may ask for your password first."));
  claim_then([] {
    fprint::call(bus(), g.op_path, fprint::kDeviceIface, "DeleteEnrolledFingers2", {},
                 [](const fprint::Reply &r) {
                   if (g.op != Op::remove_all) return;
                   if (!r.ok) {
                     failed(r);
                     return;
                   }
                   release_then([] { finish(_("All your fingerprints on this reader were deleted."), Kind::done); });
                 },
                 kAuthTimeout);
  });
}

void cancel() {
  if (g.op == Op::enroll && g.running)
    stop_then("EnrollStop", [] { finish(_("Enrolling was cancelled."), Kind::info); });
  else if (g.op == Op::verify && g.running)
    stop_then("VerifyStop", [] { finish(_("The test was cancelled."), Kind::info); });
}

void on_signal(const fprint::Signal &s) {
  // Only the fprintd that answered our Claim, about the reader claimed.
  if (g.op == Op::none || !g.claimed || s.sender != g.owner || s.path != g.op_path) return;
  fprint::ScanType scan = reader() ? reader()->scan : fprint::ScanType::press;
  if (g.op == Op::enroll && s.member == "EnrollStatus" && g.running) {
    fprint::EnrollStep st = fprint::enroll_step(s.result, s.done, scan);
    if (st.kind == fprint::EnrollStep::stage) ++g.stage;
    if (!st.done) {
      say(st.message, st.kind == fprint::EnrollStep::retry ? Kind::info : Kind::working);
      changed();
      return;
    }
    bool ok = st.kind == fprint::EnrollStep::completed;
    std::string finger = g.op_finger;
    std::string text = ok ? format(_("%s: the fingerprint was enrolled. Use Test to try it."),
                                   fprint::finger_label(finger).c_str())
                          : st.message;
    if (ok) g.selected = finger;
    stop_then("EnrollStop", [text, ok] { finish(text, ok ? Kind::done : Kind::error); });
    return;
  }
  if (g.op == Op::verify && s.member == "VerifyFingerSelected" && g.running) {
    if (fprint::valid_finger(s.result)) {
      say(format(_("%s: touch the fingerprint reader."), fprint::finger_label(s.result).c_str()), Kind::working);
      changed();
    }
    return;
  }
  if (g.op == Op::verify && s.member == "VerifyStatus" && g.running) {
    fprint::VerifyStep st = fprint::verify_step(s.result, s.done, scan);
    if (!st.done) {
      say(st.message, Kind::info);
      changed();
      return;
    }
    std::string text = st.kind == fprint::VerifyStep::match
                           ? std::string(_("Fingerprint recognized: it works."))
                           : st.kind == fprint::VerifyStep::no_match
                                 ? std::string(_("Fingerprint not recognized. Try again, or enroll this finger."))
                                 : st.message;
    Kind kind = st.kind == fprint::VerifyStep::match ? Kind::done : Kind::error;
    stop_then("VerifyStop", [text, kind] { finish(text, kind); });
  }
}

// ------------------------------------------------------ "Use for" (PAM)

void set_pam(bool sign_in, bool admin) {
  if (g.pam_busy) return;
  const char *si = sign_in ? "on" : "off", *ad = admin ? "on" : "off";
  std::vector<std::string> argv;
  std::string test = env("SDE_FINGERPRINT_HELPER");
  if (!test.empty()) {
    // The tests: the helper on their fake root, never on this computer's.
    std::string root = env("SDE_FINGERPRINT_PAM_ROOT");
    if (root.empty() || root == "/") {
      g.pam_message = "SDE_FINGERPRINT_HELPER needs SDE_FINGERPRINT_PAM_ROOT (a fake root)";
      g.pam_kind = Kind::error;
      changed();
      return;
    }
    argv = {test, "set", "--sign-in", si, "--admin", ad, "--root", root};
  } else {
    argv = {"pkexec", std::string(SDE_LIBEXECDIR) + "/sde/sde-fingerprint-pam", "set", "--sign-in", si, "--admin", ad};
  }
  g.pam_busy = true;
  g.pam_message = _("Changing the system's authentication... An administrator's password is needed.");
  g.pam_kind = Kind::working;
  changed();
  bool via_pkexec = test.empty();
  bool started = run_async(argv, [via_pkexec](const std::string &out, int status) {
    g.pam_busy = false;
    if (status == 0) {
      g.pam_message = _("Saved. The change applies to the next sign-in and administrator prompt.");
      g.pam_kind = Kind::done;
    } else if (via_pkexec && status == 126) {
      g.pam_message = _("Nothing was changed: the password was not given.");
      g.pam_kind = Kind::info;
    } else if (via_pkexec && status == 127) {
      g.pam_message = _("Nothing was changed: you are not allowed to do this.");
      g.pam_kind = Kind::error;
    } else {
      std::string why;
      for (const std::string &line : split(out, '\n'))
        if (starts_with(line, "ERROR ")) {
          size_t sp = line.find(' ', 6);
          why = sp == std::string::npos ? line.substr(6) : line.substr(sp + 1);
        }
      g.pam_message = why.empty() ? format(_("The change did not work (status %d)."), status) : why;
      g.pam_kind = Kind::error;
    }
    changed();
  });
  if (!started) {
    g.pam_busy = false;
    g.pam_message = via_pkexec ? _("pkexec (polkit) is not installed: the system's authentication cannot be changed.")
                               : "the test helper cannot be started";
    g.pam_kind = Kind::error;
    changed();
  }
}

// ------------------------------------------------------------ installing

void install(const std::vector<std::string> &packages) {
  if (packages.empty() || g.installing) return;
  std::string list;
  for (const std::string &p : packages) list += (list.empty() ? "" : " ") + p;
  if (!find_program("pkcon").empty()) {
    std::vector<std::string> argv = {"pkcon", "-p", "-y", "install"};
    argv.insert(argv.end(), packages.begin(), packages.end());
    g.installing = true;
    g.install_message = format(_("Installing %s (PackageKit)... An administrator's password is needed."), list.c_str());
    g.install_kind = Kind::working;
    changed();
    run_async(argv, [list](const std::string &out, int status) {
      g.installing = false;
      if (status == 0) {
        g.install_message = format(_("%s installed."), list.c_str());
        g.install_kind = Kind::done;
      } else {
        std::string tail = trim(out);
        if (tail.size() > 300) tail = "..." + tail.substr(tail.size() - 300);
        g.install_message = format(_("Installing %s did not work (status %d). %s"), list.c_str(), status, tail.c_str());
        g.install_kind = Kind::error;
      }
      g.service = Service::unknown;
      reload();
      changed();
    });
    return;
  }
  if (!find_program("merkadu").empty()) {
    spawn_detached({"merkadu", "--package", packages.front()});
    g.install_message = format(_("Merkadu shows %s: install it there, then come back to this page."), packages.front().c_str());
    g.install_kind = Kind::info;
  } else {
    g.install_message = format(_("Install the packages with: sudo apt install %s"), list.c_str());
    g.install_kind = Kind::info;
  }
  changed();
}

// ---------------------------------------------------------------- widgets

Color kind_color(Kind k) {
  const Theme &t = settings_theme();
  switch (k) {
    case Kind::working: return t.accent_text;
    case Kind::done: return t.success;
    case Kind::error: return t.error;
    default: return t.text;
  }
}

// A wrapped message in its color (working, done, error or plain).
class Message : public Fl_Widget {
 public:
  Message(int x, int y, int w, int h, std::string text, Kind kind)
      : Fl_Widget(x, y, w, h), text_(std::move(text)), kind_(kind) {}
  static int measure(const std::string &text, int w) {
    text_font(settings_theme().font_size);
    int ww = w - S(28), hh = 0;
    fl_measure(text.c_str(), ww, hh, 0);
    return std::max(S(24), hh + S(6));
  }
  void draw() override {
    const Theme &t = settings_theme();
    Color c = kind_color(kind_);
    {
      Painter p(window()->w(), window()->h());
      double r = Sd(4.5), cx = x() + S(8), cy = y() + S(12);
      if (kind_ == Kind::working || kind_ == Kind::done || kind_ == Kind::error) p.circle(cx, cy, r, c);
    }
    text_font(t.font_size);
    fl_color(c.fl());
    fl_draw(text_.c_str(), x() + S(22), y() + S(2), w() - S(28), h() - S(2),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  }

 private:
  std::string text_;
  Kind kind_;
};

// The stages of an enrollment.
class StageBar : public Fl_Widget {
 public:
  StageBar(int x, int y, int w, int h, int done, int total) : Fl_Widget(x, y, w, h), done_(done), total_(total) {}
  void draw() override {
    const Theme &t = settings_theme();
    {
      Painter p(window()->w(), window()->h());
      int bar_h = S(8), by = y() + h() - bar_h;
      p.fill_rounded(x(), by, w(), bar_h, bar_h / 2.0, t.surface_alt);
      p.stroke_rounded(x() + 0.5, by + 0.5, w() - 1, bar_h - 1, bar_h / 2.0, t.border);
      double f = total_ > 0 ? std::min(1.0, double(done_) / total_) : 0;
      if (f > 0) p.fill_rounded(x(), by, std::max(double(bar_h), w() * f), bar_h, bar_h / 2.0, t.accent);
    }
    text_font(t.font_size - 1);
    std::string label = total_ > 0 ? format(_("Step %d of %d"), std::min(done_ + 1, total_), total_)
                                   : format(_("Scans: %d"), done_);
    draw_text(label, x(), y(), w(), h() - S(10), t.text_dim);
  }

 private:
  int done_, total_;
};

// ------------------------------------------------------------ the hands
//
// Two hands seen from above, palms down: the left hand on the left with
// its thumb towards the middle, the right hand on the right. Each finger
// is a widget of its own (named for the QA dump and the keyboard).

struct Shape {
  // A capsule from |bx|,|by| to |tx|,|ty| (the tip), |width| wide, in
  // window pixels.
  double bx, by, tx, ty, width;
};

struct HandsLayout {
  double palm_x[2], palm_y, palm_w, palm_h;  // [0] left, [1] right
  int caption_y;
  Shape fingers[10];  // in the order of fprint::fingers()
};

// The fingers of a right hand in a 1 x 1 box (x to the right, y down):
// thumb, index, middle, ring, little: base and tip, width.
const double kRight[5][5] = {
    {0.34, 0.78, 0.09, 0.49, 0.15},  // thumb
    {0.38, 0.60, 0.38, 0.14, 0.135},
    {0.525, 0.60, 0.525, 0.05, 0.14},
    {0.67, 0.60, 0.67, 0.11, 0.135},
    {0.80, 0.62, 0.80, 0.27, 0.12},
};

HandsLayout layout_hands(int X, int Y, int W, int H) {
  HandsLayout l;
  int caption = S(24);
  double hand_h = H - caption, hand_w = std::min((W - S(28)) / 2.0, hand_h * 0.95);
  double gap = std::min(double(S(60)), W - 2 * hand_w);
  double left_x = X + (W - 2 * hand_w - gap) / 2.0, right_x = left_x + hand_w + gap;
  l.palm_y = Y + hand_h * 0.47;
  l.palm_h = hand_h * 0.50;
  l.palm_w = hand_w * 0.58;
  l.palm_x[1] = right_x + hand_w * 0.30;
  l.palm_x[0] = left_x + hand_w * (1 - 0.30 - 0.58);
  l.caption_y = int(Y + hand_h);
  // fprint::fingers(): left little ... left thumb, right thumb ... right little.
  for (int i = 0; i < 5; ++i) {
    const double *f = kRight[i];
    Shape right = {right_x + f[0] * hand_w, Y + f[1] * hand_h, right_x + f[2] * hand_w, Y + f[3] * hand_h,
                   f[4] * hand_w};
    Shape left = {left_x + (1 - f[0]) * hand_w, Y + f[1] * hand_h, left_x + (1 - f[2]) * hand_w, Y + f[3] * hand_h,
                  f[4] * hand_w};
    l.fingers[5 + i] = right;
    l.fingers[4 - i] = left;
  }
  return l;
}

// The capsule path of |s| moved by |dx|, |dy|.
void capsule(cairo_t *cr, const Shape &s, double grow = 0) {
  double vx = s.tx - s.bx, vy = s.ty - s.by, len = std::hypot(vx, vy);
  double r = s.width / 2 + grow;
  cairo_save(cr);
  cairo_translate(cr, s.bx, s.by);
  cairo_rotate(cr, std::atan2(vy, vx));
  cairo_new_path(cr);
  cairo_arc(cr, len, 0, r, -M_PI / 2, M_PI / 2);
  cairo_arc(cr, 0, 0, r, M_PI / 2, 3 * M_PI / 2);
  cairo_close_path(cr);
  cairo_restore(cr);
}

bool in_shape(const Shape &s, double px, double py) {
  double vx = s.tx - s.bx, vy = s.ty - s.by, len2 = vx * vx + vy * vy;
  double t = len2 > 0 ? std::max(0.0, std::min(1.0, ((px - s.bx) * vx + (py - s.by) * vy) / len2)) : 0;
  double dx = px - (s.bx + t * vx), dy = py - (s.by + t * vy);
  return dx * dx + dy * dy <= (s.width / 2) * (s.width / 2);
}

void select_finger(const std::string &name);

class Palms : public Fl_Widget {
 public:
  Palms(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    const Theme &t = settings_theme();
    HandsLayout l = layout_hands(x(), y(), w(), h());
    {
      Painter p(window()->w(), window()->h());
      for (int i = 0; i < 2; ++i) {
        double r = l.palm_w * 0.28;
        p.fill_rounded(l.palm_x[i], l.palm_y, l.palm_w, l.palm_h, r, t.surface_alt);
        p.stroke_rounded(l.palm_x[i] + 0.5, l.palm_y + 0.5, l.palm_w - 1, l.palm_h - 1, r, t.border);
      }
    }
    text_font(t.font_size - 1);
    for (int i = 0; i < 2; ++i)
      draw_text(i ? _("Right hand") : _("Left hand"), int(l.palm_x[i] - S(30)), l.caption_y, int(l.palm_w + S(60)),
                S(22), t.text_dim, 1);
  }
};

class FingerButton : public Fl_Widget {
 public:
  // |shape| in window pixels at the time of construction.
  FingerButton(const Shape &shape, const std::string &name)
      : Fl_Widget(int(std::min(shape.bx, shape.tx) - shape.width / 2 - 2), int(std::min(shape.by, shape.ty) - shape.width / 2 - 2),
                  int(std::fabs(shape.tx - shape.bx) + shape.width + 4), int(std::fabs(shape.ty - shape.by) + shape.width + 4)),
        name_(name), label_(fprint::finger_label(name)) {
    // The shape relative to the widget (the page scrolls it).
    shape_ = shape;
    shape_.bx -= x();
    shape_.by -= y();
    shape_.tx -= x();
    shape_.ty -= y();
    // The QA dump and refresh_page() find it by its name.
    copy_tooltip(label_.c_str());
    if (g.op != Op::none) deactivate();
    all().push_back(this);
  }
  ~FingerButton() override { all().erase(std::remove(all().begin(), all().end(), this), all().end()); }
  static std::vector<FingerButton *> &all() {
    static std::vector<FingerButton *> list;
    return list;
  }
  const std::string &name() const { return name_; }
  Shape shape() const {
    Shape s = shape_;
    s.bx += x();
    s.by += y();
    s.tx += x();
    s.ty += y();
    return s;
  }
  void draw() override {
    const Theme &t = settings_theme();
    Shape s = shape();
    bool on = enrolled(name_), chosen = name_ == g.selected, busy = g.op != Op::none && name_ == g.op_finger;
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    if (chosen || busy) {
      capsule(cr, s, Sd(3.5));
      p.set(t.accent.alpha(0.28));
      cairo_fill(cr);
    }
    capsule(cr, s);
    Color fill = on ? t.accent : hot_ && active_r() ? t.hover : t.surface;
    p.set(fill);
    cairo_fill_preserve(cr);
    p.set(chosen ? t.accent : on ? t.accent.mix(t.text, 0.2) : t.border);
    cairo_set_line_width(cr, chosen ? Sd(2.2) : Sd(1.1));
    cairo_stroke(cr);
    if (on) {
      // A small fingerprint at the tip of an enrolled finger.
      double g_size = s.width * 0.62;
      double dx = s.tx - s.bx, dy = s.ty - s.by, len = std::hypot(dx, dy);
      double ux = len > 0 ? dx / len : 0, uy = len > 0 ? dy / len : -1;
      double cx = s.tx - ux * s.width * 0.25, cy = s.ty - uy * s.width * 0.25;
      fprint::draw_glyph(p, cx - g_size / 2, cy - g_size / 2, g_size, t.on_accent);
    }
    if (Fl::focus() == this) {
      capsule(cr, s, Sd(5.5));
      p.set(t.accent);
      cairo_set_line_width(cr, Sd(1.5));
      double dash[] = {Sd(3), Sd(2.5)};
      cairo_set_dash(cr, dash, 2, 0);
      cairo_stroke(cr);
      cairo_set_dash(cr, nullptr, 0, 0);
    }
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        if (!active_r() || !in_shape(shape(), Fl::event_x(), Fl::event_y())) return 0;
        take_focus();
        return 1;
      case FL_RELEASE:
        if (active_r() && in_shape(shape(), Fl::event_x(), Fl::event_y())) choose();
        return 1;
      case FL_ENTER:
      case FL_MOVE: {
        bool in = in_shape(shape(), Fl::event_x(), Fl::event_y());
        if (in != hot_) {
          hot_ = in;
          redraw();
        }
        return in ? 1 : 0;
      }
      case FL_LEAVE:
        hot_ = false;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == ' ' || k == FL_Enter || k == FL_KP_Enter) {
          choose();
          return 1;
        }
        if (k == FL_Left || k == FL_Right) {
          // The neighbouring finger, across both hands; it keeps the
          // keyboard once the page is built again.
          const std::vector<std::string> &names = fprint::fingers();
          auto it = std::find(names.begin(), names.end(), name_);
          long i = it - names.begin() + (k == FL_Left ? -1 : 1);
          if (i >= 0 && i < long(names.size())) {
            focus_next() = names[size_t(i)];
            select_finger(names[size_t(i)]);
          }
          return 1;
        }
        return 0;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

  // The finger to give the keyboard to after the next build.
  static std::string &focus_next() {
    static std::string name;
    return name;
  }
  static void focus_later(void *) {
    std::string want = focus_next();
    focus_next().clear();
    for (FingerButton *b : all())
      if (b->name() == want && b->visible_r() && b->active_r()) b->take_focus();
  }

 private:
  void choose() { select_finger(name_); }
  std::string name_, label_;
  Shape shape_;
  bool hot_ = false;
};

void select_finger(const std::string &name) {
  if (g.op != Op::none || !fprint::valid_finger(name)) return;
  g.selected = name;
  g.confirm_remove_all = false;
  if (g.kind != Kind::working) g.message.clear();
  changed();
}

// A row of buttons, left aligned, wrapping on narrow pages.
struct ButtonSpec {
  ButtonSpec(std::string l, std::function<void()> a, bool p = false, bool e = true, std::string tip = "")
      : label(std::move(l)), action(std::move(a)), primary(p), enabled(e), tooltip(std::move(tip)) {}
  std::string label;
  std::function<void()> action;
  bool primary;
  bool enabled;
  std::string tooltip;
};

void button_row(Page *page, const std::vector<ButtonSpec> &specs) {
  int x0 = page->left() + S(20), w = page->width() - S(40), x = x0, y = page->cursor(), h = S(34), gap = S(10);
  const Theme &t = settings_theme();
  text_font(t.font_size + 1);
  for (const ButtonSpec &b : specs) {
    int bw = std::max(S(110), text_width(b.label) + S(36));
    if (x > x0 && x + bw > x0 + w) {
      x = x0;
      y += h + gap;
    }
    auto *btn = new FlatButton(x, y, bw, h, b.label, b.primary);
    btn->copy_label(b.label.c_str());
    if (!b.tooltip.empty()) btn->copy_tooltip(b.tooltip.c_str());
    btn->callback([](Fl_Widget *, void *d) {
      Fl::add_timeout(0, [](void *dd) { (*static_cast<std::function<void()> *>(dd))(); }, d);
    }, keep(new std::function<void()>(b.action)));
    if (!b.enabled) btn->deactivate();
    x += bw + gap;
  }
  page->advance(y + h - page->cursor() + S(10));
}

void message_line(Page *page, const std::string &text, Kind kind) {
  if (text.empty()) return;
  int x = page->left() + S(20), w = page->width() - S(40);
  int h = Message::measure(text, w);
  new Message(x, page->cursor(), w, h, text, kind);
  page->advance(h + S(6));
}

std::vector<std::string> missing_packages(const fingerprint_pam::State &pam) {
  std::vector<std::string> out;
  if (g.service == Service::missing) out.push_back("fprintd");
  if (!pam.module) out.push_back("libpam-fprintd");
  return out;
}

void install_row(Page *page, const fingerprint_pam::State &pam) {
  std::vector<std::string> pkgs = missing_packages(pam);
  if (pkgs.empty()) return;
  std::string list;
  for (const std::string &p : pkgs) list += (list.empty() ? "" : " ") + p;
  std::string label = pkgs.size() == 2 ? std::string(_("Install fprintd and libpam-fprintd"))
                                       : format(_("Install %s"), pkgs.front().c_str());
  button_row(page, {{label, [pkgs] { install(pkgs); }, true, !g.installing, list}});
  message_line(page, g.install_message, g.install_kind);
}

// ------------------------------------------------------------ the cards

void reader_card(Page *page, const fingerprint_pam::State &pam) {
  page->anchor("reader");
  page->begin_card(_("Fingerprint reader"));
  if (g.service == Service::unknown || (g.loading && g.readers.empty() && g.service != Service::ready)) {
    page->note(_("Looking for a fingerprint reader..."));
  } else if (g.service == Service::missing) {
    page->note(_("The fingerprint service is not installed. Fingerprint readers work through fprintd (package "
                 "fprintd), the standard fingerprint service of Debian; signing in and administrator "
                 "authentication with a finger also need its PAM module (package libpam-fprintd)."));
    install_row(page, pam);
  } else if (g.service == Service::no_answer) {
    page->note(g.service_error);
    button_row(page, {{_("Try Again"), [] { reload(); }, false, !g.loading}});
  } else if (g.readers.empty()) {
    page->note(_("No fingerprint reader was found. fprintd supports the readers of many laptops and USB "
                 "readers (the libfprint list of supported devices); readers that need a vendor driver "
                 "do not show up here."));
    button_row(page, {{_("Look Again"), [] { reload(); }, false, !g.loading}});
  } else {
    if (g.readers.size() > 1) {
      std::vector<std::string> names;
      for (const Reader &r : g.readers) names.push_back(r.name);
      Fl_Choice *ch = page->choice_row(_("Reader"), names, int(g.reader), [](int v) {
        if (v < 0 || size_t(v) >= g.readers.size() || g.op != Op::none) return;
        g.reader = size_t(v);
        g.enrolled_known = false;
        g.message.clear();
        load_enrolled();
      });
      if (ch) {
        ch->tooltip(_("Fingerprint reader"));
        if (g.op != Op::none) ch->deactivate();
      }
    }
    const Reader &rd = g.readers[g.reader];
    std::string how = rd.scan == fprint::ScanType::swipe ? _("Swipe your finger across it") : _("Touch it with your finger");
    page->row(rd.name, S(150), S(30), [&how](int x, int y, int w, int h) {
      auto *l = new Label(x, y, w, h, how, Label::DIM);
      return l;
    }, _("Fingerprints are kept by fprintd on this computer (or in the reader), not in your home folder."));
  }
  page->end_card();
}

void fingers_card(Page *page) {
  page->anchor("fingers");
  page->begin_card(_("Your fingerprints"));
  int x = page->left() + S(20), w = page->width() - S(40);
  int hands_h = std::min(S(220), std::max(S(150), w * 2 / 5));
  int hands_w = std::min(w, S(600));
  int hx = x + (w - hands_w) / 2, hy = page->cursor();
  new Palms(hx, hy, hands_w, hands_h);
  HandsLayout l = layout_hands(hx, hy, hands_w, hands_h);
  for (size_t i = 0; i < fprint::fingers().size(); ++i) new FingerButton(l.fingers[i], fprint::fingers()[i]);
  // After refresh_page() has given the keyboard back to the finger that had it.
  if (!FingerButton::focus_next().empty()) Fl::add_timeout(0, FingerButton::focus_later);
  page->advance(hands_h + S(8));

  std::string chosen = fprint::finger_label(g.selected);
  bool on = enrolled(g.selected);
  int n = int(g.enrolled.size());
  std::string summary = !g.enrolled_known ? std::string(_("Reading your fingerprints..."))
                        : n == 0 ? std::string(_("No finger is enrolled yet. Choose a finger, then Enroll."))
                        : n == 1 ? std::string(_("1 finger is enrolled."))
                                 : format(_("%d fingers are enrolled."), n);
  new Label(x, page->cursor(), w, S(26),
            chosen + "  ·  " + (on ? std::string(_("enrolled")) : std::string(_("not enrolled"))), Label::BOLD);
  page->advance(S(28));
  page->note(summary);

  const Reader *rd = reader();
  if (g.op == Op::enroll && g.running) {
    int total = rd ? rd->stages : 0;
    new StageBar(x, page->cursor(), std::min(w, S(420)), S(30), g.stage, total);
    page->advance(S(36));
  }
  message_line(page, g.message, g.kind);

  bool idle = g.op == Op::none && g.enrolled_known;
  std::vector<ButtonSpec> buttons;
  if (g.op == Op::enroll || g.op == Op::verify) {
    buttons.push_back({_("Cancel"), [] { cancel(); }, false, g.running});
  } else if (g.confirm_remove_all) {
    message_line(page, _("Delete all your fingerprints on this reader? Signing in, unlocking and administrator "
                         "prompts will ask for your password only."),
                 Kind::info);
    buttons.push_back({_("Delete All Fingerprints"), [] { delete_all(); }, true, idle});
    buttons.push_back({_("Keep Them"), [] {
                         g.confirm_remove_all = false;
                         changed();
                       },
                       false, true});
  } else {
    std::string sel = g.selected;
    if (!on)
      buttons.push_back({_("Enroll"), [sel] { enroll(sel); }, true, idle,
                         format(_("Enroll the %s"), fprint::finger_label(sel).c_str())});
    buttons.push_back({_("Test"), [] { verify(); }, on, idle && n > 0, _("Check an enrolled finger")});
    if (on)
      buttons.push_back({_("Delete"), [sel] { delete_finger(sel); }, false, idle,
                         format(_("Delete the fingerprint of the %s"), fprint::finger_label(sel).c_str())});
    if (n > 0)
      buttons.push_back({_("Delete All..."), [] {
                           g.confirm_remove_all = true;
                           changed();
                         },
                         false, idle});
  }
  button_row(page, buttons);
  page->end_card();
}

void use_card(Page *page, const fingerprint_pam::State &pam) {
  page->anchor("use");
  page->begin_card(_("Use the fingerprint for"));
  bool pam_ok = pam.debian_pam && pam.module && !pam.by_hand && !g.pam_busy;
  Switch *sw = page->switch_row(
      _("Sign in"), pam.sign_in, [pam](bool on) { set_pam(on, pam.admin); },
      _("The login screen (LightDM) and text consoles ask for a finger first; the password still works. A "
        "keyring that opens with your password at login asks for it later."));
  if (sw) {
    sw->tooltip(_("Use the fingerprint to sign in"));
    if (!pam_ok) sw->deactivate();
  }
  sw = page->switch_row(
      _("Unlock the screen"), settings_config().get_bool("lock", "fingerprint", true),
      [](bool on) {
        settings_config().set("lock", "fingerprint", on ? "true" : "false");
        apply_change();
      },
      _("SDE Screen Lock accepts a finger next to your password. After five fingers that are not "
        "recognized, only the password unlocks."));
  if (sw) sw->tooltip(_("Use the fingerprint to unlock the screen"));
  sw = page->switch_row(
      _("Administrator authentication (sudo, polkit)"), pam.admin, [pam](bool on) { set_pam(pam.sign_in, on); },
      _("sudo in a terminal and the administrator prompts of the Control Center, Liria, Merkadu and other "
        "programs ask for a finger; you can type the password instead."));
  if (sw) {
    sw->tooltip(_("Use the fingerprint for administrator authentication"));
    if (!pam_ok) sw->deactivate();
  }
  if (!g.pam_message.empty()) message_line(page, g.pam_message, g.pam_kind);
  if (!pam.debian_pam)
    page->note(_("Signing in and administrator authentication: this system does not build its PAM "
                 "configuration with pam-auth-update, so SDE cannot change it."));
  else if (pam.by_hand)
    page->note(_("pam_fprintd was put into /etc/pam.d/common-auth by hand: pam-auth-update (and this page) "
                 "do not change hand-made settings."));
  else if (!pam.module) {
    page->note(_("Signing in and administrator authentication with a finger need the PAM module of fprintd "
                 "(package libpam-fprintd)."));
    if (g.service != Service::missing) install_row(page, pam);
  } else
    page->note(_("These two change the system's PAM configuration for everyone with pam-auth-update, the Debian "
                 "standard (the \"Fingerprint authentication\" profile of libpam-fprintd), which asks for an "
                 "administrator's password."));
  page->end_card();
}

}  // namespace

void build_fingerprint_page(Page *page) {
  page->heading(_("Fingerprint"), _("Sign in, unlock the screen and authorize administrator tasks with a finger"));
  if (g.op == Op::none && !g.loading && now_s() - g.loaded_at > 4.0) load();
  fingerprint_pam::State pam = fingerprint_pam::read_state(pam_root());
  reader_card(page, pam);
  if (g.service == Service::ready && !g.readers.empty()) fingers_card(page);
  use_card(page, pam);
  page->note(_("Folders and the rest of the system: Linux has no standard lock for single folders. SDE uses "
               "the fingerprint wherever the system asks you to authenticate: the lock screen, the login screen, "
               "sudo and polkit's administrator prompts (opening a folder as administrator in Liria, installing "
               "software, changing system settings)."));
  // The state for the UI test (tests/test_fingerprint_ui.py).
  std::string enrolled_list;
  for (const std::string &f : g.enrolled) enrolled_list += (enrolled_list.empty() ? "" : ",") + f;
  const char *service = g.service == Service::ready     ? "ready"
                        : g.service == Service::missing ? "missing"
                        : g.service == Service::no_answer ? "no-answer"
                                                          : "unknown";
  qa_line(std::string("@fingerprint:service=") + service + ";readers=" + std::to_string(g.readers.size()) +
              ";enrolled=" + enrolled_list + ";selected=" + g.selected + ";op=" + std::to_string(int(g.op)) +
              ";stage=" + std::to_string(g.stage) + ";pam-busy=" + (g.pam_busy ? "1" : "0") + ";sign-in=" +
              (pam.sign_in ? "1" : "0") + ";admin=" + (pam.admin ? "1" : "0"),
          0, 0, 0, 0);
  auto one_line = [](std::string s) {
    for (char &c : s)
      if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    return s;
  };
  qa_line("@fingerprint-message:" + one_line(g.message), 0, 0, 0, 0);
  qa_line("@fingerprint-pam-message:" + one_line(g.pam_message), 0, 0, 0, 0);
  qa_line("@fingerprint-install-message:" + one_line(g.install_message), 0, 0, 0, 0);
  page->finish();
}

}  // namespace sde
