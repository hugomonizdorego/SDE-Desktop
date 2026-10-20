#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the SDE Debian package (.deb) from this source tree.
#
#   tools/build-deb.sh [--suffix SUFFIX] [output-dir]
#
# --suffix adds a distribution tag to the package version, e.g. "deb12"
# gives 0.1.0~alpha2+deb12 (used for the per-distribution release builds).
#
# The package lands in output-dir (default: ./dist). Install it with
#   sudo sh ./dist/install-debion.sh ./dist
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SUFFIX=""
if [ "${1:-}" = "--suffix" ]; then
    SUFFIX=$2
    shift 2
fi
OUT=${1:-$ROOT/dist}
VERSION=$(cat "$ROOT/VERSION")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

command -v dpkg-buildpackage >/dev/null 2>&1 || {
    echo "build-deb.sh: dpkg-buildpackage not found; run sudo tools/install-deps.sh" >&2
    exit 1
}

echo "Building sde $VERSION with native application forks"
# Download/verify sources before dpkg-buildpackage. Native compilation itself
# has no network access and never installs upstream application packages.
python3 "$ROOT/tools/prepare-native-suite.py"
python3 "$ROOT/tools/prepare-components.py"
python3 "$ROOT/tools/prepare-gallery.py"

# Build in a clean copy so the source tree stays untouched.
mkdir -p "$WORK/sde-$VERSION"
tar -C "$ROOT" --exclude=.git --exclude=dist --exclude=build --exclude=target --exclude=obj-* --exclude=source-work --exclude=__pycache__ -cf - . |
    tar -C "$WORK/sde-$VERSION" -xf -
if [ -n "$SUFFIX" ]; then
    # Prepend a changelog entry carrying the distribution suffix.
    (cd "$WORK/sde-$VERSION" &&
        { printf 'sde (%s+%s) unstable; urgency=medium\n\n  * Build for %s.\n\n -- %s  %s\n\n' \
              "$VERSION" "$SUFFIX" "$SUFFIX" \
              "$(sed -n 's/^ -- \(.*>\).*/\1/p' debian/changelog | head -n1)" \
              "$(date -R)"; cat debian/changelog; } > debian/changelog.new &&
        mv debian/changelog.new debian/changelog)
fi
(cd "$WORK/sde-$VERSION" && dpkg-buildpackage -us -uc -b)

mkdir -p "$OUT"
# Preserve non-installed integration fixtures for the CI GUI checks only.
mkdir -p "$OUT/test-tools"
find "$WORK/sde-$VERSION" -type f \( -name sde-menu-probe -o -name sde-panel-service-fixture -o -name sde-sni-demo \)     -exec cp '{}' "$OUT/test-tools/" \;
# Publish complete corresponding sources beside the binaries, including all
# patched upstream trees, submodules, copyright notices and build scripts.
tar -C "$WORK" --exclude='*/obj-*' --exclude='*/build' --exclude='*/target' \
    --exclude='*/__pycache__' --exclude='*/debian/.debhelper' --exclude='*/debian/sde' \
    --exclude='*/debian/debion-desktop-defaults' --exclude='*/debian/sde-desktop' --exclude='*/debian/sde-dbgsym' --exclude='*/release/*.deb' -cJf "$OUT/sde-native-sources_${VERSION}${SUFFIX:+-$SUFFIX}.tar.xz" "sde-$VERSION"
cp "$WORK"/*.deb "$OUT"/
cp "$WORK"/*.buildinfo "$WORK"/*.changes "$OUT"/ 2>/dev/null || true
cp "$ROOT/tools/install-debion.sh" "$OUT/install-debion.sh"
(cd "$OUT" && sha256sum ./*.deb ./*sources*.tar.xz ./install-debion.sh > SHA256SUMS.sha256)
echo
echo "Packages written to $OUT:"
ls -1 "$OUT"/*.deb
if command -v lintian >/dev/null 2>&1; then
    echo
    echo "lintian:"
    lintian --tag-display-limit 0 "$OUT"/*.deb || true
fi
