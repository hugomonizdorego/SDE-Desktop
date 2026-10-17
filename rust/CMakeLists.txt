# SPDX-License-Identifier: GPL-3.0-or-later
find_program(CARGO_EXECUTABLE cargo REQUIRED)
find_package(Threads REQUIRED)

# No registry dependencies: release builds and tests work without network.
set(SDE_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")
set(SDE_RUST_CONFIG_LIB "${SDE_CARGO_TARGET_DIR}/release/libsde_config_ffi.a")
set(SDE_AUTOSTART_BIN "${SDE_CARGO_TARGET_DIR}/release/sde-autostart")
set(SDE_SUITE_BIN "${SDE_CARGO_TARGET_DIR}/release/sde-suite")
file(GLOB_RECURSE SDE_RUST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.rs" "${CMAKE_CURRENT_SOURCE_DIR}/*/Cargo.toml")
add_custom_command(
  OUTPUT "${SDE_RUST_CONFIG_LIB}" "${SDE_AUTOSTART_BIN}" "${SDE_SUITE_BIN}"
  COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${SDE_CARGO_TARGET_DIR}"
          ${CARGO_EXECUTABLE} build --manifest-path "${PROJECT_SOURCE_DIR}/Cargo.toml"
          --workspace --release --locked --offline
  # Cargo may rebuild only the binary when VERSION changes. Keep both CMake
  # outputs newer than the inputs even when the archive's bytes are unchanged.
  COMMAND ${CMAKE_COMMAND} -E touch "${SDE_RUST_CONFIG_LIB}" "${SDE_AUTOSTART_BIN}" "${SDE_SUITE_BIN}"
  DEPENDS ${SDE_RUST_SOURCES} "${PROJECT_SOURCE_DIR}/Cargo.toml"
          "${PROJECT_SOURCE_DIR}/Cargo.lock" "${PROJECT_SOURCE_DIR}/data/sde.conf"
          "${PROJECT_SOURCE_DIR}/VERSION"
          "${PROJECT_SOURCE_DIR}/data/suite.tsv"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  COMMENT "Building SDE Rust core and autostart (offline)"
  VERBATIM)
add_custom_target(sde-rust-build ALL DEPENDS "${SDE_RUST_CONFIG_LIB}" "${SDE_AUTOSTART_BIN}" "${SDE_SUITE_BIN}")
add_library(sde-rust-config STATIC IMPORTED GLOBAL)
set_target_properties(sde-rust-config PROPERTIES IMPORTED_LOCATION "${SDE_RUST_CONFIG_LIB}")
add_dependencies(sde-rust-config sde-rust-build)
target_link_libraries(sde-rust-config INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m rt util)
install(PROGRAMS "${SDE_AUTOSTART_BIN}" DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "${SDE_SUITE_BIN}" DESTINATION ${CMAKE_INSTALL_BINDIR})

# Shared with tests/CMakeLists.txt (sibling directory).
set(SDE_CARGO_TARGET_DIR "${SDE_CARGO_TARGET_DIR}" PARENT_SCOPE)
set(CARGO_EXECUTABLE "${CARGO_EXECUTABLE}" PARENT_SCOPE)
