#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise installed SDE suite launchers using harmless argument-recording backends."""

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bin-dir", type=Path, default=Path("/usr/bin"))
args = parser.parse_args()
binary = args.bin_dir.resolve()
with tempfile.TemporaryDirectory(prefix="sde-suite-test-") as temporary:
    root = Path(temporary)
    log = root / "arguments.json"
    env = {
        **os.environ,
        "PATH": str(root),
        "SDE_TEST_ARGS": str(log),
        "HOME": str(root),
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_CONFIG_DIRS": str(root / "system"),
    }
    for backend in [
        "pluma",
        "caja",
        "atril",
        "engrampa",
        "eom",
        "mate-system-monitor",
        "mate-power-preferences",
        "mate-power-manager",
        "mate-power-statistics",
        "sde-update",
    ]:
        path = root / backend
        path.write_text(
            '#!/usr/bin/python3\nimport json,os,sys\nwith open(os.environ["SDE_TEST_ARGS"],"w") as f: json.dump([sys.argv,os.getenv("GTK_THEME")],f)\n'
        )
        path.chmod(0o755)

    def call(*argv, **kwargs):
        return subprocess.run(
            [str(binary / argv[0]), *argv[1:]],
            env=env,
            check=True,
            capture_output=True,
            text=True,
            **kwargs
        )

    call("sde-suite", "--check")
    value = str(root / "a file;$(touch SHOULD-NOT-EXIST)")
    for alias, backend, prefix in [
        ("niny", "pluma", []),
        ("liria", "caja", ["--no-desktop"]),
        ("xanara", "atril", []),
        ("taca", "engrampa", []),
        ("sde-view", "eom", []),
    ]:
        call(alias, value, "file:///tmp/another%20file")
        actual, theme = json.loads(log.read_text())
        assert Path(actual[0]).name == backend, actual
        assert actual[1:] == prefix + [value, "file:///tmp/another%20file"], actual
        assert "https://github.com/" in call(alias, "--source").stdout
        call(alias, "--version")
    for option, backend in [
        (None, "mate-power-preferences"),
        ("--daemon", "mate-power-manager"),
        ("--statistics", "mate-power-statistics"),
    ]:
        call("sde-power-manager", *([option] if option else []))
        assert Path(json.loads(log.read_text())[0][0]).name == backend
    (root / "config/sde").mkdir(parents=True)
    (root / "config/sde/sde.conf").write_text("[appearance]\ntheme=dark\n")
    env.pop("GTK_THEME", None)
    call("niny", value)
    assert json.loads(log.read_text())[1] == "Adwaita:dark"
    env["GTK_THEME"] = "UserTheme"
    call("niny", value)
    assert json.loads(log.read_text())[1] == "UserTheme"
    assert not (root / "SHOULD-NOT-EXIST").exists()
    assert call("sde-update", "--plan", "refresh").stdout.splitlines() == [
        "/usr/bin/pkcon",
        "refresh",
        "force",
    ]
    assert call("sde-update", "--plan", "upgrade").stdout.splitlines() == [
        "/usr/bin/pkcon",
        "update",
    ]
    rejected = subprocess.run([str(binary / "sde-update"), "--plan", "bad"], env=env)
    assert rejected.returncode == 2
    (root / "pluma").unlink()
    missing = subprocess.run(
        [str(binary / "niny"), value], env=env, capture_output=True
    )
    assert missing.returncode == 127
print(
    "Suite routing, literal file arguments, backend errors and update action plans passed."
)
