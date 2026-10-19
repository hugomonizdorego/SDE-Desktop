#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Install the complete DebION SDE alpha profile from one artifact directory.
# sudo sh ./install-debion.sh [--yes] [directory]
set -eu
yes_option=
if [ "${1:-}" = --yes ]; then yes_option=-y; shift; fi
[ "$#" -le 1 ] || { echo 'Usage: install-debion.sh [--yes] [directory]' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo.' >&2; exit 1; }
. /etc/os-release
case "${ID:-} ${ID_LIKE:-}" in
    *debian*|*debion*|*DebION*) ;;
    *) echo 'This build targets DebION / Debian testing.' >&2; exit 1 ;;
esac
artifacts=$(cd "${1:-.}" && pwd)
# Require an unambiguous matched set before changing any package configuration.
set -- "$artifacts"/debion-desktop-defaults_*.deb
[ "$#" -eq 1 ] && [ -f "$1" ] || { echo 'Exactly one defaults .deb is required.' >&2; exit 1; }
defaults=$1
set -- "$artifacts"/sde_*.deb
[ "$#" -eq 1 ] && [ -f "$1" ] || { echo 'Exactly one sde .deb is required.' >&2; exit 1; }
core=$1
set -- "$artifacts"/sde-desktop_*.deb
[ "$#" -eq 1 ] && [ -f "$1" ] || { echo 'Exactly one sde-desktop .deb is required.' >&2; exit 1; }
desktop=$1
version=$(dpkg-deb -f "$core" Version)
for package in "$defaults" "$desktop"; do
    [ "$(dpkg-deb -f "$package" Version)" = "$version" ] || {
        echo 'All three packages must have the same version.' >&2; exit 1;
    }
done
# Bootstrap the signed Mozilla source before asking APT to resolve firefox.
# No apt/dpkg/network subprocess is run from a package maintainer script.
apt-get update
apt-get install $yes_option --no-install-recommends "$defaults"
apt-get update
apt-cache show firefox >/dev/null 2>&1 || {
    echo 'Firefox release is unavailable. Check the Mozilla APT source; ESR will not be substituted.' >&2; exit 1;
}
apt-get install $yes_option --no-install-recommends "$core" "$desktop"
echo 'DebION SDE installed. Select SDE in LightDM after restarting the system.'
