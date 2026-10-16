# SPDX-License-Identifier: GPL-3.0-or-later

add_library(sde-common STATIC
  common/config.cpp
  common/desktop_entry.cpp
  common/icons.cpp
  common/paint.cpp
  common/spawn.cpp
  common/theme.cpp
  common/util.cpp
  common/wm.cpp
  common/x11.cpp
)
target_include_directories(sde-common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${CMAKE_BINARY_DIR}/generated)
target_compile_options(sde-common PUBLIC ${FLTK_CXXFLAGS})
target_link_libraries(sde-common PUBLIC
  ${FLTK_LDFLAGS} PkgConfig::CAIRO PkgConfig::RSVG ${X11_LIBRARIES})

# The session manager does not use FLTK; it only needs Xlib.
add_executable(sde-session session/main.cpp)
target_include_directories(sde-session PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/common ${CMAKE_BINARY_DIR}/generated)
target_sources(sde-session PRIVATE
  common/config.cpp common/desktop_entry.cpp common/spawn.cpp common/util.cpp
  common/wm.cpp)
target_link_libraries(sde-session PRIVATE ${X11_LIBRARIES})

add_executable(sde-panel
  panel/main.cpp
  panel/panel.cpp
  panel/menu.cpp
  panel/tray.cpp)
target_link_libraries(sde-panel PRIVATE sde-common)

add_executable(sde-desktop desktop/main.cpp)
target_link_libraries(sde-desktop PRIVATE sde-common)

add_executable(sde-settings settings/main.cpp)
target_link_libraries(sde-settings PRIVATE sde-common)

add_executable(sde-run run/main.cpp)
target_link_libraries(sde-run PRIVATE sde-common)

add_executable(sde-logout logout/main.cpp)
target_link_libraries(sde-logout PRIVATE sde-common)

install(TARGETS sde-session sde-panel sde-desktop sde-settings sde-run sde-logout
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
