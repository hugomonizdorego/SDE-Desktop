#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Read-only list of APT package sources for SDE Update. Nothing is written.
#
# Protocol (tab separated):
#   SDE-SOURCES  1
#   S  file  line  format  enabled  types  uris  suites  components  signed_by
# format is "one-line" (sources.list) or "deb822" (.sources); line is the
# line number where the entry starts; enabled is 1 or 0; lists use spaces.
import glob
import os
import re
import sys

PROTOCOL = "SDE-SOURCES\t1"
TYPES = ("deb", "deb-src")


def clean(value):
    return " ".join(str(value).split())


def entry(path, line, fmt, enabled, types, uris, suites, components, signed_by=""):
    return {
        "file": path,
        "line": line,
        "format": fmt,
        "enabled": enabled,
        "types": types,
        "uris": uris,
        "suites": suites,
        "components": components,
        "signed_by": signed_by,
    }


def parse_one_line(text, path=""):
    entries = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        enabled = True
        if line.startswith("#"):
            # Commented-out entries are shown as disabled sources.
            line = line.lstrip("#").strip()
            enabled = False
        if not line:
            continue
        options = {}
        match = re.match(r"^(deb|deb-src)\s+\[([^\]]*)\]\s*(.*)$", line)
        if match:
            kind, opts, rest = match.groups()
            for item in opts.split():
                key, _, value = item.partition("=")
                options[key] = value
        else:
            parts = line.split(None, 1)
            if len(parts) < 2 or parts[0] not in TYPES:
                continue
            kind, rest = parts
        fields = rest.split("#", 1)[0].split()
        if len(fields) < 2 or "://" not in fields[0] and not fields[0].startswith(
            ("cdrom:", "file:", "/")
        ):
            continue
        entries.append(
            entry(
                path,
                number,
                "one-line",
                enabled,
                kind,
                fields[0],
                fields[1],
                " ".join(fields[2:]),
                options.get("signed-by", ""),
            )
        )
    return entries


def parse_deb822(text, path=""):
    entries = []
    fields, start, key = {}, 0, None

    def flush():
        if fields.get("types") and fields.get("uris"):
            enabled = fields.get("enabled", "yes").strip().lower() not in ("no", "false", "0")
            signed = fields.get("signed-by", "").strip()
            if "-----BEGIN" in signed:
                signed = "(embedded key)"
            entries.append(
                entry(
                    path,
                    start,
                    "deb822",
                    enabled,
                    clean(fields.get("types", "")),
                    clean(fields.get("uris", "")),
                    clean(fields.get("suites", "")),
                    clean(fields.get("components", "")),
                    clean(signed.splitlines()[0] if signed else ""),
                )
            )

    for number, raw in enumerate(text.splitlines(), 1):
        if raw.startswith("#"):
            continue
        if not raw.strip():
            flush()
            fields, start, key = {}, 0, None
            continue
        if raw[0] in " \t" and key:
            fields[key] += "\n" + raw.strip()
            continue
        name, sep, value = raw.partition(":")
        if not sep:
            continue
        key = name.strip().lower()
        fields[key] = value.strip()
        if not start:
            start = number
    flush()
    return entries


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def collect(root="/etc/apt"):
    entries = []
    main = os.path.join(root, "sources.list")
    if os.path.isfile(main):
        entries += parse_one_line(read(main), main)
    directory = os.path.join(root, "sources.list.d")
    for path in sorted(glob.glob(os.path.join(directory, "*.list"))):
        entries += parse_one_line(read(path), path)
    for path in sorted(glob.glob(os.path.join(directory, "*.sources"))):
        entries += parse_deb822(read(path), path)
    return entries


def emit(entries, output):
    print(PROTOCOL, file=output)
    for e in entries:
        fields = [
            "S",
            e["file"],
            e["line"],
            e["format"],
            "1" if e["enabled"] else "0",
            e["types"],
            e["uris"],
            e["suites"],
            e["components"],
            e["signed_by"],
        ]
        print("\t".join(map(clean, fields)), file=output)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/etc/apt"
    emit(collect(root), sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
