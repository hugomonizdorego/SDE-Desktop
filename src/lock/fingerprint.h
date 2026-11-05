// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE Screen Lock: unlocking with a fingerprint (docs/FINGERPRINT.md).
//
// While the screen is locked, the lock screen holds the fingerprint reader
// that has the user's fingerprints (fprintd's Claim) and runs Verify on it
// next to the password field: "Touch the fingerprint reader". A recognized
// finger unlocks after the account checks of PAM service
// sde-lock-fingerprint. After kMaxFingerprintFailures fingers that are not
// recognized only the password unlocks: verifying stops, but the reader
// stays claimed, so that the password's own PAM stack (common-auth may run
// pam_fprintd, which would wait for a finger before it reads the password)
// checks the password at once. For the same reason the reader is held
// without verifying when fingerprint unlocking is off but common-auth runs
// pam_fprintd for sde-lock. Switch User releases the reader (the greeter
// may want it); the next key or click takes it again.
//
// Statuses are only believed from the fprintd that answered the Claim
// (its unique bus name: only root may own net.reactivated.Fprint) and for
// the claimed device, as pam_fprintd does; fprintd itself checks polkit's
// net.reactivated.fprint.device.verify (allowed for the user of the active
// session). Nothing waits and nothing polls: the D-Bus calls and signals
// are handled from the event loop, and attempts after a reader error back
// off.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "dbus.h"

namespace sde {
namespace lock {

constexpr int kMaxFingerprintFailures = 5;

class FingerprintUnlock {
 public:
  struct Options {
    bool verify = true;  // unlock with a finger ([lock] fingerprint)
    bool hold = false;   // claim the reader even when not verifying
    std::function<void()> changed;   // text() or listening() changed: redraw
    std::function<void()> activity;  // the reader saw a finger (wake the monitors)
    std::function<void()> matched;   // a finger was recognized
    std::function<void(const std::string &)> log;
  };

  FingerprintUnlock(dbus::Bus *bus, Options options);
  ~FingerprintUnlock();
  FingerprintUnlock(const FingerprintUnlock &) = delete;
  FingerprintUnlock &operator=(const FingerprintUnlock &) = delete;

  // Looks for a reader with the user's fingerprints and claims it.
  void start();
  // Switch User: releases the reader.
  void suspend();
  // A key or a click: takes the reader again after suspend() or after it
  // was lost (fprintd restarted); nothing after too many failures.
  void wake();
  // Verifying ends for this lock (the account check refused a finger);
  // the reader stays claimed. |why| is shown.
  void give_up(const std::string &why);

  // The line under the password field ("" for none) and its style.
  const std::string &text() const;
  bool error() const;
  // Verifying now: the fingerprint symbol is shown.
  bool listening() const;
  int failures() const;

  struct Impl;

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace lock
}  // namespace sde
