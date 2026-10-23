# Security policy

This is the security policy of the Selviana Desktop Environment (SDE): the
programs in `src/`, the toolkit fork in `sde-fltk/`, the suite components built
from pinned upstream sources (Liria from Caja, SDE WM from Openbox, the SDE
compositor from picom) and the DebION integration package
`debion-desktop-base` (`data/networkmanager/`, `debian/`).

## Supported versions

SDE is alpha software. Security fixes go into the newest development release
only; there are no maintenance branches yet.

| Version | Supported |
| --- | --- |
| 0.2.0 Alpha 1 (current) | yes |
| 0.1.0 alphas (0.1.0~alpha2 … 0.1.0~alpha52) | no: please update |

The rolling `alpha-latest` pre-release on GitHub is built from `main` and
receives fixes first.

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub's private
vulnerability reporting:

1. Open <https://github.com/hugomonizdorego/SDE-Desktop/security/advisories/new>
   (repository → **Security** → **Report a vulnerability**).
2. Describe the problem, the affected program and version
   (`<program> --version`), and how to reproduce it. A proof of concept, logs
   (`~/.local/state/sde/session.log`) and the configuration you used help.
3. Do not open a public issue, pull request or discussion for an unfixed
   vulnerability, and do not test against systems you do not own.

What to expect:

- an acknowledgement within 7 days;
- an assessment (severity, affected versions) and a plan within 14 days;
- a fix in the next alpha, published together with a GitHub security advisory
  that credits you (unless you prefer otherwise). We aim to publish within
  90 days of the report, earlier for actively exploited problems.

Vulnerabilities in upstream projects that SDE builds (Caja, Openbox, picom,
FLTK, the Graphite and Flat Remix themes) or uses from Debian (GLib, GTK,
poppler, libarchive, polkit, PAM, …) should also be reported upstream; tell us
when SDE ships an affected copy so that the pin can be updated.

## Privileged components

SDE programs run as the logged-in user. No SDE program is installed set-user-ID
or with file capabilities. Root privileges are only ever obtained through
polkit (pkexec, PackageKit, systemd-timedated), sudo in a terminal, or package
maintainer scripts. Every place that crosses that boundary passes **argument
vectors, never shell command lines** built from user or network data.

| Component | Runs as root through | Authorization | Notes |
| --- | --- | --- | --- |
| `/usr/sbin/debion-network-check` (`debion-desktop-base`) | `pkexec` from the panel ("Fix now") and Control Center → Network; `debion-network-repair.service` at boot; package scripts | polkit `org.debion.network-check.fix`: `auth_admin_keep` | Through pkexec only the report, quick check, WPAD lookup, `--fix`, `--dns-fallback [off]`, `--apt-proxy URL\|none` and `--boot-repair` run; `DEBION_*` path overrides are dropped. Proxy URLs must be a single line matching a strict pattern. Logs to `/var/log/debion-network-check.log`. |
| `/usr/libexec/debion-network-repair` | `pkexec` from the panel after login | polkit `org.debion.network-check.repair`: `auth_admin_keep`; `50-debion-network-repair.rules` allows active **local** members of `sudo` without a password | Takes no arguments; only runs `debion-network-check --boot-repair` (safe, idempotent repairs). |
| Software Updater (`sde-update`, `data/update/transaction.py`) | PackageKit (`pkcon`/`pkgcli`), or `pkexec /usr/bin/apt-get` / `/usr/bin/dpkg` | PackageKit's polkit policy, or `org.freedesktop.policykit.exec` (`auth_admin`) | The helper runs as the user and accepts only fixed actions; package names must match Debian Policy 5.6.1 completely. |
| Driver Manager (`sde-drivers`) | `pkexec /bin/sh -c SCRIPT sh SRC DST PKG…` | `org.freedesktop.policykit.exec` (`auth_admin`) | The script is a constant; paths and package names are positional arguments; only `/etc/apt/sources.list` or `/etc/apt/sources.list.d/debian.sources` can be replaced; package names are validated. |
| Welcome, System Reports | Welcome starts Synaptic through Synaptic's own `synaptic-pkexec`; System Reports has an install helper (`pkexec env DEBIAN_FRONTEND=noninteractive apt-get install -y PKG…`) that no page uses in 0.2.0 Alpha 1 | `auth_admin` | Package lists would be fixed in the program. |
| System Monitor | `pkexec kill -s SIG PID`, `pkexec renice` | `auth_admin` | Only for other users' processes. |
| Panel time zone sync | `timedatectl` (systemd-timedated asks polkit), or `pkexec timedatectl` after an explicit "Sync" | systemd's `org.freedesktop.timedate1.*` | Zone names are validated (`Area/City` syntax). The automatic hourly check only uses the HTTPS location service. |
| Liria "Open as Administrator" | `sudo liria` in a terminal | sudo (password) | `/etc/sudoers.d/sde-liria` (mode 0440) keeps only `DISPLAY` and `XAUTHORITY` for `/usr/bin/liria`. |
| Package scripts (`debian/*.postinst`) | dpkg | root | Network handover and repair at installation; backups in `/var/backups/debion-network/`; opt-out files documented in `docs/TROUBLESHOOTING.md`. |

Session services that hold secrets or act for other programs:

- **sde-lock** (screen lock, `org.freedesktop.ScreenSaver`): checks the password
  with PAM service `sde-lock` (`@include common-auth`, `common-account`). The
  password lives in a locked (`mlock`) buffer and is wiped after every attempt;
  `SetActive(false)` never unlocks; logind's `Lock`/`Unlock` signals are only
  accepted from logind itself; a rescue prompt appears when the lock screen
  keeps failing. Nothing about passwords is logged.
- **sde-polkit-agent**: the session's polkit authentication agent. The typed
  password is overwritten in the input buffer after it is handed to polkit.
- **sde-notifyd** (`org.freedesktop.Notifications`): bounds every field
  (summary, body, actions, image data); its history interface
  (`org.sde.Notifications`) refuses sandboxed callers (Flatpak, Snap).
- **sde-panel** as StatusNotifierWatcher/host: tray item names and paths from
  other programs are validated before they are used.

Test and QA hooks (`SDE_*_FIXTURE`, `SDE_PANEL_QA_DIR`, `SDE_DRIVERS_DEMO`,
`SDE_APT_SOURCES`, …) are read from the environment of the user's own
processes only and never reach a privileged helper.

## Hardening

- **Compiler and linker:** the Debian package builds with
  `DEB_BUILD_MAINT_OPTIONS = hardening=+all` (`debian/rules`): PIE,
  `-fstack-protector-strong`, `_FORTIFY_SOURCE`, `-fstack-clash-protection`,
  `-fcf-protection` (CET), full RELRO and `-z now`. The suite components
  (SDE WM, the compositor, Liria) are configured with the same environment
  (`CFLAGS`, `CPPFLAGS`, `LDFLAGS` from debhelper). SDE WM is a release (not
  debug) build since 0.2.0 Alpha 1, so `_FORTIFY_SOURCE` takes effect there
  too. `hardening-check` of a core build with these flags: PIE, full RELRO
  and immediate binding for every program, stack protector and fortified
  functions for every C++ program. CET marking (IBT/shadow stack) is only
  complete in `sde-xsettingsd`: the other C++ programs link SDE's Rust
  configuration library, whose objects (the Rust standard library among
  them) carry no CET notes, so the linker leaves the whole program unmarked
  (see A1-45 in the audit). The Rust parts have no third-party crates.
- **Warnings:** SDE's own code builds without warnings with `-Wall -Wextra`.
- **Sanitizers:** the unit and X11 tests pass with
  `-fsanitize=address,undefined` (see `docs/AUDIT-0.2.0-alpha1.md` for the
  procedure).
- **No shell for data:** programs are started with `posix_spawn`/`execvp`
  argument vectors; `.desktop` `Exec=` lines are split once and file names
  are inserted as separate arguments (no `sh -c`).
- **D-Bus:** names, object paths and UTF-8 strings from other programs are
  validated before they reach libdbus (which aborts on invalid input);
  byte arrays are kept compactly and received file descriptors are closed.
- **Files:** archives are extracted with libarchive's secure flags plus SDE's
  own checks (no absolute paths, no `..`, no symlink/hardlink escapes, no
  set-user-ID bits, no device files); desktop drag and drop never follows a
  symbolic link out of the target and never replaces a file without asking;
  temporary files use `mkstemp`/`O_EXCL|O_NOFOLLOW` in private directories.
- **Network:** weather, geocoding, IP location and time zone requests use
  HTTPS without downgrade on redirect (`curl --proto =https`); answers are
  parsed with depth and size limits and range-checked. The only plain-HTTP
  service (ip-api.com, a fallback for the IP location) can move the weather
  forecast but never sets the system time zone automatically.
- **CI:** GitHub Actions on current Node 24 action majors, read-only tokens
  except for the release and publish jobs, no expressions inside run scripts.

The audit of 0.2.0 Alpha 1, with every finding and how it was verified, is in
[`docs/AUDIT-0.2.0-alpha1.md`](AUDIT-0.2.0-alpha1.md).
