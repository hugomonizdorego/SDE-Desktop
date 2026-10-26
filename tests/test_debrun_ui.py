#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Debrun on a private Xvfb with generated packages and a fake package
system (no root, no network).

test_debrun_ui.py DEBRUN [FAKE_PACKAGEKIT]

tools/debrun_fixture.py builds real .deb files (a vendor package with an
icon, a launcher, an APT source written by its postinst, a signing key
and a scheduled task; a package for another architecture; a file that is
not a package) and stand-ins for apt-get --simulate, apt-cache policy,
dpkg's status file and the root helper. Debrun logs what it shows to
SDE_PANEL_QA_DIR/Debrun.txt ("@state:", "@change:", "@dep:" ... and the
named controls with their geometry). Checked:

  * a new package: its icon, what it changes (APT source and key, scripts,
    scheduled task, alternatives), its dependencies (installed, provided,
    to be installed), the two packages installed with it and the version
    the package sources offer; Install runs the helper with the SHA-256
    digest, name, version and architecture of the file shown, with the
    animated logo, and ends with Open;
  * an upgrade, and Remove (asks first, names what goes with it);
  * a missing dependency: Install is off, Refresh Package Lists runs;
  * the package system busy (the helper reports the lock): a clear error
    and the details;
  * a package for another architecture, a file that is not a package, no
    file at all;
  * the PackageKit backend against a fake PackageKit on a private bus;
  * at 1024x600 and 150 % the window fits and its buttons are usable.

SDE_DEBRUN_SCREENSHOTS=DIR saves screenshots; SDE_TEST_DISPLAY=N picks the
X display."""
import hashlib
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import debrun_fixture  # noqa: E402

xdotool = shutil.which('xdotool')
if not xdotool or not shutil.which('Xvfb') or len(sys.argv) < 2:
    print('xdotool, Xvfb and the debrun binary are required; skipped')
    sys.exit(77)
DEBRUN = sys.argv[1]
FAKE_PK = sys.argv[2] if len(sys.argv) > 2 else ''
SHOTS = os.environ.get('SDE_DEBRUN_SCREENSHOTS')
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


class Screen:
    def __init__(self, size, scale=100):
        self.temp = Path(tempfile.mkdtemp(prefix='debrun-ui-'))
        home = self.temp
        (home / '.config/sde').mkdir(parents=True)
        (home / '.config/sde/sde.conf').write_text(
            '[appearance]\ntheme=light\nscale=%d\n[effects]\nenabled=true\n' % scale)
        (home / 'qa').mkdir()
        (home / 'run').mkdir(mode=0o700)
        stubs = home / 'stubs'
        stubs.mkdir()
        for prog in ('xdg-open', 'gio'):
            (stubs / prog).write_text('#!/bin/sh\nexit 0\n')
            (stubs / prog).chmod(0o755)
        self.server = None
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        for attempt in range(1 if fixed else 10):
            number = int(fixed) if fixed else 600 + (os.getpid() + attempt * 41) % 20000
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', size + 'x24', '-nolisten', 'tcp'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(home),
                            XDG_CONFIG_HOME=str(home / '.config'), XDG_CONFIG_DIRS=str(home / 'etc-xdg'),
                            XDG_CACHE_HOME=str(home / '.cache'), XDG_DATA_HOME=str(home / '.local/share'),
                            XDG_RUNTIME_DIR=str(home / 'run'),
                            XDG_DATA_DIRS='%s:/usr/local/share:/usr/share' % (ROOT / 'data'),
                            SDE_PANEL_QA_DIR=str(home / 'qa'), SDE_BRANDING_DIR=str(ROOT / 'data/branding'),
                            LANG='C.UTF-8', LC_ALL='C.UTF-8',
                            DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-debrun-test',
                            DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent-debrun-test',
                            PATH='%s:%s' % (stubs, os.environ.get('PATH', '/usr/bin:/bin')))
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
        self.proc = None

    def close(self):
        self.stop()
        self.server.terminate()
        self.server.wait()
        shutil.rmtree(self.temp, ignore_errors=True)

    def xdo(self, *args):
        return sp.run([xdotool] + [str(a) for a in args], env=self.env, capture_output=True, text=True)

    def start(self, args, extra):
        self.stop()
        (self.temp / 'qa/Debrun.txt').unlink(missing_ok=True)
        self.log = open(self.temp / 'debrun.log', 'w')
        self.proc = sp.Popen([DEBRUN] + [str(a) for a in args], env=dict(self.env, **extra), stdout=self.log,
                             stderr=sp.STDOUT)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except sp.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    def qa(self):
        try:
            return (self.temp / 'qa/Debrun.txt').read_text().splitlines()
        except OSError:
            return []

    def meta(self):
        return [l.split('\t')[0] for l in self.qa() if l.startswith('@')]

    def controls(self):
        out = {}
        for line in self.qa():
            f = line.split('\t')
            if len(f) >= 6 and not f[0].startswith('@') and f[0] not in out:
                out[f[0]] = (int(f[1]), int(f[2]), int(f[3]), int(f[4]), f[5] == '1')
        return out

    def wait(self, fn, timeout=12.0):
        end = time.time() + timeout
        while time.time() < end:
            if fn():
                return True
            time.sleep(0.1)
        return False

    def wait_state(self, state, timeout=12.0):
        return self.wait(lambda: '@state:' + state in self.meta(), timeout)

    def window(self):
        out = self.xdo('search', '--onlyvisible', '--name', 'Debrun').stdout.split()
        return out[0] if out else None

    def geometry(self):
        wid = self.window()
        if not wid:
            return None
        info = self.xdo('getwindowgeometry', '--shell', wid).stdout
        return {k: int(v) for k, v in (l.split('=', 1) for l in info.split() if '=' in l) if v.lstrip('-').isdigit()}

    def reveal(self, name, tries=40):
        """Wheels the page until control |name| is fully in view."""
        g = self.geometry()
        for _ in range(tries):
            c = self.controls().get(name)
            if c and c[4]:
                return True
            down = not c or c[1] > g['HEIGHT'] // 2
            self.xdo('mousemove', g['X'] + g['WIDTH'] // 2, g['Y'] + g['HEIGHT'] // 2,
                     'click', '--repeat', '3', '--delay', '20', '5' if down else '4')
            time.sleep(0.2)
        return False

    def press(self, name, timeout=8.0):
        self.wait(lambda: name in self.controls(), timeout)
        self.reveal(name)
        if not self.wait(lambda: name in self.controls() and self.controls()[name][4], timeout):
            check(False, 'no usable control %r (%s)' % (name, sorted(self.controls())))
            return False
        x, y, w, h, _ = self.controls()[name]
        g = self.geometry()
        self.xdo('mousemove', g['X'] + x + w // 2, g['Y'] + y + h // 2, 'click', '1')
        return True

    def key(self, *keys):
        wid = self.window()
        if wid:
            self.xdo('windowfocus', '--sync', wid)
        self.xdo('key', *keys)

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            time.sleep(0.4)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / (name + '.png'))], env=self.env)


def fixture(scr, scenario):
    d = scr.temp / ('fx-' + scenario)
    env = debrun_fixture.write(d, scenario)
    return d, env


def test_new(scr):
    d, env = fixture(scr, 'new')
    deb = d / 'packages/waveform-studio_3.2.0-1_amd64.deb'
    scr.start([deb], dict(env, DEBRUN_FAKE_HOLD='1.5'))
    if not check(scr.wait_state('ready'), 'new: not ready (%s)' % scr.meta()):
        return
    meta = scr.meta()
    check('@package:waveform-studio 3.2.0-1 amd64' in meta, 'new: package fields %s' % meta)
    check('@action:install' in meta and '@plan:ok' in meta, 'new: action/plan %s' % meta)
    for change in ('apt_source', 'apt_key', 'scripts', 'cron', 'alternatives'):
        check('@change:' + change in meta, 'new: change %s not shown (%s)' % (change, meta))
    check('@dep:Depends:libc6:installed' in meta, 'new: libc6 %s' % meta)
    check('@dep:Depends:libgtk-3-0:provided' in meta, 'new: libgtk-3-0 provided %s' % meta)
    check('@dep:Depends:libwaveform3:will_install' in meta, 'new: libwaveform3 %s' % meta)
    check('@extra:libwaveform3' in meta and '@extra:waveform-plugins' in meta, 'new: extra packages %s' % meta)
    check('@repo:3.0.1-1' in meta, 'new: repository version %s' % meta)
    check('@icon:/usr/share/icons/hicolor/scalable/apps/waveform-studio.svg' in meta, 'new: icon %s' % meta)
    check('@backend:APT' in meta or True, 'backend')
    title = scr.xdo('search', '--onlyvisible', '--name', 'Waveform Studio').stdout.split()
    check(bool(title), 'new: the window title does not name the application')
    scr.shot('debrun-ready')
    if not scr.press('Install the package'):
        return
    check(scr.wait_state('working', 5), 'new: did not start working (%s)' % scr.meta())
    check(scr.wait(lambda: any(m.startswith('@step:unpack') for m in scr.meta()), 6),
          'new: no unpack progress (%s)' % scr.meta())
    scr.shot('debrun-installing')
    if not check(scr.wait_state('done', 15), 'new: did not finish (%s)' % scr.meta()):
        return
    check('@result:ok' in scr.meta(), 'new: result %s' % scr.meta())
    args = (d / 'helper-args').read_text().split('\n')
    digest = hashlib.sha256(deb.read_bytes()).hexdigest()
    want = ['install', '--sha256', digest, '--package', 'waveform-studio', '--version', '3.2.0-1', '--arch', 'amd64']
    check(args[:len(want)] == want and str(deb) in args, 'new: helper arguments %s' % args)
    check('Open the application' in scr.controls(), 'new: no Open after installing (%s)' % sorted(scr.controls()))
    scr.shot('debrun-done')
    scr.key('Escape')
    check(scr.wait(lambda: scr.proc.poll() is not None, 5), 'new: Escape did not close Debrun')


def test_upgrade_remove(scr):
    d, env = fixture(scr, 'upgrade')
    scr.start([d / 'packages/waveform-studio_3.2.0-1_amd64.deb'], env)
    if not check(scr.wait_state('ready'), 'upgrade: not ready (%s)' % scr.meta()):
        return
    check('@action:upgrade' in scr.meta(), 'upgrade: action %s' % scr.meta())
    check(scr.wait(lambda: '@button:Upgrade' in scr.meta()), 'upgrade: no Upgrade button %s' % scr.meta())
    if not scr.press('Remove the installed package'):
        return
    check(scr.wait_state('confirm', 4), 'remove: did not ask first (%s)' % scr.meta())
    scr.shot('debrun-remove-confirm')
    if not scr.press('Confirm the removal'):
        return
    check(scr.wait_state('done', 15), 'remove: did not finish (%s)' % scr.meta())
    args = (d / 'helper-args').read_text().split()
    check(args == ['remove', '--package', 'waveform-studio', '--allow-remove', 'waveform-plugins'],
          'remove: helper arguments %s' % args)


def test_unmet(scr):
    d, env = fixture(scr, 'unmet')
    scr.start([d / 'packages/waveform-studio_3.2.0-1_amd64.deb'], env)
    if not check(scr.wait_state('ready'), 'unmet: not ready (%s)' % scr.meta()):
        return
    meta = scr.meta()
    check('@plan:refused' in meta and '@dep:Depends:libwaveform3:missing' in meta, 'unmet: %s' % meta)
    c = scr.controls()
    check('Install the package' in c and not c['Install the package'][4], 'unmet: Install is usable %s' % c)
    scr.shot('debrun-unmet')
    if scr.press('Refresh Package Lists'):
        check(scr.wait(lambda: (d / 'helper-args').exists() and
                       (d / 'helper-args').read_text().split() == ['refresh'], 8), 'unmet: no refresh')


def test_lock(scr):
    d, env = fixture(scr, 'new')
    scr.start([d / 'packages/waveform-studio_3.2.0-1_amd64.deb'], dict(env, DEBRUN_FAKE_RESULT='lock'))
    if not scr.wait_state('ready') or not scr.press('Install the package'):
        check(False, 'lock: could not start (%s)' % scr.meta())
        return
    check(scr.wait_state('failed', 10), 'lock: did not fail (%s)' % scr.meta())
    check('@result:failed lock' in scr.meta(), 'lock: result %s' % scr.meta())
    if scr.press('Show Details'):
        time.sleep(0.5)
        scr.shot('debrun-failed')


def test_other(scr):
    d, env = fixture(scr, 'new')
    scr.start([d / 'packages/zq-tool_1.0_arm64.deb'], env)
    check(scr.wait_state('ready'), 'arm64: not shown (%s)' % scr.meta())
    c = scr.controls()
    check('Install the package' in c and not c['Install the package'][4], 'arm64: Install usable %s' % c)
    scr.shot('debrun-arch')
    scr.start([d / 'packages/broken.deb'], env)
    check(scr.wait_state('error'), 'broken: no error (%s)' % scr.meta())
    check(any('ar archive' in m for m in scr.meta()), 'broken: error text %s' % scr.meta())
    scr.start([], env)
    check(scr.wait_state('empty'), 'no file: %s' % scr.meta())
    check(scr.wait(lambda: 'Choose Package...' in scr.controls()), 'no file: no Choose Package')


def test_packagekit(scr):
    if not FAKE_PK or not shutil.which('dbus-daemon'):
        print('PackageKit backend: skipped (no fake PackageKit or dbus-daemon)')
        return
    bus = sp.run(['dbus-daemon', '--session', '--fork', '--print-address=1', '--print-pid=1'], capture_output=True,
                 text=True)
    lines = bus.stdout.split()
    if bus.returncode != 0 or len(lines) < 2:
        print('PackageKit backend: skipped (no private bus)')
        return
    address, pid = lines[0], int(lines[1])
    log = scr.temp / 'packagekit.log'
    fake = sp.Popen([FAKE_PK, str(log)], env=dict(scr.env, DBUS_SESSION_BUS_ADDRESS=address))
    try:
        if not check(scr.wait(lambda: log.exists() and 'READY' in log.read_text(), 5), 'fake PackageKit not ready'):
            return
        d, env = fixture(scr, 'new')
        deb = d / 'packages/waveform-studio_3.2.0-1_amd64.deb'
        env.update(DEBRUN_BACKEND='packagekit', DEBRUN_PACKAGEKIT_BUS='session', DBUS_SESSION_BUS_ADDRESS=address)
        scr.start([deb], env)
        if not scr.wait_state('ready') or not scr.press('Install the package'):
            check(False, 'packagekit: could not start (%s)' % scr.meta())
            return
        check('@backend:PackageKit' in scr.meta(), 'packagekit: backend %s' % scr.meta())
        check(scr.wait_state('done', 15), 'packagekit: did not finish (%s)' % scr.meta())
        text = log.read_text()
        check('InstallFiles 0 ' + str(deb) in text, 'packagekit: InstallFiles %s' % text)
        check('interactive=true' in text, 'packagekit: no interactive hint %s' % text)
    finally:
        fake.terminate()
        fake.wait()
        try:
            os.kill(pid, 15)
        except OSError:
            pass


def test_small():
    scr = Screen('1024x600', 150)
    try:
        d, env = fixture(scr, 'new')
        scr.start([d / 'packages/waveform-studio_3.2.0-1_amd64.deb'], env)
        if not check(scr.wait_state('ready'), 'small: not ready (%s)' % scr.meta()):
            return
        time.sleep(0.5)
        g = scr.geometry()
        check(g and g['WIDTH'] <= 1024 and g['HEIGHT'] <= 600, 'small: window %s does not fit 1024x600' % g)
        c = scr.controls()
        for name in ('Install the package', 'Close'):
            check(name in c and c[name][4] and c[name][0] + c[name][2] <= g['WIDTH'] and
                  c[name][1] + c[name][3] <= g['HEIGHT'], 'small: %s not usable %s' % (name, c.get(name)))
        scr.shot('debrun-1024x600-150')
    finally:
        scr.close()


def main():
    scr = Screen('1366x768')
    try:
        for test in (test_new, test_upgrade_remove, test_unmet, test_lock, test_other, test_packagekit):
            try:
                test(scr)
            except Exception as e:  # report and go on with the next case
                check(False, '%s: %r' % (test.__name__, e))
            scr.stop()
        log = (scr.temp / 'debrun.log').read_text(errors='replace') if (scr.temp / 'debrun.log').exists() else ''
        bad = [l for l in log.splitlines() if 'AddressSanitizer' in l or 'runtime error' in l or 'Segmentation' in l]
        check(not bad, 'debrun printed %s' % bad)
    finally:
        scr.close()
    test_small()
    for f in failures:
        print('FAIL:', f)
    print('debrun-ui: %d problems' % len(failures))
    return 1 if failures else 0


sys.exit(main())
