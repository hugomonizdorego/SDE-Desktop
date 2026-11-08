#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame timing of scrolling and hovering on a large, high-resolution screen
(docs/TOUCH.md, "Lag").

Starts a private Xvfb (2880x1800 by default, the panel of many new laptops),
SDE at 200 % ([appearance] scale), a window manager, the panel, and the
Control Center, and measures what one frame costs while:

  * touch:  a finger flicks the Control Center's longest page (Appearance)
            four times: real gestures through SDE-FLTK's dispatch, written
            into the program's SDE_TOUCH_FIFO in real time (XTest cannot
            touch, and no kernel input device is created);
  * wheel:  the mouse wheel scrolls the same page (xdotool), for comparison;
  * menu:   a finger flicks the Start menu's list;
  * hover:  the pointer sweeps over the taskbar.

For each it reports the frames drawn, the time inside draw() per frame
(SDE_PERF_STATS counters), the pixels per frame and the CPU of the program
and of the X server. On Xvfb the X server does all drawing on the CPU, the
situation of a laptop without a working GPU driver; numbers compare builds,
not machines.

With --compositor, the compositor's own frame time is measured in its
benchmark mode (--benchmark) with the configuration SDE generates
(sde-session --compositor-config) and variants without blur.

Example:
  dbus-run-session -- python3 tools/touch-perf.py --bin-dir build/src \\
      --display :263 --compositor /path/to/sde-compositor --json out.json
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLK = os.sysconf('SC_CLK_TCK')

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--bin-dir', type=Path, required=True, help='directory with sde-settings, sde-panel, sde-session')
ap.add_argument('--data-dir', type=Path, default=ROOT / 'data')
ap.add_argument('--display', default=':263')
ap.add_argument('--size', default='2880x1800')
ap.add_argument('--scale', type=int, default=200, help='SDE interface scale in percent')
ap.add_argument('--compositor', default='', help='sde-compositor (or picom) to benchmark')
ap.add_argument('--frames', type=int, default=120, help='compositor benchmark frames')
ap.add_argument('--scenarios', default='touch,wheel,menu,hover')
ap.add_argument('--full-redraw', action='store_true', help='SDE_FULL_REDRAW=1 (compare)')
ap.add_argument('--json', type=Path)
args = ap.parse_args()
BIN = args.bin_dir.resolve()
W, H = map(int, args.size.split('x'))

tmp = Path(tempfile.mkdtemp(prefix='sde-touch-perf-'))
home = tmp / 'home'
for d in ('.config/sde', 'run', 'Desktop'):
    (home / d).mkdir(parents=True, exist_ok=True)
(home / 'run').chmod(0o700)
(home / '.config/sde/sde.conf').write_text(
    f'[appearance]\nscale={args.scale}\n[welcome]\nshow=false\n[panel]\nhide_duplicate_tray=true\n')
env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'),
           XDG_RUNTIME_DIR=str(home / 'run'), DISPLAY=args.display, SDE_PERF_STATS='1',
           NO_AT_BRIDGE='1')
env.pop('SDE_TOUCH', None)
if args.full_redraw:
    env['SDE_FULL_REDRAW'] = '1'
procs = {}


def start(name, argv, extra=None):
    log = open(tmp / f'{name}.log', 'w')
    e = dict(env, **(extra or {}))
    procs[name] = sp.Popen(argv, env=e, stdout=log, stderr=sp.STDOUT, start_new_session=True)
    return procs[name]


def stop_all():
    for p in reversed(list(procs.values())):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    for p in procs.values():
        try:
            p.wait(timeout=5)
        except sp.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)


def cpu(pid):
    try:
        f = Path(f'/proc/{pid}/stat').read_text()
        f = f[f.rfind(')') + 2:].split()
        return int(f[11]) + int(f[12])
    except OSError:
        return 0


def stats(name):
    """The SDE_PERF_STATS counters of |name| now (SIGUSR2 prints them)."""
    p = procs[name]
    log = tmp / f'{name}.log'
    os.kill(p.pid, signal.SIGUSR2)
    time.sleep(0.3)
    rows = [l for l in log.read_text(errors='replace').splitlines() if l.startswith('sde-perf:') and 'flush=' in l]
    if not rows:
        return {}
    return {k: float(v) for k, v in re.findall(r'(\w+)=([\d.]+)', rows[-1])}


def xdo(*a):
    sp.run(['xdotool', *a], env=env, stdout=sp.PIPE, stderr=sp.DEVNULL, text=True)


def geometry(title):
    r = sp.run(['xdotool', 'search', '--sync', '--onlyvisible', '--name', title, 'getwindowgeometry', '--shell'],
               env=env, stdout=sp.PIPE, stderr=sp.DEVNULL, text=True, timeout=20)
    g = dict(l.split('=', 1) for l in r.stdout.splitlines() if '=' in l)
    return int(g['X']), int(g['Y']), int(g['WIDTH']), int(g['HEIGHT'])


def finger(fifo, x, y0, y1, steps=40, step_s=0.008, hold=0):
    """One finger from (x, y0) to (x, y1), in real time; |hold| s still at the end."""
    with open(fifo, 'w') as f:
        def put(line):
            f.write(line + '\n')
            f.flush()
        put(f'b 1 {x} {y0}')
        for i in range(1, steps + 1):
            time.sleep(step_s)
            put(f'u 1 {x} {y0 + (y1 - y0) * i / steps:.1f}')
        if hold:
            time.sleep(hold)
            put(f'u 1 {x} {y1:.1f}')
        time.sleep(step_s)
        put(f'e 1 {x} {y1}')


def phase(name, program, action):
    """Runs |action| and reports frames and CPU of |program| and Xvfb."""
    before = stats(program)
    c0 = {n: cpu(p.pid) for n, p in procs.items()}
    t0 = time.time()
    action()
    time.sleep(0.4)
    dt = time.time() - t0
    after = stats(program)
    c1 = {n: cpu(p.pid) for n, p in procs.items()}
    d = {k: after.get(k, 0) - before.get(k, 0) for k in ('flush', 'full', 'partial', 'px', 'draw_ms')}
    frames = int(d['full'] + d['partial'])
    out = {
        'seconds': round(dt, 2),
        'frames': frames,
        'full': int(d['full']),
        'draw_ms_per_frame': round(d['draw_ms'] / frames, 2) if frames else 0,
        'mpx_per_frame': round(d['px'] / frames / 1e6, 2) if frames else 0,
        'cpu_ms': {n: round((c1[n] - c0[n]) * 1000 / CLK) for n in c1},
    }
    print(f'{name:8s} frames {frames:4d} (full {out["full"]:3d})  draw {out["draw_ms_per_frame"]:6.2f} ms/frame  '
          f'{out["mpx_per_frame"]:5.2f} Mpx/frame  CPU ms: ' +
          ', '.join(f'{n} {v}' for n, v in out['cpu_ms'].items()), flush=True)
    return out


def compositor_bench(config, label):
    exe = args.compositor
    # A busy screen: the Control Center over the desktop.
    t0 = time.time()
    r = sp.run([exe, '--config', str(config), '--benchmark', str(args.frames)], env=env,
               stdout=sp.PIPE, stderr=sp.STDOUT, text=True, timeout=600)
    dt = time.time() - t0
    ms = dt * 1000 / args.frames
    print(f'compositor {label:28s} {ms:7.2f} ms/frame ({args.frames} frames, exit {r.returncode})', flush=True)
    return {'ms_per_frame': round(ms, 2), 'exit': r.returncode}


results = {'size': args.size, 'scale': args.scale, 'phases': {}, 'compositor': {}}
try:
    start('Xvfb', ['Xvfb', args.display, '-screen', '0', args.size + 'x24', '-nolisten', 'tcp', '+extension', 'GLX'])
    for _ in range(80):
        if Path(f'/tmp/.X11-unix/X{args.display.lstrip(":")}').exists():
            break
        time.sleep(0.1)
    time.sleep(0.5)
    wm = shutil.which('sde-wm', path=str(BIN)) or shutil.which('openbox')
    if wm:
        start('wm', [wm, '--config-file', str(args.data_dir / 'openbox/rc.xml')])
        time.sleep(1)
    panel_fifo = tmp / 'panel.fifo'
    os.mkfifo(panel_fifo)
    start('sde-panel', [str(BIN / 'sde-panel')], {'SDE_TOUCH_FIFO': str(panel_fifo)})
    cc_fifo = tmp / 'settings.fifo'
    os.mkfifo(cc_fifo)
    start('sde-settings', [str(BIN / 'sde-settings'), '--page', 'appearance'], {'SDE_TOUCH_FIFO': str(cc_fifo)})
    x, y, w, h = geometry('Control Center')
    # The page's middle, right of the sidebar.
    px, top, bottom = x + w * 2 // 3, y + h // 5, y + h * 4 // 5
    time.sleep(2)
    stats('sde-settings')
    print(f'{args.size} at {args.scale} %, Control Center {w}x{h}', flush=True)
    wanted = args.scenarios.split(',')
    if 'touch' in wanted:
        def flicks():
            for _ in range(2):
                finger(cc_fifo, px, bottom, top, steps=30)  # up: the page scrolls down
                time.sleep(1.6)
                finger(cc_fifo, px, top, bottom, steps=30)
                time.sleep(1.6)
        results['phases']['touch'] = phase('touch', 'sde-settings', flicks)

        def drag():
            finger(cc_fifo, px, bottom, top, steps=120, step_s=0.008, hold=0.2)
            finger(cc_fifo, px, top, bottom, steps=120, step_s=0.008, hold=0.2)
        results['phases']['drag'] = phase('drag', 'sde-settings', drag)
    if 'wheel' in wanted and shutil.which('xdotool'):
        def wheel():
            xdo('mousemove', str(px), str((top + bottom) // 2))
            for b in ('5', '4'):
                for _ in range(20):
                    xdo('click', b)
                    time.sleep(0.03)
        results['phases']['wheel'] = phase('wheel', 'sde-settings', wheel)
    if 'menu' in wanted:
        sp.run([str(BIN / 'sde-panel'), '--menu'], env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL, timeout=10)
        time.sleep(1.5)
        try:
            mx, my, mw, mh = geometry('SDE Menu')
        except Exception:
            mx, my, mw, mh = 0, H // 3, W // 2, H // 2
        lx = mx + mw // 4

        def menu():
            for _ in range(2):
                finger(panel_fifo, lx, my + mh * 3 // 4, my + mh // 3, steps=25)
                time.sleep(1.4)
                finger(panel_fifo, lx, my + mh // 3, my + mh * 3 // 4, steps=25)
                time.sleep(1.4)
        results['phases']['menu'] = phase('menu', 'sde-panel', menu)
        xdo('key', 'Escape')
        time.sleep(0.5)
    if 'hover' in wanted and shutil.which('xdotool'):
        def hover():
            for _ in range(3):
                for hx in range(0, W, 48):
                    xdo('mousemove', str(hx), str(H - 20))
                    time.sleep(0.008)
        results['phases']['hover'] = phase('hover', 'sde-panel', hover)
    if args.compositor:
        # The profile of a laptop with a working GPU (OpenGL, blur, vsync),
        # also on Xvfb, where OpenGL renders on the CPU (llvmpipe).
        genv = dict(env, SDE_GRAPHICS='hardware', SDE_OPENGL='accelerated:touch-perf')
        r = sp.run([str(BIN / 'sde-session'), '--compositor-config'], env=genv, stdout=sp.PIPE, text=True, timeout=30)
        conf = Path(r.stdout.strip())
        if conf.is_file():
            text = conf.read_text()
            variants = {'generated': text}
            free = re.sub(r'vsync = \w+', 'vsync = false', text)  # paint time, not vblank waits
            variants['generated, no vsync'] = free
            variants['no blur, no vsync'] = re.sub(r'blur-background\s*=\s*true', 'blur-background = false', free)
            variants['no blur, xrender'] = re.sub(r'backend = "\w+"', 'backend = "xrender"',
                                                  variants['no blur, no vsync'])
            for label, body in variants.items():
                path = tmp / ('compositor-' + re.sub(r'\W+', '-', label) + '.conf')
                path.write_text(body)
                results['compositor'][label] = compositor_bench(path, label)
            results['compositor_config'] = text
finally:
    stop_all()
    if args.json:
        args.json.write_text(json.dumps(results, indent=2))
    shutil.rmtree(tmp, ignore_errors=True)
