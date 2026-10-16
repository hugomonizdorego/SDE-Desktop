#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the SDE Debian package (.deb) from this source tree.
#
#   tools/build-deb.sh [output-dir]
#
# The package lands in output-dir (default: ./dist). Install it with
#   sudo apt install ./dist/sde_*.deb
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/dist}
VERSION=$(cat "$ROOT/VERSION")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

command -v dpkg-buildpackage >/dev/null 2>&1 || {
    echo "build-deb.sh: dpkg-buildpackage not found; run sudo tools/install-deps.sh" >&2
    exit 1
}

echo "Building sde $VERSION"
# Build in a clean copy so the source tree stays untouched.
mkdir -p "$WORK/sde-$VERSION"
tar -C "$ROOT" --exclude=.git --exclude=dist --exclude=build --exclude=obj-* -cf - . |
    tar -C "$WORK/sde-$VERSION" -xf -
(cd "$WORK/sde-$VERSION" && dpkg-buildpackage -us -uc -b)

mkdir -p "$OUT"
cp "$WORK"/*.deb "$OUT"/
cp "$WORK"/*.buildinfo "$WORK"/*.changes "$OUT"/ 2>/dev/null || true
echo
echo "Packages written to $OUT:"
ls -1 "$OUT"/*.deb
if command -v lintian >/dev/null 2>&1; then
    echo
    echo "lintian:"
    lintian --tag-display-limit 0 "$OUT"/*.deb || true
fi
