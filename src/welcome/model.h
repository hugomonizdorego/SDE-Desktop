// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE Welcome model: the start-up preference, the live-session detection,
// firewall and codec status, and the window's content (texts, icons and
// commands) read from /usr/share/SDE-Welcome. Display-free, unit tested.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sde {
class Config;
namespace welcome {

// sde.conf [welcome] show: open SDE Welcome at every login (the "Show this
// dialog at startup" check box). Default false since 0.1.0~alpha51: the
// first login after installation opens it anyway (see should_autostart).
bool show_at_startup(const Config &cfg);
void set_show_at_startup(Config &cfg, bool show);

// ---- Live session and first login (0.1.0~alpha51)
//
// SDE Welcome must not pop up in the live session of the DebION ISO (the
// user is trying the system or installing it), but must greet every user
// at their first login after installation. docs/MINT-CONCEPTS.md has the
// details.

// What the live detection looks at. probe_live_session() fills it from the
// running system; the paths can be redirected for tests:
//   SDE_PROC_CMDLINE   /proc/cmdline
//   SDE_PROC_MOUNTS    /proc/mounts
//   SDE_LIVE_ROOT      prefix for /run/live, /lib/live/mount/medium,
//                      /run/live/medium and /etc/live (default "")
//   SDE_DESKTOP_DIR    the desktop folder (default $HOME/Desktop)
struct LiveProbe {
  std::string cmdline;              // kernel command line
  std::string mounts;               // /proc/mounts
  bool run_live = false;            // /run/live exists (live-boot)
  bool live_medium = false;         // /lib/live/mount/medium or /run/live/medium
  bool live_config = false;         // /etc/live/config* (live-config marker)
  std::string user;                 // login name
  bool installer_launcher = false;  // an installer launcher on the desktop
};
LiveProbe probe_live_session();

// Why this is a live session ("kernel command line: boot=live", ...), or ""
// for an installed system. Signals, any one is enough:
//   * /proc/cmdline: boot=live, boot=casper, rd.live.image, live-media=...,
//     live-media-path=..., or toram together with one of these
//   * /run/live, /lib/live/mount/medium or /run/live/medium exists
//   * / is an overlay whose lowerdir lies on a squashfs mount, or / is a
//     tmpfs while a squashfs is mounted (overlayroot on an installed disk,
//     whose lower layer is ext4/btrfs, is not live)
//   * the user is "user" or "live" and the live-config marker exists
//   * a Calamares / debian-installer launcher is on the desktop
std::string live_reason(const LiveProbe &probe);
bool is_live_session(const LiveProbe &probe);
bool is_live_session();  // probe_live_session() of this system

// Per-user stamp written when SDE Welcome was shown on an installed system:
// $XDG_CONFIG_HOME/sde/welcome-done.
std::string done_stamp_path();
// The user met SDE Welcome before: the stamp exists, or (upgrades from
// alpha50 and older) the user's sde.conf already has a [welcome] section.
bool welcome_done();
bool mark_welcome_done();

// sde-welcome --autostart: never in a live session; on an installed system
// at the first login of each user, later only while the check box is on.
bool should_autostart(bool live, bool done, bool show_setting);

// Firewall (ufw). /etc/ufw/ufw.conf is world readable: ENABLED=yes means
// ufw starts at boot. "ufw status" output (root only) -> "active",
// "inactive" or "" when unknown.
bool ufw_conf_enabled(const std::string &ufw_conf);
std::string parse_ufw_status(const std::string &output);

// Debian's GStreamer codec packages (all in main): good, bad, ugly, libav.
const std::vector<std::string> &codec_packages();
// |wanted| minus |installed|, in |wanted|'s order.
std::vector<std::string> missing_packages(const std::vector<std::string> &wanted,
                                          const std::vector<std::string> &installed);

// ---- Content (0.2 Alpha 2)
//
// The texts, icons and commands of the window are files that distributions
// edit, not code:
//   /usr/share/SDE-Welcome/welcome.conf      title, subtitle, logo, size
//   /usr/share/SDE-Welcome/steps/*.desktop   one tile per file, in the order
//                                            of the file names
// A file of the same name in /etc/SDE-Welcome takes the place of the
// packaged one (Hidden=true there removes a tile); keys set in
// /etc/SDE-Welcome/welcome.conf replace the packaged keys one by one.
// $SDE_WELCOME_DIR, when set, is searched first (trying out a
// customization); $SDE_WELCOME_DIRS replaces all folders (unit tests).
// docs/WELCOME.md describes the format.

// Folders searched, most important first.
std::vector<std::string> content_dirs();

// The window: welcome.conf [Welcome].
struct Content {
  std::string title;     // %OS% already replaced by the system's name
  std::string subtitle;
  std::string logo;      // icon name or absolute file; "" uses os-release LOGO
  bool desktop_line = true;  // "Desktop: Selviana Desktop Environment 0.2 ..."
  int width = 720, height = 540;  // pixels at 100 %
  int columns = 2;
};
// |os_name| is NAME from os-release ("" when unknown: the title is then
// "Welcome").
Content load_content(const std::string &os_name);

// One tile: steps/NN-name.desktop.
struct Step {
  std::string file;                // "10-appearance.desktop"
  std::string name, comment;
  std::vector<std::string> icons;  // names or absolute files, first found wins
  std::vector<std::string> argv;   // Exec=, or empty when |url| is opened
  std::string url;                 // Type=Link
};
// The tiles to show: visible steps whose program is installed (TryExec, or
// the program of Exec=), or else the step named by X-SDE-Welcome-Fallback.
std::vector<Step> load_steps();

// Desktop-entry style key file: [Group], Key=value, Key[ll_CC]=value,
// # comments. Exposed for the tests.
struct KeyFile {
  std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> groups;
  bool load(const std::string &path);
  void parse(const std::string &text);
  bool has(const std::string &group, const std::string &key) const;
  std::string get(const std::string &group, const std::string &key, const std::string &fallback = "") const;
  // The value for the current language (LC_ALL, LC_MESSAGES, LANG) when the
  // file has one; otherwise the plain value, translated in the gettext
  // domain the group names with X-SDE-Gettext-Domain, if any.
  std::string localized(const std::string &group, const std::string &key) const;
  // Replaces or adds the keys of |other| (an /etc override).
  void merge(const KeyFile &other);
};

// Panel layouts offered by the First Steps ([panel] style).
struct Layout {
  const char *id;      // full, short, floating
  const char *label;
  const char *description;
};
const std::vector<Layout> &panel_layouts();

}  // namespace welcome
}  // namespace sde
