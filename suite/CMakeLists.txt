# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
