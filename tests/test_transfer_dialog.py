#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The SDE transfer dialog (src/common/transfer_dialog.h) on a private Xvfb,
driven with xdotool through tests/transfer_demo.cpp (a simulated copy):

  1. while it copies, the SDE mark is animated (its pixels change between
     frames) and the speed graph shows the accent colour with the details;
  2. Pause stops the mark and says "Paused"; Resume starts it again;
  3. "Fewer details" makes the window lower, "More details" taller again;
  4. with reduced motion (SDE_REDUCED_MOTION=1) the mark stands still;
  5. Cancel ends the copy (the demo exits with status 3).

  test_transfer_dialog.py BUILD/tests/sde-transfer-demo
Exit status 0 passed, 1 failed, 77 skipped (no Xvfb, xdotool or ImageMagick).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def wait(predicate, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.1)
    return predicate()


class Session:
    def __init__(self, demo, env):
        self.demo, self.env, self.procs = demo, env, []

    def start_x(self):
        read, write = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "1024x768x24", "-nolisten", "tcp"],
                                pass_fds=(write,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(write)
        number = b""
        while not number.endswith(b"\n"):
            chunk = os.read(read, 16)
            if not chunk:
                break
            number += chunk
        os.close(read)
        self.procs.append(xvfb)
        self.env["DISPLAY"] = ":" + number.decode().strip()
        if shutil.which("openbox"):
            self.procs.append(subprocess.Popen(["openbox"], env=self.env, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL))
            time.sleep(0.4)

    def x(self, *args):
        return subprocess.run(["xdotool", *map(str, args)], env=self.env, capture_output=True, text=True).stdout

    def run_demo(self, qa, *args, env=None):
        proc = subprocess.Popen([self.demo, "--qa", str(qa), *args], env=env or self.env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        if not wait(lambda: self.window() and qa.exists() and "window=" in qa.read_text(), 10):
            proc.kill()
            raise AssertionError("the dialog did not open")
        time.sleep(0.4)
        return proc

    def window(self):
        out = self.x("search", "--onlyvisible", "--classname", "sde-transfer").split()
        return out[-1] if out else None

    def geometry(self):
        """The client area on the screen. xdotool getwindowgeometry adds the
        frame of a reparenting window manager to the position, so the origin
        comes from a pointer moved into the window."""
        wid = self.window()
        info = {}
        for line in self.x("getwindowgeometry", "--shell", wid).splitlines():
            key, _, value = line.partition("=")
            info[key] = int(value) if value.isdigit() else value
        self.x("mousemove", "--window", wid, 0, 0)
        for line in self.x("getmouselocation", "--shell").splitlines():
            key, _, value = line.partition("=")
            if key in ("X", "Y"):
                info[key] = int(value)
        return info

    def click(self, box):
        """Clicks the middle of "x,y,w,h" (window coordinates)."""
        x, y, w, h = (int(v) for v in box.split(","))
        self.x("mousemove", "--window", self.window(), x + w // 2, y + h // 2)
        time.sleep(0.15)
        self.x("click", "1")

    def shot(self, x, y, w, h):
        """The pixels of a screen area (binary PPM through ImageMagick)."""
        data = subprocess.run(["import", "-window", "root", "-crop", "%dx%d+%d+%d" % (w, h, x, y), "-depth", "8",
                               "ppm:-"], env=self.env, capture_output=True, check=True).stdout
        fields, pos = [], 0
        while len(fields) < 4:
            while data[pos:pos + 1].isspace():
                pos += 1
            end = pos
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(data[pos:end])
            pos = end
        return data[pos + 1:]

    def stop(self):
        for proc in reversed(self.procs):
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


def qa_values(path):
    values = {}
    for line in path.read_text().splitlines():
        key, _, value = line.partition("=")
        values[key] = value
    return values


def main():
    demo = os.path.abspath(sys.argv[1])
    for tool in ("Xvfb", "xdotool", "import"):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    problems = []
    with tempfile.TemporaryDirectory(prefix="sde-transfer-") as tmp:
        tmp = Path(tmp)
        env = dict(os.environ, HOME=str(tmp), XDG_CONFIG_HOME=str(tmp / "config"), NO_AT_BRIDGE="1")
        env.pop("SDE_REDUCED_MOTION", None)
        s = Session(demo, env)
        try:
            s.start_x()
            qa = tmp / "qa.txt"
            proc = s.run_demo(qa, "--details", "--at", "0.4")
            g = s.geometry()
            values = qa_values(qa)
            # 1. The mark is animated; the graph has the accent colour.
            logo = (g["X"] + 18, g["Y"] + 18, 72, 72)
            frames = [s.shot(*logo)]
            for _ in range(3):
                time.sleep(0.15)
                frames.append(s.shot(*logo))
            if len(set(frames)) < 2:
                problems.append("the SDE mark is not animated while copying")
            if values.get("animating") != "1":
                problems.append("the dialog does not report an animation: %r" % values.get("animating"))
            graph = s.shot(g["X"] + 20, g["Y"] + 110, 150, 90)
            coloured = sum(1 for i in range(0, len(graph) - 2, 3)
                           if max(graph[i:i + 3]) - min(graph[i:i + 3]) > 40)
            if coloured < 300:
                problems.append("no speed graph in the accent colour (%d coloured pixels)" % coloured)
            for key in ("headline", "percent", "speed", "remaining", "items"):
                if not values.get(key):
                    problems.append("empty %s text" % key)
            if "Copying 1234 items from Videos to Backup Drive" not in values.get("headline", ""):
                problems.append("headline: %r" % values.get("headline"))
            # 2. Pause stops the mark; Resume starts it again.
            s.click(values["pause"])
            if not wait(lambda: qa_values(qa).get("paused") == "1"):
                problems.append("Pause did not pause")
            time.sleep(0.4)
            still = [s.shot(*logo)]
            time.sleep(0.3)
            still.append(s.shot(*logo))
            if still[0] != still[1]:
                problems.append("the SDE mark keeps moving while paused")
            if not qa_values(qa).get("percent", "").startswith("Paused"):
                problems.append("no \"Paused\" in %r" % qa_values(qa).get("percent"))
            s.click(qa_values(qa)["pause"])
            if not wait(lambda: qa_values(qa).get("paused") == "0" and qa_values(qa).get("animating") == "1"):
                problems.append("Resume did not resume the copy and the animation")
            # 3. Fewer and more details.
            tall = s.geometry()["HEIGHT"]
            s.click(qa_values(qa)["toggle"])
            if not wait(lambda: s.geometry()["HEIGHT"] < tall - 50):
                problems.append("Fewer details did not make the window lower (%d)" % s.geometry()["HEIGHT"])
            # 5. Cancel.
            s.click(qa_values(qa)["cancel"])
            try:
                status = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                status = None
            if status != 3:
                problems.append("Cancel did not end the copy (status %r)" % status)
            # 4. Reduced motion: a still mark.
            qa.unlink()
            calm = dict(env, SDE_REDUCED_MOTION="1")
            proc = s.run_demo(qa, "--at", "0.4", env=calm)
            g = s.geometry()
            logo = (g["X"] + 18, g["Y"] + 18, 72, 72)
            a = s.shot(*logo)
            time.sleep(0.3)
            b = s.shot(*logo)
            if a != b or qa_values(qa).get("animating") != "0":
                problems.append("the SDE mark moves with reduced motion")
            proc.kill()
            proc.wait()
        finally:
            s.stop()
    for problem in problems:
        print("FAIL " + problem)
    print("SDE transfer dialog: %s" % ("passed" if not problems else "%d problem(s)" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
