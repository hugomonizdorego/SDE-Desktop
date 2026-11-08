#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build libraries only (debian/control is the complete list); no upstream
# application packages. SDE WM (Openbox) is built with autotools, SDE
# compositor (picom) with meson. The MATE build tools of the native suite
# went with its last fork, Liria's Caja fork, in 0.2.0 Alpha 4.
set -eu
apt-get install -y --no-install-recommends \
    autoconf automake libtool libtool-bin autopoint \
    libpolkit-gobject-1-dev libpolkit-agent-1-dev \
    libgtk-3-dev libglib2.0-dev libgdk-pixbuf-2.0-dev libxml2-dev libexif-dev \
    libpoppler-glib-dev libarchive-dev \
    libpam0g-dev libxss-dev libxrandr-dev libxdamage-dev libsm-dev libice-dev \
    libglib2.0-bin libglib2.0-dev-bin

apt-get install -y --no-install-recommends \
    meson ninja-build libxcursor-dev libxinerama-dev \
    libpango1.0-dev libimlib2-dev libconfig-dev libev-dev libepoxy-dev libpcre2-dev libpixman-1-dev libx11-xcb-dev \
    libxcb-composite0-dev libxcb-damage0-dev libxcb-glx0-dev libxcb-image0-dev libxcb-present-dev \
    libxcb-randr0-dev libxcb-render0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-sync-dev \
    libxcb-util-dev libxcb-xfixes0-dev uthash-dev
