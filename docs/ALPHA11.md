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
