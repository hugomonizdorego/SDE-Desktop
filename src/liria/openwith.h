// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Opening files: the applications for a file's type (the installed desktop
// entries and their MimeType=, also for the types it is a kind of: a
// Python script opens as text), the default one by the freedesktop.org
// rules (mimeapps.list of the user, the system and SDE,
// /etc/xdg/sde-mimeapps.list; src/settings/default_apps.h, the model of
// Control Center > Default Applications) and making an application the
// default ([Default Applications] in ~/.config/mimeapps.list). Programs
// start through sde::launch_entry(), without a shell.
//
// Also the recently used files of the Home page (recently-used.xbel, shared
// with every GTK program): forgetting one or all.
#pragma once

#include <string>
#include <vector>

#include "../settings/default_apps.h"

namespace sde {
namespace liria {

class Apps {
 public:
  // The installed applications (scanned once; rescan() after installs).
  const std::vector<MimeApp> &all();
  void rescan();
  // Applications for |mime| and the types it is a kind of, the default first.
  std::vector<const MimeApp *> for_type(const std::string &mime);
  // The desktop file ID of the default for |mime| ("" when none).
  std::string default_for(const std::string &mime);
  // Makes |app_id| the default for |mime| (the user's mimeapps.list).
  bool set_default(const std::string &mime, const std::string &app_id, std::string *error = nullptr);
  const MimeApp *find(const std::string &app_id);
  // Starts |app_id| with |files| (paths or URIs).
  bool launch(const std::string &app_id, const std::vector<std::string> &files);

 private:
  std::vector<MimeApp> apps_;
  bool scanned_ = false;
};

// Recently used files (Home page).
bool forget_recent(const std::string &path);
bool clear_recent();

}  // namespace liria
}  // namespace sde
