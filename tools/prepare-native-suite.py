#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the complete SDE fork sources from immutable upstream commits."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "suite"))
import redesign  # noqa: E402  (SDE alpha10 suite redesign recipe)
LOCK = ROOT / "suite/forks.lock.json"
LEGAL = re.compile(r"^(COPYING|LICENSE|AUTHORS|COPYRIGHT|THANKS|TRADEMARK|ChangeLog|NEWS)", re.I)


def run(*command, cwd):
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def brand(text):
    # Whole-file substitutions are fast even for large translation catalogs.
    # Immutable legal notices and source links are masked before rewriting.
    protected = []
    def protect(match):
        protected.append(match.group())
        return "SDEPROTECTEDTOKEN%dEND" % (len(protected) - 1)
    link = r"https?://[^\s<>\"'\)\]]+|[\w.+-]+@(?:[\w-]+\.)+[A-Za-z]{2,}\b"
    # Lex quoted strings and comments together. A MIME literal such as
    # "image/*" must never be mistaken for the start of a C comment.
    token = r'/\*[\s\S]*?\*/|<!--[\s\S]*?-->|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|' + link
    def mask(match):
        value = match.group()
        if value.startswith(('/*', '<!--')):
            if re.search(r"copyright|\(c\)|©|SPDX|Free Software Foundation|permission is hereby granted", value, re.I):
                return protect(match)
            # GTK-Doc identifiers and type/ownership annotations must follow
            # the new C symbols so generated plugin APIs remain introspectable.
            return re.sub(link, protect, value)
        if value.startswith(('"', "'")):
            if re.search(r"copyright|\(c\)|©|SPDX", value, re.I):
                return protect(match)
            return re.sub(link, protect, value)
        return protect(match)
    text = re.sub(token, mask, text)
    text = re.sub(r"(?m)^\s*(?:\#(?!\s*(?:include|define|undef|if|ifdef|ifndef|elif|else|endif|error|warning|line|pragma)\b)|//).*?$", protect, text)
    for old, new in [('org.mate.pluma', 'org.debion.niny'), ('org.mate.Pluma', 'org.debion.Niny'), ('org.mate.caja', 'org.debion.liria'), ('org.mate.Caja', 'org.debion.Liria'), ('org.mate.atril', 'org.debion.xanara'), ('org.mate.Atril', 'org.debion.Xanara'), ('org.mate.engrampa', 'org.debion.taca'), ('org.mate.eom', 'org.debion.sdeview'), ('org.mate.system-monitor', 'org.debion.system-monitor'), ('/org/mate/pluma', '/org/debion/niny'), ('/org/mate/caja', '/org/debion/liria'), ('/org/mate/atril', '/org/debion/xanara'), ('/org/mate/engrampa', '/org/debion/taca'), ('/org/mate/eom', '/org/debion/sdeview'), ('/org/mate/system-monitor', '/org/debion/system-monitor'), ('MATE System Monitor', 'SDE System Monitor'), ('Eye of MATE', 'Photo'), ('Eye of Mate', 'Photo'), ('SDE View', 'Photo'), ('mate-system-monitor', 'sde-system-monitor'), ('mate_system_monitor', 'sde_system_monitor'), ('pluma', 'niny'), ('Pluma', 'Niny'), ('PLUMA', 'NINY'), ('caja', 'liria'), ('Caja', 'Liria'), ('CAJA', 'LIRIA'), ('atril', 'xanara'), ('Atril', 'Xanara'), ('ATRIL', 'XANARA'), ('engrampa', 'taca'), ('Engrampa', 'Taca'), ('ENGRAMPA', 'TACA'), ('libeom', 'libsdeview'), ('"System Monitor"', '"SDE System Monitor"')]:
        text = text.replace(old, new)
    # Do not corrupt GTK geometry APIs when changing the eom namespace.
    text = text.replace("-DEOM_", "-DSDEVIEW_")
    text = text.replace("org.mate.session", "org.debion.session")
    text = text.replace("org.mate.SystemMonitor", "org.debion.SystemMonitor")
    text = text.replace("org.mate.Taca", "org.debion.Taca").replace("/org/mate/Taca", "/org/debion/Taca")
    text = text.replace("org.mate.media-handling", "org.debion.media-handling")
    text = text.replace("/org/mate/desktop/media-handling", "/org/debion/desktop/media-handling")
    text = text.replace("org.mate.applications-terminal", "org.debion.applications-terminal")
    for old, new in [("System Monitor", "SDE System Monitor")]:
        text = text.replace(">" + old + "<", ">" + new + "<")
    for old, new in [("eom", "sdeview"), ("Eom", "Sdeview"), ("EOM", "SDEVIEW")]:
        text = re.sub(r"(?<![A-Za-z0-9])" + old + r"(?=$|[^a-z0-9])", new, text)
    # A shell/comment line can contain an already protected URL. Unmask outer
    # tokens first, then their nested earlier tokens.
    while re.search(r"SDEPROTECTEDTOKEN\d+END", text):
        text = re.sub(r"SDEPROTECTEDTOKEN(\d+)END", lambda m: protected[int(m[1])], text)
    return text



def stamp_version(text, version):
    # Lex character literals as well as strings. A C '\\"' must not make a
    # following os-release "VERSION=" field look like a product-version macro.
    token = r"(?m)^[ \t]*#[^\n]*|/\*[\s\S]*?\*/|//[^\n]*|\"(?:[^\"\\\n]|\\[\s\S])*\"|'(?:[^'\\\n]|\\[\s\S])*'|\b(?:VERSION|PACKAGE_VERSION)\b"
    return re.sub(token, lambda m: json.dumps(version) if m.group() in {"VERSION", "PACKAGE_VERSION"} else m.group(), text)


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
        # Two original product labels can become one translated SDE label.
        # Merge those gettext entries without dropping the catalog header.
        for path in staged.rglob("*.po"):
            subprocess.run(["msguniq", "--use-first", "-o", str(path), str(path)], check=True)
        if record["id"] == "liria":
            p = staged / "src/liria-application.c"
            p.write_text(p.read_text().replace("self->priv->no_desktop = FALSE;", "self->priv->no_desktop = TRUE;"))
        # SDE View's public executable uses a hyphen; C identifiers use sdeview.
        if record["id"] == "sde-view":
            p = staged / "src/Makefile.am"
            s = p.read_text().replace("bin_PROGRAMS = sdeview", "bin_PROGRAMS = sde-view").replace("bin_PROGRAMS=sdeview", "bin_PROGRAMS=sde-view")
            s = re.sub(r"(?m)^sdeview_(SOURCES|CFLAGS|CPPFLAGS|LDFLAGS|LDADD|DEPENDENCIES)\b", r"sde_view_\1", s)
            s = s.replace("Sdeview-1.0.gir: sdeview", "Sdeview-1.0.gir: sde-view")
            s = s.replace("_gir_PROGRAM = $(builddir)/sdeview", "_gir_PROGRAM = $(builddir)/sde-view")
            p.write_text(s)
            for p in staged.rglob("*.in"):
                if p.is_file():
                    p.write_text(p.read_text().replace("Exec=sdeview", "Exec=sde-view").replace(">sdeview.desktop<", ">sde-view.desktop<"))
        # Native GTK programs read SDE's theme at startup; there is no launcher
        # executable in front of niny/liria/xanara/taca or the other fork binaries.
        helper = (ROOT / "suite/sde-brand.h").read_text()
        for path in list(staged.rglob("*.c")) + list(staged.rglob("*.cpp")):
            s = path.read_text()
            # About dialogs and command versions identify the independent SDE
            # build; upstream ABI/library version variables stay unchanged.
            s = stamp_version(s, record["build_version"])
            if re.search(r"\bmain\s*\(\s*(?:int|gint)\b", s):
                header = path.parent / "sde-brand.h"
                header.write_text("#define SDE_NATIVE_NAME " + json.dumps(record["name"]) + "\n#define SDE_NATIVE_VERSION " + json.dumps(record["build_version"]) + "\n#define SDE_NATIVE_LIRIA " + str(int(record["id"] == "liria")) + "\n" + helper)
                s += '\n#include "sde-brand.h"\n'
            path.write_text(s)
        # SDE alpha10 redesign: overlays and checked source edits.
        redesign.apply(record, staged)
        attribution = f"{record['name']} — SDE native fork\nUpstream: {record['url']}\nRevision: {record['commit']}\n\nOriginal copyright, author and license documents are retained.\nSDE changes: product/namespace/paths, GTK startup integration, the SDE redesign\n(suite/redesign.py and suite/overlays) and Debian packaging.\n\n"
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
    recipe = b"".join(path.read_bytes() for path in redesign.inputs())
    expected = hashlib.sha256(LOCK.read_bytes() + Path(__file__).read_bytes() + (ROOT / "suite/sde-brand.h").read_bytes() + recipe).hexdigest()
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
