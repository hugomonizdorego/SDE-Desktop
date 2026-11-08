# SPDX-License-Identifier: GPL-3.0-or-later
# Window management of SDE 0.2.0 Alpha 4 (docs/WINDOWS.md, docs/SHORTCUTS.md):
#   sde-window-menu  the window menu (SDE WM patch 0006 runs it)
#   sde-windows      the window and workspace commands of the shortcuts
#   sde-taskview     Task View (Win+Tab): the workspaces and their windows
#   sde-screenshot   screenshots (PrtScn, Alt+PrtScn, Win+Shift+S)
# The decisions that need no display are a library shared with the tests.
add_library(sde-windows-core STATIC snap.cpp taskview_model.cpp screenshot_model.cpp)
target_include_directories(sde-windows-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sde-windows-core PUBLIC sde-common)

add_executable(sde-window-menu window_menu_main.cpp)
target_link_libraries(sde-window-menu PRIVATE sde-windows-core)

add_executable(sde-windows windows_main.cpp)
target_link_libraries(sde-windows PRIVATE sde-windows-core)

add_executable(sde-taskview taskview_main.cpp)
target_link_libraries(sde-taskview PRIVATE sde-windows-core)
if(XCOMPOSITE_FOUND)
  target_link_libraries(sde-taskview PRIVATE PkgConfig::XCOMPOSITE)
  target_compile_definitions(sde-taskview PRIVATE SDE_HAVE_XCOMPOSITE=1)
endif()

add_executable(sde-screenshot screenshot_main.cpp)
target_link_libraries(sde-screenshot PRIVATE sde-windows-core)

install(TARGETS sde-window-menu sde-windows sde-taskview sde-screenshot
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The printable shortcut sheet (tools/make-shortcut-sheet.py) where Welcome
# and the Control Center open it (shortcuts::sheet_path(); not under
# /usr/share/doc, which may be left out or compressed), and with the guides.
install(FILES ${PROJECT_SOURCE_DIR}/docs/SDE-Keyboard-Shortcuts.pdf DESTINATION ${CMAKE_INSTALL_DATADIR}/sde/doc)
install(FILES ${PROJECT_SOURCE_DIR}/docs/SHORTCUTS.md ${PROJECT_SOURCE_DIR}/docs/WINDOWS.md
              ${PROJECT_SOURCE_DIR}/docs/SDE-Keyboard-Shortcuts.pdf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde)
