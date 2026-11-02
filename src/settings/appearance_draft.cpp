// SPDX-License-Identifier: GPL-3.0-or-later
#include "appearance_draft.h"

#include <algorithm>
#include <cstdlib>

#include "config.h"
#include "theme.h"
#include "util.h"

namespace sde {

namespace {

bool known_family(const std::string &id) {
  for (const auto &f : theme_families())
    if (f.id == id) return true;
  return false;
}

bool known_accent(const std::string &id) {
  auto all = theme_accents();
  return std::find(all.begin(), all.end(), id) != all.end();
}

int clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

void set_if_changed(Config &cfg, const char *section, const char *key, const std::string &value) {
  if (cfg.get(section, key, "\x01") != value) cfg.set(section, key, value);
}

const char *flag(bool on) { return on ? "true" : "false"; }

}  // namespace

AppearanceLook AppearanceLook::from_config(const Config &cfg) {
  AppearanceLook l;
  ThemeChoice c = theme_choice(cfg);
  l.family = c.family;
  l.mode = c.mode;
  l.accent = c.accent;
  l.custom_accent = c.custom_accent;
  l.translucent = cfg.get_bool("aero", "enabled", true);
  l.transparency = clamp(cfg.get_int("aero", "transparency", 25), 0, 100);
  l.blur = cfg.get_bool("aero", "blur", true);
  l.blur_strength = clamp(cfg.get_int("aero", "blur_strength", 6), 1, 12);
  l.rounded_corners = cfg.get_bool("aero", "rounded_corners", true);
  l.animations = cfg.get_bool("aero", "animations", true);
  return l;
}

void AppearanceLook::write(Config &cfg) const {
  set_if_changed(cfg, "appearance", "style", family);
  set_if_changed(cfg, "appearance", "theme", mode);
  set_if_changed(cfg, "appearance", "accent_name", accent);
  set_if_changed(cfg, "appearance", "accent",
                 custom_accent.empty() ? theme_accent_color(family, accent, false).to_hex() : custom_accent);
  set_if_changed(cfg, "aero", "enabled", flag(translucent));
  set_if_changed(cfg, "aero", "transparency", std::to_string(transparency));
  set_if_changed(cfg, "aero", "blur", flag(blur));
  set_if_changed(cfg, "aero", "blur_strength", std::to_string(blur_strength));
  set_if_changed(cfg, "aero", "rounded_corners", flag(rounded_corners));
  set_if_changed(cfg, "aero", "animations", flag(animations));
}

std::string AppearanceLook::encode() const {
  return "family=" + family + ";mode=" + mode + ";accent=" + accent + ";custom=" + custom_accent +
         ";translucent=" + (translucent ? "1" : "0") + ";transparency=" + std::to_string(transparency) +
         ";blur=" + (blur ? "1" : "0") + ";strength=" + std::to_string(blur_strength) +
         ";corners=" + (rounded_corners ? "1" : "0") + ";animations=" + (animations ? "1" : "0");
}

bool AppearanceLook::decode(const std::string &text, AppearanceLook &out) {
  AppearanceLook l;
  bool family = false;
  for (const auto &part : split(text, ';')) {
    size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(part.substr(0, eq)), v = trim(part.substr(eq + 1));
    if (k == "family" && known_family(v)) {
      l.family = v;
      family = true;
    } else if (k == "mode" && (v == "light" || v == "dark" || v == "auto")) {
      l.mode = v;
    } else if (k == "accent" && known_accent(v)) {
      l.accent = v;
    } else if (k == "custom") {
      Color c;
      l.custom_accent = Color::parse(v, c) ? c.to_hex() : "";
    } else if (k == "translucent") {
      l.translucent = v == "1";
    } else if (k == "transparency") {
      l.transparency = clamp(atoi(v.c_str()), 0, 100);
    } else if (k == "blur") {
      l.blur = v == "1";
    } else if (k == "strength") {
      l.blur_strength = clamp(atoi(v.c_str()), 1, 12);
    } else if (k == "corners") {
      l.rounded_corners = v == "1";
    } else if (k == "animations") {
      l.animations = v == "1";
    }
  }
  if (!family) return false;
  out = l;
  return true;
}

bool AppearanceLook::operator==(const AppearanceLook &o) const {
  return family == o.family && mode == o.mode && accent == o.accent && custom_accent == o.custom_accent &&
         translucent == o.translucent && transparency == o.transparency && blur == o.blur &&
         blur_strength == o.blur_strength && rounded_corners == o.rounded_corners && animations == o.animations;
}

std::vector<std::string> look_changes(const AppearanceLook &a, const AppearanceLook &b) {
  std::vector<std::string> out;
  if (a.family != b.family) out.push_back("family");
  if (a.mode != b.mode) out.push_back("mode");
  if (a.accent != b.accent || a.custom_accent != b.custom_accent) out.push_back("accent");
  if (a.translucent != b.translucent || a.transparency != b.transparency) out.push_back("transparency");
  if (a.blur != b.blur || a.blur_strength != b.blur_strength) out.push_back("blur");
  if (a.rounded_corners != b.rounded_corners) out.push_back("corners");
  if (a.animations != b.animations) out.push_back("animations");
  return out;
}

void AppearanceDraft::load(const Config &cfg) {
  applied_ = AppearanceLook::from_config(cfg);
  pending_ = applied_;
}

void AppearanceDraft::set_family(const std::string &family) {
  if (known_family(family)) pending_.family = family;
}

void AppearanceDraft::set_mode(const std::string &mode) {
  if (mode == "light" || mode == "dark" || mode == "auto") pending_.mode = mode;
}

void AppearanceDraft::set_accent(const std::string &accent) {
  if (!known_accent(accent)) return;
  pending_.accent = accent;
  pending_.custom_accent.clear();
}

void AppearanceDraft::set_preset(int transparency) {
  pending_.translucent = true;
  pending_.transparency = clamp(transparency, 0, 100);
}

void AppearanceDraft::set_transparency(int transparency) {
  pending_.transparency = clamp(transparency, 0, 100);
  if (pending_.transparency > 0) pending_.translucent = true;
}

void AppearanceDraft::set_translucent(bool on) { pending_.translucent = on; }
void AppearanceDraft::set_blur(bool on) { pending_.blur = on; }
void AppearanceDraft::set_blur_strength(int strength) { pending_.blur_strength = clamp(strength, 1, 12); }
void AppearanceDraft::set_rounded_corners(bool on) { pending_.rounded_corners = on; }
void AppearanceDraft::set_animations(bool on) { pending_.animations = on; }

Config AppearanceDraft::preview_config(const Config &base) const {
  Config c = base;
  pending_.write(c);
  return c;
}

void AppearanceDraft::apply(Config &cfg) {
  pending_.write(cfg);
  applied_ = pending_;
}

void AppearanceDraft::rebase(const Config &cfg) {
  AppearanceLook now = AppearanceLook::from_config(cfg);
  AppearanceLook &p = pending_;
  const AppearanceLook &a = applied_;
  if (p.family == a.family) p.family = now.family;
  if (p.mode == a.mode) p.mode = now.mode;
  if (p.accent == a.accent && p.custom_accent == a.custom_accent) {
    p.accent = now.accent;
    p.custom_accent = now.custom_accent;
  }
  if (p.translucent == a.translucent && p.transparency == a.transparency) {
    p.translucent = now.translucent;
    p.transparency = now.transparency;
  }
  if (p.blur == a.blur && p.blur_strength == a.blur_strength) {
    p.blur = now.blur;
    p.blur_strength = now.blur_strength;
  }
  if (p.rounded_corners == a.rounded_corners) p.rounded_corners = now.rounded_corners;
  if (p.animations == a.animations) p.animations = now.animations;
  applied_ = now;
}

}  // namespace sde
