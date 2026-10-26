# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Debian packages and a fake package system for Debrun in QA.

build_deb(path, control, files, scripts) writes a real .deb (an ar archive
with control.tar.xz and data.tar.xz, as dpkg-deb makes them) without
dpkg-deb. write(dir, scenario) prepares a whole scenario in |dir|:

  dir/packages/*.deb   waveform-studio_3.2.0-1_amd64.deb (a vendor package
                       with an icon, a launcher, an APT source written by its
                       postinst, a signing key and a scheduled task),
                       zq-tool_1.0_arm64.deb (another architecture) and
                       broken.deb (not a package)
  dir/bin/apt-get      prints dir/apt/<install|remove>.out (APT's simulation)
  dir/bin/apt-cache    prints dir/apt/policy.out
  dir/bin/debrun-helper  a stand-in for the root helper (helper.h's output;
                       DEBRUN_FAKE_RESULT=lock|unmet fails, DEBRUN_FAKE_HOLD
                       keeps it working that many seconds); its arguments
                       go to dir/helper-args
  dir/status           dpkg's status file

and returns the environment for Debrun (DEBRUN_APT_GET, DEBRUN_APT_CACHE,
DEBRUN_DPKG_STATUS, DEBRUN_ARCHITECTURES, DEBRUN_TEST_HELPER ...).
Scenarios: "new" (installs with two more packages), "upgrade" (an older
version is installed; removing it takes waveform-plugins along), "unmet"
(a dependency is missing).
"""
import io
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ICON = ROOT / 'tests/fixtures/merkadu/icons/waveform.svg'


def _ar_member(name, data):
    header = '%-16s%-12d%-6s%-6s%-8s%-10d`\n' % (name, int(time.time()), '0', '0', '100644', len(data))
    return header.encode() + data + (b'\n' if len(data) % 2 else b'')


def _tar(entries):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:xz', format=tarfile.GNU_FORMAT) as tar:
        for name, data, mode in entries:
            info = tarfile.TarInfo('./' + name)
            info.mtime = int(time.time())
            info.uname = info.gname = 'root'
            if data is None:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
            else:
                info.size = len(data)
                info.mode = mode
                tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def build_deb(path, control, files, scripts=None):
    """|files|: [(path, bytes or None for a directory, mode)];
    |scripts|: {name: text}."""
    ctl = [('control', control.encode(), 0o644)]
    for name, text in (scripts or {}).items():
        ctl.append((name, text.encode(), 0o755 if name in ('preinst', 'postinst', 'prerm', 'postrm', 'config')
                    else 0o644))
    data = []
    dirs = set()
    for name, content, mode in files:
        parts = name.split('/')[:-1]
        for i in range(1, len(parts) + 1):
            d = '/'.join(parts[:i])
            if d not in dirs:
                dirs.add(d)
                data.append((d, None, 0o755))
        data.append((name, content, mode))
    blob = (b'!<arch>\n' + _ar_member('debian-binary', b'2.0\n') + _ar_member('control.tar.xz', _tar(ctl)) +
            _ar_member('data.tar.xz', _tar(data)))
    Path(path).write_bytes(blob)
    return path


WAVEFORM_CONTROL = """Package: waveform-studio
Version: 3.2.0-1
Architecture: amd64
Maintainer: Waveform Labs <packages@waveform.example>
Installed-Size: 61540
Depends: libc6 (>= 2.34), libgtk-3-0 (>= 3.24) | libgtk-4-1, libwaveform3 (>= 3.2), libasound2t64 | libasound2
Recommends: waveform-plugins
Section: sound
Priority: optional
Homepage: https://waveform.example/studio
Description: Record, edit and mix audio
 Waveform Studio records, edits and mixes audio with unlimited tracks,
 effects and a mixer.
 .
 This package was built by Waveform Labs for Debian and Ubuntu.
"""

WAVEFORM_POSTINST = """#!/bin/sh
set -e
# Add the Waveform Labs repository for updates.
cat > /etc/apt/sources.list.d/waveform-studio.sources <<EOF
Types: deb
URIs: https://packages.waveform.example/debian
Suites: stable
Components: main
Signed-By: /usr/share/keyrings/waveform-labs.gpg
EOF
update-alternatives --install /usr/bin/x-audio-editor x-audio-editor /opt/waveform-studio/waveform 50
"""


def waveform_files():
    icon = ICON.read_bytes() if ICON.exists() else b'<svg xmlns="http://www.w3.org/2000/svg"/>'
    return [
        ('opt/waveform-studio/waveform', b'\x7fELF' + b'\0' * 2000, 0o755),
        ('opt/waveform-studio/resources/app.pak', b'P' * 5000, 0o644),
        ('usr/bin/waveform-studio', b'#!/bin/sh\nexec /opt/waveform-studio/waveform "$@"\n', 0o755),
        ('usr/share/applications/waveform-studio.desktop',
         b'[Desktop Entry]\nType=Application\nName=Waveform Studio\nComment=Record, edit and mix audio\n'
         b'Exec=waveform-studio %F\nIcon=waveform-studio\nCategories=AudioVideo;Audio;\n', 0o644),
        ('usr/share/icons/hicolor/scalable/apps/waveform-studio.svg', icon, 0o644),
        ('usr/share/keyrings/waveform-labs.gpg', b'\x99\x01\x0dkey', 0o644),
        ('etc/cron.daily/waveform-studio', b'#!/bin/sh\n# keeps the repository configured\n', 0o755),
        ('usr/share/doc/waveform-studio/copyright', b'Copyright Waveform Labs\n', 0o644),
    ]


STATUS = """Package: libc6
Status: install ok installed
Architecture: amd64
Multi-Arch: same
Version: 2.40-3

Package: libgtk-3-0t64
Status: install ok installed
Architecture: amd64
Version: 3.24.43-3
Provides: libgtk-3-0 (= 3.24.43-3)

Package: libasound2t64
Status: install ok installed
Architecture: amd64
Version: 1.2.12-1
Provides: libasound2 (= 1.2.12-1)

Package: dpkg
Status: install ok installed
Architecture: amd64
Essential: yes
Version: 1.22.11
"""

INSTALLED_WAVEFORM = """
Package: waveform-studio
Status: install ok installed
Architecture: amd64
Version: 3.1.4-2

Package: waveform-plugins
Status: install ok installed
Architecture: amd64
Version: 3.1.4-2
"""

APT_HEADER = "Reading package lists...\nBuilding dependency tree...\nReading state information...\n"

APT_NEW = APT_HEADER + """The following additional packages will be installed:
  libwaveform3 waveform-plugins
The following NEW packages will be installed:
  libwaveform3 waveform-plugins waveform-studio
Inst libwaveform3 (3.2.1-1 Debian:testing [amd64])
Inst waveform-plugins (3.2.0-1 Debian:testing [amd64])
Inst waveform-studio (3.2.0-1 local-deb [amd64])
Conf libwaveform3 (3.2.1-1 Debian:testing [amd64])
Conf waveform-plugins (3.2.0-1 Debian:testing [amd64])
Conf waveform-studio (3.2.0-1 local-deb [amd64])
"""

APT_UPGRADE = APT_HEADER + """Inst libwaveform3 (3.2.1-1 Debian:testing [amd64])
Inst waveform-studio [3.1.4-2] (3.2.0-1 local-deb [amd64])
Conf libwaveform3 (3.2.1-1 Debian:testing [amd64])
Conf waveform-studio (3.2.0-1 local-deb [amd64])
"""

APT_REMOVE = APT_HEADER + """The following packages will be REMOVED:
  waveform-plugins waveform-studio
Remv waveform-plugins [3.1.4-2]
Remv waveform-studio [3.1.4-2]
"""

APT_UNMET = APT_HEADER + """Some packages could not be installed. This may mean that you have
requested an impossible situation or if you are using the unstable
distribution that some required packages have not yet been created
or been moved out of Incoming.
The following information may help to resolve the situation:

The following packages have unmet dependencies:
 waveform-studio : Depends: libwaveform3 (>= 3.2) but it is not installable
E: Unable to correct problems, you have held broken packages.
"""

POLICY_NONE = ""
POLICY_REPO = """waveform-studio:
  Installed: (none)
  Candidate: 3.0.1-1
  Version table:
     3.0.1-1 500
        500 http://deb.debian.org/debian testing/main amd64 Packages
"""

HELPER = r"""#!/bin/sh
# A stand-in for debrun-helper (src/debrun/helper.h) for the UI checks.
printf '%s\n' "$@" > "$DEBRUN_FAKE_DIR/helper-args"
echo "DEBRUN-HELPER 1"
hold=${DEBRUN_FAKE_HOLD:-0.4}
case "$1" in
install)
  echo "PHASE copy"; sleep 0.2
  echo "PHASE check"; sleep 0.2
  if [ "$DEBRUN_FAKE_RESULT" = lock ]; then
    echo "PHASE wait"; sleep 0.3
    echo "LOG E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 4242 (unattended-upgr)"
    echo "ERROR lock Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 4242 (unattended-upgr)"
    echo "RESULT failed"; exit 1
  fi
  echo "PROGRESS 12 download"; sleep 0.2
  echo "PROGRESS 38 download"; sleep 0.2
  echo "LOG Selecting previously unselected package libwaveform3:amd64."
  echo "PROGRESS 56 unpack libwaveform3"; sleep "$hold"
  echo "PROGRESS 73 unpack waveform-studio"; sleep 0.2
  echo "PROGRESS 91 configure waveform-studio"; sleep 0.2
  echo "PROGRESS 100 configure"
  echo "RESULT ok"; exit 0 ;;
remove)
  echo "PHASE remove"; sleep 0.2
  echo "PROGRESS 40 remove waveform-plugins"; sleep "$hold"
  echo "PROGRESS 80 remove waveform-studio"; sleep 0.2
  echo "RESULT ok"; exit 0 ;;
refresh)
  echo "PHASE refresh"; sleep 0.2
  echo "PROGRESS 50 download"; sleep "$hold"
  echo "RESULT ok"; exit 0 ;;
esac
echo "ERROR usage unknown action"
echo "RESULT refused"
exit 2
"""

APT_GET = r"""#!/bin/sh
# A stand-in for apt-get --simulate: prints the scenario's plan.
mode=install
for a in "$@"; do
  case "$a" in install|remove) mode=$a ;; esac
done
cat "$DEBRUN_FAKE_DIR/apt/$mode.out" 2>/dev/null
exit "$(cat "$DEBRUN_FAKE_DIR/apt/$mode.status" 2>/dev/null || echo 0)"
"""

APT_CACHE = r"""#!/bin/sh
cat "$DEBRUN_FAKE_DIR/apt/policy.out" 2>/dev/null
exit 0
"""


def write(directory, scenario='new'):
    d = Path(directory)
    (d / 'packages').mkdir(parents=True, exist_ok=True)
    (d / 'bin').mkdir(exist_ok=True)
    (d / 'apt').mkdir(exist_ok=True)
    build_deb(d / 'packages/waveform-studio_3.2.0-1_amd64.deb', WAVEFORM_CONTROL, waveform_files(),
              {'postinst': WAVEFORM_POSTINST, 'prerm': '#!/bin/sh\nset -e\n',
               'md5sums': '0123456789abcdef  opt/waveform-studio/waveform\n'})
    build_deb(d / 'packages/zq-tool_1.0_arm64.deb',
              'Package: zq-tool\nVersion: 1.0\nArchitecture: arm64\nMaintainer: Example <x@example.org>\n'
              'Description: A tool for ARM computers\n',
              [('usr/bin/zq-tool', b'\x7fELF', 0o755)])
    (d / 'packages/broken.deb').write_bytes(b'<html>404 Not Found</html>\n')
    for name, text in (('debrun-helper', HELPER), ('apt-get', APT_GET), ('apt-cache', APT_CACHE)):
        (d / 'bin' / name).write_text(text)
        (d / 'bin' / name).chmod(0o755)
    status = STATUS + (INSTALLED_WAVEFORM if scenario == 'upgrade' else '')
    (d / 'status').write_text(status)
    plan = {'new': (APT_NEW, 0), 'upgrade': (APT_UPGRADE, 0), 'unmet': (APT_UNMET, 100)}[scenario]
    (d / 'apt/install.out').write_text(plan[0])
    (d / 'apt/install.status').write_text(str(plan[1]))
    (d / 'apt/remove.out').write_text(APT_REMOVE)
    (d / 'apt/remove.status').write_text('0')
    (d / 'apt/policy.out').write_text(POLICY_REPO if scenario == 'new' else POLICY_NONE)
    return {
        'DEBRUN_FAKE_DIR': str(d),
        'DEBRUN_APT_GET': str(d / 'bin/apt-get'),
        'DEBRUN_APT_CACHE': str(d / 'bin/apt-cache'),
        'DEBRUN_DPKG_STATUS': str(d / 'status'),
        'DEBRUN_ARCHITECTURES': 'amd64 i386',
        'DEBRUN_TEST_HELPER': str(d / 'bin/debrun-helper'),
        'DEBRUN_BACKEND': 'apt',
    }
