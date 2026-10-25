// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// The user's picture (avatar) and name, shared by the Control Center (Users
// & Avatar), the panel (Start menu account button, Action Center) and the
// lock screen.
//
// Where the picture lives:
//   * AccountsService (org.freedesktop.Accounts on the system bus) keeps
//     the picture of the login screen: User.IconFile, usually a copy in
//     /var/lib/AccountsService/icons/<user>;
//   * ~/.face (with the ~/.face.icon link to it) is the picture of the
//     user's own programs and of display managers without AccountsService.
// set_avatar() writes both; current_avatar_path() reads them back.
//
// Stock pictures are in /usr/share/Avatar (SDE_AVATAR_DIR replaces it in
// tests). Pictures are PNG, JPEG or SVG files of at most 10 MB, recognised by
// their content. No shell commands are run.
//
// Changes are announced with the _SDE_AVATAR_SERIAL property of the root
// window: the panel and the lock screen reload the picture when it changes.
#pragma once

#include <cairo.h>

#include <functional>
#include <string>
#include <vector>

namespace sde {
namespace avatar {

constexpr int kSize = 256;                     // ~/.face: 256 x 256 PNG
constexpr long long kMaxBytes = 10LL << 20;    // the largest picture accepted
constexpr int kMaxSide = 16384;                // the largest PNG/JPEG width or height
constexpr long long kMaxPixels = 64LL << 20;   // ...and pixel count (decompression bombs)
extern const char *const kSerialAtom;          // "_SDE_AVATAR_SERIAL"

// ------------------------------------------------------------- the files

// The folder of the stock pictures: $SDE_AVATAR_DIR, else /usr/share/Avatar.
std::string stock_dir();
// Its PNG, JPEG and SVG pictures, sorted by name (none when it is missing).
std::vector<std::string> stock_avatars();
// ~/.face of the user ($HOME).
std::string face_path();

// The picture to show: AccountsService's IconFile when it exists and can be
// read (unless ~/.face is newer: a change AccountsService did not accept),
// else ~/.face, else "" (the caller draws the initials instead).
std::string current_avatar_path();

// Decodes |path| (PNG, JPEG with its EXIF orientation, or SVG), crops the
// centred square and scales it to |size| x |size| pixels. Returns an ARGB32
// image surface (the caller destroys it) or nullptr; *error says why.
cairo_surface_t *load_square(const std::string &path, int size, std::string *error = nullptr);
// The same for a picture already in memory.
cairo_surface_t *decode_square(const std::string &bytes, int size, std::string *error = nullptr);

// Makes |source| the user's picture:
//   1. checks it: a regular file (a symbolic link only inside the stock
//      folder, to a file owned by root), not empty, at most 10 MB, a PNG,
//      JPEG or SVG picture of at most 16384 pixels a side;
//   2. crops the centred square and scales it to 256 x 256;
//   3. writes ~/.face atomically (mode 0644; an existing link or file is
//      replaced, never written through) and points ~/.face.icon at it;
//   4. asks AccountsService to use ~/.face (User.SetIconFile; polkit allows
//      it for the user's own account in an active session). A refusal is
//      not an error: ~/.face is used anyway and *note says so;
//   5. announces the change (notify_changed()).
// Returns false with *error when nothing was changed.
bool set_avatar(const std::string &source, std::string *error = nullptr, std::string *note = nullptr);
// Removes ~/.face, ~/.face.icon and the AccountsService picture: the
// initials are shown again.
bool clear_avatar(std::string *error = nullptr, std::string *note = nullptr);

// --------------------------------------------------------- the account

struct Account {
  std::string user;       // login name
  std::string real_name;  // AccountsService RealName, else the GECOS full name (may be empty)
  bool admin = false;     // member of sudo (or admin, wheel): "Administrator"
  bool accounts_service = false;  // AccountsService answered
  const std::string &display_name() const { return real_name.empty() ? user : real_name; }
};
Account account();
// True when |user| is a member of sudo (or admin, wheel): an administrator.
bool administrator(const std::string &user);

// Changes the real name (AccountsService User.SetRealName, allowed for the
// user's own account). Names are UTF-8, at most 128 bytes, without control
// characters, ':' or ','.
bool valid_real_name(const std::string &name, std::string *error = nullptr);
bool set_real_name(const std::string &name, std::string *error = nullptr);

// "Hugo Moniz do Rego" -> "HR", "selviana" -> "S", "" -> "?".
std::string initials(const std::string &name);

// Bumps _SDE_AVATAR_SERIAL on the root window (FLTK's display when it is
// open, else a connection of its own; nothing without $DISPLAY).
void notify_changed();

// --------------------------------------------------- AccountsService

// The calls made to AccountsService; the tests install a fake one.
class AccountsBackend {
 public:
  virtual ~AccountsBackend() = default;
  // RealName and IconFile of |user|; false when AccountsService does not
  // answer (not installed, no system bus, unknown user).
  virtual bool lookup(const std::string &user, std::string *real_name, std::string *icon_file) = 0;
  virtual bool set_icon_file(const std::string &user, const std::string &file, std::string *error) = 0;
  virtual bool set_real_name(const std::string &user, const std::string &name, std::string *error) = 0;
};
// The backend in use: org.freedesktop.Accounts on the system bus (honours
// DBUS_SYSTEM_BUS_ADDRESS), or the one passed here (nullptr restores it).
AccountsBackend &backend();
void set_backend(AccountsBackend *b);

// ------------------------------------------------ for FLTK programs

// Result of a background operation.
struct Result {
  bool ok = false;
  std::string error, note;
};
// Runs |work| on a worker thread and |done| from the FLTK event loop once it
// has finished (polled, so Fl::lock() is not needed); then announces the
// change when it succeeded. set_avatar(), clear_avatar() and
// set_real_name() talk to AccountsService, which may take a moment (or ask
// for a password): programs with a window call them through this.
void run_async(std::function<Result()> work, std::function<void(const Result &)> done);
// The usual operations through run_async().
void set_avatar_async(const std::string &source, std::function<void(const Result &)> done);
void clear_avatar_async(std::function<void(const Result &)> done);
void set_real_name_async(const std::string &name, std::function<void(const Result &)> done);
// Looks the account and the picture up on a worker thread (AccountsService
// may need to be started first).
void lookup_async(std::function<void(const Account &, const std::string &path)> done);

}  // namespace avatar
}  // namespace sde
