#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""The SDE core controller (0.2.0 Alpha 4, docs/CORE.md) on a private
session bus and Xvfb, with a temporary home: sde-core itself through gdbus,
and its client (src/common/core_client.h) through sde-core-probe.

test_core.py SDE_CORE SDE_CORE_PROBE      (inside dbus-run-session)

Checked:
  * Version, Capabilities and Introspect; old duplicate keys merged at start;
  * Settings: typed Get (b, i, s), UnknownKey, Set and its refusal by the
    schema (nothing written), SetMany with resets, Changed with the keys,
    a program that writes sde.conf itself and bumps _SDE_CONFIG_SERIAL
    (announced on D-Bus), GetAll and Schema;
  * Look: Get, LookChanged for a theme switch (no session: at once), the
    colors of a wallpaper (a PNG made here);
  * Apps: Report and Problems with the Problem signal, an unknown report
    refused, Running; single instances: a second start hands its arguments
    to the first one, which is replaced when it ends, and registers again
    when sde-core starts anew;
  * Session: Workspaces from the root window's EWMH properties,
    SetPhase, PhaseChanged and Info;
  * the client: writes through sde-core and their Changed, LookChanged only
    for the look, and without sde-core the file written directly (still
    checked by the schema) with the root window's serial followed instead.

SDE_TEST_DISPLAY=N picks the X display (otherwise the first free one)."""
import os
import re
import shutil
import struct
import subprocess as sp
import sys
import tempfile
import time
import zlib
from pathlib import Path

if len(sys.argv) < 3 or not all(shutil.which(t) for t in ('Xvfb', 'gdbus', 'xprop')) or \
        not os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
    print('Xvfb, gdbus, xprop and a session bus (dbus-run-session) are required; skipped')
    sys.exit(77)
CORE, PROBE = sys.argv[1], sys.argv[2]
NAME, PATH = 'org.selviana.Core', '/org/selviana/Core'
failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


def wait(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        value = fn()
        if value:
            return value
        time.sleep(0.05)
    return fn()


def png(path, rgb, w=16, h=12):
    """A small PNG of one color (the wallpaper)."""
    raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


class World:
    def __init__(self):
        self.temp = Path(tempfile.mkdtemp(prefix='sde-core-'))
        self.home = self.temp / 'home'
        (self.home / '.config/sde').mkdir(parents=True)
        self.conf = self.home / '.config/sde/sde.conf'
        # An old duplicate key: merged into [panel] clock_seconds at start.
        self.conf.write_text('[clock]\nseconds=true\n[panel]\nheight=40\n')
        run = self.temp / 'run'
        run.mkdir(mode=0o700)
        self.procs = []
        fixed = os.environ.get('SDE_TEST_DISPLAY', '').lstrip(':')
        if fixed:
            self.server = sp.Popen(['Xvfb', ':' + fixed, '-screen', '0', '800x600x24', '-nolisten', 'tcp'],
                                   stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            display = ':' + fixed
        else:
            read, write = os.pipe()
            self.server = sp.Popen(['Xvfb', '-displayfd', str(write), '-screen', '0', '800x600x24', '-nolisten',
                                    'tcp'], pass_fds=(write,), stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            os.close(write)
            number = b''
            while not number.endswith(b'\n'):
                chunk = os.read(read, 16)
                if not chunk:
                    break
                number += chunk
            os.close(read)
            display = ':' + number.decode().strip()
        self.procs.append(self.server)
        self.env = dict(os.environ, DISPLAY=display, HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / '.config'),
                        XDG_CONFIG_DIRS=str(self.temp / 'xdg'), XDG_CACHE_HOME=str(self.home / '.cache'),
                        XDG_DATA_HOME=str(self.home / '.local/share'), XDG_STATE_HOME=str(self.home / '.local/state'),
                        XDG_RUNTIME_DIR=str(run), GSETTINGS_BACKEND='memory', SDE_GRAPHICS='software',
                        LANG='C.UTF-8', LC_ALL='C.UTF-8')
        if not wait(lambda: sp.run(['xprop', '-root'], env=self.env, capture_output=True).returncode == 0, 10):
            raise RuntimeError('Xvfb did not start')
        self.core = None
        self.monitor_log = self.temp / 'monitor.log'
        self.monitor = sp.Popen(['gdbus', 'monitor', '--session', '--dest', NAME], env=self.env,
                                stdout=open(self.monitor_log, 'w'), stderr=sp.STDOUT)
        self.procs.append(self.monitor)

    def start_core(self):
        self.core = sp.Popen([CORE], env=self.env, stdout=open(self.temp / 'core.log', 'a'), stderr=sp.STDOUT)
        self.procs.append(self.core)
        if not wait(lambda: self.owner(), 10):
            raise RuntimeError('sde-core did not start: ' + (self.temp / 'core.log').read_text())

    def stop_core(self):
        self.core.terminate()
        self.core.wait()
        wait(lambda: not self.owner(), 5)

    def owner(self):
        r = sp.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus', '--object-path',
                    '/org/freedesktop/DBus', '--method', 'org.freedesktop.DBus.NameHasOwner', NAME],
                   env=self.env, capture_output=True, text=True)
        return '(true,)' in r.stdout

    def call(self, method, *args):
        """(output, error) of org.selviana.Core's |method| ("Settings.Get")."""
        iface = NAME if '.' not in method else NAME + '.' + method.rsplit('.', 1)[0]
        member = method.rsplit('.', 1)[-1]
        if method == 'Introspect':
            iface = 'org.freedesktop.DBus.Introspectable'
        r = sp.run(['gdbus', 'call', '--session', '--dest', NAME, '--object-path', PATH, '--method',
                    iface + '.' + member, *args], env=self.env, capture_output=True, text=True, timeout=20)
        return r.stdout.strip(), r.stderr.strip()

    def signals(self, member):
        try:
            text = self.monitor_log.read_text()
        except OSError:
            return []
        return [l for l in text.splitlines() if '.' + member + ' (' in l]

    def probe(self, *args, **kw):
        return sp.run([PROBE, *args], env=self.env, capture_output=True, text=True, timeout=30, **kw)

    def spawn_probe(self, name, *args):
        log = self.temp / (name + '.log')
        p = sp.Popen([PROBE, *args], env=self.env, stdout=open(log, 'w'), stderr=sp.STDOUT)
        self.procs.append(p)
        return p, log

    def bump_serial(self, value):
        sp.run(['xprop', '-root', '-f', '_SDE_CONFIG_SERIAL', '32c', '-set', '_SDE_CONFIG_SERIAL', str(value)],
               env=self.env, check=True)

    def close(self):
        for p in reversed(self.procs):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except sp.TimeoutExpired:
                    p.kill()
        shutil.rmtree(self.temp, ignore_errors=True)


def lines(path):
    try:
        return path.read_text().splitlines()
    except OSError:
        return []


def main():
    w = World()
    try:
        w.start_core()
        out, _ = w.call('Version')
        check(re.match(r"\('[^']+', uint32 1\)", out), 'Version: ' + out)
        out, _ = w.call('Capabilities')
        check("'settings'" in out and "'apps.instances'" in out and "'session.workspaces'" in out, 'Capabilities: ' + out)
        out, _ = w.call('Introspect')
        check('org.selviana.Core.Settings' in out and 'SetMany' in out, 'Introspect')
        text = w.conf.read_text()
        check('clock_seconds=true' in text and '\nseconds=' not in text, 'the old [clock] seconds was not merged: ' + text)

        # ---- Settings
        check(w.call('Settings.Get', 'panel', 'position')[0] == "(<'bottom'>,)", 'Get a default string')
        check(w.call('Settings.Get', 'aero', 'transparency')[0] == '(<25>,)', 'Get an integer')
        check(w.call('Settings.Get', 'panel', 'clock_seconds')[0] == '(<true>,)', 'Get a migrated boolean')
        check('UnknownKey' in w.call('Settings.Get', 'nothing', 'here')[1], 'Get of a key nobody set')
        check(w.call('Settings.Set', 'panel', 'position', "<'top'>")[0] == '()', 'Set')
        check('position=top' in w.conf.read_text(), 'Set did not write the file')
        check(wait(lambda: any("('panel', 'position')" in l for l in w.signals('Changed'))), 'no Changed for Set')
        before = w.conf.read_text()
        _, err = w.call('Settings.Set', 'panel', 'position', "<'left'>")
        check('InvalidValue' in err and 'bottom, top' in err, 'a value outside the schema: ' + err)
        _, err = w.call('Settings.Set', 'aero', 'transparency', '<300>')
        check('InvalidValue' in err, 'an integer outside its range: ' + err)
        check(w.conf.read_text() == before, 'a refused value changed the file')
        out, _ = w.call('Settings.SetMany', "[('panel', 'autohide', <true>), ('aero', 'transparency', <'60'>)]",
                        "[('panel', 'position')]", '{}')
        check("('aero', 'transparency')" in out and "('panel', 'autohide')" in out and "('panel', 'position')" in out,
              'SetMany: ' + out)
        text = w.conf.read_text()
        check('autohide=true' in text and 'transparency=60' in text and 'position=' not in text, 'SetMany wrote: ' + text)
        # A program of 0.2.0 Alpha 3 writes the file and says so on the root window.
        w.conf.write_text(w.conf.read_text() + '\n[desktop]\nshow_trash=true\n')
        w.bump_serial(1000)
        check(wait(lambda: any("('desktop', 'show_trash')" in l for l in w.signals('Changed'))),
              'a change announced on the root window did not reach D-Bus')
        out, _ = w.call('Settings.GetAll')
        check("'desktop': {" in out and "'show_trash': <true>" in out and "'height': <40>" in out, 'GetAll')
        out, _ = w.call('Settings.Schema')
        check("('panel', 'position', 'enum', 'bottom|top', true, 'bottom', 'panel')" in out, 'Schema: ' + out[:300])

        # ---- Look
        out, _ = w.call('Look.Get')
        check("'family': <'graphite'>" in out and "'dark': <false>" in out, 'Look.Get: ' + out[:300])
        looks = len(w.signals('LookChanged'))
        out, _ = w.call('Settings.SetMany', "[('appearance', 'theme', <'dark'>)]", '@a(ss) []', "{'appearance': <true>}")
        check("('appearance', 'theme')" in out, 'a theme switch: ' + out)
        check(wait(lambda: len(w.signals('LookChanged')) > looks), 'no LookChanged for a theme switch')
        check("'dark': <true>" in w.call('Look.Get')[0], 'the look did not turn dark')
        picture = w.temp / 'wall.png'
        png(picture, (0x33, 0x66, 0xCC))
        w.call('Settings.Set', 'appearance', 'wallpaper', "<'%s'>" % picture)
        def wallpaper_color():
            m = re.search(r"'wallpaper-colors': <\['#([0-9A-F]{6})'", w.call('Look.Get')[0])
            if not m:
                return False
            got = [int(m.group(1)[i:i + 2], 16) for i in (0, 2, 4)]
            # Scaling the picture may move a channel by a step or two.
            return all(abs(a - b) <= 3 for a, b in zip(got, (0x33, 0x66, 0xCC)))
        check(wait(wallpaper_color), 'the wallpaper colors: ' + re.sub(r'.*(.wallpaper)', r'\1', w.call('Look.Get')[0]))
        check("'wallpaper-dark': <true>" in w.call('Look.Get')[0], 'a dark blue wallpaper is not dark')

        # ---- Apps
        w.call('Apps.Report', 'crashed', 'uint32 4242', 'Niny', 'niny.desktop', '11')
        out, _ = w.call('Apps.Problems')
        check("uint32 4242, 'crashed', 'Niny', 'niny.desktop', '11'" in out, 'Problems: ' + out)
        check(wait(lambda: any('4242' in l for l in w.signals('Problem'))), 'no Problem signal')
        check('InvalidArgs' in w.call('Apps.Report', 'exploded', 'uint32 1', '', '', '')[1], 'an unknown report')
        w.call('Apps.Report', 'component-started', 'uint32 77', 'panel', '', '')
        check("(uint32 77, 'component', 'panel', 'panel', 'running')" in w.call('Apps.Running')[0], 'Running')

        first, first_log = w.spawn_probe('first', 'instance', 'org.selviana.Probe', '--page', 'one')
        check(wait(lambda: 'primary' in lines(first_log)), 'the first instance: %s' % lines(first_log))
        second = w.probe('instance', 'org.selviana.Probe', '--page', 'two')
        check(second.stdout.strip() == 'handed-over', 'the second start: ' + second.stdout + second.stderr)
        check(wait(lambda: 'activated --page two' in lines(first_log)), 'the first instance got: %s' % lines(first_log))
        first.terminate()
        first.wait()
        third, third_log = w.spawn_probe('third', 'instance', 'org.selviana.Probe')
        check(wait(lambda: 'primary' in lines(third_log)), 'a start after the instance ended: %s' % lines(third_log))

        # ---- Session
        sp.run(['xprop', '-root', '-f', '_NET_NUMBER_OF_DESKTOPS', '32c', '-set', '_NET_NUMBER_OF_DESKTOPS', '3'],
               env=w.env, check=True)
        sp.run(['xprop', '-root', '-f', '_NET_DESKTOP_NAMES', '8u', '-set', '_NET_DESKTOP_NAMES', 'Web'], env=w.env,
               check=True)
        check(wait(lambda: w.call('Session.Workspaces')[0] == "(uint32 3, ['Web', '', ''], uint32 0)"),
              'Workspaces: ' + w.call('Session.Workspaces')[0])
        check(wait(lambda: w.signals('WorkspacesChanged')), 'no WorkspacesChanged')
        w.call('Session.SetPhase', 'running')
        check(wait(lambda: w.signals('PhaseChanged')), 'no PhaseChanged')
        check("'phase': <'running'>" in w.call('Session.Info')[0], 'Info')
        check('InvalidArgs' in w.call('Session.SetPhase', 'dancing')[1], 'an unknown phase')

        # ---- The client, with sde-core
        check(w.probe('available').stdout.strip() == 'yes', 'the client does not see sde-core')
        watch, watch_log = w.spawn_probe('watch', 'watch', '20')
        check(wait(lambda: 'watching sde-core' in lines(watch_log)), 'the watch: %s' % lines(watch_log))
        w.probe('write', 'panel', 'show_tray', 'false')
        check(wait(lambda: 'settings [panel] show_tray' in lines(watch_log)), 'the client did not get Changed: %s' %
              lines(watch_log))
        check('show_tray=false' in w.conf.read_text(), 'the client did not write through sde-core')
        check('look' not in lines(watch_log), 'a panel setting announced a new look')
        w.probe('write', 'appearance', 'accent_name', 'green')
        check(wait(lambda: 'look' in lines(watch_log)), 'the client did not get LookChanged: %s' % lines(watch_log))
        refused = w.probe('write', 'panel', 'height', 'tall')
        check('height=40' in w.conf.read_text(), 'sde-core accepted a value its schema refuses')
        check('refused' in refused.stderr, 'the client did not say the value was refused: ' + refused.stderr)

        # sde-core starts again: the instance registers again.
        w.stop_core()
        w.start_core()
        check(wait(lambda: w.probe('instance', 'org.selviana.Probe', 'x').stdout.strip() == 'handed-over', 10),
              'the instance did not register again with a new sde-core')
        check(wait(lambda: 'activated x' in lines(third_log)), 'the instance got: %s' % lines(third_log))
        watch.terminate()
        watch.wait()

        # ---- The client without sde-core: the file and the root window.
        w.stop_core()
        check(w.probe('available').stdout.strip() == 'no', 'the client sees an sde-core that ended')
        watch, watch_log = w.spawn_probe('watch2', 'watch', '20')
        check(wait(lambda: 'watching the root window' in lines(watch_log)), 'the fallback watch: %s' % lines(watch_log))
        w.probe('write', 'panel', 'position', 'top')
        check('position=top' in w.conf.read_text(), 'the client did not write the file without sde-core')
        check(wait(lambda: 'settings *' in lines(watch_log) and 'look' in lines(watch_log)),
              'the serial was not followed without sde-core: %s' % lines(watch_log))
        bad = w.probe('write', 'panel', 'position', 'sideways')
        check('position=top' in w.conf.read_text() and 'not saving' in bad.stderr,
              'the schema was not used without sde-core: ' + bad.stderr)
        w.probe('write', 'panel', 'position', '-')
        check('position=' not in w.conf.read_text(), 'a reset without sde-core')
    finally:
        log = w.temp / 'core.log'
        if failures and log.exists():
            print(log.read_text())
        w.close()
    print('%d check(s) failed' % len(failures) if failures else 'sde-core: all checks passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
