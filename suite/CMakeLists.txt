# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Python3 COMPONENTS Interpreter REQUIRED)
option(SDE_BUILD_NATIVE_SUITE "Compile the native SDE application forks" ON)
if(SDE_BUILD_NATIVE_SUITE)
  set(SDE_SUITE_SOURCES "${PROJECT_SOURCE_DIR}/vendor/suite" CACHE PATH "Prepared native fork source directory")
  if(NOT EXISTS "${SDE_SUITE_SOURCES}/.sde-fork-lock")
    message(FATAL_ERROR "Prepare fork sources first: python3 tools/prepare-native-suite.py")
  endif()
  set(SDE_SUITE_STAGE "${CMAKE_BINARY_DIR}/native-stage")
  add_custom_target(sde-native-suite ALL
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/build-native-suite.py"
            --sources "${SDE_SUITE_SOURCES}"
            --build "${CMAKE_BINARY_DIR}/native-build" --stage "${SDE_SUITE_STAGE}"
            --prefix "${CMAKE_INSTALL_PREFIX}" --libdir "${CMAKE_INSTALL_FULL_LIBDIR}"
            --sysconfdir "${CMAKE_INSTALL_FULL_SYSCONFDIR}"
    USES_TERMINAL VERBATIM)
  install(DIRECTORY "${SDE_SUITE_STAGE}${CMAKE_INSTALL_PREFIX}/"
          DESTINATION "${CMAKE_INSTALL_PREFIX}" USE_SOURCE_PERMISSIONS
          PATTERN "include" EXCLUDE PATTERN "pkgconfig" EXCLUDE
          PATTERN "*.la" EXCLUDE PATTERN "*.a" EXCLUDE)
  install(DIRECTORY "${SDE_SUITE_STAGE}${CMAKE_INSTALL_FULL_SYSCONFDIR}/"
          DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}" USE_SOURCE_PERMISSIONS)
endif()
