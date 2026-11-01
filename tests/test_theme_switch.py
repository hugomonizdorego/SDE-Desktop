#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""How a theme switch reaches the programs of a session (0.2.0 Alpha 3.1,
docs/THEMES.md "Switching themes"), on a private Xvfb with the real
sde-session and stand-ins for the programs it starts, which log the
signals they get: sde-compositor, sde-xsettingsd, the window manager (an
xev on the root window sees its _OB_CONTROL messages), and a pkill that
must never be called.

test_theme_switch.py SDE_SESSION BUILD_DIR

Checked, for "sde-session --apply-appearance --wait" after sde.conf
changed:
  * an accent: XSETTINGS re-read once, the window manager reconfigured
    once, the compositor not signalled at all;
  * a family: the compositor reloaded once with SIGUSR1 (in place), never
    restarted (the same process all along);
  * transparency only: the compositor, not XSETTINGS;
  * nothing changed: nobody is told;
  * every apply announces _SDE_CONFIG_SERIAL exactly once, is answered in
    _SDE_APPEARANCE_DONE (--wait returns), shows the picture of the screen
    while it switches ("SDE theme switch"), and nothing runs pkill.

SDE_TEST_DISPLAY=N picks the X display."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) < 3 or not all(shutil.which(t) for t in ('Xvfb', 'xprop', 'xdotool')):
    print('Xvfb, xprop, xdotool and sde-session are required; skipped')
    sys.exit(77)
SESSION, BUILD = sys.argv[1], Path(sys.argv[2])
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


STUB_SIGNALS = r'''#!/usr/bin/env python3
import os, signal, sys, time
log = os.environ['STUB_LOG_DIR'] + '/' + os.path.basename(sys.argv[0]) + '.log'
def say(what):
    with open(log, 'a') as f:
        f.write(what + '\n')
say('start %d' % os.getpid())
signal.signal(signal.SIGUSR1, lambda *a: say('usr1'))
signal.signal(signal.SIGUSR2, lambda *a: say('usr2'))
signal.signal(signal.SIGHUP, lambda *a: say('hup'))
signal.signal(signal.SIGTERM, lambda *a: (say('term'), sys.exit(0)))
while True:
    time.sleep(1)
'''

STUB_WM = '''#!/bin/sh
# Announces itself as the window manager and logs what is sent to the root.
xprop -root -f _NET_SUPPORTING_WM_CHECK 32c -set _NET_SUPPORTING_WM_CHECK 1
if command -v xev >/dev/null; then
  exec xev -root -event substructure > "$STUB_LOG_DIR/wm.log" 2>&1
fi
exec sleep 100000
'''

STUB_SLEEP = '#!/bin/sh\nexec sleep 100000\n'
STUB_TRUE = '#!/bin/sh\nexit 0\n'
STUB_PKILL = '#!/bin/sh\necho "pkill $*" >> "$STUB_LOG_DIR/pkill.log"\nexit 1\n'


class Session:
    def __init__(self):
        self.temp = Path(tempfile.mkdtemp(prefix='theme-switch-'))
        home = self.temp / 'home'
        self.home = home
        self.logs = self.temp / 'logs'
        self.logs.mkdir()
        bindir = self.temp / 'bin'
        bindir.mkdir()
        for name, text in (('sde-compositor', STUB_SIGNALS), ('sde-xsettingsd', STUB_SIGNALS), ('sde-wm', STUB_WM),
                           ('sde-desktop', STUB_SLEEP), ('sde-panel', STUB_SLEEP), ('sde-notifyd', STUB_SLEEP),
                           ('sde-settings', STUB_TRUE), ('sde-autostart', STUB_TRUE), ('pkill', STUB_PKILL)):
            (bindir / name).write_text(text)
            (bindir / name).chmod(0o755)
        (home / '.config/sde/openbox').mkdir(parents=True)
        shutil.copyfile(ROOT / 'data/openbox/rc.xml', home / '.config/sde/openbox/rc.xml')
        run = self.temp / 'run'
        run.mkdir(mode=0o700)
        self.write_conf('graphite', 'blue', 25)
        self.procs = []
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        self.server = None
        for attempt in range(1 if fixed else 10):
            number = int(fixed) if fixed else 700 + (os.getpid() + attempt * 53) % 20000
            env = dict(os.environ, DISPLAY=':%d' % number)
            server = sp.Popen(['Xvfb', ':%d' % number, '-screen', '0', '800x600x24', '-nolisten', 'tcp', '-noreset'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            for _ in range(100):
                if server.poll() is not None:
                    break
                if sp.run(['xprop', '-root', '-display', ':%d' % number], env=env,
                          capture_output=True).returncode == 0:
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
        self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(home), PATH='%s:%s' % (bindir, os.environ['PATH']),
                        XDG_CONFIG_HOME=str(home / '.config'), XDG_CONFIG_DIRS=str(self.temp / 'xdg'),
                        XDG_DATA_HOME=str(home / '.local/share'), XDG_STATE_HOME=str(home / '.local/state'),
                        XDG_CACHE_HOME=str(home / '.cache'), XDG_RUNTIME_DIR=str(run),
                        XDG_DATA_DIRS='%s:/usr/local/share:/usr/share' % BUILD, SDE_GRAPHICS='hardware',
                        GSETTINGS_BACKEND='memory', STUB_LOG_DIR=str(self.logs), LANG='C.UTF-8', LC_ALL='C.UTF-8')
        self.env.pop('DBUS_SESSION_BUS_ADDRESS', None)

    def write_conf(self, family, accent, transparency):
        conf = self.home / '.config/sde/sde.conf'
        conf.parent.mkdir(parents=True, exist_ok=True)
        conf.write_text('[appearance]\nstyle=%s\ntheme=light\naccent_name=%s\n[aero]\nenabled=true\n'
                        'transparency=%d\n[compositor]\nmode=auto\nbackend=xrender\n[session]\npolkit_agent=none\n'
                        % (family, accent, transparency))

    def start(self):
        self.session = sp.Popen([SESSION], env=self.env, stdout=open(self.temp / 'session.out', 'w'),
                                stderr=sp.STDOUT)
        self.procs.insert(0, self.session)
        ok = self.wait(lambda: self.lines('sde-compositor') and self.lines('sde-xsettingsd') and
                       sp.run(['xdotool', 'search', '--name', '^sde-session$'], env=self.env,
                              capture_output=True).returncode == 0, 20)
        if not ok:
            raise RuntimeError('the session did not start: ' + (self.temp / 'session.out').read_text())

    def wait(self, fn, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if fn():
                return True
            time.sleep(0.05)
        return False

    def lines(self, name):
        try:
            return (self.logs / (name + '.log')).read_text().splitlines()
        except OSError:
            return []

    def count(self, name, what):
        return sum(1 for l in self.lines(name) if l == what)

    def wm_messages(self):
        try:
            return (self.logs / 'wm.log').read_text().count('(_OB_CONTROL)')
        except OSError:
            return -1

    def serial(self):
        out = sp.run(['xprop', '-root', '_SDE_CONFIG_SERIAL'], env=self.env, capture_output=True, text=True).stdout
        try:
            return int(out.strip().split('=')[-1])
        except ValueError:
            return 0

    def apply(self, watch_picture=False):
        """sde-session --apply-appearance --wait; True when the picture of
        the screen was seen while it ran (|watch_picture|)."""
        p = sp.Popen([SESSION, '--apply-appearance', '--wait'], env=self.env)
        seen = False
        while p.poll() is None:
            if watch_picture and not seen:
                seen = sp.run(['xdotool', 'search', '--name', '^SDE theme switch$'], env=self.env,
                              capture_output=True).returncode == 0
            time.sleep(0.02)
        check(p.returncode == 0, 'sde-session --apply-appearance --wait failed')
        time.sleep(0.3)  # signals and the xev log
        return seen

    def close(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=8)
                except sp.TimeoutExpired:
                    p.kill()
                    p.wait()
        shutil.rmtree(self.temp, ignore_errors=True)


def main():
    s = Session()
    try:
        s.start()
        gtk_themes = (BUILD / 'themes/SDE-Graphite-Light-Teal/gtk-3.0/gtk.css').exists()
        wm = s.wm_messages() >= 0
        start = s.lines('sde-compositor')[0]

        # An accent: GTK and the window manager, not the compositor.
        serial = s.serial()
        s.write_conf('graphite', 'teal', 25)
        t0 = time.time()
        seen = s.apply(watch_picture=True)
        check(time.time() - t0 < 6, 'the switch was not answered in time (_SDE_APPEARANCE_DONE)')
        check(seen, 'no picture of the screen during the switch')
        check(s.serial() == serial + 1, 'the accent change was not announced exactly once (%d -> %d)'
              % (serial, s.serial()))
        check(s.count('sde-compositor', 'usr1') == 0, 'an accent change reloaded the compositor')
        if gtk_themes:
            check(s.count('sde-xsettingsd', 'hup') == 1, 'XSETTINGS was not re-read once for the accent (%d)'
                  % s.count('sde-xsettingsd', 'hup'))
        if wm:
            check(s.wm_messages() == 1, 'the window manager was not reconfigured once (%d)' % s.wm_messages())

        # A family: the compositor reloads in place, once.
        serial = s.serial()
        s.write_conf('fluent', 'teal', 25)
        s.apply()
        check(s.serial() == serial + 1, 'the family change was not announced exactly once')
        check(s.count('sde-compositor', 'usr1') == 1, 'the compositor was not reloaded once for a family (%d)'
              % s.count('sde-compositor', 'usr1'))
        if gtk_themes:
            check(s.count('sde-xsettingsd', 'hup') == 2, 'XSETTINGS was not re-read for the family')
        if wm:
            check(s.wm_messages() == 2, 'the window manager was not reconfigured once more (%d)' % s.wm_messages())

        # Transparency only: the compositor, not XSETTINGS.
        hups = s.count('sde-xsettingsd', 'hup')
        s.write_conf('fluent', 'teal', 80)
        s.apply()
        check(s.count('sde-compositor', 'usr1') == 2, 'the compositor was not reloaded for the transparency')
        check(s.count('sde-xsettingsd', 'hup') == hups, 'XSETTINGS was re-read for a transparency change')

        # Nothing changed: nobody is told.
        usr1, hups, msgs = s.count('sde-compositor', 'usr1'), s.count('sde-xsettingsd', 'hup'), s.wm_messages()
        s.apply()
        check(s.count('sde-compositor', 'usr1') == usr1 and s.count('sde-xsettingsd', 'hup') == hups and
              s.wm_messages() == msgs, 'an apply without changes signalled a program')

        # Never restarted, never pkill.
        check(s.lines('sde-compositor')[0] == start and
              sum(1 for l in s.lines('sde-compositor') if l.startswith('start')) == 1,
              'the compositor was restarted')
        check(s.count('sde-compositor', 'usr2') == 0 and s.count('sde-compositor', 'term') == 0,
              'the compositor was stopped')
        check(not (s.logs / 'pkill.log').exists(), 'pkill was run: ' +
              ((s.logs / 'pkill.log').read_text() if (s.logs / 'pkill.log').exists() else ''))
        if failures:
            log = s.home / '.local/state/sde/session.log'
            print('--- session.log\n' + (log.read_text() if log.exists() else '(none)'))
    finally:
        s.close()
    if failures:
        print('%d check(s) failed' % len(failures))
        sys.exit(1)
    print('test_theme_switch: all passed')


if __name__ == '__main__':
    main()
