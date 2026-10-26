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

# liria-sde-pins.c builds the window's Quick access pin list code
# (src/liria-sde-pins.c) into the extension as well.
libliria_sde_tools_la_SOURCES = liria-sde-tools.c liria-sde-home.c liria-sde-pins.c liria-sde-model.c
libliria_sde_tools_la_LDFLAGS = -module -avoid-version -no-undefined
libliria_sde_tools_la_LIBADD = \
	$(top_builddir)/libliria-extension/libliria-extension.la \
	$(LIBLIRIA_EXTENSION_LIBS) \
	$(NULL)

extensioninfodir = $(datadir)/liria/extensions
extensioninfo_DATA = libliria-sde-tools.so.liria-extension

# Unit tests of the Quick access pin list (parser, writer, edits), of
# the preferences and decisions behind the ribbon, Home view and drops,
# and of the network places typed in the location bar (0.2.0 Alpha 3).
check_PROGRAMS = test-liria-sde-pins test-liria-sde-model test-liria-sde-address
test_liria_sde_pins_SOURCES = test-liria-sde-pins.c
test_liria_sde_pins_LDADD = $(LIBLIRIA_EXTENSION_LIBS)
test_liria_sde_model_SOURCES = test-liria-sde-model.c
test_liria_sde_model_LDADD = $(LIBLIRIA_EXTENSION_LIBS)
test_liria_sde_address_SOURCES = test-liria-sde-address.c
test_liria_sde_address_LDADD = $(LIBLIRIA_EXTENSION_LIBS)
TESTS = test-liria-sde-pins test-liria-sde-model test-liria-sde-address

EXTRA_DIST = $(extensioninfo_DATA)
