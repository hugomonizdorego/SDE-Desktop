// SPDX-License-Identifier: GPL-3.0-or-later
// The rules behind the live badges of the Start menu, the search and the
// taskbar (0.2 Alpha 3, see badges.h), kept free of FLTK so that they can be
// unit tested: LauncherEntry properties, which application a notification
// or a launcher entry belongs to, the count to show and the badge's colours.
#pragma once

#include <string>
#include <vector>

#include "dbus.h"
#include "desktop_entry.h"
#include "theme.h"

namespace badges {

struct Badge {
  int count = 0;
  bool urgent = false;
  double progress = -1;  // 0..1; below 0: none
  bool shown() const { return count > 0 || urgent; }
  bool operator==(const Badge &o) const {
    return count == o.count && urgent == o.urgent && progress == o.progress;
  }
  bool operator!=(const Badge &o) const { return !(*this == o); }
};

// What an application announced with the Unity LauncherEntry API. Update
// signals carry only the properties that changed.
struct LauncherState {
  long long count = 0;
  bool count_visible = false;
  double progress = 0;
  bool progress_visible = false;
  bool urgent = false;
};
// Applies an a{sv} of properties ("count" x, "count-visible" b,
// "progress" d, "progress-visible" b, "urgent" b) from an Update signal or
// a Query reply; other properties and other types are ignored.
void apply(LauncherState &s, const sde::dbus::Value &props);
// "application://firefox.desktop" -> "firefox.desktop" (".desktop" added
// when it is missing); "" when |uri| is not an application URI.
std::string app_id_from_uri(const std::string &uri);

// Where a notification or a launcher entry came from.
struct Source {
  std::string desktop_id;  // a desktop file ID: the desktop-entry hint, the LauncherEntry URI
  std::string program;     // the sender's program (/proc/PID/exe), "flatpak:ID" in a Flatpak sandbox
  std::string app_name;    // Notify's app_name
  std::string icon;        // Notify's app_icon
};
// The program an Exec= line starts: past env, interpreters and wrappers
// (sudo, flatpak run: the Flatpak's ID), without a script's extension.
std::string exec_program(const std::string &exec);
// The desktop file ID of the application |src| belongs to, of |apps|; ""
// when none fits. In this order: the desktop file ID (also in another case);
// the program (a Flatpak's ID; the program of Exec=, StartupWMClass=, the
// desktop file ID without ".desktop"; an application that only starts the
// program wins over one that opens something with it, a web app of the
// browser); the application name (Name=, then as a program, then one name
// beginning with the other: "Telegram Desktop" and "Telegram"); the icon
// (Icon=, when one application has it).
std::string match_app(const Source &src, const std::vector<sde::DesktopEntry> &apps);

// An application's badge: its own LauncherEntry count when it shows one,
// else its unread notifications; urgent and progress from LauncherEntry.
Badge combine(const LauncherState *launcher, int unread);
// The text of a count: "7", "99+".
std::string count_text(int count);

// The colours of a badge drawn on |surface|, from the application's icon
// (|argb|: w x h cairo ARGB32 pixels, premultiplied, |stride| bytes a row):
// the hue that most of its coloured pixels share, darkened or lightened
// until white or dark text on it has a contrast of at least 4.5:1 and it
// stands apart from |surface| (1.25:1; a colour of its own stands apart
// anyway). Icons without a colour of their own (grey, black and white) take
// |fallback|.
struct Colors {
  sde::Color fill, text;
};
Colors colors_from_pixels(const unsigned char *argb, int w, int h, int stride, const sde::Color &surface,
                          const sde::Color &fallback);
// |fill| made readable as above.
Colors readable(sde::Color fill, const sde::Color &surface);

}  // namespace badges
