/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Network places typed in Liria's location bar (0.2.0 Alpha 3): what the
 * user typed made into a location (smb://, \\server\share, an IP address
 * or computer name, localhost, sftp://, ftp://, nfs://, dav(s)://, smb:)
 * and the list of recent servers. No widgets and no I/O except the
 * recent server file; the window part is liria-sde-network.c. Shared with
 * the unit test (sde-tools/test-liria-sde-address.c).
 */
#ifndef LIRIA_SDE_ADDRESS_H
#define LIRIA_SDE_ADDRESS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
	LIRIA_SDE_ADDRESS_EMPTY,
	LIRIA_SDE_ADDRESS_PATH,     /* a local path, or a name in the current folder */
	LIRIA_SDE_ADDRESS_LOCATION, /* a location URI, cleaned up */
	LIRIA_SDE_ADDRESS_HOST,     /* a computer: its Windows shares, other protocols offered */
	LIRIA_SDE_ADDRESS_ERROR     /* cannot be a location; message says why */
} LiriaSdeAddressKind;

typedef struct {
	LiriaSdeAddressKind kind;
	gchar *uri;         /* LOCATION, HOST: what to open, never with a password;
	                     * PATH: the path as typed, without quotes around it */
	gchar *scheme;      /* LOCATION, HOST: lower case, aliases resolved (ssh: is sftp) */
	gchar *host;        /* network locations: the server, without [ ]; NULL for smb:// */
	gchar *user;        /* a user name typed in the address (smb: DOMAIN;user) */
	gchar *password;    /* a password typed in the address, taken out of uri */
	gint port;          /* 0: the protocol's own */
	gboolean ambiguous; /* HOST: a plain name, which may also be a file in the folder */
	gchar *message;     /* ERROR: for the user */
} LiriaSdeAddress;

G_GNUC_INTERNAL LiriaSdeAddress *liria_sde_address_parse (const gchar *text);
G_GNUC_INTERNAL void liria_sde_address_free (LiriaSdeAddress *address);

/* smb, sftp, ftp, ftps, nfs, dav, davs, afp: places on other computers. */
G_GNUC_INTERNAL gboolean liria_sde_address_is_network_scheme (const gchar *scheme);
/* ftp and dav send passwords and files as plain text. */
G_GNUC_INTERNAL gboolean liria_sde_address_is_plain_text (const gchar *scheme);
/* "Windows shares (SMB)", "SSH (SFTP)", ... */
G_GNUC_INTERNAL const gchar *liria_sde_address_scheme_label (const gchar *scheme);
/* The same computer reached with another protocol: sftp://user@host/ */
G_GNUC_INTERNAL gchar *liria_sde_address_with_scheme (const LiriaSdeAddress *address, const gchar *scheme);
/* The protocols offered for a computer, the default (smb) first. */
G_GNUC_INTERNAL const gchar *const *liria_sde_address_host_schemes (void);

/* Recent servers: ~/.config/sde/liria-servers, one location per line
 * (the server, for Windows shares the share), most recent first. */
G_GNUC_INTERNAL gchar *liria_sde_servers_root (const gchar *uri);
G_GNUC_INTERNAL GPtrArray *liria_sde_servers_parse (const gchar *text);
G_GNUC_INTERNAL gchar *liria_sde_servers_serialize (GPtrArray *servers);
G_GNUC_INTERNAL void liria_sde_servers_push (GPtrArray *servers, const gchar *uri);
G_GNUC_INTERNAL GPtrArray *liria_sde_servers_matching (GPtrArray *servers, const gchar *typed);
G_GNUC_INTERNAL gchar *liria_sde_servers_path (void);
G_GNUC_INTERNAL GPtrArray *liria_sde_servers_load (void);
G_GNUC_INTERNAL gboolean liria_sde_servers_remember (const gchar *uri);

/* A Secret Service (gnome-keyring, KeePassXC, ...) runs or can be started
 * on the session bus, so GVfs can keep a password that the user wants
 * remembered. */
G_GNUC_INTERNAL gboolean liria_sde_keyring_available (void);

G_END_DECLS

#endif
