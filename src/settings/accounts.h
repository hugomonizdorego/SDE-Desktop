// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// The user accounts of this computer, for Control Center > Users & Avatar.
//
// Accounts are listed and changed through AccountsService
// (org.freedesktop.Accounts on the system bus; DBUS_SYSTEM_BUS_ADDRESS
// replaces it in tests). polkit decides about every change and asks for a
// password through the session's agent (sde-polkit-agent): adding,
// removing and changing other people's accounts needs an administrator.
// Administrators are the members of the sudo group (Debian), which
// AccountsService reports as AccountType 1.
//
// The rules kept here, unit tested (tests/test_accounts.cpp):
//  * a user name starts with a lowercase letter, then has lowercase letters,
//    digits, '-' or '_', at most 32 characters, and no user or group has
//    it yet (adduser's rules on Debian);
//  * a password has at least 8 characters and is not the user name; it is
//    only ever sent as a crypt(3) hash with a fresh salt (yescrypt when
//    libcrypt has it, else SHA-512);
//  * nobody removes, locks or changes the type of their own account here,
//    and the last administrator stays an unlocked administrator.
// Your own password is changed with passwd(1) on a pseudo-terminal, as
// GNOME and Cinnamon do it: passwd checks the current password and the
// system's password rules (PAM), and needs no administrator. No shell is
// run anywhere, and passwords are never put on a command line.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace sde {
namespace accounts {

enum class Type { standard = 0, administrator = 1 };
// AccountsService's PasswordMode: a password, one the user chooses at the
// next login, or none.
enum class PasswordMode { regular = 0, set_at_login = 1, none = 2 };

struct User {
  std::string path;        // AccountsService object ("" without AccountsService)
  uid_t uid = 0;
  std::string name;        // login name
  std::string real_name;   // may be empty
  std::string icon;        // IconFile, "" for none
  std::string home;
  Type type = Type::standard;
  PasswordMode password_mode = PasswordMode::regular;
  bool locked = false;
  bool auto_login = false;
  bool self = false;       // the account running the Control Center
  bool logged_in = false;  // has a session now (utmp)
  const std::string &display_name() const { return real_name.empty() ? name : real_name; }
  bool admin() const { return type == Type::administrator; }
};

// ------------------------------------------------------------- the rules

// Letters and digits of |text| without accents, in lower case, words kept
// apart by single spaces ("Élodie Ñúñez-Gómez" -> "elodie nunez gomez").
// Letters without a Latin spelling are left out.
std::string ascii_words(const std::string &text);
// "" when |name| can be the user name of a new account, else why not.
// |taken| are names known to be in use besides the system's users and
// groups (AccountsService's list).
std::string user_name_problem(const std::string &name, const std::vector<std::string> &taken = {});
// A free user name for |full_name|: the first name, then first and last
// name, initial and last name, then the first name with a number
// ("Hugo Moniz do Rego" -> "hugo", "hugorego", "hrego", "hugo2"). "" when
// the name has no Latin letters (the user types one).
std::string suggest_user_name(const std::string &full_name, const std::vector<std::string> &taken = {});
// True when a user or a group called |name| exists on this system.
bool name_in_use(const std::string &name);

// How hard |password| is to guess: 0 too short (under 8 characters), 1
// weak, 2 fair, 3 good, 4 strong. Length, the kinds of characters,
// repeated or sequential runs and the user's own names count.
int password_strength(const std::string &password, const std::string &user = "",
                      const std::string &real_name = "");
// "Too short", "Weak", "Fair", "Good", "Strong".
std::string strength_label(int strength);
// "" when |password| (typed twice: |again|) can become the password of
// |user|, else why not.
std::string password_problem(const std::string &password, const std::string &again,
                             const std::string &user = "", const std::string &real_name = "");
// The crypt(3) hash of |password| with a fresh random salt: yescrypt ("$y$")
// when libcrypt offers it, else SHA-512 ("$6$"); "" when hashing failed.
std::string hash_password(const std::string &password);

// Administrators that can still log in (not locked).
int active_admins(const std::vector<User> &all);
// "" when the change may be made to |target| of the list |all|, else why
// not (yourself, or the last administrator).
std::string why_not_remove(const std::vector<User> &all, const User &target);
std::string why_not_set_type(const std::vector<User> &all, const User &target, Type type);
std::string why_not_lock(const std::vector<User> &all, const User &target);

// "Administrator" or "Standard".
std::string type_label(Type t);

// --------------------------------------------------- AccountsService

// The calls made to AccountsService; the tests install a fake one. Every
// call blocks (polkit may wait for a password): run them off the FLTK
// thread. They return false with *error (a sentence for the user) when
// AccountsService refused or did not answer.
class Backend {
 public:
  virtual ~Backend() = default;
  // The people's accounts (not system accounts), yours first.
  virtual bool list(std::vector<User> *users, std::string *error) = 0;
  virtual bool create(const std::string &name, const std::string &real_name, Type type,
                      std::string *path, std::string *error) = 0;
  virtual bool remove(const User &u, bool remove_files, std::string *error) = 0;
  virtual bool set_type(const User &u, Type type, std::string *error) = 0;
  // |hash| is a crypt(3) hash, never the password.
  virtual bool set_password(const User &u, const std::string &hash, std::string *error) = 0;
  virtual bool set_password_mode(const User &u, PasswordMode mode, std::string *error) = 0;
  virtual bool set_locked(const User &u, bool locked, std::string *error) = 0;
  virtual bool set_auto_login(const User &u, bool on, std::string *error) = 0;
  virtual bool set_real_name(const User &u, const std::string &name, std::string *error) = 0;
  // |file| a PNG for the login screen (AccountsService copies it), "" removes it.
  virtual bool set_icon(const User &u, const std::string &file, std::string *error) = 0;
};
// org.freedesktop.Accounts on the system bus, or the one set here (nullptr
// restores it).
Backend &backend();
void set_backend(Backend *b);

// The accounts of /etc/passwd that belong to people (UID_MIN to UID_MAX
// of /etc/login.defs, a login shell) and yours, for when AccountsService
// is not there: shown, not changed.
std::vector<User> local_users();

// ------------------------------------------------------------ changes
// Each checks its arguments with the rules above first.

struct NewUser {
  std::string name, real_name;
  Type type = Type::standard;
  bool password_at_login = false;  // the user chooses one at the first login
  std::string password, again;     // else this one, typed twice
};
// CreateUser, then SetPassword with the hash (or SetPasswordMode 1). The
// account exists even when setting its password failed: *error says so.
bool add_user(const NewUser &n, const std::vector<User> &all, std::string *error);
// Another person's password (SetPassword with a hash), or "they choose one
// at the next login" (SetPasswordMode 1) when |at_login|.
bool set_password(const User &u, bool at_login, const std::string &password, const std::string &again,
                  std::string *error);
// The picture of another person's account for the login screen: |source|
// (PNG, JPEG or SVG) cropped to a 256 x 256 PNG, given to AccountsService.
bool set_picture(const User &u, const std::string &source, std::string *error);

// Your own password with passwd(1) (the first "passwd" in $PATH, or
// |program|): it is given |current|, then |password| twice, on a
// pseudo-terminal in the C locale. False with *error in plain words (wrong
// current password, refused by the password rules, ...).
bool change_own_password(const std::string &current, const std::string &password,
                         const std::string &again, std::string *error, const std::string &program = "");

}  // namespace accounts
}  // namespace sde
