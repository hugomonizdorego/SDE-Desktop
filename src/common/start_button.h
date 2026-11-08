// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's Start button (0.2.0 Alpha 4, docs/PANEL.md "Start button"):
// its picture, its text and which of them it shows, set in Control Center >
// Panel > Start button:
//
//   [panel] start_style = both | icon | text      (default both)
//           start_label = the text                (default "SDE")
//           start_icon  = a prepared picture      (default: the SDE logo)
//
// A picture the user picks (PNG, JPEG or SVG) is cropped to its centred
// square and stored at kStoredSize pixels next to sde.conf
// (~/.config/sde/start-button-<n>.png); the panel draws it at its icon size
// (20 to 28 pixels at 100 %). The panel and the Control Center's preview
// lay the button out with layout(), so the preview is the real thing.
#pragma once

#include <string>

namespace sde {
class Config;

namespace start_button {

enum class Style { Both, IconOnly, TextOnly };

// Pictures smaller than this look blurred on the panel at 200 %.
constexpr int kMinPicture = 64;
// Side of the stored picture.
constexpr int kStoredSize = 128;
// Longest text, in characters.
constexpr size_t kMaxLabel = 24;
// The default text.
constexpr const char *kDefaultLabel = "SDE";

const char *style_name(Style s);         // "both", "icon", "text"
Style parse_style(const std::string &s);  // anything unknown: Both
// The text as the panel shows it: trimmed, no control characters, at most
// kMaxLabel characters (whole UTF-8 characters).
std::string clean_label(const std::string &s);

struct Settings {
  Style style = Style::Both;
  std::string label = kDefaultLabel;  // "" is allowed with IconOnly
  std::string icon;                   // the prepared picture; "" for the SDE logo
  // What is drawn: the text alone with an empty label shows the icon after
  // all, and the icon alone with no picture the SDE logo.
  bool shows_icon() const { return style != Style::TextOnly || label.empty(); }
  bool shows_text() const { return style != Style::IconOnly && !label.empty(); }
};
// [panel] start_style, start_label and start_icon (an icon file that does
// not exist any more counts as none).
Settings from_config(const Config &cfg);

// Where the parts go inside the button's slot, in pixels: |icon| is the
// icon's size, |text_w| the width of the label in the panel's bold font,
// |unit| one pixel at the interface scale (S(1) as a double).
struct Layout {
  int width = 0;   // the slot on the panel, from its left edge
  int box_x = 0, box_w = 0;  // the accent-coloured button inside it
  int icon_x = 0;            // left of the icon
  int text_x = 0, text_w = 0;  // the text's box
};
Layout layout(const Settings &s, int icon, int text_w, double unit);

// Makes |source| (PNG, JPEG or SVG) the Start button's picture: crops the
// centred square, scales it to kStoredSize and writes it to |dir| (default
// ~/.config/sde). Returns its path, or "" with *error. *small is set when
// the picture is smaller than kMinPicture, *cropped when it was not square.
std::string prepare_picture(const std::string &source, std::string *error, bool *small = nullptr,
                            bool *cropped = nullptr, const std::string &dir = "");
// Removes the pictures prepare_picture() wrote to |dir| except |keep|.
void remove_pictures(const std::string &keep, const std::string &dir = "");

}  // namespace start_button
}  // namespace sde
