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

if(SDE_BUILD_COMPONENTS)
  add_test(NAME sde-calculator COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_calculator.py $<TARGET_FILE:calculator>)
endif()

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

add_executable(sde-panel-controls-tests test_panel_controls.cpp ../src/panel/controls.cpp)
target_link_libraries(sde-panel-controls-tests PRIVATE sde-common)
add_test(NAME sde-panel-controls COMMAND sde-panel-controls-tests)
add_executable(sde-menu-model-tests test_menu_model.cpp ../src/panel/menu_model.cpp)
target_link_libraries(sde-menu-model-tests PRIVATE sde-common)
add_test(NAME sde-menu-model COMMAND sde-menu-model-tests)
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

# SDE System Monitor /proc and /sys parsers.
add_executable(sde-sysmon-tests test_sysmon.cpp)
target_link_libraries(sde-sysmon-tests PRIVATE sde-sysmon-core)
add_test(NAME sde-sysmon-parsers COMMAND sde-sysmon-tests)
