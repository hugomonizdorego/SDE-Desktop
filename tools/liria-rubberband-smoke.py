#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rubber band smoke test of a built Liria on Xvfb (xdotool, ImageMagick).

  tools/liria-rubberband-smoke.py --prefix /usr              # one run
  tools/liria-rubberband-smoke.py --prefix stage/usr --all   # every theme

Starts Xvfb, a window manager when one is installed (picom too with
--compositor), xsettingsd for the interface scale when it is installed and
Liria in the icon view of a throw-away folder. Like a user selecting files
with the mouse, it presses the left button on the empty part of the view,
drags the rubber band over every file and takes a screenshot before the
release. Then it checks the pixels (0.2.0: the band was an opaque white or
dark sheet that hid the files being selected):

  1. the files stay visible under the band: the luminance spread of their
     icons and labels is at least half of the spread without the band;
  2. the band is a translucent accent sheet: over empty space its fill
     moves the view colour 10 to 35 % of the way to the colour of its 1 px
     border, which is not the view colour;
  3. after the release the selected files are highlighted (at least 10 %
     of the pixels of their icons and labels change);
  4. in the list view Ctrl+A highlights the rows (the selection was
     invisible in some themes);
  5. Liria prints no critical warnings and no CSS parsing errors.

--all runs every SDE theme family (Graphite, Fluent, Glass), light and
dark, at 100 % (1366x768) and 150 % (1024x600), without and with a
compositor; --screenshots DIR keeps the screenshots. Exit status 0 =
passed, 1 = failed, 77 = skipped (no Xvfb, xdotool, ImageMagick or Liria).
"""
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FAMILIES = ("Graphite", "Fluent", "Glass")
SIZES = {100: "1366x768", 150: "1024x600"}


def wait_for(predicate, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.2)
    return predicate()


class Shot:
    """A screenshot (binary PPM from ImageMagick's import, 8 or 16 bit)."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        fields, pos = [], 0
        while len(fields) < 4:
            while data[pos:pos + 1].isspace():
                pos += 1
            if data[pos:pos + 1] == b"#":
                pos = data.index(b"\n", pos) + 1
                continue
            end = pos
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(data[pos:end])
            pos = end
        if fields[0] != b"P6" or int(fields[3]) not in (255, 65535):
            raise ValueError("not a PPM screenshot: %s" % path)
        self.width, self.height = int(fields[1]), int(fields[2])
        self.pixels = data[pos + 1:]
        if int(fields[3]) == 65535:
            self.pixels = self.pixels[::2]  # the high bytes

    def at(self, x, y):
        i = (y * self.width + x) * 3
        return tuple(self.pixels[i:i + 3])


def luminance(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def distance(a, b):
    return sum((p - q) ** 2 for p, q in zip(a, b)) ** 0.5


def rect_points(x0, y0, x1, y1, step=1):
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            yield x, y


def find_view(shot, x, y):
    """The view area around the empty point (x, y) below the files: scans
    left and right along its row while the pixels keep the view colour (the
    sidebar and the window edge or scroll bar end it), down to the status
    bar and up along three columns (a file can stop one of them, the header
    ends the others). Returns the view colour and the area."""
    bg = shot.at(x, y)
    left = right = x
    while left > 0 and shot.at(left - 1, y) == bg:
        left -= 1
    while right < shot.width - 1 and shot.at(right + 1, y) == bg:
        right += 1
    bottom = y
    while bottom < shot.height - 1 and shot.at(x, bottom + 1) == bg:
        bottom += 1
    tops = []
    for column in (left + 1, (left + right) // 2, right - 1):
        top = y
        while top > 0 and shot.at(column, top - 1) == bg:
            top -= 1
        tops.append(top)
    return bg, left, min(tops), right, bottom


def border_colour(band, before, edge_points):
    """The colour of the band's 1 px border: of the pixels along an edge
    (and one pixel on either side), the ones that changed most."""
    best = []
    for points in edge_points:
        changed = [band.at(x, y) for x, y in points]
        delta = statistics.mean(distance(band.at(x, y), before.at(x, y)) for x, y in points)
        best.append((delta, changed))
    best.sort(key=lambda item: -item[0])
    pixels = best[0][1]
    return tuple(int(statistics.median(p[i] for p in pixels)) for i in range(3)), best[0][0]


def analyse(before, band, after, bg, x0, y0, x1, y1):
    """Checks 1-3 for a band from (x0, y0) to (x1, y1) (x0 < x1, y0 < y1)."""
    problems = []
    inner = list(rect_points(x0 + 4, y0 + 4, x1 - 3, y1 - 3, 2))
    # The files: the area of the icons and labels inside the band.
    files = [(x, y) for x, y in inner if before.at(x, y) != bg]
    if len(files) < 200:
        return ["no files under the band to check"], {"spread": (0, 0)}
    area = list(rect_points(min(x for x, _ in files), min(y for _, y in files),
                            max(x for x, _ in files) + 1, max(y for _, y in files) + 1, 2))
    spread0 = statistics.pstdev([luminance(before.at(x, y)) for x, y in area])
    spread1 = statistics.pstdev([luminance(band.at(x, y)) for x, y in area])
    if spread1 < 0.5 * spread0:
        problems.append("files hidden under the band: luminance spread %.1f with the band, %.1f without"
                        % (spread1, spread0))
    # The border: the band's right and bottom edges are near the pointer
    # (x1, y1); try the columns and rows next to it.
    ys = range(y0 + 8, y1 - 8, 3)
    xs = range(x0 + 8, x1 - 8, 3)
    edges = [[(x, y) for y in ys] for x in range(x1 - 2, x1 + 3)]
    edges += [[(x, y) for x in xs] for y in range(y1 - 2, y1 + 3)]
    edges += [[(x, y) for y in ys] for x in range(x0 - 2, x0 + 3)]
    accent, moved = border_colour(band, before, edges)
    span = [a - b for a, b in zip(accent, bg)]
    norm = sum(v * v for v in span)
    if moved < 25 or norm < 30 ** 2:
        problems.append("no rubber band border found (border %s, view %s)" % (accent, bg))
        return problems, {"spread": (spread0, spread1)}
    blank = [(x, y) for x, y in inner if before.at(x, y) == bg]
    alphas = [sum((p - b) * s for p, b, s in zip(band.at(x, y), bg, span)) / norm for x, y in blank]
    alpha = statistics.median(alphas) if alphas else 0.0
    if not 0.10 <= alpha <= 0.35:
        problems.append("rubber band fill is not a translucent accent: %.0f %% of the border colour %s "
                        "over the view colour %s" % (alpha * 100, accent, bg))
    changed = sum(1 for x, y in area if distance(before.at(x, y), after.at(x, y)) > 20)
    if changed < 0.10 * len(area):
        problems.append("selected files are not highlighted after the drag (%d of %d pixels changed)"
                        % (changed, len(area)))
    return problems, {"spread": (spread0, spread1), "alpha": alpha, "accent": accent, "view": bg,
                      "highlighted": changed / len(area)}


def make_home(home, dark):
    for name in ("Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos"):
        (home / name).mkdir(parents=True, exist_ok=True)
    work = home / "Work"
    for name in ("Projects", "Reports"):
        (work / name).mkdir(parents=True, exist_ok=True)
    for name, text in (("notes.txt", "notes\n"), ("table.csv", "a,b\n1,2\n"), ("page.html", "<html></html>\n"),
                       ("run.sh", "#!/bin/sh\necho run\n")):
        (work / name).write_text(text)
    (home / ".config/sde").mkdir(parents=True, exist_ok=True)
    (home / ".config/sde/liria.conf").write_text("[ribbon]\nmode=off\n")
    (home / ".config/user-dirs.dirs").write_text("".join(
        'XDG_%s_DIR="$HOME/%s"\n' % (key, name) for key, name in
        (("DESKTOP", "Desktop"), ("DOCUMENTS", "Documents"), ("DOWNLOAD", "Downloads"), ("MUSIC", "Music"),
         ("PICTURES", "Pictures"), ("VIDEOS", "Videos"))))
    return work


def icon_theme(dark):
    for name in (("Papirus-Dark", "Papirus") if dark else ("Papirus",)):
        if Path("/usr/share/icons", name).is_dir():
            return name
    return "Adwaita"


def run_one(args, theme, scale, compositor, shots):
    """One configuration; returns a list of problems (empty = passed) or
    None when it was skipped."""
    dark = "-Dark-" in theme
    size = SIZES[scale]
    width, height = (int(v) for v in size.split("x"))
    dpi = 96 * scale // 100
    name = "%s-%d-%s" % (theme, scale, "comp" if compositor else "nocomp")
    if compositor and not shutil.which("picom"):
        print("skip " + name + ": picom is not installed")
        return None
    home = Path(tempfile.mkdtemp(prefix="liria-band-"))
    work = make_home(home, dark)
    (home / ".themes").symlink_to(args.themes.resolve())
    env = dict(os.environ, HOME=str(home), DISPLAY=args.display, XDG_CONFIG_HOME=str(home / ".config"),
               XDG_DATA_HOME=str(home / ".local/share"), GTK_THEME=theme, NO_AT_BRIDGE="1",
               GSETTINGS_SCHEMA_DIR=str(args.prefix / "share/glib-2.0/schemas"),
               XDG_DATA_DIRS=str(args.prefix / "share") + ":/usr/local/share:/usr/share")
    env.pop("SDE_SESSION_VERSION", None)
    (home / ".config/gtk-3.0").mkdir(parents=True)
    (home / ".config/gtk-3.0/settings.ini").write_text(
        "[Settings]\ngtk-theme-name=%s\ngtk-icon-theme-name=%s\ngtk-font-name=Sans 10\n" % (theme, icon_theme(dark)))
    procs = []
    problems = []

    def x(*command):
        return subprocess.run(["xdotool", *map(str, command)], env=env, capture_output=True, text=True).stdout

    def screenshot(path):
        subprocess.run(["import", "-window", "root", "-depth", "8", "ppm:" + str(path)], env=env, check=True)
        return Shot(path)

    try:
        procs.append(subprocess.Popen(["Xvfb", args.display, "-screen", "0", size + "x24", "-nolisten", "tcp",
                                       "-dpi", str(dpi)], env=env, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL))
        socket = Path("/tmp/.X11-unix/X" + args.display.lstrip(":").split(".")[0])
        if not wait_for(socket.exists, 10):
            return ["Xvfb did not start on " + args.display]
        time.sleep(0.5)
        bus = subprocess.run(["dbus-launch", "--sh-syntax"], env=env, capture_output=True, text=True).stdout
        for line in bus.splitlines():
            if line.startswith("DBUS_SESSION_BUS_ADDRESS="):
                env["DBUS_SESSION_BUS_ADDRESS"] = line.split("=", 1)[1].rstrip(";").strip("'")
            if line.startswith("DBUS_SESSION_BUS_PID="):
                procs.append(int(line.split("=", 1)[1].rstrip(";")))
        if shutil.which("xsettingsd"):
            conf = home / "xsettingsd.conf"
            conf.write_text('Net/ThemeName "%s"\nNet/IconThemeName "%s"\nGtk/FontName "Sans 10"\n'
                            "Xft/DPI %d\nXft/Antialias 1\n" % (theme, icon_theme(dark), dpi * 1024))
            procs.append(subprocess.Popen(["xsettingsd", "-c", str(conf)], env=env, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL))
        elif scale != 100:
            env["GDK_DPI_SCALE"] = str(scale / 100)
        wm = shutil.which("openbox") or shutil.which("sde-wm")
        if wm:
            procs.append(subprocess.Popen([wm], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if compositor:
            procs.append(subprocess.Popen(["picom", "--backend", "xrender", "--no-fading-openclose"], env=env,
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.8)
        log = open(home / "liria.log", "w")
        procs.append(subprocess.Popen([str(args.prefix / "bin/liria"), "--no-desktop", str(work)], env=env,
                                      cwd=str(home), stdout=log, stderr=subprocess.STDOUT))
        if not wait_for(lambda: x("search", "--onlyvisible", "--name", "^Work$").strip(), 20):
            return ["Liria did not open its window"]
        wid = x("search", "--onlyvisible", "--name", "^Work$").split()[0]
        x("windowsize", wid, width - 60, height - 70)
        x("windowmove", wid, 20, 10)
        time.sleep(1.0)
        x("windowactivate", wid)
        time.sleep(0.5)
        x("key", "ctrl+1")
        time.sleep(1.5)
        # The window fills most of the screen; its six files take the first
        # row(s) of the view, so the point at 3/4 of the width and 2/3 of
        # the height is empty view space.
        x("mousemove", width - 5, height - 5)
        time.sleep(0.5)
        before = screenshot(home / "before.ppm")
        bg, left, top, right, bottom = find_view(before, width * 3 // 4, height * 2 // 3)
        if right - left < 300 or bottom - top < 150:
            return ["view area not found (%d,%d to %d,%d)" % (left, top, right, bottom)]
        # The band goes from the empty bottom right corner of the view over
        # every file to its top left corner.
        sx, sy = right - 12, bottom - 12
        ex, ey = left + 6, top + 6
        x("mousemove", sx, sy)
        time.sleep(0.5)
        before = screenshot(home / "before.ppm")
        x("mousedown", 1)
        time.sleep(0.2)
        for i in range(1, 21):
            x("mousemove", sx + (ex - sx) * i // 20, sy + (ey - sy) * i // 20)
            time.sleep(0.04)
        time.sleep(0.8)
        band = screenshot(home / "band.ppm")
        x("mouseup", 1)
        time.sleep(0.8)
        x("mousemove", sx, sy)
        time.sleep(0.6)
        after = screenshot(home / "after.ppm")
        found, values = analyse(before, band, after, bg, ex, ey, sx, sy)
        problems += found
        # 4. The list view: Ctrl+A highlights the rows. A click on the empty
        # part below them (above a horizontal scroll bar) first clears the
        # selection kept from the icons.
        x("key", "ctrl+2")
        time.sleep(1.5)
        x("mousemove", (left + right) // 2, top + (bottom - top) * 3 // 4)
        x("click", 1)
        time.sleep(0.6)
        x("mousemove", width - 5, height - 5)
        time.sleep(0.4)
        plain = screenshot(home / "list.ppm")
        x("key", "ctrl+a")
        time.sleep(0.8)
        listed = screenshot(home / "list-selected.ppm")
        rows = list(rect_points(left + 4, top + 40, sx - 20, min(top + 40 + 140, sy - 4), 2))
        changed = sum(1 for px, py in rows if distance(plain.at(px, py), listed.at(px, py)) > 12)
        values["rows"] = changed / max(len(rows), 1)
        if changed < 0.25 * len(rows):
            problems.append("list view: Ctrl+A does not highlight the rows (%d of %d pixels changed)"
                            % (changed, len(rows)))
        if shots is not None:
            shots.mkdir(parents=True, exist_ok=True)
            for part in ("before", "band", "after", "list-selected"):
                subprocess.run(["convert", str(home / (part + ".ppm")), str(shots / ("%s-%s.png" % (name, part)))],
                               env=env)
        log.close()
        warnings = [line for line in (home / "liria.log").read_text(errors="replace").splitlines()
                    if "CRITICAL" in line or "parsing error" in line]
        if warnings:
            problems.append("Liria printed %d warning(s), e.g. %s" % (len(warnings), warnings[0].strip()))
        print("     %s: band %s, view %s, fill %.0f %%, spread %.1f -> %.1f, highlighted %.1f %%, rows %.0f %%"
              % (name, values.get("accent"), values.get("view"), 100 * values.get("alpha", 0),
                 values["spread"][0], values["spread"][1], 100 * values.get("highlighted", 0),
                 100 * values["rows"]))
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
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path("/usr"))
    parser.add_argument("--display", default=":189")
    parser.add_argument("--themes", type=Path, default=ROOT / "build/themes",
                        help="folder with the built SDE GTK themes (tools/build-themes.py)")
    parser.add_argument("--theme", default="SDE-Graphite-Light-Blue")
    parser.add_argument("--scale", type=int, choices=sorted(SIZES), default=100)
    parser.add_argument("--compositor", action="store_true", help="run picom")
    parser.add_argument("--all", action="store_true",
                        help="every family, light and dark, 100 and 150 %%, without and with a compositor")
    parser.add_argument("--screenshots", type=Path, help="save the screenshots (PNG) here")
    parser.add_argument("--keep", action="store_true", help="keep the temporary home folders")
    args = parser.parse_args()
    for tool in ("Xvfb", "xdotool", "import", "dbus-launch"):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    if not (args.prefix / "bin/liria").is_file():
        print("skipped: %s not found" % (args.prefix / "bin/liria"))
        return 77
    if not (args.themes / args.theme).is_dir():
        print("skipped: the SDE themes are not built in %s (tools/build-themes.py)" % args.themes)
        return 77
    if args.all:
        runs = [("SDE-%s-%s-Blue" % (family, mode), scale, compositor) for family in FAMILIES
                for mode in ("Light", "Dark") for scale in sorted(SIZES) for compositor in (False, True)]
    else:
        runs = [(args.theme, args.scale, args.compositor)]
    failures = 0
    for theme, scale, compositor in runs:
        problems = run_one(args, theme, scale, compositor, args.screenshots)
        if problems is None:
            continue
        label = "%s at %d %%%s" % (theme, scale, " with a compositor" if compositor else "")
        print(("ok   " if not problems else "FAIL ") + label, flush=True)
        for problem in problems:
            print("     " + problem)
        failures += bool(problems)
    print("Liria rubber band: %s" % ("passed" if not failures else "%d failure(s)" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
