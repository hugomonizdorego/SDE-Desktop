// SPDX-License-Identifier: GPL-3.0-or-later
// Programs SDE started (0.2.0 Alpha 3.1, docs/FORCE-CLOSE.md).
//
// launch_entry() (Start menu, taskbar, desktop, search, ...) and sde-run
// leave a small record for every application they start in
// $XDG_RUNTIME_DIR/sde/launches/<pid>: its process, when it started, and
// its name, icon and desktop file. sde-watchdog reads them to notice a
// program that keeps running without ever showing a window ("<App> has not
// opened yet"); sde-session, which adopts the detached programs as the
// session's child subreaper, reads them when one of them dies from a signal
// ("<App> closed unexpectedly") and removes them when they end.
//
// Records are written only inside an SDE session (sde-session exports
// SDE_SESSION_VERSION), so tests and programs run elsewhere leave nothing
// behind.
#pragma once

#include <string>
#include <vector>

namespace sde {
struct DesktopEntry;
namespace launches {

struct Record {
  long pid = 0;
  unsigned long long start = 0;  // the process's start time (/proc/<pid>/stat), against pid reuse
  long long time = 0;            // when SDE started it (seconds since the epoch)
  std::string name, icon, desktop_id;
  std::vector<std::string> argv;
};

// The directory of the records: $XDG_RUNTIME_DIR/sde/launches (see
// runtime_dir()); $SDE_LAUNCHES_DIR replaces it (tests).
std::string dir();

std::string serialize(const Record &r);
bool parse(const std::string &text, Record &out);

// Writes, reads and removes the record of |pid| (atomically: a reader sees
// a whole record or none).
bool save(const Record &r);
bool load(long pid, Record &out);
void remove(long pid);
// Every record, in no particular order (files that are not records are
// skipped).
std::vector<Record> all();

// The start time of process |pid| (field 22 of <proc>/<pid>/stat), 0 when
// there is no such process.
unsigned long long start_time(long pid, const std::string &proc = "/proc");
// True when |r|'s process still runs (the same process, not a new one that
// got its number).
bool alive(const Record &r, const std::string &proc = "/proc");

// Whether launches are recorded here (inside an SDE session, or with
// $SDE_LAUNCHES_DIR set).
bool enabled();
// Records the start of |entry| as process |pid|, run as |argv|.
void record(long pid, const DesktopEntry &entry, const std::vector<std::string> &argv);
// Records the start of |argv| (sde-run): only when its program is an
// installed application (an Exec= of a desktop file), whose name and icon
// are used; plain commands are not watched.
void record_command(long pid, const std::vector<std::string> &argv);

}  // namespace launches
}  // namespace sde
