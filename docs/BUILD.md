# Building SDE

## Supported systems

- DebION GNU/Linux / Debian testing (primary target)
- Debian 12 (bookworm) and Debian 13 (trixie)
- Ubuntu 22.04 LTS (jammy) and Ubuntu 24.04 LTS (noble)

All of them ship FLTK 1.3, CMake 3.16 or newer and a GCC with C++17 support.
0.1 Alpha was developed and screenshot-tested on Ubuntu 24.04; CI builds the
packages on all five distribution targets.

## Dependencies

Build (all from the distribution archive):

```
build-essential cmake pkgconf rustc cargo debhelper fakeroot lintian
libfltk1.3-dev libcairo2-dev librsvg2-dev libx11-dev libxft-dev libdbus-1-dev gettext python3
```

Core runtime (the `.deb` declares these): `openbox librsvg2-common dbus-x11 xdg-utils
x11-utils x11-xserver-utils x11-xkb-utils xinput fonts-dejavu-core`.

The `sde` package also requires the MATE suite engines, screen locker, Python APT
and PackageKit. `sde-desktop` installs the complete display/network/audio/media
stack. See [SUITE.md](SUITE.md) for exact scope and source credits.

Recommended for quick settings and the desktop: `network-manager`, `pulseaudio-utils`
(or PipeWire's `pactl`), `libglib2.0-bin` (move to trash), `libnotify-bin`.

Recommended: an icon theme (`papirus-icon-theme`, `adwaita-icon-theme` or
Eduka-Icons), a polkit agent (`policykit-1-gnome`, `mate-polkit` or `lxpolkit`),
a terminal and a display manager (`lightdm`). Liria/Caja is included with SDE.

Install everything with:

```sh
sudo tools/install-deps.sh             # build only
sudo tools/install-deps.sh --runtime   # plus runtime and recommended packages
sudo tools/install-deps.sh --preview   # plus Xvfb, xdotool, ImageMagick for screenshots
```

## Debian package

```sh
tools/build-deb.sh            # output in dist/
sudo apt install ./dist/sde_*.deb ./dist/sde-desktop_*.deb
```

The script builds in a temporary copy with `dpkg-buildpackage -us -uc -b` and runs
`lintian` on the result. The version comes from the `VERSION` file and
`debian/changelog`.

## Development build

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

Programs read their data from `/usr/share/sde`, so install the package (or run
`sudo cmake --install build`) once before testing binaries from `build/src`.

## Screenshots

```sh
tools/screenshot.sh                          # uses the installed SDE
SDE_BIN=build/src tools/screenshot.sh /tmp/shots
```

It starts SDE on a virtual X server (Xvfb, 1366x768) with a temporary home
directory and saves PNG files to `docs/screenshots/`. Run it as a normal user.

## Unit tests

The Rust workspace requires Rust 1.59 or later and has no registry dependencies.
CMake builds it with `--locked --offline` into `build/cargo`. The installed
programs do not require a Rust toolchain. For Rust-only work:

```sh
cargo test --workspace --locked --offline
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked --offline -- -D warnings
```

Install `rustfmt` and `rust-clippy` separately if using the distribution toolchain
for those developer checks. See [RUST.md](RUST.md).

`ctest --test-dir build` runs the tests in `tests/` (desktop entries, command
parsing, D-Bus values, UI scale and WCAG contrast of every theme and accent).
It also runs the Rust unit and autostart CLI integration tests.
`dpkg-buildpackage` runs them too. `build/tests/sde-sni-demo` is a small
StatusNotifierItem with a menu for testing the tray by hand.

## Releases

Push a tag such as `v0.1.0-alpha2` (or run the **Release** workflow by hand).
`.github/workflows/release.yml` builds the package in Debian 12, Debian 13,
Ubuntu 22.04 and Ubuntu 24.04 containers, names each one after its distribution
(`sde_0.1.0-alpha2_debian12_amd64.deb`, ...), writes `SHA256SUMS` and publishes a
GitHub pre-release with the newest `CHANGELOG.md` section as notes.

Locally, `tools/build-deb.sh --suffix deb12` gives the package a distribution
version suffix (`0.1.0~alpha2+deb12`).

## Continuous integration

`.github/workflows/build.yml` builds and lints the package in Debian 12, Debian 13,
Ubuntu 22.04 and Ubuntu 24.04 containers and uploads the `.deb` files as artifacts.
