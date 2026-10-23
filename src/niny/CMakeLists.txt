# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
#
# Niny Text Editor. The text model (codec, highlighter, search, undo,
# settings, text operations) is a separate library so the unit tests can use
# it without a display.
string(JSON NINY_VERSION ERROR_VARIABLE _niny_error GET "${SDE_APP_VERSIONS}" niny)
if(_niny_error OR NOT NINY_VERSION)
  set(NINY_VERSION "${SDE_VERSION}")
endif()

add_library(niny-core STATIC
  codec.cpp
  syntax.cpp
  search.cpp
  undo.cpp
  settings.cpp
  textops.cpp
  markdown.cpp)
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
  instance.cpp
  popup.cpp
  menus.cpp
  mdview.cpp
  spell.cpp
  settings_page.cpp)
target_compile_definitions(niny PRIVATE NINY_VERSION="${NINY_VERSION}")
target_link_libraries(niny PRIVATE niny-core)
install(TARGETS niny RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
