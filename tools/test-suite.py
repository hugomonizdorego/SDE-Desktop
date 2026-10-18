#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove the installed SDE apps are native fork ELFs, not upstream launchers."""
import argparse, os, subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bin-dir',type=Path,default=Path('/usr/bin'))
args=parser.parse_args()
binary=args.bin_dir.resolve()
apps=['niny','liria','xanara','taca','sde-view','sde-system-monitor',
      'sde-power-manager','sde-power-preferences','sde-power-statistics',
      'sde-screensaver','sde-screensaver-command','sde-screensaver-preferences']
helper=(binary/'sde-suite').read_bytes()
for name in apps:
    path=binary/name
    assert path.is_file() and not path.is_symlink(),name
    data=path.read_bytes()
    assert data[:4]==b'\x7fELF' and data!=helper,name
    dynamic=subprocess.check_output(['readelf','-d',str(path)],text=True)
    if name == 'sde-screensaver-command':
        # This native command-line client talks to the locker over D-Bus;
        # it intentionally has no GTK window or direct GObject dependency.
        assert 'libglib-2.0' in dynamic and 'libdbus' in dynamic,(name,dynamic)
    else:
        assert 'libgtk' in dynamic or 'libgobject' in dynamic,(name,dynamic)
subprocess.run([str(binary/'sde-suite'),'--check'],check=True)
client='/usr/bin/pkgcli' if os.access('/usr/bin/pkgcli',os.X_OK) else '/usr/bin/pkcon'
for action,expected in [('refresh',[client,'refresh']+(['force'] if client.endswith('pkcon') else [])),('upgrade',[client,'update'])]:
    actual=subprocess.check_output([str(binary/'sde-update'),'--plan',action],text=True).splitlines()
    assert actual==expected,(actual,expected)
assert subprocess.run([str(binary/'sde-update'),'--plan','invalid']).returncode==2
for upstream in ['pluma','caja','atril','engrampa','eom','mate-system-monitor','mate-power-manager','mate-screensaver']:
    result=subprocess.run(['dpkg-query','-W','-f=${db:Status-Abbrev}',upstream],capture_output=True,text=True)
    assert not result.stdout.startswith('ii'), 'Original application must not be installed: '+upstream
files=subprocess.check_output(['dpkg-query','-L','sde'],text=True).splitlines()
for path in files:
    assert not path.endswith(('/mime.cache','/mimeinfo.cache','/icon-theme.cache','/gschemas.compiled')),path
print('Native ELF executables, metadata, transaction plans and absence of original MATE apps passed.')
