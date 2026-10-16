# SPDX-License-Identifier: GPL-3.0-or-later
# Translations. Source strings are US English. Add a language by listing its
# code in LINGUAS and creating <code>.po from sde.pot (see README.md).

find_program(XGETTEXT_EXECUTABLE xgettext)
find_program(MSGFMT_EXECUTABLE msgfmt)
find_program(MSGMERGE_EXECUTABLE msgmerge)

if(XGETTEXT_EXECUTABLE)
  file(GLOB_RECURSE SDE_I18N_SOURCES RELATIVE ${CMAKE_SOURCE_DIR}
       ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)
  list(SORT SDE_I18N_SOURCES)
  # "make pot" refreshes po/sde.pot from the sources.
  add_custom_target(pot
    COMMAND ${XGETTEXT_EXECUTABLE} --from-code=UTF-8 --language=C++ --keyword=_ --keyword=N_
            --package-name=sde --package-version=${SDE_VERSION}
            --msgid-bugs-address=https://github.com/hugomonizdorego/SDE-Desktop/issues
            --add-comments=TRANSLATORS: --sort-by-file
            -o ${CMAKE_CURRENT_SOURCE_DIR}/sde.pot ${SDE_I18N_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Updating po/sde.pot")
endif()

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/LINGUAS SDE_LINGUAS REGEX "^[^#]")
if(MSGFMT_EXECUTABLE)
  set(SDE_MO_FILES)
  foreach(lang ${SDE_LINGUAS})
    string(STRIP "${lang}" lang)
    if(lang STREQUAL "")
      continue()
    endif()
    set(mo ${CMAKE_CURRENT_BINARY_DIR}/${lang}.mo)
    add_custom_command(OUTPUT ${mo}
      COMMAND ${MSGFMT_EXECUTABLE} --check -o ${mo} ${CMAKE_CURRENT_SOURCE_DIR}/${lang}.po
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${lang}.po)
    list(APPEND SDE_MO_FILES ${mo})
    install(FILES ${mo} DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${lang}/LC_MESSAGES RENAME sde.mo)
  endforeach()
  add_custom_target(translations ALL DEPENDS ${SDE_MO_FILES})
endif()
