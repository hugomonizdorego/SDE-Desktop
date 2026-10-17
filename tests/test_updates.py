# SPDX-License-Identifier: GPL-3.0-or-later
import io
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "data/update"))
from check import collect, emit, clean


def package(
    name, source="example", security=False, hold=False, upgrade=True, installed=True
):
    candidate = NS(
        source_name=source,
        source_version="2",
        version="2",
        size=42,
        raw_description="Example",
        description="Example package",
        section="utils",
        origins=[
            NS(
                origin="Debian",
                site="deb.debian.org",
                archive="testing",
                label="Debian-Security" if security else "Debian",
                component="main",
            )
        ],
    )
    return NS(
        name=name,
        candidate=candidate,
        installed=NS(version="1") if installed else None,
        is_installed=installed,
        is_upgradable=upgrade,
        _pkg=NS(selected_state=1 if hold else 0),
    )


class UpdatesTest(unittest.TestCase):
    def test_groups_binary_packages_by_source_and_totals_download(self):
        updates = collect([package("example-common"), package("example")], 1)
        self.assertEqual(len(updates), 1)
        self.assertEqual(updates[0].size, 84)
        self.assertEqual(updates[0].main_package_name, "example")
        self.assertEqual(set(updates[0].package_names), {"example", "example-common"})

    def test_excludes_held_current_and_uninstalled_packages(self):
        self.assertEqual(
            collect(
                [
                    package("held", hold=True),
                    package("current", upgrade=False),
                    package("new", installed=False),
                ],
                1,
            ),
            [],
        )

    def test_only_security_origin_marks_security(self):
        updates = collect(
            [package("z", "z", security=True), package("firefox", "firefox")], 1
        )
        self.assertEqual([item.type for item in updates], ["security", "package"])

    def test_protocol_and_control_characters(self):
        output = io.StringIO()
        emit(collect([package("test")], 1), output)
        lines = output.getvalue().splitlines()
        self.assertEqual(lines[0], "SDE-UPDATES\t1")
        self.assertEqual(len(lines[1].split("\t")), 6)
        self.assertEqual(clean("a\tb\nc"), "a b c")


if __name__ == "__main__":
    unittest.main()
