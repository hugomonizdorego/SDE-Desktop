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

    def test_prepared_tree_has_no_brand_leaks(self):
        source = self.root / 'vendor/suite/liria'
        if not source.is_dir():
            self.skipTest('prepare the native suite first')
        self.assertEqual(self.check.scan_source(source), [])


if __name__ == "__main__":
    unittest.main()
