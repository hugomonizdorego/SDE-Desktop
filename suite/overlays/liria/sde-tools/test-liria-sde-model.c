/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Unit test for Liria's SDE preferences and widget-free decisions
 * (src/liria-sde-model.c): ribbon modes and their stored preference,
 * collapsed-section flags, ribbon command sensitivity, the Home view's
 * suggested folders and drive grid, and what a drop on Quick access or
 * the sidebar's Pinned section does (pin, reorder or file operation).
 * Run by "make check" in sde-tools/ and by tests/test_native_forks.py.
 */
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "../src/liria-sde-model.c"
#include "../src/liria-sde-pins.c"

static gchar *home;

static void
test_ribbon_modes (void)
{
	g_assert_cmpint (liria_sde_ribbon_mode_parse (NULL), ==, LIRIA_SDE_RIBBON_FULL);
	g_assert_cmpint (liria_sde_ribbon_mode_parse ("full"), ==, LIRIA_SDE_RIBBON_FULL);
	g_assert_cmpint (liria_sde_ribbon_mode_parse ("collapsed"), ==, LIRIA_SDE_RIBBON_COLLAPSED);
	g_assert_cmpint (liria_sde_ribbon_mode_parse ("off"), ==, LIRIA_SDE_RIBBON_OFF);
	g_assert_cmpint (liria_sde_ribbon_mode_parse ("nonsense"), ==, LIRIA_SDE_RIBBON_FULL);
	for (gint m = LIRIA_SDE_RIBBON_FULL; m <= LIRIA_SDE_RIBBON_OFF; m++)
		g_assert_cmpint (liria_sde_ribbon_mode_parse (liria_sde_ribbon_mode_name (m)), ==, m);
	/* Ctrl+F1 collapses and expands; an "off" ribbon comes back full. */
	g_assert_cmpint (liria_sde_ribbon_mode_toggle (LIRIA_SDE_RIBBON_FULL), ==, LIRIA_SDE_RIBBON_COLLAPSED);
	g_assert_cmpint (liria_sde_ribbon_mode_toggle (LIRIA_SDE_RIBBON_COLLAPSED), ==, LIRIA_SDE_RIBBON_FULL);
	g_assert_cmpint (liria_sde_ribbon_mode_toggle (LIRIA_SDE_RIBBON_OFF), ==, LIRIA_SDE_RIBBON_FULL);
}

static void
test_prefs (void)
{
	gchar *path = liria_sde_prefs_path (), *text = NULL;

	g_assert_true (g_str_has_prefix (path, home));
	g_assert_true (g_str_has_suffix (path, "/sde/liria.conf"));
	g_unlink (path);
	/* Missing file: the ribbon is full, the overview shown. */
	g_assert_cmpint (liria_sde_ribbon_mode_load (), ==, LIRIA_SDE_RIBBON_FULL);
	g_assert_true (liria_sde_prefs_get_bool ("home", "show-overview", TRUE));
	g_assert_true (liria_sde_ribbon_mode_save (LIRIA_SDE_RIBBON_COLLAPSED));
	g_assert_cmpint (liria_sde_ribbon_mode_load (), ==, LIRIA_SDE_RIBBON_COLLAPSED);
	g_assert_true (liria_sde_ribbon_mode_save (LIRIA_SDE_RIBBON_OFF));
	g_assert_cmpint (liria_sde_ribbon_mode_load (), ==, LIRIA_SDE_RIBBON_OFF);
	g_assert_true (liria_sde_prefs_set_bool ("home", "show-overview", FALSE));
	g_assert_false (liria_sde_prefs_get_bool ("home", "show-overview", TRUE));
	/* Both values live side by side in one key file. */
	g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
	g_assert_nonnull (strstr (text, "[ribbon]"));
	g_assert_nonnull (strstr (text, "mode=off"));
	g_assert_nonnull (strstr (text, "show-overview=false"));
	g_free (text);

	g_assert_false (liria_sde_prefs_has_flag ("home", "collapsed", "drives"));
	g_assert_true (liria_sde_prefs_set_flag ("home", "collapsed", "drives", TRUE));
	g_assert_true (liria_sde_prefs_set_flag ("home", "collapsed", "recent", TRUE));
	g_assert_true (liria_sde_prefs_set_flag ("home", "collapsed", "drives", TRUE));
	g_assert_true (liria_sde_prefs_has_flag ("home", "collapsed", "drives"));
	g_assert_true (liria_sde_prefs_has_flag ("home", "collapsed", "recent"));
	g_assert_false (liria_sde_prefs_has_flag ("home", "collapsed", "quick"));
	g_assert_true (liria_sde_prefs_set_flag ("home", "collapsed", "drives", FALSE));
	g_assert_false (liria_sde_prefs_has_flag ("home", "collapsed", "drives"));
	g_assert_true (liria_sde_prefs_has_flag ("home", "collapsed", "recent"));
	g_free (path);
}

static void
test_flags (void)
{
	gchar *s;

	g_assert_true (liria_sde_flags_contain ("a;b; c", "c"));
	g_assert_false (liria_sde_flags_contain ("a;bc", "b"));
	g_assert_false (liria_sde_flags_contain (NULL, "a"));
	s = liria_sde_flags_with ("a;;b;a", "c", TRUE);
	g_assert_cmpstr (s, ==, "a;b;c");
	g_free (s);
	s = liria_sde_flags_with ("a;b;c", "b", FALSE);
	g_assert_cmpstr (s, ==, "a;c");
	g_free (s);
	s = liria_sde_flags_with ("", "x", TRUE);
	g_assert_cmpstr (s, ==, "x");
	g_free (s);
}

static void
test_commands (void)
{
	LiriaSdeSelection none = { 0, 0, 0, TRUE, FALSE };
	LiriaSdeSelection one = { 1, 1, 0, TRUE, TRUE };
	LiriaSdeSelection two_ro = { 2, 0, 0, FALSE, TRUE };
	LiriaSdeSelection archives = { 2, 0, 2, TRUE, FALSE };
	guint c;

	c = liria_sde_selection_commands (&none);
	g_assert_true (c & LIRIA_SDE_CMD_PIN);
	g_assert_true (c & LIRIA_SDE_CMD_NEW);
	g_assert_false (c & (LIRIA_SDE_CMD_COPY | LIRIA_SDE_CMD_CUT | LIRIA_SDE_CMD_RENAME | LIRIA_SDE_CMD_PASTE));
	c = liria_sde_selection_commands (&one);
	g_assert_true (c & LIRIA_SDE_CMD_RENAME);
	g_assert_true (c & LIRIA_SDE_CMD_PASTE);
	g_assert_true (c & LIRIA_SDE_CMD_CUT);
	g_assert_false (c & LIRIA_SDE_CMD_EXTRACT);
	c = liria_sde_selection_commands (&two_ro);
	g_assert_true (c & LIRIA_SDE_CMD_COPY);
	g_assert_false (c & (LIRIA_SDE_CMD_CUT | LIRIA_SDE_CMD_RENAME | LIRIA_SDE_CMD_PASTE | LIRIA_SDE_CMD_NEW));
	c = liria_sde_selection_commands (&archives);
	g_assert_true (c & LIRIA_SDE_CMD_EXTRACT);
	g_assert_false (c & LIRIA_SDE_CMD_RENAME);
}

static void
test_frequent_folders (void)
{
	const gchar *recent[] = {
		"file:///home/u/Projects/a/x.c", "file:///home/u/Docs/report.odt", "file:///home/u/Projects/a/y.c",
		"file:///home/u/top.txt", "file:///etc.conf", "sftp://host/srv/file", "file:///home/u/Music/s.ogg",
		"file:///home/u/Projects/a/z.c", "file:///home/u/Docs/plan.odt", "file:///home/u/Old/old.txt", NULL
	};
	const gchar *pinned[] = { "file:///home/u/Music", NULL };
	GPtrArray *folders = liria_sde_home_frequent_folders (recent, pinned, "file:///home/u", 2);

	/* Projects/a (3) before Docs (2); home, root, remote and pinned are left out. */
	g_assert_cmpuint (folders->len, ==, 2);
	g_assert_cmpstr (folders->pdata[0], ==, "file:///home/u/Projects/a");
	g_assert_cmpstr (folders->pdata[1], ==, "file:///home/u/Docs");
	g_ptr_array_unref (folders);
	folders = liria_sde_home_frequent_folders (recent, NULL, "file:///home/u", 10);
	g_assert_cmpuint (folders->len, ==, 4);
	g_assert_cmpstr (folders->pdata[2], ==, "file:///home/u/Music"); /* first seen before Old */
	g_assert_cmpstr (folders->pdata[3], ==, "file:///home/u/Old");
	g_ptr_array_unref (folders);
	folders = liria_sde_home_frequent_folders (NULL, NULL, NULL, 4);
	g_assert_cmpuint (folders->len, ==, 0);
	g_ptr_array_unref (folders);
}

static void
test_columns (void)
{
	/* Drive cards: at least 220 px, 8 px apart, at most 3 per row. */
	g_assert_cmpuint (liria_sde_home_columns (900, 220, 8, 3), ==, 3);
	g_assert_cmpuint (liria_sde_home_columns (676, 220, 8, 3), ==, 3);
	g_assert_cmpuint (liria_sde_home_columns (675, 220, 8, 3), ==, 2);
	g_assert_cmpuint (liria_sde_home_columns (448, 220, 8, 3), ==, 2);
	g_assert_cmpuint (liria_sde_home_columns (300, 220, 8, 3), ==, 1);
	g_assert_cmpuint (liria_sde_home_columns (100, 220, 8, 3), ==, 1);
	g_assert_cmpuint (liria_sde_home_columns (0, 220, 8, 3), ==, 1);
}

static void
test_drop_decisions (void)
{
	/* Files and folders dropped on the area, a gap or a file card are pinned. */
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_AREA, FALSE, 1), ==, LIRIA_SDE_DROP_PIN);
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_FILE_CARD, FALSE, 3), ==, LIRIA_SDE_DROP_PIN);
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_PINNED_HEADING, FALSE, 1), ==, LIRIA_SDE_DROP_PIN);
	/* On a folder or drive card they are copied or moved into it. */
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_FOLDER_CARD, FALSE, 2), ==, LIRIA_SDE_DROP_FILES);
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_DRIVE_CARD, FALSE, 1), ==, LIRIA_SDE_DROP_FILES);
	/* A pin card dragged within Quick access changes its place. */
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_AREA, TRUE, 1), ==, LIRIA_SDE_DROP_REORDER);
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_FOLDER_CARD, TRUE, 1), ==, LIRIA_SDE_DROP_REORDER);
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_DRIVE_CARD, TRUE, 1), ==, LIRIA_SDE_DROP_FILES);
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_AREA, FALSE, 0), ==, LIRIA_SDE_DROP_NONE);
	/* Left half: before the card, right half: after it. */
	g_assert_cmpint (liria_sde_drop_pin_index (2, 110, 100, 120), ==, 2);
	g_assert_cmpint (liria_sde_drop_pin_index (2, 200, 100, 120), ==, 3);
	g_assert_cmpint (liria_sde_drop_pin_index (-1, 200, 100, 120), ==, -1);
}

/* A drop on Quick access pins the dropped folders where they were dropped. */
static void
test_pin_on_drop (void)
{
	gchar *pins_file = liria_sde_pins_path ();
	gchar *a = g_build_filename (home, "A", NULL), *b = g_build_filename (home, "B", NULL);
	gchar *c = g_build_filename (home, "C", NULL);
	gchar *ua = g_filename_to_uri (a, NULL, NULL), *ub = g_filename_to_uri (b, NULL, NULL);
	gchar *uc = g_filename_to_uri (c, NULL, NULL);
	const gchar *first[] = { ua, ub, NULL }, *dropped[] = { uc, ua, NULL };
	GPtrArray *pins;
	gint index;

	g_unlink (pins_file);
	g_assert_true (liria_sde_pins_save (g_ptr_array_new ()));
	g_assert_true (liria_sde_pins_add (first, -1));
	/* Dropped on the right half of card 0 (A): C goes between A and B;
	 * A is pinned already and keeps its place. */
	index = liria_sde_drop_pin_index (0, 90, 0, 100);
	g_assert_cmpint (liria_sde_drop_decide (LIRIA_SDE_DROP_AREA, FALSE, 2), ==, LIRIA_SDE_DROP_PIN);
	g_assert_true (liria_sde_pins_add (dropped, index));
	pins = liria_sde_pins_load ();
	g_assert_cmpuint (pins->len, ==, 3);
	g_assert_cmpstr (((LiriaSdePin *) pins->pdata[0])->uri, ==, ua);
	g_assert_cmpstr (((LiriaSdePin *) pins->pdata[1])->uri, ==, uc);
	g_assert_cmpstr (((LiriaSdePin *) pins->pdata[2])->uri, ==, ub);
	g_ptr_array_unref (pins);
	/* Dropping only known pins changes nothing. */
	g_assert_false (liria_sde_pins_add (first, 0));
	g_free (ua);
	g_free (ub);
	g_free (uc);
	g_free (a);
	g_free (b);
	g_free (c);
	g_free (pins_file);
}

/* 0.2.0 Alpha 2: ribbon icon sizes follow the interface scale and stay at
 * sizes icon themes draw; full-colour icons are told from grey glyphs. */
static void
test_ribbon_icons (void)
{
	guchar grey[4 * 4 * 4], blue[4 * 4 * 4], accent[4 * 4 * 3];

	/* 100 % (96 dpi, or no setting), 125 %, 150 %, 175 % and 200 %. */
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (-1, TRUE), ==, 32);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (-1, FALSE), ==, 16);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (96 * 1024, TRUE), ==, 32);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (96 * 1024, FALSE), ==, 16);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (120 * 1024, FALSE), ==, 22);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (120 * 1024, TRUE), ==, 48);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (144 * 1024, FALSE), ==, 24);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (144 * 1024, TRUE), ==, 48);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (168 * 1024, FALSE), ==, 32);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (192 * 1024, FALSE), ==, 32);
	g_assert_cmpint (liria_sde_ribbon_icon_pixels (192 * 1024, TRUE), ==, 64);
	/* A grey glyph (Papirus actions, #444444) with transparent corners. */
	for (guint i = 0; i < 16; i++) {
		grey[i * 4] = grey[i * 4 + 1] = grey[i * 4 + 2] = 0x44;
		grey[i * 4 + 3] = (i % 5 == 0) ? 0 : 255;
		blue[i * 4] = 0x52, blue[i * 4 + 1] = 0x94, blue[i * 4 + 2] = 0xE2;
		blue[i * 4 + 3] = 255;
	}
	g_assert_false (liria_sde_pixels_colourful (grey, 4, 4, 16, 4));
	g_assert_true (liria_sde_pixels_colourful (blue, 4, 4, 16, 4));
	/* Two saturated pixels of the twelve visible ones are enough (1/8
	 * or more), one is not. */
	memcpy (blue, grey, sizeof grey);
	blue[1 * 4] = 0x52, blue[1 * 4 + 1] = 0x94, blue[1 * 4 + 2] = 0xE2;
	g_assert_false (liria_sde_pixels_colourful (blue, 4, 4, 16, 4));
	blue[2 * 4] = 0xE2, blue[2 * 4 + 1] = 0x94, blue[2 * 4 + 2] = 0x32;
	g_assert_true (liria_sde_pixels_colourful (blue, 4, 4, 16, 4));
	/* Invisible pixels do not count; RGB rows work as well. */
	memset (grey, 0, sizeof grey);
	g_assert_false (liria_sde_pixels_colourful (grey, 4, 4, 16, 4));
	for (guint i = 0; i < 16; i++)
		accent[i * 3] = 0x38, accent[i * 3 + 1] = 0x76, accent[i * 3 + 2] = 0xB9;
	g_assert_true (liria_sde_pixels_colourful (accent, 4, 4, 12, 3));
	g_assert_false (liria_sde_pixels_colourful (NULL, 4, 4, 12, 3));
}

int
main (int argc, char **argv)
{
	gint status;

	home = g_dir_make_tmp ("liria-model-XXXXXX", NULL);
	g_setenv ("XDG_CONFIG_HOME", home, TRUE);
	g_setenv ("HOME", home, TRUE);
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/liria/model/ribbon-modes", test_ribbon_modes);
	g_test_add_func ("/liria/model/prefs", test_prefs);
	g_test_add_func ("/liria/model/flags", test_flags);
	g_test_add_func ("/liria/model/commands", test_commands);
	g_test_add_func ("/liria/model/frequent-folders", test_frequent_folders);
	g_test_add_func ("/liria/model/columns", test_columns);
	g_test_add_func ("/liria/model/drop-decisions", test_drop_decisions);
	g_test_add_func ("/liria/model/pin-on-drop", test_pin_on_drop);
	g_test_add_func ("/liria/model/ribbon-icons", test_ribbon_icons);
	status = g_test_run ();
	{
		gchar *argv_rm[] = { (gchar *) "rm", (gchar *) "-rf", home, NULL };
		g_spawn_sync (NULL, argv_rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
	}
	g_free (home);
	return status;
}
