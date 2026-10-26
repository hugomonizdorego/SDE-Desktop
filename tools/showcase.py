#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""SDE showcase: screenshots of everything, for a development update.

Starts SDE in a private Xvfb session - window manager, compositor, panel,
desktop, notifications and the QA fixtures (a fake NetworkManager, battery,
USB stick, music player and Bluetooth adapter, weather, time zone and a
fixed clock) - with a sample home folder for "Alex Tester"
(tools/showcase_content.py), and captures a consistent, captioned set of
screenshots:

  OUTPUT/<set>/<NN>-<name>.png   graphite-light, graphite-dark, the theme
                                 sets (fluent/glass light/dark) and a
                                 1920x1080 at 125 % hero set
  OUTPUT/index.json              file, caption, set, theme, size, component
  OUTPUT/contact-<set>.jpg       one labelled contact sheet per set
  OUTPUT/README.md               the gallery, grouped by component
  --zip FILE                     everything above in one shareable zip

Runs against the installed system (default: /usr/bin and /usr/share/sde, as
CI does after installing the packages) or a build tree:

  tools/showcase.py --build-dir build --data-dir data --output-dir OUT
  tools/showcase.py --theme graphite-light --only 'start-menu*,panel-*'
  tools/showcase.py --list

Programs and features that are missing are skipped with a warning, never a
failure: the gallery shows what this build has. The exit status is not zero
only when no session could start at all (or with --strict, when a shot
failed).
"""
import argparse
import ctypes
import ctypes.util
import datetime as dt
import fnmatch
import glob
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess as sp
import sys
import tempfile
import time
import threading
import traceback
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import showcase_content  # noqa: E402

# ================================================================ the sets


class ThemeSet:
    def __init__(self, name, family, mode, size='1366x768', scale=100, scope='full', label=''):
        self.name, self.family, self.mode, self.size, self.scale = name, family, mode, size, scale
        self.scope, self.label = scope, label
        self.w, self.h = map(int, size.split('x'))

    @property
    def theme(self):
        return f'{self.family}-{self.mode}'


SETS = [
    ThemeSet('graphite-light', 'graphite', 'light', label='SDE Graphite Light (default)'),
    ThemeSet('graphite-dark', 'graphite', 'dark', label='SDE Graphite Dark'),
    ThemeSet('fluent-light', 'fluent', 'light', scope='themes', label='SDE Fluent Light'),
    ThemeSet('fluent-dark', 'fluent', 'dark', scope='themes', label='SDE Fluent Dark'),
    ThemeSet('glass-light', 'glass', 'light', scope='themes', label='SDE Liquid Glass Light'),
    ThemeSet('glass-dark', 'glass', 'dark', scope='themes', label='SDE Liquid Glass Dark'),
    ThemeSet('hero-1920x1080', 'graphite', 'light', '1920x1080', 125, scope='hero',
             label='SDE Graphite Light, 1920x1080 at 125 %'),
]

# ============================================================ the catalogue

SCENES = []


class Scene:
    def __init__(self, fn, component, images, scopes, needs):
        self.fn, self.component, self.images, self.scopes, self.needs = fn, component, images, scopes, needs
        self.name = fn.__name__.replace('_', '-')


def scene(component, images, scopes=('full',), needs=()):
    """Registers a scene: a function that captures |images| (a list of
    (name, caption), or a function of the run context that returns one).
    |needs| are programs that must exist, or the scene is skipped."""
    def deco(fn):
        SCENES.append(Scene(fn, component, images, tuple(scopes), tuple(needs)))
        return fn
    return deco


class SkipShot(Exception):
    """A shot (or scene) that this build cannot show."""


class Run:
    """What every session of one run shares: options, numbering, index."""

    def __init__(self, args):
        self.args = args
        self.out = args.output_dir.resolve()
        self.index = []
        self.skipped = []   # (set, image or scene, reason)
        self.failed = []    # (set, scene, error)
        self.pages = []
        self.numbers = {}
        self.catalogue = []  # (scene, name, caption)
        self.sets = []       # the selected sets (selected_sets())
        self.stop = threading.Event()  # set: the sessions end after their scene

    def number(self):
        """Numbers every image of the catalogue (independent of filters and
        skips), so the same view has the same file name in every set."""
        for sc in SCENES:
            images = sc.images(self) if callable(sc.images) else sc.images
            for name, caption in images:
                self.catalogue.append((sc, name, caption))
        width = 2 if len(self.catalogue) < 100 else 3
        for i, (sc, name, caption) in enumerate(self.catalogue, 1):
            self.numbers[name] = f'{i:0{width}d}-{name}'

    def images_of(self, sc):
        return [(n, c) for s, n, c in self.catalogue if s is sc]

    def selected(self, sc, name=None):
        only = [p.strip() for p in (self.args.only or '').split(',') if p.strip()]
        if not only:
            return True
        keys = [sc.name, sc.component.lower().replace(' ', '-')] + ([name] if name else
                                                                     [n for n, _ in self.images_of(sc)])
        return any(fnmatch.fnmatch(k, p) for p in only for k in keys)


def warn(text):
    print(f'showcase: warning: {text}', flush=True)


def info(text):
    print(f'showcase: {text}', flush=True)

# ============================================================ X helpers


class XGrab:
    """Screen grabs with XGetImage on the root window through libX11
    (ctypes): what is on the screen, composited windows included. xwd
    reads windows of other visuals (GTK's ARGB windows) one by one and
    returns black or grey areas then. None when libX11 cannot be used."""

    class XImage(ctypes.Structure):
        _fields_ = [('width', ctypes.c_int), ('height', ctypes.c_int), ('xoffset', ctypes.c_int),
                    ('format', ctypes.c_int), ('data', ctypes.c_void_p), ('byte_order', ctypes.c_int),
                    ('bitmap_unit', ctypes.c_int), ('bitmap_bit_order', ctypes.c_int),
                    ('bitmap_pad', ctypes.c_int), ('depth', ctypes.c_int), ('bytes_per_line', ctypes.c_int),
                    ('bits_per_pixel', ctypes.c_int), ('red_mask', ctypes.c_ulong),
                    ('green_mask', ctypes.c_ulong), ('blue_mask', ctypes.c_ulong), ('obdata', ctypes.c_void_p),
                    ('funcs', ctypes.c_void_p * 6)]

    def __init__(self, display):
        self.x = ctypes.CDLL(ctypes.util.find_library('X11') or 'libX11.so.6')
        self.x.XOpenDisplay.restype = ctypes.c_void_p
        self.x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x.XDefaultRootWindow.restype = ctypes.c_ulong
        self.x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x.XGetImage.restype = ctypes.POINTER(self.XImage)
        self.x.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                     ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
        self.x.XDestroyImage.argtypes = [ctypes.POINTER(self.XImage)]
        self.x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.x.XDisplayWidth.argtypes = self.x.XDisplayHeight.argtypes = [ctypes.c_void_p, ctypes.c_int]
        # Several sessions may grab at once (--jobs), each with its own
        # connection; libX11 1.8 does this itself, older ones need it.
        self.x.XInitThreads()
        self.d = self.x.XOpenDisplay(display.encode())
        if not self.d:
            raise RuntimeError(f'cannot open display {display}')
        self.root = self.x.XDefaultRootWindow(self.d)
        self.w, self.h = self.x.XDisplayWidth(self.d, 0), self.x.XDisplayHeight(self.d, 0)

    def grab(self):
        im = self.x.XGetImage(self.d, self.root, 0, 0, self.w, self.h, 0xFFFFFFFF, 2)  # ZPixmap
        if not im:
            raise RuntimeError('XGetImage failed')
        try:
            i = im.contents
            if i.bits_per_pixel != 32 or i.byte_order != 0:
                raise RuntimeError(f'unsupported screen format ({i.bits_per_pixel} bpp)')
            data = ctypes.string_at(i.data, i.bytes_per_line * i.height)
            return i.width, i.height, data, i.bytes_per_line, 4
        finally:
            self.x.XDestroyImage(im)

    class XWindowAttributes(ctypes.Structure):
        _fields_ = [('x', ctypes.c_int), ('y', ctypes.c_int), ('width', ctypes.c_int), ('height', ctypes.c_int),
                    ('border_width', ctypes.c_int), ('depth', ctypes.c_int), ('visual', ctypes.c_void_p),
                    ('root', ctypes.c_ulong), ('klass', ctypes.c_int), ('bit_gravity', ctypes.c_int),
                    ('win_gravity', ctypes.c_int), ('backing_store', ctypes.c_int),
                    ('backing_planes', ctypes.c_ulong), ('backing_pixel', ctypes.c_ulong),
                    ('save_under', ctypes.c_int), ('colormap', ctypes.c_ulong), ('map_installed', ctypes.c_int),
                    ('map_state', ctypes.c_int), ('all_event_masks', ctypes.c_long),
                    ('your_event_mask', ctypes.c_long), ('do_not_propagate_mask', ctypes.c_long),
                    ('override_redirect', ctypes.c_int), ('screen', ctypes.c_void_p)]

    def overlays(self):
        """Mapped override-redirect top-level windows (popups, bubbles):
        {window: (x, y, w, h)}."""
        x = self.x
        x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                                 ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                                 ctypes.POINTER(ctypes.c_uint)]
        x.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(self.XWindowAttributes)]
        x.XFree.argtypes = [ctypes.c_void_p]
        root, parent = ctypes.c_ulong(), ctypes.c_ulong()
        children, n = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        out = {}
        if not x.XQueryTree(self.d, self.root, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(children),
                            ctypes.byref(n)):
            return out
        try:
            for i in range(n.value):
                a = self.XWindowAttributes()
                if x.XGetWindowAttributes(self.d, children[i], ctypes.byref(a)) and a.map_state == 2 and \
                        a.override_redirect:
                    out[children[i]] = (a.x, a.y, a.width, a.height)
        finally:
            if children:
                x.XFree(children)
        return out

    def press(self, keysyms):
        """Presses keys by their keycodes through XTEST (first to last,
        released in reverse): exactly these keys, with no modifier added.
        False when XTEST or a keysym is not available."""
        try:
            t = ctypes.CDLL(ctypes.util.find_library('Xtst') or 'libXtst.so.6')
        except OSError:
            return False
        self.x.XStringToKeysym.restype = ctypes.c_ulong
        self.x.XStringToKeysym.argtypes = [ctypes.c_char_p]
        self.x.XKeysymToKeycode.restype = ctypes.c_ubyte
        self.x.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        t.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        codes = [self.x.XKeysymToKeycode(self.d, self.x.XStringToKeysym(k.encode())) for k in keysyms]
        if not all(codes):
            return False
        for c in codes:
            t.XTestFakeKeyEvent(self.d, c, 1, 0)
        for c in reversed(codes):
            t.XTestFakeKeyEvent(self.d, c, 0, 0)
        self.x.XSync(self.d, 0)
        return True

    def close(self):
        if self.d:
            self.x.XCloseDisplay(self.d)
            self.d = None


def read_xwd(data):
    """(width, height, pixel bytes, stride, bytes per pixel) of an xwd dump
    of a TrueColor screen. Pixels are B, G, R (and X), least significant
    byte first. With windows of other depths on screen (GTK's ARGB
    windows) xwd says 24 bits per pixel but still stores 4 bytes each."""
    f = struct.unpack('>25I', data[:100])
    header, w, h, order, stride, ncolors = f[0], f[4], f[5], f[7], f[12], f[19]
    pb = stride // w if w else 0
    if pb not in (3, 4) or order != 0:
        raise RuntimeError(f'xwd: unsupported image format (bytes per pixel {pb}, byte order {order})')
    off = header + ncolors * 12
    return w, h, data[off:off + stride * h], stride, pb


def rgb_rows(img, box=None):
    w, h, data, stride, pb = img
    x0, y0, x1, y1 = box or (0, 0, w, h)
    rows = []
    for y in range(y0, y1):
        b = data[y * stride + x0 * pb:y * stride + x1 * pb]
        row = bytearray(len(b) // pb * 3)
        row[0::3] = b[2::pb]
        row[1::3] = b[1::pb]
        row[2::3] = b[0::pb]
        rows.append(bytes(row))
    return rows


def region_hash(img, box):
    w, h, data, stride, pb = img
    x0, y0, x1, y1 = box
    m = hashlib.blake2b(digest_size=16)
    for y in range(max(0, y0), min(h, y1)):
        m.update(data[y * stride + max(0, x0) * pb:y * stride + min(w, x1) * pb])
    return m.hexdigest()


def region_spread(img, box):
    """Standard deviation of the luminance of a sample of the region: about
    0 for a blank (one color) window."""
    w, h, data, stride, pb = img
    x0, y0, x1, y1 = max(0, box[0]), max(0, box[1]), min(w, box[2]), min(h, box[3])
    vals = []
    for y in range(y0, y1, max(1, (y1 - y0) // 40)):
        row = data[y * stride:(y + 1) * stride]
        for x in range(x0, x1, max(1, (x1 - x0) // 60)):
            i = x * pb
            vals.append(0.299 * row[i + 2] + 0.587 * row[i + 1] + 0.114 * row[i])
    if len(vals) < 2:
        return 0.0
    mean = sum(vals) / len(vals)
    return (sum((v - mean) ** 2 for v in vals) / len(vals)) ** 0.5

# =============================================================== a session


POPUPS = ['SDE Menu', 'SDE Search', 'Action Center', 'Notifications', 'SDE Preview', 'Sound', 'Network',
          'Bluetooth', 'Power', 'Media', 'Drives', 'Keyboard', 'Touchpad', 'Tray', 'Alarms']


class Session:
    """One private SDE session (one theme set)."""

    def __init__(self, run, tset, display):
        self.run_ctx, self.set, self.display = run, tset, display
        self.args = run.args
        self.W, self.H, self.scale = tset.w, tset.h, tset.scale / 100
        self.tmp = Path(tempfile.mkdtemp(prefix='sde-showcase-'))
        self.home = self.tmp / showcase_content.USER_LOGIN  # Liria shows its name
        self.logs = self.tmp / 'logs'
        self.logs.mkdir(parents=True)
        self.procs = []      # (Popen, name) of the session
        self.shot_procs = []  # processes of the current scene
        self.out = run.out / tset.name
        self.out.mkdir(parents=True, exist_ok=True)
        self.baseline = set()
        self.notifications = []
        # "Today" where the session is (TZ below): the agenda, the clock.
        r = sp.run(['date', '+%F'], env=dict(os.environ, TZ=self.timezone()), stdout=sp.PIPE, text=True)
        try:
            self.date = dt.date.fromisoformat(r.stdout.strip())
        except ValueError:
            self.date = dt.date.today()
        self.skipped_names = set()
        self.captured = []
        self.current = None
        self.env = {}
        self.xgrab = None

    # ------------------------------------------------------------ programs

    def bin_dirs(self):
        return [Path(d).resolve() for d in self.args.bin_dirs]

    def which(self, name):
        for d in [self.home / 'bin'] + self.bin_dirs():
            p = Path(d) / name
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)
        return shutil.which(name, path=self.env.get('PATH') or os.environ.get('PATH'))

    def test_tool(self, name):
        for d in self.args.test_dirs:
            p = Path(d) / name
            if p.is_file() and os.access(p, os.X_OK):
                return str(p.resolve())
        return None

    def has(self, name):
        return self.which(name) is not None

    def run(self, *cmd, check=False, timeout=30, input=None):
        try:
            return sp.run([str(c) for c in cmd], env=self.env, check=check, stdout=sp.PIPE, stderr=sp.PIPE,
                          text=True, timeout=timeout, input=input)
        except sp.TimeoutExpired:
            return sp.CompletedProcess(cmd, 124, '', 'timeout')

    def spawn(self, *cmd, name=None, scene=True, env=None):
        """Starts a program in its own process group (killed with the
        scene, or with the session for scene=False)."""
        cmd = [str(c) for c in cmd]
        name = name or Path(cmd[0]).name
        log = open(self.logs / (name + '.log'), 'a')
        log.write(f'$ {" ".join(cmd)}\n')
        log.flush()
        p = sp.Popen(cmd, env=env or self.env, stdout=log, stderr=sp.STDOUT, start_new_session=True,
                     stdin=sp.DEVNULL)
        (self.shot_procs if scene else self.procs).append((p, name))
        return p

    @staticmethod
    def stop(p, grace=2.0):
        if p.poll() is not None:
            return
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            p.wait(timeout=grace)
        except sp.TimeoutExpired:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            try:
                p.wait(timeout=2)
            except sp.TimeoutExpired:
                pass

    # ------------------------------------------------------------- windows

    def xdo(self, *args, check=False):
        return self.run('xdotool', *args, check=check)

    def visible(self, pattern):
        r = self.xdo('search', '--onlyvisible', '--name', pattern)
        ids = r.stdout.split() if r.returncode == 0 else []
        return ids[-1] if ids else None

    def visible_all(self, pattern='.'):
        r = self.xdo('search', '--onlyvisible', '--name', pattern)
        return r.stdout.split() if r.returncode == 0 else []

    def title(self, wid):
        return self.xdo('getwindowname', wid).stdout.strip()

    def wait(self, fn, timeout=10.0, interval=0.08):
        end = time.time() + timeout
        while True:
            v = fn()
            if v:
                return v
            if time.time() > end:
                return None
            time.sleep(interval)

    def find(self, pattern, timeout=10.0):
        """A visible window whose title matches |pattern| (regex), or None."""
        return self.wait(lambda: self.visible(pattern), timeout)

    def need(self, pattern, timeout=10.0, what=None):
        w = self.find(pattern, timeout)
        if not w:
            raise SkipShot(f'{what or pattern} did not appear (windows: {self.window_titles()})')
        return w

    def gone(self, pattern, timeout=5.0):
        return self.wait(lambda: not self.visible(pattern), timeout)

    def window_titles(self):
        return sorted({self.title(w) for w in self.visible_all()} - {''})

    def geom(self, wid):
        text = self.run('xwininfo', '-id', wid).stdout

        def field(label):
            m = re.search(re.escape(label) + r'\s*:\s*(-?\d+)', text)
            return int(m.group(1)) if m else 0
        return {'x': field('Absolute upper-left X'), 'y': field('Absolute upper-left Y'),
                'w': field('Width'), 'h': field('Height'), 'mapped': 'IsViewable' in text}

    def frame_box(self, wid):
        """The window with its frame and shadow margin, clipped to the screen."""
        g = self.geom(wid)
        m = round(40 * self.scale)
        return (max(0, g['x'] - m), max(0, g['y'] - m), min(self.W, g['x'] + g['w'] + m),
                min(self.H, g['y'] + g['h'] + m))

    def activate(self, wid):
        self.xdo('windowactivate', '--sync', wid)
        time.sleep(0.15)

    def key(self, *keys, wid=None):
        args = ['key', '--delay', '40'] + (['--window', wid] if wid else []) + list(keys)
        self.xdo(*args)
        time.sleep(0.15)

    def raw_key(self, combo):
        """A key combination ("F1", "shift+F10") by keycodes through XTEST.
        With the keymaps of Debian testing, "xdotool key F1" sends Alt+F1
        (and Alt+Shift+F10): the window manager takes Alt+F1 for the Start
        Menu, and Liria opens no context menu."""
        mods = {'shift': 'Shift_L', 'ctrl': 'Control_L', 'alt': 'Alt_L', 'super': 'Super_L'}
        keysyms = [mods.get(k.lower(), k) for k in combo.split('+')]
        x = self.x11()
        if not x or not x.press(keysyms):
            self.key(combo)
            return
        time.sleep(0.15)

    def type(self, text, delay=25):
        self.xdo('type', '--delay', str(delay), text)
        time.sleep(0.2)

    def move(self, x, y):
        self.xdo('mousemove', str(int(x)), str(int(y)))

    def click(self, x, y, button=1):
        self.xdo('mousemove', str(int(x)), str(int(y)), 'click', str(button))
        time.sleep(0.25)

    def click_in(self, wid, x, y, button=1, scaled=True):
        g = self.geom(wid)
        k = self.scale if scaled else 1
        self.click(g['x'] + x * k, g['y'] + y * k, button)

    def park_pointer(self):
        """Moves the pointer where it hovers nothing: the top edge, right of
        the desktop icons (popups and centered windows stay below it)."""
        self.move(self.W * 0.55, 2)
        time.sleep(0.1)

    def frame_extents(self, wid):
        m = re.search(r'=\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)',
                      self.run('xprop', '-id', wid, '_NET_FRAME_EXTENTS').stdout)
        return tuple(int(v) for v in m.groups()) if m else (0, 0, 0, 0)

    def work_area(self):
        m = re.search(r'=\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)', self.run('xprop', '-root', '_NET_WORKAREA').stdout)
        return tuple(int(v) for v in m.groups()) if m else (0, 0, self.W, self.H)

    def fit(self, wid):
        """Moves a window so that its frame (title bar included) is on the
        screen above the panel. SDE programs center the window itself in
        the work area, so a tall window's title bar starts above the screen
        (static gravity); a user would move it the same way."""
        time.sleep(0.2)
        g = self.geom(wid)
        left, right, top, bottom = self.frame_extents(wid)
        wx, wy, ww, wh = self.work_area()
        fx, fy, fw, fh = g['x'] - left, g['y'] - top, g['w'] + left + right, g['h'] + top + bottom
        nx = min(max(fx, wx), max(wx, wx + ww - fw))
        ny = min(max(fy, wy), max(wy, wy + wh - fh))
        if (nx, ny) != (fx, fy):
            self.xdo('windowmove', wid, str(nx + left), str(ny + top))
            self.wait(lambda: (self.geom(wid)['x'], self.geom(wid)['y']) == (nx + left, ny + top), 2)
        return wid

    def app(self, cmd, title, timeout=15.0, fit=True, settle=True):
        """Starts a program of the scene and waits for its window (title
        regex), fitted on the screen and painted."""
        exe = self.which(cmd[0])
        if not exe:
            raise SkipShot(f'{cmd[0]} is not installed')
        p = self.spawn(*cmd)
        w = self.find(title, timeout)
        if not w:
            # Once more, as a user would: the program ended without a window
            # (its exit status is in the log) or its window never showed.
            code = p.poll()
            warn(f'{self.set.name}: no {cmd[0]} window after {timeout:.0f} s ' +
                 (f'(it exited with status {code})' if code is not None else '(still running)') +
                 '; starting it again')
            self.stop(p, 1)
            p = self.spawn(*cmd)
            w = self.need(title, timeout, what=f'the {cmd[0]} window')
        if fit:
            self.fit(w)
        self.activate(w)
        if settle:
            self.window_settle(w, quiet=0.5, timeout=8)
        return w

    def size(self, wid, w, h):
        """Resizes a window (unscaled size, at most the work area) and
        centers it in the work area."""
        left, right, top, bottom = self.frame_extents(wid)
        wx, wy, ww, wh = self.work_area()
        w = min(round(w * self.scale), ww - left - right)
        h = min(round(h * self.scale), wh - top - bottom)
        self.xdo('windowsize', wid, str(w), str(h))
        self.wait(lambda: (self.geom(wid)['w'], self.geom(wid)['h']) == (w, h), 2)
        x = wx + (ww - w - left - right) // 2 + left
        y = wy + (wh - h - top - bottom) // 2 + top
        self.xdo('windowmove', wid, str(x), str(y))
        self.wait(lambda: (self.geom(wid)['x'], self.geom(wid)['y']) == (x, y), 2)
        self.window_settle(wid, quiet=0.4, timeout=4)

    def stop_scene_procs(self):
        """Ends the programs the scene started so far."""
        for p, _ in reversed(self.shot_procs):
            self.stop(p, 1.5)
        self.shot_procs = []
        time.sleep(0.2)

    def kill_windows(self, pattern):
        """Ends the programs that own windows matching |pattern| (programs
        the panel started for the scene)."""
        session = self.session_pids()
        for w in self.visible_all(pattern):
            pid = self.run('xdotool', 'getwindowpid', w).stdout.strip()
            if pid.isdigit() and int(pid) not in session:
                try:
                    os.kill(int(pid), signal.SIGTERM)
                except (ProcessLookupError, PermissionError):
                    pass
        self.gone(pattern, 3)

    def box_of(self, wid):
        """(x0, y0, x1, y1) of a window on the screen."""
        g = self.geom(wid)
        return (max(0, g['x']), max(0, g['y']), min(self.W, g['x'] + g['w']), min(self.H, g['y'] + g['h']))

    def hash_box(self, box):
        return region_hash(self.grab(), box)

    def skip(self, name, reason):
        if self.run_ctx.selected(self.current, name):
            self.run_ctx.skipped.append((self.set.name, name, reason))
            self.skipped_names.add(name)
            warn(f'{self.set.name}: {name} skipped: {reason}')

    def fixture_mode(self, mode):
        """Switches the panel service fixture (sde-panel-service-fixture)."""
        r = self.run('dbus-send', '--session', '--type=method_call', '--print-reply', '--dest=org.bluez',
                     '/SDETest', 'org.sde.Test.Mode', 'string:' + mode, timeout=5)
        time.sleep(0.5)
        return r.returncode == 0

    # ---------------------------------------------------------- QA layouts

    def layout(self, title):
        """Named controls of a panel window: name -> (x, y, w, h, active, scrolled)."""
        try:
            text = (self.home / 'qa' / (title.replace(' ', '-') + '.txt')).read_text()
        except FileNotFoundError:
            return {}
        items = {}
        for line in text.splitlines():
            f = line.split('\t')
            if len(f) >= 6:
                try:
                    items.setdefault(f[0], tuple(int(v) for v in f[1:6]) + ((int(f[6]) if len(f) > 6 else 0),))
                except ValueError:
                    pass
        return items

    def control(self, title, name, timeout=5.0, active=False):
        def get():
            r = self.layout(title).get(name)
            return r if r and (not active or r[4]) else None
        return self.wait(get, timeout)

    def press(self, title, name, fx=0.5, fy=0.5, button=1, timeout=5.0):
        """Clicks a named control of a panel window (QA layout)."""
        win = self.find('^' + re.escape(title) + '$', timeout)
        r = self.control(title, name, timeout)
        if not win or not r:
            raise SkipShot(f'{title}: no control {name!r}')
        g = self.geom(win)
        self.click(g['x'] + r[0] + r[2] * fx, g['y'] + r[1] + r[3] * fy, button)
        return r

    def panel_button(self, name):
        panel = self.visible('^SDE Panel$')
        r = self.control('SDE Panel', name, 3)
        if not panel or not r:
            return None
        g = self.geom(panel)
        return g['x'] + r[0] + r[2] // 2, g['y'] + r[1] + r[3] // 2

    def click_panel(self, name, expect=None, button=1):
        for _ in range(2):
            pos = self.panel_button(name)
            if not pos:
                raise SkipShot(f'no panel button {name!r}')
            self.click(*pos, button=button)
            if not expect:
                return None
            w = self.find(expect, 3)
            if w:
                return w
        raise SkipShot(f'{name!r} did not open {expect}')

    def panel_command(self, args, title, timeout=4.0):
        """sde-panel --menu / --action-center / --section NAME ... opens |title|."""
        for _ in range(3):
            w = self.visible('^' + re.escape(title) + '$')
            if w:
                return w
            self.run('sde-panel', *args)
            w = self.find('^' + re.escape(title) + '$', timeout)
            if w:
                return w
        raise SkipShot(f'sde-panel {" ".join(args)} did not open {title}')

    def menu_seq(self):
        return next((int(k[5:]) for k in self.layout('context-menu') if k.startswith('@seq:')), 0)

    def right_click_at(self, x, y):
        seq = self.menu_seq()
        self.click(x, y, 3)
        if not self.wait(lambda: self.menu_seq() > seq, 3):
            raise SkipShot('no context menu')
        time.sleep(0.3)
        return self.layout('context-menu')

    # ----------------------------------------------------------- settling

    def x11(self):
        """The showcase's own libX11 connection (XGrab), or None."""
        if self.xgrab is None:
            try:
                self.xgrab = XGrab(self.env['DISPLAY'])
            except (OSError, RuntimeError, AttributeError) as e:
                warn(f'libX11 screen grabs unavailable ({e}); using ImageMagick import')
                self.xgrab = False
        return self.xgrab or None

    def overlays(self):
        """Mapped override-redirect windows {id: (x, y, w, h)} (the bubbles
        of sde-notifyd have no name: this finds them)."""
        x = self.x11()
        return x.overlays() if x else {}

    def grab(self):
        """The screen: (width, height, BGRX bytes, stride, bytes per pixel)."""
        if self.x11():
            return self.xgrab.grab()
        if shutil.which('import'):
            r = sp.run(['import', '-window', 'root', '-depth', '8', 'xwd:-'], env=self.env, stdout=sp.PIPE,
                       stderr=sp.DEVNULL, timeout=30)
            if r.returncode == 0 and r.stdout:
                return read_xwd(r.stdout)
        r = sp.run(['xwd', '-root', '-silent'], env=self.env, stdout=sp.PIPE, stderr=sp.DEVNULL, timeout=20)
        return read_xwd(r.stdout)

    def settle(self, box=None, quiet=0.5, timeout=6.0, painted=True):
        """Waits until |box| (default: the screen) stops changing for
        |quiet| seconds and, with |painted|, is not one plain color."""
        box = box or (0, 0, self.W, self.H)
        end = time.time() + timeout
        last, since = None, time.time()
        img = None
        while time.time() < end:
            img = self.grab()
            h = region_hash(img, box)
            if h != last:
                last, since = h, time.time()
            elif time.time() - since >= quiet and (not painted or region_spread(img, box) > 1.5):
                return img
            time.sleep(0.12)
        return img

    def window_settle(self, wid, quiet=0.5, timeout=6.0):
        g = self.geom(wid)
        box = (max(0, g['x']), max(0, g['y']), min(self.W, g['x'] + g['w']), min(self.H, g['y'] + g['h']))
        return self.settle(box, quiet, timeout)

    # ------------------------------------------------------------ capture

    def shot(self, name, settle_box=None, quiet=0.5, timeout=6.0, wid=None):
        """Captures image |name| (declared by the running scene) when the
        screen (or |wid|, or |settle_box|) has settled. Xvfb draws no
        pointer; park_pointer() keeps it from hovering anything."""
        if name not in self.run_ctx.numbers:
            raise RuntimeError(f'undeclared image {name!r}')
        if not self.run_ctx.selected(self.current, name):
            return None
        if wid:
            g = self.geom(wid)
            settle_box = (max(0, g['x']), max(0, g['y']), min(self.W, g['x'] + g['w']),
                          min(self.H, g['y'] + g['h']))
        self.close_bubbles(keep=self.keep_bubbles)
        img = self.settle(settle_box, quiet, timeout)
        if settle_box and region_spread(img, settle_box) <= 1.5:
            warn(f'{self.set.name}/{name}: the window looks blank')
        file = self.out / (self.run_ctx.numbers[name] + '.png')
        file.write_bytes(showcase_content.png_bytes(img[0], img[1], rgb_rows(img)))
        # The same view under an older number (another build's catalogue).
        for old in self.out.glob(f'*-{name}.png'):
            if old != file and re.fullmatch(r'\d+-' + re.escape(name) + r'\.png', old.name):
                old.unlink()
        caption = dict(self.run_ctx.images_of(self.current)).get(name, name)
        self.run_ctx.index.append({
            'file': f'{self.set.name}/{file.name}', 'name': name, 'caption': caption,
            'component': self.current.component, 'set': self.set.name, 'theme': self.set.theme,
            'label': self.set.label, 'size': self.set.size, 'scale': self.set.scale})
        self.captured.append(name)
        info(f'{self.set.name}: {file.name}')
        return file

    def save_debug(self, name):
        """The screen as it is, into the logs (for a failed scene)."""
        img = self.grab()
        (self.logs / (name + '.png')).write_bytes(showcase_content.png_bytes(img[0], img[1], rgb_rows(img)))

    # -------------------------------------------------------- notifications

    keep_bubbles = False

    def notify(self, app, summary, body='', icon=None, urgency=None):
        args = ['notify-send', '-p', '-a', app] + (['-i', icon] if icon else []) + \
            (['-u', urgency] if urgency else []) + [summary, body]
        r = self.run(*args)
        try:
            self.notifications.append(int(r.stdout.strip().split()[-1]))
        except (ValueError, IndexError):
            pass

    def close_bubbles(self, keep=False):
        """Closes notification bubbles, which would show in later shots, and
        empties the history (no unread badge). The bubbles are unnamed
        override-redirect windows: sde-notifyd's history tells instead."""
        if keep or not self.notification_ids():
            return
        self.clear_notifications()
        time.sleep(0.3)

    def notification_ids(self):
        """Ids in sde-notifyd's history (org.sde.Notifications.GetHistory)."""
        r = self.run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
                     '/org/freedesktop/Notifications', '--method', 'org.sde.Notifications.GetHistory', timeout=5)
        return [int(m) for m in re.findall(r'\(uint32 (\d+),', r.stdout)]

    def close_notification(self, nid):
        self.run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
                 '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.CloseNotification',
                 str(nid), timeout=5)

    def clear_notifications(self):
        self.run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
                 '/org/freedesktop/Notifications', '--method', 'org.sde.Notifications.Clear', timeout=5)

    # ------------------------------------------------------------ cleanup

    def dismiss_popups(self):
        """Closes the panel's popups (Escape), so none shows in a later shot."""
        pattern = '^(' + '|'.join(POPUPS) + ')$'
        for _ in range(4):
            if not self.visible_all(pattern):
                return
            self.key('Escape')
            time.sleep(0.25)
        for w in self.visible_all(pattern):
            warn(f'{self.title(w)} stayed open; unmapping it')
            self.xdo('windowunmap', w)

    def clean(self):
        """Closes everything the scene opened."""
        self.dismiss_popups()
        for p, name in reversed(self.shot_procs):
            self.stop(p, 1.5)
        self.shot_procs = []
        session = self.session_pids()
        for w in self.visible_all():
            if w in self.baseline:
                continue
            title = self.title(w)
            pid = self.run('xdotool', 'getwindowpid', w).stdout.strip()
            klass = self.run('xprop', '-id', w, 'WM_CLASS').stdout.lower()
            if pid.isdigit() and int(pid) not in session:
                # Started by the panel or another program of the session.
                try:
                    os.kill(int(pid), signal.SIGTERM)
                except (ProcessLookupError, PermissionError):
                    pass
            elif (pid.isdigit() and int(pid) in session) or any(
                    c in klass for c in ('sde-panel', 'sde-desktop', 'sde-notifyd', 'sde panel')):
                self.xdo('windowunmap', w)
            elif title:
                warn(f'closing a window of an unknown program: {title!r}')
                self.xdo('windowkill', w)
        self.wait(lambda: not [w for w in self.visible_all() if w not in self.baseline], 3)
        self.close_bubbles()
        self.park_pointer()

    def session_pids(self):
        return {p.pid for p, _ in self.procs}

    def private_pids(self):
        """Processes whose HOME is this session's private home (started by
        the session; nothing else has that temporary path)."""
        want = b'HOME=' + str(self.home).encode()
        mine = {os.getpid(), os.getppid()}
        out = []
        for d in os.listdir('/proc'):
            if not d.isdigit() or int(d) in mine:
                continue
            try:
                with open(f'/proc/{d}/environ', 'rb') as f:
                    if want in f.read().split(b'\0'):
                        out.append(int(d))
            except OSError:
                pass
        return out

    # -------------------------------------------------------- start / stop

    def base_config(self):
        t = self.set
        return {
            'appearance': {'style': t.family, 'theme': t.mode, 'scale': str(t.scale), 'icon_theme': 'auto',
                           'wallpaper': self.wallpaper() or 'auto', 'wallpaper_mode': 'zoom'},
            'panel': {'pinned': 'liria.desktop;niny.desktop;sde-gallery.desktop;xanara.desktop;sde-settings.desktop',
                      'search': 'auto',
                      # A steady LED and battery icon (its colours and marks
                      # stay): the screenshots wait until the screen stops changing.
                      'network_led_blink': 'false', 'battery_animation': 'false'},
            # 0.2 Alpha 2: tile groups by category, as SDE makes them from
            # the installed applications (fixed here so that every run shows
            # the same tiles).
            'menu': {'tiles': 'Office,xanara.desktop:w,niny.desktop:m;Graphics,sde-gallery.desktop:w,'
                              'sde-view.desktop:m;System & Settings,liria.desktop:m,sde-settings.desktop:m,'
                              'sde-system-monitor.desktop:m,sde-update.desktop:w,sde-display-settings.desktop:m;'
                              'Accessories,org.debion.Calculator.desktop:m,taca.desktop:m,sde-run.desktop:m',
                     'tiles_version': '2'},
            'action_center': {'multicolor': 'true', 'expanded': 'true'},
            'desktop': {'show_home': 'true', 'show_trash': 'true'},
            'weather': {'animation': 'false'},
            'update': {'auto_refresh': 'false', 'check_interval': '0', 'notify': 'true'},
            'welcome': {'show': 'false'},
            'session': {'polkit_agent': 'none'},
        }

    def write_config(self, overrides=None, reload=False):
        """Writes sde.conf: the showcase defaults with |overrides|
        ({section: {key: value}}), and with |reload| tells the session."""
        cfg = self.base_config()
        for section, values in (overrides or {}).items():
            cfg.setdefault(section, {}).update(values)
        text = ''.join(f'[{s}]\n' + ''.join(f'{k}={v}\n' for k, v in kv.items()) + '\n' for s, kv in cfg.items())
        path = self.home / '.config/sde/sde.conf'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if reload:
            self.reload()

    def wallpaper(self):
        name = f'selviana-{self.set.mode}.svg'
        for c in [self.args.data_dir.parent / 'backgrounds/sde' / name, self.args.data_dir / 'backgrounds' / name,
                  Path('/usr/share/backgrounds/sde') / name]:
            if c.is_file():
                return str(c.resolve())
        return None

    def reload(self):
        """Tells the panel and the desktop that sde.conf changed."""
        self.run('xprop', '-root', '-f', '_SDE_CONFIG_SERIAL', '32c', '-set', '_SDE_CONFIG_SERIAL',
                 str(time.time_ns() % 1000000000))
        time.sleep(0.8)

    def set_clock(self, when=None):
        """Pins the clock of the panel and the Action Center to |when|
        ("HH:MM" today, SDE_CLOCK_FIXTURE), or with None lets it show the
        real time like every other program. Writing the same time with
        seconds restarts it from there."""
        path = self.home / 'clock-fixture'
        if when is None:
            text = ''
        else:
            self._clock_toggle = not getattr(self, '_clock_toggle', False)
            text = f'{self.date.isoformat()}T{when}' + (':00' if self._clock_toggle else '') + '\n'
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
            time.sleep(0.4)  # the panel reads it every 300 ms

    def make_env(self):
        a = self.args
        bins = [str(d) for d in self.bin_dirs()]
        data_dirs = [str(d) for d in a.xdg_data_dirs]
        data_dirs += os.environ.get('XDG_DATA_DIRS', '/usr/local/share:/usr/share').split(':')
        env = dict(os.environ)
        for k in ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_SYSTEM_BUS_ADDRESS', 'WAYLAND_DISPLAY', 'GTK_THEME',
                  'XDG_SESSION_ID', 'XDG_SEAT', 'XDG_SEAT_PATH', 'SESSION_MANAGER', 'XAUTHORITY',
                  'GSETTINGS_SCHEMA_DIR', 'LD_PRELOAD'):
            env.pop(k, None)
        env.update(
            PATH=':'.join([str(self.home / 'bin')] + bins + [str(Path(p) / 'bin') for p in a.prefixes] +
                          [os.environ.get('PATH', '/usr/bin:/bin')]),
            DISPLAY=f':{self.display}',
            HOME=str(self.home), USER=showcase_content.USER_LOGIN, LOGNAME=showcase_content.USER_LOGIN,
            XDG_CONFIG_HOME=str(self.home / '.config'), XDG_CONFIG_DIRS=str(self.home / '.system-config'),
            XDG_DATA_HOME=str(self.home / '.local/share'), XDG_STATE_HOME=str(self.home / '.local/state'),
            XDG_CACHE_HOME=str(self.home / '.cache'), XDG_RUNTIME_DIR=str(self.tmp / 'run'),
            XDG_DATA_DIRS=':'.join(dict.fromkeys(data_dirs)),
            XDG_CURRENT_DESKTOP='SDE-Desktop:SDE', XDG_SESSION_DESKTOP='SDE-Desktop', XDG_SESSION_TYPE='x11',
            DESKTOP_SESSION='sde-desktop',
            LANG='C.UTF-8', LC_ALL='C.UTF-8', TZ=self.timezone(), NO_AT_BRIDGE='1', GSK_RENDERER='cairo',
            GTK_A11Y='none',
            SDE_PANEL_QA_DIR=str(self.home / 'qa'),
            SDE_WEATHER_FIXTURE='partly', SDE_TZ_FIXTURE='Asia/Jakarta',
            SDE_CLOCK_FIXTURE='@' + str(self.home / 'clock-fixture'),
            SDE_POWER_SUPPLY_DIR=str(self.home / '.fixtures/power'),
            SDE_BACKLIGHT_DIR=str(self.home / '.fixtures/backlight'),
            SDE_PANEL_FAKE_TOUCHPAD='1', SDE_DRIVERS_DEMO='1',
            SDE_UPDATE_ROOT=str(self.home / '.fixtures/update-root'),
            # The distribution SDE runs on is named from os-release: DebION.
            SDE_OS_RELEASE=str(self.home / '.fixtures/os-release'),
            # About windows log their sections (Alt+1..4) for the waits.
            SDE_ABOUT_QA_FILE=str(self.home / 'qa/about.log'),
            PYTHONPATH=str(self.home / '.fixtures/python'),
        )
        schemas = [str(Path(p) / 'share/glib-2.0/schemas') for p in a.prefixes
                   if (Path(p) / 'share/glib-2.0/schemas/gschemas.compiled').exists()]
        if schemas:
            env['GSETTINGS_SCHEMA_DIR'] = ':'.join(schemas)
        if a.update_helpers:
            env['SDE_UPDATE_HELPERS'] = str(a.update_helpers)
        # The SDE logo: data/branding in a source tree (installed: sde/branding).
        if (a.data_dir / 'branding/sde-logo.svg').exists():
            env['SDE_BRANDING_DIR'] = str(a.data_dir / 'branding')
        if (self.home / '.fixtures/avatars').is_dir():
            env['SDE_AVATAR_DIR'] = str(self.home / '.fixtures/avatars')
        if a.effects == 'full':
            # Every SDE program draws for the full effects of a real GPU
            # (Xvfb has OpenGL through Mesa's software renderer).
            env['SDE_GRAPHICS'] = 'hardware'
        # SDE Welcome's tiles: data/welcome of a source tree when SDE is not
        # installed (installed: /usr/share/SDE-Welcome).
        if not Path('/usr/share/SDE-Welcome/steps').is_dir() and (a.data_dir / 'welcome/steps').is_dir():
            env['SDE_WELCOME_DIR'] = str(a.data_dir / 'welcome')
        # Merkadu: fake sources with fictional apps (nothing is installed or
        # downloaded), a change that holds at 42 % and a still logo.
        if (MERKADU_FIXTURES / 'catalog.json').exists():
            env.update(MERKADU_FAKE=str(MERKADU_FIXTURES), MERKADU_CONFIG=str(MERKADU_FIXTURES / 'merkadu.conf'),
                       MERKADU_FAKE_HOLD='42', MERKADU_LOGO_PHASE='1.55')
        if (a.data_dir / 'merkadu/categories.menu').exists():
            env['MERKADU_DATA_DIR'] = str(a.data_dir / 'merkadu')
        env.update(self.nss_wrapper())
        self.env = env

    @staticmethod
    def timezone():
        return 'Asia/Jakarta' if Path('/usr/share/zoneinfo/Asia/Jakarta').exists() else 'WIB-7'

    def nss_wrapper(self):
        """The full name "Alex Tester" for getpwuid() (nss_wrapper, when installed)."""
        libs = sorted(glob.glob('/usr/lib/*/libnss_wrapper.so') + glob.glob('/usr/lib/libnss_wrapper.so') +
                      glob.glob('/usr/lib64/libnss_wrapper.so'))
        if not libs:
            if not getattr(Session, '_nss_warned', False):
                warn('libnss-wrapper is not installed: the account shows the real user name')
                Session._nss_warned = True
            return {}
        nss = self.home / '.fixtures/nss'
        nss.mkdir(parents=True, exist_ok=True)
        uid, gid = os.getuid(), os.getgid()
        (nss / 'passwd').write_text(f'{showcase_content.USER_LOGIN}:x:{uid}:{gid}:{showcase_content.USER_NAME},,,:'
                                    f'{self.home}:/bin/bash\n')
        (nss / 'group').write_text(f'{showcase_content.USER_LOGIN}:x:{gid}:\n')
        (nss / 'hosts').write_text('127.0.0.1 localhost\n127.0.1.1 alex-laptop\n')
        return {'LD_PRELOAD': libs[0], 'NSS_WRAPPER_PASSWD': str(nss / 'passwd'),
                'NSS_WRAPPER_GROUP': str(nss / 'group'), 'NSS_WRAPPER_HOSTS': str(nss / 'hosts')}

    def prepare_home(self):
        a = self.args
        self.home.mkdir(parents=True)
        (self.tmp / 'run').mkdir(mode=0o700)
        for d in ('bin', 'qa', '.fixtures/python', '.system-config'):
            (self.home / d).mkdir(parents=True, exist_ok=True)
        # File times and agenda dates in the session's time zone.
        os.environ['TZ'] = self.timezone()
        time.tzset()
        showcase_content.make_home(self.home, self.date, self.run_ctx.out / '.cache/pictures',
                                   [str(d) for d in a.xdg_data_dirs] + ['/usr/local/share', '/usr/share'])
        # Battery: 78 %, discharging, about 4 h left; a backlight at 70 %.
        bat = self.home / '.fixtures/power/BAT0'
        bat.mkdir(parents=True)
        for k, v in {'type': 'Battery', 'capacity': '78', 'status': 'Discharging', 'energy_now': '46800000',
                     'energy_full': '60000000', 'power_now': '11140000', 'manufacturer': 'Generic',
                     'model_name': 'Li-ion 60 Wh'}.items():
            (bat / k).write_text(v + '\n')
        bl = self.home / '.fixtures/backlight/showcase'
        bl.mkdir(parents=True)
        (bl / 'brightness').write_text('70\n')
        (bl / 'max_brightness').write_text('100\n')
        # Software Updater: fake APT and Flatpak (tools/update_fixture.py),
        # with everyday package names instead of its "example" fixtures.
        import update_fixture
        update_fixture.write(self.home / '.fixtures/python', self.home / 'bin', self.home / '.fixtures/update-root')
        apt = self.home / '.fixtures/python/apt.py'
        text = apt.read_text()
        for old, new in UPDATE_NAMES:
            text = text.replace(old, new)
        apt.write_text(text)
        # DebION GNU/Linux as the distribution (os-release via SDE_OS_RELEASE),
        # with its logo as the LOGO icon (debion-desktop-base, or the source).
        logo = next((p for p in (Path('/usr/share/desktop-base/debion-logos/logo-256.png'),
                                 ROOT / 'data/desktop-base/debion-logos/logo-256.png') if p.is_file()), None)
        if logo:
            icons = self.home / '.local/share/icons/hicolor/256x256/apps'
            icons.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(logo, icons / 'debion-logo.png')
        (self.home / '.fixtures/os-release').write_text(
            'PRETTY_NAME="DebION GNU/Linux"\nNAME="DebION GNU/Linux"\nID=debion\nID_LIKE=debian\n'
            'VERSION_CODENAME=forky\nHOME_URL="https://github.com/hugomonizdorego/SDE-Desktop"\n' +
            ('LOGO=debion-logo\n' if logo else ''))
        # The default applications of SDE (Photo for pictures, Xanara for
        # PDFs...), as the installed /etc/xdg/sde-mimeapps.list gives them.
        for src in (Path('/etc/xdg/sde-mimeapps.list'), ROOT / 'data/sde-mimeapps.list'):
            if src.is_file():
                shutil.copyfile(src, self.home / '.config/sde-mimeapps.list')
                break
        # The stock account pictures from a source tree, as installed (PNG
        # only, /usr/share/Avatar); SDE_AVATAR_DIR below.
        if (a.data_dir / 'avatars').is_dir() and not Path('/usr/share/Avatar').is_dir():
            stock = self.home / '.fixtures/avatars'
            stock.mkdir(parents=True, exist_ok=True)
            for p in sorted((a.data_dir / 'avatars').glob('*.png')):
                shutil.copyfile(p, stock / p.name)
        self.write_stubs()
        if a.themes_dir:
            (self.home / '.local/share').mkdir(parents=True, exist_ok=True)
            link = self.home / '.local/share/themes'
            if not link.exists():
                link.symlink_to(Path(a.themes_dir).resolve())
        # SDE WM configuration (the session makes it from the packaged one).
        rc = a.data_dir / 'openbox/rc.xml'
        dest = self.home / '.config/sde/openbox/rc.xml'
        dest.parent.mkdir(parents=True, exist_ok=True)
        if rc.is_file():
            shutil.copyfile(rc, dest)
        self.write_config()

    def write_stubs(self):
        """Commands of the session that must not touch the real system."""
        b = self.home / 'bin'
        for name, text in STUBS.items():
            (b / name).write_text(text)
            (b / name).chmod(0o755)
        # Openbox from the distribution when SDE WM is not installed.
        if not any((d / 'sde-wm').exists() for d in self.bin_dirs()) and not shutil.which('sde-wm'):
            ob = shutil.which('openbox')
            if ob:
                (b / 'sde-wm').write_text(f'#!/bin/sh\nexec {ob} "$@"\n')
                (b / 'sde-wm').chmod(0o755)

    def start(self):
        t0 = time.time()
        self.prepare_home()
        self.make_env()
        self.set_clock()
        # A private session bus; the fixture's services pose as system
        # services on it (NetworkManager, UDisks2, BlueZ), like panel-smoke.
        bus = sp.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'], env=self.env,
                       stdout=sp.PIPE, stderr=open(self.logs / 'dbus.log', 'w'), text=True, start_new_session=True)
        self.procs.append((bus, 'dbus-daemon'))
        addr = bus.stdout.readline().strip()
        if not addr:
            raise RuntimeError('dbus-daemon did not start')
        self.env['DBUS_SESSION_BUS_ADDRESS'] = addr
        self.env['DBUS_SYSTEM_BUS_ADDRESS'] = addr
        xenv = {k: v for k, v in self.env.items() if not k.startswith('NSS_WRAPPER') and k != 'LD_PRELOAD'}
        xv = self.spawn('Xvfb', f':{self.display}', '-screen', '0', f'{self.set.size}x24', '-nolisten', 'tcp',
                        '-ac', '-noreset', scene=False, env=xenv)
        if not self.wait(lambda: self.xdo('getmouselocation').returncode == 0, 10) or xv.poll() is not None:
            raise RuntimeError(f'Xvfb :{self.display} did not start')
        # GTK settings and the SDE WM theme (sde-settings --apply, like the session).
        r = self.run('sde-settings', '--apply', timeout=20)
        if r.returncode != 0:
            warn(f'sde-settings --apply: {r.stderr.strip()[:200]}')
        xs = self.home / '.config/sde/xsettingsd.conf'
        daemon = 'sde-xsettingsd' if self.has('sde-xsettingsd') else 'xsettingsd' if shutil.which('xsettingsd') else None
        if daemon and xs.exists():
            self.spawn(daemon, '--config', xs, scene=False)
        else:
            warn('no XSETTINGS daemon: GTK programs use their default theme')
        self.spawn('sde-wm', '--config-file', self.home / '.config/sde/openbox/rc.xml', '--sm-disable', scene=False)
        if not self.wait(lambda: '_NET_SUPPORTING_WM_CHECK(WINDOW)' in
                         self.run('xprop', '-root', '_NET_SUPPORTING_WM_CHECK').stdout, 8):
            warn('the window manager did not start')
        self.spawn('sde-notifyd', scene=False)
        self.spawn('sde-desktop', scene=False)
        if not self.find('^SDE Desktop$', 30):
            raise RuntimeError('sde-desktop did not start')
        self.spawn('sde-panel', scene=False)
        if not self.find('^SDE Panel$', 30):
            raise RuntimeError('sde-panel did not start')
        self.start_compositor()
        fixture = self.test_tool('sde-panel-service-fixture')
        if fixture:
            env = dict(self.env, SDE_FIXTURE_NAMES='showcase')
            self.spawn(fixture, self.logs / 'service.log', name='service-fixture', scene=False, env=env)
            if not self.wait(lambda: (self.logs / 'service.log').exists() and
                             'READY' in (self.logs / 'service.log').read_text(errors='replace'), 10):
                warn('the panel service fixture did not start')
            self.sample_account()
        else:
            warn('sde-panel-service-fixture not found (--test-dir): no Bluetooth, drive or music player')
        time.sleep(1.0)
        self.park_pointer()
        self.settle(quiet=0.8, timeout=12)
        self.baseline = set(self.visible_all())
        info(f'{self.set.name}: session on :{self.display} ready in {time.time() - t0:.1f} s')

    def sample_account(self):
        """Alex Tester and ~/.face in the fixture's AccountsService, when the
        fixture has one (the panel, lock screen and Control Center ask it)."""
        call = ['dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.Accounts']
        r = self.run(*call, '/org/freedesktop/Accounts', 'org.freedesktop.Accounts.FindUserByName',
                     f'string:{showcase_content.USER_LOGIN}', timeout=5)
        m = re.search(r'object path "([^"]+)"', r.stdout)
        if r.returncode != 0 or not m:
            return
        user = m.group(1)
        self.run(*call, user, 'org.freedesktop.Accounts.User.SetRealName', f'string:{showcase_content.USER_NAME}',
                 timeout=5)
        if (self.home / '.face').exists():
            self.run(*call, user, 'org.freedesktop.Accounts.User.SetIconFile', f'string:{self.home / ".face"}',
                     timeout=5)

    def start_compositor(self):
        conf = self.args.data_dir / 'compositor/sde.conf'
        prog = 'sde-compositor' if self.has('sde-compositor') else 'picom' if shutil.which('picom') else None
        if self.args.no_compositor:
            return
        if not prog or not conf.is_file():
            warn('no compositor (sde-compositor): no shadows or rounded corners')
            return
        mine = self.home / '.config/sde/showcase-compositor.conf'
        generated = None
        if self.args.effects == 'full':
            # What sde-session starts on a real GPU (OpenGL, blur, shaders).
            r = self.run('sde-session', '--compositor-config', timeout=20)
            path = Path(r.stdout.strip().splitlines()[-1]) if r.returncode == 0 and r.stdout.strip() else None
            if prog != 'sde-compositor':
                warn('--effects full needs sde-compositor; using the lite configuration')
            elif path and path.is_file():
                generated = path
            else:
                warn(f'sde-session --compositor-config failed ({r.stderr.strip()[:200]}); using the lite one')
        if generated:
            mine = generated
        else:
            radius = {'fluent': 8, 'graphite': 10, 'glass': 12}.get(self.set.family, 10)
            text = re.sub(r'corner-radius = \d+;', f'corner-radius = {round(radius * self.scale)};',
                          conf.read_text())
            if self.set.mode == 'dark':
                text = re.sub(r'shadow-opacity = [0-9.]+;', 'shadow-opacity = 0.32;', text)
            mine.write_text(text)
        p = self.spawn(prog, '--config', mine, scene=False)
        time.sleep(0.8)
        if p.poll() is not None:
            warn(f'{prog} exited ({p.returncode}); continuing without a compositor')

    def close(self):
        if getattr(self, 'closed', False):
            return
        self.closed = True
        if self.xgrab:
            self.xgrab.close()
        for p, _ in reversed(self.shot_procs):
            self.stop(p, 1)
        for p, _ in reversed(self.procs):
            self.stop(p, 2)
        # Programs the session started (the panel's udevadm, dconf...) and
        # that left their process group: find them by their private HOME.
        for pid in self.private_pids():
            try:
                os.kill(pid, signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass
        keep = self.run_ctx.out / 'logs' / getattr(self, 'log_name', self.set.name)
        try:
            if keep.exists():
                shutil.rmtree(keep)
            shutil.copytree(self.logs, keep)
        except OSError:
            pass
        if not self.args.keep_home:
            shutil.rmtree(self.tmp, ignore_errors=True)
        else:
            info(f'{self.set.name}: home kept in {self.home}')


UPDATE_NAMES = [
    # tools/update_fixture.py's QA packages -> everyday ones (left alone if
    # the fixture changes).
    ("pkg('example-security', 'example-security', True,\n            desc='Example security fixture')",
     "pkg('libxml2', 'libxml2', True, size=690000, desc='GNOME XML library', section='libs',\n"
     "            old='2.12.7+dfsg-3', new='2.12.7+dfsg-3+deb13u1')"),
    ("pkg('example-common', 'example'), pkg('example', 'example'),",
     "pkg('curl', 'curl', size=320000, desc='command line tool for transferring data with URL syntax',\n"
     "            section='web', old='8.14.1-2', new='8.14.1-3'),\n"
     "        pkg('libcurl4t64', 'curl', size=410000, desc='easy-to-use client-side URL transfer library',\n"
     "            section='libs', old='8.14.1-2', new='8.14.1-3'),"),
]

STUBS = {
    # Commands the panel and the settings call: answers for the showcase,
    # and nothing that touches the real system.
    'nmcli': r"""#!/usr/bin/env python3
import sys
a = sys.argv[1:]; s = ' '.join(a)
if 'DEVICE,TYPE,STATE,CONNECTION' in s: print('eth0:ethernet:connected:Wired connection 1\nwlan0:wifi:connected:Selviana Home')
elif 'TYPE,STATE,CONNECTION' in s: print('ethernet:connected:Wired connection 1\nwifi:connected:Selviana Home')
elif a == ['radio', 'wifi']: print('enabled')
elif a == ['radio', 'wwan']: print('disabled')
elif 'IN-USE,SIGNAL,SECURITY,SSID' in s: print('*:86:WPA2:Selviana Home\n:71:WPA2:Library Guest\n:58:WPA3:Cafe Aurora\n:34:--:Public Wi-Fi')
elif 'IP4.ADDRESS,IP4.GATEWAY,IP4.DNS' in s: print('192.168.1.24/24\n192.168.1.1\n192.168.1.1')
elif a[:2] == ['-t', 'networking']: print('full')
""",
    'pactl': r"""#!/usr/bin/env python3
import sys
a = sys.argv[1:]
if not a: sys.exit(0)
if a[0] == 'get-sink-volume': print('Volume: front-left: 42598 / 65% / -11 dB')
elif a[0] == 'get-sink-mute': print('Mute: no')
elif a[0] == 'get-default-sink': print('speakers')
elif a == ['list', 'sinks']: print('Sink #1\nName: speakers\nDescription: Built-in Audio Speakers')
elif a == ['list', 'sink-inputs']: print('Sink Input #0\nMute: no\nVolume: front-left: 52428 / 80% / -6 dB\napplication.name = "Music"\nmedia.name = "Morning Light"\napplication.process.binary = "music"')
elif a == ['list', 'sources']: print('Source #1\nName: microphone\nDescription: Built-in Microphone')
elif a[0] == 'get-source-volume': print('Volume: front-left: 36044 / 55% / -15 dB')
elif a[0] == 'get-source-mute': print('Mute: no')
elif a[0] == 'get-default-source': print('microphone')
""",
    'powerprofilesctl': """#!/bin/sh
[ "$1" = set ] && exit 0
for n in performance balanced power-saver; do
  if [ "$n" = balanced ]; then echo "* $n:"; else echo "  $n:"; fi
  echo "    Driver:     placeholder"; echo
done
""",
    'debion-network-check': """#!/bin/sh
printf '{"version":2,"verdict":"ok","repairable":false,"auto_repair":true}\\n'
""",
    # Control Center's Bluetooth page asks bluetoothctl (the panel asks BlueZ
    # on D-Bus, the fixture): the same adapter and headphones.
    'bluetoothctl': """#!/bin/sh
case "$*" in
show) printf 'Controller 00:1A:7D:DA:71:13 (public)\\n\\tName: alex-laptop\\n\\tAlias: alex-laptop\\n\\tPowered: yes\\n\\tDiscoverable: no\\n\\tPairable: yes\\n' ;;
'devices Paired'|'paired-devices') echo 'Device 00:11:22:33:44:55 Wireless Headphones' ;;
'devices Connected') ;;
devices) echo 'Device 00:11:22:33:44:55 Wireless Headphones' ;;
*) exit 0 ;;
esac
""",
    'xdg-open': '#!/bin/sh\nexit 0\n',
    'gio': '#!/bin/sh\nexit 0\n',
    'timedatectl': '#!/bin/sh\nexit 0\n',
    'pkexec': '#!/bin/sh\nexit 1\n',
    'pkcheck': '#!/bin/sh\nexit 1\n',
    'canberra-gtk-play': '#!/bin/sh\nexit 0\n',
    'paplay': '#!/bin/sh\nexit 0\n',
    'pw-play': '#!/bin/sh\nexit 0\n',
    'systemctl': '#!/bin/sh\nexit 0\n',
    'loginctl': '#!/bin/sh\nexit 0\n',
    'xinput': '#!/bin/sh\nexit 0\n',
    'fuser': '#!/bin/sh\nexit 1\n',
    'curl': '#!/bin/sh\nexit 7\n',
}

# ================================================================== scenes
#
# Each scene opens what it shows, captures its images with s.shot(name) and
# leaves nothing open (Session.clean() runs after it anyway). A missing
# program or control raises SkipShot: the image is listed as skipped.

FULL, THEMES, HERO = 'full', 'themes', 'hero'


@scene('Desktop', [('desktop', 'The desktop: the Selviana wallpaper, desktop icons and the panel')],
       scopes=(FULL, THEMES, HERO))
def desktop(s):
    s.park_pointer()
    s.shot('desktop', quiet=0.8)


@scene('Desktop', [('desktop-menu', 'Right-click on the desktop: view, sort, new folder or document, terminal')])
def desktop_menu(s):
    s.click(s.W * 0.42, s.H * 0.45, 3)
    s.shot('desktop-menu', quiet=0.4)
    s.key('Escape')


def open_menu(s):
    """The Start Menu, opened with a click on the Start button (sde-panel
    --menu is the fallback: a command can get lost while the panel starts
    or a popup closes)."""
    s.dismiss_popups()
    s.gone('^SDE Menu$', 2)
    w = None
    if s.control('SDE Panel', 'start', 3):
        try:
            w = s.click_panel('start', '^SDE Menu$')
        except SkipShot:
            w = None
    w = w or s.panel_command(['--menu'], 'SDE Menu')
    s.control('SDE Menu', '@tiles', 5)
    s.park_pointer()
    return w


@scene('Start Menu', [('start-menu', 'Start Menu: tiles in groups, most used and all applications A-Z, '
                                      'and the rail with the account, folders, settings and power')],
       scopes=(FULL, THEMES, HERO), needs=['sde-panel'])
def start_menu(s):
    open_menu(s)
    s.shot('start-menu', quiet=0.6)


@scene('Start Menu', [
    ('start-menu-rail', 'Start Menu: the rail opened with its labels'),
    ('start-menu-letters', 'Start Menu: a letter heading opens the A-Z jump grid'),
    ('start-menu-categories', 'Start Menu: the category filter of the application list'),
    ('start-menu-tile-menu', 'Start Menu: right-click on a tile (resize, move to a group, unpin)'),
    ('start-menu-app-menu', 'Start Menu: right-click on an application (pin, app info, Uninstall... with Merkadu)'),
    ('start-menu-drag', 'Start Menu: a tile dragged into another group (the group and the drop place lit)'),
    ('start-menu-account', 'Start Menu: the account on the rail'),
    ('start-menu-power', 'Start Menu: Power off asks first'),
    ('start-menu-search', 'Typing in the Start Menu searches apps, settings and files'),
], needs=['sde-panel'])
def start_menu_more(s):
    open_menu(s)
    s.press('SDE Menu', 'rail:menu')
    if s.wait(lambda: '@rail-open' in s.layout('SDE Menu'), 3):
        s.park_pointer()
        s.shot('start-menu-rail', quiet=0.5)
    s.key('Escape')
    s.wait(lambda: '@rail-open' not in s.layout('SDE Menu'), 2)
    letter = next((k for k in s.layout('SDE Menu') if k.startswith('letter:')), None)
    if letter:
        s.press('SDE Menu', letter)
        if s.wait(lambda: any(k.startswith('jump:') for k in s.layout('SDE Menu')), 3):
            s.park_pointer()
            s.shot('start-menu-letters', quiet=0.5)
        s.key('Escape')
    open_menu(s)
    flt = next((k for k in s.layout('SDE Menu') if k.startswith('filter:')), None)
    if flt:
        s.press('SDE Menu', flt)
        s.shot('start-menu-categories', quiet=0.5)
        s.key('Escape')
    open_menu(s)
    tile = next((k for k in s.layout('SDE Menu') if k.startswith('tile:')), None)
    if tile:
        s.press('SDE Menu', tile, button=3)
        s.shot('start-menu-tile-menu', quiet=0.5)
        s.key('Escape')
    open_menu(s)
    app = next((k for k in s.layout('SDE Menu') if k.startswith('app:')), None)
    if app:
        s.press('SDE Menu', app, button=3)
        s.shot('start-menu-app-menu', quiet=0.5)
        s.key('Escape')
    # A tile held over another group: Escape ends the drag, nothing moves.
    w = open_menu(s)
    lay = s.layout('SDE Menu')
    tiles = [k for k in lay if k.startswith('tile:')]
    if w and len(tiles) >= 2 and sum(k.startswith('group:') for k in lay) >= 2:
        g = s.geom(w)
        src, dst = lay[tiles[-1]], lay[tiles[0]]
        sx, sy = g['x'] + src[0] + src[2] // 2, g['y'] + src[1] + src[3] // 2
        tx, ty = g['x'] + dst[0] + dst[2] * 3 // 4, g['y'] + dst[1] + dst[3] // 2
        s.xdo('mousemove', str(sx), str(sy), 'mousedown', '1')
        for k in range(1, 7):
            s.move(sx + (tx - sx) * k / 6, sy + (ty - sy) * k / 6)
            time.sleep(0.06)
        try:
            s.shot('start-menu-drag', quiet=0.5)
        finally:
            s.key('Escape')
            s.xdo('mouseup', '1')
    open_menu(s)
    if 'rail:account' in s.layout('SDE Menu'):
        s.press('SDE Menu', 'rail:account')
        s.shot('start-menu-account', quiet=0.5)
        s.key('Escape')
    open_menu(s)
    if 'rail:power' in s.layout('SDE Menu'):
        s.press('SDE Menu', 'rail:power')
        w = s.find('^Power off$', 3)
        if w:
            s.park_pointer()
            s.shot('start-menu-power', quiet=0.5)
            s.key('Escape')  # Escape cancels; nothing is powered off
            s.gone('^Power off$', 3)
    open_menu(s)
    s.type('ga', delay=80)
    if s.find('^SDE Search$', 4) and s.wait(lambda: any(k.startswith('result:') for k in s.layout('SDE Search')), 4):
        s.park_pointer()
        s.shot('start-menu-search', quiet=0.6)
    s.key('Escape')


FLYOUTS = [
    ('network', 'Network', 'panel-network', 'Network: wired and Wi-Fi connections with switches, the network LED'),
    ('sound', 'Sound', 'panel-sound', 'Sound: output volume and device, microphone, volume per application'),
    ('bluetooth', 'Bluetooth', 'panel-bluetooth', 'Bluetooth: the adapter and paired headphones'),
    ('power', 'Power', 'panel-battery', 'Battery: charge and time left, brightness and power profiles'),
    ('drives', 'Drives', 'panel-drives', 'Drives: a USB stick with Open and Eject'),
    ('touchpad', 'Touchpad', 'panel-touchpad', 'Touchpad: a live preview and the main touchpad settings'),
    ('keyboard', 'Keyboard', 'panel-keyboard', 'Keyboard: the layout indicator'),
]


@scene('Panel', [(img, cap) for _, _, img, cap in FLYOUTS], needs=['sde-panel'])
def panel_flyouts(s):
    for section, title, img, _ in FLYOUTS:
        if not s.run_ctx.selected(s.current, img):
            continue
        s.dismiss_popups()
        s.gone('^' + title + '$', 2)
        # Like a user: a click on the status icon (sde-panel --section
        # can get lost while the previous popup closes; it is the fallback).
        w = None
        if s.control('SDE Panel', 'status:' + section, 1):
            try:
                w = s.click_panel('status:' + section, '^' + title + '$')
            except SkipShot:
                w = None
        if not w:
            try:
                w = s.panel_command(['--section', section], title)
            except SkipShot as e:
                s.skip(img, f'no {title} flyout ({e})')
                continue
        s.park_pointer()
        time.sleep(0.3)  # the icon's tooltip would show after a while
        s.shot(img, quiet=0.5)
        s.key('Escape')
        s.gone('^' + title + '$', 2)


# The fixture's UPower battery (battery_mode in tests/panel_service_fixture.cpp);
# SHOWCASE_BATTERY is the one of the other scenes (78 %, 4 h 12 min left).
SHOWCASE_BATTERY = 'state=discharging percent=78 energy=46.8 full=60 rate=11.14 tte=15124 ttf=0 warning=1'


@scene('Panel', [
    ('panel-battery-charging', 'Battery: charging, with the time until full and the percentage on the panel'),
    ('panel-battery-low', 'Battery: a low battery in amber, with its label and percentage on the panel'),
    ('panel-battery-menu', 'Battery: right-click to show the percentage, a label or the time remaining'),
], needs=['sde-panel'])
def panel_battery(s):
    def flyout(name, battery, panel):
        try:
            s.dismiss_popups()
            s.write_config({'panel': panel}, reload=True)
            if not s.fixture_mode('battery ' + battery):
                raise SkipShot('no battery in the service fixture')
            s.wait(lambda: s.control('SDE Panel', 'status:power', 1), 3)
            time.sleep(0.4)  # the panel lays out its new text
            s.click_panel('status:power', '^Power$')
            s.park_pointer()
            s.shot(name, quiet=0.5)
        except SkipShot as e:
            s.skip(name, str(e))
        finally:
            s.key('Escape')

    # 64 % of 60 Wh at 24 W: 54 minutes until full.
    flyout('panel-battery-charging', 'state=charging percent=64 energy=38.4 full=60 rate=24 ttf=3240 tte=0',
           {'battery_display': 'label-percent', 'battery_time': 'true'})
    flyout('panel-battery-low', 'state=discharging percent=14 energy=8.4 full=60 rate=9.2 tte=3287 ttf=0',
           {'battery_display': 'label-percent'})
    try:
        s.dismiss_popups()
        s.write_config({'panel': {'battery_display': 'percent'}}, reload=True)
        s.fixture_mode('battery ' + SHOWCASE_BATTERY)
        pos = s.panel_button('status:power')
        if not pos:
            raise SkipShot('no battery icon on the panel')
        # FLTK's Down skips inactive items (the battery's state line).
        active = [k[5:] for k, v in s.right_click_at(*pos).items() if k.startswith('item:') and v[4]]
        if 'Show on the Panel' not in active:
            raise SkipShot('no "Show on the Panel" in the battery menu')
        # Down to the submenu and open it (its first entry selected), one key
        # at a time: a Right in the same batch as the Downs finds no submenu.
        for _ in range(active.index('Show on the Panel') + 1):
            s.key('Down')
            time.sleep(0.08)
        time.sleep(0.3)
        s.key('Right')
        time.sleep(0.3)
        s.shot('panel-battery-menu', quiet=0.4)
    except SkipShot as e:
        s.skip('panel-battery-menu', str(e))
    finally:
        s.key('Escape', 'Escape')
        s.fixture_mode('battery ' + SHOWCASE_BATTERY)
        s.write_config(reload=True)


@scene('Panel', [('panel-clock', 'The clock: date, alarms and the way to the calendar')], needs=['sde-panel'])
def panel_clock(s):
    s.panel_command(['--alarms'], 'Alarms')
    s.park_pointer()
    s.shot('panel-clock', quiet=0.5)
    s.key('Escape')


@scene('Panel', [('panel-menu', 'Right-click on the panel: show desktop, search, lock the panel, settings, About SDE')],
       needs=['sde-panel'])
def panel_menu(s):
    L = s.layout('SDE Panel')
    xs = [v[0] for k, v in L.items() if k.startswith(('status:', 'tray-item:', 'clock'))]
    panel = s.visible('^SDE Panel$')
    if not xs or not panel:
        raise SkipShot('no panel layout')
    g = s.geom(panel)
    s.right_click_at(g['x'] + min(xs) - round(60 * s.scale), g['y'] + g['h'] // 2)
    s.shot('panel-menu', quiet=0.4)
    s.key('Escape')


@scene('Notifications', [
    ('notification-bubbles', 'Notifications: bubbles from Software Updater and a new USB drive'),
    ('panel-notifications', 'The notifications button: unread badge and the list grouped by application'),
], needs=['sde-panel', 'sde-notifyd'])
def notifications(s):
    s.clear_notifications()
    # Bubbles stay 30 s (instead of 6) while both arrive.
    s.write_config({'notifications': {'timeout': '30'}}, reload=True)
    s.keep_bubbles = True
    windows = set(s.overlays())
    try:
        # Software Updater's own notification (fixture updates)...
        if s.has('sde-update'):
            s.spawn('sde-update', '--notifier', '--once', name='sde-update-notifier')
            s.wait(lambda: len(s.notification_ids()) >= 1, 10)
        # ...and the panel's for a USB drive plugged in (fixture; it sets
        # its own, shorter time: last, so both bubbles are on screen).
        before = len(s.notification_ids())
        if s.fixture_mode('drive-remove') and s.fixture_mode('drive-insert'):
            s.wait(lambda: len(s.notification_ids()) > before, 6)
        if not s.notification_ids():
            s.notify('Software Updater', '8 software updates are available',
                     '2 critical and 3 security updates. Open Software Updater to install them.',
                     'system-software-update')
        if not s.wait(s.notification_ids, 4):
            raise SkipShot('no notification arrived')
        s.park_pointer()
        time.sleep(0.8)  # the bubbles map (unnamed windows: nothing to wait for)
        s.shot('notification-bubbles', quiet=0.4, timeout=3)
        # The bubbles away (unmapped as if they had expired); the history
        # stays for the notifications button. CloseNotification would
        # withdraw them from the history too.
        for w in set(s.overlays()) - windows:
            s.xdo('windowunmap', str(w))
        time.sleep(0.4)
        s.click_panel('notifications', '^Notifications$')
        s.park_pointer()
        s.shot('panel-notifications', quiet=0.5)
        s.key('Escape')
    finally:
        s.keep_bubbles = False
        s.write_config(reload=True)
        s.clear_notifications()


@scene('Panel', [('panel-previews', 'Taskbar: hovering a button previews the windows of the application')],
       needs=['sde-panel'])
def taskbar_previews(s):
    docs, pics = s.home / 'Documents', s.home / 'Pictures/Nature'
    if s.has('liria'):
        a = s.app(['liria', '--no-desktop', docs], '^Documents$')
        s.size(a, 900, 560)
        b = s.app(['liria', '--no-desktop', pics], '^Nature$')
        s.size(b, 900, 560)
    elif s.test_tool('sde-preview-window'):
        tool = s.test_tool('sde-preview-window')
        s.spawn(tool, 'Garden Planner', '2F6FD6')
        s.spawn(tool, 'Field Report', '16A394')
        s.need('^Field Report$')
    else:
        raise SkipShot('no application with two windows')
    btn = s.wait(lambda: next((v for k, v in s.layout('SDE Panel').items() if k.startswith('task:')), None), 5)
    if not btn:
        raise SkipShot('no taskbar button')
    task = next(k for k in s.layout('SDE Panel') if k.startswith('task:'))
    r = s.layout('SDE Panel')[task]
    g = s.geom(s.visible('^SDE Panel$'))
    s.move(g['x'] + r[0] + r[2] // 2, g['y'] + r[1] + r[3] // 2)
    pv = s.find('^SDE Preview$', 4)
    if not pv:
        raise SkipShot('no preview window')
    s.wait(lambda: sum(k.startswith('@thumb:') for k in s.layout('SDE Preview')) >= 1, 3)
    time.sleep(0.6)
    s.shot('panel-previews', quiet=0.5)
    s.park_pointer()


def open_ac(s):
    """The Action Center, opened with a click on its panel button
    (sde-panel --action-center is the fallback, as for the Start Menu)."""
    s.dismiss_popups()
    s.gone('^Action Center$', 2)
    w = None
    if s.control('SDE Panel', 'action-center', 3):
        try:
            w = s.click_panel('action-center', '^Action Center$')
        except SkipShot:
            w = None
    w = w or s.panel_command(['--action-center'], 'Action Center')
    s.wait(lambda: '@body' in s.layout('Action Center'), 3)
    last = None
    for _ in range(20):  # the layout settles when the services answered
        L = s.layout('Action Center')
        if L == last:
            break
        last = L
        time.sleep(0.15)
    s.park_pointer()
    return w


@scene('Action Center', [('action-center', 'Action Center: clock and weather, quick settings, sliders, music, '
                                           'calendar and agenda')],
       scopes=(FULL, HERO), needs=['sde-panel'])
def action_center(s):
    s.set_clock('10:42')
    open_ac(s)
    s.shot('action-center', quiet=0.6)
    s.key('Escape')
    s.set_clock(None)


@scene('Action Center', [
    ('action-center-compact', 'Action Center, compact: quick settings, sliders and the music player'),
    ('action-center-night', 'Action Center at night: stars and the moon behind the clock'),
    ('action-center-led', 'Clock styles: LED digits with a blinking colon, in the Action Center and on the panel'),
    ('action-center-analog', 'Clock styles: an analog clock with the seconds hand'),
    ('action-center-power', 'Action Center: sleep, log out, restart and shut down'),
    ('action-center-brightness', 'Action Center: brightness settings with Night Light'),
], needs=['sde-panel'])
def action_center_more(s):
    # Each view on its own: one that does not open skips only its image.
    def view(name, config, clock='10:42', ready=None):
        try:
            s.set_clock(clock)
            s.write_config(config, reload=True)
            open_ac(s)
            if ready:
                s.wait(lambda: ready in s.layout('Action Center'), 3)
            s.shot(name, quiet=0.6)
        except SkipShot as e:
            s.skip(name, str(e))
        finally:
            s.key('Escape')

    view('action-center-compact', {'action_center': {'expanded': 'false'}})
    view('action-center-night', {}, clock='21:30')
    # Seconds and the blinking colon are options of their own (0.2 Alpha 2).
    for style, option in (('led', 'clock_blink'), ('analog', 'clock_seconds')):
        view(f'action-center-{style}', {'action_center': {'expanded': 'false', 'clock_style': style},
                                        'clock': {'style': style}, 'panel': {option: 'true'}},
             ready=f'@clock-{style}')
    s.write_config(reload=True)
    s.set_clock('10:42')
    # A footer or a card opens with a click; the click can race the popup
    # still laying out (services answering): try again then.
    for button, wanted, name in [('Power', 'Shut Down', 'action-center-power'),
                                 ('Brightness settings', 'Night Light', 'action-center-brightness')]:
        try:
            open_ac(s)
            for _ in range(3):
                if s.control('Action Center', wanted, 0.3):
                    break
                if s.control('Action Center', button, 2):
                    s.press('Action Center', button)
                    s.control('Action Center', wanted, 2)
            if not s.control('Action Center', wanted, 0.3):
                raise SkipShot(f'{button} did not open {wanted}')
            s.park_pointer()
            s.shot(name, quiet=0.5)
        except SkipShot as e:
            s.skip(name, str(e))
        finally:
            s.key('Escape')
    s.set_clock(None)


SETTINGS_TITLE = '^(Settings|Control Center|SDE Settings)$'
ABOUT_SCROLL = 6  # wheel steps from the top of Control Center > About to its licenses card


def settings_page(s, page):
    w = s.app(['sde-settings', '--page', page], SETTINGS_TITLE)
    s.park_pointer()
    return w


@scene('Control Center', [('control-center', 'Control Center: the overview with a card for each area')],
       scopes=(FULL, HERO), needs=['sde-settings'])
def control_center(s):
    settings_page(s, 'home')
    s.shot('control-center', quiet=0.6)


PAGE_CAPTIONS = {
    'appearance': 'themes (Graphite, Fluent, Glass), light or dark, accent color, transparency, wallpaper',
    'panel': 'panel style and position, search, clock and status icons',
    'desktop': 'desktop icons',
    'action-center': 'clock card, quick settings and what the Action Center shows',
    'clock': 'Digital, LED or Analog clock, seconds and the blinking colon, time format and weather',
    'notifications': 'Do Not Disturb and how long notifications stay',
    'effects': 'animations and the compositor',
    'display': 'monitors, resolution and scale',
    'input': 'keyboard layouts and the mouse',
    'touchpad': 'tap to click, scrolling and speed',
    'sound': 'output and input devices and volume',
    'network': 'connections, Wi-Fi and proxy',
    'bluetooth': 'adapter and devices',
    'power': 'the battery on the panel and its warnings, idle times, lid and power button',
    'lock': 'screen lock',
    'suite': 'the DebION applications, sources and licenses',
    'default-apps': 'default applications for files and links',
    'system': 'system information and maintenance',
    'about': 'version, build and the development log',
    'start-menu': 'size, the lists and the account on the rail',
    'date-time': 'time zone and network time',
}
# The account page has its own image (avatar-picker).
ACCOUNT_PAGES = ('users', 'account', 'accounts', 'user', 'avatar')


def control_center_images(run):
    pages = [p for p in run.pages if p != 'home' and p not in ACCOUNT_PAGES] or list(PAGE_CAPTIONS)
    return [(f'control-center-{p}', f'Control Center, {p.replace("-", " ").title()}: ' +
             PAGE_CAPTIONS.get(p, 'settings')) for p in pages]


# Pages shown scrolled to a card (sde-settings --page <alias>): Power at its
# Battery card, with the battery's display on the panel (0.2 Alpha 2).
PAGE_ANCHORS = {'power': 'battery'}


@scene('Control Center', control_center_images, needs=['sde-settings'])
def control_center_pages(s):
    for name, _ in s.run_ctx.images_of(s.current):
        if not s.run_ctx.selected(s.current, name):
            continue
        page = name[len('control-center-'):]
        try:
            settings_page(s, PAGE_ANCHORS.get(page, page))
            s.shot(name, quiet=0.6)
        except SkipShot as e:
            s.skip(name, str(e))
        s.stop_scene_procs()
        s.gone(SETTINGS_TITLE, 3)


def about_sde_window(s):
    """Control Center > About as the panel's "About SDE" opens it."""
    w = None
    try:
        L = s.layout('SDE Panel')
        xs = [v[0] for k, v in L.items() if k.startswith(('status:', 'tray-item:', 'clock'))]
        g = s.geom(s.visible('^SDE Panel$'))
        items = s.right_click_at(g['x'] + min(xs) - round(60 * s.scale), g['y'] + g['h'] // 2)
        labels = [k.split(':', 1)[1] for k in items if k.startswith('item:') and items[k][4]]
        labels = [lb[:-3] if lb.endswith(':on') else lb for lb in labels]
        if 'About SDE' in labels:
            s.key(*(['Down'] * (labels.index('About SDE') + 1)), 'Return')
            w = s.find(SETTINGS_TITLE, 10)
        else:
            s.key('Escape')
    except (SkipShot, ValueError):
        s.key('Escape')
    if not w:
        w = settings_page(s, 'about')
    s.fit(w)
    s.window_settle(w)
    return w


@scene('About', [('about-sde', 'About SDE (the panel menu): copyright, the license and the open-source licenses'),
                 ('about-licenses', 'Open-source licenses: every SDE program and bundled component')],
       needs=['sde-settings'])
def about_sde(s):
    w = about_sde_window(s)
    # Down to the "Copyright and licenses" card.
    g = s.geom(w)
    s.move(g['x'] + g['w'] * 0.7, g['y'] + g['h'] * 0.6)
    for _ in range(ABOUT_SCROLL):
        s.xdo('click', '5')
    s.park_pointer()
    s.shot('about-sde', quiet=0.6)
    # Its "Show Licenses" dialog. Control Center lists no controls for
    # tests, so the same dialog comes from the UI kit helper (test tools),
    # over the About page; the helper's own window is hidden.
    demo = s.test_tool('sde-ui-kit-demo')
    if not demo:
        s.skip('about-licenses', 'sde-ui-kit-demo not found (--test-dir)')
        return
    s.spawn(demo, '--show', 'licenses', name='ui-kit-demo')
    d = s.find('^Open-Source Licenses$', 15)
    if not d:
        s.skip('about-licenses', 'the Open-Source Licenses dialog did not open')
        return
    for helper in s.visible_all('^UI Kit Demo$'):
        s.xdo('windowunmap', helper)
    s.fit(d)
    s.window_settle(d, quiet=0.6)
    s.park_pointer()
    s.shot('about-licenses', quiet=0.6)


ABOUT_SECTIONS = [('about', 'about-app', 'About Calculator: the About section (Selviana Desktop Environment, '
                                          'version, developer)'),
                  ('credits', 'about-app-credits', 'About Calculator: Credits'),
                  ('license', 'about-app-license', 'About Calculator: License (GNU GPL notice, View License)'),
                  ('legal', 'about-app-legal', 'About Calculator: Legal (copyright and trademarks)')]


@scene('About', [(img, cap) for _, img, cap in ABOUT_SECTIONS] +
       [('about-app-license-text', 'About Calculator: the full license text')], needs=['calculator'])
def about_app(s):
    calc = s.app(['calculator'], '^Calculator$')
    qa = s.home / 'qa/about.log'
    s.raw_key('F1')
    a = s.find('^About Calculator$', 5)
    if not a:
        # Once more with the window activated again; then say what is on the
        # screen and what Calculator wrote, so a build log shows the cause.
        s.activate(calc)
        s.raw_key('F1')
        a = s.find('^About Calculator$', 5)
    if not a:
        names = [s.title(w) for w in s.visible_all()]
        log = s.logs / 'calculator.log'
        tail = log.read_text(errors='replace').splitlines()[-15:] if log.exists() else []
        warn(f'{s.set.name}: about-app: windows {names}; calculator.log: ' + ' | '.join(tail))
        raise SkipShot('About Calculator did not open (F1)')
    s.fit(a)
    for i, (section, img, _) in enumerate(ABOUT_SECTIONS, 1):
        if i > 1:
            # Alt+1..4 choose a section (the About window logs it for tests).
            before = qa.read_text().count('section ') if qa.exists() else 0
            s.key(f'alt+{i}')
            if not s.wait(lambda: qa.exists() and qa.read_text().count('section ') > before, 3):
                s.skip(img, f'Alt+{i} did not show the {section} section (an older About window)')
                continue
        s.window_settle(a, quiet=0.4)
        s.park_pointer()
        s.shot(img, quiet=0.4)
    s.key('alt+3')
    time.sleep(0.4)
    s.key('alt+v')
    v = s.find('^Calculator .* License$', 5)
    if not v:
        s.skip('about-app-license-text', 'no license viewer (Alt+V)')
        return
    s.fit(v)
    s.window_settle(v, quiet=0.5)
    s.park_pointer()
    s.shot('about-app-license-text', quiet=0.5)
    s.key('Escape')


@scene('Liria', [('liria', 'Liria: the home folder with Quick access pins, recent files and drives')],
       scopes=(FULL, THEMES, HERO), needs=['liria'])
def liria(s):
    w = s.app(['liria', '--no-desktop', s.home], '^' + s.home.name + '$')
    s.size(w, 1040, 640)
    s.park_pointer()
    s.shot('liria', quiet=0.8)


@scene('Liria', [
    ('liria-ribbon', 'Liria: the ribbon with the Home tab, a document selected'),
    ('liria-details', 'Liria: the details view with sizes, types and dates'),
    ('liria-context-menu', 'Liria: right-click on a file'),
    ('liria-properties', 'Liria: the properties of a picture'),
], needs=['liria'])
def liria_more(s):
    w = s.app(['liria', '--no-desktop', s.home / 'Documents'], '^Documents$')
    s.size(w, 1040, 640)
    s.activate(w)
    # Icons sorted by name: Projects, then the files; the fifth is the PDF.
    s.key('Home', 'Right', 'Right', 'Right', 'Right')
    s.park_pointer()
    s.shot('liria-ribbon', quiet=0.8)
    s.key('ctrl+2')
    time.sleep(0.8)
    s.park_pointer()
    s.shot('liria-details', quiet=0.8)
    s.key('ctrl+1')  # back to icons: Liria keeps the view per folder
    time.sleep(0.5)
    s.stop_scene_procs()
    w = s.app(['liria', '--no-desktop', s.home / 'Pictures/Nature'], '^Nature$')
    s.size(w, 1040, 640)
    s.activate(w)
    # A picture selected with the keyboard, its menu with Shift+F10 (it
    # opens at the pointer: over the files).
    s.key('Home', 'Right')
    time.sleep(0.3)
    g = s.geom(w)
    s.move(g['x'] + g['w'] * 0.42, g['y'] + g['h'] * 0.36)
    box = s.box_of(w)
    before = s.hash_box(box)
    s.raw_key('shift+F10')
    if not s.wait(lambda: s.hash_box(box) != before, 1.5):
        s.key('Menu')
    s.shot('liria-context-menu', quiet=0.6)
    s.key('Escape')
    s.key('alt+Return')
    p = s.find('^(.* )?Properties$', 5)
    if p:
        s.fit(p)
        s.window_settle(p)
        s.park_pointer()
        s.shot('liria-properties', quiet=0.6)


def app_with_env(s, cmd, title, extra):
    """s.app () with extra environment variables for the program (the QA
    hooks of Liria)."""
    saved = s.env
    s.env = dict(saved, **extra)
    try:
        return s.app(cmd, title)
    finally:
        s.env = saved


def qa_pairs(path):
    """The key=value lines of a QA file."""
    try:
        return dict(line.partition('=')[::2] for line in Path(path).read_text().splitlines())
    except OSError:
        return {}


def qa_box(values, key):
    try:
        return tuple(int(v) for v in values[key].split(','))
    except (KeyError, ValueError):
        return None


def click_box(s, wid, box):
    """Clicks the middle of a box given in window coordinates (QA files)."""
    g = s.geom(wid)
    s.click(g['x'] + box[0] + box[2] // 2, g['y'] + box[1] + box[3] // 2)


def ribbon_tabs(path):
    """The ribbon's tabs from LIRIA_SDE_QA_RIBBON: {page: (x, y, w, h)}."""
    tabs = {}
    try:
        for line in Path(path).read_text().splitlines():
            f = line.split('\t')
            if f[0] == 'tab' and len(f) == 3:
                tabs[f[1]] = tuple(int(v) for v in f[2].split(','))
    except (OSError, ValueError):
        pass
    return tabs


@scene('Liria', [
    ('liria-ribbon-archive', 'Liria: the Archive tab of the ribbon for a selected archive'),
    ('liria-ribbon-share', 'Liria: the Share tab of the ribbon (email, a removable drive, links)'),
    ('liria-ribbon-view', 'Liria: the View tab of the ribbon (panes, layout, sorting, hidden files)'),
], needs=['liria'])
def liria_ribbon_tabs(s):
    """The ribbon's other tabs in the icon theme's colours; the tabs' places
    come from LIRIA_SDE_QA_RIBBON."""
    qa = s.home / 'qa/liria-ribbon.txt'
    qa.parent.mkdir(parents=True, exist_ok=True)
    w = app_with_env(s, ['liria', '--no-desktop', s.home / 'Downloads'], '^Downloads$',
                     {'LIRIA_SDE_QA_RIBBON': str(qa)})
    s.size(w, 1040, 640)
    s.activate(w)
    if not s.wait(lambda: 'share' in ribbon_tabs(qa), 8):
        raise SkipShot('the ribbon reported no tabs (LIRIA_SDE_QA_RIBBON)')
    # The archive selected by typing its name; the Archive tab appears.
    s.type('Autumn', 60)
    time.sleep(0.5)
    s.key('Escape')
    if s.wait(lambda: 'archive' in ribbon_tabs(qa), 5):
        click_box(s, w, ribbon_tabs(qa)['archive'])
        s.park_pointer()
        s.shot('liria-ribbon-archive', quiet=0.6)
    else:
        s.skip('liria-ribbon-archive', 'no Archive tab for the selected archive')
    for page in ('share', 'view'):
        click_box(s, w, ribbon_tabs(qa)[page])
        s.park_pointer()
        s.shot('liria-ribbon-' + page, quiet=0.6)
    # Liria remembers the tab: Home again for the next scenes.
    click_box(s, w, ribbon_tabs(qa)['home'])
    time.sleep(0.3)


@scene('Liria', [('liria-copy-progress', 'Liria: copying videos, with More details: the speed graph, the time '
                                         'and items remaining and the animated SDE mark')], needs=['liria'])
def liria_copy_progress(s):
    """A copy slowed down to about 20 MB/s (LIRIA_SDE_QA_COPY_RATE), the
    file operations window with More details."""
    src = s.home / 'Videos/Trip to the Lakes'
    dst = s.home / 'Backup'
    src.mkdir(parents=True, exist_ok=True)
    dst.mkdir(exist_ok=True)
    large = [('Drone flight over Lake Varen.mp4', 420), ('Kayaking at Moss Lake.mp4', 260),
             ('Campfire at Stone Bay.mp4', 180)]
    for name, mib in large:
        with open(src / name, 'wb') as f:
            f.truncate(mib << 20)
    qa = s.home / 'qa/liria-progress.txt'
    qa.parent.mkdir(parents=True, exist_ok=True)
    env = {'LIRIA_SDE_QA_PROGRESS': str(qa), 'LIRIA_SDE_QA_COPY_RATE': '20000000'}
    try:
        w = app_with_env(s, ['liria', '--no-desktop', src], '^Trip to the Lakes$', env)
        s.size(w, 900, 560)
        s.activate(w)
        s.key('ctrl+a')
        s.key('ctrl+c')
        b = app_with_env(s, ['liria', '--no-desktop', dst], '^Backup$', env)
        s.size(b, 1040, 640)
        s.activate(b)
        g = s.geom(b)
        s.click(g['x'] + g['w'] * 0.7, g['y'] + g['h'] * 0.8)  # the empty view: keyboard focus
        s.key('ctrl+v')
        if not s.wait(lambda: qa_box(qa_pairs(qa), 'toggle') and qa_pairs(qa).get('window'), 15):
            raise SkipShot('no file operations window')
        progress = qa_pairs(qa)['window']
        click_box(s, progress, qa_box(qa_pairs(qa), 'toggle'))
        s.wait(lambda: qa_pairs(qa).get('details') == '1', 3)
        time.sleep(3.0)  # the graph fills
        s.park_pointer()
        # The window changes four times a second: no waiting for stillness.
        s.shot('liria-copy-progress', quiet=0.2, timeout=0.5)
        cancel = qa_box(qa_pairs(qa), 'cancel')
        if cancel:
            click_box(s, progress, cancel)
            s.wait(lambda: qa_pairs(qa).get('done') == '1', 10)
    finally:
        s.stop_scene_procs()
        shutil.rmtree(dst, ignore_errors=True)
        for name, _ in large:
            (src / name).unlink(missing_ok=True)


@scene('Desktop', [('sde-transfer', 'SDE: the copy and move dialog of the desktop with More details (speed '
                                    'graph, time and items remaining)')])
def sde_transfer(s):
    """SDE-FLTK's copy and move dialog (src/common/transfer_dialog.h) with a
    simulated copy from the test tools."""
    demo = s.test_tool('sde-transfer-demo')
    if not demo:
        raise SkipShot('sde-transfer-demo not found (--test-dir)')
    s.spawn(demo, '--details', '--at', '0.46', name='transfer-demo')
    w = s.need('complete$', 10, what='the copy dialog')
    s.activate(w)
    time.sleep(1.0)
    s.park_pointer()
    # The mark is animated: no waiting for stillness.
    s.shot('sde-transfer', quiet=0.2, timeout=0.5)


@scene('Niny', [('niny', 'Niny: Markdown with its formatted view, tabs, the menu bar and the formatting toolbar')],
       scopes=(FULL, THEMES), needs=['niny'])
def niny(s):
    docs = s.home / 'Documents'
    s.app(['niny', docs / 'Projects/garden-planner/planner.py', docs / 'Garden Planner.md'], 'Niny$')
    s.key('ctrl+shift+m')
    time.sleep(0.6)
    s.park_pointer()
    s.shot('niny', quiet=0.6)


@scene('Niny', [('niny-code', 'Niny: Python with syntax highlighting')], needs=['niny'])
def niny_code(s):
    s.app(['niny', s.home / 'Documents/Projects/garden-planner/planner.py'], 'Niny$')
    s.park_pointer()
    s.shot('niny-code', quiet=0.6)


@scene('Niny', [('niny-menu', 'Niny: the menu bar (File to Help) with View open: Menu Bar, Font…, zoom and theme'),
                ('niny-font', 'Niny: View > Font…, the family and size with a preview'),
                ('niny-shortcuts', 'Niny: Help > Keyboard Shortcuts')], needs=['niny'])
def niny_menu(s):
    s.app(['niny', s.home / 'Documents/Meeting Notes.txt'], 'Niny$')
    s.park_pointer()
    s.key('alt+v')  # the underlined letter of View
    time.sleep(0.6)
    s.shot('niny-menu', quiet=0.6)
    s.key('f', 'Return')  # the first item that starts with F: Font…
    time.sleep(0.8)
    s.shot('niny-font', quiet=0.6)
    s.key('Escape')
    time.sleep(0.3)
    s.raw_key('F1')
    time.sleep(0.8)
    s.shot('niny-shortcuts', quiet=0.6)


@scene('Niny', [('niny-single-tab', 'Niny: the only tab cannot be closed; its × is inactive and says why')],
       needs=['niny'])
def niny_single_tab(s):
    w = s.app(['niny', s.home / 'Documents/Meeting Notes.txt'], 'Niny$')
    r = s.control('Niny', 'close:0')  # Niny's QA layout (SDE_PANEL_QA_DIR)
    if not r:
        raise SkipShot('Niny wrote no layout of its tabs')
    g = s.geom(w)
    s.move(g['x'] + r[0] + r[2] // 2, g['y'] + r[1] + r[3] // 2)
    time.sleep(1.6)  # the tooltip
    s.shot('niny-single-tab', quiet=0.5)


@scene('Xanara', [('xanara', 'Xanara: a PDF report with thumbnails and the Annotate tools')], needs=['xanara'])
def xanara(s):
    s.app(['xanara', s.home / 'Documents/Project Aurora - Field Report.pdf'], 'Xanara$')
    s.key('ctrl+1')  # fit the page
    s.park_pointer()
    s.shot('xanara', quiet=0.8)


@scene('Taca', [('taca', 'Taca: the contents of a ZIP archive')], needs=['taca'])
def taca(s):
    s.app(['taca', s.home / 'Downloads/Autumn Photos.zip'], 'Taca$')
    s.park_pointer()
    s.shot('taca', quiet=0.6)


@scene('Photo', [('photo', 'Photo: a picture with its details and the film strip'),
                 ('photo-draw', 'Photo: the Draw tab with pens, shapes and colors')], needs=['sde-view'])
def photo(s):
    pic = s.home / 'Pictures/Nature/Mountain Lake.png'
    s.app(['sde-view', pic], 'Photo$')
    s.park_pointer()
    s.shot('photo', quiet=0.6)
    s.stop_scene_procs()
    s.gone('Photo$', 3)
    s.app(['sde-view', '--draw', pic], 'Photo$')
    s.park_pointer()
    s.shot('photo-draw', quiet=0.6)


@scene('Gallery', [('gallery', 'Gallery: all photos by month, folders and albums'),
                   ('gallery-album', 'Gallery: a folder of holiday photos')], needs=['sde-gallery'])
def gallery(s):
    s.app(['sde-gallery'], '^Gallery$')
    s.park_pointer()
    s.shot('gallery', quiet=0.8)
    s.spawn('sde-gallery', s.home / 'Pictures/Holidays 2026', name='sde-gallery-browse')
    time.sleep(1.0)
    s.park_pointer()
    s.shot('gallery-album', quiet=0.8)


@scene('Calculator', [('calculator', 'Calculator: standard mode with the history'),
                      ('calculator-scientific', 'Calculator: scientific mode'),
                      ('calculator-programmer', 'Calculator: programmer mode with bases and bits'),
                      ('calculator-converter', 'Calculator: unit converter')], needs=['calculator'])
def calculator(s):
    s.app(['calculator', '--mode', 'standard'], '^Calculator$')
    for expr in ('1250*12', '15000/12+250', '42.5*4'):
        s.type(expr, 40)
        s.key('Return')
    s.park_pointer()
    s.shot('calculator', quiet=0.5)
    s.key('ctrl+2')
    s.key('Escape')
    s.type('sqrt(2)*pi+3^2', 40)
    s.key('Return')
    s.park_pointer()
    s.shot('calculator-scientific', quiet=0.5)
    s.key('ctrl+3')
    s.key('Escape')
    s.type('2026', 40)
    s.park_pointer()
    s.shot('calculator-programmer', quiet=0.5)
    s.key('ctrl+4')
    s.park_pointer()
    s.shot('calculator-converter', quiet=0.5)


@scene('System Monitor', [('system-monitor', 'System Monitor: the SDE processes (search "sde")'),
                          ('system-monitor-performance', 'System Monitor: processor, memory and disk graphs')],
       needs=['sde-system-monitor'])
def system_monitor(s):
    s.app(['sde-system-monitor', '--page', 'processes'], '^System Monitor$')
    # The session's own programs, not whatever else runs on the machine
    # (Ctrl+F: the search field).
    s.key('ctrl+f')
    s.type('sde', delay=80)
    time.sleep(1.2)
    s.park_pointer()
    s.shot('system-monitor', quiet=0.8, timeout=4)
    s.stop_scene_procs()
    s.gone('^System Monitor$', 3)
    s.app(['sde-system-monitor', '--page', 'performance'], '^System Monitor$', settle=False)
    time.sleep(6)  # some history in the graphs
    s.park_pointer()
    s.shot('system-monitor-performance', quiet=0.3, timeout=2)


@scene('Software Updater', [('software-updater', 'Software Updater: critical, security, main and Flatpak '
                                                 'updates (test data)')], needs=['sde-update'])
def software_updater(s):
    w = s.app(['sde-update'], '^Software Updater$', settle=False)
    time.sleep(2.0)  # check.py and flatpak.py
    s.window_settle(w, quiet=0.8, timeout=8)
    s.park_pointer()
    s.shot('software-updater', quiet=0.6)


MERKADU_FIXTURES = ROOT / 'tests/fixtures/merkadu'


def merkadu_window(s, args, ready):
    """Merkadu with its fake sources (make_env), at a page whose QA layout
    has control |ready|."""
    s.stop_scene_procs()
    s.gone('^Merkadu$', 3)
    w = s.app(['merkadu'] + args, '^Merkadu$', settle=False)
    if not s.control('Merkadu', ready, timeout=10):
        raise SkipShot(f'Merkadu did not show {ready} (fake sources: {MERKADU_FIXTURES})')
    s.park_pointer()
    return w


@scene('Merkadu', [('merkadu', 'Merkadu, the software center: featured, new and top apps from Debian, Flatpak '
                               'and Snap (test data)'),
                   ('merkadu-category', 'Merkadu: a department with its subcategories'),
                   ('merkadu-search', 'Merkadu: one search in every source, with the source of each app'),
                   ('merkadu-details', 'Merkadu: an app page with its sources, screenshots and details'),
                   ('merkadu-installed', 'Merkadu: installed apps by department'),
                   ('merkadu-history', 'Merkadu: installations, updates and removals of every source'),
                   ('merkadu-progress', 'Merkadu: installing an app, with the animated SDE logo')],
       needs=['merkadu'])
def merkadu(s):
    if not (MERKADU_FIXTURES / 'catalog.json').exists():
        raise SkipShot('no Merkadu test data (tests/fixtures/merkadu)')
    w = merkadu_window(s, [], 'banner:org.example.paintbox')
    s.shot('merkadu', quiet=0.6, wid=w)
    w = merkadu_window(s, ['--category', 'graphics'], 'app:org.example.vectorial')
    s.shot('merkadu-category', quiet=0.6, wid=w)
    w = merkadu_window(s, [], 'banner:org.example.paintbox')
    s.key('ctrl+f')
    s.type('write', 60)
    s.key('Return')  # Enter also asks the Snap Store
    s.control('Merkadu', 'app:org.example.cloudnotes', timeout=6)
    s.park_pointer()
    s.shot('merkadu-search', quiet=0.6, wid=w)
    w = merkadu_window(s, ['--details', 'org.example.Paintbox'], 'source:snap')
    s.shot('merkadu-details', quiet=0.6, wid=w)
    w = merkadu_window(s, ['--page', 'installed'], 'app:org.example.waveform')
    s.shot('merkadu-installed', quiet=0.6, wid=w)
    w = merkadu_window(s, ['--page', 'history'], 'history:0')
    s.shot('merkadu-history', quiet=0.6, wid=w)
    # The change holds at MERKADU_FAKE_HOLD and the logo at MERKADU_LOGO_PHASE.
    w = merkadu_window(s, ['--details', 'org.example.Lumen'], 'action:org.example.lumen:Install')
    s.press('Merkadu', 'action:org.example.lumen:Install')
    s.control('Merkadu', 'nav:progress', timeout=5)
    s.press('Merkadu', 'nav:progress')
    s.control('Merkadu', '@page:progress', timeout=5)
    s.park_pointer()
    s.shot('merkadu-progress', quiet=0.6, wid=w)


@scene('Merkadu', [('merkadu-uninstall', 'Uninstall from the Start menu (merkadu --uninstall): the packages '
                                         'removed with the app and the space it frees'),
                   ('merkadu-uninstall-progress', 'Uninstalling, with the animated SDE logo')],
       needs=['merkadu'])
def merkadu_uninstall(s):
    desktop = MERKADU_FIXTURES / 'applications/waveform-studio.desktop'
    if not desktop.exists():
        raise SkipShot('no Merkadu test data (tests/fixtures/merkadu)')
    w = s.app(['merkadu', '--uninstall', desktop], '^Uninstall$', settle=False)
    if not s.control('Merkadu Uninstall', '@state:confirm', timeout=10):
        raise SkipShot('the uninstall window did not ask')
    s.park_pointer()
    s.shot('merkadu-uninstall', quiet=0.6, wid=w)
    r = s.control('Merkadu Uninstall', 'Uninstall', timeout=3)
    g = s.geom(w)
    s.click(g['x'] + r[0] + r[2] / 2, g['y'] + r[1] + r[3] / 2)
    s.control('Merkadu Uninstall', 'logo', timeout=5)
    s.park_pointer()
    s.shot('merkadu-uninstall-progress', quiet=0.6, wid=w)


@scene('Welcome', [('welcome', 'Welcome: first steps after installing DebION')], needs=['sde-welcome'])
def welcome(s):
    s.app(['sde-welcome'], '^Welcome$')
    s.park_pointer()
    s.shot('welcome', quiet=0.6)


@scene('Driver Manager', [('driver-manager', 'Driver Manager: proprietary and open drivers (demo devices)')],
       needs=['sde-drivers'])
def driver_manager(s):
    s.app(['sde-drivers'], '^Driver Manager$')
    s.park_pointer()
    s.shot('driver-manager', quiet=0.6)


@scene('System Reports', [('system-reports', 'System Reports: system information and crash reports')],
       needs=['sde-system-reports'])
def system_reports(s):
    s.app(['sde-system-reports'], '^System Reports$')
    s.park_pointer()
    s.shot('system-reports', quiet=0.6)


@scene('Display Settings', [('display-settings', 'Display Settings: arrangement, resolution, scale and profiles')],
       needs=['sde-display-settings'])
def display_settings(s):
    s.app(['sde-display-settings'], '^Display Settings$')
    s.park_pointer()
    s.shot('display-settings', quiet=0.6)


@scene('Run and Log Out', [('run-dialog', 'Run Command (Alt+F2)'),
                           ('logout-dialog', 'Leave SDE: lock, suspend, log out, restart or shut down')])
def run_and_logout(s):
    # Each dialog on its own: one that does not open skips only its image.
    try:
        s.app(['sde-run'], '^Run Command$')
        s.type('liria ~/Pictures', 50)
        s.park_pointer()
        s.shot('run-dialog', quiet=0.5)
        s.key('Escape')
    except SkipShot as e:
        s.skip('run-dialog', str(e))
    s.stop_scene_procs()
    try:
        s.app(['sde-logout'], '^Leave SDE$|^Log Out$')
        s.park_pointer()
        s.shot('logout-dialog', quiet=0.5)
        s.key('Escape')  # cancels
    except SkipShot as e:
        s.skip('logout-dialog', str(e))


@scene('Lock Screen', [('lock-screen', 'The lock screen (test fixture: nothing is locked)')])
def lock_screen(s):
    fixture = s.test_tool('sde-lock-ui-fixture')
    if not fixture:
        raise SkipShot('sde-lock-ui-fixture not found (--test-dir)')
    r, w = os.pipe()
    env = dict(s.env, SDE_LOCK_FIXTURE_NAME=showcase_content.USER_NAME,
               SDE_LOCK_FIXTURE_AVATAR=str(s.home / '.face'))
    p = sp.Popen([fixture, str(w)], env=env, pass_fds=(w,), stdout=open(s.logs / 'lock.log', 'a'),
                 stderr=sp.STDOUT, start_new_session=True)
    s.shot_procs.append((p, 'lock-fixture'))
    os.close(w)
    # The pipe stays open while the lock screen runs: it reports a
    # heartbeat every second and would die of SIGPIPE without a reader.
    try:
        got = b''
        os.set_blocking(r, False)
        end = time.time() + 10
        while time.time() < end and b'L' not in got:
            try:
                got += os.read(r, 64) or b''
            except BlockingIOError:
                pass
            time.sleep(0.05)
        if b'M' not in got:
            raise SkipShot('the lock screen did not map')
        time.sleep(0.8)
        s.shot('lock-screen', quiet=0.8)
        s.type('sde-test', 30)  # the fixture's password: unlocks
        s.key('Return')
        s.wait(lambda: p.poll() is not None, 5)
    finally:
        os.close(r)


@scene('Accounts', [('avatar-picker', 'Control Center, Users & Avatar: the account picture, the name and '
                                       'the stock pictures')], needs=['sde-settings'])
def avatar_picker(s):
    page = next((p for p in s.run_ctx.pages if p in ACCOUNT_PAGES), None)
    if not page:
        raise SkipShot('no account page in this build')
    settings_page(s, page)
    s.shot('avatar-picker', quiet=0.6)


def reveal_control(s, title, name, tries=40):
    """Wheels the page of a Control Center window until control |name| is
    fully in view (a small screen at a large interface size)."""
    win = s.find('^' + re.escape(title) + '$', 3)
    if not win:
        raise SkipShot(f'no {title} window')
    g = s.geom(win)
    for _ in range(tries):
        r = s.layout(title).get(name)
        if r and r[4]:
            return r
        down = not r or r[1] > g['h'] // 2
        steps = 1 if not r else max(1, min(12, abs(r[1] - g['h'] // 2) // 60))
        s.xdo('mousemove', str(g['x'] + g['w'] - 40), str(g['y'] + g['h'] // 2),
              'click', '--repeat', str(steps), '--delay', '15', '5' if down else '4')
        time.sleep(0.2)
    raise SkipShot(f'{title}: cannot scroll to {name!r}')


@scene('Accounts', [('user-accounts', 'Control Center, Users & Avatar: the users of this computer, their type '
                                      'and state'),
                    ('user-add', 'Control Center, Users & Avatar: adding a user (the user name follows the full '
                                 'name; the strength of the password)')], needs=['sde-settings'])
def user_accounts(s):
    """The fixture's AccountsService: Alex, Mia and Sam."""
    if not any(p in ACCOUNT_PAGES for p in s.run_ctx.pages):
        raise SkipShot('no account page in this build')
    settings_page(s, 'accounts')
    if not s.control('Control Center', 'user:mia', 4):
        raise SkipShot('no AccountsService (the service fixture) in this session')
    s.shot('user-accounts', quiet=0.6)
    try:
        reveal_control(s, 'Control Center', 'Add User...')
        s.press('Control Center', 'Add User...')
        s.control('Control Center', "New user's full name", 3)
        for name, text in (("New user's full name", 'Jordan Lee'), ("New user's password", 'a long sunny walk'),
                           ("Confirm the new user's password", 'a long sunny walk')):
            reveal_control(s, 'Control Center', name)
            s.press('Control Center', name)
            s.type(text)
        s.park_pointer()
        s.shot('user-add', quiet=0.6)
    except SkipShot as e:
        s.skip('user-add', str(e))


# ================================================================== output


def write_outputs(run):
    """index.json, the contact sheets and README.md for everything in the
    output folder: the images of this run and those of earlier runs into
    the same folder (other sets or scenes) that are still there."""
    out = run.out
    # The sets of this run first when it renamed them (--resolution, --scale).
    known = list(run.sets) + [t for t in SETS if t.name not in {s.name for s in run.sets}]
    set_names = [t.name for t in known]
    images, skipped = list(run.index), [{'set': s, 'name': n, 'reason': r} for s, n, r in run.skipped]
    try:
        old = json.loads((out / 'index.json').read_text())
    except (OSError, ValueError):
        old = {}
    now = {(e['set'], e['name']) for e in images}
    images += [e for e in old.get('images', []) if (e.get('set'), e.get('name')) not in now and
               e.get('set') in set_names and (out / e.get('file', '-')).is_file()]
    have = {(e['set'], e['name']) for e in images}
    redone = {(s['set'], s['name']) for s in skipped}
    skipped += [s for s in old.get('skipped', []) if (s.get('set'), s.get('name')) not in redone]
    skipped = [s for s in skipped if (s['set'], s['name']) not in have]
    order = {name: i for i, (_, name, _) in enumerate(run.catalogue)}
    images.sort(key=lambda e: (set_names.index(e['set']), order.get(e['name'], len(order)), e['name']))
    skipped.sort(key=lambda s: (set_names.index(s['set']) if s['set'] in set_names else 99,
                                order.get(s['name'], len(order))))
    sets = [t for t in known if any(e['set'] == t.name for e in images)]
    meta = {
        'title': 'SDE development screenshots',
        'version': (ROOT / 'VERSION').read_text().strip() if (ROOT / 'VERSION').exists() else '',
        'programs': program_version(run),
        'date': dt.date.today().isoformat(),
        'sets': [{'name': t.name, 'label': t.label, 'theme': t.theme, 'size': t.size, 'scale': t.scale}
                 for t in sets],
        'images': images,
        'skipped': skipped,
    }
    (out / 'index.json').write_text(json.dumps(meta, indent=2) + '\n')
    sheets = contact_sheets(run, sets, images)
    (out / 'README.md').write_text(readme(meta, sets, images, skipped, sheets))
    info(f'wrote {out / "README.md"}, index.json and {len(sheets)} contact sheets ({len(images)} images)')


def readme(meta, sets, images, skipped, sheets):
    """The gallery: the sets, the themes side by side, then every view by
    component (the default set, with links to the others)."""
    by = {(e['set'], e['name']): e for e in images}
    sets_of = {}
    for e in images:
        sets_of.setdefault(e['name'], []).append(e['set'])
    main = 'graphite-light' if any(t.name == 'graphite-light' for t in sets) else (sets[0].name if sets else '')
    version = meta['programs'] or meta['version']
    title = ' '.join(filter(None, ['Selviana Desktop Environment', pretty_version(version)]))
    lines = [f'# {title}: development screenshots', '',
             f'Captured on {meta["date"]}' + (f' from SDE `{version}`' if version else '') +
             ' with `tools/showcase.py`: the real programs on a virtual X11 '
             'display, with original sample content. Network, sound, Bluetooth, battery, drives, weather, '
             'updates and drivers are test data; "Alex Tester" is a sample account. Click an image for '
             'full size.', '', '| Set | Theme | Screen | Images |', '| --- | --- | --- | --- |']
    for t in sets:
        n = sum(1 for e in images if e['set'] == t.name)
        sheet = f' · [contact sheet]({sheets[t.name]})' if t.name in sheets else ''
        lines.append(f'| `{t.name}` | {t.label} | {t.size} at {t.scale} % | {n}{sheet} |')
    lines.append('')
    theme_sets = {t.name for t in sets if t.scope == 'themes' or t.family != 'graphite'}
    compare = []
    for e in images:
        if e['name'] not in compare and theme_sets & set(sets_of[e['name']]):
            compare.append(e['name'])
    if compare and len(sets) > 1:
        heads = [by[next((s, n) for s in sets_of[n])]['component'] for n in compare]
        lines += ['## Themes side by side', '', '| | ' + ' | '.join(heads) + ' |',
                  '| --- |' + ' --- |' * len(compare)]
        for t in sets:
            cells = [f'[![{t.label}: {h}]({by[(t.name, n)]["file"]})]({by[(t.name, n)]["file"]})'
                     if (t.name, n) in by else '' for n, h in zip(compare, heads)]
            lines.append(f'| {t.label} | ' + ' | '.join(cells) + ' |')
        lines.append('')
    components = []
    for e in images:
        if e['component'] not in components:
            components.append(e['component'])
    for comp in components:
        shown, seen = [], set()
        for e in images:
            if e['component'] == comp and e['name'] not in seen:
                seen.add(e['name'])
                shown.append(by.get((main, e['name']), e))
        lines += [f'## {comp}', '', '| | |', '| --- | --- |']
        for i in range(0, len(shown), 2):
            cells = []
            for e in shown[i:i + 2]:
                others = [by[(s, e['name'])] for s in [t.name for t in sets] if s != e['set'] and (s, e['name']) in by]
                also = ('<br>Also: ' + ' · '.join(f'[{o["set"]}]({o["file"]})' for o in others)) if others else ''
                note = '' if e['set'] == main else f' ({e["set"]})'
                cells.append(f'[![{e["caption"]}]({e["file"]})]({e["file"]})<br>{e["caption"]}{note}{also}')
            cells += [''] * (2 - len(cells))
            lines.append('| ' + ' | '.join(cells) + ' |')
        lines.append('')
    if skipped:
        lines += ['## Not captured', '', 'Views this build does not have yet, or that failed this time (the '
                  'reason is in the log of the showcase run).', '']
        for s in skipped:
            lines.append(f'- `{s["set"]}` {s["name"]}: {s["reason"]}')
        lines.append('')
    return '\n'.join(lines)


def program_version(run):
    try:
        r = sp.run([shutil.which('sde-panel', path=':'.join(str(d) for d in run.args.bin_dirs) + ':' +
                                 os.environ.get('PATH', '')) or 'sde-panel', '--version'],
                   stdout=sp.PIPE, stderr=sp.DEVNULL, text=True, timeout=10)
        # "sde-panel (Selviana Desktop Environment) 0.1.0~alpha52"
        first = (r.stdout.splitlines() or [''])[0]
        m = re.search(r'\b(\d+\.\d+[\w.~+-]*)', first)
        return m.group(1) if m else first.strip()
    except (OSError, sp.SubprocessError):
        return ''


def pretty_version(v):
    """0.1.0~alpha52 -> 0.1 Alpha 52 (as the About pages write it)."""
    m = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)~(alpha|beta|rc)(\d+)', v or '')
    if not m:
        return v or ''
    major, minor, patch, kind, n = m.groups()
    return f'{major}.{minor}' + (f'.{patch}' if patch != '0' else '') + \
        f' {"RC" if kind == "rc" else kind.capitalize()} {n}'


def contact_sheets(run, sets, images):
    """One labelled contact sheet per set (ImageMagick montage)."""
    montage = shutil.which('montage')
    if not montage:
        warn('ImageMagick montage not found: no contact sheets')
        return {}
    out = {}
    for t in sets:
        files = [e for e in images if e['set'] == t.name]
        if not files:
            continue
        name = f'contact-{t.name}.jpg'
        cols = 4 if len(files) > 8 else max(1, min(4, len(files)))
        args = [montage, '-background', '#1f2328', '-fill', '#e6edf3', '-font', 'DejaVu-Sans', '-pointsize', '13',
                '-title', f'SDE showcase - {t.label}']
        for e in files:
            num = Path(e['file']).name.split('-', 1)[0]
            label = f'{num}  {e["caption"]}'
            if len(label) > 56:  # what fits under a 420 px tile at 13 px
                label = label[:53].rsplit(' ', 1)[0].rstrip(',:;') + '...'
            args += ['-label', label, str(run.out / e['file'])]
        args += ['-tile', f'{cols}x', '-geometry', '420x+10+10', '-quality', '86', str(run.out / name)]
        r = sp.run(args, stdout=sp.PIPE, stderr=sp.PIPE, text=True)
        if r.returncode != 0:
            # Without the DejaVu font name: ImageMagick's default font.
            args = [a for i, a in enumerate(args) if a != '-font' and (i == 0 or args[i - 1] != '-font')]
            r = sp.run(args, stdout=sp.PIPE, stderr=sp.PIPE, text=True)
        if r.returncode == 0:
            out[t.name] = name
        else:
            warn(f'contact sheet {name}: {r.stderr.strip()[:200]}')
    return out


def make_zip(run, path):
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
        base = path.stem
        for f in sorted(run.out.rglob('*')):
            rel = f.relative_to(run.out)
            if f.is_dir() or rel.parts[0] in ('logs', '.cache') or f == path:
                continue
            z.write(f, f'{base}/{rel}')
    info(f'wrote {path} ({path.stat().st_size // 1024} KiB)')

# ==================================================================== main


def free_display(start):
    n = start
    while Path(f'/tmp/.X{n}-lock').exists() or Path(f'/tmp/.X11-unix/X{n}').exists():
        n += 1
    return n


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin-dir', dest='bin_dirs', action='append', type=Path, default=[],
                    help='directory with the SDE programs (repeatable; default /usr/bin)')
    ap.add_argument('--build-dir', type=Path,
                    help='a CMake build tree: its programs, test fixtures and themes')
    ap.add_argument('--data-dir', type=Path, default=Path('/usr/share/sde'),
                    help='SDE data (openbox/rc.xml, compositor/...): /usr/share/sde or the source data/')
    ap.add_argument('--test-dir', dest='test_dirs', action='append', type=Path, default=[],
                    help='QA fixtures (sde-panel-service-fixture, sde-lock-ui-fixture, ...), e.g. dist/test-tools')
    ap.add_argument('--themes-dir', type=Path, help='SDE themes of a build tree (build/themes)')
    ap.add_argument('--prefix', dest='prefixes', action='append', type=Path, default=[],
                    help='another install prefix with programs (bin/) and data (share/), e.g. a Liria build')
    ap.add_argument('--output-dir', type=Path, default=Path('dist/showcase'))
    ap.add_argument('--theme', help='sets to capture (comma-separated names or globs, e.g. graphite-light,'
                    "'glass-*'); default: all")
    ap.add_argument('--size', help='only sets of this size, e.g. 1366x768')
    ap.add_argument('--resolution', help='capture the selected sets at this screen size instead, e.g. '
                    '1024x600 (the set names get the size and scale)')
    ap.add_argument('--scale', type=int, help='capture the selected sets at this interface scale, in percent')
    ap.add_argument('--scope', choices=('full', 'themes', 'hero'),
                    help="scenes of this scope in every set, e.g. 'full' for all scenes in the Fluent and "
                         "Glass sets (default: each set's own)")
    ap.add_argument('--effects', choices=('lite', 'full'), default='lite',
                    help='compositor effects: lite (XRender with data/compositor/sde.conf, as in CI) or full '
                         '(OpenGL with the configuration sde-session writes for a real GPU: blur, translucency '
                         'and the Liquid Glass shader; needs sde-compositor)')
    ap.add_argument('--only', help='scenes or images (comma-separated globs, e.g. "start-menu*,control-center-*")')
    ap.add_argument('--zip', type=Path, help='also write one shareable zip')
    ap.add_argument('--display', type=int, default=140, help='first Xvfb display number to try')
    ap.add_argument('--no-compositor', action='store_true')
    ap.add_argument('--keep-home', action='store_true', help='keep the sample home folders (debugging)')
    ap.add_argument('--hold', action='store_true',
                    help='start the first selected set and wait (prints the environment to explore it)')
    ap.add_argument('--list', action='store_true', help='list the sets and images and exit')
    ap.add_argument('--strict', action='store_true', help='exit with an error when a scene failed')
    ap.add_argument('--jobs', type=int, default=1, help='sets captured at the same time (own displays)')
    a = ap.parse_args(argv)
    a.xdg_data_dirs = []
    if a.build_dir:
        b = a.build_dir.resolve()
        for d in ['src', 'src/niny', 'src/welcome', 'src/merkadu', 'cargo/release', 'rust/release']:
            if (b / d).is_dir():
                a.bin_dirs.append(b / d)
        if (b / 'tests').is_dir():
            a.test_dirs.append(b / 'tests')
        if not a.themes_dir and (b / 'themes').is_dir():
            a.themes_dir = b / 'themes'
    if not a.bin_dirs:
        a.bin_dirs = [Path('/usr/bin')]
    a.data_dir = a.data_dir.resolve()
    # Desktop files and icons: the source data/ (applications/, icons/) or
    # the installed share/ that holds sde/.
    if (a.data_dir / 'applications').is_dir():
        a.xdg_data_dirs.append(a.data_dir)
    elif a.data_dir.name == 'sde':
        a.xdg_data_dirs.append(a.data_dir.parent)
    for p in a.prefixes:
        if (p / 'share').is_dir():
            a.xdg_data_dirs.append((p / 'share').resolve())
    a.update_helpers = a.data_dir / 'update' if (a.data_dir / 'update/check.py').exists() else None
    return a


def selected_sets(a):
    pats = [p.strip() for p in (a.theme or '').split(',') if p.strip()]
    out = []
    for t in SETS:
        if pats and not any(fnmatch.fnmatch(t.name, p) for p in pats):
            continue
        if a.size and t.size != a.size:
            continue
        if a.resolution or a.scale:
            # The same theme at another size or scale, under its own name.
            size, scale = a.resolution or t.size, a.scale or t.scale
            base = t.name.split('-1920x1080')[0]
            t = ThemeSet(f'{base}-{size}-{scale}', t.family, t.mode, size, scale, t.scope,
                         f'{t.label.split(",")[0]}, {size} at {scale} %')
        if a.scope:
            t = ThemeSet(t.name, t.family, t.mode, t.size, t.scale, a.scope, t.label)
        out.append(t)
    return out


def discover(run):
    """Run-wide facts the catalogue depends on (Control Center pages)."""
    exe = None
    for d in run.args.bin_dirs:
        if (Path(d) / 'sde-settings').exists():
            exe = str(Path(d) / 'sde-settings')
            break
    exe = exe or shutil.which('sde-settings')
    if exe:
        try:
            r = sp.run([exe, '--list-pages'], stdout=sp.PIPE, stderr=sp.DEVNULL, text=True, timeout=20,
                       env=dict(os.environ, DISPLAY=''))
            run.pages = [p for p in r.stdout.split() if re.fullmatch(r'[a-z0-9-]+', p)]
        except (OSError, sp.SubprocessError):
            pass


def run_set(run, tset, display):
    # A busy machine can delay the first programs: one more try, fresh.
    for attempt in (1, 2):
        if attempt == 2:
            # The same display once its server is gone (the others are
            # spoken for by the other sets), else one well past them.
            for _ in range(10):
                if free_display(display) == display:
                    break
                time.sleep(0.5)
            else:
                display = free_display(display + 100)
        s = Session(run, tset, display)
        try:
            try:
                s.start()
            except Exception as e:  # noqa: BLE001 - reported, the next set may work
                if attempt == 1 and not run.stop.is_set():
                    warn(f'{tset.name}: the session did not start ({e}); starting it again')
                    s.log_name = f'{tset.name}-first-start'  # its logs, beside the second's
                    continue
                warn(f'{tset.name}: the session did not start: {e}')
                run.failed.append((tset.name, 'session', str(e)))
                return False
            return run_session(run, tset, s)
        finally:
            s.close()
    return False


def run_session(run, tset, s):
    if run.args.hold:
        print('\n'.join(f'export {k}={sh_quote(v)}' for k, v in sorted(s.env.items())
                        if k in ('DISPLAY', 'HOME', 'PATH', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_CONFIG_HOME',
                                 'XDG_DATA_DIRS', 'XDG_RUNTIME_DIR', 'SDE_PANEL_QA_DIR', 'LD_PRELOAD',
                                 'NSS_WRAPPER_PASSWD', 'NSS_WRAPPER_GROUP', 'XDG_CONFIG_DIRS', 'TZ',
                                 'XDG_CACHE_HOME', 'XDG_STATE_HOME', 'XDG_DATA_HOME', 'GSETTINGS_SCHEMA_DIR',
                                 'SDE_UPDATE_ROOT', 'PYTHONPATH', 'SDE_UPDATE_HELPERS', 'SDE_CLOCK_FIXTURE',
                                 'SDE_WEATHER_FIXTURE', 'SDE_POWER_SUPPLY_DIR', 'SDE_BACKLIGHT_DIR',
                                 'SDE_DRIVERS_DEMO', 'SDE_PANEL_FAKE_TOUCHPAD', 'DBUS_SYSTEM_BUS_ADDRESS',
                                 'LANG', 'LC_ALL', 'USER', 'LOGNAME', 'NO_AT_BRIDGE')), flush=True)
        (run.out / 'hold.env').write_text('\n'.join(f'export {k}={sh_quote(v)}' for k, v in sorted(s.env.items())
                                                    if '\n' not in v) + '\n')
        info(f'holding; environment in {run.out / "hold.env"}; stop with Ctrl+C')
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass
        return True
    t0 = time.time()
    for sc in SCENES:
        if run.stop.is_set():
            break
        if tset.scope not in sc.scopes or not run.selected(sc):
            continue
        missing = [n for n in sc.needs if not s.has(n)]
        if missing:
            for name, _ in run.images_of(sc):
                if run.selected(sc, name):
                    run.skipped.append((tset.name, name, f'{", ".join(missing)} not installed'))
            warn(f'{tset.name}: {sc.name} skipped ({", ".join(missing)} not installed)')
            continue
        s.current, s.captured, s.keep_bubbles, s.skipped_names = sc, [], False, set()
        t1 = time.time()
        reason = 'not shown by this build'
        try:
            sc.fn(s)
        except SkipShot as e:
            warn(f'{tset.name}: {sc.name}: {e}')
            reason = str(e)
        except Exception as e:  # noqa: BLE001 - one broken scene must not stop the gallery
            warn(f'{tset.name}: {sc.name} failed: {e}')
            traceback.print_exc()
            run.failed.append((tset.name, sc.name, str(e)))
            reason = f'failed: {e}'
            try:
                s.save_debug(f'failed-{sc.name}')
            except (OSError, sp.SubprocessError, RuntimeError):
                pass
        finally:
            for name, _ in run.images_of(sc):
                if name not in s.captured and name not in s.skipped_names and run.selected(sc, name):
                    run.skipped.append((tset.name, name, reason))
            try:
                s.clean()
            except Exception as e:  # noqa: BLE001
                warn(f'{tset.name}: cleaning up after {sc.name}: {e}')
        info(f'{tset.name}: {sc.name} {time.time() - t1:.1f} s')
    info(f'{tset.name}: done in {time.time() - t0:.0f} s')
    return True


def sh_quote(v):
    return "'" + str(v).replace("'", "'\\''") + "'"


def interrupted(sig, frame):
    raise KeyboardInterrupt(f'signal {sig}')


def main(argv=None):
    a = parse_args(argv)
    # A CI timeout or a kill stops the sessions too (try/finally).
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    run = Run(a)
    discover(run)
    run.number()
    sets = run.sets = selected_sets(a)
    if a.list:
        for t in SETS:
            print(f'set {t.name}: {t.label}, {t.size} @ {t.scale} % ({t.scope})')
        for sc, name, caption in run.catalogue:
            print(f'{run.numbers[name]}.png  [{",".join(sc.scopes)}] {sc.component}: {caption}')
        return 0
    if not sets:
        print('showcase: no set matches --theme/--size', file=sys.stderr)
        return 2
    for tool in ('Xvfb', 'xdotool', 'xwininfo', 'xprop', 'dbus-daemon', 'gdbus', 'dbus-send'):
        if not shutil.which(tool):
            print(f'showcase: {tool} is required (Debian: xvfb xdotool x11-utils dbus libglib2.0-bin)',
                  file=sys.stderr)
            return 2
    run.out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    # The sample pictures once (the sessions copy them, maybe in parallel).
    showcase_content.make_pictures(run.out / '.cache/pictures')
    displays, d = [], a.display
    for t in sets:
        d = free_display(d)
        displays.append(d)
        d += 1
    if a.hold:
        run_set(run, sets[0], displays[0])
        return 0
    jobs = max(1, min(a.jobs, len(sets)))
    try:
        if jobs == 1:
            results = [run_set(run, t, d) for t, d in zip(sets, displays)]
        else:
            # Sessions are independent (own display, bus and home): run
            # several at once; each waits mostly on its programs.
            with ThreadPoolExecutor(jobs) as pool:
                futures = [pool.submit(run_set, run, t, d) for t, d in zip(sets, displays)]
                try:
                    results = [f.result() for f in futures]
                except KeyboardInterrupt:
                    run.stop.set()  # the sessions end after their current scene
                    raise
    except KeyboardInterrupt:
        warn('interrupted; writing what was captured')
        results = [True]
    started = sum(1 for r in results if r)
    write_outputs(run)
    if a.zip:
        make_zip(run, a.zip)
    info(f'{len(run.index)} images, {len(run.skipped)} skipped, {len(run.failed)} failed, '
         f'{time.time() - t0:.0f} s')
    for s, n, r in run.skipped:
        print(f'showcase: skipped {s}/{n}: {r}')
    if not started:
        return 1
    return 1 if a.strict and run.failed else 0


if __name__ == '__main__':
    sys.exit(main())
