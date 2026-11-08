#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Scrolling a Control Center page moves its pixels instead of repainting
them (src/common/paint.cpp, docs/TOUCH.md "Lag"): the screen after two
finger drags (SDE_TOUCH_FIFO) must be pixel-identical to the same drags
with SDE_FULL_REDRAW=1, at 100 % and at 200 %.

Usage: test_scroll_blit.py SDE-SETTINGS
Exit status 77 (skipped) without Xvfb, xdotool or xwd.
"""
import os
import shutil
import struct
import subprocess as sp
import sys
import tempfile
import time


def screen(env):
    """The colours of the screen's pixels (xwd keeps a padding byte per pixel
    whose content means nothing)."""
    data = sp.run(['xwd', '-root', '-silent'], env=env, stdout=sp.PIPE, timeout=30).stdout
    h = struct.unpack('>25I', data[:100])
    header, order, bpp, ncolors = h[0], h[7], h[11], h[19]
    masks = h[16] | h[17] | h[18]
    pixels = data[header + ncolors * 12:]
    if bpp != 32:
        return pixels
    keep = [i for i in range(4) if (masks >> (8 * (i if order == 0 else 3 - i))) & 0xFF]
    return b''.join(pixels[i::4] for i in keep)


def run(settings, scale, size, full):
    read_fd, write_fd = os.pipe()
    server = sp.Popen(['Xvfb', '-displayfd', str(write_fd), '-noreset', '-nolisten', 'tcp', '-screen', '0',
                       size + 'x24'], pass_fds=(write_fd,), stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    os.close(write_fd)
    number = b''
    while not number.endswith(b'\n'):
        chunk = os.read(read_fd, 16)
        if not chunk:
            break
        number += chunk
    os.close(read_fd)
    with tempfile.TemporaryDirectory(prefix='sde-scroll-blit-') as home:
        os.makedirs(os.path.join(home, '.config', 'sde'))
        os.makedirs(os.path.join(home, 'run'), mode=0o700)
        with open(os.path.join(home, '.config', 'sde', 'sde.conf'), 'w') as f:
            f.write(f'[appearance]\nscale={scale}\n')
        fifo = os.path.join(home, 'fifo')
        os.mkfifo(fifo)
        env = dict(os.environ, DISPLAY=':' + number.decode().strip(), HOME=home,
                   XDG_CONFIG_HOME=os.path.join(home, '.config'), XDG_RUNTIME_DIR=os.path.join(home, 'run'),
                   SDE_TOUCH_FIFO=fifo)
        for k in ('SDE_PANEL_QA_DIR', 'SDE_FULL_REDRAW', 'SDE_TOUCH'):
            env.pop(k, None)
        if full:
            env['SDE_FULL_REDRAW'] = '1'
        app = sp.Popen([settings, '--page', 'appearance'], env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        try:
            out = sp.run(['xdotool', 'search', '--sync', '--name', 'Control Center', 'getwindowgeometry', '--shell'],
                         env=env, stdout=sp.PIPE, text=True, timeout=30).stdout
            g = dict(line.split('=', 1) for line in out.splitlines() if '=' in line)
            wx, wy, ww, wh = (int(g[k]) for k in ('X', 'Y', 'WIDTH', 'HEIGHT'))
            time.sleep(2)
            before = screen(env)
            px = wx + ww * 2 // 3
            with open(fifo, 'w') as f:
                for y0, y1 in ((wy + wh * 4 // 5, wy + wh // 5), (wy + wh * 3 // 4, wy + wh // 2)):
                    f.write(f'b 1 {px} {y0}\n')
                    f.flush()
                    for i in range(1, 41):
                        time.sleep(0.012)
                        f.write(f'u 1 {px} {y0 + (y1 - y0) * i / 40:.1f}\n')
                        f.flush()
                    time.sleep(0.25)  # held still: no momentum, the same place every time
                    f.write(f'u 1 {px} {y1}\ne 1 {px} {y1}\n')
                    f.flush()
                    time.sleep(0.5)
            time.sleep(1.0)
            return before, screen(env)
        finally:
            app.terminate()
            app.wait(timeout=10)
            server.terminate()
            server.wait(timeout=10)


def main():
    settings = os.path.abspath(sys.argv[1])
    for tool in ('Xvfb', 'xdotool', 'xwd'):
        if not shutil.which(tool):
            print('skip: no ' + tool)
            return 77
    failures = 0
    for scale, size in ((100, '1440x900'), (200, '2880x1800')):
        before, moved = run(settings, scale, size, False)
        _, drawn = run(settings, scale, size, True)
        scrolled = before != moved
        same = moved == drawn and len(moved) > 1000
        print(('ok    ' if scrolled else 'FAIL  ') + f'two drags scroll the page at {scale} %')
        print(('ok    ' if same else 'FAIL  ') + f'the scrolled page at {scale} % equals a full repaint')
        failures += (0 if same else 1) + (0 if scrolled else 1)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
