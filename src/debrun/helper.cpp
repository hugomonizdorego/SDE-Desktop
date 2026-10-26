// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "helper.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "deb.h"
#include "sha256.h"
#include "util.h"

namespace sde {
namespace debrun {
namespace helper {

const char *const kProtocol = "DEBRUN-HELPER 1";

namespace {

bool clean_text(const std::string &s) {
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7F) return false;
  return true;
}

}  // namespace

bool parse_request(const std::vector<std::string> &args, Request *out, std::string *error) {
  *out = Request();
  auto fail = [&](const std::string &why) {
    if (error) *error = why;
    return false;
  };
  if (args.empty()) return fail("no action");
  out->action = args[0];
  if (out->action != "install" && out->action != "remove" && out->action != "refresh")
    return fail("unknown action " + out->action);
  std::vector<std::string> seen;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string &a = args[i];
    if (a == "--reinstall" || a == "--downgrade") {
      if (out->action != "install") return fail(a + " is only for install");
      if (std::find(seen.begin(), seen.end(), a) != seen.end()) return fail("twice: " + a);
      seen.push_back(a);
      (a == "--reinstall" ? out->reinstall : out->downgrade) = true;
      continue;
    }
    if (a == "--sha256" || a == "--package" || a == "--version" || a == "--arch" || a == "--allow-remove") {
      if (i + 1 >= args.size()) return fail("missing value of " + a);
      const std::string &v = args[++i];
      if (a == "--allow-remove") {
        if (out->action == "refresh") return fail(a + " is not for refresh");
        if (!valid_package_name(v)) return fail("not a package name: " + v);
        if (out->allow_remove.size() >= 500) return fail("too many packages");
        out->allow_remove.push_back(v);
        continue;
      }
      if (std::find(seen.begin(), seen.end(), a) != seen.end()) return fail("twice: " + a);
      seen.push_back(a);
      if (a == "--package") {
        if (out->action == "refresh") return fail(a + " is not for refresh");
        if (!valid_package_name(v)) return fail("not a package name: " + v);
        out->package = v;
      } else {
        if (out->action != "install") return fail(a + " is only for install");
        if (a == "--sha256" && !valid_sha256_hex(v)) return fail("not a SHA-256 digest");
        if (a == "--version" && !valid_version(v)) return fail("not a version: " + v);
        if (a == "--arch" && !valid_architecture(v)) return fail("not an architecture: " + v);
        (a == "--sha256" ? out->sha256 : a == "--version" ? out->version : out->arch) = v;
      }
      continue;
    }
    if (out->action != "install" || !out->file.empty()) return fail("unexpected argument");
    if (a.empty() || a[0] != '/' || a.size() > 4096 || !clean_text(a)) return fail("FILE must be an absolute path");
    out->file = a;
  }
  if (out->reinstall && out->downgrade) return fail("--reinstall and --downgrade together");
  if (out->action == "install" &&
      (out->file.empty() || out->sha256.empty() || out->package.empty() || out->version.empty() || out->arch.empty()))
    return fail("install needs FILE, --sha256, --package, --version and --arch");
  if (out->action == "remove" && out->package.empty()) return fail("remove needs --package");
  return true;
}

std::vector<std::string> request_args(const Request &r) {
  std::vector<std::string> out = {r.action};
  if (r.action == "install") {
    out.insert(out.end(), {"--sha256", r.sha256, "--package", r.package, "--version", r.version, "--arch", r.arch});
    if (r.reinstall) out.push_back("--reinstall");
    if (r.downgrade) out.push_back("--downgrade");
  } else if (r.action == "remove") {
    out.insert(out.end(), {"--package", r.package});
  }
  for (const auto &n : r.allow_remove) out.insert(out.end(), {"--allow-remove", n});
  if (r.action == "install") out.push_back(r.file);
  return out;
}

bool protected_package(const std::string &name) {
  static const char *const kProtected[] = {"sde", "sde-desktop", "merkadu"};
  for (const char *p : kProtected)
    if (name == p) return true;
  return false;
}

bool parse_status_line(const std::string &line, const std::string &action, Progress *out) {
  *out = Progress();
  // TYPE:FIELD:PERCENT:TEXT (the text may contain ':').
  size_t a = line.find(':');
  size_t b = a == std::string::npos ? a : line.find(':', a + 1);
  size_t c = b == std::string::npos ? b : line.find(':', b + 1);
  if (c == std::string::npos) return false;
  std::string type = line.substr(0, a), field = line.substr(a + 1, b - a - 1);
  std::string number = line.substr(b + 1, c - b - 1), text = line.substr(c + 1);
  char *end = nullptr;
  double pct = strtod(number.c_str(), &end);
  if (end == number.c_str() || !std::isfinite(pct)) return false;
  pct = std::max(0.0, std::min(100.0, pct));
  if (type == "dlstatus") {
    out->phase = "download";
    out->percent = int(action == "refresh" ? pct : pct * 0.4);
    return true;
  }
  if (type == "pmstatus" || type == "pmerror") {
    double base = action == "install" ? 40 : 0, share = action == "install" ? 0.6 : 1.0;
    out->percent = int(base + pct * share);
    if (type == "pmerror") {
      out->package = field;
      out->error = trim(text);
      out->phase = "error";
      return true;
    }
    std::string t = lower(text);
    if (field == "dpkg-exec") {
      out->phase = action == "remove" ? "remove" : "unpack";
    } else {
      out->package = field;
      if (starts_with(t, "preparing to configure") || starts_with(t, "configuring") || starts_with(t, "installed"))
        out->phase = "configure";
      else if (starts_with(t, "removing") || starts_with(t, "removed") || starts_with(t, "preparing for removal") ||
               starts_with(t, "completely removing") || starts_with(t, "completely removed"))
        out->phase = "remove";
      else if (t.find("trigger") != std::string::npos)
        out->phase = "trigger";
      else
        out->phase = "unpack";
    }
    // dpkg's package names may carry the architecture ("sl:amd64").
    if (!valid_package_name(out->package.substr(0, out->package.find(':')))) out->package.clear();
    return true;
  }
  return false;
}

Event parse_event(const std::string &raw) {
  Event e;
  std::string line = raw;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  auto word = [&](size_t start, size_t *next) {
    size_t sp = line.find(' ', start);
    *next = sp == std::string::npos ? line.size() : sp + 1;
    return line.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
  };
  size_t pos = 0;
  std::string head = word(0, &pos);
  if (line == kProtocol) {
    e.type = Event::Type::hello;
  } else if (head == "PHASE") {
    e.type = Event::Type::phase;
    e.word = word(pos, &pos);
  } else if (head == "PROGRESS") {
    e.type = Event::Type::progress;
    e.percent = atoi(word(pos, &pos).c_str());
    e.word = word(pos, &pos);
    e.package = pos < line.size() ? line.substr(pos) : "";
    if (e.percent < -1 || e.percent > 100) e.percent = -1;
  } else if (head == "LOG") {
    e.type = Event::Type::log;
    e.text = pos < line.size() ? line.substr(pos) : "";
  } else if (head == "ERROR") {
    e.type = Event::Type::error;
    e.word = word(pos, &pos);
    e.text = pos < line.size() ? line.substr(pos) : "";
  } else if (head == "RESULT") {
    e.type = Event::Type::result;
    e.word = word(pos, &pos);
  }
  return e;
}

}  // namespace helper
}  // namespace debrun
}  // namespace sde
