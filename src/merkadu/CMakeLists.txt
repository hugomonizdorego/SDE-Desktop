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

# The program: the window, the uninstall dialog, the backends and the fakes
# of MERKADU_FAKE (the UI checks). The page widgets come from the Control
# Center (settings/ui.cpp).
add_executable(merkadu
  ${_src}/merkadu/main.cpp
  ${_src}/merkadu/store.cpp
  ${_src}/merkadu/window.cpp
  ${_src}/merkadu/pages.cpp
  ${_src}/merkadu/ui.cpp
  ${_src}/merkadu/uninstall.cpp
  ${_src}/merkadu/logo_anim.cpp
  ${_src}/merkadu/image_cache.cpp
  ${_src}/merkadu/packagekit.cpp
  ${_src}/merkadu/flatpak.cpp
  ${_src}/merkadu/snap.cpp
  ${_src}/merkadu/fake.cpp
  ${_src}/settings/ui.cpp)
target_link_libraries(merkadu PRIVATE merkadu-core)
if(FLATPAK_FOUND)
  target_compile_definitions(merkadu PRIVATE MERKADU_HAVE_FLATPAK=1)
  target_link_libraries(merkadu PRIVATE PkgConfig::FLATPAK)
else()
  message(WARNING "Merkadu: libflatpak not found, building without the Flatpak source")
endif()
install(TARGETS merkadu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/data/merkadu/categories.menu ${PROJECT_SOURCE_DIR}/data/merkadu/merkadu.conf
              ${PROJECT_SOURCE_DIR}/data/merkadu/flathub.flatpakrepo
        DESTINATION ${CMAKE_INSTALL_DATADIR}/merkadu)
# The icons of the archive's AppStream catalog (APT leaves them off).
install(FILES ${PROJECT_SOURCE_DIR}/data/merkadu/60merkadu DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/apt/apt.conf.d)
