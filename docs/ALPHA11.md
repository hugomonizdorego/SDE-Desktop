# SDE 0.1 Alpha 11 — DebION application profile

The native suite is built from source as before. Development credits in native
About dialogs now read **DebION Dev-team**, linking to SDE-Desktop. Original
authors, source links, copyright ownership and licenses remain available.
SDE and Liria use `0.1.0~alpha11`; Gallery uses `1.0.0~alpha2`. Other changed
native applications use `1.0.0~alpha3`. WM and compositor remain `1.0.0~alpha2`.

The full `sde-desktop` metapackage adds DebION's complementary applications and
services. See [the complete package profile and default applications](DEBION.md).
The native `sde` package remains usable independently.

Use APT to resolve dependencies, through the included installer:

```sh
sha256sum -c SHA256SUMS.sha256
sudo sh ./install-debion.sh .
```

The installer first installs `debion-desktop-defaults`, refreshes APT for the
official Mozilla repository, and then installs the matching SDE core and desktop
packages. Debug symbols are optional. Maintainer scripts do not download packages
or call APT. This is an alpha for DebION / Debian testing; Debian stable and
Ubuntu are not the binary target.

Validation is recorded in the pull request and CI run. Network, Bluetooth,
printing, scan hardware and a real LightDM boot require testing on physical
systems; Xvfb tests cannot establish their hardware compatibility.

## Verified build — 19 October 2026

Code commit: `481bd15520f090deccbb05167c6bf400ba8ba79a`.
Both the [push build](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39224810528)
and [pull request build](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39224816153) passed:

- Full source-built native suite, Gallery, Calculator, WM and compositor; 10 CTest tests.
- Rust formatting, Clippy, stable tests and minimum Rust 1.59 checks.
- Actual installation of all three desktop packages and every complementary
  dependency, including Firefox release from the signed Mozilla source.
- Native ELF/version checks, original MATE/gthumb application absence,
  copyright files, MIME associations and preserved personal browser choices.
- Menu categories through the production menu loader, GNOME Terminal selection,
  exact argument forwarding and personal terminal preference overrides.
- Celluloid's libmpv dependency and blocked mpv player, plus the source/key
  conffile pairing for correct repository removal behavior.
- GUI at 1366×768 / 100% and 1024×600 / 150%: panel, Action Center, tray,
  media and Bluetooth fixtures, display previews, preferences and native apps.
- Gallery on a fresh profile, light/dark themes, photo browser, About and SDE
  wallpaper integration; clickable native About developer/upstream links.

[Download Debian amd64 packages, installer and complete sources](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39224810528/artifacts/12174101625).
The artifact ZIP SHA256 is
`29050192ce78c05170aeaa4291a7bd9cdaf1ac45ed37b2ec5c5f7cd6bcecf9e0`.
The downloaded ZIP and all six entries in its `SHA256SUMS.sha256` were verified.
Debug symbols are included but optional; the installer selects the other three
`.deb` files automatically.

[Current native screenshots](screenshots/alpha11/README.md) come from the same
verified code and package installation. Documentation/screenshot updates after
`481bd15` do not change the tested executable or packaging sources.
