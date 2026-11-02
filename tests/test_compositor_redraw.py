#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The SDE compositor shows what windows draw after they were mapped or
resized, with OpenGL too (0.2.0 Alpha 3.1).

With Mesa's software OpenGL (Xvfb, virtual machines without 3D, a machine
without its graphics driver) GLX_EXT_texture_from_pixmap copies a window's
pixmap when the compositor binds it, and picom 12 binds a window only when
it is mapped or resized. A window resized while its program was still busy
starting kept its old size on the screen, the rest of its frame
see-through, until the compositor was reset; anything drawn after a window
was mapped did not show either.
suite/patches/sde-compositor/0002-rebind-damaged-windows.patch binds a
damaged window again on such drivers.

This test runs the compositor on a private Xvfb with the OpenGL (GLX)
backend, and with XRender as a control, and lets
tests/compositor_redraw_probe.cpp draw windows and read the screen back.

  test_compositor_redraw.py --compositor sde-compositor --probe PROBE

SDE_COMPOSITOR overrides --compositor. Exit status 77 (skipped) without a
built compositor, without Xvfb, or when the X server has no usable GLX."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CONFIG = """# tests/test_compositor_redraw.py: nothing but the windows themselves.
shadow = false;
fading = false;
vsync = false;
use-damage = true;
"""
# What picom's GLX backend logs when the X server or the drivers offer no
# usable GLX: then the OpenGL part is skipped, not failed.
NO_GLX = ('No GLX extension', 'GLX_EXT_texture_from_pixmap is not supported', 'Root visual is not',
          'Failed to get GLX context', 'Failed to attach GLX context', 'Failed to setup OpenGL',
          "Couldn't find a suitable fbconfig", 'Failed to acquire XVisualInfo')


def xvfb():
    """A private X server: (process, display), or (None, None). The display
    is SDE_TEST_DISPLAY when set, otherwise the first free one (-displayfd)."""
    if not shutil.which('Xvfb'):
        return None, None
    args = ['-screen', '0', '800x600x24', '-nolisten', 'tcp']
    fixed = os.environ.get('SDE_TEST_DISPLAY', '').lstrip(':')
    if fixed:
        p = subprocess.Popen(['Xvfb', f':{fixed}', *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if Path(f'/tmp/.X11-unix/X{fixed}').exists():
                return p, f':{fixed}'
            time.sleep(0.05)
        p.terminate()
        return None, None
    read, write = os.pipe()
    p = subprocess.Popen(['Xvfb', '-displayfd', str(write), *args], pass_fds=(write,),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write)
    number = b''
    while not number.endswith(b'\n'):
        chunk = os.read(read, 16)
        if not chunk:
            break
        number += chunk
    os.close(read)
    if not number.strip():
        p.terminate()
        return None, None
    return p, ':' + number.decode().strip()


def stop(proc):
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def run_backend(compositor, probe, backend, tmp):
    """'pass', 'skip: why' or 'fail: why' for one backend."""
    server, display = xvfb()
    if server is None:
        return 'skip: no Xvfb'
    home = tmp / backend
    home.mkdir()
    conf = home / 'compositor.conf'
    conf.write_text(CONFIG)
    log = home / 'compositor.log'
    env = dict(os.environ, DISPLAY=display, HOME=str(home), XDG_CONFIG_HOME=str(home / 'config'),
               XDG_RUNTIME_DIR=str(home))
    env.pop('WAYLAND_DISPLAY', None)
    comp = None
    try:
        with open(home / 'compositor.out', 'w') as out:
            comp = subprocess.Popen([compositor, '--backend', backend, '--config', str(conf), '--log-level=info',
                                     f'--log-file={log}'], env=env, stdout=out, stderr=subprocess.STDOUT)
        r = subprocess.run([probe], env=env, timeout=240, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
        print(f'{backend}:')
        for line in r.stdout.strip().splitlines():
            print('  ' + line)
        died = comp.poll() is not None
        text = (log.read_text(errors='replace') if log.exists() else '') + \
            (home / 'compositor.out').read_text(errors='replace')
        if died:
            if backend == 'glx' and comp.returncode > 0 and any(s in text for s in NO_GLX):
                return 'skip: the compositor cannot use GLX here: ' + text.strip()[-300:]
            return f'fail: the compositor exited ({comp.returncode}): ' + text.strip()[-500:]
        if r.returncode == 77:
            return 'fail: the compositor runs but does not draw the screen: ' + text.strip()[-500:]
        if r.returncode != 0:
            return 'fail: windows do not show what they drew'
        return 'pass'
    except subprocess.TimeoutExpired:
        return 'fail: the probe did not finish'
    finally:
        stop(comp)
        stop(server)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compositor', default='')
    ap.add_argument('--probe', required=True)
    a = ap.parse_args()
    compositor = os.environ.get('SDE_COMPOSITOR') or a.compositor
    if not compositor or not Path(compositor).is_file():
        print(f'no built SDE compositor ({compositor or "none given"}): skipped')
        return 77
    if not shutil.which('Xvfb'):
        print('Xvfb not available: skipped')
        return 77
    results = {}
    with tempfile.TemporaryDirectory(prefix='sde-compositor-redraw-') as tmp:
        for backend in ('glx', 'xrender'):
            results[backend] = run_backend(compositor, a.probe, backend, Path(tmp))
            print(f'{backend}: {results[backend]}')
    if any(r.startswith('fail') for r in results.values()):
        return 1
    if results['glx'] != 'pass':
        return 77
    print('the compositor shows what windows draw, with OpenGL and XRender')
    return 0


if __name__ == '__main__':
    sys.exit(main())
