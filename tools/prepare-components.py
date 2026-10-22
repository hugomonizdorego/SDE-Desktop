#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch immutable source trees of the forked SDE components before offline
builds: Calculator engine, SDE WM (Openbox), SDE compositor (picom) and the
display profile model. SDE-FLTK lives in sde-fltk/; sde-xsettingsd and
sde-polkit-agent are SDE programs in src/."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT / 'vendor/components')
    p.add_argument('--cache', type=Path, default=ROOT / 'source-work/components')
    args = p.parse_args()
    lock = ROOT / 'suite/components.lock.json'
    version_file = ROOT / 'suite/versions.json'
    versions = json.loads(version_file.read_text())
    # SDE patches (suite/patches/<component>/*.patch) are applied in order.
    patches = sorted((ROOT / 'suite/patches').glob('*/*.patch'))
    signature = hashlib.sha256(lock.read_bytes() + version_file.read_bytes() + Path(__file__).read_bytes() +
                               b''.join(p.read_bytes() for p in patches)).hexdigest()
    marker = args.output / '.sde-component-lock'
    if marker.exists():
        if marker.read_text().strip() == signature:
            print('SDE component sources already prepared'); return
        p.error('Recipe changed; preserve edits and choose another --output')
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    for app in json.loads(lock.read_text())['components']:
        cache = args.cache / app['id']
        if not cache.exists():
            subprocess.run(['git','init','--quiet',str(cache)], check=True)
        def git(*command):
            return subprocess.check_output(['git',*command],cwd=cache,text=True).strip()
        if git('status','--porcelain','--ignore-submodules=all'):
            p.error('Modified cache: ' + str(cache))
        subprocess.run(['git','fetch','--quiet','--depth','1',app['url']+'.git',app['commit']],cwd=cache,check=True)
        git('checkout','--quiet','--detach','FETCH_HEAD')
        if git('rev-parse','HEAD') != app['commit']:
            p.error('Revision mismatch: ' + app['id'])
        if (cache / '.gitmodules').exists():
            git('submodule','update','--init','--recursive','--depth','1')
        out = args.output / app['id']
        if out.exists():
            p.error('Output exists; choose another --output: ' + str(out))
        shutil.copytree(cache,out,ignore=shutil.ignore_patterns('.git'))
        for patch in sorted((ROOT / 'suite/patches' / app['id']).glob('*.patch')):
            subprocess.run(['patch', '--quiet', '-p1', '-d', str(out), '-i', str(patch)], check=True)
        if app['id'] == 'sde-wm':
            path = out / 'configure.ac'
            path.write_text(path.read_text().replace('AC_INIT([openbox], [3.6.1]', 'AC_INIT([openbox], ['+versions[app['id']]+']'))
            for name in ['openbox/openbox.c', 'openbox/screen.c']:
                path = out / name
                path.write_text(path.read_text().replace('"Openbox %s\\n"', '"SDE WM %s (Openbox)\\n"').replace('"Openbox"','"SDE WM"'))
        elif app['id'] == 'sde-compositor':
            path = out / 'meson.build'
            path.write_text(path.read_text().replace("version: '12.5'", "version: '"+versions[app['id']]+"'"))
            path = out / 'src/meson.build'
            path.write_text(path.read_text().replace("\n\t'picom',\n\tsrcs,", "\n\t'sde-compositor',\n\tsrcs,"))
        notice = (f"{app['id']} {versions[app['id']]} — SDE alpha build\n"
                  f"Source: {app['url']}\nRevision: {app['commit']}\nLicense: {app['license']}\n\n"
                  "Original copyrights and license files are retained in the complete corresponding source archive.\n"
                  "SDE modifications: native desktop integration, independent build identity and packaging.\n")
        if app['id'] == 'calculator':
            notice += '\nCopyright (c) 2026 Raad Altaie\n'
        for name in ['COPYING','LICENSE','CONTRIBUTORS','AUTHORS']:
            if (out/name).is_file(): notice += '\n=== '+name+' ===\n'+(out/name).read_text(errors='replace')
        for path in (out/'LICENSES').glob('*') if (out/'LICENSES').is_dir() else []:
            if path.is_file(): notice += '\n=== LICENSES/'+path.name+' ===\n'+path.read_text()
        (out/'copyright').write_text(notice)
        print('Prepared',app['id'],app['commit'],flush=True)
    marker.write_text(signature+'\n')

if __name__ == '__main__': main()
