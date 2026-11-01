// SPDX-License-Identifier: GPL-3.0-or-later
// Control Center > Appearance: the look being chosen and the look in use
// (0.2.0 Alpha 3.1). Choosing a theme, mode, accent or a setting of
// Transparency and clarity changes only the chosen look; Apply writes it
// and the session switches every program at once, Revert forgets it, and
// Preview shows it in a window of its own (theme_preview.cpp) before
// anything else changes. Kept free of FLTK for the unit tests.
#pragma once

#include <string>
#include <vector>

namespace sde {

class Config;

// The part of sde.conf that makes the look of the desktop: the Theme card
// ([appearance] style, theme, accent_name, accent) and the Transparency and
// clarity card ([aero]).
struct AppearanceLook {
  std::string family = "graphite";  // graphite, fluent, glass
  std::string mode = "light";       // light, dark, auto
  std::string accent = "blue";      // a named accent (theme_accents())
  std::string custom_accent;        // "#RRGGBB": a color of the user's own, kept until a swatch is chosen
  bool translucent = true;          // [aero] enabled
  int transparency = 25;            // [aero] transparency, 0-100
  bool blur = true;
  int blur_strength = 6;            // 1-12
  bool rounded_corners = true;
  bool animations = true;

  static AppearanceLook from_config(const Config &cfg);
  // Writes the look into |cfg| (not saved); keys that already hold the
  // value are left alone.
  void write(Config &cfg) const;
  // "family=fluent;mode=dark;..." for the preview window, and back. Unknown
  // keys are ignored and values are checked; false for text without a family.
  std::string encode() const;
  static bool decode(const std::string &text, AppearanceLook &out);

  bool operator==(const AppearanceLook &o) const;
  bool operator!=(const AppearanceLook &o) const { return !(*this == o); }
};

// What differs between two looks, by card, in the order of the page:
// "family", "mode", "accent", "transparency" (translucency on/off or its
// level), "blur" (on/off or strength), "corners", "animations".
std::vector<std::string> look_changes(const AppearanceLook &from, const AppearanceLook &to);

// The chosen ("pending") look and the look in use ("applied").
class AppearanceDraft {
 public:
  // Both looks from the configuration in use.
  void load(const Config &cfg);
  const AppearanceLook &applied() const { return applied_; }
  const AppearanceLook &pending() const { return pending_; }
  // True while the chosen look differs from the one in use.
  bool dirty() const { return pending_ != applied_; }
  std::vector<std::string> changes() const { return look_changes(applied_, pending_); }

  void set_family(const std::string &family);
  void set_mode(const std::string &mode);
  void set_accent(const std::string &accent);   // a named accent; drops a color of the user's own
  // A preset of Transparency and clarity (Off 0, Subtle 25, Clear 50, Glass
  // 80): translucent surfaces on at that level.
  void set_preset(int transparency);
  // The slider: a level above 0 turns translucent surfaces on.
  void set_transparency(int transparency);
  void set_translucent(bool on);
  void set_blur(bool on);
  void set_blur_strength(int strength);
  void set_rounded_corners(bool on);
  void set_animations(bool on);
  void set(const AppearanceLook &look) { pending_ = look; }

  // |base| with the chosen look written in: what the previews draw.
  Config preview_config(const Config &base) const;
  // Writes the chosen look into |cfg| (the caller saves and applies it);
  // it is the look in use from now on.
  void apply(Config &cfg);
  // Forgets the chosen look.
  void revert() { pending_ = applied_; }
  // The configuration in use changed elsewhere (the panel's dark mode, the
  // automatic mode at 19:00, another Control Center): the look in use
  // follows it. Choices that were made stay chosen; a card that was not
  // touched follows the new look in use.
  void rebase(const Config &cfg);

 private:
  AppearanceLook applied_, pending_;
};

}  // namespace sde
