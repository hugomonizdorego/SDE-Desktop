#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Merkadu with its fake sources on a private Xvfb (no network, no root).

test_merkadu_ui.py MERKADU [SOURCE_DIR]

MERKADU_FAKE points the three sources (Debian, Flatpak, Snap) at the
fictional apps of tests/fixtures/merkadu; Merkadu logs what it shows to
SDE_PANEL_QA_DIR (Merkadu.txt, Merkadu-Uninstall.txt: "@page:", "@state:"
lines and the named controls with their geometry). Checked:

  * the lobby (banner, featured, new, top apps, departments), F1 (About
    Merkadu, closed with Escape), a department
    with its subcategories, a search with its source chips, an app page;
  * installing a Flatpak: progress, then Open and Remove; the app appears
    in Installed and the change in History; the Sources page; Ctrl+Q;
  * the refresh of the software lists when Merkadu opens and with the
    Refresh button; Home with Highlights and Recommended for You; All
    Software (every package by status, a system package that cannot be
    removed, the search), marking an update and an installation, the review
    with what comes with them, Apply; Back, Forward and Home; the Updates
    page pointing to the Software Updater;
  * an empty catalog explained, with Refresh Software Lists;
  * `merkadu --uninstall FILE` (the Start menu's contract): a Debian
    package with the packages removed with it and the space freed (exit 0
    after Uninstall), an app of SDE's own package (refused, exit 1), a
    Flatpak (Escape cancels: exit 2), a launcher in the home folder
    (removed, exit 0), a simulated failure (exit 1) and a relative path
    (exit 1 without a window);
  * at 1024x600 and 150 % the main window and the uninstall dialog fit the
    screen.

SDE_MERKADU_SCREENSHOTS=DIR saves screenshots."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

xdotool = shutil.which('xdotool')
if not xdotool or not shutil.which('Xvfb') or len(sys.argv) < 2:
    print('xdotool, Xvfb and the merkadu binary are required; skipped')
    sys.exit(77)
BINARY = sys.argv[1]
ROOT = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/merkadu'
SHOTS = os.environ.get('SDE_MERKADU_SCREENSHOTS')
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
    return ok


def xdo(env, *args):
    return sp.run([xdotool] + [str(a) for a in args], env=env, capture_output=True, text=True)


class Screen:
    """A private X server with its own home, configuration and QA folder."""

    def __init__(self, size, scale=100):
        self.temp = Path(tempfile.mkdtemp(prefix='merkadu-ui-'))
        home = self.temp
        (home / '.config/sde').mkdir(parents=True)
        (home / '.config/sde/sde.conf').write_text(
            '[appearance]\ntheme=light\nscale=%d\n[effects]\nenabled=true\n' % scale)
        (home / 'qa').mkdir()
        (home / 'run').mkdir(mode=0o700)
        stubs = home / 'bin'
        stubs.mkdir()
        for prog in ('xdg-open', 'gio'):  # links must not start a browser
            (stubs / prog).write_text('#!/bin/sh\nexit 0\n')
            (stubs / prog).chmod(0o755)
        self.server = None
        # SDE_XVFB_DISPLAYS=FIRST-LAST keeps the X servers in a range (tests
        # running side by side).
        first, last = 500, 20499
        if '-' in os.environ.get('SDE_XVFB_DISPLAYS', ''):
            first, last = (int(v) for v in os.environ['SDE_XVFB_DISPLAYS'].split('-', 1))
        for attempt in range(10):
            number = first + (os.getpid() + attempt * 37) % (last - first + 1)
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', size + 'x24',
                               '-nolisten', 'tcp'], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(home),
                            XDG_CONFIG_HOME=str(home / '.config'), XDG_CONFIG_DIRS=str(home / 'etc-xdg'),
                            XDG_CACHE_HOME=str(home / '.cache'), XDG_DATA_HOME=str(home / '.local/share'),
                            XDG_RUNTIME_DIR=str(home / 'run'),
                            XDG_DATA_DIRS='%s:/usr/local/share:/usr/share' % (ROOT / 'data'),
                            SDE_PANEL_QA_DIR=str(home / 'qa'), SDE_BRANDING_DIR=str(ROOT / 'data/branding'),
                            MERKADU_FAKE=str(FIXTURES), MERKADU_CONFIG=str(FIXTURES / 'merkadu.conf'),
                            MERKADU_DATA_DIR=str(ROOT / 'data/merkadu'), MERKADU_FAKE_SECONDS='1',
                            LANG='C.UTF-8', LC_ALL='C.UTF-8',
                            DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-merkadu-test',
                            DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent-merkadu-test',
                            PATH='%s:%s' % (stubs, os.environ.get('PATH', '/usr/bin:/bin')))
            for _ in range(100):
                if server.poll() is not None:
                    break
                if xdo(self.env, 'getmouselocation').returncode == 0:
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

    def close(self):
        self.server.terminate()
        self.server.wait()
        shutil.rmtree(self.temp, ignore_errors=True)

    def qa(self, name):
        try:
            return (self.temp / 'qa' / name).read_text().splitlines()
        except OSError:
            return []

    def controls(self, name):
        out = {}
        for line in self.qa(name):
            f = line.split('\t')
            if len(f) >= 6 and f[0] not in out:
                out[f[0]] = [int(v) for v in f[1:5]]
        return out

    def wait_qa(self, name, line, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if line in self.controls(name):
                return True
            time.sleep(0.1)
        return False

    def wait_for(self, name, test, timeout=10.0):
        """The first QA line of |name| for which |test| is true, or None."""
        end = time.time() + timeout
        while time.time() < end:
            for line in self.controls(name):
                if test(line):
                    return line
            time.sleep(0.1)
        return None

    def window(self, title, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            ids = xdo(self.env, 'search', '--onlyvisible', '--name', title).stdout.split()
            if ids:
                return ids[0]
            time.sleep(0.1)
        return None

    def geometry(self, wid):
        out = xdo(self.env, 'getwindowgeometry', '--shell', wid).stdout
        return {k: int(v) for k, v in (l.split('=', 1) for l in out.split() if '=' in l)
                if v.lstrip('-').isdigit()}

    def click(self, wid, qa_name, control):
        c = self.controls(qa_name).get(control)
        if not check(c is not None, 'no control "%s" in %s: %s' % (control, qa_name, self.qa(qa_name)[:12])):
            return False
        xdo(self.env, 'mousemove', '--window', wid, c[0] + c[2] // 2, c[1] + c[3] // 2)
        time.sleep(0.15)
        xdo(self.env, 'click', '1')
        return True

    def key(self, wid, *keys):
        xdo(self.env, 'windowfocus', '--sync', wid)
        time.sleep(0.1)
        xdo(self.env, 'key', '--clearmodifiers', *keys)

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / name)], env=self.env, check=False)

    def start(self, *args, env=None):
        log = open(self.temp / 'merkadu.log', 'a+')
        return sp.Popen([BINARY] + list(args), env=dict(self.env, **(env or {})), stdout=log, stderr=sp.STDOUT)


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except sp.TimeoutExpired:
            proc.kill()
            proc.wait()


def wait_exit(proc, timeout=8.0):
    try:
        return proc.wait(timeout=timeout)
    except sp.TimeoutExpired:
        return None


def main_window(s):
    """The main window: pages, an installation, Installed, History."""
    M = 'Merkadu.txt'
    proc = s.start()
    try:
        wid = s.window('^Merkadu$', 15)
        if not check(wid, 'the main window did not open'):
            return
        if not check(s.wait_qa(M, '@page:explore') and s.wait_qa(M, '@state:catalog=1,apps=20,jobs=0', 15),
                     'the lobby did not load the fake catalog: %s' % s.qa(M)[:3]):
            return
        c = s.controls(M)
        for name in ('banner:org.example.paintbox', 'section:featured', 'section:new', 'section:top',
                     'section:departments', 'app:org.example.starfield', 'category:graphics',
                     'nav:installed', 'nav:history', 'nav:sources'):
            check(name in c, 'lobby: %s missing' % name)
        s.shot('merkadu-lobby.png')

        # F1: About Merkadu (sde::handle_about_event; F1 may arrive with Alt).
        s.key(wid, 'F1')
        about = s.window('^About Merkadu$', 8)
        if check(about, 'F1 did not open About Merkadu'):
            s.key(about, 'Escape')
            end = time.time() + 5
            while time.time() < end and xdo(s.env, 'search', '--onlyvisible', '--name', '^About Merkadu$').stdout:
                time.sleep(0.1)
            check(not xdo(s.env, 'search', '--onlyvisible', '--name', '^About Merkadu$').stdout,
                  'About Merkadu did not close with Escape')
            check(proc.poll() is None, 'Merkadu exited after About')

        s.click(wid, M, 'nav:category:graphics')
        if check(s.wait_qa(M, '@page:category:graphics'), 'Graphics did not open'):
            c = s.controls(M)
            check('chip:sub:Photography' in c, 'Graphics: no Photography subcategory')
            check('app:org.example.paintbox' in c and 'app:org.example.lumen' in c,
                  'Graphics: Paintbox or Lumen missing')

        s.key(wid, 'ctrl+f')
        time.sleep(0.2)
        xdo(s.env, 'type', '--delay', '40', 'paint')
        if check(s.wait_qa(M, '@page:search:paint'), 'searching "paint" did not show results'):
            c = s.controls(M)
            check('app:org.example.paintbox' in c, 'search: Paintbox missing')
            check(any(k.startswith('chip:source:Debian') for k in c) and
                  any(k.startswith('chip:source:Flatpak') for k in c), 'search: source chips missing')

        s.click(wid, M, 'app:org.example.paintbox')
        if check(s.wait_qa(M, '@page:details:org.example.paintbox'), 'the app page did not open'):
            c = s.controls(M)
            check('source:debian' in c and 'source:flatpak' in c, 'app page: source chips missing')
            check('screenshots' in c, 'app page: no screenshots')

        # Install a Flatpak (Lumen): progress, then Open and Remove.
        s.key(wid, 'Escape')
        proc_args_page = s.wait_qa(M, '@page:search:paint')
        check(proc_args_page, 'Escape did not go back to the search')
        s.click(wid, M, 'nav:category:graphics')
        s.wait_qa(M, '@page:category:graphics')
        s.click(wid, M, 'app:org.example.lumen')
        if check(s.wait_qa(M, '@page:details:org.example.lumen'), 'Lumen did not open'):
            s.click(wid, M, 'action:org.example.lumen:Install')
            check(s.wait_qa(M, 'job:1', 5), 'installing did not show its progress')
            s.shot('merkadu-installing.png')
            check(s.wait_qa(M, 'action:org.example.lumen:Remove', 10), 'Lumen was not installed: %s' % s.qa(M)[:6])
            check('action:org.example.lumen:Open' in s.controls(M), 'no Open button after installing')

        s.click(wid, M, 'nav:installed')
        if check(s.wait_qa(M, '@page:installed'), 'Installed did not open'):
            c = s.controls(M)
            check('app:org.example.lumen' in c, 'Installed: Lumen missing')
            check('action:org.debion.calculator:Part of SDE' in c, 'Installed: the SDE app is not marked')

        s.click(wid, M, 'nav:history')
        if check(s.wait_qa(M, '@page:history'), 'History did not open'):
            check(s.wait_qa(M, 'Installed Lumen', 5), 'History: the installation of Lumen is missing')
            check('chip:filter:Removals' in s.controls(M), 'History: no filters')
        s.shot('merkadu-history.png')

        s.click(wid, M, 'nav:sources')
        check(s.wait_qa(M, '@page:sources'), 'Sources did not open')
        check(s.wait_qa(M, 'Hide the Snap Folder'), 'Sources: no "Hide the Snap Folder" button')

        s.key(wid, 'ctrl+q')
        status = wait_exit(proc)
        check(status == 0, 'Ctrl+Q: exit status %s' % status)
    finally:
        stop(proc)


def all_software(s):
    """The refresh, Home, All Software with marked changes, the navigation."""
    M = 'Merkadu.txt'
    proc = s.start(env={'MERKADU_FAKE_REFRESH': '2'})
    try:
        wid = s.window('^Merkadu$', 15)
        if not check(wid, 'all software: the main window did not open'):
            return
        # The software lists are refreshed in the background when Merkadu opens.
        check(s.wait_for(M, lambda l: l.startswith('@refresh:running=1'), 8),
              'no refresh when Merkadu opened: %s' % s.qa(M)[:5])
        check(s.wait_qa(M, '@refresh:running=0,errors=0,done=1', 10), 'the refresh did not finish: %s' % s.qa(M)[:5])
        check(s.wait_qa(M, '@state:catalog=1,apps=20,jobs=0', 10), 'home: the catalog did not load')
        c = s.controls(M)
        for name in ('Back', 'Forward', 'Home', 'Refresh the software lists', 'section:featured',
                     'section:recommended', 'nav:packages', 'nav:updates'):
            check(name in c, 'home: %s missing' % name)
        # The Refresh button refreshes again.
        s.click(wid, M, 'Refresh the software lists')
        check(s.wait_for(M, lambda l: l.startswith('@refresh:running=1'), 5), 'Refresh did not refresh')
        check(s.wait_qa(M, '@refresh:running=0,errors=0,done=1', 10), 'the second refresh did not finish')

        s.click(wid, M, 'nav:packages')
        if not check(s.wait_qa(M, '@page:packages') and s.wait_for(M, lambda l: l.startswith('@packages:loaded=1')) and
                     s.wait_qa(M, 'table'), 'All Software did not open: %s' % s.qa(M)[:6]):
            return
        c = s.controls(M)
        for name in ('chip:status:all', 'chip:status:installed', 'chip:status:updates', 'chip:status:available',
                     'table', 'pkg:bash', 'pkg:curl'):
            check(name in c, 'All Software: %s missing' % name)
        s.shot('merkadu-all-software.png')
        # A package of the base system cannot be marked for removal.
        s.click(wid, M, 'pkg:bash')
        if check(s.wait_qa(M, 'protected:bash', 5), 'bash is not protected: %s' % s.qa(M)[5:40]):
            check('action:mark:Mark for Removal' not in s.controls(M), 'bash can be marked for removal')
        # Updates: curl and the kernel; mark curl.
        s.click(wid, M, 'chip:status:updates')
        if check(s.wait_qa(M, '@page:packages:updates||'), 'the Updates filter did not apply'):
            pkgs = [k for k in s.controls(M) if k.startswith('pkg:')]
            check('pkg:curl' in pkgs and 'pkg:linux-image-amd64' in pkgs and 'pkg:bash' not in pkgs,
                  'Updates filter: %s' % pkgs)
            s.click(wid, M, 'pkg:curl')
            if check(s.wait_qa(M, 'action:mark:Mark for Update', 5), 'curl: no Mark for Update'):
                s.click(wid, M, 'action:mark:Mark for Update')
                check(s.wait_for(M, lambda l: l.startswith('@packages:') and l.endswith(',marks=1'), 5),
                      'curl was not marked: %s' % s.qa(M)[:5])
        # The search filters the list in place: mark python3-pixel.
        s.click(wid, M, 'chip:status:available')
        s.wait_qa(M, '@page:packages:available||')
        s.key(wid, 'ctrl+f')
        time.sleep(0.2)
        xdo(s.env, 'type', '--delay', '40', 'pixel')
        if check(s.wait_qa(M, '@page:packages:available||pixel'), 'the search did not filter All Software: %s'
                 % s.qa(M)[:2]):
            s.click(wid, M, 'pkg:python3-pixel')
            if check(s.wait_qa(M, 'action:mark:Mark for Installation', 5), 'python3-pixel: no Mark for Installation'):
                s.click(wid, M, 'action:mark:Mark for Installation')
                check(s.wait_for(M, lambda l: l.startswith('@packages:') and l.endswith(',marks=2'), 5),
                      'python3-pixel was not marked')
        # The review: what comes with the marked changes.
        s.click(wid, M, 'action:review')
        if check(s.wait_qa(M, '@page:changes') and s.wait_qa(M, 'action:apply', 8), 'the review did not open'):
            c = s.controls(M)
            for name in ('plan:install:python3-pixel', 'plan:install:libpaint2', 'plan:update:curl'):
                check(name in c, 'review: %s missing (%s)' % (name, [k for k in c if k.startswith('plan:')]))
            s.shot('merkadu-review.png')
            # Back, Forward.
            s.click(wid, M, 'Back')
            check(s.wait_qa(M, '@page:packages:available||pixel'), 'Back did not go back to All Software')
            # Filters replace the page; the review was one step: Home, All Software.
            check(s.wait_qa(M, '@nav:back=1,forward=1'), 'Back: %s' % s.qa(M)[4:5])
            s.click(wid, M, 'Forward')
            check(s.wait_qa(M, '@page:changes') and s.wait_qa(M, 'action:apply', 8), 'Forward did not return')
            s.click(wid, M, 'action:apply')
            check(s.wait_qa(M, '@page:progress', 5), 'Apply did not show the progress')
            check(s.wait_for(M, lambda l: l.startswith('@packages:') and l.endswith(',marks=0'), 5),
                  'the marks were not cleared after Apply')
            check(s.wait_for(M, lambda l: l.startswith('@state:') and l.endswith(',jobs=0'), 15),
                  'the changes did not finish: %s' % s.qa(M)[:3])
        s.click(wid, M, 'Home')
        check(s.wait_qa(M, '@page:explore'), 'Home did not go home')
        # Updates: apps only, the system is the Software Updater's.
        s.click(wid, M, 'nav:updates')
        if check(s.wait_qa(M, '@page:updates'), 'Updates did not open'):
            check(s.wait_qa(M, 'action:software-updater', 5), 'Updates: no Open Software Updater')
    finally:
        stop(proc)


def empty_catalog(s):
    """No catalog at all: Merkadu says why and offers the fix."""
    M = 'Merkadu.txt'
    proc = s.start(env={'MERKADU_FAKE_EMPTY': 'all', 'MERKADU_FAKE_REFRESH': '2'})
    try:
        wid = s.window('^Merkadu$', 15)
        if not check(wid, 'empty catalog: the main window did not open'):
            return
        check(s.wait_qa(M, 'empty:loading', 8), 'empty catalog: no loading state while refreshing: %s' % s.qa(M)[:8])
        if check(s.wait_qa(M, 'empty:lists', 12), 'empty catalog: not explained: %s' % s.qa(M)[:8]):
            c = s.controls(M)
            check('action:empty:Refresh Software Lists' in c and 'action:empty:Sources' in c,
                  'empty catalog: no Refresh Software Lists or Sources button')
            s.shot('merkadu-empty.png')
    finally:
        stop(proc)


def uninstall(s, desktop, steps, expect, title, env=None, check_state=None):
    """Runs merkadu --uninstall DESKTOP; |steps|(wid) acts; checks the exit."""
    U = 'Merkadu-Uninstall.txt'
    try:
        (s.temp / 'qa' / U).unlink()
    except OSError:
        pass
    proc = s.start('--uninstall', str(desktop), env=env)
    try:
        wid = s.window('^Uninstall$', 15)
        if not check(wid, '%s: the uninstall window did not open' % title):
            return
        if check_state and not check(s.wait_qa(U, check_state), '%s: expected %s, got %s' % (
                title, check_state, s.qa(U)[:6])):
            return
        steps(wid)
        status = wait_exit(proc, 10)
        check(status == expect, '%s: exit status %s, expected %s (%s)' % (title, status, expect, s.qa(U)[:6]))
    finally:
        stop(proc)


def uninstall_cases(s):
    U = 'Merkadu-Uninstall.txt'
    apps = FIXTURES / 'applications'

    def remove_package(wid):
        c = s.controls(U)
        check('@source:debian:waveform-studio' in c, 'waveform: wrong origin %s' % list(c)[:4])
        check('@also:libwaveform3' in c and '@also:waveform-studio-data' in c,
              'waveform: the packages removed with it are not listed')
        check('@freed:63000000' in c, 'waveform: the space freed is wrong')
        s.shot('merkadu-uninstall-confirm.png')
        s.click(wid, U, 'Uninstall')
        check(s.wait_qa(U, 'logo', 3) or s.wait_qa(U, '@state:done', 1), 'waveform: no progress')
        check(s.wait_qa(U, '@state:done', 10), 'waveform: not removed: %s' % s.qa(U)[:3])
        s.shot('merkadu-uninstall-done.png')
        s.key(wid, 'Return')

    uninstall(s, apps / 'waveform-studio.desktop', remove_package, 0, 'Debian package',
              check_state='@state:confirm')
    uninstall(s, apps / 'org.debion.Calculator.desktop', lambda wid: s.key(wid, 'Escape'), 1, 'SDE app',
              check_state='@state:refused')

    def cancel(wid):
        check('@source:flatpak:system:app/org.example.Postbox/x86_64/stable' in s.controls(U),
              'Postbox: wrong origin %s' % s.qa(U)[:3])
        s.key(wid, 'Escape')

    uninstall(s, apps / 'org.example.Postbox.desktop', cancel, 2, 'Flatpak', check_state='@state:confirm')

    launchers = s.temp / '.local/share/applications'
    launchers.mkdir(parents=True, exist_ok=True)
    mine = launchers / 'my-tool.desktop'
    mine.write_text('[Desktop Entry]\nType=Application\nName=My Tool\nExec=my-tool\nIcon=applications-other\n')

    def remove_launcher(wid):
        s.click(wid, U, 'Remove Launcher')
        check(s.wait_qa(U, '@state:done', 5), 'user launcher: not removed')
        s.key(wid, 'Return')

    uninstall(s, mine, remove_launcher, 0, 'user launcher', check_state='@state:confirm')
    check(not mine.exists(), 'user launcher: the file is still there')

    def failing(wid):
        s.click(wid, U, 'Uninstall')
        check(s.wait_qa(U, '@state:failed', 10), 'failure: not reported: %s' % s.qa(U)[:3])
        s.key(wid, 'Return')

    uninstall(s, apps / 'waveform-studio.desktop', failing, 1, 'failure',
              env={'MERKADU_FAKE_FAIL': 'waveform-studio'}, check_state='@state:confirm')

    proc = s.start('--uninstall', 'relative.desktop')
    status = wait_exit(proc, 10)
    stop(proc)
    check(status == 1, 'relative path: exit status %s' % status)


def small_screen(s):
    """1024x600 at 150 %: the windows fit the screen."""
    proc = s.start()
    try:
        wid = s.window('^Merkadu$', 15)
        if check(wid, 'small screen: the main window did not open'):
            s.wait_qa('Merkadu.txt', '@page:explore')
            g = s.geometry(wid)
            check(g.get('X', -1) >= 0 and g.get('Y', -1) >= 0 and g['X'] + g['WIDTH'] <= 1024 and
                  g['Y'] + g['HEIGHT'] <= 600, 'small screen: the main window does not fit: %s' % g)
            s.shot('merkadu-small.png')
    finally:
        stop(proc)
    proc = s.start('--uninstall', str(FIXTURES / 'applications/waveform-studio.desktop'))
    try:
        wid = s.window('^Uninstall$', 15)
        if check(wid, 'small screen: the uninstall window did not open'):
            s.wait_qa('Merkadu-Uninstall.txt', '@state:confirm')
            g = s.geometry(wid)
            check(g.get('Y', -1) >= 0 and g['Y'] + g['HEIGHT'] <= 600 and g['X'] + g['WIDTH'] <= 1024,
                  'small screen: the uninstall window does not fit: %s' % g)
    finally:
        stop(proc)


def problems(s):
    try:
        text = (s.temp / 'merkadu.log').read_text()
    except OSError:
        return
    for line in text.splitlines():
        if 'AddressSanitizer' in line or 'runtime error' in line or 'Segmentation' in line:
            failures.append('log: ' + line)


screen = Screen('1366x768')
try:
    main_window(screen)
    all_software(screen)
    empty_catalog(screen)
    uninstall_cases(screen)
    problems(screen)
finally:
    screen.close()
screen = Screen('1024x600', scale=150)
try:
    small_screen(screen)
    problems(screen)
finally:
    screen.close()

for f in failures:
    print('FAIL:', f)
print('merkadu-ui: %d problems' % len(failures))
sys.exit(1 if failures else 0)
