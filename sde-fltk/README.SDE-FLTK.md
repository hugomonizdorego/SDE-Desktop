# SDE-FLTK

SDE-FLTK is the user interface toolkit of the Selviana Desktop Environment
(SDE). It is a fork of **FLTK 1.4.5** (the Fast Light
Toolkit, <https://www.fltk.org>, upstream tag `release-1.4.5`, revision
`a9b1113516ffd15fc7602a6d425a317df30f4720`), kept in this repository and
maintained by the **Selviana DE Project**.

* Every SDE program links SDE-FLTK statically; SDE does not need the
  distribution's FLTK packages.
* It is built from this source tree by SDE's CMake build (see the top-level
  `CMakeLists.txt`, option `SDE_BUNDLED_FLTK`): the X11 backend with Xlib
  drawing and Xft fonts, without the Wayland backend, OpenGL, SVG images,
  FLUID and the forms library.
* FLTK's own GUI scaling is off: SDE programs scale themselves with the SDE
  scale setting (see `CHANGES.SDE-FLTK`).
* Not included from the upstream release: the test and example programs,
  the upstream CI files, the documentation sources (except the man pages)
  and the bundled copies of zlib, libjpeg, libpng, libdecor and nanosvg
  (SDE-FLTK uses the system libraries and needs neither libdecor nor
  nanosvg).

## License and copyright

FLTK is copyright 1998-2026 by Bill Spitzak and others (see `CREDITS.txt`)
and is distributed under the GNU Library General Public License version 2
with the FLTK exceptions for static linking (see `COPYING`). The fork keeps
that license and every original copyright notice; the license does not
allow them to be removed or replaced. Changes made by the Selviana DE
Project are copyright 2026 Selviana DE Project and contributors and are
released under the same license.
SDE-FLTK changes are listed in `CHANGES.SDE-FLTK`.
