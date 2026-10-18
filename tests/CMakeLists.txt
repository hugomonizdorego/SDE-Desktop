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
