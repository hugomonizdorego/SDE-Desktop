// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "config.h"
#include "desktop_entry.h"
#include "process.h"
#include "util.h"
namespace sde {
std::vector<Output> parse_xrandr(const std::string &text) {
  std::vector<Output> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || starts_with(line, "Screen ")) continue;
    if (line[0] != ' ' && line[0] != '\t') {
      std::istringstream words(line);
      Output o;
      std::string state;
      words >> o.name >> state;
      if (state != "connected" && state != "disconnected") continue;
      o.connected = state == "connected";
      std::string w;
      while (words >> w) {
        if (w == "primary") {
          o.primary = true;
          continue;
        }
        int ww, hh, xx, yy;
        char c1, c2, c3;
        std::istringstream g(w);
        if (g >> ww >> c1 >> hh >> c2 >> xx >> c3 >> yy && c1 == 'x' && (c2 == '+' || c2 == '-')) {
          o.enabled = true;
          o.width = ww;
          o.height = hh;
          o.x = c2 == '-' ? -xx : xx;
          o.y = c3 == '-' ? -yy : yy;
          break;
        }
        if (w[0] == '(') break;
      }
      out.push_back(o);
      continue;
    }
    if (out.empty()) continue;
    std::istringstream words(line);
    std::string mode;
    words >> mode;
    if (mode.empty() || mode.find('x') == std::string::npos ||
        !isdigit(static_cast<unsigned char>(mode[0])))
      continue;
    Output &o = out.back();
    if (std::find(o.modes.begin(), o.modes.end(), mode) == o.modes.end()) o.modes.push_back(mode);
    if (line.find('*') != std::string::npos) o.current_mode = mode;
    if (line.find('+') != std::string::npos && o.preferred_mode.empty()) o.preferred_mode = mode;
  }
  for (auto &o : out)
    if (o.enabled && o.current_mode.empty())
      o.current_mode = std::to_string(o.width) + "x" + std::to_string(o.height);
  return out;
}

namespace {
std::string hash(const std::string &text) {
  gchar *v = g_compute_checksum_for_data(
      G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(text.data()), text.size());
  std::string result = v ? v : "";
  g_free(v);
  return result;
}
std::string rate_string(double rate) {
  char buf[40];
  g_ascii_formatd(buf, sizeof buf, "%.6f", rate);
  return buf;
}
std::string profile_path(const std::string &name) {
  return config_home() + "/sde/display-profiles/" + hash(name) + ".ini";
}
std::vector<Output> connected(std::vector<Output> outputs) {
  outputs.erase(
      std::remove_if(outputs.begin(), outputs.end(), [](const Output &o) { return !o.connected; }),
      outputs.end());
  return outputs;
}
Display *query_display = nullptr;
XErrorHandler previous_handler = nullptr;
bool query_failed = false;
int query_error(Display *d, XErrorEvent *e) {
  if (d == query_display) {
    query_failed = true;
    return 0;
  }
  return previous_handler ? previous_handler(d, e) : 0;
}
bool valid_edid(const std::vector<unsigned char> &edid) {
  const unsigned char magic[] = {0, 255, 255, 255, 255, 255, 255, 0};
  if (edid.size() < 128 || !std::equal(magic, magic + 8, edid.begin())) return false;
  unsigned sum = 0;
  for (size_t i = 0; i < 128; ++i) sum += edid[i];
  return sum % 256 == 0;
}
}  // namespace
std::string edid_name(const std::vector<unsigned char> &edid) {
  if (!valid_edid(edid)) return "";
  for (size_t i = 54; i + 18 <= 126; i += 18) {
    if (edid[i] || edid[i + 1] || edid[i + 2] || edid[i + 3] != 0xfc) continue;
    std::string name;
    for (size_t j = i + 5; j < i + 18 && edid[j] != 10; ++j)
      if (edid[j] >= 32 && edid[j] < 127) name += char(edid[j]);
    if (!trim(name).empty()) return trim(name);
  }
  return "";
}
std::vector<Output> query_outputs(bool detect) {
  std::vector<Output> outputs;
  Display *d = XOpenDisplay(nullptr);
  if (!d) return outputs;
  query_display = d;
  query_failed = false;
  previous_handler = XSetErrorHandler(query_error);
  auto close_query = [&] {
    XSync(d, False);
    if (query_failed) outputs.clear();
    XSetErrorHandler(previous_handler);
    query_display = nullptr;
    XCloseDisplay(d);
  };
  int event, error, major = 1, minor = 3;
  if (!XRRQueryExtension(d, &event, &error) || !XRRQueryVersion(d, &major, &minor) || major < 1 ||
      (major == 1 && minor < 3)) {
    close_query();
    return outputs;
  }
  Window root = DefaultRootWindow(d);
  XRRScreenResources *res =
      detect ? XRRGetScreenResources(d, root) : XRRGetScreenResourcesCurrent(d, root);
  if (!res) {
    close_query();
    return outputs;
  }
  RROutput primary = XRRGetOutputPrimary(d, root);
  Atom edid_atom = XInternAtom(d, "EDID", True);
  for (int i = 0; i < res->noutput; ++i) {
    XRROutputInfo *info = XRRGetOutputInfo(d, res, res->outputs[i]);
    if (!info) continue;
    Output o;
    o.name.assign(info->name, info->nameLen);
    o.connected = info->connection == RR_Connected;
    o.primary = primary == res->outputs[i];
    o.mm_width = int(info->mm_width);
    o.mm_height = int(info->mm_height);
    if (o.connected && edid_atom != None) {
      Atom type;
      int format;
      unsigned long n, remaining;
      unsigned char *bytes = nullptr;
      if (XRRGetOutputProperty(d, res->outputs[i], edid_atom, 0, 1024, False, False,
                               AnyPropertyType, &type, &format, &n, &remaining,
                               &bytes) == Success &&
          bytes && format == 8) {
        std::vector<unsigned char> edid(bytes, bytes + n);
        if (valid_edid(edid)) {
          o.identity = hash(std::string(reinterpret_cast<char *>(bytes), n));
          o.readable_name = edid_name(edid);
        }
      }
      if (bytes) XFree(bytes);
    }
    RRMode current = None;
    Rotation supported = 0;
    if (info->crtc) {
      XRRCrtcInfo *ci = XRRGetCrtcInfo(d, res, info->crtc);
      if (ci) {
        current = ci->mode;
        o.enabled = current != None;
        o.x = ci->x;
        o.y = ci->y;
        o.width = int(ci->width);
        o.height = int(ci->height);
        supported = ci->rotations;
        if (ci->rotation & RR_Rotate_90)
          o.rotation = "left";
        else if (ci->rotation & RR_Rotate_180)
          o.rotation = "inverted";
        else if (ci->rotation & RR_Rotate_270)
          o.rotation = "right";
        XRRFreeCrtcInfo(ci);
      }
    } else
      for (int c = 0; c < info->ncrtc; ++c) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(d, res, info->crtcs[c]);
        if (ci) {
          supported |= ci->rotations;
          XRRFreeCrtcInfo(ci);
        }
      }
    for (const auto &r : std::vector<std::pair<Rotation, std::string>>{{RR_Rotate_0, "normal"},
                                                                       {RR_Rotate_90, "left"},
                                                                       {RR_Rotate_180, "inverted"},
                                                                       {RR_Rotate_270, "right"}})
      if (supported & r.first) o.rotations.push_back(r.second);
    if (o.rotations.empty()) o.rotations.push_back("normal");
    for (int m = 0; m < info->nmode; ++m)
      for (int j = 0; j < res->nmode; ++j) {
        const auto &mi = res->modes[j];
        if (mi.id != info->modes[m]) continue;
        DisplayMode mode;
        mode.name.assign(mi.name, mi.nameLength);
        mode.width = int(mi.width);
        mode.height = int(mi.height);
        double total = double(mi.hTotal) * mi.vTotal;
        mode.rate = total > 0 ? double(mi.dotClock) / total : 0;
        if (mi.modeFlags & RR_Interlace) mode.rate *= 2;
        if (mi.modeFlags & RR_DoubleScan) mode.rate /= 2;
        o.available.push_back(mode);
        if (std::find(o.modes.begin(), o.modes.end(), mode.name) == o.modes.end())
          o.modes.push_back(mode.name);
        if (m < info->npreferred && o.preferred_mode.empty()) o.preferred_mode = mode.name;
        if (mi.id == current) {
          o.current_mode = mode.name;
          o.rate = mode.rate;
        }
      }
    outputs.push_back(o);
    XRRFreeOutputInfo(info);
  }
  XRRFreeScreenResources(res);
  close_query();
  return outputs;
}
std::string display_signature(const std::vector<Output> &outputs) {
  std::vector<std::string> ids;
  for (const auto &o : outputs)
    if (o.connected) ids.push_back(o.name + ":" + o.identity);
  std::sort(ids.begin(), ids.end());
  std::string result;
  for (const auto &id : ids) result += id + "\n";
  return result;
}
std::string display_topology(const std::vector<Output> &outputs) {
  std::string text = display_signature(outputs);
  for (const auto &o : outputs) {
    text += o.name + o.current_mode + o.rotation + std::to_string(o.enabled) +
            std::to_string(o.primary) + ":" + std::to_string(o.x) + ":" + std::to_string(o.y) +
            ":" + rate_string(o.rate);
    for (const auto &m : o.available) text += m.name + ":" + rate_string(m.rate);
  }
  return text;
}
int output_width(const Output &o) {
  for (const auto &m : o.available)
    if (m.name == o.current_mode)
      return o.rotation == "left" || o.rotation == "right" ? m.height : m.width;
  int w = 0, h = 0;
  sscanf(o.current_mode.c_str(), "%dx%d", &w, &h);
  return o.rotation == "left" || o.rotation == "right" ? h : w;
}
int output_height(const Output &o) {
  for (const auto &m : o.available)
    if (m.name == o.current_mode)
      return o.rotation == "left" || o.rotation == "right" ? m.width : m.height;
  int w = 0, h = 0;
  sscanf(o.current_mode.c_str(), "%dx%d", &w, &h);
  return o.rotation == "left" || o.rotation == "right" ? w : h;
}
std::string validate_display_layout(const std::vector<Output> &wanted,
                                    const std::vector<Output> &hardware) {
  if (connected(wanted).size() != connected(hardware).size() ||
      display_signature(wanted) != display_signature(hardware))
    return "Displays changed. Detect displays again before applying this layout.";
  std::set<std::string> names;
  int enabled = 0, primary = 0;
  for (const auto &o : wanted) {
    if (!o.connected) continue;
    if (!names.insert(o.name).second) return "A display appears more than once.";
    auto h = std::find_if(hardware.begin(), hardware.end(),
                          [&](const Output &v) { return v.connected && v.name == o.name; });
    if (h == hardware.end()) return "A display is no longer connected.";
    if (!o.enabled) continue;
    ++enabled;
    primary += o.primary;
    if (o.current_mode.empty() ||
        std::find(h->modes.begin(), h->modes.end(), o.current_mode) == h->modes.end())
      return "The selected resolution is not supported by this display.";
    if (o.rotation != "normal" && o.rotation != "left" && o.rotation != "right" &&
        o.rotation != "inverted")
      return "Invalid rotation.";
    if (!h->rotations.empty() &&
        std::find(h->rotations.begin(), h->rotations.end(), o.rotation) == h->rotations.end())
      return "The selected rotation is not supported by this display.";
    if (!std::isfinite(o.rate) || o.rate < 0 ||
        (o.rate > 0 &&
         !std::any_of(h->available.begin(), h->available.end(), [&](const DisplayMode &m) {
           return m.name == o.current_mode && std::fabs(m.rate - o.rate) < .02;
         })))
      return "The selected refresh rate is not supported by this resolution.";
    int w = output_width(o), height = output_height(o);
    if (w <= 0 || height <= 0 || o.x < 0 || o.y < 0 || o.x > 32767 - w || o.y > 32767 - height)
      return "Display positions exceed the supported desktop size.";
  }
  if (!enabled) return "At least one display must stay on.";
  if (primary != 1) return "Choose exactly one enabled main display.";
  return "";
}
std::vector<std::string> xrandr_args(const std::vector<Output> &outputs) {
  std::vector<std::string> args;
  for (const auto &o : outputs) {
    if (!o.connected) continue;
    args.insert(args.end(), {"--output", o.name});
    if (!o.enabled) {
      args.push_back("--off");
      continue;
    }
    args.insert(args.end(),
                {"--mode", o.current_mode.empty() ? o.preferred_mode : o.current_mode, "--pos",
                 std::to_string(o.x) + "x" + std::to_string(o.y), "--rotate", o.rotation});
    if (o.rate > 0) args.insert(args.end(), {"--rate", rate_string(o.rate)});
    if (o.primary) args.push_back("--primary");
  }
  return args;
}
bool run_display_layout(const std::vector<Output> &outputs) {
  if (!validate_display_layout(outputs, query_outputs()).empty()) return false;
  std::vector<std::string> command{"xrandr"};
  auto args = xrandr_args(outputs);
  command.insert(command.end(), args.begin(), args.end());
  int code = -1;
  run_sync(command, &code, 6000);
  return code == 0;
}
std::vector<std::string> display_profiles() {
  std::vector<std::string> names;
  std::string dir = config_home() + "/sde/display-profiles";
  for (const auto &file : list_dir(dir)) {
    if (!ends_with(file, ".ini")) continue;
    GKeyFile *k = g_key_file_new();
    if (g_key_file_load_from_file(k, (dir + "/" + file).c_str(), G_KEY_FILE_NONE, nullptr)) {
      gchar *name = g_key_file_get_string(k, "Profile", "name", nullptr);
      if (name && profile_path(name) == dir + "/" + file) names.emplace_back(name);
      g_free(name);
    }
    g_key_file_unref(k);
  }
  std::sort(names.begin(), names.end());
  return names;
}
bool save_display_profile(const DisplayProfile &p) {
  if (p.name.empty() || p.name.size() > 100 ||
      (p.scale != 100 && p.scale != 125 && p.scale != 150 && p.scale != 175 && p.scale != 200))
    return false;
  GKeyFile *k = g_key_file_new();
  g_key_file_set_integer(k, "Profile", "format", 1);
  g_key_file_set_string(k, "Profile", "name", p.name.c_str());
  g_key_file_set_integer(k, "Profile", "scale", p.scale);
  auto outs = connected(p.outputs);
  g_key_file_set_integer(k, "Profile", "count", int(outs.size()));
  for (size_t i = 0; i < outs.size(); ++i) {
    const auto &o = outs[i];
    std::string group = "Display " + std::to_string(i);
    for (const auto &v : std::vector<std::pair<std::string, std::string>>{{"connector", o.name},
                                                                          {"identity", o.identity},
                                                                          {"mode", o.current_mode},
                                                                          {"rotation", o.rotation}})
      g_key_file_set_string(k, group.c_str(), v.first.c_str(), v.second.c_str());
    g_key_file_set_integer(k, group.c_str(), "x", o.x);
    g_key_file_set_integer(k, group.c_str(), "y", o.y);
    g_key_file_set_double(k, group.c_str(), "rate", o.rate);
    g_key_file_set_boolean(k, group.c_str(), "enabled", o.enabled);
    g_key_file_set_boolean(k, group.c_str(), "primary", o.primary);
  }
  gsize n;
  gchar *data = g_key_file_to_data(k, &n, nullptr);
  bool ok = data && make_dirs(config_home() + "/sde/display-profiles") &&
            write_file_atomic(profile_path(p.name), std::string(data, n));
  g_free(data);
  g_key_file_unref(k);
  return ok;
}
bool load_display_profile(const std::string &name, DisplayProfile &profile,
                          const std::vector<Output> &hardware) {
  GKeyFile *k = g_key_file_new();
  if (!g_key_file_load_from_file(k, profile_path(name).c_str(), G_KEY_FILE_NONE, nullptr)) {
    g_key_file_unref(k);
    return false;
  }
  DisplayProfile p;
  p.name = name;
  p.scale = g_key_file_get_integer(k, "Profile", "scale", nullptr);
  int n = g_key_file_get_integer(k, "Profile", "count", nullptr);
  bool ok = g_key_file_get_integer(k, "Profile", "format", nullptr) == 1 && n > 0 && n <= 64 &&
            size_t(n) == connected(hardware).size();
  for (int i = 0; ok && i < n; ++i) {
    std::string group = "Display " + std::to_string(i);
    auto str = [&](const char *key) {
      gchar *v = g_key_file_get_string(k, group.c_str(), key, nullptr);
      std::string s = v ? v : "";
      g_free(v);
      return s;
    };
    std::string connector = str("connector"), identity = str("identity");
    auto h = std::find_if(hardware.begin(), hardware.end(), [&](const Output &o) {
      return o.connected && o.name == connector && o.identity == identity;
    });
    // A unique EDID can survive a dock connector rename; identical EDIDs require exact connectors.
    if (h == hardware.end() && !identity.empty() &&
        std::count_if(hardware.begin(), hardware.end(),
                      [&](const Output &o) { return o.connected && o.identity == identity; }) == 1)
      h = std::find_if(hardware.begin(), hardware.end(),
                       [&](const Output &o) { return o.connected && o.identity == identity; });
    if (h == hardware.end()) {
      ok = false;
      break;
    }
    Output o = *h;
    o.current_mode = str("mode");
    o.rotation = str("rotation");
    o.x = g_key_file_get_integer(k, group.c_str(), "x", nullptr);
    o.y = g_key_file_get_integer(k, group.c_str(), "y", nullptr);
    o.rate = g_key_file_get_double(k, group.c_str(), "rate", nullptr);
    o.enabled = g_key_file_get_boolean(k, group.c_str(), "enabled", nullptr);
    o.primary = g_key_file_get_boolean(k, group.c_str(), "primary", nullptr);
    p.outputs.push_back(o);
  }
  g_key_file_unref(k);
  ok = ok &&
       (p.scale == 100 || p.scale == 125 || p.scale == 150 || p.scale == 175 || p.scale == 200) &&
       validate_display_layout(p.outputs, hardware).empty();
  if (ok) profile = p;
  return ok;
}
bool delete_display_profile(const std::string &name) {
  return unlink(profile_path(name).c_str()) == 0;
}
void save_display_layout(Config &cfg, const std::vector<Output> &outputs) {
  std::string names, args;
  for (const auto &o : outputs)
    if (o.connected) names += (names.empty() ? "" : ",") + o.name;
  for (const auto &a : xrandr_args(outputs)) {
    gchar *q = g_shell_quote(a.c_str());
    args += (args.empty() ? "" : " ") + std::string(q);
    g_free(q);
  }
  cfg.set("display", "outputs", names);
  cfg.set("display", "layout", args);
  cfg.set("display", "hardware", hash(display_signature(outputs)));
  save_display_profile({"Last confirmed", cfg.get_int("appearance", "scale", 100), outputs});
  cfg.save();
}
bool import_legacy_display_layout(const std::string &text, const std::vector<Output> &hardware,
                                  std::vector<Output> &outputs) {
  auto result = connected(hardware);
  for (auto &o : result) o.primary = false;
  Output *selected = nullptr;
  std::set<std::string> seen;
  auto args = split_command(text);
  for (size_t i = 0; i < args.size(); ++i) {
    const auto &key = args[i];
    if (key == "--output") {
      if (++i == args.size() || !seen.insert(args[i]).second) return false;
      auto h = std::find_if(result.begin(), result.end(),
                            [&](const Output &o) { return o.name == args[i]; });
      if (h == result.end()) return false;
      selected = &*h;
    } else if (!selected)
      return false;
    else if (key == "--off")
      selected->enabled = false;
    else if (key == "--primary")
      selected->primary = true;
    else {
      if (++i == args.size()) return false;
      const auto &v = args[i];
      if (key == "--mode") {
        selected->current_mode = v;
        selected->enabled = true;
        selected->rate = 0;
      } else if (key == "--rotate")
        selected->rotation = v;
      else if (key == "--rate") {
        char *end = nullptr;
        selected->rate = g_ascii_strtod(v.c_str(), &end);
        if (!end || end == v.c_str() || *end) return false;
      } else if (key == "--pos") {
        char extra;
        if (sscanf(v.c_str(), "%dx%d%c", &selected->x, &selected->y, &extra) != 2) return false;
      } else
        return false;
    }
  }
  bool primary = false;
  for (const auto &o : result) primary |= o.enabled && o.primary;
  if (!primary)
    for (auto &o : result)
      if (o.enabled) {
        o.primary = true;
        break;
      }
  if (seen.size() != result.size() || !validate_display_layout(result, hardware).empty())
    return false;
  outputs = result;
  return true;
}
bool apply_display_layout(const Config &cfg) {
  auto hardware = query_outputs(true);
  DisplayProfile p;
  if (!load_display_profile("Last confirmed", p, hardware)) {
    if (!cfg.get("display", "hardware").empty()) return false;
    std::string names;
    for (const auto &o : hardware)
      if (o.connected) names += (names.empty() ? "" : ",") + o.name;
    if (names != cfg.get("display", "outputs") ||
        !import_legacy_display_layout(cfg.get("display", "layout"), hardware, p.outputs))
      return false;
    p.name = "Last confirmed";
    p.scale = cfg.get_int("appearance", "scale", 100);
    if (p.scale != 125 && p.scale != 150 && p.scale != 175 && p.scale != 200) p.scale = 100;
  }
  bool primary = false;
  for (const auto &o : hardware) primary |= o.enabled && o.primary;
  if (!primary)
    for (auto &o : hardware)
      if (o.enabled) {
        o.primary = true;
        break;
      }
  DisplayPreview guard(hardware);
  if (!guard.ready() || !run_display_layout(p.outputs) || !guard.keep()) return false;
  save_display_profile(p);
  return true;
}
namespace {
void restore_displays(const std::vector<Output> &previous) {
  auto hardware = connected(query_outputs(true));
  bool enabled = false;
  for (auto &h : hardware) {
    auto old = std::find_if(previous.begin(), previous.end(), [&](const Output &o) {
      return o.name == h.name && o.identity == h.identity;
    });
    if (old != previous.end()) {
      Output copy = *old;
      copy.modes = h.modes;
      copy.available = h.available;
      copy.rotations = h.rotations;
      h = copy;
    }
    enabled |= h.enabled;
  }
  if (!enabled && !hardware.empty()) {
    auto &o = hardware.front();
    o.enabled = o.primary = true;
    o.x = o.y = 0;
    o.rotation = "normal";
    o.rate = 0;
    o.current_mode =
        o.preferred_mode.empty() ? (o.modes.empty() ? "" : o.modes.front()) : o.preferred_mode;
  }
  bool primary = false;
  for (auto &o : hardware)
    if (o.enabled && o.primary) {
      if (primary) o.primary = false;
      primary = true;
    }
  if (!primary)
    for (auto &o : hardware)
      if (o.enabled) {
        o.primary = true;
        break;
      }
  if (run_display_layout(hardware)) return;
  auto fallback = connected(query_outputs(true));
  for (size_t i = 0; i < fallback.size(); ++i) {
    if (fallback[i].modes.empty()) continue;
    auto safe = fallback;
    for (size_t j = 0; j < safe.size(); ++j) {
      auto &o = safe[j];
      o.enabled = o.primary = i == j;
      o.x = o.y = 0;
      o.rotation = "normal";
      o.rate = 0;
      if (!o.modes.empty())
        o.current_mode = o.preferred_mode.empty() ? o.modes.front() : o.preferred_mode;
    }
    if (run_display_layout(safe)) return;
  }
}
}  // namespace
DisplayPreview::DisplayPreview(const std::vector<Output> &previous, int seconds) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return;
  pid_ = fork();
  if (pid_ < 0) {
    close(fds[0]);
    close(fds[1]);
    return;
  }
  if (pid_ == 0) {
    close(fds[1]);
    pollfd p{fds[0], POLLIN | POLLHUP, 0};
    int result;
    do {
      result = poll(&p, 1, seconds * 1000);
    } while (result < 0 && errno == EINTR);
    char command = 0;
    if (result > 0) {
      ssize_t n = read(fds[0], &command, 1);
      if (n != 1) command = 0;
    }
    close(fds[0]);
    if (command != 'K') restore_displays(previous);
    _exit(command == 'K' ? 0 : 1);
  }
  close(fds[0]);
  fd_ = fds[1];
}
DisplayPreview::~DisplayPreview() {
  if (pid_ > 0) revert();
}
bool DisplayPreview::finish(char command) {
  if (pid_ <= 0) return false;
  sigset_t block, old;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  sigprocmask(SIG_BLOCK, &block, &old);
  if (write(fd_, &command, 1) < 0) {
    timespec t{0, 0};
    sigtimedwait(&block, nullptr, &t);
  }
  close(fd_);
  fd_ = -1;
  sigprocmask(SIG_SETMASK, &old, nullptr);
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid_, &status, 0);
  } while (waited < 0 && errno == EINTR);
  pid_ = -1;
  return waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
bool DisplayPreview::keep() { return finish('K'); }
void DisplayPreview::revert() { finish('R'); }
}  // namespace sde
