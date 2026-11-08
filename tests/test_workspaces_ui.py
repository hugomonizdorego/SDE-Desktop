#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Workspaces, Task View and the window commands of SDE's shortcuts on a
private Xvfb (0.2.0 Alpha 4, docs/WINDOWS.md):

  1. Ctrl+Win+D (SDE WM's binding runs sde-windows) adds a workspace and
     shows it; sde.conf and rc.xml keep the count; at 5 nothing is added;
     Ctrl+Win+F4 closes the workspace in view, "workspace close 2" closes
     the second one: its windows move to the one before it, the later
     ones move down with their names;
  2. Control Center > Workspaces: the number and a name change the window
     manager's workspaces at once and are saved;
  3. the taskbar shows the windows of the workspace in view only;
  4. Task View lists the workspaces and the windows of the one in view (a
     picture or the icon), Page Down shows the next workspace's windows,
     dragging a window onto a workspace moves it, "New workspace" adds
     one, clicking a window switches to it, Esc and a second sde-taskview
     close it;
  5. Win+Left/Right/Up/Down snap, maximize and restore; Win+M,
     Win+Shift+M and Win+Home minimize and bring back.

Every process signalled is one this test started. Run it under
dbus-run-session: test_workspaces_ui.py BUILD_DIR [--wm SDE-WM] [--display N]
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
        self.tmp = Path(tempfile.mkdtemp(prefix='sde-workspaces-ui-'))
        self.home = self.tmp / 'home'
        (self.home / '.config/sde/openbox').mkdir(parents=True)
        (self.tmp / 'qa').mkdir()
        (self.tmp / 'run').mkdir(mode=0o700)
        self.procs = []
        self.env = dict(os.environ)
        self.env.update(HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / '.config'),
                        XDG_CACHE_HOME=str(self.home / '.cache'), XDG_DATA_HOME=str(self.home / '.local/share'),
                        XDG_RUNTIME_DIR=str(self.tmp / 'run'), XDG_CONFIG_DIRS=str(self.tmp / 'no-system-config'),
                        SDE_PANEL_QA_DIR=str(self.tmp / 'qa'), SDE_TASKVIEW_QA_DIR=str(self.tmp / 'qa'),
                        LANG='C.UTF-8', LC_ALL='C.UTF-8',
                        PATH=f'{self.build / "src/windows"}:{self.build / "src"}:{self.build / "tests"}:'
                             f'{os.environ["PATH"]}')
        self.rc = self.home / '.config/sde/openbox/rc.xml'
        shutil.copyfile(ROOT / 'data/openbox/rc.xml', self.rc)
        # No pinned apps: Win+1 is the first window's button.
        (self.home / '.config/sde/sde.conf').write_text('[panel]\nwindow_previews=false\npinned=\n')
        self.wm = args.wm

    def spawn(self, *cmd, log=None, env=None):
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

    # ------------------------------------------------------------ X helpers
    def root(self, name):
        return self.run('xprop', '-root', name).stdout.strip()

    def count(self):
        m = re.search(r'= (\d+)', self.root('_NET_NUMBER_OF_DESKTOPS'))
        return int(m.group(1)) if m else 0

    def current(self):
        m = re.search(r'= (\d+)', self.root('_NET_CURRENT_DESKTOP'))
        return int(m.group(1)) if m else -1

    def names(self):
        return re.findall(r'"([^"]*)"', self.root('_NET_DESKTOP_NAMES'))

    def prop(self, win, name):
        r = self.run('xprop', '-id', win, name)
        return r.stdout.strip() if r.returncode == 0 and 'not found' not in r.stdout else ''

    def desktop(self, win):
        m = re.search(r'= (\d+)', self.prop(win, '_NET_WM_DESKTOP'))
        return int(m.group(1)) if m else -1

    def frame(self, win):
        """The frame's outer rectangle (x, y, w, h)."""
        text = self.run('xwininfo', '-id', win).stdout
        get = lambda label: int(re.search(re.escape(label) + r'\s*:\s*(-?\d+)', text).group(1))
        e = [int(v) for v in re.findall(r'\d+', self.prop(win, '_NET_FRAME_EXTENTS').split('=')[-1])] or [0, 0, 0, 0]
        return (get('Absolute upper-left X') - e[0], get('Absolute upper-left Y') - e[2],
                get('Width') + e[0] + e[1], get('Height') + e[2] + e[3])

    def key(self, *keys):
        for k in keys:
            self.run('xdotool', 'key', k)
            time.sleep(0.15)

    def conf(self):
        try:
            return (self.home / '.config/sde/sde.conf').read_text()
        except FileNotFoundError:
            return ''

    def qa(self, name):
        try:
            return (self.tmp / 'qa' / name).read_text()
        except FileNotFoundError:
            return ''

    def client(self, title):
        ready = self.tmp / f'ready-{len(self.procs)}'
        self.spawn(self.build / 'tests/sde-frozen-client', '--title', title, '--ready', ready, log='client.log')
        return wait(lambda: ready.exists() and ready.read_text().strip(), 8)

    def activate(self, win):
        self.run('xdotool', 'windowactivate', '--sync', win)
        time.sleep(0.3)

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


def workspace_keys(t, a, b, c):
    """1. Ctrl+Win+D, Ctrl+Win+F4, sde-windows workspace close N."""
    check(t.count() == 2, f'two workspaces to begin with ({t.count()})')
    t.activate(a)
    t.key('ctrl+super+d')
    check(wait(lambda: t.count() == 3, 5), 'Ctrl+Win+D adds a third workspace')
    check(wait(lambda: t.current() == 2, 3), 'and shows it')
    check('count=3' in t.conf(), 'sde.conf keeps the count')
    check(wait(lambda: '<number>3</number>' in t.rc.read_text(), 5), 'so does SDE WM\'s rc.xml')
    # b on workspace 2, c on workspace 3; workspace 3 gets a name.
    t.run('xdotool', 'set_desktop_for_window', b, '1')
    t.run('xdotool', 'set_desktop_for_window', c, '2')
    conf = t.conf().replace('name3=\n', '') + '\n[workspaces]\nname3=Music\n'
    (t.home / '.config/sde/sde.conf').write_text(conf)
    t.run('sde-windows', 'workspace', 'new')
    t.run('sde-windows', 'workspace', 'new')
    check(wait(lambda: t.count() == 5, 5), 'up to five workspaces')
    r = t.run('sde-windows', 'workspace', 'new')
    time.sleep(0.4)
    check(r.returncode == 1 and t.count() == 5, 'not more than five')
    check(t.names()[2] == 'Music', f'a named workspace ({t.names()})')
    # Close workspace 2: b goes to workspace 1, c (and "Music") to 2.
    r = t.run('sde-windows', 'workspace', 'close', '2')
    check(r.returncode == 0 and wait(lambda: t.count() == 4, 5), 'workspace close 2: four left')
    check(wait(lambda: t.desktop(b) == 0, 3), 'its window moved to the workspace before it')
    check(t.desktop(c) == 1, 'the window of the next workspace moved down with it')
    check(t.names()[1] == 'Music', f'and so did its name ({t.names()})')
    check('name2=Music' in t.conf() and 'count=4' in t.conf(), 'sde.conf follows')
    # Ctrl+Win+F4 closes the workspace in view (the last two are empty).
    t.run('xdotool', 'set_desktop', '3')
    time.sleep(0.4)
    t.key('ctrl+super+F4')
    check(wait(lambda: t.count() == 3, 5), 'Ctrl+Win+F4 closes the workspace in view')
    check(wait(lambda: t.current() == 2, 3), 'and shows the one before it')
    t.run('sde-windows', 'workspace', 'close', '3')
    wait(lambda: t.count() == 2, 5)
    t.run('xdotool', 'set_desktop', '0')
    time.sleep(0.4)


def control_center(t):
    """2. Control Center > Workspaces."""
    p = t.spawn('sde-settings', '--page', 'workspaces', log='settings.log')
    cc = wait(lambda: (t.run('xdotool', 'search', '--onlyvisible', '--name', '^Control Center$').stdout.split()
                       or [None])[0], 15)
    if not check(bool(cc), 'the Control Center opens on Workspaces'):
        return
    def control(name):
        for line in t.qa('Control-Center.txt').splitlines():
            f = line.split('\t')
            if f[0] == name and len(f) >= 6 and f[5] == '1':
                return [int(v) for v in f[1:5]]
        return None
    box = wait(lambda: control('Number of workspaces'), 10)
    if not check(bool(box), 'the number of workspaces is on the page'):
        p.terminate()
        return
    geo = t.run('xwininfo', '-id', cc).stdout
    wx = int(re.search(r'Absolute upper-left X:\s*(-?\d+)', geo).group(1))
    wy = int(re.search(r'Absolute upper-left Y:\s*(-?\d+)', geo).group(1))
    t.run('xdotool', 'mousemove', wx + box[0] + box[2] // 2, wy + box[1] + box[3] // 2, 'click', '1')
    time.sleep(0.8)
    t.key('Down', 'Return')
    check(wait(lambda: t.count() == 3, 5), 'choosing 3 makes three workspaces at once')
    check('count=3' in t.conf(), 'and saves it')
    field = wait(lambda: control('Name of workspace 3'), 6)
    check(bool(field), 'a name field for the new workspace')
    box = control('Name of workspace 1')
    if box:
        t.run('xdotool', 'mousemove', wx + box[0] + box[2] // 2, wy + box[1] + box[3] // 2, 'click', '1')
        time.sleep(0.3)
        t.key('ctrl+a')
        t.run('xdotool', 'type', '--delay', '40', 'Mail')
        t.key('Return')
        check(wait(lambda: t.names()[:1] == ['Mail'], 5), f'a new name reaches the window manager ({t.names()})')
        check('name1=Mail' in t.conf(), 'and is saved')
    p.terminate()
    p.wait(5)


def taskbar(t, a, b):
    """3. The taskbar follows the workspace in view."""
    panel = t.spawn('sde-panel', log='panel.log')
    try:
        taskbar_checks(t, a, b)
    finally:
        # The Start menu of an unpatched window manager would take the keys
        # of the next checks (Win opens it on press there).
        panel.terminate()
        panel.wait(5)


def taskbar_checks(t, a, b):
    def tasks():
        return [line.split('\t')[0] for line in t.qa('SDE-Panel.txt').splitlines() if line.startswith('task:')]
    if not check(bool(wait(lambda: t.qa('SDE-Panel.txt'), 20)), 'the panel starts'):
        return
    t.run('xdotool', 'set_desktop', '0')
    check(wait(lambda: any('Alpha' in k for k in tasks()) and not any('Bravo' in k for k in tasks()), 6),
          f'workspace 1: only its windows on the taskbar ({tasks()})')
    t.run('xdotool', 'set_desktop', '1')
    check(wait(lambda: any('Charlie' in k for k in tasks()) and not any('Alpha' in k for k in tasks()), 6),
          f'workspace 2: its windows ({tasks()})')
    t.run('xdotool', 'set_desktop', '0')
    time.sleep(0.5)
    # SDE WM 0007: Win alone opens the Start menu when it is let go; Win
    # with another key does not open it.
    def start_menu():
        return t.run('xdotool', 'search', '--onlyvisible', '--name', '^SDE Menu$').stdout.split()
    # An SDE WM of this tree (patches 0006 and 0007 come together). Another
    # window manager opens the Start menu as soon as Win goes down, and the
    # menu then takes the keys of Win+1 and Win+T too.
    if b'sde-window-menu' not in Path(t.wm).read_bytes():
        print('skip  Win alone, Win+1, Win+T: the window manager has no SDE patch 0007', flush=True)
        return
    t.activate(a)
    t.key('super+F12')
    time.sleep(1.0)
    check(not start_menu(), 'Win with another key does not open the Start menu')
    t.key('super')
    check(bool(wait(start_menu, 5)), 'Win alone opens the Start menu')
    t.key('Escape')
    wait(lambda: not start_menu(), 5)
    # Win+1: the first button, as a click (the active window goes down,
    # then comes back); Win+T goes through the buttons.
    t.activate(a)
    t.key('super+1')
    check(wait(lambda: '_NET_WM_STATE_HIDDEN' in t.prop(a, '_NET_WM_STATE'), 5),
          'Win+1 on the active window\'s button minimizes it')
    t.key('super+1')
    check(wait(lambda: '_NET_WM_STATE_HIDDEN' not in t.prop(a, '_NET_WM_STATE'), 5), 'Win+1 again brings it back')
    panel = (t.run('xdotool', 'search', '--name', '^SDE Panel$').stdout.split() or [''])[0]
    focus = lambda: t.run('xdotool', 'getwindowfocus').stdout.strip()
    t.key('super+t')
    check(bool(panel) and wait(lambda: focus() == panel, 5), f'Win+T gives the taskbar the keyboard ({focus()})')
    t.key('Escape')
    check(wait(lambda: f'0x{int(a, 16):x}' in t.root('_NET_ACTIVE_WINDOW') and focus() != panel, 5),
          'Esc goes back to the window')
    t.key('super+t')
    wait(lambda: focus() == panel, 5)
    t.key('Return')
    check(wait(lambda: focus() != panel and f'0x{int(a, 16):x}' in t.root('_NET_ACTIVE_WINDOW'), 5),
          'Enter opens the chosen button\'s window')


def taskview_cards(t):
    cards = {}
    for line in t.qa('taskview.txt').splitlines():
        f = line.split('\t')
        if len(f) >= 5:
            cards[f[0]] = [int(v) for v in f[1:5]] + f[5:]
        elif ':' in line:
            k, v = line.split(':', 1)
            cards[k] = v
    return cards


def taskview(t, a, b, c):
    """4. Task View."""
    (t.tmp / 'qa/taskview.txt').unlink(missing_ok=True)
    p = t.spawn('sde-taskview', log='taskview.log')
    cards = wait(lambda: taskview_cards(t).get('shown') and taskview_cards(t), 10)
    if not check(bool(cards), 'Task View opens'):
        return
    ws = [k for k in cards if k.startswith('workspace:')]
    check(len(ws) == t.count(), f'a card for each workspace ({ws})')
    wins = [k for k in cards if k.startswith('window:')]
    check(any('Alpha' in k for k in wins) and not any('Charlie' in k for k in wins),
          f'the windows of the workspace in view ({wins})')
    check(all(cards[k][4] == 'picture' for k in wins), 'windows on screen have a picture')
    check('new' in cards, 'a card to add a workspace')
    t.key('Next')
    cards = wait(lambda: taskview_cards(t).get('shown') == '2' and taskview_cards(t), 5)
    check(bool(cards) and any('Charlie' in k for k in cards if k.startswith('window:')),
          'Page Down shows the next workspace\'s windows')
    check(bool(cards) and all(cards[k][4] == 'icon' for k in cards if k.startswith('window:')),
          'windows of another workspace show their icon')
    t.key('Prior')
    cards = wait(lambda: taskview_cards(t).get('shown') == '1' and taskview_cards(t), 5)
    # Drag Alpha onto workspace 2.
    alpha = next((k for k in cards if k.startswith('window:') and 'Alpha' in k), None) if cards else None
    target = next((v for k, v in cards.items() if k.startswith('workspace:2:')), None) if cards else None
    if check(bool(alpha and target), 'Alpha and workspace 2 are on screen'):
        x, y, w, h = cards[alpha][:4]
        tx, ty, tw, th = target[:4]
        t.run('xdotool', 'mousemove', x + w // 2, y + h // 2, 'mousedown', '1')
        for i in range(1, 9):
            t.run('xdotool', 'mousemove', x + w // 2 + (tx + tw // 2 - x - w // 2) * i // 8,
                  y + h // 2 + (ty + th // 3 - y - h // 2) * i // 8)
            time.sleep(0.05)
        t.run('xdotool', 'mouseup', '1')
        check(wait(lambda: t.desktop(a) == 1, 5), 'dragging a window onto workspace 2 moves it there')
    # "New workspace" adds one; Task View stays open.
    cards = taskview_cards(t)
    if 'new' in cards:
        x, y, w, h = cards['new'][:4]
        before = t.count()
        t.run('xdotool', 'mousemove', x + w // 2, y + h // 3, 'click', '1')
        check(wait(lambda: t.count() == before + 1, 5), '"New workspace" adds a workspace')
        check(p.poll() is None, 'Task View stays open')
    # A second sde-taskview closes the first.
    t.run('sde-taskview')
    check(wait(lambda: p.poll() is not None, 5), 'running sde-taskview again closes Task View')
    t.run('sde-windows', 'workspace', 'close', str(t.count()))
    wait(lambda: t.count() == 3, 5)
    # Clicking a window of another workspace switches to it.
    (t.tmp / 'qa/taskview.txt').unlink(missing_ok=True)
    p = t.spawn('sde-taskview', log='taskview.log')
    wait(lambda: taskview_cards(t).get('shown'), 10)
    t.key('Next')
    cards = wait(lambda: taskview_cards(t).get('shown') == '2' and taskview_cards(t), 5)
    win = next((k for k in cards if k.startswith('window:') and 'Alpha' in k), None) if cards else None
    if check(bool(win), 'Alpha is on workspace 2 in Task View'):
        x, y, w, h = cards[win][:4]
        t.run('xdotool', 'mousemove', x + w // 2, y + h // 2, 'click', '1')
        check(wait(lambda: p.poll() is not None, 5), 'a click on a window closes Task View')
        check(wait(lambda: t.current() == 1, 5), 'and goes to the window\'s workspace')
        check(wait(lambda: f'0x{int(a, 16):x}' in t.root('_NET_ACTIVE_WINDOW'), 5), 'and to the window')
    # Esc closes it without changes.
    (t.tmp / 'qa/taskview.txt').unlink(missing_ok=True)
    p = t.spawn('sde-taskview', log='taskview.log')
    wait(lambda: taskview_cards(t).get('shown'), 10)
    t.key('Escape')
    check(wait(lambda: p.poll() is not None, 5), 'Esc closes Task View')
    t.run('xdotool', 'set_desktop_for_window', a, '0')
    t.run('xdotool', 'set_desktop', '0')
    time.sleep(0.4)


def snapping(t, a, b):
    """5. Win+arrows, Win+M, Win+Shift+M, Win+Home."""
    t.activate(a)
    before = t.frame(a)
    t.key('super+Left')
    left = wait(lambda: t.frame(a)[0] <= 2 and t.frame(a)[2] < 700 and t.frame(a), 5)
    check(bool(left), f'Win+Left snaps to the left half ({t.frame(a)})')
    t.key('super+Up')
    check(wait(lambda: t.frame(a)[3] < 500 and t.frame(a)[1] <= 2, 5), f'Win+Up: the top-left quarter ({t.frame(a)})')
    t.key('super+Down')
    check(wait(lambda: t.frame(a)[3] > 600, 5), f'Win+Down: the left half again ({t.frame(a)})')
    t.key('super+Right')
    back = wait(lambda: abs(t.frame(a)[0] - before[0]) < 30 and abs(t.frame(a)[2] - before[2]) < 30 and t.frame(a), 5)
    check(bool(back), f'Win+Right on the left half: back where it was ({t.frame(a)} vs {before})')
    t.key('super+Up')
    check(wait(lambda: '_NET_WM_STATE_MAXIMIZED_HORZ' in t.prop(a, '_NET_WM_STATE'), 5), 'Win+Up maximizes')
    t.key('super+Down')
    check(wait(lambda: '_NET_WM_STATE_MAXIMIZED' not in t.prop(a, '_NET_WM_STATE'), 5), 'Win+Down restores')
    t.key('super+Down')
    check(wait(lambda: '_NET_WM_STATE_HIDDEN' in t.prop(a, '_NET_WM_STATE'), 5), 'Win+Down again minimizes')
    t.run('xdotool', 'windowactivate', a)
    wait(lambda: '_NET_WM_STATE_HIDDEN' not in t.prop(a, '_NET_WM_STATE'), 5)
    # b back on workspace 1 for the minimize checks.
    t.run('xdotool', 'set_desktop_for_window', b, '0')
    time.sleep(0.3)
    t.activate(a)
    t.key('super+m')
    check(wait(lambda: all('_NET_WM_STATE_HIDDEN' in t.prop(w, '_NET_WM_STATE') for w in (a, b)), 5),
          'Win+M minimizes all windows')
    t.key('super+shift+m')
    check(wait(lambda: not any('_NET_WM_STATE_HIDDEN' in t.prop(w, '_NET_WM_STATE') for w in (a, b)), 5),
          'Win+Shift+M brings them back')
    t.activate(a)
    t.key('super+Home')
    check(wait(lambda: '_NET_WM_STATE_HIDDEN' in t.prop(b, '_NET_WM_STATE'), 5) and
          '_NET_WM_STATE_HIDDEN' not in t.prop(a, '_NET_WM_STATE'), 'Win+Home minimizes all but the active window')
    t.key('super+Home')
    check(wait(lambda: '_NET_WM_STATE_HIDDEN' not in t.prop(b, '_NET_WM_STATE'), 5), 'Win+Home again brings them back')


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
        t.spawn(args.wm, '--config-file', t.rc, '--sm-disable', log='wm.log')
        # Seconds on a loaded machine (ctest -j, a fresh font cache).
        if not check(wait(lambda: '_NET_SUPPORTING_WM_CHECK(WINDOW)' in t.root('_NET_SUPPORTING_WM_CHECK'), 30),
                     'the window manager starts'):
            print((t.tmp / 'wm.log').read_text(errors='replace')[-1500:], flush=True)
            return 1
        a, b, c = t.client('Alpha Window'), t.client('Bravo Window'), t.client('Charlie Window')
        if not check(all((a, b, c)), 'three test windows'):
            return 1
        time.sleep(0.5)
        workspace_keys(t, a, b, c)
        control_center(t)
        # Alpha on 1, Bravo and Charlie on 2 from here on.
        t.run('sde-windows', 'workspace', 'close', '3')
        wait(lambda: t.count() == 2, 5)
        for w, d in ((a, '0'), (b, '1'), (c, '1')):
            t.run('xdotool', 'set_desktop_for_window', w, d)
        t.run('xdotool', 'set_desktop', '0')
        time.sleep(0.5)
        taskbar(t, a, b)
        taskview(t, a, b, c)
        snapping(t, a, b)
    finally:
        t.stop()
    print('FAILED: ' + '; '.join(FAIL) if FAIL else 'all workspace checks passed')
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
