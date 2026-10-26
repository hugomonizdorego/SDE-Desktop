# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
#
# Debrun, the installer of downloaded Debian packages (docs/DEBRUN.md),
# written from scratch by the Selviana DE Project. debrun-core (the .deb
# reader, versions and relations, dpkg's status file, APT's simulation and
# the helper's contract) needs no display: the program, the root helper and
# the unit tests share it.
set(_src ${PROJECT_SOURCE_DIR}/src)

add_library(debrun-core STATIC
  ${_src}/debrun/deb.cpp
  ${_src}/debrun/system.cpp
  ${_src}/debrun/helper.cpp
  ${_src}/common/sha256.cpp)
target_include_directories(debrun-core PUBLIC ${_src}/debrun ${_src}/common ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(debrun-core PUBLIC PkgConfig::LIBARCHIVE)
# The string and gettext helpers of sde-common, for the programs that do
# not link the toolkit (the root helper, the unit tests).
add_library(debrun-base STATIC ${_src}/common/util.cpp ${_src}/common/i18n.cpp)
target_include_directories(debrun-base PUBLIC ${_src}/common ${CMAKE_BINARY_DIR}/generated)

# The root part, started by pkexec (data/polkit/org.debion.sde.debrun.policy).
add_executable(debrun-helper ${_src}/debrun/helper_main.cpp)
target_link_libraries(debrun-helper PRIVATE debrun-core debrun-base)
install(TARGETS debrun-helper RUNTIME DESTINATION libexec/sde)  # SDE_LIBEXECDIR/sde

configure_file(${PROJECT_SOURCE_DIR}/data/polkit/org.debion.sde.debrun.policy.in
               ${CMAKE_BINARY_DIR}/polkit/org.debion.sde.debrun.policy @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/polkit/org.debion.sde.debrun.policy
        DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
