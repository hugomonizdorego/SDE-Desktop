# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DebION Dev-Team
#
# SDE Welcome (sde-welcome), SDE Driver Manager (sde-drivers) and SDE System
# Reports (sde-system-reports): first-login guide and system tools after the
# Linux Mint concepts in docs/MINT-CONCEPTS.md, written from scratch by the
# DebION Dev-Team. The parsers are a display-free library for the tests.
set(_src ${PROJECT_SOURCE_DIR}/src)

add_library(sde-sysreport-core STATIC
  ${_src}/sysreport/drivers.cpp
  ${_src}/sysreport/reports.cpp
  ${_src}/welcome/model.cpp)
target_include_directories(sde-sysreport-core PUBLIC ${_src}/sysreport ${_src}/welcome)
# Graphics card detection is shared with Settings > About.
target_link_libraries(sde-sysreport-core PUBLIC sde-settings-model sde-common)

# Window shell and pages; the page widgets come from sde-settings (ui.cpp).
add_library(sde-sysreport-ui STATIC
  ${_src}/sysreport/shell.cpp
  ${_src}/sysreport/pages.cpp
  ${_src}/settings/ui.cpp)
target_link_libraries(sde-sysreport-ui PUBLIC sde-sysreport-core)

add_executable(sde-welcome ${_src}/welcome/main.cpp)
target_link_libraries(sde-welcome PRIVATE sde-sysreport-ui)
add_executable(sde-drivers ${_src}/sysreport/drivers_main.cpp)
target_link_libraries(sde-drivers PRIVATE sde-sysreport-ui)
add_executable(sde-system-reports ${_src}/sysreport/reports_main.cpp)
target_link_libraries(sde-system-reports PRIVATE sde-sysreport-ui)

install(TARGETS sde-welcome sde-drivers sde-system-reports RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
