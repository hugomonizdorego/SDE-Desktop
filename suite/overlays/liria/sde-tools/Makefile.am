# SPDX-License-Identifier: GPL-2.0-or-later
# SDE Tools extension for Liria (added by SDE, see suite/redesign.py).

AM_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_builddir) \
	$(LIBLIRIA_EXTENSION_CFLAGS) \
	$(WARN_CFLAGS) \
	$(NULL)

extensiondir = $(libdir)/liria/extensions-2.0
extension_LTLIBRARIES = libliria-sde-tools.la

libliria_sde_tools_la_SOURCES = liria-sde-tools.c liria-sde-home.c
libliria_sde_tools_la_LDFLAGS = -module -avoid-version -no-undefined
libliria_sde_tools_la_LIBADD = \
	$(top_builddir)/libliria-extension/libliria-extension.la \
	$(LIBLIRIA_EXTENSION_LIBS) \
	$(NULL)

extensioninfodir = $(datadir)/liria/extensions
extensioninfo_DATA = libliria-sde-tools.so.liria-extension

EXTRA_DIST = $(extensioninfo_DATA)
