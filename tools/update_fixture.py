# SPDX-License-Identifier: GPL-3.0-or-later
"""Fake APT data for the Software Updater (sde-update) in QA.

write(fixtures, bin_dir, root) creates:
  fixtures/apt.py, fixtures/apt_pkg.py   python-apt stand-ins (PYTHONPATH)
  bin_dir                                (created; the updater needs no
                                         fake programs since Flatpak apps
                                         are updated in Merkadu)
  root/var/lib/dpkg/info, root/usr/share/applications
                                         desktop files (SDE_UPDATE_ROOT)
so that all three groups appear: Critical (a signed kernel and glibc),
Security (OpenSSL, Firefox ESR, example-security) and Main (LibreOffice
Writer, VLC, example).
"""
from pathlib import Path

APT = """from types import SimpleNamespace as N
def pkg(name, source, security=False, priority='optional', size=4096,
        desc='UI test fixture', section='utils', old='1.0', new='2.0'):
    c = N(source_name=source, source_version=new, version=new, size=size,
          priority=priority, raw_description=desc, description=desc + '.',
          section=section,
          origins=[N(origin='Debian', site='fixture.invalid',
                     archive='testing-security' if security else 'testing',
                     label='Debian-Security' if security else 'Debian',
                     component='main')])
    return N(name=name, candidate=c, installed=N(version=old), is_installed=True,
             is_upgradable=True, _pkg=N(selected_state=0))
def Cache():
    return [
        pkg('linux-image-amd64', 'linux-signed-amd64', True, size=1460,
            desc='Linux for 64-bit PCs (meta-package)', section='kernel',
            old='6.12.8-1', new='6.12.9-1'),
        pkg('linux-image-6.12.9-amd64', 'linux-signed-amd64', True, size=98300000,
            desc='Linux 6.12 for 64-bit PCs (signed)', section='kernel',
            old='', new='6.12.9-1'),
        pkg('libc6', 'glibc', priority='required', size=2900000,
            desc='GNU C Library: Shared libraries', section='libs',
            old='2.41-4', new='2.41-6'),
        pkg('libc-bin', 'glibc', priority='required', size=650000,
            desc='GNU C Library: Binaries', section='libs', old='2.41-4', new='2.41-6'),
        pkg('example-security', 'example-security', True,
            desc='Example security fixture'),
        pkg('openssl', 'openssl', True, size=1400000,
            desc='Secure Sockets Layer toolkit - cryptographic utility',
            old='3.5.1-1', new='3.5.1-2'),
        pkg('libssl3t64', 'openssl', True, size=2300000,
            desc='Secure Sockets Layer toolkit - shared libraries',
            old='3.5.1-1', new='3.5.1-2'),
        pkg('firefox-esr', 'firefox-esr', True, size=70500000,
            desc='Mozilla Firefox web browser - Extended Support Release (ESR)',
            section='web', old='140.3.0esr-1', new='140.4.0esr-1'),
        pkg('example-common', 'example'), pkg('example', 'example'),
        pkg('libreoffice-writer', 'libreoffice', size=9800000,
            desc='office productivity suite -- word processor', section='editors',
            old='4:25.2.5-1', new='4:25.2.6-1'),
        pkg('vlc', 'vlc', size=420000, desc='multimedia player and streamer',
            section='video', old='3.0.21-8', new='3.0.21-9'),
    ]
"""

DESKTOP = {
    # package: (desktop file, Name, Icon)
    "firefox-esr": ("firefox-esr.desktop", "Firefox ESR", "firefox-esr"),
    "libreoffice-writer": ("libreoffice-writer.desktop", "LibreOffice Writer", "x-office-document"),
    "vlc": ("vlc.desktop", "VLC media player", "vlc"),
}


def write(fixtures, bin_dir, root):
    fixtures, bin_dir, root = Path(fixtures), Path(bin_dir), Path(root)
    fixtures.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(parents=True, exist_ok=True)
    (fixtures / "apt_pkg.py").write_text("SELSTATE_HOLD = 1\n")
    (fixtures / "apt.py").write_text(APT)
    info = root / "var/lib/dpkg/info"
    apps = root / "usr/share/applications"
    info.mkdir(parents=True, exist_ok=True)
    apps.mkdir(parents=True, exist_ok=True)
    for package, (desktop, name, icon) in DESKTOP.items():
        (info / (package + ".list")).write_text(
            "/usr/bin/" + package + "\n/usr/share/applications/" + desktop + "\n"
        )
        (apps / desktop).write_text(
            "[Desktop Entry]\nType=Application\nName=" + name + "\nIcon=" + icon + "\nExec=" + package + "\n"
        )
