#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Menus and lists of the SDE GTK themes highlight at once, and check
buttons do not look like radio buttons (0.2.0 Alpha 3.1).

1. The built style sheets (GTK 3, GTK 4 and libadwaita, imports followed):
   no rule that styles a menu item, a popover menu button, a menu bar item,
   a list or tree row, an icon view or a flow box child may have a
   transition or an animation that takes time. Liria's menu lit two or
   three items at once while they faded in and out. No tree view rule may
   depend on the row's position (:nth-child): GTK 3 then caches no cell
   style, which cost Liria's list view and sidebar a fifth to a half more
   CPU.
2. The indicators, drawn by GTK 3 (and GTK 4 when its helper was built) in
   a window and in a menu, and by SDE-FLTK (the file chooser's check
   buttons): a check box is a (rounded) square and a radio button a circle.
   The shape is measured by how much of its bounding box it fills (holes
   included): a circle fills 79 %, a square with soft corners over 90 %.

  test_theme_motion.py --themes BUILD/themes [--fltk PROBE] [--gtk3 PROBE] [--gtk4 PROBE]

Exit status 77 (skipped) when there are no built GTK themes."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

INSTANT = {'menuitem', 'modelbutton', 'row', 'treeview', 'iconview', 'flowboxchild', 'item'}
THEMES = ['SDE-Graphite-Light-Blue', 'SDE-Graphite-Dark-Green', 'SDE-Fluent-Light-Blue', 'SDE-Fluent-Dark-Purple',
          'SDE-Glass-Light-Blue', 'SDE-Glass-Dark-Blue']


# ------------------------------------------------------------------ CSS

def rules(text):
    """(selector text, body) of the top-level rules, comments and @-blocks skipped."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    i, n, depth, head, body = 0, len(text), 0, 0, 0
    while i < n:
        c = text[i]
        if c in '"\'':
            j = text.find(c, i + 1)
            i = n if j < 0 else j + 1
            continue
        if c == '{':
            if depth == 0:
                body = i + 1
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                sel = text[head:body - 1].strip()
                if not sel.startswith('@'):
                    yield sel, text[body:i]
                head = i + 1
        elif c == ';' and depth == 0:
            head = i + 1
        i += 1


def selectors(head):
    out, depth, cur = [], 0, ''
    for ch in head:
        depth += (ch == '(') - (ch == ')')
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    return [s for s in out + [cur.strip()] if s]


def instant(selector):
    s = selector
    while True:
        t = re.sub(r':not\([^()]*\)', '', s)
        if t == s:
            break
        s = t
    return bool(set(re.findall(r'(?:^|[\s>+~])([A-Za-z][\w-]*)', s)) & INSTANT)


def takes_time(value):
    """Whether a transition or animation value has a duration above 0."""
    value = value.strip()
    if value in ('none', 'initial', 'unset', 'inherit'):
        return False
    for part in re.split(r',(?![^(]*\))', value):
        times = re.findall(r'(?<![\w.-])(\d*\.?\d+)(ms|s)\b', part)
        if times and float(times[0][0]) > 0:
            return True
    return False


def load_css(path, seen=None):
    """|path| with its @import url(...) rules replaced by the imported text."""
    seen = seen or set()
    path = Path(path).resolve()
    if path in seen or not path.is_file():
        return ''
    seen.add(path)
    text = path.read_text(errors='replace')

    def imp(m):
        return load_css(path.parent / m.group(1), seen)
    return re.sub(r'@import\s+url\(\s*["\']?([^"\')]+)["\']?\s*\)\s*;', imp, text)


def by_position(selector):
    """A tree view selector that depends on the row's position (:nth-child,
    :nth-last-child): GTK 3 then styles every cell of every tree view by its
    row and caches none of them (Liria's list view and sidebar spent a fifth
    to a half more CPU on one such rule, 0.2.0 Alpha 3.1)."""
    return 'treeview' in selector and re.search(r':nth-(?:last-)?child\(', selector) is not None


def check_motion(themes):
    failures, files = [], 0
    for d in sorted(p for p in themes.iterdir() if p.is_dir() and p.name.startswith('SDE-')):
        for sub in ('gtk-3.0/gtk.css', 'gtk-4.0/gtk.css', 'libadwaita/gtk.css'):
            css = d / sub
            if not css.exists():
                continue
            files += 1
            for head, body in rules(load_css(css)):
                decls = re.findall(r'(?<![\w-])(transition|animation)(?:-duration)?\s*:\s*([^;]*)', body)
                slow = [f'{k}: {v.strip()}' for k, v in decls if takes_time(v)]
                if slow and any(instant(s) for s in selectors(head)):
                    failures.append(f'{d.name}/{sub}: {" ".join(head.split())[:120]} {{ {"; ".join(slow)} }}')
                for s in selectors(head):
                    if by_position(s):
                        failures.append(f'{d.name}/{sub}: {" ".join(s.split())[:120]}: tree view rows by position')
    return files, failures


# ----------------------------------------------------------- indicators

def read_pam(path):
    data = Path(path).read_bytes()
    end = data.index(b'ENDHDR\n') + 7
    head = data[:end].decode()
    w = int(re.search(r'WIDTH (\d+)', head).group(1))
    h = int(re.search(r'HEIGHT (\d+)', head).group(1))
    px = data[end:]
    return w, h, [tuple(px[i:i + 4]) for i in range(0, w * h * 4, 4)]


def shape(path):
    """(fill ratio of the bounding box, aspect) of the indicator in |path|:
    pixels with alpha (GTK) or unlike the background (FLTK, opaque)."""
    w, h, px = read_pam(path)
    opaque = all(p[3] == 255 for p in px)
    bg = px[0]
    if opaque:
        ink = [sum(abs(a - b) for a, b in zip(p[:3], bg[:3])) > 60 for p in px]
    else:
        ink = [p[3] >= 128 for p in px]
    xs = [i % w for i, v in enumerate(ink) if v]
    ys = [i // w for i, v in enumerate(ink) if v]
    if not xs:
        return None
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    bw, bh = x1 - x0 + 1, y1 - y0 + 1
    # The outside: background pixels reachable from the box's border.
    outside = set()
    stack = [(x, y) for x in range(x0, x1 + 1) for y in (y0, y1)] + \
            [(x, y) for y in range(y0, y1 + 1) for x in (x0, x1)]
    while stack:
        x, y = stack.pop()
        if (x, y) in outside or not (x0 <= x <= x1 and y0 <= y <= y1) or ink[y * w + x]:
            continue
        outside.add((x, y))
        stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return 1 - len(outside) / (bw * bh), bw / bh


def check_indicators(out, kind, names, lenient=False):
    """Failures for the images of one helper run: checks square, radios round.
    |lenient| (GTK 4): an indicator the installed GTK does not draw (an
    older GTK 4 than the theme's style sheet needs) is noted, not failed."""
    failures, report = [], []
    for ctx in ('window', 'menu', 'fltk'):
        got = {}
        for name in names:
            p = out / f'{ctx}-{name}.pam'
            if p.exists():
                got[name] = shape(p)
        if not got:
            continue
        for name, s in got.items():
            if s is None:
                if lenient:
                    report.append(f'{ctx}-{name} not drawn')
                else:
                    failures.append(f'{kind} {ctx}-{name}: nothing drawn')
                continue
            fill, aspect = s
            report.append(f'{ctx}-{name} {fill:.2f}')
            if not 0.8 <= aspect <= 1.25:
                failures.append(f'{kind} {ctx}-{name}: not square ({aspect:.2f})')
            if name.startswith('check') and fill < 0.87:
                failures.append(f'{kind} {ctx}-{name}: round like a radio button (fills {fill:.2f})')
            if name.startswith('radio') and fill > 0.84:
                failures.append(f'{kind} {ctx}-{name}: square like a check box (fills {fill:.2f})')
    print(f'  {kind}: ' + ', '.join(report))
    return failures


def xvfb():
    """A private X server: (process, display), or (None, None). The display
    is SDE_TEST_DISPLAY when set, otherwise the first free one (-displayfd)."""
    if not shutil.which('Xvfb'):
        return None, None
    fixed = os.environ.get('SDE_TEST_DISPLAY', '').lstrip(':')
    if fixed:
        p = subprocess.Popen(['Xvfb', f':{fixed}', '-screen', '0', '800x600x24', '-nolisten', 'tcp'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if Path(f'/tmp/.X11-unix/X{fixed}').exists():
                return p, f':{fixed}'
            time.sleep(0.05)
        p.terminate()
        return None, None
    read, write = os.pipe()
    p = subprocess.Popen(['Xvfb', '-displayfd', str(write), '-screen', '0', '800x600x24', '-nolisten', 'tcp'],
                         pass_fds=(write,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write)
    number = b''
    while not number.endswith(b'\n'):
        chunk = os.read(read, 16)
        if not chunk:
            break
        number += chunk
    os.close(read)
    if not number.strip():
        p.terminate()
        return None, None
    return p, ':' + number.decode().strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--themes', type=Path, required=True)
    ap.add_argument('--fltk')
    ap.add_argument('--gtk3')
    ap.add_argument('--gtk4')
    a = ap.parse_args()
    files, failures = check_motion(a.themes) if a.themes.is_dir() else (0, [])
    if not files:
        print('no built GTK themes: skipped')
        return 77
    print(f'motion: {files} style sheets checked')
    probes = [(k, v) for k, v in (('gtk3', a.gtk3), ('gtk4', a.gtk4)) if v and Path(v).exists()]
    server, display = xvfb() if probes or a.fltk else (None, None)
    if server is None:
        print('Xvfb not available: indicators not drawn')
    else:
        try:
            with tempfile.TemporaryDirectory(prefix='sde-indicators-') as tmp:
                tmp = Path(tmp)
                (tmp / 'data').mkdir()
                (tmp / 'data/themes').symlink_to(a.themes.resolve())
                env = dict(os.environ, DISPLAY=display, HOME=str(tmp), XDG_DATA_HOME=str(tmp / 'data'),
                           XDG_CONFIG_HOME=str(tmp / 'config'), XDG_CACHE_HOME=str(tmp / 'cache'),
                           XDG_RUNTIME_DIR=str(tmp), GSETTINGS_BACKEND='memory', NO_AT_BRIDGE='1',
                           GTK_A11Y='none', GSK_RENDERER='cairo', GDK_BACKEND='x11', LC_ALL='C.UTF-8')
                env.pop('WAYLAND_DISPLAY', None)
                for theme in THEMES:
                    if not (a.themes / theme).is_dir():
                        continue
                    for kind, probe in probes:
                        out = tmp / f'{kind}-{theme}'
                        out.mkdir()
                        r = subprocess.run([probe, str(out)], env=dict(env, GTK_THEME=theme), timeout=60,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                        if r.returncode != 0:
                            failures.append(f'{kind} {theme}: the helper failed: {r.stdout.strip()[-300:]}')
                            continue
                        failures += check_indicators(out, f'{kind} {theme}', ['check-off', 'check-on', 'radio-off',
                                                                              'radio-on'], lenient=kind == 'gtk4')
                if a.fltk:
                    for family in ('graphite', 'fluent', 'glass'):
                        for scale in ('100', '150'):
                            out = tmp / f'fltk-{family}-{scale}'
                            out.mkdir()
                            r = subprocess.run([a.fltk, str(out), family, 'light', scale], env=env, timeout=60,
                                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                            if r.returncode != 0:
                                failures.append(f'fltk {family} {scale}: the helper failed: {r.stdout.strip()[-300:]}')
                                continue
                            failures += check_indicators(out, f'fltk {family} {scale} %',
                                                         ['check-off', 'check-on', 'radio-off', 'radio-on'])
        finally:
            server.terminate()
            server.wait(timeout=10)
    for f in failures:
        print('FAIL', f)
    if failures:
        print(f'{len(failures)} failure(s)')
        return 1
    print('theme motion and indicators passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
