// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Renaming a user account (Control Center > Users & Avatar): its login name
// and, when it is named after it, its home folder. A rename cannot be made
// while the person is logged in (their programs hold the home folder and
// know the old name), so it is recorded now and made at the next start of
// the computer, before anyone can log in (docs/CONTROL-CENTER.md):
//
//  * sde-account-rename schedule (pkexec, polkit) checks the new name and
//    writes a request to /var/lib/sde/account-renames/UID.rename;
//  * sde-account-rename.service runs "sde-account-rename apply" early at
//    boot, before the display manager, logind and cron: usermod -l -d -m,
//    groupmod -n for the user's own group, then the files that name the
//    account (AccountsService's user file and picture, automatic login of
//    LightDM and GDM, the crontab, the mail spool, /etc/subuid and
//    /etc/subgid, sudoers drop-ins, systemd's linger flag) and, when asked,
//    a link from the old home folder; every step is journaled, so a failure
//    (or a crash) is undone step by step;
//  * the Control Center shows "Will be renamed to ... at the next restart"
//    with Cancel.
//
// This file is the model, without a display (tests/test_account_rename.cpp
// runs it against a fake root): the request files, the name rules (shared
// with accounts.cpp), the checks, the edits of the files and the steps.
// Every path is below a root directory ("/" on a real system).
#pragma once

#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

namespace sde {
namespace account_rename {

// ------------------------------------------------------------- names

// "" when |name| can be a login name by its spelling: a lowercase letter,
// then lowercase letters, digits, '-' or '_', at most 32 characters, and
// none of the names of the system ("root", "sudo", "lightdm" ...).
// accounts::user_name_problem() uses it too.
std::string login_name_problem(const std::string &name);

// ------------------------------------------------------------ requests

struct Request {
  uid_t uid = 0;
  std::string old_name, new_name;
  std::string old_home, new_home;  // equal when the folder keeps its name
  bool link = false;               // a link from the old home folder to the new one
  uid_t by = 0;                    // who asked (PKEXEC_UID)
  long long time = 0;              // when (seconds since 1970)
  std::string state = "pending";   // "pending" or "failed"
  std::string error;               // failed: why (a sentence)
};

// The directory of the requests (SDE_ACCOUNT_RENAME_DIR replaces it in the
// Control Center's tests; the helper takes --dir).
std::string default_dir();
std::string request_path(const std::string &dir, uid_t uid);
// key=value lines; false with *error for anything else.
bool parse_request(const std::string &text, Request *out, std::string *error);
std::string format_request(const Request &r);
// The requests of |dir| (files that cannot be read or parsed are skipped).
std::vector<Request> load_requests(const std::string &dir);
// Writes |r| to |dir| atomically (mode 0644: the Control Center reads it).
bool save_request(const std::string &dir, const Request &r, std::string *error);

// The home folder after the rename: the same folder with the new name when
// its name is the old login name ("/home/sam" -> "/home/samuel"), else
// unchanged.
std::string new_home_for(const std::string &home, const std::string &old_name, const std::string &new_name);

// ------------------------------------------------- the account database

struct Passwd {
  std::string name;
  uid_t uid = 0;
  gid_t gid = 0;
  std::string gecos, home, shell;
};
struct Group {
  std::string name;
  gid_t gid = 0;
  std::vector<std::string> members;
};
std::vector<Passwd> read_passwd(const std::string &root);
std::vector<Group> read_group(const std::string &root);
// The first UID of people's accounts and the last (UID_MIN and UID_MAX of
// /etc/login.defs; 1000 and 60000 without it).
void uid_range(const std::string &root, uid_t *min, uid_t *max);

// Files below |root| that name |name| as a user: group and gshadow member
// lists, sudoers (and sudoers.d), polkit rules, sshd's AllowUsers, PAM's
// access.conf, cron.allow and at.allow. A new name must not be one of them:
// whoever got that name would get what they grant.
std::vector<std::string> mentions(const std::string &root, const std::string &name);

// "" when account |uid| (now |old_name|) may be renamed to |new_name| on the
// system below |root|; else why not, in a sentence for the user. |others|:
// the other requests (two accounts cannot ask for the same name).
std::string check(const std::string &root, uid_t uid, const std::string &old_name, const std::string &new_name,
                  const std::vector<Request> &others);

// ------------------------------------------------------- file edits

// The lines that name the account, with the new name. Each returns the
// text unchanged when nothing names it.
// LightDM: "autologin-user=OLD" (lightdm.conf and lightdm.conf.d).
std::string edit_lightdm(const std::string &text, const std::string &old_name, const std::string &new_name);
// GDM: "AutomaticLogin=OLD", "TimedLogin=OLD" (gdm3/daemon.conf).
std::string edit_gdm(const std::string &text, const std::string &old_name, const std::string &new_name);
// /etc/subuid and /etc/subgid: "OLD:100000:65536".
std::string edit_subid(const std::string &text, const std::string &old_name, const std::string &new_name);
// sudoers: OLD (and %OLD, the user's own group) as a word, outside comments.
std::string edit_sudoers(const std::string &text, const std::string &old_name, const std::string &new_name);
// AccountsService's user file: "Icon=.../icons/OLD".
std::string edit_accountsservice(const std::string &text, const std::string &old_icon, const std::string &new_icon);

// ------------------------------------------------------------ applying

// Runs a program (argv, no shell) and returns its exit status; -1 when it
// could not run. The tests may replace it.
using Runner = std::function<int(const std::vector<std::string> &argv, std::string *output)>;
int run_program(const std::vector<std::string> &argv, std::string *output);

class Applier {
 public:
  // |root|: "/" or a fake root; |dir|: the requests and their journals.
  Applier(std::string root, std::string dir, Runner run = run_program);
  // Logs a line (syslog and the log file in the helper).
  std::function<void(const std::string &)> log;
  // Fails the step of this name ("usermod", "groupmod", "accountsservice",
  // "lightdm", "gdm", "crontab", "mail", "subuid", "subgid", "sudoers",
  // "linger", "link") on purpose, to test the rollback.
  std::string fail_at;
  // Stops after the step of this name as if the computer had stopped: the
  // journal stays, nothing is undone (tests of recover()).
  std::string crash_after;
  // Whether processes of |uid| run (then the account is in use and the
  // rename waits for the next start). Only asked on the real root.
  std::function<bool(uid_t uid)> in_use;

  // Makes the rename of |r|. True when it is made; false with *error when
  // it was undone (*postponed: it was not started because the account is
  // in use, the request stays).
  bool apply(const Request &r, std::string *error, bool *postponed = nullptr);
  // Undoes the steps a journal lists (a rename interrupted by a crash).
  // True when nothing was left to undo or everything was undone;
  // *completed: the journal shows the rename was made (nothing undone).
  bool recover(uid_t uid, std::string *error, bool *completed = nullptr);

 private:
  std::string root_, dir_;
  Runner run_;
  std::string journal_;  // the journal of the rename being made
  std::string path(const std::string &p) const;  // below the root
  bool step(const std::string &line);            // appends to the journal
  bool undo(const std::vector<std::string> &lines, std::string *error);
  bool backup(const std::string &file, std::string *saved);
  bool edit_file(const std::string &name, const std::string &file,
                 const std::function<std::string(const std::string &)> &edit, std::string *error);
  bool move(const std::string &name, const std::string &from, const std::string &to, std::string *error);
};

// Whether processes of |uid| run now (/proc/*/status).
bool uid_has_processes(uid_t uid);

}  // namespace account_rename
}  // namespace sde
