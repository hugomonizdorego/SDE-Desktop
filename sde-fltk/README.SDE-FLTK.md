# SDE-FLTK

SDE-FLTK is the user interface toolkit of the Selviana Desktop Environment
(SDE). It is a fork of **FLTK 1.3.11** (the Fast Light
Toolkit, <https://www.fltk.org>, upstream revision
`702172a951a2bb4f25afe31008ef3fcbb5bfb92f`), kept in this repository and
maintained by the **Selviana DE Project**.

* Every SDE program links SDE-FLTK statically; SDE does not need the
  distribution's FLTK packages.
* It is built from this source tree by SDE's CMake build (see the top-level
  `CMakeLists.txt`, option `SDE_BUNDLED_FLTK`).
* Not included from the upstream release: the test and example programs, the
  IDE project files, the documentation sources (except the man pages) and
  the bundled copies of
  zlib, libjpeg and libpng (SDE-FLTK uses the system libraries).

## License and copyright

FLTK is copyright 1998-2025 by Bill Spitzak and others (see `CREDITS`) and is
distributed under the GNU Library General Public License version 2 with the
FLTK exceptions for static linking (see `COPYING`). The fork keeps that
license and every original copyright notice; the license does not allow
them to be removed or replaced. Changes made by the Selviana DE Project are
copyright 2026 Selviana DE Project and contributors and are released under
the same license.
SDE-FLTK changes are listed in `CHANGES.SDE-FLTK`.
