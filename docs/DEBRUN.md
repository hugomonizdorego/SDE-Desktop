# Debrun, the installer of downloaded Debian packages

Debrun (command `debrun`, icon `debrun`, package `sde`) installs Debian
packages (`.deb` files) that were downloaded from the website of their
makers — web browsers, editors, chat and video programs often come this
way — the way Debian installs packages: APT installs the package together
with what it needs from the configured package sources. It is the SDE
counterpart of GDebi (no code was taken from it).

Debrun is **not** in the Start menu or the Control Center
(`debrun.desktop` has `NoDisplay=true`). It opens when a `.deb` is
opened:

- a double-click on a `.deb` in Liria or on the desktop (Debrun is the SDE
  default application for `application/vnd.debian.binary-package`,
  `application/x-deb` and `application/x-debian-package` in
  `/etc/xdg/sde-mimeapps.list`);
- Liria's context menu: **Install with Debrun**, and **Open With** →
  Debrun;
- `debrun FILE.deb` in a terminal (also `file://` URIs).

Without a file it shows a page that explains what it is for, with
**Choose Package…** (Ctrl+O).

## What it shows

The header shows the application the package installs — the name and icon
of its launcher (`/usr/share/applications/*.desktop` inside the package,
the icon read from the package itself: hicolor, pixmaps or, like Chrome,
a logo in `/opt`) — and the package name, version and architecture. Below:

1. **Not from the package sources of *distribution*** (a warning): nobody
   from the distribution checked the package; install it only when you
   trust its maker. It names the maintainer, where the browser saved the
   file from (the `user.xdg.origin.url` attribute Chromium-based browsers
   write) and, when the package sources offer the same package, their
   version — installed from there it gets security updates with the
   system.
2. **What it changes besides its files**, from the package's files and its
   maintainer scripts: an APT package source (with its URLs, as the
   packages of Chrome, VS Code or Microsoft Edge add one) and a signing key
   for APT, maintainer scripts that run as root, scripts that download
   files (`curl`, `wget`), scheduled tasks (cron), system services
   (systemd units, init scripts), programs started at every login,
   set-user-ID or set-group-ID programs (shown in red in the file list),
   system users or groups, sudo rules, polkit rules, device (udev) rules,
   kernel modules and DKMS, login (PAM) configuration, environment and
   library path settings, update-alternatives roles (`x-www-browser` ...),
   debconf questions, configuration files in `/etc` and `Essential: yes`.
3. **Dependencies**: every `Pre-Depends`, `Depends` and `Recommends`
   relation, each one *installed* (with the version), *provided by*
   another installed package, *will be installed* (version and origin from
   APT's plan) or *not available*; `Conflicts` and `Breaks` that hit an
   installed package (removed by the change, or blocking it); the other
   packages installed or removed with it. The verdict is APT's own
   simulation (`apt-get --simulate install ./package.deb`, run as the
   user), so what Debrun shows is what APT will do. When a dependency is
   missing, **Refresh Package Lists** downloads the lists again (`apt-get
   update`; the lists on the computer may be old).
4. **Description**, **Package** (name, version, architecture, maintainer,
   homepage, section, installed size — with a warning when `/usr` has less
   free space —, download size, the installed version with **Remove…**, the
   SHA-256 digest of the file in groups of eight, to compare with the one
   the maker publishes) and **Files** (the first 60, **Show All**).

The bar at the bottom says what will happen — *Ready to install*, *Ready
to upgrade from version A to B*, *This version is installed already*
(Reinstall), *A newer version is installed: installing this one is a
downgrade*, *N more packages are installed with it*, *It removes N
packages* — or why it cannot be installed, and holds the action button:
**Install**, **Upgrade**, **Reinstall** or **Downgrade**.

## Installing and removing

The action asks for an administrator's password (the SDE polkit dialog,
remembered for a few minutes) and installs with the animated SDE logo and
a progress bar: *Waiting for permission*, *Checking the package*,
*Downloading the packages it needs*, *Unpacking …*, *Setting up …*.
A change that removes other packages (the new package conflicts with
them) is confirmed first, naming them. After an installation **Open**
starts the application through its launcher.

**Remove…** (in the Package card, for an installed package of the same
name) asks first, naming the packages removed with it (APT's simulation
of the removal), then removes it with `apt-get remove`. SDE's own packages
(`sde`, `sde-desktop`, `merkadu`) and essential or protected packages are
never removed by Debrun.

Errors are sentences, not exit codes: a package for another architecture
(with how to enable i386 when it is a 32-bit package on a 64-bit PC),
dependencies that the package sources cannot meet, a held package
(`apt-mark hold`), another program using the package system (APT waits up
to five minutes for its lock first), too little disk space, a file that is
not a Debian package or an incomplete download, the password dialog
dismissed or refused. **Show Details** lists what APT and dpkg printed.

## How it installs

Debrun runs as the user and never as root. Two ways to make the change
(`[debrun] backend=` in `sde.conf`, normally the system's
`/etc/xdg/sde/sde.conf`):

| backend | how | polkit |
| --- | --- | --- |
| `apt` (default when `pkexec` is installed) | `pkexec /usr/libexec/sde/debrun-helper install …`: the helper below runs `apt-get install` on its own copy of the file | `org.debion.sde.debrun.install`, `.remove`, `.refresh` (`auth_admin_keep`) |
| `packagekit` | PackageKit's `InstallFiles` (the file is hashed again first and must be the one shown), `Resolve` + `RemovePackages`, `RefreshCache`, like Merkadu | PackageKit's `package-install-untrusted`, `package-remove`, `system-sources-refresh` |

`auto` takes `apt` when `pkexec` and the helper are installed, else
PackageKit.

### The helper and its threat model

`debrun-helper` (`src/debrun/helper_main.cpp`, contract in `helper.h`) is
the only part that runs as root. It assumes that everything the user runs
may be hostile, including Debrun itself:

- **Arguments** are validated again: only `install`, `remove` and
  `refresh` with their options, each once; package names, versions and
  architectures in Debian syntax; an absolute path without control
  characters; a 64-digit SHA-256 digest; at most 500 removals. No shell is
  run anywhere; APT gets argument vectors and a clean environment
  (`PATH`, `LC_ALL=C.UTF-8`, `DEBIAN_FRONTEND=noninteractive`).
- **The file** is opened with the calling user's rights (`PKEXEC_UID`:
  user and groups), so root reads nothing on the user's behalf that the
  user could not read, without following a symbolic link in the last
  component (Debrun resolves links itself first) and only when it is a
  regular file of at most 8 GiB. It is copied to
  `/var/cache/sde/debrun/XXXXXX/package.deb` (`/var/cache/sde/debrun` is
  owned by root, mode 0711: nobody else can replace the copy) while its
  SHA-256 digest is computed.
- **What the user saw is what is installed**: the digest must be the one
  Debrun showed, and the copy's `Package`, `Version` and `Architecture`
  the confirmed ones; a file that changed after it was shown (a browser
  still writing it, another program replacing it) is refused.
- **No surprises**: the change is simulated again as root; it is refused
  when it would remove a package the user was not shown (`--allow-remove`
  names those that were), an essential or protected package, or SDE
  itself. Downgrades and reinstallations are only done when they were
  asked for (`--downgrade`, `--reinstall`); held packages are never
  changed (no `--allow-change-held-packages`); without approved removals
  APT runs with `--no-remove`. Changed configuration files are kept
  (`--force-confold`), debconf questions get their default answers.
- **Logging**: every request and its outcome goes to the system log
  (facility authpriv, `debrun-helper[PID]: uid 1000: install ...` with the
  path and digest); APT and dpkg keep `/var/log/apt/history.log`,
  `/var/log/apt/term.log` and `/var/log/dpkg.log`.
- The copy is removed afterwards; copies older than a day (a helper that
  was killed) are removed by the next run. Closing Debrun does not stop a
  running installation (the helper ignores a closed pipe and lets dpkg
  finish).

What it does not protect against: the package itself. A `.deb` runs its
maintainer scripts as root; that is how Debian packages work, and why
Debrun shows them, the APT sources they add and the other changes before
anything is installed, and why the decision stays with the user (and an
administrator's password). A program already running as the user can
change the user's files at any time; the digest check protects against
downloads that change, not against such a program.

## For developers

- `src/debrun/deb.*`: the `.deb` reader (ar container parsed directly;
  `control.tar` and `data.tar` read with libarchive straight from the
  file, gzip, xz, lzma, bzip2 and zstd), deb822 paragraphs, Debian
  versions (`compare_versions()` gives dpkg's order, checked against
  `dpkg --compare-versions` in the tests), relations, and the changes
  beyond the files.
- `src/debrun/system.*`: dpkg's status file, the architectures
  (`dpkg --print-architecture`, `--print-foreign-architectures`),
  install/upgrade/reinstall/downgrade, APT's simulation output,
  `apt-cache policy`, the relation checks.
- `src/debrun/helper.*`, `helper_main.cpp`: the helper's contract and the
  helper; `helper_client.cpp` and `packagekit.cpp`: the two backends;
  `window.cpp`: the window (the page widgets of the Control Center,
  Merkadu's logo animation and PackageKit enums).
- Tests: `tests/test_debrun.cpp` (the model, with `.deb` files of every
  compression built in the test and broken ones) and
  `tests/test_debrun_ui.py` (the window on Xvfb with packages built by
  `tools/debrun_fixture.py`, a stand-in helper and a fake PackageKit,
  `tests/fake_packagekit.cpp`). Hooks of the unprivileged program only
  (they never reach the helper, which replaces its environment):
  `DEBRUN_DPKG_STATUS`, `DEBRUN_ARCHITECTURES`, `DEBRUN_APT_GET`,
  `DEBRUN_APT_CACHE`, `DEBRUN_TEST_HELPER` (runs another helper program
  *without* pkexec), `DEBRUN_BACKEND`, `DEBRUN_PACKAGEKIT_BUS=session`,
  `SDE_PANEL_QA_DIR` (the state and controls in `Debrun.txt`).
