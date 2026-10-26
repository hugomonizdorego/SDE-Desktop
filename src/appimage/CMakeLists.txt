# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
#
# sde-appimage: AppImages run with a double-click after they were verified
# (docs/APPIMAGE.md), written from scratch by the Selviana DE Project. The
# model (ELF and AppImage sections, digests, OpenPGP armor, gpgv's answers,
# the trust store and the policy) has no display, for the unit tests.
set(_src ${PROJECT_SOURCE_DIR}/src)

add_library(sde-appimage-core STATIC ${_src}/appimage/appimage.cpp ${_src}/common/sha256.cpp)
target_include_directories(sde-appimage-core PUBLIC ${_src}/appimage ${_src}/common ${CMAKE_BINARY_DIR}/generated)

add_executable(sde-appimage ${_src}/appimage/main.cpp)
# GIO moves AppImages that are deleted to the trash.
target_link_libraries(sde-appimage PRIVATE sde-appimage-core sde-common PkgConfig::GIO)
install(TARGETS sde-appimage RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The administrator's policy (a conffile of the package).
install(FILES ${PROJECT_SOURCE_DIR}/data/appimage/appimage.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/sde)
install(FILES ${PROJECT_SOURCE_DIR}/docs/APPIMAGE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde)
