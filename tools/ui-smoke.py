#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Isolated native GUI smoke check. Wi-Fi/audio are explicit test fixtures.
import argparse
import re
import configparser
import os, subprocess as sp, time, tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bin-dir", type=Path, default=Path("/usr/bin"))
parser.add_argument("--data-dir", type=Path, default=Path("/usr/share/sde"))
parser.add_argument("--output-dir", type=Path, default=Path("dist/ui-smoke"))
parser.add_argument(
    "--suite-apps",
    action="store_true",
    help="Also open native suite applications with sample files",
)
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
fixtures = home / "fixtures"
fixtures.mkdir()
(fixtures / "apt_pkg.py").write_text("SELSTATE_HOLD = 1\n")
(fixtures / "apt.py").write_text("""from types import SimpleNamespace as N
def pkg(name, source, security=False):
    c=N(source_name=source,source_version='2.0',version='2.0',size=4096,
        raw_description='UI test fixture',description='UI test fixture',section='utils',
        origins=[N(origin='Debian',site='fixture.invalid',archive='testing',
                   label='Debian-Security' if security else 'Debian',component='main')])
    return N(name=name,candidate=c,installed=N(version='1.0'),is_installed=True,
             is_upgradable=True,_pkg=N(selected_state=0))
def Cache():
    return [pkg('example-security','example-security',True),
            pkg('example-common','example'),pkg('example','example')]
""")
env["PYTHONPATH"] = str(fixtures)
env["SDE_UI_OPEN"] = str(home / "opened-uri")
(home / "bin/xdg-open").write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$SDE_UI_OPEN"\n')
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
    # xdotool can include frame offsets twice for reparented FLTK windows.
    # XWinInfo reports the actual client origin used by our widget coordinates.
    info = run("xwininfo", "-id", str(w)).stdout

    def field(name):
        return int(re.search(re.escape(name) + r"\s*:\s*(-?\d+)", info).group(1))

    return {
        "WINDOW": int(w),
        "X": field("Absolute upper-left X"),
        "Y": field("Absolute upper-left Y"),
        "WIDTH": field("Width"),
        "HEIGHT": field("Height"),
    }


def shot(name):
    time.sleep(0.5)
    run("import", "-window", "root", str(out / (name + ".png")))
    print("saved", name, flush=True)


def config(style=None, theme="light", scale=None, analog=False, top=False):
    if scale is None:
        scale = args.scale
    (home / ".config/sde/sde.conf").write_text(
        f'[appearance]\ntheme={theme}\nscale={scale}\n[panel]\n{("style=" + style) if style else ""}\nposition={'top' if top else 'bottom'}\nwidth_percent=96\n[action_center]\nmulticolor=true\nclock_style={"analog" if analog else "digital"}\n'
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
    g = geom(wid)
    assert g["X"] == 0 and g["WIDTH"] == W, g
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
    for page in ["suite", "system", "about"]:
        app = spawn("sde-settings", "--page", page)
        settings = find("^Settings$")
        shot("07-settings-" + page)
        if page == "suite" and args.scale == 100:
            g = geom(settings)
            run(
                "xdotool",
                "mousemove",
                str(g["X"] + 370),
                str(g["Y"] + 290),
                "click",
                "1",
            )
            time.sleep(0.8)
            assert (
                home / "opened-uri"
            ).read_text().strip() == "https://github.com/mate-desktop/pluma"
            run(
                "xdotool",
                "mousemove",
                str(g["X"] + 720),
                str(g["Y"] + 290),
                "click",
                "1",
            )
            time.sleep(0.3)
            assert (
                home / "opened-uri"
            ).read_text().strip() == "/usr/share/doc/sde/upstream/pluma/copyright"
        run(
            "xdotool",
            "mousemove",
            str(geom(settings)["X"] + geom(settings)["WIDTH"] - 70),
            str(geom(settings)["Y"] + geom(settings)["HEIGHT"] // 2),
            "click",
            "5",
            "click",
            "5",
            "click",
            "5",
        )
        shot("08-settings-" + page + "-scrolled")
        app.terminate()
        app.wait()
    metadata = run("sde-update", "--check").stdout
    assert (
        metadata.startswith("SDE-UPDATES\t1\n") and "example-security" in metadata
    ), metadata
    assert len(metadata.splitlines()) == 3, metadata
    update = spawn("sde-update")
    updater = find("^SDE Update$")
    g = geom(updater)
    assert g["X"] >= 0 and g["Y"] >= 0 and g["Y"] + g["HEIGHT"] <= H, g
    time.sleep(1)
    shot("09-update")
    update.terminate()
    update.wait()
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
    if args.suite_apps:
        import zipfile

        config("full")
        reload_panel()
        folder = home / "SDE-Suite-Check"
        folder.mkdir()
        text = folder / "SDE-sample.txt"
        text.write_text("SDE Suite · Niny native build check\n")
        document = folder / "SDE-sample.pdf"
        stream = b"BT /F1 18 Tf 40 150 Td (SDE Suite - Xanara) Tj ET"
        objects = [
            b"<< /Type /Catalog /Pages 2 0 R >>",
            b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 220] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
            b"<< /Length "
            + str(len(stream)).encode()
            + b" >>\nstream\n"
            + stream
            + b"\nendstream",
        ]
        pdf = b"%PDF-1.4\n"
        offsets = [0]
        for i, obj in enumerate(objects, 1):
            offsets.append(len(pdf))
            pdf += str(i).encode() + b" 0 obj\n" + obj + b"\nendobj\n"
        xref = len(pdf)
        pdf += b"xref\n0 6\n0000000000 65535 f \n"
        pdf += b"".join(f"{offset:010d} 00000 n \n".encode() for offset in offsets[1:])
        pdf += (
            b"trailer << /Size 6 /Root 1 0 R >>\nstartxref\n"
            + str(xref).encode()
            + b"\n%%EOF\n"
        )
        document.write_bytes(pdf)
        archive = folder / "SDE-sample.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.write(text, text.name)
        picture = folder / "SDE-sample.ppm"
        picture.write_bytes(b"P6\n2 2\n255\n" + bytes([22, 125, 154]) * 4)
        for app_name, file, title in [
            ("niny", text, "SDE-sample.txt"),
            ("liria", folder, "SDE-Suite-Check"),
            ("xanara", document, "SDE-sample.pdf"),
            ("taca", archive, "SDE-sample.zip"),
            ("sde-view", picture, "SDE-sample.ppm"),
        ]:
            engine = spawn(app_name, str(file))
            native = find(title)
            identity = run("xprop", "-id", native, "WM_CLASS").stdout.lower()
            expected = app_name.replace("sde-view", "sdeview")
            assert expected in identity.replace("sde-view", "sdeview"), (app_name, identity)
            shot("10-engine-" + app_name)
            run("xdotool", "windowclose", native, check=False)
            if engine.poll() is None:
                engine.terminate()
        print(
            "SOURCE-BUILT NINY, LIRIA, XANARA, TACA AND SDE VIEW NATIVE WINDOWS PASS",
            flush=True,
        )
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
