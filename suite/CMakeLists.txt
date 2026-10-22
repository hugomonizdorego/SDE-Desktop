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
  # Since alpha16 (no power-manager/screensaver forks) the suite may stage
  # nothing under /etc.
  install(CODE "if(EXISTS \"${SDE_SUITE_STAGE}${CMAKE_INSTALL_FULL_SYSCONFDIR}\")
    file(INSTALL \"${SDE_SUITE_STAGE}${CMAKE_INSTALL_FULL_SYSCONFDIR}/\"
         DESTINATION \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_SYSCONFDIR}\" USE_SOURCE_PERMISSIONS)
  endif()")
endif()

option(SDE_BUILD_COMPONENTS "Build the SDE session components (SDE WM, SDE compositor)" ON)
set(SDE_COMPONENT_SOURCES "${PROJECT_SOURCE_DIR}/vendor/components" CACHE PATH "Prepared WM/compositor source trees")
if(SDE_BUILD_COMPONENTS)
  if(NOT EXISTS "${SDE_COMPONENT_SOURCES}/.sde-component-lock")
    message(FATAL_ERROR "Run python3 tools/prepare-components.py")
  endif()
  set(SDE_SESSION_STAGE "${CMAKE_BINARY_DIR}/session-stage")
  add_custom_target(sde-session-components ALL
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/build-session-components.py"
            --sources "${SDE_COMPONENT_SOURCES}" --build "${CMAKE_BINARY_DIR}/session-build"
            --stage "${SDE_SESSION_STAGE}" --prefix "${CMAKE_INSTALL_PREFIX}"
    USES_TERMINAL VERBATIM)
  install(DIRECTORY "${SDE_SESSION_STAGE}${CMAKE_INSTALL_PREFIX}/"
          DESTINATION "${CMAKE_INSTALL_PREFIX}" USE_SOURCE_PERMISSIONS)
endif()
