// SPDX-License-Identifier: GPL-3.0-or-later
#include "model.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <set>

#include "i18n.h"
#include "util.h"

namespace sde {
namespace update {

namespace {

std::vector<std::string> fields_of(const std::string &line) { return split(line, '\t', true); }

long long to_ll(const std::string &s) {
  char *end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  return end && *end == '\0' && !s.empty() ? v : 0;
}

std::vector<std::string> split_list(const std::string &s) {
  std::vector<std::string> out;
  for (auto &part : split(s, ',')) {
    std::string t = trim(part);
    if (!t.empty()) out.push_back(t);
  }
  return out;
}

}  // namespace

Kind kind_from_id(const std::string &id) {
  if (id == "security") return Kind::Security;
  if (id == "kernel") return Kind::Kernel;
  if (id == "backport") return Kind::Backport;
  return Kind::Software;
}

const char *kind_title(Kind kind) {
  switch (kind) {
    case Kind::Security: return _("Security Updates");
    case Kind::Kernel: return _("Kernel Updates");
    case Kind::Backport: return _("Backports");
    default: return _("Software Updates");
  }
}

const char *kind_badge(Kind kind) {
  switch (kind) {
    case Kind::Security: return _("Security");
    case Kind::Kernel: return _("Kernel");
    case Kind::Backport: return _("Backport");
    default: return _("Recommended");
  }
}

const char *group_title(Group group) {
  switch (group) {
    case Group::Critical: return _("Critical updates");
    case Group::Security: return _("Security updates");
    case Group::Flatpak: return _("Flatpak updates");
    default: return _("Main updates");
  }
}

const char *group_badge(Group group) {
  switch (group) {
    case Group::Critical: return _("Critical");
    case Group::Security: return _("Security");
    case Group::Flatpak: return _("Flatpak");
    default: return _("Main");
  }
}

const char *group_id(Group group) {
  switch (group) {
    case Group::Critical: return "critical";
    case Group::Security: return "security";
    case Group::Flatpak: return "flatpak";
    default: return "main";
  }
}

std::string Update::title() const {
  if (!app_name.empty()) return app_name;
  if (!summary.empty()) {
    std::string t = summary;
    if (t[0] >= 'a' && t[0] <= 'z') t[0] = char(t[0] - 'a' + 'A');
    return t;
  }
  return name;
}

namespace {

std::string base_name(const std::string &package) { return package.substr(0, package.find(':')); }

bool any_name(const Update &u, const std::function<bool(const std::string &)> &test) {
  if (test(u.source) || test(u.name) || test(base_name(u.main_package))) return true;
  for (auto &p : u.packages)
    if (test(base_name(p))) return true;
  return false;
}

bool is_kernel_name(const std::string &n) {
  return n == "linux" || starts_with(n, "linux-image-") || starts_with(n, "linux-signed") ||
         n == "linux-latest" || starts_with(n, "linux-modules-");
}

bool is_firmware_name(const std::string &n) {
  return starts_with(n, "firmware-") || n == "linux-firmware" || ends_with(n, "-microcode");
}

}  // namespace

bool needs_reboot(const Update &u) {
  if (u.flatpak) return false;
  if (u.kind == Kind::Kernel) return true;
  static const std::set<std::string> core = {"glibc",       "libc6", "systemd",     "libsystemd0",
                                              "libsystemd-shared", "udev", "dbus", "dbus-daemon",
                                              "dbus-broker"};
  return any_name(u, [](const std::string &n) {
    return is_kernel_name(n) || is_firmware_name(n) || core.count(n) != 0;
  });
}

Group classify(const Update &u) {
  if (u.flatpak) return Group::Flatpak;
  std::string urgency = lower(u.urgency), priority = lower(u.priority);
  if (urgency == "critical" || urgency == "emergency" || urgency == "high") return Group::Critical;
  if (priority == "required" || priority == "important") return Group::Critical;
  if (u.kind == Kind::Kernel || u.reboot || needs_reboot(u)) return Group::Critical;
  static const std::set<std::string> core = {"apt", "dpkg", "grub2", "shim", "shim-signed", "base-files"};
  if (any_name(u, [](const std::string &n) {
        return core.count(n) != 0 || starts_with(n, "libapt-pkg") || starts_with(n, "grub-");
      }))
    return Group::Critical;
  if (u.kind == Kind::Security || ends_with(u.archive, "-security") ||
      u.site.find("security.") != std::string::npos)
    return Group::Security;
  return Group::Main;
}

void sort_updates(std::vector<Update> &updates) {
  std::stable_sort(updates.begin(), updates.end(), [](const Update &a, const Update &b) {
    if (a.group != b.group) return int(a.group) < int(b.group);
    return lower(a.title()) < lower(b.title());
  });
}

int UpdateList::count(Group group) const {
  return int(std::count_if(updates.begin(), updates.end(),
                           [group](const Update &u) { return u.group == group; }));
}

long long UpdateList::size(Group group) const {
  long long total = 0;
  for (auto &u : updates)
    if (u.group == group) total += u.size;
  return total;
}

long long UpdateList::selected_size() const {
  long long total = 0;
  for (auto &u : updates)
    if (u.selected) total += u.size;
  return total;
}

int UpdateList::selected_count() const {
  return int(std::count_if(updates.begin(), updates.end(), [](const Update &u) { return u.selected; }));
}

bool UpdateList::needs_reboot() const {
  for (auto &u : updates)
    if (u.selected && u.reboot) return true;
  return false;
}

long long UpdateList::total_size() const {
  long long total = 0;
  for (auto &u : updates) total += u.size;
  return total;
}

int UpdateList::count(Kind kind) const {
  return int(std::count_if(updates.begin(), updates.end(),
                           [kind](const Update &u) { return u.kind == kind; }));
}

UpdateList parse_updates(const std::string &output) {
  UpdateList list;
  auto lines = split(output, '\n');
  if (lines.empty()) return list;
  if (lines[0] == "SDE-UPDATES\t2") list.protocol = 2;
  else if (lines[0] == "SDE-UPDATES\t1") list.protocol = 1;
  else return list;
  for (size_t i = 1; i < lines.size(); ++i) {
    if (lines[i].empty()) continue;
    auto f = fields_of(lines[i]);
    Update u;
    if (list.protocol == 1) {
      if (f.size() != 6) return UpdateList{};
      u.kind = kind_from_id(f[0]);
      u.name = f[1];
      u.old_version = f[2];
      u.new_version = f[3];
      u.size = to_ll(f[4]);
      u.packages = split_list(f[5]);
      u.main_package = u.packages.empty() ? u.name : u.packages.front();
      u.source = u.name;
      list.updates.push_back(std::move(u));
      continue;
    }
    if (f[0] == "H" && f.size() >= 2) {
      list.held.push_back(f[1]);
      continue;
    }
    if (f[0] != "U") continue;  // future record types
    if (f.size() < 14) return UpdateList{};
    u.kind = kind_from_id(f[1]);
    u.name = f[2];
    u.main_package = f[3];
    u.source = f[4];
    u.old_version = f[5];
    u.new_version = f[6];
    u.size = to_ll(f[7]);
    u.packages = split_list(f[8]);
    u.origin = f[9];
    u.archive = f[10];
    u.site = f[11];
    u.summary = f[12];
    u.description = f[13];
    // Optional fields of Alpha 51 and later.
    if (f.size() > 14) u.priority = f[14];
    if (f.size() > 15) u.section = f[15];
    if (f.size() > 16) u.urgency = f[16];
    if (f.size() > 17) u.app_name = f[17];
    if (f.size() > 18) u.icon = f[18];
    if (u.main_package.empty() && !u.packages.empty()) u.main_package = u.packages.front();
    list.updates.push_back(std::move(u));
  }
  for (auto &u : list.updates) {
    u.reboot = needs_reboot(u);
    u.group = classify(u);
  }
  // Groups in a fixed order; titles alphabetical inside a group.
  sort_updates(list.updates);
  list.valid = true;
  return list;
}

bool parse_flatpaks(const std::string &output, UpdateList &list) {
  auto lines = split(output, '\n');
  if (lines.empty() || lines[0] != "SDE-FLATPAK\t1") return false;
  for (size_t i = 1; i < lines.size(); ++i) {
    auto f = fields_of(lines[i]);
    if (f.empty()) continue;
    if (f[0] == "I" && f.size() >= 2) {
      list.flatpak_installed = f[1] == "1";
    } else if (f[0] == "E" && f.size() >= 2) {
      list.flatpak_errors.push_back(f[1]);
    } else if (f[0] == "F" && f.size() >= 11) {
      Update u;
      u.flatpak = true;
      u.installation = f[1];
      u.ref = f[2];
      if ((u.installation != "system" && u.installation != "user") || !valid_flatpak_ref(u.ref)) continue;
      u.app_name = f[3];
      u.application = f[4];
      u.branch = f[5];
      u.origin = f[6];
      u.old_version = f[7];
      u.new_version = f[8];
      u.size = to_ll(f[9]);
      u.summary = f[10];
      u.name = u.application.empty() ? u.ref : u.application;
      u.main_package = u.name;
      u.source = u.name;
      u.icon = u.application;
      u.archive = u.branch;
      u.group = Group::Flatpak;
      list.updates.push_back(std::move(u));
    }
  }
  sort_updates(list.updates);
  return true;
}

bool matches(const Update &u, const std::string &query) {
  std::string q = lower(trim(query));
  if (q.empty()) return true;
  if (lower(u.name).find(q) != std::string::npos) return true;
  if (lower(u.summary).find(q) != std::string::npos) return true;
  for (auto &p : u.packages)
    if (lower(p).find(q) != std::string::npos) return true;
  return false;
}

std::vector<std::string> selected_packages(const std::vector<Update> &updates, bool *all) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  bool every = std::any_of(updates.begin(), updates.end(), [](const Update &u) { return !u.flatpak; });
  for (auto &u : updates) {
    if (u.flatpak) continue;
    if (!u.selected) {
      every = false;
      continue;
    }
    for (auto &p : u.packages)
      if (valid_package_name(p) && seen.insert(p).second) out.push_back(p);
  }
  if (all) *all = every;
  return out;
}

std::vector<std::string> selected_flatpaks(const std::vector<Update> &updates) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (auto &u : updates)
    if (u.flatpak && u.selected && valid_flatpak_item(u.flatpak_item()) &&
        seen.insert(u.flatpak_item()).second)
      out.push_back(u.flatpak_item());
  return out;
}

std::vector<std::vector<std::string>> install_commands(const std::vector<Update> &updates) {
  std::vector<std::vector<std::string>> out;
  bool all = false;
  auto packages = selected_packages(updates, &all);
  if (!packages.empty()) {
    std::vector<std::string> cmd = {"upgrade"};
    // Everything selected: a full upgrade lets APT/PackageKit add new dependencies.
    if (!all) cmd.insert(cmd.end(), packages.begin(), packages.end());
    out.push_back(std::move(cmd));
  }
  auto refs = selected_flatpaks(updates);
  if (!refs.empty()) {
    std::vector<std::string> cmd = {"flatpak"};
    cmd.insert(cmd.end(), refs.begin(), refs.end());
    out.push_back(std::move(cmd));
  }
  return out;
}

int HistoryEntry::count(const std::string &action) const {
  return int(std::count_if(changes.begin(), changes.end(),
                           [&](const Change &c) { return c.action == action; }));
}

std::string HistoryEntry::summary() const {
  struct Part {
    const char *action;
    const char *text;
  };
  const Part parts[] = {
      {"Upgrade", N_("Upgraded %d")},   {"Install", N_("Installed %d")},
      {"Remove", N_("Removed %d")},     {"Purge", N_("Purged %d")},
      {"Downgrade", N_("Downgraded %d")}, {"Reinstall", N_("Reinstalled %d")},
  };
  std::string out;
  for (auto &part : parts) {
    int n = count(part.action);
    if (!n) continue;
    if (!out.empty()) out += " · ";
    out += format(gettext(part.text), n);
  }
  if (out.empty()) out = _("No package changes");
  return out;
}

bool parse_history(const std::string &output, std::vector<HistoryEntry> &out) {
  out.clear();
  auto lines = split(output, '\n');
  if (lines.empty() || lines[0] != "SDE-HISTORY\t1") return false;
  for (size_t i = 1; i < lines.size(); ++i) {
    auto f = fields_of(lines[i]);
    if (f.empty()) continue;
    if (f[0] == "T" && f.size() >= 6) {
      HistoryEntry e;
      e.start = f[1];
      e.end = f[2];
      e.commandline = f[3];
      e.requested_by = f[4];
      e.error = f[5];
      out.push_back(std::move(e));
    } else if (f[0] == "C" && f.size() >= 7 && !out.empty()) {
      Change c;
      c.action = f[1];
      c.package = f[2];
      c.arch = f[3];
      c.old_version = f[4];
      c.new_version = f[5];
      c.automatic = f[6] == "1";
      out.back().changes.push_back(std::move(c));
    }
  }
  return true;
}

bool matches(const HistoryEntry &e, const std::string &query) {
  std::string q = lower(trim(query));
  if (q.empty()) return true;
  if (lower(e.commandline).find(q) != std::string::npos) return true;
  if (e.start.find(q) != std::string::npos) return true;
  for (auto &c : e.changes)
    if (lower(c.package).find(q) != std::string::npos) return true;
  return false;
}

bool parse_sources(const std::string &output, std::vector<Source> &out) {
  out.clear();
  auto lines = split(output, '\n');
  if (lines.empty() || lines[0] != "SDE-SOURCES\t1") return false;
  for (size_t i = 1; i < lines.size(); ++i) {
    auto f = fields_of(lines[i]);
    if (f.size() < 10 || f[0] != "S") continue;
    Source s;
    s.file = f[1];
    s.line = int(to_ll(f[2]));
    s.format = f[3];
    s.enabled = f[4] == "1";
    s.types = f[5];
    s.uris = f[6];
    s.suites = f[7];
    s.components = f[8];
    s.signed_by = f[9];
    out.push_back(std::move(s));
  }
  return true;
}

void apply_transaction_line(Progress &p, const std::string &line) {
  auto f = fields_of(line);
  if (f.empty() || f[0].empty()) return;
  const std::string &key = f[0];
  std::string value = f.size() > 1 ? f[1] : "";
  if (key == "SDE-TRANSACTION") {
    p.started = true;
  } else if (key == "BACKEND") {
    // A second BACKEND line means the helper fell back to APT: errors of the
    // first attempt (PackageKit not running) no longer matter.
    if (!p.backend.empty() && p.backend != value) p.errors.clear();
    p.backend = value;
  } else if (key == "CANCELLABLE") {
    p.cancellable = value == "1";
  } else if (key == "STATUS") {
    p.status = value;
  } else if (key == "PERCENT") {
    int v = int(to_ll(value));
    if (value == "-1") v = -1;
    p.percent = std::max(-1, std::min(100, v));
  } else if (key == "ITEM" && f.size() >= 3) {
    p.item_info = f[1];
    p.item_name = f[2];
    p.item_version = f.size() > 3 ? f[3] : "";
    if (!p.item_name.empty() &&
        std::find(p.seen.begin(), p.seen.end(), p.item_name) == p.seen.end()) {
      p.seen.push_back(p.item_name);
      ++p.items_seen;
    }
    p.log.push_back(p.item_info + " " + p.item_name + (p.item_version.empty() ? "" : " " + p.item_version));
  } else if (key == "LOG") {
    if (!value.empty()) p.log.push_back(value);
  } else if (key == "ERROR") {
    p.errors.push_back(value);
    p.log.push_back("E: " + value);
  } else if (key == "EXIT" && f.size() >= 3) {
    p.exit_code = int(to_ll(f[1]));
    p.outcome = f[2];
    p.finished = true;
  }
  // Keep the log bounded for very long transactions.
  if (p.log.size() > 4000) p.log.erase(p.log.begin(), p.log.begin() + 1000);
}

std::string status_text(const std::string &status) {
  static const struct {
    const char *in, *out;
  } table[] = {
      {"Starting", N_("Starting")},
      {"Waiting in queue", N_("Waiting in queue")},
      {"Waiting for authentication", N_("Waiting for authentication")},
      {"Waiting for other tasks", N_("Waiting for other tasks")},
      {"Waiting for package manager lock", N_("Waiting for the package manager")},
      {"Running", N_("Running")},
      {"Querying", N_("Querying")},
      {"Resolving dependencies", N_("Resolving dependencies")},
      {"Downloading packages", N_("Downloading packages")},
      {"Downloading repository information", N_("Downloading repository information")},
      {"Loading cache", N_("Loading cache")},
      {"Refreshing software list", N_("Refreshing software list")},
      {"Installing packages", N_("Installing packages")},
      {"Installing updates", N_("Installing updates")},
      {"Updating packages", N_("Updating packages")},
      {"Removing packages", N_("Removing packages")},
      {"Cleaning up packages", N_("Cleaning up packages")},
      {"Testing changes", N_("Testing changes")},
      {"Committing changes", N_("Committing changes")},
      {"Finished", N_("Finished")},
      {"Cancelling", N_("Cancelling")},
  };
  for (auto &row : table)
    if (status == row.in) return gettext(row.out);
  return status;
}

std::vector<std::string> parse_autoremove(const std::string &simulation) {
  std::vector<std::string> out;
  for (auto &line : split(simulation, '\n')) {
    if (!starts_with(line, "Remv ")) continue;
    std::string rest = line.substr(5);
    size_t end = rest.find(' ');
    std::string name = rest.substr(0, end);
    if (valid_package_name(name)) out.push_back(name);
  }
  return out;
}

const std::vector<int> &interval_hours() {
  static const std::vector<int> hours = {0, 1, 6, 12, 24, 168};
  return hours;
}

std::string interval_label(int hours) {
  switch (hours) {
    case 0: return _("Never");
    case 1: return _("Every hour");
    case 6: return _("Every 6 hours");
    case 12: return _("Every 12 hours");
    case 24: return _("Daily");
    case 168: return _("Weekly");
    default: return format(_("Every %d hours"), hours);
  }
}

std::string format_size(long long bytes) {
  if (bytes < 0) bytes = 0;
  if (bytes < 1000) return format(_("%lld bytes"), bytes);
  const char *units[] = {"kB", "MB", "GB", "TB"};
  double v = double(bytes);
  int u = -1;
  while (v >= 1000 && u < 3) {
    v /= 1000;
    ++u;
  }
  return format(v >= 100 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

std::string format_age(std::time_t then, std::time_t now) {
  if (then <= 0) return _("never");
  long d = long(now - then);
  if (d < 60) return _("just now");
  if (d < 3600) {
    long m = d / 60;
    return m == 1 ? _("1 minute ago") : format(_("%ld minutes ago"), m);
  }
  if (d < 86400) {
    long h = d / 3600;
    return h == 1 ? _("1 hour ago") : format(_("%ld hours ago"), h);
  }
  long days = d / 86400;
  return days == 1 ? _("yesterday") : format(_("%ld days ago"), days);
}

std::string format_date(const std::string &stamp) {
  int y, mo, d, h, mi;
  if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 ||
      mo > 12)
    return stamp;
  static const char *months[] = {N_("Jan"), N_("Feb"), N_("Mar"), N_("Apr"),
                                 N_("May"), N_("Jun"), N_("Jul"), N_("Aug"),
                                 N_("Sep"), N_("Oct"), N_("Nov"), N_("Dec")};
  return format("%s %d, %d · %02d:%02d", gettext(months[mo - 1]), d, y, h, mi);
}

bool valid_package_name(const std::string &name) {
  if (name.empty() || name.size() > 200) return false;
  auto lower_alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
  if (!lower_alnum(name[0])) return false;
  size_t colon = name.find(':');
  std::string base = name.substr(0, colon);
  for (char c : base)
    if (!lower_alnum(c) && c != '+' && c != '-' && c != '.') return false;
  if (colon != std::string::npos) {
    std::string arch = name.substr(colon + 1);
    if (arch.empty()) return false;
    for (char c : arch)
      if (!lower_alnum(c)) return false;
  }
  return true;
}

bool valid_flatpak_ref(const std::string &ref) {
  auto parts = split(ref, '/');
  if (parts.size() != 4 || ref.size() > 255) return false;
  if (parts[0] != "app" && parts[0] != "runtime") return false;
  auto ok = [](const std::string &s, bool dots) {
    if (s.empty()) return false;
    if (!(std::isalnum((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
      if (!(std::isalnum((unsigned char)c) || c == '_' || (dots && (c == '.' || c == '-')))) return false;
    return true;
  };
  return ok(parts[1], true) && ok(parts[2], false) && ok(parts[3], true);
}

bool valid_flatpak_item(const std::string &item) {
  size_t colon = item.find(':');
  if (colon == std::string::npos) return false;
  std::string inst = item.substr(0, colon);
  return (inst == "system" || inst == "user") && valid_flatpak_ref(item.substr(colon + 1));
}

const std::vector<int> &remind_hours() {
  static const std::vector<int> hours = {1, 4, 24, 72, 168};
  return hours;
}

std::string remind_label(int hours) {
  switch (hours) {
    case 1: return _("In 1 hour");
    case 4: return _("In 4 hours");
    case 24: return _("Tomorrow");
    case 72: return _("In 3 days");
    case 168: return _("In a week");
    default: return format(_("In %d hours"), hours);
  }
}

}  // namespace update
}  // namespace sde
