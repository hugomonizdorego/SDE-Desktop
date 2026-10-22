#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opens and closes the About window of SDE programs on a private Xvfb.

test_about_ui.py NAME=BINARY ...   (NAME: niny, sysmon, run, display, xanara)

Regression test for alpha 50: Niny crashed on View > About Niny because the
widgets created after the settings page (find bar, palette, question sheet)
had no window. Each program must show "About <name>", close it with Escape
or the Close button, and keep running; Niny also opens the find bar, the
command palette and the settings page. SDE_ABOUT_SCREENSHOTS=DIR saves
screenshots."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

xdotool = shutil.which('xdotool')
if not xdotool or not shutil.which('Xvfb'):
    print('xdotool and Xvfb are required; skipped')
    sys.exit(77)
shots = os.environ.get('SDE_ABOUT_SCREENSHOTS')
programs = dict(a.split('=', 1) for a in sys.argv[1:])


def xdo(env, *args, check=False):
    return sp.run([xdotool] + [str(a) for a in args], env=env, capture_output=True, text=True,
                  check=check)


def shot(env, name):
    if shots and shutil.which('import'):
        Path(shots).mkdir(parents=True, exist_ok=True)
        sp.run(['import', '-window', 'root', str(Path(shots) / name)], env=env, check=False)


def find_window(env, title, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        out = xdo(env, 'search', '--onlyvisible', '--name', '^%s$' % title).stdout.split()
        if out:
            return out[0]
        time.sleep(0.1)
    return None


def focus(env, wid):
    xdo(env, 'windowfocus', '--sync', wid)
    time.sleep(0.2)


def gone(env, title, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if not xdo(env, 'search', '--onlyvisible', '--name', '^%s$' % title).stdout.split():
            return True
        time.sleep(0.1)
    return False


# name -> (main window title, About title, keys that open About)
CASES = {
    'niny': ('Untitled .* Niny', 'About Niny', None),  # "Untitled — Niny"
    'sysmon': ('System Monitor', 'About System Monitor', ['F1']),
    'run': ('Run Command', 'About Run Command', ['F1']),
    'display': ('Display Settings', 'About Display Settings', ['F1']),
}


def open_niny_about(env, main):
    # Command palette, then "About Niny" (the same command as View > About).
    xdo(env, 'key', 'ctrl+shift+p')
    time.sleep(0.4)
    xdo(env, 'type', '--delay', '20', 'about niny')
    time.sleep(0.3)
    xdo(env, 'key', 'Return')


failures = []
with tempfile.TemporaryDirectory(prefix='sde-about-ui-') as temp:
    number = 400 + os.getpid() % 20000
    server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1280x800x24',
                       '-nolisten', 'tcp'], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    stubs = Path(temp) / 'bin'
    stubs.mkdir()
    for prog in ['xdg-open', 'gio']:  # the Website button must not start a browser
        (stubs / prog).write_text('#!/bin/sh\nexit 0\n')
        (stubs / prog).chmod(0o755)
    env = dict(os.environ, DISPLAY=':%d' % number, HOME=temp, XDG_CONFIG_HOME=temp + '/config',
               XDG_RUNTIME_DIR=temp, LANG='C.UTF-8', LC_ALL='C.UTF-8',
               PATH='%s:%s' % (stubs, os.environ.get('PATH', '/usr/bin:/bin')))
    try:
        for _ in range(100):
            if xdo(env, 'getmouselocation').returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('Xvfb did not start')
        for name, binary in programs.items():
            if name not in CASES:
                failures.append('%s: unknown program' % name)
                continue
            title, about, keys = CASES[name]
            log = open(Path(temp) / ('%s.log' % name), 'w+')
            proc = sp.Popen([binary], env=env, stdout=log, stderr=sp.STDOUT)
            try:
                main = find_window(env, title, timeout=15)
                if not main:
                    names = xdo(env, 'search', '--onlyvisible', '--name', '.', 'getwindowname', '%@').stdout
                    failures.append('%s: main window "%s" did not appear (windows: %s)' % (
                        name, title, ', '.join(names.split('\n')).strip(', ')))
                    continue
                time.sleep(0.5)
                for attempt in range(2):  # open twice: Escape, then the Close button
                    focus(env, main)
                    if keys:
                        xdo(env, 'key', *keys)
                    else:
                        open_niny_about(env, main)
                    win = find_window(env, about)
                    if proc.poll() is not None:
                        failures.append('%s: exited (%s) while opening About' % (name, proc.returncode))
                        break
                    if not win:
                        failures.append('%s: "%s" did not open' % (name, about))
                        break
                    shot(env, 'about-%s-%d.png' % (name, attempt))
                    focus(env, win)
                    if attempt == 0:
                        xdo(env, 'key', 'Escape')
                    else:
                        xdo(env, 'key', 'Return')  # Close is the default button
                    if not gone(env, about):
                        failures.append('%s: About did not close' % name)
                        break
                    time.sleep(0.3)
                    if proc.poll() is not None:
                        failures.append('%s: exited (%s) after closing About' % (name, proc.returncode))
                        break
                if name == 'niny' and proc.poll() is None:
                    # These were orphaned (no window) before the fix as well.
                    for k in (['ctrl+f'], ['Escape'], ['ctrl+shift+p'], ['Escape'], ['ctrl+comma'],
                              ['ctrl+comma']):
                        focus(env, main)
                        xdo(env, 'key', *k)
                        time.sleep(0.3)
                    shot(env, 'about-niny-after.png')
                    if proc.poll() is not None:
                        failures.append('niny: exited (%s) using find bar/palette/settings' % proc.returncode)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except sp.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                log.seek(0)
                text = log.read()
                log.close()
                bad = [l for l in text.splitlines() if 'ERROR: AddressSanitizer' in l or 'runtime error' in l
                       or 'Segmentation' in l or ': BUG:' in l]
                if bad:
                    failures.append('%s: %s' % (name, bad[0]))
    finally:
        server.terminate()
        server.wait()

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('About windows open and close: %s' % ', '.join(programs))
