#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy progress smoke test of a built Liria on Xvfb (xdotool).

  tools/liria-progress-smoke.py --prefix /usr                   # an installed Liria
  tools/liria-progress-smoke.py --prefix stage/usr --screenshots out

Copies a folder of test files (three large files and many small ones) into
another with Ctrl+C and Ctrl+V, slowed down to a few MB/s with
LIRIA_SDE_QA_COPY_RATE, and checks Liria's file operations window
(libliria-private/liria-sde-progress.c, the design of SDE's copy and move
dialog, 0.2.0 Alpha 3) through what it reports in LIRIA_SDE_QA_PROGRESS
and through the window's pixels:

  1. the headline names the items and the destination, the line under the
     bar says how much of how much, how fast and how long, the percentage
     grows and the picture moves (the penguin carries papers);
  2. Details (from, to, items) makes the window taller;
  3. Pause stops the copy in the middle of a large file and the penguin
     and says "Paused"; Resume goes on;
  4. Cancel ends the copy before every file is there;
  5. with reduced motion (SDE_REDUCED_MOTION=1) the picture stands still,
     and a copy that runs to the end has every file, byte for byte (large
     files are copied in slices on internal disks, liria-sde-copy.c);
  6. a copy through the safe writer (SDE_SAFE_COPY=always, as on a USB
     stick) says at the end that the files are on the drive.

--screenshots DIR saves the window with and without the details, paused
and finished.
Exit status 0 = passed, 1 = failed, 77 = skipped (no Xvfb, xdotool,
ImageMagick or Liria).
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# MiB; files of 32 MiB and more are copied in slices (liria-sde-copy.c).
LARGE = (("Holiday video.mov", 40), ("Disk image.iso", 34), ("Archive.tar", 36))
SMALL = 60
RATE = 4_000_000  # bytes per second
FAST = 16_000_000  # the complete copy


def wait_for(predicate, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.1)
    return predicate()


def qa_values(path):
    try:
        text = path.read_text()
    except OSError:
        return {}
    values = {}
    for line in text.splitlines():
        key, _, value = line.partition("=")
        values[key] = value
    return values


def rect(values, key):
    """x, y, w, h of a "x,y,w,h" entry, or None."""
    try:
        return tuple(int(v) for v in values[key].split(","))
    except (KeyError, ValueError):
        return None


def pixels(ppm):
    """The RGB bytes of a binary PPM (ImageMagick, depth 8)."""
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


class Session:
    def __init__(self, args):
        self.args = args
        self.procs = []
        self.liria = None
        self.home = Path(tempfile.mkdtemp(prefix="liria-progress-"))

    def start(self):
        args = self.args
        dpi = 96 * args.scale // 100
        dark = "-Dark" in args.theme
        icons = ("Papirus-Dark" if dark else "Papirus") if Path("/usr/share/icons/Papirus").is_dir() else "Adwaita"
        home = self.home
        for name in ("Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", "Source", "Backup",
                     "Backup 2", "Backup 3", ".config/sde", ".config/gtk-3.0"):
            (home / name).mkdir(parents=True, exist_ok=True)
        (home / ".config/user-dirs.dirs").write_text("".join(
            'XDG_%s_DIR="$HOME/%s"\n' % (key, name) for key, name in
            (("DESKTOP", "Desktop"), ("DOCUMENTS", "Documents"), ("DOWNLOAD", "Downloads"), ("MUSIC", "Music"),
             ("PICTURES", "Pictures"), ("VIDEOS", "Videos"))))
        (home / ".config/sde/liria.conf").write_text("[ribbon]\nmode=off\n")
        (home / ".config/gtk-3.0/settings.ini").write_text(
            "[Settings]\ngtk-theme-name=%s\ngtk-icon-theme-name=%s\ngtk-font-name=Sans 10\n" % (args.theme, icons))
        for name, mib in LARGE:
            # A pattern whose period is no power of two: a slice in the
            # wrong place changes the copy.
            block = name.encode() + bytes(range(251))
            data = block * ((mib << 20) // len(block) + 1)
            (home / "Source" / name).write_bytes(data[:mib << 20])
        for i in range(SMALL):
            (home / "Source" / ("photo-%02d.jpg" % (i + 1))).write_bytes(bytes([i + 1]) * (48 * 1024))
        if args.themes and (args.themes / args.theme).is_dir():
            (home / ".themes").mkdir()
            (home / ".themes" / args.theme).symlink_to(args.themes.resolve() / args.theme)
        self.env = env = dict(
            os.environ, HOME=str(home), DISPLAY=args.display, XDG_CONFIG_HOME=str(home / ".config"),
            XDG_DATA_HOME=str(home / ".local/share"), XDG_CACHE_HOME=str(home / ".cache"),
            GTK_THEME=args.theme, NO_AT_BRIDGE="1", GSETTINGS_BACKEND="memory",
            GSETTINGS_SCHEMA_DIR=str(args.prefix / "share/glib-2.0/schemas"),
            XDG_DATA_DIRS=str(args.prefix / "share") + ":/usr/local/share:/usr/share",
            SDE_BRANDING_DIR=str(ROOT / "data/branding"))
        for key in ("SDE_SESSION_VERSION", "SDE_REDUCED_MOTION", "LIRIA_SDE_QA_PROGRESS"):
            env.pop(key, None)
        self.procs.append(subprocess.Popen(["Xvfb", args.display, "-screen", "0", args.size + "x24", "-nolisten",
                                            "tcp", "-dpi", str(dpi)], stdout=subprocess.DEVNULL,
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
        if shutil.which("xsettingsd"):
            conf = home / "xsettingsd.conf"
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

    def stop_liria(self):
        if self.liria is not None:
            self.liria.terminate()
            try:
                self.liria.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.liria.kill()
                self.liria.wait()
            self.liria = None

    def stop(self):
        self.stop_liria()
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
        else:
            print("kept %s" % self.home)

    def x(self, *command):
        return subprocess.run(["xdotool", *map(str, command)], env=self.env, capture_output=True,
                              text=True).stdout

    def find(self, name):
        out = self.x("search", "--onlyvisible", "--name", name).split()
        return out[-1] if out else None

    def capture(self, wid, box=None):
        """The pixels of the window (or of the box x, y, w, h in it)."""
        command = ["import", "-window", str(wid)]
        if box:
            command += ["-crop", "%dx%d+%d+%d" % (box[2], box[3], box[0], box[1])]
        out = subprocess.run(command + ["-depth", "8", "ppm:-"], env=self.env, capture_output=True).stdout
        return pixels(out) if out else b""

    def save(self, wid, path):
        """The window, without the pointer's hover and tooltips."""
        width, height = (int(v) for v in self.args.size.split("x"))
        self.x("mousemove", width - 1, height - 1)
        time.sleep(0.5)
        subprocess.run(["import", "-window", str(wid), str(path)], env=self.env)

    def click(self, wid, box):
        x, y, w, h = box
        self.x("mousemove", "--window", wid, x + w // 2, y + h // 2)
        time.sleep(0.2)
        self.x("click", "1")
        time.sleep(0.1)

    def copy(self, env, target, qa):
        """Starts Liria on Source, copies everything and pastes it into
        |target|; returns the progress window."""
        liria = str(self.args.prefix / "bin/liria")
        log = open(self.home / "liria.log", "a")
        self.liria = subprocess.Popen([liria, "--no-desktop", str(self.home / "Source")], env=env,
                                      cwd=str(self.home), stdout=log, stderr=subprocess.STDOUT)
        if not wait_for(lambda: self.find("^Source"), 30):
            raise AssertionError("Liria did not open the Source folder")
        source = self.find("^Source")
        self.x("windowactivate", "--sync", source)
        time.sleep(1.0)
        self.x("key", "ctrl+a")
        time.sleep(0.3)
        self.x("key", "ctrl+c")
        time.sleep(0.3)
        subprocess.Popen([liria, "--no-desktop", str(target)], env=env, cwd=str(self.home), stdout=log,
                         stderr=subprocess.STDOUT)
        if not wait_for(lambda: self.find("^" + target.name), 20):
            raise AssertionError("Liria did not open %s" % target.name)
        window = self.find("^" + target.name)
        self.x("windowactivate", "--sync", window)
        time.sleep(0.8)
        geometry = {}
        for line in self.x("getwindowgeometry", "--shell", window).splitlines():
            key, _, value = line.partition("=")
            geometry[key] = int(value) if value.isdigit() else 0
        # An empty part of the empty folder's view, for the keyboard focus.
        self.x("mousemove", "--window", window, geometry.get("WIDTH", 800) * 2 // 3,
               geometry.get("HEIGHT", 600) * 3 // 4)
        self.x("click", "1")
        time.sleep(0.3)
        self.x("key", "ctrl+v")
        if not wait_for(lambda: qa_values(qa).get("window") and rect(qa_values(qa), "pause"), 20):
            raise AssertionError("no file operations window (%r)" % qa_values(qa))
        time.sleep(0.6)
        return qa_values(qa)["window"]


def changes(session, wid, box, frames=5, gap=0.12):
    """How often the pixels of |box| changed between |frames| captures."""
    shots = []
    for _ in range(frames):
        shots.append(session.capture(wid, box))
        time.sleep(gap)
    return sum(1 for a, b in zip(shots, shots[1:]) if a != b)


def coloured(data, threshold=40):
    return sum(1 for i in range(0, len(data) - 2, 3) if max(data[i:i + 3]) - min(data[i:i + 3]) > threshold)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path("/usr"))
    parser.add_argument("--display", default=":188")
    parser.add_argument("--theme", default="SDE-Graphite-Light-Blue")
    parser.add_argument("--themes", type=Path, default=ROOT / "build/themes",
                        help="folder with the built SDE GTK themes (tools/build-themes.py)")
    parser.add_argument("--size", default="1366x768")
    parser.add_argument("--scale", type=int, default=100)
    parser.add_argument("--screenshots", type=Path, help="save the window into this folder")
    parser.add_argument("--keep", action="store_true", help="keep the temporary home folder")
    args = parser.parse_args()
    for tool in ("Xvfb", "xdotool", "import", "dbus-launch"):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    if not (args.prefix / "bin/liria").is_file():
        print("skipped: %s not found" % (args.prefix / "bin/liria"))
        return 77
    if args.screenshots:
        args.screenshots.mkdir(parents=True, exist_ok=True)
    problems = []
    s = Session(args)
    try:
        s.start()
        qa = s.home / "qa.txt"
        env = dict(s.env, LIRIA_SDE_QA_PROGRESS=str(qa), LIRIA_SDE_QA_COPY_RATE=str(RATE))
        wid = s.copy(env, s.home / "Backup", qa)
        values = qa_values(qa)
        # 1. Headline, the line under the bar, the percentage, the picture.
        items = len(LARGE) + SMALL
        if values.get("headline") != "Copying %d items to Backup" % items:
            problems.append("headline: %r" % values.get("headline"))
        first = values.get("bytes", "0/0").split("/")[0]
        scene = rect(values, "scene")
        if not scene or changes(s, wid, scene) < 2 or values.get("animating") != "1":
            problems.append("the picture does not move while copying")
        if not wait_for(lambda: float(qa_values(qa).get("bytes", "0/0").split("/")[0] or 0) > float(first or 0), 5):
            problems.append("the copy does not move on")
        if not wait_for(lambda: qa_values(qa).get("status", "").endswith("left"), 6):
            problems.append("status: %r" % qa_values(qa).get("status"))
        if " of " not in qa_values(qa).get("status", ""):
            problems.append("status without the sizes: %r" % qa_values(qa).get("status"))
        if not qa_values(qa).get("percent", "").endswith("%"):
            problems.append("percent: %r" % qa_values(qa).get("percent"))
        if not qa_values(qa).get("title", "").startswith(qa_values(qa).get("percent", "-")):
            problems.append("title: %r" % qa_values(qa).get("title"))
        if args.screenshots:
            s.save(wid, args.screenshots / "liria-progress.png")
        # 2. Details.
        low = rect(qa_values(qa), "toggle")
        s.click(wid, rect(qa_values(qa), "toggle"))
        if not wait_for(lambda: qa_values(qa).get("details") == "1", 5):
            problems.append("Details did not open the details")
        else:
            time.sleep(0.6)
            values = qa_values(qa)
            if args.screenshots:
                s.save(wid, args.screenshots / "liria-progress-details.png")
        # 3. Pause in the middle of a file, and Resume.
        s.click(wid, rect(qa_values(qa), "pause"))
        if not wait_for(lambda: qa_values(qa).get("paused") == "1", 5):
            problems.append("Pause did not pause")
        time.sleep(1.0)
        before = qa_values(qa).get("bytes")
        still = [s.capture(wid, scene)]
        time.sleep(1.2)
        still.append(s.capture(wid, scene))
        if qa_values(qa).get("bytes") != before:
            problems.append("the copy goes on while paused (%s, then %s)" % (before, qa_values(qa).get("bytes")))
        if still[0] != still[1] or qa_values(qa).get("animating") != "0":
            problems.append("the penguin keeps walking while paused")
        if not qa_values(qa).get("status", "").endswith("Paused"):
            problems.append("no \"Paused\" in %r" % qa_values(qa).get("status"))
        if args.screenshots:
            s.save(wid, args.screenshots / "liria-progress-paused.png")
        s.click(wid, rect(qa_values(qa), "pause"))
        if not wait_for(lambda: qa_values(qa).get("paused") == "0" and qa_values(qa).get("animating") == "1", 5):
            problems.append("Resume did not resume the copy and the picture")
        if not wait_for(lambda: qa_values(qa).get("bytes") != before, 5):
            problems.append("the copy does not go on after Resume")
        # 4. Cancel.
        s.click(wid, rect(qa_values(qa), "cancel"))
        if not wait_for(lambda: qa_values(qa).get("done") == "1", 15):
            problems.append("Cancel did not end the copy")
        total = sum(mib << 20 for _, mib in LARGE) + SMALL * 48 * 1024
        copied = sum(p.stat().st_size for p in (s.home / "Backup").iterdir())
        if copied >= total:
            problems.append("everything was copied in spite of Cancel")
        s.stop_liria()
        # 5. Reduced motion, and a copy to the end.
        qa.unlink(missing_ok=True)
        calm = dict(env, SDE_REDUCED_MOTION="1", LIRIA_SDE_QA_COPY_RATE=str(FAST))
        wid = s.copy(calm, s.home / "Backup 2", qa)
        scene = rect(qa_values(qa), "scene")
        # A still picture changes only when the progress fills the
        # destination folder a bit more: once at most in these frames.
        if changes(s, wid, scene) > 1 or qa_values(qa).get("animating") != "0" or qa_values(qa).get("reduced") != "1":
            problems.append("the picture moves with reduced motion")
        if not wait_for(lambda: qa_values(qa).get("done") == "1", 60):
            problems.append("the complete copy did not end")
        else:
            names = sorted(p.name for p in (s.home / "Source").iterdir())
            copied = sorted(p.name for p in (s.home / "Backup 2").iterdir())
            same = names == copied and all(filecmp.cmp(s.home / "Source" / n, s.home / "Backup 2" / n, shallow=False)
                                           for n in names)
            if not same:
                problems.append("the copy differs from the original")
        s.stop_liria()
        # 6. Through the safe writer: at the end, the files are on the drive.
        qa.unlink(missing_ok=True)
        safe = dict(env, SDE_SAFE_COPY="always", LIRIA_SDE_QA_COPY_RATE=str(FAST))
        wid = s.copy(safe, s.home / "Backup 3", qa)
        if not wait_for(lambda: qa_values(qa).get("finished") == "1", 60):
            problems.append("the safe copy did not end")
        else:
            values = qa_values(qa)
            if not values.get("status", "").startswith("The files are on ") or not values.get("drive"):
                problems.append("finished status: %r (drive %r)" % (values.get("status"), values.get("drive")))
            if values.get("headline") != "Copied %d items to Backup 3" % items:
                problems.append("finished headline: %r" % values.get("headline"))
            if args.screenshots and values.get("done") != "1":
                s.save(wid, args.screenshots / "liria-progress-done.png")
            if not wait_for(lambda: qa_values(qa).get("done") == "1", 15):
                problems.append("the finished operation did not go by itself")
            names = sorted(p.name for p in (s.home / "Source").iterdir())
            if sorted(p.name for p in (s.home / "Backup 3").iterdir()) != names or not all(
                    filecmp.cmp(s.home / "Source" / n, s.home / "Backup 3" / n, shallow=False) for n in names):
                problems.append("the safe copy differs from the original")
    except AssertionError as error:
        problems.append(str(error))
    finally:
        s.stop()
    for problem in problems:
        print("FAIL " + problem)
    print("Liria copy progress: %s" % ("passed" if not problems else "%d problem(s)" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
