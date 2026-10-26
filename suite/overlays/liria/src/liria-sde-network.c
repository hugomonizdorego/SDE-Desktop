/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * Network places in Liria's windows (0.2.0 Alpha 3, see
 * liria-sde-network.h and liria-sde-address.c).
 *
 * Liria configures nothing about the protocols: GVfs and the system's
 * own settings decide (smb.conf and its client protocol limits, the
 * system and user ssh configuration and known_hosts, the proxy). Liria
 * only reads what the user typed, says what is missing when GVfs cannot
 * open a protocol here, signs in with its own dialog and remembers the
 * servers it opened (never their passwords; those stay in the keyring,
 * and only when the user asks for it).
 *
 * LIRIA_SDE_QA_NETWORK=FILE appends what Liria showed: "message" (under
 * the location entry), "open" (what the entry opened), "suggest" (the
 * suggestions), "sign-in" (the dialog and what it asked), "failure" (the
 * message and the protocols offered) and "remembered".
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "liria-sde-address.h"
#include "liria-sde-network.h"
#include "liria-window.h"

#define SDE_SUGGESTIONS_MAX 8

/* ------------------------------------------------------------------ QA */

static void G_GNUC_PRINTF (2, 3)
sde_qa (const char *kind, const char *format, ...)
{
    const char *path = g_getenv ("LIRIA_SDE_QA_NETWORK");
    va_list args;
    char *text;
    FILE *file;

    if (path == NULL || *path == '\0')
        return;
    va_start (args, format);
    text = g_strdup_vprintf (format, args);
    va_end (args);
    g_strdelimit (text, "\n", ' ');
    file = fopen (path, "a");
    if (file != NULL)
    {
        fprintf (file, "%s\t%s\n", kind, text);
        fclose (file);
    }
    g_free (text);
}

/* -------------------------------------------------------- availability */

gboolean
liria_sde_network_scheme_supported (const char *scheme)
{
    const gchar *const *schemes = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());

    return scheme != NULL && schemes != NULL && g_strv_contains (schemes, scheme);
}

/* GVfs itself (the gvfs package) is there: GIO uses its daemon. */
static gboolean
sde_gvfs_installed (void)
{
    return g_strcmp0 (G_OBJECT_TYPE_NAME (g_vfs_get_default ()), "GDaemonVfs") == 0;
}

char *
liria_sde_network_missing_message (const char *scheme)
{
    static const struct
    {
        const char *scheme;
        const char *backends; /* gvfs is there, gvfs-backends is not */
        const char *both;     /* neither */
    } messages[] = {
        { "smb", N_("Windows shares need the package gvfs-backends."),
          N_("Windows shares need the packages gvfs and gvfs-backends.") },
        { "sftp", N_("SSH (SFTP) locations need the package gvfs-backends."),
          N_("SSH (SFTP) locations need the packages gvfs and gvfs-backends.") },
        { "ftp", N_("FTP locations need the package gvfs-backends."),
          N_("FTP locations need the packages gvfs and gvfs-backends.") },
        { "ftps", N_("FTP locations need the package gvfs-backends."),
          N_("FTP locations need the packages gvfs and gvfs-backends.") },
        { "nfs", N_("NFS shares need the package gvfs-backends."),
          N_("NFS shares need the packages gvfs and gvfs-backends.") },
        { "dav", N_("WebDAV locations need the package gvfs-backends."),
          N_("WebDAV locations need the packages gvfs and gvfs-backends.") },
        { "davs", N_("WebDAV locations need the package gvfs-backends."),
          N_("WebDAV locations need the packages gvfs and gvfs-backends.") },
        { "afp", N_("Apple (AFP) shares need the package gvfs-backends."),
          N_("Apple (AFP) shares need the packages gvfs and gvfs-backends.") },
        { "network", N_("Browsing the network needs the package gvfs-backends."),
          N_("Browsing the network needs the packages gvfs and gvfs-backends.") },
    };
    gboolean gvfs = sde_gvfs_installed ();
    guint i;

    if (liria_sde_network_scheme_supported (scheme))
        return NULL;
    for (i = 0; i < G_N_ELEMENTS (messages); i++)
    {
        if (g_strcmp0 (messages[i].scheme, scheme) == 0)
            return g_strconcat (_(gvfs ? messages[i].backends : messages[i].both), " ",
                                gvfs ? _("It is not installed on this computer; ask your administrator to install it.")
                                     : _("They are not installed on this computer; ask your administrator to install them."),
                                NULL);
    }
    return NULL;
}

/* --------------------------------------------- passwords in an address */

/* A password typed in the address (smb://user:password@host/) is kept
 * here, never in the location, until the sign-in for that server asks
 * for it (once) or a minute has passed. */
static GHashTable *sde_typed_passwords = NULL;

static char *
sde_server_key (const char *uri)
{
    gchar *scheme = NULL, *user = NULL, *host = NULL, *key = NULL;
    gint port = -1;

    if (uri != NULL &&
        g_uri_split_with_user (uri, G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_NON_DNS, &scheme, &user, NULL, NULL,
                               &host, &port, NULL, NULL, NULL, NULL) && host != NULL)
    {
        char *text = g_strdup_printf ("%s://%s@%s:%d", scheme, user != NULL ? user : "", host, port);

        key = g_ascii_strdown (text, -1);
        g_free (text);
    }
    g_free (scheme);
    g_free (user);
    g_free (host);
    return key;
}

static void
sde_secret_free (gpointer data)
{
    char *secret = data;

    memset (secret, 0, strlen (secret));
    g_free (secret);
}

static gboolean
sde_forget_typed_password (gpointer key)
{
    if (sde_typed_passwords != NULL)
        g_hash_table_remove (sde_typed_passwords, key);
    return G_SOURCE_REMOVE;
}

static void
sde_keep_typed_password (const char *uri, const char *password)
{
    char *key = sde_server_key (uri);

    if (key == NULL)
        return;
    if (sde_typed_passwords == NULL)
        sde_typed_passwords = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, sde_secret_free);
    g_hash_table_replace (sde_typed_passwords, g_strdup (key), g_strdup (password));
    g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 60, sde_forget_typed_password, key, g_free);
}

static char *
sde_take_typed_password (const char *uri)
{
    char *key = sde_server_key (uri);
    gpointer stolen_key = NULL, password = NULL;

    if (key != NULL && sde_typed_passwords != NULL &&
        g_hash_table_steal_extended (sde_typed_passwords, key, &stolen_key, &password))
        g_free (stolen_key);
    g_free (key);
    return password;
}

/* ---------------------------------------------------- the entry message */

static void
sde_message_hide (GtkWidget *popover)
{
    if (gtk_widget_get_visible (popover))
        gtk_popover_popdown (GTK_POPOVER (popover));
}

static gboolean
sde_message_focus_out (GtkWidget *entry, GdkEvent *event, GtkWidget *popover)
{
    sde_message_hide (popover);
    return FALSE;
}

/* A short message under the location entry; the next key or click
 * closes it, and the text stays to be corrected. */
static void
sde_entry_message (GtkEntry *entry, const char *primary, const char *secondary)
{
    GtkWidget *popover = g_object_get_data (G_OBJECT (entry), "sde-network-message");
    GtkWidget *title, *detail;

    if (popover == NULL)
    {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *icon = gtk_image_new_from_icon_name ("dialog-information", GTK_ICON_SIZE_DND);

        popover = gtk_popover_new (GTK_WIDGET (entry));
        gtk_popover_set_modal (GTK_POPOVER (popover), FALSE);
        gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);
        gtk_style_context_add_class (gtk_widget_get_style_context (popover), "sde-network-message");
        gtk_container_set_border_width (GTK_CONTAINER (box), 10);
        gtk_widget_set_valign (icon, GTK_ALIGN_START);
        gtk_box_pack_start (GTK_BOX (box), icon, FALSE, FALSE, 0);
        title = gtk_label_new (NULL);
        detail = gtk_label_new (NULL);
        gtk_style_context_add_class (gtk_widget_get_style_context (title), "sde-network-message-title");
        gtk_style_context_add_class (gtk_widget_get_style_context (detail), "sde-network-message-detail");
        gtk_label_set_xalign (GTK_LABEL (title), 0);
        gtk_label_set_xalign (GTK_LABEL (detail), 0);
        gtk_label_set_line_wrap (GTK_LABEL (title), TRUE);
        gtk_label_set_line_wrap (GTK_LABEL (detail), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (title), 56);
        gtk_label_set_max_width_chars (GTK_LABEL (detail), 56);
        gtk_label_set_selectable (GTK_LABEL (detail), TRUE);
        gtk_box_pack_start (GTK_BOX (text), title, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (text), detail, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), text, TRUE, TRUE, 0);
        gtk_container_add (GTK_CONTAINER (popover), box);
        gtk_widget_show_all (box);
        g_object_set_data (G_OBJECT (popover), "sde-title", title);
        g_object_set_data (G_OBJECT (popover), "sde-detail", detail);
        g_object_set_data (G_OBJECT (entry), "sde-network-message", popover);
        g_signal_connect_object (entry, "changed", G_CALLBACK (sde_message_hide), popover, G_CONNECT_SWAPPED);
        g_signal_connect_object (entry, "focus-out-event", G_CALLBACK (sde_message_focus_out), popover, 0);
        g_signal_connect_object (entry, "destroy", G_CALLBACK (gtk_widget_destroy), popover, G_CONNECT_SWAPPED);
    }
    title = g_object_get_data (G_OBJECT (popover), "sde-title");
    detail = g_object_get_data (G_OBJECT (popover), "sde-detail");
    gtk_label_set_text (GTK_LABEL (title), primary);
    gtk_label_set_text (GTK_LABEL (detail), secondary != NULL ? secondary : "");
    gtk_widget_set_visible (detail, secondary != NULL && *secondary != '\0');
    gtk_popover_popup (GTK_POPOVER (popover));
    sde_qa ("message", "%s | %s", primary, secondary != NULL ? secondary : "");
}

/* --------------------------------------------------------- suggestions */

enum
{
    SUGGEST_MARKUP,
    SUGGEST_URI,
    SUGGEST_COLUMNS
};

static void
sde_suggest_add (GtkListStore *store, const char *what, const char *uri)
{
    GtkTreeIter iter;
    char *markup = g_markup_printf_escaped ("%s  <span alpha=\"60%%\">%s</span>", what, uri);

    gtk_list_store_append (store, &iter);
    gtk_list_store_set (store, &iter, SUGGEST_MARKUP, markup, SUGGEST_URI, uri, -1);
    g_free (markup);
}

/* Fills the suggestions for the text being typed: the protocols of a
 * computer (those GVfs can open here, Windows shares first), then the
 * recent servers it may stand for. Nothing for paths. */
static void
sde_suggest_refresh (GtkEntry *entry, GtkListStore *store)
{
    const char *text = gtk_entry_get_text (entry);
    LiriaSdeAddress *address;
    GPtrArray *servers, *found;
    GString *log;
    guint i, n = 0;

    gtk_list_store_clear (store);
    if (!gtk_widget_has_focus (GTK_WIDGET (entry)) || text == NULL || *text == '\0' ||
        g_object_get_data (G_OBJECT (entry), "sde-network-activating") != NULL)
        return;
    address = liria_sde_address_parse (text);
    log = g_string_new (NULL);
    if (address->kind == LIRIA_SDE_ADDRESS_HOST)
    {
        const char *const *schemes = liria_sde_address_host_schemes ();

        for (i = 0; schemes[i] != NULL; i++)
        {
            char *uri, *what;

            if (!liria_sde_network_scheme_supported (schemes[i]))
                continue;
            uri = i == 0 ? g_strdup (address->uri) : liria_sde_address_with_scheme (address, schemes[i]);
            what = g_strdup_printf (_("%s on %s"), liria_sde_address_scheme_label (schemes[i]), address->host);
            sde_suggest_add (store, what, uri);
            g_string_append_printf (log, "%s ", uri);
            g_free (what);
            g_free (uri);
            n++;
        }
    }
    if (address->kind == LIRIA_SDE_ADDRESS_HOST || address->kind == LIRIA_SDE_ADDRESS_LOCATION ||
        (address->kind == LIRIA_SDE_ADDRESS_PATH && address->uri != NULL && address->uri[0] != '/' &&
         address->uri[0] != '~'))
    {
        servers = liria_sde_servers_load ();
        found = liria_sde_servers_matching (servers, text);
        for (i = 0; i < found->len && n < SDE_SUGGESTIONS_MAX; i++, n++)
        {
            sde_suggest_add (store, _("Recent"), found->pdata[i]);
            g_string_append_printf (log, "%s ", (const char *) found->pdata[i]);
        }
        g_ptr_array_unref (found);
        g_ptr_array_unref (servers);
    }
    if (n > 0)
        sde_qa ("suggest", "%s", log->str);
    g_string_free (log, TRUE);
    liria_sde_address_free (address);
}

static gboolean
sde_suggest_match (GtkEntryCompletion *completion, const char *key, GtkTreeIter *iter, gpointer data)
{
    return TRUE; /* the list is made for the text being typed */
}

static gboolean
sde_activate_idle (gpointer data)
{
    GtkWidget *entry = data;

    if (gtk_widget_get_realized (entry))
        g_signal_emit_by_name (entry, "activate");
    g_object_unref (entry);
    return G_SOURCE_REMOVE;
}

static gboolean
sde_suggest_selected (GtkEntryCompletion *completion, GtkTreeModel *model, GtkTreeIter *iter, GtkEntry *entry)
{
    char *uri = NULL;

    gtk_tree_model_get (model, iter, SUGGEST_URI, &uri, -1);
    if (uri != NULL)
    {
        gtk_entry_set_text (entry, uri);
        gtk_editable_set_position (GTK_EDITABLE (entry), -1);
        g_idle_add (sde_activate_idle, g_object_ref (entry));
    }
    g_free (uri);
    return TRUE;
}

void
liria_sde_network_setup_entry (GtkEntry *entry)
{
    GtkEntryCompletion *completion = gtk_entry_completion_new ();
    GtkListStore *store = gtk_list_store_new (SUGGEST_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);
    GtkCellRenderer *cell = gtk_cell_renderer_text_new ();

    /* Before the completion's own handler, so it sees the new list. */
    g_signal_connect_object (entry, "changed", G_CALLBACK (sde_suggest_refresh), store, 0);
    gtk_entry_completion_set_model (completion, GTK_TREE_MODEL (store));
    gtk_entry_completion_set_match_func (completion, sde_suggest_match, NULL, NULL);
    gtk_entry_completion_set_minimum_key_length (completion, 1);
    gtk_entry_completion_set_popup_single_match (completion, TRUE);
    gtk_entry_completion_set_inline_completion (completion, FALSE);
    g_object_set (cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (completion), cell, TRUE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (completion), cell, "markup", SUGGEST_MARKUP);
    g_signal_connect (completion, "match-selected", G_CALLBACK (sde_suggest_selected), entry);
    gtk_entry_set_completion (entry, completion);
    g_object_set_data_full (G_OBJECT (entry), "sde-network-store", store, g_object_unref);
    g_object_unref (completion);
}

/* ---------------------------------------------------------- activation */

/* A plain name is a file or folder of the current folder when one is
 * there; in remote folders it stays a name in the folder, as before. */
static gboolean
sde_name_in_folder (const char *directory, const char *name)
{
    char *path;
    gboolean exists;

    if (directory == NULL || *directory == '\0')
        return FALSE;
    if (!g_path_is_absolute (directory))
        return TRUE;
    path = g_build_filename (directory, name, NULL);
    exists = g_file_test (path, G_FILE_TEST_EXISTS);
    g_free (path);
    return exists;
}

gboolean
liria_sde_network_entry_activate (GtkEntry *entry, const char *current_directory)
{
    LiriaSdeAddress *address = liria_sde_address_parse (gtk_entry_get_text (entry));
    gboolean stop = FALSE;
    char *missing;

    switch (address->kind)
    {
    case LIRIA_SDE_ADDRESS_EMPTY:
        break;
    case LIRIA_SDE_ADDRESS_PATH:
        if (g_strcmp0 (address->uri, gtk_entry_get_text (entry)) != 0)
            gtk_entry_set_text (entry, address->uri);
        break;
    case LIRIA_SDE_ADDRESS_ERROR:
        sde_entry_message (entry, _("This is not a location Liria can open."), address->message);
        stop = TRUE;
        break;
    case LIRIA_SDE_ADDRESS_HOST:
        if (address->ambiguous && sde_name_in_folder (current_directory, address->host))
            break;
        /* fall through */
    case LIRIA_SDE_ADDRESS_LOCATION:
        missing = liria_sde_network_missing_message (address->scheme);
        if (missing != NULL)
        {
            sde_entry_message (entry, _("This kind of location cannot be opened on this computer."), missing);
            g_free (missing);
            stop = TRUE;
            break;
        }
        if (address->password != NULL)
            sde_keep_typed_password (address->uri, address->password);
        if (address->kind == LIRIA_SDE_ADDRESS_HOST)
        {
            /* The failure message offers the other protocols. */
            g_object_set_data_full (G_OBJECT (gtk_widget_get_toplevel (GTK_WIDGET (entry))), "sde-network-typed-host",
                                    g_strdup (address->uri), g_free);
        }
        g_object_set_data (G_OBJECT (entry), "sde-network-activating", GINT_TO_POINTER (1));
        gtk_entry_set_text (entry, address->uri);
        g_object_set_data (G_OBJECT (entry), "sde-network-activating", NULL);
        sde_qa ("open", "%s", address->uri);
        break;
    }
    liria_sde_address_free (address);
    if (stop)
        g_signal_stop_emission_by_name (entry, "activate");
    return stop;
}

/* ------------------------------------------------------------- sign in */

typedef struct
{
    GMountOperation *op;
    GAskPasswordFlags flags;
    GtkWidget *dialog;
    GtkWidget *guest;
    GtkWidget *user;
    GtkWidget *domain;
    GtkWidget *password;
    GtkWidget *remember;
} SdeSignIn;

static void
sde_sign_in_guest_toggled (GtkToggleButton *guest, SdeSignIn *s)
{
    gboolean user = !gtk_toggle_button_get_active (guest);

    if (s->user != NULL)
        gtk_widget_set_sensitive (s->user, user);
    if (s->domain != NULL)
        gtk_widget_set_sensitive (s->domain, user);
    if (s->password != NULL)
        gtk_widget_set_sensitive (s->password, user);
    if (s->remember != NULL)
        gtk_widget_set_sensitive (s->remember, user);
}

static void
sde_sign_in_response (GtkDialog *dialog, int response, SdeSignIn *s)
{
    if (response == GTK_RESPONSE_OK)
    {
        if (s->guest != NULL && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (s->guest)))
        {
            g_mount_operation_set_anonymous (s->op, TRUE);
        }
        else
        {
            char *user = s->user != NULL ? g_strdup (gtk_entry_get_text (GTK_ENTRY (s->user))) : NULL;
            char *slash = user != NULL ? strchr (user, '\\') : NULL;

            g_mount_operation_set_anonymous (s->op, FALSE);
            /* DOMAIN\user, as Windows writes it */
            if (slash != NULL && s->domain != NULL)
            {
                *slash = '\0';
                gtk_entry_set_text (GTK_ENTRY (s->domain), user);
                memmove (user, slash + 1, strlen (slash + 1) + 1);
            }
            if (user != NULL)
                g_mount_operation_set_username (s->op, user);
            if (s->domain != NULL)
                g_mount_operation_set_domain (s->op, gtk_entry_get_text (GTK_ENTRY (s->domain)));
            if (s->password != NULL)
                g_mount_operation_set_password (s->op, gtk_entry_get_text (GTK_ENTRY (s->password)));
            g_mount_operation_set_password_save (s->op, s->remember != NULL &&
                                                 gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (s->remember))
                                                 ? G_PASSWORD_SAVE_PERMANENTLY : G_PASSWORD_SAVE_NEVER);
            g_free (user);
        }
        sde_qa ("sign-in", "answered remember=%d",
                s->remember != NULL && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (s->remember)));
        g_mount_operation_reply (s->op, G_MOUNT_OPERATION_HANDLED);
    }
    else
    {
        sde_qa ("sign-in", "cancelled");
        g_mount_operation_reply (s->op, G_MOUNT_OPERATION_ABORTED);
    }
    if (s->password != NULL)
        gtk_entry_set_text (GTK_ENTRY (s->password), "");
    gtk_widget_destroy (GTK_WIDGET (dialog));
    g_object_unref (s->op);
    g_free (s);
}

static GtkWidget *
sde_sign_in_row (GtkWidget *grid, int row, const char *label, GtkWidget *entry)
{
    GtkWidget *title = gtk_label_new_with_mnemonic (label);

    gtk_label_set_xalign (GTK_LABEL (title), 1);
    gtk_label_set_mnemonic_widget (GTK_LABEL (title), entry);
    gtk_style_context_add_class (gtk_widget_get_style_context (title), "dim-label");
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_grid_attach (GTK_GRID (grid), title, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), entry, 1, row, 1, 1);
    return entry;
}

static void
sde_password_peek (GtkEntry *entry, GtkEntryIconPosition position, GdkEvent *event, gpointer data)
{
    gboolean shown = !gtk_entry_get_visibility (entry);

    gtk_entry_set_visibility (entry, shown);
    gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY,
                                       shown ? "view-conceal-symbolic" : "view-reveal-symbolic");
}

/* The server and the share of the location being opened, for the title. */
static char *
sde_sign_in_title (GMountOperation *op)
{
    const char *uri = g_object_get_data (G_OBJECT (op), "sde-location");
    gchar *host = NULL, *path = NULL, *scheme = NULL, *title;

    if (uri == NULL ||
        !g_uri_split (uri, G_URI_FLAGS_NON_DNS, &scheme, NULL, &host, NULL, &path, NULL, NULL, NULL) ||
        host == NULL || *host == '\0')
        title = g_strdup (_("Sign in"));
    else
    {
        char **parts = g_strsplit (path != NULL ? path : "", "/", 3);
        const char *share = parts[0] != NULL && parts[1] != NULL ? parts[1] : "";

        if (g_strcmp0 (scheme, "smb") == 0 && *share != '\0')
            title = g_strdup_printf (_("Sign in to “%s” on %s"), share, host);
        else
            title = g_strdup_printf (_("Sign in to %s"), host);
        g_strfreev (parts);
    }
    g_free (scheme);
    g_free (host);
    g_free (path);
    return title;
}

static void
sde_ask_password (GMountOperation *op, const char *message, const char *default_user, const char *default_domain,
                  GAskPasswordFlags flags, gpointer data)
{
    GtkWindow *parent = gtk_mount_operation_get_parent (GTK_MOUNT_OPERATION (op));
    const char *uri = g_object_get_data (G_OBJECT (op), "sde-location");
    int asked = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (op), "sde-asked"));
    char *scheme = uri != NULL ? g_uri_parse_scheme (uri) : NULL;
    char *typed, *title;
    gboolean keyring;
    GtkWidget *content, *header, *icon, *texts, *label, *grid;
    SdeSignIn *s;
    int row = 0;

    /* Liria's own dialog instead of the generic one of GTK. */
    g_signal_stop_emission_by_name (op, "ask-password");
    g_object_set_data (G_OBJECT (op), "sde-asked", GINT_TO_POINTER (asked + 1));

    /* A password typed in the address is tried first, once. */
    typed = asked == 0 && (flags & G_ASK_PASSWORD_NEED_PASSWORD) ? sde_take_typed_password (uri) : NULL;
    if (typed != NULL)
    {
        if (default_user != NULL && (flags & G_ASK_PASSWORD_NEED_USERNAME))
            g_mount_operation_set_username (op, default_user);
        if (default_domain != NULL && (flags & G_ASK_PASSWORD_NEED_DOMAIN))
            g_mount_operation_set_domain (op, default_domain);
        g_mount_operation_set_password (op, typed);
        g_mount_operation_set_password_save (op, G_PASSWORD_SAVE_NEVER);
        sde_secret_free (typed);
        sde_qa ("sign-in", "password from the address");
        g_mount_operation_reply (op, G_MOUNT_OPERATION_HANDLED);
        g_free (scheme);
        return;
    }

    s = g_new0 (SdeSignIn, 1);
    s->op = g_object_ref (op);
    s->flags = flags;
    keyring = (flags & G_ASK_PASSWORD_SAVING_SUPPORTED) && liria_sde_keyring_available ();
    s->dialog = gtk_dialog_new_with_buttons (_("Sign In"), parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                             _("_Cancel"), GTK_RESPONSE_CANCEL, _("C_onnect"), GTK_RESPONSE_OK,
                                             NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (s->dialog), GTK_RESPONSE_OK);
    gtk_style_context_add_class (gtk_widget_get_style_context (gtk_dialog_get_widget_for_response (
                                     GTK_DIALOG (s->dialog), GTK_RESPONSE_OK)), "suggested-action");
    gtk_style_context_add_class (gtk_widget_get_style_context (s->dialog), "sde-sign-in");
    gtk_window_set_resizable (GTK_WINDOW (s->dialog), FALSE);
    content = gtk_dialog_get_content_area (GTK_DIALOG (s->dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content), 14);
    gtk_box_set_spacing (GTK_BOX (content), 12);

    header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    icon = gtk_image_new_from_icon_name ("folder-remote", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign (icon, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (header), icon, FALSE, FALSE, 0);
    texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    title = sde_sign_in_title (op);
    label = gtk_label_new (title);
    gtk_style_context_add_class (gtk_widget_get_style_context (label), "sde-sign-in-title");
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 40);
    gtk_box_pack_start (GTK_BOX (texts), label, FALSE, FALSE, 0);
    if (asked > 0)
    {
        label = gtk_label_new (_("The user name or the password was not accepted. Try again."));
        gtk_style_context_add_class (gtk_widget_get_style_context (label), "sde-sign-in-error");
    }
    else
    {
        label = gtk_label_new (message != NULL ? message : "");
        gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
    }
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 48);
    gtk_box_pack_start (GTK_BOX (texts), label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (header), texts, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (content), header, FALSE, FALSE, 0);

    if (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED)
    {
        GtkWidget *user_choice = gtk_radio_button_new_with_mnemonic (NULL, _("As a _registered user"));
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);

        s->guest = gtk_radio_button_new_with_mnemonic_from_widget (GTK_RADIO_BUTTON (user_choice),
                                                                   _("As a _guest"));
        gtk_box_pack_start (GTK_BOX (box), user_choice, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), s->guest, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (content), box, FALSE, FALSE, 0);
        g_signal_connect (s->guest, "toggled", G_CALLBACK (sde_sign_in_guest_toggled), s);
    }

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
    if (flags & G_ASK_PASSWORD_NEED_USERNAME)
    {
        s->user = sde_sign_in_row (grid, row++, _("_User name"), gtk_entry_new ());
        gtk_entry_set_text (GTK_ENTRY (s->user), default_user != NULL ? default_user : g_get_user_name ());
    }
    if (flags & G_ASK_PASSWORD_NEED_DOMAIN)
    {
        s->domain = sde_sign_in_row (grid, row++, _("_Domain"), gtk_entry_new ());
        gtk_entry_set_text (GTK_ENTRY (s->domain), default_domain != NULL ? default_domain : "");
    }
    if (flags & G_ASK_PASSWORD_NEED_PASSWORD)
    {
        s->password = sde_sign_in_row (grid, row++, _("_Password"), gtk_entry_new ());
        gtk_entry_set_visibility (GTK_ENTRY (s->password), FALSE);
        gtk_entry_set_input_purpose (GTK_ENTRY (s->password), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_icon_from_icon_name (GTK_ENTRY (s->password), GTK_ENTRY_ICON_SECONDARY,
                                           "view-reveal-symbolic");
        gtk_entry_set_icon_tooltip_text (GTK_ENTRY (s->password), GTK_ENTRY_ICON_SECONDARY,
                                         _("Show or hide the password"));
        g_signal_connect (s->password, "icon-press", G_CALLBACK (sde_password_peek), NULL);
    }
    gtk_box_pack_start (GTK_BOX (content), grid, FALSE, FALSE, 0);

    if ((flags & G_ASK_PASSWORD_NEED_PASSWORD) && keyring)
    {
        s->remember = gtk_check_button_new_with_mnemonic (_("_Remember the password in the keyring"));
        gtk_box_pack_start (GTK_BOX (content), s->remember, FALSE, FALSE, 0);
    }
    else if ((flags & G_ASK_PASSWORD_NEED_PASSWORD) && (flags & G_ASK_PASSWORD_SAVING_SUPPORTED))
    {
        label = gtk_label_new (_("Passwords can be remembered when a keyring (gnome-keyring) runs in the session."));
        gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
        gtk_label_set_xalign (GTK_LABEL (label), 0);
        gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (label), 48);
        gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);
    }
    if (liria_sde_address_is_plain_text (scheme) && (flags & G_ASK_PASSWORD_NEED_PASSWORD))
    {
        label = gtk_label_new (g_strcmp0 (scheme, "ftp") == 0
                               ? _("FTP is not encrypted: the password and the files can be read on the network. "
                                   "Use SSH (sftp://) or FTPS where the server offers it.")
                               : _("This WebDAV address is not encrypted: the password and the files can be read "
                                   "on the network. Use davs:// where the server offers it."));
        gtk_style_context_add_class (gtk_widget_get_style_context (label), "sde-sign-in-warning");
        gtk_label_set_xalign (GTK_LABEL (label), 0);
        gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (label), 48);
        gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);
    }

    g_signal_connect (s->dialog, "response", G_CALLBACK (sde_sign_in_response), s);
    gtk_widget_show_all (s->dialog);
    if (s->user != NULL && *gtk_entry_get_text (GTK_ENTRY (s->user)) == '\0')
        gtk_widget_grab_focus (s->user);
    else if (s->password != NULL)
        gtk_widget_grab_focus (s->password);
    sde_qa ("sign-in", "%s | user=%d domain=%d password=%d guest=%d keyring=%d plain=%d retry=%d", title,
            s->user != NULL, s->domain != NULL, s->password != NULL, s->guest != NULL, s->remember != NULL,
            liria_sde_address_is_plain_text (scheme), asked > 0);
    g_free (title);
    g_free (scheme);
}

GMountOperation *
liria_sde_network_mount_operation_new (GtkWindow *parent, GFile *location)
{
    GMountOperation *op = gtk_mount_operation_new (parent);

    g_mount_operation_set_password_save (op, G_PASSWORD_SAVE_NEVER);
    if (location != NULL)
        g_object_set_data_full (G_OBJECT (op), "sde-location", g_file_get_uri (location), g_free);
    g_signal_connect (op, "ask-password", G_CALLBACK (sde_ask_password), NULL);
    return op;
}

/* -------------------------------------------------- shown and failures */

void
liria_sde_network_location_shown (GFile *location)
{
    char *uri = g_file_get_uri (location);

    if (liria_sde_servers_remember (uri))
        sde_qa ("remembered", "%s", uri);
    g_free (uri);
}

typedef struct
{
    GtkWidget *window;
    char **choices; /* the location of each protocol button */
} SdeFailure;

static void
sde_failure_response (GtkDialog *dialog, int response, SdeFailure *f)
{
    if (response >= 0 && f->choices != NULL && (guint) response < g_strv_length (f->choices) &&
        f->window != NULL)
    {
        GFile *location = g_file_new_for_uri (f->choices[response]);

        sde_qa ("open", "%s", f->choices[response]);
        liria_window_go_to (LIRIA_WINDOW (f->window), location);
        g_object_unref (location);
    }
    gtk_widget_destroy (GTK_WIDGET (dialog));
    if (f->window != NULL)
        g_object_remove_weak_pointer (G_OBJECT (f->window), (gpointer *) &f->window);
    g_strfreev (f->choices);
    g_free (f);
}

gboolean
liria_sde_network_show_failure (GtkWidget *window, GFile *location, GError *error)
{
    char *uri = g_file_get_uri (location), *scheme = g_file_get_uri_scheme (location);
    const char *typed_host = g_object_get_data (G_OBJECT (window), "sde-network-typed-host");
    gchar *host = NULL, *path = NULL;
    char *primary = NULL, *secondary = NULL, *missing;
    gboolean server_root, offer, ambiguous = FALSE;
    GPtrArray *choices = g_ptr_array_new_with_free_func (g_free);
    GtkWidget *dialog;
    GString *offered = g_string_new (NULL);
    SdeFailure *f;
    guint i;

    if (error == NULL || (!liria_sde_address_is_network_scheme (scheme) && g_strcmp0 (scheme, "network") != 0))
    {
        g_free (uri);
        g_free (scheme);
        g_ptr_array_unref (choices);
        g_string_free (offered, TRUE);
        return FALSE;
    }
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
        goto done;
    g_uri_split (uri, G_URI_FLAGS_NON_DNS, NULL, NULL, &host, NULL, &path, NULL, NULL, NULL);
    server_root = host != NULL && *host != '\0' && (path == NULL || *path == '\0' || strcmp (path, "/") == 0);
    if (typed_host != NULL && g_strcmp0 (typed_host, uri) == 0)
    {
        LiriaSdeAddress *address = liria_sde_address_parse (typed_host);

        ambiguous = address->ambiguous;
        liria_sde_address_free (address);
    }

    missing = liria_sde_network_missing_message (scheme);
    if (missing != NULL)
    {
        primary = g_strdup (_("This kind of location cannot be opened on this computer."));
        secondary = missing;
    }
    else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND))
    {
        primary = g_strdup_printf (_("No computer called “%s” was found."), host);
        secondary = g_strdup (ambiguous ? _("There is no file or folder of that name in this folder either. Check "
                                            "the name, or type the computer's IP address.")
                                        : _("Check the name, or type the computer's IP address."));
    }
    else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    {
        primary = g_strdup_printf (_("Access to “%s” was denied."), host != NULL ? host : uri);
        secondary = g_strdup (_("Check the user name and the password, or ask the administrator of the server "
                                "for access."));
    }
    else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    {
        primary = g_strdup_printf (_("“%s” did not answer in time."), host);
        secondary = g_strdup (error->message);
    }
    else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) && !server_root)
    {
        primary = g_strdup_printf (_("“%s” was not found on %s."), path, host);
        secondary = g_strdup (_("Check the name of the share and of the folder."));
    }
    else if (host != NULL && *host != '\0')
    {
        primary = g_strdup_printf (_("%s on %s cannot be opened."), liria_sde_address_scheme_label (scheme), host);
        secondary = g_strdup (error->message);
    }
    else
    {
        primary = g_strdup_printf (_("“%s” cannot be opened."), uri);
        secondary = g_strdup (error->message);
    }

    /* A computer that was typed: the other ways to reach it. */
    offer = missing == NULL && server_root && typed_host != NULL && g_strcmp0 (typed_host, uri) == 0 &&
            !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND);
    dialog = gtk_message_dialog_new (GTK_WINDOW (window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                     GTK_BUTTONS_NONE, "%s", primary);
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s%s", secondary,
                                              offer ? _("\n\nTry another way to connect to it:") : "");
    gtk_window_set_title (GTK_WINDOW (dialog), _("Cannot Open the Location"));
    if (offer)
    {
        LiriaSdeAddress *address = liria_sde_address_parse (typed_host);
        const char *const *schemes = liria_sde_address_host_schemes ();

        for (i = 0; schemes[i] != NULL; i++)
        {
            char *alternative;

            if (g_strcmp0 (schemes[i], scheme) == 0 || !liria_sde_network_scheme_supported (schemes[i]))
                continue;
            alternative = liria_sde_address_with_scheme (address, schemes[i]);
            if (alternative == NULL)
                continue;
            gtk_dialog_add_button (GTK_DIALOG (dialog), liria_sde_address_scheme_label (schemes[i]), choices->len);
            g_string_append_printf (offered, "%s ", alternative);
            g_ptr_array_add (choices, alternative);
        }
        liria_sde_address_free (address);
    }
    gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Close"), GTK_RESPONSE_CLOSE);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_CLOSE);
    g_ptr_array_add (choices, NULL);
    f = g_new0 (SdeFailure, 1);
    f->window = window;
    g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &f->window);
    f->choices = (char **) g_ptr_array_free (choices, FALSE);
    choices = NULL;
    g_signal_connect (dialog, "response", G_CALLBACK (sde_failure_response), f);
    gtk_widget_show (dialog);
    sde_qa ("failure", "%s | %s | %s", primary, secondary, offered->str);

done:
    if (window != NULL)
        g_object_set_data (G_OBJECT (window), "sde-network-typed-host", NULL);
    if (choices != NULL)
        g_ptr_array_unref (choices);
    g_string_free (offered, TRUE);
    g_free (primary);
    g_free (secondary);
    g_free (host);
    g_free (path);
    g_free (uri);
    g_free (scheme);
    return TRUE;
}
