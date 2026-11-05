#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Control Center > Appearance (0.2.0 Alpha 3.1) on a private Xvfb: a look
is chosen first and applied with Apply; Preview shows it before.

test_appearance_ui.py SDE_SETTINGS BUILD_DIR [SDE_THEME_PREVIEW_GTK]

Checked: choosing a family, the dark mode and the Glass level changes only
the chosen look (the configuration file keeps the look in use) and shows
the bar with Preview, Revert and Apply; Preview opens the preview window
with the chosen look (and its GTK part embedded when the helper is
given); the preview's Transparency slider changes the chosen look in the
Control Center; Cancel closes the preview and keeps the choice; Revert
returns to the look in use; Apply in the preview writes the look, the
Control Center takes it and the bar goes; Ctrl+Enter applies an accent.
Without a session the look applies at once (the session's calm switch is
tested by test_theme_switch.py).

SDE_APPEARANCE_SCREENSHOTS=DIR saves screenshots; SDE_TEST_DISPLAY=N picks
the X display."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
xdotool = shutil.which('xdotool')
if len(sys.argv) < 3 or not xdotool or not shutil.which('Xvfb') or not shutil.which('xwininfo'):
    print('xdotool, xwininfo, Xvfb and sde-settings are required; skipped')
    sys.exit(77)
SETTINGS, BUILD = sys.argv[1], Path(sys.argv[2])
HELPER = sys.argv[3] if len(sys.argv) > 3 and Path(sys.argv[3]).exists() else ''
SHOTS = os.environ.get('SDE_APPEARANCE_SCREENSHOTS')
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


def serves(server, number):
    """Whether |server| (the Xvfb started here) is the one serving :number:
    its lock file holds the server's PID. With a number already in use the
    new server exits, and until it does a check would reach the other one."""
    try:
        pid = int(Path('/tmp/.X%d-lock' % number).read_text().strip())
    except (OSError, ValueError):
        return False
    return pid == server.pid and server.poll() is None


class Session:
    def __init__(self):
        self.temp = Path(tempfile.mkdtemp(prefix='appearance-ui-'))
        home = self.temp
        (home / '.config/sde').mkdir(parents=True)
        self.conf = home / '.config/sde/sde.conf'
        self.conf.write_text('[appearance]\nstyle=graphite\ntheme=light\naccent_name=blue\nscale=100\n'
                             '[aero]\nenabled=true\ntransparency=25\n')
        (home / 'qa').mkdir()
        (home / 'run').mkdir(mode=0o700)
        self.procs = []
        self.server = None
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        for attempt in range(1 if fixed else 10):
            number = int(fixed) if fixed else 900 + (os.getpid() + attempt * 61) % 20000
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1366x768x24', '-nolisten', 'tcp'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(home),
                            XDG_CONFIG_HOME=str(home / '.config'), XDG_CONFIG_DIRS=str(home / 'etc-xdg'),
                            XDG_CACHE_HOME=str(home / '.cache'), XDG_DATA_HOME=str(home / '.local/share'),
                            XDG_RUNTIME_DIR=str(home / 'run'),
                            XDG_DATA_DIRS='%s:%s:/usr/local/share:/usr/share' % (BUILD, ROOT / 'data'),
                            SDE_PANEL_QA_DIR=str(home / 'qa'), SDE_BRANDING_DIR=str(ROOT / 'data/branding'),
                            GSETTINGS_BACKEND='memory', NO_AT_BRIDGE='1', GTK_A11Y='none',
                            LANG='C.UTF-8', LC_ALL='C.UTF-8')
            self.env.pop('DBUS_SESSION_BUS_ADDRESS', None)
            if HELPER:
                self.env['SDE_THEME_PREVIEW_GTK'] = HELPER
            for _ in range(100):
                if server.poll() is not None:
                    break
                if sp.run([xdotool, 'getmouselocation'], env=self.env, capture_output=True).returncode == 0 and serves(server, number):
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

    def start(self):
        self.app = sp.Popen([SETTINGS, '--page', 'appearance'], env=self.env,
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

    def wait(self, fn, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if fn():
                return True
            time.sleep(0.1)
        return False

    def rows(self, name):
        try:
            return [l.split('\t') for l in (self.temp / 'qa' / name).read_text().splitlines()]
        except OSError:
            return []

    def qa(self, key, name='Control-Center.txt'):
        for r in self.rows(name):
            if r[0].startswith('@' + key + ':'):
                return r[0][len(key) + 2:]
        return None

    def look(self, key, which='pending'):
        text = self.qa('appearance-' + which) or ''
        return dict(p.split('=', 1) for p in text.split(';') if '=' in p).get(key)

    def control(self, name, file='Control-Center.txt'):
        for r in self.rows(file):
            if len(r) >= 6 and not r[0].startswith('@') and (r[0] == name or name in r[0]):
                return int(r[1]), int(r[2]), int(r[3]), int(r[4])
        return None

    def origin(self, title):
        out = self.xdo('search', '--onlyvisible', '--name', title).stdout.split()
        if not out:
            return None
        info = sp.run(['xwininfo', '-id', out[-1]], env=self.env, capture_output=True, text=True).stdout
        vals = {}
        for line in info.splitlines():
            if 'Absolute upper-left' in line:
                k, v = line.split(':')
                vals[k.strip()[-1]] = int(v)
        return vals.get('X'), vals.get('Y')

    def shown(self, name):
        for r in self.rows('Control-Center.txt'):
            if len(r) >= 6 and not r[0].startswith('@') and (r[0] == name or name in r[0]):
                return r[5] == '1'
        return False

    def reveal(self, name):
        """Wheels the page until control |name| is fully in view."""
        ox, oy = self.origin('^Control Center$')
        for _ in range(40):
            c = self.control(name)
            if c and self.shown(name):
                return True
            down = not c or c[1] > 330
            self.xdo('mousemove', ox + 700, oy + 300, 'click', '--repeat', 2, '--delay', 20, '5' if down else '4')
            time.sleep(0.15)
        return False

    def press(self, name, file='Control-Center.txt', title='^Control Center$', fx=0.5, fy=0.5):
        if not self.wait(lambda: self.control(name, file) and self.origin(title)):
            check(False, 'no control %r (%s)' % (name, [r[0] for r in self.rows(file)][:40]))
            return False
        if file == 'Control-Center.txt' and not check(self.reveal(name), 'cannot scroll to %r' % name):
            return False
        x, y, w, h = self.control(name, file)
        ox, oy = self.origin(title)
        self.xdo('mousemove', ox + x + int(w * fx), oy + y + int(h * fy), 'click', '1')
        self.xdo('mousemove', 5, 5)
        time.sleep(0.4)
        return True

    def drag(self, name, f0, f1, file, title):
        x, y, w, h = self.control(name, file)
        ox, oy = self.origin(title)
        yy = oy + y + h // 2
        xa, xb = ox + x + int(w * f0), ox + x + int(w * f1)
        self.xdo('mousemove', xa, yy, 'mousedown', '1')
        for i in range(1, 16):
            self.xdo('mousemove', xa + (xb - xa) * i // 15, yy)
            time.sleep(0.03)
        self.xdo('mouseup', '1')
        self.xdo('mousemove', 5, 5)

    def disk(self, key):
        for line in self.conf.read_text().splitlines():
            if line.startswith(key + '='):
                return line.split('=', 1)[1]
        return None

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / (name + '.png'))], env=self.env)


def main():
    s = Session()
    try:
        s.start()
        if not check(s.wait(lambda: s.look('family') == 'graphite', 20), 'the Appearance page did not open'):
            return
        check(s.control('Apply the chosen look') is None, 'the bar shows without a chosen look')
        s.shot('00-in-use')

        # Choosing: only the chosen look changes.
        s.press('Fluent Design System')
        s.press('Dark mode')
        s.press('Transparency: Glass')
        check(s.wait(lambda: s.look('family') == 'fluent' and s.look('mode') == 'dark' and
                     s.look('transparency') == '80'), 'the choices did not reach the chosen look: %s'
              % s.qa('appearance-pending'))
        check(s.look('family', 'applied') == 'graphite', 'choosing applied the look')
        check(s.disk('style') == 'graphite', 'choosing wrote the configuration')
        # 0.2.0 Alpha 4: the sliders show their value (in their names too),
        # following the preset; the wallpaper colour switches.
        check(s.wait(lambda: s.control('Transparency level: 80 %') is not None),
              'the Transparency slider does not show 80 %%: %s' % [r[0] for r in s.rows('Control-Center.txt')
                                                                    if r[0].startswith('Transparency')])
        check(s.control('Blur strength: 50 %') is not None, 'the Blur strength slider does not show 50 %')
        check(s.control('Tint surfaces with the wallpaper color') is not None and
              s.control('Accent color from the wallpaper') is not None, 'no switches for the wallpaper color')
        check(s.wait(lambda: s.control('Apply the chosen look') is not None), 'no bar with Apply')
        check(s.control('Preview the chosen look') and s.control('Revert to the look in use'),
              'the bar has no Preview or Revert')
        s.shot('01-chosen')

        # Preview shows the chosen look; its slider changes the choice.
        s.press('Preview the chosen look')
        check(s.wait(lambda: (s.qa('look', 'Appearance-Preview.txt') or '').startswith('family=fluent;mode=dark'), 15),
              'the preview window did not show the chosen look')
        check(s.qa('appearance-preview') == 'open', 'the Control Center does not know the preview is open')
        if HELPER:
            check(s.wait(lambda: s.qa('gtk', 'Appearance-Preview.txt') == 'embedded', 15),
                  'the GTK part was not embedded (%s)' % s.qa('gtk', 'Appearance-Preview.txt'))
        time.sleep(0.5)
        s.shot('02-preview')
        # Along the track: since 0.2.0 Alpha 4 the value is shown at its
        # right, and the name carries it ("Transparency level (preview): 80 %").
        name = next((r[0] for r in s.rows('Appearance-Preview.txt') if r[0].startswith('Transparency level (preview)')),
                    '')
        check(name.endswith(': 80 %'), 'the preview slider does not name its value: %r' % name)
        s.drag('Transparency level (preview)', 0.6, 0.2, 'Appearance-Preview.txt', '^Appearance Preview$')
        check(s.wait(lambda: s.look('transparency') not in (None, '80')),
              "the preview's slider did not change the chosen look")
        # The preview sends a look for every step of the drag: the last one.
        moved = None
        for _ in range(20):
            now = s.look('transparency')
            if now == moved:
                break
            moved = now
            time.sleep(0.3)

        # Cancel: the preview closes, the choice stays, nothing is written.
        s.press('Close the preview without applying', 'Appearance-Preview.txt', '^Appearance Preview$')
        check(s.wait(lambda: s.qa('appearance-preview') == 'closed'), 'Cancel did not close the preview')
        check(s.look('family') == 'fluent' and s.look('transparency') == moved, 'Cancel dropped the choice')
        check(s.disk('style') == 'graphite', 'Cancel wrote the configuration')

        # Revert: back to the look in use.
        s.press('Revert to the look in use')
        check(s.wait(lambda: s.look('family') == 'graphite' and s.look('mode') == 'light'),
              'Revert did not return to the look in use')
        check(s.wait(lambda: s.control('Apply the chosen look') is None), 'the bar stayed after Revert')

        # Apply in the preview: written and taken by the Control Center.
        s.press('Fluent Design System')
        s.press('Preview the chosen look')
        check(s.wait(lambda: (s.qa('look', 'Appearance-Preview.txt') or '').startswith('family=fluent'), 15),
              'the preview did not open again')
        s.press('Apply this look', 'Appearance-Preview.txt', '^Appearance Preview$')
        check(s.wait(lambda: s.look('family', 'applied') == 'fluent', 10), 'Apply in the preview did not apply')
        check(s.disk('style') == 'fluent', 'Apply did not write the look')
        check(s.wait(lambda: s.control('Apply the chosen look') is None), 'the bar stayed after Apply')
        xs = s.temp / '.config/sde/xsettingsd.conf'
        if (BUILD / 'themes/SDE-Fluent-Light-Blue/gtk-3.0/gtk.css').exists():
            check(xs.exists() and 'SDE-Fluent-Light-Blue' in xs.read_text(), 'GTK was not given the new theme')
        s.shot('03-applied')

        # The keyboard: Ctrl+Enter applies (an accent here).
        s.press('Accent color', fx=1.5 / 9)
        check(s.wait(lambda: s.look('accent') == 'teal'), 'the accent was not chosen: %s' % s.look('accent'))
        ox, oy = s.origin('^Control Center$')
        s.xdo('mousemove', ox + 60, oy + 40)  # without a window manager the keyboard follows the pointer
        s.xdo('key', 'ctrl+Return')
        check(s.wait(lambda: s.look('accent', 'applied') == 'teal', 10), 'Ctrl+Enter did not apply')
        check(s.disk('accent_name') == 'teal', 'Ctrl+Enter did not write the accent')
    finally:
        if failures:
            print('--- settings.log\n' + (s.temp / 'settings.log').read_text(errors='replace')[-3000:])
        s.close()
    if failures:
        print('%d check(s) failed' % len(failures))
        sys.exit(1)
    print('test_appearance_ui: all passed')


if __name__ == '__main__':
    main()
