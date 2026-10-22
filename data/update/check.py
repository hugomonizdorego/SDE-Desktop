#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Read-only APT metadata provider for the Software Updater (sde-update).
# PackageKit or pkexec own authenticated transactions.
#
# Protocol (tab separated, one record per line, UTF-8):
#   SDE-UPDATES  2
#   U  type  display_name  main_package  source  old  new  size  packages
#      origin  archive  site  summary  description
#      priority  section  urgency  app_name  icon        (since Alpha 51)
#   H  package          (held back by the administrator; never upgraded)
# type is security, kernel, backport or package. packages is comma separated.
# priority and section are the Debian control fields (the strongest priority
# of the group); urgency is the changelog urgency when known (else empty);
# app_name and icon come from a desktop file the installed package ships, so
# the window can show "Firefox ESR" with its icon instead of the package name.
# Readers must accept the 14-field records of earlier versions.
import configparser
import os
import sys

from model import Update

PROTOCOL = "SDE-UPDATES\t2"
TYPE_ORDER = {"security": 0, "kernel": 1, "package": 2, "backport": 3}
MAX_DESCRIPTION = 1500


def collect(cache, hold_state, held=None):
    groups = {}
    for package in cache:
        if not package.is_installed or not package.is_upgradable:
            continue
        if package._pkg.selected_state == hold_state:
            if held is not None:
                held.append(package.name)
            continue
        source = package.candidate.source_name
        if source in groups:
            groups[source].add_package(package)
        else:
            groups[source] = Update(package)
    return sorted(
        groups.values(),
        key=lambda item: (TYPE_ORDER.get(item.type, 2), item.display_name),
    )


def clean(value):
    return " ".join(str(value).split())


# SDE_UPDATE_ROOT points the desktop-file lookup at a fake root (tests, QA).
ROOT = os.environ.get("SDE_UPDATE_ROOT", "").rstrip("/")
DPKG_INFO = ROOT + "/var/lib/dpkg/info"
APPLICATIONS = "/usr/share/applications/"


def desktop_files(package, info_dir=DPKG_INFO):
    """Desktop files an installed package ships, from dpkg's file lists."""
    found = []
    for name in (package + ".list", package.split(":")[0] + ".list"):
        path = os.path.join(info_dir, name)
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    line = line.strip()
                    if line.startswith(APPLICATIONS) and line.endswith(".desktop"):
                        found.append(line)
        except OSError:
            continue
        if found:
            break
    return found


def read_desktop(path):
    """(Name, Icon) of a visible application desktop file, or None."""
    parser = configparser.RawConfigParser(strict=False, interpolation=None)
    parser.optionxform = str
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            parser.read_file(handle)
        entry = parser["Desktop Entry"]
    except (OSError, KeyError, configparser.Error):
        return None
    if entry.get("Type", "Application") != "Application":
        return None
    if entry.get("NoDisplay", "false").lower() == "true":
        return None
    name = entry.get("Name", "").strip()
    return (name, entry.get("Icon", "").strip()) if name else None


def application_of(update, info_dir=DPKG_INFO):
    """Name and icon of the application an update belongs to ("", "")."""
    packages = [update.main_package_name] + [
        p for p in sorted(update.package_names) if p != update.main_package_name
    ]
    for package in packages[:8]:
        for path in desktop_files(package, info_dir):
            app = read_desktop(ROOT + path)
            if app:
                return app
    return ("", "")


def record(update, info_dir=DPKG_INFO):
    app_name, icon = application_of(update, info_dir)
    description = clean(update.description or "")
    if len(description) > MAX_DESCRIPTION:
        description = description[:MAX_DESCRIPTION].rstrip() + "…"
    return [
        "U",
        update.type,
        update.display_name,
        update.main_package_name,
        update.real_source_name,
        update.old_version,
        update.new_version,
        update.size,
        ", ".join(sorted(update.package_names)),
        update.origin,
        update.archive,
        update.site,
        update.short_description or "",
        description,
        getattr(update, "priority", ""),
        getattr(update, "section", ""),
        getattr(update, "urgency", ""),
        app_name,
        icon,
    ]


def emit(updates, output, held=(), info_dir=DPKG_INFO):
    print(PROTOCOL, file=output)
    for update in updates:
        print("\t".join(map(clean, record(update, info_dir))), file=output)
    for name in sorted(held):
        print("H\t" + clean(name), file=output)


def main():
    try:
        import apt
        import apt_pkg

        held = []
        updates = collect(apt.Cache(), apt_pkg.SELSTATE_HOLD, held)
        emit(updates, sys.stdout, held)
        return 0
    except Exception as error:
        # Stdout is a versioned protocol; errors must not look like zero updates.
        print("Software Updater: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
