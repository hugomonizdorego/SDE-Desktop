#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DebION Dev-Team
#
# Every SDE program names the DebION Dev-Team as its developer: in its
# desktop file (X-Developer) and in its AppStream metainfo. Also checks the
# metainfo files for the fields AppStream requires; when appstreamcli is
# installed it validates them too.
import glob
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEV = "DebION Dev-Team"
# AppStream developer names accepted besides DEV: the desktop environment's
# own project (the Control Center since 0.2).
DEVELOPERS = {DEV, "Selviana DE Project"}
errors = []

desktops = {}
for path in sorted(glob.glob(os.path.join(ROOT, "data/applications/*.desktop"))):
    name = os.path.basename(path)
    entry, section = {}, None
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line.startswith("["):
            section = line
        elif section == "[Desktop Entry]" and "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            entry[k] = v
    desktops[name] = entry
    if entry.get("X-Developer") != DEV:
        errors.append(f"{name}: X-Developer must be '{DEV}'")

launchables = set()
ids = set()
for path in sorted(glob.glob(os.path.join(ROOT, "data/metainfo/*.metainfo.xml"))):
    name = os.path.basename(path)
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        errors.append(f"{name}: {e}")
        continue
    cid = root.findtext("id")
    if not cid or name != cid + ".metainfo.xml":
        errors.append(f"{name}: file name must be <id>.metainfo.xml")
    ids.add(cid)
    for tag in ("metadata_license", "project_license", "name", "summary", "description",
                "content_rating", "url"):
        if root.find(tag) is None:
            errors.append(f"{name}: missing <{tag}>")
    dev = root.find("developer")
    if dev is None or dev.get("id") != "org.debion" or dev.findtext("name") not in DEVELOPERS:
        errors.append(f"{name}: <developer id=\"org.debion\"><name>{DEV}</name> missing")
    if root.get("type") == "desktop-application":
        l = root.find("launchable")
        if l is None or l.get("type") != "desktop-id":
            errors.append(f"{name}: desktop applications need a desktop-id launchable")
        elif l.text not in desktops:
            errors.append(f"{name}: launchable {l.text} is not in data/applications")
        else:
            launchables.add(l.text)

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

for e in errors:
    print("FAIL:", e)
print(f"{len(desktops)} desktop files, {len(ids)} metainfo files, {len(errors)} problems")
sys.exit(1 if errors else 0)
