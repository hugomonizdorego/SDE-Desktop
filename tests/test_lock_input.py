#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""SDE lock screen input recovery on a private Xvfb (alpha 51).

Regression test for "after the screen locks, the mouse and the keyboard do
nothing on the lock screen": the lock screen must end up with a working
password field whatever other clients do.

  1. a popup menu of another client holds the keyboard and pointer grabs
     (a GTK menu or a panel popup left open): the lock screen unmaps it,
     gets the grabs, and typing the password unlocks;
  2. another client holds only the pointer: the keyboard is ours at once,
     typing works while the pointer is still held;
  3. the monitors change while locked (simulated RandR: Xvfb's single
     output cannot change, so the fixture reads the monitor list from a
     file): the windows follow, the form follows the pointer to the other
     monitor, and clicking the Enter button there unlocks;
  4. SIGUSR1 (sent by the daemon after a resume) keeps a working lock.

Uses the QA fixture (fake authenticator, password "sde-test"); runs openbox
as window manager when it is installed. SDE_LOCK_SCREENSHOTS=DIR saves
screenshots."""
import os
import shutil
import signal
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

XLIB = '''
import ctypes, sys, time
from ctypes import c_void_p, c_ulong, c_int, c_uint, c_long
x = ctypes.cdll.LoadLibrary("libX11.so.6")
x.XOpenDisplay.restype = c_void_p
d = x.XOpenDisplay(None)
x.XDefaultRootWindow.restype = c_ulong
x.XDefaultRootWindow.argtypes = [c_void_p]
root = x.XDefaultRootWindow(d)
x.XGrabKeyboard.argtypes = [c_void_p, c_ulong, c_int, c_int, c_int, c_ulong]
x.XGrabPointer.argtypes = [c_void_p, c_ulong, c_int, c_uint, c_int, c_int, c_ulong, c_ulong, c_ulong]
x.XFlush.argtypes = [c_void_p]
x.XSync.argtypes = [c_void_p, c_int]
'''

# A stuck popup menu: an override-redirect window holding both grabs, like
# a GTK menu or an SDE panel popup. Prints "grabs 0 0", then "unmapped"
# when somebody unmaps it.
POPUP = XLIB + '''
class Attr(ctypes.Structure):
    _fields_ = [("background_pixmap", c_ulong), ("background_pixel", c_ulong),
                ("border_pixmap", c_ulong), ("border_pixel", c_ulong), ("bit_gravity", c_int),
                ("win_gravity", c_int), ("backing_store", c_int), ("backing_planes", c_ulong),
                ("backing_pixel", c_ulong), ("save_under", c_int), ("event_mask", c_long),
                ("do_not_propagate_mask", c_long), ("override_redirect", c_int),
                ("colormap", c_ulong), ("cursor", c_ulong)]
a = Attr()
a.override_redirect = 1
a.background_pixel = 0xC0C0C0
a.event_mask = 1 << 17  # StructureNotifyMask
x.XCreateWindow.restype = c_ulong
x.XCreateWindow.argtypes = [c_void_p, c_ulong, c_int, c_int, c_uint, c_uint, c_uint, c_int, c_uint,
                            c_void_p, c_ulong, ctypes.POINTER(Attr)]
# CWBackPixel | CWOverrideRedirect | CWEventMask
w = x.XCreateWindow(d, root, 120, 90, 220, 300, 0, 0, 1, None, 2 | 512 | 2048, ctypes.byref(a))
x.XMapRaised.argtypes = [c_void_p, c_ulong]
x.XMapRaised(d, w)
x.XSync(d, 0)
time.sleep(0.2)
kb = x.XGrabKeyboard(d, w, 0, 1, 1, 0)
ptr = x.XGrabPointer(d, w, 0, 4 | 8, 1, 1, 0, 0, 0)
print("grabs", kb, ptr, flush=True)
ev = (ctypes.c_long * 24)()
x.XNextEvent.argtypes = [c_void_p, c_void_p]
while True:
    x.XNextEvent(d, ev)
    if ctypes.cast(ev, ctypes.POINTER(c_int))[0] == 18:  # UnmapNotify
        print("unmapped", flush=True)
'''

# A client that holds only the pointer (a drag, a screenshot tool's
# selection) for as long as it runs.
POINTER = XLIB + '''
print("grab", x.XGrabPointer(d, root, 0, 4 | 8, 1, 1, 0, 0, 0), flush=True)
x.XFlush(d)
time.sleep(60)
'''


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


def keyboard_free(env):
    """True when another client could grab the keyboard (nobody holds it)."""
    probe = sp.run([sys.executable, '-c', XLIB + '''
r = x.XGrabKeyboard(d, root, 0, 1, 1, 0)
print(r)
'''], env=env, capture_output=True, text=True)
    return probe.stdout.strip() == '0'


def lock_windows(env):
    """(x, y, w, h) of the mapped lock screen windows."""
    ids = sp.run([xdotool, 'search', '--onlyvisible', '--name', '^SDE Screen Lock$'], env=env,
                 capture_output=True, text=True).stdout.split()
    out = []
    for i in ids:
        info = sp.run(['xwininfo', '-id', i], env=env, capture_output=True, text=True).stdout
        vals = {}
        for line in info.splitlines():
            line = line.strip()
            for key in ('Absolute upper-left X', 'Absolute upper-left Y', 'Width', 'Height'):
                if line.startswith(key + ':'):
                    vals[key] = int(line.split(':')[1])
        if len(vals) == 4:
            out.append((vals['Absolute upper-left X'], vals['Absolute upper-left Y'], vals['Width'],
                        vals['Height']))
    return sorted(out)


def enter_button(mon):
    """Centre of the Enter button of the form on monitor (x, y, w, h), at
    scale 1 (ui.cpp: the form starts at 44 % of the height)."""
    x, y, w, h = mon
    form_top = int(h * 0.44)
    field_y = form_top + 118 + 44
    return x + w // 2 + 160 - 6 - 17, y + field_y + 23


def start_ui(env, *extra):
    r, w = os.pipe()
    ui = sp.Popen([str(fixture), str(w), *extra], env=env, pass_fds=(w,))
    os.close(w)
    return ui, r


def unlock_by_typing(env, ui, r, click=None):
    sp.run([xdotool, 'type', '--delay', '25', 'sde-test'], env=env, check=True)
    if click:
        sp.run([xdotool, 'mousemove', str(click[0]), str(click[1]), 'click', '1'], env=env, check=True)
    else:
        sp.run([xdotool, 'key', 'Return'], env=env, check=True)
    got = read_reports(r, b'U', timeout=6)
    assert b'U' in got, 'typing the password did not unlock: %r' % got
    assert ui.wait(timeout=5) == 0
    os.close(r)


with tempfile.TemporaryDirectory(prefix='sde-lock-input-') as temp:
    number = 330 + os.getpid() % 20000
    server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1280x800x24',
                       '-nolisten', 'tcp'], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    env = dict(os.environ, DISPLAY=':%d' % number, HOME=temp, XDG_CONFIG_HOME=temp + '/config',
               LANG='C.UTF-8', LC_ALL='C.UTF-8',
               # An input method that does not exist must not matter either.
               XMODIFIERS='@im=ibus')
    env.pop('XDG_SEAT_PATH', None)
    env.pop('SDE_LOCK_FIXTURE_MONITORS', None)
    helpers = []
    try:
        for _ in range(100):
            if sp.run([xdotool, 'getmouselocation'], env=env, stdout=sp.DEVNULL,
                      stderr=sp.DEVNULL).returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('Xvfb did not start')
        if shutil.which('openbox'):
            helpers.append(sp.Popen(['openbox', '--sm-disable'], env=env, stdout=sp.DEVNULL,
                                    stderr=sp.DEVNULL))
            time.sleep(0.8)

        # 1. A stuck popup menu holds both grabs.
        popup = sp.Popen([sys.executable, '-c', POPUP], env=env, stdout=sp.PIPE, text=True)
        helpers.append(popup)
        assert popup.stdout.readline().split() == ['grabs', '0', '0'], 'test popup did not grab'
        ui, r = start_ui(env)
        got = read_reports(r, b'ML', timeout=10)
        assert b'L' in got, 'the lock screen did not get the grabs from the popup: %r' % got
        assert popup.stdout.readline().strip() == 'unmapped', 'the popup was not unmapped'
        assert not keyboard_free(env), 'keyboard not grabbed'
        shot(env, 'input-1-after-popup.png')
        unlock_by_typing(env, ui, r)
        popup.kill()
        print('1. stuck popup menu: grabs recovered, password typed, unlocked')

        # 2. Only the pointer is held by another client, for good.
        holder = sp.Popen([sys.executable, '-c', POINTER], env=env, stdout=sp.PIPE, text=True)
        helpers.append(holder)
        assert holder.stdout.readline().split() == ['grab', '0'], 'test pointer grab failed'
        ui, r = start_ui(env)
        got = read_reports(r, b'MW', timeout=10)
        assert b'W' in got and b'L' not in got, got
        assert not keyboard_free(env), 'the keyboard must be grabbed although the pointer is not'
        time.sleep(0.4)
        shot(env, 'input-2-pointer-held.png')
        unlock_by_typing(env, ui, r)
        holder.kill()
        holder.wait()
        print('2. pointer held elsewhere: keyboard grabbed, password typed, unlocked')

        # 3. Monitor changes while locked.
        monitors = Path(temp, 'monitors')
        monitors.write_text('0 0 1280 800\n')
        env3 = dict(env, SDE_LOCK_FIXTURE_MONITORS=str(monitors))
        sp.run([xdotool, 'mousemove', '300', '300'], env=env, check=True)
        ui, r = start_ui(env3)
        got = read_reports(r, b'ML', timeout=10)
        assert b'L' in got, got
        assert lock_windows(env) == [(0, 0, 1280, 800)], lock_windows(env)
        # Smaller screen (a projector, a resume without the external monitor).
        monitors.write_text('0 0 1024 600\n')
        for _ in range(40):
            if lock_windows(env) == [(0, 0, 1024, 600)]:
                break
            time.sleep(0.1)
        assert lock_windows(env) == [(0, 0, 1024, 600)], lock_windows(env)
        # Two monitors: the form follows the pointer to the second one.
        monitors.write_text('0 0 640 800\n640 0 640 800\n')
        for _ in range(40):
            if lock_windows(env) == [(0, 0, 640, 800), (640, 0, 640, 800)]:
                break
            time.sleep(0.1)
        assert lock_windows(env) == [(0, 0, 640, 800), (640, 0, 640, 800)], lock_windows(env)
        assert not keyboard_free(env), 'keyboard lost during the monitor change'
        sp.run([xdotool, 'mousemove', '900', '200'], env=env, check=True)
        time.sleep(0.3)
        # 4. A resume: SIGUSR1 re-checks everything; input keeps working.
        ui.send_signal(signal.SIGUSR1)
        time.sleep(0.5)
        assert ui.poll() is None
        assert not keyboard_free(env), 'keyboard lost after SIGUSR1'
        shot(env, 'input-3-two-monitors.png')
        unlock_by_typing(env, ui, r, click=enter_button((640, 0, 640, 800)))
        print('3./4. monitor changes and resume: windows follow, click and typing unlock')

        # 5. The daemon's rescue password prompt (shown when the lock screen
        # process keeps failing): a plain field that takes the password.
        ui, r = start_ui(env, '--rescue')
        got = read_reports(r, b'L', timeout=8)
        assert b'L' in got, got
        assert not keyboard_free(env), 'keyboard not grabbed on the rescue prompt'
        sp.run([xdotool, 'type', '--delay', '25', 'wrong'], env=env, check=True)
        sp.run([xdotool, 'key', 'Return'], env=env, check=True)
        time.sleep(0.6)
        shot(env, 'input-5-rescue-prompt.png')
        assert ui.poll() is None, 'a wrong password closed the rescue prompt'
        time.sleep(0.8)  # the attempt delay after one failure is 1 s
        unlock_by_typing(env, ui, r)
        print('5. rescue prompt: wrong password rejected, right one accepted')
        print('Lock screen input recovery: all passed.')
    finally:
        for h in helpers:
            if h.poll() is None:
                h.kill()
                h.wait()
        server.terminate()
        server.wait()
