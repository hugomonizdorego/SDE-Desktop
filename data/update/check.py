#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Read-only APT metadata provider. PackageKit owns authenticated transactions.
#
# Protocol (tab separated, one record per line, UTF-8):
#   SDE-UPDATES  2
#   U  type  display_name  main_package  source  old  new  size  packages
#      origin  archive  site  summary  description
#   H  package          (held back by the administrator; never upgraded)
# type is security, kernel, backport or package. packages is comma separated.
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


def record(update):
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
    ]


def emit(updates, output, held=()):
    print(PROTOCOL, file=output)
    for update in updates:
        print("\t".join(map(clean, record(update))), file=output)
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
        print("SDE Update: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
