#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build libraries only. No pluma/caja/atril/engrampa/eom application packages.
set -eu
apt-get install -y --no-install-recommends \
    autoconf automake autoconf-archive libtool libtool-bin autopoint intltool mate-common \
    gtk-doc-tools yelp-tools xsltproc docbook-xsl docbook-xml gobject-introspection \
    libgtk-3-dev libgail-3-dev libgtksourceview-4-dev libenchant-2-dev iso-codes libmate-desktop-dev libpeas-dev \
    libgtop2-dev libsystemd-dev libgtkmm-3.0-dev libwnck-3-dev libxml2-dev libexif-dev \
    libexempi-dev libsecret-1-dev libpoppler-glib-dev libsynctex-dev libspectre-dev \
    libdjvulibre-dev libtiff-dev libarchive-dev libjson-glib-dev \
    libcanberra-gtk3-dev libdbus-glib-1-dev libnotify-dev libupower-glib-dev \
    libpam0g-dev libxss-dev libxrandr-dev libxxf86vm-dev libsm-dev libice-dev \
    liblcms2-dev libxklavier-dev libglib2.0-bin libglib2.0-dev-bin chrpath
