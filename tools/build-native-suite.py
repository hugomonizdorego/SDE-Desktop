#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and stage the SDE GTK forks. Never installs upstream application packages."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--prefix", default="/usr")
    parser.add_argument("--libdir", default="/usr/lib")
    parser.add_argument("--sysconfdir", default="/etc")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 2))
    parser.add_argument("--only", help="Build one fork for development")
    args = parser.parse_args()
    args.sources = args.sources.resolve()
    args.build = args.build.resolve()
    args.stage = args.stage.resolve()
    args.build.mkdir(parents=True, exist_ok=True)
    args.stage.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / "suite/forks.lock.json").read_text())
    if args.only and args.only not in {app["id"] for app in lock["forks"]}:
        parser.error("Unknown native fork: " + args.only)
    if not (args.sources / ".sde-fork-lock").is_file():
        parser.error("Run python3 tools/prepare-native-suite.py before the offline build")
    recipe = (args.sources / ".sde-fork-lock").read_bytes() + Path(__file__).read_bytes()
    recipe += (str(args.stage) + args.prefix + args.libdir + args.sysconfdir).encode()
    for app in lock["forks"]:
        if args.only and args.only != app["id"]:
            continue
        # Downstream source edits must invalidate the build even when the
        # upstream pin and preparation recipe have not changed.
        digest = hashlib.sha256(recipe)
        for path in sorted((args.sources / app["id"]).rglob("*")):
            digest.update(str(path.relative_to(args.sources)).encode())
            if path.is_symlink():
                digest.update(os.readlink(path).encode())
            elif path.is_file():
                digest.update(path.read_bytes())
        signature = digest.hexdigest()
        destination = args.build / app["id"]
        stamp = destination / ".built"
        installed = args.stage / args.prefix.lstrip("/") / "bin" / app["id"]
        if stamp.exists() and stamp.read_text().strip() == signature and installed.is_file():
            print("Native", app["id"], "already built", flush=True)
            continue
        if destination.exists():
            # This directory is generated build output, never the editable source.
            shutil.rmtree(destination)
        destination.mkdir()
        source = destination / "source"
        shutil.copytree(args.sources / app["id"], source)
        obj = destination / "obj"
        obj.mkdir()
        env = os.environ.copy()
        env["NOCONFIGURE"] = "1"
        env["SOURCE_DATE_EPOCH"] = env.get("SOURCE_DATE_EPOCH", "1792195200")
        # Point only fork-owned pkg-config files at DESTDIR. Host GTK/MATE
        # libraries still use the distribution paths, never a fake sysroot.
        pc = args.build / "pkgconfig"
        pc.mkdir(exist_ok=True)
        libroot = args.stage / args.libdir.lstrip("/")
        for original in libroot.glob("pkgconfig/*.pc"):
            value = original.read_text().replace("prefix=" + args.prefix, "prefix=" + str(args.stage / args.prefix.lstrip("/")))
            value = value.replace("libdir=" + args.libdir, "libdir=" + str(libroot))
            (pc / original.name).write_text(value)
        env["PKG_CONFIG_PATH"] = str(pc) + os.pathsep + env.get("PKG_CONFIG_PATH", "")
        env.pop("PKG_CONFIG_SYSROOT_DIR", None)
        env["LD_LIBRARY_PATH"] = str(libroot) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        logfile = args.build / (app["id"] + ".log")
        logfile.write_text("")
        print("Building native", app["id"], "from", app["upstream"], app["commit"], flush=True)
        def run(command, cwd):
            with logfile.open("a") as output:
                output.write("\n$ " + " ".join(command) + "\n")
                output.flush()
                result = subprocess.run(command, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT)
            if result.returncode:
                print("\n".join(logfile.read_text(errors="replace").splitlines()[-140:]), file=sys.stderr)
                raise SystemExit(app["id"] + " build failed; full log: " + str(logfile))
        run(["sh", "./autogen.sh"], source)
        configure = [str(source / "configure"), "--prefix=" + args.prefix, "--libdir=" + args.libdir,
                     "--libexecdir=" + args.prefix + "/libexec", "--sysconfdir=" + args.sysconfdir,
                     "--disable-maintainer-mode", "--disable-schemas-compile"]
        # Cross-app references have been renamed to the Liria extension API.
        configure.extend(option.replace("caja", "liria") for option in app["configure"])
        if app["id"] == "taca":
            # pkg-config CFLAGS/LIBS use staging paths during compilation;
            # the installed extension directory must use the final prefix.
            configure.append("--with-liriadir=" + args.libdir + "/liria/extensions-2.0")
        run(configure, obj)
        run(["make", "-j" + str(args.jobs)], obj)
        run(["make", "DESTDIR=" + str(args.stage), "install"], obj)
        if app["id"] == "sde-screensaver":
            # Upstream's install hook probes a hardcoded host include path
            # and misses this file with multiarch PAM or out-of-tree builds.
            pam = args.stage / args.sysconfdir.lstrip("/") / "pam.d/sde-screensaver"
            pam.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / "data/sde-screensaver", pam)
        docs = args.stage / args.prefix.lstrip("/") / "share/doc/sde/upstream" / app["upstream"]
        docs.mkdir(parents=True, exist_ok=True)
        for name in ["COPYING", "COPYING.LIB", "COPYRIGHT", "AUTHORS", "TRADEMARK_NOTICE", "SDE-UPSTREAM-NOTICE", "SDE-COPYRIGHT-HEADERS"]:
            original = source / name
            if original.is_file():
                shutil.copy2(original, docs / name)
        notice = (source / "SDE-UPSTREAM-NOTICE").read_text()
        for name in ["AUTHORS", "COPYING", "COPYING.LIB", "TRADEMARK_NOTICE", "SDE-COPYRIGHT-HEADERS"]:
            if (source / name).is_file():
                notice += "\n\n=== " + name + " ===\n" + (source / name).read_text(errors="replace")
        (docs / "copyright").write_text(notice)
        stamp.write_text(signature + "\n")
    # SDE owns menu entries and autostarts. Native app resources, schemas,
    # D-Bus services, helpers, plugins and icons remain those of the SDE forks.
    for relative in [args.prefix.lstrip("/") + "/share/applications", args.sysconfdir.lstrip("/") + "/xdg/autostart"]:
        path = args.stage / relative
        if path.exists():
            # Keep screensaver theme descriptors in applications/screensavers;
            # only top-level launchers/autostarts are replaced by SDE entries.
            for desktop in path.glob("*.desktop"):
                desktop.unlink()
    # Some upstream install hooks generate indexes for DESTDIR alone. Those
    # incomplete indexes must never replace the distribution's shared caches.
    mime = args.stage / args.prefix.lstrip("/") / "share/mime"
    if mime.exists():
        for entry in mime.iterdir():
            if entry.name == "packages":
                continue
            if entry.is_dir() and not entry.is_symlink():
                shutil.rmtree(entry)
            else:
                entry.unlink()
    for name in ["gschemas.compiled", "icon-theme.cache", "mimeinfo.cache"]:
        for cache in args.stage.rglob(name):
            cache.unlink()
    for elf in args.stage.rglob("*"):
        if not elf.is_file() or elf.is_symlink():
            continue
        with elf.open("rb") as stream:
            native = stream.read(4) == b"\x7fELF"
        if not native:
            continue
        dynamic = subprocess.run(["readelf", "-d", str(elf)], capture_output=True, text=True, check=False).stdout
        if any(str(path) in dynamic for path in [args.stage, args.build]):
            subprocess.run(["chrpath", "-d", str(elf)], check=True)
    print("Native SDE suite staged at", args.stage, flush=True)


if __name__ == "__main__":
    main()
