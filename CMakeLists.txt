# SPDX-License-Identifier: GPL-3.0-or-later
# Selviana Desktop Environment (SDE) - top-level build file.

cmake_minimum_required(VERSION 3.19)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SDE_VERSION_FILE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
string(STRIP "${SDE_VERSION_FILE}" SDE_VERSION)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" SDE_VERSION_NUMERIC "${SDE_VERSION}")

project(sde VERSION ${SDE_VERSION_NUMERIC} LANGUAGES CXX)

set(SDE_VERSION_LABEL "0.1 Alpha 7" CACHE STRING "Human readable release name")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(X11 REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-xlib)
pkg_check_modules(RSVG REQUIRED IMPORTED_TARGET librsvg-2.0)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

# FLTK 1.3 (Debian 12/13, Ubuntu 22.04/24.04) or 1.4. fltk-config is the most
# portable way to find it on Debian based systems.
find_program(FLTK_CONFIG fltk-config REQUIRED)
execute_process(COMMAND ${FLTK_CONFIG} --version
                OUTPUT_VARIABLE FLTK_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${FLTK_CONFIG} --use-images --cxxflags
                OUTPUT_VARIABLE FLTK_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${FLTK_CONFIG} --use-images --ldflags
                OUTPUT_VARIABLE FLTK_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(FLTK_CXXFLAGS UNIX_COMMAND "${FLTK_CXXFLAGS}")
separate_arguments(FLTK_LDFLAGS UNIX_COMMAND "${FLTK_LDFLAGS}")
message(STATUS "FLTK ${FLTK_VERSION}")

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(SDE_DATADIR "${CMAKE_INSTALL_FULL_DATADIR}/sde")
set(SDE_SYSCONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
set(SDE_LOCALEDIR "${CMAKE_INSTALL_FULL_LOCALEDIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/suite/versions.json")
file(READ "${PROJECT_SOURCE_DIR}/suite/versions.json" SDE_APP_VERSIONS)
string(JSON SDE_UPDATE_VERSION GET "${SDE_APP_VERSIONS}" sde-update)
configure_file(src/common/build_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/build_config.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/suite.tsv" SDE_SUITE_DATA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/suite.tsv")
configure_file(src/common/suite_data.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/suite_data.h @ONLY)

add_subdirectory(rust)
add_subdirectory(suite)
add_subdirectory(src)
add_subdirectory(po)

option(SDE_BUILD_TESTS "Build unit tests" ON)
if(SDE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- data files
install(PROGRAMS data/startsde DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS data/sde-lock DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/sde-mimeapps.list DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/xdg)
install(DIRECTORY data/autostart/ DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/xdg/autostart)
install(DIRECTORY data/update/ DESTINATION ${CMAKE_INSTALL_DATADIR}/sde/update
        PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(FILES docs/SUITE.md docs/THIRD-PARTY.md upstream/sources.lock.json suite/versions.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde)
install(FILES data/xsessions/sde.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/xsessions)
install(FILES data/sde.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/xdg/sde)
install(FILES data/schemas/org.debion.session.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES data/openbox/rc.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/sde/openbox)
install(DIRECTORY data/themes/ DESTINATION ${CMAKE_INSTALL_DATADIR}/themes)
install(DIRECTORY data/backgrounds/ DESTINATION ${CMAKE_INSTALL_DATADIR}/backgrounds/sde)
install(DIRECTORY data/icons/ DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
install(DIRECTORY data/applications/ DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
file(GLOB SDE_MANPAGES CONFIGURE_DEPENDS data/man/*.1)
install(FILES ${SDE_MANPAGES} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

install(FILES data/compositor/sde.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/sde/compositor)
install(FILES data/sudoers/sde-liria DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/sudoers.d
        PERMISSIONS OWNER_READ GROUP_READ)

install(DIRECTORY data/applications/ DESTINATION ${CMAKE_INSTALL_DATADIR}/sde/applications)
install(FILES data/portals/sde-portals.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
