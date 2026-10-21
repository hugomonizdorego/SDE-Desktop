# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DebION Dev-Team
#
# Niny Text Editor. The text model (codec, highlighter, search, undo,
# settings, text operations) is a separate library so the unit tests can use
# it without a display.
set(NINY_VERSION "1.0.0")

add_library(niny-core STATIC
  codec.cpp
  syntax.cpp
  search.cpp
  undo.cpp
  settings.cpp
  textops.cpp)
target_include_directories(niny-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(niny-core PUBLIC sde-common)

add_executable(niny
  main.cpp
  app.cpp
  actions.cpp
  document.cpp
  editor.cpp
  chrome.cpp
  sidebar.cpp
  findbar.cpp
  palette.cpp
  dialogs.cpp
  icons.cpp
  look.cpp
  instance.cpp)
target_compile_definitions(niny PRIVATE NINY_VERSION="${NINY_VERSION}")
target_link_libraries(niny PRIVATE niny-core)
install(TARGETS niny RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
