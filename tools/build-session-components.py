#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the forked SDE session components without network: SDE WM
(Openbox) and SDE compositor (picom). SDE-FLTK is built by CMake at
configure time, because every SDE program links it."""
import argparse
import hashlib
import os
import shutil
import subprocess
from pathlib import Path

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sources',type=Path,required=True)
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--stage',type=Path,required=True)
    p.add_argument('--prefix',default='/usr')
    args=p.parse_args()
    args.sources=args.sources.resolve(); args.build=args.build.resolve(); args.stage=args.stage.resolve()
    args.build.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parent.parent
    for name in ['sde-wm','sde-compositor']:
        src=args.sources/name
        digest=hashlib.sha256(Path(__file__).read_bytes()+str(args.stage).encode()+args.prefix.encode())
        for f in sorted(src.rglob('*')):
            if f.is_file(): digest.update(str(f.relative_to(src)).encode()+f.read_bytes())
        stamp=args.build/(name+'.built')
        bindir='bin'
        binary=args.stage/args.prefix.lstrip('/')/bindir/name
        if stamp.exists() and stamp.read_text()==digest.hexdigest() and binary.exists(): continue
        work=args.build/name
        if work.exists(): shutil.rmtree(work)
        shutil.copytree(src,work/'source'); source=work/'source'; obj=work/'obj'; obj.mkdir()
        logfile=args.build/(name+'.log')
        def run(cmd,cwd):
            with logfile.open('a') as stream:
                stream.write('$ '+' '.join(cmd)+'\n'); stream.flush()
                status=subprocess.run(cmd,cwd=cwd,stdout=stream,stderr=subprocess.STDOUT)
            if status.returncode:
                print('\n'.join(logfile.read_text(errors='replace').splitlines()[-100:]),flush=True)
                raise SystemExit(name+' build failed: '+str(logfile))
        logfile.write_text('')
        if name=='sde-wm':
            run(['autoreconf','-vif'],source)
            run([str(source/'configure'),'--prefix='+args.prefix,'--datadir='+args.prefix+'/share/sde/wm',
                 '--sysconfdir=/etc/xdg/sde/wm','--disable-shared','--enable-static','--disable-nls',
                 '--disable-session-management','--disable-startup-notification','--enable-imlib2','--disable-librsvg'],obj)
            run(['make','-j4','openbox/openbox'],obj)
            artifact=obj/'openbox/openbox'
        else:
            run(['meson','setup',str(obj),str(source),'--prefix='+args.prefix,'--buildtype=release',
                 '--wrap-mode=nodownload','-Dcompton=false','-Dwith_docs=false','-Dunittest=true'],work)
            run(['ninja','-C',str(obj),'src/sde-compositor'],work)
            run(['meson','test','-C',str(obj),'--print-errorlogs'],work)
            artifact=obj/'src/sde-compositor'
        binary.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(artifact,binary)
        upstream={'sde-wm':'openbox','sde-compositor':'picom'}[name]
        docs=args.stage/args.prefix.lstrip('/')/'share/doc/sde/upstream'/upstream
        docs.mkdir(parents=True,exist_ok=True)
        for f in ['copyright','COPYING','AUTHORS','CONTRIBUTORS']:
            if (src/f).is_file(): shutil.copy2(src/f,docs/f)
        if (src/'LICENSES').exists(): shutil.copytree(src/'LICENSES',docs/'LICENSES',dirs_exist_ok=True)
        stamp.write_text(digest.hexdigest())
        print('Built native',name,flush=True)

if __name__=='__main__': main()
