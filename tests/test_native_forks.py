# SPDX-License-Identifier: GPL-3.0-or-later
import json
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("forks", Path(__file__).resolve().parents[1] / "tools/prepare-native-suite.py")
forks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(forks)


class BrandingTest(unittest.TestCase):
    def test_independent_build_versions_and_desktop_placement(self):
        root = Path(__file__).resolve().parents[1]
        versions = json.loads((root / 'suite/versions.json').read_text())
        self.assertEqual(versions['liria'], (root/'VERSION').read_text().strip())
        rows = [r.split('\t') for r in (root/'data/suite.tsv').read_text().splitlines() if r and not r.startswith('#')]
        self.assertEqual(len(rows), 11)
        for row in rows:
            self.assertEqual(row[9], versions[row[0]])
            self.assertIn('alpha', row[9])
            desktop = 'org.debion.Calculator' if row[0] == 'calculator' else row[0]
            self.assertTrue((root/'data/applications'/ (desktop+'.desktop')).is_file())
        for record in json.loads((root/'suite/forks.lock.json').read_text())['forks']:
            self.assertEqual(record['build_version'], versions[record['id']])

    def test_build_version_keeps_os_release_fields_and_character_quotes(self):
        source = r'''if (s.at(start) == '\\"') { auto label = "VERSION="; }
const char *version = VERSION;
const char *other = "# VERSION=";
// PACKAGE_VERSION remains in this historical comment
const char *build = PACKAGE_VERSION;
#undef VERSION
'''
        expected = source.replace('= VERSION;', '= "1.0.0~alpha1";').replace('= PACKAGE_VERSION;', '= "1.0.0~alpha1";')
        self.assertEqual(forks.stamp_version(source, '1.0.0~alpha1'), expected)

    def test_native_identifiers_and_includes_change(self):
        self.assertEqual(forks.brand('#include "caja-window.h"'), '#include "liria-window.h"')
        self.assertEqual(forks.brand('#define EOM_WINDOW "org.mate.eom"'), '#define SDEVIEW_WINDOW "org.debion.sdeview"')
        self.assertEqual(forks.brand('pluma_window_new(CajaFile *file)'), 'niny_window_new(LiriaFile *file)')

    def test_header_guards_and_copyright_identifiers_stay_consistent(self):
        source = '#ifndef CAJA_TYPES_H\n#define CAJA_TYPES_H\n#ifdef CAJA_FEATURE\n#define CAJA_COPYRIGHT "Copyright (C) Caja authors"\n#endif\n#endif\n'
        expected = source.replace('CAJA_', 'LIRIA_')
        self.assertEqual(forks.brand(source), expected)

    def test_generated_enum_templates_and_nested_comments(self):
        self.assertEqual(forks.brand('#define PLUMA_TYPE_@ENUMSHORT@ (@enum_name@_get_type())'),
                         '#define NINY_TYPE_@ENUMSHORT@ (@enum_name@_get_type())')
        source = '#include "caja.h"\n/* background */\nchar *c = "Copyright Caja authors";\n# source: https://github.com/mate-desktop/caja\n'
        self.assertEqual(forks.brand(source), source.replace('"caja.h"', '"liria.h"'))

    def test_mime_wildcards_in_strings_do_not_hide_native_code(self):
        source = 'if (check("image/*")) eom_load(EomImage *image);\n/* unchanged EOM comment */\n'
        expected = source.replace('eom_load', 'sdeview_load').replace('EomImage', 'SdeviewImage').replace('EOM comment', 'SDEVIEW comment')
        self.assertEqual(forks.brand(source), expected)

    def test_gtk_doc_ownership_and_type_annotations_follow_c_api(self):
        source = '/**\n * pluma_app_get_documents:\n * Returns: (element-type PlumaDocument) (transfer container): open documents\n */\nGList *pluma_app_get_documents(PlumaApp *app);\n'
        self.assertEqual(forks.brand(source), source.replace('pluma', 'niny').replace('Pluma', 'Niny'))

    def test_gtk_geometry_api_is_never_rebranded(self):
        self.assertEqual(forks.brand('gtk_window_set_geometry_hints(eom_window, geometry);'),
                         'gtk_window_set_geometry_hints(sdeview_window, geometry);')
        self.assertEqual(forks.brand('-DEOM_DATA_DIR=path'), '-DSDEVIEW_DATA_DIR=path')

    def test_copyright_licenses_and_source_links_are_preserved(self):
        source = '/* Copyright (C) 2012 Pluma MATE authors. GPLv2+ */\nconst char *url = "https://github.com/mate-desktop/pluma";\n'
        self.assertEqual(forks.brand(source), source)

    def test_app_settings_contracts_are_independent(self):
        self.assertEqual(forks.brand('"org.mate.session" "org.mate.SessionManager"'),
                         '"org.debion.session" "org.mate.SessionManager"')
        self.assertEqual(forks.brand('"org.mate.Engrampa" "org.mate.SystemMonitor" "org.mate.media-handling"'),
                         '"org.debion.Taca" "org.debion.SystemMonitor" "org.debion.media-handling"')


class LiriaAlpha15Test(unittest.TestCase):
    """Liria copy engine, safe removal, clipboard and recent files."""

    def setUp(self):
        import sys
        self.root = Path(__file__).resolve().parents[1]
        sys.path.insert(0, str(self.root / 'suite'))
        import redesign
        self.redesign = redesign

    def test_new_recipe_anchors_must_be_unique(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'x.c'
            path.write_text('a;\na;\n')
            with self.assertRaises(ValueError):
                self.redesign.edit_once(path, 'a;', 'b;')
            path.write_text('a;\n')
            self.redesign.edit_once(path, 'a;', 'b;')
            self.assertEqual(path.read_text(), 'b;\n')

    def test_safe_copy_overlay_bounds_dirty_data_and_syncs(self):
        source = (self.root / 'suite/overlays/liria/libliria-private/liria-sde-copy.c').read_text()
        for needle in ('sync_file_range', 'syncfs', 'fdatasync', 'SYNC_FILE_RANGE_WAIT_AFTER',
                       'G_IO_ERROR_EXISTS', 'G_IO_ERROR_INVALID_FILENAME', '"/usb"'):
            self.assertIn(needle, source)
        recipe = (self.root / 'suite/redesign.py').read_text()
        for name in ('liria_copy_engine', 'liria_safe_remove', 'liria_clipboard'):
            self.assertIn(name + '(staged)', recipe)

    def test_fat_names_lose_pipe_and_backslash(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            staged = Path(tmp)
            ops = staged / 'libliria-private/liria-file-operations.c'
            ops.parent.mkdir(parents=True)
            ops.write_text('#define FAT_FORBIDDEN_CHARACTERS "/:;*?\\"<>"\n')
            self.redesign.edit_once(ops, '#define FAT_FORBIDDEN_CHARACTERS "/:;*?\\"<>"',
                                    '#define FAT_FORBIDDEN_CHARACTERS "/:;*?\\"<>|\\\\"')
            self.assertEqual(ops.read_text(), '#define FAT_FORBIDDEN_CHARACTERS "/:;*?\\"<>|\\\\"\n')

    def test_home_recent_files_can_be_cleared(self):
        home = (self.root / 'suite/overlays/liria/sde-tools/liria-sde-home.c').read_text()
        for needle in ('gtk_recent_manager_purge_items', 'gtk_recent_manager_remove_item',
                       'Remove from Recent', 'sde-home-clear'):
            self.assertIn(needle, home)
        css = (self.root / 'suite/overlays/liria/data/liria-sde.css').read_text()
        self.assertIn('button.sde-home-clear', css)


class LiriaAlpha16Test(unittest.TestCase):
    """Liria re-fork: branding, header and menus, own style sheet."""

    def setUp(self):
        import sys
        self.root = Path(__file__).resolve().parents[1]
        sys.path.insert(0, str(self.root / 'suite'))
        import redesign
        self.redesign = redesign
        self.overlay = self.root / 'suite/overlays/liria'
        spec = importlib.util.spec_from_file_location('liria_branding', self.root / 'tools/liria-branding-check.py')
        self.check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.check)

    def test_pinned_upstream_release(self):
        record = [r for r in json.loads((self.root / 'suite/forks.lock.json').read_text())['forks'] if r['id'] == 'liria'][0]
        self.assertEqual(record['commit'], '27fdb849de02f551eab46eb78f48065da9d1d1ec')  # caja v1.28.0
        self.assertEqual(record['version'], '1.28.0')

    def test_css_uses_only_standard_named_colours(self):
        import re
        css = (self.overlay / 'data/liria-sde.css').read_text()
        names = set(re.findall(r'@([A-Za-z_]+)', css))
        self.assertTrue(names)
        self.assertLessEqual(names, {'theme_bg_color', 'theme_fg_color', 'theme_base_color', 'theme_text_color',
                                     'theme_selected_bg_color', 'theme_selected_fg_color'})
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        self.assertIn('GTK_STYLE_PROVIDER_PRIORITY_APPLICATION', ui)
        self.assertIn('/org/debion/liria/liria-sde.css', ui)

    def test_about_names_developer_and_keeps_legal_notice(self):
        """0.2.0: Liria is made by the Selviana DE Project (SDE and the
        DebION distribution are separate projects); every copyright notice
        of the original work stays, in the License and Credits pages."""
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        about = ui[ui.index('#define SDE_ABOUT_GROUP'):ui.index('/* --------------------------------------------------- application level')]
        for needle in ('#define SDE_DEVELOPER   "Selviana DE Project"', '_("Developer: %s"), SDE_DEVELOPER',
                       '_("Part of Selviana Desktop Environment")',
                       'Copyright \\xC2\\xA9 2026 Selviana DE Project and contributors',
                       'Selviana Desktop Environment, SDE and the SDE logo are trademarks of the Selviana DE Project.',
                       'Copyright \\xC2\\xA9 1999\\xE2\\x80\\x93" "2009 The Nautilus authors',
                       'Copyright \\xC2\\xA9 2011\\xE2\\x80\\x93" "2021 The Caja authors',
                       'Copyright \\xC2\\xA9 2012\\xE2\\x80\\x93" "2021 The MATE developers',
                       'License: GPL-2.0-or-later AND LGPL-2.0-or-later', 'Novell, Inc.', 'Eazel trademarks',
                       'modified version of the Caja file manager 1.28 (MATE), itself derived from GNOME Nautilus',
                       '"Caja 1.28 (MATE) https://github.com/mate-desktop/caja"', '"GNOME Nautilus https://',
                       '_("Based on"), based_on', '_("Original authors")', 'either version 2',
                       'of the License, or (at your option) any later version', 'GPL-2.0-or-later',
                       '/usr/share/doc/sde/upstream/caja/', '"version", "@SDE_BUILD_VERSION@"',
                       '"logo-icon-name", liria_sde_ui_icon_name ()'):
            self.assertIn(needle, about)
        self.assertNotIn('DebION', about)
        # Liria's icon ("liria" in hicolor) with a fallback, also for windows.
        self.assertIn('gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), "liria") ? "liria" : "system-file-manager"',
                      about)
        self.assertIn('gtk_window_set_default_icon_name (liria_sde_ui_icon_name ());', ui)
        recipe = (self.root / 'suite/redesign.py').read_text()
        self.assertIn('"liria") ? "liria" : "system-file-manager"); /* SDE */', recipe)
        self.assertIn('<developer_name>Selviana DE Project</developer_name>', recipe)
        import re
        help_page = re.sub(r'<!--[\s\S]*?-->', '', (self.overlay / 'data/help/index.html').read_text())
        self.assertIn('Developer: Selviana DE Project', help_page)
        extension = (self.overlay / 'sde-tools/libliria-sde-tools.so.liria-extension').read_text()
        self.assertIn('Author=Selviana DE Project', extension)
        self.assertIn('Version=@SDE_BUILD_VERSION@', extension)
        self.assertIn('rglob("*.liria-extension")', recipe)
        for path in self.overlay.rglob('*'):
            if path.is_file():
                self.assertNotIn('DebION', path.read_text(errors='replace'), path)

    def test_about_matches_legal_data(self):
        """The About window agrees with data/legal/components.json: Liria's
        copyright lines and license, SDE's developer, website and trademark."""
        import codecs
        import re
        legal = json.loads((self.root / 'data/legal/components.json').read_text())
        liria = [c for c in legal['components'] if c['id'] == 'liria'][0]
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        about = ui[ui.index('#define SDE_ABOUT_GROUP'):ui.index('/* --------------------------------------------------- application level')]
        literal = r'"(?:[^"\\\n]|\\.)*"'
        text = '\n'.join(codecs.escape_decode(''.join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group())).encode())[0]
                         .decode('utf-8') for m in re.finditer(literal + r'(?:\s*' + literal + ')*', about))
        for line in liria['copyright']:
            self.assertIn(line, text)
        self.assertIn(liria['license'], text)
        self.assertIn(legal['trademarks']['sde'], text)
        self.assertIn(legal['project']['developer'], text)
        self.assertIn('#define SDE_PROJECT_URL     "%s"' % legal['project']['website'], ui)
        self.assertIn('liria', liria['icons'])

    def test_main_menu_and_header(self):
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        for action in ('"New Window"', '"New Tab"', 'Open _Terminal Here', '"Connect to Server"',
                       '"Show Hidden Files"', '_("_Sort")', '_("_View")', '"Preferences"',
                       '_Keyboard Shortcuts', '_About Liria', 'GTK_ICON_SIZE_SMALL_TOOLBAR', '"F10"',
                       'GtkShortcutsWindow', 'go-previous-symbolic', 'open-menu-symbolic'):
            self.assertIn(action, ui)
        recipe = (self.root / 'suite/redesign.py').read_text()
        for name in ('liria_branding', 'liria_header', 'liria_view_switcher', 'liria_history_buttons',
                     'liria_context_menus', 'liria_sidebar', 'liria_translations', 'liria_copy_engine',
                     'liria_safe_remove', 'liria_clipboard'):
            self.assertIn(name + '(staged)', recipe)

    def test_context_menu_send_to_and_grouping(self):
        tools = (self.overlay / 'sde-tools/liria-sde-tools.c').read_text()
        for needle in ('"sde-copy-to"', 'Send To', 'More Actions', 'g_drive_is_removable', 'Compress...',
                       'Extract Here'):
            self.assertIn(needle, tools)
        self.assertNotIn('mate-terminal', tools)
        self.assertNotIn('get_toolbar_items', tools)

    def test_overlays_and_man_page_are_branded(self):
        for path in list(self.overlay.rglob('*')) + [self.root / 'data/man/liria.1',
                                                     self.root / 'data/applications/liria.desktop']:
            if path.is_file():
                self.assertEqual(self.redesign.brand_leaks(path.read_text()), [], path)

    def test_branding_check_rules(self):
        leaks = self.check.leaks_in_text('x', 'Liria is the file manager for the MATE desktop\n'
                                              'Copyright (C) 2011-2021 The Caja authors\n'
                                              'Liria is a modified version of the Caja file manager 1.28.\n'
                                              'Keywords=files;MATE;\nOpen Caja\nhelp:mate-user-guide\n')
        self.assertEqual(len(leaks), 4)

    def test_quick_access_pins_overlay(self):
        pins = (self.overlay / 'src/liria-sde-pins.c').read_text()
        for needle in ('"liria-quick-access"', 'g_get_user_config_dir', 'g_file_set_contents', "'\\t'",
                       'G_USER_DIRECTORY_DOCUMENTS', '"trash:///"', 'liria_sde_pins_reset'):
            self.assertIn(needle, pins)
        self.assertEqual((self.overlay / 'sde-tools/liria-sde-pins.c').read_text().count(
            '#include "../src/liria-sde-pins.c"'), 1)
        home = (self.overlay / 'sde-tools/liria-sde-home.c').read_text()
        for needle in ('liria_sde_pins_load', 'g_file_monitor_file', 'gtk_drag_dest_set', 'gtk_drag_source_set',
                       '"text/uri-list"', 'Unpin from Quick Access', 'Remove from Quick Access', 'Move _Left',
                       'Move _Right', 'Reset to _Defaults', 'Not available', 'sde-card-missing', 'sde-quick-add',
                       'GDK_KEY_Delete', 'GDK_KEY_F2', 'gtk_file_chooser_dialog_new', 'G_FILE_ATTRIBUTE_THUMBNAIL_PATH',
                       'g_content_type_get_description', 'popup-menu'):
            self.assertIn(needle, home)
        # Drops never ask the source to delete what was dropped (Liria moves
        # files itself), and cards dragged out never offer a move.
        import re
        self.assertNotIn('gtk_drag_finish (context, TRUE, TRUE', home)
        self.assertRegex(home, r'gtk_drag_finish \(context, [^,]+, FALSE, time\)')
        for call in re.findall(r'gtk_drag_source_set \([^;]*;', home):
            self.assertNotIn('GDK_ACTION_MOVE', call)
        tools = (self.overlay / 'sde-tools/liria-sde-tools.c').read_text()
        for needle in ('Pin to Quick Access', 'Unpin from Quick Access', 'liria_sde_pins_add', 'liria_sde_pins_remove'):
            self.assertIn(needle, tools)
        make = (self.overlay / 'sde-tools/Makefile.am').read_text()
        self.assertIn('liria-sde-pins.c', make)
        self.assertIn('TESTS = test-liria-sde-pins', make)
        css = (self.overlay / 'data/liria-sde.css').read_text()
        for needle in ('button.sde-card-missing', 'button.sde-quick-add', 'flowbox.sde-quick-access:drop(active)'):
            self.assertIn(needle, css)
        self.assertIn('Pin to Quick Access', (self.overlay / 'data/help/index.html').read_text())

    def test_quick_access_recipe_edits_window_and_sidebar(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            staged = Path(tmp)
            src = staged / 'src'
            src.mkdir()
            (src / 'Makefile.am').write_text('liria_SOURCES = \\\n\tliria-sde-ui.c \\\n')
            (src / 'liria-places-sidebar.c').write_text(
                '#include "liria-sde-ui.h"\n'
                '    GtkWidget *popup_menu_open_in_new_tab_item;\n'
                '    sidebar->popup_menu_remove_item = NULL;\n'
                'static void\nbookmarks_check_popup_sensitivity (LiriaPlacesSidebar *sidebar)\n{\n'
                '    gtk_widget_show (sidebar->popup_menu_open_in_new_tab_item);\n}\n'
                '/* Rename the selected bookmark */\n'
                '                      G_CALLBACK (open_shortcut_in_new_window_cb), sidebar);\n'
                '    gtk_widget_show (item);\n'
                '    gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);\n')
            self.redesign.liria_quick_access(staged)
            self.assertIn('\tliria-sde-pins.c \\\n\tliria-sde-pins.h \\\n\tliria-sde-ui.c', (src / 'Makefile.am').read_text())
            sidebar = (src / 'liria-places-sidebar.c').read_text()
            for needle in ('#include "liria-sde-pins.h"', 'popup_menu_sde_pin_item = NULL', 'sde_pin_shortcut_cb',
                           'liria_sde_pins_is_pinned (uri)', 'sde_menu_item_set_text', '"view-pin-symbolic"'):
                self.assertIn(needle, sidebar)
        self.assertIn('liria_quick_access(staged)', (self.root / 'suite/redesign.py').read_text())

    def test_quick_access_pin_list_unit(self):
        import shutil
        import subprocess
        import tempfile
        if not shutil.which('gcc') or not shutil.which('pkg-config') or \
                subprocess.run(['pkg-config', '--exists', 'gio-2.0']).returncode != 0:
            self.skipTest('gcc and gio-2.0 development files are needed')
        flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'gio-2.0'], capture_output=True, text=True,
                               check=True).stdout.split()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'test-liria-sde-pins'
            subprocess.run(['gcc', '-Wall', '-Werror', '-o', str(binary),
                            str(self.overlay / 'sde-tools/test-liria-sde-pins.c')] + flags, check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ok 3 /liria/pins/defaults-and-edits', result.stdout)

    def test_model_unit(self):
        """Ribbon modes and preferences, Home view suggestions and grid,
        pin-on-drop decisions (src/liria-sde-model.c)."""
        import shutil
        import subprocess
        import tempfile
        if not shutil.which('gcc') or not shutil.which('pkg-config') or \
                subprocess.run(['pkg-config', '--exists', 'gio-2.0']).returncode != 0:
            self.skipTest('gcc and gio-2.0 development files are needed')
        flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'gio-2.0'], capture_output=True, text=True,
                               check=True).stdout.split()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'test-liria-sde-model'
            subprocess.run(['gcc', '-Wall', '-Werror', '-o', str(binary),
                            str(self.overlay / 'sde-tools/test-liria-sde-model.c')] + flags, check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in ('ribbon-modes', 'prefs', 'commands', 'frequent-folders', 'columns', 'drop-decisions',
                         'pin-on-drop'):
                self.assertIn('/liria/model/' + name, result.stdout)
        make = (self.overlay / 'sde-tools/Makefile.am').read_text()
        self.assertIn('test-liria-sde-model', make)
        self.assertIn('liria-sde-model.c', make)

    def test_home_view_replaces_folder_grid(self):
        home = (self.overlay / 'sde-tools/liria-sde-home.c').read_text()
        for needle in ('"sde-home-replaces-view"', 'liria_sde_home_frequent_folders', 'liria_sde_drop_decide',
                       '"sde-drop"', 'sde-home-section-button', 'liria_sde_prefs_set_flag ("home", "collapsed"',
                       'drive_drag_dest', 'card_drag_source', 'Frequent folder', '"sde-drag-ask"'):
            self.assertIn(needle, home)
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        for needle in ('"sde-home-replaces-view"', 'gtk_widget_set_visible (slot->view_box, !replace)',
                       '"sde-drop"', 'liria_drag_drop_action_ask'):
            self.assertIn(needle, ui)
        recipe = (self.root / 'suite/redesign.py').read_text()
        for name in ('liria_home_view', 'liria_right_drag'):
            self.assertIn(name + '(staged)', recipe)

    def test_sidebar_pinned_colour_icons_and_pathbar(self):
        recipe = (self.root / 'suite/redesign.py').read_text()
        for name in ('liria_sidebar_pinned', 'liria_pathbar'):
            self.assertIn(name + '(staged)', recipe)
        helpers = self.redesign.SIDEBAR_PINNED_HELPERS
        for needle in ('liria_sde_pins_load', 'SECTION_COMPUTER', 'liria_sde_ui_sidebar_collapsed',
                       'liria_sde_ui_sidebar_toggle', 'view-pin-symbolic', 'g_file_monitor_file',
                       'pan-end-symbolic'):
            self.assertIn(needle, helpers)
        for needle in ('_("Pinned")', '_("Drives")', 'liria_sde_pins_add ((const char * const *) uris, position)',
                       'need_sliders = FALSE', 'liria_sde_ui_sidebar_icon_pixels ()', 'sde_crumb_click_check',
                       'slot->pending_location'):
            self.assertIn(needle, recipe)
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        # Full-colour theme icons in the sidebar, no symbolic-first lookup.
        self.assertNotIn('liria_sde_ui_symbolic_icon', ui + recipe)
        for needle in ('liria_sde_ui_sidebar_icon', 'gtk-xft-dpi', 'sde_location_area_pressed',
                       'liria_navigation_window_pane_show_navigation_bar_temporarily',
                       '"sidebar", "collapsed"'):
            self.assertIn(needle, ui)

    def test_ribbon(self):
        ribbon = (self.overlay / 'src/liria-sde-ribbon.c').read_text()
        # Tabs, contextual tabs and the commands asked for.
        for needle in ('_("Home")', '_("Share")', '_("View")', '_("Drive")', '_("Archive")',
                       '_("Pin to Quick access")', '"Copy"', '"Paste"', '"Cut"', '_("Copy path")',
                       '_("Paste shortcut")', '_("Move to")', '_("Copy to")', '"Trash"', '"Rename"',
                       '"New Folder"', '"New Empty File"', '"Properties"', '"Open"', '"OtherApplication1"',
                       '"Select All"', '_("Select none")', '"Invert Selection"', '"xdg-email"', '"--add"',
                       '_("Copy to drive")', '"Show Hide Sidebar"', '"Show Hide Extra Pane"', '"Show Hidden Files"',
                       '_("Sort by")', '"Self Eject Volume"', '"--extract-here"', 'GDK_ACTION_LINK'):
            self.assertIn(needle, ribbon)
        # Three modes, remembered, Ctrl+F1, sensitivity from the selection model.
        for needle in ('liria_sde_ribbon_mode_save', 'liria_sde_ribbon_mode_toggle', 'GDK_KEY_F1',
                       'LIRIA_SDE_RIBBON_OFF', 'LIRIA_SDE_RIBBON_COLLAPSED', 'liria_sde_selection_commands',
                       'gtk_application_get_windows', '"actions-changed"', '"selection-changed"'):
            self.assertIn(needle, ribbon)
        # Symbolic icons of the theme only, no product branding.
        self.assertNotRegex(ribbon, r'(?i)microsoft|windows explorer')
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        self.assertIn('liria_sde_ribbon_setup (window, ui_manager)', ui)
        self.assertIn('liria_sde_ribbon_add_mode_items (sde_submenu (menu, _("Ri_bbon")))', ui)
        self.assertNotIn('"<Control>F1", G_CALLBACK (sde_accel_window_call)', ui)
        self.assertIn('liria_ribbon(staged)', (self.root / 'suite/redesign.py').read_text())
        css = (self.overlay / 'data/liria-sde.css').read_text()
        for needle in ('button.sde-ribbon-tab:checked', 'button.sde-ribbon-button', '.sde-ribbon-group-label'):
            self.assertIn(needle, css)

    def test_drag_and_drop_smoke(self):
        """Real drags on Xvfb with xdotool (tools/liria-dnd-smoke.py) when a
        built Liria is named by SDE_LIRIA_PREFIX (e.g. a DESTDIR stage/usr)."""
        import os
        import subprocess
        import sys
        script = self.root / 'tools/liria-dnd-smoke.py'
        self.assertIn('right-button drag asks', script.read_text())
        prefix = os.environ.get('SDE_LIRIA_PREFIX')
        if not prefix:
            self.skipTest('set SDE_LIRIA_PREFIX to a built Liria prefix')
        result = subprocess.run([sys.executable, str(script), '--prefix', prefix], capture_output=True, text=True,
                                timeout=300)
        if result.returncode == 77:
            self.skipTest(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_home_view_recipe_restores_view(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            staged = Path(tmp)
            src = staged / 'src'
            src.mkdir()
            (src / 'Makefile.am').write_text('\tliria-sde-pins.h \\\n')
            (src / 'liria-window-slot.c').write_text(
                '    gtk_container_foreach (GTK_CONTAINER (slot->extra_location_widgets),\n'
                '                           remove_all,\n'
                '                           slot->extra_location_widgets);\n'
                '    gtk_widget_hide (slot->extra_location_frame);\n}\n')
            self.redesign.liria_home_view(staged)
            self.assertIn('liria-sde-model.c', (src / 'Makefile.am').read_text())
            slot = (src / 'liria-window-slot.c').read_text()
            self.assertIn('gtk_widget_show (slot->view_box);', slot)
            self.assertIn('gtk_box_set_child_packing', slot)

    def test_prepared_tree_has_no_brand_leaks(self):
        source = self.root / 'vendor/suite/liria'
        if not source.is_dir():
            self.skipTest('prepare the native suite first')
        self.assertEqual(self.check.scan_source(source), [])


class GtkCss:
    """A small GTK CSS cascade for the style sheet checks: rules in order
    (a later rule wins at equal specificity), compound selectors with
    element names, classes and pseudo-classes (also in :not()), descendant
    and child combinators. Structural pseudo-classes never match."""

    STATES = {'hover', 'active', 'selected', 'focus', 'backdrop', 'disabled', 'checked', 'drop(active)'}
    SIMPLE = r'(?:\*|[\w-]+|\.[\w-]+|#[\w-]+|:[\w-]+(?:\([^()]*\))?)'

    def __init__(self, *texts):
        import re
        self.rules = []
        text = re.sub(r'/\*.*?\*/', '', '\n'.join(texts), flags=re.S)
        text = re.sub(r'@(?:define-color|import)[^;]*;', '', text)
        for selectors, body in re.findall(r'([^{}]+)\{([^{}]*)\}', text):
            declarations = dict((k.strip(), v.strip()) for k, v in
                                (d.split(':', 1) for d in body.split(';') if ':' in d))
            for selector in selectors.split(','):
                parsed = self.parse(selector.strip())
                if parsed:
                    self.rules.append((self.specificity(parsed), len(self.rules), parsed, declarations))

    def parse(self, selector):
        import re
        parts, combinator = [], ' '
        for token in re.findall(r'>|[^\s>]+', selector):
            if token == '>':
                combinator = '>'
                continue
            simples = re.findall(r':not\(' + self.SIMPLE + r'\)|' + self.SIMPLE, token)
            if ''.join(simples) != token:
                return None
            parts.append((combinator, simples))
            combinator = ' '
        return parts

    @staticmethod
    def specificity(parts):
        a = b = c = 0
        for _, simples in parts:
            for s in simples:
                s = s[5:-1] if s.startswith(':not(') else s
                if s.startswith('#'):
                    a += 1
                elif s.startswith(('.', ':')):
                    b += 1
                elif s != '*':
                    c += 1
        return a, b, c

    def matches_simple(self, simple, node):
        name, classes, states = node
        if simple.startswith(':not('):
            return not self.matches_simple(simple[5:-1], node)
        if simple.startswith('.'):
            return simple[1:] in classes
        if simple.startswith(':'):
            return simple[1:] in states
        return simple in ('*', name)

    def matches(self, parts, path):
        def match(i, j):  # parts[:i + 1] with parts[i] on path[j]
            if not all(self.matches_simple(s, path[j]) for s in parts[i][1]):
                return False
            if i == 0:
                return True
            if parts[i][0] == '>':
                return j > 0 and match(i - 1, j - 1)
            return any(match(i - 1, k) for k in range(j - 1, -1, -1))
        return match(len(parts) - 1, len(path) - 1)

    def value(self, path, *properties):
        """The winning declaration of the first of |properties| declared
        (e.g. 'background-color', 'background') for the last node of
        |path|, a list of (element, classes, states)."""
        best = None
        for spec, order, parts, declarations in self.rules:
            for prop in properties:
                if prop in declarations and self.matches(parts, path):
                    if best is None or (spec, order) > best[0]:
                        best = ((spec, order), declarations[prop])
                    break
        return best[1] if best else None


class LiriaRubberBandCssTest(unittest.TestCase):
    """0.2.0: the rubber band is a translucent accent sheet with a 1 px
    accent border in every SDE theme and in Liria's own style sheet, and
    selected files stay visible (the .view rules made the band an opaque
    sheet over the files being selected)."""

    def setUp(self):
        self.root = Path(__file__).resolve().parents[1]
        self.overlays = self.root / 'data/themes/overlays'
        self.liria_css = (self.root / 'suite/overlays/liria/data/liria-sde.css').read_text()

    def overlay(self, name):
        return (self.overlays / name).read_text()

    def theme_chains(self):
        """The SDE layers of each family's gtk-3.0/gtk.css, in the order of
        tools/build-themes.py (Glass imports Graphite)."""
        graphite = [self.overlay(n + '-gtk-3.0.css') for n in ('graphite', 'soft', 'caption', 'apps')]
        fluent = [self.overlay(n + '-gtk-3.0.css') for n in ('fluent', 'soft', 'caption', 'apps')]
        glass = graphite + [self.overlay(n + '-gtk-3.0.css') for n in ('glass', 'soft', 'caption')]
        return {'graphite': GtkCss(*graphite), 'fluent': GtkCss(*fluent), 'glass': GtkCss(*glass)}

    def assertTranslucentAccent(self, value, accent, what):
        import re
        match = re.fullmatch(r'alpha\(\s*' + re.escape(accent) + r'\s*,\s*([0-9.]+)\s*\)', value or '')
        self.assertIsNotNone(match, '%s: background %r is not a translucent %s' % (what, value, accent))
        self.assertTrue(0.15 <= float(match.group(1)) <= 0.25, '%s: alpha %s' % (what, match.group(1)))

    # The nodes Liria's icon view (an EelCanvas with the .view class, which
    # adds .rubberband while it draws the band), another program's icon
    # view and a tree view (a rubberband node) style the band with.
    LIRIA = [('window', {'background', 'liria-navigation-window'}, set()), ('box', set(), set()),
             ('notebook', {'liria-notebook'}, set()), ('scrolledwindow', set(), set())]
    BANDS = {
        'Liria icon view': LIRIA + [('widget', {'view', 'rubberband'}, set())],
        'other icon view': [('window', {'background'}, set()), ('widget', {'view', 'rubberband'}, set())],
        'tree view': [('window', {'background'}, set()), ('treeview', {'view'}, set()), ('rubberband', set(), set())],
        'Liria tree view': LIRIA + [('treeview', {'view'}, set()), ('rubberband', set(), set())],
    }

    def test_theme_rubber_band_is_translucent_accent(self):
        for family, css in self.theme_chains().items():
            for what, path in self.BANDS.items():
                label = '%s, %s' % (family, what)
                self.assertTranslucentAccent(css.value(path, 'background-color', 'background'), '@sde_accent', label)
                self.assertEqual(css.value(path, 'border'), '1px solid @sde_accent', label)
        gtk4 = GtkCss(self.overlay('soft-gtk-4.0.css'))
        for path in ([('window', set(), set()), ('gridview', {'view'}, set()), ('rubberband', set(), set())],
                     [('window', set(), set()), ('columnview', {'view'}, set()), ('listview', {'view'}, set()),
                      ('rubberband', set(), set())]):
            self.assertTranslucentAccent(gtk4.value(path, 'background-color'), '@sde_accent', 'GTK 4')
            self.assertEqual(gtk4.value(path, 'border'), '1px solid @sde_accent')

    def test_liria_rubber_band_and_selection(self):
        css = GtkCss(self.liria_css)
        for what in ('Liria icon view', 'Liria tree view'):
            self.assertTranslucentAccent(css.value(self.BANDS[what], 'background-color'),
                                         '@theme_selected_bg_color', what)
            self.assertEqual(css.value(self.BANDS[what], 'border'), '1px solid @theme_selected_bg_color')
        self.assertEqual(css.value(self.LIRIA + [('widget', {'view'}, set())], 'background-color'),
                         '@theme_base_color')
        # Selected rows: a soft accent highlight, not the view colour; the
        # sidebar keeps its own.
        row = css.value(self.LIRIA + [('treeview', {'view'}, {'selected'})], 'background-color')
        self.assertTrue(row.startswith('alpha(@theme_selected_bg_color'), row)
        side = [('window', {'background', 'liria-navigation-window'}, set()), ('widget', {'liria-side-pane'}, set()),
                ('treeview', {'view'}, {'selected'})]
        self.assertEqual(css.value(side, 'background-color'), 'alpha(@theme_selected_bg_color, 0.18)')
        # Selected icons: the canvas multiplies them by the background
        # colour, so it is a light, fully transparent tint; the label
        # highlight is an image. Also when the view has no focus (:active).
        for state in ('selected', 'active'):
            item = self.LIRIA + [('widget', {'view', 'liria-canvas-item'}, {state})]
            self.assertEqual(css.value(item, 'background-color'),
                             'alpha(mix(white, @theme_selected_bg_color, 0.35), 0)')
            self.assertIn('alpha(@theme_selected_bg_color, 0.24)', css.value(item, 'background-image'))
        # The theme layers alone keep selected rows out of the view colour.
        for family, theme in self.theme_chains().items():
            value = theme.value(self.LIRIA + [('treeview', {'view'}, {'selected'})], 'background-color')
            self.assertNotIn(value, ('@sde_view', '@sde_surface'), family)

    def test_gtk3_not_takes_simple_selectors(self):
        """GTK 3 rejects a compound selector in :not() and drops the rule
        with a parsing warning in every application."""
        import re
        sheets = {p.name: p.read_text() for p in self.overlays.glob('*-gtk-3.0.css')}
        sheets['liria-sde.css'] = self.liria_css
        for name, text in sheets.items():
            text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
            for inner in re.findall(r':not\(([^()]*(?:\([^()]*\))?)\)', text):
                self.assertRegex(inner, r'^(?:[.:#]?[\w-]+|:[\w-]+\([\w-]+\))$', '%s: :not(%s)' % (name, inner))

    def test_rubber_band_smoke(self):
        """Real drags on Xvfb (tools/liria-rubberband-smoke.py) when a built
        Liria is named by SDE_LIRIA_PREFIX; SDE_THEMES names the built SDE
        themes (default build/themes), SDE_RUBBERBAND_ALL=1 runs every
        family, mode, scale and compositor setting."""
        import os
        import subprocess
        import sys
        script = self.root / 'tools/liria-rubberband-smoke.py'
        self.assertIn('files hidden under the band', script.read_text())
        prefix = os.environ.get('SDE_LIRIA_PREFIX')
        if not prefix:
            self.skipTest('set SDE_LIRIA_PREFIX to a built Liria prefix')
        command = [sys.executable, str(script), '--prefix', prefix]
        if os.environ.get('SDE_THEMES'):
            command += ['--themes', os.environ['SDE_THEMES']]
        if os.environ.get('SDE_RUBBERBAND_ALL') == '1':
            command.append('--all')
        result = subprocess.run(command, capture_output=True, text=True, timeout=1800)
        if result.returncode == 77:
            self.skipTest(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
