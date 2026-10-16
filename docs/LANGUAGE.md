# Programming language recommendation

**Recommendation: C++17, with FLTK 1.3 for widgets, cairo/librsvg for drawing and
Xlib for desktop integration. Shell (POSIX `sh`) only for the small session
launcher and the build tools.**

## Why C++17

| Need | How C++17 meets it |
| --- | --- |
| Small and fast | Native code; each SDE program uses only a few MB of RAM and starts instantly. |
| Direct X11/EWMH access | Xlib, XEmbed, EWMH and ICCCM are C APIs; C++ calls them with no bindings layer. |
| Toolkit fit | FLTK, the toolkit EDE used, is a C++ library; it is small (about 1 MB) and in every Debian/Ubuntu release. |
| Long-term maintenance | The C++17 standard is supported by GCC 9 and later, so it builds on Debian 12/13 and Ubuntu 22.04/24.04 without extra repositories. |
| Packaging | CMake + debhelper is the standard, well-supported path for Debian packages. |

## Alternatives considered

| Option | Why not (for now) |
| --- | --- |
| C | Possible, but string handling, containers and RAII make C++ safer and shorter for UI code. |
| Rust | Good safety, but the GTK/Qt bindings are large, FLTK bindings are less mature, and Debian 12's Rust toolchain is old; it adds a heavy build dependency. |
| Python | Fast to write but slower to start and uses far more memory per process (panel, desktop and settings all run at the same time). |
| Vala/GTK 4 or Qt 6 | Mature toolkits, but they pull in much larger runtimes and their look is hard to make consistent with a light desktop. |

## Libraries used in 0.1 Alpha

| Library | Debian package (build) | Purpose |
| --- | --- | --- |
| FLTK 1.3 | `libfltk1.3-dev` | Windows, input, buttons, text fields |
| cairo | `libcairo2-dev` | Anti-aliased rounded shapes, shadows, image scaling |
| librsvg | `librsvg2-dev` | SVG icons and wallpapers |
| Xlib, Xft | `libx11-dev`, `libxft-dev` | EWMH taskbar, struts, system tray, fonts |
| libdbus | `libdbus-1-dev` | Notifications, StatusNotifierItem tray, logind brightness |
| gettext | `gettext` (libintl is in glibc) | Translations |
| Openbox | `openbox` (runtime) | Window manager, configured by SDE |

A later release may replace Openbox with an SDE window manager and add Wayland
support; the code is split into `src/common` (shared library code) and one
directory per program so that this can happen piece by piece.
