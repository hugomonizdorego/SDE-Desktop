# Building SDE

SDE targets **Debian testing** (and distributions based on it, such as DebION
GNU/Linux). Older Debian and Ubuntu releases are not CI targets: SDE
compositor (picom) needs libconfig 1.7 or later, for example.

## Debian packages

```sh
sudo sh tools/install-deps.sh
sudo sh tools/install-native-deps.sh
sh tools/build-deb.sh dist
sudo apt install $(ls ./dist/*.deb | grep -v -e -dbgsym_ -e /debion-desktop-base_)
```

`debian/control` is the authoritative build dependency list. The scripts install
compilers, Rust, the X11, cairo, GLib and GTK 3 development files, the document,
archive and software-center libraries, and the build tools of SDE WM (autotools)
and SDE compositor (meson). They do not install any upstream desktop
application.

`build-deb.sh` downloads and verifies pinned source before compilation, builds in
a temporary source copy with `dpkg-buildpackage -us -uc -b`, runs the tests and
reports lintian findings. Outputs are SDE Core (`sde-core`), one package per
application (`liria`, `xanara`, `niny`, `merkadu`, `sde-calculator`, `taca`,
`sde-photo`, `sde-gallery`), the `sde-desktop` metapackage, the transitional
`sde`, `debion-desktop-base`, the `-dbgsym` debug symbols, the complete
corresponding sources and checksums; [PACKAGING.md](PACKAGING.md) describes
them and how a file finds its package. Use APT for installation so shared
libraries and system services are resolved.

On a host where SDE compositor cannot be built (libconfig older than 1.7, as on
Ubuntu 24.04), `SDE_CMAKE_OPTIONS=-DSDE_BUILD_COMPONENTS=OFF sh tools/build-deb.sh
dist` builds and checks the packages without SDE WM and SDE compositor; do not
publish those.

## Editable source and offline compilation

```sh
python3 tools/prepare-components.py
python3 tools/prepare-themes.py
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The preparation steps fetch exact commits of SDE WM, SDE compositor and the
theme sources (`suite/components.lock.json`, `suite/themes.lock.json`),
preserve original legal notices and write the source to `vendor/`. Compilation
then needs no network. `source-work/` is only the original checkout cache. Do
not edit that cache; edit the prepared source or the preparation recipe. The
tools refuse to replace a prepared tree with a different recipe automatically.

Release source archives already contain prepared sources, so CMake can build
them offline without fetching GitHub. Upstream copyrights are installed under
`/usr/share/doc/sde/upstream`. `cmake --install build --component <package>`
installs one package's files (`build/install-components.txt` lists them).

The native suite (GTK applications built from pinned MATE sources) ended with
Liria's Caja fork in 0.2.0 Alpha 4; every SDE application is now built by this
CMake project. `SDE_BUILD_NATIVE_SUITE` is no longer read.

## Tests

Rust requires 1.59+ and has no registry dependencies:

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked --offline -- -D warnings
cargo test --workspace --locked --offline
python3 tests/test_native_forks.py
```

CTest also checks C++ parsing, geometry, themes, update models and the Rust ABI.
After installation, `tools/test-suite.py` checks direct ELF executables, the
package of each program and the absence of original application packages. `tools/check-installed-suite.py`
checks MIME defaults and user overrides, nonexecuting autostart plans, and
copyright availability. Native UI checks use Xvfb, Openbox, D-Bus and sample
files; they do not lock, update or suspend the test machine:

```sh
sudo sh tools/install-deps.sh --preview
dbus-run-session -- python3 tools/ui-smoke.py --suite-apps --output-dir dist/ui-smoke
```

ui-smoke also manages user accounts on Control Center → Users & Avatar against
the fake AccountsService of `sde-panel-service-fixture` when it finds the QA
fixtures (`--test-dir`, by default `dist/test-tools` or `build/tests`), and
`--scale 75` checks the smallest interface size.

## CI and releases

`build.yml` checks Rust stable/MSRV and builds, installs and exercises every
package in Debian testing (and checks that each program comes from its own
package). It uploads packages, corresponding sources, checksums and UI
captures, and on main publishes every `.deb` with `SHA256SUMS` as the
`alpha-latest` pre-release. `release.yml` builds the same target on a version
tag or manual invocation and publishes every package and the source archive
with SHA256SUMS as a GitHub pre-release. No release is created merely by
opening a PR.

Measurements from alpha2 and older screenshots do not describe the resource use
of this complete suite. Test real display-manager login, polkit authorization,
notification delivery, screen locking and power policies on the target distribution before
releasing an installer image.

## Alpha7 components

Run `python3 tools/prepare-components.py` before configuring; this fetches the
pinned Openbox and picom sources (Calculator is a native SDE program since
alpha50). `tools/build-deb.sh` performs both
source-preparation steps automatically, then builds without network access.
Additional dependencies are listed in `debian/control` and installed by
`tools/install-native-deps.sh`. GTK 3 is kept for the GTK part of the theme
preview; GTK 4 and JavaScriptCoreGTK are not needed. CMake >= 3.19 is
required. For core-only development, explicitly disable
`SDE_BUILD_COMPONENTS`.
