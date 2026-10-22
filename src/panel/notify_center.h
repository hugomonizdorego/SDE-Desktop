// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's notifications button and flyout (alpha52).
//
// NotifyCenter mirrors the notification history kept by sde-notifyd
// (org.sde.Notifications on /org/freedesktop/Notifications: GetHistory,
// MarkRead, Dismiss, DismissApp, Clear, InvokeAction and HistoryChanged).
// The panel shows a bell with the unread count (a crossed bell while Do Not
// Disturb is on); the flyout lists the notifications grouped by
// application, newest first, each with its actions and a dismiss button,
// plus Clear all, Do Not Disturb and a link to the notification settings.
// The bubbles themselves stay in sde-notifyd.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "popup.h"

class Fl_Scroll;

struct NotifyEntry {
  uint32_t id = 0;
  std::string app, icon, summary, body;
  std::vector<std::pair<std::string, std::string>> actions;  // key, label
  uint32_t time = 0, urgency = 1;
  bool unread = true;
  bool has_default() const;
};

class NotifyCenter {
 public:
  static NotifyCenter &get();
  // Connects to the session bus; |changed| runs after every history change.
  void start(std::function<void()> changed);
  // sde-notifyd answers (its history is known).
  bool available() const { return available_; }
  // Newest first.
  const std::vector<NotifyEntry> &entries() const { return entries_; }
  int unread() const;
  int total() const { return int(entries_.size()); }
  void refresh();
  void mark_read();
  void dismiss(uint32_t id);
  void dismiss_app(const std::string &app);
  void clear();
  void invoke(uint32_t id, const std::string &key);

 private:
  void changed();
  std::vector<NotifyEntry> entries_;
  std::function<void()> changed_;
  bool started_ = false, available_ = false, pending_ = false, again_ = false;
};

class NotifyFlyout : public PanelPopup {
 public:
  NotifyFlyout();
  void status_changed();  // history or Do Not Disturb changed
  void draw() override;
  int handle(int event) override;

 protected:
  void prepare() override;
  void closed() override;

 private:
  void rebuild();
  std::string structure() const;
  static void rebuild_timeout(void *);
  std::string structure_;
  Fl_Scroll *body_ = nullptr;
  int header_ = 0, content_height_ = 0;
  bool rebuild_pending_ = false;
};
