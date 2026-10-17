#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare exact upstream source checkouts without changing copyright or licenses."""

import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(*args, cwd):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "apps", nargs="*", help="Upstream IDs, e.g. pluma caja mintupdate"
    )
    parser.add_argument("--output", type=Path, default=ROOT / "source-work")
    parser.add_argument(
        "--references",
        action="store_true",
        help="Include LXQt/Budgie/COSMIC references",
    )
    parser.add_argument(
        "--verify-only", action="store_true", help="Verify existing clean checkouts"
    )
    args = parser.parse_args()
    records = json.loads((ROOT / "upstream/sources.lock.json").read_text())["sources"]
    unknown = set(args.apps) - {row["id"] for row in records}
    if unknown:
        parser.error("Unknown sources: " + ", ".join(sorted(unknown)))
    selected = (
        [row for row in records if row["id"] in args.apps]
        if args.apps
        else [
            row for row in records if args.references or row["use"] != "reference-only"
        ]
    )
    args.output.mkdir(parents=True, exist_ok=True)
    for row in selected:
        if not re.fullmatch(r"[a-z0-9-]+", row["id"]) or not re.fullmatch(
            r"[0-9a-f]{40}", row["commit"]
        ):
            raise ValueError("Invalid source lock record")
        if not re.fullmatch(
            r"https://github.com/[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+", row["repository"]
        ):
            raise ValueError("Source must be a public GitHub repository")
        destination = args.output / row["id"]
        if not destination.exists():
            if args.verify_only:
                raise ValueError("Missing checkout: " + str(destination))
            with tempfile.TemporaryDirectory(
                prefix="sde-source-", dir=args.output
            ) as temporary:
                checkout = Path(temporary) / "checkout"
                checkout.mkdir()
                run("git", "init", "--quiet", cwd=checkout)
                run(
                    "git",
                    "fetch",
                    "--quiet",
                    "--depth",
                    "1",
                    row["repository"] + ".git",
                    row["commit"],
                    cwd=checkout,
                )
                run(
                    "git", "checkout", "--quiet", "--detach", "FETCH_HEAD", cwd=checkout
                )
                checkout.rename(destination)
        actual = run("git", "rev-parse", "HEAD", cwd=destination)
        if actual != row["commit"] or run(
            "git", "status", "--porcelain", cwd=destination
        ):
            raise ValueError(
                "Source differs from lock; preserve your edits and use another output directory: "
                + str(destination)
            )
        # Keep the entire tree: AUTHORS, file headers, COPYING, library licenses,
        # translations, trademark notices and build metadata, not just a label.
        if row["use"] != "reference-only" and not any(
            (destination / name).exists()
            for name in ["COPYING", "LICENSE", "debian/copyright"]
        ):
            raise ValueError("No upstream license/copyright found: " + row["id"])
        print(row["id"], actual, destination, flush=True)


if __name__ == "__main__":
    main()
