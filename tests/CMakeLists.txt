# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(sde-tests test_main.cpp)
target_link_libraries(sde-tests PRIVATE sde-common)
add_test(NAME sde-tests COMMAND sde-tests)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
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

if(SDE_BUILD_NATIVE_SUITE AND SDE_XVFB)
  add_test(NAME sde-gallery-source COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_gallery_source.py ${SDE_GALLERY_SOURCES})
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  add_executable(sde-native-about-tests test_native_about.cpp)
  target_link_libraries(sde-native-about-tests PRIVATE PkgConfig::GTK3)
  # assert() remains active in the distribution's RelWithDebInfo build.
  target_compile_options(sde-native-about-tests PRIVATE -UNDEBUG)
  add_test(NAME sde-native-about COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_native_about.py $<TARGET_FILE:sde-native-about-tests>)
endif()
