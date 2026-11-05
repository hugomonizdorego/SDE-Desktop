# Building SDE

SDE targets **Debian testing** (and distributions based on it, such as DebION
GNU/Linux). Older Debian and Ubuntu releases are not CI targets.

## Debian packages

```sh
sudo sh tools/install-deps.sh
sudo sh tools/install-native-deps.sh
sh tools/build-deb.sh dist
sudo apt install ./dist/sde_*.deb ./dist/sde-desktop_*.deb
```

`debian/control` is the authoritative build dependency list. The scripts install
compilers, Rust, GLib/GIO, cairo, document backends and the build tools of the
session components. They install no upstream desktop application.

`build-deb.sh` downloads and verifies pinned source before compilation, builds in
a temporary source copy with `dpkg-buildpackage -us -uc -b`, runs the tests and
reports lintian findings. Outputs include `sde`, the `sde-desktop` metapackage,
optional debug symbols, complete corresponding sources and checksums. Use APT
for installation so shared libraries and system services are resolved.

## Editable source and offline compilation

```sh
python3 tools/prepare-components.py   # SDE WM and SDE Compositor sources
python3 tools/prepare-themes.py       # the GTK theme sources
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The preparation steps fetch the pinned sources of the session components and
themes; compilation then needs no network. Every application, Liria included
since 0.2.0 Alpha 4 ([LIRIA.md](LIRIA.md)), is SDE's own source in `src/` and
needs no preparation. Up to 0.2.0 Alpha 3.1 `tools/prepare-native-suite.py`
prepared the Caja fork Liria came from; that machinery and the
`SDE_BUILD_NATIVE_SUITE` option are gone.

Release source archives already contain the prepared sources, so CMake can
build them offline without fetching GitHub. The build installs the executables,
resources, schemas and helpers; it excludes development headers and static
libraries. Every `install()` rule names an install component (Liria's is
`liria`), from which the Debian packages are made.

For shell-only development, configure `-DSDE_BUILD_COMPONENTS=OFF`: SDE WM and
SDE Compositor are not built then.

## Tests

Rust requires 1.59+ and has no registry dependencies:

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked --offline -- -D warnings
cargo test --workspace --locked --offline
python3 -m unittest tests/test_native_forks.py tests/test_legal_data.py tests/test_metainfo.py
```

CTest also checks C++ parsing, geometry, themes, update models and the Rust ABI.
After installation, `tools/test-suite.py` checks direct ELF executables and the
absence of original application packages. `tools/check-installed-suite.py`
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

`build.yml` checks Rust stable/MSRV and builds, installs and exercises the complete
suite in Debian testing. It uploads packages, corresponding sources, checksums
and UI captures. `release.yml` builds the same target on a version tag or manual
invocation, publishes the package pair and source archive with SHA256SUMS as a
GitHub pre-release. No release is created merely by opening a PR.

Measurements from alpha2 and older screenshots do not describe the resource use
of this complete suite. Test real display-manager login, polkit authorization,
notification delivery, screen locking and power policies on the target distribution before
releasing an installer image.

## Alpha7 components

Run `python3 tools/prepare-components.py` before configuring; this fetches the
pinned Openbox and picom sources (Calculator is a native SDE program since
alpha50). `tools/build-deb.sh` performs the source preparation automatically,
then builds without network access. Additional dependencies are listed in
`debian/control` and installed by `tools/install-native-deps.sh`. CMake >= 3.19
is required. For core-only development, disable `SDE_BUILD_COMPONENTS`.
