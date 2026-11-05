# Packaging: SDE Core and one package per application

Since 0.2.0 Alpha 4 the desktop is **SDE Core** (`sde-core`) and every SDE
application is a Debian package of its own, so that a problem or a crash of
an application, and its fix, concern one package. `sde-desktop` installs
all of them; the old `sde` package is a transitional package for upgrades.
Program names did not change.

## The packages

| Package | Architecture | What it holds |
| --- | --- | --- |
| `sde-core` | any | The desktop: `startsde`, `sde-session`, `sde-panel`, `sde-desktop`, `sde-settings` (`sde-control-center`), `sde-notifyd`, `sde-lock`, `sde-power`, `sde-run`, `sde-logout`, `sde-watchdog`, `sde-force-close`, `sde-update`, `sde-welcome`, `sde-drivers`, `sde-system-reports`, `sde-system-monitor`, `sde-display-settings`, `debrun` and `debrun-helper`, `sde-appimage`, `sde-autostart`, `sde-suite`, `sde-xsettingsd`, `sde-polkit-agent`, `xdg-desktop-portal-sde`, `sde-account-rename`, `sde-gl-probe`, `sde-theme-preview-gtk`, SDE WM (`sde-wm`), SDE compositor (`sde-compositor`); the SDE themes, icons, wallpapers, logo and avatars, the session file, `/etc/xdg/sde/sde.conf`, `/etc/xdg/sde-mimeapps.list`, the autostarts, the PAM service `sde-lock`, polkit actions, the D-Bus and portal files, man pages, translations and `/usr/share/doc/sde` (release notes, licenses of SDE-FLTK, Openbox, picom and the themes). Every program added later without a package of its own (for example the `sde-core` controller daemon) belongs here too. |
| `liria` | any | Liria, the file manager: `liria`, its desktop entry, AppStream file, man page and icons, `/etc/sudoers.d/sde-liria`. |
| `xanara` | any | Xanara, PDF viewer and editor: `xanara`. |
| `niny` | any | Niny, text editor: `niny`. |
| `merkadu` | any | Merkadu, software center: `merkadu`, `/usr/share/merkadu`, `/etc/apt/apt.conf.d/60merkadu`; its postinst adds Flathub. |
| `sde-calculator` | any | Calculator: `calculator` (desktop entry `org.debion.Calculator`). |
| `taca` | any | Taca, archive manager: `taca`. |
| `sde-photo` | any | Photo: `sde-view`, `/usr/share/sde/photo` (action plugins). |
| `sde-gallery` | any | Gallery: `sde-gallery`, `/usr/share/sde/gallery` (plugins, offline places). |
| `sde-desktop` | all | Metapackage: SDE Core, every application, GNOME Terminal, LightDM with Slick Greeter, Xorg, network, sound, Bluetooth, portals, removable media. Ships the login screen configuration (below). |
| `sde` | any | Transitional (Section: oldlibs): depends on `sde-core` and the applications that the old `sde` package contained (not Merkadu, which was separate). It can be removed after the upgrade. |
| `debion-desktop-base` | all | DebION GNU/Linux integration (artwork, network defaults), a separate project's package. |
| `*-dbgsym` | any | Debug symbols of each package with programs (`sde-core-dbgsym`, `liria-dbgsym`, ...), made by `dh_strip`. |

Each application package has its desktop entry (also in SDE's copy,
`/usr/share/sde/applications`), its AppStream file, man page and icons, and
its own `/usr/share/doc/<package>/copyright`, which the Control Center's
Applications page and `sde-suite APP --copyright` open (`data/suite.tsv`,
column 5).

### Dependencies

* The applications depend on what they need at run time, their shared
  libraries (`${shlibs:Depends}`), and **recommend** `sde-core`: they run on
  any X11 desktop and follow the SDE look when SDE Core is installed (SDE's
  theme palettes are compiled in; the logo, the translations and sde.conf
  come from SDE Core). A package depends on `sde-core (>= ${binary:Version})`
  only when it cannot work without a file of SDE Core.
* Helpers a single application uses are that application's: Taca recommends
  7-Zip and zip (`unrar` is suggested), Xanara Tesseract's English data,
  Photo and Gallery `cups-client` (printing with `lp`), Gallery Photo,
  Liria GVfs and UDisks (network places, trash and mounting through GIO).
* `sde-core` needs a terminal: `gnome-terminal | x-terminal-emulator`.
  xterm is only suggested since 0.2.0 Alpha 4.
* `sde-desktop` depends on `sde-core` and every application
  (`>= ${source:Version}`), `gnome-terminal`, `lightdm` and `slick-greeter`,
  and recommends `lightdm-settings` (Login Window).
* Merkadu refuses to remove `sde-core`, `sde-desktop`, `sde` and `merkadu`
  (`[Uninstall] protected=` adds more). Removing an application would also
  remove `sde-desktop`, so it is refused while the metapackage is installed.

## Which package a file goes to

The rule: **an application's install() rules say `COMPONENT <package>`;
everything else is SDE Core.**

The packages are CMake install components:

```cmake
install(TARGETS niny RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT niny)
install(DIRECTORY data/photo/ DESTINATION ${CMAKE_INSTALL_DATADIR}/sde/photo COMPONENT sde-photo)
```

An application's files in the shared `data/` folders are listed once, in
the top-level `CMakeLists.txt`:

```cmake
sde_app_data(niny DESKTOP niny.desktop METAINFO org.debion.Niny.metainfo.xml MAN niny.1 ICONS niny)
```

`DESKTOP` installs `data/applications/<id>` into `/usr/share/applications`
and `/usr/share/sde/applications`, `METAINFO` `data/metainfo/<file>`, `MAN`
`data/man/<page>` (section 1) and `ICONS` every size of
`data/icons/hicolor/*/apps/<name>.*`; the shared rules (`install(DIRECTORY
data/applications/ ...)` and the others) leave these files out.

An install() rule without `COMPONENT` is in CMake's default component,
`Unspecified`, which is the `sde-core` package. A new program, polkit action,
PAM service or settings page of the desktop needs no packaging change at
all. Do not name a component that is not a package: the build stops.

How `debian/rules` uses them:

1. `dh_auto_install` installs everything into `debian/tmp`.
2. `debian/install-components` installs each component into
   `debian/<package>` (`cmake --install obj-* --component <package>`, and
   `Unspecified` into `sde-core`) and logs what each package got. It stops
   when a component in `obj-*/install-components.txt` (written by CMake once
   every directory was read) is not a package of `COMPONENT_PACKAGES`, when a
   package has no component, and when two components install the same file.
3. `dh_missing --fail-missing` (debhelper compatibility level 13) then stops
   the build when a file of the complete installation in `debian/tmp` is in
   no package.

`debian/<package>.install` lists are only used for files that do not come
from CMake: `sde-desktop` takes the login screen files from `data/lightdm`,
`debion-desktop-base` its artwork from `data/desktop-base`.

To look at a split without building packages:

```sh
cmake --build build
cat build/install-components.txt
DESTDIR=/tmp/niny cmake --install build --component niny
```

### A new application package

1. Give its install() rules `COMPONENT <package>` and list its data with
   `sde_app_data()`.
2. Add a stanza to `debian/control` (Depends: `${shlibs:Depends}`,
   `${misc:Depends}`; Recommends: `sde-core`) and the package to
   `COMPONENT_PACKAGES` in `debian/rules`, to the Depends of `sde-desktop`
   and, if it should be protected, to Merkadu's list.
3. When files move from an existing package (for example from `sde-core`),
   the new package needs `Breaks:` and `Replaces:` of the old one (`<<` the
   first version without them, with a `~`, e.g. `sde-core (<< 0.2.0~beta1~)`),
   and the maintainer scripts of those files move with them.

### Conffiles and maintainer scripts

* A conffile that moves to another package is taken over by dpkg with its
  checksum (`Replaces`): an unmodified file is updated silently, a modified
  one is kept or asked about as for any conffile change.
* A conffile that a new version drops is removed with `rm_conffile` in
  `debian/<package>.maintscript` of the package that owned it. Conffiles of
  0.2.0~alpha3.1 and older belonged to `sde`: their removals go to
  `debian/sde.maintscript` (it already removes the alpha15 power manager
  and screen saver files).
* `startsde` registers the `x-session-manager` alternative in
  `sde-core.postinst` (removed in `sde-core.prerm`); `sde-account-rename.service`
  is enabled by `dh_installsystemd` in `sde-core`.

## Upgrading from 0.2.0~alpha3.1

Download every package (not the `-dbgsym` ones unless you want them, not
`debion-desktop-base` unless you use DebION) into an empty folder and run
`sudo apt install ./*.deb`. What happens:

* `sde` becomes the transitional package and pulls in `sde-core` and the
  seven applications it contained; `sde-core` and the applications have
  `Breaks:`/`Replaces: sde (<< 0.2.0~alpha4~)`, so dpkg moves every file
  from `sde` to its new package without conflicts. Merkadu was already a
  package of its own.
* The conffiles move with their files: `/etc/xdg/sde/sde.conf`,
  `/etc/xdg/sde-mimeapps.list`, `/etc/xdg/autostart/sde-*.desktop`,
  `/etc/pam.d/sde-lock` and `/etc/sde/appimage.conf` to `sde-core`,
  `/etc/sudoers.d/sde-liria` to `liria`. Changes made to them are kept.
* The `x-session-manager` alternative stays registered (by `sde-core`); an
  administrator's choice is kept. The account rename service stays enabled.
* `sde-dbgsym` gives way to the new `-dbgsym` packages
  (`Breaks`/`Replaces: sde-dbgsym (<< 0.2.0~alpha4~)`).
* The files of Liria's Caja fork (`/usr/lib/*/liria`, `libliria-extension`,
  `/usr/share/liria`, its schemas, `/usr/share/doc/sde/upstream/caja`) are
  removed: they are in no new package. The new Liria comes with `liria`.
* `sde-desktop` installs GNOME Terminal, LightDM and Slick Greeter. Its old
  LightDM GTK Greeter look (`/usr/share/lightdm/lightdm-gtk-greeter.conf.d/
  50-sde-desktop-greeter.conf`) is removed; an installed `lightdm-gtk-greeter`
  stays but is no longer used (`sudo apt remove lightdm-gtk-greeter` if you
  like). The next login shows Slick Greeter.
* `mate-desktop-common`, `unzip`, `bzip2`, `xz-utils`, `python3-gi`,
  `python3-gi-cairo`, `gir1.2-peas-1.0`, `gir1.2-gtk-3.0` and
  `gir1.2-gtksource-4` are no longer needed by SDE (and `xterm` is only
  suggested); packages that apt installed for the old `sde` may then be
  offered by `sudo apt autoremove` (`sudo apt-mark manual <package>` keeps
  one).
* After the upgrade `sudo apt remove sde` removes the transitional package
  and nothing else.

## The login screen (package sde-desktop)

* `/usr/share/lightdm/lightdm.conf.d/90-sde-desktop.conf`:
  `greeter-session=slick-greeter`, `user-session=sde-desktop`,
  `greeter-hide-users=false` (Debian's `01_debian.conf` hides the users),
  `greeter-show-manual-login=false` and `allow-guest=false`. LightDM reads
  `/usr/share/lightdm/lightdm.conf.d/*.conf` in order (90 comes after the
  greeters' own files), then `/etc/lightdm/lightdm.conf.d/*.conf` and
  `/etc/lightdm/lightdm.conf`, where the administrator's settings and Login
  Window's win.
* Slick Greeter reads its settings from GSettings and replaces any of them
  set in `/etc/lightdm/slick-greeter.conf` (Login Window writes that file).
  SDE's defaults are the vendor override
  `/usr/share/glib-2.0/schemas/40_sde-desktop.gschema.override`: the
  `SDE-Graphite-Dark-Blue` theme, `Papirus-Dark` icons, the login background
  of the active desktop-base theme (DebION's when `debion-desktop-base` is
  installed), the SDE mark at the bottom left (the 64 px `sde-logo` icon;
  the corner is too small for the full logo, which needs 200 px) and the
  clock format. A distribution changes them with a later override
  (e.g. `50_<distribution>.gschema.override`).
* The session badge of Slick Greeter is the SDE logo
  (`/usr/share/slick-greeter/badges/sde-desktop.svg`).
* A Debian live system: live-config's `lightdm` component writes
  `autologin-user=<live user>` and `autologin-user-timeout=0` into
  `/etc/lightdm/lightdm.conf`; with `user-session=sde-desktop` the live user
  starts SDE-Desktop directly. SDE does not set automatic login itself, so
  installed systems show the greeter (Control Center > Users & Avatar turns
  automatic login on for an account).

## The default terminal

GNOME Terminal is the terminal of SDE-Desktop:

* `sde-desktop` depends on `gnome-terminal`. Its postinst, on the
  installation and on the upgrade from an older `sde-desktop` only, points
  `x-terminal-emulator` to `/usr/bin/gnome-terminal.wrapper` when the
  alternative is in automatic mode and another terminal won by priority; a
  manual choice (`update-alternatives --config x-terminal-emulator`) is kept.
* SDE's own default is `[applications] terminal` in `sde.conf`, chosen in
  Control Center > Default Applications; `auto` uses `x-terminal-emulator`
  (shown there as GNOME Terminal), then GNOME Terminal and the usual
  terminals. The desktop's "Open Terminal Here", Terminal=true launchers and
  Ctrl+Alt+T (`sde-run --terminal`) follow it.
* Terminals have no MIME type, so `/etc/xdg/sde-mimeapps.list` does not name
  one.

## Building and publishing

`sh tools/build-deb.sh dist` builds all packages (see [BUILD.md](BUILD.md)),
runs the tests and lintian, and writes `SHA256SUMS.sha256` for every `.deb`
and the corresponding sources. CI (`.github/workflows/build.yml`) installs
every package except the debug symbols, `sde-desktop` (resolved with
`--simulate`) and `debion-desktop-base` (installed afterwards), checks that
each program comes from its package, and publishes every `.deb` with
`SHA256SUMS` as the `alpha-latest` pre-release; `release.yml` does the same
for tagged releases.
