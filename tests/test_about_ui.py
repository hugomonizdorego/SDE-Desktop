#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Opens the About window of SDE programs on a private Xvfb, walks through
its sections and opens the license viewer.

test_about_ui.py NAME=BINARY ...
  NAME: niny, sysmon, run, display, calculator, welcome, reports, merkadu, debrun, or
  licenses (BINARY is sde-ui-kit-demo: the Open-Source Licenses dialog)

Each program must show "About <name>"; Ctrl+Tab must move through the
About, Credits, License and Legal sections and Alt+1/Alt+3 jump to one;
Alt+V on the License section opens "<name> — License" with the full text,
Escape closes it and the About window stays. About closes with Escape or
the Close button (Return) and the program keeps running. Niny also opens the
find bar, the command palette and the settings page (alpha 50 regression:
they had no window). The About window and the viewer must fit a 1024x600
screen at 150 % (second pass with the "run" program). The windows log
what they show to SDE_ABOUT_QA_FILE. SDE_ABOUT_SCREENSHOTS=DIR saves
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
        out = xdo(env, 'search', '--onlyvisible', '--name', title).stdout.split()
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
        if not xdo(env, 'search', '--onlyvisible', '--name', title).stdout.split():
            return True
        time.sleep(0.1)
    return False


def geometry(env, wid):
    out = xdo(env, 'getwindowgeometry', '--shell', wid).stdout
    return {k: int(v) for k, v in (l.split('=', 1) for l in out.split() if '=' in l and l[0].isupper())
            if v.lstrip('-').isdigit()}


def diagnose(env, proc, qa):
    """What a build log needs when a window did not open: the About log,
    the program's state (R: busy, S: waiting) and where it waits, the
    focused window and every named window."""
    parts = ['qa: %s' % qa_lines(qa)]
    try:
        status = Path('/proc/%d/status' % proc.pid).read_text()
        parts.append('state: ' + next((l for l in status.splitlines() if l.startswith('State:')), '?'))
        parts.append('wchan: ' + Path('/proc/%d/wchan' % proc.pid).read_text().strip())
    except OSError as e:
        parts.append('proc: %s' % e)
    focused = xdo(env, 'getwindowfocus', 'getwindowname').stdout.strip()
    parts.append('focus: %s' % focused)
    names = xdo(env, 'search', '--name', '.', 'getwindowname', '%@').stdout.strip().split('\n')
    parts.append('windows: %s' % [n for n in names if n])
    return '; '.join(parts)


def qa_lines(path):
    try:
        return Path(path).read_text().splitlines()
    except OSError:
        return []


def wait_qa(path, line, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        if line in qa_lines(path):
            return True
        time.sleep(0.1)
    return False


# name -> (main window title, About title, keys that open About)
CASES = {
    'niny': ('^Untitled .* Niny$', 'About Niny', None),  # "Untitled — Niny"
    'sysmon': ('^System Monitor$', 'About System Monitor', ['F1']),
    'run': ('^Run Command$', 'About Run Command', ['F1']),
    'display': ('^Display Settings$', 'About Display Settings', ['F1']),
    'calculator': ('^Calculator$', 'About Calculator', ['F1']),
    'welcome': ('^Welcome$', 'About SDE Welcome', ['F1']),
    'reports': ('^System Reports$', 'About System Reports', ['F1']),
    'merkadu': ('^Merkadu$', 'About Merkadu', ['F1']),
    'debrun': ('^Debrun$', 'About Debrun', ['F1']),
}


def open_niny_about(env, main):
    # Command palette, then "About Niny" (the same command as View > About).
    xdo(env, 'key', 'ctrl+shift+p')
    time.sleep(0.4)
    xdo(env, 'type', '--delay', '20', 'about niny')
    time.sleep(0.3)
    xdo(env, 'key', 'Return')


def start_x(number, size, temp):
    server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', size + 'x24',
                       '-nolisten', 'tcp'], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    env = make_env(number, temp)
    for _ in range(100):
        if xdo(env, 'getmouselocation').returncode == 0:
            return server, env
        time.sleep(0.05)
    server.terminate()
    raise RuntimeError('Xvfb did not start')


def make_env(number, temp):
    stubs = Path(temp) / 'bin'
    stubs.mkdir(exist_ok=True)
    for prog in ['xdg-open', 'gio']:  # the Website button must not start a browser
        (stubs / prog).write_text('#!/bin/sh\nexit 0\n')
        (stubs / prog).chmod(0o755)
    return dict(os.environ, DISPLAY=':%d' % number, HOME=temp, XDG_CONFIG_HOME=temp + '/config',
                XDG_RUNTIME_DIR=temp, LANG='C.UTF-8', LC_ALL='C.UTF-8',
                SDE_ABOUT_QA_FILE=temp + '/about-qa.log',
                DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-sde-about-test-bus',
                PATH='%s:%s' % (stubs, os.environ.get('PATH', '/usr/bin:/bin')))


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except sp.TimeoutExpired:
            proc.kill()
            proc.wait()


def log_problems(name, log):
    log.seek(0)
    text = log.read()
    log.close()
    bad = [l for l in text.splitlines() if 'ERROR: AddressSanitizer' in l or 'runtime error' in l
           or 'Segmentation' in l or ': BUG:' in l]
    return ['%s: %s' % (name, bad[0])] if bad else []


def walk_sections(env, name, about, win, qa, failures, fit=None):
    """Ctrl+Tab through the sections, Alt+1/Alt+3, then the license viewer."""
    focus(env, win)
    if not wait_qa(qa, 'section about'):
        failures.append('%s: About did not report its first section' % name)
        return
    for section in ('credits', 'license', 'legal'):
        open(qa, 'w').close()
        xdo(env, 'key', 'ctrl+Tab')
        if not wait_qa(qa, 'section ' + section):
            failures.append('%s: Ctrl+Tab did not show the %s section (%s)' % (name, section, qa_lines(qa)))
            return
        shot(env, 'about-%s-%s.png' % (name, section))
    open(qa, 'w').close()
    xdo(env, 'key', 'alt+1')
    if not wait_qa(qa, 'section about'):
        failures.append('%s: Alt+1 did not show the About section' % name)
    open(qa, 'w').close()
    xdo(env, 'key', 'alt+3')
    if not wait_qa(qa, 'section license'):
        failures.append('%s: Alt+3 did not show the License section' % name)
        return
    xdo(env, 'key', 'alt+v')
    viewer_title = '^%s .* License$' % about[len('About '):]
    viewer = find_window(env, viewer_title)
    if not viewer:
        failures.append('%s: Alt+V did not open the license viewer (%s)' % (name, qa_lines(qa)))
        return
    if not any(l.startswith('viewer ') for l in qa_lines(qa)):
        failures.append('%s: the license viewer was not logged' % name)
    if fit:
        fit(viewer, 'license viewer')
    shot(env, 'about-%s-viewer.png' % name)
    focus(env, viewer)
    xdo(env, 'key', 'Escape')
    if not gone(env, viewer_title):
        failures.append('%s: the license viewer did not close with Escape' % name)
    if not find_window(env, '^%s$' % about, timeout=3):
        failures.append('%s: About closed together with the license viewer' % name)


failures = []
with tempfile.TemporaryDirectory(prefix='sde-about-ui-') as temp:
    number = 400 + os.getpid() % 20000
    server, env = start_x(number, '1280x800', temp)
    qa = env['SDE_ABOUT_QA_FILE']
    try:
        for name, binary in programs.items():
            if name == 'licenses':
                continue
            if name not in CASES:
                failures.append('%s: unknown program' % name)
                continue
            title, about, keys = CASES[name]
            failed_before = len(failures)
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
                    open(qa, 'w').close()
                    focus(env, main)
                    if keys:
                        xdo(env, 'key', *keys)
                    else:
                        open_niny_about(env, main)
                    win = find_window(env, '^%s$' % about)
                    if proc.poll() is not None:
                        failures.append('%s: exited (%s) while opening About' % (name, proc.returncode))
                        break
                    if not win:
                        failures.append('%s: "%s" did not open (%s)' % (name, about, diagnose(env, proc, qa)))
                        break
                    shot(env, 'about-%s-%d.png' % (name, attempt))
                    if attempt == 0:
                        walk_sections(env, name, about, win, qa, failures)
                    focus(env, win)
                    xdo(env, 'key', 'Escape' if attempt == 0 else 'Return')  # Close is the default button
                    if not gone(env, '^%s$' % about):
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
                stop(proc)
                if len(failures) > failed_before:
                    # What the program wrote, so that a build log shows why.
                    log.seek(0)
                    for line in log.read().splitlines()[-20:]:
                        print('%s output: %s' % (name, line))
                failures += log_problems(name, log)

        # Control Center > About > Open-source licenses (the same dialog).
        if 'licenses' in programs:
            open(qa, 'w').close()
            log = open(Path(temp) / 'licenses.log', 'w+')
            proc = sp.Popen([programs['licenses'], '--show', 'licenses'], env=env, stdout=log, stderr=sp.STDOUT)
            try:
                dialog = find_window(env, '^Open-Source Licenses$', timeout=15)
                if not dialog:
                    failures.append('licenses: the Open-Source Licenses dialog did not open')
                else:
                    focus(env, dialog)
                    shot(env, 'licenses-0.png')
                    xdo(env, 'key', 'Down', 'Down', 'Return')
                    viewer = find_window(env, ' License$')
                    if not viewer:
                        failures.append('licenses: Enter did not open the license text (%s)' % qa_lines(qa))
                    else:
                        if not any(l.startswith('viewer ') for l in qa_lines(qa)):
                            failures.append('licenses: the viewer was not logged')
                        shot(env, 'licenses-viewer.png')
                        focus(env, viewer)
                        xdo(env, 'key', 'Escape')
                        if not gone(env, ' License$'):
                            failures.append('licenses: the viewer did not close')
                    focus(env, dialog)
                    xdo(env, 'key', 'Escape')
                    if not gone(env, '^Open-Source Licenses$'):
                        failures.append('licenses: the dialog did not close with Escape')
                    if proc.poll() is not None:
                        failures.append('licenses: the program exited (%s)' % proc.returncode)
            finally:
                stop(proc)
                failures += log_problems('licenses', log)
    finally:
        server.terminate()
        server.wait()

    # 1024x600 at 150 %: About and the license viewer stay on the screen.
    if 'run' in programs:
        small = tempfile.mkdtemp(prefix='small-', dir=temp)
        Path(small, 'config/sde').mkdir(parents=True)
        Path(small, 'config/sde/sde.conf').write_text('[appearance]\nscale=150\n')
        server, env = start_x(number + 1, '1024x600', small)
        qa = env['SDE_ABOUT_QA_FILE']
        log = open(Path(small) / 'run.log', 'w+')
        proc = sp.Popen([programs['run']], env=env, stdout=log, stderr=sp.STDOUT)

        def fit(wid, what):
            g = geometry(env, wid)
            if not g or g['X'] < 0 or g['Y'] < 0 or g['X'] + g['WIDTH'] > 1024 or g['Y'] + g['HEIGHT'] > 600:
                failures.append('run at 1024x600, 150 %%: the %s does not fit the screen (%s)' % (what, g))

        try:
            main = find_window(env, '^Run Command$', timeout=15)
            if not main:
                failures.append('run at 1024x600, 150 %: the window did not appear')
            else:
                focus(env, main)
                xdo(env, 'key', 'F1')
                win = find_window(env, '^About Run Command$')
                if not win:
                    failures.append('run at 1024x600, 150 %: About did not open')
                else:
                    fit(win, 'About window')
                    shot(env, 'about-small-0.png')
                    walk_sections(env, 'run (1024x600)', 'About Run Command', win, qa, failures, fit)
                    focus(env, win)
                    xdo(env, 'key', 'Escape')
                    if not gone(env, '^About Run Command$'):
                        failures.append('run at 1024x600, 150 %: About did not close')
        finally:
            stop(proc)
            failures += log_problems('run (1024x600)', log)
            server.terminate()
            server.wait()

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('About windows: sections, license viewer, open and close: %s' % ', '.join(programs))
