# Building SDE

## Supported systems

- Debian 12 (bookworm) and Debian 13 (trixie)
- Ubuntu 22.04 LTS (jammy) and Ubuntu 24.04 LTS (noble)

All of them ship FLTK 1.3, CMake 3.16 or newer and a GCC with C++17 support.
0.1 Alpha was developed and screenshot-tested on Ubuntu 24.04; CI builds the
package on all four.

## Dependencies

Build (all from the distribution archive):

```
build-essential cmake pkgconf debhelper fakeroot lintian
libfltk1.3-dev libcairo2-dev librsvg2-dev libx11-dev libxft-dev
```

Runtime (the `.deb` declares these): `openbox librsvg2-common dbus-x11 xdg-utils
x11-xserver-utils fonts-dejavu-core`.

Recommended: an icon theme (`papirus-icon-theme`, `adwaita-icon-theme` or
Eduka-Icons), a polkit agent (`policykit-1-gnome`, `mate-polkit` or `lxpolkit`),
a terminal, a file manager (`pcmanfm`) and a display manager (`lightdm`).

Install everything with:

```sh
sudo tools/install-deps.sh             # build only
sudo tools/install-deps.sh --runtime   # plus runtime and recommended packages
sudo tools/install-deps.sh --preview   # plus Xvfb, xdotool, ImageMagick for screenshots
```

## Debian package

```sh
tools/build-deb.sh            # output in dist/
sudo apt install ./dist/sde_0.1.0~alpha1_amd64.deb
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

## Continuous integration

`.github/workflows/build.yml` builds and lints the package in Debian 12, Debian 13,
Ubuntu 22.04 and Ubuntu 24.04 containers and uploads the `.deb` files as artifacts.
