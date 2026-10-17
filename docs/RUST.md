# Rust migration: alpha3

The first milestone moves configuration used by every SDE component into Rust
and replaces the session's XDG autostart loop with a native Rust executable.
The graphical programs and session supervisor still use C++17.

Alpha4 also moves panel placement, sizing and edge reservation into the safe Rust
core. The private ABI returns a C-compatible geometry structure to the native panel.
Tests cover full/floating/short styles, screen offsets, small screens and auto-hide.

## Workspace

| Crate | Output | Responsibility |
| --- | --- | --- |
| `sde-core` | Rust library | Layered configuration, atomic persistence, XDG paths, desktop-entry parsing, autostart discovery |
| `sde-config-ffi` | Static library | Small private C ABI, handle/string ownership |
| `sde-autostart` | Executable | Discover, filter and start autostart entries; dry-run diagnostics |

The core and executable forbid unsafe Rust. Raw pointers are confined to the
FFI crate. `src/common/config.cpp` owns a Rust handle through the existing Config
API; copying a Config clones the Rust data. Strings are freed by their allocating
library. Callers must not access the same mutable handle concurrently.

The Cargo workspace has no registry dependencies. Builds need Rust 1.59+ and
Cargo, use the checked-in lockfile and require no crate downloads. CMake builds
release Rust outputs into its own build directory and links the static core
into the existing programs. Rust's standard library is linked into those native
outputs; the installed system does not need Rust or Cargo.

## Configuration compatibility

The paths and INI format are retained: built-in defaults, system directories
in reverse XDG precedence, then the user's `sde/sde.conf`. Only user overrides
are written. `unset` restores the system/default value. Built-in defaults now
come directly from `data/sde.conf` at compile time.

Writes use a new exclusive temporary file in the destination directory, mode
0600, file sync, atomic rename and directory sync. Invalid keys, embedded NULs
and multiline values are rejected. Non-UTF-8 configuration files are ignored;
text created by the settings application is UTF-8. Out-of-range integers use
the caller's fallback instead of narrowing to an unrelated value.

## Autostart behavior

`sde-session` starts `sde-autostart` once after the desktop components and polkit
agent. The helper's stdout/stderr inherit the session log. It does not restart
individual applications. The session is a Linux subreaper and adopts/reaps the
helper's children after the helper exits.

```sh
sde-autostart --dry-run
```

This command reads configuration and prints the plan without launching anything.
It honors user overrides (including Hidden=true), Type, OnlyShowIn, NotShowIn,
TryExec, Terminal and Path. NoDisplay does not prevent autostart. Desktop names
are case-sensitive. The optional X-GNOME-Autostart-enabled=false extension is
also honored. Missing directories are normal; unreadable directories are errors.
Relative XDG paths are ignored. Executables are resolved from absolute PATH
entries or absolute program paths, so launch behavior does not depend on cwd.

Exec arguments are passed directly, without evaluating shell syntax. File/URL
field codes are removed because autostart has no selected files. Names follow
LC_ALL, LC_MESSAGES, then LANG, including Tetun/Portuguese/Indonesian locales when
entries provide those translations. Polkit and notification programs managed by
SDE are skipped. Invalid individual entries are reported and other entries
continue; a failure produces status 1. Successful spawning does not promise that
the application will remain running.

Autostart delays, desktop-specific condition expressions, startup notification
and D-Bus-only activation are not implemented. The Rust desktop-entry parser
currently serves autostart only; the menu/desktop still use their C++ parser.

References: [XDG Autostart](https://specifications.freedesktop.org/autostart/latest/)
and [Desktop Entry Exec](https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html).

## Validation

```sh
cargo test --workspace --locked --offline
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked --offline -- -D warnings
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Tests cover configuration precedence and saves, ownership across the C++/Rust
boundary, integer overflow, hidden overrides, locale lookup, argument quoting,
invalid entries, dry-run side effects and real launch behavior. The distribution
CI builds Debian packages and runs version checks for all installed binaries;
a separate Rust job checks formatting, Clippy and the minimum compiler version.

Before a stable release, also test a real display-manager login, logout,
applications launched into terminals, polkit prompts, notification delivery and
crash recovery on each supported distribution. Existing screenshots and the
alpha2 memory measurement are not measurements of this new version.

## Next milestones

1. Expose the Rust desktop-entry parser to the panel and desktop through a tested
   interface, eliminating the second parser.
2. Port session supervision, preserving signals, process cleanup, rescue dialogs
   and X11 server disconnect behavior.
3. Port the notification service and D-Bus integration with protocol tests.
4. Select a GUI integration strategy, then migrate run/logout dialogs, settings,
   desktop and panel independently. Preserve tray, keyboard and accessibility
   behavior before replacing the corresponding C++ programs.

## Alpha6 native suite

`rust/sde-suite` provides optional dispatch and source/copyright diagnostics from
`data/suite.tsv`. Native app commands are independent ELF executables built from
the pinned C/C++ GTK source forks, rather than symlinks to this helper. Their
main programs apply the SDE GTK launch preference through `suite/sde-brand.h`.
Liria defaults to no desktop ownership. Power preferences, statistics and daemon
are separate native programs. SDE Update stays C++/FLTK with an attributed Python
APT model. No new Cargo registry dependency or idle dispatcher daemon is added.
