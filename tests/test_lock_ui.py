#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""SDE lock screen on a private Xvfb: grabs before drawing, waits (visibly)
for a grab another client holds, rejects a wrong password, unlocks with the
right one.

Uses the QA fixture (fake authenticator, password "sde-test").
SDE_LOCK_SCREENSHOTS=DIR saves screenshots of each step."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

fixture = Path(sys.argv[1]).resolve()
xdotool = shutil.which('xdotool')
if not xdotool or not shutil.which('Xvfb'):
    print('xdotool and Xvfb are required; skipped')
    sys.exit(77)
shots = os.environ.get('SDE_LOCK_SCREENSHOTS')


def shot(env, name):
    if shots and shutil.which('import'):
        Path(shots).mkdir(parents=True, exist_ok=True)
        sp.run(['import', '-window', 'root', str(Path(shots) / name)], env=env, check=False)


def read_reports(fd, want, timeout=8):
    got = b''
    end = time.time() + timeout
    os.set_blocking(fd, False)
    while time.time() < end:
        try:
            chunk = os.read(fd, 64)
            if chunk:
                got += chunk
            elif chunk == b'':
                break
        except BlockingIOError:
            pass
        if all(c in got for c in want):
            return got
        time.sleep(0.05)
    return got


with tempfile.TemporaryDirectory(prefix='sde-lock-ui-') as temp:
    number = 300 + os.getpid() % 20000
    server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1280x800x24',
                       '-nolisten', 'tcp'], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    env = dict(os.environ, DISPLAY=':%d' % number, HOME=temp, XDG_CONFIG_HOME=temp + '/config',
               LANG='C.UTF-8', LC_ALL='C.UTF-8')
    env.pop('XDG_SEAT_PATH', None)
    try:
        for _ in range(100):
            if sp.run([xdotool, 'getmouselocation'], env=env, stdout=sp.DEVNULL,
                      stderr=sp.DEVNULL).returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('Xvfb did not start')

        # 1. Another client holds the keyboard (a stuck menu, a screenshot
        # tool). Before alpha 51 the lock refused after 6 s and left the
        # session open; now it covers the screen, says why ("W") and keeps
        # retrying: once the grab is released the keyboard is ours ("L")
        # and the password can be typed.
        holder = sp.Popen([sys.executable, '-c', '''
import ctypes, sys, time
x = ctypes.cdll.LoadLibrary("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
d = x.XOpenDisplay(None)
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
root = x.XDefaultRootWindow(d)
x.XGrabKeyboard.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
print(x.XGrabKeyboard(d, root, 0, 1, 1, 0), flush=True)
x.XFlush.argtypes = [ctypes.c_void_p]
x.XFlush(d)
time.sleep(30)
'''], env=env, stdout=sp.PIPE, text=True)
        assert holder.stdout.readline().strip() == '0', 'test grab failed'
        r, w = os.pipe()
        ui = sp.Popen([str(fixture), str(w)], env=env, pass_fds=(w,))
        os.close(w)
        got = read_reports(r, b'MW', timeout=12)
        assert b'W' in got and b'L' not in got and b'F' not in got, got
        assert ui.poll() is None, 'the lock screen gave up'
        time.sleep(0.5)
        shot(env, 'lock-waiting-for-keyboard.png')
        holder.kill()
        holder.wait()
        got = read_reports(r, b'L', timeout=5)
        assert b'L' in got, 'no grab after the other client released it: %r' % got
        sp.run([xdotool, 'type', '--delay', '30', 'sde-test'], env=env, check=True)
        sp.run([xdotool, 'key', 'Return'], env=env, check=True)
        got = read_reports(r, b'U', timeout=5)
        assert b'U' in got, got
        assert ui.wait(timeout=5) == 0
        os.close(r)

        # 2. Normal lock.
        r, w = os.pipe()
        ui = sp.Popen([str(fixture), str(w), '--switch-user'], env=env, pass_fds=(w,))
        os.close(w)
        got = read_reports(r, b'ML')
        assert got.startswith(b'M') and b'L' in got, got
        time.sleep(0.8)
        shot(env, 'lock-screen.png')
        # The keyboard is grabbed: another grab must fail.
        probe = sp.run([sys.executable, '-c', '''
import ctypes
x = ctypes.cdll.LoadLibrary("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
d = x.XOpenDisplay(None)
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XGrabKeyboard.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
print(x.XGrabKeyboard(d, x.XDefaultRootWindow(d), 0, 1, 1, 0))
'''], env=env, capture_output=True, text=True)
        assert probe.stdout.strip() == '1', 'keyboard not grabbed: ' + probe.stdout  # AlreadyGrabbed

        sp.run([xdotool, 'type', '--delay', '30', 'wrong'], env=env, check=True)
        time.sleep(0.3)
        shot(env, 'lock-typing.png')
        sp.run([xdotool, 'key', 'Return'], env=env, check=True)
        time.sleep(0.5)
        shot(env, 'lock-wrong.png')
        assert ui.poll() is None, 'a wrong password unlocked the screen'
        assert b'U' not in read_reports(r, b'U', timeout=0.5)
        time.sleep(1.2)  # the attempt delay after one failure is 1 s
        sp.run([xdotool, 'type', '--delay', '30', 'sde-test'], env=env, check=True)
        sp.run([xdotool, 'key', 'Return'], env=env, check=True)
        got = read_reports(r, b'U', timeout=5)
        assert b'U' in got, got
        assert ui.wait(timeout=5) == 0
        os.close(r)
        shot(env, 'unlocked.png')
        print('Lock screen: waited for the grab, grabbed, rejected, unlocked.')
    finally:
        server.terminate()
        server.wait()
