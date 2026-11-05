# SPDX-License-Identifier: GPL-3.0-or-later
# Liria, the SDE file manager (docs/LIRIA.md), written from scratch by the
# Selviana DE Project on SDE-FLTK and the SDE UI kit. The display-free part
# (locations, Quick access pins, typed addresses and recent servers, the
# Trash, file operations and their undo, preferences, applications for a
# type) is liria-core, which the unit tests link.
#
# Packaging: everything Liria installs belongs to the install component
# "liria" (the liria package).
pkg_check_modules(LIRIA_GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
find_package(Threads REQUIRED)

add_library(liria-core STATIC
  location.cpp
  pins.cpp
  address.cpp
  trash.cpp
  fileops.cpp
  prefs.cpp
  openwith.cpp
  viewmodel.cpp
  ${PROJECT_SOURCE_DIR}/src/settings/default_apps.cpp)
target_include_directories(liria-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(liria-core PUBLIC sde-desktop-model sde-common PkgConfig::LIRIA_GIO Threads::Threads)

# The window: the bars, the views, the menus and dialogs, the clipboard,
# the jobs with the transfer dialog, GIO's drives and mounts. XDND is the
# SDE desktop's (src/desktop/xdnd.cpp).
add_library(liria-ui OBJECT
  glyphs.cpp
  glibloop.cpp
  drives.cpp
  widgets.cpp
  fileview.cpp
  sidebar.cpp
  home.cpp
  clipboard.cpp
  jobs.cpp
  menus.cpp
  dialogs.cpp
  window.cpp
  ${PROJECT_SOURCE_DIR}/src/desktop/xdnd.cpp)
target_link_libraries(liria-ui PUBLIC liria-core)
target_compile_definitions(liria-ui PRIVATE SDE_APP_VERSION="${SDE_VERSION}")
