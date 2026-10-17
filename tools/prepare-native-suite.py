#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the complete SDE fork sources from immutable upstream commits."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "suite/forks.lock.json"
LEGAL = re.compile(r"^(COPYING|LICENSE|AUTHORS|COPYRIGHT|THANKS|TRADEMARK|ChangeLog|NEWS)", re.I)


def run(*command, cwd):
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def brand(text):
    # Whole-file substitutions are fast even for large translation catalogs.
    # Immutable notices, comments and source links are masked before rewriting.
    protected = []
    def protect(match):
        protected.append(match.group())
        return "SDEPROTECTEDTOKEN%dEND" % (len(protected) - 1)
    text = re.sub(r"https?://[^\s<>\"'\)\]]+|[\w.+-]+@[\w.-]+|/\*[\s\S]*?\*/|<!--[\s\S]*?-->", protect, text)
    # Preserve notice text, while still renaming identifiers such as
    # CAJA_COPYRIGHT in definitions and uses consistently.
    text = re.sub(r'"(?:[^"\\]|\\.)*(?:[Cc]opyright|\(c\)|©|SPDX)(?:[^"\\]|\\.)*"', protect, text)
    text = re.sub(r"(?m)^\s*(?:\#(?!\s*(?:include|define|undef|if|ifdef|ifndef|elif|else|endif|error|warning|line|pragma)\b)|//).*?$", protect, text)
    for old, new in [('org.mate.pluma', 'org.debion.niny'), ('org.mate.Pluma', 'org.debion.Niny'), ('org.mate.caja', 'org.debion.liria'), ('org.mate.Caja', 'org.debion.Liria'), ('org.mate.atril', 'org.debion.xanara'), ('org.mate.Atril', 'org.debion.Xanara'), ('org.mate.engrampa', 'org.debion.taca'), ('org.mate.eom', 'org.debion.sdeview'), ('org.mate.power-manager', 'org.debion.power-manager'), ('org.mate.system-monitor', 'org.debion.system-monitor'), ('org.mate.screensaver', 'org.debion.screensaver'), ('org.mate.PowerManager', 'org.debion.PowerManager'), ('org.mate.ScreenSaver', 'org.debion.ScreenSaver'), ('/org/mate/pluma', '/org/debion/niny'), ('/org/mate/caja', '/org/debion/liria'), ('/org/mate/atril', '/org/debion/xanara'), ('/org/mate/engrampa', '/org/debion/taca'), ('/org/mate/eom', '/org/debion/sdeview'), ('/org/mate/power-manager', '/org/debion/power-manager'), ('/org/mate/system-monitor', '/org/debion/system-monitor'), ('/org/mate/screensaver', '/org/debion/screensaver'), ('/org/mate/ScreenSaver', '/org/debion/ScreenSaver'), ('MATE System Monitor', 'SDE System Monitor'), ('MATE Power Manager', 'SDE Power Manager'), ('MATE Screensaver', 'SDE Screen Lock'), ('MATE ScreenSaver', 'SDE Screen Lock'), ('Eye of MATE', 'SDE View'), ('Eye of Mate', 'SDE View'), ('mate-system-monitor', 'sde-system-monitor'), ('mate_system_monitor', 'sde_system_monitor'), ('mate-power-manager', 'sde-power-manager'), ('mate_power_manager', 'sde_power_manager'), ('mate-power-preferences', 'sde-power-preferences'), ('mate-power-statistics', 'sde-power-statistics'), ('mate-screensaver', 'sde-screensaver'), ('mate_screensaver', 'sde_screensaver'), ('pluma', 'niny'), ('Pluma', 'Niny'), ('PLUMA', 'NINY'), ('caja', 'liria'), ('Caja', 'Liria'), ('CAJA', 'LIRIA'), ('atril', 'xanara'), ('Atril', 'Xanara'), ('ATRIL', 'XANARA'), ('engrampa', 'taca'), ('Engrampa', 'Taca'), ('ENGRAMPA', 'TACA'), ('libeom', 'libsdeview'), ('"System Monitor"', '"SDE System Monitor"'), ('"Power Preferences"', '"SDE Power Preferences"'), ('"Power Statistics"', '"SDE Power Statistics"')]:
        text = text.replace(old, new)
    # Do not corrupt GTK geometry APIs when changing the eom namespace.
    for old, new in [("eom", "sdeview"), ("Eom", "Sdeview"), ("EOM", "SDEVIEW")]:
        text = re.sub(r"(?<![A-Za-z0-9])" + old + r"(?=$|[^a-z0-9])", new, text)
    return re.sub(r"SDEPROTECTEDTOKEN(\d+)END", lambda m: protected[int(m[1])], text)


def prepare(record, cache, destination):
    checkout = cache / record["upstream"]
    if not checkout.exists():
        checkout.mkdir(parents=True)
        run("git", "init", "--quiet", cwd=checkout)
    if not (checkout / ".git").is_dir():
        raise ValueError("Cache is not a git checkout: " + str(checkout))
    if run("git", "status", "--porcelain", "--ignore-submodules=all", cwd=checkout):
        raise ValueError("Refusing to discard modified cache: " + str(checkout))
    try:
        commit = run("git", "rev-parse", "HEAD", cwd=checkout)
    except subprocess.CalledProcessError:
        commit = ""
    if commit != record["commit"]:
        run("git", "fetch", "--quiet", "--depth", "1", record["url"] + ".git", record["commit"], cwd=checkout)
        run("git", "checkout", "--quiet", "--detach", "FETCH_HEAD", cwd=checkout)
    if (checkout / ".gitmodules").exists():
        run("git", "submodule", "update", "--init", "--recursive", "--depth", "1", cwd=checkout)
    if run("git", "rev-parse", "HEAD", cwd=checkout) != record["commit"]:
        raise ValueError("Upstream revision mismatch")
    source = destination / record["id"]
    if source.exists():
        raise ValueError("Output exists; preserve edits and choose another output: " + str(source))
    with tempfile.TemporaryDirectory(prefix="fork-", dir=destination) as temporary:
        staged = Path(temporary) / "source"
        shutil.copytree(checkout, staged, ignore=shutil.ignore_patterns(".git"))
        # All files (including submodules) are retained in the corresponding
        # source. Legal documents and every original copyright line stay intact.
        notices = []
        for path in list(staged.rglob("*")):
            if not path.is_file() or path.is_symlink():
                continue
            try:
                original = path.read_text(encoding="utf-8", errors="surrogateescape")
            except (UnicodeError, ValueError):
                continue
            if "\0" in original:
                continue
            headers = re.findall(r"/\*[\s\S]*?\*/", original)
            for header in headers:
                if re.search(r"copyright|\(c\)|©", header, re.I):
                    notices.append(str(path.relative_to(staged)) + "\n" + header)
            if not LEGAL.match(path.name):
                path.write_text(brand(original), encoding="utf-8", errors="surrogateescape")
        # Rename identifiers in filenames/directories after contents are patched.
        for path in sorted(staged.rglob("*"), key=lambda p: len(p.parts), reverse=True):
            wanted = brand(path.name)
            if wanted != path.name and not LEGAL.match(path.name):
                path.rename(path.with_name(wanted))
        if record["id"] == "liria":
            p = staged / "src/liria-application.c"
            p.write_text(p.read_text().replace("self->priv->no_desktop = FALSE;", "self->priv->no_desktop = TRUE;"))
        # SDE View's public executable uses a hyphen; C identifiers use sdeview.
        if record["id"] == "sde-view":
            p = staged / "src/Makefile.am"
            s = p.read_text().replace("bin_PROGRAMS = sdeview", "bin_PROGRAMS = sde-view").replace("bin_PROGRAMS=sdeview", "bin_PROGRAMS=sde-view")
            s = re.sub(r"(?m)^sdeview_", "sde_view_", s)
            p.write_text(s)
            for p in staged.rglob("*.in"):
                if p.is_file():
                    p.write_text(p.read_text().replace("Exec=sdeview", "Exec=sde-view"))
        # Native GTK programs read SDE's theme at startup; there is no launcher
        # executable in front of niny/liria/xanara/taca or the other fork binaries.
        helper = (ROOT / "suite/sde-brand.h").read_text()
        for path in list(staged.rglob("*.c")) + list(staged.rglob("*.cpp")):
            s = path.read_text()
            if re.search(r"\bmain\s*\(\s*(?:int|gint)\b", s):
                header = path.parent / "sde-brand.h"
                header.write_text(helper)
                path.write_text(s + '\n#include "sde-brand.h"\n')
        if record["id"] == "sde-screensaver":
            pam = staged / "data/sde-screensaver"
            pam.write_text("# SDE screen lock: Debian PAM policy\n@include common-auth\n@include common-account\n@include common-session\n")
        attribution = f"{record['name']} — SDE native fork\nUpstream: {record['url']}\nRevision: {record['commit']}\n\nOriginal copyright, author and license documents are retained.\nSDE changes: product/namespace/paths, GTK startup integration and Debian packaging.\n\n"
        (staged / "SDE-UPSTREAM-NOTICE").write_text(attribution)
        (staged / "SDE-COPYRIGHT-HEADERS").write_text("\n\n".join(notices))
        staged.rename(source)
    print("Prepared", record["id"], record["commit"], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apps", nargs="*")
    parser.add_argument("--cache", type=Path, default=ROOT / "source-work/native")
    parser.add_argument("--output", type=Path, default=ROOT / "vendor/suite")
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text())
    expected = hashlib.sha256(LOCK.read_bytes() + Path(__file__).read_bytes() + (ROOT / "suite/sde-brand.h").read_bytes()).hexdigest()
    marker = args.output / ".sde-fork-lock"
    if not args.apps and marker.exists() and marker.read_text().strip() == expected:
        print("SDE fork sources already prepared", flush=True)
        return
    if marker.exists():
        parser.error("Fork recipe changed; preserve edits and choose a new --output directory")
    unknown = set(args.apps) - {row["id"] for row in lock["forks"]}
    if unknown:
        parser.error("Unknown forks: " + ", ".join(sorted(unknown)))
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    for record in lock["forks"]:
        if not args.apps or record["id"] in args.apps:
            prepare(record, args.cache, args.output)
    if not args.apps:
        marker.write_text(expected + "\n")


if __name__ == "__main__":
    main()
