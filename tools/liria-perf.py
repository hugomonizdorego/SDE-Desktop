#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""How fast Liria (src/liria) is, on a private Xvfb (0.2.0 Alpha 4).

  tools/liria-perf.py [--liria build/src/liria/liria] [--display 241]
                      [--size 1366x768] [--scale 100] [--repeat 3]
                      [--json results.json] [--only NAME,...]

Starts Xvfb (--size, SDE's scale --scale) and Openbox when installed, and Liria in a
throw-away home with folders of 1 000 and 10 000 files, without a session
bus (Liria alone). Liria describes its window in SDE_PANEL_QA_DIR, so the
scenarios know when it is done; each is driven like a user (xdotool,
XTest) and measured until Liria is idle again (no 10 ms with more than
1 ms of CPU time for 0.3 s):

  startup      process start until the first frame ("first", Liria's own
               clock) and until idle (the Home page: pins, drives, recent)
  open-1k      the address bar opens the folder of 1 000 files
  open-10k     ... and the folder of 10 000 files (icon view)
  details, compact, icons
               Ctrl+Shift+6, Ctrl+Shift+5 and Ctrl+Shift+3 over 10 000 files
  select-all   Ctrl+A over 10 000 files
  scroll       40 mouse wheel steps over the 10 000 files
  hover-*      the pointer moves like a hand (XTest, 100 motion events a
               second, 600 pixels a second) over the files (icons, details,
               compact; 10 000 files), the sidebar, the Home page and the
               command bar; the CPU use of Liria and of the X server while
               it moves ("cpu%", "xcpu%")

For each the wall time ("ms") and the CPU time of Liria and of the X
server ("cpu", "xcpu"; /proc/PID/stat) are printed; with --repeat the
median of the runs. Exit status 0, or 77 without Xvfb, xdotool or Liria.
docs/LIRIA.md has the results and those of the Liria of 0.2.0 Alpha 3.1.
"""
import argparse
import ctypes
import json
import os
import shutil
import statistics
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TICK = os.sysconf('SC_CLK_TCK')


def cpu_seconds(pid):
    try:
        f = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return (int(f[11]) + int(f[12])) / TICK
    except (OSError, IndexError, ValueError):
        return 0.0


class XTest:
    """Pointer motion without a process per event."""

    def __init__(self, display):
        self.x11 = ctypes.CDLL('libX11.so.6')
        self.xtst = ctypes.CDLL('libXtst.so.6')
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                                   ctypes.c_ulong]
        self.dpy = self.x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise RuntimeError('cannot open ' + display)

    def move(self, x, y):
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(x), int(y), 0)
        self.x11.XFlush(self.dpy)


class Bench:
    def __init__(self, args, temp):
        self.args, self.temp = args, Path(temp)
        self.home = self.temp / 'home'
        self.qa = self.temp / 'qa'
        self.qa.mkdir()
        dirs = (('DESKTOP', 'Desktop'), ('DOCUMENTS', 'Documents'), ('DOWNLOAD', 'Downloads'), ('MUSIC', 'Music'),
                ('PICTURES', 'Pictures'), ('VIDEOS', 'Videos'))
        for _, d in dirs:
            (self.home / d).mkdir(parents=True)
        (self.home / '.config/sde').mkdir(parents=True)
        (self.home / '.config/sde/sde.conf').write_text(f'[appearance]\nscale={args.scale}\n')
        (self.home / '.config/user-dirs.dirs').write_text(''.join(f'XDG_{k}_DIR="$HOME/{v}"\n' for k, v in dirs))
        for n, name in ((1000, 'k1'), (10000, 'k10')):
            d = self.home / name
            d.mkdir()
            for i in range(n):
                (d / f'document {i:05d}.txt').write_text('x')
        self.display = ':%d' % args.display
        self.env = dict(os.environ, DISPLAY=self.display, HOME=str(self.home),
                        XDG_CONFIG_HOME=str(self.home / '.config'), XDG_DATA_HOME=str(self.home / '.local/share'),
                        XDG_CACHE_HOME=str(self.home / '.cache'), XDG_STATE_HOME=str(self.home / '.local/state'),
                        XDG_RUNTIME_DIR=str(self.temp / 'run'), SDE_PANEL_QA_DIR=str(self.qa),
                        LANG='C.UTF-8', LC_ALL='C.UTF-8', GIO_USE_VOLUME_MONITOR='unix',
                        DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-liria-perf')
        (self.temp / 'run').mkdir(mode=0o700)
        self.xvfb = sp.Popen(['Xvfb', self.display, '-screen', '0', args.size + 'x24', '-nolisten', 'tcp', '-noreset'],
                             stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        for _ in range(100):
            if sp.run(['xdotool', 'getmouselocation'], env=self.env, capture_output=True).returncode == 0:
                break
            time.sleep(0.05)
        self.wm = sp.Popen(['openbox'], env=self.env, stdout=sp.DEVNULL, stderr=sp.DEVNULL) \
            if shutil.which('openbox') else None
        time.sleep(0.4)
        self.xt = XTest(self.display)
        self.proc = None

    def close(self):
        for p in (self.proc, self.wm, self.xvfb):
            if p and p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except sp.TimeoutExpired:
                    p.kill()

    # ------------------------------------------------------------ Liria

    def state(self):
        out, controls = {}, {}
        try:
            text = (self.qa / 'Liria.txt').read_text()
        except OSError:
            return out, controls
        for line in text.splitlines():
            f = line.split('\t')
            if f[0].startswith('@'):
                k, _, v = f[0][1:].partition(':')
                out[k] = v
            elif len(f) >= 6:
                controls[f[0]] = [int(v) for v in f[1:5]]
        return out, controls

    def wait(self, cond, timeout=30.0):
        end = time.time() + timeout
        while time.time() < end:
            st = self.state()
            if cond(*st):
                return True
            time.sleep(0.005)
        return False

    def idle(self, quiet=0.3, timeout=30.0):
        """Waits until Liria used at most 1 ms of CPU in each 10 ms for |quiet| s."""
        end = time.time() + timeout
        last, calm_since = cpu_seconds(self.proc.pid), time.time()
        while time.time() < end:
            time.sleep(0.01)
            now = cpu_seconds(self.proc.pid)
            if now - last > 0.001:
                calm_since = time.time()
            last = now
            if time.time() - calm_since >= quiet:
                return calm_since
        return time.time()

    def xdo(self, *a):
        sp.run(['xdotool'] + [str(x) for x in a], env=self.env, capture_output=True)

    def origin(self):
        st, _ = self.state()
        out = sp.run(['xwininfo', '-id', st['xid']], env=self.env, capture_output=True, text=True).stdout
        x = int(out.split('Absolute upper-left X:')[1].split()[0])
        y = int(out.split('Absolute upper-left Y:')[1].split()[0])
        return x, y

    def start(self):
        (self.qa / 'Liria.txt').unlink(missing_ok=True)
        t0 = time.time()
        self.proc = sp.Popen([self.args.liria], env=self.env, stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        self.wait(lambda s, c: s.get('xid') and s.get('location') == 'liria:home')
        calm = self.idle()
        st, _ = self.state()
        self.xdo('windowactivate', '--sync', st['xid'])
        w, h = (int(v) for v in self.args.size.split('x'))
        self.xdo('windowsize', '--sync', st['xid'], w - 86, h - 48)
        self.xdo('windowmove', '--sync', st['xid'], 40, 20)
        time.sleep(0.3)
        self.idle()
        return {'ms': (calm - t0) * 1000, 'first': float(st.get('startup-ms', 0)),
                'cpu': cpu_seconds(self.proc.pid) * 1000}

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(5)
        self.proc = None

    def measure(self, action, done, timeout=30.0):
        c0, x0, t0 = cpu_seconds(self.proc.pid), cpu_seconds(self.xvfb.pid), time.time()
        action()
        self.wait(done, timeout)
        shown = time.time()
        calm = self.idle()
        return {'ms': (max(calm, shown) - t0) * 1000, 'shown': (shown - t0) * 1000,
                'cpu': (cpu_seconds(self.proc.pid) - c0) * 1000, 'xcpu': (cpu_seconds(self.xvfb.pid) - x0) * 1000}

    def go(self, path):
        self.xdo('key', 'ctrl+l')
        time.sleep(0.15)
        self.xdo('key', 'ctrl+a')
        self.xdo('type', '--delay', 2, str(path))
        time.sleep(0.1)

    def hover(self, rect, rows=6, speed=600.0, rate=100.0):
        """Sweeps the pointer over |rect| (window pixels) like a hand."""
        ox, oy = self.origin()
        x0, y0, w, h = rect
        path = []
        for r in range(rows):
            y = oy + y0 + h * (r + 0.5) / rows
            xs = (x0 + 8, x0 + w - 8) if r % 2 == 0 else (x0 + w - 8, x0 + 8)
            n = max(2, int(abs(xs[1] - xs[0]) / speed * rate))
            path += [(ox + xs[0] + (xs[1] - xs[0]) * i / n, y) for i in range(n + 1)]
        self.xt.move(*path[0])
        self.idle()
        c0, x0c, t0 = cpu_seconds(self.proc.pid), cpu_seconds(self.xvfb.pid), time.time()
        for i, p in enumerate(path):
            self.xt.move(*p)
            delay = t0 + (i + 1) / rate - time.time()
            if delay > 0:
                time.sleep(delay)
        wall = time.time() - t0
        result = {'ms': wall * 1000, 'cpu%': (cpu_seconds(self.proc.pid) - c0) / wall * 100,
                  'xcpu%': (cpu_seconds(self.xvfb.pid) - x0c) / wall * 100}
        # Away from the window: no tooltip opens during the next scenario.
        w, h = (int(v) for v in self.args.size.split('x'))
        self.xt.move(w - 6, h - 6)
        self.idle()
        return result


def scenarios(b):
    """(name, function) pairs; each returns a dict of numbers."""
    k1, k10 = b.home / 'k1', b.home / 'k10'

    def opened(path, n):
        return lambda s, c: s.get('location') == str(path) and s.get('loading') == '0' and s.get('items') == str(n)

    def open_folder(path, n):
        b.go(path)  # typed before the clock starts: Enter opens it
        return b.measure(lambda: b.xdo('key', 'Return'), opened(path, n))

    def layout(key, name):
        return b.measure(lambda: b.xdo('key', key), lambda s, c: s.get('layout') == name)

    def files_rect():
        _, c = b.state()
        return c.get('files')

    out = []
    out.append(('open-1k', lambda: open_folder(k1, 1000)))
    out.append(('open-10k', lambda: open_folder(k10, 10000)))
    out.append(('hover-icons', lambda: b.hover(files_rect())))
    out.append(('details', lambda: layout('ctrl+shift+6', 'details')))
    out.append(('hover-details', lambda: b.hover(files_rect())))
    out.append(('compact', lambda: layout('ctrl+shift+5', 'compact')))
    out.append(('hover-compact', lambda: b.hover(files_rect())))
    out.append(('icons', lambda: layout('ctrl+shift+3', 'icons')))
    out.append(('select-all', lambda: b.measure(lambda: b.xdo('key', 'ctrl+a'),
                                                lambda s, c: s.get('selected', '').count('|') == 9999)))

    def scroll():
        r = files_rect()
        ox, oy = b.origin()
        b.xdo('mousemove', ox + r[0] + r[2] // 2, oy + r[1] + r[3] // 2)
        b.idle()
        return b.measure(lambda: b.xdo('click', '--repeat', 40, '--delay', 25, 5), lambda s, c: True)
    out.append(('scroll', scroll))

    def sidebar():
        _, c = b.state()
        places = [v for k, v in c.items() if k.startswith('place:') or k.startswith('heading:')]
        x = min(p[0] for p in places)
        y = min(p[1] for p in places)
        w = max(p[2] for p in places)
        h = max(p[1] + p[3] for p in places) - y
        return b.hover((x, y, w, h), rows=8)
    out.append(('hover-sidebar', sidebar))

    def commands():
        _, c = b.state()
        cmds = [v for k, v in c.items() if k.startswith('command:')]
        x = min(p[0] for p in cmds)
        return b.hover((x, cmds[0][1], max(p[0] + p[2] for p in cmds) - x, cmds[0][3]), rows=2)
    out.append(('hover-commands', commands))

    def home():
        b.measure(lambda: b.xdo('key', 'alt+Home'), lambda s, c: s.get('location') == 'liria:home')
        _, c = b.state()
        cards = [v for k, v in c.items() if k.startswith('card:')]
        x = min(p[0] for p in cards)
        y = min(p[1] for p in cards)
        return b.hover((x, y, max(p[0] + p[2] for p in cards) - x, min(400, max(p[1] + p[3] for p in cards) - y)))
    out.append(('hover-home', home))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--liria', default=str(ROOT / 'build/src/liria/liria'))
    ap.add_argument('--display', type=int, default=241)
    ap.add_argument('--size', default='1366x768')
    ap.add_argument('--scale', type=int, default=100)
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--json')
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if not (shutil.which('Xvfb') and shutil.which('xdotool') and shutil.which('xwininfo') and
            os.access(a.liria, os.X_OK)):
        print('Xvfb, xdotool, xwininfo and Liria are needed; skipped')
        return 77
    only = {o.strip() for o in a.only.split(',') if o.strip()}
    runs = {}
    with tempfile.TemporaryDirectory(prefix='liria-perf-') as temp:
        b = Bench(a, temp)
        try:
            # Once without measuring: the caches of a computer in use.
            b.start()
            b.stop()
            for _ in range(a.repeat):
                runs.setdefault('startup', []).append(b.start())
                for name, fn in scenarios(b):
                    if only and name not in only:
                        continue
                    runs.setdefault(name, []).append(fn())
                b.stop()
        finally:
            b.close()
    results = {}
    for name, rs in runs.items():
        results[name] = {k: statistics.median(r[k] for r in rs) for k in rs[0]}
        print('%-15s ' % name + '  '.join('%s %.1f' % (k, v) for k, v in results[name].items()), flush=True)
    if a.json:
        Path(a.json).write_text(json.dumps(results, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
