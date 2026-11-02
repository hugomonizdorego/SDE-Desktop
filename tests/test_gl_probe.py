#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""sde-gl-probe, sde-session's OpenGL check at login (0.2.0 Alpha 3.1), on
Xvfb: Mesa's software OpenGL is reported as such (with and without
GLX_MESA_query_renderer), a display without GLX and no display at all
neither crash nor keep the session waiting.

  test_gl_probe.py PROBE

Exit status 77 (skipped) without Xvfb."""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def xvfb(extra):
    """A private X server with |extra| arguments: (process, display)."""
    read, write = os.pipe()
    p = subprocess.Popen(['Xvfb', '-displayfd', str(write), '-screen', '0', '640x480x24', '-nolisten', 'tcp',
                          *extra], pass_fds=(write,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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


def probe(path, display, **env):
    e = dict(os.environ, **env)
    if display:
        e['DISPLAY'] = display
    else:
        e.pop('DISPLAY', None)
    t0 = time.monotonic()
    r = subprocess.run([path], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
    return r.returncode, r.stdout.strip(), time.monotonic() - t0


def main():
    path = sys.argv[1]
    if not shutil.which('Xvfb'):
        print('Xvfb not available: skipped')
        return 77
    failures = []
    for extra, kind in (([], 'with GLX'), (['-extension', 'GLX'], 'without GLX')):
        server, display = xvfb(extra)
        if server is None:
            print('Xvfb did not start: skipped')
            return 77
        try:
            for env in ({}, {'SDE_GL_PROBE_CONTEXT': '1'}):
                status, out, took = probe(path, display, **env)
                label = f'{kind}{" (context)" if env else ""}'
                print(f'{label}: exit {status}, {took:.2f} s: {out}')
                if status != 0 or took > 5:
                    failures.append(f'{label}: exit {status} after {took:.1f} s')
                elif kind == 'with GLX' and not (out.startswith('glx=1 accelerated=0 renderer=') or
                                                 out.startswith('glx=0 ')):
                    # Xvfb's OpenGL is always Mesa's software renderer (or
                    # missing when the drivers are not installed).
                    failures.append(f'{label}: not reported as software: {out}')
                elif kind == 'without GLX' and not out.startswith('glx=0 '):
                    failures.append(f'{label}: GLX reported: {out}')
        finally:
            server.terminate()
            server.wait(timeout=10)
    status, out, took = probe(path, None)
    print(f'no display: exit {status}, {took:.2f} s: {out}')
    if status != 2 or took > 5:
        failures.append(f'no display: exit {status} after {took:.1f} s')
    for f in failures:
        print('FAIL', f)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
