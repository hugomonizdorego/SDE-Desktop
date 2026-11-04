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

# 0.2.0 Alpha 2: the copy and move progress (src/common/transfer_model.h,
# transfer_dialog.*): the model and texts, and the dialog on Xvfb with a
# simulated copy (sde-transfer-demo, not installed; also for showcase.py).
add_executable(sde-transfer-tests test_transfer.cpp)
target_link_libraries(sde-transfer-tests PRIVATE sde-common)
target_compile_definitions(sde-transfer-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME sde-transfer-model COMMAND sde-transfer-tests)
add_executable(sde-transfer-demo transfer_demo.cpp)
target_link_libraries(sde-transfer-demo PRIVATE sde-common)
# 0.2.0 Alpha 3: safe copying (src/common/safe_copy.h): the writer against a
# simulated slow drive, and a benchmark for real drives (not installed;
# tools/safe-copy-bench.py).
add_executable(sde-safe-copy-tests test_safe_copy.cpp)
target_link_libraries(sde-safe-copy-tests PRIVATE sde-common)
add_test(NAME sde-safe-copy COMMAND sde-safe-copy-tests)
add_executable(sde-safe-copy-bench safe_copy_bench.cpp)
target_link_libraries(sde-safe-copy-bench PRIVATE sde-desktop-model sde-common)

# 0.2.0 Alpha 3.1: the SDE file chooser (src/common/file_chooser*.h): its
# model (sorting, filters, history, completion, places, recent files,
# bookmarks, drives, thumbnails, state) without a display, and the dialog on
# Xvfb through sde-file-chooser-demo (not installed; also for showcase.py).
add_executable(sde-file-chooser-tests test_file_chooser.cpp)
target_link_libraries(sde-file-chooser-tests PRIVATE sde-common)
add_test(NAME sde-file-chooser-model COMMAND sde-file-chooser-tests)
add_executable(sde-file-chooser-demo file_chooser_demo.cpp)
target_link_libraries(sde-file-chooser-demo PRIVATE sde-common)

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
  add_test(NAME sde-transfer-dialog COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_transfer_dialog.py $<TARGET_FILE:sde-transfer-demo>)
  set_tests_properties(sde-transfer-dialog PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
    ENVIRONMENT "SDE_BRANDING_DIR=${PROJECT_SOURCE_DIR}/data/branding")
endif()
# The file chooser on Xvfb (tests/test_file_chooser_ui.py; a drop through
# sde-xdnd-peer); skipped without xdotool.
if(SDE_XVFB)
  add_test(NAME sde-file-chooser-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_file_chooser_ui.py
    $<TARGET_FILE:sde-file-chooser-demo> $<TARGET_FILE:sde-xdnd-peer>)
  set_tests_properties(sde-file-chooser-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
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
# The FileChooser portal backend (xdg-desktop-portal-sde) on a private bus.
if(SDE_XVFB AND SDE_DBUS_RUN_SESSION)
  add_test(NAME sde-file-chooser-portal COMMAND ${SDE_DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_file_chooser_portal.py $<TARGET_FILE:xdg-desktop-portal-sde>)
  set_tests_properties(sde-file-chooser-portal PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
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
# 0.2.0 Alpha 4: My processes by default, the kernel's threads described in
# their own group on request, never offered to be ended (Xvfb).
if(SDE_XVFB AND TARGET sde-system-monitor)
  add_test(NAME sde-sysmon-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_sysmon_ui.py
    $<TARGET_FILE:sde-system-monitor>)
  set_tests_properties(sde-sysmon-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()

add_executable(sde-settings-tests test_settings.cpp)
target_link_libraries(sde-settings-tests PRIVATE sde-settings-model)
add_test(NAME sde-settings-model COMMAND sde-settings-tests ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pci.ids)
# Control Center > Users & Avatar: user names, passwords and their hashes,
# the administrator rules, AccountsService calls and passwd(1).
add_executable(sde-accounts-tests test_accounts.cpp)
target_link_libraries(sde-accounts-tests PRIVATE sde-settings-model)
add_test(NAME sde-accounts-model COMMAND sde-accounts-tests)

add_executable(sde-xanara-tests test_xanara.cpp)
target_link_libraries(sde-xanara-tests PRIVATE sde-xanara-core)
add_test(NAME sde-xanara COMMAND sde-xanara-tests)
# 0.2.0 Alpha 3: Xanara's direct editing in the window (click a word, type,
# save, reopen, move a picture, close the tab), checked with poppler by
# sde-xanara-probe; no other program may run.
add_executable(sde-xanara-probe xanara_probe.cpp)
target_link_libraries(sde-xanara-probe PRIVATE sde-xanara-core)
if(SDE_XVFB AND TARGET xanara)
  # 0.2.0 Alpha 4: the start page first (new documents, a tool, a drop
  # through sde-xdnd-peer).
  add_test(NAME sde-xanara-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_xanara_ui.py
    $<TARGET_FILE:xanara> $<TARGET_FILE:sde-xanara-probe> $<TARGET_FILE:sde-xdnd-peer>)
  set_tests_properties(sde-xanara-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

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
# Photo's collages (the ten designs) and the plugins of Photo and Gallery.
add_executable(sde-collage-tests test_collage.cpp)
target_link_libraries(sde-collage-tests PRIVATE sde-photo-core)
add_test(NAME sde-photo-collage COMMAND sde-collage-tests)
add_executable(sde-gallery-plugins-tests test_gallery_plugins.cpp)
target_link_libraries(sde-gallery-plugins-tests PRIVATE sde-gallery-core)
target_compile_definitions(sde-gallery-plugins-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME sde-gallery-plugins COMMAND sde-gallery-plugins-tests)
add_executable(sde-photo-plugins-tests test_photo_plugins.cpp)
target_link_libraries(sde-photo-plugins-tests PRIVATE sde-photo-core)
add_test(NAME sde-photo-plugins COMMAND sde-photo-plugins-tests)

# Niny Text Editor: codec, highlighter, find/replace, undo, settings, text ops.
add_executable(niny-tests test_niny.cpp)
target_link_libraries(niny-tests PRIVATE niny-core)
add_test(NAME niny-core COMMAND niny-tests)
# Niny's tabs and menu bar on Xvfb (0.2 Alpha 3): each × closes its own tab,
# the only tab stays, the menus open from the mouse and keyboard and fit.
if(SDE_XVFB)
  add_test(NAME niny-tabs-menus COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_niny_ui.py
    $<TARGET_FILE:niny>)
  set_tests_properties(niny-tabs-menus PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

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
    debrun=$<TARGET_FILE:debrun>
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
# 0.2.0 Alpha 3.1: the Action Center's session buttons (order, logind's
# answers, the second click, the glyphs).
add_executable(sde-session-actions-tests test_session_actions.cpp ../src/panel/session_actions.cpp)
target_link_libraries(sde-session-actions-tests PRIVATE sde-common)
add_test(NAME sde-session-actions COMMAND sde-session-actions-tests)
add_executable(sde-grab-fixture grab_fixture.cpp)
target_link_libraries(sde-grab-fixture PRIVATE ${X11_LIBRARIES})
# alpha51: a plain application window for the taskbar preview checks.
add_executable(sde-preview-window preview_window_fixture.cpp)
target_link_libraries(sde-preview-window PRIVATE ${X11_LIBRARIES})
# 0.2.0 Alpha 3.1: a window that stops answering _NET_WM_PING on SIGUSR1
# (Force Close checks, showcase), and a pinger.
add_executable(sde-frozen-client frozen_client_fixture.cpp)
target_link_libraries(sde-frozen-client PRIVATE ${X11_LIBRARIES})
# Force Close: which processes may be ended, the watchdog's decisions and
# the launch records (no display).
add_executable(sde-force-close-tests test_force_close.cpp)
target_link_libraries(sde-force-close-tests PRIVATE sde-forceclose-core)
add_test(NAME sde-force-close-rules COMMAND sde-force-close-tests)

# SDE Welcome, Driver Manager and System Reports: firmware log mapping, GPU
# heuristics, APT sources rewriting, crash log parsing, issue URLs.
add_executable(sde-sysreport-tests test_sysreport.cpp)
target_link_libraries(sde-sysreport-tests PRIVATE sde-sysreport-core)
add_test(NAME sde-sysreport-model COMMAND sde-sysreport-tests ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sysreport
                                         ${CMAKE_SOURCE_DIR}/data/welcome)

# debion-network-check: interfaces handover, DHCP/DNS/WPAD parsers and the
# --quick --json verdicts and the verified DNS repair (--fix, --dns-fallback)
# against a simulated NetworkManager, with fixtures and stub commands.
add_test(NAME debion-network-check COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_network_check.py)
set_tests_properties(debion-network-check PROPERTIES TIMEOUT 300)

# Caption buttons and their migration, transparency contrast, icon theme
# lookup (tests/fixtures/icon-themes) and the build history of Settings > About.
# Application icons (0.2 Alpha 2): near matches, SDE drawings and category
# icons when the theme lacks Icon= (tests/fixtures/icon-themes).
add_executable(sde-app-icons-tests test_app_icons.cpp)
target_link_libraries(sde-app-icons-tests PRIVATE sde-common)
target_compile_definitions(sde-app-icons-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME sde-app-icons COMMAND sde-app-icons-tests)

add_executable(sde-theme-tests test_theme.cpp)
target_link_libraries(sde-theme-tests PRIVATE sde-settings-model sde-common)
target_compile_definitions(sde-theme-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                   SDE_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(sde-theme-tests sde-build-info sde-themes)
add_test(NAME sde-theme COMMAND sde-theme-tests)
# Control Center > Appearance (0.2.0 Alpha 3.1): the chosen and the applied
# look, and the files a theme switch writes (an accent change leaves the
# compositor alone; the GTK block follows the theme).
add_executable(sde-appearance-tests test_appearance.cpp)
target_link_libraries(sde-appearance-tests PRIVATE sde-settings-model sde-common)
target_compile_definitions(sde-appearance-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                        SDE_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(sde-appearance-tests sde-themes)
add_test(NAME sde-appearance-model COMMAND sde-appearance-tests)
# The theme switch through the session (0.2.0 Alpha 3.1): each program told
# once and only when its input changed, the compositor reloaded in place and
# never restarted, no pkill (stand-ins log the signals; needs Xvfb).
if(SDE_XVFB)
  add_test(NAME sde-theme-switch COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_theme_switch.py
    $<TARGET_FILE:sde-session> ${CMAKE_BINARY_DIR})
  set_tests_properties(sde-theme-switch PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  # Appearance: choose, Preview (with its GTK part), Cancel, Revert, Apply
  # in the preview and Ctrl+Enter, on Xvfb.
  add_test(NAME sde-appearance-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_appearance_ui.py
    $<TARGET_FILE:sde-settings> ${CMAKE_BINARY_DIR}
    $<$<TARGET_EXISTS:sde-theme-preview-gtk>:$<TARGET_FILE:sde-theme-preview-gtk>>)
  set_tests_properties(sde-appearance-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
endif()
# 0.2.0 Alpha 3.1: every translucent surface (Liquid Glass's shader and the
# whole-window translucency of the other families, frames and other
# toolkits' menus) over a busy web page: text contrast, a calm surface and
# even transparency steps, with the compositor configuration SDE generates.
add_executable(sde-glass-tests test_glass.cpp)
target_link_libraries(sde-glass-tests PRIVATE sde-common)
add_test(NAME sde-glass-busy-backdrop COMMAND sde-glass-tests)
set_tests_properties(sde-glass-busy-backdrop PROPERTIES TIMEOUT 300)
# 0.2.0 Alpha 3.1: menus and lists of the SDE GTK themes highlight at once
# (no transitions on menu items, rows and the like in the built style
# sheets), and check buttons are square while radio buttons are round, as
# drawn by GTK 3, GTK 4 (when found) and SDE-FLTK (QA helpers, not
# installed) on Xvfb.
add_executable(sde-fltk-indicator-probe fltk_indicator_probe.cpp)
target_link_libraries(sde-fltk-indicator-probe PRIVATE sde-common)
set(SDE_INDICATOR_PROBES --fltk $<TARGET_FILE:sde-fltk-indicator-probe>)
pkg_check_modules(SDE_PROBE_GTK3 IMPORTED_TARGET gtk+-3.0)
if(SDE_PROBE_GTK3_FOUND)
  add_executable(sde-gtk3-indicator-probe gtk3_indicator_probe.cpp)
  target_link_libraries(sde-gtk3-indicator-probe PRIVATE PkgConfig::SDE_PROBE_GTK3)
  list(APPEND SDE_INDICATOR_PROBES --gtk3 $<TARGET_FILE:sde-gtk3-indicator-probe>)
endif()
pkg_check_modules(SDE_PROBE_GTK4 IMPORTED_TARGET gtk4>=4.14)
if(SDE_PROBE_GTK4_FOUND)
  add_executable(sde-gtk4-indicator-probe gtk4_indicator_probe.cpp)
  target_link_libraries(sde-gtk4-indicator-probe PRIVATE PkgConfig::SDE_PROBE_GTK4)
  list(APPEND SDE_INDICATOR_PROBES --gtk4 $<TARGET_FILE:sde-gtk4-indicator-probe>)
endif()
add_test(NAME sde-theme-motion COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_theme_motion.py
  --themes ${CMAKE_BINARY_DIR}/themes ${SDE_INDICATOR_PROBES})
set_tests_properties(sde-theme-motion PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
# 0.2.0 Alpha 3.1: the SDE compositor shows what windows draw after they were
# mapped or resized, also with Mesa's software OpenGL (compositor patch
# 0002), on Xvfb with the GLX backend. The compositor is the one built with
# the session components (SDE_BUILD_COMPONENTS), SDE_TEST_COMPOSITOR or, at
# run time, $SDE_COMPOSITOR; without one the test is skipped.
set(SDE_TEST_COMPOSITOR "" CACHE FILEPATH "sde-compositor for sde-compositor-redraw (default: the staged build)")
set(SDE_REDRAW_COMPOSITOR "${SDE_TEST_COMPOSITOR}")
if(NOT SDE_REDRAW_COMPOSITOR AND SDE_BUILD_COMPONENTS)
  set(SDE_REDRAW_COMPOSITOR "${CMAKE_BINARY_DIR}/session-stage${CMAKE_INSTALL_PREFIX}/bin/sde-compositor")
endif()
find_package(X11 REQUIRED)
add_executable(sde-compositor-redraw-probe compositor_redraw_probe.cpp)
target_link_libraries(sde-compositor-redraw-probe PRIVATE ${X11_LIBRARIES})
add_test(NAME sde-compositor-redraw COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_compositor_redraw.py
  --compositor "${SDE_REDRAW_COMPOSITOR}" --probe $<TARGET_FILE:sde-compositor-redraw-probe>)
set_tests_properties(sde-compositor-redraw PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
# 0.2.0 Alpha 3.1: sde-session's OpenGL check at login on Xvfb (software
# OpenGL, no GLX, no display); the decision it feeds is in sde-tests.
add_test(NAME sde-gl-probe COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gl_probe.py
  $<TARGET_FILE:sde-gl-probe>)
set_tests_properties(sde-gl-probe PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
# --version without an X display, with the icons installed (FLTK 1.4).
add_test(NAME sde-version-without-display
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_version_no_display.py ${PROJECT_SOURCE_DIR}/data
                 $<TARGET_FILE:sde-settings> $<TARGET_FILE:sde-run> $<TARGET_FILE:sde-logout> $<TARGET_FILE:niny>
                 $<TARGET_FILE:sde-welcome> $<TARGET_FILE:sde-display-settings> $<TARGET_FILE:sde-panel>
                 $<TARGET_FILE:sde-desktop> $<TARGET_FILE:xanara> $<TARGET_FILE:taca>
                 $<$<TARGET_EXISTS:merkadu>:$<TARGET_FILE:merkadu>>)
# Build number and commit of package builds (BUILD-INFO, tools/build-info.py).
add_test(NAME sde-build-info COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_build_info.py)
# Graphite's accent-colored PNG assets move to the SDE accent (tools/build-themes.py).
add_test(NAME sde-theme-assets COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_theme_assets.py)

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

# The user's picture (src/common/avatar.h): decoding, centre crop, ~/.face,
# link and size checks with an in-memory AccountsService; then the D-Bus
# calls against a fake org.freedesktop.Accounts (needs dbus-run-session).
add_executable(sde-avatar-tests test_avatar.cpp)
target_link_libraries(sde-avatar-tests PRIVATE sde-common)
add_test(NAME sde-avatar COMMAND sde-avatar-tests)
if(SDE_DBUS_RUN_SESSION)
  add_test(NAME sde-avatar-accountsservice COMMAND ${SDE_DBUS_RUN_SESSION} -- $<TARGET_FILE:sde-avatar-tests> --dbus)
  set_tests_properties(sde-avatar-accountsservice PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# Merkadu: catalog merging, search, departments (Ubuntu Software Center's
# menu), AppStream conversion, snapd's JSON and HTTP framing, history and
# uninstall decisions (fixtures in tests/fixtures/merkadu).
if(TARGET merkadu-core)
  add_executable(merkadu-tests test_merkadu.cpp)
  target_compile_definitions(merkadu-tests PRIVATE SDE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(merkadu-tests PRIVATE merkadu-core)
  add_test(NAME merkadu-model COMMAND merkadu-tests)
endif()
# The window and `merkadu --uninstall` with the fake sources (no network or
# root): pages, an installation, history and the uninstall exit codes, the
# highlights, Pause and Cancel, Enable Snap Support against a fake snapd
# (fake_snapd.py).
if(TARGET merkadu AND SDE_XVFB)
  add_test(NAME merkadu-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_merkadu_ui.py
    $<TARGET_FILE:merkadu> ${PROJECT_SOURCE_DIR})
  set_tests_properties(merkadu-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 480)
endif()

# Debrun (src/debrun): control files, versions (and dpkg's order), relations,
# .deb files of every compression and broken ones, the changes beyond the
# files, dpkg's status file, APT's simulation and the helper's contract.
add_executable(debrun-tests test_debrun.cpp)
target_link_libraries(debrun-tests PRIVATE debrun-core sde-installer-base)
add_test(NAME debrun-model COMMAND debrun-tests)
# Debrun's window with generated packages and a fake package system
# (tools/debrun_fixture.py), and the PackageKit backend against a fake
# PackageKit on a private bus (debrun-fake-packagekit, not installed).
add_executable(debrun-fake-packagekit fake_packagekit.cpp)
target_link_libraries(debrun-fake-packagekit PRIVATE PkgConfig::DBUS)
if(SDE_XVFB)
  add_test(NAME debrun-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_debrun_ui.py
    $<TARGET_FILE:debrun> $<TARGET_FILE:debrun-fake-packagekit>)
  set_tests_properties(debrun-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
endif()

# sde-appimage (src/appimage): ELF headers and AppImage sections, the signed
# digest, OpenPGP armor, gpgv's answers, the trust store, the policy and
# the verdicts (with a stand-in for gpgv).
add_executable(appimage-tests test_appimage.cpp)
target_link_libraries(appimage-tests PRIVATE sde-appimage-core sde-installer-base)
add_test(NAME appimage-model COMMAND appimage-tests)
# sde-appimage with generated AppImages and real GnuPG signatures
# (tools/appimage_fixture.py; skipped without gpg), its window on Xvfb when
# Xvfb and xdotool are installed.
add_test(NAME appimage-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_appimage_ui.py
  $<TARGET_FILE:sde-appimage>)
set_tests_properties(appimage-ui PROPERTIES TIMEOUT 240)

# Renaming accounts (src/settings/account_rename.*): requests, name rules,
# checks and file edits; as root, whole renames, failures undone at every
# step and interrupted renames in a fake root (usermod --prefix); the
# helper's commands and refusals.
add_executable(account-rename-tests test_account_rename.cpp ${PROJECT_SOURCE_DIR}/src/settings/account_rename.cpp)
target_include_directories(account-rename-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/settings)
target_link_libraries(account-rename-tests PRIVATE sde-installer-base)
add_test(NAME account-rename COMMAND account-rename-tests $<TARGET_FILE:sde-account-rename>)
# Users & Avatar: renaming an account with the fake AccountsService and the
# real helper on a fake root (no pkexec), on Xvfb.
if(SDE_XVFB)
  add_test(NAME account-rename-ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_account_rename_ui.py
    $<TARGET_FILE:sde-settings> $<TARGET_FILE:sde-account-rename> $<TARGET_FILE:sde-panel-service-fixture>)
  set_tests_properties(account-rename-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
endif()

# startsde: the Flatpak and Snap directories in XDG_DATA_DIRS and PATH, also
# when display managers did not add them (stubs instead of the session).
add_test(NAME sde-session-env COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_session_env.py
  ${PROJECT_SOURCE_DIR})

# 0.2 Alpha 3 live badges of the Start menu and the taskbar: LauncherEntry
# properties, matching notifications to applications, counts, colours (no
# display or bus needed); and an application announcing a badge for the UI
# tests (a QA-only fixture, not installed).
add_executable(sde-badges-tests test_badges.cpp ../src/panel/badge_model.cpp)
target_link_libraries(sde-badges-tests PRIVATE sde-common)
add_test(NAME sde-badges COMMAND sde-badges-tests)
add_executable(sde-launcher-entry-fixture launcher_entry_fixture.cpp)
target_link_libraries(sde-launcher-entry-fixture PRIVATE sde-common)

# 0.2.0 Alpha 3.1 Force Close on Xvfb: pings, marks, the dialog (Wait,
# recovery, Force Close), programs that never open, refused targets, close
# attempts in a patched SDE WM and crashes through sde-session.
if(SDE_XVFB AND SDE_DBUS_RUN_SESSION)
  set(SDE_FORCE_CLOSE_WM "")
  if(SDE_BUILD_COMPONENTS)
    set(SDE_FORCE_CLOSE_WM --wm "${CMAKE_BINARY_DIR}/session-stage${CMAKE_INSTALL_PREFIX}/bin/sde-wm")
  endif()
  add_test(NAME sde-force-close-ui COMMAND ${SDE_DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_force_close_ui.py ${CMAKE_BINARY_DIR} ${SDE_FORCE_CLOSE_WM})
  set_tests_properties(sde-force-close-ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
endif()
