# Programming languages

SDE alpha3 begins a staged migration to Rust. Rust now implements the shared
configuration engine and XDG autostart. C++17, FLTK, cairo/librsvg, Xlib and libdbus
continue to implement the graphical programs and session supervisor.

| Part | Language | Reason |
| --- | --- | --- |
| Configuration parsing, layers, validation and persistence | Rust | Shared safe core, independently testable |
| XDG autostart discovery, desktop-entry parsing and launch planning | Rust | Native executable with no registry dependencies |
| Config API used by existing applications | C++ facade / C ABI | All applications use the Rust engine without rewriting the UI |
| Panel, desktop, settings, notifications, run/logout dialogs | C++17 | Preserve current FLTK UI and desktop integrations during migration |
| Session supervisor | C++17 | Preserve crash recovery, X11 and rescue dialog until a tested Rust replacement exists |
| startsde and build helpers | POSIX shell | Small wrappers around native programs and packaging tools |

Rust source requires Rust 1.59 or later. The workspace uses only the standard
library and local crates, and builds with `--locked --offline`. It does not use
GTK/Qt bindings or download another copy of FLTK. Rust and Cargo are build-time
dependencies; installed SDE programs do not need them.

This release is a mixed Rust/C++ desktop. A complete Rust source migration will
require ports and integration tests for session supervision, D-Bus, X11, tray,
notifications and the graphical interface. Openbox and the external C/C++ system
libraries remain separate dependencies even after a Rust source migration.

See [RUST.md](RUST.md) for architecture, development commands and next steps, and
[BUILD.md](BUILD.md) for the supported distributions and library packages.
