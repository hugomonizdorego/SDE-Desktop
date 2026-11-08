#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Liria, SDE's file manager (src/liria, docs/LIRIA.md), on a private Xvfb,
driven with xdotool.

  test_liria_ui.py BUILD/src/liria/liria

SDE_PANEL_QA_DIR makes Liria describe its active window in Liria.txt
("@location:", "@selected:", "@menu:" ... and its named parts in window
pixels); the test clicks there and checks, in a temporary home:

- the Home page: Quick access holds exactly the six default pins (and the
  pin file is not written), Drives, Network locations and Recent files;
- opening a pinned folder from its card; selecting with clicks, Shift and
  Ctrl+A, and with the keyboard;
- the context menu of a Debian package (Install with Debrun) and of a
  text file (Open with), Properties chosen from it;
- copying a 30 MB file with Ctrl+C and Ctrl+V into a subfolder, slowed
  down (SDE_QA_COPY_RATE) so that the copy dialog with the penguin shows;
- tabs (Ctrl+T, Ctrl+Tab, the + button, Ctrl+W); Back, Forward and Up
  (Alt+Left/Right/Up, Backspace, the buttons); the address bar (Ctrl+L);
- F2 rename, Delete to the Trash and Ctrl+Z back, Ctrl+Shift+N; Ctrl+H;
  the details view sorted by a column; search below the folder (Ctrl+F);
- a folder of 10 000 files, listed completely;
- closing the last tab ends the program.

Exit status 0 passed, 1 failed, 77 skipped (no Xvfb or xdotool).
SDE_TEST_DISPLAY=N picks the X display; SDE_LIRIA_SHOTS=DIR saves
screenshots."""
import os
import re
import shutil
import struct
import subprocess as sp
import sys
import tempfile
import time
import zlib
from pathlib import Path

if not shutil.which('xdotool') or not shutil.which('Xvfb') or not shutil.which('xwininfo'):
    print('xdotool, xwininfo and Xvfb are required; skipped')
    sys.exit(77)

LIRIA = os.path.abspath(sys.argv[1])
SHOTS = os.environ.get('SDE_LIRIA_SHOTS')
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print('FAIL:', what, flush=True)
    return cond


def png(path, w, h, color):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    row = b'\x00' + bytes(color) * w
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(row * h)) + chunk(b'IEND', b''))


def make_home(home):
    dirs = (('DESKTOP', 'Desktop'), ('DOCUMENTS', 'Documents'), ('DOWNLOAD', 'Downloads'), ('MUSIC', 'Music'),
            ('PICTURES', 'Pictures'), ('VIDEOS', 'Videos'), ('TEMPLATES', 'Templates'))
    for _, d in dirs:
        (home / d).mkdir(parents=True, exist_ok=True)
    (home / '.config/sde').mkdir(parents=True, exist_ok=True)
    (home / '.config/user-dirs.dirs').write_text(''.join(f'XDG_{k}_DIR="$HOME/{v}"\n' for k, v in dirs))
    (home / '.config/sde/sde.conf').write_text('[appearance]\nscale=100\nicon_theme=auto\n')
    (home / '.local/share').mkdir(parents=True, exist_ok=True)
    docs = home / 'Documents'
    (docs / 'Projects/Sub').mkdir(parents=True)
    (docs / 'Projects/Sub/deep-notes.txt').write_text('deep\n')
    (docs / 'Notes.txt').write_text('notes\n')
    (docs / 'Report.pdf').write_bytes(b'%PDF-1.4\n%%EOF\n')
    (docs / 'Budget.ods').write_bytes(b'PK\x03\x04' + b'\0' * 2000)
    (docs / 'setup.deb').write_bytes(b'!<arch>\ndebian-binary   ')
    with open(docs / 'big.bin', 'wb') as f:
        block = bytes(range(256)) * 4096
        for _ in range(30):
            f.write(block)
    png(home / 'Pictures/lake.png', 320, 200, (40, 160, 120))
    png(home / 'Pictures/sunset.png', 200, 320, (220, 120, 60))
    many = home / 'Many'
    many.mkdir()
    for i in range(10000):
        (many / f'file-{i:05d}.txt').touch()


class Session:
    def __init__(self, temp):
        self.temp = Path(temp)
        self.home = self.temp / 'home'
        self.home.mkdir()
        make_home(self.home)
        self.qa = self.temp / 'qa'
        self.qa.mkdir()
        self.procs = []
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        for attempt in range(1 if fixed else 10):
            number = int(fixed) if fixed else 600 + (os.getpid() + attempt * 41) % 20000
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1366x768x24', '-nolisten', 'tcp'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(self.home),
                            XDG_CONFIG_HOME=str(self.home / '.config'), XDG_DATA_HOME=str(self.home / '.local/share'),
                            XDG_CACHE_HOME=str(self.home / '.cache'), XDG_STATE_HOME=str(self.home / '.local/state'),
                            XDG_RUNTIME_DIR=str(self.temp / 'run'), XDG_CURRENT_DESKTOP='SDE',
                            SDE_PANEL_QA_DIR=str(self.qa), LANG='C.UTF-8', LC_ALL='C.UTF-8', LIRIA_TIMING='1',
                            SDE_QA_COPY_RATE='20000000', GIO_USE_VOLUME_MONITOR='unix', GIO_USE_VFS='local',
                            # Alone: never the Liria of the session running the tests.
                            DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-liria-ui-test')
            (self.temp / 'run').mkdir(mode=0o700, exist_ok=True)
            for _ in range(60):
                if server.poll() is not None or self.xdo('getmouselocation').returncode == 0:
                    break
                time.sleep(0.05)
            if server.poll() is None and self.xdo('getmouselocation').returncode == 0:
                self.procs.append(server)
                break
            server.kill()
        else:
            raise RuntimeError('no Xvfb display')
        if shutil.which('openbox'):
            self.procs.append(sp.Popen(['openbox'], env=self.env, stdout=sp.DEVNULL, stderr=sp.DEVNULL))
            time.sleep(0.5)
        self.proc = None
        self.log = open(self.temp / 'liria.log', 'w')

    def xdo(self, *a):
        return sp.run(['xdotool'] + [str(x) for x in a], env=self.env, capture_output=True, text=True)

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        for p in reversed(self.procs):
            p.terminate()
            try:
                p.wait(timeout=5)
            except sp.TimeoutExpired:
                p.kill()
        self.log.close()

    def start(self, *args):
        self.proc = sp.Popen([LIRIA] + [str(a) for a in args], env=self.env, stdout=self.log, stderr=sp.STDOUT)
        if not self.wait(lambda: self.state('xid') and self.state('loading') == '0', 20):
            raise RuntimeError('Liria did not open: %s' % self.proc.poll())
        self.xdo('windowfocus', '--sync', self.state('xid'))
        time.sleep(0.3)

    # ------------------------------------------------------------ the QA description

    def read(self):
        states, controls = {}, {}
        try:
            text = (self.qa / 'Liria.txt').read_text()
        except OSError:
            return states, controls
        for line in text.splitlines():
            f = line.split('\t')
            if f[0].startswith('@'):
                k, _, v = f[0][1:].partition(':')
                states[k] = v
            elif len(f) >= 6:
                controls.setdefault(f[0], [int(v) for v in f[1:6]])
        return states, controls

    def state(self, key):
        return self.read()[0].get(key)

    def controls(self):
        return self.read()[1]

    def wait(self, fn, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if fn():
                    return True
            except Exception:
                pass
            time.sleep(0.05)
        return False

    def expect(self, key, value, what, timeout=8.0):
        ok = self.wait(lambda: self.state(key) == value, timeout)
        return check(ok, f'{what}: @{key} is {self.state(key)!r}, expected {value!r}')

    def origin(self):
        info = sp.run(['xwininfo', '-id', self.state('xid')], env=self.env, capture_output=True, text=True).stdout
        return (int(re.search(r'Absolute upper-left X:\s*(-?\d+)', info).group(1)),
                int(re.search(r'Absolute upper-left Y:\s*(-?\d+)', info).group(1)))

    def point(self, name, dx=0.5, dy=0.5):
        if not self.wait(lambda: name in self.controls(), 5):
            raise RuntimeError(f'no control {name!r} in {sorted(self.controls())[:60]}')
        r = self.controls()[name]
        x, y = self.origin()
        return x + r[0] + int(r[2] * dx), y + r[1] + int(r[3] * dy)

    def click(self, name, button=1, double=False, mods=None, dx=0.5, dy=0.5):
        x, y = self.point(name, dx, dy)
        self.xdo('mousemove', x, y)
        time.sleep(0.12)
        for m in mods or []:
            self.xdo('keydown', m)
        self.xdo('click', '--repeat', 2 if double else 1, '--delay', 90, button)
        for m in mods or []:
            self.xdo('keyup', m)
        time.sleep(0.35)

    def key(self, *keys):
        for k in keys:
            self.xdo('key', '--delay', 40, k)
            time.sleep(0.2)

    def type(self, text):
        self.xdo('type', '--delay', 25, text)
        time.sleep(0.3)

    def window_named(self, pattern):
        out = self.xdo('search', '--onlyvisible', '--name', pattern).stdout.split()
        return out[-1] if out else None

    def window_of_class(self, cls):
        out = self.xdo('search', '--onlyvisible', '--class', cls).stdout.split()
        out += self.xdo('search', '--onlyvisible', '--classname', cls).stdout.split()
        return out[-1] if out else None

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            os.makedirs(SHOTS, exist_ok=True)
            sp.run(['import', '-window', 'root', os.path.join(SHOTS, name + '.png')], env=self.env,
                   capture_output=True)


def items(s):
    return sorted(k[5:] for k in s.controls() if k.startswith('item:'))


def run(s):
    home, docs = s.home, s.home / 'Documents'
    pins_file = home / '.config/sde/liria-quick-access'

    # ---------------------------------------------------------------- Home
    s.start()
    st = s.read()[0]
    print('first frame', st.get('first-frame-ms'), 'ms', flush=True)
    check(st.get('location') == 'liria:home', f'starts on the Home page, not {st.get("location")!r}')
    pins = [k.split(':', 3)[3] for k in s.controls() if k.startswith('card:quick:pin:')]
    check(pins == ['Desktop', 'Downloads', 'Documents', 'Pictures', 'Music', 'Videos'],
          f'Quick access shows the six default pins in order, not {pins}')
    for section in ('quick', 'drives', 'network', 'recent'):
        check(any(k.startswith(f'card:{section}:header:') for k in s.controls()), f'the Home page has {section}')
    check('place:home' in s.controls() and 'place:network' in s.controls(), 'the sidebar has Home and Network')
    check(not any(k.startswith('heading:network') for k in s.controls()), 'Network is a place, not a heading')
    check(not pins_file.exists(), 'Liria pins nothing by itself (no pin file written)')
    check(s.state('csd') == '1', 'the tabs are in the title bar')
    s.shot('liria-home')

    # ---------------------------------------------------------------- open, select
    s.click('card:quick:pin:Documents', double=True)
    s.expect('location', str(docs), 'Documents opens from its card')
    check(s.wait(lambda: s.state('items') == '6'), f'Documents shows 6 items, not {s.state("items")}')
    s.click('item:Notes.txt')
    s.expect('selected', 'Notes.txt', 'a click selects')
    s.click('item:big.bin')
    s.click('item:Notes.txt', mods=['shift'])
    s.expect('selected', 'big.bin|Budget.ods|Notes.txt', 'Shift+click selects the range')
    s.click('item:setup.deb', mods=['ctrl'])
    s.expect('selected', 'big.bin|Budget.ods|Notes.txt|setup.deb', 'Ctrl+click adds')
    s.key('ctrl+a')
    check(s.wait(lambda: len(s.state('selected').split('|')) == 6), 'Ctrl+A selects everything')
    s.key('End')
    s.expect('selected', 'setup.deb', 'End selects the last item')
    s.key('Home')
    s.expect('selected', 'Projects', 'Home selects the first item (folders first)')
    s.key('Right')
    s.expect('selected', 'big.bin', 'an arrow key moves the selection')

    # ---------------------------------------------------------------- context menus
    s.click('item:setup.deb', button=3)
    check(s.wait(lambda: 'Install with Debrun' in s.state('menu')), f'a package offers Debrun: {s.state("menu")!r}')
    s.shot('liria-menu')
    s.key('Escape')
    s.expect('menu', '', 'Escape closes the menu')
    s.click('item:Notes.txt', button=3)
    check(s.wait(lambda: 'Open with >' in s.state('menu') and s.state('menu').endswith('Properties')),
          f'a text file offers Open with, Properties last: {s.state("menu")!r}')
    check('Install with Debrun' not in s.state('menu'), 'Debrun only for packages')
    s.key('Up', 'Return')
    check(s.wait(lambda: s.window_named('Notes.txt Properties')), 'Properties opens from the menu')
    s.shot('liria-properties')
    s.key('Escape')
    check(s.wait(lambda: not s.window_named('Notes.txt Properties')), 'Escape closes Properties')
    s.click('files', button=3, dx=0.9, dy=0.92)
    check(s.wait(lambda: 'Paste' in s.state('menu') and 'New >' in s.state('menu')),
          f'the folder\'s own menu: {s.state("menu")!r}')
    s.key('Escape')
    s.expect('menu', '', 'Escape closes the folder menu')

    # ---------------------------------------------------------------- copy with the penguin
    s.click('item:big.bin')
    s.key('ctrl+c')
    s.expect('clipboard', 'copy:1', 'Ctrl+C copies')
    s.click('item:Projects', double=True)
    s.expect('location', str(docs / 'Projects'), 'a double click opens a folder')
    s.key('ctrl+v')
    check(s.wait(lambda: s.window_of_class('sde-transfer'), 4), 'the copy dialog shows while copying')
    s.shot('liria-copy')
    check(s.wait(lambda: s.state('jobs') == '0' and (docs / 'Projects/big.bin').exists(), 20), 'the copy ends')
    check((docs / 'Projects/big.bin').stat().st_size == (docs / 'big.bin').stat().st_size, 'the copy is complete')
    check((docs / 'big.bin').exists(), 'a copy keeps the original')
    s.expect('selected', 'big.bin', 'the pasted file is selected')
    # The copy dialog took the keyboard, as Windows' does: back to Liria.
    s.click('item:big.bin')

    # ---------------------------------------------------------------- tabs
    s.key('ctrl+t')
    s.expect('tabs', '2', 'Ctrl+T opens a tab')
    s.expect('location', 'liria:home', 'a new tab shows Home')
    s.key('ctrl+Tab')
    s.expect('location', str(docs / 'Projects'), 'Ctrl+Tab goes to the next tab')
    s.expect('tab', '0', 'the first one')
    s.key('ctrl+shift+Tab')
    s.expect('location', 'liria:home', 'Ctrl+Shift+Tab goes to the previous tab')
    s.key('ctrl+w')
    s.expect('tabs', '1', 'Ctrl+W closes the tab')
    s.expect('location', str(docs / 'Projects'), 'the other tab remains')
    s.click('new-tab')
    s.expect('tabs', '2', 'the + button opens a tab')
    s.shot('liria-tabs')
    s.key('ctrl+w')
    s.expect('tabs', '1', 'and Ctrl+W closes it')
    s.expect('location', str(docs / 'Projects'), 'the remaining tab')

    # ---------------------------------------------------------------- navigation
    s.key('alt+Up')
    s.expect('location', str(docs), 'Alt+Up goes up')
    s.key('alt+Left')
    s.expect('location', str(docs / 'Projects'), 'Alt+Left goes back')
    s.key('alt+Right')
    s.expect('location', str(docs), 'Alt+Right goes forward')
    s.key('BackSpace')
    s.expect('location', str(docs / 'Projects'), 'Backspace goes back')
    s.click('back')
    s.expect('location', str(docs), 'the Back button')
    s.click('forward')
    s.expect('location', str(docs / 'Projects'), 'the Forward button')
    s.click('up')
    s.expect('location', str(docs), 'the Up button')
    s.click('crumb:0:home')
    s.expect('location', str(home), 'a breadcrumb')
    s.key('ctrl+l')
    s.expect('editing', '1', 'Ctrl+L edits the address')
    s.key('ctrl+a')
    s.type(str(docs))
    s.key('Return')
    s.expect('location', str(docs), 'a typed path opens')
    s.expect('editing', '0', 'and the address shows breadcrumbs again')

    # ---------------------------------------------------------------- rename, trash, undo, new folder
    s.click('item:Notes.txt')
    s.key('F2')
    s.expect('renaming', str(docs / 'Notes.txt'), 'F2 renames')
    s.key('ctrl+a')
    s.type('Ideas.txt')
    s.key('Return')
    check(s.wait(lambda: (docs / 'Ideas.txt').exists() and not (docs / 'Notes.txt').exists()), 'the file is renamed')
    s.expect('selected', 'Ideas.txt', 'the renamed file stays selected')
    s.key('Delete')
    trashed = home / '.local/share/Trash/files/Ideas.txt'
    check(s.wait(lambda: trashed.exists() and not (docs / 'Ideas.txt').exists()), 'Delete moves it to the Trash')
    info = home / '.local/share/Trash/info/Ideas.txt.trashinfo'
    check(info.exists() and f'Path={docs}/Ideas.txt' in info.read_text(), 'with its .trashinfo')
    s.key('ctrl+z')
    check(s.wait(lambda: (docs / 'Ideas.txt').exists() and not trashed.exists()), 'Ctrl+Z brings it back')
    s.key('ctrl+shift+n')
    check(s.wait(lambda: (docs / 'New folder').is_dir()), 'Ctrl+Shift+N makes a folder')
    s.expect('renaming', str(docs / 'New folder'), 'and starts renaming it')
    s.type('Work')
    s.key('Return')
    check(s.wait(lambda: (docs / 'Work').is_dir() and not (docs / 'New folder').exists()), 'the new folder is named')

    # ---------------------------------------------------------------- views, hidden, search
    s.key('ctrl+shift+6')
    s.expect('layout', 'details', 'Ctrl+Shift+6 shows details')
    s.click('column:size')
    check(s.wait(lambda: s.state('sort').startswith('size:')), f'a column sorts: {s.state("sort")}')
    s.shot('liria-details')
    s.key('ctrl+shift+2')
    s.expect('layout', 'icons', 'Ctrl+Shift+2 shows large icons')
    s.expect('icon-size', '128', 'at 128 pixels')
    (docs / '.hidden-file').write_text('h')
    s.key('ctrl+h')
    s.expect('hidden', '1', 'Ctrl+H shows hidden items')
    check(s.wait(lambda: 'item:.hidden-file' in s.controls()), 'the hidden file is listed')
    s.key('ctrl+h')
    s.expect('hidden', '0', 'Ctrl+H hides them again')
    s.key('ctrl+f')
    s.type('deep')
    check(s.wait(lambda: s.state('searching') == '0' and items(s) == ['deep-notes.txt'], 10),
          f'the search finds the file in a subfolder: {items(s)}')
    s.key('Escape')

    # ---------------------------------------------------------------- 10 000 files, Home
    s.key('ctrl+l', 'ctrl+a')
    s.type(str(home / 'Many'))
    s.key('Return')
    check(s.wait(lambda: s.state('location') == str(home / 'Many') and s.state('loading') == '0' and
                 s.state('items') == '10000', 30), f'10 000 files are listed: {s.state("items")}')
    print('10 000 files loaded in', s.state('loaded-ms'), 'ms', flush=True)
    s.key('End')
    s.expect('selected', 'file-09999.txt', 'End reaches the last of 10 000 files')
    s.click('place:home')
    s.expect('location', 'liria:home', 'Home in the sidebar')
    s.key('ctrl+w')
    check(s.wait(lambda: not s.window_of_class('liria'), 10), 'closing the last tab closes the window')
    # A Liria that has files on the clipboard keeps running without windows
    # (other programs may still paste them); one that has none ends.
    check(s.proc.poll() is None, 'Liria keeps the copied files for other programs')
    s.proc.terminate()
    s.proc.wait(10)
    (s.qa / 'Liria.txt').unlink()
    s.start(docs)
    s.expect('location', str(docs), 'Liria opens the folder given to it')
    s.key('ctrl+w')
    check(s.wait(lambda: s.proc.poll() is not None, 10), 'closing the last tab ends Liria')
    check(s.proc.poll() == 0, f'Liria exits with 0, not {s.proc.poll()}')


def main():
    with tempfile.TemporaryDirectory(prefix='liria-ui-') as temp:
        s = Session(temp)
        try:
            run(s)
        except Exception as e:  # a missing control: report it as a failure
            check(False, f'{type(e).__name__}: {e}')
        finally:
            s.close()
            log = (Path(temp) / 'liria.log').read_text(errors='replace')
            if failures and log.strip():
                print('--- liria output ---\n' + log[-4000:])
    if failures:
        print(f'{len(failures)} check(s) failed')
        return 1
    print('liria-ui: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
