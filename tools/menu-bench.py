#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Start menu benchmark (0.2 Alpha 1): how fast and smooth the SDE Start menu is.

Starts a private Xvfb display and D-Bus bus and sde-panel with a generated set
of applications (--apps, default 320, plus the installed ones), real icon
names from the installed icon theme, a few broken .desktop files, a launch
history and a home folder with files, then measures with the panel's own
timing hook (SDE_MENU_PERF_LOG, see src/panel/menu_perf.h):

  * open latency: the first (cold) open after the panel started, then warm
    opens by command (Super runs `sde-panel --menu`) and by clicking Start,
    from the request to the first frame on the X server;
  * typing: each keystroke to the frame with the new results, typed in the
    menu (handed to the panel search) and in the search;
  * scrolling: wheel bursts, scrollbar drag, Page/arrow keys: event to frame,
    the frames drawn and the frame rate while scrolling;
  * hover: pointer moves over the list and the tiles;
  * CPU while the menu stays open with the pointer resting on a tile;
  * memory (RSS) after start, after the first open and after many opens;
  * the time `sde-panel --menu` itself takes (the Super key's command).

Example:
  python3 tools/menu-bench.py --bin-dir build/src --json /tmp/menu-before.json
  python3 tools/menu-bench.py --bin-dir build/src --size 1024x600 --scale 150
Compare two runs with --compare before.json after.json.
"""
import argparse
import json
import os
import re
import shutil
import signal
import statistics
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

CLK = os.sysconf("SC_CLK_TCK")
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--bin-dir", type=Path, default=Path("build/src"))
ap.add_argument("--apps", type=int, default=320, help="generated applications")
ap.add_argument("--display", default=":171")
ap.add_argument("--size", default="1366x768")
ap.add_argument("--scale", type=int, default=100)
ap.add_argument("--opens", type=int, default=15, help="warm opens by command")
ap.add_argument("--idle-seconds", type=float, default=8.0)
ap.add_argument("--settle", type=float, default=3.0, help="seconds after the panel started")
ap.add_argument("--theme", default="light")
ap.add_argument("--hover", default="pop", help="[effects] hover (pop, glow, wave, none...)")
ap.add_argument("--json", type=Path)
ap.add_argument("--keep", type=Path, help="copy the perf log and panel log here")
ap.add_argument("--label", default="")
ap.add_argument("--compare", nargs=2, type=Path, metavar=("BEFORE", "AFTER"),
                help="print a before/after table of two --json results and exit")
args = ap.parse_args()


def summary(values):
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    p95 = v[min(len(v) - 1, int(round(0.95 * (len(v) - 1))))]
    return {"n": len(v), "median": round(statistics.median(v), 2), "p95": round(p95, 2),
            "max": round(v[-1], 2), "min": round(v[0], 2)}


ROWS = [
    ("cold open (first open after start)", "open_cold_ms"),
    ("warm open by command (Super)", "open_warm_ms"),
    ("warm open by clicking Start", "open_click_ms"),
    ("`sde-panel --menu` command itself", "command_ms"),
    ("first key in the menu -> search shown", "key_open_ms"),
    ("keystroke -> results (from the menu)", "key_menu_ms"),
    ("keystroke -> results (search)", "key_search_ms"),
    ("  of which filtering", "filter_ms"),
    ("hover -> frame", "hover_ms"),
    ("wheel event -> frame", "wheel_ms"),
    ("wheel burst: frame draw", "wheel_frame_draw_ms"),
    ("wheel burst: frames per second", "wheel_fps"),
    ("scrollbar drag event -> frame", "drag_ms"),
    ("Page/arrow key -> frame", "nav_ms"),
    ("idle open: CPU %", "idle_cpu_pct"),
    ("idle open: frames drawn", "idle_frames"),
    ("RSS after start (MB)", "rss_start_mb"),
    ("RSS after first open (MB)", "rss_open_mb"),
    ("RSS after the run (MB)", "rss_end_mb"),
    ("Super commands that never opened", "lost_commands"),
]


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, dict):
        return f"{v['median']:.1f} (p95 {v['p95']:.1f}, max {v['max']:.1f}, n={v['n']})"
    return f"{v:.2f}" if isinstance(v, float) else str(v)


def table(res, title):
    print(f"\n== {title}")
    for label, key in ROWS:
        print(f"{label:<42} {fmt(res.get(key))}")


if args.compare:
    a, b = (json.loads(p.read_text()) for p in args.compare)
    print(f"{'measurement':<42} {'before':>34} {'after':>34}")
    for label, key in ROWS:
        print(f"{label:<42} {fmt(a.get(key)):>34} {fmt(b.get(key)):>34}")
    sys.exit(0)

W, H = map(int, args.size.split("x"))
scale = args.scale / 100
bin_dir = args.bin_dir.resolve()
tmp = tempfile.TemporaryDirectory(prefix="sde-menu-bench-")
home = Path(tmp.name)
share = home / "share"
apps_dir = share / "applications"
for d in (".config/sde", ".local/state/sde", ".local/share", "run", "bin", "Documents/Reports",
          "Pictures", "Desktop", "Downloads", "Music", "Videos"):
    (home / d).mkdir(parents=True, exist_ok=True)
(home / "run").chmod(0o700)
apps_dir.mkdir(parents=True)

# Real icon names: application icons of the installed theme(s).
icon_names = []
for theme in ("Papirus", "Adwaita", "hicolor"):
    for sub in ("48x48/apps", "64x64/apps", "scalable/apps", "256x256/apps"):
        d = Path("/usr/share/icons") / theme / sub
        if d.is_dir():
            icon_names += sorted(p.stem for p in d.iterdir() if p.suffix in (".svg", ".png"))
    if len(icon_names) > 400:
        break
icon_names = sorted(set(icon_names)) or ["application-x-executable"]
WORDS_A = ("Aurora Blue Crystal Delta Ember Falcon Garnet Harbor Indigo Jade Kestrel Lumen Maple "
           "Nimbus Onyx Pixel Quartz Raven Sierra Tundra Umber Vertex Willow Xenon Yonder Zephyr").split()
WORDS_B = ("Editor Mail Player Viewer Studio Notes Terminal Browser Calendar Draw Paint Writer "
           "Sheets Slides Music Video Photos Maps Weather Clock Monitor Backup Chat Reader").split()
CATS = ["Utility", "Graphics", "Network", "AudioVideo", "Office", "Development", "Education",
        "Game", "Science", "Settings", "System"]
step = max(1, len(icon_names) // max(1, args.apps))
for i in range(args.apps):
    name = f"{WORDS_A[i % len(WORDS_A)]} {WORDS_B[(i // len(WORDS_A)) % len(WORDS_B)]} {i:03d}"
    icon = icon_names[(i * step) % len(icon_names)]
    if i % 29 == 7:
        icon = f"bench-missing-icon-{i}"  # an application without an icon
    if i % 97 == 13:
        name = "A very long application name that keeps going " * 3 + str(i)
    (apps_dir / f"bench-{i:03d}.desktop").write_text(
        "[Desktop Entry]\nType=Application\n"
        f"Name={name}\nGenericName={WORDS_B[i % len(WORDS_B)]} tool\n"
        f"Comment=Benchmark application number {i}\nKeywords=bench;{WORDS_A[i % 7].lower()};\n"
        f"Exec=true bench-{i}\nIcon={icon}\nCategories={CATS[i % len(CATS)]};\n")
# Broken entries: they must not show up, slow down or crash anything.
(apps_dir / "broken-nogroup.desktop").write_text("Name=Broken\nExec=true\n")
(apps_dir / "broken-noexec.desktop").write_text("[Desktop Entry]\nType=Application\nName=No Exec\n")
(apps_dir / "broken-binary.desktop").write_bytes(bytes(range(256)) * 8)
(apps_dir / "broken-empty.desktop").write_text("")
(apps_dir / "broken-utf8.desktop").write_bytes(
    b"[Desktop Entry]\nType=Application\nName=Bad \xff\xfe UTF-8\nExec=true\n")
# A launch history for "Most used" and the search ranking.
now = int(time.time())
(home / ".local/state/sde/menu-usage").write_text(
    "".join(f"bench-{i:03d}.desktop\t{20 - i}\t{now - i * 3600}\n" for i in range(12)))
# Files for the search's file results.
for i in range(400):
    (home / "Documents" / f"Report {i:03d}.txt").write_text("x")
for i in range(200):
    (home / "Documents/Reports" / f"Office notes {i:03d}.odt").write_text("x")
for i in range(150):
    (home / "Pictures" / f"Photo {i:03d}.png").write_text("x")
(home / ".config/sde/sde.conf").write_text(
    f"[appearance]\nscale={args.scale}\ntheme={args.theme}\n[effects]\nenabled=true\nhover={args.hover}\n")
if shutil.which("convert"):
    sp.run(["convert", "-size", "256x256", "gradient:#2672BE-#E0525C", str(home / ".face")],
           stdout=sp.DEVNULL, stderr=sp.DEVNULL)

perf_log = home / "perf.log"
env = os.environ.copy()
env.pop("SDE_PANEL_QA_DIR", None)
env.update(
    PATH=f"{home / 'bin'}:{bin_dir}:{env.get('PATH', '')}", DISPLAY=args.display, HOME=str(home),
    XDG_CONFIG_HOME=str(home / ".config"), XDG_CONFIG_DIRS=str(home / "system"),
    XDG_DATA_HOME=str(home / ".local/share"), XDG_STATE_HOME=str(home / ".local/state"),
    XDG_CACHE_HOME=str(home / ".cache"), XDG_RUNTIME_DIR=str(home / "run"),
    XDG_DATA_DIRS=f"{share}:/usr/local/share:/usr/share", LANG="C.UTF-8", LC_ALL="C.UTF-8",
    SDE_MENU_PERF_LOG=str(perf_log))
# Quiet stand-ins for the helpers the panel polls; launched applications
# ("true") and folders opened from the menu do nothing.
(home / "bin/nmcli").write_text("#!/bin/sh\ncase \"$*\" in\n*TYPE,STATE,CONNECTION*) "
                                "echo 'ethernet:connected:Bench LAN';;\n'radio wifi') echo enabled;;\nesac\n")
(home / "bin/pactl").write_text("#!/bin/sh\ncase \"$1\" in\nget-sink-volume) echo 'Volume: "
                                "front-left: 42598 / 65% / -11 dB';;\nget-sink-mute) echo 'Mute: no';;\n"
                                "subscribe) exec sleep 100000;;\nesac\n")
for name in ("liria", "xdg-open", "sde-settings", "sde-gallery", "debion-network-check"):
    (home / "bin" / name).write_text("#!/bin/sh\nexit 0\n")
for p in (home / "bin").iterdir():
    p.chmod(0o755)

procs = []


def start(argv, log):
    p = sp.Popen(argv, env=env, stdout=open(home / log, "w"), stderr=sp.STDOUT,
                 start_new_session=True)
    procs.append(p)
    return p


def xdo(*a):
    sp.run(["xdotool", *map(str, a)], env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL)


def visible(name):
    r = sp.run(["xdotool", "search", "--onlyvisible", "--name", name], env=env,
               stdout=sp.PIPE, stderr=sp.DEVNULL, text=True)
    return r.stdout.split()[-1] if r.returncode == 0 and r.stdout.strip() else None


def wait(fn, what, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.02)
    raise RuntimeError("timeout: " + what)


def geom(wid):
    text = sp.run(["xwininfo", "-id", str(wid)], env=env, stdout=sp.PIPE, text=True).stdout
    get = lambda k: int(re.search(re.escape(k) + r"\s*:\s*(-?\d+)", text).group(1))
    return get("Absolute upper-left X"), get("Absolute upper-left Y"), get("Width"), get("Height")


def lines():
    try:
        return perf_log.read_text().splitlines()
    except FileNotFoundError:
        return []


def parse(line):
    f = line.split()
    out = {"kind": f[0], "who": f[1] if len(f) > 1 else ""}
    for part in f[2:]:
        if "=" in part:
            k, v = part.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


def events_since(n, kind=None, who=None):
    out = [parse(l) for l in lines()[n:]]
    return [e for e in out if (kind is None or e["kind"] == kind) and (who is None or e["who"] == who)]


def proc_stat(pid):
    stat = Path(f"/proc/{pid}/stat").read_text()
    f = stat[stat.rfind(")") + 2:].split()
    rss = int(re.search(r"VmRSS:\s+(\d+)", Path(f"/proc/{pid}/status").read_text()).group(1))
    return int(f[11]) + int(f[12]), rss


def menu_open():
    return visible("^SDE Menu$")


LOST = []


def open_menu_command(park=True):
    """`sde-panel --menu` as the Super key runs it; returns the panel's "open"
    line and how long the command took. A command that never opens the menu
    (lost before 0.2, see send_command in src/common/x11.cpp) is counted and
    sent again."""
    if park:  # away from the menu: no hover while it opens
        xdo("mousemove", W - 2, 2)
    for attempt in range(4):
        n = len(lines())
        t0 = time.perf_counter()
        sp.run([str(bin_dir / "sde-panel"), "--menu"], env=env)
        cmd = (time.perf_counter() - t0) * 1000
        try:
            ev = wait(lambda: events_since(n, "open", "menu"), "menu open line", 3.0)[-1]
        except RuntimeError:
            LOST.append(time.time())
            continue
        wait(menu_open, "menu window")
        return ev, cmd
    raise RuntimeError("the menu did not open")


def close_menu():
    if menu_open():
        xdo("key", "Escape")
        wait(lambda: not menu_open() or None, "menu closed")
    time.sleep(0.25)


def layout():
    for l in reversed(lines()):
        if l.startswith("layout menu"):
            e = {}
            for part in l.split()[2:]:
                k, v = part.split("=", 1)
                if "," in v:
                    e.setdefault(k, [int(x) for x in v.split(",")])
                else:
                    e.setdefault("n_" + k, int(v))
            return e
    return None


res = {"label": args.label, "size": args.size, "scale": args.scale, "apps": args.apps}
try:
    xv = start(["Xvfb", args.display, "-screen", "0", args.size + "x24", "-nolisten", "tcp", "-ac"],
               "xvfb.log")
    wait(lambda: Path(f"/tmp/.X11-unix/X{args.display.lstrip(':')}").exists(), "Xvfb", 30)
    # The socket exists before the server takes connections.
    wait(lambda: sp.run(["xdotool", "getmouselocation"], env=env, stdout=sp.DEVNULL,
                        stderr=sp.DEVNULL).returncode == 0, "Xvfb ready", 30)
    bus = sp.run(["dbus-daemon", "--session", "--fork", "--print-address=1", "--print-pid=1"],
                 env=env, stdout=sp.PIPE, text=True)
    bus_pid = None
    if bus.returncode == 0:
        addr, bus_pid = bus.stdout.split()[:2]
        env["DBUS_SESSION_BUS_ADDRESS"] = addr
    panel = start([str(bin_dir / "sde-panel")], "sde-panel.log")
    pwin = wait(lambda: visible("^SDE Panel$"), "panel window")
    time.sleep(args.settle)
    res["rss_start_mb"] = round(proc_stat(panel.pid)[1] / 1024, 1)
    # The Super key's command on its own (process start, X connection).
    times = []
    for _ in range(5):
        t0 = time.perf_counter()
        sp.run([str(bin_dir / "sde-panel"), "--version"], env=env, stdout=sp.DEVNULL)
        times.append((time.perf_counter() - t0) * 1000)
    res["version_cmd_ms"] = summary(times)

    # 1) Cold open, then warm opens.
    ev, cmd = open_menu_command()
    res["open_cold_ms"] = ev["ms"]
    res["open_cold_detail"] = ev
    time.sleep(0.5)
    res["rss_open_mb"] = round(proc_stat(panel.pid)[1] / 1024, 1)
    lay = layout()
    res["layout"] = lay
    mw = wait(menu_open, "menu")
    mg = geom(mw)
    close_menu()
    warm, cmds, detail = [], [cmd], []
    for _ in range(args.opens):
        ev, cmd = open_menu_command()
        warm.append(ev["ms"])
        cmds.append(cmd)
        detail.append(ev)
        time.sleep(0.3)
        close_menu()
    res["open_warm_ms"] = summary(warm)
    res["command_ms"] = summary(cmds)
    res["open_warm_prepare_ms"] = summary([d.get("prepare") for d in detail])
    res["open_warm_show_ms"] = summary([d.get("show") for d in detail])
    res["open_warm_draw_ms"] = summary([d.get("draw") for d in detail])
    # Clicking the Start button (left end of the panel).
    pg = geom(pwin)
    clicks = []
    for _ in range(5):
        n = len(lines())
        xdo("mousemove", pg[0] + round(18 * scale), pg[1] + pg[3] // 2)
        time.sleep(0.1)
        xdo("click", 1)
        ev = wait(lambda: events_since(n, "open", "menu"), "menu open by click")[-1]
        clicks.append(ev["ms"])
        wait(menu_open, "menu window")
        time.sleep(0.3)
        close_menu()
    res["open_click_ms"] = summary(clicks)

    # 2) With the menu open: hover, wheel, scrollbar, keys, idle.
    open_menu_command()
    time.sleep(0.4)
    lay = layout() or lay
    mg = geom(menu_open())
    lx, ly, lw, lh, lcontent = lay["list"]
    tx, ty, tw, th, tcontent = lay.get("tiles", [lx + lw, ly, 100, lh, lh])
    n = len(lines())
    for i in range(24):
        xdo("mousemove", mg[0] + lx + lw // 2, mg[1] + ly + round(20 * scale) + i * round(18 * scale))
        time.sleep(0.04)
    for i in range(12):
        xdo("mousemove", mg[0] + tx + round(30 * scale) + (i % 4) * round(50 * scale),
            mg[1] + ty + round(60 * scale) + (i // 4) * round(50 * scale))
        time.sleep(0.06)
    time.sleep(0.4)
    res["hover_ms"] = summary([e["ms"] for e in events_since(n, "hover", "menu")])
    # Wheel bursts over the list.
    xdo("mousemove", mg[0] + lx + lw // 2, mg[1] + ly + lh // 2)
    time.sleep(0.3)
    n = len(lines())
    t_start = time.time()
    for direction in (5, 4):
        sp.run(["xdotool", "click", "--repeat", "30", "--delay", "30", str(direction)], env=env)
        time.sleep(0.5)
    evs = events_since(n)
    wheel = [e for e in evs if e["kind"] == "wheel"]
    frames = [e for e in evs if e["who"] == "menu" and e["kind"] in ("frame", "wheel")]
    res["wheel_ms"] = summary([e["ms"] for e in wheel])
    res["wheel_frame_draw_ms"] = summary([e.get("draw", 0) + e.get("sync", 0) for e in frames])
    if len(frames) > 1:
        span = (frames[-1]["t"] - frames[0]["t"]) / 1000
        res["wheel_frames"] = len(frames)
        # Frames per second while frames were coming (bursts of 30 events).
        ts = [f["t"] for f in frames]
        gaps = [b - a for a, b in zip(ts, ts[1:]) if b - a < 200]
        res["wheel_fps"] = round(1000 / statistics.median(gaps), 1) if gaps else None
        res["wheel_frame_gap_ms"] = summary(gaps)
    # Scrollbar drag.
    bx = mg[0] + lx + lw - round(4 * scale)
    n = len(lines())
    xdo("mousemove", bx, mg[1] + ly + round(12 * scale), "mousedown", 1)
    time.sleep(0.1)
    for i in range(40):
        xdo("mousemove", bx, mg[1] + ly + round(12 * scale) + i * (lh - round(30 * scale)) // 40)
        time.sleep(0.016)
    xdo("mouseup", 1)
    time.sleep(0.4)
    res["drag_ms"] = summary([e["ms"] for e in events_since(n, "drag", "menu")])
    # Keys: Page Down/Up and arrows.
    xdo("mousemove", mg[0] + lx + lw // 2, mg[1] + ly + lh // 2)
    n = len(lines())
    for key in ["Next"] * 8 + ["Prior"] * 8 + ["Down"] * 15 + ["Up"] * 5:
        xdo("key", key)
        time.sleep(0.08)
    time.sleep(0.4)
    res["nav_ms"] = summary([e["ms"] for e in events_since(n, "nav", "menu")])
    # Idle: the pointer rests on a tile.
    xdo("mousemove", mg[0] + tx + round(40 * scale), mg[1] + ty + round(70 * scale))
    time.sleep(1.5)
    n = len(lines())
    c0, _ = proc_stat(panel.pid)
    time.sleep(args.idle_seconds)
    c1, rss = proc_stat(panel.pid)
    res["idle_cpu_pct"] = round(100.0 * (c1 - c0) / CLK / args.idle_seconds, 2)
    res["idle_frames"] = len([e for e in events_since(n) if e["who"] == "menu"])
    close_menu()

    # 3) Typing: in the menu (handed to the search), then in the search.
    xdo("mousemove", W // 2, round(40 * scale))
    open_menu_command()
    time.sleep(0.4)
    n = len(lines())
    sp.run(["xdotool", "type", "--delay", "150", "aurora edit"], env=env)
    time.sleep(0.8)
    evs = events_since(n)
    res["key_open_ms"] = summary([e["ms"] for e in evs if e["kind"] == "key-open"])
    menu_keys = [e for e in evs if e["kind"] == "key" and e["who"] == "search"]
    res["key_menu_ms"] = summary([e["ms"] for e in menu_keys])
    xdo("key", "Escape")
    wait(lambda: not visible("^SDE Search$") or None, "search closed")
    time.sleep(0.3)
    n = len(lines())
    for attempt in range(4):
        sp.run([str(bin_dir / "sde-panel"), "--search"], env=env)
        try:
            wait(lambda: visible("^SDE Search$"), "search", 3.0)
            break
        except RuntimeError:
            LOST.append(time.time())
    time.sleep(0.5)
    sp.run(["xdotool", "type", "--delay", "150", "office report"], env=env)
    for _ in range(6):
        xdo("key", "BackSpace")
        time.sleep(0.15)
    sp.run(["xdotool", "type", "--delay", "150", "notes"], env=env)
    time.sleep(0.8)
    keys = [e for e in events_since(n) if e["kind"] == "key" and e["who"] == "search"]
    res["key_search_ms"] = summary([e["ms"] for e in keys])
    res["filter_ms"] = summary([e.get("filter") for e in keys + menu_keys if "filter" in e])
    res["search_open_ms"] = summary([e["ms"] for e in events_since(n, "open", "search")])
    xdo("key", "Escape")
    time.sleep(0.4)

    # 4) Many opens (leaks), then the final memory.
    for _ in range(20):
        open_menu_command()
        xdo("mousemove", mg[0] + lx + lw // 2, mg[1] + ly + lh // 3)
        sp.run(["xdotool", "click", "--repeat", "3", "--delay", "20", "5"], env=env)
        time.sleep(0.1)
        close_menu()
    time.sleep(0.5)
    res["rss_end_mb"] = round(proc_stat(panel.pid)[1] / 1024, 1)
    res["lost_commands"] = len(LOST)
    res["panel_alive"] = panel.poll() is None
finally:
    for p in reversed(procs):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    for p in procs:
        try:
            p.wait(timeout=3)
        except sp.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
    if "bus_pid" in dir() and bus_pid:
        try:
            os.kill(int(bus_pid), signal.SIGTERM)
        except (ProcessLookupError, ValueError):
            pass
    if args.keep:
        args.keep.mkdir(parents=True, exist_ok=True)
        for f in ("perf.log", "sde-panel.log"):
            if (home / f).exists():
                shutil.copy2(home / f, args.keep / f)

table(res, f"SDE Start menu benchmark {args.label} ({args.size} at {args.scale} %, "
           f"{args.apps} generated applications)")
if args.json:
    args.json.write_text(json.dumps(res, indent=2))
