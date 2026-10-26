#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""The environment startsde gives the session (data/startsde).

test_session_env.py [SOURCE_DIR]

startsde runs with stubs of the programs it calls (sde-session records the
environment it gets, dbus-update-activation-environment its arguments,
flatpak lists an extra installation). Checked: the Flatpak export
directories (the user's, the system's, other installations) come first in
XDG_DATA_DIRS, the snap launchers last, nothing is added twice (also with a
trailing slash), the default /usr/local/share:/usr/share is kept when
XDG_DATA_DIRS was unset, /snap/bin is in PATH once and XDG_DATA_DIRS is
handed to D-Bus activation and the systemd user manager."""
import shutil
import subprocess as sp
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
STARTSDE = ROOT / 'data/startsde'


def stub(path, text):
    path.write_text('#!/bin/sh\n' + text)
    path.chmod(0o755)


class StartsdeEnvironment(unittest.TestCase):
    def run_startsde(self, data_dirs=None, flatpak='', path_extra=''):
        tmp = Path(tempfile.mkdtemp(prefix='sde-session-env-'))
        self.addCleanup(shutil.rmtree, tmp, True)
        home, bin_dir = tmp / 'home', tmp / 'bin'
        home.mkdir()
        bin_dir.mkdir()
        out = tmp / 'env.txt'
        stub(bin_dir / 'sde-session', 'env > "%s"\n' % out)
        stub(bin_dir / 'dbus-update-activation-environment', 'echo "$@" > "%s"\n' % (tmp / 'dbus.txt'))
        for name in ('xdg-user-dirs-update', 'xrdb'):
            stub(bin_dir / name, 'exit 0\n')
        stub(bin_dir / 'loginctl', 'echo yes\n')
        stub(bin_dir / 'flatpak', 'test "$1" = --installations && printf "%s" || exit 1\n' % flatpak)
        env = {'HOME': str(home), 'PATH': '%s%s:/usr/bin:/bin' % (bin_dir, path_extra),
               'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent', 'XDG_SESSION_ID': '1'}
        if data_dirs is not None:
            env['XDG_DATA_DIRS'] = data_dirs
        sp.run(['sh', str(STARTSDE)], env=env, check=True, timeout=30)
        values = dict(line.split('=', 1) for line in out.read_text().splitlines() if '=' in line)
        return home, values, (tmp / 'dbus.txt').read_text()

    def test_unset(self):
        home, env, dbus = self.run_startsde(flatpak='/var/lib/flatpak\n/opt/extra-flatpak\n')
        self.assertEqual(env['XDG_DATA_DIRS'].split(':'), [
            str(home / '.local/share/flatpak/exports/share'), '/var/lib/flatpak/exports/share',
            '/opt/extra-flatpak/exports/share', '/usr/local/share', '/usr/share', '/var/lib/snapd/desktop'])
        self.assertEqual(env['PATH'].split(':').count('/snap/bin'), 1)
        self.assertIn('XDG_DATA_DIRS', dbus.split())

    def test_already_there(self):
        # What /etc/X11/Xsession.d/20flatpak produces, and snapd's profile.d.
        given = '/home/x/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share/:' \
                '/usr/share/gnome:/usr/local/share:/usr/share:/var/lib/snapd/desktop'
        home, env, _ = self.run_startsde(data_dirs=given, path_extra=':/snap/bin')
        dirs = env['XDG_DATA_DIRS'].split(':')
        self.assertEqual(dirs[0], str(home / '.local/share/flatpak/exports/share'))
        self.assertEqual(dirs[1:], given.split(':'))
        self.assertEqual(env['PATH'].split(':').count('/snap/bin'), 1)

    def test_without_flatpak_installations(self):
        home, env, _ = self.run_startsde(data_dirs='/usr/share')
        self.assertEqual(env['XDG_DATA_DIRS'].split(':'), [
            str(home / '.local/share/flatpak/exports/share'), '/var/lib/flatpak/exports/share', '/usr/share',
            '/var/lib/snapd/desktop'])


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
