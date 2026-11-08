#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Liria on D-Bus, on a private session bus and a private Xvfb:

  test_liria_dbus.py BUILD/src/liria/liria SOURCE/data/dbus

The bus is a dbus-daemon of the test with its own service folder, where
the test puts Liria's D-Bus activation files (data/dbus/*.service.in, made
to start the built program). Then:

- org.freedesktop.FileManager1.ShowFolders starts Liria through D-Bus
  activation (as a browser's "Show in folder" does) and opens the folder;
- ShowItems opens the folder of a file with the file selected;
- ShowItemProperties opens the Properties of a file;
- org.debion.Liria.Open, and the command line of a second "liria" that
  hands its locations to the running one and ends (one Liria per session);
- "liria --quit" ends the running Liria and frees both names.

Exit status 0 passed, 1 failed, 77 skipped (no Xvfb, xdotool, gdbus or
dbus-daemon). SDE_TEST_DISPLAY=N picks the X display."""
import os
import shutil
import signal
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

if not shutil.which('gdbus') or not shutil.which('dbus-daemon'):
    print('gdbus and dbus-daemon are required; skipped')
    sys.exit(77)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_liria_ui as ui  # noqa: E402  (the Xvfb session; exits 77 without Xvfb or xdotool)

LIRIA = os.path.abspath(sys.argv[1])
SERVICES = Path(sys.argv[2])
check = ui.check
started = []  # Liria processes the test's bus started

BUS_CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path={socket}</listen>
  <servicedir>{services}</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


class Bus:
    def __init__(self, temp, env):
        self.services = temp / 'services'
        self.services.mkdir()
        for template in sorted(SERVICES.glob('*.service.in')):
            text = template.read_text().replace('@CMAKE_INSTALL_FULL_BINDIR@/liria', LIRIA)
            name = [ln.split('=', 1)[1] for ln in text.splitlines() if ln.startswith('Name=')][0]
            (self.services / (name + '.service')).write_text(text)
        socket = temp / 'bus'
        conf = temp / 'bus.conf'
        conf.write_text(BUS_CONFIG.format(socket=socket, services=self.services))
        self.address = 'unix:path=%s' % socket
        # Services the bus starts get its environment: the test's display and home.
        self.env = dict(env, DBUS_SESSION_BUS_ADDRESS=self.address)
        self.proc = sp.Popen(['dbus-daemon', '--config-file', str(conf), '--nofork', '--nopidfile'], env=self.env,
                             stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        end = time.time() + 10
        while not socket.exists() and time.time() < end:
            time.sleep(0.05)

    def call(self, dest, path, method, *args, timeout=30):
        r = sp.run(['gdbus', 'call', '--session', '--dest', dest, '--object-path', path, '--method', method] +
                   list(args), env=self.env, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()

    def has_owner(self, name):
        code, out = self.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus.NameHasOwner',
                              name)
        return code == 0 and 'true' in out

    def owner_pid(self, name):
        code, out = self.call('org.freedesktop.DBus', '/org/freedesktop/DBus',
                              'org.freedesktop.DBus.GetConnectionUnixProcessID', name)
        digits = ''.join(c for c in out.split(',')[0] if c.isdigit())
        return int(digits) if code == 0 and digits else None

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except sp.TimeoutExpired:
            self.proc.kill()


def uris(*paths):
    return '[' + ', '.join("'file://%s'" % p for p in paths) + ']'


def liria_windows(s):
    return s.xdo('search', '--onlyvisible', '--class', 'liria').stdout.split()


def run(s, bus):
    home, docs = s.home, s.home / 'Documents'
    fm, fm_path = 'org.freedesktop.FileManager1', '/org/freedesktop/FileManager1'
    s.env = bus.env  # the Liria started below finds the test's bus
    check(not bus.has_owner(fm), 'nobody owns FileManager1 on the new bus')

    # ShowFolders starts Liria through D-Bus activation.
    code, out = bus.call(fm, fm_path, fm + '.ShowFolders', uris(docs), '')
    check(code == 0, f'ShowFolders answers: {out}')
    check(s.wait(lambda: s.state('location') == str(docs) and s.state('loading') == '0', 20),
          f'ShowFolders opens the folder: {s.state("location")}')
    check(bus.has_owner(fm) and bus.has_owner('org.debion.Liria'), 'the activated Liria owns both names')
    pid = bus.owner_pid('org.debion.Liria')
    if pid:
        started.append(pid)
    check(pid is not None and pid == bus.owner_pid(fm), 'one process has both names')
    code, out = bus.call(fm, fm_path, 'org.freedesktop.DBus.Introspectable.Introspect')
    check(all(m in out for m in ('ShowFolders', 'ShowItems', 'ShowItemProperties')), 'the interface is described')

    # ShowItems: the file's folder, the file selected.
    code, out = bus.call(fm, fm_path, fm + '.ShowItems', uris(docs / 'Notes.txt'), '')
    check(code == 0, f'ShowItems answers: {out}')
    check(s.wait(lambda: s.state('selected') == 'Notes.txt' and s.state('location') == str(docs), 10),
          f'ShowItems selects the file: {s.state("location")} {s.state("selected")}')
    check(s.wait(lambda: len(liria_windows(s)) == 2), f'a window each: {len(liria_windows(s))}')

    # ShowItemProperties.
    code, out = bus.call(fm, fm_path, fm + '.ShowItemProperties', uris(home / 'Pictures/lake.png'), '')
    check(code == 0, f'ShowItemProperties answers: {out}')
    props = None
    if check(s.wait(lambda: s.window_named('lake.png Properties'), 10), 'ShowItemProperties opens Properties'):
        props = s.window_named('lake.png Properties')
        s.xdo('windowactivate', '--sync', props)
        s.key('Escape')
        check(s.wait(lambda: not s.window_named('lake.png Properties')), 'Escape closes Properties')

    # A second liria hands its locations to the running one and ends.
    downloads = home / 'Downloads'
    t0 = time.time()
    r = sp.run([LIRIA, str(downloads)], env=bus.env, capture_output=True, text=True, timeout=20)
    print('hand-off in %.0f ms' % ((time.time() - t0) * 1000), flush=True)
    check(r.returncode == 0, f'the second liria ends with 0: {r.returncode} {r.stderr}')
    check(s.wait(lambda: s.state('location') == str(downloads), 10), 'the running Liria opens its location')
    check(bus.owner_pid('org.debion.Liria') == pid, 'the first Liria still owns the name')
    r = sp.run([LIRIA, '--select', str(docs / 'Report.pdf')], env=bus.env, capture_output=True, timeout=20)
    check(r.returncode == 0 and s.wait(lambda: s.state('selected') == 'Report.pdf', 10),
          f'liria --select shows the file selected: {s.state("selected")}')
    code, out = bus.call('org.debion.Liria', '/org/debion/Liria', 'org.debion.Liria.Open', uris(home / 'Music'), '[]')
    check(code == 0 and s.wait(lambda: s.state('location') == str(home / 'Music'), 10),
          f'org.debion.Liria.Open: {out} {s.state("location")}')

    # liria --quit ends it; both names are free again.
    r = sp.run([LIRIA, '--quit'], env=bus.env, capture_output=True, timeout=20)
    check(r.returncode == 0, f'liria --quit ends with 0: {r.returncode}')
    check(s.wait(lambda: not bus.has_owner('org.debion.Liria') and not bus.has_owner(fm), 15),
          'the names are free after --quit')
    check(s.wait(lambda: not liria_windows(s), 10), 'every window is closed')
    r = sp.run([LIRIA, '--quit'], env=bus.env, capture_output=True, timeout=20)
    check(r.returncode == 1, f'--quit without a running Liria ends with 1: {r.returncode}')
    if pid and s.wait(lambda: not os.path.exists('/proc/%d' % pid), 10) is False:
        check(False, 'the activated Liria ended')


def main():
    with tempfile.TemporaryDirectory(prefix='liria-dbus-') as temp:
        s = ui.Session(temp)
        bus = Bus(Path(temp), {k: v for k, v in s.env.items() if k != 'DBUS_SESSION_BUS_ADDRESS'})
        try:
            run(s, bus)
        except Exception as e:
            check(False, f'{type(e).__name__}: {e}')
        finally:
            for pid in started:
                if os.path.exists('/proc/%d' % pid):
                    os.kill(pid, signal.SIGTERM)
            bus.close()
            s.close()
    if ui.failures:
        print(f'{len(ui.failures)} check(s) failed')
        return 1
    print('liria-dbus: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
