#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check package MIME defaults and SDE-only autostarts without launching services."""

import configparser
import os
import subprocess
import tempfile
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="sde-installed-") as temporary:
    root = Path(temporary)
    env = {
        **os.environ,
        "HOME": str(root),
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_CONFIG_DIRS": "/etc/xdg",
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_DATA_DIRS": "/usr/share",
        "XDG_CURRENT_DESKTOP": "SDE",
    }
    assert any(
        os.access("/usr/bin/" + client, os.X_OK) for client in ["pkgcli", "pkcon"]
    ), "PackageKit client missing"
    expected = {
        "text/plain": "niny.desktop",
        "inode/directory": "liria.desktop",
        "application/pdf": "xanara.desktop",
        "application/zip": "taca.desktop",
        "image/png": "sde-view.desktop",
    }
    for mime, application in expected.items():
        actual = subprocess.check_output(
            ["xdg-mime", "query", "default", mime], env=env, text=True
        ).strip()
        assert actual == application, (mime, actual, application)
    applications = root / "data/applications"
    applications.mkdir(parents=True)
    (applications / "user-editor.desktop").write_text("[Desktop Entry]\nType=Application\nName=User Editor\nExec=niny %U\nMimeType=text/plain;\n")
    config = root / "config"
    config.mkdir(exist_ok=True)
    (config / "mimeapps.list").write_text(
        "[Default Applications]\ntext/plain=user-editor.desktop;\n"
    )
    actual = subprocess.check_output(
        ["xdg-mime", "query", "default", "text/plain"], env=env, text=True
    ).strip()
    assert actual == "user-editor.desktop", actual
    dry = subprocess.check_output(["sde-autostart", "--dry-run"], env=env, text=True)
    assert any(
        line.startswith("start sde-power.desktop:") and "sde-power --daemon" in line
        for line in dry.splitlines()
    ), dry
    assert any(
        line.startswith("start sde-lock.desktop:") and "sde-lock --daemon" in line
        for line in dry.splitlines()
    ), dry
    for name in ["sde-power", "sde-lock"]:
        parser = configparser.ConfigParser(interpolation=None)
        parser.read("/etc/xdg/autostart/" + name + ".desktop")
        assert parser["Desktop Entry"]["OnlyShowIn"] == "SDE;"
    for package in [
        "sde/upstream/caja",
        "sde",
    ]:
        assert Path("/usr/share/doc", package, "copyright").is_file(), package
    pam = Path("/etc/pam.d/sde-lock").read_text()
    assert "@include common-auth" in pam and "@include common-account" in pam, pam
    # The retired fork's PAM service and autostarts are gone.
    for old in ["/etc/pam.d/sde-screensaver", "/etc/xdg/autostart/sde-screensaver.desktop",
                "/etc/xdg/autostart/sde-power-manager.desktop"]:
        assert not Path(old).exists(), old
    lock_help = subprocess.check_output(["sde-lock", "--help"], text=True)
    assert "--daemon" in lock_help, lock_help
    status = subprocess.run(["sde-power", "--brightness", "bogus"], capture_output=True)
    assert status.returncode == 2, status
    idle = subprocess.check_output(
        ["gsettings", "get", "org.debion.session", "idle-delay"],
        env={**env, "GSETTINGS_BACKEND": "memory"}, text=True
    ).strip()
    assert idle == "5", idle
    schemas = set(subprocess.check_output(
        ["gsettings", "list-schemas"], env=env, text=True
    ).splitlines())
    required = {"org.mate.lockdown", "org.mate.interface", "org.mate.background",
                "org.debion.media-handling", "org.debion.applications-terminal"}
    assert required <= schemas, required - schemas
print(
    "Installed MIME defaults, user overrides, dry-run autostarts and copyright files passed."
)
