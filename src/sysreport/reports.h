// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE System Reports model (sde-system-reports): crash reports of SDE
// programs and a system information summary for bug reports.
//
// The crash handler of sde-common (src/common/crash.cpp) writes
//
//   sde-panel crashed: Segmentation fault. Backtrace (please attach it to a bug report):
//   sde-panel(+0x1234)[0x55...]
//   ...
//   -- end of backtrace
//
// to stderr, which sde-session sends to ~/.local/state/sde/session.log
// (the previous session's log is session.log.old). sde-session itself logs
// "HH:MM:SS sde-session: panel exited (killed by signal 11 (Segmentation
// fault))" for components that stopped. Both become reports here.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace reports {

struct CrashReport {
  std::string program;    // "sde-panel"
  std::string signal;     // "Segmentation fault"
  std::string time;       // "14:02:11" when the log has a time stamp nearby
  std::string source;     // log file it came from
  std::string backtrace;  // the frames, one per line ("" for exit reports)
  bool has_backtrace = false;
  // Stable identifier (program + signal + first frames) to group repeats.
  std::string signature() const;
};

// Crash reports in a log, oldest first.
std::vector<CrashReport> parse_crash_log(const std::string &log, const std::string &source = "");
// Reports with the same signature folded into one, |count| filled in.
struct Grouped {
  CrashReport report;
  int count = 1;
};
std::vector<Grouped> group(const std::vector<CrashReport> &reports);

// Log files that may hold reports: ~/.local/state/sde/*.log and *.log.old
// (session.log first). $SDE_REPORTS_DIR overrides the directory (tests).
std::vector<std::string> report_files();
// Every report of report_files(), newest file last.
std::vector<CrashReport> collect();

// Percent-encoding for URL query values (RFC 3986 unreserved kept).
std::string url_encode(const std::string &s);
// https://github.com/hugomonizdorego/SDE-Desktop/issues/new?title=...&body=...
// The body is shortened (backtrace first) so the URL stays below |max_len|.
std::string issue_url(const std::string &title, const std::string &body, size_t max_len = 7000);
// Issue title and Markdown body for a report.
std::string issue_title(const CrashReport &r, const std::string &version);
std::string report_text(const CrashReport &r, const std::string &system_info);

// "Key: value" lines describing this computer: SDE and OS version, kernel,
// CPU, memory, graphics cards, virtual machine, session, display.
std::string system_summary();

// Plain-text parsers used by system_summary(), tested with fixtures.
std::string cpu_model(const std::string &cpuinfo);        // "model name" of the first CPU
std::string memory_total(const std::string &meminfo);     // "15.5 GiB"

}  // namespace reports
}  // namespace sde
