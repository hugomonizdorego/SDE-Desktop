# SPDX-License-Identifier: GPL-3.0-or-later

add_library(sde-common STATIC
  display/backend.cpp
  display/profile.cpp
  common/config.cpp
  common/crash.cpp
  common/integration.cpp
  common/about.cpp
  common/avatar.cpp
  common/avatar_view.cpp
  common/legal.cpp
  common/menu.cpp
  common/dbus.cpp
  common/effects.cpp
  common/desktop_entry.cpp
  common/glyphs.cpp
  common/i18n.cpp
  common/icons.cpp
  common/paint.cpp
  common/power.cpp
  common/process.cpp
  common/spawn.cpp
  common/suite.cpp
  common/theme.cpp
  common/touchpad.cpp
  common/transfer_dialog.cpp
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
# The user's picture (common/avatar.cpp) decodes JPEG photos with libjpeg,
# which SDE-FLTK needs anyway.
find_package(JPEG REQUIRED)
target_link_libraries(sde-common PUBLIC JPEG::JPEG)
if(TARGET sde-fltk-static)
  add_dependencies(sde-common sde-fltk-update)
  target_link_libraries(sde-common PUBLIC sde-fltk-static)
endif()

# The session manager draws nothing itself, but shares the configuration,
# theme and window manager helpers with the other programs.
add_executable(sde-session session/main.cpp)
target_link_libraries(sde-session PRIVATE sde-common)

add_executable(sde-panel
  panel/main.cpp
  panel/panel.cpp
  panel/app_catalog.cpp
  panel/menu.cpp
  panel/menu_model.cpp
  panel/menu_perf.cpp
  panel/search.cpp
  panel/popup.cpp
  panel/quick.cpp
  panel/controls.cpp
  panel/calendar.cpp
  panel/sni.cpp
  panel/tray.cpp
  panel/agenda.cpp
  panel/alarms.cpp
  panel/alarm_popup.cpp
  panel/panel_widgets.cpp
  panel/weather.cpp
  panel/timezone.cpp
  panel/nightlight.cpp
  panel/clockface.cpp
  panel/art.cpp
  panel/status_model.cpp
  panel/status_flyout.cpp
  panel/drives.cpp
  panel/touchpad_monitor.cpp
  panel/preview.cpp
  panel/panel_menus.cpp
  panel/notify_center.cpp
  # The Start menu's account button and popup, the Action Center's user.
  panel/account.cpp)
target_link_libraries(sde-panel PRIVATE sde-common)
if(XCOMPOSITE_FOUND)
  target_link_libraries(sde-panel PRIVATE PkgConfig::XCOMPOSITE)
  target_compile_definitions(sde-panel PRIVATE SDE_HAVE_XCOMPOSITE=1)
endif()

# Desktop drag and drop model and file operations (unit tested).
add_library(sde-desktop-model STATIC desktop/desktop_model.cpp desktop/file_ops.cpp)
target_include_directories(sde-desktop-model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/desktop)
target_link_libraries(sde-desktop-model PUBLIC sde-common)
add_executable(sde-desktop desktop/main.cpp desktop/xdnd.cpp)
# WebP and other pixbuf wallpapers are decoded with the Gallery core.
target_link_libraries(sde-desktop PRIVATE sde-desktop-model sde-gallery-core sde-common)
# Function names in crash backtraces.
set_target_properties(sde-panel sde-desktop PROPERTIES ENABLE_EXPORTS ON)

add_executable(sde-settings
  settings/main.cpp
  settings/ui.cpp
  settings/display.cpp
  settings/input.cpp
  settings/system.cpp
  settings/power.cpp
  settings/touchpad.cpp
  settings/network.cpp
  settings/clock.cpp
  settings/default_apps_page.cpp
  settings/about_page.cpp
  # The overview hub and Users & Avatar (the Control Center).
  settings/hub.cpp
  settings/users.cpp
  # Date and time: the panel clock's "Sync time zone" (and its JSON reader).
  panel/timezone.cpp
  panel/weather.cpp
  # Clock & Weather page: the clock style previews (alpha52).
  panel/clockface.cpp
  panel/nightlight.cpp)
# Display-free helpers (graphics card detection, wallpaper scan, pactl/
# nmcli/bluetoothctl parsers), unit
# tested; thumbnails come from the Gallery core (decoding threads and the
# freedesktop.org thumbnail cache).
add_library(sde-settings-model STATIC settings/gpu.cpp settings/wallpapers.cpp
  settings/devices.cpp settings/proxy.cpp settings/build_history.cpp settings/default_apps.cpp
  settings/accounts.cpp)
target_include_directories(sde-settings-model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/settings)
# User accounts (settings/accounts.cpp): passwords are hashed with libxcrypt
# (crypt_gensalt_rn, yescrypt) before AccountsService sees them.
pkg_check_modules(LIBCRYPT REQUIRED IMPORTED_TARGET libcrypt)
target_link_libraries(sde-settings-model PUBLIC sde-gallery-core sde-common PkgConfig::LIBCRYPT)
target_link_libraries(sde-settings PRIVATE sde-settings-model sde-common)

add_executable(sde-run run/main.cpp)
target_link_libraries(sde-run PRIVATE sde-common)

add_executable(sde-logout logout/main.cpp)
target_link_libraries(sde-logout PRIVATE sde-common)

add_executable(sde-notifyd notifyd/main.cpp)
target_link_libraries(sde-notifyd PRIVATE sde-common)

add_executable(sde-update update/main.cpp update/app.cpp update/model.cpp update/ui.cpp)
target_link_libraries(sde-update PRIVATE sde-common)

# Written from scratch by the Selviana DE Project: the XSETTINGS manager and the
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


# Gallery: an original SDE photo library. The model, metadata reader,
# storage media, albums and thumbnail cache are a separate library so that
# the unit tests (and Photo, and Settings for wallpaper thumbnails) can use
# them without a display.
add_library(sde-gallery-core STATIC
  gallery/library.cpp
  gallery/thumbnails.cpp
  gallery/exif.cpp
  gallery/media.cpp
  gallery/album.cpp
  gallery/collections.cpp
  gallery/plugins.cpp)
target_include_directories(sde-gallery-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gallery)
target_link_libraries(sde-gallery-core PUBLIC sde-common)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
target_link_libraries(sde-gallery-core PUBLIC PkgConfig::GIO)
pkg_check_modules(GDKPIXBUF IMPORTED_TARGET gdk-pixbuf-2.0)
if(GDKPIXBUF_FOUND)
  # Optional decoders (WebP, TIFF, ...) through the installed pixbuf loaders;
  # Photo also saves and plays animated GIFs with it.
  target_compile_definitions(sde-gallery-core PUBLIC SDE_GALLERY_PIXBUF=1)
  target_link_libraries(sde-gallery-core PUBLIC PkgConfig::GDKPIXBUF)
endif()
# EXIF is read by a built-in parser; libexif (when found) adds what it misses.
option(SDE_WITH_LIBEXIF "Use libexif for photo metadata when available" ON)
if(SDE_WITH_LIBEXIF)
  pkg_check_modules(LIBEXIF IMPORTED_TARGET libexif)
  if(LIBEXIF_FOUND)
    target_compile_definitions(sde-gallery-core PUBLIC SDE_HAVE_LIBEXIF=1)
    target_link_libraries(sde-gallery-core PUBLIC PkgConfig::LIBEXIF)
  endif()
endif()
find_package(Threads REQUIRED)
target_link_libraries(sde-gallery-core PUBLIC Threads::Threads)
# Drawing shared by Gallery and Photo: line icons and the themed menu bar.
add_library(sde-gallery-ui STATIC gallery/icons.cpp gallery/menubar.cpp gallery/link.cpp gallery/plugins_ui.cpp)
target_link_libraries(sde-gallery-ui PUBLIC sde-gallery-core)
add_executable(sde-gallery
  gallery/main.cpp
  gallery/window.cpp
  gallery/sidebar.cpp
  gallery/grid.cpp
  gallery/viewer.cpp
  gallery/organize.cpp)
# Batch rotate/export use Photo's encoder (lossless JPEG rotation, EXIF).
target_link_libraries(sde-gallery PRIVATE sde-gallery-ui sde-photo-core)

# Photo (sde-view): the SDE photo viewer and editor, written from scratch by
# the Selviana DE Project on top of sde-gallery-core.
add_library(sde-photo-core STATIC photo/edit.cpp photo/save.cpp photo/draw.cpp photo/collage.cpp)
target_include_directories(sde-photo-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/photo)
target_link_libraries(sde-photo-core PUBLIC sde-gallery-core)
add_executable(sde-view
  photo/main.cpp
  photo/window.cpp
  photo/canvas.cpp
  photo/panels.cpp
  photo/drawing.cpp
  photo/clipboard.cpp
  photo/collage_window.cpp
  photo/collage_panel.cpp
  photo/tools.cpp)
target_link_libraries(sde-view PRIVATE sde-photo-core sde-gallery-ui)
install(TARGETS sde-view RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Its icon (sde-photo, and sdeview for older launchers) is in data/icons.

# Xanara: the SDE document viewer and PDF editor, written from scratch by
# the Selviana DE Project on poppler-glib. The document model, renderer and tile
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
add_library(sde-xanara-core STATIC xanara/document.cpp xanara/cache.cpp xanara/model.cpp)
target_include_directories(sde-xanara-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/xanara)
target_link_libraries(sde-xanara-core PUBLIC sde-common PkgConfig::POPPLER_GLIB Threads::Threads)
# JPEG export writes with libjpeg (already needed by SDE-FLTK); without it
# Xanara falls back to pdftoppm when that is installed.
find_package(JPEG)
if(JPEG_FOUND)
  target_compile_definitions(sde-xanara-core PRIVATE XANARA_HAVE_JPEG=1)
  target_link_libraries(sde-xanara-core PUBLIC JPEG::JPEG)
endif()
foreach(feature XANARA_HAVE_FREE_TEXT XANARA_HAVE_INK XANARA_HAVE_BORDER_WIDTH)
  if(${feature})
    target_compile_definitions(sde-xanara-core PUBLIC ${feature}=1)
  endif()
endforeach()
add_executable(xanara
  xanara/main.cpp
  xanara/window.cpp
  xanara/chrome.cpp
  xanara/commands.cpp
  xanara/home.cpp
  xanara/props.cpp
  xanara/view.cpp
  xanara/sidebar.cpp
  xanara/icons.cpp)
target_link_libraries(xanara PRIVATE sde-xanara-core)
install(TARGETS xanara RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Taca: the SDE archive manager, written from scratch by the Selviana DE Project.
# The archive engine is a separate library for the unit tests (no display).
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive>=3.6)
add_library(taca-core STATIC
  taca/formats.cpp
  taca/safety.cpp
  taca/engine.cpp
  taca/external.cpp
  taca/model.cpp
  taca/tools.cpp)
target_include_directories(taca-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/taca)
target_link_libraries(taca-core PUBLIC sde-common PkgConfig::LIBARCHIVE Threads::Threads)
add_executable(taca
  taca/main.cpp
  taca/window.cpp
  taca/fileview.cpp
  taca/dialogs.cpp
  taca/icons.cpp
  taca/ui.cpp
  taca/bars.cpp
  taca/wizard.cpp)
target_link_libraries(taca PRIVATE taca-core)
install(TARGETS taca RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Native suite applications report their own version from suite/versions.json.
function(sde_app_version target id)
  string(JSON _version ERROR_VARIABLE _error GET "${SDE_APP_VERSIONS}" ${id})
  if(_error OR NOT _version)
    set(_version "${SDE_VERSION}")
  endif()
  target_compile_definitions(${target} PRIVATE SDE_APP_VERSION="${_version}")
endfunction()
sde_app_version(xanara xanara)
sde_app_version(taca taca)
sde_app_version(sde-view sde-view)

install(TARGETS sde-session sde-panel sde-desktop sde-settings sde-run sde-logout sde-notifyd sde-update
        sde-xsettingsd sde-gallery
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Calculator: native from-scratch SDE application (Selviana DE Project) that
# replaced the GTK4 frontend of the scientific-calculator component. The
# parser, programmer mode, converter and history live in calculator-core,
# which the unit tests link.
add_library(calculator-core STATIC
  calculator/engine.cpp
  calculator/programmer.cpp
  calculator/converter.cpp
  calculator/state.cpp)
target_include_directories(calculator-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/calculator)
target_link_libraries(calculator-core PUBLIC sde-common)
add_executable(calculator
  calculator/main.cpp
  calculator/window.cpp
  calculator/keys.cpp)
target_link_libraries(calculator PRIVATE calculator-core)
sde_app_version(calculator calculator)
install(TARGETS calculator RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# SDE System Monitor: native from-scratch rewrite (Selviana DE Project).
# The /proc and /sys parsers live in a small library shared with the tests.
add_library(sde-sysmon-core STATIC sysmon/procfs.cpp sysmon/sampler.cpp)
target_include_directories(sde-sysmon-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sysmon)
find_package(Threads REQUIRED)
target_link_libraries(sde-sysmon-core PUBLIC Threads::Threads)
add_executable(sde-system-monitor
  sysmon/main.cpp
  sysmon/widgets.cpp
  sysmon/apps.cpp
  sysmon/processes.cpp
  sysmon/performance.cpp
  sysmon/filesystems.cpp
  sysmon/startup.cpp)
string(JSON SDE_SYSMON_VERSION ERROR_VARIABLE SDE_SYSMON_VERSION_ERROR
       GET "${SDE_APP_VERSIONS}" sde-system-monitor)
if(SDE_SYSMON_VERSION_ERROR OR NOT SDE_SYSMON_VERSION)
  set(SDE_SYSMON_VERSION "${SDE_VERSION}")
endif()
target_compile_definitions(sde-system-monitor PRIVATE SDE_SYSMON_VERSION="${SDE_SYSMON_VERSION}")
target_link_libraries(sde-system-monitor PRIVATE sde-sysmon-core sde-common)
install(TARGETS sde-system-monitor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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

# SDE Power and SDE Screen Lock: the power manager and the screen locker,
# written from scratch by the Selviana DE Project (src/power, src/lock). Their
# decisions (idle stages, battery levels, inhibitors, PAM conversation,
# attempt delays) are small libraries shared with the unit tests.
pkg_check_modules(XSS REQUIRED IMPORTED_TARGET xscrnsaver)
pkg_check_modules(XEXT REQUIRED IMPORTED_TARGET xext)
add_library(sde-power-core STATIC power/policy.cpp power/session.cpp)
target_include_directories(sde-power-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/power)
target_link_libraries(sde-power-core PUBLIC sde-common PkgConfig::XSS PkgConfig::XEXT)
add_executable(sde-power power/main.cpp power/osd.cpp)
target_link_libraries(sde-power PRIVATE sde-power-core)
sde_app_version(sde-power sde-power)
install(TARGETS sde-power RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

pkg_check_modules(PAM REQUIRED IMPORTED_TARGET pam)
# Display-free parts of the locker (PAM conversation, attempt delays,
# inhibitors) and the lock screen; the tests link the lock screen with a
# fake authenticator, sde-lock always with PAM.
add_library(sde-lock-core STATIC lock/auth.cpp lock/policy.cpp)
target_include_directories(sde-lock-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lock)
target_link_libraries(sde-lock-core PUBLIC PkgConfig::PAM)
add_library(sde-lock-ui STATIC lock/ui.cpp lock/rescue.cpp)
target_link_libraries(sde-lock-ui PUBLIC sde-lock-core sde-power-core Threads::Threads)
add_executable(sde-lock lock/main.cpp lock/daemon.cpp)
target_link_libraries(sde-lock PRIVATE sde-lock-ui)
sde_app_version(sde-lock sde-lock)
install(TARGETS sde-lock RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# PAM service of the locker (Debian's common-auth/common-account stacks).
install(FILES ${PROJECT_SOURCE_DIR}/data/pam/sde-lock DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/pam.d)
# Compatibility commands of the former SDE Power Manager / SDE Screen Lock
# forks: they open the Control Center's pages or call sde-lock. And
# sde-control-center: the Control Center (sde-settings) under its new name.
install(PROGRAMS ${PROJECT_SOURCE_DIR}/data/compat/sde-power-preferences
                 ${PROJECT_SOURCE_DIR}/data/compat/sde-screensaver-preferences
                 ${PROJECT_SOURCE_DIR}/data/compat/sde-screensaver-command
                 ${PROJECT_SOURCE_DIR}/data/compat/sde-control-center
        DESTINATION ${CMAKE_INSTALL_BINDIR})

# Niny Text Editor: a native SDE editor written from scratch (src/niny).
add_subdirectory(niny)

# SDE Welcome, Driver Manager and System Reports (docs/MINT-CONCEPTS.md).
add_subdirectory(welcome)

# Merkadu, the software center (docs/MERKADU.md).
add_subdirectory(merkadu)
