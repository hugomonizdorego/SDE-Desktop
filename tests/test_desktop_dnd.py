#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drag and drop on sde-desktop, on a private Xvfb with xdotool.

test_desktop_dnd.py SDE_DESKTOP XDND_PEER

 1. an icon dragged to an empty spot stays there (desktop-positions.ini)
    and keeps its place after a rename;
 2. a file dragged onto a folder icon is moved into the folder;
 3. a file dragged onto the Trash icon goes to the home trash;
 4. a rubber band selects two icons, which are dragged together;
 5. XDND target: a text/uri-list drop from another program (xdnd-peer) is
    moved onto the desktop and reported as XdndActionMove; plain text
    becomes "Dropped text.txt"; a link becomes a .desktop link;
 6. XDND source: an icon dragged onto another program's window arrives as
    a file:// URI list, and the file stays (the peer copies).
SDE_DND_SCREENSHOTS=DIR saves screenshots."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

xdotool = shutil.which('xdotool')
if not xdotool or not shutil.which('Xvfb') or len(sys.argv) < 3:
    print('xdotool, Xvfb and the program paths are required; skipped')
    sys.exit(77)
desktop_bin, peer_bin = sys.argv[1], sys.argv[2]
shots = os.environ.get('SDE_DND_SCREENSHOTS')
failures = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        failures.append(what)


def xdo(env, *args):
    return sp.run([xdotool] + [str(a) for a in args], env=env, capture_output=True, text=True)


def shot(env, name):
    if shots and shutil.which('import'):
        Path(shots).mkdir(parents=True, exist_ok=True)
        sp.run(['import', '-window', 'root', str(Path(shots) / name)], env=env, check=False)


def wait_for(cond, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.1)
    return cond()


def layout(path):
    out = {}
    try:
        for line in Path(path).read_text().splitlines():
            key, x, y, w, h = line.split('\t')
            out[key] = (int(x), int(y), int(w), int(h))
    except (OSError, ValueError):
        pass
    return out


def center(rect):
    x, y, w, h = rect
    return x + w // 2, y + h // 3


def drag(env, start, end, steps=12, mods=None):
    xdo(env, 'mousemove', start[0], start[1])
    time.sleep(0.15)
    if mods:
        xdo(env, 'keydown', mods)
    xdo(env, 'mousedown', 1)
    time.sleep(0.1)
    for i in range(1, steps + 1):
        x = start[0] + (end[0] - start[0]) * i // steps
        y = start[1] + (end[1] - start[1]) * i // steps
        xdo(env, 'mousemove', x, y)
        time.sleep(0.04)
    time.sleep(0.25)
    xdo(env, 'mouseup', 1)
    if mods:
        xdo(env, 'keyup', mods)
    time.sleep(0.4)


with tempfile.TemporaryDirectory(prefix='sde-desktop-dnd-') as temp:
    home = Path(temp)
    desk = home / 'Desktop'
    (desk / 'Folder A').mkdir(parents=True)
    for name in ['move-me.txt', 'note.txt', 'trash-me.txt', 'pair-1.txt', 'pair-2.txt']:
        (desk / name).write_text(name + '\n')
    config = home / 'config'
    (config / 'sde').mkdir(parents=True)
    (config / 'sde' / 'sde.conf').write_text(
        '[desktop]\nshow_trash=true\nshow_home=true\nicon_size=medium\n'
        '[appearance]\nscale=100\n')
    elsewhere = home / 'elsewhere'
    elsewhere.mkdir()
    (elsewhere / 'from-peer.txt').write_text('peer\n')
    stubs = home / 'bin'
    stubs.mkdir()
    for prog in ['xdg-open', 'gio', 'liria']:
        (stubs / prog).write_text('#!/bin/sh\nexit 0\n')
        (stubs / prog).chmod(0o755)
    layout_file = home / 'layout.tsv'
    number = 300 + os.getpid() % 20000
    server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1024x768x24', '-nolisten', 'tcp'],
                      stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    env = dict(os.environ, DISPLAY=':%d' % number, HOME=temp, XDG_CONFIG_HOME=str(config),
               XDG_DATA_HOME=str(home / 'data'), XDG_RUNTIME_DIR=temp, LANG='C.UTF-8', LC_ALL='C.UTF-8',
               SDE_DESKTOP_LAYOUT_FILE=str(layout_file), SDE_SCALE='1',
               PATH='%s:%s' % (stubs, os.environ.get('PATH', '/usr/bin:/bin')))
    env.pop('XDG_CONFIG_DIRS', None)
    proc = None
    log = open(home / 'desktop.log', 'w+')
    try:
        for _ in range(100):
            if xdo(env, 'getmouselocation').returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('Xvfb did not start')
        proc = sp.Popen([desktop_bin], env=env, stdout=log, stderr=sp.STDOUT)
        check(wait_for(lambda: 'note.txt' in layout(layout_file), 15), 'desktop shows its icons')
        time.sleep(1.0)
        lay = layout(layout_file)
        print('layout:', lay)
        w, h = lay['note.txt'][2], lay['note.txt'][3]
        shot(env, 'dnd-0-start.png')

        # 1. Free placement.
        target = (lay['note.txt'][0] + 4 * w, lay['note.txt'][1] + h)
        drag(env, center(lay['note.txt']), (target[0] + w // 2, target[1] + h // 3))
        lay2 = layout(layout_file)
        check(lay2.get('note.txt', (0, 0))[:2] == target, 'icon moved to the empty cell (%s -> %s)'
              % (lay['note.txt'][:2], lay2.get('note.txt')))
        positions = (config / 'sde' / 'desktop-positions.ini').read_text() if (
            config / 'sde' / 'desktop-positions.ini').exists() else ''
        check('note.txt=' in positions, 'position stored in desktop-positions.ini')
        shot(env, 'dnd-1-moved.png')
        os.rename(desk / 'note.txt', desk / 'renamed.txt')
        check(wait_for(lambda: 'renamed.txt' in layout(layout_file)), 'renamed file shown')
        check(layout(layout_file).get('renamed.txt', (0, 0))[:2] == target, 'renamed icon keeps its place')

        # 2. Onto a folder: moved.
        lay = layout(layout_file)
        drag(env, center(lay['move-me.txt']), center(lay['Folder A']))
        check(wait_for(lambda: (desk / 'Folder A' / 'move-me.txt').exists()), 'file moved into the folder')
        check(not (desk / 'move-me.txt').exists(), 'file gone from the desktop')
        # Ctrl copies instead.
        (desk / 'copy-me.txt').write_text('copy\n')
        check(wait_for(lambda: 'copy-me.txt' in layout(layout_file)), 'new file shown')
        lay = layout(layout_file)
        drag(env, center(lay['copy-me.txt']), center(lay['Folder A']), mods='ctrl')
        check(wait_for(lambda: (desk / 'Folder A' / 'copy-me.txt').exists()), 'Ctrl+drop copied into the folder')
        check((desk / 'copy-me.txt').exists(), 'Ctrl+drop kept the original')

        # 3. Onto the Trash.
        lay = layout(layout_file)
        drag(env, center(lay['trash-me.txt']), center(lay['@trash']))
        trashed = home / 'data' / 'Trash' / 'files' / 'trash-me.txt'
        check(wait_for(lambda: trashed.exists()), 'file moved to the trash')
        check((home / 'data' / 'Trash' / 'info' / 'trash-me.txt.trashinfo').exists(), 'trash info written')

        # 4. Rubber band around pair-1 and pair-2, then drag both.
        lay = layout(layout_file)
        p1, p2 = lay['pair-1.txt'], lay['pair-2.txt']
        x0 = min(p1[0], p2[0]) + w + 6
        y0 = min(p1[1], p2[1]) - 2 if min(p1[1], p2[1]) > 4 else 2
        x1 = min(p1[0], p2[0]) + w // 2
        y1 = max(p1[1], p2[1]) + h // 2
        drag(env, (x0, y0 + 3), (x1, y1), steps=8)
        shot(env, 'dnd-4-band.png')
        dest = (lay['renamed.txt'][0] + w, lay['renamed.txt'][1] - 2 * h)
        drag(env, center(p1), (dest[0] + w // 2, dest[1] + h // 3))
        lay2 = layout(layout_file)
        check(lay2.get('pair-1.txt', (0, 0))[:2] == dest, 'first selected icon at the drop cell')
        moved2 = lay2.get('pair-2.txt', (0, 0))[:2]
        check(moved2 != p2[:2] and moved2[0] == dest[0] + (p2[0] - p1[0]),
              'second selected icon moved with it (%s -> %s)' % (p2[:2], moved2))
        shot(env, 'dnd-4-multi.png')

        # 5. XDND target.
        wid = xdo(env, 'search', '--classname', 'sde-desktop').stdout.split()
        check(bool(wid), 'desktop window found')
        wid = wid[0] if wid else '0'
        empty = (lay2['renamed.txt'][0] + w * 2 + w // 2, lay2['renamed.txt'][1] - h * 3 + h // 2)
        uri = 'file://' + str(elsewhere / 'from-peer.txt').replace(' ', '%20')
        r = sp.run([peer_bin, 'source', wid, str(empty[0]), str(empty[1]), 'text/uri-list', uri + '\\n'],
                   env=env, capture_output=True, text=True, timeout=30)
        print(r.stdout.strip())
        check('status 1' in r.stdout, 'desktop accepts a uri-list drop')
        check('finished 1 XdndActionMove' in r.stdout, 'drop finished as a move (same file system)')
        check(wait_for(lambda: (desk / 'from-peer.txt').exists()), 'dropped file on the desktop')
        check(not (elsewhere / 'from-peer.txt').exists(), 'dropped file moved away from its folder')
        check(wait_for(lambda: 'from-peer.txt' in layout(layout_file)), 'dropped file shown')
        dropped = layout(layout_file).get('from-peer.txt', (0, 0))
        check(abs(dropped[0] + w // 2 - empty[0]) <= w and abs(dropped[1] + h // 2 - empty[1]) <= h,
              'dropped file placed where it was dropped (%s near %s)' % (dropped[:2], empty))
        r = sp.run([peer_bin, 'source', wid, str(empty[0] + w), str(empty[1]), 'UTF8_STRING',
                    'Some dropped words'], env=env, capture_output=True, text=True, timeout=30)
        print(r.stdout.strip())
        check('status 1 XdndActionCopy' in r.stdout and 'finished 1 XdndActionCopy' in r.stdout,
              'text drop accepted and finished as a copy')
        check(wait_for(lambda: (desk / 'Dropped text.txt').exists()), '"Dropped text.txt" created')
        if (desk / 'Dropped text.txt').exists():
            check((desk / 'Dropped text.txt').read_text() == 'Some dropped words', 'dropped text saved')
        r = sp.run([peer_bin, 'source', wid, str(empty[0] + 2 * w), str(empty[1]), 'text/uri-list',
                    'https://www.debian.org/intro/\\n'], env=env, capture_output=True, text=True, timeout=30)
        check(wait_for(lambda: (desk / 'www.debian.org.desktop').exists()), 'link drop made a .desktop link')
        if (desk / 'www.debian.org.desktop').exists():
            check('URL=https://www.debian.org/intro/' in (desk / 'www.debian.org.desktop').read_text(),
                  'link points to the URL')
        shot(env, 'dnd-5-dropped.png')

        # 6. XDND source: drag an icon onto another program's window.
        peer = sp.Popen([peer_bin, 'target', '700', '300', '300', '300'], env=env, stdout=sp.PIPE, text=True)
        ready = peer.stdout.readline()
        check(ready.startswith('ready'), 'peer target window mapped')
        time.sleep(0.3)
        lay = layout(layout_file)
        drag(env, center(lay['renamed.txt']), (850, 450), steps=16)
        try:
            out, _ = peer.communicate(timeout=10)
        except sp.TimeoutExpired:
            peer.kill()
            out, _ = peer.communicate()
        print(out.strip())
        check('enter version=5 first-type=text/uri-list' in out, 'desktop offers text/uri-list (XDND 5)')
        expect = 'file://' + str(desk / 'renamed.txt').replace(' ', '%20') + '|'
        check(('drop XdndActionCopy ' + expect) in out, 'peer received the file URI')
        time.sleep(0.5)
        check((desk / 'renamed.txt').exists(), 'copied file stays on the desktop')
        check(proc.poll() is None, 'desktop still running')
        shot(env, 'dnd-6-source.png')
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except sp.TimeoutExpired:
                proc.kill()
        server.terminate()
        server.wait()
        log.seek(0)
        text = log.read()
        if failures and text:
            print('--- sde-desktop log ---\n' + text)

if failures:
    print('%d check(s) failed' % len(failures))
    sys.exit(1)
print('sde-desktop drag and drop tests passed')
