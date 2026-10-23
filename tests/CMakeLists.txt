# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(sde-tests test_main.cpp)
target_link_libraries(sde-tests PRIVATE sde-common)
add_test(NAME sde-tests COMMAND sde-tests)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(sde-update-tests test_update_model.cpp ../src/update/model.cpp)
target_include_directories(sde-update-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/update)
target_compile_definitions(sde-update-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(sde-update-tests PRIVATE sde-common)
add_test(NAME sde-update-protocols COMMAND sde-update-tests)
add_test(NAME sde-update-model COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_updates.py)
add_test(NAME sde-native-branding COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_native_forks.py)
add_test(NAME sde-rust-tests
  COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${SDE_CARGO_TARGET_DIR}"
          ${CARGO_EXECUTABLE} test --manifest-path "${PROJECT_SOURCE_DIR}/Cargo.toml"
          --workspace --locked --offline)

# Test helper: a StatusNotifierItem with a menu (not installed).
add_executable(sde-sni-demo sni_demo.cpp)
target_link_libraries(sde-sni-demo PRIVATE sde-common)

# Calculator: parser, precedence, percent, functions, errors, formatting,
# programmer mode, converter and history; then the command line (--eval).
add_executable(calculator-tests test_calculator.cpp)
target_link_libraries(calculator-tests PRIVATE calculator-core)
add_test(NAME calculator-engine COMMAND calculator-tests)
add_test(NAME sde-calculator COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_calculator.py $<TARGET_FILE:calculator>)

add_executable(sde-display-tests test_display.cpp)
target_link_libraries(sde-display-tests PRIVATE sde-common)
add_test(NAME sde-display-model COMMAND sde-display-tests)
add_executable(sde-display-preview-probe display_preview_probe.cpp)
target_link_libraries(sde-display-preview-probe PRIVATE sde-common)
find_program(SDE_XVFB Xvfb)
if(SDE_XVFB)
  add_test(NAME sde-display-preview COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_display_preview.py $<TARGET_FILE:sde-display-preview-probe>)
endif()

add_executable(sde-panel-controls-tests test_panel_controls.cpp ../src/panel/controls.cpp
  ../src/panel/status_model.cpp ../src/panel/drives.cpp)
target_link_libraries(sde-panel-controls-tests PRIVATE sde-common)
add_test(NAME sde-panel-controls COMMAND sde-panel-controls-tests)
add_executable(sde-menu-model-tests test_menu_model.cpp ../src/panel/menu_model.cpp)
target_link_libraries(sde-menu-model-tests PRIVATE sde-common)
add_test(NAME sde-menu-model COMMAND sde-menu-model-tests)
# 0.2: the panel's application catalog (inotify; no display needed).
add_executable(sde-app-catalog-tests test_app_catalog.cpp ../src/panel/app_catalog.cpp
  ../src/panel/menu_model.cpp ../src/panel/menu_perf.cpp)
target_link_libraries(sde-app-catalog-tests PRIVATE sde-common)
add_test(NAME sde-app-catalog COMMAND sde-app-catalog-tests)
# QA-only D-Bus/XEmbed fixtures: deliberately not installed in the packages.
add_executable(sde-panel-service-fixture panel_service_fixture.cpp)
target_link_libraries(sde-panel-service-fixture PRIVATE sde-common)

# alpha12 panel crash: D-Bus call timeouts (needs dbus-run-session).
find_program(SDE_DBUS_RUN_SESSION dbus-run-session)
add_executable(sde-dbus-timeout-tests test_dbus_timeout.cpp)
target_link_libraries(sde-dbus-timeout-tests PRIVATE sde-common)
if(SDE_DBUS_RUN_SESSION)
  add_test(NAME sde-dbus-timeouts COMMAND ${SDE_DBUS_RUN_SESSION} -- $<TARGET_FILE:sde-dbus-timeout-tests>)
  set_tests_properties(sde-dbus-timeouts PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif()

add_executable(sde-xsettings-tests test_xsettings.cpp)
target_link_libraries(sde-xsettings-tests PRIVATE sde-xsettings-model)
add_test(NAME sde-xsettings COMMAND sde-xsettings-tests)

add_executable(sde-gallery-tests test_gallery.cpp)
target_link_libraries(sde-gallery-tests PRIVATE sde-gallery-core)
add_test(NAME sde-gallery-model COMMAND sde-gallery-tests)

# Panel clock: alarms and agenda model (parsing, ring and reminder times).
add_executable(sde-agenda-tests test_agenda.cpp ../src/panel/agenda.cpp)
add_test(NAME sde-agenda-model COMMAND sde-agenda-tests)

# SDE System Monitor /proc and /sys parsers.
add_executable(sde-sysmon-tests test_sysmon.cpp)
target_link_libraries(sde-sysmon-tests PRIVATE sde-sysmon-core)
add_test(NAME sde-sysmon-parsers COMMAND sde-sysmon-tests)

add_executable(sde-settings-tests test_settings.cpp)
target_link_libraries(sde-settings-tests PRIVATE sde-settings-model)
add_test(NAME sde-settings-model COMMAND sde-settings-tests ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pci.ids)

add_executable(sde-xanara-tests test_xanara.cpp)
target_link_libraries(sde-xanara-tests PRIVATE sde-xanara-core)
add_test(NAME sde-xanara COMMAND sde-xanara-tests)

add_executable(sde-taca-tests test_taca.cpp)
target_link_libraries(sde-taca-tests PRIVATE taca-core)
add_test(NAME taca-engine COMMAND sde-taca-tests)
set_tests_properties(taca-engine PROPERTIES TIMEOUT 120)

add_executable(sde-photo-tests test_photo.cpp)
target_link_libraries(sde-photo-tests PRIVATE sde-photo-core)
add_test(NAME sde-photo-model COMMAND sde-photo-tests)
add_executable(sde-photo-draw-tests test_photo_draw.cpp)
target_link_libraries(sde-photo-draw-tests PRIVATE sde-photo-core)
add_test(NAME sde-photo-draw COMMAND sde-photo-draw-tests)

# Niny Text Editor: codec, highlighter, find/replace, undo, settings, text ops.
add_executable(niny-tests test_niny.cpp)
target_link_libraries(niny-tests PRIVATE niny-core)
add_test(NAME niny-core COMMAND niny-tests)

# SDE Screen Lock and SDE Power: PAM conversation (fake PAM stack), password
# buffer, attempt delays, inhibitors, idle stages, battery levels, brightness.
add_executable(sde-lock-tests test_lock.cpp)
target_link_libraries(sde-lock-tests PRIVATE sde-lock-core sde-power-core)
add_test(NAME sde-lock-power-model COMMAND sde-lock-tests)
# QA-only lock screen with a fake authenticator (not installed).
add_executable(sde-lock-ui-fixture lock_ui_fixture.cpp)
target_link_libraries(sde-lock-ui-fixture PRIVATE sde-lock-ui)
if(SDE_XVFB)
  add_test(NAME sde-lock-screen COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lock_ui.py $<TARGET_FILE:sde-lock-ui-fixture>)
  set_tests_properties(sde-lock-screen PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
  # Alpha 51: grabs held by other clients, monitor changes, resume.
  add_test(NAME sde-lock-input COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lock_input.py $<TARGET_FILE:sde-lock-ui-fixture>)
  set_tests_properties(sde-lock-input PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()
if(SDE_XVFB AND SDE_DBUS_RUN_SESSION)
  add_test(NAME sde-lock-daemon COMMAND ${SDE_DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_lock_daemon.py $<TARGET_FILE:sde-lock>)
  set_tests_properties(sde-lock-daemon PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()

# About windows open and close without crashing (Niny alpha 50 regression).
if(SDE_XVFB)
  add_test(NAME sde-about-windows COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_about_ui.py
    niny=$<TARGET_FILE:niny> sysmon=$<TARGET_FILE:sde-system-monitor>
    run=$<TARGET_FILE:sde-run> display=$<TARGET_FILE:sde-display-settings>
    calculator=$<TARGET_FILE:calculator> welcome=$<TARGET_FILE:sde-welcome>
    reports=$<TARGET_FILE:sde-system-reports> licenses=$<TARGET_FILE:sde-ui-kit-demo>)
  set_tests_properties(sde-about-windows PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# Test helper: stock FLTK menus, dialogs and widgets in the SDE look (not
# installed); used for screenshots of the UI kit (docs/UI-KIT.md).
add_executable(sde-ui-kit-demo ui_kit_demo.cpp)
target_link_libraries(sde-ui-kit-demo PRIVATE sde-common)
add_test(NAME sde-developer-metadata COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_metainfo.py)
# Legal data (data/legal/components.json) against the desktop files, metainfo,
# suite.tsv, the SPDX headers of the sources and debian/copyright.
add_test(NAME sde-legal-data COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_legal_data.py)

# Action Center weather and Night Light models (offline JSON fixtures).
add_executable(sde-weather-tests test_weather.cpp ../src/panel/weather.cpp ../src/panel/nightlight.cpp)
target_compile_definitions(sde-weather-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(sde-weather-tests PRIVATE sde-common)
add_test(NAME sde-weather-model COMMAND sde-weather-tests)

# Clock faces (alpha52): formats, seven-segment masks, day or night.
add_executable(sde-clockface-tests test_clockface.cpp ../src/panel/clockface.cpp ../src/panel/nightlight.cpp ../src/panel/weather.cpp)
target_link_libraries(sde-clockface-tests PRIVATE sde-common)
add_test(NAME sde-clockface-model COMMAND sde-clockface-tests)

# Clock "Sync time zone": geolocation answers, zone names, the timedatectl plan.
add_executable(sde-timezone-tests test_timezone.cpp ../src/panel/timezone.cpp ../src/panel/weather.cpp)
target_compile_definitions(sde-timezone-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(sde-timezone-tests PRIVATE sde-common)
add_test(NAME sde-timezone-model COMMAND sde-timezone-tests)

# alpha50 panel status rules (tooltips, network LED, keyboard indicators,
# UDisks2 drives, touchpad properties) and the grab helper of panel-smoke.
add_executable(sde-panel-status-tests test_panel_status.cpp ../src/panel/status_model.cpp)
target_link_libraries(sde-panel-status-tests PRIVATE sde-common)
add_test(NAME sde-panel-status COMMAND sde-panel-status-tests)
add_executable(sde-grab-fixture grab_fixture.cpp)
target_link_libraries(sde-grab-fixture PRIVATE ${X11_LIBRARIES})
# alpha51: a plain application window for the taskbar preview checks.
add_executable(sde-preview-window preview_window_fixture.cpp)
target_link_libraries(sde-preview-window PRIVATE ${X11_LIBRARIES})

# SDE Welcome, Driver Manager and System Reports: firmware log mapping, GPU
# heuristics, APT sources rewriting, crash log parsing, issue URLs.
add_executable(sde-sysreport-tests test_sysreport.cpp)
target_link_libraries(sde-sysreport-tests PRIVATE sde-sysreport-core)
add_test(NAME sde-sysreport-model COMMAND sde-sysreport-tests ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sysreport)

# debion-network-check: interfaces handover, DHCP/DNS/WPAD parsers and the
# --quick --json verdicts and the verified DNS repair (--fix, --dns-fallback)
# against a simulated NetworkManager, with fixtures and stub commands.
add_test(NAME debion-network-check COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_network_check.py)
set_tests_properties(debion-network-check PROPERTIES TIMEOUT 300)

# Caption buttons and their migration, transparency contrast, icon theme
# lookup (tests/fixtures/icon-themes) and the build history of Settings > About.
add_executable(sde-theme-tests test_theme.cpp)
target_link_libraries(sde-theme-tests PRIVATE sde-settings-model sde-common)
target_compile_definitions(sde-theme-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                   SDE_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(sde-theme-tests sde-build-info sde-themes)
add_test(NAME sde-theme COMMAND sde-theme-tests)

# Desktop drag and drop: drop actions, URI lists, positions, file operations.
add_executable(sde-desktop-tests test_desktop.cpp)
target_link_libraries(sde-desktop-tests PRIVATE sde-desktop-model)
add_test(NAME sde-desktop-model COMMAND sde-desktop-tests)
# Test helper: an independent XDND source and target (Xlib only).
find_package(X11 REQUIRED)
add_executable(sde-xdnd-peer xdnd_peer.cpp)
target_link_libraries(sde-xdnd-peer PRIVATE ${X11_LIBRARIES})
if(SDE_XVFB)
  add_test(NAME sde-desktop-dnd COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_desktop_dnd.py $<TARGET_FILE:sde-desktop> $<TARGET_FILE:sde-xdnd-peer>)
  set_tests_properties(sde-desktop-dnd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()

# Settings > Default Applications: categories, mimeapps.list, defaults.
add_executable(sde-default-apps-tests test_default_apps.cpp)
target_link_libraries(sde-default-apps-tests PRIVATE sde-settings-model)
add_test(NAME sde-default-apps COMMAND sde-default-apps-tests)
