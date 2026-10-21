# SPDX-License-Identifier: GPL-3.0-or-later

add_library(sde-common STATIC
  display/backend.cpp
  display/profile.cpp
  common/config.cpp
  common/integration.cpp
  common/dbus.cpp
  common/desktop_entry.cpp
  common/glyphs.cpp
  common/i18n.cpp
  common/icons.cpp
  common/paint.cpp
  common/process.cpp
  common/spawn.cpp
  common/suite.cpp
  common/theme.cpp
  common/util.cpp
  common/widgets.cpp
  common/wm.cpp
  common/x11.cpp
)
target_include_directories(sde-common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${CMAKE_BINARY_DIR}/generated)
target_compile_options(sde-common PUBLIC ${FLTK_CXXFLAGS})
target_link_libraries(sde-common PUBLIC
  ${FLTK_LDFLAGS} PkgConfig::CAIRO PkgConfig::RSVG PkgConfig::DBUS ${X11_LIBRARIES}
  sde-rust-config PkgConfig::XRANDR PkgConfig::XINPUT)
add_dependencies(sde-common sde-rust-build)

# The session manager draws nothing itself, but shares the configuration,
# theme and window manager helpers with the other programs.
add_executable(sde-session session/main.cpp)
target_link_libraries(sde-session PRIVATE sde-common)

add_executable(sde-panel
  panel/main.cpp
  panel/panel.cpp
  panel/menu.cpp
  panel/popup.cpp
  panel/quick.cpp
  panel/controls.cpp
  panel/calendar.cpp
  panel/sni.cpp
  panel/tray.cpp)
target_link_libraries(sde-panel PRIVATE sde-common)

add_executable(sde-desktop desktop/main.cpp)
target_link_libraries(sde-desktop PRIVATE sde-common)

add_executable(sde-settings
  settings/main.cpp
  settings/ui.cpp
  settings/display.cpp
  settings/input.cpp
  settings/system.cpp)
target_link_libraries(sde-settings PRIVATE sde-common)

add_executable(sde-run run/main.cpp)
target_link_libraries(sde-run PRIVATE sde-common)

add_executable(sde-logout logout/main.cpp)
target_link_libraries(sde-logout PRIVATE sde-common)

add_executable(sde-notifyd notifyd/main.cpp)
target_link_libraries(sde-notifyd PRIVATE sde-common)

add_executable(sde-update update/main.cpp)
target_link_libraries(sde-update PRIVATE sde-common)

install(TARGETS sde-session sde-panel sde-desktop sde-settings sde-run sde-logout sde-notifyd sde-update
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(SDE_BUILD_COMPONENTS)
  add_subdirectory(calculator)
endif()

# SDE System Monitor: native from-scratch rewrite (DebION Dev-Team).
# The /proc and /sys parsers live in a small library shared with the tests.
add_library(sde-sysmon-core STATIC sysmon/procfs.cpp sysmon/sampler.cpp)
target_include_directories(sde-sysmon-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sysmon)
find_package(Threads REQUIRED)
target_link_libraries(sde-sysmon-core PUBLIC Threads::Threads)
add_executable(sde-system-monitor
  sysmon/main.cpp
  sysmon/widgets.cpp
  sysmon/apps.cpp
  sysmon/processes.cpp
  sysmon/performance.cpp
  sysmon/filesystems.cpp
  sysmon/startup.cpp)
string(JSON SDE_SYSMON_VERSION ERROR_VARIABLE SDE_SYSMON_VERSION_ERROR
       GET "${SDE_APP_VERSIONS}" sde-system-monitor)
if(SDE_SYSMON_VERSION_ERROR OR NOT SDE_SYSMON_VERSION)
  set(SDE_SYSMON_VERSION "${SDE_VERSION}")
endif()
target_compile_definitions(sde-system-monitor PRIVATE SDE_SYSMON_VERSION="${SDE_SYSMON_VERSION}")
target_link_libraries(sde-system-monitor PRIVATE sde-sysmon-core sde-common)
install(TARGETS sde-system-monitor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(sde-display-settings display/main.cpp settings/display.cpp settings/ui.cpp)
target_include_directories(sde-display-settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/settings)
target_link_libraries(sde-display-settings PRIVATE sde-common)
install(TARGETS sde-display-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES display/profile.LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde/upstream/DisplayProfileManager RENAME copyright)
if(SDE_BUILD_COMPONENTS)
  install(FILES "${SDE_COMPONENT_SOURCES}/sde-display-settings/LICENSE"
                "${SDE_COMPONENT_SOURCES}/sde-display-settings/copyright"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde/upstream/DisplayProfileManager)
endif()
