#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Context menu latency smoke test of a built Liria on Xvfb (0.2.0 Alpha 3.1).

  tools/liria-menu-smoke.py --prefix <built Liria prefix> [--themes DIR]
                            [--repeat 3] [--slow] [--compositor] [--json FILE]

Starts Xvfb (1366x768, 96 dpi), a D-Bus session bus, xsettingsd and a window
manager when they are installed, a compositor with --compositor, and Liria
with the SDE GTK theme in a throw-away home folder. Like a user it right-
clicks (XTest: press, 60 ms, release) files of many types (a folder, a
picture, a PDF, a Debian package, an AppImage, archives, text, a script and
a program), the empty part of a folder, a row of the list view, a place of
the sidebar, a card of the Home view and a file among 1 000 selected ones,
opens the main menu, moves the pointer over the items of a context menu
and rests on a submenu. Liria records with LIRIA_SDE_QA_FRAMES
(src/liria-sde-qa.c) the X time of every click, every menu it maps, every
frame of a menu window and every pointer event that moved the highlight,
so the times are measured from the click (X server time) to the end of the
first frame that painted the menu:

  menu       from the press or release that opens the menu (Liria opens
             the menu of a file in the icon view and of a place in the
             sidebar on release: a right-button drag moves files) to the
             menu painted; "press" is from the press, "handler" the time
             Liria spent in that event (building and showing the menu)
  highlight  from a pointer motion that reaches another item to the frame
             that shows the highlight there, and the frames each item
             change took (a fade of the highlight takes several frames)
  submenu    from the moment the pointer reached an item with a submenu
             to the submenu painted (GTK opens submenus after 225 ms)

The first menu after the start is measured on its own; then every file is
right-clicked once without measuring (the first menu of a type reads its
applications' icons and data from the disk) before the measured rounds.
With --slow, Xvfb and Liria share one CPU core with a busy process (taskset;
the test machine is not throttled), as on a slow computer. The test fails
when a menu takes longer than --max-menu ms (default 250, 500 with --slow;
four times that for the first menu), a highlight longer than
--max-highlight ms (default 100, 200 with --slow) at the 95th percentile,
an item change takes more than 3 frames on average (a fade), or Liria
prints critical warnings. The limits are generous for shared CI machines;
on an idle computer menus take a few tens of ms.
--json writes every measurement. Exit status 0 = passed, 1 = failed, 77 =
skipped (no Xvfb, xdotool or Liria, or a Liria without the menu recorder).
"""
import argparse
import ctypes
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
import zlib
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GDK_KEY_PRESS, GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE = 8, 4, 7


# ------------------------------------------------------------------ files

def png(width, height):
    """A small PNG picture (pure Python)."""
    raw = b"".join(b"\0" + b"".join(bytes(((x * 7 + y) % 256, (y * 5) % 256, 160)) for x in range(width))
                   for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def pdf():
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>"]
    out = b"%PDF-1.4\n"
    offsets = []
    for i, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    out += b"".join(b"%010d 00000 n \n" % o for o in offsets)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref)
    return out


def deb():
    """A Debian package as ar(1) writes it (members are not unpacked)."""
    def member(name, data):
        header = b"%-16s%-12d%-6d%-6d%-8s%-10d`\n" % (name, 0, 0, 0, b"100644", len(data))
        return header + data + (b"\n" if len(data) % 2 else b"")
    return b"!<arch>\n" + member(b"debian-binary", b"2.0\n") + member(b"control.tar.xz", b"\0" * 32) \
        + member(b"data.tar.xz", b"\0" * 32)


def make_home(home):
    for name in ("Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos"):
        (home / name).mkdir(parents=True, exist_ok=True)
    mixed = home / "Mixed"
    (mixed / "Projects").mkdir(parents=True)
    (mixed / "photo.png").write_bytes(png(96, 64))
    (mixed / "scan.pdf").write_bytes(pdf())
    (mixed / "tool_1.0_amd64.deb").write_bytes(deb())
    appimage = mixed / "Viewer-x86_64.AppImage"
    # An ELF header with the AppImage type 2 magic ("AI\x02" at byte 8).
    appimage.write_bytes(b"\x7fELF\x02\x01\x01\x00AI\x02" + b"\0" * 501)
    appimage.chmod(0o755)
    with zipfile.ZipFile(mixed / "archive.zip", "w") as z:
        z.writestr("readme.txt", "zip\n")
    with tarfile.open(mixed / "backup.tar.gz", "w:gz") as t:
        info = tarfile.TarInfo("readme.txt")
        info.size = 4
        t.addfile(info, __import__("io").BytesIO(b"tar\n"))
    (mixed / "notes.txt").write_text("Notes\n")
    script = mixed / "run.sh"
    script.write_text("#!/bin/sh\necho run\n")
    script.chmod(0o755)
    program = mixed / "program"
    shutil.copy2("/bin/true", program)
    (mixed / "page.html").write_text("<html><body>page</body></html>\n")
    many = home / "Files-1000"
    many.mkdir()
    kinds = (".txt", ".pdf", ".zip", ".odt", ".html", ".mp3", ".csv", ".sh")
    for i in range(1000):
        (many / ("document-%04d%s" % (i, kinds[i % len(kinds)]))).write_bytes(b"x" * (i % 97))
    (home / ".config/sde").mkdir(parents=True, exist_ok=True)
    (home / ".config/sde/liria.conf").write_text("[ribbon]\nmode=full\n")
    (home / ".config/gtk-3.0").mkdir(parents=True, exist_ok=True)
    (home / ".config/gtk-3.0/bookmarks").write_text("")
    (home / ".config/user-dirs.dirs").write_text("".join(
        'XDG_%s_DIR="$HOME/%s"\n' % (key, name) for key, name in
        (("DESKTOP", "Desktop"), ("DOCUMENTS", "Documents"), ("DOWNLOAD", "Downloads"), ("MUSIC", "Music"),
         ("PICTURES", "Pictures"), ("VIDEOS", "Videos"))))
    return mixed


# ------------------------------------------------------------------- X11

class XEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("event", ctypes.c_ulong), ("window", ctypes.c_ulong),
                ("override_redirect", ctypes.c_int), ("pad", ctypes.c_long * 20)]


class X:
    """XTest input and the windows mapped on the screen (libX11 and libXtst
    through ctypes, no extra Python modules)."""

    def __init__(self, display):
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xtst = ctypes.CDLL("libXtst.so.6")
        x11 = self.x11
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XSelectInput.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_long]
        x11.XPending.argtypes = [ctypes.c_void_p]
        x11.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.POINTER(XEvent)]
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x11.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long,
                                           ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
                                           ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
                                           ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                                           ctypes.POINTER(ctypes.c_void_p)]
        x11.XFree.argtypes = [ctypes.c_void_p]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                                   ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.dpy = x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise SystemExit("cannot open display " + display)
        # SubstructureNotify on the root: MapNotify of every top-level window.
        x11.XSelectInput(self.dpy, x11.XDefaultRootWindow(self.dpy), 1 << 19)
        x11.XFlush(self.dpy)
        self.maps = []  # (monotonic time, window, override redirect)
        self.pos = (0, 0)

    def poll(self):
        event = XEvent()
        while self.x11.XPending(self.dpy):
            self.x11.XNextEvent(self.dpy, ctypes.byref(event))
            if event.type == 19:  # MapNotify
                self.maps.append((time.monotonic(), event.window, bool(event.override_redirect)))

    def move(self, x, y):
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(round(x)), int(round(y)), 0)
        self.x11.XFlush(self.dpy)
        self.pos = (x, y)

    def button(self, button, press):
        self.xtst.XTestFakeButtonEvent(self.dpy, button, 1 if press else 0, 0)
        self.x11.XFlush(self.dpy)

    def path(self, points, speed, rate=100):
        """Moves through |points| at |speed| pixels a second, |rate| motion
        events a second (like a hand)."""
        tick = time.monotonic()
        x, y = self.pos
        for tx, ty in points:
            n = max(1, int(((tx - x) ** 2 + (ty - y) ** 2) ** 0.5 / (speed / rate)))
            for i in range(1, n + 1):
                tick += 1.0 / rate
                delay = tick - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                self.move(x + (tx - x) * i / n, y + (ty - y) * i / n)
            x, y = tx, ty

    def cardinals(self, window, name):
        """A CARDINAL property of a window, or None."""
        atom = self.x11.XInternAtom(self.dpy, name.encode(), 0)
        kind, fmt = ctypes.c_ulong(), ctypes.c_int()
        count, after, data = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
        if self.x11.XGetWindowProperty(self.dpy, window, atom, 0, 32, 0, 0, ctypes.byref(kind), ctypes.byref(fmt),
                                       ctypes.byref(count), ctypes.byref(after), ctypes.byref(data)) != 0:
            return None
        if not data.value:
            return None
        values = [ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))[i] for i in range(count.value)] \
            if fmt.value == 32 else None
        self.x11.XFree(data)
        return values

    def close(self):
        self.x11.XCloseDisplay(self.dpy)


def cpu_ns(pid):
    try:
        return int(Path("/proc/%d/schedstat" % pid).read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def percentile(values, share):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * share))]


def icon_theme():
    return "Papirus" if Path("/usr/share/icons/Papirus").is_dir() else "Adwaita"


# ------------------------------------------------------------------ test

class Smoke:
    def __init__(self, args):
        self.args = args
        self.procs = []
        self.home = Path(tempfile.mkdtemp(prefix="liria-menu-"))
        self.width, self.height = (int(v) for v in args.size.split("x"))
        self.liria = None
        self.x = None
        self.qa = self.home / "qa-frames"
        self.offset = 0
        self.results = {}
        self.problems = []

    # -------------------------------------------------------------- setup
    def taskset(self, command):
        return (["taskset", "-c", "0"] + command) if self.args.slow else command

    def start(self):
        args = self.args
        self.mixed = make_home(self.home)
        dpi = 96 * args.scale // 100
        icons = icon_theme()
        self.env = env = dict(
            os.environ, HOME=str(self.home), DISPLAY=args.display, XDG_CONFIG_HOME=str(self.home / ".config"),
            XDG_DATA_HOME=str(self.home / ".local/share"), XDG_CACHE_HOME=str(self.home / ".cache"),
            XDG_RUNTIME_DIR=str(self.home / ".run"), GTK_THEME=args.theme, NO_AT_BRIDGE="1",
            GSETTINGS_BACKEND="memory", GSETTINGS_SCHEMA_DIR=str(args.prefix / "share/glib-2.0/schemas"),
            XDG_DATA_DIRS=str(args.prefix / "share") + ":/usr/local/share:/usr/share",
            LIRIA_SDE_QA_FRAMES=str(self.qa))
        (self.home / ".run").mkdir(mode=0o700)
        env.pop("SDE_SESSION_VERSION", None)
        if args.themes and (args.themes / args.theme).is_dir():
            (self.home / ".themes").mkdir()
            (self.home / ".themes" / args.theme).symlink_to(args.themes.resolve() / args.theme)
        (self.home / ".config/gtk-3.0/settings.ini").write_text(
            "[Settings]\ngtk-theme-name=%s\ngtk-icon-theme-name=%s\ngtk-font-name=Sans 10\n" % (args.theme, icons))
        if args.slow:
            # A busy process on the CPU that Xvfb and Liria get.
            self.procs.append(subprocess.Popen(["taskset", "-c", "0", sys.executable, "-c", "while True: pass"]))
        self.xvfb = subprocess.Popen(self.taskset(["Xvfb", args.display, "-screen", "0", args.size + "x24",
                                                   "-nolisten", "tcp", "-dpi", str(dpi)]),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.procs.append(self.xvfb)
        socket = Path("/tmp/.X11-unix/X" + args.display.lstrip(":").split(".")[0])
        end = time.time() + 10
        while not socket.exists() and time.time() < end:
            time.sleep(0.1)
        time.sleep(0.4)
        bus = subprocess.run(["dbus-launch", "--sh-syntax"], env=env, capture_output=True, text=True).stdout
        for line in bus.splitlines():
            if line.startswith("DBUS_SESSION_BUS_ADDRESS="):
                env["DBUS_SESSION_BUS_ADDRESS"] = line.split("=", 1)[1].rstrip(";").strip("'")
            if line.startswith("DBUS_SESSION_BUS_PID="):
                self.procs.append(int(line.split("=", 1)[1].rstrip(";")))
        if shutil.which("xsettingsd"):
            conf = self.home / "xsettingsd.conf"
            conf.write_text('Net/ThemeName "%s"\nNet/IconThemeName "%s"\nGtk/FontName "Sans 10"\n'
                            "Xft/DPI %d\nXft/Antialias 1\n" % (args.theme, icons, dpi * 1024))
            self.procs.append(subprocess.Popen(["xsettingsd", "-c", str(conf)], env=env,
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        elif args.scale != 100:
            env["GDK_DPI_SCALE"] = str(args.scale / 100)
        wm = shutil.which("sde-wm") or shutil.which("openbox")
        if wm:
            self.procs.append(subprocess.Popen([wm], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if args.compositor:
            command = [args.compositor_bin] + (["--config", str(args.compositor_config)]
                                               if args.compositor_config else ["--backend", "xrender"])
            self.procs.append(subprocess.Popen(self.taskset(command), env=env, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL))
        time.sleep(0.8)
        self.x = X(args.display)

    def stop(self):
        if self.liria is not None:
            self.liria.terminate()
            try:
                self.liria.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.liria.kill()
        if self.x is not None:
            self.x.close()
        for proc in reversed(self.procs):
            try:
                if isinstance(proc, int):
                    os.kill(proc, signal.SIGTERM)
                else:
                    proc.terminate()
                    proc.wait(timeout=5)
            except Exception:
                pass
        if self.args.keep:
            print("kept", self.home)
        else:
            shutil.rmtree(self.home, ignore_errors=True)

    # ------------------------------------------------------------ helpers
    def key(self, *keys):
        subprocess.run(["xdotool", "key", *keys], env=self.env, capture_output=True)

    def idle(self, quiet=0.4, timeout=30.0):
        """Waits until Liria has been idle (no 10 ms with more than 1 ms of
        CPU time) for |quiet| seconds."""
        pid = self.liria.pid
        last_cpu, last_busy = cpu_ns(pid), time.monotonic()
        end = last_busy + timeout
        while time.monotonic() < end:
            time.sleep(0.01)
            now = cpu_ns(pid)
            if now - last_cpu > 1000000:
                last_busy = time.monotonic()
            last_cpu = now
            if time.monotonic() - last_busy >= quiet:
                return
        self.x.poll()

    def records(self):
        """The QA records written since the last call."""
        with self.qa.open(errors="replace") as f:
            f.seek(self.offset)
            text = f.read()
        complete = text[:text.rfind("\n") + 1]
        self.offset += len(complete.encode())
        return [line.split("\t") for line in complete.splitlines()]

    def geometry(self):
        path = Path(str(self.qa) + ".geometry")
        for _ in range(3):
            path.unlink(missing_ok=True)
            os.kill(self.liria.pid, signal.SIGUSR2)
            end = time.monotonic() + 3
            while time.monotonic() < end:
                time.sleep(0.05)
                text = path.read_text(errors="replace") if path.exists() else ""
                if text.endswith("end\n"):
                    parts = {}
                    for line in text.splitlines():
                        f = line.split("\t")
                        if len(f) >= 6:
                            parts.setdefault(f[0], []).append(tuple(int(v) for v in f[1:5]) + tuple(f[5:]))
                    return parts
        raise SystemExit("Liria wrote no geometry; is it built with src/liria-sde-qa.c?")

    @staticmethod
    def centre(rect):
        return rect[0] + rect[2] / 2, rect[1] + rect[3] / 2

    def wait_menu(self, before, timeout=10.0):
        """Waits for a menu to be mapped and painted; returns the records
        since |before| (a list) and the painted menu's id."""
        records = list(before)
        end = time.monotonic() + timeout
        menu = None
        while time.monotonic() < end:
            records += self.records()
            for r in records:
                if r[0] == "menu-map" and menu is None:
                    menu = r[2]
                if r[0] == "menu-frame" and menu is not None and r[5] == menu and int(r[4]) > 0:
                    return records, menu
            time.sleep(0.005)
        return records, None

    def close_menus(self):
        self.key("Escape")
        time.sleep(0.05)
        self.key("Escape")
        self.idle(0.3)
        self.records()

    def go(self, path):
        self.key("ctrl+l")
        time.sleep(0.3)
        self.key("ctrl+a")
        subprocess.run(["xdotool", "type", "--delay", "2", str(path)], env=self.env, capture_output=True)
        time.sleep(0.2)
        self.key("Return")
        self.idle(0.6, 60)

    # ------------------------------------------------------------- clicks
    def click(self, name, point, button=3, hold=0.06, expect_menu=True, record=True):
        """Clicks at |point| and measures the menu that opens (kept in the
        results when |record|)."""
        self.records()
        self.x.poll()
        maps0 = len(self.x.maps)
        cpu0 = cpu_ns(self.liria.pid)
        self.x.move(*point)
        time.sleep(0.05)
        self.x.button(button, True)
        time.sleep(hold)
        self.x.button(button, False)
        records, menu = self.wait_menu([])
        self.x.poll()
        if not expect_menu:
            return None
        if menu is None:
            self.problems.append("%s: no menu appeared" % name)
            return None
        time.sleep(0.15)
        records += self.records()
        result = self.analyse_menu(records, menu, button)
        maps = [m for m in self.x.maps[maps0:] if m[2]]
        if maps:
            # When an X client saw the menu window mapped (a check that does
            # not depend on Liria's recorder).
            result["xmap"] = round(maps[0][0] * 1000 - result["trigger_x"], 1)
            extents = self.x.cardinals(maps[0][1], "_GTK_FRAME_EXTENTS")
            result["frame_extents"] = extents
        result["cpu"] = round((cpu_ns(self.liria.pid) - cpu0) / 1e6, 1)
        if record:
            self.results.setdefault(name, []).append(result)
        return result

    def analyse_menu(self, records, menu, button):
        press = release = None
        for r in records:
            if r[0] == "input" and int(r[2]) == GDK_BUTTON_PRESS and int(r[5]) == button and press is None:
                press = r
            if r[0] == "input" and int(r[2]) == GDK_BUTTON_RELEASE and int(r[5]) == button and release is None:
                release = r
        mapped = next(r for r in records if r[0] == "menu-map" and r[2] == menu)
        frame = next(r for r in records if r[0] == "menu-frame" and r[5] == menu and int(r[4]) > 0)
        painted = (int(frame[1]) + int(frame[2]) + int(frame[3])) / 1000.0
        map_time = int(mapped[1])
        trigger = None
        for r in (press, release):
            if r is not None and int(r[1]) <= map_time <= int(r[1]) + int(r[3]):
                trigger = r
        trigger = trigger or release or press
        result = {"menu": round(painted - int(trigger[4]), 1),
                  "press": round(painted - int(press[4]), 1) if press else None,
                  "handler": round(int(trigger[3]) / 1000.0, 1),
                  "paint": round(painted - map_time / 1000.0, 1),
                  "on": "release" if trigger is release else "press",
                  "items": int(mapped[3]), "first": mapped[6] if len(mapped) > 6 else "-",
                  "trigger_x": int(trigger[4])}
        return result

    # -------------------------------------------------------------- hover
    def hover(self, name, items, menu):
        """Moves down and up over |items| (menu-item records of one menu);
        returns the highlight latencies."""
        rects = [r for r in items if r[4] != "-" and r[5] == menu]
        if len(rects) < 3:
            self.problems.append("%s: menu items not found" % name)
            return
        rects.sort(key=lambda r: r[1])
        x = rects[0][0] + rects[0][2] * 0.4
        points = [(x, r[1] + r[3] / 2) for r in rects]
        self.x.move(*points[0])
        self.idle(0.3)
        self.records()
        cpu0 = cpu_ns(self.liria.pid)
        start = time.monotonic()
        self.x.path(points[1:] + points[::-1][1:], self.args.speed)
        moved = time.monotonic() - start
        self.idle(0.3)
        cpu = (cpu_ns(self.liria.pid) - cpu0) / 1e9
        records = self.records()
        selects = [(int(r[1]), int(r[3]), r[4]) for r in records if r[0] == "menu-select" and r[2] == menu]
        frames = [(int(r[1]), int(r[1]) + int(r[2]) + int(r[3])) for r in records
                  if r[0] == "menu-frame" and r[2] and r[5] == menu]
        latencies = []
        for handled, xtime, label in selects:
            after = [end for begin, end in frames if end >= handled]
            if after:
                latencies.append(min(after) / 1000.0 - xtime)
        result = {"changes": len(selects), "frames": len(frames),
                  "frames_per_change": round(len(frames) / max(1, len(selects)), 2),
                  "p50": round(percentile(latencies, 0.5), 1), "p95": round(percentile(latencies, 0.95), 1),
                  "max": round(max(latencies, default=0), 1), "cpu%": round(100 * cpu / max(moved, 0.01), 1)}
        self.results.setdefault(name, []).append(result)

    def submenu(self, name, items, menu, labels):
        """Rests on the first item of |labels| that has a submenu; returns
        the time from reaching it to the submenu painted."""
        target = None
        for wanted in labels:
            target = next((r for r in items if r[-1] == menu and r[4].replace("_", "") == wanted), None)
            if target:
                break
        if target is None:
            self.problems.append("%s: none of %s in the menu" % (name, ", ".join(labels)))
            return
        self.records()
        self.x.path([self.centre(target)], self.args.speed)
        records, sub = [], None
        end = time.monotonic() + 3
        while time.monotonic() < end and sub is None:
            records += self.records()
            sub = next((r[2] for r in records if r[0] == "menu-map" and r[2] != menu), None)
            time.sleep(0.01)
        if sub is None:
            self.problems.append("%s: no submenu opened" % name)
            return
        time.sleep(0.15)
        records += self.records()
        # The moment the pointer reached the item (the path crosses others).
        select = next((r for r in records if r[0] == "menu-select" and r[2] == menu and r[4] == target[4]), None)
        frame = next((r for r in records if r[0] == "menu-frame" and r[5] == sub and int(r[4]) > 0), None)
        mapped = next(r for r in records if r[0] == "menu-map" and r[2] == sub)
        if select is None or frame is None:
            self.problems.append("%s: submenu timing incomplete" % name)
            return
        painted = (int(frame[1]) + int(frame[2]) + int(frame[3])) / 1000.0
        self.results.setdefault(name, []).append({"submenu": round(painted - int(select[3]), 1),
                                                  "paint": round(painted - int(mapped[1]) / 1000.0, 1),
                                                  "items": int(mapped[3])})

    # ---------------------------------------------------------- scenarios
    def empty_point(self, g, kind="icon-view"):
        view = g[kind][0]
        return view[0] + view[2] * 0.85, view[1] + view[3] * 0.85

    def run(self):
        args = self.args
        self.qa.write_text("")
        log = open(self.home / "liria.log", "w")
        self.liria = subprocess.Popen(self.taskset([str(args.prefix / "bin/liria"), "--no-desktop",
                                                    str(self.mixed)]),
                                      env=self.env, cwd=str(self.home), stdout=log, stderr=subprocess.STDOUT)
        end = time.monotonic() + 30
        wid = None
        while time.monotonic() < end and not wid:
            out = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^Mixed$"], env=self.env,
                                 capture_output=True, text=True).stdout.split()
            wid = out[-1] if out else None
            time.sleep(0.1)
        if wid is None:
            raise SystemExit("Liria did not open its window; see %s" % (self.home / "liria.log"))
        subprocess.run(["xdotool", "windowmove", wid, "0", "0"], env=self.env)
        subprocess.run(["xdotool", "windowsize", wid, str(self.width), str(self.height - 30)], env=self.env)
        subprocess.run(["xdotool", "windowactivate", wid], env=self.env)
        self.idle(1.0)
        self.records()
        self.key("ctrl+1")
        self.idle(0.6)
        g = self.geometry()
        files = {f[4]: f for f in g.get("file", [])}
        empty = self.empty_point(g)

        def deselect():
            self.close_menus()
            self.x.move(*empty)
            self.x.button(1, True)
            self.x.button(1, False)
            self.idle(0.4)

        # The first menu after the start pays for loading its icons and styles.
        first = sorted(files)[0] if files else None
        if first:
            deselect()
            self.click("first menu (%s)" % first, self.centre(files[first]))
        order = ["Projects", "photo.png", "scan.pdf", "tool_1.0_amd64.deb", "Viewer-x86_64.AppImage",
                 "archive.zip", "backup.tar.gz", "notes.txt", "run.sh", "program", "page.html"]
        # Not measured: the first menu of each type reads its applications'
        # icons and data from the disk, which takes as long as the disk
        # (after a reboot of a shared CI machine, half a second).
        for name in order:
            if name in files:
                deselect()
                self.click("warm-up", self.centre(files[name]), record=False)
        for _ in range(args.repeat):
            for name in order:
                if name not in files:
                    self.problems.append("file %s not found in the view" % name)
                    continue
                deselect()
                self.click("file " + name, self.centre(files[name]))
            deselect()
            self.click("folder background", empty)
        # The highlight and a submenu, in the menu of a text file.
        deselect()
        self.click("menu for hover", self.centre(files["notes.txt"]))
        g = self.geometry()
        items = g.get("menu-item", [])
        menu = items[0][-1] if items else None
        if menu:
            self.hover("highlight (context menu)", items, menu)
            self.submenu("submenu (context menu)", items, menu, ("Open With", "Open With Other Application",
                                                                  "More Actions"))
        self.close_menus()
        # The main menu (header button) and its highlight.
        button = next((b for b in self.geometry().get("header-button", []) if b[4] == "Main Menu"), None)
        if button is None:
            self.problems.append("main menu button not found")
        else:
            for _ in range(args.repeat):
                self.close_menus()
                self.click("main menu", self.centre(button), button=1)
            items = self.geometry().get("menu-item", [])
            menu = items[0][-1] if items else None
            if menu:
                self.hover("highlight (main menu)", items, menu)
                self.submenu("submenu (main menu)", items, menu, ("View",))
            self.close_menus()
        # The list view.
        self.key("ctrl+2")
        self.idle(0.6)
        g = self.geometry()
        rows = {f[4]: f for f in g.get("file", [])}
        for _ in range(args.repeat):
            if "notes.txt" in rows and g.get("list-view"):
                self.close_menus()
                self.click("list view file", self.centre(rows["notes.txt"]))
        self.close_menus()
        self.key("ctrl+1")
        self.idle(0.6)
        # The sidebar.
        places = {p[4]: p for p in self.geometry().get("place", [])}
        place = places.get("Documents") or (next(iter(places.values())) if places else None)
        for _ in range(args.repeat):
            if place is None:
                self.problems.append("no sidebar place found")
                break
            self.close_menus()
            self.click("sidebar place", self.centre(place))
        self.close_menus()
        # 1 000 selected files.
        self.go(self.home / "Files-1000")
        self.key("ctrl+a")
        self.idle(0.6)
        g = self.geometry()
        shown = g.get("file", [])
        for _ in range(args.repeat):
            if not shown:
                self.problems.append("no files in Files-1000")
                break
            self.close_menus()
            self.click("1 000 selected files", self.centre(shown[len(shown) // 2]))
        self.close_menus()
        # The Home view: a Quick access card.
        self.key("alt+Home")
        self.idle(0.8)
        cards = self.geometry().get("card", [])
        for _ in range(args.repeat):
            if not cards:
                self.problems.append("no Home cards found")
                break
            self.close_menus()
            self.click("Home card", self.centre(cards[0]))
        self.close_menus()
        log.close()
        warnings = [line for line in (self.home / "liria.log").read_text(errors="replace").splitlines()
                    if "CRITICAL" in line]
        if warnings:
            self.problems.append("Liria printed %d critical warning(s), e.g. %s" % (len(warnings), warnings[0]))

    # ------------------------------------------------------------- report
    def report(self):
        args = self.args
        max_menu = args.max_menu or (500 if args.slow else 250)
        # The first menu after the start also loads icons and styles from
        # the disk.
        max_first = 4 * max_menu
        max_highlight = args.max_highlight or (200 if args.slow else 100)
        print("%-34s %8s %8s %8s %8s %6s" % ("menu", "ms", "press", "handler", "paint", "items"))
        for name, values in self.results.items():
            if "menu" not in values[0]:
                continue
            med = {k: statistics.median(v[k] for v in values) for k in ("menu", "handler", "paint")}
            press = [v["press"] for v in values if v["press"] is not None]
            print("%-34s %8.1f %8s %8.1f %8.1f %6d  (on %s)" % (
                name, med["menu"], "%.1f" % statistics.median(press) if press else "-", med["handler"],
                med["paint"], values[0]["items"], values[0]["on"]))
            worst = max(v["menu"] for v in values)
            limit = max_first if name.startswith("first menu") else max_menu
            if worst > limit:
                self.problems.append("%s: the menu took %.0f ms (limit %d ms)" % (name, worst, limit))
        for name, values in self.results.items():
            if "p95" in values[0]:
                v = values[-1]
                print("%-34s p50 %.1f ms, p95 %.1f ms, max %.1f ms, %d changes, %.2f frames each, cpu %.1f %%"
                      % (name, v["p50"], v["p95"], v["max"], v["changes"], v["frames_per_change"], v["cpu%"]))
                if v["p95"] > max_highlight:
                    self.problems.append("%s: the highlight took %.0f ms (95th percentile, limit %d ms)"
                                         % (name, v["p95"], max_highlight))
                if v["frames_per_change"] > 3:
                    self.problems.append("%s: %.1f frames per item change (a fade of the highlight)"
                                         % (name, v["frames_per_change"]))
            if "submenu" in values[0]:
                v = values[-1]
                print("%-34s %.1f ms (painted %.1f ms after it was mapped, %d items)"
                      % (name, v["submenu"], v["paint"], v["items"]))
                if v["submenu"] > 225 + max_menu:
                    self.problems.append("%s: took %.0f ms" % (name, v["submenu"]))
        if args.json:
            args.json.write_text(json.dumps(self.results, indent=1) + "\n")
        for problem in self.problems:
            print("FAIL " + problem)
        print("Liria menus: %s" % ("passed" if not self.problems else "%d problem(s)" % len(self.problems)))
        return 1 if self.problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path("/usr"))
    parser.add_argument("--display", default=":252")
    parser.add_argument("--size", default="1366x768")
    parser.add_argument("--scale", type=int, default=100)
    parser.add_argument("--themes", type=Path, default=ROOT / "build/themes",
                        help="folder with the built SDE GTK themes (tools/build-themes.py)")
    parser.add_argument("--theme", default="SDE-Graphite-Light-Blue")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--speed", type=float, default=400, help="pointer speed over menus (pixels a second)")
    parser.add_argument("--slow", action="store_true", help="share one CPU core with a busy process")
    parser.add_argument("--compositor", action="store_true", help="run a compositor (sde-compositor or picom)")
    parser.add_argument("--compositor-bin", default=shutil.which("sde-compositor") or "picom")
    parser.add_argument("--compositor-config", type=Path, help="its configuration (default: XRender, no file)")
    parser.add_argument("--max-menu", type=float, help="limit for a menu in ms (default 250, 500 with --slow)")
    parser.add_argument("--max-highlight", type=float,
                        help="limit for the 95th percentile of the highlight in ms (default 100, 200 with --slow)")
    parser.add_argument("--json", type=Path, help="write every measurement as JSON")
    parser.add_argument("--keep", action="store_true", help="keep the temporary home")
    args = parser.parse_args()
    for tool in ("Xvfb", "xdotool", "dbus-launch") + (("taskset",) if args.slow else ()):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    if not (args.prefix / "bin/liria").is_file():
        print("skipped: %s not found" % (args.prefix / "bin/liria"))
        return 77
    if b"menu-frame" not in (args.prefix / "bin/liria").read_bytes():
        print("skipped: this Liria has no menu recorder (src/liria-sde-qa.c of 0.2.0 Alpha 3.1)")
        return 77
    smoke = Smoke(args)
    try:
        smoke.start()
        smoke.run()
    finally:
        smoke.stop()
    return smoke.report()


if __name__ == "__main__":
    sys.exit(main())
