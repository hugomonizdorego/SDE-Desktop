#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Read-only Flatpak update provider for the Software Updater (sde-update).
# Runs as the user and never changes anything: it asks `flatpak remote-ls
# --updates` for each installation (system and user) and `flatpak list` for
# the installed versions.
#
# Protocol (tab separated, one record per line, UTF-8):
#   SDE-FLATPAK  1
#   I  0|1        (1 when flatpak is installed; nothing else follows for 0)
#   F  installation  ref  name  application  branch  origin  old  new  size
#      summary
#   E  text       (a non-fatal error, e.g. a remote that cannot be reached)
# installation is system or user; ref is kind/id/arch/branch; size is the
# download size in bytes (0 when unknown).
import os
import re
import shutil
import subprocess
import sys

PROTOCOL = "SDE-FLATPAK\t1"
TIMEOUT = 60
COLUMNS = "ref,name,application,version,branch,origin,download-size,description"
REF = re.compile(r"^(app|runtime)/[A-Za-z0-9_][A-Za-z0-9_.-]*/[A-Za-z0-9_]+/[A-Za-z0-9_.-]+$")
UNITS = {
    "": 1, "b": 1, "byte": 1, "bytes": 1,
    "kb": 1000, "mb": 1000 ** 2, "gb": 1000 ** 3, "tb": 1000 ** 4,
    "kib": 1024, "mib": 1024 ** 2, "gib": 1024 ** 3, "tib": 1024 ** 4,
}


def clean(value):
    return " ".join(str(value).split())


def valid_ref(ref):
    return bool(REF.fullmatch(ref or ""))


def parse_size(text):
    """'12.3 MB' / '512 bytes' / '1,5 kB' / '4096' -> bytes (0 if unknown)."""
    text = (text or "").strip().replace(" ", " ")
    match = re.match(r"^([0-9]+(?:[.,][0-9]+)?)\s*([A-Za-z]*)$", text)
    if not match:
        return 0
    unit = UNITS.get(match.group(2).lower())
    if unit is None:
        return 0
    return int(round(float(match.group(1).replace(",", ".")) * unit))


def parse_remote_ls(text, installation):
    """Rows of `flatpak remote-ls --updates --columns=COLUMNS` (tab separated)."""
    out = []
    for line in text.splitlines():
        fields = line.split("\t")
        if len(fields) < 7:
            continue
        fields += [""] * (8 - len(fields))
        ref, name, app, version, branch, origin, size, summary = fields[:8]
        ref = ref.strip()
        if not valid_ref(ref):
            continue
        out.append({
            "installation": installation,
            "ref": ref,
            "name": name.strip() or ref.split("/")[1],
            "application": app.strip() or ref.split("/")[1],
            "branch": branch.strip() or ref.split("/")[3],
            "origin": origin.strip(),
            "old": "",
            "new": version.strip(),
            "size": parse_size(size),
            "summary": summary.strip(),
        })
    return out


def parse_list(text):
    """`flatpak list --columns=ref,version` -> {ref: version}."""
    versions = {}
    for line in text.splitlines():
        fields = line.split("\t")
        if fields and valid_ref(fields[0].strip()):
            versions[fields[0].strip()] = fields[1].strip() if len(fields) > 1 else ""
    return versions


def run(argv):
    try:
        result = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.DEVNULL,
            timeout=TIMEOUT,
            env=dict(os.environ, LC_ALL="C"),
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, str(error)
    out = result.stdout.decode("utf-8", "replace")
    err = result.stderr.decode("utf-8", "replace").strip()
    return (out if result.returncode == 0 else None), err


def collect(flatpak, errors):
    updates = []
    for installation in ("system", "user"):
        flag = "--" + installation
        listing, err = run([flatpak, "remote-ls", flag, "--updates", "--columns=" + COLUMNS])
        if listing is None:
            # A user installation that was never used has no remotes.
            if err and "No remote" not in err and installation == "system":
                errors.append(err.splitlines()[-1])
            continue
        found = parse_remote_ls(listing, installation)
        if not found:
            continue
        installed, _ = run([flatpak, "list", flag, "--columns=ref,version"])
        versions = parse_list(installed or "")
        for item in found:
            item["old"] = versions.get(item["ref"], "")
        updates += found
    updates.sort(key=lambda u: (u["ref"].startswith("runtime/"), u["name"].lower()))
    return updates


def emit(updates, output, installed=True, errors=()):
    print(PROTOCOL, file=output)
    print("I\t" + ("1" if installed else "0"), file=output)
    if not installed:
        return
    for u in updates:
        fields = ["F", u["installation"], u["ref"], u["name"], u["application"], u["branch"],
                  u["origin"], u["old"], u["new"], u["size"], u["summary"]]
        print("\t".join(clean(f) for f in fields), file=output)
    for error in errors:
        print("E\t" + clean(error), file=output)


def main():
    flatpak = shutil.which("flatpak") or ""
    if not flatpak:
        emit([], sys.stdout, installed=False)
        return 0
    errors = []
    emit(collect(flatpak, errors), sys.stdout, True, errors)
    return 0


if __name__ == "__main__":
    sys.exit(main())
