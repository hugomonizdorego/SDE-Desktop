#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Control Center > Users & Avatar: renaming an account, on a private Xvfb
and a private D-Bus with the fake AccountsService of
sde-panel-service-fixture (the people sam and mia), and the real
sde-account-rename working on a fake root and a request directory of the
test (SDE_ACCOUNT_RENAME_HELPER, _ROOT and _DIR: no pkexec, nothing of this
computer changes).

test_account_rename_ui.py SDE_SETTINGS SDE_ACCOUNT_RENAME SERVICE_FIXTURE

Checked: sam's card has Rename...; a name that is taken is refused in the
form; "samuel" with the link is recorded by the helper (the request names
the new home folder and the link), the card and the list say "Will be
renamed to samuel at the next restart" with Cancel, which removes the
request; a failed rename (mia) shows why and Dismiss forgets it; at
1024x600 and 150 % the form fits.

SDE_RENAME_SCREENSHOTS=DIR saves screenshots; SDE_TEST_DISPLAY=N picks the
X display."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
xdotool = shutil.which('xdotool')
if len(sys.argv) < 4 or not xdotool or not shutil.which('Xvfb') or not shutil.which('dbus-daemon'):
    print('xdotool, Xvfb, dbus-daemon and the three programs are required; skipped')
    sys.exit(77)
SETTINGS, HELPER, FIXTURE = sys.argv[1:4]
SHOTS = os.environ.get('SDE_RENAME_SCREENSHOTS')
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


def fake_root(path):
    """The accounts the helper checks against (uids as the fixture's)."""
    for d in ('etc', 'home/sam/Documents', 'home/mia'):
        (path / d).mkdir(parents=True, exist_ok=True)
    (path / 'etc/passwd').write_text('root:x:0:0:root:/root:/bin/bash\n'
                                     'sam:x:1001:1001:Sam Rivera:/home/sam:/bin/bash\n'
                                     'mia:x:1002:1002:Mia Chen:/home/mia:/bin/bash\n')
    (path / 'etc/group').write_text('root:x:0:\nsudo:x:27:sam\nsam:x:1001:\nmia:x:1002:\n')


class Session:
    def __init__(self, size='1366x768', scale=100):
        self.temp = Path(tempfile.mkdtemp(prefix='rename-ui-'))
        home = self.temp
        (home / '.config/sde').mkdir(parents=True)
        (home / '.config/sde/sde.conf').write_text(
            '[appearance]\ntheme=light\nscale=%d\n[effects]\nenabled=true\n' % scale)
        (home / 'qa').mkdir()
        (home / 'run').mkdir(mode=0o700)
        self.root = home / 'fakeroot'
        fake_root(self.root)
        self.requests = home / 'requests'
        self.width, self.height = (int(v) for v in size.split('x'))
        self.procs = []
        bus = sp.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'], stdout=sp.PIPE, text=True)
        self.procs.append(bus)
        address = bus.stdout.readline().strip()
        self.server = None
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        for attempt in range(1 if fixed else 10):
            number = int(fixed) if fixed else 600 + (os.getpid() + attempt * 71) % 20000
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', size + 'x24', '-nolisten', 'tcp'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(home),
                            XDG_CONFIG_HOME=str(home / '.config'), XDG_CONFIG_DIRS=str(home / 'etc-xdg'),
                            XDG_CACHE_HOME=str(home / '.cache'), XDG_DATA_HOME=str(home / '.local/share'),
                            XDG_RUNTIME_DIR=str(home / 'run'),
                            XDG_DATA_DIRS='%s:/usr/local/share:/usr/share' % (ROOT / 'data'),
                            SDE_PANEL_QA_DIR=str(home / 'qa'), SDE_BRANDING_DIR=str(ROOT / 'data/branding'),
                            SDE_AVATAR_DIR=str(ROOT / 'data/avatars'), LANG='C.UTF-8', LC_ALL='C.UTF-8',
                            DBUS_SESSION_BUS_ADDRESS=address, DBUS_SYSTEM_BUS_ADDRESS=address,
                            SDE_ACCOUNT_RENAME_HELPER=HELPER, SDE_ACCOUNT_RENAME_DIR=str(self.requests),
                            SDE_ACCOUNT_RENAME_ROOT=str(self.root))
            for _ in range(100):
                if server.poll() is not None:
                    break
                if sp.run([xdotool, 'getmouselocation'], env=self.env, capture_output=True).returncode == 0:
                    self.server = server
                    break
                time.sleep(0.05)
            if self.server:
                break
            if server.poll() is None:
                server.terminate()
                server.wait()
        if not self.server:
            raise RuntimeError('Xvfb did not start')
        self.procs.append(self.server)
        log = home / 'accounts.log'
        fixture = sp.Popen([FIXTURE, str(log)], env=dict(self.env, SDE_FIXTURE_SERVICES='accounts'),
                           stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        self.procs.insert(0, fixture)
        for _ in range(80):
            if log.exists() and 'READY' in log.read_text():
                break
            time.sleep(0.1)
        else:
            raise RuntimeError('the AccountsService fixture did not start')
        self.app = None

    def start(self):
        self.app = sp.Popen([SETTINGS, '--page', 'users'], env=self.env, stdout=open(self.temp / 'settings.log', 'w'),
                            stderr=sp.STDOUT)
        self.procs.insert(0, self.app)

    def close(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except sp.TimeoutExpired:
                    p.kill()
                    p.wait()
        shutil.rmtree(self.temp, ignore_errors=True)

    def xdo(self, *args):
        return sp.run([xdotool] + [str(a) for a in args], env=self.env, capture_output=True, text=True)

    def lines(self):
        try:
            return [l.split('\t') for l in (self.temp / 'qa/Control-Center.txt').read_text().splitlines()]
        except OSError:
            return []

    def names(self):
        return [f[0] for f in self.lines()]

    def control(self, name):
        for f in self.lines():
            if f[0] == name and len(f) >= 6:
                return int(f[1]), int(f[2]), int(f[3]), int(f[4]), f[5] == '1'
        return None

    def wait(self, fn, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if fn():
                return True
            time.sleep(0.1)
        return False

    def window(self):
        out = self.xdo('search', '--onlyvisible', '--name', '^Control Center$').stdout.split()
        return out[-1] if out else None

    def geometry(self):
        wid = self.window()
        if not wid:
            return None
        info = self.xdo('getwindowgeometry', '--shell', wid).stdout
        return {k: int(v) for k, v in (l.split('=', 1) for l in info.split() if '=' in l) if v.lstrip('-').isdigit()}

    def reveal(self, name):
        g = self.geometry()
        for _ in range(60):
            c = self.control(name)
            if c and c[4]:
                return True
            down = not c or c[1] > g['HEIGHT'] // 2
            far = 1 if not c else max(1, min(10, abs(c[1] - g['HEIGHT'] // 2) // 60))
            self.xdo('mousemove', g['X'] + g['WIDTH'] - 40, g['Y'] + g['HEIGHT'] // 2,
                     'click', '--repeat', far, '--delay', 15, '5' if down else '4')
            time.sleep(0.15)
        return False

    def press(self, name):
        if not self.wait(lambda: self.control(name) is not None):
            check(False, 'no control %r (%s)' % (name, self.names()))
            return False
        if not check(self.reveal(name), 'cannot scroll to %r' % name):
            return False
        x, y, w, h, _ = self.control(name)
        g = self.geometry()
        self.xdo('mousemove', g['X'] + x + w // 2, g['Y'] + y + h // 2, 'click', '1')
        time.sleep(0.4)
        return True

    def type(self, text):
        self.xdo('type', '--delay', '30', text)

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            time.sleep(0.5)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / (name + '.png'))], env=self.env)


def request(s, uid):
    try:
        return dict(l.split('=', 1) for l in (s.requests / ('%d.rename' % uid)).read_text().splitlines()
                    if '=' in l and not l.startswith('#'))
    except OSError:
        return None


def test_rename(s):
    s.start()
    if not check(s.wait(lambda: 'user:sam' in s.names(), 15), 'the accounts are not listed (%s)' % s.names()):
        return
    s.press('user:sam')
    if not check(s.wait(lambda: 'Rename this account' in s.names()), "sam's card has no Rename... (%s)" % s.names()):
        return
    s.press('Rename this account')
    if not check(s.wait(lambda: 'New user name' in s.names()), 'no rename form (%s)' % s.names()):
        return
    # A name in use: refused before the helper is asked.
    s.press('New user name')
    s.type('mia')
    s.press('Rename at Restart')
    time.sleep(0.6)
    check(request(s, 1001) is None, 'a taken name was recorded')
    s.press('New user name')
    s.xdo('key', 'ctrl+a', 'BackSpace')
    s.type('samuel')
    s.press('Link from the old folder')
    time.sleep(0.3)
    s.shot('rename-form')
    s.press('Rename at Restart')
    if not check(s.wait(lambda: request(s, 1001) is not None), 'the helper recorded nothing'):
        return
    r = request(s, 1001)
    check(r.get('old') == 'sam' and r.get('new') == 'samuel' and r.get('new_home') == '/home/samuel' and
          r.get('link') == '1' and r.get('state') == 'pending', 'request %s' % r)
    check(s.wait(lambda: '@rename:sam:samuel:pending' in s.names()), 'the card does not show it (%s)' % s.names())
    check(s.wait(lambda: 'Cancel the rename' in s.names()), 'no Cancel (%s)' % s.names())
    s.reveal('Cancel the rename')
    s.shot('rename-pending')
    s.press('Cancel the rename')
    check(s.wait(lambda: request(s, 1001) is None), 'Cancel did not remove the request')
    check(s.wait(lambda: '@rename:sam:samuel:pending' not in s.names()), 'still shown after Cancel')
    check(s.wait(lambda: 'Rename this account' in s.names()), 'no Rename... after Cancel')
    time.sleep(1.0)  # the list is read again after a change
    # A rename that failed at the last start: why, and Dismiss.
    (s.requests / '1002.rename').write_text(
        'uid=1002\nold=mia\nnew=maria\nold_home=/home/mia\nnew_home=/home/maria\nlink=0\nby=0\ntime=1\n'
        'state=failed\nerror=usermod: user mia is currently used by process 4242\n')
    for _ in range(3):
        s.press('user:mia')
        if s.wait(lambda: 'Full name of mia' in s.names(), 4):
            break
    check(s.wait(lambda: '@rename:mia:maria:failed' in s.names()), 'the failed rename is not shown (%s)' % s.names())
    if check(s.wait(lambda: 'Forget the failed rename' in s.names()), 'no Dismiss (%s)' % s.names()):
        s.reveal('Forget the failed rename')
        s.shot('rename-failed')
        s.press('Forget the failed rename')
        check(s.wait(lambda: request(s, 1002) is None), 'Dismiss did not remove the failed request')


def test_small():
    s = Session('1024x600', 150)
    try:
        s.start()
        if not check(s.wait(lambda: 'user:sam' in s.names(), 15), 'small: no accounts'):
            return
        s.press('user:sam')
        s.press('Rename this account')
        if not check(s.wait(lambda: 'Rename at Restart' in s.names()), 'small: no form'):
            return
        g = s.geometry()
        for name in ('New user name', 'Rename at Restart', 'Link from the old folder'):
            check(s.reveal(name), 'small: %s cannot be reached' % name)
            c = s.control(name)
            check(c and c[0] >= 0 and c[0] + c[2] <= g['WIDTH'], 'small: %s sideways %s' % (name, c))
        s.shot('rename-1024x600-150')
    finally:
        s.close()


def main():
    s = Session()
    try:
        test_rename(s)
    finally:
        s.close()
    test_small()
    print('account-rename-ui: %d problem(s)' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
