// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE Welcome model: the start-up preference, firewall and codec status and
// the tools the First Steps open. Display-free, unit tested.
#pragma once

#include <string>
#include <vector>

namespace sde {
class Config;
namespace welcome {

extern const char *const kProjectUrl;  // SDE on GitHub
extern const char *const kIssuesUrl;
extern const char *const kDonateUrl;   // PayPal
extern const char *const kDocsUrl;     // docs/ on GitHub

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

// A program the First Steps start, first installed candidate wins.
struct Tool {
  std::string name;               // "GNOME Software"
  std::vector<std::string> argv;  // "gnome-software"
};
// Software centers in order of preference (installed ones only).
std::vector<Tool> software_centers();
// Timeshift's launcher (asks for the password itself), or an empty argv.
Tool snapshot_tool();
// A firewall front end (gufw), or an empty argv.
Tool firewall_tool();

// Panel layouts offered by the First Steps ([panel] style).
struct Layout {
  const char *id;      // full, short, floating
  const char *label;
  const char *description;
};
const std::vector<Layout> &panel_layouts();

}  // namespace welcome
}  // namespace sde
