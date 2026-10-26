#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
#
# Every SDE program names the Selviana DE Project as its developer: in its
# desktop file (X-Developer) and in its AppStream metainfo (<developer> with
# the project's developer id). Also checks the metainfo files for the fields
# AppStream requires, a stock icon that matches the desktop file, and no
# leftover "DebION Dev-Team"; when appstreamcli is installed it validates
# them too. Licenses are checked by test_legal_data.py.
import glob
import json
import os
import shutil
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = json.load(open(os.path.join(ROOT, "data/legal/components.json"), encoding="utf-8"))["project"]
DEV = PROJECT["developer"]          # "Selviana DE Project"
DEV_ID = PROJECT["developer_id"]    # reverse-DNS id of the namespace the project controls
OLD_DEV = "DebION Dev-Team"
# Files that may lag behind while a rename is under way (none at present:
# the Control Center's two files were brought up to date with its rename).
# A file listed here that is correct makes the test fail, so an exception
# cannot outlive its reason.
PENDING = set()
errors, pending_seen = [], []


def problem(name, message):
    if name in PENDING:
        pending_seen.append(f"{name}: {message}")
    else:
        errors.append(f"{name}: {message}")


desktops = {}
for path in sorted(glob.glob(os.path.join(ROOT, "data/applications/*.desktop"))):
    name = os.path.basename(path)
    entry, section = {}, None
    text = open(path, encoding="utf-8").read()
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("["):
            section = line
        elif section == "[Desktop Entry]" and "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            entry[k] = v
    desktops[name] = entry
    before = len(errors) + len(pending_seen)
    if entry.get("X-Developer") != DEV:
        problem(name, f"X-Developer must be '{DEV}'")
    if "X-DebION-Developer" in entry or OLD_DEV in text:
        problem(name, f"still names '{OLD_DEV}'")
    if name in PENDING and len(errors) + len(pending_seen) == before:
        errors.append(f"{name} is correct now: remove it from PENDING in tests/test_metainfo.py")

launchables = set()
ids = set()
for path in sorted(glob.glob(os.path.join(ROOT, "data/metainfo/*.metainfo.xml"))):
    name = os.path.basename(path)
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        errors.append(f"{name}: {e}")
        continue
    before = len(errors) + len(pending_seen)
    cid = root.findtext("id")
    if not cid or name != cid + ".metainfo.xml":
        errors.append(f"{name}: file name must be <id>.metainfo.xml")
    ids.add(cid)
    for tag in ("metadata_license", "project_license", "name", "summary", "description",
                "content_rating", "url"):
        if root.find(tag) is None:
            errors.append(f"{name}: missing <{tag}>")
    dev = root.find("developer")
    if dev is None or dev.get("id") != DEV_ID or dev.findtext("name") != DEV:
        problem(name, f"<developer id=\"{DEV_ID}\"><name>{DEV}</name> missing")
    if OLD_DEV in open(path, encoding="utf-8").read():
        problem(name, f"still names '{OLD_DEV}'")
    icon = root.find("icon")
    if icon is None or icon.get("type") != "stock" or not (icon.text or "").strip():
        problem(name, "<icon type=\"stock\"> missing")
    if root.get("type") == "desktop-application":
        l = root.find("launchable")
        if l is None or l.get("type") != "desktop-id":
            errors.append(f"{name}: desktop applications need a desktop-id launchable")
        elif l.text not in desktops:
            errors.append(f"{name}: launchable {l.text} is not in data/applications")
        else:
            launchables.add(l.text)
            want = desktops[l.text].get("Icon")
            if icon is not None and want and (icon.text or "").strip() != want:
                problem(name, f"stock icon {icon.text} differs from Icon={want} of {l.text}")
    if name in PENDING and len(errors) + len(pending_seen) == before:
        errors.append(f"{name} is correct now: remove it from PENDING in tests/test_metainfo.py")

for d in desktops:
    if desktops[d].get("NoDisplay") == "true":
        continue
    if d not in launchables:
        errors.append(f"{d}: no metainfo file in data/metainfo")
if "org.debion.SDE" not in ids:
    errors.append("org.debion.SDE.metainfo.xml (the desktop session) is missing")

tool = shutil.which("appstreamcli")
if tool:
    files = sorted(glob.glob(os.path.join(ROOT, "data/metainfo/*.metainfo.xml")))
    r = subprocess.run([tool, "validate", "--no-net", "--pedantic"] + files,
                       capture_output=True, text=True)
    if r.returncode != 0:
        errors.append("appstreamcli validate failed:\n" + r.stdout + r.stderr)

for p in pending_seen:
    print("PENDING (Control Center rename):", p)
for e in errors:
    print("FAIL:", e)
print(f"{len(desktops)} desktop files, {len(ids)} metainfo files, {len(errors)} problems")


class MetainfoTest(unittest.TestCase):
    """The checks above run when the file is imported; this makes them a
    test for `python3 -m unittest`, next to the other test files."""

    def test_no_problems(self):
        self.assertEqual(errors, [])


if __name__ == '__main__':
    sys.exit(1 if errors else 0)
