// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "reports.h"

#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#include "build_config.h"
#include "gpu.h"
#include "i18n.h"
#include "integration.h"
#include "util.h"

namespace sde {
namespace reports {

namespace {

const char *kIssues = "https://github.com/hugomonizdorego/SDE-Desktop/issues/new";

bool is_stamp(const std::string &s) {
  return s.size() >= 8 && std::isdigit(static_cast<unsigned char>(s[0])) &&
         std::isdigit(static_cast<unsigned char>(s[1])) && s[2] == ':' &&
         std::isdigit(static_cast<unsigned char>(s[3])) && s[5] == ':';
}

// The part of a frame that stays the same between runs: "sde-panel(+0x1234)"
// without the absolute address in brackets.
std::string frame_key(const std::string &frame) {
  size_t b = frame.rfind('[');
  return trim(b == std::string::npos ? frame : frame.substr(0, b));
}

}  // namespace

std::string CrashReport::signature() const {
  std::string sig = program + "|" + signal;
  std::istringstream in(backtrace);
  std::string line;
  int n = 0;
  // Skip the crash handler's own frames (libc, crash.cpp) at the top.
  while (std::getline(in, line) && n < 6) {
    std::string k = frame_key(line);
    if (k.find("libc.so") != std::string::npos || k.find("on_crash") != std::string::npos) continue;
    sig += "|" + k;
    ++n;
  }
  return sig;
}

std::vector<CrashReport> parse_crash_log(const std::string &log, const std::string &source) {
  std::vector<CrashReport> out;
  std::istringstream in(log);
  std::string line, last_stamp;
  CrashReport *open = nullptr;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (is_stamp(line)) last_stamp = line.substr(0, 8);
    if (open) {
      if (starts_with(line, "-- end of backtrace")) {
        open = nullptr;
      } else {
        open->backtrace += line + "\n";
      }
      continue;
    }
    size_t p = line.find(" crashed: ");
    if (p != std::string::npos && line.find("Backtrace") != std::string::npos) {
      CrashReport r;
      r.program = trim(line.substr(0, p));
      if (is_stamp(r.program)) r.program = trim(r.program.substr(8));
      size_t s = p + 10, e = line.find(". Backtrace", s);
      r.signal = trim(line.substr(s, e == std::string::npos ? std::string::npos : e - s));
      r.time = last_stamp;
      r.source = source;
      r.has_backtrace = true;
      out.push_back(r);
      open = &out.back();
      continue;
    }
    // "HH:MM:SS sde-session: panel exited (killed by signal 11 (Segmentation fault))"
    p = line.find(" exited (killed by signal ");
    if (p != std::string::npos && line.find("sde-session: ") != std::string::npos) {
      CrashReport r;
      size_t n = line.find("sde-session: ") + 13;
      r.program = trim(line.substr(n, p - n));
      size_t a = line.find('(', p + 26);
      size_t b = line.rfind(')');
      if (a != std::string::npos && b != std::string::npos && b > a) {
        r.signal = line.substr(a + 1, b - a - 1);
        if (!r.signal.empty() && r.signal.back() == ')') r.signal.pop_back();
      } else {
        r.signal = trim(line.substr(p + 26));
      }
      r.time = is_stamp(line) ? line.substr(0, 8) : last_stamp;
      r.source = source;
      // The component's own backtrace usually came right before: then this
      // line only confirms it.
      bool dup = false;
      if (!out.empty() && out.back().has_backtrace) {
        const std::string &prev = out.back().program;
        dup = prev == r.program || prev == "sde-" + r.program || ends_with(prev, r.program);
      }
      if (!dup) out.push_back(r);
    }
  }
  return out;
}

std::vector<Grouped> group(const std::vector<CrashReport> &reports) {
  std::vector<Grouped> out;
  for (const auto &r : reports) {
    std::string sig = r.signature();
    auto it = std::find_if(out.begin(), out.end(),
                           [&](const Grouped &g) { return g.report.signature() == sig; });
    if (it == out.end()) {
      out.push_back({r, 1});
    } else {
      ++it->count;
      it->report = r;  // keep the latest occurrence
    }
  }
  return out;
}

std::vector<std::string> report_files() {
  std::string dir = env("SDE_REPORTS_DIR");
  if (dir.empty()) dir = state_home() + "/sde";
  std::vector<std::string> out;
  // Older logs first, so that the newest report ends up last.
  for (const char *first : {"session.log.old", "session.log"})
    if (file_exists(join_path(dir, first))) out.push_back(join_path(dir, first));
  auto names = list_dir(dir);
  std::sort(names.begin(), names.end());
  for (const auto &n : names) {
    if (n == "session.log" || n == "session.log.old") continue;
    if (ends_with(n, ".log") || ends_with(n, ".log.old") || ends_with(n, ".crash"))
      out.push_back(join_path(dir, n));
  }
  return out;
}

std::vector<CrashReport> collect() {
  std::vector<CrashReport> out;
  for (const auto &f : report_files()) {
    std::string text;
    if (!read_file(f, text)) continue;
    for (auto &r : parse_crash_log(text, f)) out.push_back(r);
  }
  return out;
}

std::string url_encode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string issue_url(const std::string &title, const std::string &body, size_t max_len) {
  std::string base = std::string(kIssues) + "?title=" + url_encode(title) + "&body=";
  std::string b = body;
  std::string enc = url_encode(b);
  if (base.size() + enc.size() > max_len) {
    const std::string cut = "\n\n(shortened: please paste the full report from the clipboard)\n";
    // Shorten by whole lines until it fits.
    while (!b.empty() && base.size() + url_encode(b + cut).size() > max_len) {
      size_t nl = b.rfind('\n', b.size() > 1 ? b.size() - 2 : 0);
      b = nl == std::string::npos ? std::string() : b.substr(0, nl + 1);
    }
    enc = url_encode(b + cut);
  }
  return base + enc;
}

std::string issue_title(const CrashReport &r, const std::string &version) {
  return "[crash] " + r.program + ": " + (r.signal.empty() ? "stopped" : r.signal) + " (" + version + ")";
}

std::string report_text(const CrashReport &r, const std::string &system_info) {
  std::string t;
  t += "### What happened\n\n" + r.program + " stopped: " + r.signal;
  if (!r.time.empty()) t += " (at " + r.time + ")";
  t += ".\n\n<!-- What were you doing when it happened? -->\n\n";
  if (r.has_backtrace && !r.backtrace.empty()) t += "### Backtrace\n\n```\n" + r.backtrace + "```\n\n";
  t += "### System\n\n```\n" + system_info + "```\n";
  return t;
}

std::string cpu_model(const std::string &cpuinfo) {
  std::istringstream in(cpuinfo);
  std::string line;
  while (std::getline(in, line)) {
    size_t c = line.find(':');
    if (c == std::string::npos) continue;
    std::string k = trim(line.substr(0, c));
    if (k == "model name" || k == "Model" || k == "Hardware") {
      std::string v = trim(line.substr(c + 1));
      // Collapse runs of spaces ("Intel(R) Core(TM)  i5").
      std::string out;
      for (char ch : v)
        if (!(ch == ' ' && !out.empty() && out.back() == ' ')) out += ch;
      return out;
    }
  }
  return "";
}

std::string memory_total(const std::string &meminfo) {
  std::istringstream in(meminfo);
  std::string key;
  long long kb = 0;
  std::string unit;
  while (in >> key) {
    if (key == "MemTotal:") {
      in >> kb;
      char buf[32];
      snprintf(buf, sizeof buf, "%.1f GiB", double(kb) / (1024.0 * 1024.0));
      return buf;
    }
    std::getline(in, unit);
  }
  return "";
}

std::string system_summary() {
  std::string s;
  auto add = [&](const char *k, const std::string &v) {
    if (!v.empty()) s += std::string(k) + ": " + v + "\n";
  };
  add("SDE", std::string(SDE_VERSION) + " (" + SDE_VERSION_LABEL + ")");
  std::string os = os_release("PRETTY_NAME");
  add("OS", os.empty() ? "unknown" : os);
  struct utsname u;
  if (uname(&u) == 0) add("Kernel", std::string(u.release) + " " + u.machine);
  std::string text;
  if (read_file("/proc/cpuinfo", text)) add("CPU", cpu_model(text));
  text.clear();
  if (read_file("/proc/meminfo", text)) add("Memory", memory_total(text));
  int i = 0;
  for (const auto &g : detect_gpus()) {
    std::string label = gpu_label(g);
    if (!g.driver.empty()) label += " [driver: " + g.driver + "]";
    add(i++ ? "GPU" : "Graphics", label);
  }
  add("Rendering", detect_graphics_class());
  add("Virtual machine", virtual_machine_name());
  add("Session", env("XDG_SESSION_TYPE", "x11") + ", " + env("XDG_CURRENT_DESKTOP", "SDE"));
  add("Language", env("LANG"));
  return s;
}

}  // namespace reports
}  // namespace sde
