// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Files on the clipboard (Cut, Copy, Paste), as file managers share them on
// X11: Liria owns the CLIPBOARD selection and offers
//   x-special/gnome-copied-files  "copy" or "cut", then one URI per line
//                                 (GTK file managers, the SDE desktop)
//   text/uri-list                 the URIs (browsers, Qt programs)
//   application/x-kde-cutselection "1" for a cut (Dolphin)
//   UTF8_STRING, text/plain       the paths, for text fields
// Cut items are drawn faded until they are pasted or the clipboard goes
// to another program. Pasting reads what any program put there (the
// window's desk::Xdnd asks for it).
#pragma once

#include <X11/Xlib.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace sde {
namespace liria {

class FileClipboard {
 public:
  FileClipboard();
  ~FileClipboard();
  FileClipboard(const FileClipboard &) = delete;
  FileClipboard &operator=(const FileClipboard &) = delete;

  // Puts |paths| (absolute paths or URIs) on the clipboard.
  void set(const std::vector<std::string> &paths, bool cut, unsigned long time);
  // Forgets what Liria put there (after a cut was pasted).
  void clear();
  bool owned() const { return owned_; }
  bool cut() const { return owned_ && cut_; }
  bool is_cut(const std::string &path) const { return owned_ && cut_ && cut_set_.count(path) != 0; }
  const std::vector<std::string> &paths() const { return paths_; }
  // Whether a program offers files (or Liria does).
  bool has_files();
  std::function<void()> changed;  // the cut marks changed

  // The selection's texts (also for tests).
  static std::string gnome_text(const std::vector<std::string> &uris, bool cut);
  static std::string uri_list(const std::vector<std::string> &uris);
  static bool parse_gnome(const std::string &text, std::vector<std::string> &uris, bool &cut);

 private:
  static int system_handler(void *event, void *data);
  int handle(XEvent &e);
  void answer(const XSelectionRequestEvent &r);
  ::Window win_ = 0;
  bool owned_ = false, cut_ = false;
  std::vector<std::string> paths_, uris_;
  std::set<std::string> cut_set_;
  unsigned long time_ = 0;
};

}  // namespace liria
}  // namespace sde
