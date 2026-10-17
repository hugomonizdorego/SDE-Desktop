#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Isolated native GUI smoke check. Wi-Fi/audio are explicit test fixtures.
import argparse
import configparser
import os, subprocess as sp, time, tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bin-dir", type=Path, default=Path("/usr/bin"))
parser.add_argument("--data-dir", type=Path, default=Path("/usr/share/sde"))
parser.add_argument("--output-dir", type=Path, default=Path("dist/ui-smoke"))
parser.add_argument("--size", default="1366x768")
parser.add_argument("--scale", type=int, default=100, choices=[100, 125, 150, 175, 200])
parser.add_argument(
    "--tcp-display",
    action="store_true",
    help="Use an isolated TCP X server when Unix sockets are unavailable",
)
args = parser.parse_args()
W, H = map(int, args.size.split("x"))
out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
tmp = tempfile.TemporaryDirectory(prefix="sde-ui-")
home = Path(tmp.name)
env = os.environ.copy()
env.update(
    PATH=str(home / "bin") + ":" + str(args.bin_dir.resolve()) + ":" + env["PATH"],
    DISPLAY="127.0.0.1:98" if args.tcp_display else ":98",
    HOME=str(home),
    XDG_CONFIG_HOME=str(home / ".config"),
    XDG_CONFIG_DIRS=str(home / "system"),
    XDG_DATA_DIRS=str(args.data_dir.parent)
    + ":"
    + env.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share"),
    LANG="C.UTF-8",
    LC_ALL="C.UTF-8",
    XDG_RUNTIME_DIR=str(home / "run"),
)
# The smoke check exercises the native interface and process helpers. A real
# session bus is inherited when available; otherwise fail bus lookups quickly.
env.setdefault("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-sde-ui-smoke-bus")
(home / "run").mkdir(mode=0o700)
(home / ".config/sde").mkdir(parents=True)
(home / "bin").mkdir()
(home / "bin/nmcli").write_text("""#!/bin/sh
case "$*" in
*TYPE,STATE,CONNECTION*) printf 'wifi:connected:SDE Design Lab\\n' ;;
'radio wifi') echo enabled ;;
*IN-USE,SIGNAL,SECURITY,SSID*) printf '*:84:WPA2:SDE Design Lab\\n:62:WPA2:School Library\\n:40:--:Guest Network\\n' ;;
*) exit 0 ;;
esac
""")
(home / "bin/pactl").write_text("""#!/bin/sh
case "$1" in
get-sink-volume) echo 'Volume: front-left: 42598 / 65% / -11.23 dB' ;;
get-sink-mute) echo 'Mute: no' ;;
get-default-sink) echo qa-speakers ;;
list) printf 'Sink #1\\nName: qa-speakers\\nDescription: Test Speakers\\n' ;;
subscribe) exit 0 ;;
esac
""")
for p in (home / "bin").iterdir():
    p.chmod(0o755)
procs = []


def run(*args, check=True):
    return sp.run(args, env=env, check=check, text=True, stdout=sp.PIPE, stderr=sp.PIPE)


def spawn(*args):
    log = open(home / (Path(args[0]).name + ".log"), "w")
    p = sp.Popen(args, env=env, stdout=log, stderr=log)
    procs.append(p)
    return p


def find(name):
    for _ in range(80):
        r = run("xdotool", "search", "--onlyvisible", "--name", name, check=False)
        if r.returncode == 0:
            return r.stdout.strip().splitlines()[-1]
        time.sleep(0.1)
    print("PROCS", [(p.args, p.poll()) for p in procs], flush=True)
    for win in run("xdotool", "search", "--name", ".", check=False).stdout.splitlines():
        print(
            "WINDOW",
            win,
            run("xdotool", "getwindowname", win, check=False).stdout,
            flush=True,
        )
    run("import", "-window", "root", str(out / "debug.png"))
    raise RuntimeError(
        "window not found: "
        + name
        + " logs: "
        + str([(p.name, p.read_text()) for p in home.glob("*.log")])
    )


def geom(w):
    return {
        k: int(v)
        for k, v in [
            s.split("=")
            for s in run(
                "xdotool", "getwindowgeometry", "--shell", w
            ).stdout.splitlines()
        ]
    }


def shot(name):
    time.sleep(0.5)
    run("import", "-window", "root", str(out / (name + ".png")))
    print("saved", name, flush=True)


def config(style="floating", theme="light", scale=None, analog=False, top=False):
    if scale is None:
        scale = args.scale
    (home / ".config/sde/sde.conf").write_text(
        f'[appearance]\ntheme={theme}\nscale={scale}\n[panel]\nstyle={style}\nposition={'top' if top else 'bottom'}\nwidth_percent=96\n[action_center]\nmulticolor=true\nclock_style={"analog" if analog else "digital"}\n'
    )


def reload_panel():
    # Property update observed by sde-panel (without another desktop service).
    run(
        "xprop",
        "-root",
        "-f",
        "_SDE_CONFIG_SERIAL",
        "32c",
        "-set",
        "_SDE_CONFIG_SERIAL",
        str(time.time_ns() % 1000000000),
    )
    time.sleep(0.8)


try:
    config()
    transport = (
        ["-nolisten", "unix", "-nolisten", "local", "-listen", "tcp", "-ac"]
        if args.tcp_display
        else ["-nolisten", "tcp", "-ac"]
    )
    xv = spawn("Xvfb", ":98", "-screen", "0", args.size + "x24", *transport)
    time.sleep(0.6)
    if xv.poll() is not None:
        raise RuntimeError("Could not start a private test display")
    spawn("openbox", "--config-file", str(args.data_dir / "openbox/rc.xml"))
    time.sleep(0.6)
    spawn("sde-desktop")
    panel = spawn("sde-panel")
    wid = find("^SDE Panel$")
    print("panel", geom(wid), flush=True)
    time.sleep(1.2)
    run("sde-panel", "--action-center")
    ac = find("^Action Center$")
    print("AC", geom(ac), flush=True)
    shot("01-action-center-light")
    g = geom(ac)
    assert g["Y"] >= 0 and g["Y"] + g["HEIGHT"] <= H, g
    run("xdotool", "key", "Escape")
    app = spawn("sde-settings")
    settings = find("^Settings$")
    shot("02-settings-overview")
    app.terminate()
    app.wait()
    app = spawn("sde-settings", "--page", "panel")
    settings = find("^Settings$")
    shot("03-settings-panel")
    app.terminate()
    app.wait()
    app = spawn("sde-settings", "--page", "action-center")
    find("^Settings$")
    shot("04-settings-action-center")
    app.terminate()
    app.wait()
    if args.scale == 100:
        # Overview card -> Panel page -> Short preview; verify persisted live change.
        app = spawn("sde-settings")
        settings = find("^Settings$")
        g = geom(settings)
        run("xdotool", "mousemove", str(g["X"] + 700), str(g["Y"] + 250), "click", "1")
        time.sleep(0.3)
        run("xdotool", "mousemove", str(g["X"] + 780), str(g["Y"] + 190), "click", "1")
        time.sleep(0.6)
        cfg = configparser.ConfigParser()
        cfg.read(home / ".config/sde/sde.conf")
        assert cfg.get("panel", "style") == "short", dict(cfg)
        app.terminate()
        app.wait()
        run("sde-panel", "--action-center")
        ac = find("^Action Center$")
        g = geom(ac)
        # Expand connections, then collapse, and enable Do Not Disturb.
        run("xdotool", "mousemove", str(g["X"] + 70), str(g["Y"] + 205), "click", "1")
        time.sleep(0.5)
        g = geom(ac)
        assert g["Y"] >= 0 and g["Y"] + g["HEIGHT"] <= H, g
        shot("06-network-connections")
        run("xdotool", "mousemove", str(g["X"] + 70), str(g["Y"] + 205), "click", "1")
        time.sleep(0.5)
        g = geom(ac)
        run("xdotool", "mousemove", str(g["X"] + 310), str(g["Y"] + 205), "click", "1")
        time.sleep(0.5)
        cfg.read(home / ".config/sde/sde.conf")
        assert cfg.getboolean("notifications", "do_not_disturb")
        g = geom(ac)
        run("xdotool", "mousemove", str(g["X"] + 70), str(g["Y"] + 292), "click", "1")
        time.sleep(0.5)
        cfg.read(home / ".config/sde/sde.conf")
        assert cfg.get("appearance", "theme") == "dark"
        run("xdotool", "key", "Escape")
        print("OVERVIEW NAVIGATION, LIVE PANEL STYLE, NETWORK AND DND PASS", flush=True)
    config("short", "dark", 100, True)
    reload_panel()
    print("short", geom(wid), flush=True)
    run("sde-panel", "--quick-settings")
    find("^Action Center$")
    shot("05-action-center-dark")
    run("xdotool", "key", "Escape")
    config("full")
    reload_panel()
    g = geom(wid)
    assert g["X"] == 0 and g["WIDTH"] == W, g
    config("floating")
    reload_panel()
    g = geom(wid)
    assert g["X"] > 0 and g["WIDTH"] < W, g
    config("floating", top=True)
    reload_panel()
    g = geom(wid)
    assert g["Y"] > 0 and g["Y"] < 50, g
    run("sde-panel", "--action-center")
    ac = find("^Action Center$")
    g = geom(ac)
    assert g["Y"] >= 0 and g["Y"] + g["HEIGHT"] <= H, g
    run(
        "xdotool",
        "mousemove",
        str(g["X"] + g["WIDTH"] // 2),
        str(g["Y"] + g["HEIGHT"] // 2),
        "click",
        "5",
        "click",
        "5",
    )
    run("xdotool", "key", "Escape")
    print("PANEL STYLES, TOP PLACEMENT AND POPUP BOUNDS PASS", flush=True)
    if os.environ.get("SDE_UI_DEBUG"):
        print((home / "sde-panel.log").read_text(), flush=True)
except Exception:
    print("LOGS", [(p.name, p.read_text()) for p in home.glob("*.log")], flush=True)
    raise
finally:
    for p in reversed(procs):
        if p.poll() is None:
            p.terminate()
    for p in reversed(procs):
        try:
            p.wait(timeout=3)
        except sp.TimeoutExpired:
            p.kill()
