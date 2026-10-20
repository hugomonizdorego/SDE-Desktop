#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Read-only APT history provider for SDE Update (/var/log/apt/history.log*).
#
# Protocol (tab separated):
#   SDE-HISTORY  1
#   T  start  end  commandline  requested_by  error     (one transaction)
#   C  action  package  arch  old  new  automatic       (its package changes)
# Transactions are printed newest first. action is Install, Upgrade, Remove,
# Purge, Downgrade or Reinstall; old/new are empty when they do not apply.
import glob
import gzip
import os
import re
import sys

PROTOCOL = "SDE-HISTORY\t1"
ACTIONS = ("Install", "Upgrade", "Remove", "Purge", "Downgrade", "Reinstall")
ENTRY = re.compile(r"([^\s,()]+) \(([^)]*)\)")
MAX_TRANSACTIONS = 400


def clean(value):
    return " ".join(str(value).split())


def parse_changes(action, text):
    changes = []
    for name, versions in ENTRY.findall(text):
        package, _, arch = name.partition(":")
        parts = [v.strip() for v in versions.split(",")]
        automatic = "automatic" in parts
        parts = [v for v in parts if v and v != "automatic"]
        old = new = ""
        if action in ("Upgrade", "Downgrade") and len(parts) >= 2:
            old, new = parts[0], parts[1]
        elif action in ("Remove", "Purge"):
            old = parts[0] if parts else ""
        elif parts:
            new = parts[0]
        changes.append(
            {
                "action": action,
                "package": package,
                "arch": arch,
                "old": old,
                "new": new,
                "automatic": automatic,
            }
        )
    return changes


def parse(text):
    """Parses the text of one history.log file into transactions."""
    transactions = []
    current = None
    for line in text.splitlines():
        if not line.strip():
            continue
        key, sep, value = line.partition(": ")
        if not sep:
            continue
        value = value.strip()
        if key == "Start-Date":
            current = {
                "start": clean(value),
                "end": "",
                "commandline": "",
                "requested_by": "",
                "error": "",
                "changes": [],
            }
            transactions.append(current)
        elif current is None:
            continue
        elif key == "End-Date":
            current["end"] = clean(value)
        elif key == "Commandline":
            current["commandline"] = value
        elif key == "Requested-By":
            current["requested_by"] = value
        elif key == "Error":
            current["error"] = value
        elif key in ACTIONS:
            current["changes"].extend(parse_changes(key, value))
    return transactions


def read_log(path):
    try:
        if path.endswith(".gz"):
            with gzip.open(path, "rt", encoding="utf-8", errors="replace") as handle:
                return handle.read()
        with open(path, encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def log_files(directory):
    def rotation(path):
        match = re.search(r"history\.log\.(\d+)", path)
        return int(match.group(1)) if match else 0

    files = [os.path.join(directory, "history.log")]
    files += glob.glob(os.path.join(directory, "history.log.*"))
    return sorted({f for f in files if os.path.isfile(f)}, key=rotation)


def collect(directory="/var/log/apt", limit=MAX_TRANSACTIONS):
    transactions = []
    for path in log_files(directory):
        transactions.extend(parse(read_log(path)))
        if len(transactions) >= limit * 2:
            break
    transactions.sort(key=lambda t: t["start"], reverse=True)
    return transactions[:limit]


def emit(transactions, output):
    print(PROTOCOL, file=output)
    for t in transactions:
        fields = ["T", t["start"], t["end"], t["commandline"], t["requested_by"], t["error"]]
        print("\t".join(map(clean, fields)), file=output)
        for c in t["changes"]:
            fields = [
                "C",
                c["action"],
                c["package"],
                c["arch"],
                c["old"],
                c["new"],
                "1" if c["automatic"] else "0",
            ]
            print("\t".join(map(clean, fields)), file=output)


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "/var/log/apt"
    emit(collect(directory), sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
