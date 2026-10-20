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
    # Xvfb has no GPU: exercise GTK4 deterministically with its software renderer.
    GSK_RENDERER="cairo",
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


def config(style=None, theme="light", scale=None, analog=False, top=False, compact=False):
    if scale is None:
        scale = args.scale
    extra = "show_clock=false\nshow_media=false\nshow_tray=false\nmove_status=false\n" if compact else ""
    (home / ".config/sde/sde.conf").write_text(
        f'[appearance]\ntheme={theme}\nscale={scale}\n[panel]\n{("style=" + style) if style else ""}\nposition={'top' if top else 'bottom'}\nwidth_percent=96\n[action_center]\nmulticolor=true\nclock_style={"analog" if analog else "digital"}\n{extra}'
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
    spawn("sde-wm", "--config-file", str(args.data_dir / "openbox/rc.xml"))
    time.sleep(0.6)
    spawn("sde-desktop")
    panel = spawn("sde-panel")
    wid = find("^SDE Panel$")
    print("panel", geom(wid), flush=True)
    g = geom(wid)
    assert g["X"] == 0 and g["WIDTH"] == W, g
    time.sleep(1.2)
    compositor = spawn("sde-compositor", "--config", str(args.data_dir / "compositor/sde.conf"))
    time.sleep(0.6)
    assert compositor.poll() is None, "SDE compositor did not start"
    run("sde-panel", "--menu")
    menu = find("^SDE Menu$")
    g = geom(menu)
    assert g["X"] >= 0 and g["Y"] >= 0 and g["X"] + g["WIDTH"] <= W and g["Y"] + g["HEIGHT"] <= H, g
    shot("00-start-menu")
    # First chip of the category strip (Accessories) opens its page.
    run("xdotool", "mousemove", str(g["X"] + int(60 * args.scale / 100)),
        str(g["Y"] + int(127 * args.scale / 100)), "click", "1")
    shot("00-accessories")
    run("xdotool", "type", "calculator")
    shot("00-start-search")
    run("xdotool", "key", "Return")
    calculator = find("^Calculator$")
    run("xdotool", "windowactivate", "--sync", calculator)
    run("xdotool", "key", "ctrl+a")
    run("xdotool", "type", "sin(30)+2^3")
    run("xdotool", "key", "Return")
    shot("11-calculator-native")
    # A mapped GTK4 window must also contain rendered pixels, not just a title.
    cg = geom(calculator)
    sample = f"12x12+{cg['X'] + 24}+{cg['Y'] + cg['HEIGHT'] // 2}"
    brightness = run("convert", str(out / "11-calculator-native.png"), "-crop", sample,
                     "+repage", "-format", "%[fx:mean]", "info:").stdout.strip()
    assert float(brightness) > 0.70, ("GTK4 Calculator did not paint", brightness, cg)
    run("xdotool", "key", "alt+F4")
    # New package metadata becomes searchable while the same menu stays open.
    run("sde-panel", "--menu")
    find("^SDE Menu$")
    run("xdotool", "type", "SDELiveInstallProbe")
    time.sleep(.3)
    app_dir = home / ".local/share/applications"
    app_dir.mkdir(parents=True, exist_ok=True)
    live = app_dir / "sde-live-install-probe.desktop"
    live.write_text("[Desktop Entry]\nType=Application\nName=SDELiveInstallProbe\nExec=calculator\nCategories=Utility;Calculator;\n")
    time.sleep(2.8)
    run("xdotool", "key", "Return")
    find("^Calculator$")
    run("xdotool", "key", "alt+F4")
    live.unlink()
    print("LIVE APPLICATION INSTALL AND ACCESSORIES CATEGORY PASS", flush=True)
    display_app = spawn("sde-display-settings")
    dw = find("^Display Settings$")
    dg = geom(dw)
    assert dg["X"] >= 0 and dg["Y"] >= 0 and dg["X"] + dg["WIDTH"] <= W and dg["Y"] + dg["HEIGHT"] <= H, dg
    shot("13-display-settings")
    run("xdotool", "mousemove", str(dg["X"] + dg["WIDTH"] - int(130 * args.scale / 100)),
        str(dg["Y"] + int(112 * args.scale / 100)), "click", "1")
    confirm = find("^Keep display settings[?]$")
    cg = geom(confirm)
    shot("14-display-confirm")
    run("xdotool", "mousemove", str(cg["X"] + int(355 * args.scale / 100)),
        str(cg["Y"] + int(142 * args.scale / 100)), "click", "1")
    time.sleep(.5)
    assert (home / ".config/sde/display-profiles").is_dir(), "Confirmed profile was not saved"
    cp = configparser.ConfigParser()
    cp.read(home / ".config/sde/sde.conf")
    assert cp.get("appearance", "scale") == str(args.scale)
    run("xdotool", "mousemove", str(dg["X"] + dg["WIDTH"] - 70), str(dg["Y"] + dg["HEIGHT"] // 2))
    for _ in range(18): run("xdotool", "click", "5")
    shot("15-display-profiles-scale")
    display_app.terminate()
    display_app.wait()
    print("SDE WM, COMPOSITOR, START SEARCH AND GTK4 CALCULATOR PASS", flush=True)
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
        metadata.startswith("SDE-UPDATES\t2\n") and "example-security" in metadata
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
        run("sde-panel", "--section", "network")
        ac = find("^Action Center$")
        g = geom(ac)
        pg = geom(wid)
        assert g["X"] + g["WIDTH"] == pg["X"] + pg["WIDTH"], (g, pg)
        assert g["Y"] + g["HEIGHT"] == pg["Y"], (g, pg)
        shot("06-network-connections")
        run("xdotool", "key", "Escape")
        config("short", compact=True)
        reload_panel()
        run("sde-panel", "--action-center")
        ac = find("^Action Center$")
        g = geom(ac)
        # Compact fixture: two-column tiles start at y=42, DND/Dark are row 3.
        run("xdotool", "mousemove", str(g["X"] + 70), str(g["Y"] + 254), "click", "1")
        time.sleep(0.5)
        cfg.read(home / ".config/sde/sde.conf")
        assert cfg.getboolean("notifications", "do_not_disturb")
        g = geom(ac)
        run("xdotool", "mousemove", str(g["X"] + 300), str(g["Y"] + 254), "click", "1")
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
            if app_name == "sde-view":
                # Photo's Effects side pane (SDE plugin) opens with E.
                run("xdotool", "windowactivate", "--sync", native, check=False)
                run("xdotool", "key", "--window", native, "e")
                time.sleep(1.0)
                shot("10-engine-photo-effects")
            run("xdotool", "windowclose", native, check=False)
            if engine.poll() is None:
                engine.terminate()
        # The actual sudo command exercises the compiled Liria root startup path.
        # Xvfb is private; no authentication or power operation is triggered.
        root_folder = home / "Liria-Administrator-Check"
        root_folder.mkdir()
        admin = spawn("sudo", "liria", "--no-desktop", str(root_folder))
        root_window = find("Liria-Administrator-Check")
        import pwd
        process_id = int(run("xprop", "-id", root_window, "_NET_WM_PID").stdout.split("=")[-1].strip())
        status = Path(f"/proc/{process_id}/status").read_text()
        assert re.search(r"^Uid:\s+0\s+0\s+0\s+0", status, re.M), status
        process_env = dict(item.split(b"=", 1) for item in Path(f"/proc/{process_id}/environ").read_bytes().split(b"\0") if b"=" in item)
        assert process_env.get(b"SDE_LIRIA_ROOT_BUS") == b"1", process_env
        assert process_env.get(b"HOME") == pwd.getpwuid(0).pw_dir.encode(), process_env
        assert process_env.get(b"DBUS_SESSION_BUS_ADDRESS", b"") not in [b"", env.get("DBUS_SESSION_BUS_ADDRESS", "").encode()]
        assert b"XDG_CONFIG_HOME" not in process_env
        shot("12-liria-administrator")
        run("xdotool", "windowactivate", "--sync", root_window)
        run("xdotool", "key", "alt+F4")
        if admin.poll() is None: admin.terminate()
        print(
            "SOURCE-BUILT NINY, LIRIA, XANARA, TACA AND PHOTO NATIVE WINDOWS PASS",
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
