#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build libraries only. No pluma/caja/atril/engrampa/eom application packages.
set -eu
apt-get install -y --no-install-recommends \
    autoconf automake autoconf-archive libtool libtool-bin autopoint intltool mate-common \
    gtk-doc-tools yelp-tools xsltproc docbook-xsl docbook-xml gobject-introspection libgirepository1.0-dev polkitd libpolkit-gobject-1-dev libpolkit-agent-1-dev \
    libgtk-3-dev libgail-3-dev libgtksourceview-4-dev libenchant-2-dev iso-codes libmate-desktop-dev libmate-menu-dev libpeas-dev \
    libgtop2-dev libsystemd-dev libwnck-3-dev libxml2-dev libxml2-utils libexif-dev \
    libexempi-dev libsecret-1-dev libpoppler-glib-dev libsynctex-dev libspectre-dev \
    libdjvulibre-dev libtiff-dev libarchive-dev libjson-glib-dev \
    libcanberra-gtk3-dev libdbus-glib-1-dev libnotify-dev libupower-glib-dev \
    libpam0g-dev libxss-dev libxrandr-dev libxxf86vm-dev libsm-dev libice-dev \
    liblcms2-dev libxklavier-dev libglib2.0-bin libglib2.0-dev-bin chrpath

apt-get install -y --no-install-recommends \
    libgtk-4-dev libjavascriptcoregtk-4.1-dev meson ninja-build libxcursor-dev libxinerama-dev \
    libpango1.0-dev libimlib2-dev libconfig-dev libev-dev libepoxy-dev libpcre2-dev libpixman-1-dev libx11-xcb-dev \
    libxcb-composite0-dev libxcb-damage0-dev libxcb-glx0-dev libxcb-image0-dev libxcb-present-dev \
    libxcb-randr0-dev libxcb-render0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-sync-dev \
    libxcb-util-dev libxcb-xfixes0-dev uthash-dev
