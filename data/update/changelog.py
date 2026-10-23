#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Changelog provider for SDE Update. Runs as the user and never changes the
# system: `apt-get changelog` (network), then PackageKit's GetUpdateDetail
# through `pkcon get-update-detail`, then the installed changelog.Debian.gz.
#
# Output: "SOURCE<TAB>apt|packagekit|installed|none" then the changelog text.
import gzip
import os
import re
import subprocess
import sys

NAME = re.compile(r"^[a-z0-9][a-z0-9+.-]*(:[a-z0-9]+)?$")
MAX_LINES = 400
TIMEOUT = 20
DETAIL_KEYS = {
    "package", "updates", "obsoletes", "vendor", "bugzilla", "cve", "restart",
    "state", "issued", "updated", "description",
}


def valid_name(name):
    return bool(NAME.fullmatch(name or ""))  # not match(): "$" allows a final newline


def run(argv, timeout=TIMEOUT):
    try:
        result = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            timeout=timeout,
            env=dict(os.environ, LC_ALL="C", PAGER="cat"),
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.decode("utf-8", "replace")


def limit(text, lines=MAX_LINES):
    rows = text.rstrip().splitlines()
    if len(rows) > lines:
        rows = rows[:lines] + ["…"]
    return "\n".join(rows)


def from_packagekit_detail(text):
    """Extracts the changelog (or update text) from `pkcon get-update-detail`."""
    out, inside = [], False
    for line in text.splitlines():
        key = line.strip().split(":", 1)[0].strip().lower()
        if key in ("changelog", "update text"):
            inside = True
            value = line.split(":", 1)[1].strip() if ":" in line else ""
            if value:
                out.append(value)
            continue
        if inside and key in DETAIL_KEYS and ":" in line:
            inside = False
        if inside:
            out.append(line.strip())
    return "\n".join(out).strip()


def installed_changelog(name, doc="/usr/share/doc"):
    base = name.split(":")[0]
    for file in ("changelog.Debian.gz", "changelog.gz"):
        path = os.path.join(doc, base, file)
        try:
            with gzip.open(path, "rt", encoding="utf-8", errors="replace") as handle:
                return handle.read(200000)
        except OSError:
            continue
    return ""


def changelog(name):
    text = run(["apt-get", "changelog", "-qq", name])
    if text and text.strip():
        return "apt", limit(text)
    text = run(["pkcon", "-p", "get-update-detail", name])
    if text:
        detail = from_packagekit_detail(text)
        if detail:
            return "packagekit", limit(detail)
    text = installed_changelog(name)
    if text:
        return "installed", limit(text, 120)
    return "none", ""


def main():
    if len(sys.argv) != 2 or not valid_name(sys.argv[1]):
        print("Usage: changelog.py PACKAGE", file=sys.stderr)
        return 2
    source, text = changelog(sys.argv[1])
    print("SOURCE\t" + source)
    if text:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
