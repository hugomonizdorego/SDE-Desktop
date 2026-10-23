# SDE 0.2.0 Alpha 1: release audit

The release audit of SDE 0.2.0 Alpha 1: builds, compiler and sanitizer
runs, static analysis, a security review of the code that crosses a trust
boundary, the upstream pins, and the test suites at two screen sizes. Real
problems were fixed with small commits and tests; the rest is listed under
[Open items](#open-items) with a recommendation.

- **Base:** `claude/kind-goodall-dktuun` at 0.1.0~alpha52 (`636e15c`), with
  the About/legal work (`263af2b`) and the branding, Liria and suite work
  (`8882c8f`, up to `df408bf`) merged during the audit and reviewed with it.
- **Environment:** Ubuntu 24.04.5 container (the CI images are Debian 12/13
  and Ubuntu 24.04), GCC 13.3, Clang 18.1.3, cppcheck 2.13, clang-tidy 18,
  ShellCheck 0.9.0, pyflakes 4.0, bandit 1.9.4, poppler-glib 24.02,
  libdbus 1.14.10, GLib 2.80, polkit 124.
- **Severity:** *High*: code execution or privilege escalation;
  *Medium*: memory-safety bugs reachable from untrusted input (files,
  other programs on the bus, the network) or a weakened privilege boundary;
  *Low*: denial of service, defense in depth, information that should not
  leak; *Info*: build, CI and hygiene.

## Summary

| Severity | Found | Fixed | Open |
| --- | --- | --- | --- |
| High | 1 | 1 | 0 |
| Medium | 8 | 8 | 0 |
| Low | 19 | 12 | 7 |
| Info | 19 | 7 | 12 |

The one High finding needed code already running in an administrator's
session (and an approval that polkit kept, or the administrator approving
the prompt) to become root. Only one finding was reachable from the network
without user action: the plain-HTTP fallback could set the time zone
(A1-10). The Medium findings need a crafted file opened by the user, a
program in the same session (including sandboxed apps), or another local
user on a system without `pam_systemd`.

## Findings

Status gives the fixing commit on the audit branch (the integrator's merge
keeps them). "Verified" says how the fix was checked.

| ID | Severity | Component | Finding | Status | Verified |
| --- | --- | --- | --- | --- | --- |
| A1-01 | High | `debion-network-check` (root via pkexec) | `--apt-proxy URL` checked the URL with `grep` line by line: a value with a newline passed with its first line, and the rest was written as root into `/etc/apt/apt.conf.d/80debion-proxy` (for example `APT::Update::Pre-Invoke`), i.e. root command execution. The polkit action (`auth_admin_keep`) covered every mode, including package-script modes and test building blocks such as `--wifi-profiles DIR` (root-owned files in any directory, following a dangling symlink). | Fixed `3e4bc45`: single-line URLs matched completely (`grep -x`, CR/LF refused); through pkexec only report, quick check, WPAD, `--fix`, `--dns-fallback`, `--apt-proxy` and `--boot-repair` run. | `tests/test_network_check.py` (`AptProxy`: valid URL, removal, newline/CR/quote injection, pkexec mode refusal); 53 tests pass. |
| A1-02 | Medium | libdbus wrapper (`src/common/dbus.*`), `sde-panel` | libdbus aborts the process on an invalid bus name, object path or UTF-8 string. Any session client could kill the panel: `RegisterStatusNotifierItem("/bad//path")`, or a tray item whose `Menu` property is not a valid path. | Fixed `8918fe6`: `dbus::method()`/`signal()` validate names and paths and return null; `Writer` ignores a null message, sends invalid UTF-8 as U+FFFD and an invalid path as `/`; the SNI watcher drops invalid registrations (at most 256 items). | `sde-tests` `test_dbus_hardening`; a probe that sent the bad registration aborted the old panel with SIGABRT and is ignored now. |
| A1-03 | Medium | `sde-notifyd` (`rgba_pixels`) | The `image-data` hint's `channels` was trusted: `channels = 2^30` overflowed `w * channels` and the copy read far outside the message (crash, or memory of the daemon drawn into the notification). | Fixed `8918fe6`: only 8-bit RGB/RGBA with consistent stride and length, at most 4096×4096. | `sde-tests` `test_rgba_pixels`. |
| A1-04 | Medium | `sde-notifyd` history API | `org.sde.Notifications` (history, `InvokeAction`, `Dismiss`, `Clear`) lives on the same connection as `org.freedesktop.Notifications`. A Flatpak app allowed to talk to the notification service reaches it through its D-Bus proxy and could read every program's notifications or trigger their actions. | Fixed `8918fe6`: sandboxed callers (Flatpak `/.flatpak-info`, Snap AppArmor label, looked up by the caller's PID) get `AccessDenied`. Field sizes are bounded (summary 1 KB, body 16 KB, 16 actions, …) and `desktop-entry` must be an ID. | `test_dbus_hardening` with a fake `/proc` tree (Flatpak, Snap, host). |
| A1-05 | Medium | Gallery/Photo metadata (`src/gallery/exif.cpp`) | `read_webp()` compared the `Exif\0\0` prefix also when the EXIF chunk was shorter than 6 bytes; `len - 6` wrapped and the TIFF parser read out of bounds (crafted image: crash or garbage). `icc_description()` read the `mluc` header of a tag shorter than 16 bytes past the profile end. | Fixed `29dcf2b`. | `test_gallery`: short EXIF chunk, 12-byte `mluc` at the end of a profile. |
| A1-06 | Medium | Xanara (poppler-glib) | `poppler_annot_stamp_set_custom_image()` (poppler 24.02; the loop is the same in other versions) reads each pixel's alpha after stepping to the next pixel: 3 bytes past the image. Large images are `mmap()`ed and can end at a page boundary, so adding a stamp, drawing or signature could crash. | Fixed `f456991` (work-around): poppler gets an ARGB32 copy with 64 spare bytes. To report upstream. | AddressSanitizer run of `sde-xanara-tests` (heap-buffer-overflow before, clean after). |
| A1-07 | Medium | Xanara links | URI links of any scheme (`file:`, `smb:`, custom handlers) and `Launch`/`GoToR` actions (any file next to the document) were passed to `xdg-open` on one click. | Fixed `554e59f`: web and mail links open directly; others show their target and open only after **Open**, as other PDF viewers do; PDFs still open in Xanara. | Build, `sde-xanara` tests, code review. |
| A1-08 | Medium | `runtime_dir()` fallback, `write_file_atomic()` | Without `$XDG_RUNTIME_DIR` (no `pam_systemd`), `/tmp/sde-$UID` was used even when another user had created it (or a symlink) first. The Driver Manager writes the new APT sources there for root to install; Niny's socket and the session pid file live there too. `write_file_atomic()` opened its temporary file with `fopen("wb")`, following a planted symlink. | Fixed `1ae4803`: only a real directory owned by the user (mode tightened to 0700), else `~/.cache/sde-runtime`; temporary files `O_CREAT\|O_EXCL\|O_NOFOLLOW`. | `sde-tests` `test_runtime_dir_fallback` (symlink, loose mode, other owner, link at the temporary name). |
| A1-09 | Medium | SDE WM (Openbox 3.6.1) | Upstream fixes missing: `client_calc_layer()` walked `stacking_list` while restacking (use-after-free with GLib ≥ 2.76, Debian #1033385); `screen_pointer_pos()` reused its loop variable (endless loop with more than two X screens, Debian #983882); `DestroyImlibLoader()` freed an image it did not load (Debian #1007147). | Fixed `608e283` (`suite/patches/sde-wm/0002-upstream-crash-fixes.patch`). | `tools/prepare-components.py` applies 0001–0003 to `release-3.6.1`; built with `tools/build-session-components.py` (Debian build flags); `tools/ui-smoke.py` ran with it. |
| A1-10 | Low | Panel network fetches (`weather.cpp`, `timezone.cpp`, `art.cpp`) | HTTPS requests could be redirected to plain HTTP. The automatic hourly time-zone check fell back to `ip-api.com`, which only answers over plain HTTP, so anyone on the network path could choose the system time zone. Album-art URLs come from any media player on the bus. | Fixed `b4a0c60`: `--proto`/`--proto-redir =https` for HTTPS URLs, `--max-filesize`, `--` before the URL; the automatic zone check uses HTTPS services only (a **Sync** the user presses may still use the fallback and shows the result); album art: web protocols only. | curl 8.5 accepts the options and refuses plain `http:` and `file:` URLs with them; `sde-weather-tests`, `sde-timezone-tests`. |
| A1-11 | Low | libdbus wrapper | Received `UNIX_FD` arguments were never closed (one descriptor leaked per message: descriptor exhaustion); each byte of an `ay` array became a ~120-byte value (a 10 MB `image-data` hint cost more than 1 GB). | Fixed `8918fe6`: descriptors closed, byte arrays kept as one block. | `test_dbus_hardening` (fd closing, byte arrays). |
| A1-12 | Low | `sde-lock` daemon | logind's `Lock`/`Unlock`/`PrepareForSleep` handlers checked only the object path; the match rules already limited the sender, but any broader match rule on the connection later would have let a forged `Session.Unlock` end the lock. | Fixed `e4a4605`: the sender must be the current owner of `org.freedesktop.login1` (looked up again after a logind restart). | Build, lock fixture tests, code review. |
| A1-13 | Low | Lock screen, rescue prompt, polkit agent | Password copies were left in freed memory: a `push_back()` of the NUL reallocated the vector without wiping the first copy; the polkit agent's secret input kept the text in its buffer after `value("")`. | Fixed `c4380ae`: one allocation including the NUL, overwritten before clearing. | Build, lock UI fixture tests, review. |
| A1-14 | Low | Driver Manager (`sde-drivers`) | Defense in depth for the root step (`pkexec /bin/sh -c SCRIPT sh SRC DST PKG…`, arguments never pasted into the script): any string was accepted as a package name, and any destination path for the edited sources. | Fixed `572609a`: Debian package names only (Policy 5.6.1, so never an option), destination `/etc/apt/sources.list` or `/etc/apt/sources.list.d/debian.sources` only. | `test_sysreport` (invalid names, destinations). |
| A1-15 | Low | Software Updater helpers (`data/update/*.py`) | `re.match()` with a pattern ending in `$` accepts a trailing newline (`"bash\n"`). Nothing reached a shell (argument vectors only). | Fixed `9f82914`: `fullmatch()`. | `tests/test_updates.py`. |
| A1-16 | Low | SDE WM build | Openbox's `configure` turns every version containing "alpha"/"beta" into a debug build (`-O0 -DDEBUG`, assertions on): SDE WM 1.0.0~alpha1 shipped unoptimized and `_FORTIFY_SOURCE` had no effect. | Fixed `608e283` (`0003-release-build-for-alpha-versions.patch`). | `hardening-check sde-wm`: PIE, stack protector, FORTIFY, RELRO, BIND_NOW, stack clash and CET all "yes". |
| A1-17 | Low | Niny popup | `run()` read `levels_[0]` after a loop that also ends when `levels_` is empty (cppcheck `containerOutOfBounds`). | Fixed `fb1300b`. | Build, Niny tests. |
| A1-18 | Low | Photo side panel | Seven heap `std::function` callback objects were never freed (224 bytes per window, LeakSanitizer). | Fixed `8ecaa00`. | ASan build of `sde-view` opened and closed on Xvfb: no leaks besides suppressed fontconfig caches. |
| A1-19 | Low | `sde-lock --ui`; tests | The lock UI never deleted its full-screen windows (2 MB backgrounds each) at the end; LeakSanitizer turned the fixture's exit status into a failure. The DnD test read the layout dump too early under load; the Xanara tests leaked annotation references. | Fixed `40ff128`. | ctest under ASan+UBSan. |
| A1-20 | Info | CI (`.github/workflows`) | `actions/checkout`, `upload-artifact`, `download-artifact` v4 run on the deprecated Node 20; default token permissions; matrix values expanded with `${{ }}` inside `run:` scripts. | Fixed `5703ad2`: checkout v7, upload-artifact v7, download-artifact v8 (node24); `permissions: contents: read` (write only for publish/release); `persist-credentials: false` where nothing is pushed; values through `env`. | Tags and `action.yml` (`using: node24`) checked in the upstream repositories. |
| A1-21 | Info | Build warnings | ~380 GCC `-Wcast-function-type` warnings per program from four SDE-FLTK header lines, and a few in SDE's code; Clang `-Wmismatched-tags` from `Fl_SDE_Style.H` and three unused entities (Taca, Niny). | Fixed `48b0bef`, `03ed484`. | GCC 13 and Clang 18 builds with `-Wall -Wextra`: no warnings in SDE's code. |
| A1-22 | Info | System Monitor | Unfinished item: the graphs, chips, search field and scroll area kept the colors of the theme they started with. | Fixed `96f9844`: follows `_SDE_CONFIG_SERIAL` like the other programs. | Xvfb: light/blue → dark/green through `sde.conf` + `_SDE_CONFIG_SERIAL`, screenshots before/after. |
| A1-23 | Info | UI tests (`tools/panel-smoke.py`, `tools/ui-smoke.py`) | Known flaky checks: "tray not docked" kept the first answer for the icon's parent (the root window, before the dock); after a screenshot grab the search/menu were typed into after a fixed pause; Escape was pressed before a popup had the keyboard. Commands that "got lost" were a product bug (A1-47). | Fixed `fdef382`, `11c317f`, `93c2cc4`: ask for both parents every time; popups mark `<SDE_PANEL_QA_DIR>/<title>.input` once they hold their grabs and the focus, and the tests type or press Escape only then (and wait until the popup is gone). A screenshot (`debug-failure.png`) is saved at any failed wait. | panel-smoke and ui-smoke at both sizes. |
| A1-24 | Info | `tools/ui-smoke.py` | Program logs (GTK, X and D-Bus warnings) were only kept when a run failed. | Fixed `fb1300b`: always copied to `<output-dir>/logs`. | ui-smoke runs. |
| A1-40 | Info | SDE Graphite theme assets | Unfinished item: the PNG assets Graphite ships in its accent color (the knob of scales with marks, the checked selection-mode check box) kept upstream's accent (#1A73E8 for blue, #FB8C00 for orange) next to SDE's calmer CSS-drawn accents. | Fixed `7a5c7bd`: `tools/build-themes.py` moves the palettes of those 20 indexed PNGs per accent to the SDE accent (edges keep their alpha; grays, shadows and disabled assets stay; no new build dependency). | `tests/test_theme_assets.py` (synthetic palettes, CRCs, all nine upstream accents); GTK 3.24 on Xvfb with SDE-Graphite-Light-Blue and -Dark-Orange: the marks sliders match the plain sliders. |
| A1-42 | Info | Gallery | "Copy/Move to Folder" showed "Moving 0 photos…": `steps.size()` and `std::move(steps)` were arguments of the same call, and GCC builds the by-value parameter first (clang-tidy `bugprone-use-after-move`; the vector itself was used correctly). | Fixed `11c317f`. | Build; review. |
| A1-46 | Low | `sde-panel` popups | The panel closes its popups when another window becomes active (after a 500 ms grace period). The Action Center and the flyouts do not grab the pointer, and when one takes the keyboard Openbox sets `_NET_ACTIVE_WINDOW` to None ("focus went to a black hole"); when that change arrived after the 500 ms (a busy system), the popup closed right after it opened. | Fixed `93c2cc4`: only a real window becoming active closes the popups. | Without a window manager, `_NET_ACTIVE_WINDOW` set to 0 0.8 s after opening the Action Center: closed 3 of 3 times before, open 3 of 3 after; now a panel-smoke check. |
| A1-47 | Low | `sde-panel --menu`, `--quick-settings`, `--calendar`, `--section`, `--show-desktop` | These commands (Super, Super+A, Super+C, search results) set a root property or send a client message and exited right after `XFlush()`. Under load the X server sometimes closed the client before it had read the request, so the command never reached the panel: Super did not open the menu, ui-smoke failed in 3 of 3 runs with "window not found" after a command, and panel-smoke had retried commands since alpha14 ("~2 in 80"). | Fixed `106a6ba`: `XSync()` (a round trip) before exiting. | Xvfb with six busy loops: 60 runs gave 55 `_SDE_COMMAND` PropertyNotify events before (`xev -root`), 60 of 60 and 100 of 100 after; opening the Action Center right after a configuration reload failed 4–6 times in 24 before, 0 in 24 after. |

### Open items

| ID | Severity | Component | Finding | Recommendation |
| --- | --- | --- | --- | --- |
| A1-25 | Low | Driver Manager | The root step is `pkexec /bin/sh -c …`: polkit shows its generic "run `/bin/sh` as the super user" message, so the user cannot see what is approved. | A dedicated helper (`/usr/libexec/sde-drivers-apply`) with its own polkit action and message, like `debion-network-check`. |
| A1-26 | Low | `org.debion.network-check.fix` | `auth_admin_keep`: for five minutes after one approval, any process of the user can run the desktop actions again without a prompt (after A1-01 these are fixed repairs, DNS fallback, and an APT proxy that must be a plain URL; APT still checks signatures). | Use `auth_admin` for `--apt-proxy`, or a separate action for it. |
| A1-27 | Low | `sde-panel` SNI host | Tray items choose absolute icon paths and `IconThemePath`. Only regular files are read, but every name stays in the icon cache (unbounded) and large PNGs are decoded at full size: a session client can make the panel use a lot of memory. | Bound the cache, skip files over a few MB and images over 1024×1024. |
| A1-28 | Low | `src/common/process.cpp` | `stop_stream()` and `run_sync()` wait for the child without `WNOHANG` after `kill()`. A child that changed its user (a pkexec target) cannot be signalled, so the wait would block the program. No caller combines them today (pkexec always goes through `run_async`). | Keep pkexec on `run_async`; or reap asynchronously when `kill()` fails. |
| A1-29 | Low | Weather | The approximate location still falls back to `ip-api.com` over plain HTTP when the HTTPS services fail. It can only move the forecast area (range-checked), never the time zone (A1-10). | An HTTPS-only fallback, or none. |
| A1-30 | Low | Lock screen, polkit agent | FLTK's `Fl_Input` keeps an undo buffer; the code avoids `replace()`, but toolkit buffers are not locked in memory. | Accepted for an alpha; a dedicated password field without undo in SDE-FLTK later. |
| A1-31 | Info | SDE compositor (picom v12.5, `a456d43`) | picom v13 (2026-02-07) fixes crashes (negative `blur-opacity` #1493, an assertion when no window is focused #1384, shadow image size #257) but changes behavior (shadow opacity applied once, background no longer stretched, options removed). No CVE. | Move to v13 in 0.2.0 Alpha 2 with a visual review of the SDE compositor configuration. |
| A1-32 | Info | SDE WM (Openbox 3.6.1) | Upstream has `release-3.7-rc2` (git only, no release). The relevant crash fixes are backported (A1-09). | Stay on 3.6.1 + patches. |
| A1-33 | Info | Liria (Caja 1.28.0), SDE-FLTK (FLTK 1.3.11), themes | Caja 1.28.0 and FLTK 1.3.11 are the current stable releases of their series (Caja 1.29.1 is a development release; FLTK 1.4.5 changes the API); no security advisories against them. The theme pins are data (SCSS/SVG/PNG). | Nothing to bump. |
| A1-34 | Info | poppler-glib | Besides A1-06, annotations returned by `poppler_annot_*_new()` must be unreferenced by the caller (the tests leaked them, fixed in A1-19). | Report the stamp-image read upstream. |
| A1-35 | Info | Rust crates | `cargo audit` does not apply: `Cargo.lock` lists only the four workspace crates (no registry dependencies). | — |
| A1-36 | Info | Hardening of the GTK suite components | `hardening-check` of Liria and the compositor could not run here (they need newer MATE and libconfig than Ubuntu 24.04 has); both are configured with the `dpkg-buildflags` environment by `debian/rules`. | Run `hardening-check` on the Debian 13 CI packages. |
| A1-37 | Info | Packaging | A full `dpkg-buildpackage` (with the native suite) is not possible in this container; the core was built with the `debian/rules` flags instead (see [Builds](#builds-and-hardening)) and the source package was checked with lintian (A1-43). | Run lintian on the built packages in CI. |
| A1-38 | Info | Static-analysis leftovers | cppcheck `noCopyConstructor` for `OtherTypesDialog` (never copied), two `containerOutOfBounds` false positives (`niny/markdown.cpp`, `panel/controls.cpp`); pyflakes: unused variables in test tools; bandit B324 (SHA-1 for Git object IDs in `bootstrap/fetch_source.py`, as Git's format requires; sizes and SHA-256 are checked too). | No action needed. |
| A1-39 | Info | WM caption buttons at 150 % | Design item, not changed: `sde-session` scales the title font with the desktop scale (`sync_openbox()`), so caption buttons grow, but the glyph masks stay 1-bit 14 px bitmaps (`soft_caption_masks()`): at 150 % the close disc and glyphs look small and jagged in the larger buttons. | Generate the masks from the same geometry at 21 px (150 %) and 28 px (200 %) into Openbox-only theme variants (`<theme>-150/openbox-3`, `<theme>-200/openbox-3`, themerc linked to the 100 % one), and let `sync_openbox()` pick the variant from `Theme::scale` (≥ 1.25 → 150, ≥ 1.75 → 200). Theme pickers must skip directories without `gtk-3.0`. Antialiased glyphs would need image buttons in SDE WM. |
| A1-41 | Info | About/legal and branding merges | Reviewed `src/common/legal.*`, `about.*`, `data/legal/components.json`, `tools/legal-data.py`, the branding tools and install rules: local, read-only data; JSON parsed with size limits; no new privileged or network code. | — |
| A1-43 | Info | Debian source package | `lintian --pedantic` on the source package (`dpkg-source -b` of a clean export): no errors; warnings: `debian/copyright` names `vendor/*` paths that only exist after `tools/prepare-*.py` (`superfluous-file-pattern`), the AppStream files say `CC0-1.0` while `debian/copyright` gives `data/metainfo/*` GPL-3.0-or-later (`inconsistent-appstream-metadata-license`), the CC-BY-SA-4.0 text is abbreviated, and `xterm \| x-terminal-emulator` and `xdg-user-dirs` are in both Depends and Recommends of `sde`. | For the packaging/legal owners: a `Files: data/metainfo/*` stanza with `CC0-1.0`, the full CC-BY-SA-4.0 text (or a pointer to `/usr/share/common-licenses` when Debian ships it), and dropping the duplicates from Recommends. |
| A1-44 | Info | Liria "Open as Administrator" | Runs `x-terminal-emulator -e sudo liria --no-desktop PATH` (argument vector; Debian requires xterm-style `-e`). A whole GTK program as root on the user's X display can be driven by every other X client of the session (X11 does not isolate clients). | Prefer GVfs `admin://` (polkit per operation, the file manager stays unprivileged) when `gvfs-backends` is installed. |
| A1-45 | Low | CET marking of the C++ programs | 22 of the 23 C++ programs are not marked for CET (IBT/shadow stack) although they are compiled with `-fcf-protection`: they link SDE's Rust configuration library, whose objects carry no CET notes (stable Rust cannot build them with `-Z cf-protection`). User-space shadow stacks need Linux 6.6+, a CET-enabled glibc and marked programs; an unmarked program simply runs without them, so nothing breaks, but SECURITY.md must not claim CET. | Keep the Rust library out of programs that do not need it, or link it into one helper; revisit when Rust's `cf-protection` is stable. SECURITY.md says so. |

## Security review notes

Reviewed and found sound (no change needed):

- **Process start:** `posix_spawn` with argument vectors everywhere
  (`src/common/spawn.cpp`, `process.cpp`); `.desktop` `Exec=` lines are split
  once, field codes become separate arguments, never `sh -c`. pkexec targets
  are absolute paths of fixed helpers; their arguments are validated
  (zones, package names, paths, signals and PIDs).
- **Archives (Taca):** libarchive's secure extraction flags plus SDE's checks
  (no absolute paths, `..`, symlink/hardlink escapes, set-user-ID bits or
  device files).
- **Desktop drag and drop and file operations:** `O_EXCL`,
  `RENAME_NOREPLACE`, links never followed out of the target, names of link
  entries sanitized.
- **Lock screen:** PAM conversation handles only echo-off prompts with the
  locked buffer; `SetActive(false)` from other programs never unlocks; an
  expired password (`PAM_NEW_AUTHTOK_REQD`) still unlocks, an expired
  account does not.
- **Polkit agent:** registers for the session only; identities come from
  polkit; the cookie is passed back unchanged.
- **Niny:** its instance socket is created with umask 077 in the runtime
  directory (see A1-08).
- **QA and test hooks** (`SDE_*_FIXTURE`, `SDE_PANEL_QA_DIR`,
  `SDE_DRIVERS_DEMO`, `SDE_APT_SOURCES`, …) are environment variables of the
  user's own processes and never reach a privileged helper (pkexec clears
  the environment).
- **Packaging:** `/etc/sudoers.d/sde-liria` is mode 0440 and keeps only
  `DISPLAY`/`XAUTHORITY`; maintainer scripts back up what they change.

## Builds and hardening

| Build | Result |
| --- | --- |
| GCC 13, Release, `-Wall -Wextra` (`CMakeLists.txt`) | no warnings in SDE's code (after A1-21) |
| Clang 18, Release, `-Wall -Wextra` | no warnings in SDE's code (after A1-21) |
| `dpkg-buildflags` with `hardening=+all` (as `debian/rules`), core programs | builds; `hardening-check` below |
| SDE WM with `dpkg-buildflags` (`tools/build-session-components.py`) | `hardening-check`: all "yes" (after A1-16) |

The core (`-DSDE_BUILD_NATIVE_SUITE=OFF -DSDE_BUILD_COMPONENTS=OFF`) was
configured with the environment debhelper gives CMake: `dpkg-buildflags`
with `DEB_BUILD_MAINT_OPTIONS="hardening=+all optimize=-lto"` (no LTO, as on
Debian; Ubuntu enables it by default), `DEB_CXXFLAGS_MAINT_APPEND=-O2`, and
`CPPFLAGS` appended to `CFLAGS`/`CXXFLAGS`. Installed with `DESTDIR`,
`hardening-check` over the 25 programs:

| Protection | Result |
| --- | --- |
| PIE, full RELRO, immediate binding | 25 of 25 |
| Stack protector, fortified functions | 23 of 23 C++ programs; "no" for the two Rust programs (`sde-autostart`, `sde-suite`), as expected for Rust |
| CET (IBT/shadow stack) | 1 of 25 (`sde-xsettingsd`): see A1-45 |
| Stack clash protection | "unknown" everywhere (`hardening-check` cannot tell on x86-64) |

A1-45 in detail: every object of SDE (`libsde-common.a`: 26 of 26) and of
SDE-FLTK (`libfltk.a`: 157 of 157) carries the `IBT, SHSTK` property, but
the C++ programs also link `libsde_config_ffi.a`, whose 309 objects (the
Rust standard library among them) carry none, so the linker clears the
marking for the whole program. `sde-xsettingsd` does not link it.

## Sanitizers

The unit tests, the Xvfb tests and the UI fixtures were run with
AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer:

```sh
san="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSDE_BUILD_TESTS=ON \
  -DSDE_BUILD_NATIVE_SUITE=OFF -DSDE_BUILD_COMPONENTS=OFF \
  -DCMAKE_C_FLAGS="$san" \
  -DCMAKE_CXX_FLAGS="$san -fno-sanitize=vptr -fno-sanitize-recover=undefined" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j3
cd build-asan && ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  LSAN_OPTIONS=suppressions=$PWD/../lsan.supp ctest --output-on-failure -j2
```

`-fno-sanitize=vptr` is needed because some tests link only part of a
program; `-fno-sanitize-recover=undefined` makes every UBSan report fail
its test. The LeakSanitizer suppressions (`lsan.supp`) cover third-party
caches that live as long as the program:

```
leak:libfontconfig.so
leak:libexpat.so
leak:libXft.so
leak:cairo_toy_font_face_create
leak:Fl_Font_Descriptor
leak:XOpenDisplay
leak:_XimOpenIM
leak:XCreateIC
leak:libpoppler.so
leak:libpoppler-glib.so
```

Result: 38/38 tests pass (before the About/legal merge added
`sde-legal-data`), no UndefinedBehaviorSanitizer reports; the findings of
the run are A1-06, A1-18 and A1-19.

## Static analysis

| Tool | Scope | Result |
| --- | --- | --- |
| cppcheck 2.13 (`--enable=warning,performance,portability --std=c++17`) | `src/`, `tests/` | 3 `containerOutOfBounds` (1 real: A1-17), 1 copy-constructor note, performance notes (`passedByValue`, `substr`) |
| clang-tidy 18 (`bugprone-*`, `cert-*`, `clang-analyzer-*`, noisy checks off) | 250 files of `src/`, `tests/` | 271 diagnostics, 1 real bug (A1-42). The rest: `atoi()` in configuration and theme parsing where 0 is the wanted fallback (`cert-err34-c`, 95); FLTK widgets owned by their parent group reported as leaks (`NewDeleteLeaks`, 82); integer division and rounding in drawing code (37); `system()` in tests (14); a dozen path-sensitive reports checked by hand and found infeasible (size formatters, Xanara note placement, popup QA path, ...) and style notes |
| ShellCheck 0.9.0 | `data/`, `tools/`, maintainer scripts | style notes only (`A && B \|\| C`, unused variables in a GRUB theme file) |
| pyflakes | Python helpers and tools | unused variables/imports in test tools |
| bandit 1.9.4 (medium and high) | `data/update`, `bootstrap`, `tools` | B324 (Git SHA-1, expected), B310 (fixed HTTPS URL), B108 (X socket path): no issue |

## Tests run

TEST_RESULTS

## Upstream pins

| Component | Pin | Newest upstream | Decision |
| --- | --- | --- | --- |
| SDE WM ← Openbox | `release-3.6.1` (`249020d`) + 3 patches | `release-3.7-rc2` (git tag only) | backports (A1-09, A1-16) |
| SDE compositor ← picom | v12.5 (`a456d43`) | v13 (2026-02-07) | Alpha 2 (A1-31) |
| Liria ← Caja | 1.28.0 (`27fdb84`) | 1.28.0 (1.29.1 development) | current |
| SDE-FLTK ← FLTK | 1.3.11 | 1.3.11 (1.4.5 in the next series) | current |
| Display settings ← DisplayProfileManager | `c873b66` | — | data/model port only |
| SDE Graphite/Fluent ← Graphite/Fluent GTK themes | `suite/themes.lock.json` | — | data only |
| Rust | 4 workspace crates | — | no registry crates |
