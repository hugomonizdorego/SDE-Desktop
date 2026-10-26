// SPDX-License-Identifier: GPL-3.0-or-later
// Live badges (0.2 Alpha 3): how much news an application has, on its tiles
// and folders in the Start menu, its row in the A-Z list, its search results
// and its taskbar buttons, in the application's own colour.
//
// Two sources, neither of them polled:
//  * SDE's notifications: the unread notifications of sde-notifyd's history
//    (notify_center.h), counted per application. A notification belongs to
//    the application of its desktop-entry hint, its sender's program, its
//    application name or its icon (badges::match_app()). It counts until it
//    is read: the notifications flyout was opened and closed, it was
//    dismissed or clicked, or a window of its application became active.
//  * The Unity LauncherEntry D-Bus API: "Update" signals of
//    com.canonical.Unity.LauncherEntry (application://NAME.desktop with
//    count, count-visible, progress, progress-visible and urgent), which
//    Telegram, Chromium, Chrome and their web apps, Electron and libunity
//    applications send. The panel owns com.canonical.Unity, as docks do, so
//    that libunity applications send them; the first Update of a sender is
//    completed with its Query method; a sender that leaves the bus takes its
//    badges with it.
// An application shows its own LauncherEntry count when it has one, else
// its unread notifications (badge_model.h).
#pragma once

#include <dbus/dbus.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "badge_model.h"
#include "paint.h"

namespace badges {

class Badges {
 public:
  static Badges &get();
  // Listens to LauncherEntry on the session bus (not in safe mode).
  void start();
  // |fn(data)| runs (once per turn of the event loop) after badges changed.
  void subscribe(void (*fn)(void *), void *data);
  void unsubscribe(void *data);
  // The notification history changed (NotifyCenter), or the applications.
  void notifications_changed();
  // The badge of an application (desktop file ID); none: an empty one.
  Badge of(const std::string &id) const;
  // The badges of |ids| together (a folder): their counts added, urgent
  // when one of them is; no progress.
  Badge sum(const std::vector<std::string> &ids) const;
  bool any() const { return !badges_.empty(); }
  // A window of |id| became active: its notifications are read.
  void app_focused(const std::string &id);
  // The colours of |id|'s badge on |surface| (from its icon, |accent| when
  // it has no colour of its own), cached per icon and colours.
  Colors colors(const std::string &id, const sde::Color &surface, const sde::Color &accent);
  // UI tests: "@badge:<ID>=<count>[:urgent][:progress <percent>]" lines.
  std::string qa_lines() const;

 private:
  struct Launcher {
    std::string sender, uri, app;  // app: the desktop file ID it counts for
    LauncherState state;
  };
  void on_update(DBusMessage *m);
  void on_name_lost(DBusMessage *m);
  void query(const std::string &key, const std::string &sender, const std::string &path);
  void resolve(Launcher &l);
  void schedule();
  static void recompute(void *);
  std::map<std::string, Launcher> launchers_;     // sender + "\n" + URI
  std::map<std::string, std::string> programs_;   // sender: its program
  std::map<uint32_t, std::string> notes_;         // notification: its application
  unsigned generation_ = 0;                       // of the catalog notes_ were matched with
  std::map<std::string, Badge> badges_;
  std::map<std::string, Colors> colors_;
  std::vector<std::pair<void (*)(void *), void *>> subscribers_;
  bool started_ = false, pending_ = false;
};

// Drawing, shared by the Start menu, the search and the panel: a pill with
// the count (a dot when it is only urgent), its right edge at |right|, |h|
// high, on the cairo layer (a ring of |ring| around it when |ring| is not
// transparent), and its number in FLTK text after the cairo layer.
int pill_width(const Badge &b, int h);
void paint_pill(sde::Painter &p, double right, double top, int h, const Badge &b, const Colors &c,
                const sde::Color &ring);
void draw_pill_text(int right, int top, int h, const Badge &b, const Colors &c);
// A thin progress bar on |track|.
void paint_progress(sde::Painter &p, double x, double y, double w, double h, double progress, const Colors &c,
                    const sde::Color &track);

}  // namespace badges
