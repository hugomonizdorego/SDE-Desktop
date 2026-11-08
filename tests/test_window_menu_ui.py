#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""SDE's window menu on a private Xvfb (0.2.0 Alpha 4, docs/WINDOWS.md):

  1. sde-window-menu --window XID --at X,Y opens the menu there (its items,
     in the taskbar button's order, are in SDE_WINDOW_MENU_QA_DIR); Escape
     closes it without doing anything;
  2. with SDE WM patch 0006: a right click on the title bar opens SDE's menu
     at the pointer, Alt+Space and the window icon below the title bar at
     the window's left edge;
  3. the items work, chosen with the keyboard: Maximize, Restore, Always on
     Top, Move to Workspace > Workspace 2, Close;
  4. Openbox's own menu is the fallback when the program cannot be started
     (SDE_WM_WINDOW_MENU names a missing program): a menu opens, but not SDE's.

The test window is tests/frozen_client_fixture.cpp (it answers pings and
closes on WM_DELETE_WINDOW). Every process signalled is one this test
started. test_window_menu_ui.py BUILD_DIR [--wm SDE-WM] [--display N]
Exit status 0 passed, 1 failed, 77 skipped (no Xvfb, xdotool or WM).
"""
import argparse
import os
import re
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAIL = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what, flush=True)
    if not cond:
        FAIL.append(what)
    return cond


def wait(fn, timeout=8.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


class Test:
    def __init__(self, args):
        self.build = Path(args.build).resolve()
        self.tmp = Path(tempfile.mkdtemp(prefix='sde-window-menu-ui-'))
        self.home = self.tmp / 'home'
        (self.home / '.config/sde/openbox').mkdir(parents=True)
        (self.tmp / 'qa').mkdir()
        (self.tmp / 'run').mkdir(mode=0o700)
        self.procs = []
        self.wm_proc = None
        self.env = dict(os.environ)
        self.env.update(HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / '.config'),
                        XDG_CACHE_HOME=str(self.home / '.cache'), XDG_DATA_HOME=str(self.home / '.local/share'),
                        XDG_RUNTIME_DIR=str(self.tmp / 'run'), SDE_WINDOW_MENU_QA_DIR=str(self.tmp / 'qa'),
                        LANG='C.UTF-8', LC_ALL='C.UTF-8',
                        PATH=f'{self.build / "src/windows"}:{self.build / "src"}:{self.build / "tests"}:'
                             f'{os.environ["PATH"]}')
        self.env.pop('SDE_WM_WINDOW_MENU', None)
        self.rc = self.home / '.config/sde/openbox/rc.xml'
        shutil.copyfile(ROOT / 'data/openbox/rc.xml', self.rc)
        self.wm = args.wm

    def spawn(self, *cmd, env=None, log=None):
        out = open(self.tmp / (log or (Path(str(cmd[0])).name + '.log')), 'a')
        p = sp.Popen([str(c) for c in cmd], env=env or self.env, stdout=out, stderr=sp.STDOUT)
        self.procs.append(p)
        return p

    def run(self, *cmd, timeout=30):
        return sp.run([str(c) for c in cmd], env=self.env, stdout=sp.PIPE, stderr=sp.PIPE, text=True,
                      timeout=timeout)

    def start_x(self, display):
        for n in ([display] if display else range(240, 300)):
            if Path(f'/tmp/.X{n}-lock').exists() or Path(f'/tmp/.X11-unix/X{n}').exists():
                continue
            xvfb = self.spawn('Xvfb', f':{n}', '-screen', '0', '1280x800x24', '-nolisten', 'tcp', log=f'xvfb-{n}.log')
            self.env['DISPLAY'] = f':{n}'
            # Ours, not the server of a test that took the number at the
            # same moment (ctest -j): it must still run.
            if wait(lambda: self.run('xprop', '-root', '_XKB_RULES_NAMES').returncode == 0, 6) and \
                    (time.sleep(0.3) or xvfb.poll() is None):
                return True
        return False

    def start_wm(self, **extra):
        if self.wm_proc and self.wm_proc.poll() is None:
            self.wm_proc.terminate()
            self.wm_proc.wait(5)
            wait(lambda: '_NET_SUPPORTING_WM_CHECK(WINDOW)' not in self.run('xprop', '-root').stdout, 3)
        env = dict(self.env, **extra)
        self.wm_proc = self.spawn(self.wm, '--config-file', self.rc, '--sm-disable', env=env, log='wm.log')
        # Seconds on a loaded machine (ctest -j, a fresh font cache); what
        # the window manager said when it does not come.
        if wait(lambda: '_NET_SUPPORTING_WM_CHECK(WINDOW)' in
                self.run('xprop', '-root', '_NET_SUPPORTING_WM_CHECK').stdout, 30):
            return True
        print((self.tmp / 'wm.log').read_text(errors='replace')[-1500:], flush=True)
        return False

    # ------------------------------------------------------------ X helpers
    def prop(self, win, name):
        r = self.run('xprop', '-id', win, name)
        return r.stdout.strip() if r.returncode == 0 and 'not found' not in r.stdout else ''

    def geometry(self, win):
        text = self.run('xwininfo', '-id', win).stdout
        get = lambda label: int(re.search(re.escape(label) + r'\s*:\s*(-?\d+)', text).group(1))
        return (get('Absolute upper-left X'), get('Absolute upper-left Y'), get('Width'), get('Height'))

    def extents(self, win):
        m = re.findall(r'\d+', self.prop(win, '_NET_FRAME_EXTENTS').split('=')[-1])
        return [int(v) for v in m] if len(m) == 4 else [0, 0, 0, 0]

    def key(self, *keys):
        for k in keys:
            self.run('xdotool', 'key', k)
            time.sleep(0.12)

    def menu(self):
        try:
            return (self.tmp / 'qa/window-menu.txt').read_text()
        except FileNotFoundError:
            return ''

    def forget_menu(self):
        (self.tmp / 'qa/window-menu.txt').unlink(missing_ok=True)

    def menu_running(self):
        r = sp.run(['pgrep', '-u', str(os.getuid()), '-x', 'sde-window-menu'], stdout=sp.PIPE, text=True)
        return bool(r.stdout.split())

    def viewable_children(self):
        out = set()
        for line in self.run('xwininfo', '-root', '-children').stdout.splitlines():
            m = re.match(r'\s+(0x[0-9a-f]+)', line)
            if m and 'IsViewable' in self.run('xwininfo', '-id', m.group(1)).stdout:
                out.add(m.group(1))
        return out

    def client(self, title):
        ready = self.tmp / f'ready-{len(self.procs)}'
        self.spawn(self.build / 'tests/sde-frozen-client', '--title', title, '--ready', ready, log='client.log')
        return wait(lambda: ready.exists() and ready.read_text().strip(), 8)

    def stop(self):
        for p in reversed(self.procs):
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(5)
            except sp.TimeoutExpired:
                p.kill()  # only processes this test started
        shutil.rmtree(self.tmp, True)


def patched(wm):
    try:
        return b'sde-window-menu' in Path(wm).read_bytes()
    except OSError:
        return False


def items(text):
    return [line.split('\t')[0] for line in text.splitlines() if line.startswith(('item:', 'sub:'))]


def direct_menu(t, win):
    """1. The program on its own."""
    t.forget_menu()
    p = t.spawn('sde-window-menu', '--window', win, '--at', '300,200', log='menu.log')
    text = wait(t.menu, 8)
    check('at:300,200' in text, 'sde-window-menu opens at --at')
    got = items(text)
    want = ['item:Restore', 'item:Move', 'item:Size', 'item:Minimize', 'item:Maximize', 'item:Always on Top',
            'item:Move to Workspace', 'sub:Move to Workspace/Workspace 1:on', 'sub:Move to Workspace/Workspace 2',
            'sub:Move to Workspace/All Workspaces', 'sub:Move to Workspace/New Workspace', 'item:Close']
    check(got == want, f'the items of the taskbar menu, in order ({got})')
    check('item:Restore\t0' in text, 'Restore is disabled for a window that is not maximized')
    for gone in ('Roll', 'Decorate', 'Layer', 'Iconify', 'Send to'):
        check(gone not in text, f'no Openbox item "{gone}"')
    time.sleep(0.4)
    t.key('Escape')
    check(wait(lambda: p.poll() is not None, 5), 'Escape closes the menu')
    check('_NET_WM_STATE_MAXIMIZED' not in t.prop(win, '_NET_WM_STATE'), 'Escape does nothing to the window')


def through_wm(t, win):
    """2 and 3: SDE WM runs the menu."""
    x, y, w, h = t.geometry(win)
    left, right, top, bottom = t.extents(win)
    t.run('xdotool', 'windowactivate', '--sync', win)
    # A right click on the title bar: at the pointer.
    t.forget_menu()
    px, py = x + w // 2, y - top // 2
    t.run('xdotool', 'mousemove', px, py, 'click', '3')
    text = wait(t.menu, 8)
    check(f'at:{px},{py}' in text, f'a right click on the title bar opens SDE\'s menu at the pointer ({text[:40]!r})')
    time.sleep(0.4)
    t.key('Escape')
    wait(lambda: not t.menu_running(), 5)
    # Alt+Space: below the title bar, at the frame's left edge.
    t.forget_menu()
    t.run('xdotool', 'mousemove', x + w // 2, y + h // 2)
    t.key('alt+space')
    text = wait(t.menu, 8)
    check(f'at:{x - left},{y}' in text or f'at:{x - left},{y - 1}' in text,
          f'Alt+Space opens it below the title bar ({text[:40]!r}, frame {x - left},{y - top})')
    time.sleep(0.4)
    # Move, Size, Minimize, Maximize (Restore is disabled).
    t.key('Down', 'Down', 'Down', 'Down', 'Return')
    check(wait(lambda: '_NET_WM_STATE_MAXIMIZED_HORZ' in t.prop(win, '_NET_WM_STATE'), 5), 'Maximize maximizes')
    wait(lambda: not t.menu_running(), 5)
    t.forget_menu()
    t.key('alt+space')
    text = wait(t.menu, 8)
    check('item:Restore\n' in text and 'item:Maximize\t0' in text,
          'a maximized window: Restore enabled, Maximize disabled')
    time.sleep(0.4)
    t.key('Down', 'Return')
    check(wait(lambda: '_NET_WM_STATE_MAXIMIZED' not in t.prop(win, '_NET_WM_STATE'), 5), 'Restore restores')
    wait(lambda: not t.menu_running(), 5)
    # Always on Top: the fifth active item.
    t.forget_menu()
    t.key('alt+space')
    wait(t.menu, 8)
    time.sleep(0.4)
    t.key('Down', 'Down', 'Down', 'Down', 'Down', 'Return')
    check(wait(lambda: '_NET_WM_STATE_ABOVE' in t.prop(win, '_NET_WM_STATE'), 5), 'Always on Top')
    wait(lambda: not t.menu_running(), 5)
    t.forget_menu()
    t.key('alt+space')
    text = wait(t.menu, 8)
    check('item:Always on Top:on' in text, 'Always on Top shows its check mark')
    time.sleep(0.4)
    # Move to Workspace > Workspace 2.
    t.key('Down', 'Down', 'Down', 'Down', 'Down', 'Down', 'Right', 'Down', 'Return')
    check(wait(lambda: t.prop(win, '_NET_WM_DESKTOP').endswith('= 1'), 5), 'Move to Workspace > Workspace 2')
    wait(lambda: not t.menu_running(), 5)
    t.run('xdotool', 'set_desktop', '1')
    time.sleep(0.5)
    t.run('xdotool', 'windowactivate', '--sync', win)
    # Close (Alt+F4 in the menu): the last item.
    t.forget_menu()
    t.key('alt+space')
    wait(t.menu, 8)
    time.sleep(0.4)
    t.key('Up', 'Return')
    check(wait(lambda: t.run('xwininfo', '-id', win).returncode != 0, 6), 'Close closes the window')


def fallback(t):
    """4. Openbox's menu when sde-window-menu cannot start."""
    if not t.start_wm(SDE_WM_WINDOW_MENU=str(t.tmp / 'no-such-program')):
        check(False, 'the window manager starts again')
        return
    win = t.client('Fallback Test')
    if not check(bool(win), 'a test window'):
        return
    time.sleep(0.5)
    x, y, w, h = t.geometry(win)
    left, right, top, bottom = t.extents(win)
    before = t.viewable_children()
    t.forget_menu()
    t.run('xdotool', 'mousemove', x + w // 2, y - top // 2, 'click', '3')
    opened = wait(lambda: len(t.viewable_children() - before) > 0, 5)
    check(bool(opened), 'Openbox opens its own menu when sde-window-menu cannot start')
    check(not t.menu(), 'SDE\'s menu did not run')
    t.key('Escape')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build')
    ap.add_argument('--wm')
    ap.add_argument('--display', type=int)
    args = ap.parse_args()
    for tool in ('Xvfb', 'xdotool', 'xprop', 'xwininfo'):
        if not shutil.which(tool):
            print(f'{tool} is required; skipped')
            return 77
    if not args.wm or not Path(args.wm).exists():
        args.wm = shutil.which('sde-wm') or shutil.which('openbox')
    if not args.wm:
        print('no window manager (sde-wm or openbox); skipped')
        return 77
    t = Test(args)
    try:
        if not t.start_x(args.display):
            print('Xvfb did not start; skipped')
            return 77
        if not check(t.start_wm(), 'the window manager starts'):
            return 1
        win = t.client('Window Menu Test')
        if not check(bool(win), 'a test window'):
            return 1
        time.sleep(0.5)
        direct_menu(t, win)
        if patched(args.wm):
            through_wm(t, win)
            fallback(t)
        else:
            print(f'skip  title bar, Alt+Space, fallback: {args.wm} has no SDE patch 0006', flush=True)
    finally:
        t.stop()
    print('FAILED: ' + '; '.join(FAIL) if FAIL else 'all window menu checks passed')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
