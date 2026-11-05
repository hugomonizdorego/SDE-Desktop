#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""SDE lock screen, unlocking with a fingerprint, on a private Xvfb and a
private D-Bus with the fake fprintd (tests/fake_fprintd.cpp). Uses the QA
fixture (SDE_LOCK_FIXTURE_FINGERPRINT; password "sde-test"; nothing is
locked and no real reader or fingerprint is involved).

test_lock_fingerprint.py LOCK_UI_FIXTURE FAKE_FPRINTD

Checked: the reader with the user's fingerprints is claimed and verified
once the screen is locked; a retry status keeps it waiting; a status sent
by another program is ignored; a recognized finger unlocks; five fingers
that are not recognized end verifying (the reader stays claimed, so that
a pam_fprintd in the password's stack cannot wait for a finger) and the
password unlocks; an account check that refuses the finger keeps the
screen locked; "hold" claims without verifying; no fingerprints, no
claim; Switch User releases the reader and a key takes it again; the
claim ends with the lock screen. SDE_FINGERPRINT_SCREENSHOTS=DIR saves
screenshots; SDE_TEST_DISPLAY picks the X display."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

xdotool = shutil.which('xdotool')
if len(sys.argv) < 3 or not xdotool or not shutil.which('Xvfb') or not shutil.which('dbus-daemon'):
    print('xdotool, Xvfb, dbus-daemon and the two programs are required; skipped')
    sys.exit(77)
FIXTURE, FAKE = (str(Path(p).resolve()) for p in sys.argv[1:3])
SHOTS = os.environ.get('SDE_FINGERPRINT_SCREENSHOTS')
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


def wait(fn, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.05)
    return False


class Lock:
    """One lock screen (the fixture) and what it reports."""

    def __init__(self, env, mode, *args):
        self.r, w = os.pipe()
        self.got = b''
        e = dict(env, SDE_LOCK_FIXTURE_FINGERPRINT=mode)
        self.p = sp.Popen([FIXTURE, str(w)] + list(args), env=e, pass_fds=(w,), stdout=sp.DEVNULL,
                          stderr=open(env['HOME'] + '/lock.log', 'a'))
        os.close(w)
        os.set_blocking(self.r, False)

    def read(self):
        try:
            while True:
                chunk = os.read(self.r, 64)
                if not chunk:
                    break
                self.got += chunk
        except BlockingIOError:
            pass
        return self.got

    def saw(self, c, timeout=8.0):
        return wait(lambda: c in self.read(), timeout)

    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(timeout=5)
            except sp.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        os.close(self.r)


with tempfile.TemporaryDirectory(prefix='sde-lock-fingerprint-') as temp:
    procs = []
    bus = sp.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'], stdout=sp.PIPE,
                   stderr=sp.DEVNULL, text=True)
    procs.append(bus)
    address = bus.stdout.readline().strip()
    server = None
    fixed = os.environ.get('SDE_TEST_DISPLAY')
    for attempt in range(1 if fixed else 10):
        number = int(fixed) if fixed else 300 + (os.getpid() + attempt * 53) % 20000
        candidate = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1280x800x24', '-nolisten', 'tcp'],
                             stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        env = dict(os.environ, DISPLAY=':%d' % number, HOME=temp, XDG_CONFIG_HOME=temp + '/config',
                   LANG='C.UTF-8', LC_ALL='C.UTF-8', DBUS_SESSION_BUS_ADDRESS=address,
                   DBUS_SYSTEM_BUS_ADDRESS=address)
        env.pop('XDG_SEAT_PATH', None)
        if wait(lambda: candidate.poll() is None and
                sp.run([xdotool, 'getmouselocation'], env=env, capture_output=True).returncode == 0, 5):
            server = candidate
            break
        if candidate.poll() is None:
            candidate.terminate()
            candidate.wait()
    if not server:
        print('Xvfb did not start')
        sys.exit(1)
    procs.append(server)
    log = Path(temp) / 'fprintd.log'
    fake = sp.Popen([FAKE, str(log), '--devices', '1', '--enrolled', 'right-index-finger'], env=env,
                    stdin=sp.PIPE, text=True, stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    procs.insert(0, fake)

    def text():
        try:
            return log.read_text()
        except OSError:
            return ''

    def command(line):
        fake.stdin.write(line + '\n')
        fake.stdin.flush()
        time.sleep(0.1)

    def count(what):
        return text().count(what)

    def state():
        n = count('STATE ')
        command('state')
        wait(lambda: count('STATE ') > n, 3)
        lines = [l for l in text().splitlines() if l.startswith('STATE ')]
        return dict(kv.split('=', 1) for kv in lines[-1][6:].split() if '=' in kv) if lines else {}

    def shot(name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / name)], env=env, check=False)

    def password(lock):
        sp.run([xdotool, 'type', '--delay', '30', 'sde-test'], env=env, check=True)
        sp.run([xdotool, 'key', 'Return'], env=env, check=True)
        return lock.saw(b'U', 6)

    try:
        if not wait(lambda: 'READY' in text(), 8):
            raise RuntimeError('the fake fprintd did not start')

        # 1. A recognized finger unlocks; retries and other senders do not.
        lock = Lock(env, '1')
        check(lock.saw(b'L'), 'not locked: %r' % lock.read())
        check(wait(lambda: 'CALL VerifyStart any' in text(), 8), 'no VerifyStart: ' + text())
        st = state()
        check(st.get('claimed', '').startswith(':') and st.get('action') == 'verify', 'not verifying: %s' % st)
        time.sleep(0.5)
        shot('lock-fingerprint.png')
        command('touch verify-retry-scan')
        time.sleep(0.6)
        check(b'U' not in lock.read() and lock.p.poll() is None, 'a retry status unlocked')
        shot('lock-fingerprint-retry.png')
        if shutil.which('dbus-send'):
            # The same status from another program: the bus delivers it to
            # nobody who matched fprintd's name, and the lock checks the
            # sender itself.
            sp.run(['dbus-send', '--bus=' + address, '--type=signal', '/net/reactivated/Fprint/Device/0',
                    'net.reactivated.Fprint.Device.VerifyStatus', 'string:verify-match', 'boolean:true'],
                   env=env, check=False)
            time.sleep(0.8)
            check(b'U' not in lock.read(), 'a VerifyStatus from another program unlocked the screen')
        command('touch verify-match')
        check(lock.saw(b'U', 6), 'a recognized finger did not unlock: %r' % lock.read())
        check(lock.p.wait(timeout=5) == 0, 'the lock screen did not exit after the finger')
        check(wait(lambda: state().get('claimed') == '', 5), 'the claim outlived the lock screen')
        lock.close()

        # 2. Five fingers that are not recognized: only the password.
        lock = Lock(env, '1')
        check(lock.saw(b'L'), 'not locked (2)')
        for attempt in range(1, 6):
            starts = count('CALL VerifyStart')
            check(wait(lambda: state().get('action') == 'verify', 8), 'not verifying before attempt %d' % attempt)
            command('touch verify-no-match')
            if attempt < 5:
                check(wait(lambda: count('CALL VerifyStart') > starts, 6), 'no new VerifyStart after %d' % attempt)
            if attempt == 1:
                time.sleep(0.2)
                shot('lock-fingerprint-not-recognized.png')
        time.sleep(1.5)
        st = state()
        check(st.get('action') == '' and st.get('claimed', '').startswith(':'),
              'after five failures the reader must be held without verifying: %s' % st)
        check(b'U' not in lock.read() and lock.p.poll() is None, 'failed fingers unlocked')
        shot('lock-fingerprint-too-many.png')
        starts = count('CALL VerifyStart')
        command('touch verify-match')  # ignored: verifying is over
        time.sleep(0.8)
        check(b'U' not in lock.read() and count('CALL VerifyStart') == starts, 'verifying went on after five failures')
        check(password(lock), 'the password did not unlock after the fingerprint failures')
        check(lock.p.wait(timeout=5) == 0, 'exit status (2)')
        lock.close()

        # 3. The account check refuses the finger: still locked.
        lock = Lock(env, 'deny')
        check(lock.saw(b'L'), 'not locked (3)')
        check(wait(lambda: state().get('action') == 'verify', 8), 'not verifying (3)')
        command('touch verify-match')
        time.sleep(1.0)
        check(b'U' not in lock.read() and lock.p.poll() is None, 'a refused account unlocked with a finger')
        shot('lock-fingerprint-refused.png')
        check(password(lock), 'the password did not unlock (3)')
        lock.close()

        # 4. "hold": the reader is claimed, nothing is verified.
        starts = count('CALL VerifyStart')
        claims = count('CALL Claim')
        lock = Lock(env, 'hold')
        check(lock.saw(b'L'), 'not locked (4)')
        check(wait(lambda: count('CALL Claim') > claims, 8), 'hold: no Claim')
        time.sleep(0.8)
        check(count('CALL VerifyStart') == starts, 'hold: verifying anyway')
        check(password(lock), 'the password did not unlock (4)')
        lock.close()

        # 5. No fingerprints: the reader is not claimed.
        command('enrolled ')
        claims = count('CALL Claim')
        lock = Lock(env, '1')
        check(lock.saw(b'L'), 'not locked (5)')
        check(wait(lambda: count('CALL ListEnrolledFingers') > 0, 5), 'no ListEnrolledFingers')
        time.sleep(1.0)
        check(count('CALL Claim') == claims, 'a reader without fingerprints was claimed')
        check(password(lock), 'the password did not unlock (5)')
        lock.close()

        # 6. Switch User releases the reader; a key takes it again.
        command('enrolled right-index-finger')
        lock = Lock(env, '1', '--switch-user')
        check(lock.saw(b'L'), 'not locked (6)')
        check(wait(lambda: state().get('action') == 'verify', 8), 'not verifying (6)')
        releases, claims = count('CALL Release'), count('CALL Claim')
        sp.run([xdotool, 'mousemove', '100', '752', 'click', '1'], env=env, check=True)  # Switch User
        check(wait(lambda: count('CALL Release') > releases, 5), 'Switch User did not release the reader')
        check(wait(lambda: state().get('claimed') == '', 5), 'still claimed after Switch User')
        sp.run([xdotool, 'key', 'Escape'], env=env, check=True)
        check(wait(lambda: count('CALL Claim') > claims, 5), 'a key did not take the reader again')
        check(wait(lambda: state().get('action') == 'verify', 8), 'not verifying again after Switch User')
        check(password(lock), 'the password did not unlock (6)')
        lock.close()
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except sp.TimeoutExpired:
                    p.kill()
                    p.wait()
        if failures:
            print(text()[-4000:])
            try:
                print((Path(temp) / 'lock.log').read_text()[-3000:])
            except OSError:
                pass

if failures:
    print('%d failures' % len(failures))
    sys.exit(1)
print('Lock screen: unlocked with a finger, fell back to the password, released the reader.')
