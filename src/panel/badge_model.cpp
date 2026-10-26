// SPDX-License-Identifier: GPL-3.0-or-later
#include "badge_model.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "util.h"

using sde::Color;
using sde::DesktopEntry;

namespace badges {

void apply(LauncherState &s, const sde::dbus::Value &props) {
  const sde::dbus::Value &dict = props.inner();
  if (dict.type != DBUS_TYPE_ARRAY) return;
  for (auto &e : dict.items) {
    if (e.type != DBUS_TYPE_DICT_ENTRY || e.items.size() != 2) continue;
    const std::string &key = e.items[0].s;
    const sde::dbus::Value &v = e.items[1].inner();
    bool number = std::strchr("ynqiuxtd", v.type) != nullptr && v.type != 0;
    if (key == "count" && number) s.count = v.type == 'd' ? (long long)v.d : (long long)v.i;
    else if (key == "count-visible" && v.type == 'b') s.count_visible = v.b;
    else if (key == "progress" && v.type == 'd') s.progress = std::max(0.0, std::min(1.0, v.d));
    else if (key == "progress-visible" && v.type == 'b') s.progress_visible = v.b;
    else if (key == "urgent" && v.type == 'b') s.urgent = v.b;
  }
}

std::string app_id_from_uri(const std::string &uri) {
  const std::string scheme = "application://";
  if (!sde::starts_with(uri, scheme)) return "";
  std::string id = sde::trim(uri.substr(scheme.size()));
  if (id.empty() || id.find('/') != std::string::npos) return "";
  if (!sde::ends_with(id, ".desktop")) id += ".desktop";
  return id;
}

std::string exec_program(const std::string &exec) {
  // Programs that only start another one.
  static const char *const wrappers[] = {"env",    "sh",       "bash",     "dash",    "zsh",        "nice",
                                         "ionice", "nohup",    "setsid",   "stdbuf",  "taskset",    "sudo",
                                         "pkexec", "firejail", "torsocks", "gamemoderun", "prime-run",
                                         "dbus-launch", "java", "mono",   "wine",    "wine64",     "perl",
                                         "ruby",   "node",     "nodejs",   "gjs",     "lua"};
  bool flatpak = false;
  for (auto &a : sde::split_command(exec)) {
    if (a.empty() || a[0] == '-' || a[0] == '%') continue;           // options, field codes
    if (a.find('=') != std::string::npos && a[0] != '/') continue;  // env VAR=value
    std::string base = sde::basename_of(a);
    if (base == "flatpak") {
      flatpak = true;
      continue;
    }
    if (flatpak && base == "run") continue;
    if (flatpak) return "flatpak:" + base;
    if (sde::starts_with(base, "python") ||
        std::any_of(std::begin(wrappers), std::end(wrappers), [&](const char *w) { return base == w; }))
      continue;
    for (const char *ext : {".py", ".sh", ".pl", ".rb", ".jar", ".AppImage", ".appimage"})
      if (sde::ends_with(base, ext) && base.size() > std::strlen(ext)) return base.substr(0, base.size() - std::strlen(ext));
    return base;
  }
  return "";
}

namespace {

std::string id_stem(const std::string &id) {
  std::string s = sde::lower(id);
  if (sde::ends_with(s, ".desktop")) s.resize(s.size() - 8);
  return s;
}

std::string by_id(std::string id, const std::vector<DesktopEntry> &apps) {
  id = sde::trim(id);
  if (id.empty() || id.find('/') != std::string::npos) return "";
  if (!sde::ends_with(id, ".desktop")) id += ".desktop";
  for (auto &a : apps)
    if (a.id == id) return a.id;
  std::string l = sde::lower(id);
  for (auto &a : apps)
    if (sde::lower(a.id) == l) return a.id;
  return "";
}

std::string by_program(const std::string &program, const std::vector<DesktopEntry> &apps) {
  std::string p = sde::trim(program);
  if (p.empty()) return "";
  if (sde::starts_with(p, "flatpak:")) return by_id(p.substr(8), apps);
  p = sde::lower(p);
  // The application itself before one that opens something with it (a
  // browser before its web apps, which add --app-id=...).
  const DesktopEntry *best = nullptr;
  size_t best_args = 0;
  for (auto &a : apps) {
    bool fits = sde::lower(exec_program(a.exec)) == p || sde::lower(a.startup_wm_class) == p || id_stem(a.id) == p;
    if (!fits) continue;
    size_t args = sde::split_command(a.exec).size();
    if (!best || args < best_args) best = &a, best_args = args;
  }
  return best ? best->id : "";
}

}  // namespace

std::string match_app(const Source &src, const std::vector<DesktopEntry> &apps) {
  std::string id = by_id(src.desktop_id, apps);
  if (id.empty()) id = by_program(src.program, apps);
  if (!id.empty()) return id;
  std::string name = sde::lower(sde::trim(src.app_name));
  if (!name.empty()) {
    for (auto &a : apps)
      if (sde::lower(a.name) == name) return a.id;
    id = by_program(name, apps);
    if (!id.empty()) return id;
    // One name beginning with the other ("Telegram Desktop" and Telegram,
    // "Firefox" and Firefox ESR), when only one application fits.
    const DesktopEntry *only = nullptr;
    int n = 0;
    for (auto &a : apps) {
      std::string an = sde::lower(a.name);
      if (!an.empty() && (sde::starts_with(name, an + " ") || sde::starts_with(an, name + " "))) only = &a, ++n;
    }
    if (n == 1) return only->id;
  }
  std::string icon = sde::trim(src.icon);
  if (!icon.empty() && icon.find('/') == std::string::npos) {
    const DesktopEntry *only = nullptr;
    int n = 0;
    for (auto &a : apps)
      if (a.icon == icon) only = &a, ++n;
    if (n == 1) return only->id;
  }
  return "";
}

Badge combine(const LauncherState *launcher, int unread) {
  Badge b;
  if (launcher && launcher->count_visible && launcher->count > 0)
    b.count = int(std::min<long long>(launcher->count, 1000000));
  else
    b.count = std::max(0, unread);
  if (launcher) {
    b.urgent = launcher->urgent;
    if (launcher->progress_visible) b.progress = std::max(0.0, std::min(1.0, launcher->progress));
  }
  return b;
}

std::string count_text(int count) { return count > 99 ? "99+" : std::to_string(std::max(0, count)); }

Colors readable(Color fill, const Color &surface) {
  fill.a = 1;
  const Color white = Color::hex(0xFFFFFF), dark = Color::hex(0x1B1B1F), black = Color::hex(0x000000);
  // White text unless the colour is light (yellow, lime, sky blue): dark
  // text then; the fill moves away from the text until it has 4.5:1.
  bool light_text = fill.contrast(white) >= 2.6 || fill.contrast(white) >= fill.contrast(dark);
  // A badge almost as light as a light surface gets darker (white text),
  // one almost as dark as a dark surface lighter (dark text); a colour of
  // its own stands apart anyway.
  const double apart = 1.25;
  if (fill.contrast(surface) < apart) light_text = surface.luminance() >= 0.2;
  Color text = light_text ? white : dark;
  for (int i = 0; i < 40 && (fill.contrast(text) < 4.5 || fill.contrast(surface) < apart); ++i)
    fill = fill.mix(light_text ? black : white, 0.06);
  return {fill, text};
}

Colors colors_from_pixels(const unsigned char *argb, int w, int h, int stride, const Color &surface,
                          const Color &fallback) {
  // A histogram of the hues of the coloured pixels, weighted by their
  // saturation and opacity (grey, black and white pixels do not count).
  const int bins = 24;
  double weight[bins] = {}, red[bins] = {}, green[bins] = {}, blue[bins] = {};
  double opaque = 0;
  for (int y = 0; argb && y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint32_t p;
      std::memcpy(&p, argb + size_t(y) * size_t(stride) + size_t(x) * 4, 4);
      double a = double(p >> 24);
      if (a < 128) continue;
      double r = ((p >> 16) & 255) * 255.0 / a, g = ((p >> 8) & 255) * 255.0 / a, b = (p & 255) * 255.0 / a;
      r = std::min(255.0, r), g = std::min(255.0, g), b = std::min(255.0, b);
      opaque += 1;
      double mx = std::max({r, g, b}), mn = std::min({r, g, b});
      double v = mx / 255.0, s = mx > 0 ? (mx - mn) / mx : 0;
      if (s < 0.25 || v < 0.18) continue;
      double hue;
      if (mx == r) hue = (g - b) / (mx - mn);
      else if (mx == g) hue = 2 + (b - r) / (mx - mn);
      else hue = 4 + (r - g) / (mx - mn);
      if (hue < 0) hue += 6;
      int bin = int(hue / 6 * bins) % bins;
      double wt = s * (a / 255.0) * (0.5 + 0.5 * v);
      weight[bin] += wt;
      red[bin] += wt * r;
      green[bin] += wt * g;
      blue[bin] += wt * b;
    }
  // The strongest hue with its neighbours (a hue can fall on two bins).
  int best = -1;
  double best_w = 0;
  for (int i = 0; i < bins; ++i) {
    double w3 = weight[i] + 0.5 * (weight[(i + 1) % bins] + weight[(i + bins - 1) % bins]);
    if (w3 > best_w) best_w = w3, best = i;
  }
  if (best < 0 || best_w < 0.05 * opaque) return readable(fallback, surface);
  double ws = 0, rs = 0, gs = 0, bs = 0;
  for (int d = -1; d <= 1; ++d) {
    int i = (best + d + bins) % bins;
    double k = d == 0 ? 1.0 : 0.5;
    ws += k * weight[i], rs += k * red[i], gs += k * green[i], bs += k * blue[i];
  }
  Color c{rs / ws / 255.0, gs / ws / 255.0, bs / ws / 255.0, 1};
  return readable(c, surface);
}

}  // namespace badges
