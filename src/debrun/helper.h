// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The contract between Debrun (as the user) and debrun-helper (as root,
// through pkexec): its arguments, their validation and the lines it prints.
// Shared by both programs and unit tested.
//
//   debrun-helper install --sha256 HEX --package NAME --version VERSION
//                 --arch ARCH [--reinstall | --downgrade]
//                 [--allow-remove NAME]... FILE
//   debrun-helper remove --package NAME [--allow-remove NAME]...
//   debrun-helper refresh
//
// FILE is the absolute path of the .deb the user saw; HEX its SHA-256 as
// Debrun computed it. The helper opens FILE as the calling user, copies it
// to a directory only root can write to, checks the digest and the control
// fields of the copy, simulates the change and refuses removals the user
// did not see (--allow-remove), then installs the copy with apt-get.
//
// Output, one record per line:
//   DEBRUN-HELPER 1
//   PHASE copy|check|wait|download|unpack|configure|remove|refresh
//   PROGRESS PERCENT PHASE PACKAGE    (PERCENT of the whole change, -1 unknown)
//   LOG TEXT                          (what apt-get and dpkg printed)
//   ERROR CODE TEXT                   (CODE below)
//   RESULT ok|failed|refused
// Exit status: 0 done, 1 failed, 2 refused before anything changed.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace debrun {
namespace helper {

extern const char *const kProtocol;  // "DEBRUN-HELPER 1"

// Error codes of ERROR lines.
//   usage      arguments refused (a bug, or someone else calling it)
//   root       not running as root
//   open       FILE cannot be opened by the user (or is not a regular file)
//   changed    FILE is not what the user saw (digest or control fields differ)
//   arch       the architecture is not enabled on this computer
//   unmet      dependencies cannot be met from the package sources
//   held       a package involved is held (apt-mark hold)
//   essential  it would remove an essential or protected package
//   removals   it would remove packages the user was not shown
//   lock       another package manager kept the package system busy
//   space      not enough disk space
//   failed     apt-get or dpkg failed (the LOG lines say why)

struct Request {
  std::string action;  // install, remove, refresh
  std::string file, sha256, package, version, arch;
  bool reinstall = false, downgrade = false;
  std::vector<std::string> allow_remove;
};

// Checks the helper's arguments (without the program name): only the
// forms above, each option once (--allow-remove up to 500 times), package
// names, versions and architectures in Debian syntax, an absolute FILE
// without control characters. False with *error.
bool parse_request(const std::vector<std::string> &args, Request *out, std::string *error);
// The arguments for |r| (the client side).
std::vector<std::string> request_args(const Request &r);

// Packages Debrun never removes: the desktop itself and its software center.
bool protected_package(const std::string &name);

// One line of apt-get's APT::Status-Fd ("pmstatus:sl:40:Unpacking sl
// (amd64)", "dlstatus:1:12.5:Retrieving file 1 of 3"). |percent| becomes
// the share of the whole change: downloads take the first 40 % of an
// installation, dpkg the rest (a refresh is all download).
struct Progress {
  int percent = -1;
  std::string phase;    // download, unpack, configure, remove, trigger
  std::string package;  // "" for downloads and dpkg as a whole
  std::string error;    // pmerror text
};
bool parse_status_line(const std::string &line, const std::string &action, Progress *out);

// One line of the helper's output, for Debrun.
struct Event {
  enum class Type { none, hello, phase, progress, log, error, result };
  Type type = Type::none;
  std::string word;     // the phase, error code or result
  std::string package;  // PROGRESS
  std::string text;     // LOG and ERROR
  int percent = -1;
};
Event parse_event(const std::string &line);

}  // namespace helper
}  // namespace debrun
}  // namespace sde
