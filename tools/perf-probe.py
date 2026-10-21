#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproducible idle/interaction CPU probe for the SDE session pieces.

Starts a private Xvfb display, a private D-Bus session bus, SDE WM, the
compositor, sde-desktop, sde-panel and sde-notifyd, then reports per process
over N seconds:

  * CPU time (user+system, % of one core),
  * wakeups (voluntary + involuntary context switches per second), the best
    proxy for timers/polling that fire while the desktop is idle,
  * repaint counters printed by SDE_PERF_STATS=1 (BufferedWindow flushes,
    full redraws, partial redraws, buffer copies and time spent in draw()).

Phases: "idle" (nobody touches anything) and, with --interact, "pointer"
(xdotool sweeps the pointer over the desktop icons and the taskbar) and
"popups" (Start menu / Action Center opened and closed).

Network and audio helpers are replaced by quiet fixtures (like
tools/ui-smoke.py) so the numbers do not depend on the build machine; pass
--real-helpers to use the installed nmcli/pactl instead.

Example:
  python3 tools/perf-probe.py --bin-dir build/src --seconds 20 --interact
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
CLK = os.sysconf("SC_CLK_TCK")

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--bin-dir", type=Path, default=Path("/usr/bin"),
                help="directory holding sde-panel, sde-desktop, sde-notifyd")
ap.add_argument("--data-dir", type=Path, default=ROOT / "data",
                help="SDE data directory (openbox/rc.xml, compositor/sde.conf)")
ap.add_argument("--display", default=":160")
ap.add_argument("--size", default="1366x768")
ap.add_argument("--seconds", type=float, default=20.0, help="length of each phase")
ap.add_argument("--warmup", type=float, default=4.0)
ap.add_argument("--interact", action="store_true", help="also measure pointer and popup phases")
ap.add_argument("--no-compositor", action="store_true")
ap.add_argument("--compositor", default="",
                help="compositor binary (default: sde-compositor, else picom)")
ap.add_argument("--compositor-config", type=Path, default=None,
                help="compositor configuration (default: data/compositor/sde.conf)")
ap.add_argument("--wm", default="", help="window manager binary (default: sde-wm, else openbox)")
ap.add_argument("--real-helpers", action="store_true", help="do not replace nmcli/pactl")
ap.add_argument("--json", type=Path, help="also write the results as JSON")
ap.add_argument("--keep-logs", type=Path, help="copy the component logs here")
args = ap.parse_args()


def which(name, extra=()):
    for d in list(extra) + os.environ.get("PATH", "").split(":"):
        p = Path(d) / name
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    return ""


tmp = tempfile.TemporaryDirectory(prefix="sde-perf-")
home = Path(tmp.name)
for d in ("run", ".config/sde", "bin", "Desktop"):
    (home / d).mkdir(parents=True, exist_ok=True)
(home / "run").chmod(0o700)
for i in range(6):  # a few desktop icons to hover over
    (home / "Desktop" / f"Document {i}.txt").write_text("perf probe\n")

if not args.real_helpers:
    # Quiet, deterministic stand-ins. "pactl subscribe" stays alive like the
    # real one so the panel does not enter its 10 s retry loop.
    (home / "bin/nmcli").write_text("""#!/bin/sh
case "$*" in
*TYPE,STATE,CONNECTION*) printf 'wifi:connected:SDE Perf Lab\\nethernet:unavailable:\\n' ;;
'radio wifi') echo enabled ;;
*IN-USE,SIGNAL,SECURITY,SSID*) printf '*:84:WPA2:SDE Perf Lab\\n:62:WPA2:Library\\n' ;;
*) exit 0 ;;
esac
""")
    (home / "bin/pactl").write_text("""#!/bin/sh
case "$1" in
get-sink-volume) echo 'Volume: front-left: 42598 / 65% / -11.23 dB' ;;
get-sink-mute) echo 'Mute: no' ;;
get-default-sink) echo perf-speakers ;;
list) printf 'Sink #1\\nName: perf-speakers\\nDescription: Perf Speakers\\n' ;;
subscribe) exec sleep 100000 ;;
esac
""")
    for p in (home / "bin").iterdir():
        p.chmod(0o755)

env = os.environ.copy()
env.update(
    PATH=f"{home / 'bin'}:{args.bin_dir.resolve()}:{env.get('PATH', '')}",
    DISPLAY=args.display,
    HOME=str(home),
    XDG_CONFIG_HOME=str(home / ".config"),
    XDG_RUNTIME_DIR=str(home / "run"),
    XDG_DATA_DIRS=f"{args.data_dir.resolve().parent}:/usr/local/share:/usr/share",
    LANG="C.UTF-8",
    LC_ALL="C.UTF-8",
    SDE_PERF_STATS="1",  # instrumented builds dump counters on SIGUSR2
)
env.pop("DBUS_SESSION_BUS_ADDRESS", None)
(home / ".config/sde/sde.conf").write_text("[panel]\nposition=bottom\n")

procs = {}  # name -> Popen


def start(name, argv, **kw):
    log = open(home / f"{name}.log", "w")
    p = sp.Popen(argv, env=env, stdout=log, stderr=sp.STDOUT, start_new_session=True, **kw)
    procs[name] = p
    return p


def stop_all():
    for name, p in reversed(list(procs.items())):
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)  # its own session: helpers too
            except ProcessLookupError:
                pass
    deadline = time.time() + 3
    for p in procs.values():
        try:
            p.wait(max(0.1, deadline - time.time()))
        except sp.TimeoutExpired:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def proc_sample(pid):
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
        fields = stat[stat.rfind(")") + 2:].split()
        cpu = int(fields[11]) + int(fields[12])  # utime + stime
        st = Path(f"/proc/{pid}/status").read_text()
        vol = int(re.search(r"voluntary_ctxt_switches:\s+(\d+)", st).group(1))
        inv = int(re.search(r"nonvoluntary_ctxt_switches:\s+(\d+)", st).group(1))
        rss = int(re.search(r"VmRSS:\s+(\d+)", st).group(1))
        return cpu, vol + inv, rss
    except (OSError, AttributeError):
        return None


def children_cpu(pid):
    """CPU of helper processes (nmcli, pactl...) is reaped into cutime/cstime."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
        fields = stat[stat.rfind(")") + 2:].split()
        return int(fields[13]) + int(fields[14])
    except OSError:
        return 0


def measure(seconds, action=None):
    before = {n: (proc_sample(p.pid), children_cpu(p.pid)) for n, p in procs.items()}
    t0 = time.time()
    if action:
        action(seconds)
    rest = seconds - (time.time() - t0)
    if rest > 0:
        time.sleep(rest)
    dt = time.time() - t0
    out = {}
    for n, p in procs.items():
        a, ca = before[n]
        b = proc_sample(p.pid)
        if not a or not b:
            out[n] = None
            continue
        out[n] = {
            "cpu_pct": round(100.0 * (b[0] - a[0]) / CLK / dt, 2),
            "helper_cpu_pct": round(100.0 * (children_cpu(p.pid) - ca) / CLK / dt, 2),
            "wakeups_per_s": round((b[1] - a[1]) / dt, 1),
            "rss_kb": b[2],
        }
    return out


def xdo(*a):
    sp.run(["xdotool", *a], env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL)


W, H = map(int, args.size.split("x"))


def sweep_pointer(seconds):
    end = time.time() + seconds
    while time.time() < end:
        for y in range(40, 520, 24):  # desktop icon column
            xdo("mousemove", "48", str(y))
            time.sleep(0.02)
        for x in range(0, W, 40):  # taskbar
            xdo("mousemove", str(x), str(H - 20))
            time.sleep(0.02)


def popups(seconds):
    end = time.time() + seconds
    panel = args.bin_dir.resolve() / "sde-panel"
    while time.time() < end:
        for cmd in ("--menu", "--quick") if shutil.which("xdotool") else ():
            sp.run([str(panel), cmd], env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL, timeout=5)
            time.sleep(0.8)
            xdo("key", "Escape")
            time.sleep(0.4)


def perf_stats(name):
    p = procs.get(name)
    log = home / f"{name}.log"
    text = log.read_text(errors="replace")
    # Only builds that announced SDE_PERF_STATS install the SIGUSR2 handler;
    # the default action would terminate older binaries.
    if p and p.poll() is None and "sde-perf: enabled" in text:
        os.kill(p.pid, signal.SIGUSR2)
        time.sleep(0.3)
        text = log.read_text(errors="replace")
    rows = [l for l in text.splitlines() if l.startswith("sde-perf:")]
    return rows


def snapshot_stats():
    return {n: perf_stats(n) for n in ("sde-panel", "sde-desktop", "sde-notifyd")}


results = {"phases": {}}
try:
    xv = start("Xvfb", ["Xvfb", args.display, "-screen", "0", args.size + "x24", "-nolisten", "tcp", "-ac"])
    for _ in range(50):
        if Path(f"/tmp/.X11-unix/X{args.display.lstrip(':')}").exists():
            break
        time.sleep(0.1)
    bus = sp.run(["dbus-daemon", "--session", "--fork", "--print-address=1", "--print-pid=1"],
                 env=env, stdout=sp.PIPE, text=True)
    if bus.returncode == 0:
        addr, bus_pid = bus.stdout.split()[:2]
        env["DBUS_SESSION_BUS_ADDRESS"] = addr
    else:
        bus_pid = None
    wm = args.wm or which("sde-wm", [str(args.bin_dir)]) or which("openbox")
    if wm:
        start("wm", [wm, "--config-file", str(args.data_dir / "openbox/rc.xml")])
        time.sleep(0.8)
    start("sde-desktop", [str(args.bin_dir.resolve() / "sde-desktop")])
    start("sde-panel", [str(args.bin_dir.resolve() / "sde-panel")])
    start("sde-notifyd", [str(args.bin_dir.resolve() / "sde-notifyd")])
    if not args.no_compositor:
        comp = args.compositor or which("sde-compositor", [str(args.bin_dir)]) or which("picom")
        conf = args.compositor_config or (args.data_dir / "compositor/sde.conf")
        if comp:
            start("compositor", [comp, "--config", str(conf)])
    time.sleep(args.warmup)
    dead = [n for n, p in procs.items() if p.poll() is not None]
    if dead:
        print("warning: exited early:", ", ".join(dead), file=sys.stderr)
    base_stats = snapshot_stats()
    results["phases"]["idle"] = measure(args.seconds)
    results["stats_idle"] = snapshot_stats()
    if args.interact:
        results["phases"]["pointer"] = measure(args.seconds, sweep_pointer)
        results["stats_pointer"] = snapshot_stats()
        results["phases"]["popups"] = measure(args.seconds, popups)
        results["stats_popups"] = snapshot_stats()
    results["stats_base"] = base_stats
finally:
    stop_all()
    if 'bus_pid' in dir() and bus_pid:
        try:
            os.kill(int(bus_pid), signal.SIGTERM)
        except (ProcessLookupError, ValueError):
            pass
    if args.keep_logs:
        args.keep_logs.mkdir(parents=True, exist_ok=True)
        for f in home.glob("*.log"):
            shutil.copy2(f, args.keep_logs / f.name)

for phase, data in results["phases"].items():
    print(f"\n== {phase} ({args.seconds:.0f} s)")
    print(f"{'process':<14}{'cpu %':>8}{'helpers %':>11}{'wakeups/s':>11}{'rss MB':>8}")
    for n, r in data.items():
        if not r:
            print(f"{n:<14}{'(exited)':>8}")
            continue
        print(f"{n:<14}{r['cpu_pct']:>8.2f}{r['helper_cpu_pct']:>11.2f}{r['wakeups_per_s']:>11.1f}"
              f"{r['rss_kb'] / 1024:>8.1f}")
    for n, rows in results.get("stats_" + phase, {}).items():
        for row in [r for r in rows if "enabled" not in r][-4:]:
            print("  ", n, row)
if args.json:
    args.json.write_text(json.dumps(results, indent=2))
