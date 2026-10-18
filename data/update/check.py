#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Read-only APT metadata provider. PackageKit owns authenticated transactions.
import sys

from model import Update


def collect(cache, hold_state):
    groups = {}
    for package in cache:
        if not package.is_installed or not package.is_upgradable:
            continue
        if package._pkg.selected_state == hold_state:
            continue
        source = package.candidate.source_name
        if source in groups:
            groups[source].add_package(package)
        else:
            groups[source] = Update(package)
    return sorted(
        groups.values(), key=lambda item: (item.type != "security", item.display_name)
    )


def clean(value):
    return " ".join(str(value).split())


def emit(updates, output):
    print("SDE-UPDATES\t1", file=output)
    for update in updates:
        print(
            "\t".join(
                map(
                    clean,
                    [
                        update.type,
                        update.display_name,
                        update.old_version,
                        update.new_version,
                        update.size,
                        ", ".join(update.package_names),
                    ],
                )
            ),
            file=output,
        )


def main():
    try:
        import apt
        import apt_pkg

        emit(collect(apt.Cache(), apt_pkg.SELSTATE_HOLD), sys.stdout)
        return 0
    except Exception as error:
        # Stdout is a versioned protocol; errors must not look like zero updates.
        print("SDE Update: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
