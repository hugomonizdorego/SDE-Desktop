// SPDX-License-Identifier: GPL-3.0-or-later
#include "weather.h"

#include <FL/Fl.H>
#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "paint.h"
#include "process.h"
#include "theme.h"
#include "util.h"

using namespace sde;

namespace weather {

// =================================================================== JSON

const Json *Json::get(const std::string &key) const {
  if (type != OBJECT) return nullptr;
  for (const auto &f : fields)
    if (f.first == key) return &f.second;
  return nullptr;
}
double Json::number(const std::string &key, double fallback) const {
  const Json *v = get(key);
  if (!v) return fallback;
  if (v->type == NUMBER) return v->num;
  if (v->type == BOOL) return v->b ? 1 : 0;
  if (v->type == STRING) {
    char *end = nullptr;
    double d = strtod(v->str.c_str(), &end);
    if (end && end != v->str.c_str() && !*end) return d;
  }
  return fallback;
}
bool Json::has_number(const std::string &key) const {
  const Json *v = get(key);
  return v && v->type == NUMBER && std::isfinite(v->num);
}
std::string Json::text(const std::string &key) const {
  const Json *v = get(key);
  return v && v->type == STRING ? v->str : std::string();
}

namespace {

struct Reader {
  const char *p, *end;
  int depth = 0;
  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool lit(const char *s) {
    size_t n = strlen(s);
    if (size_t(end - p) < n || strncmp(p, s, n) != 0) return false;
    p += n;
    return true;
  }
  static void utf8(std::string &out, unsigned c) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  bool hex4(unsigned &c) {
    if (end - p < 4) return false;
    c = 0;
    for (int i = 0; i < 4; ++i) {
      char h = *p++;
      c <<= 4;
      if (h >= '0' && h <= '9') c |= unsigned(h - '0');
      else if (h >= 'a' && h <= 'f') c |= unsigned(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') c |= unsigned(h - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool string(std::string &out) {
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end && *p != '"') {
      char c = *p++;
      if (static_cast<unsigned char>(c) < 0x20) return false;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p >= end) return false;
      char e = *p++;
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned c1;
          if (!hex4(c1)) return false;
          if (c1 >= 0xD800 && c1 < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            unsigned c2;
            if (!hex4(c2)) return false;
            if (c2 >= 0xDC00 && c2 < 0xE000) c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
          }
          utf8(out, c1);
          break;
        }
        default: return false;
      }
    }
    if (p >= end) return false;
    ++p;
    return true;
  }
  bool value(Json &v) {
    if (++depth > 64) return false;
    ws();
    bool ok = false;
    if (p >= end) {
      ok = false;
    } else if (*p == '{') {
      ++p;
      v.type = Json::OBJECT;
      ws();
      if (p < end && *p == '}') {
        ++p;
        ok = true;
      } else {
        while (true) {
          ws();
          std::string key;
          if (!string(key)) break;
          ws();
          if (p >= end || *p != ':') break;
          ++p;
          Json child;
          if (!value(child)) break;
          v.fields.emplace_back(std::move(key), std::move(child));
          ws();
          if (p < end && *p == ',') {
            ++p;
            continue;
          }
          if (p < end && *p == '}') {
            ++p;
            ok = true;
          }
          break;
        }
      }
    } else if (*p == '[') {
      ++p;
      v.type = Json::ARRAY;
      ws();
      if (p < end && *p == ']') {
        ++p;
        ok = true;
      } else {
        while (true) {
          Json child;
          if (!value(child)) break;
          v.items.push_back(std::move(child));
          ws();
          if (p < end && *p == ',') {
            ++p;
            continue;
          }
          if (p < end && *p == ']') {
            ++p;
            ok = true;
          }
          break;
        }
      }
    } else if (*p == '"') {
      v.type = Json::STRING;
      ok = string(v.str);
    } else if (lit("true")) {
      v.type = Json::BOOL;
      v.b = ok = true;
    } else if (lit("false")) {
      v.type = Json::BOOL;
      ok = true;
    } else if (lit("null")) {
      v.type = Json::NUL;
      ok = true;
    } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
      std::string n;
      while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) n += *p++;
      char *stop = nullptr;
      v.type = Json::NUMBER;
      v.num = strtod(n.c_str(), &stop);
      ok = stop && !*stop && std::isfinite(v.num);
    }
    --depth;
    return ok;
  }
};

}  // namespace

bool parse_json(const std::string &text, Json &out) {
  Reader r{text.data(), text.data() + text.size()};
  Json v;
  if (!r.value(v)) return false;
  r.ws();
  if (r.p != r.end) return false;
  out = std::move(v);
  return true;
}

std::string json_escape(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", unsigned(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

// ============================================================= conditions

Condition from_wmo(int code) {
  switch (code) {
    case 0: return {Kind::Clear, 0, N_("Clear sky")};
    case 1: return {Kind::PartlyCloudy, 0, N_("Mainly clear")};
    case 2: return {Kind::PartlyCloudy, 1, N_("Partly cloudy")};
    case 3: return {Kind::Cloudy, 1, N_("Overcast")};
    case 45: return {Kind::Fog, 1, N_("Fog")};
    case 48: return {Kind::Fog, 2, N_("Rime fog")};
    case 51: return {Kind::Drizzle, 0, N_("Light drizzle")};
    case 53: return {Kind::Drizzle, 1, N_("Drizzle")};
    case 55: return {Kind::Drizzle, 2, N_("Dense drizzle")};
    case 56: return {Kind::Drizzle, 0, N_("Light freezing drizzle")};
    case 57: return {Kind::Drizzle, 2, N_("Freezing drizzle")};
    case 61: return {Kind::Rain, 0, N_("Light rain")};
    case 63: return {Kind::Rain, 1, N_("Rain")};
    case 65: return {Kind::Rain, 2, N_("Heavy rain")};
    case 66: return {Kind::Rain, 0, N_("Light freezing rain")};
    case 67: return {Kind::Rain, 2, N_("Freezing rain")};
    case 71: return {Kind::Snow, 0, N_("Light snow")};
    case 73: return {Kind::Snow, 1, N_("Snow")};
    case 75: return {Kind::Snow, 2, N_("Heavy snow")};
    case 77: return {Kind::Snow, 0, N_("Snow grains")};
    case 80: return {Kind::Rain, 0, N_("Light rain showers")};
    case 81: return {Kind::Rain, 1, N_("Rain showers")};
    case 82: return {Kind::Rain, 2, N_("Violent rain showers")};
    case 85: return {Kind::Snow, 1, N_("Snow showers")};
    case 86: return {Kind::Snow, 2, N_("Heavy snow showers")};
    case 95: return {Kind::Thunder, 1, N_("Thunderstorm")};
    case 96: return {Kind::Thunder, 1, N_("Thunderstorm with hail")};
    case 99: return {Kind::Thunder, 2, N_("Thunderstorm with heavy hail")};
  }
  return {Kind::Unknown, 1, ""};
}

const char *kind_name(Kind k) {
  switch (k) {
    case Kind::Clear: return "clear";
    case Kind::PartlyCloudy: return "partly-cloudy";
    case Kind::Cloudy: return "cloudy";
    case Kind::Fog: return "fog";
    case Kind::Drizzle: return "drizzle";
    case Kind::Rain: return "rain";
    case Kind::Snow: return "snow";
    case Kind::Thunder: return "thunder";
    case Kind::Unknown: break;
  }
  return "unknown";
}

// ================================================================ parsing

namespace {
bool valid_lat_lon(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 &&
         lon <= 180;
}
}  // namespace

bool parse_ip_location(const std::string &text, Location &out) {
  Json j;
  if (!parse_json(text, j) || j.type != Json::OBJECT) return false;
  // ipapi.co reports errors as {"error": true, "reason": ...}; ip-api.com
  // as {"status": "fail", "message": ...}.
  if (const Json *e = j.get("error"))
    if (e->type == Json::BOOL && e->b) return false;
  if (j.get("status") && j.text("status") != "success") return false;
  Location l;
  if (j.has_number("latitude") && j.has_number("longitude")) {
    l.lat = j.number("latitude", 0);
    l.lon = j.number("longitude", 0);
  } else if (j.has_number("lat") && j.has_number("lon")) {
    l.lat = j.number("lat", 0);
    l.lon = j.number("lon", 0);
  } else {
    return false;
  }
  if (!valid_lat_lon(l.lat, l.lon)) return false;
  l.city = j.text("city");
  l.country = j.text("country_name");
  if (l.country.empty()) l.country = j.text("country");
  l.valid = true;
  out = l;
  return true;
}

bool parse_geocoding(const std::string &text, Location &out) {
  Json j;
  if (!parse_json(text, j)) return false;
  const Json *results = j.get("results");
  if (!results || results->type != Json::ARRAY || results->items.empty()) return false;
  const Json &r = results->items[0];
  if (!r.has_number("latitude") || !r.has_number("longitude")) return false;
  Location l;
  l.lat = r.number("latitude", 0);
  l.lon = r.number("longitude", 0);
  if (!valid_lat_lon(l.lat, l.lon)) return false;
  l.city = r.text("name");
  l.country = r.text("country");
  l.valid = true;
  out = l;
  return true;
}

bool parse_forecast(const std::string &text, Report &out) {
  Json j;
  if (!parse_json(text, j)) return false;
  if (const Json *e = j.get("error"))
    if (e->type == Json::BOOL && e->b) return false;
  const Json *c = j.get("current");
  if (!c || c->type != Json::OBJECT || !c->has_number("temperature_2m") ||
      !c->has_number("weather_code"))
    return false;
  Report r = out;
  r.temp_c = c->number("temperature_2m", 0);
  r.code = int(c->number("weather_code", -1));
  r.is_day = c->number("is_day", 1) != 0;
  r.wind_kmh = c->number("wind_speed_10m", 0);
  if (r.temp_c < -100 || r.temp_c > 70) return false;
  r.valid = true;
  out = r;
  return true;
}

bool parse_lat_lon(const std::string &text, double &lat, double &lon) {
  auto parts = split(text, ',', true);
  if (parts.size() != 2) return false;
  double v[2];
  for (int i = 0; i < 2; ++i) {
    std::string s = trim(parts[size_t(i)]);
    if (s.empty()) return false;
    char *end = nullptr;
    v[i] = strtod(s.c_str(), &end);
    if (!end || *end) return false;
  }
  if (!valid_lat_lon(v[0], v[1])) return false;
  lat = v[0];
  lon = v[1];
  return true;
}

std::string url_encode(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string forecast_url(double lat, double lon) {
  char buf[256];
  snprintf(buf, sizeof buf,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day,wind_speed_10m&timezone=auto",
           lat, lon);
  return buf;
}

std::string geocoding_url(const std::string &name) {
  return "https://geocoding-api.open-meteo.com/v1/search?count=1&format=json&name=" +
         url_encode(trim(name));
}

const char *const kIpLocationUrls[2] = {
    "https://ipapi.co/json/",
    "http://ip-api.com/json/?fields=status,message,country,city,lat,lon",
};

std::string cache_json(const Report &r, const std::string &key) {
  char nums[256];
  snprintf(nums, sizeof nums,
           "\"temperature\":%.2f,\"code\":%d,\"is_day\":%d,\"wind\":%.2f,\"fetched\":%lld,"
           "\"lat\":%.5f,\"lon\":%.5f",
           r.temp_c, r.code, r.is_day ? 1 : 0, r.wind_kmh, r.fetched, r.location.lat,
           r.location.lon);
  return std::string("{\"version\":1,\"key\":") + json_escape(key) + "," + nums +
         ",\"city\":" + json_escape(r.location.city) +
         ",\"country\":" + json_escape(r.location.country) + "}\n";
}

bool parse_cache(const std::string &text, Report &out, std::string &key) {
  Json j;
  if (!parse_json(text, j) || j.number("version", 0) != 1 || !j.has_number("code") ||
      !j.has_number("temperature") || !j.has_number("fetched"))
    return false;
  Report r;
  r.temp_c = j.number("temperature", 0);
  r.code = int(j.number("code", -1));
  r.is_day = j.number("is_day", 1) != 0;
  r.wind_kmh = j.number("wind", 0);
  r.fetched = (long long)j.number("fetched", 0);
  r.location.lat = j.number("lat", 0);
  r.location.lon = j.number("lon", 0);
  r.location.city = j.text("city");
  r.location.country = j.text("country");
  r.location.valid = valid_lat_lon(r.location.lat, r.location.lon);
  r.valid = true;
  key = j.text("key");
  out = r;
  return true;
}

// ================================================================== units

bool use_fahrenheit(const std::string &unit, const std::string &locale) {
  std::string u = lower(trim(unit));
  if (u == "fahrenheit" || u == "f") return true;
  if (u == "celsius" || u == "c") return false;
  // ll_CC[.codeset][@modifier]
  size_t us = locale.find('_');
  if (us == std::string::npos) return false;
  std::string cc = locale.substr(us + 1, 2);
  for (const char *c : {"US", "LR", "MM", "BS", "BZ", "KY", "PW", "FM", "MH"})
    if (cc == c) return true;
  return false;
}

std::string locale_for_units() {
  for (const char *v : {"LC_ALL", "LC_MEASUREMENT", "LANG"}) {
    std::string s = env(v);
    if (!s.empty()) return s;
  }
  return "";
}

std::string format_temperature(double c, bool f) {
  double v = f ? c * 9 / 5 + 32 : c;
  long n = std::lround(v);
  if (n == 0) n = 0;  // no "-0"
  return std::to_string(n) + (f ? "°F" : "°C");
}

bool fixture_report(const std::string &name, Report &out) {
  struct F {
    const char *name;
    int code;
    bool day;
    double temp;
  };
  static const F list[] = {
      {"clear", 0, true, 27},       {"clear-night", 0, false, 18}, {"partly", 2, true, 24},
      {"partly-night", 2, false, 17}, {"cloudy", 3, true, 19},      {"fog", 45, true, 9},
      {"drizzle", 53, true, 15},    {"rain", 63, true, 21},        {"rain-night", 65, false, 16},
      {"snow", 73, true, -3},       {"snow-night", 75, false, -6}, {"thunder", 95, true, 26},
      {"thunder-night", 99, false, 22},
  };
  for (const auto &f : list)
    if (name == f.name) {
      Report r;
      r.valid = true;
      r.code = f.code;
      r.is_day = f.day;
      r.temp_c = f.temp;
      r.wind_kmh = 12;
      r.fetched = (long long)time(nullptr);
      r.location.valid = true;
      r.location.lat = -6.2;
      r.location.lon = 106.8;
      r.location.city = "Jakarta";
      r.location.country = "Indonesia";
      out = r;
      return true;
    }
  return false;
}

// =============================================================== settings

Settings Settings::from_config(const Config &cfg) {
  Settings s;
  s.enabled = cfg.get_bool("weather", "enabled", true);
  s.location = trim(cfg.get("weather", "location", "auto"));
  if (s.location.empty()) s.location = "auto";
  s.fahrenheit = use_fahrenheit(cfg.get("weather", "unit", "auto"), locale_for_units());
  // Reduced motion: [weather] animation=false, or the system-wide switches
  // for older computers and for no animations.
  std::string anim = lower(cfg.get("weather", "animation", "auto"));
  if (anim == "false" || anim == "off" || anim == "no" || anim == "0")
    s.animate = false;
  else if (anim == "true" || anim == "on")
    s.animate = true;
  else
    s.animate = !cfg.get_bool("appearance", "low_resource", false) &&
                cfg.get_bool("aero", "animations", true) &&
                !cfg.get_bool("appearance", "reduced_motion", false);
  s.cache_minutes = std::max(10, std::min(180, cfg.get_int("weather", "cache_minutes", 25)));
  s.fps = std::max(10, std::min(30, cfg.get_int("weather", "fps", 24)));
  return s;
}

// ================================================================ service

namespace {
long long unix_now() { return (long long)time(nullptr); }
const std::string &fixture_name() {
  static const std::string name = env("SDE_WEATHER_FIXTURE");
  return name;
}
// curl exit codes that mean "no internet" rather than a service error.
bool network_failure(int status) {
  return status == 5 || status == 6 || status == 7 || status == 28 || status == 35 ||
         status == 56 || status < 0;
}
std::vector<std::string> curl(const std::string &url) {
  return {"curl", "-fsSL", "--max-time", "10", "--connect-timeout", "6",
          "-A", std::string("SDE/") + SDE_VERSION + " (weather)", url};
}
const long long kStale = 3 * 3600;      // a cached report is shown this long
const long long kLocationAge = 6 * 3600;  // the IP location is reused this long
const long long kRetry = 5 * 60;          // after a failure
bool g_reachable = true;
}  // namespace

Service &Service::get() {
  static Service s;
  return s;
}

std::string Service::cache_path() {
  std::string base = env("XDG_CACHE_HOME");
  if (base.empty()) base = join_path(home_dir(), ".cache");
  return join_path(join_path(base, "sde"), "weather.json");
}

void Service::load_cache() {
  if (loaded_) return;
  loaded_ = true;
  std::string text;
  Report r;
  std::string key;
  if (read_file(cache_path(), text) && parse_cache(text, r, key)) {
    report_ = r;
    key_ = key;
  }
}

void Service::save_cache() {
  std::string path = cache_path();
  make_dirs(path.substr(0, path.rfind('/')));
  write_file_atomic(path, cache_json(report_, key_));
}

const Report *Service::current(const Settings &s, bool online) const {
  if (!fixture_name().empty()) {
    static Report fixed;
    static bool ok = fixture_report(fixture_name(), fixed);
    return ok && s.enabled ? &fixed : nullptr;
  }
  if (!s.enabled || !online || !g_reachable) return nullptr;
  const_cast<Service *>(this)->load_cache();
  if (!report_.valid || key_ != s.location) return nullptr;
  long long age = unix_now() - report_.fetched;
  if (age < -600 || age > kStale) return nullptr;
  return &report_;
}

void Service::request(const Settings &s, bool online) {
  if (!fixture_name().empty() || !s.enabled || !online || busy_) return;
  load_cache();
  long long now = unix_now();
  bool same = report_.valid && key_ == s.location;
  if (same && now - report_.fetched >= 0 && now - report_.fetched < s.cache_minutes * 60LL) return;
  if (last_attempt_ && now - last_attempt_ < kRetry) return;
  if (find_program("curl").empty()) return;
  last_attempt_ = now;
  busy_ = true;
  pending_ = s.location;
  double lat, lon;
  if (parse_lat_lon(s.location, lat, lon)) {
    Location l;
    l.valid = true;
    l.lat = lat;
    l.lon = lon;
    if (same) {
      l.city = report_.location.city;
      l.country = report_.location.country;
    }
    fetch_weather(l);
  } else if (lower(s.location) == "auto" && same && report_.location.valid &&
             now - report_.fetched < kLocationAge) {
    fetch_weather(report_.location);  // the IP location changes rarely
  } else {
    fetch_location(s, 0);
  }
}

void Service::fetch_location(const Settings &s, int step) {
  bool automatic = lower(s.location) == "auto";
  std::string url = automatic ? kIpLocationUrls[step] : geocoding_url(s.location);
  bool started = run_async(curl(url), [this, s, step, automatic](const std::string &out, int st) {
    Location l;
    bool ok = st == 0 && (automatic ? parse_ip_location(out, l) : parse_geocoding(out, l));
    if (ok) {
      fetch_weather(l);
      return;
    }
    if (automatic && step == 0 && !network_failure(st)) {
      fetch_location(s, 1);  // the second service
      return;
    }
    g_reachable = !network_failure(st);
    finish(false);
  });
  if (!started) finish(false);
}

void Service::fetch_weather(const Location &loc) {
  bool started = run_async(curl(forecast_url(loc.lat, loc.lon)),
                           [this, loc](const std::string &out, int st) {
                             Report r;
                             r.location = loc;
                             if (st == 0 && parse_forecast(out, r)) {
                               r.fetched = unix_now();
                               report_ = r;
                               key_ = pending_;
                               g_reachable = true;
                               save_cache();
                               finish(true);
                             } else {
                               g_reachable = !network_failure(st);
                               finish(false);
                             }
                           });
  if (!started) finish(false);
}

void Service::finish(bool ok) {
  busy_ = false;
  if (ok) last_attempt_ = 0;
  if (changed_) changed_();
}

// ================================================================== scene

namespace {

double hash(int i) {
  double v = std::sin(i * 12.9898 + 78.233) * 43758.5453;
  return v - std::floor(v);
}

void set(cairo_t *cr, const Color &c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void cloud(cairo_t *cr, double cx, double cy, double s, const Color &c) {
  // Three puffs on a flat base.
  set(cr, c);
  cairo_new_path(cr);
  cairo_arc(cr, cx - s * .55, cy + s * .08, s * .38, 0, 2 * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx, cy - s * .12, s * .55, 0, 2 * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx + s * .6, cy + s * .1, s * .36, 0, 2 * M_PI);
  cairo_new_sub_path(cr);
  cairo_rectangle(cr, cx - s * .55, cy + s * .05, s * 1.15, s * .41);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  cairo_fill(cr);
}

struct Sky {
  Color top, bottom, cloud;
};

Sky sky_for(Kind k, bool day) {
  auto h = [](unsigned v) { return Color::hex(v); };
  switch (k) {
    case Kind::Clear:
      return day ? Sky{h(0x2F80D8), h(0x8CC7F2), h(0xFFFFFF)}
                 : Sky{h(0x0A1230), h(0x283766), h(0xC9D3EA)};
    case Kind::PartlyCloudy:
      return day ? Sky{h(0x3B86D2), h(0x9DCBEE), h(0xFFFFFF)}
                 : Sky{h(0x101935), h(0x2E3C68), h(0xB8C3DC)};
    case Kind::Cloudy:
      return day ? Sky{h(0x5F7389), h(0x9AAABB), h(0xE6EBF0)}
                 : Sky{h(0x1A2132), h(0x39445A), h(0x8792A6)};
    case Kind::Fog:
      return day ? Sky{h(0x75818D), h(0xA9B2BA), h(0xE8ECEF)}
                 : Sky{h(0x262C35), h(0x4B535E), h(0xA0A8B3)};
    case Kind::Drizzle:
    case Kind::Rain:
      return day ? Sky{h(0x3E4E62), h(0x6E7F93), h(0xB9C3CF)}
                 : Sky{h(0x121926), h(0x2C3647), h(0x6E7A8C)};
    case Kind::Snow:
      return day ? Sky{h(0x5A7290), h(0x9AAFC6), h(0xF1F5F9)}
                 : Sky{h(0x18233A), h(0x3B4A66), h(0xC6D0DE)};
    case Kind::Thunder:
      return day ? Sky{h(0x262B3D), h(0x4B5068), h(0x8A90A6)}
                 : Sky{h(0x0E101A), h(0x272B3D), h(0x5B6078)};
    case Kind::Unknown: break;
  }
  return Sky{h(0x47617D), h(0x7F95AB), h(0xE0E6EC)};
}

}  // namespace

void draw_scene(Painter &p, double x, double y, double w, double h, double radius,
                const Report &r, double t) {
  cairo_t *cr = p.cr();
  Condition c = from_wmo(r.code);
  Kind k = c.kind;
  bool day = r.is_day;
  Sky sky = sky_for(k, day);
  cairo_save(cr);
  p.rounded_rect(x, y, w, h, radius);
  cairo_clip(cr);
  // Sky.
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
  cairo_pattern_add_color_stop_rgb(g, 0, sky.top.r, sky.top.g, sky.top.b);
  cairo_pattern_add_color_stop_rgb(g, 1, sky.bottom.r, sky.bottom.g, sky.bottom.b);
  cairo_set_source(cr, g);
  cairo_paint(cr);
  cairo_pattern_destroy(g);

  double u = h;  // scene unit: everything scales with the card height
  bool open_sky = k == Kind::Clear || k == Kind::PartlyCloudy;
  // Night: stars and the moon; day: the sun with slowly turning rays.
  double bx = x + w * .80, by = y + h * .36, br = u * .17;
  if (!day) {
    int stars = open_sky ? 22 : k == Kind::Cloudy ? 6 : 0;
    for (int i = 0; i < stars; ++i) {
      double sx = x + hash(i * 3 + 1) * w, sy = y + hash(i * 3 + 2) * h * .85;
      double tw = .45 + .55 * (.5 + .5 * std::sin(t * (1.2 + hash(i) * 2.2) + i));
      double sr = u * (.008 + .012 * hash(i * 7));
      set(cr, Color::hex(0xFFFFFF).alpha(.85 * tw));
      cairo_new_path(cr);
      cairo_arc(cr, sx, sy, sr, 0, 2 * M_PI);
      cairo_fill(cr);
    }
    if (open_sky || k == Kind::Cloudy || k == Kind::Fog) {
      // Glow, then a crescent cut out in a group.
      cairo_pattern_t *glow = cairo_pattern_create_radial(bx, by, br * .6, bx, by, br * 2.6);
      cairo_pattern_add_color_stop_rgba(glow, 0, 1, .97, .85, .28);
      cairo_pattern_add_color_stop_rgba(glow, 1, 1, .97, .85, 0);
      cairo_set_source(cr, glow);
      cairo_paint(cr);
      cairo_pattern_destroy(glow);
      cairo_push_group(cr);
      set(cr, Color::hex(0xF7F1D8));
      cairo_new_path(cr);
      cairo_arc(cr, bx, by, br, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
      cairo_new_path(cr);
      cairo_arc(cr, bx + br * .45, by - br * .3, br * .85, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_pop_group_to_source(cr);
      cairo_paint(cr);
    }
  } else if (open_sky) {
    cairo_pattern_t *glow = cairo_pattern_create_radial(bx, by, br * .5, bx, by, br * 3.2);
    cairo_pattern_add_color_stop_rgba(glow, 0, 1, .93, .55, .55);
    cairo_pattern_add_color_stop_rgba(glow, 1, 1, .93, .55, 0);
    cairo_set_source(cr, glow);
    cairo_paint(cr);
    cairo_pattern_destroy(glow);
    double a0 = t * .35;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, u * .025);
    for (int i = 0; i < 10; ++i) {
      double a = a0 + i * M_PI / 5;
      double pulse = .5 + .5 * std::sin(t * 1.6 + i);
      set(cr, Color::hex(0xFFE9A8).alpha(.55 + .3 * pulse));
      cairo_move_to(cr, bx + std::cos(a) * br * 1.35, by + std::sin(a) * br * 1.35);
      cairo_line_to(cr, bx + std::cos(a) * br * (1.75 + .2 * pulse),
                    by + std::sin(a) * br * (1.75 + .2 * pulse));
      cairo_stroke(cr);
    }
    set(cr, Color::hex(0xFFD25A));
    cairo_new_path(cr);
    cairo_arc(cr, bx, by, br, 0, 2 * M_PI);
    cairo_fill(cr);
  }
  // Clouds drift from left to right and wrap around.
  int clouds = k == Kind::Clear ? 0 : k == Kind::PartlyCloudy ? 2 : k == Kind::Fog ? 1 : 4;
  for (int i = 0; i < clouds; ++i) {
    double s = u * (.20 + .10 * hash(i + 40)) * (k == Kind::PartlyCloudy ? 1.0 : 1.15);
    double speed = u * (.05 + .05 * hash(i + 50));
    double span = w + s * 3;
    double cx = x - s * 1.5 + std::fmod(hash(i + 60) * span + t * speed, span);
    double cy = y + h * (.18 + .30 * hash(i + 70)) + (k == Kind::PartlyCloudy ? h * .12 : 0);
    double alpha = k == Kind::PartlyCloudy ? .92 : .55 + .35 * hash(i + 80);
    cloud(cr, cx, cy, s, sky.cloud.alpha(alpha));
  }
  // Precipitation.
  if (k == Kind::Rain || k == Kind::Drizzle || k == Kind::Thunder) {
    bool drizzle = k == Kind::Drizzle;
    int drops = drizzle ? 10 + 6 * c.intensity : 14 + 10 * c.intensity;
    double len = u * (drizzle ? .07 : .16), speed = u * (drizzle ? 1.1 : 1.9);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, std::max(1.0, u * (drizzle ? .014 : .018)));
    set(cr, Color::hex(0xD6E6F5).alpha(drizzle ? .55 : .62));
    for (int i = 0; i < drops; ++i) {
      double v = speed * (.8 + .4 * hash(i + 200));
      double range = h + len;
      double dy = std::fmod(hash(i + 100) * range + t * v, range) - len;
      double dx = x + hash(i + 300) * (w + len) - len * .3 * (dy - y) / h;
      cairo_move_to(cr, dx, y + dy);
      cairo_line_to(cr, dx - len * .25, y + dy + len);
    }
    cairo_stroke(cr);
  } else if (k == Kind::Snow) {
    int flakes = 14 + 9 * c.intensity;
    set(cr, Color::hex(0xFFFFFF).alpha(.9));
    for (int i = 0; i < flakes; ++i) {
      double fr = u * (.012 + .016 * hash(i + 500));
      double v = u * (.16 + .14 * hash(i + 600));
      double range = h + fr * 4;
      double fy = y + std::fmod(hash(i + 700) * range + t * v, range) - fr * 2;
      double fx = x + hash(i + 800) * w + std::sin(t * (.8 + hash(i) ) + i) * u * .06;
      cairo_new_path(cr);
      cairo_arc(cr, fx, fy, fr, 0, 2 * M_PI);
      cairo_fill(cr);
    }
  } else if (k == Kind::Fog) {
    for (int i = 0; i < 4; ++i) {
      double bh = u * .11, fy = y + h * (.30 + .17 * i);
      double off = std::sin(t * (.25 + .07 * i) + i * 1.7) * w * .08;
      set(cr, Color::hex(0xF2F4F6).alpha(.20 + .06 * (i % 2)));
      p.rounded_rect(x - w * .1 + off + (i % 2) * w * .15, fy, w * .95, bh, bh / 2);
      cairo_fill(cr);
    }
  }
  // Lightning: two quick flashes and a bolt every few seconds.
  if (k == Kind::Thunder) {
    double cycle = std::fmod(t, 5.5);
    double flash = cycle < .08 ? .45 : (cycle > .18 && cycle < .26) ? .30 : 0;
    if (flash > 0) {
      set(cr, Color::hex(0xFFFFFF).alpha(flash));
      cairo_paint(cr);
      double lx = x + w * (.55 + .2 * hash(int(t / 5.5)));
      set(cr, Color::hex(0xFFF6C8).alpha(.95));
      cairo_set_line_width(cr, std::max(1.5, u * .03));
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_move_to(cr, lx, y + h * .2);
      cairo_line_to(cr, lx - u * .08, y + h * .5);
      cairo_line_to(cr, lx + u * .02, y + h * .52);
      cairo_line_to(cr, lx - u * .07, y + h * .85);
      cairo_stroke(cr);
    }
  }
  // Keep the text side readable: a soft shade from the left.
  cairo_pattern_t *shade = cairo_pattern_create_linear(x, y, x + w * .75, y);
  cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, .05, .30);
  cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, .05, 0);
  cairo_set_source(cr, shade);
  cairo_paint(cr);
  cairo_pattern_destroy(shade);
  cairo_restore(cr);
}

}  // namespace weather
