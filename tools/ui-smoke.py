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
parser.add_argument("--themes-dir", type=Path, default=Path(__file__).resolve().parent.parent / "build/themes",
                    help="SDE themes built by tools/build-themes.py (default: build/themes)")
parser.add_argument("--test-dir", type=Path, default=None,
                    help="QA fixtures (sde-panel-service-fixture); default: dist/test-tools or build/tests "
                         "when there. Without them the account administration checks are skipped.")
parser.add_argument("--theme", default="",
                    help="[appearance] lines, e.g. 'style=fluent\\ntheme=dark\\naccent_name=green'")
parser.add_argument("--size", default="1366x768")
parser.add_argument("--display", type=int, default=98, help="Xvfb display number")
parser.add_argument("--scale", type=int, default=100, choices=[75, 100, 125, 150, 175, 200])
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
    DISPLAY=f"127.0.0.1:{args.display}" if args.tcp_display else f":{args.display}",
    HOME=str(home),
    XDG_CONFIG_HOME=str(home / ".config"),
    XDG_CONFIG_DIRS=str(home / "system"),
    XDG_DATA_DIRS=str(args.data_dir.parent)
    + ":"
    + env.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share"),
    # Xvfb has no GPU: GTK4 clients (if any) use the software renderer.
    GSK_RENDERER="cairo",
    LANG="C.UTF-8",
    LC_ALL="C.UTF-8",
    XDG_RUNTIME_DIR=str(home / "run"),
    SDE_PANEL_QA_DIR=str(home / "qa"),
    XDG_CACHE_HOME=str(home / ".cache"),
    # The Action Center's weather from a fixed report (no network in QA).
    SDE_WEATHER_FIXTURE=os.environ.get("SDE_WEATHER_FIXTURE", "partly"),
    # Calculator writes its display (expression, result, error) here.
    SDE_CALC_QA_FILE=str(home / "qa" / "calculator.txt"),
    # 0.2: the stock account pictures (/usr/share/Avatar) are test fixtures.
    SDE_AVATAR_DIR=str(Path(__file__).resolve().parent.parent / "tests/fixtures/avatars"),
)
# The smoke check exercises the native interface and process helpers. A real
# session bus is inherited when available; otherwise fail bus lookups quickly.
env.setdefault("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-sde-ui-smoke-bus")
(home / "run").mkdir(mode=0o700)
(home / ".config/sde").mkdir(parents=True)
# The SDE themes (SDE-Graphite-Light-Blue, ...) built next to the binaries,
# so SDE WM frames and GTK windows use them without an installation.
if args.themes_dir and (args.themes_dir / "SDE-Graphite-Light-Blue/openbox-3/themerc").is_file():
    (home / ".local/share").mkdir(parents=True)
    (home / ".local/share/themes").symlink_to(args.themes_dir.resolve())
    print("themes", args.themes_dir, flush=True)
(home / "bin").mkdir()
(home / "qa").mkdir()
fixtures = home / "fixtures"
fixtures.mkdir()
# Software Updater: fake python-apt, flatpak and desktop files so that the
# Critical, Security, Main and Flatpak groups all appear (tools/update_fixture.py).
import sys as _sys  # noqa: E402

_sys.path.insert(0, str(Path(__file__).resolve().parent))
import update_fixture  # noqa: E402

update_fixture.write(fixtures, home / "bin", home / "update-root")
env["SDE_UPDATE_ROOT"] = str(home / "update-root")
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


def open_popup(title, *command):
    """Sends an sde-panel popup command and returns the popup window. A
    command can be lost while the panel relayouts after a configuration
    change (seen under load): it is sent again while the popup does not
    show, never while it does (the commands toggle)."""
    for _ in range(3):
        if run("xdotool", "search", "--onlyvisible", "--name", "^" + title + "$", check=False).stdout.strip():
            break
        run("sde-panel", *command)
        for _ in range(40):
            if run("xdotool", "search", "--onlyvisible", "--name", "^" + title + "$", check=False).stdout.strip():
                break
            time.sleep(0.1)
        else:
            continue
        break
    return find("^" + title + "$")


def control(title, name):
    """Window-relative (x, y, w, h) of a named control from the panel QA dump."""
    path = home / "qa" / (title.replace(" ", "-") + ".txt")
    for _ in range(80):
        if path.exists():
            for line in path.read_text().splitlines():
                f = line.split("\t")
                if len(f) >= 6 and f[0] == name and f[5] == "1":
                    return tuple(int(v) for v in f[1:5])
        time.sleep(0.1)
    raise RuntimeError(f"{title}: no control {name!r}")


def click_control(win, title, name):
    x, y, w, h = control(title, name)
    g = geom(win)
    run("xdotool", "mousemove", str(g["X"] + x + w // 2), str(g["Y"] + y + h // 2), "click", "1")
    time.sleep(0.5)


def wait_input(title):
    """Waits until a panel popup holds the keyboard (<qa>/<title>.input, see
    qa_mark_input in src/panel/popup.cpp): under load keys sent earlier
    reach another window."""
    marker = home / "qa" / (title.replace(" ", "-") + ".input")
    for _ in range(100):
        if marker.exists():
            return
        time.sleep(0.1)
    raise AssertionError(f"{title} never took the keyboard")


def escape_popup(title):
    """Escape closes a panel popup. Pressed only once the popup holds the
    keyboard: otherwise the popup stays open, and the next command toggles
    it closed instead of opening it."""
    wait_input(title)
    run("xdotool", "key", "Escape")
    for _ in range(80):
        if run("xdotool", "search", "--onlyvisible", "--name", f"^{title}$", check=False).returncode != 0:
            return
        time.sleep(0.1)
    raise AssertionError(f"{title} still open after Escape")


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
        f'[appearance]\ntheme={theme}\nscale={scale}\n{args.theme.encode().decode("unicode_escape")}\n[panel]\n{("style=" + style) if style else ""}\nposition={'top' if top else 'bottom'}\nwidth_percent=96\n[action_center]\nmulticolor=true\nclock_style={"analog" if analog else "digital"}\n{extra}\n[update]\n# No PackageKit/APT refresh from QA runs.\nauto_refresh=false\n'
    )


def wait_page(page):
    """The Control Center shows |page| (its QA dump names it)."""
    path = home / "qa" / "Control-Center.txt"
    for _ in range(80):
        if path.exists() and path.read_text().startswith("@page:" + page + "\t"):
            return
        time.sleep(0.1)
    raise RuntimeError("Control Center did not show page " + page)


def control_center_checks(win):
    """0.2: SDE Settings is the Control Center: the overview is a hub of
    every setting by category whose tiles keep inside the page (no sideways
    scrolling, also at 1024x600 and 150 %), with a search across them."""
    wait_page("home")
    g = geom(win)
    lines = [l.split("\t") for l in (home / "qa" / "Control-Center.txt").read_text().splitlines()]
    names = [f[0] for f in lines]
    for category in ("personalization", "system", "network", "devices", "administration", "about"):
        assert "category:" + category in names, (category, names)
    for tile in ("appearance", "desktop", "panel", "start-menu", "clock", "notifications", "display",
                 "sound", "power", "date-time", "users", "default-apps", "startup", "keyboard", "mouse",
                 "network", "proxy", "bluetooth", "drives", "printers", "updates", "drivers", "reports",
                 "sysmon", "about"):
        assert "tile:" + tile in names, (tile, names)
    for f in lines:
        if f[0].startswith("tile:") or f[0].startswith("category:"):
            x, w = int(f[1]), int(f[3])
            assert x >= 0 and x + w <= g["WIDTH"] - 4, ("sideways overflow", f, g)
    # The window itself fits the screen.
    assert g["X"] >= 0 and g["X"] + g["WIDTH"] <= W and g["Y"] + g["HEIGHT"] <= H, g
    for page in ("users", "start-menu", "date-time"):
        assert "nav:" + page in names, (page, names)
    # The search: "avatar" finds Users & Avatar; Enter opens it.
    click_control(win, "Control Center", "Find a setting")
    run("xdotool", "type", "--delay", "40", "avatar")
    for _ in range(50):
        text = (home / "qa" / "Control-Center.txt").read_text()
        if "category:results" in text and "tile:users" in text and "tile:appearance" not in text:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("the overview search did not find Users & Avatar")
    shot("02-settings-overview-search")
    run("xdotool", "key", "Return")
    wait_page("users")
    print("CONTROL CENTER OVERVIEW, CATEGORIES AND SEARCH PASS", flush=True)


def users_page_checks():
    """0.2: Users & Avatar: the stock pictures of SDE_AVATAR_DIR; one click
    makes ~/.face a 256x256 PNG (with ~/.face.icon), shown at once."""
    import struct
    # No AccountsService of the machine running the checks.
    avatar_env = dict(env, DBUS_SYSTEM_BUS_ADDRESS="unix:path=/nonexistent-sde-ui-smoke-system-bus")
    log = open(home / "sde-settings-users.log", "w")
    app = sp.Popen(["sde-settings", "--page", "avatar"], env=avatar_env, stdout=log, stderr=log)
    procs.append(app)
    win = find("^Control Center$")
    wait_page("users")
    # The full name field (inactive here: no AccountsService to change it).
    names = [l.split("\t")[0] for l in (home / "qa" / "Control-Center.txt").read_text().splitlines()]
    for name in ("Your full name", "Sample Forest", "Sample Sunrise"):
        assert name in names, (name, names)
    # Without AccountsService the accounts are listed, nothing is offered.
    assert any(n.startswith("user:") for n in names) and "Add User..." not in names, names
    shot("16-control-center-users")
    click_control(win, "Control Center", "Sample Sunrise")
    face = home / ".face"
    for _ in range(80):
        if face.exists():
            break
        time.sleep(0.1)
    data = face.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", data[:16]
    assert struct.unpack(">II", data[16:24]) == (256, 256), data[16:24]
    assert os.readlink(home / ".face.icon") == ".face"
    time.sleep(1.0)
    shot("16-control-center-users-avatar")
    app.terminate()
    app.wait()
    print("CONTROL CENTER USERS & AVATAR PASS", flush=True)


def cc_names():
    path = home / "qa" / "Control-Center.txt"
    return [l.split("\t") for l in path.read_text().splitlines()] if path.exists() else []


def cc_reveal(win, name):
    """Wheels the Control Center page until control |name| is fully shown."""
    g = geom(win)
    for _ in range(80):
        rows = [f for f in cc_names() if f[0] == name]
        if rows and rows[0][5] == "1":
            return
        down = not rows or int(rows[0][2]) > g["HEIGHT"] // 2
        # Several wheel steps while it is far away (one step is a few lines).
        far = 1 if not rows else max(1, min(12, abs(int(rows[0][2]) - g["HEIGHT"] // 2) // 60))
        run("xdotool", "mousemove", str(g["X"] + g["WIDTH"] - 40), str(g["Y"] + g["HEIGHT"] // 2),
            "click", "--repeat", str(far), "--delay", "15", "5" if down else "4")
        time.sleep(0.15)
    raise AssertionError(("Control Center: cannot scroll to", name, [f[0] for f in cc_names()]))


def cc_press(win, name):
    for _ in range(80):
        if any(f[0] == name for f in cc_names()):
            break
        time.sleep(0.1)
    cc_reveal(win, name)
    click_control(win, "Control Center", name)


def user_admin_checks():
    """0.2 Alpha 2: Users & Avatar manages the users of this computer through
    AccountsService (the fixture's fake one, on the session bus): add a user
    (user name made from the full name, password hashed with crypt(3)), make
    it an administrator, lock it, remove it with its files; your own password
    with passwd(1) (a fake one that checks the conversation)."""
    tests = [d for d in ([args.test_dir] if args.test_dir else
                         [Path("dist/test-tools"), Path(__file__).resolve().parent.parent / "build/tests"])
             if d and (d / "sde-panel-service-fixture").is_file()]
    if not tests:
        print("SKIP account administration: no sde-panel-service-fixture (--test-dir)", flush=True)
        return
    bus = env["DBUS_SESSION_BUS_ADDRESS"]
    log = home / "accounts.log"
    fenv = dict(env, SDE_FIXTURE_SERVICES="accounts")
    out_log = open(home / "accounts-fixture.log", "w")
    fixture = sp.Popen([str(tests[0].resolve() / "sde-panel-service-fixture"), str(log)], env=fenv,
                       stdout=out_log, stderr=out_log)
    procs.append(fixture)
    for _ in range(80):
        if log.exists() and "READY" in log.read_text():
            break
        time.sleep(0.1)
    else:
        raise AssertionError("the AccountsService fixture did not start")

    def logged(text):
        for _ in range(100):
            if text in log.read_text():
                return
            time.sleep(0.1)
        raise AssertionError(("not asked of AccountsService", text, log.read_text()))

    (home / "bin/passwd").write_text(
        "#!/bin/sh\nstty -echo 2>/dev/null\n"
        "printf 'Current password: '; read cur; printf '\\n'\n"
        "[ \"$cur\" = 'old secret 1' ] || { echo 'passwd: Authentication token manipulation error'; exit 10; }\n"
        "printf 'New password: '; read new; printf '\\n'\n"
        "printf 'Retype new password: '; read again; printf '\\n'\n"
        "[ \"$new\" = \"$again\" ] || exit 4\n"
        "echo \"$new\" > \"$HOME/passwd-changed\"\n"
        "echo 'passwd: password updated successfully'\n")
    (home / "bin/passwd").chmod(0o755)
    app_log = open(home / "sde-settings-accounts.log", "w")
    app = sp.Popen(["sde-settings", "--page", "accounts"], env=dict(env, DBUS_SYSTEM_BUS_ADDRESS=bus),
                   stdout=app_log, stderr=app_log)
    procs.append(app)
    win = find("^Control Center$")
    wait_page("users")
    for _ in range(80):
        names = [f[0] for f in cc_names()]
        if "user:sam" in names and "user:mia" in names and "Add User..." in names:
            break
        time.sleep(0.1)
    else:
        raise AssertionError(("the accounts are not listed", names))
    g = geom(win)
    for f in cc_names():
        if f[0].startswith("user:"):
            x, w = int(f[1]), int(f[3])
            assert x >= 0 and x + w <= g["WIDTH"], ("sideways overflow", f, g)
    shot("16-control-center-users-list")
    # Add Robin Example: the user name follows the full name.
    cc_press(win, "Add User...")
    cc_press(win, "New user's full name")
    run("xdotool", "type", "--delay", "30", "Robin Example")
    cc_press(win, "New user's password")
    run("xdotool", "type", "--delay", "30", "correct horse battery")
    cc_press(win, "Confirm the new user's password")
    run("xdotool", "type", "--delay", "30", "correct horse battery")
    time.sleep(0.4)
    shot("16-control-center-users-add")
    cc_press(win, "Add User")
    logged("ACCOUNTS CreateUser robin Robin Example 0")
    for _ in range(50):
        if re.search(r"ACCOUNTS SetPassword robin \$(y|6)\$", log.read_text()):
            break
        time.sleep(0.1)
    else:
        raise AssertionError(("no crypt(3) hash for robin", log.read_text()))
    assert "plain" not in log.read_text(), log.read_text()
    # Robin's card: an administrator, then locked, then removed with the files.
    for _ in range(50):
        if any(f[0] == "Lock account" for f in cc_names()):
            break
        time.sleep(0.1)
    cc_press(win, "Account type")
    run("xdotool", "key", "Down", "Return")
    logged("ACCOUNTS SetAccountType robin 1")
    time.sleep(0.8)
    cc_press(win, "Lock account")
    logged("ACCOUNTS SetLocked robin true")
    time.sleep(0.8)
    cc_reveal(win, "user:robin")
    shot("16-control-center-users-card")
    cc_press(win, "Remove User...")
    cc_press(win, "Delete Files")
    logged("ACCOUNTS DeleteUser robin remove-files")
    for _ in range(50):
        if not any(f[0] == "user:robin" for f in cc_names()):
            break
        time.sleep(0.1)
    else:
        raise AssertionError("Robin is still listed")
    # Your own password, through passwd(1).
    cc_press(win, "Change Password...")
    cc_press(win, "Current password")
    run("xdotool", "type", "--delay", "30", "old secret 1")
    cc_press(win, "New password")
    run("xdotool", "type", "--delay", "30", "a new secret 22")
    cc_press(win, "Confirm new password")
    run("xdotool", "type", "--delay", "30", "a new secret 22")
    cc_press(win, "Change Password")
    for _ in range(80):
        if (home / "passwd-changed").exists():
            break
        time.sleep(0.1)
    assert (home / "passwd-changed").read_text().strip() == "a new secret 22"
    (home / "passwd-changed").unlink()
    (home / "bin/passwd").unlink()
    app.terminate()
    app.wait()
    fixture.terminate()
    fixture.wait()
    print("CONTROL CENTER USER ACCOUNTS PASS", flush=True)


def installer_checks():
    """0.2.0 Alpha 3: Debrun reads a generated package with a fake package
    system (tools/debrun_fixture.py) and is ready to install it; the
    AppImage Runner refuses an unsigned AppImage (tools/appimage_fixture.py)
    with Delete as the default, and Escape keeps the file."""
    import debrun_fixture
    import appimage_fixture
    fx = home / "debrun-fixture"
    denv = debrun_fixture.write(fx, "new")
    log = open(home / "debrun.log", "w")
    deb = sp.Popen(["debrun", str(fx / "packages/waveform-studio_3.2.0-1_amd64.deb")], env=dict(env, **denv),
                   stdout=log, stderr=log)
    procs.append(deb)
    win = find("Debrun$")
    qa = home / "qa" / "Debrun.txt"
    for _ in range(150):
        if qa.exists() and "@state:ready" in qa.read_text():
            break
        time.sleep(0.1)
    else:
        raise AssertionError(("Debrun is not ready", qa.read_text() if qa.exists() else "no QA dump"))
    text = qa.read_text()
    assert "@action:install" in text and "@change:apt_source" in text, text
    g = geom(win)
    assert g["X"] >= 0 and g["X"] + g["WIDTH"] <= W and g["Y"] + g["HEIGHT"] <= H, g
    shot("30-debrun")
    deb.terminate()
    deb.wait()
    files = appimage_fixture.write(home / "appimages")
    policy = home / "appimage.conf"
    policy.write_text("policy=verified-only\n")
    log = open(home / "sde-appimage.log", "w")
    ai = sp.Popen(["sde-appimage", str(files["unsigned"])], env=dict(env, SDE_APPIMAGE_POLICY=str(policy)),
                  stdout=log, stderr=log)
    procs.append(ai)
    win = find("AppImage Runner$")
    aqa = home / "qa" / "AppImage.txt"
    for _ in range(100):
        if aqa.exists() and "@focus:Delete" in aqa.read_text():
            break
        time.sleep(0.1)
    else:
        raise AssertionError(("the AppImage window has no default", aqa.read_text() if aqa.exists() else ""))
    names = [l.split("\t")[0] for l in aqa.read_text().splitlines()]
    assert "@verdict:unsigned" in names and "Run Anyway" not in names, names
    g = geom(win)
    assert g["X"] >= 0 and g["X"] + g["WIDTH"] <= W and g["Y"] + g["HEIGHT"] <= H, g
    shot("31-appimage-unsigned")
    run("xdotool", "windowfocus", "--sync", win, "key", "Escape", check=False)
    assert ai.wait(timeout=10) == 1
    assert files["unsigned"].exists() and not files["unsigned"].stat().st_mode & 0o111
    print("INSTALLERS PASS", flush=True)


def account_rename_checks():
    """0.2.0 Alpha 3: Users & Avatar renames Sam at the next restart: the
    fake AccountsService of the fixture and the real sde-account-rename on
    a fake root (no pkexec, nothing of this computer changes); the request
    is shown with Cancel Rename, which removes it."""
    tests = [d for d in ([args.test_dir] if args.test_dir else
                         [Path("dist/test-tools"), Path(__file__).resolve().parent.parent / "build/tests"])
             if d and (d / "sde-panel-service-fixture").is_file()]
    helpers = [p for p in (args.bin_dir / "sde-account-rename", args.bin_dir.parent / "libexec/sde/sde-account-rename",
                           Path(__file__).resolve().parent.parent / "build/src/sde-account-rename") if p.is_file()]
    if not tests or not helpers:
        print("SKIP account rename: no sde-panel-service-fixture or sde-account-rename", flush=True)
        return
    root = home / "rename-root"
    (root / "etc").mkdir(parents=True)
    (root / "home/sam").mkdir(parents=True)
    (root / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/bash\nsam:x:1001:1001:Sam Rivera:/home/sam:/bin/bash\n"
                                     "mia:x:1002:1002:Mia Chen:/home/mia:/bin/bash\n")
    (root / "etc/group").write_text("root:x:0:\nsam:x:1001:\nmia:x:1002:\n")
    requests = home / "rename-requests"
    log = home / "rename-accounts.log"
    out_log = open(home / "rename-fixture.log", "w")
    fixture = sp.Popen([str(tests[0].resolve() / "sde-panel-service-fixture"), str(log)],
                       env=dict(env, SDE_FIXTURE_SERVICES="accounts"), stdout=out_log, stderr=out_log)
    procs.append(fixture)
    for _ in range(80):
        if log.exists() and "READY" in log.read_text():
            break
        time.sleep(0.1)
    else:
        raise AssertionError("the AccountsService fixture did not start")
    app_log = open(home / "sde-settings-rename.log", "w")
    app = sp.Popen(["sde-settings", "--page", "accounts"],
                   env=dict(env, DBUS_SYSTEM_BUS_ADDRESS=env["DBUS_SESSION_BUS_ADDRESS"],
                            SDE_ACCOUNT_RENAME_HELPER=str(helpers[0].resolve()), SDE_ACCOUNT_RENAME_DIR=str(requests),
                            SDE_ACCOUNT_RENAME_ROOT=str(root)),
                   stdout=app_log, stderr=app_log)
    procs.append(app)
    win = find("^Control Center$")
    wait_page("users")
    cc_press(win, "user:sam")
    cc_press(win, "Rename this account")
    cc_press(win, "New user name")
    run("xdotool", "type", "--delay", "30", "samuel")
    cc_press(win, "Rename at Restart")
    request = requests / "1001.rename"
    for _ in range(100):
        if request.exists() and any(f[0] == "@rename:sam:samuel:pending" for f in cc_names()):
            break
        time.sleep(0.1)
    else:
        raise AssertionError(("the rename was not recorded or shown", [f[0] for f in cc_names()]))
    assert "new_home=/home/samuel" in request.read_text(), request.read_text()
    cc_reveal(win, "Cancel the rename")
    shot("16-control-center-users-rename")
    cc_press(win, "Cancel the rename")
    for _ in range(100):
        if not request.exists():
            break
        time.sleep(0.1)
    else:
        raise AssertionError("Cancel Rename left the request")
    app.terminate()
    app.wait()
    fixture.terminate()
    fixture.wait()
    print("CONTROL CENTER ACCOUNT RENAME PASS", flush=True)


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
    xv = spawn("Xvfb", f":{args.display}", "-screen", "0", args.size + "x24", *transport)
    time.sleep(0.6)
    if xv.poll() is not None:
        raise RuntimeError("Could not start a private test display")
    # Absolute: SDE WM ignores a relative --config-file and would run unthemed.
    spawn("sde-wm", "--config-file", str((args.data_dir / "openbox/rc.xml").resolve()))
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
    # A command can race the panel starting up (seen ~2 in 80 runs): send
    # it again while the menu does not show.
    for attempt in range(3):
        run("sde-panel", "--menu")
        for _ in range(30):
            if run("xdotool", "search", "--onlyvisible", "--name", "^SDE Menu$", check=False).stdout.strip():
                break
            time.sleep(0.1)
        else:
            continue
        break
    menu = find("^SDE Menu$")
    g = geom(menu)
    assert g["X"] >= 0 and g["Y"] >= 0 and g["X"] + g["WIDTH"] <= W and g["Y"] + g["HEIGHT"] <= H, g
    shot("00-start-menu")
    # The menu draws its own controls and lists them in the QA dump: the rail,
    # the A-Z list with its category filter and the tile groups. There is no
    # search field (search is on the panel) and no row of category chips.
    path = home / "qa" / "SDE-Menu.txt"
    control("SDE Menu", "@tiles")
    names = [l.split("\t")[0] for l in path.read_text().splitlines()]
    rail = ("rail:account", "rail:documents", "rail:pictures", "rail:gallery", "rail:settings",
            "rail:power")
    for name in rail:
        assert name in names, (name, names)
    # Top to bottom in this order, Power off last and on screen.
    ys = [control("SDE Menu", n)[1] for n in rail]
    assert ys == sorted(ys), ys
    assert ys[-1] + control("SDE Menu", "rail:power")[3] <= g["HEIGHT"], (ys, g)
    assert "search" not in names and not any(n.startswith("category:") for n in names), names
    # Alpha 51: Log Out (sde-logout.desktop) is no application of the list.
    assert "app:Log Out" not in names, names
    assert any(n.startswith("tile:") for n in names) and any(n.startswith("group:") for n in names), names
    for n in names:
        if n.startswith("tile:") or n.startswith("app:"):
            x, y, w, h = control("SDE Menu", n)
            assert 0 <= x and x + w <= g["WIDTH"], (n, x, w, g)
    # Narrow screens (1024 px at 150 %) show only the search icon.
    try:
        control("SDE Panel", "search")
    except RuntimeError:
        control("SDE Panel", "search-icon")
    # The category filter narrows the list to Accessories.
    click_control(menu, "SDE Menu", next(n for n in names if n.startswith("filter:")))
    run("xdotool", "key", "Down", "Down", "Return")
    time.sleep(.5)
    control("SDE Menu", "filter:Accessories")
    shot("00-accessories")
    # Typing in the menu searches on the panel.
    run("xdotool", "type", "calculator")
    find("^SDE Search$")
    time.sleep(.5)
    shot("00-start-search")
    run("xdotool", "key", "Return")
    calculator = find("^Calculator$")
    run("xdotool", "windowactivate", "--sync", calculator)
    # Native SDE Calculator (alpha50): "2 + 2" must compute 4, typed with and
    # without spaces (the GTK4 component cleared the input instead).
    calc_qa = home / "qa" / "calculator.txt"

    def calculator_shows(expected):
        state = {}
        for _ in range(50):
            if calc_qa.exists():
                state = dict(l.split("=", 1) for l in calc_qa.read_text().splitlines() if "=" in l)
                if state.get("evaluated") == "1" and state.get("result") == expected:
                    return state
            time.sleep(0.1)
        raise AssertionError(("Calculator result", expected, state))

    for expression, expected in [("2+2", "4"), ("2 + 2", "4"), ("sin(30)+2^3", "8.5")]:
        run("xdotool", "key", "Escape")
        run("xdotool", "type", "--delay", "30", expression)
        run("xdotool", "key", "Return")
        state = calculator_shows(expected)
        assert state.get("error") == "", state
    run("xdotool", "key", "Escape")
    run("xdotool", "type", "--delay", "30", "1/0")
    run("xdotool", "key", "Return")
    time.sleep(0.4)
    state = dict(l.split("=", 1) for l in calc_qa.read_text().splitlines() if "=" in l)
    assert state["error"] == "Cannot divide by zero" and state["expression"], state  # input kept
    run("xdotool", "key", "Escape")
    run("xdotool", "type", "--delay", "30", "2+2")
    run("xdotool", "key", "Return")
    calculator_shows("4")
    time.sleep(0.3)
    shot("11-calculator-native")
    # The window must contain painted keys, not just a title (any theme).
    cg = geom(calculator)
    crop = f"{cg['WIDTH']}x{cg['HEIGHT']}+{cg['X']}+{cg['Y']}"
    spread = run("convert", str(out / "11-calculator-native.png"), "-crop", crop, "+repage",
                 "-colorspace", "Gray", "-format", "%[fx:standard_deviation]", "info:").stdout.strip()
    assert float(spread) > 0.03, ("Calculator did not paint", spread, cg)
    run("xdotool", "key", "alt+F4")
    # New package metadata becomes searchable while the same menu stays open.
    open_popup("SDE Menu", "--menu")
    wait_input("SDE Menu")
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
    print("SDE WM, COMPOSITOR, START SEARCH AND NATIVE CALCULATOR PASS", flush=True)
    run("sde-panel", "--action-center")
    ac = find("^Action Center$")
    print("AC", geom(ac), flush=True)
    shot("01-action-center-light")
    g = geom(ac)
    assert g["Y"] >= 0 and g["Y"] + g["HEIGHT"] <= H, g
    # Session and settings buttons live in the Action Center footer.
    for name in ("Control Center", "Lock Screen", "Power", "Account settings"):
        control("Action Center", name)
    # The weather picture behind the clock (SDE_WEATHER_FIXTURE): since
    # alpha52 the Action Center has no scrolled body; the clock hero sits
    # inside the popup and only its own buttons are drawn on it.
    names = [l.split("\t")[0] for l in (home / "qa" / "Action-Center.txt").read_text().splitlines()]
    sky = [n for n in names if n.startswith("@weather-")]
    assert sky, names
    wx, wy, ww, wh = control("Action Center", sky[0])
    assert wx >= 0 and wy >= 0 and wy + wh <= g["HEIGHT"], (wx, wy, ww, wh)
    assert any(n.startswith("Weather: ") for n in names), names
    escape_popup("Action Center")
    app = spawn("sde-settings")
    settings = find("^Control Center$")
    shot("02-settings-overview")
    control_center_checks(settings)
    app.terminate()
    app.wait()
    users_page_checks()
    user_admin_checks()
    account_rename_checks()
    installer_checks()
    app = spawn("sde-settings", "--page", "panel")
    settings = find("^Control Center$")
    shot("03-settings-panel")
    app.terminate()
    app.wait()
    app = spawn("sde-settings", "--page", "action-center")
    find("^Control Center$")
    shot("04-settings-action-center")
    app.terminate()
    app.wait()
    # Every page the panel flyouts open directly, plus the wallpaper grid.
    pages = run("sde-settings", "--list-pages").stdout.split()
    for needed in ["sound", "network", "bluetooth", "display", "input", "appearance",
                   "desktop", "about", "system", "users", "start-menu", "date-time"]:
        assert needed in pages, pages
    # sde-control-center is the same program under its new name.
    wrapper = Path(__file__).resolve().parent.parent / "data/compat/sde-control-center"
    assert run(str(wrapper), "--list-pages").stdout.split() == pages
    for page in ["wallpaper", "desktop", "sound", "network", "bluetooth", "power", "keyboard"]:
        app = spawn("sde-settings", "--page", page)
        find("^Control Center$")
        time.sleep(0.8)
        shot("08-settings-" + page)
        app.terminate()
        app.wait()
    for page in ["suite", "system", "about"]:
        app = spawn("sde-settings", "--page", page)
        settings = find("^Control Center$")
        shot("07-settings-" + page)
        if page == "suite" and args.scale == 100:
            # The first suite application's source and copyright buttons,
            # found by their tooltips (alpha 2: a Merkadu card above them
            # moved them).
            click_control(settings, "Control Center", "https://github.com/hugomonizdorego/SDE-Desktop")
            time.sleep(0.3)
            assert (
                home / "opened-uri"
            ).read_text().strip() == "https://github.com/hugomonizdorego/SDE-Desktop"
            click_control(settings, "Control Center", "/usr/share/doc/sde/copyright")
            time.sleep(0.3)
            assert (
                home / "opened-uri"
            ).read_text().strip() == "/usr/share/doc/sde/copyright"
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
    records = [line.split("\t") for line in metadata.splitlines()[1:]]
    assert metadata.startswith("SDE-UPDATES\t2\n") and "example-security" in metadata, metadata
    assert len(records) == 8 and all(len(r) == 19 for r in records), metadata
    assert {r[1] for r in records} == {"kernel", "security", "package"}, metadata
    assert any(r[14] == "required" for r in records), metadata  # glibc: Critical
    assert any(r[17] == "Firefox ESR" for r in records), metadata  # desktop file name
    plan = run("sde-update", "--plan", "flatpak", "system:app/org.gimp.GIMP/x86_64/stable").stdout
    assert plan.splitlines()[1:3] == ["update", "--system"], plan
    for theme in ("light", "dark"):
        config(theme=theme)
        update = spawn("sde-update")
        updater = find("^Software Updater$")
        g = geom(updater)
        assert g["X"] >= 0 and g["Y"] >= 0 and g["Y"] + g["HEIGHT"] <= H, g
        assert g["X"] + g["WIDTH"] <= W, g
        time.sleep(2)  # check.py and flatpak.py
        shot("09-update-" + theme)
        if theme == "light":
            # The technical description (Tab from the tree to its expander).
            run("xdotool", "key", "--window", str(updater), "Tab", "space")
            time.sleep(1)
            shot("09-update-technical")
        update.terminate()
        update.wait()
    config()
    if args.scale == 100:
        # Overview tile -> Panel page -> Short preview; verify persisted live change.
        app = spawn("sde-settings")
        settings = find("^Control Center$")
        click_control(settings, "Control Center", "Panel")
        wait_page("panel")
        click_control(settings, "Control Center", "Short")
        time.sleep(0.6)
        cfg = configparser.ConfigParser()
        cfg.read(home / ".config/sde/sde.conf")
        assert cfg.get("panel", "style") == "short", dict(cfg)
        app.terminate()
        app.wait()
        run("sde-panel", "--section", "network")
        # Tray icons open their own flyout above the panel.
        fly = find("^Network$")
        g = geom(fly)
        pg = geom(wid)
        assert g["X"] >= 0 and g["X"] + g["WIDTH"] <= W, (g, pg)
        assert g["Y"] + g["HEIGHT"] <= pg["Y"], (g, pg)
        shot("06-network-connections")
        escape_popup("Network")
        config("short", compact=True)
        reload_panel()
        ac = open_popup("Action Center", "--action-center")
        click_control(ac, "Action Center", "Do Not Disturb")
        cfg.read(home / ".config/sde/sde.conf")
        assert cfg.getboolean("notifications", "do_not_disturb")
        click_control(ac, "Action Center", "Dark Mode")
        cfg.read(home / ".config/sde/sde.conf")
        assert cfg.get("appearance", "theme") == "dark"
        escape_popup("Action Center")
        print("OVERVIEW NAVIGATION, LIVE PANEL STYLE, NETWORK AND DND PASS", flush=True)
    config("short", "dark", 100, True)
    reload_panel()
    print("short", geom(wid), flush=True)
    open_popup("Action Center", "--quick-settings")
    shot("05-action-center-dark")
    escape_popup("Action Center")
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
    ac = open_popup("Action Center", "--action-center")
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
    escape_popup("Action Center")
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
    # Every program's output (GTK, X and D-Bus warnings, crashes) next to
    # the screenshots, also when the run passed; panel-smoke.py does the same.
    (out / "logs").mkdir(exist_ok=True)
    for f in home.glob("*.log"):
        try:
            (out / "logs" / f.name).write_text(f.read_text(errors="replace"))
        except OSError:
            pass
