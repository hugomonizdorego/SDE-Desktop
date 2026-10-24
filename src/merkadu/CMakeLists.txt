# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
#
# Merkadu, the SDE software center (docs/MERKADU.md), based on Ubuntu
# Software Center: apps from the Debian archive (PackageKit and AppStream),
# Flatpak (libflatpak) and Snap (snapd's REST API). Packaged on its own
# (Debian package merkadu). The model (catalog, search, departments,
# history, uninstall decisions, snapd's protocol) is a display-free library
# for the unit tests.
set(_src ${PROJECT_SOURCE_DIR}/src)

pkg_check_modules(LIBXML2 IMPORTED_TARGET libxml-2.0)
pkg_check_modules(APPSTREAM IMPORTED_TARGET appstream>=1.0)
pkg_check_modules(FLATPAK IMPORTED_TARGET flatpak>=1.12)
find_package(ZLIB)
option(SDE_BUILD_MERKADU "Build Merkadu, the software center" ON)
if(NOT SDE_BUILD_MERKADU)
  return()
endif()
if(NOT LIBXML2_FOUND OR NOT APPSTREAM_FOUND OR NOT ZLIB_FOUND)
  message(WARNING "Merkadu needs libxml-2.0, appstream (>= 1.0) and zlib; not building it")
  set(SDE_HAVE_MERKADU OFF PARENT_SCOPE)
  return()
endif()
set(SDE_HAVE_MERKADU ON PARENT_SCOPE)

add_library(merkadu-core STATIC
  ${_src}/merkadu/model.cpp
  ${_src}/merkadu/json.cpp
  ${_src}/merkadu/categories.cpp
  ${_src}/merkadu/snapd.cpp
  ${_src}/merkadu/pkenum.cpp
  ${_src}/merkadu/appstream_catalog.cpp)
target_include_directories(merkadu-core PUBLIC ${_src}/merkadu)
target_compile_definitions(merkadu-core PUBLIC MERKADU_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/merkadu")
target_link_libraries(merkadu-core PUBLIC sde-common PkgConfig::LIBXML2 PkgConfig::APPSTREAM ZLIB::ZLIB
                      Threads::Threads)
