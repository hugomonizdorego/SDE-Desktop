#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual installed DebION profile, resolver policy and MIME defaults."""
import configparser
import os
import subprocess
import tempfile
from pathlib import Path

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
assert Path('/usr/share/icons/Papirus/index.theme').is_file()

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
    # A personal browser choice must survive both the system policy and installs.
    (home/'config').mkdir(exist_ok=True)
    (home/'config/mimeapps.list').write_text('[Default Applications]\nx-scheme-handler/https=thunderbird.desktop;\n')
    assert subprocess.check_output(['xdg-mime','query','default','x-scheme-handler/https'],env=env,text=True).strip() == 'thunderbird.desktop'

# Negative APT pin and metapackage conflict must not block Celluloid's engine.
subprocess.run(['apt-get', '--simulate', 'install', 'celluloid', 'libmpv2'], check=True)
blocked = subprocess.run(['apt-get', '--simulate', 'install', 'mpv'], capture_output=True, text=True)
assert blocked.returncode != 0, (blocked.stdout, blocked.stderr)
print('Complete DebION profile, Firefox release, libmpv/blocked player, MIME defaults and personal override passed.')
