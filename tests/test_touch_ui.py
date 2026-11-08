#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Touch screens in SDE-FLTK on a private Xvfb (tests/touch_probe.cpp).

Xvfb has no touch device and XTest cannot touch, so the probe feeds its
touches through Fl_Touch::inject(), the same dispatch as the X server's
XInput 2.2 touch events; the XInput selection of its windows is checked on
the real server, and the mouse through XTest.

Then the session's helper, sde-touchscreen: it runs once per display, waits
quietly without a touch screen (no touch device on Xvfb), follows Show
touches in sde.conf and ends on SIGTERM.

Usage: test_touch_ui.py PROBE [SDE-TOUCHSCREEN]
Exit status 77 (skipped) without Xvfb or XInput 2.2.
"""
import os
import shutil
import subprocess as sp
import sys
import tempfile


def main():
    probe = os.path.abspath(sys.argv[1])
    if not shutil.which('Xvfb'):
        print('skip: no Xvfb')
        return 77
    read_fd, write_fd = os.pipe()
    # -displayfd: the server picks a free display, so parallel tests never meet.
    server = sp.Popen(['Xvfb', '-displayfd', str(write_fd), '-noreset', '-nolisten', 'tcp',
                       '-screen', '0', '1024x768x24'], pass_fds=(write_fd,),
                      stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    os.close(write_fd)
    try:
        number = b''
        while not number.endswith(b'\n'):
            chunk = os.read(read_fd, 16)
            if not chunk:
                break
            number += chunk
        os.close(read_fd)
        if not number.strip():
            print('skip: Xvfb did not start')
            return 77
        with tempfile.TemporaryDirectory(prefix='sde-touch-') as home:
            env = dict(os.environ, DISPLAY=':' + number.decode().strip(), HOME=home,
                       XDG_CONFIG_HOME=os.path.join(home, '.config'))
            env.pop('SDE_TOUCH', None)
            result = sp.run([probe], env=env, timeout=120)
            if result.returncode != 0 or len(sys.argv) < 3:
                return result.returncode
            return helper(os.path.abspath(sys.argv[2]), env, home)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except sp.TimeoutExpired:
            server.kill()


def helper(exe, env, home):
    import time
    conf = os.path.join(home, '.config', 'sde')
    os.makedirs(conf, exist_ok=True)
    with open(os.path.join(conf, 'sde.conf'), 'w') as f:
        f.write('[touchscreen]\nshow_touches=true\n')
    first = sp.Popen([exe], env=env, stdout=sp.DEVNULL, stderr=sp.PIPE, text=True)
    failures = 0

    def check(ok, what):
        nonlocal failures
        print(('ok    ' if ok else 'FAIL  ') + what)
        failures += 0 if ok else 1

    try:
        time.sleep(1.0)
        check(first.poll() is None, 'sde-touchscreen keeps running without a touch screen')
        second = sp.run([exe], env=env, timeout=10, stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        check(second.returncode == 0, 'a second sde-touchscreen on the display ends at once')
        check(first.poll() is None, '... and leaves the first one running')
    finally:
        first.terminate()
        try:
            first.wait(timeout=5)
        except sp.TimeoutExpired:
            first.kill()
    check(first.returncode is not None and first.returncode >= 0 or first.returncode == -15,
          'sde-touchscreen ends on SIGTERM')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
