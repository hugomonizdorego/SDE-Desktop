# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(sde-tests test_main.cpp)
target_link_libraries(sde-tests PRIVATE sde-common)
add_test(NAME sde-tests COMMAND sde-tests)
add_test(NAME sde-rust-tests
  COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${SDE_CARGO_TARGET_DIR}"
          ${CARGO_EXECUTABLE} test --manifest-path "${PROJECT_SOURCE_DIR}/Cargo.toml"
          --workspace --locked --offline)

# Test helper: a StatusNotifierItem with a menu (not installed).
add_executable(sde-sni-demo sni_demo.cpp)
target_link_libraries(sde-sni-demo PRIVATE sde-common)
