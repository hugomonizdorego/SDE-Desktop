#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ribbon icon smoke test of a built Liria on Xvfb (xdotool).

  tools/liria-ribbon-smoke.py --prefix /usr                    # an installed Liria
  tools/liria-ribbon-smoke.py --prefix stage/usr --scale 150 --screenshots out

Opens a folder with the ribbon shown and checks what the ribbon reports in
LIRIA_SDE_QA_RIBBON (src/liria-sde-ribbon.c) and the window's pixels:

  1. every command has an icon of the icon theme: a full-colour icon, or
     the theme's glyph tinted with a theme colour where the theme draws the
     command in grey only (Papirus draws most commands so), never a plain
     grey one; the theme's full-colour icons are used where it has them;
  2. the sizes are those icon themes draw: 32 and 16 pixels at 100 %, 48
     and 24 at 150 % (--scale), and the ribbon is in colour on the screen;
  3. the Home, Share and View tabs, and the Archive tab when an archive is
     selected, show their commands (--screenshots saves each);
  4. a new icon theme (xsettingsd) changes the icons while Liria runs.

The Drive tab needs an ejectable drive and is not shown here. Exit status
0 = passed, 1 = failed, 77 = skipped (no Xvfb, xdotool, ImageMagick,
xsettingsd or Liria).
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def wait_for(predicate, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    return predicate()


def read_qa(path):
    """(commands, tabs): commands as (label, icon, kind, tint, pixels), tabs
    as {page: (x, y, w, h)}."""
    commands, tabs = [], {}
    try:
        text = path.read_text()
    except OSError:
        return commands, tabs
    for line in text.splitlines():
        fields = line.split("\t")
        if fields[0] == "tab" and len(fields) == 3:
            tabs[fields[1]] = tuple(int(v) for v in fields[2].split(","))
        elif len(fields) == 5:
            commands.append((fields[0], fields[1], fields[2], fields[3], int(fields[4] or 0)))
    return commands, tabs


def pixels(ppm):
    fields, pos = [], 0
    while len(fields) < 4:
        while ppm[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not ppm[end:end + 1].isspace():
            end += 1
        fields.append(ppm[pos:end])
        pos = end
    return ppm[pos + 1:]


def coloured(data, threshold=60):
    return sum(1 for i in range(0, len(data) - 2, 3) if max(data[i:i + 3]) - min(data[i:i + 3]) > threshold)


class Session:
    def __init__(self, args):
        self.args = args
        self.procs = []
        self.liria = None
        self.home = Path(tempfile.mkdtemp(prefix="liria-ribbon-"))
        self.dpi = 96 * args.scale // 100

    def icon_theme(self, name):
        dark = "-Dark" in self.args.theme
        if name == "Papirus" and dark:
            name = "Papirus-Dark"
        self.conf.write_text('Net/ThemeName "%s"\nNet/IconThemeName "%s"\nGtk/FontName "Sans 10"\n'
                             "Xft/DPI %d\nXft/Antialias 1\n" % (self.args.theme, name, self.dpi * 1024))

    def start(self, icons):
        args, home = self.args, self.home
        for name in ("Desktop", "Documents", "Music", "Pictures", "Videos", "Downloads/Projects", ".config/sde",
                     ".config/gtk-3.0"):
            (home / name).mkdir(parents=True, exist_ok=True)
        (home / ".config/user-dirs.dirs").write_text("".join(
            'XDG_%s_DIR="$HOME/%s"\n' % (key, name) for key, name in
            (("DESKTOP", "Desktop"), ("DOCUMENTS", "Documents"), ("DOWNLOAD", "Downloads"), ("MUSIC", "Music"),
             ("PICTURES", "Pictures"), ("VIDEOS", "Videos"))))
        (home / ".config/sde/liria.conf").write_text("[ribbon]\nmode=full\ntab=home\n")
        downloads = home / "Downloads"
        with zipfile.ZipFile(downloads / "Autumn Photos.zip", "w") as z:
            z.writestr("leaves.txt", "red and gold\n")
        (downloads / "Meeting notes.txt").write_text("Agenda\n")
        (downloads / "Budget.csv").write_text("month,amount\nOctober,120\n")
        if args.themes and (args.themes / args.theme).is_dir():
            (home / ".themes").mkdir()
            (home / ".themes" / args.theme).symlink_to(args.themes.resolve() / args.theme)
        self.env = env = dict(
            os.environ, HOME=str(home), DISPLAY=args.display, XDG_CONFIG_HOME=str(home / ".config"),
            XDG_DATA_HOME=str(home / ".local/share"), XDG_CACHE_HOME=str(home / ".cache"),
            GTK_THEME=args.theme, NO_AT_BRIDGE="1", GSETTINGS_BACKEND="memory",
            GSETTINGS_SCHEMA_DIR=str(args.prefix / "share/glib-2.0/schemas"),
            XDG_DATA_DIRS=str(args.prefix / "share") + ":/usr/local/share:/usr/share")
        env.pop("SDE_SESSION_VERSION", None)
        self.procs.append(subprocess.Popen(["Xvfb", args.display, "-screen", "0", args.size + "x24", "-nolisten",
                                            "tcp", "-dpi", str(self.dpi)], stdout=subprocess.DEVNULL,
                                           stderr=subprocess.DEVNULL))
        socket = Path("/tmp/.X11-unix/X" + args.display.lstrip(":").split(".")[0])
        wait_for(socket.exists, 10)
        time.sleep(0.4)
        bus = subprocess.run(["dbus-launch", "--sh-syntax"], env=env, capture_output=True, text=True).stdout
        for line in bus.splitlines():
            if line.startswith("DBUS_SESSION_BUS_ADDRESS="):
                env["DBUS_SESSION_BUS_ADDRESS"] = line.split("=", 1)[1].rstrip(";").strip("'")
            if line.startswith("DBUS_SESSION_BUS_PID="):
                self.procs.append(int(line.split("=", 1)[1].rstrip(";")))
        self.conf = home / "xsettingsd.conf"
        self.icon_theme(icons)
        self.xsettingsd = subprocess.Popen(["xsettingsd", "-c", str(self.conf)], env=env,
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.procs.append(self.xsettingsd)
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
                self.liria.wait()
        for proc in reversed(self.procs):
            try:
                if isinstance(proc, int):
                    os.kill(proc, signal.SIGTERM)
                else:
                    proc.terminate()
                    proc.wait(timeout=5)
            except Exception:
                pass
        if not self.args.keep:
            shutil.rmtree(self.home, ignore_errors=True)

    def x(self, *command):
        return subprocess.run(["xdotool", *map(str, command)], env=self.env, capture_output=True,
                              text=True).stdout

    def capture(self, wid, box=None):
        command = ["import", "-window", str(wid)]
        if box:
            command += ["-crop", "%dx%d+%d+%d" % (box[2], box[3], box[0], box[1])]
        out = subprocess.run(command + ["-depth", "8", "ppm:-"], env=self.env, capture_output=True).stdout
        return pixels(out) if out else b""

    def save(self, wid, name):
        if not self.args.screenshots:
            return
        width, height = (int(v) for v in self.args.size.split("x"))
        self.x("mousemove", width - 1, height - 1)
        time.sleep(0.5)
        subprocess.run(["import", "-window", str(wid), str(self.args.screenshots / (name + ".png"))], env=self.env)

    def click(self, wid, box):
        x, y, w, h = box
        self.x("mousemove", "--window", wid, x + w // 2, y + h // 2)
        time.sleep(0.2)
        self.x("click", "1")
        time.sleep(0.6)


def check_icons(commands, scale, colour_theme, what):
    problems = []
    if len(commands) < 30:
        return ["%s: only %d commands reported" % (what, len(commands))]
    large, small = {100: (32, 16), 125: (48, 22), 150: (48, 24), 175: (64, 32), 200: (64, 32)}.get(scale, (0, 0))
    for label, icon, kind, tint, size in commands:
        if not icon or icon == "(null)":
            problems.append("%s: no icon for %s" % (what, label))
        elif kind not in ("colour", "glyph") or (kind == "glyph" and tint in ("", "-")):
            problems.append("%s: %s has a plain icon (%s, %s, %s)" % (what, label, icon, kind, tint))
        if large and size not in (large, small):
            problems.append("%s: %s at %d pixels" % (what, label, size))
    colour = [c[0] for c in commands if c[2] == "colour"]
    if colour_theme and len(colour) < 4:
        problems.append("%s: only %d full-colour icons (%s)" % (what, len(colour), ", ".join(colour)))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path("/usr"))
    parser.add_argument("--display", default=":189")
    parser.add_argument("--theme", default="SDE-Graphite-Light-Blue")
    parser.add_argument("--themes", type=Path, default=ROOT / "build/themes",
                        help="folder with the built SDE GTK themes (tools/build-themes.py)")
    parser.add_argument("--size", default="1366x768")
    parser.add_argument("--scale", type=int, default=100)
    parser.add_argument("--screenshots", type=Path, help="save the window into this folder")
    parser.add_argument("--keep", action="store_true", help="keep the temporary home folder")
    args = parser.parse_args()
    for tool in ("Xvfb", "xdotool", "import", "dbus-launch", "xsettingsd"):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    if not (args.prefix / "bin/liria").is_file():
        print("skipped: %s not found" % (args.prefix / "bin/liria"))
        return 77
    if args.screenshots:
        args.screenshots.mkdir(parents=True, exist_ok=True)
    papirus = Path("/usr/share/icons/Papirus").is_dir()
    first, second = ("Papirus", "Adwaita") if papirus else ("Adwaita", "hicolor")
    problems = []
    s = Session(args)
    try:
        s.start(first)
        qa = s.home / "ribbon.txt"
        env = dict(s.env, LIRIA_SDE_QA_RIBBON=str(qa))
        log = open(s.home / "liria.log", "w")
        s.liria = subprocess.Popen([str(args.prefix / "bin/liria"), "--no-desktop", str(s.home / "Downloads")],
                                   env=env, cwd=str(s.home), stdout=log, stderr=subprocess.STDOUT)
        wid = wait_for(lambda: (s.x("search", "--onlyvisible", "--name", "^Downloads$").split() or [None])[-1], 30)
        if not wid:
            raise AssertionError("Liria did not open the Downloads folder")
        s.x("windowsize", wid, 1100, 660)
        s.x("windowactivate", "--sync", wid)
        if not wait_for(lambda: "home" in read_qa(qa)[1], 10):
            raise AssertionError("the ribbon reports no tabs")
        time.sleep(1.0)
        commands, tabs = read_qa(qa)
        # 1, 2: the icons and their sizes; colour on the screen.
        problems += check_icons(commands, args.scale, papirus, first)
        home = tabs["home"]
        ribbon = (0, home[1] + home[3], 700, 3 * home[3])
        if coloured(s.capture(wid, ribbon)) < 400:
            problems.append("the ribbon is not in colour on the screen")
        s.save(wid, "liria-ribbon-home")
        # 3. The tabs.
        for page in ("share", "view"):
            s.click(wid, read_qa(qa)[1][page])
            s.save(wid, "liria-ribbon-" + page)
        s.click(wid, read_qa(qa)[1]["home"])
        # The archive selected with the keyboard (type-ahead).
        s.x("mousemove", "--window", wid, 900, 600)
        s.x("click", "1")
        time.sleep(0.4)
        s.x("type", "--delay", "60", "Autumn")
        time.sleep(0.8)
        if not wait_for(lambda: "archive" in read_qa(qa)[1], 6):
            problems.append("no Archive tab for a selected archive")
        else:
            s.x("key", "Escape")
            time.sleep(0.3)
            s.click(wid, read_qa(qa)[1]["archive"])
            s.save(wid, "liria-ribbon-archive")
        # 4. Another icon theme while Liria runs.
        before = read_qa(qa)[0]
        s.icon_theme(second)
        s.xsettingsd.send_signal(signal.SIGHUP)
        if not wait_for(lambda: read_qa(qa)[0] and [c[1:3] for c in read_qa(qa)[0]] != [c[1:3] for c in before],
                        10):
            problems.append("the icons did not follow the icon theme %s" % second)
        else:
            time.sleep(0.8)
            problems += check_icons(read_qa(qa)[0], args.scale, False, second)
            s.click(wid, read_qa(qa)[1]["home"])
            s.save(wid, "liria-ribbon-" + second.lower())
    except (AssertionError, KeyError) as error:
        problems.append("%s: %s" % (type(error).__name__, error))
    finally:
        s.stop()
    for problem in problems:
        print("FAIL " + problem)
    print("Liria ribbon icons: %s" % ("passed" if not problems else "%d problem(s)" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
