#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report original product names (MATE, Caja) where Liria users see them.

Scans a prepared Liria source tree (--source) or an installed prefix
(--prefix, e.g. /usr or a DESTDIR stage): desktop entries, AppStream data,
GtkBuilder files, GSettings schemas, man pages, help, translations (.po in a
source tree, compiled .mo when installed) and the printable strings of the
liria executable and its extensions. Legal notices are allowed: lines with a
copyright mark, the copyright holders of the original work (The Caja and
Nautilus authors, The MATE developers), the statement that Liria is a
modified version of Caja, its credit in the About window ("Caja 1.28
(MATE)") and paths to the installed upstream notices
(/usr/share/doc/sde/upstream/caja). Exit status 1 when anything else is
found.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

WORDS = re.compile(r"\bMATE\b|\bCaja\b|\bCAJA\b|mate-desktop\.org|mate-user-guide|help:mate")
ALLOWED = re.compile(r"^Authors=|copyright|\(c\)|©|modified version of the Caja|The (?:Caja|Nautilus) authors|"
                     r"The MATE developers|Caja 1\.28 \(MATE\)|upstream/caja|SPDX", re.I)
# User-visible text resources in a prepared source tree.
SOURCE_GLOBS = ["data/*.desktop.in.in", "data/*.appdata.xml.in.in", "data/*.ui", "data/*.css",
                "data/help/*.html", "data/*.xml.in", "libliria-private/*.gschema.xml", "src/*.ui",
                "src/*.xml", "src/file-manager/*.xml", "docs/*.1", "po/*.po"]


def leaks_in_text(name, text):
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith(("#", '"Language-Team', '"Last-Translator')):
            continue
        if WORDS.search(line) and not ALLOWED.search(line):
            found.append("%s:%d: %s" % (name, number, line.strip()[:120]))
    return found


def strings(path):
    try:
        out = subprocess.run(["strings", "-n", "4", str(path)], capture_output=True, text=True,
                             errors="replace", check=False).stdout
    except FileNotFoundError:
        data = path.read_bytes()
        out = "\n".join(m.decode("latin-1") for m in re.findall(rb"[\x20-\x7e]{4,}", data))
    return out


def po_messages(text):
    """msgid/msgstr text of a .po file without comments and header."""
    entries = re.split(r"\n\s*\n", text)
    return "\n".join(e for e in entries[1:] if not e.lstrip().startswith("#~"))


def scan_source(root):
    found = []
    for pattern in SOURCE_GLOBS:
        for path in sorted(root.glob(pattern)):
            text = path.read_text(errors="replace")
            if path.suffix == ".po":
                text = "\n".join(l for l in po_messages(text).splitlines() if not l.startswith("#"))
            else:
                text = re.sub(r"<!--[\s\S]*?-->", "", text)
            found += leaks_in_text(str(path.relative_to(root)), text)
    return found


def scan_prefix(prefix):
    share = prefix / "share"
    found = []
    files = []
    for pattern in ["applications/liria*.desktop", "metainfo/liria*", "liria/**/*", "man/man1/liria*",
                    "glib-2.0/schemas/org.debion.liria*.xml", "dbus-1/services/*Liria*",
                    "mime/packages/liria.xml"]:
        files += [p for p in share.glob(pattern) if p.is_file()]
    for path in sorted(set(files)):
        if path.suffix in (".png", ".svg", ".jpg"):
            continue
        found += leaks_in_text(str(path), path.read_text(errors="replace"))
    for mo in sorted(share.glob("locale/*/LC_MESSAGES/liria.mo")):
        found += leaks_in_text(str(mo), strings(mo))
    binaries = [prefix / "bin" / "liria"] + sorted(prefix.glob("lib*/**/liria/extensions-2.0/*.so"))
    for path in binaries:
        if path.is_file():
            found += leaks_in_text(str(path), strings(path))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--source", type=Path, help="prepared Liria source tree")
    group.add_argument("--prefix", type=Path, help="installed prefix, e.g. /usr")
    args = parser.parse_args()
    found = scan_source(args.source) if args.source else scan_prefix(args.prefix)
    for line in found:
        print(line)
    print("Liria branding: %d leak(s)" % len(found))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
