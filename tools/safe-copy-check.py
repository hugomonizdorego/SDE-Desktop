#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Safe copying on a real slow drive (0.2.0 Alpha 3, src/common/safe_copy.h).

  sudo tools/safe-copy-check.py --i-use-a-test-machine --build-dir build
                                [--rate 20000000]

Makes a slow "USB stick": a loop device on a sparse file with ext4, whose
writes are limited to --rate bytes per second with the blkio cgroup (the
limit also holds for the kernel's writeback), and copies a folder of one
large and many small files onto it

  1. with the writer itself (build/tests/sde-safe-copy-bench), in the page
     cache (the old way) and through the safe writer;
  2. with the SDE desktop's file operations (desk::run_file_ops, as a drop
     or Paste does, and as Liria copies since 0.2.0 Alpha 4), the old way
     (SDE_SAFE_COPY=never) and safely,

while it reads the progress the copy reports and the drive's own count of
written bytes (/sys/block/loopN/stat) every 50 ms. Safe copies must never
report more than the drive has written, and must say they are done only
when the drive has everything. The loop device is not removable, so the
safe runs set SDE_SAFE_COPY=always. Prints the numbers (docs/TRANSFERS.md
has them). Needs root, losetup, mkfs.ext4 and the blkio cgroup (v1), and
--i-use-a-test-machine: it attaches a loop device and writes a limit into
/sys, so run it only on a machine of your own for tests, never on a shared
one or in CI. Exit status 0 passed, 1 failed, 77 skipped.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THROTTLE = Path("/sys/fs/cgroup/blkio/blkio.throttle.write_bps_device")
MIB = 1 << 20


class Drive:
    """The loop device, its file system and its write limit."""

    def __init__(self, work, rate, size="384M"):
        self.work, self.rate = work, rate
        self.image = work / "drive.img"
        self.mount = work / "drive"
        self.loop = None
        subprocess.run(["truncate", "-s", size, str(self.image)], check=True)
        self.loop = subprocess.run(["losetup", "-f", "--show", str(self.image)], check=True, capture_output=True,
                                   text=True).stdout.strip()
        self.name = os.path.basename(self.loop)
        self.dev = Path("/sys/block/%s/dev" % self.name).read_text().strip()
        subprocess.run(["mkfs.ext4", "-q", "-F", self.loop], check=True)
        self.mount.mkdir()
        subprocess.run(["mount", self.loop, str(self.mount)], check=True)
        self.mount.chmod(0o1777)
        self.limit(rate)

    def limit(self, rate):
        THROTTLE.write_text("%s %d\n" % (self.dev, rate))

    def written(self):
        """Bytes the drive has written since it was made."""
        fields = Path("/sys/block/%s/stat" % self.name).read_text().split()
        return int(fields[6]) * 512

    def settle(self):
        subprocess.run(["sync", "-f", str(self.mount)])

    def close(self):
        if self.loop:
            try:
                self.limit(0)
            except OSError:
                pass
            subprocess.run(["umount", str(self.mount)])
            subprocess.run(["losetup", "-d", self.loop])
        self.image.unlink(missing_ok=True)


def make_source(folder, large_mib, small):
    folder.mkdir(parents=True)
    with open(folder / "Holiday video.mov", "wb") as f:
        block = os.urandom(MIB)
        for _ in range(large_mib):
            f.write(block)
    (folder / "Photos").mkdir()
    for i in range(small):
        (folder / "Photos" / ("IMG_%04d.jpg" % i)).write_bytes(os.urandom(32 << 10))
    return sum(p.stat().st_size for p in folder.rglob("*") if p.is_file())


def parse(line):
    return dict(part.split("=", 1) for part in line.split() if "=" in part)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--rate", type=int, default=20_000_000, help="the drive's speed, bytes per second")
    parser.add_argument("--large", type=int, default=96, help="MiB of the large file")
    parser.add_argument("--small", type=int, default=400, help="number of 32 KiB files")
    parser.add_argument("--i-use-a-test-machine", dest="test_machine", action="store_true",
                        help="this machine is for tests: a loop device and a write limit in /sys are fine")
    args = parser.parse_args()
    if not args.test_machine:
        print("skipped: a loop device and a write limit in /sys; run it with --i-use-a-test-machine on a test machine")
        return 77
    bench = args.build_dir / "tests/sde-safe-copy-bench"
    if os.geteuid() != 0 or not THROTTLE.exists() or not shutil.which("losetup") or not shutil.which("mkfs.ext4"):
        print("skipped: needs root, losetup, mkfs.ext4 and %s" % THROTTLE)
        return 77
    if not bench.exists():
        print("skipped: %s not built" % bench)
        return 77
    problems, rows = [], []
    work = Path(tempfile.mkdtemp(prefix="sde-safe-copy-"))
    drive = None
    try:
        source = work / "Source"
        total = make_source(source, args.large, args.small)
        drive = Drive(work, args.rate)
        print("drive %s at %.0f MB/s, %d files, %.1f MB" % (drive.loop, args.rate / 1e6, args.small + 1,
                                                             total / 1e6))
        # 1. The writer itself.
        for method in ("cache", "range"):
            target = drive.mount / ("engine-" + method)
            out = subprocess.run([str(bench), str(source), str(target), "--method", method, "--device", drive.name,
                                  "--sync-after"], capture_output=True, text=True)
            v = parse(out.stdout)
            rows.append(("writer, " + ("page cache" if method == "cache" else "safe"), float(v["done"]),
                         float(v["durable"]), int(v["lead"]), None))
            if method == "range" and (int(v["lead"]) > 0 or float(v["durable"]) > float(v["done"]) + 0.05):
                problems.append("writer: ahead of the drive or done too early: %s" % out.stdout.strip())
            shutil.rmtree(target)
            drive.settle()
        # 2. The desktop's file operations.
        for mode in ("never", "always"):
            target = drive.mount / ("desktop-" + mode)
            out = subprocess.run([str(bench), str(source), str(target), "--desktop", "--device", drive.name],
                                 capture_output=True, text=True, env=dict(os.environ, SDE_SAFE_COPY=mode))
            v = parse(out.stdout)
            drive_at_done = int(v["drive"])
            rows.append(("desktop, " + ("old" if mode == "never" else "safe"), float(v["done"]), None,
                         int(v["lead"]), drive_at_done))
            if mode == "always":
                if int(v["lead"]) > 0:
                    problems.append("desktop: the progress ran ahead of the drive: %s" % out.stdout.strip())
                if drive_at_done < total:
                    problems.append("desktop: done before the drive had everything: %s" % out.stdout.strip())
                # The last phase ("Writing the last data to ...") lasts about
                # the window when the copy ends with a large file, and may
                # be shorter than a sample when it ends with small ones.
                print("desktop: last phase %s s" % v.get("finish", "?"))
            shutil.rmtree(target)
            drive.settle()
    finally:
        if drive:
            drive.close()
        shutil.rmtree(work, ignore_errors=True)
    print("%-22s %9s %9s %14s %16s" % ("copy", "done (s)", "durable", "max lead (MB)", "on drive at done"))
    for name, done, durable, lead, on_drive in rows:
        print("%-22s %9.2f %9s %14.1f %16s" % (name, done, "%.2f" % durable if durable is not None else "-",
                                              lead / 1e6, "-" if on_drive is None else
                                              "%.0f%%" % (100.0 * min(on_drive, total) / total)))
    for problem in problems:
        print("FAIL " + problem)
    print("safe copying: %s" % ("passed" if not problems else "%d problem(s)" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
