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
        self.assertEqual(len(rows), 12)
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

    def test_about_include_preserves_gnu_feature_test_macros(self):
        source = '#include "config.h"\n#define _GNU_SOURCE\n#include <unistd.h>\n#include <gtk/gtk.h>\nint main(void);\n'
        result = forks.inject_about(source)
        self.assertLess(result.index('_GNU_SOURCE'), result.index('<unistd.h>'))
        self.assertLess(result.index('<gtk/gtk.h>'), result.index('"sde-about.h"'))
        with self.assertRaises(ValueError):
            forks.inject_about('int main(void);')

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
        self.assertEqual(forks.brand('-DEOM_DATA_DIR=path mate_power_preferences_SOURCES'),
                         '-DSDEVIEW_DATA_DIR=path sde_power_preferences_SOURCES')

    def test_copyright_licenses_and_source_links_are_preserved(self):
        source = '/* Copyright (C) 2012 Pluma MATE authors. GPLv2+ */\nconst char *url = "https://github.com/mate-desktop/pluma";\n'
        self.assertEqual(forks.brand(source), source)

    def test_app_settings_and_screen_lock_contracts_are_independent(self):
        self.assertEqual(forks.brand('"org.mate.ScreenSaver" "org.mate.power-manager"'),
                         '"org.debion.ScreenSaver" "org.debion.power-manager"')
        self.assertEqual(forks.brand('"org.mate.session" "org.mate.SessionManager"'),
                         '"org.debion.session" "org.mate.SessionManager"')
        self.assertEqual(forks.brand('"org.mate.Engrampa" "org.mate.SystemMonitor" "org.mate.media-handling"'),
                         '"org.debion.Taca" "org.debion.SystemMonitor" "org.debion.media-handling"')


if __name__ == "__main__":
    unittest.main()
