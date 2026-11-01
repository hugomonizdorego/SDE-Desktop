// SPDX-License-Identifier: GPL-3.0-or-later
#include "launches.h"

#include <signal.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include "desktop_entry.h"
#include "util.h"

namespace sde {
namespace launches {

namespace {

std::string escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

std::string unescape(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 == s.size()) {
      out += s[i];
      continue;
    }
    char n = s[++i];
    out += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
  }
  return out;
}

bool all_digits(const std::string &s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

std::string path_of(long pid) { return dir() + "/" + std::to_string(pid); }

}  // namespace

std::string dir() {
  std::string own = env("SDE_LAUNCHES_DIR");
  if (!own.empty()) return own;
  return runtime_dir() + "/sde/launches";
}

std::string serialize(const Record &r) {
  std::ostringstream out;
  out << "pid=" << r.pid << "\nstart=" << r.start << "\ntime=" << r.time << "\nname=" << escape(r.name)
      << "\nicon=" << escape(r.icon) << "\ndesktop=" << escape(r.desktop_id) << "\n";
  for (const auto &a : r.argv) out << "arg=" << escape(a) << "\n";
  return out.str();
}

bool parse(const std::string &text, Record &out) {
  Record r;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "pid") r.pid = atol(value.c_str());
    else if (key == "start") r.start = strtoull(value.c_str(), nullptr, 10);
    else if (key == "time") r.time = atoll(value.c_str());
    else if (key == "name") r.name = unescape(value);
    else if (key == "icon") r.icon = unescape(value);
    else if (key == "desktop") r.desktop_id = unescape(value);
    else if (key == "arg") r.argv.push_back(unescape(value));
  }
  if (r.pid <= 1) return false;
  out = r;
  return true;
}

bool save(const Record &r) {
  if (r.pid <= 1) return false;
  if (env("SDE_LAUNCHES_DIR").empty() && !private_dir(runtime_dir())) return false;
  make_dirs(dir());
  return write_file_atomic(path_of(r.pid), serialize(r));
}

bool load(long pid, Record &out) {
  std::string text;
  if (pid <= 1 || !read_file(path_of(pid), text)) return false;
  return parse(text, out) && out.pid == pid;
}

void remove(long pid) {
  if (pid > 1) unlink(path_of(pid).c_str());
}

std::vector<Record> all() {
  std::vector<Record> out;
  for (const auto &name : list_dir(dir())) {
    if (!all_digits(name)) continue;  // half-written files end in .tmp.<pid>
    Record r;
    if (load(atol(name.c_str()), r)) out.push_back(r);
  }
  return out;
}

unsigned long long start_time(long pid, const std::string &proc) {
  std::string stat;
  if (pid <= 0 || !read_file(proc + "/" + std::to_string(pid) + "/stat", stat)) return 0;
  // "pid (comm) state ppid ...": the name may hold spaces and brackets.
  size_t close = stat.rfind(')');
  if (close == std::string::npos) return 0;
  std::istringstream in(stat.substr(close + 1));
  std::string field;
  for (int i = 3; i <= 22 && in >> field; ++i)
    if (i == 22) return strtoull(field.c_str(), nullptr, 10);
  return 0;
}

bool alive(const Record &r, const std::string &proc) {
  unsigned long long now = start_time(r.pid, proc);
  return now != 0 && (r.start == 0 || now == r.start);
}

bool enabled() { return !env("SDE_SESSION_VERSION").empty() || !env("SDE_LAUNCHES_DIR").empty(); }

void record(long pid, const DesktopEntry &entry, const std::vector<std::string> &argv) {
  if (pid <= 1 || !enabled()) return;
  Record r;
  r.pid = pid;
  r.start = start_time(pid);
  r.time = static_cast<long long>(::time(nullptr));
  r.name = entry.name;
  r.icon = entry.icon;
  r.desktop_id = entry.id;
  r.argv = argv;
  save(r);
}

void record_command(long pid, const std::vector<std::string> &argv) {
  if (pid <= 1 || argv.empty() || !enabled()) return;
  std::string program = basename_of(argv[0]);
  for (const auto &e : load_applications("SDE")) {
    auto cmd = e.command();
    if (cmd.empty() || basename_of(cmd[0]) != program) continue;
    record(pid, e, argv);
    return;
  }
}

}  // namespace launches
}  // namespace sde
