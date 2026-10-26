/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Unit test for the network places of Liria's location bar
 * (src/liria-sde-address.c): Windows paths, URIs, IP addresses and
 * computer names, passwords taken out of addresses, errors, the other
 * protocols for a computer and the recent server list, against a
 * temporary configuration directory. Run by "make check" in sde-tools/
 * and by tests/test_native_forks.py.
 */
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "../src/liria-sde-address.c"

typedef struct {
	const gchar *typed;
	LiriaSdeAddressKind kind;
	const gchar *uri;
	const gchar *scheme;
	const gchar *host;
	const gchar *user;
	const gchar *password;
	gint port;
	gboolean ambiguous;
} Case;

static const Case cases[] = {
	/* Windows shares */
	{ "smb://server/share", LIRIA_SDE_ADDRESS_LOCATION, "smb://server/share", "smb", "server", NULL, NULL, 0, FALSE },
	{ "SMB://Server/Share/", LIRIA_SDE_ADDRESS_LOCATION, "smb://Server/Share/", "smb", "Server", NULL, NULL, 0, FALSE },
	{ "smb://nas/My Share/Docs 2026", LIRIA_SDE_ADDRESS_LOCATION, "smb://nas/My%20Share/Docs%202026", "smb", "nas",
	  NULL, NULL, 0, FALSE },
	{ "smb://nas/My%20Share", LIRIA_SDE_ADDRESS_LOCATION, "smb://nas/My%20Share", "smb", "nas", NULL, NULL, 0, FALSE },
	{ "smb://nas/100%", LIRIA_SDE_ADDRESS_LOCATION, "smb://nas/100%25", "smb", "nas", NULL, NULL, 0, FALSE },
	{ "\\\\server\\share", LIRIA_SDE_ADDRESS_LOCATION, "smb://server/share", "smb", "server", NULL, NULL, 0, FALSE },
	{ "\\\\fileserver\\Finance\\2026 Q3\\", LIRIA_SDE_ADDRESS_LOCATION, "smb://fileserver/Finance/2026%20Q3/", "smb",
	  "fileserver", NULL, NULL, 0, FALSE },
	{ "\"\\\\nas\\public\"", LIRIA_SDE_ADDRESS_LOCATION, "smb://nas/public", "smb", "nas", NULL, NULL, 0, FALSE },
	{ "\\\\192.168.1.10\\scans", LIRIA_SDE_ADDRESS_LOCATION, "smb://192.168.1.10/scans", "smb", "192.168.1.10", NULL,
	  NULL, 0, FALSE },
	{ "\\\\nas", LIRIA_SDE_ADDRESS_LOCATION, "smb://nas/", "smb", "nas", NULL, NULL, 0, FALSE },
	{ "smb:\\\\nas\\share", LIRIA_SDE_ADDRESS_LOCATION, "smb://nas/share", "smb", "nas", NULL, NULL, 0, FALSE },
	{ "smb://CORP\\anna@fs01/home", LIRIA_SDE_ADDRESS_LOCATION, "smb://CORP;anna@fs01/home", "smb", "fs01",
	  "CORP;anna", NULL, 0, FALSE },
	{ "smb://anna:s3cr%40t@fs01/home", LIRIA_SDE_ADDRESS_LOCATION, "smb://anna@fs01/home", "smb", "fs01", "anna",
	  "s3cr@t", 0, FALSE },
	{ "cifs://nas/x", LIRIA_SDE_ADDRESS_LOCATION, "smb://nas/x", "smb", "nas", NULL, NULL, 0, FALSE },
	/* the network */
	{ "smb:", LIRIA_SDE_ADDRESS_LOCATION, "smb:///", "smb", NULL, NULL, NULL, 0, FALSE },
	{ "smb:/", LIRIA_SDE_ADDRESS_LOCATION, "smb:///", "smb", NULL, NULL, NULL, 0, FALSE },
	{ "smb://", LIRIA_SDE_ADDRESS_LOCATION, "smb:///", "smb", NULL, NULL, NULL, 0, FALSE },
	{ "smb:///", LIRIA_SDE_ADDRESS_LOCATION, "smb:///", "smb", NULL, NULL, NULL, 0, FALSE },
	{ "\\\\", LIRIA_SDE_ADDRESS_LOCATION, "smb:///", "smb", NULL, NULL, NULL, 0, FALSE },
	{ "network:", LIRIA_SDE_ADDRESS_LOCATION, "network:///", "network", NULL, NULL, NULL, 0, FALSE },
	/* computers */
	{ "192.168.1.10", LIRIA_SDE_ADDRESS_HOST, "smb://192.168.1.10/", "smb", "192.168.1.10", NULL, NULL, 0, FALSE },
	{ " 10.0.0.5/public ", LIRIA_SDE_ADDRESS_HOST, "smb://10.0.0.5/public", "smb", "10.0.0.5", NULL, NULL, 0, FALSE },
	{ "192.168.1.10:2222", LIRIA_SDE_ADDRESS_HOST, "smb://192.168.1.10:2222/", "smb", "192.168.1.10", NULL, NULL,
	  2222, FALSE },
	{ "localhost", LIRIA_SDE_ADDRESS_HOST, "smb://localhost/", "smb", "localhost", NULL, NULL, 0, FALSE },
	{ "LOCALHOST/share", LIRIA_SDE_ADDRESS_HOST, "smb://LOCALHOST/share", "smb", "LOCALHOST", NULL, NULL, 0, FALSE },
	{ "fe80::1", LIRIA_SDE_ADDRESS_HOST, "smb://[fe80::1]/", "smb", "fe80::1", NULL, NULL, 0, FALSE },
	{ "[2001:db8::7]", LIRIA_SDE_ADDRESS_HOST, "smb://[2001:db8::7]/", "smb", "2001:db8::7", NULL, NULL, 0, FALSE },
	{ "::1", LIRIA_SDE_ADDRESS_HOST, "smb://[::1]/", "smb", "::1", NULL, NULL, 0, FALSE },
	{ "nas", LIRIA_SDE_ADDRESS_HOST, "smb://nas/", "smb", "nas", NULL, NULL, 0, TRUE },
	{ "fileserver.corp.example", LIRIA_SDE_ADDRESS_HOST, "smb://fileserver.corp.example/", "smb",
	  "fileserver.corp.example", NULL, NULL, 0, TRUE },
	{ "nas:445", LIRIA_SDE_ADDRESS_HOST, "smb://nas:445/", "smb", "nas", NULL, NULL, 445, FALSE },
	{ "anna@nas", LIRIA_SDE_ADDRESS_HOST, "smb://anna@nas/", "smb", "nas", "anna", NULL, 0, FALSE },
	/* other protocols */
	{ "sftp://user@host/path", LIRIA_SDE_ADDRESS_LOCATION, "sftp://user@host/path", "sftp", "host", "user", NULL, 0,
	  FALSE },
	{ "sftp://host", LIRIA_SDE_ADDRESS_LOCATION, "sftp://host/", "sftp", "host", NULL, NULL, 0, FALSE },
	{ "ssh://admin@10.0.0.2:2222/srv/data", LIRIA_SDE_ADDRESS_LOCATION, "sftp://admin@10.0.0.2:2222/srv/data", "sftp",
	  "10.0.0.2", "admin", NULL, 2222, FALSE },
	{ "sftp://[fe80::5%eth0]:22/home", LIRIA_SDE_ADDRESS_LOCATION, "sftp://[fe80::5%25eth0]:22/home", "sftp",
	  "fe80::5%eth0", NULL, NULL, 22, FALSE },
	{ "ftp://ftp.example.org/pub", LIRIA_SDE_ADDRESS_LOCATION, "ftp://ftp.example.org/pub", "ftp", "ftp.example.org",
	  NULL, NULL, 0, FALSE },
	{ "ftps://files.example.org", LIRIA_SDE_ADDRESS_LOCATION, "ftps://files.example.org/", "ftps", "files.example.org",
	  NULL, NULL, 0, FALSE },
	{ "nfs://server/export/home", LIRIA_SDE_ADDRESS_LOCATION, "nfs://server/export/home", "nfs", "server", NULL, NULL,
	  0, FALSE },
	{ "dav://intranet/docs", LIRIA_SDE_ADDRESS_LOCATION, "dav://intranet/docs", "dav", "intranet", NULL, NULL, 0,
	  FALSE },
	{ "davs://cloud.example.org/remote.php/dav/files/anna/", LIRIA_SDE_ADDRESS_LOCATION,
	  "davs://cloud.example.org/remote.php/dav/files/anna/", "davs", "cloud.example.org", NULL, NULL, 0, FALSE },
	{ "webdavs://cloud.example.org", LIRIA_SDE_ADDRESS_LOCATION, "davs://cloud.example.org/", "davs",
	  "cloud.example.org", NULL, NULL, 0, FALSE },
	/* not network places */
	{ "/home/anna/Documents", LIRIA_SDE_ADDRESS_PATH, "/home/anna/Documents", NULL, NULL, NULL, NULL, 0, FALSE },
	{ "~/Downloads", LIRIA_SDE_ADDRESS_PATH, "~/Downloads", NULL, NULL, NULL, NULL, 0, FALSE },
	{ "'/home/anna/My Files'", LIRIA_SDE_ADDRESS_PATH, "/home/anna/My Files", NULL, NULL, NULL, NULL, 0, FALSE },
	{ "Documents/Reports", LIRIA_SDE_ADDRESS_PATH, "Documents/Reports", NULL, NULL, NULL, NULL, 0, FALSE },
	{ "My Folder", LIRIA_SDE_ADDRESS_PATH, "My Folder", NULL, NULL, NULL, NULL, 0, FALSE },
	{ "trash:", LIRIA_SDE_ADDRESS_LOCATION, "trash:///", "trash", NULL, NULL, NULL, 0, FALSE },
	{ "file:///etc", LIRIA_SDE_ADDRESS_LOCATION, "file:///etc", "file", NULL, NULL, NULL, 0, FALSE },
	{ "https://example.org/a", LIRIA_SDE_ADDRESS_LOCATION, "https://example.org/a", "https", NULL, NULL, NULL, 0,
	  FALSE },
	{ "", LIRIA_SDE_ADDRESS_EMPTY, NULL, NULL, NULL, NULL, NULL, 0, FALSE },
	{ "   ", LIRIA_SDE_ADDRESS_EMPTY, NULL, NULL, NULL, NULL, NULL, 0, FALSE },
};

static void
test_parse (void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (cases); i++) {
		const Case *c = &cases[i];
		LiriaSdeAddress *a = liria_sde_address_parse (c->typed);

		g_test_message ("%s", c->typed);
		g_assert_cmpint (a->kind, ==, c->kind);
		g_assert_cmpstr (a->uri, ==, c->uri);
		g_assert_cmpstr (a->scheme, ==, c->scheme);
		g_assert_cmpstr (a->host, ==, c->host);
		g_assert_cmpstr (a->user, ==, c->user);
		g_assert_cmpstr (a->password, ==, c->password);
		g_assert_cmpint (a->port, ==, c->port);
		g_assert_cmpint (a->ambiguous, ==, c->ambiguous);
		g_assert_null (a->message);
		/* What Liria opens is a URI GIO reads back unchanged. */
		if (a->kind == LIRIA_SDE_ADDRESS_LOCATION || a->kind == LIRIA_SDE_ADDRESS_HOST) {
			g_assert_true (g_uri_is_valid (a->uri, G_URI_FLAGS_NON_DNS | G_URI_FLAGS_ENCODED, NULL));
			g_assert_null (strstr (a->uri, "s3cr"));
		}
		liria_sde_address_free (a);
	}
}

static void
test_errors (void)
{
	const gchar *wrong[] = { "C:\\Users\\anna", "d:/data", "E:", "sftp://", "ftp:", "smb://bad host/x",
	                         "smb://nas:99999/", "smb://nas:0/", "sftp://[fe80::1/", "nfs://", NULL };
	guint i;

	for (i = 0; wrong[i] != NULL; i++) {
		LiriaSdeAddress *a = liria_sde_address_parse (wrong[i]);

		g_test_message ("%s", wrong[i]);
		g_assert_cmpint (a->kind, ==, LIRIA_SDE_ADDRESS_ERROR);
		g_assert_nonnull (a->message);
		g_assert_null (a->uri);
		liria_sde_address_free (a);
	}
}

static void
test_other_protocols (void)
{
	LiriaSdeAddress *a = liria_sde_address_parse ("CORP\\anna@fs01");
	const gchar *const *schemes = liria_sde_address_host_schemes ();
	gchar *uri;

	/* "CORP\anna@fs01" is not a URI: the backslash keeps it a name */
	g_assert_cmpint (a->kind, ==, LIRIA_SDE_ADDRESS_PATH);
	liria_sde_address_free (a);

	a = liria_sde_address_parse ("smb://CORP;anna@fs01:4445/share");
	uri = liria_sde_address_with_scheme (a, "sftp");
	g_assert_cmpstr (uri, ==, "sftp://anna@fs01:4445/");
	g_free (uri);
	uri = liria_sde_address_with_scheme (a, "smb");
	g_assert_cmpstr (uri, ==, "smb://CORP;anna@fs01:4445/");
	g_free (uri);
	liria_sde_address_free (a);

	a = liria_sde_address_parse ("fe80::1");
	uri = liria_sde_address_with_scheme (a, "nfs");
	g_assert_cmpstr (uri, ==, "nfs://[fe80::1]/");
	g_free (uri);
	liria_sde_address_free (a);

	a = liria_sde_address_parse ("smb:");
	g_assert_null (liria_sde_address_with_scheme (a, "sftp"));
	liria_sde_address_free (a);

	g_assert_cmpstr (schemes[0], ==, "smb");
	g_assert_true (g_strv_contains (schemes, "sftp"));
	g_assert_true (g_strv_contains (schemes, "nfs"));
	g_assert_true (g_strv_contains (schemes, "davs"));
	g_assert_true (liria_sde_address_is_plain_text ("ftp"));
	g_assert_true (liria_sde_address_is_plain_text ("dav"));
	g_assert_false (liria_sde_address_is_plain_text ("davs"));
	g_assert_false (liria_sde_address_is_plain_text ("sftp"));
	g_assert_true (liria_sde_address_is_network_scheme ("afp"));
	g_assert_false (liria_sde_address_is_network_scheme ("file"));
	g_assert_cmpstr (liria_sde_address_scheme_label ("smb"), ==, "Windows shares (SMB)");
}

static void
test_servers (void)
{
	GPtrArray *servers, *found;
	gchar *root, *text, *path;
	guint i;

	root = liria_sde_servers_root ("smb://anna@nas/Documents/2026/report.odt");
	g_assert_cmpstr (root, ==, "smb://anna@nas/Documents/");
	g_free (root);
	root = liria_sde_servers_root ("sftp://admin@[fe80::5]:2222/srv/data");
	g_assert_cmpstr (root, ==, "sftp://admin@[fe80::5]:2222/");
	g_free (root);
	root = liria_sde_servers_root ("smb://nas");
	g_assert_cmpstr (root, ==, "smb://nas/");
	g_free (root);
	g_assert_null (liria_sde_servers_root ("file:///home/anna"));
	g_assert_null (liria_sde_servers_root ("smb:///"));
	g_assert_null (liria_sde_servers_root ("trash:///"));

	servers = liria_sde_servers_parse ("# recent\n\nsmb://nas/Documents/x\nsftp://admin@web01/var/www\n"
	                                   "smb://NAS/documents/\nfile:///tmp\nnot a location\n");
	g_assert_cmpuint (servers->len, ==, 2);
	g_assert_cmpstr (servers->pdata[0], ==, "smb://nas/Documents/");
	g_assert_cmpstr (servers->pdata[1], ==, "sftp://admin@web01/");

	liria_sde_servers_push (servers, "ftp://ftp.example.org/pub/linux");
	liria_sde_servers_push (servers, "sftp://admin@web01/etc");
	g_assert_cmpuint (servers->len, ==, 3);
	g_assert_cmpstr (servers->pdata[0], ==, "sftp://admin@web01/");
	g_assert_cmpstr (servers->pdata[1], ==, "ftp://ftp.example.org/");
	g_assert_cmpstr (servers->pdata[2], ==, "smb://nas/Documents/");
	for (i = 0; i < 40; i++) {
		gchar *uri = g_strdup_printf ("smb://host%02u/share", i);

		liria_sde_servers_push (servers, uri);
		g_free (uri);
	}
	g_assert_cmpuint (servers->len, ==, SERVERS_MAX);
	g_assert_cmpstr (servers->pdata[0], ==, "smb://host39/share/");
	g_ptr_array_unref (servers);

	servers = liria_sde_servers_parse ("smb://nas/Documents/\nsftp://admin@web01/\nsmb://fs01/Finance/\n");
	found = liria_sde_servers_matching (servers, "smb://n");
	g_assert_cmpuint (found->len, ==, 1);
	g_assert_cmpstr (found->pdata[0], ==, "smb://nas/Documents/");
	g_ptr_array_unref (found);
	found = liria_sde_servers_matching (servers, "\\\\fs01\\fin");
	g_assert_cmpuint (found->len, ==, 1);
	g_assert_cmpstr (found->pdata[0], ==, "smb://fs01/Finance/");
	g_ptr_array_unref (found);
	found = liria_sde_servers_matching (servers, "web");
	g_assert_cmpuint (found->len, ==, 1);
	g_assert_cmpstr (found->pdata[0], ==, "sftp://admin@web01/");
	g_ptr_array_unref (found);
	found = liria_sde_servers_matching (servers, "\\\\");
	g_assert_cmpuint (found->len, ==, 2);
	g_ptr_array_unref (found);
	found = liria_sde_servers_matching (servers, "/home");
	g_assert_cmpuint (found->len, ==, 0);
	g_ptr_array_unref (found);
	found = liria_sde_servers_matching (servers, "");
	g_assert_cmpuint (found->len, ==, 0);
	g_ptr_array_unref (found);
	text = liria_sde_servers_serialize (servers);
	g_assert_nonnull (strstr (text, "smb://fs01/Finance/\n"));
	g_free (text);
	g_ptr_array_unref (servers);

	/* The file: private to the user, the latest first, no passwords. */
	g_assert_true (liria_sde_servers_remember ("smb://anna:secret@nas/Documents/x"));
	g_assert_true (liria_sde_servers_remember ("sftp://admin@web01/etc"));
	g_assert_true (liria_sde_servers_remember ("smb://anna@nas/Documents/y"));
	g_assert_false (liria_sde_servers_remember ("file:///home"));
	path = liria_sde_servers_path ();
	g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
	g_assert_null (strstr (text, "secret"));
	g_assert_nonnull (strstr (text, "smb://anna@nas/Documents/\nsftp://admin@web01/\n"));
	g_free (text);
	{
		GStatBuf st;

		g_assert_cmpint (g_stat (path, &st), ==, 0);
		g_assert_cmpint (st.st_mode & 0777, ==, 0600);
	}
	servers = liria_sde_servers_load ();
	g_assert_cmpuint (servers->len, ==, 2);
	g_ptr_array_unref (servers);
	g_free (path);
}

int
main (int argc, char **argv)
{
	gchar *home = g_dir_make_tmp ("liria-sde-address-XXXXXX", NULL);
	int status;

	g_setenv ("XDG_CONFIG_HOME", home, TRUE);
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/liria/address/parse", test_parse);
	g_test_add_func ("/liria/address/errors", test_errors);
	g_test_add_func ("/liria/address/other-protocols", test_other_protocols);
	g_test_add_func ("/liria/address/servers", test_servers);
	status = g_test_run ();
	{
		gchar *dir = g_build_filename (home, "sde", NULL);
		gchar *file = g_build_filename (dir, "liria-servers", NULL);

		g_remove (file);
		g_rmdir (dir);
		g_rmdir (home);
		g_free (file);
		g_free (dir);
	}
	g_free (home);
	return status;
}
