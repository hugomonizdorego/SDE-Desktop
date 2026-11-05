// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// sde-core: the SDE core controller (0.2.0 Alpha 4, docs/CORE.md).
//
// The hub every SDE program talks to, org.selviana.Core on the session
// bus, started and supervised by sde-session:
//   * Settings: typed, validated reads and writes of sde.conf and a
//     Changed signal with the keys, so programs stop polling files or the
//     X properties of the root window;
//   * Look: the theme family, mode, accent, transparency, the wallpaper and
//     its colors, and LookChanged;
//   * Apps: what SDE started, crashes and hangs (from the session and
//     sde-watchdog), launch requests and single instances;
//   * Session: lock, log out, the session's phase and the workspaces;
//   * Version and Capabilities.
// This program only speaks D-Bus and X11: what it decides (the schema,
// merging writes, what to announce, the instances, the problems, the
// wallpaper's colors) is in the Rust core (rust/sde-core/src/controller.rs).
//
// Programs of 0.2.0 Alpha 3 that write sde.conf themselves and announce it
// on the root window (_SDE_CONFIG_SERIAL) keep working: sde-core reads the
// file again then and announces what changed on D-Bus. Its own writes are
// announced on the root window too, for programs that do not use it yet
// (and for the session, which tells GTK, the window manager and the
// compositor).
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <cairo.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "core_client.h"
#include "dbus.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "integration.h"
#include "launches.h"
#include "rust_core.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "x11.h"

using namespace sde;
using core::Key;

namespace {

const char *const kCapabilities[] = {
    "settings",       "settings.schema", "settings.appearance-switch", "look", "look.wallpaper-colors",
    "apps.instances", "apps.launch",     "apps.problems",              "apps.running",
    "session.lock",   "session.logout",  "session.workspaces",         "session.phase",
};

const char kIntrospection[] = R"XML(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<!-- docs/CORE.md describes every member. -->
<node>
  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect"><arg type="s" direction="out"/></method>
  </interface>
  <interface name="org.selviana.Core">
    <method name="Version"><arg name="version" type="s" direction="out"/><arg name="api" type="u" direction="out"/></method>
    <method name="Capabilities"><arg name="capabilities" type="as" direction="out"/></method>
  </interface>
  <interface name="org.selviana.Core.Settings">
    <method name="Get"><arg name="section" type="s" direction="in"/><arg name="key" type="s" direction="in"/><arg name="value" type="v" direction="out"/></method>
    <method name="GetAll"><arg name="values" type="a{sa{sv}}" direction="out"/></method>
    <method name="Set"><arg name="section" type="s" direction="in"/><arg name="key" type="s" direction="in"/><arg name="value" type="v" direction="in"/></method>
    <method name="Reset"><arg name="section" type="s" direction="in"/><arg name="key" type="s" direction="in"/></method>
    <method name="SetMany"><arg name="values" type="a(ssv)" direction="in"/><arg name="resets" type="a(ss)" direction="in"/><arg name="options" type="a{sv}" direction="in"/><arg name="changed" type="a(ss)" direction="out"/></method>
    <method name="Schema"><arg name="keys" type="a(ssssbss)" direction="out"/></method>
    <signal name="Changed"><arg name="keys" type="a(ss)"/></signal>
  </interface>
  <interface name="org.selviana.Core.Look">
    <method name="Get"><arg name="look" type="a{sv}" direction="out"/></method>
    <signal name="LookChanged"><arg name="look" type="a{sv}"/></signal>
  </interface>
  <interface name="org.selviana.Core.Apps">
    <method name="Running"><arg name="programs" type="a(ussss)" direction="out"/></method>
    <method name="Problems"><arg name="problems" type="a(xussss)" direction="out"/></method>
    <method name="Launch"><arg name="desktop_id" type="s" direction="in"/><arg name="uris" type="as" direction="in"/><arg name="started" type="b" direction="out"/></method>
    <method name="Register"><arg name="app_id" type="s" direction="in"/><arg name="primary" type="b" direction="out"/></method>
    <method name="Unregister"><arg name="app_id" type="s" direction="in"/></method>
    <method name="Activate"><arg name="app_id" type="s" direction="in"/><arg name="args" type="as" direction="in"/><arg name="delivered" type="b" direction="out"/></method>
    <method name="Report"><arg name="kind" type="s" direction="in"/><arg name="pid" type="u" direction="in"/><arg name="name" type="s" direction="in"/><arg name="id" type="s" direction="in"/><arg name="detail" type="s" direction="in"/></method>
    <signal name="Problem"><arg name="time" type="x"/><arg name="pid" type="u"/><arg name="kind" type="s"/><arg name="name" type="s"/><arg name="id" type="s"/><arg name="detail" type="s"/></signal>
  </interface>
  <interface name="org.selviana.Core.Session">
    <method name="Lock"/>
    <method name="Logout"><arg name="confirm" type="b" direction="in"/></method>
    <method name="Info"><arg name="info" type="a{sv}" direction="out"/></method>
    <method name="Workspaces"><arg name="count" type="u" direction="out"/><arg name="names" type="as" direction="out"/><arg name="current" type="u" direction="out"/></method>
    <method name="SetPhase"><arg name="phase" type="s" direction="in"/></method>
    <signal name="PhaseChanged"><arg name="phase" type="s"/></signal>
    <signal name="WorkspacesChanged"><arg name="count" type="u"/><arg name="names" type="as"/><arg name="current" type="u"/></signal>
  </interface>
</node>
)XML";

// A value of the Look dictionary.
struct Var {
  char type = 's';
  bool b = false;
  int32_t i = 0;
  double d = 0;
  std::string s;
  std::vector<std::string> as;
  bool operator==(const Var &o) const {
    return type == o.type && b == o.b && i == o.i && std::fabs(d - o.d) < 1e-9 && s == o.s && as == o.as;
  }
  bool operator!=(const Var &o) const { return !(*this == o); }
};
Var vs(const std::string &v) {
  Var x;
  x.s = v;
  return x;
}
Var vb(bool v) {
  Var x;
  x.type = 'b';
  x.b = v;
  return x;
}
Var vi(int v) {
  Var x;
  x.type = 'i';
  x.i = v;
  return x;
}
Var vd(double v) {
  Var x;
  x.type = 'd';
  x.d = v;
  return x;
}
Var vas(const std::vector<std::string> &v) {
  Var x;
  x.type = 'a';
  x.as = v;
  return x;
}
using Dict = std::map<std::string, Var>;

void put(DBusMessageIter *it, const Var &v) {
  switch (v.type) {
    case 'b': {
      dbus::Writer x(it, DBUS_TYPE_VARIANT, "b");
      x.boolean(v.b);
      break;
    }
    case 'i': {
      dbus::Writer x(it, DBUS_TYPE_VARIANT, "i");
      x.i32(v.i);
      break;
    }
    case 'd': {
      dbus::Writer x(it, DBUS_TYPE_VARIANT, "d");
      x.f64(v.d);
      break;
    }
    case 'a': {
      dbus::Writer x(it, DBUS_TYPE_VARIANT, "as");
      x.strings(v.as);
      break;
    }
    default: {
      dbus::Writer x(it, DBUS_TYPE_VARIANT, "s");
      x.str(v.s);
    }
  }
}

void put_dict(DBusMessageIter *it, const Dict &d) {
  dbus::Writer dict(it, DBUS_TYPE_ARRAY, "{sv}");
  for (const auto &kv : d) {
    dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
    e.str(kv.first);
    put(e.iter(), kv.second);
  }
}

struct WallpaperColors {
  std::string key;  // what they were computed for
  std::vector<std::string> colors;
  bool dark = false;
  bool pending = false;
};

struct State {
  SdeCore *rust = nullptr;
  dbus::Bus *bus = nullptr;
  std::map<std::string, long> pids;  // unique name -> process
  Dict look;
  WallpaperColors wallpaper;
  bool deferred = false;  // a theme switch waits for the session to show it
} g;

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  fprintf(stderr, "sde-core: %s\n", msg);
}

// ------------------------------------------------------------- callers

long caller_pid(DBusMessage *msg) {
  const char *sender = dbus_message_get_sender(msg);
  if (!sender) return 0;
  auto it = g.pids.find(sender);
  if (it != g.pids.end()) return it->second;
  DBusMessage *m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                "GetConnectionUnixProcessID");
  dbus::Writer(m).str(sender);
  DBusMessage *reply = g.bus->call(m, 1000);
  long pid = 0;
  if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    auto args = dbus::read_args(reply);
    if (!args.empty()) pid = long(args[0].i);
  }
  if (reply) dbus_message_unref(reply);
  if (pid > 0) g.pids[sender] = pid;
  return pid;
}

// Programs in a Flatpak or Snap sandbox may read, but not change anything.
bool refused(DBusMessage *msg) {
  if (!dbus::sandboxed_process(caller_pid(msg))) return false;
  g.bus->send(dbus::Bus::error(msg, "org.freedesktop.DBus.Error.AccessDenied",
                               "sandboxed applications cannot change the session"));
  return true;
}

void reply_empty(DBusMessage *msg) { g.bus->send(dbus_message_new_method_return(msg)); }

void reply_error(DBusMessage *msg, const char *name, const std::string &text) {
  g.bus->send(dbus::Bus::error(msg, name, text));
}

// --------------------------------------------------------------- signals

void emit_changed(const std::vector<Key> &keys) {
  if (keys.empty()) return;
  DBusMessage *s = dbus::signal(core::kPath, core::kSettingsIface, "Changed");
  {
    dbus::Writer w(s);
    dbus::Writer list(w.iter(), DBUS_TYPE_ARRAY, "(ss)");
    for (const auto &k : keys) {
      dbus::Writer item(list.iter(), DBUS_TYPE_STRUCT, nullptr);
      item.str(k.first).str(k.second);
    }
  }
  g.bus->send(s);
}

void collect_key(void *ud, const char *section, const char *key) {
  static_cast<std::vector<Key> *>(ud)->push_back({section, key});
}

// ------------------------------------------------------------------ look

std::string wallpaper_path(const Config &cfg, bool dark) {
  // As sde-desktop shows it (src/desktop/main.cpp).
  std::string wp = cfg.get("appearance", "wallpaper", "auto");
  if (wp.empty() || wp == "auto" || !file_exists(wp)) {
    std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
    std::string name = dark ? "selviana-dark.svg" : "selviana-light.svg";
    wp = base + "/backgrounds/sde/" + name;
    if (!file_exists(wp) && file_exists("/usr/share/backgrounds/sde/" + name))
      wp = "/usr/share/backgrounds/sde/" + name;
  }
  return wp;
}

void emit_look() {
  DBusMessage *s = dbus::signal(core::kPath, core::kLookIface, "LookChanged");
  {
    dbus::Writer w(s);
    put_dict(w.iter(), g.look);
  }
  g.bus->send(s);
}

void start_wallpaper_colors(const std::string &path, const std::string &key);

// The look of the configuration on disk; LookChanged when it differs from
// the last one (|announce|).
void update_look(bool announce) {
  Config cfg;
  cfg.load();
  Theme t = Theme::from_config(cfg);
  ThemeChoice c = theme_choice(cfg);
  Dict d;
  d["family"] = vs(c.family);
  d["mode"] = vs(c.mode);
  d["dark"] = vb(t.dark);
  d["accent-name"] = vs(t.accent_name);
  d["accent"] = vs(t.accent.to_hex());
  d["gtk-theme"] = vs(gtk_theme_name(cfg));
  d["icon-theme"] = vs(gtk_icon_theme_name(cfg, t.dark));
  d["translucent"] = vb(t.translucent);
  d["transparency"] = vi(t.transparency);
  d["blur"] = vb(t.blur);
  d["blur-strength"] = vi(cfg.get_int("aero", "blur_strength", 6));
  d["rounded-corners"] = vb(cfg.get_bool("aero", "rounded_corners", true));
  d["animations"] = vb(cfg.get_bool("aero", "animations", true));
  d["low-resource"] = vb(cfg.get_bool("appearance", "low_resource", false));
  // The compositor's lite profile and Liquid Glass change how surfaces are
  // drawn (Theme): also a new look.
  d["lite"] = vb(t.lite);
  d["liquid-glass"] = vb(t.liquid);
  d["scale"] = vd(t.scale);
  d["font"] = vs(t.font);
  d["font-size"] = vi(cfg.get_int("appearance", "font_size", 10));
  std::string wp = wallpaper_path(cfg, t.dark), mode = cfg.get("appearance", "wallpaper_mode", "zoom");
  d["wallpaper"] = vs(wp);
  d["wallpaper-mode"] = vs(mode);
  std::string key = wp + "\n" + mode;
  if (g.wallpaper.key != key && !g.wallpaper.pending) start_wallpaper_colors(wp, key);
  d["wallpaper-colors"] = vas(g.wallpaper.key == key ? g.wallpaper.colors : std::vector<std::string>());
  d["wallpaper-dark"] = vb(g.wallpaper.key == key ? g.wallpaper.dark : t.dark);
  if (d == g.look) return;
  g.look = d;
  if (announce) emit_look();
}

// The wallpaper's colors are found on a thread (a large photo takes a
// moment to decode); LookChanged follows when they differ.
struct ColorJob {
  std::string path, key;
  std::vector<std::string> colors;
  bool dark = false;
};

void colors_ready(void *data) {
  std::unique_ptr<ColorJob> job(static_cast<ColorJob *>(data));
  g.wallpaper.pending = false;
  g.wallpaper.key = job->key;
  g.wallpaper.colors = job->colors;
  g.wallpaper.dark = job->dark;
  update_look(true);
}

void start_wallpaper_colors(const std::string &path, const std::string &key) {
  g.wallpaper.pending = true;
  auto *job = new ColorJob{path, key, {}, false};
  std::thread([job] {
    cairo_surface_t *s = load_wallpaper(job->path, 96, 54, "zoom", 0.5, 0.5, 0.5);
    cairo_format_t format = s ? cairo_image_surface_get_format(s) : CAIRO_FORMAT_INVALID;
    if (format == CAIRO_FORMAT_ARGB32 || format == CAIRO_FORMAT_RGB24) {
      cairo_surface_flush(s);
      int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
      int stride = cairo_image_surface_get_stride(s);
      const unsigned char *data = cairo_image_surface_get_data(s);
      // RGB24 leaves the alpha byte undefined: those pictures are opaque.
      uint32_t opaque = format == CAIRO_FORMAT_RGB24 ? 0xFF000000u : 0;
      std::vector<uint32_t> px;
      px.reserve(size_t(w) * size_t(h));
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) px.push_back(reinterpret_cast<const uint32_t *>(data + y * stride)[x] | opaque);
      uint32_t average = 0, colors[5];
      size_t n = sde_core_palette(px.data(), px.size(), &average, colors, 5);
      for (size_t i = 0; i < n; ++i) job->colors.push_back(Color::hex(colors[i]).to_hex());
      job->dark = n > 0 && sde_core_luminance(average) < 0.35;
    }
    if (s) cairo_surface_destroy(s);
    Fl::awake(colors_ready, job);
  }).detach();
}

// -------------------------------------------------------------- settings

bool variant_text(const dbus::Value &v, std::string &out) {
  const dbus::Value &x = v.inner();
  switch (x.type) {
    case 'b':
      out = x.b ? "true" : "false";
      return true;
    case 'y': case 'n': case 'q': case 'i': case 'u': case 'x': case 't':
      out = std::to_string(x.i);
      return true;
    case 'd': {
      if (std::fabs(x.d - std::round(x.d)) < 1e-9 && std::fabs(x.d) < 1e15) {
        out = std::to_string(static_cast<long long>(std::llround(x.d)));
      } else {
        char buf[64];
        snprintf(buf, sizeof buf, "%g", x.d);
        out = buf;
      }
      return true;
    }
    case 's':
      out = x.s;
      return true;
    default:
      return false;
  }
}

// Tells the programs that do not use sde-core yet, and the session (which
// tells GTK, the window manager and the compositor what changed).
void announce_on_root() {
  if (!fl_display) return;
  Config cfg;
  cfg.load();
  if (session_coordinates(fl_display)) x11::notify_config_changed();
  else announce_config_change(fl_display, cfg);
  XFlush(fl_display);
}

void deferred_timeout(void *) {
  // The session did not show the new look in time: announce it anyway.
  if (!g.deferred) return;
  g.deferred = false;
  std::vector<Key> keys;
  sde_core_announce(g.rust, collect_key, &keys);
  emit_changed(keys);
  update_look(true);
}

// Writes what was staged. |appearance|: a theme switch, announced when the
// session shows it. Replies to |msg| (with the changed keys for SetMany).
void commit(DBusMessage *msg, bool appearance, bool with_keys) {
  std::vector<Key> keys;
  char *error = nullptr;
  int r = sde_core_commit(g.rust, !appearance, collect_key, &keys, &error);
  std::string why = error ? error : "";
  sde_config_string_free(error);
  if (r == 1 || r == 2) {
    reply_error(msg, r == 1 ? "org.selviana.Core.Error.InvalidKey" : "org.selviana.Core.Error.InvalidValue", why);
    return;
  }
  if (r != 0) {
    reply_error(msg, "org.selviana.Core.Error.Failed", "cannot save " + Config::user_path() + ": " + why);
    return;
  }
  DBusMessage *reply = dbus_message_new_method_return(msg);
  if (with_keys) {
    dbus::Writer w(reply);
    dbus::Writer list(w.iter(), DBUS_TYPE_ARRAY, "(ss)");
    for (const auto &k : keys) {
      dbus::Writer item(list.iter(), DBUS_TYPE_STRUCT, nullptr);
      item.str(k.first).str(k.second);
    }
  }
  g.bus->send(reply);
  if (keys.empty()) return;
  if (appearance) {
    // The session switches every program in one calm step behind a
    // picture of the screen and announces it (_SDE_CONFIG_SERIAL), which
    // is when Changed and LookChanged go out (x_event()).
    Config cfg;
    cfg.load();
    if (fl_display && request_appearance_apply(fl_display)) {
      g.deferred = true;
      Fl::remove_timeout(deferred_timeout);
      Fl::add_timeout(3.0, deferred_timeout);
      return;
    }
    if (fl_display) apply_appearance_now(fl_display, cfg);
    std::vector<Key> now;
    sde_core_announce(g.rust, collect_key, &now);
    emit_changed(now);
    update_look(true);
    return;
  }
  emit_changed(keys);
  update_look(true);
  announce_on_root();
}

void get_value(DBusMessage *msg) {
  auto args = dbus::read_args(msg);
  const std::string &section = args[0].s, &key = args[1].s;
  char *value = sde_core_get(g.rust, section.c_str(), key.c_str());
  if (!value) {
    reply_error(msg, "org.selviana.Core.Error.UnknownKey", "[" + section + "] " + key + " is not set");
    return;
  }
  std::string text = value;
  sde_config_string_free(value);
  char type = sde_core_key_type(g.rust, section.c_str(), key.c_str());
  Var v = vs(text);
  if (type == 'b') {
    std::string l = lower(text);
    v = vb(l == "true" || l == "yes" || l == "1" || l == "on");
  } else if (type == 'i') {
    char *end = nullptr;
    long n = strtol(text.c_str(), &end, 10);
    // A value out of the schema's range (written by hand) is given as text.
    if (end && !*end && !text.empty() && n >= INT32_MIN && n <= INT32_MAX) v = vi(int(n));
  }
  DBusMessage *reply = dbus_message_new_method_return(msg);
  {
    dbus::Writer w(reply);
    put(w.iter(), v);
  }
  g.bus->send(reply);
}

void get_all(DBusMessage *msg) {
  struct Item {
    std::string section, key, value;
  };
  std::vector<Item> items;
  sde_core_values(g.rust, [](void *ud, const char *s, const char *k, const char *v) {
    static_cast<std::vector<Item> *>(ud)->push_back({s, k, v});
  }, &items);
  DBusMessage *reply = dbus_message_new_method_return(msg);
  {
    dbus::Writer w(reply);
    dbus::Writer sections(w.iter(), DBUS_TYPE_ARRAY, "{sa{sv}}");
    size_t i = 0;
    while (i < items.size()) {
      dbus::Writer entry(sections.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      entry.str(items[i].section);
      dbus::Writer keys(entry.iter(), DBUS_TYPE_ARRAY, "{sv}");
      std::string section = items[i].section;
      for (; i < items.size() && items[i].section == section; ++i) {
        dbus::Writer e(keys.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str(items[i].key);
        char type = sde_core_key_type(g.rust, section.c_str(), items[i].key.c_str());
        std::string l = lower(items[i].value);
        char *end = nullptr;
        long n = strtol(items[i].value.c_str(), &end, 10);
        if (type == 'b')
          put(e.iter(), vb(l == "true" || l == "yes" || l == "1" || l == "on"));
        else if (type == 'i' && end && !*end && !items[i].value.empty() && n >= INT32_MIN && n <= INT32_MAX)
          put(e.iter(), vi(int(n)));
        else
          put(e.iter(), vs(items[i].value));
      }
    }
  }
  g.bus->send(reply);
}

void schema(DBusMessage *msg) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  {
    dbus::Writer w(reply);
    dbus::Writer list(w.iter(), DBUS_TYPE_ARRAY, "(ssssbss)");
    sde_core_schema(g.rust,
                    [](void *ud, const char *s, const char *k, const char *type, const char *values,
                       const char *def, const char *page, char) {
                      auto *l = static_cast<dbus::Writer *>(ud);
                      dbus::Writer item(l->iter(), DBUS_TYPE_STRUCT, nullptr);
                      item.str(s).str(k).str(type).str(values).boolean(def != nullptr).str(def ? def : "").str(page);
                    },
                    &list);
  }
  g.bus->send(reply);
}

DBusHandlerResult settings_method(DBusMessage *msg, const std::string &member) {
  if (member == "Get" && dbus_message_has_signature(msg, "ss")) {
    get_value(msg);
  } else if (member == "GetAll" && dbus_message_has_signature(msg, "")) {
    get_all(msg);
  } else if (member == "Schema" && dbus_message_has_signature(msg, "")) {
    schema(msg);
  } else if (member == "Set" && dbus_message_has_signature(msg, "ssv")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    std::string text;
    if (!variant_text(args[2], text)) {
      reply_error(msg, "org.selviana.Core.Error.InvalidValue", "a value is a boolean, a number or a string");
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    sde_core_stage_clear(g.rust);
    sde_core_stage(g.rust, args[0].s.c_str(), args[1].s.c_str(), text.c_str());
    commit(msg, false, false);
  } else if (member == "Reset" && dbus_message_has_signature(msg, "ss")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    sde_core_stage_clear(g.rust);
    sde_core_stage(g.rust, args[0].s.c_str(), args[1].s.c_str(), nullptr);
    commit(msg, false, false);
  } else if (member == "SetMany" && dbus_message_has_signature(msg, "a(ssv)a(ss)a{sv}")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    sde_core_stage_clear(g.rust);
    for (const auto &item : args[0].items) {
      std::string text;
      if (item.items.size() != 3 || !variant_text(item.items[2], text)) {
        reply_error(msg, "org.selviana.Core.Error.InvalidValue", "a value is a boolean, a number or a string");
        return DBUS_HANDLER_RESULT_HANDLED;
      }
      sde_core_stage(g.rust, item.items[0].s.c_str(), item.items[1].s.c_str(), text.c_str());
    }
    for (const auto &item : args[1].items)
      if (item.items.size() == 2) sde_core_stage(g.rust, item.items[0].s.c_str(), item.items[1].s.c_str(), nullptr);
    const dbus::Value *appearance = args[2].get("appearance");
    commit(msg, appearance && appearance->boolean(), true);
  } else {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  return DBUS_HANDLER_RESULT_HANDLED;
}

// ------------------------------------------------------------------ apps

void emit_problem(void *, int64_t time, uint32_t pid, const char *kind, const char *name, const char *id,
                  const char *detail) {
  DBusMessage *s = dbus::signal(core::kPath, core::kAppsIface, "Problem");
  {
    dbus::Writer w(s);
    dbus_int64_t t = time;
    dbus_message_iter_append_basic(w.iter(), DBUS_TYPE_INT64, &t);
    w.u32(pid).str(kind).str(name).str(id).str(detail);
  }
  g.bus->send(s);
  log("%s: %s (%u) %s", kind, name, unsigned(pid), detail);
}

void running(DBusMessage *msg) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  {
    dbus::Writer w(reply);
    dbus::Writer list(w.iter(), DBUS_TYPE_ARRAY, "(ussss)");
    sde_core_components(g.rust,
                        [](void *ud, const char *name, uint32_t pid, const char *state, uint32_t) {
                          auto *l = static_cast<dbus::Writer *>(ud);
                          dbus::Writer item(l->iter(), DBUS_TYPE_STRUCT, nullptr);
                          item.u32(pid).str("component").str(name).str(name).str(state);
                        },
                        &list);
    for (const auto &r : launches::all()) {
      if (!launches::alive(r)) continue;
      dbus::Writer item(list.iter(), DBUS_TYPE_STRUCT, nullptr);
      item.u32(uint32_t(r.pid)).str("application").str(r.name).str(r.desktop_id)
          .str(sde_core_app_state(g.rust, uint32_t(r.pid)));
    }
  }
  g.bus->send(reply);
}

void problems(DBusMessage *msg) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  {
    dbus::Writer w(reply);
    dbus::Writer list(w.iter(), DBUS_TYPE_ARRAY, "(xussss)");
    sde_core_problems(g.rust,
                      [](void *ud, int64_t time, uint32_t pid, const char *kind, const char *name, const char *id,
                         const char *detail) {
                        auto *l = static_cast<dbus::Writer *>(ud);
                        dbus::Writer item(l->iter(), DBUS_TYPE_STRUCT, nullptr);
                        dbus_int64_t t = time;
                        dbus_message_iter_append_basic(item.iter(), DBUS_TYPE_INT64, &t);
                        item.u32(pid).str(kind).str(name).str(id).str(detail);
                      },
                      &list);
  }
  g.bus->send(reply);
}

void activate(DBusMessage *msg) {
  auto args = dbus::read_args(msg);
  std::string app_id = args[0].s;
  const char *sender = dbus_message_get_sender(msg);
  char *owner = sde_core_primary(g.rust, app_id.c_str(), nullptr);
  std::string primary = owner ? owner : "";
  sde_config_string_free(owner);
  if (primary.empty() || (sender && primary == sender)) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus::Writer(reply).boolean(false);
    g.bus->send(reply);
    return;
  }
  std::vector<std::string> list;
  for (const auto &v : args[1].items) list.push_back(v.s);
  DBusMessage *m = dbus::method(primary, core::kInstancePath, core::kInstanceIface, "Activate");
  dbus::Writer(m).str(app_id).strings(list);
  dbus_message_ref(msg);
  g.bus->call_async_any(m, [msg, app_id, primary](DBusMessage *answer) {
    bool ok = answer && dbus_message_get_type(answer) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
    // An instance that does not answer (it hangs, or it is ending) is not
    // the instance any more: the new start becomes it.
    if (!ok) {
      sde_core_unregister(g.rust, app_id.c_str(), primary.c_str());
      log("the instance of %s did not answer; letting a new one start", app_id.c_str());
    }
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus::Writer(reply).boolean(ok);
    g.bus->send(reply);
    dbus_message_unref(msg);
  }, 3000);
}

DBusHandlerResult apps_method(DBusMessage *msg, const std::string &member) {
  if (member == "Running" && dbus_message_has_signature(msg, "")) {
    running(msg);
  } else if (member == "Problems" && dbus_message_has_signature(msg, "")) {
    problems(msg);
  } else if (member == "Register" && dbus_message_has_signature(msg, "s")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    const char *sender = dbus_message_get_sender(msg);
    bool primary = sender && sde_core_register(g.rust, args[0].s.c_str(), sender, uint32_t(caller_pid(msg)));
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus::Writer(reply).boolean(primary);
    g.bus->send(reply);
  } else if (member == "Unregister" && dbus_message_has_signature(msg, "s")) {
    auto args = dbus::read_args(msg);
    const char *sender = dbus_message_get_sender(msg);
    if (sender) sde_core_unregister(g.rust, args[0].s.c_str(), sender);
    reply_empty(msg);
  } else if (member == "Activate" && dbus_message_has_signature(msg, "sas")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    activate(msg);
  } else if (member == "Launch" && dbus_message_has_signature(msg, "sas")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    std::vector<std::string> uris;
    for (const auto &v : args[1].items) uris.push_back(v.s);
    DesktopEntry e;
    bool started = find_application(args[0].s, e) && launch_entry(e, uris);
    if (!started) log("cannot launch %s", args[0].s.c_str());
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus::Writer(reply).boolean(started);
    g.bus->send(reply);
  } else if (member == "Report" && dbus_message_has_signature(msg, "susss")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    int r = sde_core_report(g.rust, int64_t(time(nullptr)), args[0].s.c_str(), uint32_t(args[1].i), args[2].s.c_str(),
                            args[3].s.c_str(), args[4].s.c_str(), emit_problem, nullptr);
    if (r < 0) reply_error(msg, "org.freedesktop.DBus.Error.InvalidArgs", "unknown report " + args[0].s);
    else if (!dbus_message_get_no_reply(msg)) reply_empty(msg);
  } else {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  return DBUS_HANDLER_RESULT_HANDLED;
}

// --------------------------------------------------------------- session

long session_pid() {
  std::string text;
  if (!read_file(runtime_dir() + "/sde-session-" + env("DISPLAY") + ".pid", text)) return 0;
  return atol(text.c_str());
}

void emit_workspaces() {
  std::vector<std::string> names;
  uint32_t current = 0;
  uint32_t count = sde_core_workspaces(g.rust, &current, [](void *ud, const char *name) {
    static_cast<std::vector<std::string> *>(ud)->push_back(name);
  }, &names);
  DBusMessage *s = dbus::signal(core::kPath, core::kSessionIface, "WorkspacesChanged");
  dbus::Writer(s).u32(count).strings(names).u32(current);
  g.bus->send(s);
}

// The window manager's workspaces (EWMH properties of the root window).
void read_workspaces(bool announce) {
  if (!fl_display) return;
  Window root = RootWindow(fl_display, fl_screen);
  auto count = x11::get_cardinals(root, "_NET_NUMBER_OF_DESKTOPS");
  auto current = x11::get_cardinals(root, "_NET_CURRENT_DESKTOP");
  std::string text = x11::get_utf8(root, "_NET_DESKTOP_NAMES");
  std::vector<std::string> names;
  std::string cur;
  for (char c : text) {
    if (c) {
      cur += c;
    } else {
      names.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) names.push_back(cur);
  std::vector<const char *> ptrs;
  for (const auto &n : names) ptrs.push_back(n.c_str());
  if (sde_core_set_workspaces(g.rust, count.empty() ? 0 : uint32_t(count[0]), ptrs.data(), ptrs.size(),
                              current.empty() ? 0 : uint32_t(current[0])) &&
      announce)
    emit_workspaces();
}

DBusHandlerResult session_method(DBusMessage *msg, const std::string &member) {
  if (member == "Lock" && dbus_message_has_signature(msg, "")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    if (spawn_detached({"sde-lock", "--lock"})) reply_empty(msg);
    else reply_error(msg, "org.selviana.Core.Error.Failed", "sde-lock is not installed");
  } else if (member == "Logout" && dbus_message_has_signature(msg, "b")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    // Asking first is the Leave dialog; otherwise as "sde-session --logout".
    std::vector<std::string> argv = args[0].b ? std::vector<std::string>{"sde-logout"}
                                              : std::vector<std::string>{"sde-session", "--logout"};
    if (spawn_detached(argv)) reply_empty(msg);
    else reply_error(msg, "org.selviana.Core.Error.Failed", argv[0] + " is not installed");
  } else if (member == "Info" && dbus_message_has_signature(msg, "")) {
    char *phase = sde_core_phase(g.rust);
    Dict d;
    d["phase"] = vs(phase ? phase : "");
    sde_config_string_free(phase);
    d["display"] = vs(env("DISPLAY"));
    d["version"] = vs(SDE_VERSION);
    d["session-pid"] = vi(int(session_pid()));
    DBusMessage *reply = dbus_message_new_method_return(msg);
    {
      dbus::Writer w(reply);
      put_dict(w.iter(), d);
    }
    g.bus->send(reply);
  } else if (member == "Workspaces" && dbus_message_has_signature(msg, "")) {
    std::vector<std::string> names;
    uint32_t current = 0;
    uint32_t count = sde_core_workspaces(g.rust, &current, [](void *ud, const char *name) {
      static_cast<std::vector<std::string> *>(ud)->push_back(name);
    }, &names);
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus::Writer(reply).u32(count).strings(names).u32(current);
    g.bus->send(reply);
  } else if (member == "SetPhase" && dbus_message_has_signature(msg, "s")) {
    if (refused(msg)) return DBUS_HANDLER_RESULT_HANDLED;
    auto args = dbus::read_args(msg);
    int r = sde_core_set_phase(g.rust, args[0].s.c_str());
    if (r < 0) {
      reply_error(msg, "org.freedesktop.DBus.Error.InvalidArgs", "the phase is starting, running or ending");
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!dbus_message_get_no_reply(msg)) reply_empty(msg);
    if (r > 0) {
      DBusMessage *s = dbus::signal(core::kPath, core::kSessionIface, "PhaseChanged");
      dbus::Writer(s).str(args[0].s);
      g.bus->send(s);
    }
  } else {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  return DBUS_HANDLER_RESULT_HANDLED;
}

// --------------------------------------------------------------- dispatch

DBusHandlerResult on_method(DBusMessage *msg) {
  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *i = dbus_message_get_interface(msg), *m = dbus_message_get_member(msg);
  std::string iface = i ? i : "", member = m ? m : "";
  DBusHandlerResult r = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  if (iface == "org.freedesktop.DBus.Introspectable" && member == "Introspect") {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus::Writer(reply).str(kIntrospection);
    g.bus->send(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (iface == core::kIface) {
    if (member == "Version" && dbus_message_has_signature(msg, "")) {
      DBusMessage *reply = dbus_message_new_method_return(msg);
      dbus::Writer(reply).str(SDE_VERSION).u32(core::kApiVersion);
      g.bus->send(reply);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (member == "Capabilities" && dbus_message_has_signature(msg, "")) {
      DBusMessage *reply = dbus_message_new_method_return(msg);
      dbus::Writer(reply).strings(std::vector<std::string>(std::begin(kCapabilities), std::end(kCapabilities)));
      g.bus->send(reply);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  } else if (iface == core::kSettingsIface) {
    r = settings_method(msg, member);
  } else if (iface == core::kLookIface) {
    if (member == "Get" && dbus_message_has_signature(msg, "")) {
      DBusMessage *reply = dbus_message_new_method_return(msg);
      {
        dbus::Writer w(reply);
        put_dict(w.iter(), g.look);
      }
      g.bus->send(reply);
      r = DBUS_HANDLER_RESULT_HANDLED;
    }
  } else if (iface == core::kAppsIface) {
    r = apps_method(msg, member);
  } else if (iface == core::kSessionIface) {
    r = session_method(msg, member);
  }
  if (r == DBUS_HANDLER_RESULT_NOT_YET_HANDLED) {
    reply_error(msg, "org.freedesktop.DBus.Error.UnknownMethod",
                "no method " + iface + "." + member + " with signature " +
                    (dbus_message_get_signature(msg) ? dbus_message_get_signature(msg) : ""));
  }
  return DBUS_HANDLER_RESULT_HANDLED;
}

// The root window: settings announced by other programs, workspaces.
int x_event(void *event, void *) {
  const XEvent *e = static_cast<const XEvent *>(event);
  if (!fl_display || e->type != PropertyNotify || e->xproperty.window != RootWindow(fl_display, fl_screen))
    return 0;
  Atom a = e->xproperty.atom;
  if (a == x11::atom("_SDE_CONFIG_SERIAL")) {
    // A program wrote sde.conf, or the session shows a theme switch.
    g.deferred = false;
    std::vector<Key> keys;
    sde_core_reload(g.rust, collect_key, &keys);
    emit_changed(keys);
    update_look(true);
  } else if (a == x11::atom("_NET_NUMBER_OF_DESKTOPS") || a == x11::atom("_NET_DESKTOP_NAMES") ||
             a == x11::atom("_NET_CURRENT_DESKTOP")) {
    read_workspaces(true);
  }
  return 0;
}

void usage() {
  printf("%s", _("Usage: sde-core [--replace | --version | --help]\n\n"
                 "The SDE core controller: settings, the look, applications and the session\n"
                 "for every SDE program, as org.selviana.Core on the session bus. Started by\n"
                 "sde-session.\n\n"
                 "  --replace  take over from an sde-core that runs already\n"
                 "  --version  print the version and exit\n"));
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  bool replace = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") return print_version("sde-core", SDE_VERSION);
    if (a == "--replace") {
      replace = true;
      continue;
    }
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    fprintf(stderr, "sde-core: %s\n", format(_("unknown option %s"), a.c_str()).c_str());
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  // X11 is optional: without it there are no workspaces and no
  // announcements on the root window.
  if (!env("DISPLAY").empty())
    if (Display *d = XOpenDisplay(nullptr)) {
      fl_display = d;  // FLTK adopts it (and never exits for a missing display)
      fl_open_display();
    }
  if (env("DBUS_SESSION_BUS_ADDRESS").empty() && !file_exists(runtime_dir() + "/bus")) {
    fprintf(stderr, "sde-core: %s\n", _("no session bus (DBUS_SESSION_BUS_ADDRESS is not set)"));
    return 1;
  }
  g.bus = dbus::Bus::session();
  if (!g.bus) return 1;
  std::string dirs;
  for (const auto &dir : config_dirs()) dirs += (dirs.empty() ? "" : ":") + dir;
  std::vector<Key> migrated;
  g.rust = sde_core_open(Config::user_path().c_str(), dirs.c_str(), collect_key, &migrated);
  for (const auto &k : migrated) log("merged the old key [%s] %s", k.first.c_str(), k.second.c_str());

  g.bus->export_object(core::kPath, on_method);
  // Instances whose program ended; cached processes of callers that left.
  g.bus->add_signal_handler(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
      [](DBusMessage *m) { return dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged"); },
      [](DBusMessage *m) {
        auto args = dbus::read_args(m);
        if (args.size() != 3 || !args[2].s.empty() || args[0].s.empty() || args[0].s[0] != ':') return;
        g.pids.erase(args[0].s);
        sde_core_owner_gone(g.rust, args[0].s.c_str(), nullptr, nullptr);
      });
  // Another sde-core took the name (--replace): this one ends.
  g.bus->add_signal_handler(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameLost'",
      [](DBusMessage *m) { return dbus_message_is_signal(m, "org.freedesktop.DBus", "NameLost"); },
      [](DBusMessage *m) {
        auto args = dbus::read_args(m);
        if (!args.empty() && args[0].s == core::kName) {
          log("replaced by another sde-core");
          exit(0);
        }
      });
  DBusError err;
  dbus_error_init(&err);
  unsigned flags = DBUS_NAME_FLAG_DO_NOT_QUEUE | DBUS_NAME_FLAG_ALLOW_REPLACEMENT;
  if (replace) flags |= DBUS_NAME_FLAG_REPLACE_EXISTING;
  int owned = dbus_bus_request_name(g.bus->conn(), core::kName, flags, &err);
  if (dbus_error_is_set(&err)) {
    log("cannot own %s: %s", core::kName, err.message);
    dbus_error_free(&err);
    return 1;
  }
  if (owned != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER && owned != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
    // Not an error: the session's sde-core runs (status 0: not restarted).
    log("another sde-core runs on this session bus (sde-core --replace takes over)");
    return 0;
  }
  if (fl_display) {
    Window root = RootWindow(fl_display, fl_screen);
    XWindowAttributes attr;
    XGetWindowAttributes(fl_display, root, &attr);
    XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
    Fl::add_system_handler(x_event, nullptr);
    read_workspaces(false);
  }
  // Wallpaper colors come back from their thread with Fl::awake().
  Fl::lock();
  update_look(false);
  log("serving %s (SDE %s, API %u)", core::kName, SDE_VERSION, core::kApiVersion);
  for (;;) Fl::wait(1e20);
}
