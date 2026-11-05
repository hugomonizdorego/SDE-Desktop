# SPDX-License-Identifier: GPL-3.0-or-later
"""The SDE suite's registry, and Liria as SDE's own program.

Up to 0.2.0 Alpha 3.1 Liria was a fork of Caja 1.28, prepared by
tools/prepare-native-suite.py from suite/overlays/liria and built with
GTK 3 and the MATE libraries. Since 0.2.0 Alpha 4 it is written from
scratch by the Selviana DE Project on SDE-FLTK (src/liria, docs/LIRIA.md),
and nothing of the fork is left: these tests keep it so. The behaviour of
the new Liria is tested by liria-tests (tests/test_liria.cpp), liria-ui
(tests/test_liria_ui.py) and liria-dbus (tests/test_liria_dbus.py).
"""
import json
import os
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')


def desktop_entry(rel):
    entry, section = {}, None
    for line in read(rel).splitlines():
        line = line.strip()
        if line.startswith('['):
            section = line
        elif section == '[Desktop Entry]' and '=' in line and not line.startswith('#'):
            k, v = line.split('=', 1)
            entry[k] = v
    return entry


class SuiteRegistryTest(unittest.TestCase):
    def test_independent_build_versions_and_desktop_placement(self):
        versions = json.loads(read('suite/versions.json'))
        rows = [r.split('\t') for r in read('data/suite.tsv').splitlines() if r and not r.startswith('#')]
        self.assertEqual(len(rows), 11)
        for row in rows:
            self.assertEqual(row[9], versions[row[0]])
            self.assertIn('alpha', row[9])
            desktop = 'org.debion.Calculator' if row[0] == 'calculator' else row[0]
            self.assertTrue((ROOT / 'data/applications' / (desktop + '.desktop')).is_file())

    def test_liria_is_a_native_sde_program(self):
        versions = json.loads(read('suite/versions.json'))
        row = [r.split('\t') for r in read('data/suite.tsv').splitlines() if r.startswith('liria\t')][0]
        self.assertEqual(row[6], 'Selviana DE Project')
        # The copy engine's headers shared with the desktop are GPL-2.0-or-later.
        self.assertEqual(row[8], 'GPL-3.0-or-later AND GPL-2.0-or-later')
        self.assertEqual(row[4], 'sde')
        self.assertEqual(versions['liria'], row[9])
        self.assertTrue(row[3].startswith('liria'))


class CajaForkRemovedTest(unittest.TestCase):
    """Nothing of the Caja fork and the machinery that built it is left."""

    def test_fork_sources_and_tools_are_gone(self):
        for rel in ['suite/overlays/liria', 'suite/forks.lock.json', 'suite/redesign.py', 'suite/sde-brand.h',
                    'tools/prepare-native-suite.py', 'tools/build-native-suite.py', 'tools/liria-branding-check.py',
                    'data/schemas/30_sde-suite.gschema.override']:
            self.assertFalse((ROOT / rel).exists(), rel)
        self.assertEqual(sorted(p.name for p in (ROOT / 'tools').glob('liria-*-smoke.py')), [])

    def test_no_native_suite_build(self):
        for rel in ['CMakeLists.txt', 'suite/CMakeLists.txt', 'src/CMakeLists.txt', 'tools/build-deb.sh']:
            text = read(rel)
            self.assertNotIn('SDE_BUILD_NATIVE_SUITE', text, rel)
            self.assertNotIn('native-suite', text, rel)
            self.assertNotIn('vendor/suite', text, rel)

    def test_no_caja_code_or_credits(self):
        word = re.compile(r'\b(caja|nautilus|eazel)\b|mate-desktop|libmate|libcaja', re.I)
        for path in sorted((ROOT / 'src/liria').rglob('*')):
            if path.is_file():
                self.assertIsNone(word.search(path.read_text(encoding='utf-8')), path)
        legal = json.loads(read('data/legal/components.json'))
        liria = [c for c in legal['components'] if c['id'] == 'liria'][0]
        self.assertEqual(liria['license'], 'GPL-3.0-or-later AND GPL-2.0-or-later')
        self.assertEqual(liria['upstream'], [])
        self.assertEqual(liria['credits'], [])
        self.assertEqual(liria['copyright'], ['Copyright © 2026 Selviana DE Project and contributors'])
        self.assertIn('src/liria/', liria['sources'])
        for rel in ['data/applications/liria.desktop', 'data/metainfo/org.debion.Liria.metainfo.xml',
                    'data/man/liria.1']:
            self.assertIsNone(word.search(read(rel)), rel)


class LiriaDataTest(unittest.TestCase):
    def test_desktop_entry(self):
        e = desktop_entry('data/applications/liria.desktop')
        self.assertEqual(e['Exec'], 'liria %U')
        self.assertEqual(e['Icon'], 'liria')
        self.assertEqual(e['StartupWMClass'], 'Liria')
        self.assertIn('inode/directory', e['MimeType'])
        self.assertNotIn('X-SDE-Upstream', e)
        self.assertEqual(e['X-Developer'], 'Selviana DE Project')

    def test_dbus_activation(self):
        for name, bus in [('org.debion.Liria.service.in', 'org.debion.Liria'),
                          ('org.debion.Liria.FileManager1.service.in', 'org.freedesktop.FileManager1')]:
            text = read('data/dbus/' + name)
            self.assertIn('Name=' + bus + '\n', text)
            self.assertIn('Exec=@CMAKE_INSTALL_FULL_BINDIR@/liria --service', text)
        self.assertIn('<dbus type="user">org.freedesktop.FileManager1</dbus>',
                      read('data/metainfo/org.debion.Liria.metainfo.xml'))

    def test_install_rules_belong_to_the_liria_component(self):
        rules = read('src/liria/CMakeLists.txt')
        installs = re.findall(r'install\((.*?)\)\n', rules, re.S)
        self.assertGreaterEqual(len(installs), 6)
        for rule in installs:
            self.assertIn('COMPONENT liria', rule)
        for path in ['bin', 'applications', 'metainfo', 'man1', 'icons', 'dbus-1/services', 'sudoers.d']:
            self.assertIn(path, rules.replace('${CMAKE_INSTALL_BINDIR}', 'bin')
                          .replace('${CMAKE_INSTALL_MANDIR}/man1', 'man1')
                          .replace('${CMAKE_INSTALL_SYSCONFDIR}/sudoers.d', 'sudoers.d'))
        top = read('CMakeLists.txt')
        self.assertIn('PATTERN "liria.desktop" EXCLUDE', top)
        self.assertIn('PATTERN "liria.*" EXCLUDE', top)
        self.assertIn('PATTERN "org.debion.Liria.metainfo.xml" EXCLUDE', top)
        self.assertIn('data/man/liria.1', top)
        self.assertNotIn('data/sudoers/sde-liria', top)

    def test_man_page_names_the_options_and_keys(self):
        man = read('data/man/liria.1')
        for word in ['\\-\\-no\\-desktop', '\\-\\-select', 'FileManager1', 'Ctrl+T', 'Shift+Delete',
                     'liria-quick-access']:
            self.assertIn(word, man)


class LiriaCommandLineTest(unittest.TestCase):
    """The built program (LIRIA_BIN, or build/src/liria/liria), without a display."""

    def binary(self):
        b = os.environ.get('LIRIA_BIN') or str(ROOT / 'build/src/liria/liria')
        if not os.access(b, os.X_OK):
            self.skipTest('liria is not built')
        return b

    def test_version_and_help(self):
        b = self.binary()
        env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'DBUS_SESSION_BUS_ADDRESS')}
        out = subprocess.run([b, '--version'], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(out.returncode, 0, out.stderr)
        versions = json.loads(read('suite/versions.json'))
        self.assertIn(versions['liria'], out.stdout)
        self.assertIn('Selviana', out.stdout)
        out = subprocess.run([b, '--help'], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(out.returncode, 0)
        self.assertIn('--no-desktop', out.stdout)
        out = subprocess.run([b, '--bogus'], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(out.returncode, 2)


if __name__ == '__main__':
    unittest.main()
