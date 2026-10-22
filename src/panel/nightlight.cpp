// SPDX-License-Identifier: GPL-3.0-or-later
#include "nightlight.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "config.h"
#include "util.h"
#include "weather.h"

using namespace sde;

namespace nightlight {

Settings Settings::from_config(const Config &cfg) {
  Settings s;
  s.enabled = cfg.get_bool("night_light", "enabled", false);
  s.temperature = std::max(2500, std::min(6500, cfg.get_int("night_light", "temperature", 4000)));
  std::string sched = lower(trim(cfg.get("night_light", "schedule", "sunset")));
  s.schedule = sched == "always" || sched == "custom" ? sched : "sunset";
  int m;
  if (parse_time(cfg.get("night_light", "from", "20:00"), m)) s.from = m;
  if (parse_time(cfg.get("night_light", "to", "07:00"), m)) s.to = m;
  return s;
}

Rgb whitepoint(int kelvin) {
  // Tanner Helland's fit of the blackbody colors, normalized so that
  // 6500 K is neutral.
  auto raw = [](double k, double &r, double &g, double &b) {
    double t = k / 100.0;
    r = t <= 66 ? 255 : 329.698727446 * std::pow(t - 60, -0.1332047592);
    g = t <= 66 ? 99.4708025861 * std::log(t) - 161.1195681661
                : 288.1221695283 * std::pow(t - 60, -0.0755148492);
    b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
  };
  kelvin = std::max(1000, std::min(6500, kelvin));
  double r, g, b, nr, ng, nb;
  raw(kelvin, r, g, b);
  raw(6500, nr, ng, nb);
  auto clamp = [](double v) { return std::max(0.0, std::min(1.0, v)); };
  Rgb c;
  c.r = clamp(r / nr);
  c.g = clamp(g / ng);
  c.b = clamp(b / nb);
  if (kelvin >= 6500) c = Rgb{};
  return c;
}

bool parse_time(const std::string &text, int &minutes) {
  int h, m;
  char extra;
  if (sscanf(trim(text).c_str(), "%d:%d%c", &h, &m, &extra) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  minutes = h * 60 + m;
  return true;
}

std::string format_time(int minutes) {
  minutes = ((minutes % 1440) + 1440) % 1440;
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}

bool sun_times(int year, int month, int day, double lat, double lon, int utc_offset_min,
               int &sunrise, int &sunset) {
  static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  int n = cum[std::max(0, std::min(11, month - 1))] + day + (leap && month > 2 ? 1 : 0);
  double days = leap ? 366 : 365;
  double g = 2 * M_PI / days * (n - 1);
  double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g) -
                            0.014615 * std::cos(2 * g) - 0.040849 * std::sin(2 * g));
  double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) -
                0.006758 * std::cos(2 * g) + 0.000907 * std::sin(2 * g) -
                0.002697 * std::cos(3 * g) + 0.00148 * std::sin(3 * g);
  double phi = lat * M_PI / 180;
  double c = std::cos(90.833 * M_PI / 180) / (std::cos(phi) * std::cos(decl)) -
             std::tan(phi) * std::tan(decl);
  if (c < -1 || c > 1) return false;
  double ha = std::acos(c) * 180 / M_PI;
  double rise = 720 - 4 * (lon + ha) - eqtime + utc_offset_min;
  double set = 720 - 4 * (lon - ha) - eqtime + utc_offset_min;
  auto wrap = [](double m) { return ((int(std::lround(m)) % 1440) + 1440) % 1440; };
  sunrise = wrap(rise);
  sunset = wrap(set);
  return true;
}

bool in_window(int minute, int from, int to) {
  if (from == to) return false;
  if (from < to) return minute >= from && minute < to;
  return minute >= from || minute < to;
}

void window_for(const Settings &s, time_t now, bool have_location, double lat, double lon,
                int &from, int &to) {
  if (s.schedule == "custom") {
    from = s.from;
    to = s.to;
    return;
  }
  from = 19 * 60;
  to = 7 * 60;
  if (s.schedule == "sunset" && have_location) {
    struct tm lt;
    localtime_r(&now, &lt);
    int rise, set;
    if (sun_times(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lat, lon,
                  int(lt.tm_gmtoff / 60), rise, set)) {
      from = set;
      to = rise;
    }
  }
}

bool active(const Settings &s, time_t now, bool have_location, double lat, double lon) {
  if (!s.enabled) return false;
  if (s.schedule == "always") return true;
  int from, to;
  window_for(s, now, have_location, lat, lon, from, to);
  struct tm lt;
  localtime_r(&now, &lt);
  return in_window(lt.tm_hour * 60 + lt.tm_min, from, to);
}

// ------------------------------------------------------------- X gamma

namespace {
// The ramps found before Night Light changed them, per CRTC.
std::map<RRCrtc, std::vector<unsigned short>> g_saved;

bool location(const Config &cfg, double &lat, double &lon) {
  if (weather::parse_lat_lon(cfg.get("weather", "location", "auto"), lat, lon)) return true;
  std::string text, key;
  weather::Report r;
  if (read_file(weather::Service::cache_path(), text) && weather::parse_cache(text, r, key) &&
      r.location.valid) {
    lat = r.location.lat;
    lon = r.location.lon;
    return true;
  }
  return false;
}
}  // namespace

Controller &Controller::get() {
  static Controller c;
  return c;
}

void Controller::start(std::function<const Config *()> config) {
  config_ = std::move(config);
  if (started_) return;
  started_ = true;
  update();
  Fl::add_timeout(60, tick, this);
}

void Controller::tick(void *data) {
  static_cast<Controller *>(data)->update();
  Fl::repeat_timeout(60, tick, data);
}

void Controller::update() {
  const Config *cfg = config_ ? config_() : nullptr;
  if (!cfg) return;
  Settings s = Settings::from_config(*cfg);
  double lat = 0, lon = 0;
  bool have = s.enabled && s.schedule == "sunset" && location(*cfg, lat, lon);
  bool on = active(s, time(nullptr), have, lat, lon);
  bool set = false;
  if (on) {
    // Re-applied every minute: monitors may have been plugged in.
    set = apply(whitepoint(s.temperature));
    if (set) {
      applied_ = true;
      applied_temp_ = s.temperature;
    }
  } else if (applied_) {
    restore();
    applied_ = false;
  }
  // UI tests (Xvfb has no gamma ramps): what the panel decided.
  std::string qa = env("SDE_PANEL_QA_DIR");
  if (!qa.empty()) {
    Rgb c = whitepoint(s.temperature);
    char buf[128];
    snprintf(buf, sizeof buf, "%s %d %.3f %.3f %.3f gamma=%s\n", on ? "on" : "off",
             s.temperature, c.r, c.g, c.b, set ? "set" : "none");
    write_file_atomic(join_path(qa, "night-light.txt"), buf);
  }
}

bool Controller::apply(const Rgb &c) {
  if (!fl_display) fl_open_display();
  Display *d = fl_display;
  if (!d) return false;
  int ev, er;
  if (!XRRQueryExtension(d, &ev, &er)) return false;
  XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, DefaultRootWindow(d));
  if (!res) return false;
  bool any = false;
  for (int i = 0; i < res->ncrtc; ++i) {
    RRCrtc crtc = res->crtcs[i];
    int size = XRRGetCrtcGammaSize(d, crtc);
    if (size < 2) continue;
    if (!g_saved.count(crtc)) {
      // Keep the ramps someone else set (calibration), once.
      // A warm ramp is probably ours from before a panel restart: restore
      // to linear then.
      XRRCrtcGamma *old = XRRGetCrtcGamma(d, crtc);
      if (old && old->size > 1 &&
          old->blue[old->size - 1] < old->red[old->size - 1] * 0.97) {
        XRRFreeGamma(old);
        old = nullptr;
      }
      if (old) {
        std::vector<unsigned short> v;
        for (int k = 0; k < old->size; ++k) v.push_back(old->red[k]);
        for (int k = 0; k < old->size; ++k) v.push_back(old->green[k]);
        for (int k = 0; k < old->size; ++k) v.push_back(old->blue[k]);
        g_saved[crtc] = v;
        XRRFreeGamma(old);
      }
    }
    XRRCrtcGamma *g = XRRAllocGamma(size);
    if (!g) continue;
    for (int k = 0; k < size; ++k) {
      double v = double(k) / (size - 1) * 65535.0;
      g->red[k] = (unsigned short)std::lround(v * c.r);
      g->green[k] = (unsigned short)std::lround(v * c.g);
      g->blue[k] = (unsigned short)std::lround(v * c.b);
    }
    XRRSetCrtcGamma(d, crtc, g);
    XRRFreeGamma(g);
    any = true;
  }
  XRRFreeScreenResources(res);
  XFlush(d);
  return any;
}

void Controller::restore() {
  Display *d = fl_display;
  if (!d) return;
  XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, DefaultRootWindow(d));
  if (!res) return;
  for (int i = 0; i < res->ncrtc; ++i) {
    RRCrtc crtc = res->crtcs[i];
    int size = XRRGetCrtcGammaSize(d, crtc);
    if (size < 2) continue;
    XRRCrtcGamma *g = XRRAllocGamma(size);
    if (!g) continue;
    auto saved = g_saved.find(crtc);
    bool use = saved != g_saved.end() && saved->second.size() == size_t(size) * 3;
    for (int k = 0; k < size; ++k) {
      unsigned short v = (unsigned short)std::lround(double(k) / (size - 1) * 65535.0);
      g->red[k] = use ? saved->second[size_t(k)] : v;
      g->green[k] = use ? saved->second[size_t(size + k)] : v;
      g->blue[k] = use ? saved->second[size_t(2 * size + k)] : v;
    }
    XRRSetCrtcGamma(d, crtc, g);
    XRRFreeGamma(g);
  }
  XRRFreeScreenResources(res);
  XFlush(d);
  g_saved.clear();
}

}  // namespace nightlight
