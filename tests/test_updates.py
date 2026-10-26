# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for the Software Updater (sde-update) helpers in data/update (no APT, network or root).
import gzip
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "data/update"))
from check import collect, emit, clean, PROTOCOL  # noqa: E402
import check  # noqa: E402
import changelog  # noqa: E402
import history  # noqa: E402
import sources  # noqa: E402
import transaction  # noqa: E402


def package(
    name,
    source="example",
    security=False,
    hold=False,
    upgrade=True,
    installed=True,
    archive="testing",
    label=None,
    section="utils",
    priority="optional",
):
    candidate = NS(
        priority=priority,
        source_name=source,
        source_version="2",
        version="2",
        size=42,
        raw_description="Example",
        description="Example package\n with a long description",
        section=section,
        origins=[
            NS(
                origin="Debian",
                site="deb.debian.org",
                archive=archive,
                label=label or ("Debian-Security" if security else "Debian"),
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


class CheckTest(unittest.TestCase):
    def test_groups_binary_packages_by_source_and_totals_download(self):
        updates = collect([package("example-common"), package("example")], 1)
        self.assertEqual(len(updates), 1)
        self.assertEqual(updates[0].size, 84)
        self.assertEqual(updates[0].main_package_name, "example")
        self.assertEqual(set(updates[0].package_names), {"example", "example-common"})

    def test_excludes_held_current_and_uninstalled_packages(self):
        held = []
        self.assertEqual(
            collect(
                [
                    package("held", hold=True),
                    package("current", upgrade=False),
                    package("new", installed=False),
                ],
                1,
                held,
            ),
            [],
        )
        self.assertEqual(held, ["held"])

    def test_types_and_order(self):
        updates = collect(
            [
                package("firefox", "firefox"),
                package("backported", "backported", archive="trixie-backports"),
                package("z", "z", security=True),
                package("linux-image-amd64", "linux-signed-amd64", section="kernel"),
            ],
            1,
        )
        self.assertEqual(
            [(u.display_name, u.type) for u in updates],
            [
                ("z", "security"),
                ("linux-signed-amd64", "kernel"),
                ("firefox", "package"),
                ("backported", "backport"),
            ],
        )

    def test_protocol_records_and_control_characters(self):
        output = io.StringIO()
        held = ["pinned"]
        emit(collect([package("test"), package("test-doc")], 1), output, held)
        lines = output.getvalue().splitlines()
        self.assertEqual(lines[0], PROTOCOL)
        fields = lines[1].split("\t")
        self.assertEqual(len(fields), 19)
        self.assertEqual(fields[0], "U")
        self.assertEqual(fields[1], "package")
        self.assertEqual(fields[3], "test")
        self.assertEqual(fields[5:8], ["1", "2", "84"])
        self.assertEqual(fields[8], "test, test-doc")
        self.assertEqual(fields[9:11], ["debian", "testing"])
        self.assertEqual(fields[13], "Example package with a long description")
        self.assertEqual(fields[14:19], ["optional", "utils", "", "", ""])
        self.assertEqual(lines[2], "H\tpinned")
        self.assertEqual(clean("a\tb\nc"), "a b c")


class PriorityAndApplicationTest(unittest.TestCase):
    def test_group_keeps_the_strongest_priority(self):
        updates = collect(
            [package("libfoo1", "foo"), package("foo-bin", "foo", priority="important")], 1
        )
        self.assertEqual(updates[0].priority, "important")
        self.assertEqual(updates[0].section, "utils")

    def test_application_name_and_icon_from_the_desktop_file(self):
        with tempfile.TemporaryDirectory() as root:
            info = Path(root, "var/lib/dpkg/info")
            apps = Path(root, "usr/share/applications")
            info.mkdir(parents=True)
            apps.mkdir(parents=True)
            Path(info, "viewer.list").write_text(
                "/usr/bin/viewer\n/usr/share/applications/hidden.desktop\n"
                "/usr/share/applications/viewer.desktop\n"
            )
            Path(apps, "hidden.desktop").write_text(
                "[Desktop Entry]\nType=Application\nName=Hidden\nNoDisplay=true\n"
            )
            Path(apps, "viewer.desktop").write_text(
                "[Desktop Entry]\nType=Application\nName=Image Viewer\nIcon=viewer-icon\n"
            )
            old = check.ROOT
            check.ROOT = root
            try:
                update = collect([package("viewer", "viewer")], 1)[0]
                self.assertEqual(
                    check.application_of(update, str(info)), ("Image Viewer", "viewer-icon")
                )
                output = io.StringIO()
                emit([update], output, info_dir=str(info))
                fields = output.getvalue().splitlines()[1].split("\t")
                self.assertEqual(fields[17:19], ["Image Viewer", "viewer-icon"])
                other = collect([package("other", "other")], 1)[0]
                self.assertEqual(check.application_of(other, str(info)), ("", ""))
            finally:
                check.ROOT = old


HISTORY = """
Start-Date: 2026-09-17  02:20:17
Commandline: apt-get upgrade
Requested-By: alice (1000)
Upgrade: bash:amd64 (5.2.21-2, 5.2.21-3), libc6:amd64 (2.40-1, 2.40-2)
Install: libnew1:amd64 (1.0-1, automatic), tool:amd64 (3.1)
End-Date: 2026-09-17  02:20:40

Start-Date: 2026-09-18  10:00:00
Commandline: apt-get purge old
Purge: old:amd64 (0.9)
Remove: gone:i386 (1.2-3)
Error: Sub-process /usr/bin/dpkg returned an error code (1)
End-Date: 2026-09-18  10:00:05
"""


class HistoryTest(unittest.TestCase):
    def test_parse_transactions_and_changes(self):
        items = history.parse(HISTORY)
        self.assertEqual(len(items), 2)
        first = items[0]
        self.assertEqual(first["start"], "2026-09-17 02:20:17")
        self.assertEqual(first["requested_by"], "alice (1000)")
        changes = {(c["action"], c["package"]): c for c in first["changes"]}
        self.assertEqual(changes[("Upgrade", "bash")]["old"], "5.2.21-2")
        self.assertEqual(changes[("Upgrade", "bash")]["new"], "5.2.21-3")
        self.assertTrue(changes[("Install", "libnew1")]["automatic"])
        self.assertEqual(changes[("Install", "libnew1")]["new"], "1.0-1")
        self.assertFalse(changes[("Install", "tool")]["automatic"])
        self.assertEqual(items[1]["changes"][1]["arch"], "i386")
        self.assertEqual(items[1]["changes"][0]["old"], "0.9")
        self.assertIn("dpkg", items[1]["error"])

    def test_rotated_logs_are_merged_newest_first(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "history.log").write_text(HISTORY.split("\n\n")[1])
            with gzip.open(os.path.join(directory, "history.log.1.gz"), "wt") as handle:
                handle.write(HISTORY.split("\n\n")[0])
            items = history.collect(directory)
            self.assertEqual([t["start"][:10] for t in items], ["2026-09-18", "2026-09-17"])
            output = io.StringIO()
            history.emit(items, output)
            lines = output.getvalue().splitlines()
            self.assertEqual(lines[0], history.PROTOCOL)
            self.assertTrue(lines[1].startswith("T\t2026-09-18 10:00:00\t"))
            self.assertEqual(len(lines[2].split("\t")), 7)

    def test_missing_directory_is_empty(self):
        self.assertEqual(history.collect("/nonexistent-sde"), [])


ONE_LINE = """# comment
deb http://deb.debian.org/debian testing main contrib
deb [arch=amd64 signed-by=/usr/share/keyrings/x.gpg] https://example.org/apt stable main
# deb-src http://deb.debian.org/debian testing main
#  just a comment line
deb cdrom:[Debian]/ testing main
"""

DEB822 = """# Debian
Types: deb deb-src
URIs: http://deb.debian.org/debian
Suites: testing testing-updates
Components: main non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: https://repo.example.org
Suites: stable
Components: main
Enabled: no
Signed-By:
 -----BEGIN PGP PUBLIC KEY BLOCK-----
 abc
 -----END PGP PUBLIC KEY BLOCK-----
"""


class SourcesTest(unittest.TestCase):
    def test_one_line_format(self):
        entries = sources.parse_one_line(ONE_LINE, "sources.list")
        self.assertEqual(len(entries), 4)
        self.assertEqual(entries[0]["uris"], "http://deb.debian.org/debian")
        self.assertEqual(entries[0]["components"], "main contrib")
        self.assertEqual(entries[1]["signed_by"], "/usr/share/keyrings/x.gpg")
        self.assertFalse(entries[2]["enabled"])
        self.assertEqual(entries[2]["types"], "deb-src")
        self.assertEqual(entries[2]["line"], 4)

    def test_deb822_format(self):
        entries = sources.parse_deb822(DEB822, "debian.sources")
        self.assertEqual(len(entries), 2)
        self.assertEqual(entries[0]["types"], "deb deb-src")
        self.assertEqual(entries[0]["suites"], "testing testing-updates")
        self.assertTrue(entries[0]["enabled"])
        self.assertEqual(entries[0]["line"], 2)
        self.assertFalse(entries[1]["enabled"])
        self.assertEqual(entries[1]["signed_by"], "(embedded key)")

    def test_collect_tree(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "sources.list").write_text(ONE_LINE)
            Path(root, "sources.list.d").mkdir()
            Path(root, "sources.list.d", "debian.sources").write_text(DEB822)
            Path(root, "sources.list.d", "ignored.save").write_text(ONE_LINE)
            output = io.StringIO()
            sources.emit(sources.collect(root), output)
            lines = output.getvalue().splitlines()
            self.assertEqual(lines[0], sources.PROTOCOL)
            self.assertEqual(len(lines), 7)
            self.assertTrue(all(len(line.split("\t")) == 10 for line in lines[1:]))


class TransactionTest(unittest.TestCase):
    def test_commands_are_fixed_and_validated(self):
        self.assertEqual(
            transaction.command("packagekit", "refresh")[1:], ["-p", "refresh", "force"]
        )
        self.assertEqual(
            transaction.command("packagekit", "upgrade", ["bash", "libc6:amd64"])[-3:],
            ["update", "bash", "libc6:amd64"],
        )
        apt = transaction.command("apt", "upgrade", ["bash"])
        self.assertEqual(os.path.basename(apt[0]), "pkexec")
        self.assertEqual(apt[1], "/usr/bin/apt-get")
        self.assertEqual(apt[-3:], ["install", "--only-upgrade", "bash"])
        # Maintenance without a PackageKit equivalent goes through pkexec.
        self.assertEqual(transaction.command("packagekit", "clean")[1:], ["/usr/bin/apt-get", "clean"])
        self.assertIn("autoremove", transaction.command("packagekit", "autoremove"))
        self.assertIsNone(transaction.command("apt", "upgrade", ["bash; rm -rf /"]))
        self.assertIsNone(transaction.command("apt", "upgrade", ["-oAPT::X=1"]))
        self.assertIsNone(transaction.command("apt", "dist-upgrade"))

    def test_no_flatpak_mode(self):
        # Applications from Flathub and the Snap Store are updated in Merkadu.
        self.assertEqual(transaction.main(["--plan", "flatpak", "user:app/org.gimp.GIMP/x86_64/stable"]), 2)
        self.assertFalse(Path(transaction.__file__).with_name("flatpak.py").exists())

    def test_plan_rejects_names_for_other_actions(self):
        self.assertEqual(transaction.main(["clean", "bash"]), 2)
        self.assertEqual(transaction.main(["install", "bash"]), 2)

    def test_pkcon_plain_output(self):
        known = ["libgtk-3-0", "bash"]
        p = transaction.parse_pkcon
        self.assertEqual(p("Status:\tDownloading packages"), [("STATUS", "Downloading packages")])
        self.assertEqual(p("Percentage:\t42"), [("PERCENT", "42")])
        self.assertEqual(p("Percentage:\t101"), [("PERCENT", "100")])
        self.assertEqual(
            p("Downloading \tlibgtk-3-0-3.24.41-1.amd64 (debian-testing)\tGTK library", known),
            [("ITEM", "Downloading", "libgtk-3-0", "3.24.41-1")],
        )
        self.assertEqual(
            p("Updating    \tbash-5.2.21-3.amd64 (debian-testing)\tGNU shell", known),
            [("ITEM", "Updating", "bash", "5.2.21-3")],
        )
        self.assertEqual(p("Installing\tfoo-1.0-2.all\tFoo")[0][2], "foo")
        self.assertEqual(p("Fatal error: No network connection")[0][0], "ERROR")
        self.assertEqual(p(""), [])

    def test_apt_status_fd(self):
        a = transaction.parse_apt
        self.assertEqual(
            a("pmstatus:bash:45.5:Unpacking bash (amd64)"),
            [("PERCENT", "45"), ("STATUS", "Installing packages"), ("ITEM", "Unpacking", "bash", "")],
        )
        self.assertEqual(a("dlstatus:3:12.0:Retrieving file 3 of 20")[0], ("PERCENT", "12"))
        self.assertEqual(a("pmerror:foo:50:subprocess failed")[0][0], "ERROR")
        self.assertEqual(a("E: Could not get lock"), [("ERROR", "Could not get lock")])

    def test_outcomes(self):
        self.assertEqual(transaction.outcome("packagekit", 0, False), "ok")
        self.assertEqual(transaction.outcome("packagekit", 5, False), "nothing")
        self.assertEqual(transaction.outcome("apt", 126, False), "denied")
        self.assertEqual(transaction.outcome("apt", 100, False), "failed")
        self.assertEqual(transaction.outcome("packagekit", 1, True), "cancelled")

    def test_run_streams_protocol(self):
        output = io.StringIO()
        code = transaction.run(
            [sys.executable, "-c", "print('Get:1 http://x y'); print('E: boom')"], (), output
        )
        lines = output.getvalue().splitlines()
        self.assertEqual(code, 0)
        self.assertEqual(lines[0], "BACKEND\tapt")
        self.assertEqual(lines[1], "CANCELLABLE\t0")
        self.assertIn("ERROR\tboom", lines)
        self.assertEqual(lines[-1], "EXIT\t0\tok")


class ChangelogTest(unittest.TestCase):
    def test_names(self):
        self.assertTrue(changelog.valid_name("libc6:amd64"))
        self.assertFalse(changelog.valid_name("--help"))
        self.assertFalse(changelog.valid_name("a b"))
        # "$" in a re.match() pattern also matches before a final newline.
        for check in (changelog.valid_name, transaction.valid_name):
            self.assertFalse(check("bash\n"))

    def test_packagekit_detail(self):
        text = (
            "Package description\n"
            "  package:     bash-5.2.21-3.amd64\n"
            "  updates:     bash-5.2.21-2.amd64\n"
            "  changelog:   bash (5.2.21-3) unstable; urgency=medium\n"
            "  * Fix a bug.\n"
            "  restart:     none\n"
        )
        detail = changelog.from_packagekit_detail(text)
        self.assertIn("bash (5.2.21-3)", detail)
        self.assertIn("* Fix a bug.", detail)
        self.assertNotIn("restart", detail)

    def test_installed_changelog_and_limit(self):
        with tempfile.TemporaryDirectory() as doc:
            Path(doc, "demo").mkdir()
            with gzip.open(os.path.join(doc, "demo", "changelog.Debian.gz"), "wt") as handle:
                handle.write("demo (1.0) unstable\n" * 10)
            self.assertIn("demo (1.0)", changelog.installed_changelog("demo:amd64", doc))
        self.assertEqual(changelog.limit("a\nb\nc", 2), "a\nb\n…")


if __name__ == "__main__":
    unittest.main()
