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
# QA-only D-Bus/XEmbed fixtures: deliberately not installed in the packages.
add_executable(sde-panel-service-fixture panel_service_fixture.cpp)
target_link_libraries(sde-panel-service-fixture PRIVATE sde-common)
