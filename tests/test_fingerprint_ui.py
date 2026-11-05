#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Control Center > Fingerprint on a private Xvfb and a private D-Bus, with
the fake fprintd (tests/fake_fprintd.cpp) as net.reactivated.Fprint and,
for "Use for", the real sde-fingerprint-pam working with pam-auth-update on
a fake root (tests/fingerprint_fakeroot.py: SDE_FINGERPRINT_HELPER and
SDE_FINGERPRINT_PAM_ROOT, no pkexec; nothing of this computer changes).

test_fingerprint_ui.py SDE_SETTINGS FAKE_FPRINTD SDE_FINGERPRINT_PAM

Checked: without fprintd the page says so and offers to install it; when
fprintd appears without a reader, "No fingerprint reader"; with a reader,
enrolling a finger (every stage, then EnrollStop and Release), a failed
enrollment (enroll-duplicate), an enrollment polkit refuses (the reader is
released), Test with a finger that is not recognized and one that is,
Delete and Delete All; the switches change common-auth of the fake root
(administrator only: SDE's profile; both: Debian's) and the lock screen's
setting. SDE_FINGERPRINT_SCREENSHOTS=DIR saves screenshots; SDE_TEST_DISPLAY
picks the X display."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import fingerprint_fakeroot as fr  # noqa: E402

xdotool = shutil.which('xdotool')
if len(sys.argv) < 4 or not xdotool or not shutil.which('Xvfb') or not shutil.which('dbus-daemon'):
    print('xdotool, Xvfb, dbus-daemon and the three programs are required; skipped')
    sys.exit(77)
SETTINGS, FAKE, HELPER = sys.argv[1:4]
SHOTS = os.environ.get('SDE_FINGERPRINT_SCREENSHOTS')
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


class Session:
    def __init__(self, size='1366x768'):
        self.temp = Path(tempfile.mkdtemp(prefix='fingerprint-ui-'))
        home = self.temp
        (home / '.config/sde').mkdir(parents=True)
        (home / '.config/sde/sde.conf').write_text('[appearance]\ntheme=light\nscale=100\n[effects]\nenabled=true\n')
        (home / 'qa').mkdir()
        (home / 'run').mkdir(mode=0o700)
        self.procs = []
        self.fake = None
        self.log = home / 'fprintd.log'
        # "Use for": a fake root (when pam-auth-update can run here).
        self.root, why = fr.make_root(home / 'pam')
        if self.root is None:
            print('Use for: not checked:', why)
        bus = sp.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'], stdout=sp.PIPE,
                       stderr=sp.DEVNULL, text=True)
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
                            LANG='C.UTF-8', LC_ALL='C.UTF-8',
                            DBUS_SESSION_BUS_ADDRESS=address, DBUS_SYSTEM_BUS_ADDRESS=address)
            if self.root is not None:
                self.env.update(SDE_FINGERPRINT_PAM_ROOT=str(self.root), SDE_FINGERPRINT_HELPER=HELPER,
                                DEBCONF_SYSTEMRC=fr.env_for(home / 'pam')['DEBCONF_SYSTEMRC'])
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
        self.app = None

    def start_fake(self, *args):
        self.fake = sp.Popen([FAKE, str(self.log)] + list(args), env=self.env, stdin=sp.PIPE, text=True,
                             stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        self.procs.insert(0, self.fake)
        if not self.wait(lambda: self.log.exists() and 'READY' in self.log.read_text(), 8):
            raise RuntimeError('the fake fprintd did not start')

    def command(self, line):
        self.fake.stdin.write(line + '\n')
        self.fake.stdin.flush()
        time.sleep(0.15)

    def fake_log(self):
        try:
            return self.log.read_text()
        except OSError:
            return ''

    def start(self):
        self.app = sp.Popen([SETTINGS, '--page', 'fingerprint'], env=self.env,
                            stdout=open(self.temp / 'settings.log', 'w'), stderr=sp.STDOUT)
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

    def state(self):
        """The page's @fingerprint line as a dict."""
        for f in self.lines():
            if f[0].startswith('@fingerprint:'):
                return dict(kv.split('=', 1) for kv in f[0][len('@fingerprint:'):].split(';') if '=' in kv)
        return {}

    def message(self, which='message'):
        for f in self.lines():
            if f[0].startswith('@fingerprint-%s:' % which):
                return f[0].split(':', 1)[1]
        return ''

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

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            self.xdo('mousemove', 5, 5)
            time.sleep(0.3)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / name)], env=self.env, check=False)

    def idle(self, timeout=15):
        return self.wait(lambda: self.state().get('op') == '0' and self.state().get('pam-busy') == '0', timeout)


s = Session()
try:
    s.start()
    if not s.wait(lambda: s.window() is not None, 20):
        raise RuntimeError('the Control Center did not open')

    # 1. No fprintd on the bus: the page says so and offers to install it.
    check(s.wait(lambda: s.state().get('service') == 'missing', 15), 'no fprintd: %s' % s.state())
    # (libpam-fprintd too when the PAM root has no pam_fprintd.so.)
    check(s.wait(lambda: any(n.startswith('Install fprintd') for n in s.names()), 5),
          'no install offer (%s)' % s.names())
    s.shot('fingerprint-missing.png')

    # 2. fprintd appears without a reader: the page looks again by itself.
    s.start_fake('--devices', '0', '--step-ms', '150')
    check(s.wait(lambda: s.state().get('service') == 'ready' and s.state().get('readers') == '0', 15),
          'no reader: %s' % s.state())
    check(s.wait(lambda: s.control('Look Again') is not None, 5), 'no Look Again (%s)' % s.names())
    s.shot('fingerprint-no-reader.png')

    # 3. A reader: enroll the right index finger (the one chosen first).
    s.command('devices 1')
    s.press('Look Again')
    check(s.wait(lambda: s.state().get('readers') == '1' and s.control('Right index finger') is not None, 15),
          'the reader does not show: %s' % s.state())
    check(s.state().get('enrolled') == '' and s.state().get('selected') == 'right-index-finger',
          'a fresh reader: %s' % s.state())
    s.shot('fingerprint-reader.png')
    s.press('Enroll a fingerprint: Right index finger')
    check(s.wait(lambda: 'EnrollStart right-index-finger' in s.fake_log(), 8), 'EnrollStart was not called')
    check(s.wait(lambda: int(s.state().get('stage', '0')) >= 2, 8), 'the stages do not count: %s' % s.state())
    s.shot('fingerprint-enrolling.png')
    check(s.wait(lambda: s.state().get('enrolled') == 'right-index-finger' and s.state().get('op') == '0', 15),
          'enrolling did not finish: %s %s' % (s.state(), s.message()))
    check('was enrolled' in s.message(), 'enrolled message: %r' % s.message())
    log = s.fake_log()
    check(log.count('SIGNAL EnrollStatus enroll-stage-passed') == 4 and 'enroll-completed done' in log,
          'the enroll statuses: %s' % log)
    check(log.find('CALL EnrollStop') > log.find('enroll-completed') and
          log.find('CALL Release', log.find('CALL EnrollStop')) > 0, 'EnrollStop and Release after the enrollment')
    s.shot('fingerprint-enrolled.png')

    # 4. A failed enrollment: the finger is enrolled already (as another).
    s.command('enroll-script enroll-retry-scan,enroll-stage-passed,enroll-duplicate')
    s.press('Left index finger')
    check(s.wait(lambda: s.state().get('selected') == 'left-index-finger', 5), 'choosing a finger: %s' % s.state())
    s.press('Enroll a fingerprint: Left index finger')
    check(s.wait(lambda: s.state().get('op') == '0' and 'enrolled already' in s.message(), 15),
          'enroll-duplicate: %s %r' % (s.state(), s.message()))
    check(s.state().get('enrolled') == 'right-index-finger', 'a failed enrollment added a finger: %s' % s.state())
    tail = s.fake_log().split('enroll-duplicate', 1)[-1]
    check('CALL EnrollStop' in tail and 'CALL Release' in tail, 'not released after a failed enrollment')
    s.shot('fingerprint-enroll-failed.png')

    # 5. polkit refuses the enrollment: a message, and the reader is free.
    s.command('enroll-script ')
    s.command('deny-enroll on')
    s.press('Enroll a fingerprint: Left index finger')
    check(s.wait(lambda: s.state().get('op') == '0' and 'Not allowed' in s.message(), 10),
          'refused enrollment: %s %r' % (s.state(), s.message()))
    s.command('deny-enroll off')
    s.command('state')
    check(s.wait(lambda: 'STATE claimed= ' in s.fake_log(), 5), 'the reader stayed claimed after a refusal')

    # 6. Test: not recognized, then recognized.
    s.press('Right index finger')
    s.command('verify-script verify-retry-scan,verify-no-match')
    s.press('Check that an enrolled finger is recognized')
    check(s.wait(lambda: s.state().get('op') == '0' and 'not recognized' in s.message(), 10),
          'no match: %s %r' % (s.state(), s.message()))
    s.command('verify-script verify-match')
    s.press('Check that an enrolled finger is recognized')
    check(s.wait(lambda: s.state().get('op') == '0' and 'recognized: it works' in s.message(), 10),
          'match: %s %r' % (s.state(), s.message()))
    log = s.fake_log()
    check(log.count('CALL VerifyStart any') == 2 and log.count('CALL VerifyStop') >= 2, 'verify calls: %s' % log)
    s.shot('fingerprint-tested.png')

    # 7. Delete the finger.
    s.press('Delete the fingerprint: Right index finger')
    check(s.wait(lambda: s.state().get('op') == '0' and s.state().get('enrolled') == '', 10),
          'delete: %s %r' % (s.state(), s.message()))
    check('CALL DeleteEnrolledFinger right-index-finger' in s.fake_log(), 'DeleteEnrolledFinger was not called')

    # 8. Delete All (with its confirmation).
    s.command('enrolled left-thumb,right-thumb')
    time.sleep(4.5)  # the page reads the fingers again when it is built later than that
    s.press('Left thumb')
    check(s.wait(lambda: s.state().get('enrolled') == 'left-thumb,right-thumb', 10), 'new fingers: %s' % s.state())
    s.press('Delete All...')
    check(s.press('Delete All Fingerprints'), 'no confirmation')
    check(s.wait(lambda: s.state().get('op') == '0' and s.state().get('enrolled') == '', 10),
          'delete all: %s %r' % (s.state(), s.message()))
    check('CALL DeleteEnrolledFingers2' in s.fake_log(), 'DeleteEnrolledFingers2 was not called')

    # 9. Use for: the system's PAM (fake root) and the lock screen's setting.
    if s.root is not None:
        check(s.state().get('sign-in') == '0' and s.state().get('admin') == '0', 'PAM at first: %s' % s.state())
        s.press('Use the fingerprint for administrator authentication')
        check(s.wait(lambda: s.state().get('pam-busy') == '0' and s.state().get('admin') == '1', 60),
              'administrator on: %s %r' % (s.state(), s.message('pam-message')))
        check(s.state().get('sign-in') == '0', 'sign-in turned on with the administrator switch')
        auth = '\n'.join(fr.auth_lines(s.root))
        check('service notin sudo:sudo-i:polkit-1' in auth and 'pam_fprintd.so' in auth, 'common-auth: %s' % auth)
        s.shot('fingerprint-use-admin.png')
        s.press('Use the fingerprint to sign in')
        check(s.wait(lambda: s.state().get('pam-busy') == '0' and s.state().get('sign-in') == '1', 60),
              'sign-in on: %s %r' % (s.state(), s.message('pam-message')))
        check(fr.modules(s.root)[:1] == ['fprintd'], 'both on is not Debian\'s profile: %s' % fr.modules(s.root))
        s.press('Use the fingerprint for administrator authentication')
        check(s.wait(lambda: s.state().get('pam-busy') == '0' and s.state().get('admin') == '0', 60),
              'administrator off: %s' % s.state())
        check(fr.modules(s.root)[:1] == ['sde-fingerprint-sign-in'], 'sign-in only: %s' % fr.modules(s.root))
        s.press('Use the fingerprint to sign in')
        check(s.wait(lambda: s.state().get('pam-busy') == '0' and s.state().get('sign-in') == '0', 60),
              'sign-in off: %s' % s.state())
        check('pam_fprintd' not in '\n'.join(fr.auth_lines(s.root)), 'pam_fprintd stayed in common-auth')
    s.press('Use the fingerprint to unlock the screen')
    conf = s.temp / '.config/sde/sde.conf'
    check(s.wait(lambda: 'fingerprint=false' in conf.read_text(), 5), 'the lock screen setting was not saved')
    s.press('Use the fingerprint to unlock the screen')
    check(s.wait(lambda: 'fingerprint=true' in conf.read_text(), 5), 'the lock screen setting was not saved again')

    # The page never left a claim behind.
    states = lambda: [l for l in s.fake_log().splitlines() if l.startswith('STATE ')]  # noqa: E731
    count = len(states())
    s.command('state')
    check(s.wait(lambda: len(states()) > count, 5) and states()[-1].startswith('STATE claimed= '),
          'the reader is still claimed: %s' % (states()[-1:] or ['?'])[0])
    check(s.app.poll() is None, 'the Control Center exited')
finally:
    if failures and (s.temp / 'settings.log').exists():
        print((s.temp / 'settings.log').read_text()[-3000:])
        print(s.fake_log()[-3000:])
    s.close()

if failures:
    print('%d failures' % len(failures))
    sys.exit(1)
print('Control Center > Fingerprint: OK')
