#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile SDE Gallery offline and stage its native resources and legal notices."""
import argparse
import hashlib
import os
import shutil
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sources', type=Path, required=True)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--stage', type=Path, required=True)
p.add_argument('--prefix', default='/usr')
a = p.parse_args()
a.sources = a.sources.resolve(); a.build = a.build.resolve(); a.stage = a.stage.resolve()
if not (a.sources/'.sde-gallery-lock').is_file():
    p.error('Run tools/prepare-gallery.py before the offline build')
digest = hashlib.sha256(Path(__file__).read_bytes() + (str(a.stage)+a.prefix).encode())
for f in sorted(a.sources.rglob('*')):
    if f.is_file(): digest.update(str(f.relative_to(a.sources)).encode()+f.read_bytes())
stamp = a.build/'.built'
if stamp.exists() and stamp.read_text() == digest.hexdigest() and (a.stage/a.prefix.lstrip('/')/'bin/gallery').exists():
    print('Native Gallery already built'); raise SystemExit(0)
if a.build.exists(): shutil.rmtree(a.build)
a.build.mkdir(parents=True)
subprocess.run(['meson','setup',str(a.build/'obj'),str(a.sources),'--prefix='+a.prefix,
                '--buildtype=release','--wrap-mode=nodownload','-Dtarball-appdata=true'], check=True)
subprocess.run(['ninja','-C',str(a.build/'obj'),'-j4'], check=True)
subprocess.run(['meson','install','-C',str(a.build/'obj'),'--destdir',str(a.stage)], check=True)
root = a.stage/a.prefix.lstrip('/')/'share'
# SDE supplies the categorized launcher. Never replace shared system indexes.
for f in (root/'applications').glob('org.debion.Gallery*.desktop'): f.unlink()
for name in ['gschemas.compiled','icon-theme.cache','mimeinfo.cache']:
    for f in a.stage.rglob(name): f.unlink()
docs = root/'doc/sde/upstream/gthumb'; docs.mkdir(parents=True, exist_ok=True)
for name in ['COPYING','copyright','SDE-UPSTREAM-NOTICE']:
    shutil.copy2(a.sources/name, docs/name)
stamp.write_text(digest.hexdigest())
print('Built native Gallery', flush=True)
