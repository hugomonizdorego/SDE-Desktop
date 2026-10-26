/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Network places typed in the location bar (see liria-sde-address.h).
 *
 *   smb://server/share/folder     as typed (spaces and the like escaped)
 *   \\server\share\folder         smb://server/share/folder
 *   smb:  smb:/  smb://  \\       smb:/// (the computers of the network)
 *   192.168.1.10  [fe80::1]  localhost  fileserver  user@nas
 *                                 smb://.../ (the shares of that computer;
 *                                 the window offers SFTP, FTP, WebDAV, NFS)
 *   nas:445                       smb://nas:445/
 *   sftp://user@host/path  ssh://host  ftp://  ftps://  nfs://  dav://  davs://
 *                                 cleaned up; a password in the address is
 *                                 taken out of it and given to the sign-in
 *   /path  ~/path  name           a path, as before
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "liria-sde-address.h"

#define SERVERS_MAX 30
#define SUB_DELIMS "!$&'()*+,;="

static const gchar *const network_schemes[] = { "smb", "sftp", "ftp", "ftps", "nfs", "dav", "davs", "afp", NULL };
/* The protocols offered for a computer: Windows shares, then the
 * encrypted ones, then those that send passwords as plain text. */
static const gchar *const host_schemes[] = { "smb", "sftp", "davs", "nfs", "dav", "ftp", NULL };

/* Other names people type for the same protocols. */
static const gchar *const aliases[][2] = {
	{ "ssh", "sftp" }, { "scp", "sftp" }, { "cifs", "smb" }, { "webdav", "dav" }, { "webdavs", "davs" },
	{ "ftpes", "ftps" }, { NULL, NULL }
};

/* Locations without a server: "trash:" means trash:///. */
static const gchar *const rooted_schemes[] = { "trash", "recent", "network", "computer", "burn", "admin", NULL };

/* Schemes that GIO, GVfs or Liria handle, so "name:123" with another
 * name is a computer and a port. */
static const gchar *const known_schemes[] = {
	"file", "trash", "recent", "network", "computer", "burn", "admin", "x-liria-search", "x-liria-desktop",
	"mtp", "gphoto2", "afc", "archive", "google-drive", "cdda", "dns-sd", "http", "https", "resource", "obex",
	"onedrive", "afp-browse", NULL
};

static gboolean
in_list (const gchar *const *list, const gchar *value)
{
	return value != NULL && g_strv_contains (list, value);
}

gboolean
liria_sde_address_is_network_scheme (const gchar *scheme)
{
	return in_list (network_schemes, scheme);
}

gboolean
liria_sde_address_is_plain_text (const gchar *scheme)
{
	return g_strcmp0 (scheme, "ftp") == 0 || g_strcmp0 (scheme, "dav") == 0 || g_strcmp0 (scheme, "http") == 0;
}

const gchar *const *
liria_sde_address_host_schemes (void)
{
	return host_schemes;
}

const gchar *
liria_sde_address_scheme_label (const gchar *scheme)
{
	if (g_strcmp0 (scheme, "smb") == 0)
		return _("Windows shares (SMB)");
	if (g_strcmp0 (scheme, "sftp") == 0)
		return _("SSH (SFTP)");
	if (g_strcmp0 (scheme, "ftp") == 0)
		return _("FTP");
	if (g_strcmp0 (scheme, "ftps") == 0)
		return _("FTP with TLS (FTPS)");
	if (g_strcmp0 (scheme, "nfs") == 0)
		return _("NFS");
	if (g_strcmp0 (scheme, "dav") == 0)
		return _("WebDAV (HTTP)");
	if (g_strcmp0 (scheme, "davs") == 0)
		return _("WebDAV (HTTPS)");
	if (g_strcmp0 (scheme, "afp") == 0)
		return _("Apple Filing Protocol (AFP)");
	if (g_strcmp0 (scheme, "network") == 0)
		return _("The network");
	return scheme;
}

void
liria_sde_address_free (LiriaSdeAddress *address)
{
	if (address == NULL)
		return;
	g_free (address->uri);
	g_free (address->scheme);
	g_free (address->host);
	g_free (address->user);
	if (address->password != NULL)
		memset (address->password, 0, strlen (address->password));
	g_free (address->password);
	g_free (address->message);
	g_free (address);
}

/* ------------------------------------------------------------- pieces */

/* The length of the URI scheme that |text| starts with, or 0. */
static gsize
scheme_length (const gchar *text)
{
	gsize i;

	if (!g_ascii_isalpha (text[0]))
		return 0;
	for (i = 1; g_ascii_isalnum (text[i]) || text[i] == '+' || text[i] == '-' || text[i] == '.'; i++)
		;
	return text[i] == ':' ? i : 0;
}

static gboolean
is_ipv4 (const gchar *host)
{
	gchar **parts = g_strsplit (host, ".", -1);
	gboolean ok = g_strv_length (parts) == 4;
	guint i;

	for (i = 0; ok && parts[i] != NULL; i++) {
		gsize n = strlen (parts[i]);
		guint j;

		ok = n >= 1 && n <= 3;
		for (j = 0; ok && j < n; j++)
			ok = g_ascii_isdigit (parts[i][j]);
		ok = ok && atoi (parts[i]) <= 255;
	}
	g_strfreev (parts);
	return ok;
}

/* fe80::1, ::1, 2001:db8::5%eth0 (without brackets). */
static gboolean
is_ipv6 (const gchar *host)
{
	const gchar *p;
	guint colons = 0;

	for (p = host; *p != '\0' && *p != '%'; p++) {
		if (*p == ':')
			colons++;
		else if (!g_ascii_isxdigit (*p) && *p != '.')
			return FALSE;
	}
	if (*p == '%') {
		if (p[1] == '\0')
			return FALSE;
		for (p++; *p != '\0'; p++)
			if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_' && *p != '.')
				return FALSE;
	}
	return colons >= 2;
}

/* A computer name: letters of any script, digits, '-', '_' and dots
 * between them (NetBIOS, mDNS and DNS names). */
static gboolean
is_host_name (const gchar *host)
{
	const gchar *p;

	if (*host == '\0' || *host == '.' || *host == '-' || strstr (host, "..") != NULL ||
	    !g_utf8_validate (host, -1, NULL))
		return FALSE;
	for (p = host; *p != '\0'; p = g_utf8_next_char (p)) {
		gunichar c = g_utf8_get_char (p);

		if (!g_unichar_isalnum (c) && c != '-' && c != '_' && c != '.')
			return FALSE;
	}
	return TRUE;
}

/* fe80::1/share: an IPv6 address that would look like a URI scheme. */
static gboolean
is_ipv6_start (const gchar *text)
{
	const gchar *slash = strchr (text, '/');
	gchar *first = slash != NULL ? g_strndup (text, slash - text) : g_strdup (text);
	gboolean ipv6 = is_ipv6 (first);

	g_free (first);
	return ipv6;
}

static gboolean
is_host (const gchar *host)
{
	return is_ipv4 (host) || is_ipv6 (host) || is_host_name (host);
}

static gboolean
is_port (const gchar *text, gint *port)
{
	guint64 value;

	if (text == NULL || *text == '\0' || strlen (text) > 5 ||
	    !g_ascii_string_to_unsigned (text, 10, 1, 65535, &value, NULL))
		return FALSE;
	*port = (gint) value;
	return TRUE;
}

/* |path| as a URI path: what a URI cannot hold is escaped, escapes that
 * are already there stay. */
static gchar *
escape_path (const gchar *path)
{
	GString *out = g_string_new (NULL);
	const gchar *p;

	if (*path != '/')
		g_string_append_c (out, '/');
	for (p = path; *p != '\0'; p++) {
		guchar c = (guchar) *p;

		if (c == '%' && g_ascii_isxdigit (p[1]) && g_ascii_isxdigit (p[2]))
			g_string_append_c (out, '%');
		else if (g_ascii_isalnum (c) || strchr ("-._~" SUB_DELIMS ":@/", c) != NULL)
			g_string_append_c (out, (gchar) c);
		else
			g_string_append_printf (out, "%%%02X", c);
	}
	return g_string_free (out, FALSE);
}

/* An escaped piece of a URI, unescaped; as typed if its escapes are not. */
static gchar *
unescape (const gchar *text, gsize length)
{
	gchar *piece = g_strndup (text, length);
	gchar *plain = g_uri_unescape_string (piece, NULL);

	if (plain == NULL)
		return piece;
	g_free (piece);
	return plain;
}

static gchar *
build_uri (const gchar *scheme, const gchar *user, const gchar *host, gint port, const gchar *path)
{
	GString *uri = g_string_new (scheme);
	gchar *escaped;

	g_string_append (uri, "://");
	if (user != NULL && *user != '\0') {
		escaped = g_uri_escape_string (user, SUB_DELIMS, TRUE);
		g_string_append_printf (uri, "%s@", escaped);
		g_free (escaped);
	}
	if (host != NULL && strchr (host, ':') != NULL) {
		gchar **zone = g_strsplit (host, "%", 2);

		g_string_append_printf (uri, "[%s%s%s]", zone[0], zone[1] != NULL ? "%25" : "",
		                        zone[1] != NULL ? zone[1] : "");
		g_strfreev (zone);
	} else if (host != NULL) {
		escaped = g_uri_escape_string (host, SUB_DELIMS, FALSE);
		g_string_append (uri, escaped);
		g_free (escaped);
	}
	if (port > 0)
		g_string_append_printf (uri, ":%d", port);
	escaped = escape_path (path != NULL ? path : "/");
	g_string_append (uri, escaped);
	g_free (escaped);
	return g_string_free (uri, FALSE);
}

static void
set_error (LiriaSdeAddress *address, gchar *message)
{
	address->kind = LIRIA_SDE_ADDRESS_ERROR;
	address->message = message;
}

/* ------------------------------------------------------- the parsers */

/* |text| is what follows "scheme:" of a network place:
 * [//][user[:password]@]host[:port][/path]. For Windows shares a
 * backslash separates folders as a slash does, and DOMAIN\user is the
 * user DOMAIN;user. */
static void
parse_network (LiriaSdeAddress *address, const gchar *scheme, const gchar *text)
{
	gboolean smb = strcmp (scheme, "smb") == 0;
	gchar *rest, *authority, *hostport, *host = NULL, *colon;
	const gchar *start = text, *end, *at = NULL, *p, *slash;
	gint port = 0;

	while (*start == '/' || (smb && *start == '\\'))
		start++;
	end = strchr (start, '/') != NULL ? strchr (start, '/') : start + strlen (start);
	for (p = start; p < end; p++)
		if (*p == '@')
			at = p;
	if (at != NULL) {
		gchar *info = g_strndup (start, at - start);

		colon = strchr (info, ':');
		if (colon != NULL) {
			address->password = unescape (colon + 1, strlen (colon + 1));
			*colon = '\0';
		}
		address->user = unescape (info, strlen (info));
		if (smb)
			g_strdelimit (address->user, "\\", ';');
		if (*address->user == '\0')
			g_clear_pointer (&address->user, g_free);
		g_free (info);
		start = at + 1;
	}
	rest = g_strdup (start);
	if (smb)
		g_strdelimit (rest, "\\", '/');
	slash = strchr (rest, '/');
	authority = slash != NULL ? g_strndup (rest, slash - rest) : g_strdup (rest);
	hostport = authority;
	if (hostport[0] == '[') {
		gchar *close = strchr (hostport, ']');

		if (close == NULL || (close[1] != '\0' && (close[1] != ':' || !is_port (close + 2, &port)))) {
			set_error (address, g_strdup_printf (_("“%s” is not the address of a computer."), hostport));
			goto out;
		}
		host = g_strndup (hostport + 1, close - hostport - 1);
	} else if ((colon = strchr (hostport, ':')) != NULL && strchr (colon + 1, ':') == NULL) {
		if (!is_port (colon + 1, &port)) {
			set_error (address, g_strdup_printf (_("“%s” is not a port number."), colon + 1));
			goto out;
		}
		host = g_strndup (hostport, colon - hostport);
	} else {
		host = g_strdup (hostport);
	}
	if (strchr (host, '%') != NULL && !is_ipv6 (host)) {
		gchar *plain = unescape (host, strlen (host));

		g_free (host);
		host = plain;
	}
	address->scheme = g_strdup (scheme);
	if (*host == '\0') {
		if (smb) {
			/* smb: with no computer: the computers of the network */
			address->kind = LIRIA_SDE_ADDRESS_LOCATION;
			address->uri = g_strdup ("smb:///");
			g_clear_pointer (&address->user, g_free);
		} else {
			set_error (address, g_strdup_printf (_("Type the name or address of the server after “%s://”."),
			                                     scheme));
		}
		goto out;
	}
	if (!is_host (host)) {
		set_error (address, g_strdup_printf (_("“%s” is not the name or address of a computer."), host));
		goto out;
	}
	address->kind = LIRIA_SDE_ADDRESS_LOCATION;
	address->host = g_strdup (host);
	address->port = port;
	address->uri = build_uri (scheme, address->user, host, port, slash != NULL ? slash : "/");
out:
	g_free (host);
	g_free (authority);
	g_free (rest);
}

/* user@computer[:port][/path], an IP address, localhost or a name. */
static gboolean
parse_host (LiriaSdeAddress *address, const gchar *text)
{
	const gchar *at = strrchr (text, '@'), *start = at != NULL ? at + 1 : text, *slash;
	gchar *hostport, *host, *colon;
	gint port = 0;
	gboolean plain;

	if (strchr (text, ' ') != NULL || strchr (text, '\\') != NULL)
		return FALSE;
	slash = strchr (start, '/');
	hostport = slash != NULL ? g_strndup (start, slash - start) : g_strdup (start);
	if (hostport[0] == '[' && g_str_has_suffix (hostport, "]"))
		host = g_strndup (hostport + 1, strlen (hostport) - 2);
	else if ((colon = strchr (hostport, ':')) != NULL && strchr (colon + 1, ':') == NULL && is_port (colon + 1, &port))
		host = g_strndup (hostport, colon - hostport);
	else
		host = g_strdup (hostport);
	g_free (hostport);
	plain = at == NULL && port == 0 && !is_ipv4 (host) && !is_ipv6 (host) &&
	        g_ascii_strcasecmp (host, "localhost") != 0;
	/* A plain name with a path ("docs/report") is a path. */
	if (!is_host (host) || (plain && slash != NULL) || (at != NULL && at == text)) {
		g_free (host);
		return FALSE;
	}
	address->kind = LIRIA_SDE_ADDRESS_HOST;
	address->scheme = g_strdup ("smb");
	address->host = host;
	address->port = port;
	address->ambiguous = plain;
	if (at != NULL)
		address->user = unescape (text, at - text);
	address->uri = build_uri ("smb", address->user, host, port, slash != NULL ? slash : "/");
	return TRUE;
}

LiriaSdeAddress *
liria_sde_address_parse (const gchar *text)
{
	LiriaSdeAddress *address = g_new0 (LiriaSdeAddress, 1);
	gchar *typed = g_strstrip (g_strdup (text != NULL ? text : ""));
	gsize n = strlen (typed), length;

	/* "\\server\share" pasted with its quotes */
	if (n >= 2 && (typed[0] == '"' || typed[0] == '\'') && typed[n - 1] == typed[0]) {
		memmove (typed, typed + 1, n - 2);
		typed[n - 2] = '\0';
		g_strstrip (typed);
	}
	address->kind = LIRIA_SDE_ADDRESS_PATH;
	if (*typed == '\0') {
		address->kind = LIRIA_SDE_ADDRESS_EMPTY;
	} else if (typed[0] == '\\' && typed[1] == '\\') {
		gchar *rest = g_strdup (typed + 2);

		g_strdelimit (rest, "\\", '/');
		parse_network (address, "smb", rest);
		g_free (rest);
	} else if (typed[0] == '/' || typed[0] == '~') {
		/* a path, as before */
	} else if (is_ipv6_start (typed)) {
		parse_host (address, typed);
	} else if ((length = scheme_length (typed)) > 0) {
		gchar *scheme = g_ascii_strdown (typed, length);
		const gchar *rest = typed + length + 1;
		guint i;

		for (i = 0; aliases[i][0] != NULL; i++)
			if (strcmp (scheme, aliases[i][0]) == 0) {
				g_free (scheme);
				scheme = g_strdup (aliases[i][1]);
				break;
			}
		if (length == 1 && (*rest == '\\' || *rest == '/' || *rest == '\0')) {
			set_error (address, g_strdup_printf (_("“%c:” is a drive letter of Windows. Disks are under "
			                                       "Drives in the sidebar, and a Windows share opens with "
			                                       "\\\\server\\share."), g_ascii_toupper (typed[0])));
		} else if (liria_sde_address_is_network_scheme (scheme)) {
			parse_network (address, scheme, rest);
		} else if (!in_list (known_schemes, scheme) && g_ascii_isdigit (*rest) &&
		           parse_host (address, typed)) {
			/* nas:445 is a computer and a port */
		} else {
			address->kind = LIRIA_SDE_ADDRESS_LOCATION;
			address->scheme = g_strdup (scheme);
			if (in_list (rooted_schemes, scheme) && (*rest == '\0' || strcmp (rest, "/") == 0 ||
			                                         strcmp (rest, "//") == 0))
				address->uri = g_strdup_printf ("%s:///", scheme);
			else
				address->uri = g_strdup_printf ("%s:%s", scheme, rest);
		}
		g_free (scheme);
	} else {
		parse_host (address, typed);
	}
	if (address->kind == LIRIA_SDE_ADDRESS_PATH)
		address->uri = g_strdup (typed);
	g_free (typed);
	return address;
}

gchar *
liria_sde_address_with_scheme (const LiriaSdeAddress *address, const gchar *scheme)
{
	const gchar *user = address->user;
	gchar *uri;
	gchar *plain = NULL;

	if (address->host == NULL)
		return NULL;
	/* DOMAIN;user is how Windows shares name a user of a domain. */
	if (user != NULL && strcmp (scheme, "smb") != 0 && strchr (user, ';') != NULL)
		user = plain = g_strdup (strchr (user, ';') + 1);
	uri = build_uri (scheme, user, address->host, address->port, "/");
	g_free (plain);
	return uri;
}

/* ---------------------------------------------------------- servers */

gchar *
liria_sde_servers_root (const gchar *uri)
{
	gchar *scheme = NULL, *user = NULL, *host = NULL, *path = NULL, *root = NULL;
	gint port = -1;

	if (uri == NULL ||
	    !g_uri_split_with_user (uri, G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED | G_URI_FLAGS_NON_DNS,
	                            &scheme, &user, NULL, NULL, &host, &port, &path, NULL, NULL, NULL))
		return NULL;
	if (liria_sde_address_is_network_scheme (scheme) && host != NULL && *host != '\0') {
		GString *text = g_string_new (NULL);

		g_string_append_printf (text, "%s://", scheme);
		if (user != NULL && *user != '\0')
			g_string_append_printf (text, "%s@", user);
		g_string_append_printf (text, strchr (host, ':') != NULL ? "[%s]" : "%s", host);
		if (port > 0)
			g_string_append_printf (text, ":%d", port);
		g_string_append_c (text, '/');
		/* Windows shares: the share, the first folder of the path */
		if (strcmp (scheme, "smb") == 0 && path != NULL) {
			const gchar *share = path;
			const gchar *end;

			while (*share == '/')
				share++;
			end = strchr (share, '/');
			if (*share != '\0') {
				g_string_append_len (text, share, end != NULL ? end - share : (gssize) strlen (share));
				g_string_append_c (text, '/');
			}
		}
		root = g_string_free (text, FALSE);
	}
	g_free (scheme);
	g_free (user);
	g_free (host);
	g_free (path);
	return root;
}

static gint
servers_find (GPtrArray *servers, const gchar *root)
{
	guint i;

	for (i = 0; i < servers->len; i++)
		if (g_ascii_strcasecmp (servers->pdata[i], root) == 0)
			return (gint) i;
	return -1;
}

GPtrArray *
liria_sde_servers_parse (const gchar *text)
{
	GPtrArray *servers = g_ptr_array_new_with_free_func (g_free);
	gchar **lines = g_strsplit (text != NULL ? text : "", "\n", -1);
	guint i;

	for (i = 0; lines[i] != NULL && servers->len < SERVERS_MAX; i++) {
		gchar *line = g_strstrip (lines[i]);
		gchar *root;

		if (*line == '\0' || *line == '#')
			continue;
		root = liria_sde_servers_root (line);
		if (root != NULL && servers_find (servers, root) < 0)
			g_ptr_array_add (servers, root);
		else
			g_free (root);
	}
	g_strfreev (lines);
	return servers;
}

gchar *
liria_sde_servers_serialize (GPtrArray *servers)
{
	GString *text = g_string_new ("# Servers opened in Liria, most recent first (no passwords)\n");
	guint i;

	for (i = 0; i < servers->len; i++)
		g_string_append_printf (text, "%s\n", (const gchar *) servers->pdata[i]);
	return g_string_free (text, FALSE);
}

void
liria_sde_servers_push (GPtrArray *servers, const gchar *uri)
{
	gchar *root = liria_sde_servers_root (uri);
	gint found;

	if (root == NULL)
		return;
	found = servers_find (servers, root);
	if (found >= 0)
		g_ptr_array_remove_index (servers, (guint) found);
	g_ptr_array_insert (servers, 0, root);
	while (servers->len > SERVERS_MAX)
		g_ptr_array_remove_index (servers, servers->len - 1);
}

/* Recent servers that what the user is typing may stand for: the start
 * of the location ("smb://n", "\\nas\do") or of the computer's name. */
GPtrArray *
liria_sde_servers_matching (GPtrArray *servers, const gchar *typed)
{
	GPtrArray *found = g_ptr_array_new_with_free_func (g_free);
	LiriaSdeAddress *address;
	gchar *lower, *as_uri = NULL;
	guint i;

	if (typed == NULL || *typed == '\0')
		return found;
	lower = g_utf8_strdown (typed, -1);
	address = liria_sde_address_parse (typed);
	if (address->kind == LIRIA_SDE_ADDRESS_LOCATION && address->host == NULL &&
	    liria_sde_address_is_network_scheme (address->scheme)) {
		/* "smb:" or "\\": every Windows share */
		as_uri = g_strdup_printf ("%s://", address->scheme);
	} else if (address->kind == LIRIA_SDE_ADDRESS_LOCATION && address->uri != NULL) {
		as_uri = g_utf8_strdown (address->uri, -1);
		/* "smb://nas/" while typing "\\nas": the share is still to come */
		if (g_str_has_suffix (as_uri, "/") && !g_str_has_suffix (typed, "/") && !g_str_has_suffix (typed, "\\"))
			as_uri[strlen (as_uri) - 1] = '\0';
	}
	for (i = 0; i < servers->len; i++) {
		const gchar *server = servers->pdata[i];
		gchar *server_lower = g_utf8_strdown (server, -1);
		const gchar *host = strstr (server_lower, "://");
		gboolean match;

		host = host != NULL ? host + 3 : server_lower;
		if (strchr (host, '@') != NULL && strchr (host, '/') != NULL && strchr (host, '@') < strchr (host, '/'))
			host = strchr (host, '@') + 1;
		match = g_str_has_prefix (server_lower, lower) || (as_uri != NULL && g_str_has_prefix (server_lower, as_uri)) ||
		        (strchr (lower, ':') == NULL && strchr (lower, '/') == NULL && strchr (lower, '\\') == NULL &&
		         g_str_has_prefix (host, lower));
		if (match)
			g_ptr_array_add (found, g_strdup (server));
		g_free (server_lower);
	}
	g_free (as_uri);
	g_free (lower);
	liria_sde_address_free (address);
	return found;
}

gchar *
liria_sde_servers_path (void)
{
	return g_build_filename (g_get_user_config_dir (), "sde", "liria-servers", NULL);
}

GPtrArray *
liria_sde_servers_load (void)
{
	gchar *path = liria_sde_servers_path (), *text = NULL;
	GPtrArray *servers;

	g_file_get_contents (path, &text, NULL, NULL);
	servers = liria_sde_servers_parse (text);
	g_free (text);
	g_free (path);
	return servers;
}

/* Puts the server of |uri| first in the list; FALSE for local places. */
gboolean
liria_sde_servers_remember (const gchar *uri)
{
	gchar *root = liria_sde_servers_root (uri), *path, *dir, *text;
	GPtrArray *servers;
	gboolean ok;

	if (root == NULL)
		return FALSE;
	servers = liria_sde_servers_load ();
	if (servers->len > 0 && g_ascii_strcasecmp (servers->pdata[0], root) == 0) {
		g_ptr_array_unref (servers);
		g_free (root);
		return TRUE;
	}
	liria_sde_servers_push (servers, root);
	path = liria_sde_servers_path ();
	dir = g_path_get_dirname (path);
	g_mkdir_with_parents (dir, 0700);
	text = liria_sde_servers_serialize (servers);
	ok = g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
	g_free (text);
	g_free (dir);
	g_free (path);
	g_ptr_array_unref (servers);
	g_free (root);
	return ok;
}

/* A Secret Service on the session bus, running or startable. */
gboolean
liria_sde_keyring_available (void)
{
	GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
	gboolean found = FALSE;
	GVariant *reply;

	if (bus == NULL)
		return FALSE;
	reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                                     "org.freedesktop.DBus", "NameHasOwner",
	                                     g_variant_new ("(s)", "org.freedesktop.secrets"), G_VARIANT_TYPE ("(b)"),
	                                     G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
	if (reply != NULL) {
		g_variant_get (reply, "(b)", &found);
		g_variant_unref (reply);
	}
	if (!found) {
		reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		                                     "org.freedesktop.DBus", "ListActivatableNames", NULL,
		                                     G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
		if (reply != NULL) {
			const gchar **names = NULL;

			g_variant_get (reply, "(^a&s)", &names);
			found = names != NULL && g_strv_contains (names, "org.freedesktop.secrets");
			g_free (names);
			g_variant_unref (reply);
		}
	}
	g_object_unref (bus);
	return found;
}
