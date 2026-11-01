#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""The SDE file chooser (src/common/file_chooser.h) on a private Xvfb,
driven with xdotool through tests/file_chooser_demo.cpp.

  test_file_chooser_ui.py BUILD/tests/sde-file-chooser-demo [BUILD/tests/sde-xdnd-peer]

SDE_PANEL_QA_DIR makes the dialog describe itself in File-Chooser.txt
("@location:", "@selected:" ... and its named controls in window pixels);
the test clicks there and checks, in a temporary home with pictures,
documents, a hidden file, bookmarks, recent files and a folder of 10 000
files:

- opening: places (Recent, Home, the XDG folders, a bookmark, File
  System), thumbnails made into ~/.cache/thumbnails, double click, Back,
  Forward and Up (Alt+Left/Right/Up, the mouse's back button, Backspace),
  type-ahead, Enter; the chosen file is printed and added to the recent
  files; the next run of the program starts in that folder;
- several files with Ctrl+click; a folder (Select Folder, the current
  folder when nothing is selected) and New Folder;
- saving: the name without its extension selected, typing over it, the
  filter's extension added, the question before a file is replaced, the
  extension following the filter menu;
- the location entry (Ctrl+L, "/" typed in the files) with completion
  (Tab) and Enter; search (Ctrl+F) with results from subfolders; hidden
  files (Ctrl+H); list view sorted by a column; Recent; a file dropped
  from another program (XDND, with sde-xdnd-peer); Escape cancels;
- 10 000 files: the first frame within the limit and every file listed;
- 1024x600 at 150 % with a panel: every button of the dialog in view.

Exit status 0 passed, 1 failed, 77 skipped (no Xvfb or xdotool).
SDE_TEST_DISPLAY=N picks the X display; SDE_FILE_CHOOSER_SHOTS=DIR saves
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

if not shutil.which('xdotool') or not shutil.which('Xvfb'):
    print('xdotool and Xvfb are required; skipped')
    sys.exit(77)

DEMO = os.path.abspath(sys.argv[1])
PEER = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
SHOTS = os.environ.get('SDE_FILE_CHOOSER_SHOTS')
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print('FAIL:', what, flush=True)
    return cond


def png(path, w, h, color):
    """A plain RGB picture."""
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    row = b'\x00' + bytes(color) * w
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(row * h)) + chunk(b'IEND', b''))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data)


def make_home(home):
    for d in ('Desktop', 'Documents', 'Downloads', 'Music', 'Pictures/Nature', 'Videos', 'Projects/Garden'):
        (home / d).mkdir(parents=True, exist_ok=True)
    (home / '.config').mkdir(exist_ok=True)
    (home / '.config/user-dirs.dirs').write_text(''.join(
        f'XDG_{k}_DIR="$HOME/{v}"\n' for k, v in (('DESKTOP', 'Desktop'), ('DOCUMENTS', 'Documents'),
                                                  ('DOWNLOAD', 'Downloads'), ('MUSIC', 'Music'),
                                                  ('PICTURES', 'Pictures'), ('VIDEOS', 'Videos'))))
    (home / '.config/gtk-3.0').mkdir(parents=True, exist_ok=True)
    (home / '.config/gtk-3.0/bookmarks').write_text(f'file://{home}/Projects/Garden Garden\n')
    png(home / 'Pictures/a.png', 320, 200, (200, 80, 60))
    png(home / 'Pictures/b.png', 200, 320, (60, 120, 200))
    png(home / 'Pictures/Nature/lake.png', 300, 200, (40, 160, 120))
    png(home / 'Pictures/Nature/lake shore.png', 300, 200, (40, 100, 160))
    for name, text in (('notes.txt', 'notes'), ('a.txt', 'a'), ('b.txt', 'bb'), ('report.md', '# Report\n'),
                       ('Zebra.txt', 'z' * 3000)):
        (home / 'Documents' / name).write_text(text)
    (home / 'readme.txt').write_text('hello')
    (home / '.secret').write_text('hidden')
    big = home / 'big'
    big.mkdir()
    for i in range(10000):
        (big / f'file-{i:05d}.dat').touch()
    (home / '.local/share').mkdir(parents=True, exist_ok=True)
    t = '2026-10-30T10:00:00Z'
    (home / '.local/share/recently-used.xbel').write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<xbel version="1.0" '
        'xmlns:bookmark="http://www.freedesktop.org/standards/desktop-bookmarks" '
        'xmlns:mime="http://www.freedesktop.org/standards/shared-mime-info">\n'
        f'<bookmark href="file://{home}/Documents/report.md" added="{t}" modified="{t}" visited="{t}"><info>'
        '<metadata owner="http://freedesktop.org"><mime:mime-type type="text/markdown"/><bookmark:applications>'
        f'<bookmark:application name="niny" exec="&apos;niny %u&apos;" modified="{t}" count="1"/>'
        '</bookmark:applications></metadata></info></bookmark>\n</xbel>\n')


class Session:
    def __init__(self, temp, size='1366x768', scale=100, panel=0):
        self.home = Path(temp) / 'home'
        self.home.mkdir(parents=True, exist_ok=True)
        self.qa = Path(temp) / 'qa'
        self.qa.mkdir(exist_ok=True)
        (self.home / '.config/sde').mkdir(parents=True, exist_ok=True)
        (self.home / '.config/sde/sde.conf').write_text(f'[appearance]\nscale={scale}\nicon_theme=auto\n')
        self.w, self.h = (int(v) for v in size.split('x'))
        self.procs = []
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        for attempt in range(1 if fixed else 10):
            number = int(fixed) if fixed else 600 + (os.getpid() + attempt * 41) % 20000
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', size + 'x24', '-nolisten', 'tcp'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(self.home),
                            XDG_CONFIG_HOME=str(self.home / '.config'), XDG_DATA_HOME=str(self.home / '.local/share'),
                            XDG_CACHE_HOME=str(self.home / '.cache'), XDG_STATE_HOME=str(self.home / '.local/state'),
                            XDG_RUNTIME_DIR=str(Path(temp) / 'run'), SDE_PANEL_QA_DIR=str(self.qa),
                            LANG='C.UTF-8', LC_ALL='C.UTF-8', SDE_FILE_CHOOSER_TIMING='1',
                            DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-file-chooser-test')
            (Path(temp) / 'run').mkdir(mode=0o700, exist_ok=True)
            for _ in range(60):
                if server.poll() is not None:
                    break
                if self.xdo('getmouselocation').returncode == 0:
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
        if panel:
            sp.run(['xprop', '-root', '-f', '_NET_WORKAREA', '32c', '-set', '_NET_WORKAREA',
                    '0, 0, %d, %d' % (self.w, self.h - panel)], env=self.env, check=False)
        self.proc = None

    def xdo(self, *a):
        return sp.run(['xdotool'] + [str(x) for x in a], env=self.env, capture_output=True, text=True)

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
        for p in reversed(self.procs):
            p.terminate()
            try:
                p.wait(timeout=5)
            except sp.TimeoutExpired:
                p.kill()

    # ------------------------------------------------------------ the dialog

    def start(self, *args):
        qf = self.qa / 'File-Chooser.txt'
        if qf.exists():
            qf.unlink()
        self.proc = sp.Popen([DEMO] + [str(a) for a in args], env=self.env, stdout=sp.PIPE, stderr=sp.PIPE, text=True)
        if not self.wait(lambda: self.state('loading') == '0' and self.window(), 15):
            raise RuntimeError('the dialog did not open: ' + str(self.proc.poll()))
        self.wid = self.window()
        self.xdo('windowfocus', '--sync', self.wid)
        time.sleep(0.3)

    def finish(self, timeout=10):
        try:
            out, err = self.proc.communicate(timeout=timeout)
        except sp.TimeoutExpired:
            self.proc.kill()
            out, err = self.proc.communicate()
            return None, out + err
        return self.proc.returncode, out

    def window(self):
        out = self.xdo('search', '--onlyvisible', '--class', 'sde-file-chooser').stdout.split()
        return out[-1] if out else None

    def layout(self):
        items = {}
        try:
            text = (self.qa / 'File-Chooser.txt').read_text()
        except FileNotFoundError:
            return items
        for line in text.splitlines():
            f = line.split('\t')
            if len(f) >= 6:
                items.setdefault(f[0], [int(v) for v in f[1:6]])
        return items

    def state(self, key):
        for k in self.layout():
            if k.startswith('@' + key + ':'):
                return k[len(key) + 2:]
        return None

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

    def expect(self, key, value, what, timeout=6.0):
        ok = self.wait(lambda: self.state(key) == value, timeout)
        return check(ok, f'{what}: @{key} is {self.state(key)!r}, expected {value!r}')

    def origin(self):
        info = sp.run(['xwininfo', '-id', self.wid], env=self.env, capture_output=True, text=True).stdout
        x = int(re.search(r'Absolute upper-left X:\s*(-?\d+)', info).group(1))
        y = int(re.search(r'Absolute upper-left Y:\s*(-?\d+)', info).group(1))
        w = int(re.search(r'Width:\s*(\d+)', info).group(1))
        h = int(re.search(r'Height:\s*(\d+)', info).group(1))
        return x, y, w, h

    def point(self, name):
        if not self.wait(lambda: name in self.layout(), 4):
            raise RuntimeError(f'no control {name!r}')
        r = self.layout()[name]
        x, y = self.origin()[:2]
        return x + r[0] + r[2] // 2, y + r[1] + r[3] // 2

    def click(self, name, button=1, double=False, ctrl=False):
        x, y = self.point(name)
        self.xdo('mousemove', x, y)
        time.sleep(0.12)
        args = ['click'] + (['--repeat', '2', '--delay', '90'] if double else []) + [str(button)]
        if ctrl:
            self.xdo('keydown', 'ctrl')
        self.xdo(*args)
        if ctrl:
            self.xdo('keyup', 'ctrl')
        time.sleep(0.25)

    def key(self, *keys):
        self.xdo('key', '--delay', '50', *keys)
        time.sleep(0.25)

    def type(self, text):
        self.xdo('type', '--delay', '40', text)
        time.sleep(0.3)

    def shot(self, name):
        if not SHOTS:
            return
        Path(SHOTS).mkdir(parents=True, exist_ok=True)
        sp.run(['import', '-window', 'root', str(Path(SHOTS) / (name + '.png'))], env=self.env, check=False)


def chosen(out):
    return [line[len('chosen '):] for line in out.splitlines() if line.startswith('chosen ')]


# ================================================================ the tests


def test_open(s):
    h = s.home
    s.start('--mode', 'open', '--folder', h, '--program', 'ui-test-open', '--title', 'Open a File')
    s.expect('location', str(h), 'starts in the folder it was given')
    lay = s.layout()
    for place in ('place:recent', 'place:home', 'place:user:DOCUMENTS', 'place:user:PICTURES',
                  f'place:bookmark:{h}/Projects/Garden', 'place:root'):
        check(place in lay, f'the sidebar has {place}')
    check('hidden' in lay, 'there is a "Show hidden files" check box')
    check('item:readme.txt' in lay and 'item:.secret' not in lay, 'hidden files are hidden')
    check(s.state('accept') == '0', 'Open is disabled while nothing is chosen')
    s.click('item:Pictures', double=True)
    s.expect('location', f'{h}/Pictures', 'a double click opens a folder')
    s.expect('thumbnailing', '0', 'thumbnails are made', 10)
    check(int(s.state('thumbnails') or 0) >= 2, 'thumbnails of the pictures are shown')
    made = list((h / '.cache/thumbnails/normal').glob('*.png')) if (h / '.cache/thumbnails/normal').is_dir() else []
    check(len(made) >= 2, 'the thumbnails are stored in ~/.cache/thumbnails/normal')
    s.shot('open-pictures')
    s.key('alt+Left')
    s.expect('location', str(h), 'Alt+Left goes back')
    s.expect('selected', 'Pictures', 'going back selects the folder one came from')
    s.key('alt+Right')
    s.expect('location', f'{h}/Pictures', 'Alt+Right goes forward')
    s.key('alt+Up')
    s.expect('location', str(h), 'Alt+Up goes up')
    s.expect('selected', 'Pictures', 'going up selects the folder one came from')
    x, y = s.point('files')
    s.xdo('mousemove', x, y)
    s.xdo('click', '8')
    time.sleep(0.3)
    s.expect('location', f'{h}/Pictures', "the mouse's back button goes back")
    s.click('item:a.png')
    s.key('BackSpace')
    s.expect('location', str(h), 'Backspace goes up')
    s.type('Doc')
    s.expect('selected', 'Documents', 'type-ahead selects the first name that starts with what was typed')
    s.key('Return')
    s.expect('location', f'{h}/Documents', 'Enter opens the selected folder')
    s.click('item:notes.txt')
    s.expect('accept', '1', 'Open is enabled for a file')
    s.key('Return')
    code, out = s.finish()
    check(code == 0 and chosen(out) == [f'{h}/Documents/notes.txt'], f'Enter chooses the file ({code}, {out!r})')
    xbel = (h / '.local/share/recently-used.xbel').read_text()
    check(f'file://{h}/Documents/notes.txt' in xbel, 'the chosen file is added to the recent files')
    # The next run of the program starts where it chose last time.
    s.start('--mode', 'open', '--default-folder', h, '--program', 'ui-test-open')
    s.expect('location', f'{h}/Documents', 'the last folder of the program is remembered')
    s.key('Escape')
    code, out = s.finish()
    check(code == 1 and 'cancelled' in out, 'Escape cancels')


def test_multi(s):
    h = s.home
    s.start('--mode', 'multi', '--folder', h / 'Documents', '--program', 'ui-test-multi')
    s.click('item:a.txt')
    s.click('item:b.txt', ctrl=True)
    s.expect('selected', 'a.txt|b.txt', 'Ctrl+click adds a file')
    s.click('accept')
    code, out = s.finish()
    check(chosen(out) == [f'{h}/Documents/a.txt', f'{h}/Documents/b.txt'], f'two files are chosen ({out!r})')


def test_folder(s):
    h = s.home
    s.start('--mode', 'folder', '--folder', h, '--program', 'ui-test-folder')
    lay = s.layout()
    check('item:readme.txt' in lay and lay['item:readme.txt'][4] == 0, 'files are shown but cannot be chosen')
    check('place:recent' not in lay, 'no Recent when choosing a folder')
    s.click('item:Pictures')
    s.click('accept')
    code, out = s.finish()
    check(chosen(out) == [f'{h}/Pictures'], f'the selected folder is chosen ({out!r})')
    s.start('--mode', 'folder', '--folder', h / 'Music', '--program', 'ui-test-folder')
    s.click('new-folder')
    s.expect('newfolder-box', '1', 'New Folder asks for a name')
    s.key('ctrl+a')
    s.type('Album 2026')
    s.key('Return')
    s.expect('selected', 'Album 2026', 'the new folder is made and selected')
    check((h / 'Music/Album 2026').is_dir(), 'the new folder exists')
    s.click('item:Album 2026', double=True)
    s.expect('location', f'{h}/Music/Album 2026', 'into the new folder')
    s.click('accept')
    code, out = s.finish()
    check(chosen(out) == [f'{h}/Music/Album 2026'], f'with nothing selected the folder shown is chosen ({out!r})')


def test_save(s):
    h = s.home
    s.start('--mode', 'save', '--folder', h / 'Documents', '--name', 'report.txt',
            '--filters', 'Text files\\t*.txt\\nMarkdown\\t*.md', '--program', 'ui-test-save')
    s.expect('name', 'report.txt', 'the proposed name')
    s.expect('name-selection', '0-6', 'the name without its extension is selected')
    s.expect('filter', '0:Text files', 'the filter of the name')
    check('item:report.md' not in s.layout() and 'item:notes.txt' in s.layout(), 'the filter shows text files only')
    s.type('notes')
    s.expect('name', 'notes.txt', 'typing replaces the selected part')
    s.key('Return')
    found = s.wait(lambda: s.xdo('search', '--onlyvisible', '--name', 'Replace File').stdout.strip(), 6)
    check(found, 'replacing a file asks first')
    s.shot('save-replace')
    if found:
        s.key('Return')
    code, out = s.finish()
    check(chosen(out) == [f'{h}/Documents/notes.txt'], f'Replace saves over the file ({out!r})')
    # The filter's extension is added, and follows the filter menu.
    s.start('--mode', 'save', '--folder', h / 'Documents', '--name', 'summary',
            '--filters', 'Text files\\t*.txt\\nMarkdown\\t*.md', '--filter', '1', '--program', 'ui-test-save')
    s.expect('name', 'summary', 'a name without an extension')
    s.click('filter')
    time.sleep(0.4)
    s.key('Up')
    s.key('Return')
    s.expect('filter', '0:Text files', 'the filter menu')
    s.expect('name', 'summary.txt', "the name gets the filter's extension")
    s.click('filter')
    time.sleep(0.4)
    s.key('Down')
    s.key('Return')
    s.expect('name', 'summary.md', 'another filter replaces the extension')
    s.key('ctrl+a')
    s.type('plan')
    s.key('Return')
    code, out = s.finish()
    check(chosen(out) == [f'{h}/Documents/plan.md'], f"the filter's extension is added ({out!r})")


def test_location_search_hidden(s):
    h = s.home
    s.start('--mode', 'open', '--folder', h, '--program', 'ui-test-location')
    s.key('ctrl+l')
    s.expect('editing', '1', 'Ctrl+L opens the location entry')
    s.expect('entry', str(h), 'the location entry shows the folder')
    s.key('ctrl+a')
    s.type('~/Doc')
    s.expect('completions', '1', 'completion offers ~/Documents/')
    s.key('Tab')
    s.expect('entry', '~/Documents/', 'Tab completes')
    s.key('Return')
    s.expect('location', f'{h}/Documents', 'Enter goes there')
    s.expect('editing', '0', 'the path bar shows the folders again')
    check(any(k.startswith('crumb:') and k.endswith(':Documents') for k in s.layout()), 'the path bar has Documents')
    home_crumb = next((k for k in s.layout() if k.startswith('crumb:') and k.endswith(':Home')), None)
    check(home_crumb is not None, 'the path bar starts at Home')
    if home_crumb:
        s.click(home_crumb)
        s.expect('location', str(h), 'a folder of the path bar can be clicked')
    s.click('files')
    s.type('/')
    s.expect('editing', '1', 'typing "/" opens the location entry')
    s.expect('entry', '/', 'with the "/"')
    s.key('Escape')
    s.expect('completions', '0', 'Escape closes the completion first')
    s.expect('editing', '1', 'and keeps the location entry')
    s.key('Escape')
    s.expect('editing', '0', 'a second Escape closes the location entry')
    items = int(s.state('items'))
    s.key('ctrl+h')
    s.expect('hidden', '1', 'Ctrl+H shows hidden files')
    check(s.wait(lambda: int(s.state('items')) > items), 'hidden files are listed')
    s.key('ctrl+h')
    s.expect('hidden', '0', 'Ctrl+H hides them again')
    check(s.wait(lambda: int(s.state('items')) == items), 'and hidden again')
    s.key('ctrl+f')
    s.type('lake')
    found = s.wait(lambda: 'item:lake.png' in s.layout() and 'item:lake shore.png' in s.layout(), 10)
    check(found, f'the search finds files in subfolders ({[k for k in s.layout() if k.startswith("item:")]})')
    s.expect('searching', '0', 'the search below the folder ends', 10)
    s.shot('search')
    s.key('Escape')
    s.key('Escape')
    s.expect('query', '', 'Escape ends the search')
    # The list view, sorted by a column.
    s.click('item:Documents', double=True)
    s.click('view:list')
    s.expect('view', 'list', 'the list view')
    s.click('column:size')
    s.expect('sort', 'size:desc', 'a column sorts')
    first = [k for k in s.layout() if k.startswith('item:')][0]
    check(first == 'item:Zebra.txt', f'the biggest file comes first ({first})')
    s.click('column:size')
    s.expect('sort', 'size:asc', 'a second click reverses the order')
    s.click('column:name')
    s.click('view:grid')
    s.expect('view', 'grid', 'back to the grid')
    s.click('place:recent')
    s.expect('location', 'recent:', 'Recent')
    s.expect('loading', '0', 'Recent is read')
    check(s.wait(lambda: 'item:report.md' in s.layout(), 4),
          f'Recent lists the recently used files ({[k for k in s.layout() if k.startswith("item:")]})')
    s.click('cancel')
    code, out = s.finish()
    check(code == 1, 'Cancel')


def test_drop(s):
    if not PEER:
        print('no sde-xdnd-peer: the drop is not checked')
        return
    h = s.home
    s.start('--mode', 'open', '--folder', h / 'Documents', '--program', 'ui-test-drop')
    x, y = s.point('files')
    r = sp.run([PEER, 'source', s.wid, str(x), str(y), 'text/uri-list', f'file://{h}/Pictures/b.png\r\n'],
               env=s.env, capture_output=True, text=True, timeout=20)
    check('finished 1' in r.stdout, f'the dialog takes the drop ({r.stdout.strip()!r})')
    s.expect('location', f'{h}/Pictures', 'a dropped file opens its folder')
    s.expect('selected', 'b.png', 'and is selected')
    s.key('Return')
    code, out = s.finish()
    check(chosen(out) == [f'{h}/Pictures/b.png'], f'the dropped file is chosen ({out!r})')


def test_big_folder(s):
    h = s.home
    s.start('--mode', 'open', '--folder', h / 'big', '--program', 'ui-test-big')
    s.expect('total', '10000', 'every file of a folder of 10 000 is read', 15)
    opened = int(s.state('opened-ms') or 99999)
    print(f'10 000 files: first frame after {opened} ms, read after {s.state("loaded-ms")} ms')
    check(opened < 400, f'the dialog shows at once ({opened} ms)')
    s.key('End')
    s.expect('selected', 'file-09999.dat', 'End goes to the last file')
    s.key('Escape')
    s.finish()


def test_small_screen(temp):
    s = Session(temp, '1024x600', 150, panel=60)
    (s.home / 'Documents').mkdir(parents=True, exist_ok=True)
    try:
        s.start('--mode', 'save', '--folder', s.home / 'Documents', '--name', 'x.txt', '--program', 'ui-test-small',
                '--filters', 'Text files\\t*.txt')
        x, y, w, h = s.origin()
        check(y + h <= 540, f'the dialog fits above the panel ({y}+{h})')
        lay = s.layout()
        for name in ('accept', 'cancel', 'name', 'filter', 'new-folder', 'back', 'up', 'pathbar', 'view:list',
                     'menu'):
            r = lay.get(name)
            usable = name in ('back', 'filter') or (r and r[4] == 1)  # Back: no history yet
            check(r and r[0] >= 0 and r[1] >= 0 and r[0] + r[2] <= w and r[1] + r[3] <= h and usable,
                  f'{name} is in view at 1024x600, 150 % ({r})')
        s.shot('small-screen')
        s.key('Escape')
        s.finish()
    finally:
        s.close()


def main():
    with tempfile.TemporaryDirectory(prefix='sde-file-chooser-ui-') as temp:
        s = Session(temp)
        make_home(s.home)
        try:
            for test in (test_open, test_multi, test_folder, test_save, test_location_search_hidden, test_drop,
                         test_big_folder):
                print('--', test.__name__, flush=True)
                try:
                    test(s)
                except Exception as e:  # a failed step ends that test, not the run
                    check(False, f'{test.__name__}: {e}')
                    if s.proc and s.proc.poll() is None:
                        s.proc.kill()
        finally:
            s.close()
        small = Path(temp) / 'small'
        small.mkdir()
        print('-- test_small_screen', flush=True)
        try:
            test_small_screen(small)
        except Exception as e:
            check(False, f'test_small_screen: {e}')
    if failures:
        print(f'{len(failures)} check(s) failed')
        return 1
    print('the file chooser works')
    return 0


if __name__ == '__main__':
    sys.exit(main())
