#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Liria responsiveness benchmark on Xvfb (0.2.0 Alpha 2).

  tools/liria-perf.py --prefix <built Liria prefix> [--display :241]
                      [--repeat 3] [--json results.json] [--only NAME,...]

Starts a private Xvfb display (1366x768 at 96 DPI, or --size/--scale), a
D-Bus session bus, xsettingsd and Openbox when they are installed, and
Liria with the SDE GTK theme and Papirus in a throw-away home that holds
folders of 1 000 and 10 000 files and 300 pictures. Every scenario is
driven like a user (xdotool) and measured until Liria is idle again (no
CPU time for 0.4 s):

  startup    process start until the window is mapped ("mapped") and until
             Liria is idle; the Home overview is the first view
  open-1k    the location entry opens the folder of 1 000 files
  open-10k   ... and the folder of 10 000 files (icon view)
  scroll     40 mouse wheel steps over the 10 000 files (icon view)
  view-list, view-compact, view-icons
             Ctrl+2, Ctrl+3 and Ctrl+1 in the folder of 10 000 files
  scroll-list
             40 mouse wheel steps in the list view
  select-all Ctrl+A over 10 000 files (selection drawing, the ribbon and
             the status bar follow the selection)
  select-one ten clicks on single files (each updates the ribbon)
  home       Alt+Home: the Home overview (pins, drives, recent files)
  thumbnails the folder of 300 pictures until every thumbnail is made
  sidebar    ten bookmarks added one by one (each rebuilds the sidebar)

For each scenario the wall time ("ms") and the CPU time of Liria and of
the X server ("cpu", "xcpu", from /proc/PID/schedstat) are printed; with
--repeat the median of the runs. --perf PATH records a perf profile of
--perf-scenario into --perf-output (perf report -i FILE). Exit status 0,
or 77 when Xvfb, xdotool or Liria is missing.
"""
import argparse
import json
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCENARIOS = ("startup", "open-1k", "open-10k", "scroll", "view-list", "scroll-list", "view-compact", "view-icons",
             "select-all", "select-one", "home", "thumbnails", "sidebar")


def png(width, height, seed):
    """A small PNG picture (pure Python): diagonal colour bands."""
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            v = (x + y + seed * 7) % 256
            row += bytes(((v * 3) % 256, (v + seed * 40) % 256, (255 - v) % 256))
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 6)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def make_home(home):
    for name in ("Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos"):
        (home / name).mkdir(parents=True, exist_ok=True)
    kinds = (".txt", ".pdf", ".zip", ".odt", ".html", ".mp3", ".csv", ".sh")
    for count in (1000, 10000):
        folder = home / ("Files-%d" % count)
        folder.mkdir()
        for i in range(count):
            (folder / ("document-%05d%s" % (i, kinds[i % len(kinds)]))).write_bytes(b"x" * (i % 97))
        for i in range(20):
            (folder / ("folder-%02d" % i)).mkdir()
    pictures = home / "Pictures/Holiday"
    pictures.mkdir(parents=True)
    samples = [png(160, 120, i) for i in range(12)]
    for i in range(300):
        (pictures / ("photo-%03d.png" % i)).write_bytes(samples[i % len(samples)])
    (home / ".config/sde").mkdir(parents=True, exist_ok=True)
    (home / ".config/gtk-3.0").mkdir(parents=True, exist_ok=True)
    (home / ".config/gtk-3.0/bookmarks").write_text("")
    (home / ".config/user-dirs.dirs").write_text("".join(
        'XDG_%s_DIR="$HOME/%s"\n' % (key, name) for key, name in
        (("DESKTOP", "Desktop"), ("DOCUMENTS", "Documents"), ("DOWNLOAD", "Downloads"), ("MUSIC", "Music"),
         ("PICTURES", "Pictures"), ("VIDEOS", "Videos"))))
    # A few recent files for the Home overview.
    recent = home / ".local/share"
    recent.mkdir(parents=True, exist_ok=True)
    items = "".join(
        '<bookmark href="file://%s/Files-1000/document-%05d.txt" added="2026-10-20T10:00:00Z" '
        'modified="2026-10-20T10:00:00Z" visited="2026-10-20T10:00:00Z"><info><metadata '
        'owner="http://freedesktop.org"><mime:mime-type type="text/plain"/><bookmark:applications>'
        '<bookmark:application name="niny" exec="niny %%u" modified="2026-10-20T10:00:00Z" count="1"/>'
        '</bookmark:applications></metadata></info></bookmark>\n' % (home, i * 8) for i in range(12))
    (recent / "recently-used.xbel").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<xbel version="1.0" '
        'xmlns:bookmark="http://www.freedesktop.org/standards/desktop-bookmarks" '
        'xmlns:mime="http://www.freedesktop.org/standards/shared-mime-info">\n' + items + "</xbel>\n")


def cpu_ns(pid):
    """CPU time of a process in nanoseconds (0 when it is gone)."""
    try:
        return int(Path("/proc/%d/schedstat" % pid).read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def children_cpu_ns(pid):
    """CPU time of the waited-for children of a process (thumbnailers)."""
    try:
        fields = Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()
        return (int(fields[13]) + int(fields[14])) * 10 ** 9 // os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError):
        return 0


class Bench:
    def __init__(self, args):
        self.args = args
        self.procs = []
        self.home = Path(tempfile.mkdtemp(prefix="liria-perf-"))
        self.liria = None
        self.xvfb = None
        width, height = (int(v) for v in args.size.split("x"))
        self.width, self.height = width, height

    # -------------------------------------------------------------- setup
    def start(self):
        args = self.args
        make_home(self.home)
        dpi = 96 * args.scale // 100
        dark = "-Dark" in args.theme
        icons = ("Papirus-Dark" if dark else "Papirus") if Path("/usr/share/icons/Papirus").is_dir() else "Adwaita"
        self.env = env = dict(
            os.environ, HOME=str(self.home), DISPLAY=args.display, XDG_CONFIG_HOME=str(self.home / ".config"),
            XDG_DATA_HOME=str(self.home / ".local/share"), XDG_CACHE_HOME=str(self.home / ".cache"),
            GTK_THEME=args.theme, NO_AT_BRIDGE="1", GSETTINGS_BACKEND="memory",
            GSETTINGS_SCHEMA_DIR=str(args.prefix / "share/glib-2.0/schemas"),
            XDG_DATA_DIRS=str(args.prefix / "share") + ":/usr/local/share:/usr/share")
        env.pop("SDE_SESSION_VERSION", None)
        if args.themes and (args.themes / args.theme).is_dir():
            (self.home / ".themes").mkdir()
            (self.home / ".themes" / args.theme).symlink_to(args.themes.resolve() / args.theme)
        (self.home / ".config/gtk-3.0/settings.ini").write_text(
            "[Settings]\ngtk-theme-name=%s\ngtk-icon-theme-name=%s\ngtk-font-name=Sans 10\n" % (args.theme, icons))
        (self.home / ".config/sde/liria.conf").write_text("[ribbon]\nmode=%s\n" % args.ribbon)
        self.xvfb = subprocess.Popen(["Xvfb", args.display, "-screen", "0", args.size + "x24", "-nolisten", "tcp",
                                      "-dpi", str(dpi)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        if shutil.which("openbox"):
            self.procs.append(subprocess.Popen(["openbox"], env=env, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL))
        time.sleep(0.8)

    def stop(self):
        if self.liria is not None:
            self.liria.terminate()
            try:
                self.liria.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.liria.kill()
        for proc in reversed(self.procs):
            try:
                if isinstance(proc, int):
                    os.kill(proc, 15)
                else:
                    proc.terminate()
                    proc.wait(timeout=5)
            except Exception:
                pass
        if not self.args.keep:
            shutil.rmtree(self.home, ignore_errors=True)

    # ------------------------------------------------------------ helpers
    def x(self, *command):
        return subprocess.run(["xdotool", *map(str, command)], env=self.env, capture_output=True,
                              text=True).stdout

    def window(self):
        out = self.x("search", "--onlyvisible", "--classname", "liria").split()
        return out[-1] if out else None

    def idle(self, start, quiet=0.4, timeout=60.0):
        """Waits until Liria has been idle for |quiet| seconds; returns the
        time of its last busy moment relative to |start| (seconds). Busy is
        more than 1 ms of CPU time within a 10 ms tick: a file manager also
        wakes up briefly for every change in the folders it watches (the
        folder and, for the path bar, its parents, such as a busy /tmp)."""
        pid = self.liria.pid
        last_cpu = cpu_ns(pid)
        last_busy = time.monotonic()
        end = start + timeout
        while time.monotonic() < end:
            time.sleep(0.01)
            now = cpu_ns(pid)
            if now - last_cpu > 1000000:
                last_busy = time.monotonic()
            last_cpu = now
            if time.monotonic() - last_busy >= quiet:
                break
        return last_busy - start

    def measure(self, action, quiet=0.4, timeout=60.0):
        """Runs |action| and measures until Liria is idle again."""
        pid = self.liria.pid
        cpu0, xcpu0, child0 = cpu_ns(pid), cpu_ns(self.xvfb.pid), children_cpu_ns(pid)
        start = time.monotonic()
        action()
        elapsed = self.idle(start, quiet, timeout)
        return {"ms": round(elapsed * 1000), "cpu": round((cpu_ns(pid) - cpu0) / 1e6),
                "xcpu": round((cpu_ns(self.xvfb.pid) - xcpu0) / 1e6),
                "child": round((children_cpu_ns(pid) - child0) / 1e6)}

    def go(self, path):
        """Types a location into the location entry; returns the Return press."""
        self.x("key", "ctrl+l")
        time.sleep(0.3)
        self.x("key", "ctrl+a")
        self.x("type", "--delay", "2", str(path))
        time.sleep(0.2)
        self.idle(time.monotonic(), 0.3, 20)
        return lambda: self.x("key", "Return")

    def pointer_over_view(self):
        # The window fills the screen; the files are right of the sidebar
        # and under the header (and ribbon).
        self.x("mousemove", self.width * 3 // 5, self.height * 3 // 5)
        time.sleep(0.2)

    # ---------------------------------------------------------- scenarios
    def run(self, only, perf_scenario=None):
        results = {}
        log = open(self.home / "liria.log", "w")
        start = time.monotonic()
        self.liria = subprocess.Popen([str(self.args.prefix / "bin/liria"), "--no-desktop", str(self.home)],
                                      env=self.env, cwd=str(self.home), stdout=log, stderr=subprocess.STDOUT)
        mapped = None
        end = start + 30
        while time.monotonic() < end and mapped is None:
            if self.window():
                mapped = time.monotonic() - start
            else:
                time.sleep(0.01)
        if mapped is None:
            raise SystemExit("Liria did not open its window; see %s" % (self.home / "liria.log"))
        settled = self.idle(start, 0.6)
        results["startup"] = {"ms": round(settled * 1000), "mapped": round(mapped * 1000),
                              "cpu": round(cpu_ns(self.liria.pid) / 1e6)}
        wid = self.window()
        self.x("windowmove", wid, 0, 0)
        self.x("windowsize", wid, self.width, self.height - 30)
        self.x("windowactivate", wid)
        self.idle(time.monotonic(), 0.5)
        perf = None

        def scenario(name, action, quiet=0.4, timeout=60.0):
            nonlocal perf
            if only and name not in only:
                # Not measured, but later scenarios start from its result.
                action()
                self.idle(time.monotonic(), quiet, timeout)
                return
            if perf_scenario == name:
                perf = subprocess.Popen([str(self.args.perf), "record"] + self.args.perf_args.split()
                                        + ["-p", str(self.liria.pid), "-o", str(self.args.perf_output)],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(0.5)
            results[name] = self.measure(action, quiet, timeout)
            if perf is not None:
                perf.send_signal(2)
                perf.wait()
                perf = None

        if not only or set(only) & set(SCENARIOS[1:10]):
            scenario("open-1k", self.go(self.home / "Files-1000"))
            scenario("open-10k", self.go(self.home / "Files-10000"), 0.6)
            self.x("key", "ctrl+1")
            self.idle(time.monotonic(), 0.4)
            self.pointer_over_view()
            scenario("scroll", lambda: self.x("click", "--repeat", "40", "--delay", "25", "5"))
            scenario("view-list", lambda: self.x("key", "ctrl+2"), 0.5)
            self.pointer_over_view()
            scenario("scroll-list", lambda: self.x("click", "--repeat", "40", "--delay", "25", "5"))
            scenario("view-compact", lambda: self.x("key", "ctrl+3"), 0.5)
            scenario("view-icons", lambda: self.x("key", "ctrl+1"), 0.5)
            self.x("key", "ctrl+Home")
            self.idle(time.monotonic(), 0.4)
            scenario("select-all", lambda: self.x("key", "ctrl+a"), 0.5)
            self.x("key", "ctrl+shift+i")
            self.idle(time.monotonic(), 0.4)

            def clicks():
                # Ten files of the first row (the view starts at the top).
                for i in range(10):
                    self.x("mousemove", self.width // 4 + (i % 5) * 90, self.height // 3 + (i // 5) * 10)
                    self.x("click", "1")
                    time.sleep(0.12)
            scenario("select-one", clicks)
        scenario("home", lambda: self.x("key", "alt+Home"), 0.6)
        if not only or "thumbnails" in only:
            scenario("thumbnails", self.go(self.home / "Pictures/Holiday"), 1.5, 120)
            made = list((self.home / ".cache/thumbnails").rglob("*.png"))
            if "thumbnails" in results:
                results["thumbnails"]["made"] = len(made)
            self.x("key", "alt+Home")
            self.idle(time.monotonic(), 0.6)

        def bookmarks():
            path = self.home / ".config/gtk-3.0/bookmarks"
            for i in range(10):
                with path.open("a") as f:
                    f.write("file://%s/Files-1000 Files %d\n" % (self.home, i))
                time.sleep(0.15)
        scenario("sidebar", bookmarks, 0.6)
        log.close()
        warnings = [line for line in (self.home / "liria.log").read_text(errors="replace").splitlines()
                    if "CRITICAL" in line or "WARNING" in line]
        results["warnings"] = len(warnings)
        if warnings and self.args.verbose:
            print("\n".join(warnings[:10]))
        self.liria.terminate()
        self.liria.wait(timeout=10)
        self.liria = None
        return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path("/usr"))
    parser.add_argument("--display", default=":241")
    parser.add_argument("--size", default="1366x768")
    parser.add_argument("--scale", type=int, default=100)
    parser.add_argument("--themes", type=Path, default=ROOT / "build/themes",
                        help="folder with the built SDE GTK themes (tools/build-themes.py)")
    parser.add_argument("--theme", default="SDE-Graphite-Light-Blue")
    parser.add_argument("--ribbon", default="full", choices=("full", "collapsed", "off"))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--only", help="comma separated scenarios: " + ", ".join(SCENARIOS))
    parser.add_argument("--json", type=Path, help="write all runs as JSON")
    parser.add_argument("--perf", type=Path, help="perf binary: record a profile of --perf-scenario")
    parser.add_argument("--perf-scenario", default="open-10k")
    parser.add_argument("--perf-output", type=Path, default=Path("liria-perf.data"))
    parser.add_argument("--perf-args", default="-F 400 --call-graph dwarf,32768",
                        help="perf record options (default: %(default)s)")
    parser.add_argument("--keep", action="store_true", help="keep the temporary home")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    for tool in ("Xvfb", "xdotool", "dbus-launch"):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    if not (args.prefix / "bin/liria").is_file():
        print("skipped: %s not found" % (args.prefix / "bin/liria"))
        return 77
    only = [s for s in args.only.split(",") if s] if args.only else []
    runs = []
    bench = Bench(args)
    try:
        bench.start()
        for i in range(args.repeat):
            runs.append(bench.run(only, args.perf_scenario if args.perf and i == 0 else None))
            print("run %d: %s" % (i + 1, json.dumps(runs[-1])), flush=True)
    finally:
        bench.stop()
    print("%-13s %8s %8s %8s %8s" % ("scenario", "ms", "cpu", "xcpu", "child"))
    for name in SCENARIOS:
        values = [run[name] for run in runs if name in run]
        if not values:
            continue
        med = {key: statistics.median(v[key] for v in values) for key in values[0] if key in ("ms", "cpu", "xcpu",
                                                                                                "child", "mapped")}
        extra = ("  mapped %d ms" % med["mapped"]) if "mapped" in med else ""
        print("%-13s %8d %8d %8s %8s%s" % (name, med["ms"], med["cpu"], int(med.get("xcpu", 0)),
                                         int(med.get("child", 0)), extra))
    if args.json:
        args.json.write_text(json.dumps(runs, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
