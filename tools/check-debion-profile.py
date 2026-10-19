#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual installed DebION profile, resolver policy and MIME defaults."""
import argparse
import configparser
import os
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--menu-probe', type=Path, default=Path('dist/test-tools/sde-menu-probe'))
args = parser.parse_args()

os.environ['LC_ALL'] = 'C.UTF-8'

required = '''sde sde-desktop debion-desktop-defaults gucharmap gnome-font-viewer
seahorse gnome-screenshot simple-scan thunderbird firefox gparted transmission-gtk
celluloid rhythmbox onboard gufw system-config-printer system-config-printer-udev
synaptic gdebi gnome-terminal pipewire pipewire-alsa pipewire-pulse wireplumber
libspa-0.2-bluetooth gamemode systemd-resolved systemd-timesyncd lightdm
lightdm-settings slick-greeter blueman bluez papirus-icon-theme network-manager
nm-connection-editor wpasupplicant wireless-regdb libpam-gnome-keyring libmpv2'''.split()
for name in required:
    state = subprocess.check_output(['dpkg-query', '-W', '-f=${db:Status-Abbrev}', name], text=True)
    assert state.startswith('ii'), (name, state)
for name in ['mpv', 'firefox-esr', 'pulseaudio']:
    state = subprocess.run(['dpkg-query', '-W', '-f=${db:Status-Abbrev}', name], capture_output=True, text=True)
    assert not state.stdout.startswith('ii'), (name, state.stdout)
policy = subprocess.check_output(['apt-cache', 'policy', 'mpv'], text=True)
assert 'Candidate: (none)' in policy, policy
firefox = subprocess.check_output(['apt-cache', 'policy', 'firefox'], text=True)
assert 'packages.mozilla.org' in firefox, firefox
version = subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'firefox'], text=True)
assert 'esr' not in version.lower() and 'snap' not in version.lower(), version
# The source and its key must both survive remove and both disappear on purge.
# Keep the key in dpkg-managed conffiles, alongside the APT source.
conffiles = subprocess.check_output(['dpkg-query', '-W', '-f=${Conffiles}', 'debion-desktop-defaults'], text=True)
for name in ['/etc/apt/keyrings/debion-mozilla-archive-keyring.asc',
             '/etc/apt/sources.list.d/debion-mozilla.sources']:
    assert name in conffiles and Path(name).is_file(), (name, conffiles)
assert 'Signed-By: /etc/apt/keyrings/debion-mozilla-archive-keyring.asc' in Path('/etc/apt/sources.list.d/debion-mozilla.sources').read_text()
assert Path('/usr/share/icons/Papirus/index.theme').is_file()
terminal = configparser.ConfigParser(interpolation=None)
terminal.read('/usr/share/applications/sde-gnome-terminal.desktop')
assert terminal['Desktop Entry']['OnlyShowIn'] == 'SDE;'
assert terminal['Desktop Entry']['Exec'] == 'gnome-terminal'
assert Path('/etc/xdg/sde-xdg-terminals.list').read_text().strip() == 'sde-gnome-terminal.desktop'

expected = {
    'text/plain': 'niny.desktop', 'inode/directory': 'liria.desktop',
    'application/pdf': 'xanara.desktop', 'application/zip': 'taca.desktop',
    'image/png': 'sde-view.desktop',
    'text/html': 'firefox.desktop', 'x-scheme-handler/https': 'firefox.desktop',
    'x-scheme-handler/mailto': 'thunderbird.desktop',
    'video/mp4': 'io.github.celluloid_player.Celluloid.desktop',
    'audio/mpeg': 'org.gnome.Rhythmbox3.desktop',
    'x-scheme-handler/magnet': 'transmission-gtk.desktop',
    'application/vnd.debian.binary-package': 'gdebi.desktop',
    'font/ttf': 'org.gnome.font-viewer.desktop',
    'application/pgp-keys': 'org.gnome.seahorse.Application.desktop',
}
with tempfile.TemporaryDirectory(prefix='debion-defaults-') as temporary:
    home = Path(temporary)
    env = {**os.environ, 'HOME': temporary, 'XDG_CONFIG_HOME': str(home/'config'),
           'XDG_DATA_HOME': str(home/'data'), 'XDG_CONFIG_DIRS': '/etc/xdg',
           'XDG_DATA_DIRS': '/usr/share', 'XDG_CURRENT_DESKTOP': 'SDE', 'LANG': 'C.UTF-8'}
    for mime, desktop in expected.items():
        entry = Path('/usr/share/applications', desktop)
        assert entry.is_file(), entry
        parser = configparser.ConfigParser(interpolation=None, strict=False)
        parser.read(entry)
        assert parser['Desktop Entry'].get('Categories'), (desktop, 'menu categories missing')
        actual = subprocess.check_output(['xdg-mime', 'query', 'default', mime], env=env, text=True).strip()
        assert actual == desktop, (mime, actual, desktop)
    terminal = subprocess.check_output(['xdg-terminal-exec', '--print-id'], env=env, text=True).strip()
    assert terminal == 'sde-gnome-terminal.desktop', terminal
    # Exercise exact argument forwarding without opening a window or a shell.
    command = subprocess.check_output(['xdg-terminal-exec', '--print-cmd', '--dir=/tmp/a b', '--', 'printf', '%s', 'a b'], env=env, text=True).splitlines()
    assert command[-4:] == ['--', 'printf', '%s', 'a b'], command
    assert any('/tmp/a b' in arg for arg in command), command
    menu = dict(line.split('\t') for line in subprocess.check_output([str(args.menu_probe.resolve())], env=env, text=True).splitlines())
    categories = {
        'niny.desktop': 'Accessories', 'liria.desktop': 'System',
        'xanara.desktop': 'Office', 'taca.desktop': 'Accessories',
        'gallery.desktop': 'Graphics', 'sde-view.desktop': 'Graphics',
        'org.debion.Calculator.desktop': 'Accessories',
        'sde-system-monitor.desktop': 'System', 'sde-update.desktop': 'Settings',
        'gucharmap.desktop': 'Accessories', 'org.gnome.font-viewer.desktop': 'Accessories',
        'org.gnome.seahorse.Application.desktop': 'Accessories',
        'org.gnome.Screenshot.desktop': 'Accessories',
        'org.gnome.SimpleScan.desktop': 'Graphics', 'thunderbird.desktop': 'Internet',
        'firefox.desktop': 'Internet', 'gparted.desktop': 'System',
        'transmission-gtk.desktop': 'Internet',
        'io.github.celluloid_player.Celluloid.desktop': 'Multimedia',
        'org.gnome.Rhythmbox3.desktop': 'Multimedia', 'onboard.desktop': 'Accessories',
        'gufw.desktop': 'Settings', 'system-config-printer.desktop': 'Settings',
        'synaptic.desktop': 'Settings', 'gdebi.desktop': 'System',
        'sde-gnome-terminal.desktop': 'System', 'lightdm-settings.desktop': 'Settings',
    }
    for desktop, category in categories.items():
        assert menu.get(desktop) == category, (desktop, category, menu.get(desktop))
    assert 'org.gnome.Terminal.desktop' not in menu, 'duplicate terminal launcher'
    # A personal browser choice must survive both the system policy and installs.
    (home/'config').mkdir(exist_ok=True)
    (home/'config/mimeapps.list').write_text('[Default Applications]\nx-scheme-handler/https=thunderbird.desktop;\n')
    assert subprocess.check_output(['xdg-mime','query','default','x-scheme-handler/https'],env=env,text=True).strip() == 'thunderbird.desktop'
    (home/'data/applications').mkdir(parents=True)
    (home/'data/applications/user-terminal.desktop').write_text('[Desktop Entry]\nType=Application\nName=User Terminal\nExec=/usr/bin/true\nCategories=System;TerminalEmulator;\nX-TerminalArgExec=--\n')
    (home/'config/xdg-terminals.list').write_text('user-terminal.desktop\n')
    terminal = subprocess.check_output(['xdg-terminal-exec', '--print-id'], env=env, text=True).strip()
    assert terminal == 'user-terminal.desktop', terminal

# Negative APT pin and metapackage conflict must not block Celluloid's engine.
subprocess.run(['apt-get', '--simulate', 'install', 'celluloid', 'libmpv2'], check=True)
blocked = subprocess.run(['apt-get', '--simulate', 'install', 'mpv'], capture_output=True, text=True)
assert blocked.returncode != 0, (blocked.stdout, blocked.stderr)
print('Complete DebION profile, Firefox release, libmpv/blocked player, MIME defaults and personal override passed.')
