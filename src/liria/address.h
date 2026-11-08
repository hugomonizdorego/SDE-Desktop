// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// What the user typed in the address bar, made into a location:
//
//   smb://server/share/folder   as typed (spaces and the like escaped)
//   \\server\share\folder       smb://server/share/folder (as in Windows)
//   smb:  smb:/  smb://  \\     smb:/// (the computers of the network)
//   192.168.1.10  [fe80::1]  localhost  fileserver  user@nas  nas:445
//                               the Windows shares of that computer
//                               (kind Host: other protocols are offered)
//   sftp://user@host:22/path  ssh://host  ftp://  ftps://  nfs://
//   dav://  davs://  webdav(s)://  afp://
//                               cleaned up; a password typed in the address
//                               is taken out of it for the sign-in dialog
//   /path  ~/path  name         a path, or a name in the current folder
//   C:\Users                    an error that explains Windows drive letters
//
// Recent servers (~/.config/sde/liria-servers, mode 0600): the last 30
// servers and shares opened, never a password; the address bar suggests
// them and the Home page lists them under Network locations. The file is
// the one of the earlier Liria. No widgets; shared with the unit tests.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace liria {

struct Address {
  enum Kind { Empty, Path, Location, Host, Error };
  Kind kind = Empty;
  std::string uri;       // Location, Host: what to open (never with a password); Path: as typed
  std::string scheme;    // lower case, aliases resolved (ssh: is sftp)
  std::string host;      // network locations: the server, without [ ]
  std::string user;      // a user typed in the address (Windows shares: DOMAIN;user)
  std::string password;  // a password typed in the address (taken out of |uri|)
  int port = 0;          // 0: the protocol's own
  bool ambiguous = false;  // Host: a plain name, which may also be a file of the folder
  std::string message;   // Error: for the user
};

Address parse_address(const std::string &typed);

// smb, sftp, ftp, ftps, nfs, dav, davs, afp.
bool is_network_scheme(const std::string &scheme);
// ftp, dav and http send passwords and files as plain text.
bool is_plain_text_scheme(const std::string &scheme);
// "Windows shares (SMB)", "SSH (SFTP)", ...
std::string scheme_label(const std::string &scheme);
// The protocols offered for a computer: smb first, then the encrypted ones.
const std::vector<std::string> &host_schemes();
// The same computer with another protocol: sftp://user@host/ ("" without a host).
std::string address_with_scheme(const Address &a, const std::string &scheme);

// ---------------------------------------------------------------- servers

// The server part of a network URI (for Windows shares with the share):
// "smb://nas/docs/a/b" -> "smb://nas/docs/", "sftp://u@h:2222/x" -> "sftp://u@h:2222/"; "" for others.
std::string server_root(const std::string &uri);

class Servers {
 public:
  static std::string file_path();  // ~/.config/sde/liria-servers
  static std::vector<std::string> parse(const std::string &text);
  static std::string format(const std::vector<std::string> &servers);
  void load(const std::string &path = "");
  // Puts the server of |uri| first; false for local places.
  bool remember(const std::string &uri);
  bool forget(const std::string &uri);
  void clear();
  // The servers |typed| may stand for (the start of the location, or of the
  // computer's name).
  std::vector<std::string> matching(const std::string &typed) const;
  const std::vector<std::string> &items() const { return items_; }

 private:
  bool save() const;
  std::vector<std::string> items_;
  std::string path_;
};

}  // namespace liria
}  // namespace sde
