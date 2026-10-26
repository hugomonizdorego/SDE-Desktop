# Control Center: window, interface size and user accounts

The Control Center (`sde-settings`, also `sde-control-center`) shows a
sidebar of pages and the page chosen in it. This document describes how the
window lays itself out, the interface sizes SDE offers and the management of
user accounts on the **Users & Avatar** page.

## The window

The sidebar and the page always cover the whole window, at any size:
maximized, tiled by the window manager or dragged at a border.

- The window lays the sidebar out again whenever its size changes, and the
  page covers the window at once; the page is built again for the new size
  once the size stops changing (the window manager sends many sizes while a
  border is dragged). Before 0.2 Alpha 2 the window never resized its
  children (`resizable(nullptr)` without a resize handler): a maximized
  window kept the sidebar and the page at the size it opened with, with a
  band of background below them and an empty area on the right.
- Content keeps a readable width: at most 860 pixels (the overview's tiles
  1080), times the interface size, centred in wider windows. Narrow windows
  keep the usual margins, and rows put their control under their label when
  there is no room beside it.
- The sidebar's header (the logo and "Control Center") and its footer
  ("Saved automatically", when the window is tall enough) stay in view;
  only the list of pages scrolls, as little as is needed to show the
  current page. After a resize the list scrolls back as far as the current
  page allows, and the page keeps its place or ends at its last card.
- A page opened at one of its cards (`sde-settings --page proxy`, an
  overview tile, a form of Users & Avatar) shows that card at the top, or
  the end of the page when the card is near the end, never empty space. It
  stays there while content arrives later (the network, the list of
  users), until the page is scrolled.
- A page built again (new data, a resize) keeps its place: pages start
  with an invisible mark at their top, because FLTK measures the scroll
  position from the top of a scroll's children; without it every rebuild
  moved the page up by its top margin.
- The window opens in the middle of the work area with room for its title
  bar (`fit_to_work_area()`).

Display Settings (`sde-display-settings`) shares the page code: it can be
resized and maximized too (it had no size range, so FLTK fixed its size),
and its page follows the window in the same way.

For page code (`src/settings/ui.h`): lay rows out with `Page::left()`,
`Page::width()` and `Page::cursor()`, never with the window's size; the page
is built again for every size. `Page::max_width()` (before anything is
added) gives a page another readable width, `Page::content_h()` and
`Page::max_scroll()` describe what was added.

## Interface size

**Appearance → Size and text → Interface size** offers Automatic, 75 %,
100 %, 125 %, 150 %, 175 % and 200 % (`[appearance] scale=auto|75|…|200` in
`sde.conf`). **Displays → Interface scale** and Display Settings offer the
same sizes, and display profiles keep them (0 stands for Automatic in a
profile; 75 to 200 in steps of 25 otherwise). Confirming display settings
keeps Automatic instead of turning it into 100 %.

- **Automatic** follows `Xft.dpi` (most display managers and `xrandr --dpi`
  set it), rounded to the nearest quarter, and never makes the interface
  smaller than 100 %.
- **75 %** fits more on a small screen. Every SDE program multiplies its
  sizes by the scale (`S()`, `Sd()`): windows, the panel (24, 30 or 36 pixels
  for Small, Medium and Large), menus and popups. Text is never drawn
  smaller than 9 pixels, also when a program asks for "two sizes smaller
  than the theme's", and the theme's base size is at least 9 pixels.
- GTK applications follow over XSETTINGS (`Xft/DPI`, e.g. 73728 = 72 DPI at
  75 %) and `gtk-xft-dpi` in `settings.ini`; SDE WM's title font is the text
  size times the scale, at least 7 points. The panel and the desktop follow
  a new size at once, the Control Center and Display Settings start again
  with it; other SDE programs use it when they next start, and GTK
  applications follow XSETTINGS (some only once reopened).

## User accounts (Users & Avatar)

The page shows your own account first: the picture (stock pictures, your
own picture, or your initials), your full name, user name and account type,
**Change Password…** and **Automatic login**. Below it, **Users of this
computer** lists the accounts of the people who use the computer, yours
first ("You"), with their picture, type and state (locked, password at the
next login, automatic login, logged in now).

The list comes from AccountsService (`org.freedesktop.Accounts` on the
system bus): its cached users plus the accounts of `/etc/passwd` with a UID
between `UID_MIN` and `UID_MAX` of `/etc/login.defs` and a login shell (an
account added with `adduser` that never logged in). AccountsService's
signals (`UserAdded`, `UserDeleted`, `Changed`) keep the list current while
the page is open.

**Add User…** opens a form in the page:

- **Full name**; the **user name** is made from it as it is typed (the
  first name, then first and last name, initial and last name, then the
  first name with a number: "Élodie Gómez" gives `elodie`, accents are
  dropped), and can be typed by hand. A user name starts with a lowercase
  letter, has only lowercase letters, digits, `-` and `_`, at most 32
  characters, and no user or group of that name exists (Debian's `adduser`
  rules); names of the system (`root`, `sudo`, `nobody`, …) are refused.
- **Account type**: Standard or Administrator. Administrators are the
  members of the `sudo` group (Debian), which AccountsService reports as
  account type 1.
- **Password**: set now, with a strength meter (too short, weak, fair, good,
  strong: length, kinds of characters, runs such as `1234` and the person's
  own names count) and a confirmation; at least 8 characters, not the user
  name. Or **let the person choose one at the first login**
  (`SetPasswordMode` 1: no password until then).

The new account's card opens once it is added. When AccountsService refuses
(polkit, a name taken meanwhile) the form stays open with the reason; when
the account was made but its password could not be set, its card opens so
that the password can be set there.

Choosing an account in the list opens its card below the list: **Full
name**, **Picture** (cropped to a 256 × 256 PNG that AccountsService copies
for the login screen, or removed), **Account type**, **Set Password…** (a new
password, or "ask for one at the next login"), **Automatic login** (one
account at most; AccountsService turns it off for the others and writes the
display manager's configuration, LightDM or GDM), **Lock account** (a locked
account cannot log in; its files are kept) and **Remove User…**, which asks
whether the home folder is kept (**Keep Files**) or deleted with the account
(**Delete Files**) and warns when the person is logged in.

What is refused, with the reason shown: removing, locking or changing the
type of your own account, and removing, locking or demoting the last
administrator that can log in.

### How changes are made

- Every change goes through AccountsService (`CreateUser`, `DeleteUser`,
  `SetAccountType`, `SetPassword`, `SetPasswordMode`, `SetLocked`,
  `SetAutomaticLogin`, `SetRealName`, `SetIconFile`) on a worker thread, with
  interactive authorization: polkit asks for an administrator's password
  through the session's agent (`sde-polkit-agent`). Changes to other
  accounts need `org.freedesktop.accounts.user-administration`
  (`auth_admin_keep`), automatic login `set-login-option`; your own name
  and picture `change-own-user-data` (no password).
- A password set here reaches AccountsService only as a crypt(3) hash made
  in the Control Center with a fresh random salt: yescrypt (`$y$`, Debian's
  default) when libxcrypt offers it, else SHA-512 (`$6$`).
- Your own password is changed with `passwd(1)` on a pseudo-terminal in
  the C locale, as GNOME and Cinnamon do: passwd checks the current
  password and the system's password rules (PAM) and needs no
  administrator. Its answer is shown in plain words (wrong current password,
  refused by a rule).
- No shell is run, passwords are never on a command line, and password
  fields overwrite their text when the page is built again or closed.

Without AccountsService (package `accountsservice`, which SDE recommends)
the page lists the accounts of `/etc/passwd` and says what to install;
nothing can be added or changed there. Your picture still works through
`~/.face`.

### The full name

The **Full name** (AccountsService's `RealName`, the GECOS field) is
changed in place: your own with **Save** (no password), another person's
with **Save Name** (an administrator's). The Start menu, the panel's
account menu, the Control Center overview and the lock screen follow at once
(the `_SDE_AVATAR_SERIAL` property of the root window tells them); the login
screen (LightDM's greeter reads AccountsService) shows it at the next login.

### Renaming an account

The **User name** row of your account and of every person's card has
**Rename…**: a new login name, checked as it is typed (the rules of
**Add User…**, not the name of another account, group or pending rename),
with what becomes of the home folder (`/home/sam` becomes `/home/samuel`;
a folder not named after the account keeps its name) and **Link from the
old folder** (a link `/home/sam → samuel` for programs that remember the
old path; the old name then stays taken in `/home`).

A login name cannot change while the person is logged in — their programs
hold the home folder and know the name — so **Rename at Restart** only
records the request; the rename is made at the next start of the computer,
before anyone can log in. Until then the row says **Will be renamed to
*samuel* at the next restart** (and the list "Renamed to *samuel* at the
next restart") with **Cancel Rename**. A rename that failed at the last
start says why (for example, programs of the account were running), with
**Dismiss**; nothing was changed then.

Who may: your own account with your own password (polkit action
`org.debion.sde.account-rename.schedule-self`, `auth_self_keep`), other
people's accounts with an administrator's
(`org.debion.sde.account-rename.schedule`, `auth_admin_keep`; Cancel:
`cancel-self` and `cancel`). Only people's accounts (UID_MIN to UID_MAX of
`/etc/login.defs`) are renamed; root keeps its name.

How (`sde-account-rename`, `man 8 sde-account-rename`):

1. The Control Center runs `pkexec /usr/libexec/sde/sde-account-rename
   schedule-self|schedule --uid UID --old NAME --new NAME [--link]`. The
   helper checks everything again as root (the account still has that name
   and UID; the new name is valid, not a user or group, not pending for
   another account, its home folder does not exist) and refuses a new name
   that the system already grants something to: a member of a group in
   `/etc/group` or `/etc/gshadow`, a user in sudo rules (`/etc/sudoers` and
   `sudoers.d`), polkit rules, sshd's `AllowUsers`/`DenyUsers`/`Match`,
   `/etc/security/access.conf`, `cron.allow` or `at.allow` (whoever got
   that name would get those rights). It writes
   `/var/lib/sde/account-renames/UID.rename` (key=value lines, mode 0644)
   and logs to the authpriv syslog and `/var/log/sde-account-rename.log`.
2. `sde-account-rename.service` (enabled by the package, skipped when the
   directory is empty) runs `sde-account-rename apply` early at boot:
   after the local file systems, before `systemd-user-sessions`,
   `systemd-logind`, the display manager, AccountsService and cron. A
   request is postponed when programs of the account run anyway. In order,
   each step journaled first (`UID.journal`, with copies of the files it
   changes):
   - `/etc/passwd`, `shadow`, `group`, `gshadow`, `subuid` and `subgid` are
     saved, then `usermod -l NEW -d /home/NEW -m OLD` (also the groups the
     account is a member of) and `groupmod -n NEW OLD` for its own group;
   - AccountsService's `users/OLD` and `icons/OLD` (and the `Icon=` line);
   - automatic login: `autologin-user=` of LightDM (`lightdm.conf` and
     `lightdm.conf.d`), `AutomaticLogin=`/`TimedLogin=` of GDM;
   - the crontab (`/var/spool/cron/crontabs`), the mail spool (`/var/mail`);
   - `/etc/subuid` and `/etc/subgid`;
   - sudo drop-ins in `/etc/sudoers.d` (the name and `/home/OLD/` paths),
     each checked with `visudo -c` before it replaces the old file;
     `/etc/sudoers` itself (the sudo package's file) is only reported;
   - systemd's linger flag (`/var/lib/systemd/linger`);
   - the link from the old home folder, when asked for.
   Files that still name the old account (sshd, polkit rules) are listed in
   the log. When a step fails, the steps made are undone in reverse order
   (the account database restored from the saved copies, the home folder
   moved back) and the request is marked failed with the reason. When the
   computer stops in the middle, the journal is undone at the next start.
3. The requests are deleted once made. Programs that stored the old home
   path in their own settings may need the link, or to be told the new
   folder.

### Tests of renaming

- `tests/test_account_rename.cpp` (CTest `account-rename`): the name rules,
  request files, the files that name a user, the checks and the file
  edits; as root (usermod and groupmod with `--prefix`), whole renames in a
  fake root, a failure at every step undone to the byte, and interrupted
  renames undone by the next start; the helper's commands and refusals
  (`--root`, `--dir` and `--fail-at` are for tests and refused through
  pkexec).
- `tests/test_account_rename_ui.py` (CTest `account-rename-ui`): the page
  with the fake AccountsService and the real helper on a fake root
  (`SDE_ACCOUNT_RENAME_HELPER`, `SDE_ACCOUNT_RENAME_DIR`,
  `SDE_ACCOUNT_RENAME_ROOT`): a taken name refused, a rename recorded,
  shown and cancelled, a failed one dismissed, 1024 × 600 at 150 %.
- Never against the accounts of the computer running the tests; a manual
  check on a test machine: `useradd -m test1`, `sde-account-rename schedule
  --uid … --old test1 --new test2`, `sde-account-rename apply`, then
  `userdel -r test2`.

### Tests

- `tests/test_accounts.cpp` (CTest `sde-accounts-model`): spelling user
  names, the rules for names and passwords, crypt(3) hashes, who may be
  removed, locked or demoted, the AccountsService calls (a fake backend) and
  passwd on a pseudo-terminal (fake passwd scripts).
- The fake AccountsService of `tests/panel_service_fixture.cpp` knows the
  session's user (an administrator) and two more people, Sam (standard) and
  Mia (password at the next login); it creates and removes accounts and
  changes their type, password (logged as the kind of hash only), lock and
  automatic login. `SDE_FIXTURE_SERVICES=accounts` serves only it, without an
  X display.
- `tools/ui-smoke.py` (with `--test-dir`, by default `dist/test-tools` or
  `build/tests`) adds a user, makes it an administrator, locks and removes
  it, and changes your own password through a fake `passwd`, against that
  fixture.
