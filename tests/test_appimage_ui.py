#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""sde-appimage with generated AppImages (tools/appimage_fixture.py), real
GnuPG signatures (throwaway keys) and its window on a private Xvfb.

test_appimage_ui.py SDE_APPIMAGE

Without a display:
  * --check: a signed AppImage is trusted (its key and signer named), the
    next version signed with the same key too; another key for the same
    application is refused and both keys are named; a changed AppImage has
    a bad signature; a signature without its key, no signature, the old
    type 1, another architecture and no AppImage at all are refused; the
    second check uses the fast path;
  * a trusted AppImage that is not executable runs (with its arguments),
    becomes executable for its owner and the log names it;
  * --forget: the next version is trusted on first use again;
  * an AppImage that cannot be verified does not run without a display
    either; its explanation goes to stderr.
On Xvfb (the window's controls in SDE_PANEL_QA_DIR/AppImage.txt):
  * no signature: the reason, Delete (focused, the default) and Keep, no
    Run Anyway; Escape keeps it and takes its executable bits; Enter moves
    it to the trash;
  * another key: both keys; a click on Delete moves it to the trash;
  * policy=ask: a changed AppImage still cannot run; an unsigned one runs
    with Run Anyway, and then without asking;
  * at 1024x600 and 150 % the window fits the screen.

Without gpg the signature cases are skipped. SDE_APPIMAGE_SCREENSHOTS=DIR
saves screenshots; SDE_TEST_DISPLAY=N picks the X display."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import appimage_fixture as fx  # noqa: E402

if len(sys.argv) < 2:
    print('usage: test_appimage_ui.py SDE_APPIMAGE')
    sys.exit(2)
PROGRAM = sys.argv[1]
xdotool = shutil.which('xdotool')
SHOTS = os.environ.get('SDE_APPIMAGE_SCREENSHOTS')
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


class Home:
    """A home directory of its own, the fixtures and the program's environment."""

    def __init__(self, signer, scale=100):
        self.temp = Path(tempfile.mkdtemp(prefix='appimage-ui-'))
        (self.temp / '.config/sde').mkdir(parents=True)
        (self.temp / '.config/sde/sde.conf').write_text(
            '[appearance]\ntheme=light\nscale=%d\n[effects]\nenabled=true\n' % scale)
        (self.temp / 'qa').mkdir()
        (self.temp / 'run').mkdir(mode=0o700)
        self.policy = self.temp / 'appimage.conf'
        self.policy.write_text('policy=verified-only\n')
        self.files = fx.write(self.temp / 'Downloads', signer)
        self.env = dict(os.environ, HOME=str(self.temp), XDG_CONFIG_HOME=str(self.temp / '.config'),
                        XDG_CONFIG_DIRS=str(self.temp / 'etc-xdg'), XDG_CACHE_HOME=str(self.temp / '.cache'),
                        XDG_DATA_HOME=str(self.temp / '.local/share'), XDG_STATE_HOME=str(self.temp / '.local/state'),
                        XDG_RUNTIME_DIR=str(self.temp / 'run'),
                        XDG_DATA_DIRS='%s:/usr/local/share:/usr/share' % (ROOT / 'data'),
                        SDE_PANEL_QA_DIR=str(self.temp / 'qa'), SDE_BRANDING_DIR=str(ROOT / 'data/branding'),
                        SDE_APPIMAGE_POLICY=str(self.policy), LANG='C.UTF-8', LC_ALL='C.UTF-8', GIO_USE_VFS='local',
                        DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-appimage-test')
        self.env.pop('SDE_APPIMAGE_TRUST', None)
        self.env.pop('SDE_APPIMAGE_GPGV', None)
        self.env['DISPLAY'] = ''

    def close(self):
        shutil.rmtree(self.temp, ignore_errors=True)

    def run(self, *args, timeout=30):
        return sp.run([PROGRAM] + [str(a) for a in args], env=self.env, capture_output=True, text=True,
                      timeout=timeout)

    def verdict(self, name):
        r = self.run('--check', self.files[name])
        lines = r.stdout.splitlines()
        info = dict(l.split(' ', 1) for l in lines[1:] if ' ' in l)
        return (lines[0] if lines else ''), info, r.returncode

    def log(self):
        try:
            return (self.temp / '.local/state/sde/appimage.log').read_text()
        except OSError:
            return ''

    def trashed(self, path):
        return (self.temp / '.local/share/Trash/files' / Path(path).name).exists()


class Screen:
    def __init__(self, home, size='1366x768'):
        self.home = home
        self.server = None
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        for attempt in range(1 if fixed else 10):
            number = int(fixed) if fixed else 600 + (os.getpid() + attempt * 53) % 20000
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', size + 'x24', '-nolisten', 'tcp'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(home.env, DISPLAY=':%d' % number)
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
        self.width, self.height = (int(v) for v in size.split('x'))
        self.proc = None

    def close(self):
        self.stop()
        self.server.terminate()
        self.server.wait()

    def xdo(self, *args):
        return sp.run([xdotool] + [str(a) for a in args], env=self.env, capture_output=True, text=True)

    def start(self, path, *args):
        self.stop()
        (self.home.temp / 'qa/AppImage.txt').unlink(missing_ok=True)
        self.out = self.home.temp / 'output.txt'
        self.log = open(self.out, 'w')
        self.proc = sp.Popen([PROGRAM, str(path)] + list(args), env=self.env, stdout=self.log, stderr=sp.STDOUT)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except sp.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    def finished(self, timeout=8.0):
        try:
            self.proc.wait(timeout=timeout)
        except sp.TimeoutExpired:
            return None
        self.log.close()
        return self.proc.returncode

    def output(self):
        return self.out.read_text()

    def qa(self):
        try:
            return (self.home.temp / 'qa/AppImage.txt').read_text().splitlines()
        except OSError:
            return []

    def meta(self):
        return [l.split('\t')[0] for l in self.qa() if l.startswith('@')]

    def controls(self):
        out = {}
        for line in self.qa():
            f = line.split('\t')
            if len(f) >= 6 and not f[0].startswith('@'):
                out[f[0]] = (int(f[1]), int(f[2]), int(f[3]), int(f[4]))
        return out

    def wait(self, fn, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if fn():
                return True
            time.sleep(0.1)
        return False

    def dialog(self):
        return self.wait(lambda: any(m.startswith('@focus:') for m in self.meta()))

    def window(self):
        out = self.xdo('search', '--onlyvisible', '--name', 'AppImage Runner').stdout.split()
        return out[0] if out else None

    def geometry(self):
        wid = self.window()
        if not wid:
            return None
        info = self.xdo('getwindowgeometry', '--shell', wid).stdout
        return {k: int(v) for k, v in (l.split('=', 1) for l in info.split() if '=' in l) if v.lstrip('-').isdigit()}

    def key(self, *keys):
        wid = self.window()
        if wid:
            self.xdo('windowfocus', '--sync', wid)
        self.xdo('key', *keys)

    def click(self, name):
        c = self.controls().get(name)
        g = self.geometry()
        if not check(c and g, 'no button %r (%s)' % (name, sorted(self.controls()))):
            return False
        self.xdo('mousemove', g['X'] + c[0] + c[2] // 2, g['Y'] + c[1] + c[3] // 2, 'click', '1')
        return True

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            time.sleep(0.4)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / (name + '.png'))], env=self.env)


def test_checks(home, signed):
    v, info, code = home.verdict('unsigned')
    check(v == 'unsigned' and code == 1 and info.get('app') == 'name:notes', 'unsigned: %s %s' % (v, info))
    check(home.verdict('type1')[0] == 'type1', 'type 1: %s' % (home.verdict('type1'),))
    if os.uname().machine == 'x86_64':
        check(home.verdict('arm')[0] == 'wrong-arch', 'aarch64: %s' % (home.verdict('arm'),))
    v, info, code = home.verdict('plain')
    check(v == 'not-appimage' and info.get('error') == 'no AppImage magic bytes', 'no magic: %s %s' % (v, info))
    if not signed:
        return
    v, info, code = home.verdict('signed')
    check(v == 'trusted' and code == 0, 'signed: %s %s' % (v, info))
    check(info.get('signer') == fx.PUBLISHER and info.get('how') == 'signed', 'signed: %s' % info)
    key = info.get('key', '')
    check(len(key) == 40, 'signed: fingerprint %r' % key)
    v, info, code = home.verdict('signed')
    check(v == 'trusted' and info.get('how') == 'known (cached)', 'signed again: %s %s' % (v, info))
    v, info, code = home.verdict('next')
    check(v == 'trusted' and info.get('key') == key and info.get('how') == 'signed', 'next version: %s %s' % (v, info))
    v, info, code = home.verdict('other-key')
    check(v == 'key-changed' and info.get('previous') == key and info.get('key') not in ('', key),
          'other key: %s %s' % (v, info))
    v, info, code = home.verdict('tampered')
    check(v == 'bad-signature' and info.get('error') == 'bad', 'tampered: %s %s' % (v, info))
    check(home.verdict('no-key')[0] == 'no-key', 'no key: %s' % (home.verdict('no-key'),))
    store = home.temp / '.local/share/sde/appimage/trust.tsv'
    check(store.exists() and store.stat().st_mode & 0o777 == 0o600, 'trust store %s' % store)
    check(store.parent.exists() and store.parent.stat().st_mode & 0o777 == 0o700, 'trust store directory mode')

    # A trusted AppImage runs, with its arguments, and becomes executable.
    path = home.files['next']
    check(path.stat().st_mode & 0o111 == 0, 'the fixture is executable already')
    r = home.run(path, 'hello world')
    check(r.returncode == 0 and r.stdout == fx.MARKER + 'hello world\n', 'run: %d %r %r' % (r.returncode, r.stdout,
                                                                                         r.stderr))
    check(path.stat().st_mode & 0o100, 'run: not executable for its owner afterwards')
    check(path.stat().st_mode & 0o011 == 0, 'run: executable for others')
    check('run %s sha256 ' % path in home.log(), 'run: not logged (%s)' % home.log())

    # Forgetting the application: the other key is trusted on first use.
    r = home.run('--forget', home.files['signed'])
    check(r.returncode == 0, 'forget: %s' % r.stdout)
    v, info, code = home.verdict('other-key')
    check(v == 'trusted', 'after --forget: %s %s' % (v, info))
    v, info, code = home.verdict('next')
    check(v == 'trusted' and info.get('how', '').startswith('known'), 'after --forget, known contents: %s' % info)
    home.run('--forget', home.files['other-key'])

    # No display: refused, explained on stderr.
    r = home.run(home.files['tampered'])
    check(r.returncode == 1 and 'not valid' in r.stderr and fx.MARKER not in r.stdout,
          'no display: %d %r' % (r.returncode, r.stderr))


def test_dialogs(home, signed):
    scr = Screen(home)
    try:
        # No signature: Escape keeps it and takes its executable bits.
        unsigned = home.files['unsigned']
        unsigned.chmod(0o755)
        scr.start(unsigned)
        if check(scr.dialog(), 'unsigned: no window (%s)' % scr.qa()):
            meta, controls = scr.meta(), scr.controls()
            check('@title:This AppImage has no signature' in meta, 'unsigned: title %s' % meta)
            check('@verdict:unsigned' in meta and '@policy:verified-only' in meta, 'unsigned: %s' % meta)
            check('@focus:Delete' in meta, 'unsigned: Delete is not the default (%s)' % meta)
            check(set(controls) == {'Delete', 'Keep'}, 'unsigned: buttons %s' % sorted(controls))
            g = scr.geometry()
            check(g and g['X'] >= 0 and g['Y'] >= 0 and g['X'] + g['WIDTH'] <= scr.width and
                  g['Y'] + g['HEIGHT'] <= scr.height, 'unsigned: window off the screen %s' % g)
            scr.shot('appimage-unsigned')
            scr.key('Escape')
            check(scr.finished() == 1, 'unsigned: Escape did not close it')
            check(unsigned.exists() and unsigned.stat().st_mode & 0o111 == 0, 'unsigned: kept with exec bits')
            check(fx.MARKER not in scr.output(), 'unsigned: it ran')
            check('kept %s' % unsigned in home.log(), 'unsigned: not logged as kept')
        # Enter: Delete, to the trash.
        scr.start(unsigned)
        if check(scr.dialog(), 'unsigned again: no window'):
            scr.key('Return')
            check(scr.finished() == 1, 'unsigned: Enter did not close it')
            check(not unsigned.exists() and home.trashed(unsigned), 'unsigned: not moved to the trash')
            check('deleted (trash) %s' % unsigned in home.log(), 'unsigned: deletion not logged')
        # Another architecture; F1 shows About AppImage Runner over it.
        if os.uname().machine == 'x86_64':
            scr.start(home.files['arm'])
            if check(scr.dialog(), 'aarch64: no window'):
                check('@title:This AppImage is made for another kind of computer' in scr.meta(), 'aarch64 %s' % scr.meta())
                scr.shot('appimage-wrong-arch')
                scr.key('F1')
                about = scr.wait(lambda: scr.xdo('search', '--onlyvisible', '--name', '^About AppImage Runner$')
                                 .stdout.split(), 6)
                if check(about, 'F1 did not open About AppImage Runner'):
                    scr.shot('appimage-about')
                    scr.xdo('key', 'Escape')
                    check(scr.wait(lambda: not scr.xdo('search', '--onlyvisible', '--name',
                                                       '^About AppImage Runner$').stdout.split(), 5),
                          'Escape did not close About')
                    check(scr.proc.poll() is None, 'the AppImage window closed with About')
                scr.key('Escape')
                scr.finished()
        if signed:
            # Another key: both named; Delete clicked.
            home.run('--check', home.files['signed'])
            other = home.files['other-key']
            scr.start(other)
            if check(scr.dialog(), 'other key: no window'):
                meta = scr.meta()
                check('@title:This AppImage is signed with another key' in meta, 'other key: %s' % meta)
                check(any(m.startswith('@previous:') for m in meta) and any(m.startswith('@key:') for m in meta),
                      'other key: keys not named (%s)' % meta)
                scr.shot('appimage-key-changed')
                scr.click('Delete')
                check(scr.finished() == 1 and home.trashed(other), 'other key: not deleted')
            # policy=ask: still no way to run a changed AppImage.
            home.policy.write_text('policy=ask\n')
            scr.start(home.files['tampered'])
            if check(scr.dialog(), 'tampered: no window'):
                meta = scr.meta()
                check('@verdict:bad-signature' in meta and '@policy:ask' in meta, 'tampered: %s' % meta)
                check('Run Anyway' not in scr.controls(), 'tampered: Run Anyway offered')
                scr.shot('appimage-bad-signature')
                scr.key('Escape')
                scr.finished()
        # policy=ask: Run Anyway, then without asking.
        home.policy.write_text('policy=ask\n')
        notes = fx.build(home.temp / 'Downloads/Notes-2.2-x86_64.AppImage', b'notes 2.2')
        scr.start(notes, 'first')
        if check(scr.dialog(), 'ask: no window'):
            check('Run Anyway' in scr.controls() and '@focus:Delete' in scr.meta(), 'ask: %s' % scr.qa())
            scr.shot('appimage-ask')
            scr.click('Run Anyway')
            check(scr.finished() == 0, 'ask: Run Anyway did not run it')
            check(scr.output() == fx.MARKER + 'first\n', 'ask: output %r' % scr.output())
            check('accepted %s' % notes in home.log(), 'ask: not logged as accepted')
        scr.start(notes, 'second')
        check(scr.finished() == 0 and scr.output() == fx.MARKER + 'second\n', 'ask: asked again (%s)' % scr.qa())
        home.policy.write_text('policy=verified-only\n')
    finally:
        scr.close()


def test_small_screen(signer):
    home = Home(signer, scale=150)
    scr = Screen(home, '1024x600')
    try:
        home.policy.write_text('policy=ask\n')
        scr.start(home.files['unsigned'])
        if check(scr.dialog(), 'small: no window'):
            g = scr.geometry()
            check(g and g['X'] + g['WIDTH'] <= scr.width and g['Y'] + g['HEIGHT'] <= scr.height,
                  'small: the window does not fit %s' % g)
            for name, (x, y, w, h) in scr.controls().items():
                check(g and x >= 0 and y >= 0 and x + w <= g['WIDTH'] and y + h <= g['HEIGHT'],
                      'small: %s outside the window %s' % (name, (x, y, w, h)))
            scr.shot('appimage-1024x600-150')
            scr.key('Escape')
            scr.finished()
    finally:
        scr.close()
        home.close()


def main():
    signer = None
    if shutil.which('gpg') and shutil.which('gpgv'):
        signer = fx.Signer()
    else:
        print('gpg or gpgv is not installed: the signature cases are skipped')
    try:
        home = Home(signer)
        try:
            test_checks(home, bool(signer))
            if xdotool and shutil.which('Xvfb'):
                test_dialogs(home, bool(signer))
            else:
                print('xdotool or Xvfb missing: the window is not tested')
        finally:
            home.close()
        if xdotool and shutil.which('Xvfb'):
            test_small_screen(signer)
    finally:
        if signer:
            signer.close()
    print('appimage-ui: %d problem(s)' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
