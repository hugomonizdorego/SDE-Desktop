#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Legal data consistency (sde-legal-data).

data/legal/components.json is the single source of truth for licenses,
copyright and trademarks (docs/LEGAL.md). This test checks that

  * the file is valid (tools/legal-data.py) and the trademark notices are
    exactly the official sentences;
  * every installed desktop application (data/applications) and every
    AppStream file (data/metainfo) belongs to a component, with the same
    license (project_license) and icon;
  * data/suite.tsv names the same licenses and no longer the old developer;
  * the license of every source file (its SPDX-License-Identifier header)
    agrees with the debian/copyright stanza that covers it and with the
    license of the component it belongs to;
  * debian/copyright describes every license it uses, and its stanzas for
    SDE's own files name the Selviana DE Project.
"""
import fnmatch
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('legal_data', ROOT / 'tools/legal-data.py')
legal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(legal)

DATA = legal.load()
COMPONENTS = DATA['components']
errors = []

OFFICIAL_TRADEMARKS = {
    'sde': 'Selviana Desktop Environment, SDE and the SDE logo are trademarks of the Selviana DE Project.',
    'linux': 'Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.',
    'debian': 'Debian is a registered trademark owned by Software in the Public Interest, Inc.',
    'paypal': 'PayPal is a trademark of PayPal, Inc.',
}
# Files maintained with the Control Center rename (see test_metainfo.py):
# their differences are reported, not failed, until they are correct; then
# the entry must go (strict, so the exception cannot outlive its reason).
PENDING = {'sde-settings.desktop', 'org.debion.Settings.metainfo.xml'}
pending_left = set()

# DEP-5 license names that differ from SPDX.
DEP5_ALIASES = {
    'GPL-3+': 'GPL-3.0-or-later', 'GPL-2+': 'GPL-2.0-or-later', 'GPL-3': 'GPL-3.0-only',
    'GPL-3.0': 'GPL-3.0-only', 'LGPL-2+': 'LGPL-2.0-or-later',
    'LGPL-2.0-only with FLTK exception': 'LGPL-2.0-only WITH FLTK-exception',
}


def parts(expression):
    return set(legal.license_parts(expression))


def spdx_parts(dep5_license):
    """'MPL-2.0 and MIT' -> {'MPL-2.0', 'MIT'} in SPDX names."""
    name = dep5_license.strip()
    if name in DEP5_ALIASES:
        return {DEP5_ALIASES[name]}
    out = set()
    for p in re.split(r'\s+(?:and|AND|or|OR)\s+', name):
        out.add(DEP5_ALIASES.get(p.strip(), p.strip()))
    return out


# ------------------------------------------------------------ the JSON file

errors += legal.validate(DATA)
for key, text in OFFICIAL_TRADEMARKS.items():
    if DATA['trademarks'].get(key) != text:
        errors.append(f'trademark notice "{key}" must be exactly: {text}')
for key in DATA['trademarks']:
    if key not in OFFICIAL_TRADEMARKS:
        errors.append(f'trademark notice "{key}" is not one of the official ones')
if '®' in DATA['trademarks']['sde']:
    errors.append('the SDE notice must not claim a registration (®)')
for c in COMPONENTS:
    for line in c.get('copyright', []):
        if 'DebION' in line:
            errors.append(f'{c["id"]}: copyright names DebION: {line}')
    if c['kind'] != 'integration' and 'DebION' in json.dumps(c):
        errors.append(f'{c["id"]}: SDE components must not name DebION')

# ------------------------------------------- desktop files and metainfo

by_desktop, by_metainfo = {}, {}
for c in COMPONENTS:
    for d in c.get('desktop', []):
        by_desktop[d] = c
    if c.get('metainfo'):
        by_metainfo[c['metainfo']] = c


def desktop_entry(path):
    entry, section = {}, None
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line.startswith('['):
            section = line
        elif section == '[Desktop Entry]' and '=' in line and not line.startswith('#'):
            k, v = line.split('=', 1)
            entry[k] = v
    return entry


for path in sorted((ROOT / 'data/applications').glob('*.desktop')):
    c = by_desktop.get(path.name)
    if not c:
        errors.append(f'{path.name}: no component in data/legal/components.json')
        continue
    icon = desktop_entry(path).get('Icon', '')
    if c.get('icons') and icon != c['icons'][0]:
        if path.name in PENDING:
            pending_left.add(path.name)
            print(f'PENDING (Control Center rename): {path.name}: Icon={icon}, legal data: {c["icons"][0]}')
        else:
            errors.append(f'{path.name}: Icon={icon} but the legal data names {c["icons"][0]}')
for d in by_desktop:
    if not (ROOT / 'data/applications' / d).is_file():
        errors.append(f'{d}: named by the legal data but not in data/applications')

for path in sorted((ROOT / 'data/metainfo').glob('*.metainfo.xml')):
    c = by_metainfo.get(path.name)
    text = path.read_text(encoding='utf-8')
    if not c:
        errors.append(f'{path.name}: no component in data/legal/components.json')
        continue
    m = re.search(r'<project_license>([^<]+)</project_license>', text)
    declared = m.group(1).strip() if m else ''
    if parts(declared) != parts(c['license']):
        msg = f'{path.name}: project_license {declared} but the legal data says {c["license"]}'
        if path.name in PENDING:
            pending_left.add(path.name)
            print('PENDING (Control Center rename):', msg)
        else:
            errors.append(msg)
for m in by_metainfo:
    if not (ROOT / 'data/metainfo' / m).is_file():
        errors.append(f'{m}: named by the legal data but not in data/metainfo')
for name in sorted(PENDING - pending_left):
    errors.append(f'{name} agrees with the legal data now: remove it from PENDING in tests/test_legal_data.py')

# ------------------------------------------------------------ data/suite.tsv

for line in (ROOT / 'data/suite.tsv').read_text(encoding='utf-8').splitlines():
    if not line or line.startswith('#'):
        continue
    f = line.split('\t')
    app, upstream, license = f[0], f[6], f[8]
    c = next((x for x in COMPONENTS if x['id'] == app or app in x.get('programs', [])), None)
    if not c:
        errors.append(f'suite.tsv: {app} has no component')
        continue
    if 'DebION' in upstream:
        errors.append(f'suite.tsv: {app}: the upstream column still names {upstream}')
    if c.get('icons') and f[5] != c['icons'][0]:
        errors.append(f'suite.tsv: {app}: icon {f[5]} but the legal data names {c["icons"][0]}')
    named = set(re.findall(r'[A-Z][A-Za-z0-9.+-]*(?: WITH [A-Za-z0-9.-]+)?', license))
    if not parts(c['license']) <= named | {'GPL-3.0-or-later'} or not (named & parts(c['license'])):
        errors.append(f'suite.tsv: {app}: license "{license}" does not match the legal data ({c["license"]})')

# ------------------------------------------------------------- debian/copyright


def dep5(path):
    paragraphs, cur = [], {}
    key = None
    for raw in path.read_text(encoding='utf-8').splitlines() + ['']:
        if not raw.strip():
            if cur:
                paragraphs.append(cur)
            cur, key = {}, None
            continue
        if raw[0] in ' \t':
            if key:
                cur[key] += '\n' + raw.strip()
            continue
        key, _, value = raw.partition(':')
        cur[key] = value.strip()
    return paragraphs


paras = dep5(ROOT / 'debian/copyright')
files_paras = [p for p in paras if 'Files' in p]
license_paras = {p['License'].split('\n')[0].strip() for p in paras if 'License' in p and 'Files' not in p}


def stanza_for(rel):
    found = None
    for p in files_paras:
        for pattern in p['Files'].split():
            if fnmatch.fnmatchcase(rel, pattern):
                found = p
    return found


for p in files_paras:
    lic = p['License'].split('\n')
    name, has_text = lic[0].strip(), len(lic) > 1
    if not has_text and name not in license_paras:
        for single in re.split(r'\s+and\s+', name):
            if single.strip() not in license_paras:
                errors.append(f'debian/copyright: License "{single}" (Files: {p["Files"].split()[0]}) has no '
                              'stand-alone License paragraph')
    first = p['Files'].split()[0]
    own = not first.startswith(('vendor/', 'sde-fltk/')) and 'Clement' not in p['Copyright'] \
        and 'zac15987' not in p['Copyright']
    if own and 'Selviana DE Project' not in p['Copyright']:
        errors.append(f'debian/copyright: Files: {first}: SDE\'s own files are '
                      'Copyright Selviana DE Project (and contributors)')
    if 'DebION Dev-Team' in p['Copyright']:
        errors.append(f'debian/copyright: Files: {first} still names the DebION Dev-Team')

SOURCE_DIRS = {'bootstrap', 'data', 'debian', 'docs', 'po', 'release', 'rust', 'sde-fltk', 'src', 'suite',
               'tests', 'tools', 'upstream', '.github'}


def source_tree():
    """The files of the source tree without git (e.g. in a package build):
    no build directories, prepared upstream trees or package staging."""
    out = []
    for p in ROOT.rglob('*'):
        rel = p.relative_to(ROOT)
        if not p.is_file() or '.git' in rel.parts or '__pycache__' in rel.parts:
            continue
        if len(rel.parts) > 1 and rel.parts[0] not in SOURCE_DIRS:
            continue
        if rel.parts[0] == 'debian' and len(rel.parts) > 2 and rel.parts[1] not in ('source', 'tests'):
            continue  # debian/sde/..., debian/.debhelper/...
        out.append(str(rel))
    return out


try:
    tracked = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True, text=True,
                             check=True).stdout.split('\n')
    if not any(tracked):
        raise subprocess.CalledProcessError(1, 'git')
except (OSError, subprocess.CalledProcessError):
    tracked = source_tree()
tracked = [t for t in tracked if t and not t.startswith(('vendor/', 'build/'))]

SPDX = re.compile(r'SPDX-License-Identifier:\s*([A-Za-z0-9.+ -]+?)(?:\s*(?:-->|\*/|;|$))')


def header_license(rel):
    try:
        with open(ROOT / rel, encoding='utf-8') as fh:
            head = [next(fh, '') for _ in range(4)]
    except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
        return None
    for line in head:
        m = SPDX.search(line)
        if m and m.group(1).strip():
            return m.group(1).strip()
    return None


component_of = {}
for c in COMPONENTS:
    for src in c.get('sources', []):
        for t in tracked:
            if t == src or (src.endswith('/') and t.startswith(src)):
                component_of[t] = c

checked = 0
for rel in tracked:
    lic = header_license(rel)
    if not lic:
        continue
    checked += 1
    stanza = stanza_for(rel)
    if not stanza:
        errors.append(f'{rel}: not covered by debian/copyright')
        continue
    allowed = spdx_parts(stanza['License'].split('\n')[0])
    if 'individual-upstream-licenses' not in allowed and not parts(lic) <= allowed:
        errors.append(f'{rel}: SPDX {lic} but debian/copyright (Files: {stanza["Files"].split()[0]}) says '
                      f'{stanza["License"].splitlines()[0]}')
    c = component_of.get(rel)
    if c and not parts(lic) <= parts(c['license']):
        errors.append(f'{rel}: SPDX {lic} is not part of the license of {c["id"]} ({c["license"]})')

# Each component's sources are covered by stanzas with its licenses.
for c in COMPONENTS:
    for src in c.get('sources', []):
        files = [t for t in tracked if t == src or (src.endswith('/') and t.startswith(src))]
        for rel in files[:1] or ([src.rstrip('/') + '/x'] if src.endswith('/') else [src]):
            stanza = stanza_for(rel)
            if not stanza:
                errors.append(f'{c["id"]}: {rel} is not covered by debian/copyright')
                continue
            allowed = spdx_parts(stanza['License'].split('\n')[0])
            if 'individual-upstream-licenses' not in allowed and not (allowed & parts(c['license'])):
                errors.append(f'{c["id"]}: debian/copyright gives {rel} {stanza["License"].splitlines()[0]}, '
                              f'the legal data {c["license"]}')

for e in errors:
    print('FAIL:', e)
print(f'{len(COMPONENTS)} components, {checked} files with SPDX headers checked against debian/copyright, '
      f'{len(errors)} problems')
sys.exit(1 if errors else 0)
