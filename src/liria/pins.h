// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Quick access: the folders, files and locations the user pinned (the
// cards of the Home page and "Pinned" in the sidebar).
//
// Only what the user pins is there. Nothing is ever added by Liria itself:
// no "frequent folders", no suggestions from the recently used files (the
// Liria of 0.2.0 Alpha 3.1 pinned folders by itself). The file is written
// only when the user pins, unpins, renames, moves or resets a pin.
//
// Storage: ~/.config/sde/liria-quick-access ($XDG_CONFIG_HOME), the file of
// the earlier Liria, so pins survive the update: UTF-8, one location per
// line in card order, a URI (an absolute path or ~/path is read too),
// optionally a TAB and a label of the user's own; blank lines and lines
// starting with '#' are ignored and a location listed twice keeps its
// first place. Without the file the defaults apply: Desktop, Downloads,
// Documents, Pictures, Music and Videos (the XDG user folders that exist
// and are not the home folder). An existing file without entries means
// that the user unpinned everything. "Reset to defaults" deletes the file.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace liria {

struct Pin {
  std::string uri;    // canonical: file:///home/a/Documents, trash:///, smb://nas/docs/
  std::string label;  // the user's own name for it, or ""
  bool operator==(const Pin &o) const { return uri == o.uri && label == o.label; }
};

class Pins {
 public:
  // ~/.config/sde/liria-quick-access
  static std::string file_path();
  // The default pins (as URIs) for the XDG user folders of this account.
  static std::vector<Pin> defaults();
  static std::vector<Pin> parse(const std::string &text);
  static std::string format(const std::vector<Pin> &pins);
  // One spelling per location: "/a/b/", "file:///a/b" and "~/b" (relative
  // to the home folder) give the same URI. "" for what is no location.
  static std::string canonical(const std::string &uri_or_path);

  // Reads the file (the defaults when it is missing). |path| for tests.
  void load(const std::string &path = "");
  // Writes the file (atomically). Called only for the user's own changes.
  bool save() const;
  // True when the pins are the user's (the file exists).
  bool custom() const { return custom_; }

  int find(const std::string &uri_or_path) const;
  bool contains(const std::string &uri_or_path) const { return find(uri_or_path) >= 0; }
  // Each of these changes the list and saves it; false when nothing changed.
  bool add(const std::string &uri_or_path, int at = -1);
  bool remove(const std::string &uri_or_path);
  bool move(int from, int to);
  bool rename(int index, const std::string &label);
  // Back to the defaults: the file is deleted.
  bool reset();

  const std::vector<Pin> &items() const { return items_; }
  // The stamp of the file when it was read, to notice another window's change.
  std::string stamp() const { return stamp_; }
  static std::string file_stamp(const std::string &path);
  const std::string &path() const { return path_; }

 private:
  std::vector<Pin> items_;
  std::string path_, stamp_;
  bool custom_ = false;
};

}  // namespace liria
}  // namespace sde
