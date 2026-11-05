// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Drives and network places through GIO: GVfs's volume monitors (UDisks2
// for disks, sticks and phones, and the network shares GVfs mounted) list
// them, mount, unmount and eject them (polkit asks when the system wants
// it; an encrypted drive asks for its passphrase in Liria's own dialog).
// The system disk ("/") is always first. Network addresses are mounted
// with g_file_mount_enclosing_volume(); a server that wants a password, or
// an unknown SSH host key, asks through MountQuestions (dialogs.cpp).
//
// Until the volume monitor is ready (it starts after Liria's window is on
// the screen, so the start stays fast) the drives come from
// /proc/self/mountinfo.
#pragma once

#include <functional>
#include <string>
#include <vector>

typedef struct _GMountOperation GMountOperation;

namespace sde {
namespace liria {

struct Drive {
  std::string id;      // stable: "root", "mount:<uri>", "volume:<uuid or name>"
  std::string name;    // "System", "SANDISK", "docs on nas"
  std::string path;    // the mount point when mounted ("" otherwise)
  std::string uri;     // the mount's root URI ("smb://nas/docs/")
  std::vector<std::string> icons;  // icon theme names, best first
  long long total = -1, free = -1;  // bytes, -1 when unknown
  bool mounted = false, can_mount = false, can_unmount = false, can_eject = false;
  bool removable = false, network = false, system = false;
};

// What the sign-in dialog answers (MountQuestions).
struct SignIn {
  bool ok = false;            // false: cancelled
  bool anonymous = false;     // "As a guest"
  std::string user, domain, password;
  bool remember = false;      // in the keyring
};

struct MountQuestions {
  // |message| from GVfs, the user and domain it suggests, what it asks for
  // (G_ASK_PASSWORD_* flags), and whether a keyring can keep a password.
  // Answer by calling |reply| (also later, from a dialog).
  std::function<void(const std::string &message, const std::string &user, const std::string &domain, int flags,
                     bool keyring, std::function<void(const SignIn &)> reply)>
      password;
  // A question with choices (an unknown SSH host key); reply with the
  // index of the choice, or -1 for none.
  std::function<void(const std::string &message, const std::vector<std::string> &choices,
                     std::function<void(int)> reply)>
      question;
};

class Drives {
 public:
  Drives();
  ~Drives();
  Drives(const Drives &) = delete;
  Drives &operator=(const Drives &) = delete;

  // Starts the volume monitor; |changed| runs whenever drives or mounts change.
  void start(std::function<void()> changed);
  bool started() const { return monitor_ != nullptr; }

  // Disks and removable drives, the system disk first.
  std::vector<Drive> drives() const;
  // Network places GVfs mounted (shares, SFTP servers, ...).
  std::vector<Drive> network() const;
  // The drive whose mount point holds |path| (the deepest), or nullptr.
  const Drive *drive_of(const std::string &path) const;
  // Mount points of the drives (for the breadcrumbs).
  std::vector<std::string> mount_points() const;

  using Done = std::function<void(bool ok, const std::string &error, const std::string &path)>;
  void mount(const std::string &id, MountQuestions q, Done done);
  void unmount(const std::string &id, Done done);
  void eject(const std::string &id, Done done);
  // Mounts the enclosing volume of a network |uri|; |done| gets the local
  // (FUSE) path of |uri| when GVfs has one ("" otherwise).
  void mount_location(const std::string &uri, MountQuestions q, Done done);
  // Whether GIO can open |scheme| here (gvfs-backends installed).
  static bool scheme_supported(const std::string &scheme);
  // The package that brings |scheme| ("gvfs-backends").
  static std::string package_for(const std::string &scheme);
  // A Secret Service runs or can be started on the session bus.
  static bool keyring_available();

  void refresh();  // reads everything again (also before start())

 private:
  struct Impl;
  Impl *impl_;
  void *monitor_ = nullptr;
  std::function<void()> changed_;
  std::vector<Drive> drives_, network_;
};

// The local path GIO gives a URI (its FUSE path for GVfs mounts), or "".
std::string local_path_of_uri(const std::string &uri);
// The URI for a local path inside a GVfs mount, or "".
std::string uri_of_local_path(const std::string &path);

}  // namespace liria
}  // namespace sde
