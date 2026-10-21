# SPDX-License-Identifier: GPL-3.0-or-later

add_library(sde-common STATIC
  display/backend.cpp
  display/profile.cpp
  common/config.cpp
  common/crash.cpp
  common/integration.cpp
  common/dbus.cpp
  common/effects.cpp
  common/desktop_entry.cpp
  common/glyphs.cpp
  common/i18n.cpp
  common/icons.cpp
  common/paint.cpp
  common/process.cpp
  common/spawn.cpp
  common/suite.cpp
  common/theme.cpp
  common/tray_icons.cpp
  common/util.cpp
  common/widgets.cpp
  common/wm.cpp
  common/x11.cpp
)
target_include_directories(sde-common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${CMAKE_BINARY_DIR}/generated)
target_compile_options(sde-common PUBLIC ${FLTK_CXXFLAGS})
target_link_libraries(sde-common PUBLIC
  ${FLTK_LDFLAGS} PkgConfig::CAIRO PkgConfig::RSVG PkgConfig::DBUS ${X11_LIBRARIES}
  sde-rust-config PkgConfig::XRANDR PkgConfig::XINPUT)
add_dependencies(sde-common sde-rust-build)

# The session manager draws nothing itself, but shares the configuration,
# theme and window manager helpers with the other programs.
add_executable(sde-session session/main.cpp)
target_link_libraries(sde-session PRIVATE sde-common)

add_executable(sde-panel
  panel/main.cpp
  panel/panel.cpp
  panel/menu.cpp
  panel/menu_model.cpp
  panel/popup.cpp
  panel/quick.cpp
  panel/controls.cpp
  panel/calendar.cpp
  panel/sni.cpp
  panel/tray.cpp)
target_link_libraries(sde-panel PRIVATE sde-common)

add_executable(sde-desktop desktop/main.cpp)
target_link_libraries(sde-desktop PRIVATE sde-common)
# Function names in crash backtraces.
set_target_properties(sde-panel sde-desktop PROPERTIES ENABLE_EXPORTS ON)

add_executable(sde-settings
  settings/main.cpp
  settings/ui.cpp
  settings/display.cpp
  settings/input.cpp
  settings/system.cpp)
target_link_libraries(sde-settings PRIVATE sde-common)

add_executable(sde-run run/main.cpp)
target_link_libraries(sde-run PRIVATE sde-common)

add_executable(sde-logout logout/main.cpp)
target_link_libraries(sde-logout PRIVATE sde-common)

add_executable(sde-notifyd notifyd/main.cpp)
target_link_libraries(sde-notifyd PRIVATE sde-common)

add_executable(sde-update update/main.cpp update/app.cpp update/model.cpp update/ui.cpp)
target_link_libraries(sde-update PRIVATE sde-common)

# Written from scratch by the DebION Dev-Team: the XSETTINGS manager and the
# polkit authentication agent of SDE.
add_library(sde-xsettings-model STATIC xsettingsd/xsettings.cpp)
target_include_directories(sde-xsettings-model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/xsettingsd)
add_executable(sde-xsettingsd xsettingsd/main.cpp)
target_include_directories(sde-xsettingsd PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(sde-xsettingsd PRIVATE sde-xsettings-model ${X11_LIBRARIES})

pkg_check_modules(POLKIT_AGENT REQUIRED IMPORTED_TARGET polkit-agent-1)
add_executable(sde-polkit-agent polkit-agent/main.cpp)
target_link_libraries(sde-polkit-agent PRIVATE sde-common PkgConfig::POLKIT_AGENT)
install(TARGETS sde-polkit-agent RUNTIME DESTINATION libexec/sde)  # SDE_LIBEXECDIR/sde


# Gallery: an original SDE photo library. The model and thumbnail cache are a
# separate library so that the unit tests can use them without a display.
add_library(sde-gallery-core STATIC gallery/library.cpp gallery/thumbnails.cpp)
target_include_directories(sde-gallery-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gallery)
target_link_libraries(sde-gallery-core PUBLIC sde-common)
pkg_check_modules(GDKPIXBUF IMPORTED_TARGET gdk-pixbuf-2.0)
if(GDKPIXBUF_FOUND)
  # Optional decoders (WebP, TIFF, ...) through the installed pixbuf loaders.
  target_compile_definitions(sde-gallery-core PRIVATE SDE_GALLERY_PIXBUF=1)
  target_link_libraries(sde-gallery-core PUBLIC PkgConfig::GDKPIXBUF)
endif()
find_package(Threads REQUIRED)
target_link_libraries(sde-gallery-core PUBLIC Threads::Threads)
add_executable(sde-gallery
  gallery/main.cpp
  gallery/window.cpp
  gallery/sidebar.cpp
  gallery/grid.cpp
  gallery/viewer.cpp
  gallery/icons.cpp)
target_link_libraries(sde-gallery PRIVATE sde-gallery-core)

# Xanara: the SDE document viewer and PDF editor, written from scratch by
# the DebION Dev-Team on poppler-glib. The document model, renderer and tile
# cache are a separate library so the unit tests run without a display.
pkg_check_modules(POPPLER_GLIB REQUIRED IMPORTED_TARGET poppler-glib)
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${POPPLER_GLIB_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${POPPLER_GLIB_LINK_LIBRARIES})
# Annotation APIs that only newer poppler releases have; older ones get an
# image stamp that looks the same.
check_cxx_source_compiles([=[
#include <poppler.h>
int main() {
  PopplerRectangle r{0, 0, 10, 10};
  PopplerAnnot *a = poppler_annot_free_text_new(nullptr, &r);
  PopplerFontDescription *fd = poppler_font_description_new("Sans");
  fd->size_pt = 12;
  poppler_annot_free_text_set_font_desc(POPPLER_ANNOT_FREE_TEXT(a), fd);
  poppler_font_description_free(fd);
  PopplerColor c{0, 0, 0};
  poppler_annot_free_text_set_font_color(POPPLER_ANNOT_FREE_TEXT(a), &c);
  return 0;
}]=] XANARA_HAVE_FREE_TEXT)
check_cxx_source_compiles([=[
#include <poppler.h>
int main() {
  PopplerRectangle r{0, 0, 10, 10};
  PopplerAnnot *a = poppler_annot_ink_new(nullptr, &r);
  PopplerPoint pts[2] = {{0, 0}, {1, 1}};
  PopplerPath *p = poppler_path_new_from_array(pts, 2);
  poppler_annot_ink_set_ink_list(POPPLER_ANNOT_INK(a), &p, 1);
  poppler_path_free(p);
  return 0;
}]=] XANARA_HAVE_INK)
check_cxx_source_compiles([=[
#include <poppler.h>
int main() { poppler_annot_set_border_width(nullptr, 2.0); return 0; }
]=] XANARA_HAVE_BORDER_WIDTH)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
add_library(sde-xanara-core STATIC xanara/document.cpp xanara/cache.cpp)
target_include_directories(sde-xanara-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/xanara)
target_link_libraries(sde-xanara-core PUBLIC sde-common PkgConfig::POPPLER_GLIB Threads::Threads)
foreach(feature XANARA_HAVE_FREE_TEXT XANARA_HAVE_INK XANARA_HAVE_BORDER_WIDTH)
  if(${feature})
    target_compile_definitions(sde-xanara-core PUBLIC ${feature}=1)
  endif()
endforeach()
add_executable(xanara
  xanara/main.cpp
  xanara/window.cpp
  xanara/view.cpp
  xanara/sidebar.cpp
  xanara/icons.cpp)
target_link_libraries(xanara PRIVATE sde-xanara-core)
install(TARGETS xanara RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS sde-session sde-panel sde-desktop sde-settings sde-run sde-logout sde-notifyd sde-update
        sde-xsettingsd sde-gallery
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(SDE_BUILD_COMPONENTS)
  add_subdirectory(calculator)
endif()

add_executable(sde-display-settings display/main.cpp settings/display.cpp settings/ui.cpp)
target_include_directories(sde-display-settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/settings)
target_link_libraries(sde-display-settings PRIVATE sde-common)
install(TARGETS sde-display-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES display/profile.LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde/upstream/DisplayProfileManager RENAME copyright)
if(SDE_BUILD_COMPONENTS)
  install(FILES "${SDE_COMPONENT_SOURCES}/sde-display-settings/LICENSE"
                "${SDE_COMPONENT_SOURCES}/sde-display-settings/copyright"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sde/upstream/DisplayProfileManager)
endif()
