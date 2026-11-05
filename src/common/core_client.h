// SPDX-License-Identifier: GPL-3.0-or-later
// The client of the SDE core controller (0.2.0 Alpha 4, docs/CORE.md).
//
// sde-core (src/core) owns org.selviana.Core on the session bus while an
// SDE session runs (sde-session starts and supervises it). SDE programs
// write settings, follow changes of the settings and the look, report
// problems and keep a single instance through it. Programs run outside SDE
// (or while sde-core restarts) still work: every call falls back to what
// SDE did before, sde.conf written directly and announced with the
// _SDE_CONFIG_SERIAL property of the root window, which is also how
// changes are followed then. The fallbacks stay for one release
// (0.2.0 Alpha 4) for programs that do not use this client yet.
//
// Everything here runs in the FLTK event loop (dbus.h) and never connects
// when no session bus is known: no bus is started for a program on its own.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sde {

class Config;

namespace core {

constexpr const char *kName = "org.selviana.Core";
constexpr const char *kPath = "/org/selviana/Core";
constexpr const char *kIface = "org.selviana.Core";
constexpr const char *kSettingsIface = "org.selviana.Core.Settings";
constexpr const char *kLookIface = "org.selviana.Core.Look";
constexpr const char *kAppsIface = "org.selviana.Core.Apps";
constexpr const char *kSessionIface = "org.selviana.Core.Session";
// What an application with a single instance exports for Activate.
constexpr const char *kInstancePath = "/org/selviana/Core/Instance";
constexpr const char *kInstanceIface = "org.selviana.Core.Instance";
constexpr const char *kErrorPrefix = "org.selviana.Core.Error.";
constexpr unsigned kApiVersion = 1;

using Key = std::pair<std::string, std::string>;  // section, key

// Whether sde-core runs now. It is asked once, without blocking, the first
// time something here is used, and followed from then on; until the
// answer comes this is false (the fallbacks are used).
bool available();
// Waits up to |timeout_ms| for that first answer (programs that decide at
// start, such as single_instance()). Returns available().
bool wait_available(int timeout_ms = 300);

// ---- Settings

// A write: |value| empty and |reset| true returns the key to its default.
struct Write {
  std::string section, key, value;
  bool reset = false;
};

enum class Mode {
  NORMAL,
  // A theme switch: the session switches every program in one calm step
  // (docs/THEMES.md) and the change is announced when it shows it.
  APPEARANCE,
};

// Writes |writes| through sde-core when it runs (checked against the
// schema, merged with what other programs wrote, announced with Changed and
// LookChanged); otherwise, or when sde-core does not answer, into sde.conf
// directly (still checked against the schema, merged with the file as it is
// on disk) with the announcement of 0.2.0 Alpha 3 (announce_config_change(),
// or apply_appearance() for Mode::APPEARANCE). Values the schema refuses
// are not written; the reason is printed. Returns at once.
void write(const std::vector<Write> &writes, Mode mode = Mode::NORMAL);
void set(const std::string &section, const std::string &key, const std::string &value);
void reset(const std::string &section, const std::string &key);

// The writes that turn |before| into |after| (their user values).
std::vector<Write> diff(const Config &before, const Config &after);

// |fn| runs (from the event loop) when settings change: with the keys from
// sde-core's Changed signal while it runs, else with an empty list when
// another program announced a change on the root window. Returns an id for
// unwatch().
int watch_settings(std::function<void(const std::vector<Key> &keys)> fn);
// |fn| runs when the look changes: sde-core's LookChanged signal while it
// runs, else a change announced on the root window (any setting: the
// programs of 0.2.0 Alpha 3 did not say which).
int watch_look(std::function<void()> fn);
void unwatch(int id);

// True when |keys| touch the look ([appearance], [aero], [compositor]); an
// empty list (a change of unknown keys) counts.
bool touches_look(const std::vector<Key> &keys);

// ---- Apps

// Makes this program the single instance of |app_id| while sde-core runs:
// when another instance is running it gets |args| (its |activated| is
// called with them) and this returns false: the caller quits. Otherwise
// this process becomes that instance, |activated| receives the arguments of
// later starts, and true is returned. Without sde-core every start is its
// own instance (true). Blocks for at most about a second.
bool single_instance(const std::string &app_id, const std::vector<std::string> &args,
                     std::function<void(const std::vector<std::string> &args)> activated);

// Tells sde-core about a problem or a state (the session and the watchdog):
// kind is crashed, not-responding, responding, not-opened, opened, ended,
// component-started, component-stopped, component-crashed or
// component-given-up. Nothing happens without sde-core.
void report(const std::string &kind, long pid, const std::string &name, const std::string &id,
            const std::string &detail);

// For the tests: forget the bus state (the next call asks again).
void reset_for_tests();

}  // namespace core
}  // namespace sde
