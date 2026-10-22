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
        ui = (self.overlay / 'src/liria-sde-ui.c').read_text()
        self.assertIn('Developer: DebION Dev-Team', ui)
        self.assertIn('"DebION Dev-Team", NULL', ui)
        self.assertIn('Copyright \\xC2\\xA9 2011-2021 The Caja authors', ui)
        self.assertIn('Copyright \\xC2\\xA9 1999-2009 The Nautilus authors', ui)
        self.assertIn('/usr/share/doc/sde/upstream/caja/', ui)
        self.assertIn('@SDE_BUILD_VERSION@', ui)

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


if __name__ == "__main__":
    unittest.main()
