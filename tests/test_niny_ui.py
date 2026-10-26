#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Niny's tabs and menu bar on a private Xvfb (0.2 Alpha 3).

test_niny_ui.py NINY [--shots DIR]

Runs Niny at 1366x768 (100 %), 1920x1080 (150 %) and 1024x600 (150 %, with
a work area that leaves room for a 60 pixel panel). SDE_PANEL_QA_DIR makes
Niny describe its tabs, their close buttons, the + button and the menus
(Niny.txt, Niny-menu.txt); the test clicks there with xdotool and checks:

- the × of a tab closes that tab and nothing else: no new "Untitled", also
  when the × of the last tab is clicked twice quickly (alpha 3: the second
  click opened a tab), in a scrolled tab strip and after a drag;
- with one tab its × is inactive: a click, a double click, the middle button
  and Ctrl+W keep the tab;
- a modified tab asks first: Cancel keeps it, Close Without Saving keeps the
  file as it was, Save writes it;
- the menu bar has File, Edit, View, Search, Document, Tools and Help; F10
  opens File with its first item highlighted, Right and Left move to the
  other menus, Alt and the underlined letter open a menu, and every menu
  fits the work area without scrolling;
- with View > Menu Bar off, F10 and Alt bring the bar back for a moment, and
  the Niny mark's menu switches it on again.

Keys without modifiers (F10, Alt) are sent by keycode through XTEST: with
some keymaps "xdotool key F10" adds Alt. SDE_TEST_DISPLAY=N sets the first
display number (N to N+2)."""
import argparse
import ctypes
import ctypes.util
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

if not shutil.which('xdotool') or not shutil.which('Xvfb'):
    print('xdotool and Xvfb are required; skipped')
    sys.exit(77)

ap = argparse.ArgumentParser()
ap.add_argument('niny')
ap.add_argument('--shots', help='save screenshots here')
args = ap.parse_args()
failures = []


class Xtest:
    """Key presses by keycode through XTEST (no modifier is added)."""

    def __init__(self, display):
        self.x = ctypes.CDLL(ctypes.util.find_library('X11') or 'libX11.so.6')
        self.x.XOpenDisplay.restype = ctypes.c_void_p
        self.x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x.XStringToKeysym.restype = ctypes.c_ulong
        self.x.XStringToKeysym.argtypes = [ctypes.c_char_p]
        self.x.XKeysymToKeycode.restype = ctypes.c_ubyte
        self.x.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.t = ctypes.CDLL(ctypes.util.find_library('Xtst') or 'libXtst.so.6')
        self.t.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.d = self.x.XOpenDisplay(display.encode())
        if not self.d:
            raise RuntimeError('cannot open ' + display)

    def press(self, combo):
        mods = {'shift': 'Shift_L', 'ctrl': 'Control_L', 'alt': 'Alt_L'}
        syms = [mods.get(k.lower(), k) for k in combo.split('+')]
        codes = [self.x.XKeysymToKeycode(self.d, self.x.XStringToKeysym(s.encode())) for s in syms]
        if not all(codes):
            raise RuntimeError('no keycode for ' + combo)
        for c in codes:
            self.t.XTestFakeKeyEvent(self.d, c, 1, 0)
        for c in reversed(codes):
            self.t.XTestFakeKeyEvent(self.d, c, 0, 0)
        self.x.XSync(self.d, 0)
        time.sleep(0.25)

    def close(self):
        self.x.XCloseDisplay(self.d)


class Session:
    """One Niny on its own Xvfb."""

    def __init__(self, number, size, scale, temp, label):
        self.label = label
        self.number = number
        self.home = Path(temp) / label
        (self.home / 'config/sde').mkdir(parents=True)
        (self.home / 'config/sde/sde.conf').write_text('[appearance]\nscale=%d\n' % scale)
        (self.home / 'qa').mkdir()
        (self.home / 'docs').mkdir()
        self.qa_dir = self.home / 'qa'
        self.server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', size + 'x24',
                                '-nolisten', 'tcp'], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(self.home),
                        XDG_CONFIG_HOME=str(self.home / 'config'), XDG_STATE_HOME=str(self.home / 'state'),
                        XDG_RUNTIME_DIR=str(self.home), LANG='C.UTF-8', LC_ALL='C.UTF-8',
                        SDE_PANEL_QA_DIR=str(self.qa_dir),
                        DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-niny-ui-test')
        for _ in range(100):
            if self.xdo('getmouselocation').returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('Xvfb :%d did not start' % number)
        self.w, self.h = (int(v) for v in size.split('x'))
        self.keys = Xtest(':%d' % number)
        self.proc = None
        self.log = open(self.home / 'niny.log', 'w+')
        self.win = None

    def xdo(self, *a):
        return sp.run(['xdotool'] + [str(x) for x in a], env=self.env, capture_output=True, text=True)

    def work_area(self, height):
        """A panel at the bottom: the work area ends at |height|."""
        sp.run(['xprop', '-root', '-f', '_NET_WORKAREA', '32c', '-set', '_NET_WORKAREA',
                '0, 0, %d, %d' % (self.w, height)], env=self.env, check=False)

    def start(self, files):
        paths = []
        for name, text in files:
            p = self.home / 'docs' / name
            p.write_text(text)
            paths.append(str(p))
        self.proc = sp.Popen([args.niny] + paths, env=self.env, stdout=self.log, stderr=sp.STDOUT)
        if not self.wait(lambda: len(self.tabs()) == len(files), 15):
            raise RuntimeError('%s: Niny did not show %d tabs (%s)' % (self.label, len(files), self.tabs()))
        for _ in range(100):
            out = self.xdo('search', '--onlyvisible', '--name', 'Niny$').stdout.split()
            if out:
                self.win = out[0]
                break
            time.sleep(0.1)
        self.focus()
        time.sleep(0.4)

    def open_more(self, name, text):
        """Hands a file to the running Niny, as a file manager would."""
        p = self.home / 'docs' / name
        p.write_text(text)
        sp.run([args.niny, str(p)], env=self.env, stdout=self.log, stderr=sp.STDOUT, timeout=20)
        return p

    def origin(self):
        out = self.xdo('getwindowgeometry', '--shell', self.win).stdout
        g = {k: int(v) for k, v in (l.split('=', 1) for l in out.split() if '=' in l) if v.lstrip('-').isdigit()}
        return g['X'], g['Y']

    def focus(self):
        if self.win:
            self.xdo('windowfocus', '--sync', self.win)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except sp.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.keys.close()
        self.server.terminate()
        self.server.wait()

    # ------------------------------------------------------------ QA files

    def read(self, name='Niny'):
        try:
            text = (self.qa_dir / (name + '.txt')).read_text()
        except OSError:
            return []
        out = []
        for line in text.splitlines():
            f = line.split('\t')
            if len(f) >= 7:
                out.append((f[0], [int(v) for v in f[1:7]]))
        return out

    def state(self, key, name='Niny'):
        for n, _ in self.read(name):
            if n.startswith('@' + key + ':'):
                return n[len(key) + 2:]
        return None

    def item(self, key, name='Niny'):
        for n, v in self.read(name):
            if n == key:
                return v
        return None

    def tabs(self):
        return [n.split(':', 2)[2] for n, _ in self.read() if n.startswith('tab:')]

    def wait(self, fn, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            r = fn()
            if r:
                return r
            time.sleep(0.05)
        return fn()

    # ------------------------------------------------------------- actions

    def click_box(self, box, clicks=1, button=1):
        if not box:
            raise ValueError("nothing to click (not in the QA file)")
        ox, oy = self.origin()
        x, y = ox + box[0] + box[2] // 2, oy + box[1] + box[3] // 2
        self.xdo('mousemove', x, y)
        time.sleep(0.15)
        self.xdo('click', '--repeat', clicks, '--delay', 90, button)
        time.sleep(0.35)

    def click_root(self, box):
        if not box:
            raise ValueError("no menu item to click")
        self.xdo('mousemove', box[0] + box[2] // 2, box[1] + box[3] // 2)
        time.sleep(0.15)
        self.xdo('click', 1)
        time.sleep(0.35)

    def park(self):
        ox, oy = self.origin()
        self.xdo('mousemove', ox + 600, oy + 400)
        time.sleep(0.1)

    def shot(self, name):
        if args.shots and shutil.which('import'):
            Path(args.shots).mkdir(parents=True, exist_ok=True)
            sp.run(['import', '-window', 'root', str(Path(args.shots) / ('%s-%s.png' % (self.label, name)))],
                   env=self.env, check=False)


def expect(s, what, ok):
    if not ok:
        failures.append('%s: %s' % (s.label, what))
    return ok


def same_tabs(s, want, what, timeout=4.0):
    got = s.wait(lambda: s.tabs() == want, timeout)
    if not got:
        failures.append('%s: %s: tabs %s, expected %s' % (s.label, what, s.tabs(), want))
        return False
    # Nothing may follow: no new "Untitled" a moment later.
    time.sleep(0.5)
    return expect(s, '%s: tabs changed afterwards to %s' % (what, s.tabs()), s.tabs() == want)


def check_tabs(s):
    """The × of each tab, the only tab, modified tabs, dragging."""
    s.park()
    same_tabs(s, ['a.txt', 'b.txt', 'c.txt'], 'start')
    s.click_box(s.item('close:1'))
    same_tabs(s, ['a.txt', 'c.txt'], 'the × of the middle tab')
    # The × of the last tab twice quickly: the second click lands on the
    # empty strip and must not open a tab.
    s.click_box(s.item('close:1'), clicks=2)
    same_tabs(s, ['a.txt'], 'a double click on the × of the last tab')
    # The only tab.
    close = s.item('close:0')
    expect(s, 'the only tab has an active ×', close is not None and close[4] == 0)
    s.click_box(close)
    same_tabs(s, ['a.txt'], 'a click on the inactive ×')
    s.click_box(close, clicks=2)
    same_tabs(s, ['a.txt'], 'a double click on the inactive ×')
    s.shot('one-tab')
    s.click_box(s.item('tab:0:a.txt'), button=2)
    same_tabs(s, ['a.txt'], 'the middle button on the only tab')
    s.park()
    s.keys.press('ctrl+w')
    same_tabs(s, ['a.txt'], 'Ctrl+W with one tab')
    expect(s, 'Ctrl+W with one tab says why', 'one tab' in (s.state('toast') or ''))
    # A new tab and its ×.
    s.keys.press('ctrl+n')
    same_tabs(s, ['a.txt', 'Untitled'], 'Ctrl+N')
    expect(s, 'two tabs: the × is active', s.item('close:0')[4] == 1 and s.item('close:1')[4] == 1)
    s.click_box(s.item('close:1'))
    same_tabs(s, ['a.txt'], 'the × of a new tab')
    # A modified tab: Cancel, Close Without Saving, Save.
    s.open_more('b.txt', 'bravo\n')
    same_tabs(s, ['a.txt', 'b.txt'], 'b.txt handed to Niny')
    s.focus()
    s.click_box(s.item('tab:0:a.txt'))
    s.keys.press('ctrl+End')
    s.xdo('type', '--delay', '20', 'changed')
    time.sleep(0.3)
    s.click_box(s.item('close:0'))
    expect(s, 'a modified tab asks before it closes', s.wait(lambda: 'a.txt' in (s.state('sheet') or '')))
    s.shot('modified')
    s.keys.press('Escape')
    s.wait(lambda: s.state('sheet') == 'none')
    same_tabs(s, ['a.txt', 'b.txt'], 'Cancel')
    s.click_box(s.item('close:0'))
    s.wait(lambda: s.item('button:Close Without Saving'))
    button = s.item('button:Close Without Saving')
    if expect(s, 'the question has Close Without Saving', button is not None):
        s.click_box(button)
    same_tabs(s, ['b.txt'], 'Close Without Saving')
    expect(s, 'Close Without Saving kept the file', (s.home / 'docs/a.txt').read_text() == 'alpha\n')
    s.open_more('a.txt', 'alpha\n')
    same_tabs(s, ['b.txt', 'a.txt'], 'a.txt handed to Niny again')
    s.focus()
    s.keys.press('ctrl+End')
    s.xdo('type', '--delay', '20', 'saved')
    time.sleep(0.3)
    s.click_box(s.item('close:1'))
    s.wait(lambda: 'a.txt' in (s.state('sheet') or ''))
    s.keys.press('Return')  # Save is the default button
    same_tabs(s, ['b.txt'], 'Save')
    expect(s, 'Save wrote the file', (s.home / 'docs/a.txt').read_text().startswith('alpha\nsaved'))
    # Dragging a tab moves it and closes nothing.
    s.open_more('c.txt', 'charlie\n')
    same_tabs(s, ['b.txt', 'c.txt'], 'c.txt handed to Niny')
    s.focus()
    a, b = s.item('tab:0:b.txt'), s.item('tab:1:c.txt')
    ox, oy = s.origin()
    y = oy + a[1] + a[3] // 2
    s.xdo('mousemove', ox + a[0] + a[2] // 3, y)
    s.xdo('mousedown', 1)
    for k in range(1, 9):
        s.xdo('mousemove', ox + a[0] + a[2] // 3 + (b[0] + b[2] // 2 - a[0] - a[2] // 3) * k // 8, y)
        time.sleep(0.04)
    s.xdo('mouseup', 1)
    time.sleep(0.3)
    same_tabs(s, ['c.txt', 'b.txt'], 'dragging the first tab onto the second')


def check_scrolled(s):
    """Many tabs: the strip scrolls and the visible × close their own tab."""
    names = ['file-%02d.txt' % i for i in range(1, 15)]
    for n in names:
        s.open_more(n, n + '\n')
    s.focus()
    s.park()
    want = s.wait(lambda: len(s.tabs()) == 2 + len(names) and s.tabs(), 10)
    if not expect(s, 'all files opened (%s)' % s.tabs(), want):
        return
    scrolled = any(v[5] for n, v in s.read() if n.startswith('tab:'))
    expect(s, 'the tab strip scrolls with %d tabs' % len(want), scrolled)
    s.shot('scrolled')
    for last in (True, False):
        # The strip may scroll after a close: the boxes are read each time.
        closes = [(n, v) for n, v in s.read() if n.startswith('close:') and v[4]]
        if not expect(s, 'visible tabs have an active ×', closes):
            return
        pick = closes[-1] if last else closes[0]
        before = s.tabs()
        index = int(pick[0].split(':')[1])
        s.click_box(pick[1])
        after = before[:index] + before[index + 1:]
        same_tabs(s, after, 'the × of %s in the scrolled strip' % before[index])
        s.park()


def check_menus(s):
    """The menu bar from the mouse and the keyboard; menus fit the screen."""
    s.park()
    titles = ['file', 'edit', 'view', 'search', 'document', 'tools', 'help']
    for t in titles:
        expect(s, 'the menu bar has %s' % t, s.item('menu:' + t) is not None)
    s.keys.press('F10')
    expect(s, 'F10 opens File', s.wait(lambda: s.state('open', 'Niny-menu') == 'file'))
    expect(s, 'F10 highlights the first item',
           s.wait(lambda: s.state('hover', 'Niny-menu') == '0:New Tab'))
    s.shot('menu-file')
    s.keys.press('Right')
    expect(s, 'Right moves to Edit', s.wait(lambda: s.state('open', 'Niny-menu') == 'edit'))
    s.keys.press('Left')
    s.keys.press('Left')
    expect(s, 'Left from File moves to Help', s.wait(lambda: s.state('open', 'Niny-menu') == 'help'))
    s.keys.press('Escape')
    expect(s, 'Escape closes the menu', s.wait(lambda: s.state('open', 'Niny-menu') == 'none'))
    for t in titles:
        s.keys.press('alt+' + t[0])
        if not expect(s, 'Alt+%s opens %s' % (t[0].upper(), t), s.wait(lambda: s.state('open', 'Niny-menu') == t)):
            s.keys.press('Escape')
            continue
        level = s.item('level:0', 'Niny-menu')
        expect(s, 'the %s menu fits the work area (%s)' % (t, level), level and level[4] == 1)
        expect(s, 'the %s menu does not scroll' % t, level and level[5] == 0)
        if t == 'view':
            s.shot('menu-view')
        s.keys.press('Escape')
        s.wait(lambda: s.state('open', 'Niny-menu') == 'none')
    # The mouse: a click opens a title, the pointer moves to the next one.
    # (On a small screen a long menu moves up over the titles to fit.)
    s.click_box(s.item('menu:file'))
    expect(s, 'a click opens File', s.wait(lambda: s.state('open', 'Niny-menu') == 'file'))
    edit = s.item('menu:edit')
    ox, oy = s.origin()
    level = s.item('level:0', 'Niny-menu')
    if level and level[1] >= oy + edit[1] + edit[3]:
        s.xdo('mousemove', ox + edit[0] + edit[2] // 2, oy + edit[1] + edit[3] // 2)
        expect(s, 'the pointer on Edit opens Edit', s.wait(lambda: s.state('open', 'Niny-menu') == 'edit'))
        s.click_box(edit)
        expect(s, 'a click on the open title closes it', s.wait(lambda: s.state('open', 'Niny-menu') == 'none'))
    else:
        s.keys.press('Escape')
        s.wait(lambda: s.state('open', 'Niny-menu') == 'none')
    # View > Menu Bar off; F10, Alt and the Niny mark.
    s.click_box(s.item('menu:view'))
    s.wait(lambda: s.item('item:0:Menu Bar', 'Niny-menu'))
    s.click_root(s.item('item:0:Menu Bar', 'Niny-menu'))
    expect(s, 'View > Menu Bar hides the menu bar', s.wait(lambda: s.state('menubar') == '0'))
    s.park()
    s.keys.press('F10')
    expect(s, 'F10 shows the hidden menu bar with File open',
           s.wait(lambda: s.state('menubar') == '1' and s.state('open', 'Niny-menu') == 'file'))
    s.keys.press('Escape')
    expect(s, 'the menu bar hides again', s.wait(lambda: s.state('menubar') == '0'))
    s.keys.press('Alt_L')
    expect(s, 'Alt shows the hidden menu bar', s.wait(lambda: s.state('menubar') == '1' and
                                                      s.state('titles') == 'focused'))
    s.shot('menu-alt')
    s.keys.press('Escape')
    expect(s, 'Escape hides it again', s.wait(lambda: s.state('menubar') == '0'))
    s.click_box(s.item('logo'))
    expect(s, 'the Niny mark opens its menu', s.wait(lambda: s.state('open', 'Niny-menu') == 'main'))
    item = s.wait(lambda: s.item('item:0:Menu Bar', 'Niny-menu'))
    if expect(s, 'the Niny mark menu has Menu Bar', item):
        s.click_root(item)
    expect(s, 'Menu Bar in the Niny mark menu shows it again', s.wait(lambda: s.state('menubar') == '1'))
    expect(s, 'Niny still runs', s.proc.poll() is None)


CASES = [
    ('100', '1366x768', 100, None),
    ('150', '1920x1080', 150, None),
    ('small', '1024x600', 150, 540),
]

base = int(os.environ.get('SDE_TEST_DISPLAY', 400 + os.getpid() % 20000))
with tempfile.TemporaryDirectory(prefix='sde-niny-ui-') as temp:
    for n, (label, size, scale, work) in enumerate(CASES):
        s = None
        try:
            s = Session(base + n, size, scale, temp, label)
            if work:
                s.work_area(work)
            s.start([('a.txt', 'alpha\n'), ('b.txt', 'bravo\n'), ('c.txt', 'charlie\n')])
            check_tabs(s)
            check_menus(s)
            if label != 'small':
                check_scrolled(s)
            expect(s, 'Niny is still running', s.proc.poll() is None)
        except Exception as e:  # report and go on with the next size
            failures.append('%s: %s: %s' % (label, type(e).__name__, e))
        finally:
            if s:
                s.stop()
                s.log.seek(0)
                bad = [l for l in s.log.read().splitlines() if 'BUG' in l or 'Segmentation' in l]
                failures.extend('%s: %s' % (label, l) for l in bad)

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('Niny tabs and menu bar: close buttons, the only tab, modified tabs, drag, scrolled strip, '
      'menus from the mouse and keyboard, fit at 100 and 150 %')
