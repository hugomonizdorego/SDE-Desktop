// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "fprint.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "i18n.h"
#include "paint.h"
#include "util.h"

namespace sde {
namespace fprint {

const char *const kService = "net.reactivated.Fprint";
const char *const kManagerPath = "/net/reactivated/Fprint/Manager";
const char *const kManagerIface = "net.reactivated.Fprint.Manager";
const char *const kDeviceIface = "net.reactivated.Fprint.Device";
const char *const kErrorAlreadyInUse = "net.reactivated.Fprint.Error.AlreadyInUse";
const char *const kErrorNoEnrolledPrints = "net.reactivated.Fprint.Error.NoEnrolledPrints";
const char *const kErrorPermissionDenied = "net.reactivated.Fprint.Error.PermissionDenied";

namespace {

struct Finger {
  const char *name, *label;
};

const Finger kFingers[] = {
    {"left-little-finger", N_("Left little finger")},
    {"left-ring-finger", N_("Left ring finger")},
    {"left-middle-finger", N_("Left middle finger")},
    {"left-index-finger", N_("Left index finger")},
    {"left-thumb", N_("Left thumb")},
    {"right-thumb", N_("Right thumb")},
    {"right-index-finger", N_("Right index finger")},
    {"right-middle-finger", N_("Right middle finger")},
    {"right-ring-finger", N_("Right ring finger")},
    {"right-little-finger", N_("Right little finger")},
};

}  // namespace

const std::vector<std::string> &fingers() {
  static const std::vector<std::string> list = [] {
    std::vector<std::string> out;
    for (const Finger &f : kFingers) out.push_back(f.name);
    return out;
  }();
  return list;
}

bool valid_finger(const std::string &name) {
  for (const Finger &f : kFingers)
    if (name == f.name) return true;
  return false;
}

bool left_hand(const std::string &name) { return starts_with(name, "left-"); }

std::string finger_label(const std::string &name) {
  for (const Finger &f : kFingers)
    if (name == f.name) return _(f.label);
  return name;
}

ScanType scan_type(const std::string &value) { return value == "swipe" ? ScanType::swipe : ScanType::press; }

std::string touch_prompt(ScanType scan) {
  return scan == ScanType::swipe ? _("Swipe your finger across the fingerprint reader")
                                 : _("Touch the fingerprint reader");
}

std::string enroll_prompt(ScanType scan) {
  return scan == ScanType::swipe ? _("Swipe this finger across the reader")
                                 : _("Place this finger on the reader, then lift it");
}

EnrollStep enroll_step(const std::string &result, bool done, ScanType scan) {
  EnrollStep s;
  bool swipe = scan == ScanType::swipe;
  s.done = done;
  if (result == "enroll-completed") {
    s.kind = EnrollStep::completed;
    s.done = true;
    s.message = _("The fingerprint was enrolled.");
  } else if (result == "enroll-stage-passed") {
    s.kind = EnrollStep::stage;
    s.message = swipe ? _("Good. Swipe the finger again.") : _("Good. Lift your finger and place it on the reader again.");
  } else if (result == "enroll-retry-scan") {
    s.kind = EnrollStep::retry;
    s.message = swipe ? _("The swipe could not be read. Swipe again.")
                      : _("The scan could not be read. Place your finger on the reader again.");
  } else if (result == "enroll-swipe-too-short") {
    s.kind = EnrollStep::retry;
    s.message = _("The swipe was too short. Swipe the whole finger across the reader.");
  } else if (result == "enroll-finger-not-centered") {
    s.kind = EnrollStep::retry;
    s.message = swipe ? _("Your finger was not centered on the reader. Swipe again.")
                      : _("Your finger was not centered on the reader. Place it in the middle.");
  } else if (result == "enroll-remove-and-retry") {
    s.kind = EnrollStep::retry;
    s.message = _("Lift your finger from the reader, then try again.");
  } else if (result == "enroll-failed") {
    s.kind = EnrollStep::failed;
    s.done = true;
    s.message = _("Enrolling did not work. Try again.");
  } else if (result == "enroll-data-full") {
    s.kind = EnrollStep::failed;
    s.done = true;
    s.message = _("The reader's storage is full: no more fingerprints fit on it. Delete fingerprints "
                  "first (other users' too).");
  } else if (result == "enroll-duplicate") {
    s.kind = EnrollStep::failed;
    s.done = true;
    s.message = _("This fingerprint is enrolled already (perhaps as another finger, or by another user). "
                  "Enroll another finger, or delete the existing fingerprint first.");
  } else if (result == "enroll-disconnected") {
    s.kind = EnrollStep::failed;
    s.done = true;
    s.message = _("The fingerprint reader was disconnected.");
  } else if (result == "enroll-unknown-error") {
    s.kind = EnrollStep::failed;
    s.done = true;
    s.message = _("The fingerprint reader reported an error (usually a driver problem). Try again; "
                  "unplugging the reader or restarting the computer may help.");
  } else if (done) {
    s.kind = EnrollStep::failed;
    s.message = format(_("Enrolling did not work (fprintd reported \"%s\")."), result.c_str());
  } else {
    s.kind = EnrollStep::retry;
    s.message = swipe ? _("Swipe again.") : _("Place your finger on the reader again.");
  }
  return s;
}

VerifyStep verify_step(const std::string &result, bool done, ScanType scan) {
  VerifyStep s;
  bool swipe = scan == ScanType::swipe;
  s.done = done;
  if (result == "verify-match") {
    s.kind = VerifyStep::match;
    s.done = true;
    s.message = _("Fingerprint recognized.");
  } else if (result == "verify-no-match") {
    s.kind = VerifyStep::no_match;
    s.done = true;
    s.message = _("Fingerprint not recognized.");
  } else if (result == "verify-retry-scan") {
    s.kind = VerifyStep::retry;
    s.message = swipe ? _("The swipe could not be read. Swipe again.")
                      : _("The scan could not be read. Touch the reader again.");
  } else if (result == "verify-swipe-too-short") {
    s.kind = VerifyStep::retry;
    s.message = _("The swipe was too short. Swipe the whole finger across the reader.");
  } else if (result == "verify-finger-not-centered") {
    s.kind = VerifyStep::retry;
    s.message = _("Your finger was not centered on the reader. Try again.");
  } else if (result == "verify-remove-and-retry") {
    s.kind = VerifyStep::retry;
    s.message = _("Lift your finger from the reader, then try again.");
  } else if (result == "verify-disconnected") {
    s.kind = VerifyStep::error;
    s.done = true;
    s.device_lost = true;
    s.message = _("The fingerprint reader was disconnected.");
  } else if (result == "verify-unknown-error") {
    s.kind = VerifyStep::error;
    s.done = true;
    s.message = _("The fingerprint reader reported an error (usually a driver problem).");
  } else if (done) {
    s.kind = VerifyStep::error;
    s.message = format(_("The fingerprint could not be checked (fprintd reported \"%s\")."), result.c_str());
  } else {
    s.kind = VerifyStep::retry;
    s.message = touch_prompt(scan);
  }
  return s;
}

bool service_missing(const std::string &error_name) {
  return error_name == DBUS_ERROR_SERVICE_UNKNOWN || error_name == DBUS_ERROR_NAME_HAS_NO_OWNER ||
         error_name == "org.freedesktop.systemd1.NoSuchUnit";
}

std::string error_text(const std::string &name, const std::string &message) {
  const std::string e = "net.reactivated.Fprint.Error.";
  if (service_missing(name)) return _("The fingerprint service (fprintd) is not installed.");
  if (name == DBUS_ERROR_NO_REPLY || name == DBUS_ERROR_TIMEOUT || name == DBUS_ERROR_TIMED_OUT)
    return _("The fingerprint service (fprintd) does not answer.");
  if (name == DBUS_ERROR_NO_SERVER || name == DBUS_ERROR_DISCONNECTED)
    return _("The system's message bus (D-Bus) cannot be reached.");
  if (name == DBUS_ERROR_UNKNOWN_METHOD) return _("This version of fprintd cannot do that.");
  if (name == DBUS_ERROR_ACCESS_DENIED) return _("The system's message bus does not allow this.");
  if (name == e + "PermissionDenied")
    return _("Not allowed: the password was not given, or the system's policy (polkit) refuses it.");
  if (name == e + "AlreadyInUse")
    return _("The fingerprint reader is in use by another program (a login or lock screen, or an "
             "administrator prompt). Try again in a moment.");
  if (name == e + "NoEnrolledPrints") return _("No fingerprints are enrolled.");
  if (name == e + "FingerAlreadyEnrolled")
    return _("This finger is enrolled already. Delete its fingerprint first to enroll it again.");
  if (name == e + "InvalidFingername") return _("fprintd does not know this finger.");
  if (name == e + "NoSuchDevice") return _("The fingerprint reader is not there any more.");
  if (name == e + "PrintsNotDeleted") return _("The fingerprint could not be deleted.");
  if (name == e + "PrintsNotDeletedFromDevice")
    return _("The fingerprint was deleted from this computer, but not from the reader's own storage.");
  if (name == e + "ClaimDevice" || name == e + "NoActionInProgress" || name == e + "Internal")
    return format(_("fprintd reported an error: %s"), message.empty() ? name.c_str() : message.c_str());
  return message.empty() ? name : message;
}

// ---------------------------------------------------------------- calls

void call(dbus::Bus *bus, const std::string &path, const std::string &iface, const std::string &member,
          const std::vector<std::string> &args, ReplyFn fn, int timeout_ms) {
  auto fail = [&fn](const char *name, const char *text) {
    Reply r;
    r.error = name;
    r.message = text;
    if (fn) fn(r);
  };
  if (!bus) {
    fail(DBUS_ERROR_NO_SERVER, "no system bus");
    return;
  }
  DBusMessage *m = dbus::method(kService, path, iface, member);
  if (!m) {
    fail(DBUS_ERROR_INVALID_ARGS, "invalid object path");
    return;
  }
  {
    dbus::Writer w(m);
    for (const std::string &a : args) w.str(a);
  }
  bus->call_async_any(
      m,
      [fn](DBusMessage *reply) {
        Reply r;
        if (!reply) {
          r.error = DBUS_ERROR_NO_REPLY;
          r.message = "no reply";
        } else {
          if (const char *s = dbus_message_get_sender(reply)) r.sender = s;
          if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            const char *name = dbus_message_get_error_name(reply);
            r.error = name ? name : DBUS_ERROR_FAILED;
            std::vector<dbus::Value> a = dbus::read_args(reply);
            if (!a.empty() && a[0].is_string()) r.message = a[0].s;
          } else {
            r.ok = true;
            r.args = dbus::read_args(reply);
          }
        }
        if (fn) fn(r);
      },
      timeout_ms);
}

std::vector<std::string> strings_of(const Reply &r) {
  std::vector<std::string> out;
  if (r.args.empty()) return out;
  for (const dbus::Value &v : r.args[0].items)
    if (v.is_string()) out.push_back(v.s);
  return out;
}

namespace {

std::function<void(const Signal &)> &signal_listener() {
  static std::function<void(const Signal &)> fn;
  return fn;
}

std::function<void(const std::string &, const std::string &)> &owner_listener() {
  static std::function<void(const std::string &, const std::string &)> fn;
  return fn;
}

}  // namespace

void watch_signals(dbus::Bus *bus, std::function<void(const Signal &)> fn) {
  signal_listener() = std::move(fn);
  static bool installed = false;
  if (installed || !bus) return;
  installed = true;
  std::string rule = std::string("type='signal',sender='") + kService + "',interface='" + kDeviceIface + "'";
  bus->add_signal_handler(
      rule,
      [](DBusMessage *m) {
        const char *iface = dbus_message_get_interface(m);
        return iface && !strcmp(iface, kDeviceIface) && dbus_message_get_sender(m) && dbus_message_get_path(m);
      },
      [](DBusMessage *m) {
        auto &fn = signal_listener();
        if (!fn) return;
        Signal s;
        s.sender = dbus_message_get_sender(m);
        s.path = dbus_message_get_path(m);
        const char *member = dbus_message_get_member(m);
        s.member = member ? member : "";
        std::vector<dbus::Value> a = dbus::read_args(m);
        if (a.empty() || a[0].type != 's') return;
        s.result = a[0].s;
        if (s.member == "EnrollStatus" || s.member == "VerifyStatus") {
          if (a.size() < 2 || a[1].type != 'b') return;
          s.done = a[1].b;
        } else if (s.member != "VerifyFingerSelected") {
          return;
        }
        // A copy: the listener may replace itself.
        auto copy = fn;
        copy(s);
      });
}

void watch_owner(dbus::Bus *bus, std::function<void(const std::string &, const std::string &)> fn) {
  owner_listener() = std::move(fn);
  static bool installed = false;
  if (installed || !bus) return;
  installed = true;
  std::string rule = std::string("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                                 "member='NameOwnerChanged',arg0='") +
                     kService + "'";
  bus->add_signal_handler(
      rule,
      [](DBusMessage *m) {
        const char *sender = dbus_message_get_sender(m);
        return sender && !strcmp(sender, DBUS_SERVICE_DBUS) &&
               dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged");
      },
      [](DBusMessage *m) {
        std::vector<dbus::Value> a = dbus::read_args(m);
        if (a.size() < 3 || a[0].s != kService) return;
        auto copy = owner_listener();
        if (copy) copy(a[1].s, a[2].s);
      });
}

// ------------------------------------------------------------- drawing

void draw_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  p.set(c);
  cairo_set_line_width(cr, std::max(1.1, s * 0.07));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  double cx = x + s * 0.5, cy = y + s * 0.5;
  auto deg = [](double d) { return d * M_PI / 180.0; };
  // The outer ridge: an arch over the top.
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, s * 0.45, deg(205), deg(335));
  cairo_stroke(cr);
  // Ridges that come down on both sides, shorter on the right.
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy + s * 0.04, s * 0.34, deg(168), deg(372));
  cairo_stroke(cr);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy + s * 0.08, s * 0.23, deg(160), deg(380));
  cairo_line_to(cr, cx + s * 0.20, cy + s * 0.42);
  cairo_stroke(cr);
  cairo_new_path(cr);
  cairo_move_to(cr, cx - s * 0.22, cy + s * 0.18);
  cairo_line_to(cr, cx - s * 0.20, cy + s * 0.36);
  cairo_stroke(cr);
  // The core: a loop that runs down to the bottom.
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy + s * 0.12, s * 0.11, deg(180), deg(360));
  cairo_line_to(cr, cx + s * 0.08, cy + s * 0.47);
  cairo_stroke(cr);
  cairo_new_path(cr);
  cairo_move_to(cr, cx - s * 0.11, cy + s * 0.12);
  cairo_line_to(cr, cx - s * 0.08, cy + s * 0.30);
  cairo_stroke(cr);
  cairo_restore(cr);
}

}  // namespace fprint
}  // namespace sde
