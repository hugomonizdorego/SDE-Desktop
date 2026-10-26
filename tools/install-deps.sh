#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs everything needed to build SDE and its Debian package on
# Debian testing / DebION. Run as root or with sudo.
#
#   sudo tools/install-deps.sh            # build dependencies
#   sudo tools/install-deps.sh --runtime  # plus what a running desktop needs
#   sudo tools/install-deps.sh --preview  # plus Xvfb tools for screenshots
set -eu

BUILD_DEPS="build-essential cmake pkgconf rustc cargo debhelper devscripts fakeroot lintian
libcairo2-dev librsvg2-dev libx11-dev libxrandr-dev libxi-dev libxft-dev libxfixes-dev libxext-dev libxrender-dev libxcomposite-dev libxinerama-dev libxcursor-dev libfontconfig-dev libpng-dev libjpeg-dev zlib1g-dev libdbus-1-dev libcrypt-dev libappstream-dev libflatpak-dev gettext python3 sassc xvfb x11-xserver-utils gpg gpg-agent gpgv"
# Merkadu: the package lists (All Software) are read with libapt-pkg.
BUILD_DEPS="$BUILD_DEPS libapt-pkg-dev"
RUNTIME_DEPS="gsettings-desktop-schemas librsvg2-common dbus-x11 xdg-utils xdg-user-dirs x11-utils
x11-xserver-utils x11-xkb-utils xinput fonts-dejavu-core xterm
packagekit packagekit-tools python3-apt mate-desktop-common zip unzip 7zip bzip2 xz-utils gpgv"
OPTIONAL_DEPS="papirus-icon-theme adwaita-icon-theme network-manager bluez blueman
pulseaudio-utils libglib2.0-bin libnotify-bin libfuse2t64 fuse3"
PREVIEW_DEPS="xvfb xdotool imagemagick x11-apps pulseaudio"

packages="$BUILD_DEPS"
for arg in "$@"; do
    case "$arg" in
        --runtime) packages="$packages $RUNTIME_DEPS $OPTIONAL_DEPS" ;;
        --preview) packages="$packages $RUNTIME_DEPS $OPTIONAL_DEPS $PREVIEW_DEPS" ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "install-deps.sh: unknown option $arg" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "install-deps.sh: please run as root (sudo $0 $*)" >&2
    exit 1
fi

. /etc/os-release
echo "Installing SDE dependencies for $PRETTY_NAME"
apt-get update
# Install what exists; optional packages may be missing on older releases.
available=""
for p in $packages; do
    if apt-cache show "$p" >/dev/null 2>&1; then
        available="$available $p"
    else
        echo "  skipping $p (not available on this release)"
    fi
done
# shellcheck disable=SC2086
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $available
