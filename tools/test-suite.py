#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove the installed SDE apps are native fork ELFs, not upstream launchers."""
import argparse, os, subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bin-dir',type=Path,default=Path('/usr/bin'))
args=parser.parse_args()
binary=args.bin_dir.resolve()
apps=['niny','liria','xanara','taca','sde-view','sde-system-monitor',
      'sde-power-manager','sde-power-preferences','sde-power-statistics',
      'sde-screensaver','sde-screensaver-command','sde-screensaver-preferences','calculator','sde-display-settings','sde-wm','sde-compositor']
helper=(binary/'sde-suite').read_bytes()
NATIVE={'sde-system-monitor','xanara','taca','sde-view','niny'}
for name in apps:
    path=binary/name
    assert path.is_file() and not path.is_symlink(),name
    data=path.read_bytes()
    assert data[:4]==b'\x7fELF' and data!=helper,name
    dynamic=subprocess.check_output(['readelf','-d',str(path)],text=True)
    if name in NATIVE:
        # Native SDE applications since alpha15 (statically linked SDE-FLTK).
        assert 'libX11' in dynamic and 'libfltk' not in dynamic and 'libgtk' not in dynamic,(name,dynamic)
    elif name == 'sde-display-settings':
        # The SDE toolkit (FLTK) is linked statically since alpha13.
        assert 'libXrandr' in dynamic and 'libfltk' not in dynamic and 'libXft' in dynamic,(name,dynamic)
    elif name in ['sde-wm','sde-compositor']:
        assert 'libX11' in dynamic or 'libxcb' in dynamic,(name,dynamic)
    elif name == 'sde-screensaver-command':
        # This native command-line client talks to the locker over D-Bus;
        # it intentionally has no GTK window or direct GObject dependency.
        assert 'libglib-2.0' in dynamic and 'libdbus' in dynamic,(name,dynamic)
    else:
        assert 'libgtk' in dynamic or 'libgobject' in dynamic,(name,dynamic)
subprocess.run([str(binary/'sde-suite'),'--check'],check=True)
import json
import shutil
versions=json.loads(Path('/usr/share/doc/sde/versions.json').read_text())
for name in ['niny','liria','xanara','taca','sde-view','sde-system-monitor','sde-power-manager','sde-screensaver','calculator','sde-display-settings','sde-wm','sde-compositor','sde-update']:
    output=subprocess.check_output([str(binary/name),'--version'],text=True)
    assert versions[name] in output,(name,output,versions[name])
subprocess.run(['visudo','-cf','/etc/sudoers.d/sde-liria'],check=True)

# SDE Update 2 streams PackageKit transactions through pkcon; without pkcon
# (PackageKit 1.4 ships only pkgcli) it uses APT through pkexec.
if os.access('/usr/bin/pkcon',os.X_OK):
    plans=[('refresh',['/usr/bin/pkcon','-p','refresh','force']),('upgrade',['/usr/bin/pkcon','-p','-y','update'])]
else:
    pkexec=shutil.which('pkexec') or '/usr/bin/pkexec'
    plans=[('refresh',[pkexec,'/usr/bin/apt-get','-q','-o','APT::Status-Fd=1','update']),
           ('clean',[pkexec,'/usr/bin/apt-get','clean'])]
for action,expected in plans:
    actual=subprocess.check_output([str(binary/'sde-update'),'--plan',action],text=True).splitlines()
    assert actual==expected,(actual,expected)
assert subprocess.run([str(binary/'sde-update'),'--plan','invalid']).returncode==2
for upstream in ['pluma','caja','atril','engrampa','eom','mate-system-monitor','mate-power-manager','mate-screensaver','mate-session-manager','openbox','picom']:
    result=subprocess.run(['dpkg-query','-W','-f=${db:Status-Abbrev}',upstream],capture_output=True,text=True)
    assert not result.stdout.startswith('ii'), 'Original application must not be installed: '+upstream
files=subprocess.check_output(['dpkg-query','-L','sde'],text=True).splitlines()
for path in files:
    assert not path.endswith(('/mime.cache','/mimeinfo.cache','/icon-theme.cache','/gschemas.compiled')),path
print('Native ELF executables, metadata, transaction plans and absence of original MATE apps passed.')
