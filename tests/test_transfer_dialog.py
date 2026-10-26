#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The SDE transfer dialog (src/common/transfer_dialog.h) on a private Xvfb,
driven with xdotool through tests/transfer_demo.cpp (a simulated copy):

  1. while it copies, the picture moves (the penguin walks: its pixels
     change between frames), the bar has the accent colour and the texts
     say what is copied where, how much of how much, how fast and how long;
  2. Pause stops the penguin and says "Paused"; Resume starts it again;
  3. Details makes the window taller, and lower again;
  4. with reduced motion (SDE_REDUCED_MOTION=1) the picture stands still;
  5. Cancel ends the copy (the demo exits with status 3);
  6. a second operation shows as a line of the queue;
  7. a copy to a removable drive ends with "The files are on SANDISK - you
     can remove it." and the window goes by itself; a failed one stays with
     its errors until its cross is clicked.

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
        env = dict(os.environ, HOME=str(tmp), XDG_CONFIG_HOME=str(tmp / "config"), NO_AT_BRIDGE="1", LC_ALL="C")
        env.pop("SDE_REDUCED_MOTION", None)
        s = Session(demo, env)
        try:
            s.start_x()
            qa = tmp / "qa.txt"
            proc = s.run_demo(qa, "--at", "0.4")
            g = s.geometry()
            values = qa_values(qa)
            # 1. The picture moves; the bar has the accent colour; the texts.
            sx, sy, sw, sh = (int(v) for v in values["scene"].split(","))
            scene = (g["X"] + sx, g["Y"] + sy, sw, sh)
            frames = [s.shot(*scene)]
            for _ in range(3):
                time.sleep(0.15)
                frames.append(s.shot(*scene))
            if len(set(frames)) < 2:
                problems.append("the picture does not move while copying")
            if values.get("animating") != "1":
                problems.append("the dialog does not report an animation: %r" % values.get("animating"))
            if values.get("headline") != "Copying 1234 items to Backup Drive":
                problems.append("headline: %r" % values.get("headline"))
            status = values.get("status", "")
            if " of 4.2" not in status or "/s" not in status or not status.endswith("left"):
                problems.append("status: %r" % status)
            if not values.get("note"):
                problems.append("no item name")
            # The bar: 8 px high, 44 px under the picture (transfer_dialog.cpp).
            bar = s.shot(g["X"] + 24, g["Y"] + sy + sh + 46, 120, 4)
            coloured = sum(1 for i in range(0, len(bar) - 2, 3) if max(bar[i:i + 3]) - min(bar[i:i + 3]) > 40)
            if coloured < 150:
                problems.append("no bar in the accent colour (%d coloured pixels)" % coloured)
            # 2. Pause stops the penguin; Resume starts it again.
            s.click(values["pause"])
            if not wait(lambda: qa_values(qa).get("paused") == "1"):
                problems.append("Pause did not pause")
            time.sleep(0.4)
            still = [s.shot(*scene)]
            time.sleep(0.3)
            still.append(s.shot(*scene))
            if still[0] != still[1]:
                problems.append("the penguin keeps walking while paused")
            if not qa_values(qa).get("status", "").endswith("Paused"):
                problems.append("no \"Paused\" in %r" % qa_values(qa).get("status"))
            s.click(qa_values(qa)["pause"])
            if not wait(lambda: qa_values(qa).get("paused") == "0" and qa_values(qa).get("animating") == "1"):
                problems.append("Resume did not resume the copy and the picture")
            # 3. Details, and back.
            low = s.geometry()["HEIGHT"]
            s.click(qa_values(qa)["toggle"])
            if not wait(lambda: s.geometry()["HEIGHT"] > low + 40 and qa_values(qa).get("details") == "1"):
                problems.append("Details did not make the window taller (%d)" % s.geometry()["HEIGHT"])
            s.click(qa_values(qa)["toggle"])
            if not wait(lambda: s.geometry()["HEIGHT"] == low):
                problems.append("hiding the details did not make the window lower again")
            # 5. Cancel.
            s.click(qa_values(qa)["cancel"])
            try:
                status = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                status = None
            if status != 3:
                problems.append("Cancel did not end the copy (status %r)" % status)
            # 4. Reduced motion: a still picture.
            qa.unlink()
            calm = dict(env, SDE_REDUCED_MOTION="1")
            proc = s.run_demo(qa, "--at", "0.4", env=calm)
            g = s.geometry()
            sx, sy, sw, sh = (int(v) for v in qa_values(qa)["scene"].split(","))
            scene = (g["X"] + sx, g["Y"] + sy, sw, sh)
            a = s.shot(*scene)
            time.sleep(0.3)
            b = s.shot(*scene)
            if a != b or qa_values(qa).get("animating") != "0":
                problems.append("the picture moves with reduced motion")
            proc.kill()
            proc.wait()
            # 6. Two operations: the second one in the queue.
            qa.unlink()
            proc = s.run_demo(qa, "--at", "0.3", "--queue")
            values = qa_values(qa)
            if values.get("count") != "2" or values.get("queue_headline") != "Moving 12 items to Documents":
                problems.append("no second operation in the queue: %r" % values)
            if not values.get("queue_pause"):
                problems.append("the queued operation has no Pause button")
            proc.kill()
            proc.wait()
            # 7. Done on a removable drive: said, then the window goes.
            qa.unlink()
            proc = s.run_demo(qa, "--done", "--drive", "SANDISK", "--removable")
            values = qa_values(qa)
            if values.get("headline") != "Copied 1234 items to Backup Drive":
                problems.append("done headline: %r" % values.get("headline"))
            if values.get("status") != "The files are on SANDISK — you can remove it.":
                problems.append("done status: %r" % values.get("status"))
            try:
                status = proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                status = None
                problems.append("the finished operation did not go by itself")
            # A failed operation stays until its cross is clicked.
            qa.unlink()
            proc = s.run_demo(qa, "--fail")
            values = qa_values(qa)
            if not values.get("headline", "").startswith("Not everything could be copied"):
                problems.append("failed headline: %r" % values.get("headline"))
            if values.get("status") != "There is not enough space on Backup Drive.":
                problems.append("failed status: %r" % values.get("status"))
            time.sleep(4)
            if proc.poll() is not None:
                problems.append("the failed operation did not stay")
            else:
                s.click(qa_values(qa)["cancel"])
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    problems.append("the cross did not close the failed operation")
        finally:
            s.stop()
    for problem in problems:
        print("FAIL " + problem)
    print("SDE transfer dialog: %s" % ("passed" if not problems else "%d problem(s)" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
