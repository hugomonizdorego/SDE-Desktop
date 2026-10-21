# Building SDE

The complete native suite targets **DebION GNU/Linux / Debian testing**. Older
Debian and Ubuntu releases are not full-suite CI targets: the selected stable
GTK application sources require libmate-desktop 1.27.1 or later.

## Debian packages

```sh
sudo sh tools/install-deps.sh
sudo sh tools/install-native-deps.sh
sh tools/build-deb.sh dist
sudo apt install ./dist/sde_*.deb ./dist/sde-desktop_*.deb
```

`debian/control` is the authoritative build dependency list. The scripts install
compilers, Rust, FLTK, GTK3, GtkSourceView4, GTKmm, document backends and native
build tools. They do not install Caja (Liria's upstream) or any other upstream
desktop application.

`build-deb.sh` downloads and verifies pinned source before compilation, builds in
a temporary source copy with `dpkg-buildpackage -us -uc -b`, runs the tests and
reports lintian findings. Outputs include `sde`, the `sde-desktop` metapackage,
optional debug symbols, complete corresponding sources and checksums. Use APT
for installation so shared libraries and system services are resolved.

## Editable source and offline compilation

```sh
python3 tools/prepare-native-suite.py
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The preparation step fetches exact commits and pinned submodules from
`suite/forks.lock.json`, preserves original legal notices and writes downstream
source to `vendor/suite`. Compilation then needs no network. `source-work/native`
is only the original checkout cache. Do not edit that cache; edit the prepared
source or the preparation recipe. The tool refuses to replace a prepared tree
with a different recipe automatically. Keep downstream edits before regenerating.
Source edits invalidate the native build stamp. Per-application build logs are
under `build/native-build/*.log`. `--only` on `tools/build-native-suite.py` can
rebuild one fork during development; Liria must be staged before Taca.

Release source archives already contain prepared sources, so CMake can build
them offline without fetching GitHub. The build installs native executables,
private libraries, plugins, schemas, resources and helpers into the same SDE
package. It excludes development headers and static libraries. Native copyrights
are installed under `/usr/share/doc/sde/upstream`.

For shell-only development on an older distribution, explicitly configure
`-DSDE_BUILD_NATIVE_SUITE=OFF`. That option omits the suite and must not be used
for the complete DebION package.

## Tests

Rust requires 1.59+ and has no registry dependencies:

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked --offline -- -D warnings
cargo test --workspace --locked --offline
python3 tests/test_native_forks.py
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

## CI and releases

`build.yml` checks Rust stable/MSRV and builds, installs and exercises the complete
suite in Debian testing. It uploads packages, corresponding sources, checksums
and UI captures. `release.yml` builds the same target on a version tag or manual
invocation, publishes the package pair and source archive with SHA256SUMS as a
GitHub pre-release. No release is created merely by opening a PR.

Measurements from alpha2 and older screenshots do not describe the resource use
of this complete suite. Test real display-manager login, polkit authorization,
notification delivery, screen locking and power policies on DebION before
releasing an installer image.

## Alpha7 components

Run `python3 tools/prepare-components.py` before configuring; this fetches the
pinned Calculator, Openbox and picom sources. `tools/build-deb.sh` performs both
source-preparation steps automatically, then builds without network access.
Additional dependencies are listed in `debian/control` and installed by
`tools/install-native-deps.sh`. GTK4 >= 4.8 and JavaScriptCoreGTK 4.1 are required
for Calculator; GTK3 is retained for the other native forks. CMake >= 3.19 is
required. For core-only development, explicitly disable both
`SDE_BUILD_NATIVE_SUITE` and `SDE_BUILD_COMPONENTS`.
