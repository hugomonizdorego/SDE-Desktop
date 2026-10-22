// SPDX-License-Identifier: GPL-3.0-or-later
// Current weather for the Action Center's clock: an animated sky behind
// the clock, and the temperature, condition and city next to it.
//
// Data (no API keys; see [weather] in sde.conf):
//  * location: [weather] location=auto asks a free IP geolocation service
//    (https://ipapi.co/json/, then http://ip-api.com/json/ when that fails);
//    "lat,lon" is used as given; any other text is a place name looked up
//    with https://geocoding-api.open-meteo.com/v1/search.
//  * weather: https://api.open-meteo.com/v1/forecast (current temperature,
//    WMO weather code, day/night and wind).
// Requests run in the background with curl (never blocking the panel); the
// result is cached in ~/.cache/sde/weather.json for [weather] cache_minutes
// (25 by default) and only shown while the computer is online.
//
// SDE_WEATHER_FIXTURE=clear|clear-night|partly|cloudy|fog|drizzle|rain|
// snow|thunder|offline replaces the service with fixed data (UI tests).
//
// The model (WMO codes, JSON parsing, units) has no FLTK dependency and is
// unit tested in tests/test_weather.cpp with offline JSON fixtures.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sde {
class Config;
class Painter;
}  // namespace sde

namespace weather {

// ------------------------------------------------------------------- JSON
// A small JSON reader, enough for the weather and geolocation services.
struct Json {
  enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } type = NUL;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> items;                           // ARRAY
  std::vector<std::pair<std::string, Json>> fields;  // OBJECT, in order
  const Json *get(const std::string &key) const;     // nullptr when absent
  double number(const std::string &key, double fallback) const;
  std::string text(const std::string &key) const;
  bool has_number(const std::string &key) const;
};
// Parses |text|; false (and |out| untouched) on malformed input.
bool parse_json(const std::string &text, Json &out);
std::string json_escape(const std::string &s);  // with the quotes

// ---------------------------------------------------------- conditions
// What the sky shows.
enum class Kind { Clear, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Snow, Thunder, Unknown };
struct Condition {
  Kind kind = Kind::Unknown;
  int intensity = 1;          // 0 light, 1 moderate, 2 heavy (particles)
  const char *text = "";      // untranslated (N_), translate with _()
};
// WMO weather interpretation code (Open-Meteo "weather_code").
Condition from_wmo(int code);
const char *kind_name(Kind k);  // "clear", "rain", ... (QA dumps)

// ------------------------------------------------------------ the data
struct Location {
  bool valid = false;
  double lat = 0, lon = 0;
  std::string city, country;
};
struct Report {
  bool valid = false;
  double temp_c = 0;
  int code = -1;
  bool is_day = true;
  double wind_kmh = 0;
  long long fetched = 0;  // unix time
  Location location;
};

// ipapi.co/json or ip-api.com/json.
bool parse_ip_location(const std::string &json, Location &out);
// geocoding-api.open-meteo.com/v1/search: the first result.
bool parse_geocoding(const std::string &json, Location &out);
// api.open-meteo.com/v1/forecast with current=temperature_2m,weather_code,
// is_day,wind_speed_10m.
bool parse_forecast(const std::string &json, Report &out);
// "lat,lon" (decimal degrees, optional spaces); false for anything else.
bool parse_lat_lon(const std::string &text, double &lat, double &lon);

std::string forecast_url(double lat, double lon);
std::string geocoding_url(const std::string &name);
std::string url_encode(const std::string &s);
extern const char *const kIpLocationUrls[2];

// Cache file contents; |key| is the [weather] location it was made for.
std::string cache_json(const Report &r, const std::string &key);
bool parse_cache(const std::string &json, Report &out, std::string &key);

// Units: [weather] unit=auto|celsius|fahrenheit; auto uses Fahrenheit for
// the locales of the few countries that measure in it (LC_ALL,
// LC_MEASUREMENT, LANG; e.g. en_US).
bool use_fahrenheit(const std::string &unit, const std::string &locale);
std::string locale_for_units();  // the environment's measurement locale
std::string format_temperature(double celsius, bool fahrenheit);  // "21°C"
// alpha52: [weather] unit=auto|celsius|fahrenheit|kelvin and
// wind_unit=auto|kmh|mph|ms|knots (auto: mph where Fahrenheit is used and
// in the United Kingdom, else km/h).
enum class TempUnit { Celsius, Fahrenheit, Kelvin };
enum class WindUnit { Kmh, Mph, Ms, Knots };
TempUnit temp_unit(const std::string &unit, const std::string &locale);
WindUnit wind_unit(const std::string &unit, const std::string &locale);
const char *temp_unit_name(TempUnit u);  // "celsius", "fahrenheit", "kelvin"
const char *wind_unit_name(WindUnit u);  // "kmh", "mph", "ms", "knots"
std::string format_temperature(double celsius, TempUnit u);  // "21°C", "70°F", "294 K"
std::string format_wind(double kmh, WindUnit u);  // "12 km/h", "7 mph", "3.3 m/s", "6 kn"

// Fixed reports for SDE_WEATHER_FIXTURE; false for an unknown name or
// "offline".
bool fixture_report(const std::string &name, Report &out);

// ------------------------------------------------------------ settings
struct Settings {
  bool enabled = true;
  std::string location = "auto";
  bool fahrenheit = false;
  TempUnit temp = TempUnit::Celsius;
  WindUnit wind = WindUnit::Kmh;
  bool animate = true;    // false: a still picture (reduced motion)
  int cache_minutes = 25;
  int fps = 24;
  static Settings from_config(const sde::Config &cfg);
};

// ------------------------------------------------------------- service
// Fetches in the background and notifies |changed| from the event loop.
class Service {
 public:
  static Service &get();
  void set_listener(std::function<void()> changed) { changed_ = std::move(changed); }
  // Starts a refresh when the cached report is older than the cache
  // lifetime (or made for another location). |online|: the network reaches
  // the internet.
  void request(const Settings &s, bool online);
  // The report to show, or nullptr (disabled, offline, nothing fetched yet,
  // or a cached report older than a few hours).
  const Report *current(const Settings &s, bool online) const;
  bool busy() const { return busy_; }
  static std::string cache_path();
  // The location of the last report, even offline or when it is old (the
  // sky's sunrise and sunset); nullptr when none is known.
  const Location *known_location() const;

 private:
  void load_cache();
  void save_cache();
  void fetch_location(const Settings &s, int step);
  void fetch_weather(const Location &loc);
  void finish(bool ok);
  std::function<void()> changed_;
  Report report_;
  std::string key_;      // location setting of report_
  std::string pending_;  // location setting being fetched
  bool loaded_ = false, busy_ = false;
  long long last_attempt_ = 0;
};

// --------------------------------------------------------------- scene
// Paints the sky for |r| into a rounded box. |t| is the animation time in
// seconds (any value for a still picture).
void draw_scene(sde::Painter &p, double x, double y, double w, double h, double radius,
                const Report &r, double t);

}  // namespace weather
