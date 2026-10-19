#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the installed GTK4 Gallery, private schemas, photo browser and About."""
import argparse
import configparser
import os
from pathlib import Path
import subprocess as sp
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output-dir', type=Path, default=Path('dist/gallery-smoke'))
a = p.parse_args()
out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='sde-gallery-') as temporary:
    home = Path(temporary)
    (home/'.config/sde').mkdir(parents=True)
    (home/'.config/sde/sde.conf').write_text('[appearance]\ntheme=light\n')
    (home/'run').mkdir(mode=0o700)
    pictures = home/'Pictures'; pictures.mkdir()
    env = {**os.environ, 'DISPLAY': ':96', 'HOME': str(home), 'XDG_CONFIG_HOME': str(home/'.config'),
           'XDG_CACHE_HOME': str(home/'.cache'), 'XDG_DATA_HOME': str(home/'.local/share'),
           'XDG_CURRENT_DESKTOP': 'SDE', 'XDG_RUNTIME_DIR': str(home/'run'),
           'GSK_RENDERER': 'cairo', 'GDK_BACKEND': 'x11', 'LANG': 'C.UTF-8',
           'GTK_A11Y': 'none', 'GSETTINGS_BACKEND': 'keyfile'}
    def run(*cmd, check=True):
        return sp.run(cmd, env=env, capture_output=True, text=True, check=check)
    procs = []
    logs = []
    def spawn(*cmd):
        log = (out/(Path(cmd[0]).name+'.log')).open('w'); logs.append(log)
        process = sp.Popen(cmd, env=env, stdout=log, stderr=log); procs.append(process)
        return process
    def wait(command):
        for _ in range(100):
            result = run(*command, check=False)
            if result.returncode == 0: return result.stdout.strip()
            time.sleep(.1)
        raise AssertionError(command)
    def shot(name):
        time.sleep(.5)
        run('import', '-window', 'root', str(out/(name+'.png')))
    try:
        spawn('Xvfb', ':96', '-screen', '0', '1366x768x24', '-ac', '-noreset', '-nolisten', 'tcp')
        wait(['xdpyinfo'])
        spawn('sde-wm', '--config-file', '/usr/share/sde/openbox/rc.xml')
        run('convert', '-size', '640x420', 'gradient:#167d9a-#d8f2f4', str(pictures/'SDE Gallery.png'))
        gallery = spawn('gallery', str(pictures))
        win = wait(['xdotool', 'search', '--onlyvisible', '--class', 'org.debion.Gallery']).splitlines()[0]
        assert gallery.poll() is None
        run('xdotool', 'windowsize', win, '1100', '680')
        run('xdotool', 'windowmove', win, '110', '40')
        shot('01-gallery-browser')
        # Open the native photo viewer through the same application instance.
        run('gallery', str(pictures/'SDE Gallery.png'))
        shot('02-gallery-photo')
        # Call the real public application action, no synthetic About fixture.
        run('gapplication', 'action', 'org.debion.Gallery', 'about')
        shot('03-gallery-about')
        run('xdotool', 'key', 'Escape')
        # The wallpaper adapter must update SDE without resetting other keys.
        run('sde-settings', '--wallpaper', str(pictures/'SDE Gallery.png'))
        cfg = configparser.ConfigParser()
        cfg.read(home/'.config/sde/sde.conf')
        assert cfg['appearance']['wallpaper'] == str(pictures/'SDE Gallery.png')
        assert cfg['appearance']['theme'] == 'light'
        schemas = run('gsettings', 'list-schemas').stdout.splitlines()
        assert 'org.debion.Gallery.browser' in schemas or 'org.debion.Gallery' in schemas
        assert 'gthumb 4.0' in run('gallery', '--version').stdout.lower()
        # Credits are compiled into the actual Gallery executable.
        data = Path('/usr/bin/gallery').read_bytes()
        for credit in [b'Hugo Moniz do Rego', b'Paolo Bacchilega', b'https://gitlab.gnome.org/GNOME/gthumb']:
            assert credit in data, credit
        print('Gallery: native GTK4 browser, image, About, schemas, version and SDE wallpaper passed.')
    finally:
        for process in reversed(procs):
            if process.poll() is None: process.terminate()
        for process in reversed(procs):
            try: process.wait(timeout=5)
            except sp.TimeoutExpired: process.kill(); process.wait()
        for log in logs: log.close()
