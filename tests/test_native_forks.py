# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("forks", Path(__file__).resolve().parents[1] / "tools/prepare-native-suite.py")
forks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(forks)


class BrandingTest(unittest.TestCase):
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
