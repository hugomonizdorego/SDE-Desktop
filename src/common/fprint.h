// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// fprintd, the fingerprint service of Linux distributions
// (net.reactivated.Fprint on the system bus, docs/FINGERPRINT.md): the names
// of its D-Bus API, the ten fingers, what its statuses and errors mean for
// the person at the reader, asynchronous calls from the FLTK event loop and
// the fingerprint symbol. PAM's side is in fprint_pam.h.
//
// Shared by Control Center > Fingerprint (enrolling, deleting, testing) and
// SDE Screen Lock (unlocking with a finger). Signals are matched by the bus
// on fprintd's well-known name; the callers also compare the sender with the
// unique name that answered their Claim before they believe a status.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "dbus.h"
#include "theme.h"

namespace sde {

class Painter;

namespace fprint {

extern const char *const kService;      // net.reactivated.Fprint
extern const char *const kManagerPath;  // /net/reactivated/Fprint/Manager
extern const char *const kManagerIface;
extern const char *const kDeviceIface;
// The error names of fprintd (net.reactivated.Fprint.Error.*).
extern const char *const kErrorAlreadyInUse;
extern const char *const kErrorNoEnrolledPrints;
extern const char *const kErrorPermissionDenied;

// fprintd's finger names in the order they are drawn: the left hand from
// the little finger to the thumb, then the right hand from the thumb to the
// little finger.
const std::vector<std::string> &fingers();
bool valid_finger(const std::string &name);
bool left_hand(const std::string &name);
// "Right index finger" (translated); |name| itself when it is not a finger.
std::string finger_label(const std::string &name);

// Readers that are touched (most laptop readers) or swiped (the device's
// "scan-type" property).
enum class ScanType { press, swipe };
ScanType scan_type(const std::string &value);

// What to tell the person before the first touch.
std::string touch_prompt(ScanType scan);
// The same while a finger is enrolled ("with this finger").
std::string enroll_prompt(ScanType scan);

// One EnrollStatus signal.
struct EnrollStep {
  enum Kind { stage, retry, completed, failed };
  Kind kind = failed;
  bool done = true;      // the enrollment is over (EnrollStop is due)
  std::string message;   // translated
};
EnrollStep enroll_step(const std::string &result, bool done, ScanType scan);

// One VerifyStatus signal.
struct VerifyStep {
  enum Kind { match, no_match, retry, error };
  Kind kind = error;
  bool done = true;      // the verification is over (VerifyStop is due)
  bool device_lost = false;  // verify-disconnected: do not use the reader again
  std::string message;   // translated
};
VerifyStep verify_step(const std::string &result, bool done, ScanType scan);

// What to tell the person about a failed call: fprintd's error |name|
// (net.reactivated.Fprint.Error.*, or one of D-Bus) and its |message|.
std::string error_text(const std::string &name, const std::string &message);
// fprintd is not installed (nothing owns or can start its name).
bool service_missing(const std::string &error_name);

// ---------------------------------------------------------------- calls

// The answer to one call.
struct Reply {
  bool ok = false;
  std::string error, message;  // D-Bus error name and text, when !ok
  std::string sender;          // unique bus name of the fprintd that answered
  std::vector<dbus::Value> args;
};
using ReplyFn = std::function<void(const Reply &)>;

// Calls |member| of |iface| at |path| of fprintd with string arguments.
// |fn| runs from the event loop with the answer, also when none came
// (org.freedesktop.DBus.Error.NoReply) or there is no system bus.
// Enrolling and deleting wait for polkit, which may ask for a password:
// those calls need long timeouts.
void call(dbus::Bus *bus, const std::string &path, const std::string &iface, const std::string &member,
          const std::vector<std::string> &args, ReplyFn fn, int timeout_ms = 25000);

// The strings of an "as" answer (GetDevices, ListEnrolledFingers).
std::vector<std::string> strings_of(const Reply &r);

// A device signal: EnrollStatus and VerifyStatus (result, done) or
// VerifyFingerSelected (result = the finger).
struct Signal {
  std::string sender, path, member, result;
  bool done = false;
};
// Delivers the signals of fprintd's devices to |fn|. One listener per
// program; a later call replaces it (nullptr: none).
void watch_signals(dbus::Bus *bus, std::function<void(const Signal &)> fn);
// Calls |fn| when the owner of fprintd's name changes (it started, exited or
// was restarted), with the old and the new unique name ("" for none).
void watch_owner(dbus::Bus *bus, std::function<void(const std::string &, const std::string &)> fn);

// ------------------------------------------------------------- drawing

// The fingerprint symbol (ridges around a core) in a |size| square.
void draw_glyph(Painter &p, double x, double y, double size, const Color &c);

}  // namespace fprint
}  // namespace sde
