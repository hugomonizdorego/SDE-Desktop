#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""The shortcut sheet against SDE's shortcut list (0.2.0 Alpha 4,
docs/SHORTCUTS.md):

  * every shortcut documented in a section of the list is in SDE's list
    (sde-windows shortcuts), in that section, with the same keys, and every
    shortcut of the list is documented;
  * every system shortcut has an SDE WM keybind for each of its keys (in the
    keybinds SDE writes and in the packaged data/openbox/rc.xml), and no
    application shortcut has one;
  * the shortcuts of "Not available in SDE" are bound to nothing;
  * the packaged rc.xml is well formed and the printable sheet exists.

test_shortcuts.py SDE-WINDOWS [SOURCE_DIR]
"""
import re
import subprocess as sp
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

FAIL = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what, flush=True)
    if not cond:
        FAIL.append(what)
    return cond


MODS = {'Ctrl': 'C', 'Win': 'W', 'Alt': 'A', 'Shift': 'S'}
NAMES = {'Space': 'space', 'Enter': 'Return', 'Esc': 'Escape', 'PrtScn': 'Print', '.': 'period',
         ';': 'semicolon', ',': 'comma', 'Comma': 'comma', 'Plus': 'plus', 'Minus': 'minus', '=': 'equal',
         'Backspace': 'BackSpace', 'Page Up': 'Prior', 'Page Down': 'Next'}


def openbox_key(shown):
    """'Ctrl+Win+Left' -> 'C-W-Left' (a key as written in docs/SHORTCUTS.md)."""
    if shown == 'Win':
        return 'Super_L'
    parts = shown.split('+')
    mods, key = parts[:-1], parts[-1]
    if shown.endswith('++'):
        mods, key = parts[:-2], '+'
    letters = ''.join(MODS[m] for m in mods)
    key = NAMES.get(key, key.lower() if len(key) == 1 else key)
    order = ''.join(c for c in 'CWAS' if c in letters)
    return ''.join(c + '-' for c in order) + key


def normalize(key):
    parts = key.split('-')
    mods = sorted(parts[:-1], key=lambda m: 'CWAS'.index(m) if m in 'CWAS' else 9)
    sym = parts[-1].lower() if len(parts[-1]) == 1 else parts[-1]
    return '-'.join(mods + [sym])


def doc_sections(text):
    """{heading: [[cells], ...]} of the tables in the Markdown text."""
    out, heading = {}, None
    for line in text.splitlines():
        if line.startswith('## '):
            heading = line[3:].strip()
            out.setdefault(heading, [])
        elif heading and line.startswith('|') and not re.match(r'^\|\s*-', line):
            cells = [c.strip() for c in line.strip().strip('|').split('|')]
            if cells and cells[0] not in ('Shortcut', 'Mouse'):
                out[heading].append(cells)
    return out


def main():
    tool = Path(sys.argv[1])
    root = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'LANGUAGE': 'C',
           'XDG_CONFIG_HOME': '/nonexistent-sde-shortcuts-test', 'XDG_CONFIG_DIRS': '/nonexistent-sde-shortcuts-test',
           'HOME': '/nonexistent-sde-shortcuts-test'}
    run = lambda *a: sp.run([str(tool), *a], env=env, stdout=sp.PIPE, text=True, check=True).stdout
    groups = dict(line.split('\t') for line in run('shortcuts', '--groups').splitlines())
    table = []
    for line in run('shortcuts').splitlines():
        f = line.split('\t')
        table.append(dict(id=f[0], shown=f[1], group=f[2], scope=f[3], keys=f[4].split(), label=f[5]))
    bound = {}
    for line in run('shortcuts', '--openbox').splitlines():
        m = re.match(r'<keybind key="([^"]+)"', line)
        if m:
            bound[normalize(m.group(1))] = line
    check(len(table) > 60 and len(bound) > 60, f'{len(table)} shortcuts, {len(bound)} SDE WM keybinds')

    sections = doc_sections((root / 'docs/SHORTCUTS.md').read_text())
    by_title = {title: gid for gid, title in groups.items()}
    documented = set()
    for heading, rows in sections.items():
        gid = by_title.get(heading)
        if not gid:
            continue
        for cells in rows:
            shown = cells[0]
            match = [s for s in table if s['shown'] == shown and s['group'] == gid]
            check(bool(match), f'{heading}: "{shown}" is in SDE\'s shortcut list')
            for s in match:
                documented.add(s['id'])
                check(cells[1] == s['label'], f'"{shown}": "{cells[1]}" says what the list says ("{s["label"]}")')
    for s in table:
        check(s['id'] in documented, f'"{s["shown"]}" ({s["id"]}) is documented')

    # System shortcuts are bound, application shortcuts are not.
    for s in table:
        keys = []
        for k in s['keys']:
            keys += [k.replace('%d', str(i)) for i in range(1, 10)] if '%d' in k else [k]
        for k in keys:
            if s['scope'] == 'system':
                check(normalize(k) in bound, f'{s["id"]}: {k} has a keybind')
            else:
                check(normalize(k) not in bound, f'{s["id"]}: {k} (an application shortcut) has no keybind')

    # Not available: bound to nothing.
    for cells in sections.get('Not available in SDE', []):
        for shown in cells[0].split(' or '):
            key = openbox_key(shown)
            check(normalize(key) not in bound, f'not available: {shown} ({key}) is bound to nothing')

    # The packaged configuration: well formed, the same keybinds.
    rc = root / 'data/openbox/rc.xml'
    try:
        tree = ET.parse(rc)
        check(True, 'data/openbox/rc.xml is well formed')
    except ET.ParseError as e:
        check(False, f'data/openbox/rc.xml is well formed ({e})')
        tree = None
    if tree is not None:
        ns = {'ob': 'http://openbox.org/3.4/rc'}
        keys = {normalize(k.get('key')) for k in tree.getroot().iterfind('ob:keyboard/ob:keybind', ns)}
        check(keys == set(bound), f'rc.xml binds the keys SDE writes ({len(keys)} and {len(bound)})')
        release = [k for k in tree.getroot().iterfind('ob:keyboard/ob:keybind', ns) if k.get('onRelease') == 'yes']
        check([k.get('key') for k in release] == ['Super_L', 'Super_R'], 'only Win alone acts on release')
        names = [n.text for n in tree.getroot().iterfind('ob:desktops/ob:names/ob:name', ns)]
        number = tree.getroot().findtext('ob:desktops/ob:number', namespaces=ns)
        check(number == '2' and names == ['Workspace 1', 'Workspace 2'], f'two workspaces by default ({number}, {names})')
    check((root / 'docs/SDE-Keyboard-Shortcuts.pdf').is_file(), 'the printable sheet docs/SDE-Keyboard-Shortcuts.pdf exists')
    print('FAILED: ' + '; '.join(FAIL[:20]) if FAIL else 'all shortcut sheet checks passed')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
