# SPDX-License-Identifier: GPL-3.0-or-later
# Selviana Desktop Environment (SDE) - top-level build file.

cmake_minimum_required(VERSION 3.19)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SDE_VERSION_FILE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
string(STRIP "${SDE_VERSION_FILE}" SDE_VERSION)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" SDE_VERSION_NUMERIC "${SDE_VERSION}")

project(sde VERSION ${SDE_VERSION_NUMERIC} LANGUAGES CXX)

set(SDE_VERSION_LABEL "0.1 Alpha 15" CACHE STRING "Human readable release name")

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
pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)
pkg_check_modules(XINPUT REQUIRED IMPORTED_TARGET xi)

# SDE-FLTK: the DebION Dev-Team fork of FLTK 1.3.11 in sde-fltk/, built here
# and linked statically, so SDE does not depend on the distribution's FLTK
# packages. -DSDE_BUNDLED_FLTK=OFF uses the system FLTK (fltk-config).
option(SDE_BUNDLED_FLTK "Build and statically link SDE-FLTK" ON)
set(SDE_TOOLKIT_SOURCE "${PROJECT_SOURCE_DIR}/sde-fltk")
if(SDE_BUNDLED_FLTK AND EXISTS "${SDE_TOOLKIT_SOURCE}/CMakeLists.txt")
  set(SDE_TOOLKIT_STAGE "${CMAKE_BINARY_DIR}/sde-fltk/stage")
  if(NOT EXISTS "${SDE_TOOLKIT_STAGE}/bin/fltk-config")
    message(STATUS "Building SDE-FLTK from ${SDE_TOOLKIT_SOURCE}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -S "${SDE_TOOLKIT_SOURCE}" -B "${CMAKE_BINARY_DIR}/sde-fltk/build"
              -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=${SDE_TOOLKIT_STAGE}"
              -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DOPTION_BUILD_SHARED_LIBS=OFF
              -DFLTK_BUILD_TEST=OFF -DFLTK_BUILD_EXAMPLES=OFF -DOPTION_USE_GL=OFF
              -DOPTION_BUILD_HTML_DOCUMENTATION=OFF -DOPTION_BUILD_PDF_DOCUMENTATION=OFF
              -DOPTION_USE_SYSTEM_LIBPNG=ON -DOPTION_USE_SYSTEM_LIBJPEG=ON -DOPTION_USE_SYSTEM_ZLIB=ON
      RESULT_VARIABLE SDE_TOOLKIT_RESULT OUTPUT_QUIET)
    if(SDE_TOOLKIT_RESULT EQUAL 0)
      execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}/sde-fltk/build" --target install --parallel 4
        RESULT_VARIABLE SDE_TOOLKIT_RESULT OUTPUT_QUIET)
    endif()
    if(NOT SDE_TOOLKIT_RESULT EQUAL 0)
      message(FATAL_ERROR "Building SDE-FLTK failed")
    endif()
  endif()
  set(FLTK_CONFIG "${SDE_TOOLKIT_STAGE}/bin/fltk-config")
  # FLTK's license and notices travel with the statically linked programs.
  install(FILES "${SDE_TOOLKIT_SOURCE}/COPYING" "${SDE_TOOLKIT_SOURCE}/CREDITS"
                "${SDE_TOOLKIT_SOURCE}/README.SDE-FLTK.md" "${SDE_TOOLKIT_SOURCE}/CHANGES.SDE-FLTK"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/sde/sde-fltk" OPTIONAL)
  message(STATUS "Using SDE-FLTK (static)")
else()
  # FLTK 1.3 (Debian 12/13, Ubuntu 22.04/24.04) or 1.4. fltk-config is the most
  # portable way to find it on Debian based systems.
  find_program(FLTK_CONFIG fltk-config REQUIRED)
endif()
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
# Session components are staged under <prefix>/libexec/sde.
set(SDE_LIBEXECDIR "${CMAKE_INSTALL_PREFIX}/libexec")
set(SDE_LOCALEDIR "${CMAKE_INSTALL_FULL_LOCALEDIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/suite/versions.json")
file(READ "${PROJECT_SOURCE_DIR}/suite/versions.json" SDE_APP_VERSIONS)
string(JSON SDE_UPDATE_VERSION GET "${SDE_APP_VERSIONS}" sde-update)
string(JSON SDE_DISPLAY_VERSION GET "${SDE_APP_VERSIONS}" sde-display-settings)
configure_file(src/common/build_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/build_config.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/suite.tsv" SDE_SUITE_DATA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/suite.tsv")
configure_file(src/common/suite_data.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/suite_data.h @ONLY)

# Theme palettes for sde::Theme, from the file that also builds the themes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${PROJECT_SOURCE_DIR}/data/themes/palettes.json" "${PROJECT_SOURCE_DIR}/tools/build-themes.py")
execute_process(COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/build-themes.py"
                        --header "${CMAKE_CURRENT_BINARY_DIR}/generated/theme_palettes.h"
                RESULT_VARIABLE SDE_PALETTE_RESULT)
if(NOT SDE_PALETTE_RESULT EQUAL 0)
  message(FATAL_ERROR "tools/build-themes.py --header failed")
endif()

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
install(FILES docs/ALPHA9.md docs/ALPHA10.md docs/ALPHA11.md docs/ALPHA12.md docs/ALPHA13.md docs/ALPHA14.md docs/ALPHA15.md docs/SUITE.md docs/THIRD-PARTY.md upstream/sources.lock.json suite/versions.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde)
install(FILES data/xsessions/sde-desktop.desktop data/xsessions/sde.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/xsessions)
install(FILES data/sde.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/xdg/sde)
install(FILES data/schemas/org.debion.session.gschema.xml data/schemas/30_sde-suite.gschema.override
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES data/openbox/rc.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/sde/openbox)
# SDE themes: SDE Graphite (default), SDE Fluent and SDE Glass, built from
# data/themes/palettes.json and the pinned upstream sources that
# tools/prepare-themes.py fetched into vendor/themes (suite/themes.lock.json).
# Without them only the Openbox window themes are built and GTK applications
# use Adwaita.
set(SDE_THEMES_VENDOR "${PROJECT_SOURCE_DIR}/vendor/themes" CACHE PATH
    "Upstream theme sources prepared by tools/prepare-themes.py")
find_program(SDE_SASSC sassc)
set(SDE_THEMES_OUT "${CMAKE_BINARY_DIR}/themes")
set(SDE_THEME_DEPS "${PROJECT_SOURCE_DIR}/tools/build-themes.py" "${PROJECT_SOURCE_DIR}/data/themes/palettes.json")
file(GLOB SDE_THEME_OVERLAYS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/data/themes/overlays/*.css")
if(EXISTS "${SDE_THEMES_VENDOR}/.sde-themes-lock" AND SDE_SASSC)
  set(SDE_THEME_ARGS --vendor "${SDE_THEMES_VENDOR}" --docs "${CMAKE_BINARY_DIR}/theme-docs")
  message(STATUS "SDE themes: building SDE Graphite, SDE Fluent and SDE Glass from ${SDE_THEMES_VENDOR}")
  install(DIRECTORY "${CMAKE_BINARY_DIR}/theme-docs/" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde/themes)
else()
  set(SDE_THEME_ARGS --openbox-only)
  message(WARNING "SDE themes: run tools/prepare-themes.py and install sassc to build the GTK themes; "
                  "building the Openbox themes only")
endif()
add_custom_command(OUTPUT "${SDE_THEMES_OUT}/.stamp"
  COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/build-themes.py" --output "${SDE_THEMES_OUT}" ${SDE_THEME_ARGS}
  COMMAND ${CMAKE_COMMAND} -E touch "${SDE_THEMES_OUT}/.stamp"
  DEPENDS ${SDE_THEME_DEPS} ${SDE_THEME_OVERLAYS} "${PROJECT_SOURCE_DIR}/suite/themes.lock.json"
  COMMENT "Building the SDE themes" VERBATIM)
add_custom_target(sde-themes ALL DEPENDS "${SDE_THEMES_OUT}/.stamp")
install(DIRECTORY "${SDE_THEMES_OUT}/" DESTINATION ${CMAKE_INSTALL_DATADIR}/themes PATTERN ".stamp" EXCLUDE)
install(FILES data/themes/palettes.json suite/themes.lock.json DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde/themes)
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
