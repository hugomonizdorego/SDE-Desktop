// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "fingerprint.h"

#include <FL/Fl.H>

#include <algorithm>
#include <vector>

#include "fprint.h"
#include "i18n.h"

namespace sde {
namespace lock {

namespace {

enum class Phase {
  idle,         // not started
  finding,      // GetDevices, ListEnrolledFingers
  claiming,     // Claim
  starting,     // VerifyStart
  verifying,    // waiting for a finger
  pausing,      // between attempts, or before trying again
  holding,      // claimed, not verifying (off, too many failures, given up)
  released,     // Switch User
  unavailable,  // no fprintd, no reader with the user's fingerprints
  matched,      // a finger was recognized
};

const char *kVerifyStop = "VerifyStop";

}  // namespace

struct FingerprintUnlock::Impl : std::enable_shared_from_this<FingerprintUnlock::Impl> {
  dbus::Bus *bus = nullptr;
  Options opt;
  Phase phase = Phase::idle;
  unsigned generation = 0;  // answers and timers of an older attempt are dropped
  std::string path, owner;  // the claimed device, the fprintd that answered
  fprint::ScanType scan = fprint::ScanType::press;
  bool claimed = false;
  int failures = 0;  // fingers not recognized
  int errors = 0;    // failed calls or reader errors in a row
  int restarts = 0;  // fprintd went away
  bool exhausted = false;   // verifying is over for this lock
  bool wake_again = false;  // wake() starts again (released, or fprintd lost)
  std::string text;
  bool error = false;

  void say(const std::string &t, bool err) {
    if (t == text && err == error) return;
    text = t;
    error = err;
    if (opt.changed) opt.changed();
  }
  void log(const std::string &line) {
    if (opt.log) opt.log(line);
  }

  // Runs |fn| after |seconds| if this attempt is still the current one.
  void later(double seconds, std::function<void(Impl &)> fn) {
    std::weak_ptr<Impl> self = shared_from_this();
    unsigned gen = generation;
    auto *job = new std::function<void()>([self, gen, fn] {
      auto me = self.lock();
      if (me && me->generation == gen) fn(*me);
    });
    Fl::add_timeout(seconds,
                    [](void *d) {
                      std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(d));
                      (*f)();
                    },
                    job);
  }

  // A reply handler that runs only while this Impl lives and the attempt
  // it belongs to is current.
  fprint::ReplyFn current(std::function<void(Impl &, const fprint::Reply &)> fn) {
    std::weak_ptr<Impl> self = shared_from_this();
    unsigned gen = generation;
    return [self, gen, fn](const fprint::Reply &r) {
      auto me = self.lock();
      if (me && me->generation == gen) fn(*me, r);
    };
  }

  void start() {
    if ((!opt.verify && !opt.hold) || phase == Phase::matched) return;
    ++generation;
    wake_again = false;
    phase = Phase::finding;
    fprint::call(bus, fprint::kManagerPath, fprint::kManagerIface, "GetDevices", {},
                 current([](Impl &me, const fprint::Reply &r) {
                   if (!r.ok) {
                     me.phase = Phase::unavailable;
                     if (!fprint::service_missing(r.error)) me.log("fingerprint: GetDevices: " + r.error);
                     return;
                   }
                   std::vector<std::string> paths;
                   for (const std::string &p : fprint::strings_of(r))
                     if (dbus::valid_path(p)) paths.push_back(p);
                   me.try_device(paths, 0);
                 }));
  }

  // The first reader that has fingerprints of this user (as pam_fprintd).
  void try_device(std::vector<std::string> paths, size_t i) {
    if (i >= paths.size()) {
      phase = Phase::unavailable;
      say("", false);
      return;
    }
    std::string p = paths[i];
    fprint::call(bus, p, fprint::kDeviceIface, "ListEnrolledFingers", {""},
                 current([paths, i, p](Impl &me, const fprint::Reply &r) {
                   if (r.ok && !fprint::strings_of(r).empty()) {
                     me.path = p;
                     me.claim();
                   } else {
                     me.try_device(paths, i + 1);
                   }
                 }));
  }

  void claim() {
    phase = Phase::claiming;
    if (bus)
      dbus::get_all_properties(bus, fprint::kService, path, fprint::kDeviceIface,
                               [self = std::weak_ptr<Impl>(shared_from_this())](const dbus::Value &props) {
                                 auto me = self.lock();
                                 if (!me || !props.valid()) return;
                                 if (const dbus::Value *v = props.get("scan-type")) me->scan = fprint::scan_type(v->str());
                                 if (me->phase == Phase::verifying) me->say(fprint::touch_prompt(me->scan), false);
                               });
    fprint::call(bus, path, fprint::kDeviceIface, "Claim", {""}, current([](Impl &me, const fprint::Reply &r) {
                   if (!r.ok) {
                     me.retry(r, "Claim");
                     return;
                   }
                   me.claimed = true;
                   me.owner = r.sender;
                   me.errors = 0;
                   me.log("fingerprint: holding the reader " + me.path);
                   if (me.opt.verify && !me.exhausted) {
                     me.verify();
                   } else {
                     me.phase = Phase::holding;
                     me.say(me.exhausted ? me.text : std::string(), me.exhausted);
                   }
                 }));
  }

  void verify() {
    phase = Phase::starting;
    fprint::call(bus, path, fprint::kDeviceIface, "VerifyStart", {"any"}, current([](Impl &me, const fprint::Reply &r) {
                   if (!r.ok) {
                     if (r.error == fprint::kErrorNoEnrolledPrints) {
                       me.phase = Phase::holding;
                       me.say("", false);
                       return;
                     }
                     me.retry(r, "VerifyStart");
                     return;
                   }
                   me.errors = 0;
                   me.phase = Phase::verifying;
                   me.say(fprint::touch_prompt(me.scan), false);
                 }));
  }

  // A call failed: the reader may be in use by another program (an
  // administrator prompt that asks for a finger), or fprintd restarted.
  // Try again later, more slowly each time.
  void retry(const fprint::Reply &r, const char *what) {
    ++errors;
    log(std::string("fingerprint: ") + what + ": " + r.error + (r.message.empty() ? "" : " (" + r.message + ")"));
    phase = Phase::pausing;
    if (errors >= 3 && opt.verify && !exhausted) say(fprint::error_text(r.error, r.message), true);
    later(std::min(30.0, 2.0 * errors), [](Impl &me) {
      if (me.phase != Phase::pausing) return;
      if (me.claimed)
        me.verify();
      else
        me.start();
    });
  }

  // VerifyStop (fprintd wants it after every result), then |next|.
  void stop_then(std::function<void(Impl &)> next) {
    fprint::call(bus, path, fprint::kDeviceIface, kVerifyStop, {},
                 current([next](Impl &me, const fprint::Reply &) { next(me); }));
  }

  void on_signal(const fprint::Signal &s) {
    if (!claimed || s.sender != owner || s.path != path || s.member != "VerifyStatus") return;
    if (phase != Phase::verifying && phase != Phase::starting) return;
    if (opt.activity) opt.activity();
    fprint::VerifyStep st = fprint::verify_step(s.result, s.done, scan);
    switch (st.kind) {
      case fprint::VerifyStep::match:
        phase = Phase::matched;
        log("fingerprint: recognized");
        say(_("Fingerprint recognized."), false);
        stop_then([](Impl &) {});
        if (opt.matched) opt.matched();
        return;
      case fprint::VerifyStep::no_match:
        ++failures;
        log("fingerprint: not recognized (" + std::to_string(failures) + " of " +
            std::to_string(kMaxFingerprintFailures) + ")");
        phase = Phase::pausing;
        if (failures >= kMaxFingerprintFailures) {
          exhausted = true;
          phase = Phase::holding;
          say(_("Too many failed fingerprint attempts. Type your password to unlock."), true);
          stop_then([](Impl &) {});
        } else {
          say(_("Fingerprint not recognized. Try again."), true);
          stop_then([](Impl &me) {
            me.later(0.8, [](Impl &me2) {
              if (me2.phase == Phase::pausing && me2.claimed) me2.verify();
            });
          });
        }
        return;
      case fprint::VerifyStep::retry:
        say(st.message, false);
        return;
      case fprint::VerifyStep::error:
        if (st.device_lost) {
          // fprintd: do not use the device any more.
          claimed = false;
          owner.clear();
          phase = Phase::unavailable;
          say(st.message, true);
          return;
        }
        ++errors;
        phase = Phase::pausing;
        if (errors >= 3) {
          phase = Phase::holding;
          say(_("The fingerprint reader does not work right now. Type your password."), true);
          stop_then([](Impl &) {});
        } else {
          say(_("The fingerprint reader reported an error. Trying again..."), true);
          stop_then([](Impl &me) {
            me.later(2.0 * me.errors, [](Impl &me2) {
              if (me2.phase == Phase::pausing && me2.claimed) me2.verify();
            });
          });
        }
        return;
    }
  }

  void on_owner(const std::string &old_owner, const std::string &) {
    // fprintd exited or crashed: the claim went with it.
    if (!claimed || old_owner != owner || phase == Phase::matched) return;
    claimed = false;
    owner.clear();
    ++generation;
    phase = Phase::unavailable;
    say("", false);
    log("fingerprint: fprintd went away");
    wake_again = true;
    if (++restarts <= 3)
      later(2.0, [](Impl &me) {
        if (me.phase == Phase::unavailable) me.start();
      });
  }

  void release() {
    if (!claimed) return;
    std::string p = path;
    bool verifying = phase == Phase::verifying || phase == Phase::starting;
    claimed = false;
    owner.clear();
    dbus::Bus *b = bus;
    auto release_call = [b, p] { fprint::call(b, p, fprint::kDeviceIface, "Release", {}, nullptr); };
    if (verifying)
      fprint::call(bus, p, fprint::kDeviceIface, kVerifyStop, {}, [release_call](const fprint::Reply &) { release_call(); });
    else
      release_call();
  }
};

FingerprintUnlock::FingerprintUnlock(dbus::Bus *bus, Options options) : impl_(std::make_shared<Impl>()) {
  impl_->bus = bus;
  impl_->opt = std::move(options);
  std::weak_ptr<Impl> self = impl_;
  fprint::watch_signals(bus, [self](const fprint::Signal &s) {
    if (auto me = self.lock()) me->on_signal(s);
  });
  fprint::watch_owner(bus, [self](const std::string &old_owner, const std::string &new_owner) {
    if (auto me = self.lock()) me->on_owner(old_owner, new_owner);
  });
}

FingerprintUnlock::~FingerprintUnlock() {
  impl_->release();
  fprint::watch_signals(impl_->bus, nullptr);
  fprint::watch_owner(impl_->bus, nullptr);
}

void FingerprintUnlock::start() {
  if (impl_->phase == Phase::idle || impl_->phase == Phase::released) impl_->start();
}

void FingerprintUnlock::suspend() {
  Impl &i = *impl_;
  if (i.phase == Phase::matched || i.phase == Phase::idle) return;
  ++i.generation;
  i.release();
  i.phase = Phase::released;
  i.wake_again = true;
  i.say("", false);
}

void FingerprintUnlock::wake() {
  Impl &i = *impl_;
  if (i.wake_again && (i.phase == Phase::released || i.phase == Phase::unavailable)) i.start();
}

void FingerprintUnlock::give_up(const std::string &why) {
  Impl &i = *impl_;
  i.exhausted = true;
  ++i.generation;
  i.phase = Phase::holding;
  i.say(why, true);
  if (i.claimed) i.stop_then([](Impl &) {});
}

const std::string &FingerprintUnlock::text() const { return impl_->text; }
bool FingerprintUnlock::error() const { return impl_->error; }
bool FingerprintUnlock::listening() const { return impl_->phase == Phase::verifying; }
int FingerprintUnlock::failures() const { return impl_->failures; }

}  // namespace lock
}  // namespace sde
