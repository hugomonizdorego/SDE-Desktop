// SPDX-License-Identifier: GPL-3.0-or-later
// Action Center weather (WMO codes, Open-Meteo and geolocation JSON, units,
// cache) and Night Light (color temperature, schedule, sunrise/sunset),
// all offline with the JSON fixtures in tests/fixtures/weather.
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/panel/nightlight.h"
#include "../src/panel/weather.h"
#include "config.h"
#include "util.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

std::string fixture(const std::string &name) {
  std::string text;
  if (!sde::read_file(std::string(SDE_TEST_SOURCE_DIR) + "/tests/fixtures/weather/" + name, text))
    std::fprintf(stderr, "missing fixture %s\n", name.c_str()), ++failures;
  return text;
}
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
}  // namespace

int main() {
  using namespace weather;
  // ------------------------------------------------------- WMO codes
  CHECK(from_wmo(0).kind == Kind::Clear);
  CHECK(from_wmo(1).kind == Kind::PartlyCloudy && from_wmo(2).kind == Kind::PartlyCloudy);
  CHECK(from_wmo(3).kind == Kind::Cloudy);
  CHECK(from_wmo(45).kind == Kind::Fog && from_wmo(48).kind == Kind::Fog);
  for (int c : {51, 53, 55, 56, 57}) CHECK(from_wmo(c).kind == Kind::Drizzle);
  for (int c : {61, 63, 65, 66, 67, 80, 81, 82}) CHECK(from_wmo(c).kind == Kind::Rain);
  for (int c : {71, 73, 75, 77, 85, 86}) CHECK(from_wmo(c).kind == Kind::Snow);
  for (int c : {95, 96, 99}) CHECK(from_wmo(c).kind == Kind::Thunder);
  for (int c : {-1, 4, 50, 100}) CHECK(from_wmo(c).kind == Kind::Unknown);
  CHECK(from_wmo(61).intensity == 0 && from_wmo(63).intensity == 1 && from_wmo(65).intensity == 2);
  CHECK(std::string(from_wmo(63).text) == "Rain" && std::string(from_wmo(95).text) == "Thunderstorm");
  CHECK(std::string(kind_name(from_wmo(73).kind)) == "snow");

  // ------------------------------------------------------------ JSON
  Json j;
  CHECK(parse_json(R"({"a":[1,2.5,-3e2,true,false,null],"s":"x\"\\\/\né🌧"})", j));
  CHECK(j.get("a") && j.get("a")->items.size() == 6 && near(j.get("a")->items[2].num, -300));
  CHECK(j.text("s") == "x\"\\/\n\xc3\xa9\xf0\x9f\x8c\xa7");
  CHECK(!parse_json("{\"a\":1} trailing", j));
  CHECK(!parse_json("{\"a\":}", j));
  CHECK(!parse_json("[1,2", j));
  CHECK(!parse_json("", j));
  CHECK(!parse_json("{\"a\":\"\x01\"}", j));
  CHECK(!parse_json(std::string(200, '['), j));  // nesting limit
  CHECK(json_escape("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"");

  // -------------------------------------------------------- forecast
  Report r;
  CHECK(parse_forecast(fixture("open-meteo-rain.json"), r));
  CHECK(r.valid && near(r.temp_c, 27.4) && r.code == 63 && r.is_day && near(r.wind_kmh, 9.7));
  Report night;
  CHECK(parse_forecast(fixture("open-meteo-clear-night.json"), night));
  CHECK(night.code == 0 && !night.is_day && near(night.temp_c, -0.4));
  Report bad;
  CHECK(!parse_forecast(fixture("open-meteo-error.json"), bad) && !bad.valid);
  CHECK(!parse_forecast("{\"current\":{\"temperature_2m\":\"warm\"}}", bad));
  CHECK(!parse_forecast("<html>502 Bad Gateway</html>", bad));
  CHECK(!parse_forecast("{\"current\":{\"temperature_2m\":900,\"weather_code\":1}}", bad));

  // ------------------------------------------------------- locations
  Location l;
  CHECK(parse_ip_location(fixture("ipapi-co.json"), l));
  CHECK(l.valid && l.city == "Bandung" && l.country == "Indonesia" && near(l.lat, -6.9039) &&
        near(l.lon, 107.6186));
  CHECK(parse_ip_location(fixture("ip-api-com.json"), l));
  CHECK(l.city == "M\xc3\xbcnchen" && l.country == "Germany" && near(l.lat, 48.1374));
  Location none;
  CHECK(!parse_ip_location(fixture("ipapi-co-ratelimited.json"), none) && !none.valid);
  CHECK(!parse_ip_location(fixture("ip-api-com-fail.json"), none));
  CHECK(!parse_ip_location("{\"latitude\":120,\"longitude\":10}", none));
  CHECK(parse_geocoding(fixture("geocoding-berlin.json"), l));
  CHECK(l.city == "Berlin" && l.country == "Germany" && near(l.lon, 13.41053));
  CHECK(!parse_geocoding(fixture("geocoding-none.json"), none));
  double lat = 0, lon = 0;
  CHECK(parse_lat_lon("-6.2, 106.8", lat, lon) && near(lat, -6.2) && near(lon, 106.8));
  CHECK(parse_lat_lon("52.52,13.41", lat, lon) && near(lat, 52.52));
  CHECK(!parse_lat_lon("auto", lat, lon) && !parse_lat_lon("Jakarta", lat, lon));
  CHECK(!parse_lat_lon("91,0", lat, lon) && !parse_lat_lon("1,2,3", lat, lon));
  CHECK(!parse_lat_lon("1x,2", lat, lon) && !parse_lat_lon(",2", lat, lon));

  // ------------------------------------------------------------ URLs
  CHECK(forecast_url(-6.2, 106.8) ==
        "https://api.open-meteo.com/v1/forecast?latitude=-6.2000&longitude=106.8000"
        "&current=temperature_2m,weather_code,is_day,wind_speed_10m&timezone=auto");
  CHECK(geocoding_url(" S\xc3\xa3o Paulo ") ==
        "https://geocoding-api.open-meteo.com/v1/search?count=1&format=json&name=S%C3%A3o%20Paulo");
  CHECK(url_encode("a&b=c") == "a%26b%3Dc");

  // ----------------------------------------------------------- cache
  r.fetched = 1792000000;
  r.location = Location{true, -6.25, 106.75, "Jakarta \"Raya\"", "Indonesia"};
  Report back;
  std::string key;
  CHECK(parse_cache(cache_json(r, "auto"), back, key));
  CHECK(key == "auto" && back.valid && back.code == 63 && back.is_day && near(back.temp_c, 27.4) &&
        back.fetched == 1792000000 && back.location.city == "Jakarta \"Raya\"" &&
        back.location.valid && near(back.location.lat, -6.25));
  CHECK(!parse_cache("{\"version\":2}", back, key) && !parse_cache("garbage", back, key));

  // ----------------------------------------------------------- units
  CHECK(use_fahrenheit("auto", "en_US.UTF-8") && use_fahrenheit("auto", "es_US"));
  CHECK(!use_fahrenheit("auto", "id_ID.UTF-8") && !use_fahrenheit("auto", "en_GB.UTF-8"));
  CHECK(!use_fahrenheit("auto", "C.UTF-8") && !use_fahrenheit("auto", ""));
  CHECK(use_fahrenheit("fahrenheit", "id_ID") && !use_fahrenheit("celsius", "en_US"));
  CHECK(format_temperature(-0.4, false) == "0°C" && format_temperature(27.4, false) == "27°C");
  CHECK(format_temperature(21.6, true) == "71°F" && format_temperature(-40, true) == "-40°F");

  // -------------------------------------------------------- fixtures
  Report f;
  CHECK(fixture_report("rain", f) && from_wmo(f.code).kind == Kind::Rain && f.is_day);
  CHECK(fixture_report("clear-night", f) && from_wmo(f.code).kind == Kind::Clear && !f.is_day);
  CHECK(fixture_report("thunder", f) && fixture_report("snow", f) && fixture_report("fog", f));
  CHECK(!fixture_report("offline", f) && !fixture_report("", f));

  // -------------------------------------------------------- settings
  sde::Config cfg;
  Settings s = Settings::from_config(cfg);
  CHECK(s.enabled && s.location == "auto" && s.animate && s.cache_minutes == 25);
  cfg.set("weather", "location", "-6.2,106.8");
  cfg.set("weather", "unit", "fahrenheit");
  cfg.set("weather", "cache_minutes", "2");
  cfg.set("appearance", "low_resource", "true");
  s = Settings::from_config(cfg);
  CHECK(s.location == "-6.2,106.8");
  CHECK(s.fahrenheit);
  CHECK(s.cache_minutes == 10);
  CHECK(!s.animate);
  cfg.set("weather", "animation", "true");
  CHECK(Settings::from_config(cfg).animate);
  cfg.set("weather", "animation", "false");
  cfg.set("appearance", "low_resource", "false");
  CHECK(!Settings::from_config(cfg).animate);

  // ===================================================== Night Light
  using namespace nightlight;
  Rgb n = whitepoint(6500);
  CHECK(near(n.r, 1) && near(n.g, 1) && near(n.b, 1));
  Rgb w = whitepoint(3000);
  CHECK(near(w.r, 1) && w.g < 0.85 && w.b < w.g && w.b > 0.2);
  Rgb w2 = whitepoint(4500);
  CHECK(w2.b > w.b && w2.g > w.g);
  int m = 0;
  CHECK(parse_time("07:30", m) && m == 450 && parse_time("23:59", m) && m == 1439);
  CHECK(!parse_time("24:00", m) && !parse_time("7", m) && !parse_time("07:30x", m));
  CHECK(format_time(450) == "07:30" && format_time(1440 + 5) == "00:05");
  CHECK(in_window(23 * 60, 20 * 60, 7 * 60) && in_window(60, 20 * 60, 7 * 60));
  CHECK(!in_window(12 * 60, 20 * 60, 7 * 60) && !in_window(7 * 60, 20 * 60, 7 * 60));
  CHECK(in_window(13 * 60, 12 * 60, 14 * 60) && !in_window(5, 5, 5));
  int rise = 0, set = 0;
  // Berlin at midsummer (CEST): about 04:43 and 21:33.
  CHECK(sun_times(2026, 6, 21, 52.52, 13.405, 120, rise, set));
  CHECK(std::abs(rise - (4 * 60 + 43)) <= 6 && std::abs(set - (21 * 60 + 33)) <= 6);
  // Jakarta (WIB, UTC+7) on 22 October: about 05:31 and 17:47.
  CHECK(sun_times(2026, 10, 22, -6.2, 106.8, 420, rise, set));
  CHECK(std::abs(rise - (5 * 60 + 31)) <= 6 && std::abs(set - (17 * 60 + 47)) <= 6);
  // Svalbard: midnight sun.
  CHECK(!sun_times(2026, 6, 21, 78.2, 15.6, 120, rise, set));
  sde::Config nc;
  nightlight::Settings ns = nightlight::Settings::from_config(nc);
  CHECK(!ns.enabled && ns.temperature == 4000 && ns.schedule == "sunset");
  nc.set("night_light", "enabled", "true");
  nc.set("night_light", "schedule", "custom");
  nc.set("night_light", "from", "22:00");
  nc.set("night_light", "to", "06:30");
  nc.set("night_light", "temperature", "1200");
  ns = nightlight::Settings::from_config(nc);
  CHECK(ns.enabled && ns.schedule == "custom" && ns.from == 1320 && ns.to == 390 &&
        ns.temperature == 2500);
  int from = 0, to = 0;
  window_for(ns, time(nullptr), false, 0, 0, from, to);
  CHECK(from == 1320 && to == 390);
  ns.schedule = "sunset";
  window_for(ns, time(nullptr), false, 0, 0, from, to);
  CHECK(from == 19 * 60 && to == 7 * 60);  // no location: the default evening
  ns.schedule = "always";
  CHECK(active(ns, time(nullptr), false, 0, 0));
  ns.enabled = false;
  CHECK(!active(ns, time(nullptr), false, 0, 0));

  if (failures) {
    std::fprintf(stderr, "%d weather/night light checks failed\n", failures);
    return 1;
  }
  std::printf("Weather (WMO codes, JSON, locations, cache, units) and Night Light passed\n");
  return 0;
}
