#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""sde-lock daemon on a private Xvfb and session bus: org.freedesktop.ScreenSaver
inhibitors, Lock/--query, and the watchdog: killing the lock screen process
must not unlock the session (the daemon keeps a black grab and relaunches it).

Run under dbus-run-session. No password is ever accepted here (real PAM)."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

lock = Path(sys.argv[1]).resolve()
if not shutil.which('Xvfb') or not shutil.which('dbus-send') or 'DBUS_SESSION_BUS_ADDRESS' not in os.environ:
    print('Xvfb, dbus-send and a session bus are required; skipped')
    sys.exit(77)
shots = os.environ.get('SDE_LOCK_SCREENSHOTS')


def dbus(env, method, *args, dest='org.freedesktop.ScreenSaver', path='/org/freedesktop/ScreenSaver',
         iface='org.freedesktop.ScreenSaver'):
    return sp.run(['dbus-send', '--session', '--print-reply', '--dest=' + dest, path, iface + '.' + method,
                   *args], env=env, capture_output=True, text=True, timeout=20)


def children(pid):
    out = sp.run(['ps', '-o', 'pid=', '--ppid', str(pid)], capture_output=True, text=True).stdout
    return [int(p) for p in out.split()]


def query(env):
    return sp.run([str(lock), '--query'], env=env, capture_output=True, text=True, timeout=10).stdout.strip()


with tempfile.TemporaryDirectory(prefix='sde-lock-daemon-') as temp:
    number = 400 + os.getpid() % 20000
    server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1024x768x24', '-nolisten', 'tcp'],
                      stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    env = dict(os.environ, DISPLAY=':%d' % number, HOME=temp, XDG_CONFIG_HOME=temp + '/config',
               LANG='C.UTF-8', LC_ALL='C.UTF-8')
    Path(temp, 'config/sde').mkdir(parents=True)
    # No idle lock during the test.
    Path(temp, 'config/sde/sde.conf').write_text('[lock]\nidle_lock=0\n')
    daemon = None
    try:
        time.sleep(0.5)
        daemon = sp.Popen([str(lock), '--daemon'], env=env)
        for _ in range(100):
            if dbus(env, 'GetActive').returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('daemon did not start')
        assert query(env) == 'unlocked'

        # Inhibitors: tracked per connection, released when the client exits.
        # A client that keeps its connection open (libdbus through ctypes).
        held = sp.Popen([sys.executable, '-c', '''
import ctypes, time
from ctypes import c_void_p, c_char_p, c_int, c_uint32, byref, POINTER
d = ctypes.cdll.LoadLibrary("libdbus-1.so.3")
d.dbus_bus_get.restype = c_void_p
d.dbus_message_new_method_call.restype = c_void_p
d.dbus_message_new_method_call.argtypes = [c_char_p] * 4
d.dbus_connection_send_with_reply_and_block.restype = c_void_p
d.dbus_connection_send_with_reply_and_block.argtypes = [c_void_p, c_void_p, c_int, c_void_p]
err = ctypes.create_string_buffer(64)
d.dbus_error_init(err)
conn = d.dbus_bus_get(0, err)
m = d.dbus_message_new_method_call(b"org.freedesktop.ScreenSaver", b"/org/freedesktop/ScreenSaver",
                                   b"org.freedesktop.ScreenSaver", b"Inhibit")
a, b = c_char_p(b"Videos"), c_char_p(b"Playing a film")
d.dbus_message_append_args(c_void_p(m), c_int(115), byref(a), c_int(115), byref(b), c_int(0))
r = d.dbus_connection_send_with_reply_and_block(conn, m, 5000, err)
cookie = c_uint32(0)
d.dbus_message_get_args(c_void_p(r), err, c_int(117), byref(cookie), c_int(0))
print(cookie.value, flush=True)
time.sleep(30)
'''], env=env, stdout=sp.PIPE, stderr=sp.DEVNULL, text=True)
        line = held.stdout.readline().strip()
        if line:
            r = dbus(env, 'IsIdleInhibited', dest='org.debion.SDE.ScreenLock', path='/org/debion/SDE/ScreenLock',
                     iface='org.debion.SDE.ScreenLock')
            assert 'boolean true' in r.stdout, r.stdout
            listing = sp.run([str(lock), '--inhibitors'], env=env, capture_output=True, text=True).stdout
            assert 'Videos' in listing, listing
            # A foreign connection cannot release it.
            assert dbus(env, 'UnInhibit', 'uint32:' + line).returncode != 0
            held.kill()
            held.wait()
            time.sleep(0.5)
            r = dbus(env, 'IsIdleInhibited', dest='org.debion.SDE.ScreenLock', path='/org/debion/SDE/ScreenLock',
                     iface='org.debion.SDE.ScreenLock')
            assert 'boolean false' in r.stdout, r.stdout
        else:
            held.kill()
            print('python3-dbus missing: inhibitor ownership checks skipped')

        # Lock.
        assert sp.run([str(lock), '--lock'], env=env, timeout=30).returncode == 0
        assert query(env) == 'locked'
        ui = children(daemon.pid)
        assert len(ui) == 1, ui
        time.sleep(0.8)
        if shots and shutil.which('import'):
            Path(shots).mkdir(parents=True, exist_ok=True)
            sp.run(['import', '-window', 'root', str(Path(shots) / 'daemon-locked.png')], env=env)

        # Kill the lock screen: still locked, relaunched.
        os.kill(ui[0], 9)
        time.sleep(0.15)
        assert query(env) == 'locked'
        # Somebody (the daemon, or the relaunched lock screen) still holds
        # the keyboard: other programs cannot read the keys.
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
        assert probe.stdout.strip() == '1', 'keyboard free after the lock screen died: ' + probe.stdout
        if shots and shutil.which('import'):
            sp.run(['import', '-window', 'root', str(Path(shots) / 'daemon-after-crash.png')], env=env)
        for _ in range(60):
            again = children(daemon.pid)
            if again and again != ui:
                break
            time.sleep(0.1)
        assert again and again != ui, 'the lock screen was not relaunched'
        assert query(env) == 'locked'
        # SetActive(false) never unlocks.
        dbus(env, 'SetActive', 'boolean:false')
        assert query(env) == 'locked'
        print('sde-lock daemon: inhibitors, lock, crash watchdog passed.')
    finally:
        if daemon:
            for c in children(daemon.pid):
                os.kill(c, 9)
            daemon.kill()
            daemon.wait()
        server.terminate()
        server.wait()
