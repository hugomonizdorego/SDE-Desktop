// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "power.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "dbus.h"
#include "i18n.h"
#include "process.h"
#include "util.h"

namespace sde {
namespace power {

namespace {

int read_int(const std::string &path, int fallback) {
  std::string text;
  if (!read_file(path, text)) return fallback;
  text = trim(text);
  if (text.empty()) return fallback;
  char *end = nullptr;
  long v = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || v < 0 || v > 1000000000L) return fallback;
  return int(v);
}

int type_rank(const std::string &type) {
  if (type == "firmware") return 0;
  if (type == "platform") return 1;
  if (type == "raw") return 2;
  return 3;
}

// A backlight name is used in a sysfs path and a D-Bus call: only plain
// names are accepted.
bool safe_name(const std::string &name) {
  if (name.empty() || name[0] == '.') return false;
  for (char c : name)
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':'))
      return false;
  return true;
}

const char *kLogin = "org.freedesktop.login1";

}  // namespace

int Backlight::percent() const { return raw_to_percent(value, max); }

std::vector<Backlight> backlights(const std::string &root) {
  std::vector<Backlight> out;
  for (const std::string &name : list_dir(root)) {
    if (!safe_name(name)) continue;
    Backlight b;
    b.name = name;
    std::string base = root + "/" + name;
    std::string type;
    if (read_file(base + "/type", type)) b.type = trim(type);
    b.max = read_int(base + "/max_brightness", 0);
    b.value = read_int(base + "/brightness", 0);
    if (b.valid()) out.push_back(b);
  }
  std::stable_sort(out.begin(), out.end(), [](const Backlight &a, const Backlight &b) {
    int ra = type_rank(a.type), rb = type_rank(b.type);
    if (ra != rb) return ra < rb;
    if (a.max != b.max) return a.max > b.max;
    return a.name < b.name;
  });
  return out;
}

bool primary_backlight(Backlight &out, const std::string &root) {
  auto all = backlights(root);
  if (all.empty()) return false;
  out = all.front();
  return true;
}

int percent_to_raw(int percent, int max) {
  if (max <= 0) return 0;
  percent = std::max(0, std::min(100, percent));
  long raw = (long(percent) * max + 50) / 100;
  return int(std::max(1L, std::min(long(max), raw)));
}

int raw_to_percent(int raw, int max) {
  if (max <= 0) return -1;
  raw = std::max(0, std::min(max, raw));
  return int((long(raw) * 100 + max / 2) / max);
}

int step_percent(int current, int steps, int step) {
  if (step <= 0) step = 5;
  current = std::max(1, std::min(100, current));
  while (steps > 0) {
    int s = current < 2 * step ? std::max(1, step / 2) : step;
    // Snap to the grid so that repeated presses give round values.
    current = std::min(100, (current / s + 1) * s);
    --steps;
  }
  while (steps < 0) {
    int s = current <= 2 * step ? std::max(1, step / 2) : step;
    int below = ((current + s - 1) / s - 1) * s;
    current = std::max(1, below);
    ++steps;
  }
  return current;
}

int brightness_percent() {
  Backlight b;
  if (!primary_backlight(b)) return -1;
  return b.percent();
}

bool set_brightness_percent(int percent) {
  Backlight b;
  if (!primary_backlight(b)) return false;
  int raw = percent_to_raw(percent, b.max);
  if (dbus::Bus *bus = dbus::Bus::system()) {
    DBusMessage *m = dbus::method(kLogin, "/org/freedesktop/login1/session/auto",
                                  "org.freedesktop.login1.Session", "SetBrightness");
    dbus::Writer(m).str("backlight").str(b.name).u32(uint32_t(raw));
    if (DBusMessage *reply = bus->call(m, 1000)) {
      dbus_message_unref(reply);
      return true;
    }
  }
  std::string file = "/sys/class/backlight/" + b.name + "/brightness";
  if (access(file.c_str(), W_OK) == 0) {
    std::ofstream out(file);
    out << raw;
    out.flush();
    if (out) return true;
  }
  if (!find_program("brightnessctl").empty()) {
    int status = -1;
    run_sync({"brightnessctl", "--quiet", "--device=" + b.name, "set", std::to_string(raw)},
             &status, 2000);
    return status == 0;
  }
  return false;
}

int step_brightness(int steps, int step) {
  int now = brightness_percent();
  if (now < 0) return -1;
  int next = step_percent(now, steps, step);
  if (next != now && !set_brightness_percent(next)) return now;
  return next;
}

namespace {

struct ProfilesService {
  const char *name, *path, *iface;
};
const ProfilesService kProfileServices[] = {
    {"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles",
     "org.freedesktop.UPower.PowerProfiles"},
    {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles"},
};

}  // namespace

Profiles power_profiles() {
  Profiles out;
  dbus::Bus *bus = dbus::Bus::system();
  if (!bus) return out;
  for (const auto &svc : kProfileServices) {
    DBusMessage *m = dbus::method(svc.name, svc.path, "org.freedesktop.DBus.Properties", "GetAll");
    dbus::Writer(m).str(svc.iface);
    DBusMessage *reply = bus->call(m, 1000);
    if (!reply) continue;
    auto args = dbus::read_args(reply);
    dbus_message_unref(reply);
    if (args.empty()) continue;
    const dbus::Value &all = args[0];
    if (const dbus::Value *active = all.get("ActiveProfile")) out.active = active->str();
    if (const dbus::Value *list = all.get("Profiles")) {
      // aa{sv}: each entry has a "Profile" string.
      for (const auto &entry : list->inner().items)
        if (const dbus::Value *p = entry.get("Profile")) out.names.push_back(p->str());
    }
    out.available = true;
    return out;
  }
  return out;
}

bool set_power_profile(const std::string &name) {
  if (name.empty()) return false;
  dbus::Bus *bus = dbus::Bus::system();
  if (!bus) return false;
  for (const auto &svc : kProfileServices) {
    DBusMessage *m = dbus::method(svc.name, svc.path, "org.freedesktop.DBus.Properties", "Set");
    {
      dbus::Writer w(m);
      w.str(svc.iface).str("ActiveProfile").variant_str(name);
    }
    if (DBusMessage *reply = bus->call(m, 2000)) {
      dbus_message_unref(reply);
      return true;
    }
  }
  return false;
}

std::string profile_label(const std::string &p) {
  if (p == "power-saver") return _("Power saver");
  if (p == "balanced") return _("Balanced");
  if (p == "performance") return _("Performance");
  return p;
}

}  // namespace power
}  // namespace sde
