#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drag and drop smoke test of a built Liria on Xvfb (xdotool).

  tools/liria-dnd-smoke.py --prefix /usr            # an installed Liria
  tools/liria-dnd-smoke.py --prefix stage/usr       # a DESTDIR stage

Starts Xvfb (1366x768, 96 dpi), a window manager when one is installed,
and two Liria windows on a throw-away home folder: the Home view on the
left and a folder with test files on the right. Then it drags with the
real X input path (xdotool) and checks the file system:

  1. a file onto the Downloads card of Quick access   -> moved there
  2. a folder onto the "+" card of Quick access        -> pinned
  3. a file with the right mouse button onto Documents -> the Move/Copy/
     Link menu appears; "Copy Here" copies it

Card positions are those of the SDE Graphite theme at 100 % with the
ribbon off; the test prints which step failed. Exit status 0 = passed,
77 = skipped (no Xvfb/xdotool/Liria).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def wait_for(predicate, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.2)
    return predicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path("/usr"))
    parser.add_argument("--display", default=":187")
    parser.add_argument("--theme", default="SDE-Graphite-Light-Blue")
    parser.add_argument("--themes", type=Path, default=ROOT / "build/themes",
                        help="folder with the built SDE GTK themes (tools/build-themes.py)")
    parser.add_argument("--keep", action="store_true", help="keep the temporary home folder")
    parser.add_argument("--screenshot", type=Path, help="save the screen before the drags (ImageMagick)")
    args = parser.parse_args()
    liria = args.prefix / "bin/liria"
    for tool in ("Xvfb", "xdotool"):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    if not liria.is_file():
        print("skipped: %s not found" % liria)
        return 77

    home = Path(tempfile.mkdtemp(prefix="liria-dnd-"))
    for name in ("Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", "Work/Folder"):
        (home / name).mkdir(parents=True, exist_ok=True)
    (home / ".config/sde").mkdir(parents=True)
    (home / ".config/user-dirs.dirs").write_text("".join(
        'XDG_%s_DIR="$HOME/%s"\n' % (key, name) for key, name in
        (("DESKTOP", "Desktop"), ("DOCUMENTS", "Documents"), ("DOWNLOAD", "Downloads"), ("MUSIC", "Music"),
         ("PICTURES", "Pictures"), ("VIDEOS", "Videos"))))
    (home / ".config/sde/liria.conf").write_text("[ribbon]\nmode=off\n")
    (home / ".config/gtk-3.0").mkdir(parents=True)
    (home / ".config/gtk-3.0/settings.ini").write_text(
        "[Settings]\ngtk-icon-theme-name=%s\n" % ("Papirus" if Path("/usr/share/icons/Papirus").is_dir() else "Adwaita"))
    for name in ("a.txt", "b.txt", "c.txt"):
        (home / "Work" / name).write_text(name)
    env = dict(os.environ, HOME=str(home), DISPLAY=args.display, XDG_CONFIG_HOME=str(home / ".config"),
               XDG_DATA_HOME=str(home / ".local/share"), GTK_THEME=args.theme, NO_AT_BRIDGE="1",
               GSETTINGS_SCHEMA_DIR=str(args.prefix / "share/glib-2.0/schemas"),
               XDG_DATA_DIRS=str(args.prefix / "share") + ":/usr/share")
    env.pop("SDE_SESSION_VERSION", None)
    themes = args.themes
    if (themes / args.theme).is_dir():
        (home / ".themes").mkdir()
        (home / ".themes" / args.theme).symlink_to(themes / args.theme)
    procs = []
    failures = []

    def x(*command):
        return subprocess.run(["xdotool", *map(str, command)], env=env, capture_output=True, text=True).stdout

    def drag(x1, y1, x2, y2, button=1):
        x("mousemove", x1, y1)
        time.sleep(0.3)
        x("mousedown", button)
        for i in range(1, 21):
            x("mousemove", x1 + (x2 - x1) * i // 20, y1 + (y2 - y1) * i // 20)
            time.sleep(0.04)
        time.sleep(0.4)
        x("mouseup", button)
        time.sleep(1.2)

    def check(step, ok):
        print(("ok   " if ok else "FAIL ") + step, flush=True)
        if not ok:
            failures.append(step)

    try:
        procs.append(subprocess.Popen(["Xvfb", args.display, "-screen", "0", "1366x768x24", "-nolisten", "tcp"],
                                      env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1)
        bus = subprocess.run(["dbus-launch", "--sh-syntax"], env=env, capture_output=True, text=True).stdout
        for line in bus.splitlines():
            if line.startswith("DBUS_SESSION_BUS_ADDRESS="):
                env["DBUS_SESSION_BUS_ADDRESS"] = line.split("=", 1)[1].rstrip(";").strip("'")
            if line.startswith("DBUS_SESSION_BUS_PID="):
                procs.append(int(line.split("=", 1)[1].rstrip(";")))
        wm = shutil.which("openbox") or shutil.which("sde-wm")
        if wm:
            procs.append(subprocess.Popen([wm], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            time.sleep(0.5)
        procs.append(subprocess.Popen([str(liria), "--no-desktop", str(home), str(home / "Work")], env=env,
                                      cwd=str(home), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if not wait_for(lambda: x("search", "--onlyvisible", "--name", "^Work$").strip(), 15):
            print("FAIL Liria did not open its windows")
            return 1
        time.sleep(2)
        if args.screenshot and shutil.which("import"):
            subprocess.run(["import", "-window", "root", str(args.screenshot)], env=env)
        # The folder window shows a list (rows stay in name order): Folder,
        # a.txt, b.txt, c.txt; the Home view has four cards per row.
        wid = x("search", "--onlyvisible", "--name", "^Work$").split()[0]
        x("windowactivate", "--sync", wid)
        time.sleep(0.5)
        x("key", "ctrl+2")
        time.sleep(1.5)
        for name, geometry in ((home.name, (0, 0, 690, 740)), ("Work", (700, 0, 666, 500))):
            wid = x("search", "--onlyvisible", "--name", "^%s$" % name).split()[0]
            x("windowsize", wid, geometry[2], geometry[3])
            x("windowmove", wid, geometry[0], geometry[1])
        time.sleep(1.5)
        if args.screenshot and shutil.which("import"):
            subprocess.run(["import", "-window", "root", str(args.screenshot)], env=env)
        # The folder window lists Folder, a.txt, b.txt, c.txt in its first row.
        drag(940, 137, 495, 178)  # a.txt -> Downloads
        check("file dropped on the Downloads card is moved there",
              wait_for(lambda: any((home / "Downloads").glob("*.txt"))))
        drag(940, 111, 615, 273)  # Folder -> "+"
        pins = home / ".config/sde/liria-quick-access"
        check("folder dropped on the + card is pinned",
              wait_for(lambda: pins.is_file() and "Work/Folder" in pins.read_text()))
        before = set(p.name for p in (home / "Work").glob("*.txt"))
        # Icons again (a fresh layout: Folder, b.txt, c.txt): the right
        # button drags b.txt onto the Documents card.
        x("windowactivate", "--sync", x("search", "--onlyvisible", "--name", "^Work$").split()[0])
        x("key", "ctrl+1")
        time.sleep(1.5)
        drag(1125, 100, 375, 178, button=3)
        if args.screenshot and shutil.which("import"):
            subprocess.run(["import", "-window", "root", str(args.screenshot.with_suffix(".ask.png"))], env=env)
        x("key", "Down", "Down", "Return")  # Move Here, Copy Here
        copied = lambda: any((home / "Documents" / name).exists() for name in before)
        check("right-button drag asks and Copy Here copies", wait_for(copied)
              and set(p.name for p in (home / "Work").glob("*.txt")) == before)
    finally:
        for proc in reversed(procs):
            try:
                if isinstance(proc, int):
                    os.kill(proc, 15)
                else:
                    proc.terminate()
                    proc.wait(timeout=5)
            except Exception:
                pass
        if args.keep:
            print("kept", home)
        else:
            shutil.rmtree(home, ignore_errors=True)
    print("Liria drag and drop: %s" % ("passed" if not failures else "%d failure(s)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
