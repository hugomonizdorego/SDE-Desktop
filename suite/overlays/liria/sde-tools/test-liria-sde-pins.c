/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Unit test for the Liria Quick access pin list (src/liria-sde-pins.c):
 * parsing, writing, defaults and the load-change-save helpers, against a
 * temporary configuration directory. Run by "make check" in sde-tools/ and
 * by tests/test_native_forks.py.
 */
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "../src/liria-sde-pins.c"

static gchar *home;

static const gchar *
pin_uri (GPtrArray *pins, guint i)
{
	return ((LiriaSdePin *) pins->pdata[i])->uri;
}

static const gchar *
pin_label (GPtrArray *pins, guint i)
{
	return ((LiriaSdePin *) pins->pdata[i])->label;
}

static void
test_parse (void)
{
	GPtrArray *pins = liria_sde_pins_parse ("# comment\n"
	                                        "\n"
	                                        "file:///srv/a\r\n"
	                                        "/srv/b\tMy B \n"
	                                        "  sftp://host/share\t\n"
	                                        "file:///srv/a\tduplicate\n"
	                                        "not a uri\n"
	                                        "trash:///\n");

	g_assert_cmpuint (pins->len, ==, 4);
	g_assert_cmpstr (pin_uri (pins, 0), ==, "file:///srv/a");
	g_assert_null (pin_label (pins, 0));
	g_assert_cmpstr (pin_uri (pins, 1), ==, "file:///srv/b");
	g_assert_cmpstr (pin_label (pins, 1), ==, "My B");
	g_assert_cmpstr (pin_uri (pins, 2), ==, "sftp://host/share");
	g_assert_null (pin_label (pins, 2));
	g_assert_cmpstr (pin_uri (pins, 3), ==, "trash:///");
	g_assert_cmpint (liria_sde_pins_find (pins, "/srv/b"), ==, 1);
	g_assert_cmpint (liria_sde_pins_find (pins, "file:///srv/b/"), ==, 1);
	g_assert_cmpint (liria_sde_pins_find (pins, "file:///srv/c"), ==, -1);
	g_ptr_array_unref (pins);

	pins = liria_sde_pins_parse ("");
	g_assert_cmpuint (pins->len, ==, 0);
	g_ptr_array_unref (pins);
	pins = liria_sde_pins_parse (NULL);
	g_assert_cmpuint (pins->len, ==, 0);
	g_ptr_array_unref (pins);
}

static void
test_round_trip (void)
{
	GPtrArray *pins = g_ptr_array_new_with_free_func ((GDestroyNotify) liria_sde_pin_free);
	GPtrArray *again;
	gchar *text;

	g_ptr_array_add (pins, liria_sde_pin_new ("file:///srv/with%20space", NULL));
	g_ptr_array_add (pins, liria_sde_pin_new ("file:///srv/x.odt", "Tab\there\nnewline"));
	text = liria_sde_pins_serialize (pins);
	g_assert_true (g_str_has_prefix (text, "#"));
	g_assert_nonnull (strstr (text, "file:///srv/x.odt\tTab here newline\n"));
	again = liria_sde_pins_parse (text);
	g_assert_cmpuint (again->len, ==, 2);
	g_assert_cmpstr (pin_uri (again, 0), ==, "file:///srv/with%20space");
	g_assert_cmpstr (pin_label (again, 1), ==, "Tab here newline");
	g_ptr_array_unref (again);
	g_ptr_array_unref (pins);
	g_free (text);
}

static void
test_defaults_and_edits (void)
{
	gchar *docs = g_build_filename (home, "Documents", NULL);
	gchar *docs_uri = g_filename_to_uri (docs, NULL, NULL);
	const gchar *add[] = { "/srv/report.pdf", "file:///srv/photos", NULL };
	const gchar *again[] = { "/srv/report.pdf", NULL };
	const gchar *remove[] = { "file:///srv/photos", "trash:///", NULL };
	gchar *path = liria_sde_pins_path (), *text = NULL;
	GPtrArray *pins;

	g_assert_true (g_str_has_suffix (path, "/sde/liria-quick-access"));
	g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

	/* No file: the defaults. Documents exists, so it is there; Trash last. */
	pins = liria_sde_pins_load ();
	g_assert_cmpint (liria_sde_pins_find (pins, docs_uri), >=, 0);
	g_assert_cmpstr (pin_uri (pins, pins->len - 1), ==, "trash:///");
	g_ptr_array_unref (pins);

	g_assert_true (liria_sde_pins_add (add, 0));
	g_assert_false (liria_sde_pins_add (again, -1)); /* already pinned */
	g_assert_true (liria_sde_pins_is_pinned ("file:///srv/report.pdf"));
	pins = liria_sde_pins_load ();
	g_assert_cmpstr (pin_uri (pins, 0), ==, "file:///srv/report.pdf");
	g_assert_cmpstr (pin_uri (pins, 1), ==, "file:///srv/photos");
	g_ptr_array_unref (pins);

	g_assert_true (liria_sde_pins_move ("file:///srv/report.pdf", 1000));
	g_assert_true (liria_sde_pins_set_label ("file:///srv/report.pdf", "  Report  "));
	pins = liria_sde_pins_load ();
	g_assert_cmpstr (pin_uri (pins, pins->len - 1), ==, "file:///srv/report.pdf");
	g_assert_cmpstr (pin_label (pins, pins->len - 1), ==, "Report");
	g_ptr_array_unref (pins);
	g_assert_false (liria_sde_pins_move ("file:///srv/missing", 0));

	g_assert_true (liria_sde_pins_remove (remove));
	g_assert_false (liria_sde_pins_is_pinned ("trash:///"));
	g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
	g_assert_null (strstr (text, "photos"));
	g_free (text);

	/* Unpinning everything leaves an empty list, not the defaults. */
	pins = g_ptr_array_new ();
	g_assert_true (liria_sde_pins_save (pins));
	g_ptr_array_unref (pins);
	pins = liria_sde_pins_load ();
	g_assert_cmpuint (pins->len, ==, 0);
	g_ptr_array_unref (pins);

	g_assert_true (liria_sde_pins_reset ());
	g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
	g_assert_true (liria_sde_pins_is_pinned ("trash:///"));

	g_free (path);
	g_free (docs_uri);
	g_free (docs);
}

static void
remove_tree (const gchar *path)
{
	GDir *dir = g_dir_open (path, 0, NULL);
	const gchar *name;

	while (dir && (name = g_dir_read_name (dir))) {
		gchar *child = g_build_filename (path, name, NULL);

		if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
			remove_tree (child);
		else
			g_unlink (child);
		g_free (child);
	}
	if (dir)
		g_dir_close (dir);
	g_rmdir (path);
}

int
main (int argc, char **argv)
{
	int status;
	gchar *config, *docs, *dirs;

	home = g_dir_make_tmp ("liria-pins-XXXXXX", NULL);
	config = g_build_filename (home, ".config", NULL);
	docs = g_build_filename (home, "Documents", NULL);
	g_mkdir_with_parents (config, 0700);
	g_mkdir_with_parents (docs, 0700);
	dirs = g_build_filename (config, "user-dirs.dirs", NULL);
	g_file_set_contents (dirs, "XDG_DOCUMENTS_DIR=\"$HOME/Documents\"\n", -1, NULL);
	g_setenv ("HOME", home, TRUE);
	g_setenv ("XDG_CONFIG_HOME", config, TRUE);

	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/liria/pins/parse", test_parse);
	g_test_add_func ("/liria/pins/round-trip", test_round_trip);
	g_test_add_func ("/liria/pins/defaults-and-edits", test_defaults_and_edits);
	status = g_test_run ();
	remove_tree (home);
	return status;
}
