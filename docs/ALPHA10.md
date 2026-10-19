# SDE 0.1 Alpha 10 — Gallery and application credits

Gallery **1.0.0~alpha1** is a native source fork of
[gthumb](https://gitlab.gnome.org/GNOME/gthumb) **4.0**, by **Paolo Bacchilega and
gThumb contributors**. SDE integration is developed by
[Hugo Moniz do Rego](https://github.com/hugomonizdorego) and SDE contributors.
It uses GTK4, libadwaita and Vala; it is not a launcher for an installed gThumb
package. Browse photographs, organize catalogs and tags, view metadata, crop,
rotate, resize and edit images with the upstream engine.

## Desktop integration

- `gallery` opens the application; files and directories are accepted as arguments.
- The Start Menu lists Gallery under **Graphics** using standard XDG categories.
- Gallery has private `org.debion.Gallery` application/resource/schema identities,
  `sde-gallery` configuration/cache directories and a separate build version.
- The SDE appearance preference selects light/dark libadwaita styling at startup;
  choosing the system GTK theme leaves the platform appearance in control.
- Set as Background updates SDE through `sde-settings --wallpaper FILE` and notifies
  the running desktop. The portal implementation remains available in other sessions.
- SDE View remains the default lightweight image viewer. Gallery is available in
  Open With and the Graphics menu; existing user MIME defaults are preserved.
- Image codecs are built from distribution libraries: JPEG, PNG, WebP, SVG, TIFF,
  GIF, AVIF/HEIF and JPEG XL. RAW support follows the installed libraw version
  (the upstream 4.0 engine requires 0.22); videos use GStreamer and installed codecs.

## About and attribution

Every graphical native application now provides the SDE developer's name and
clickable developer/source links. Niny, Liria, Xanara, Taca, SDE View and System
Monitor retain the upstream About authors, translators, copyright and license.
Power Preferences, Power Statistics and Screen Lock Preferences gain About buttons.
Gallery credits Paolo Bacchilega, links to gthumb and the SDE sources, and identifies
Hugo Moniz do Rego as the SDE developer. Calculator retains Raad Altaie's engine
credit. Display Settings credits zac15987. SDE Update links to mintupdate and SDE.
Shell, Openbox and picom attribution is available in Settings → About; their
original source and license documents are installed with the package.

Original source licenses are retained. The GTK3 integration header is GPL-2.0-or-later,
compatible with the upstream forks. The complete Gallery source, translated
catalogs, original headers and COPYING are included alongside the other patched
forks in `sde-native-sources_*.tar.xz`. Installed Gallery notices are in
`/usr/share/doc/sde/upstream/gthumb/`. Source recipe, URL and official SHA256 are
pinned in `suite/gallery.lock.json`. A checksum mismatch stops preparation.

SDE and Liria share **0.1.0~alpha10**; existing applications advance their own
sequence to **1.0.0~alpha2**. Gallery starts at **1.0.0~alpha1**.

## Build and install

Target: **DebION GNU/Linux / Debian testing**. Gallery 4.0 requires GLib 2.84,
GTK 4.18.5, libadwaita 1.8, GStreamer 1.26 and libportal 0.9 or newer. Older Debian
stable releases need those libraries backported; they are not an alpha10 target.

```sh
sudo sh tools/install-deps.sh
sudo sh tools/install-native-deps.sh
sh tools/build-deb.sh dist
sudo apt install ./dist/sde_*.deb ./dist/sde-desktop_*.deb
```

Use APT for local packages so distribution service/library dependencies are
resolved. No original gthumb, pluma, caja, atril, engrampa, eom, Openbox or picom
application packages are installed by the SDE package. The complete desktop
metapackage installs the session's system services.

## Validation and alpha limits

CI builds every fork on Debian testing, runs unit tests including real GTK3 About
dialog attribution/preservation, installs the resulting `.deb`, checks native ELF
binaries and embedded developer links, and exercises Gallery's browser, photo
viewer, actual About action, private schemas and SDE wallpaper command on Xvfb.
Gallery screenshots and logs accompany the existing desktop/panel GUI checks in
the `sde-ui-smoke` artifact. Binary packages, full corresponding sources and
SHA256SUMS accompany `sde-debian-testing`.

This remains an alpha. Xvfb does not validate cameras, physical color profiles,
RAW camera models, GPU drivers, media codecs, external displays or portal behavior
on every machine. No such hardware results are implied by the virtual GUI checks.
