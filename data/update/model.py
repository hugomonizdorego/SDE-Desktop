# SPDX-License-Identifier: GPL-3.0-or-later
# Adapted from Linux Mint mintupdate Classes.py, Update class.
# Copyright (C) 2007-2015 Clement Lefebvre <root@linuxmint.com>
# Copyright (C) 2026 SDE contributors (Debian adaptation).
# Source: https://github.com/linuxmint/mintupdate
# Revision: b4c586f9839d2943944390f4117a10acd4ba865a
# Changes: standalone model; remove Mint settings/GI and Mint/browser heuristics.
# Full upstream copyright and license are in COPYRIGHT and COPYING.
SOURCE_PACKAGE_NAME_OVERRIDES = ["linux-libc-dev"]


class Update:

    def __init__(self, package=None, source_name=None):
        self.package_names = []
        if package is not None:
            self.package_names.append(package.name)
            self.source_packages = {
                "%s=%s"
                % (package.candidate.source_name, package.candidate.source_version)
            }
            self.main_package_name = package.name
            self.package_name = package.name
            self.new_version = package.candidate.version
            if package.installed is None:
                self.old_version = ""
            else:
                self.old_version = package.installed.version
            self.size = package.candidate.size
            self.real_source_name = package.candidate.source_name
            if source_name is not None:
                self.source_name = source_name
            else:
                self.source_name = self.real_source_name

            if self.main_package_name in SOURCE_PACKAGE_NAME_OVERRIDES:
                self.display_name = self.package_name
            else:
                self.display_name = self.source_name

            self.short_description = package.candidate.raw_description
            self.description = package.candidate.description
            self.archive = ""

            self.type = "package"
            self.origin = ""
            for origin in package.candidate.origins:
                self.origin = origin.origin
                self.site = origin.site
                self.archive = origin.archive
                if origin.origin == "Ubuntu":
                    self.origin = "ubuntu"
                elif origin.origin == "Debian":
                    self.origin = "debian"
                elif origin.origin.startswith("LP-PPA"):
                    self.origin = origin.origin
                if origin.origin == "Ubuntu" and "-security" in origin.archive:
                    self.type = "security"
                    break
                if origin.origin == "Debian" and "-Security" in origin.label:
                    self.type = "security"
                    break
            if (
                package.candidate.section == "kernel"
                or self.package_name.startswith("linux-headers")
                or self.real_source_name
                in ["linux", "linux-kernel", "linux-signed", "linux-meta"]
            ):
                self.type = "kernel"

    def add_package(self, pkg):
        if self.main_package_name in SOURCE_PACKAGE_NAME_OVERRIDES:
            self.display_name = self.source_name

        self.package_names.append(pkg.name)
        self.source_packages.add(
            "%s=%s" % (pkg.candidate.source_name, pkg.candidate.source_version)
        )
        self.size += pkg.candidate.size
        if self.main_package_name is None or pkg.name == self.source_name:
            self.overwrite_main_package(pkg)
            return

        if self.main_package_name != self.source_name:
            # Overwrite dev, dbg, common, arch packages
            for suffix in [
                "-dev",
                "-dbg",
                "-common",
                "-core",
                "-data",
                "-doc",
                ":i386",
                ":amd64",
            ]:
                if self.main_package_name.endswith(suffix) and not pkg.name.endswith(
                    suffix
                ):
                    self.overwrite_main_package(pkg)
                    return
            # Overwrite lib packages
            for prefix in ["lib", "gir1.2"]:
                if self.main_package_name.startswith(
                    prefix
                ) and not pkg.name.startswith(prefix):
                    self.overwrite_main_package(pkg)
                    return
            for keyword in ["-locale-", "-l10n-", "-help-"]:
                if (keyword in self.main_package_name) and (keyword not in pkg.name):
                    self.overwrite_main_package(pkg)
                    return

    def overwrite_main_package(self, pkg):
        self.description = pkg.candidate.description
        self.short_description = pkg.candidate.raw_description
        self.main_package_name = pkg.name
