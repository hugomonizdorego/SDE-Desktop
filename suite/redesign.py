# SPDX-License-Identifier: GPL-3.0-or-later
"""SDE redesign of the native suite forks.

Applied by tools/prepare-native-suite.py after the namespace rename. Every
change is a small, checked text edit or an added file from suite/overlays.
Copyright notices and license texts of the original authors are never
removed: they stay in every source file, in the About window's license
section and under /usr/share/doc/sde/upstream. A missing anchor stops
preparation instead of silently skipping a change.
"""
import re
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OVERLAYS = HERE / "overlays"
# SDE-owned files copied into the Liria tree (man page shared with CMake).
SHARED = [ROOT / "data/man/liria.1"]
# Original product names that must not reach users. Legal notices
# (copyright lines, license files, the About license text and the
# installed upstream documentation) are the only allowed exception.
BRAND_WORDS = re.compile(r"\bMATE\b|\bCaja\b|\bCAJA\b|mate-desktop\.org|mate-user-guide")
LEGAL_FILE = re.compile(r"^(COPYING|LICENSE|AUTHORS|COPYRIGHT|THANKS|TRADEMARK|ChangeLog|NEWS|SDE-)", re.I)


def inputs():
    """Files whose content changes the prepared sources (for the lock hash)."""
    return [Path(__file__)] + sorted(p for p in OVERLAYS.rglob("*") if p.is_file()) + SHARED


def edit_once(path, old, new):
    """Like edit(), but the anchor must occur exactly once."""
    if path.read_text().count(old) != 1:
        raise ValueError("Redesign anchor not unique in %s: %r" % (path, old[:60]))
    edit(path, old, new)


def edit(path, old, new, count=1):
    text = path.read_text()
    if text.count(old) < 1:
        raise ValueError("Redesign anchor not found in %s: %r" % (path, old[:60]))
    path.write_text(text.replace(old, new, count))


def edit_all(path, old, new):
    text = path.read_text()
    if old not in text:
        raise ValueError("Redesign anchor not found in %s: %r" % (path, old[:60]))
    path.write_text(text.replace(old, new))


def replace_between(path, start, end, new):
    """Replaces the text from start (inclusive) to end (exclusive)."""
    text = path.read_text()
    if text.count(start) != 1 or text.count(end) < 1:
        raise ValueError("Redesign region not found in %s: %r" % (path, start[:60]))
    a = text.index(start)
    b = text.index(end, a)
    path.write_text(text[:a] + new + text[b:])


def overlay(record_id, staged):
    source = OVERLAYS / record_id
    if not source.is_dir():
        return
    for path in source.rglob("*"):
        if path.is_file():
            target = staged / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def brand_leaks(text):
    """Original product names in text, outside copyright lines."""
    return [line for line in text.splitlines()
            if BRAND_WORDS.search(line) and not re.search(r"copyright|\(c\)|©|modified version of the Caja", line, re.I)]


def liria(staged):
    """alpha16: Liria re-forked from Caja 1.28.0 with complete SDE branding
    and a new window layout (suite/overlays/liria/src/liria-sde-ui.c): a
    compact header with 16 px symbolic icons, one tidy main menu, linked
    view buttons, grouped context menus with Send To, a symbolic sidebar,
    Liria's own theme-neutral CSS, local help and the keyboard shortcuts
    window. The alpha14/15 work stays: Home overview, USB safe copy engine,
    clipboard fixes, safe removal and the SDE Tools extension."""
    edit(staged / "Makefile.am", "\tsrc \\\n", "\tsrc \\\n\tsde-tools \\\n")
    edit(staged / "configure.ac", "src/file-manager/Makefile\n", "src/file-manager/Makefile\nsde-tools/Makefile\n")
    liria_branding(staged)
    liria_header(staged)
    liria_view_switcher(staged)
    liria_history_buttons(staged)
    liria_context_menus(staged)
    liria_home(staged)
    liria_sidebar(staged)
    liria_quick_access(staged)
    liria_sidebar_pinned(staged)
    liria_home_view(staged)
    liria_ribbon(staged)
    liria_right_drag(staged)
    liria_pathbar(staged)
    liria_copy_engine(staged)
    liria_safe_remove(staged)
    liria_clipboard(staged)
    liria_translations(staged)


# ------------------------------------------------------------------ brand

HELP_SECTIONS = {
    "gosliria-1": "", "gosliria-36": "bookmarks", "gosliria-50": "backgrounds",
    "gosliria-51": "properties", "liria-server-connect": "connect-server",
    "liria-open-location": "open-location", "liria-select-pattern": "select-pattern",
    "gosliria-438": "views", "gosliria-56": "behavior", "gosliria-439": "display",
    "gosliria-490": "list-columns", "gosliria-60": "preview", "gosliria-61": "media",
}


def _text_files(staged):
    for path in staged.rglob("*"):
        if not path.is_file() or path.is_symlink() or LEGAL_FILE.match(path.name):
            continue
        if path.suffix in (".po", ".pot", ".png", ".svg", ".pdf", ".sxw", ".dia", ".gmo", ".mo"):
            continue
        try:
            yield path, path.read_text()
        except (UnicodeDecodeError, ValueError):
            continue


def liria_branding(staged):
    """No MATE or Caja names where users see them: desktop entries,
    AppStream data, man pages, help links, About, --version, D-Bus service
    file names, icon names, terminal fallbacks and build metadata. Legal
    notices of the original authors are kept (see liria_sde_ui_show_about
    and /usr/share/doc/sde/upstream/caja)."""
    # Files whose names carried the old product name.
    for old, new in [("data/mate-network-scheme.desktop.in.in", "data/liria-network-scheme.desktop.in.in"),
                     ("data/org.mate.freedesktop.FileManager1.service.in",
                      "data/org.debion.Liria.FileManager1.service.in")]:
        (staged / old).rename(staged / new)
    for path, text in _text_files(staged):
        changed = text.replace("mate-network-scheme.desktop", "liria-network-scheme.desktop")
        changed = changed.replace("org.mate.freedesktop.FileManager1.service", "org.debion.Liria.FileManager1.service")
        if changed != text:
            path.write_text(changed)

    # Desktop entries: SDE only, no MATE keywords or session keys.
    for path in sorted((staged / "data").glob("*.desktop.in.in")):
        text = path.read_text()
        text = re.sub(r"(?m)^Keywords=(.*?)MATE;", r"Keywords=\1", text)
        text = re.sub(r"(?m)^OnlyShowIn=.*$", "OnlyShowIn=SDE;", text)
        text = re.sub(r"(?m)^X-MATE-[^\n]*\n", "", text)
        text = text.replace("X-MATE-PersonalSettings;", "X-SDE-Settings;")
        text = text.replace("application/x-mate-saved-search;", "application/x-liria-saved-search;")
        path.write_text(text)

    appdata = staged / "data/liria.appdata.xml.in.in"
    replace_between(appdata, " <summary>", "</component>",
                    " <summary>The file manager of SDE</summary>\n"
                    " <description>\n  <p>\n   Liria browses and organizes the files and folders on your computer,\n"
                    "   removable drives and the network. It has tabs, an extra pane, a Home\n"
                    "   overview with quick access, drives and recent files, safe copying to USB\n"
                    "   drives and extensions for archives and terminals.\n  </p>\n"
                    "  <p>\n   Liria is part of SDE for DebION GNU/Linux, developed by the DebION Dev-Team.\n  </p>\n"
                    " </description>\n"
                    " <developer_name>DebION Dev-Team</developer_name>\n"
                    " <url type=\"homepage\">@PACKAGE_URL@</url>\n"
                    " <url type=\"bugtracker\">@PACKAGE_BUGREPORT@</url>\n"
                    " <project_group>SDE</project_group>\n")

    configure = staged / "configure.ac"
    edit_once(configure, "[https://github.com/mate-desktop/caja/issues]",
              "[https://github.com/hugomonizdorego/SDE-Desktop/issues]")
    edit_once(configure, "[https://mate-desktop.org])", "[https://github.com/hugomonizdorego/SDE-Desktop])")

    # The saved-search MIME type is Liria's own.
    for rel in ("data/liria.xml.in", "libliria-private/liria-file-utilities.h"):
        edit_all(staged / rel, "application/x-mate-saved-search", "application/x-liria-saved-search")

    # --version, the session check and desktop entry filtering.
    app = staged / "src/liria-application.c"
    edit_once(app, 'g_print ("MATE liria " ', 'g_print ("Liria " ')
    replace_between(app, "static gboolean\nrunning_in_mate (void)\n{", "static gboolean\nrunning_as_root (void)",
                    "static gboolean\nrunning_in_mate (void)\n{\n"
                    "    /* SDE owns the desktop and the session; Liria never manages them\n"
                    "     * unless LIRIA_MANAGE_DESKTOP=1 is set for testing. */\n"
                    "    return g_strcmp0 (g_getenv (\"LIRIA_MANAGE_DESKTOP\"), \"1\") == 0;\n}\n\n")
    egg = staged / "mate-submodules/libegg"
    replace_between(egg / "eggsmclient.c", "static gboolean\nrunning_in_mate (void)\n{", "static void\negg_sm_client_init",
                    "static gboolean\nrunning_in_mate (void)\n{\n"
                    "    return g_strcmp0 (g_getenv (\"LIRIA_MANAGE_DESKTOP\"), \"1\") == 0; /* SDE */\n}\n\n")
    edit_once(egg / "eggsmclient-xsmp.c", '"X-MATE-AutoRestart"', '"X-SDE-AutoRestart"')
    link = staged / "libliria-private/liria-link.c"
    edit_all(link, 'string_array_contains (only_show_in, "MATE")', 'string_array_contains (only_show_in, "SDE")')
    edit_all(link, 'string_array_contains (not_show_in, "MATE")', 'string_array_contains (not_show_in, "SDE")')
    for old, new in [("mate-fs-executable", "application-x-executable"), ("mate-dev-symlink", "emblem-symbolic-link"),
                     ("mate-dev-harddisk", "drive-harddisk"), ("mate-fs-web", "text-html"),
                     ("mate-fs-regular", "text-x-generic")]:
        edit_once(link, '"%s"' % old, '"%s"' % new)
    edit_once(staged / "src/file-manager/fm-directory-view.c", '"mate-fs-bookmark"', '"user-bookmarks"')
    edit_once(staged / "eel/eel-mate-extensions.c", '{ "mate-terminal", "-x",', '{ "x-terminal-emulator", "-e",')

    # Help: local SDE help (data/help/index.html) instead of the MATE guide.
    for path, text in _text_files(staged / "src"):
        changed = re.sub(r'"help:mate-user-guide(?:/([\w-]+))?"',
                         lambda m: '"file://" LIRIA_DATADIR "/help/index.html%s"'
                         % (("#" + HELP_SECTIONS[m[1]]) if m[1] and HELP_SECTIONS.get(m[1]) else ""), text)
        if changed != text:
            path.write_text(changed)
    prefs = staged / "src/liria-file-management-properties.c"
    edit_once(prefs, 'g_strdup_printf ("help:%s/%s", helpfile, sect_id)',
              'g_strdup_printf ("file://%s/help/index.html#%s", helpfile, sect_id)')
    edit_once(prefs, 'preferences_show_help (GTK_WINDOW (parent), "mate-user-guide", section);',
              'preferences_show_help (GTK_WINDOW (parent), LIRIA_DATADIR, section);')
    for old, new in HELP_SECTIONS.items():
        if new and '"%s"' % old in prefs.read_text():
            edit_once(prefs, 'section = "%s";' % old, 'section = "%s";' % new)
    data = staged / "data/Makefile.am"
    edit_once(data, "liriadatadir = $(datadir)/liria\n",
              "liriahelpdir = $(datadir)/liria/help\nliriahelp_DATA = help/index.html\n\n"
              "liriadatadir = $(datadir)/liria\n")

    # About: SDE developer, version and site; original notices in the
    # license section (liria-sde-ui.c).
    menus = staged / "src/liria-window-menus.c"
    edit_once(menus, '#include "liria-window-private.h"\n', '#include "liria-window-private.h"\n#include "liria-sde-ui.h"\n')
    replace_between(menus, "static void\naction_about_liria_callback (GtkAction *action,",
                    "static void\naction_up_callback (GtkAction *action,",
                    "static void\naction_about_liria_callback (GtkAction *action,\n"
                    "                             gpointer user_data)\n{\n"
                    "    liria_sde_ui_show_about (GTK_WINDOW (user_data));\n}\n\n")

    edit_once(staged / "data/liria-file-management-properties.ui",
              '<property name="title" translatable="yes">File Management Preferences</property>',
              '<property name="title" translatable="yes">Liria Preferences</property>')

    # Only the authors list is read (About > Credits > Original authors);
    # the full author and documenter lists stay in AUTHORS and the docs.
    about = staged / "src/liria.about"
    about.write_text(re.sub(r"(?m)^Documenters=.*\n", "", about.read_text()))

    # Man pages: SDE's own (data/man/liria.1 in the SDE tree) and short
    # pages for the helper programs.
    docs = staged / "docs"
    shutil.copy2(ROOT / "data/man/liria.1", docs / "liria.1")
    helpers = {
        "liria-autorun-software": ("run software found on inserted media",
                                   "Asks whether to run the autorun program of a newly inserted medium."),
        "liria-connect-server": ("connect to a server",
                                 "Opens the Connect to Server dialog of Liria for SFTP, FTP, SMB and WebDAV locations."),
        "liria-file-management-properties": ("Liria preferences",
                                             "Opens the Liria preferences window."),
    }
    for name, (summary, description) in helpers.items():
        (docs / (name + ".1")).write_text(
            ".\\\" SPDX-License-Identifier: GPL-2.0-or-later\n"
            ".TH %s 1 \"October 2026\" \"SDE\" \"User Commands\"\n.SH NAME\n%s \\- %s\n"
            ".SH SYNOPSIS\n.B %s\n.SH DESCRIPTION\n%s\nIt is part of the Liria file manager of SDE,\n"
            "developed by the DebION Dev-Team.\n.SH SEE ALSO\n.BR liria (1)\n"
            % (name.upper(), name.replace("-", "\\-"), summary, name, description))


def liria_translations(staged):
    """Translations of texts that named the original product are removed
    (those texts no longer exist), and catalog headers name SDE. Run after
    every source edit."""
    entry_split = re.compile(r"\n\s*\n")
    for po in sorted((staged / "po").glob("*.po")):
        text = po.read_text(encoding="utf-8", errors="surrogateescape")
        entries = entry_split.split(text)
        kept = []
        for index, entry in enumerate(entries):
            body = "\n".join(line for line in entry.splitlines() if not line.startswith("#"))
            if index == 0:
                header = re.sub(r'(?m)^"(Report-Msgid-Bugs-To|Language-Team|Project-Id-Version): [^"\\]*(?:\\n)?"$',
                                lambda m: '"%s: %s\\n"' % (m[1], "liria" if m[1] == "Project-Id-Version"
                                                            else "SDE <https://github.com/hugomonizdorego/SDE-Desktop>"),
                                entry)
                kept.append(header)
                continue
            if BRAND_WORDS.search(body):
                continue
            kept.append(entry)
        po.write_text("\n\n".join(kept), encoding="utf-8", errors="surrogateescape")
        subprocess.run(["msgfmt", "--check-format", "-o", "/dev/null", str(po)], check=True)


# ----------------------------------------------------------------- window

def liria_header(staged):
    """One compact header row (liria-sde-ui.c): Back, Forward and Parent,
    the breadcrumb path with its edit toggle and search field, the view
    buttons, Search and the main menu. Reload, Home and Computer leave the
    toolbar (Main Menu > View, Ctrl+R, Alt+Home). The menu bar stays hidden
    for its shortcuts; the toolbar and path can no longer be hidden, so the
    main menu is always reachable."""
    edit_once(staged / "src/liria-navigation-window-ui.xml",
              '\t<toolitem name="Up" action="Up"/>\n'
              '\t<toolitem name="Stop" action="Stop"/>\n'
              '\t<toolitem name="Reload" action="Reload"/>\n'
              '\t<separator/>\n'
              '\t<toolitem name="Home" action="Home"/>\n'
              '\t<toolitem name="Computer" action="Go to Computer"/>\n'
              '\t<separator/>\n'
              '\t<toolitem name="Zoom" action="Zoom"/>\n'
              '\t<toolitem name="ViewAs" action="ViewAs"/>\n',
              '\t<toolitem name="Up" action="Up"/>\n'
              '\t<toolitem name="ViewAs" action="ViewAs"/>\n')
    src = staged / "src"
    edit_once(src / "Makefile.am", "\tliria-navigation-window.c \\\n",
              "\tliria-navigation-window.c \\\n\tliria-sde-ui.c \\\n\tliria-sde-ui.h \\\n")
    window = src / "liria-navigation-window.c"
    edit_once(window, '#include "liria-navigation-window-pane.h"\n',
              '#include "liria-navigation-window-pane.h"\n#include "liria-sde-ui.h"\n')
    edit_once(window, """    gtk_widget_show (toolbar);

    liria_navigation_window_initialize_toolbars (window);
""", """    gtk_widget_show (toolbar);

    /* SDE: compact header row with the path, view buttons and main menu. */
    liria_sde_ui_setup_header (window, toolbar, ui_manager, pane);

    liria_navigation_window_initialize_toolbars (window);
""")
    edit_once(window, "    if (g_settings_get_boolean (liria_window_state, LIRIA_WINDOW_STATE_START_WITH_TOOLBAR))\n",
              "    if (TRUE) /* SDE: the header row holds the main menu */\n")
    edit_once(window, "    show_location_bar = g_settings_get_boolean (liria_window_state, LIRIA_WINDOW_STATE_START_WITH_LOCATION_BAR);\n",
              "    show_location_bar = TRUE; /* SDE: the path is part of the header */\n")
    edit_once(window, "    gtk_widget_show (GTK_WIDGET (window->sidebar));\n}\n",
              "    gtk_widget_show (GTK_WIDGET (window->sidebar));\n    liria_sde_ui_tidy_sidebar (window);\n}\n")
    # The location toggle: a symbolic pencil at the end of the path.
    pane = src / "liria-navigation-window-pane.c"
    edit_once(pane, 'image = gtk_image_new_from_icon_name ("gtk-edit", GTK_ICON_SIZE_MENU);',
              'image = gtk_image_new_from_icon_name (gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), '
              '"document-edit-symbolic") ? "document-edit-symbolic" : "gtk-edit", GTK_ICON_SIZE_MENU);')
    edit_once(pane, "    gtk_box_pack_start (GTK_BOX (hbox), pane->location_button, FALSE, FALSE, 0);\n",
              "    gtk_box_pack_end (GTK_BOX (hbox), pane->location_button, FALSE, FALSE, 0);\n")
    edit_once(pane, '_("Toggle between button and text-based location bar")', '_("Edit the location")')
    # Liria's style sheet and application actions.
    edit_once(staged / "data/liria-resources.gresource.xml",
              '    <file compressed="true">liria-bookmarks-window.ui</file>\n',
              '    <file compressed="true">liria-bookmarks-window.ui</file>\n'
              '    <file compressed="true">liria-sde.css</file>\n')
    app = src / "liria-application.c"
    edit_once(app, '#include "liria-application.h"\n', '#include "liria-application.h"\n#include "liria-sde-ui.h"\n')
    edit_once(app, "    /* initialize theming */\n    init_icons_and_styles ();\n",
              "    /* initialize theming */\n    init_icons_and_styles ();\n"
              "    liria_sde_ui_startup (app); /* SDE: style sheet, Send To */\n")
    # New Launcher needs a launcher editor SDE does not ship.
    ui = src / "file-manager/liria-directory-view-ui.xml"
    edit_all(ui, '<menuitem name="New Launcher" action="New Launcher"/>', "")


def liria_view_switcher(staged):
    """View As becomes linked icon buttons (icons, list, compact) instead
    of a text combo box."""
    action = staged / "src/liria-view-as-action.c"
    replace_between(action, "static void\nactivate_nth_short_list_item (LiriaWindow *window, guint index)",
                    "static void\ndisconnect_proxy (GtkAction *action,", r'''static const char *
sde_view_icon (const char *id)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default ();
    const char *names[4] = { "view-grid-symbolic", "view-app-grid-symbolic", "view-grid", NULL };

    if (g_strrstr (id, "List_View") != NULL)
    {
        names[0] = "view-list-symbolic";
        names[1] = "view-list-bullet-symbolic";
        names[2] = "view-list";
    }
    else if (g_strrstr (id, "Compact_View") != NULL)
    {
        names[0] = "view-compact-symbolic";
        names[1] = "view-dual-symbolic";
        names[2] = "view-continuous-symbolic";
    }
    for (int i = 0; names[i] != NULL; i++)
        if (gtk_icon_theme_has_icon (theme, names[i]))
            return names[i];
    return names[0];
}

static void
sde_view_button_toggled (GtkToggleButton *button, LiriaWindow *window)
{
    GtkWidget *box = gtk_widget_get_parent (GTK_WIDGET (button));
    const char *id = g_object_get_data (G_OBJECT (button), "sde-view-id");
    LiriaWindowSlot *slot;

    if (!gtk_toggle_button_get_active (button) || box == NULL ||
        g_object_get_data (G_OBJECT (box), "sde-updating") != NULL)
        return;
    slot = liria_window_get_active_slot (window);
    if (slot != NULL && id != NULL && !liria_window_slot_content_view_matches_iid (slot, id))
        liria_window_slot_set_content_view (slot, id);
}

static void
sde_add_view_button (GtkWidget *box, LiriaWindow *window, LiriaWindowSlot *slot,
                     const char *id, GtkWidget **group)
{
    const LiriaViewInfo *info = liria_view_factory_lookup (id);
    GtkWidget *button;

    if (info == NULL)
        return;
    button = *group != NULL ? gtk_radio_button_new_from_widget (GTK_RADIO_BUTTON (*group))
                            : gtk_radio_button_new (NULL);
    *group = button;
    gtk_toggle_button_set_mode (GTK_TOGGLE_BUTTON (button), FALSE);
    gtk_container_add (GTK_CONTAINER (button),
                       gtk_image_new_from_icon_name (sde_view_icon (id), GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text (button, _(info->view_combo_label));
    gtk_widget_set_focus_on_click (button, FALSE);
    g_object_set_data_full (G_OBJECT (button), "sde-view-id", g_strdup (id), g_free);
    gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
    if (slot != NULL && liria_window_slot_content_view_matches_iid (slot, id))
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
    g_signal_connect (button, "toggled", G_CALLBACK (sde_view_button_toggled), window);
    gtk_widget_show_all (button);
}

static void
view_as_changed_callback (LiriaWindow *window,
                          GtkWidget *box)
{
    LiriaWindowSlot *slot = liria_window_get_active_slot (window);
    GtkWidget *group = NULL;
    GList *node, *children;
    gboolean listed = FALSE;
    guint count;

    g_object_set_data (G_OBJECT (box), "sde-updating", GINT_TO_POINTER (1));
    children = gtk_container_get_children (GTK_CONTAINER (box));
    for (node = children; node != NULL; node = node->next)
        gtk_widget_destroy (node->data);
    g_list_free (children);

    for (node = window->details->short_list_viewers; node != NULL; node = node->next)
    {
        sde_add_view_button (box, window, slot, node->data, &group);
        if (slot != NULL && liria_window_slot_content_view_matches_iid (slot, node->data))
            listed = TRUE;
    }
    if (!listed && slot != NULL && liria_window_slot_get_content_view_id (slot) != NULL)
        sde_add_view_button (box, window, slot, liria_window_slot_get_content_view_id (slot), &group);

    children = gtk_container_get_children (GTK_CONTAINER (box));
    count = g_list_length (children);
    g_list_free (children);
    gtk_widget_set_visible (box, count > 1);
    g_object_set_data (G_OBJECT (box), "sde-updating", NULL);
}

static void
connect_proxy (GtkAction *action,
               GtkWidget *proxy)
{
    if (GTK_IS_TOOL_ITEM (proxy))
    {
        GtkToolItem *item = GTK_TOOL_ITEM (proxy);
        LiriaViewAsAction *vaction = LIRIA_VIEW_AS_ACTION (action);
        LiriaNavigationWindow *window = vaction->priv->window;
        GtkWidget *box;

        /* SDE: linked icon buttons, one per view, filled in when a
         * location is shown. */
        box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_style_context_add_class (gtk_widget_get_style_context (box), GTK_STYLE_CLASS_LINKED);
        gtk_style_context_add_class (gtk_widget_get_style_context (box), "sde-view-switcher");
        gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
        gtk_container_add (GTK_CONTAINER (item), box);

        g_signal_connect (window, "view-as-changed",
                          G_CALLBACK (view_as_changed_callback),
                          box);
    }

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    (* GTK_ACTION_CLASS (parent_class)->connect_proxy) (action, proxy);
    G_GNUC_END_IGNORE_DEPRECATIONS;
}

''')


def liria_history_buttons(staged):
    """Back and Forward without drop-down arrows; a right-click on either
    shows the history, as before through the arrow."""
    nav = staged / "src/liria-navigation-action.c"
    edit_once(nav, "static void\nconnect_proxy (GtkAction *action, GtkWidget *proxy)\n{", """/* SDE: right-click shows the history menu. */
static gboolean
sde_history_button_press (GtkWidget *button, GdkEventButton *event, GtkMenuToolButton *proxy)
{
    GtkWidget *menu;

    if (event->type != GDK_BUTTON_PRESS || event->button != 3)
        return FALSE;
    menu = gtk_menu_tool_button_get_menu (proxy);
    g_signal_emit_by_name (proxy, "show-menu");
    gtk_menu_popup_at_widget (GTK_MENU (menu), button, GDK_GRAVITY_SOUTH_WEST,
                              GDK_GRAVITY_NORTH_WEST, (GdkEvent *) event);
    return TRUE;
}

static void
connect_proxy (GtkAction *action, GtkWidget *proxy)
{""")
    edit_once(nav, """        g_signal_connect (child, "button-release-event", G_CALLBACK (proxy_button_release_event_cb), NULL);
    }
""", """        g_signal_connect (child, "button-release-event", G_CALLBACK (proxy_button_release_event_cb), NULL);

        /* SDE: no drop-down arrow next to Back and Forward. */
        {
            GList *kids = gtk_container_get_children (GTK_CONTAINER (gtk_widget_get_parent (child)));
            GList *k;

            for (k = kids; k != NULL; k = k->next)
            {
                if (k->data != (gpointer) child)
                {
                    gtk_widget_hide (k->data);
                    gtk_widget_set_no_show_all (k->data, TRUE);
                }
            }
            g_list_free (kids);
            gtk_widget_set_tooltip_text (child, naction->priv->direction == LIRIA_NAVIGATION_DIRECTION_BACK
                                                ? _("Go back (right-click for history)")
                                                : _("Go forward (right-click for history)"));
            g_signal_connect (child, "button-press-event", G_CALLBACK (sde_history_button_press), proxy);
        }
    }
""")


def liria_context_menus(staged):
    """Right-click menus in clear groups: open; clipboard; rename, link,
    copy/move/send to; tools from extensions (compress, extract, edit);
    trash; drives; properties."""
    ui = staged / "src/file-manager/liria-directory-view-ui.xml"
    replace_between(ui, '<popup name="selection">', '<popup name="location">', '''<popup name="selection">
	<placeholder name="Open Placeholder">
		<menuitem name="Open" action="Open"/>
		<placeholder name="Applications Placeholder"/>
		<menu action="Open With">
			<placeholder name="Applications Placeholder"/>
			<separator name="open with separator"/>
			<menuitem name="OtherApplication" action="OtherApplication1"/>
		</menu>
		<placeholder name="OtherApplicationPlaceholder">
		        <menuitem name="OtherApplication2" action="OtherApplication2"/>
		</placeholder>
		<menuitem name="OpenInNewTab" action="OpenInNewTab"/>
		<menuitem name="OpenAlternate" action="OpenAlternate"/>
		<menu action="Scripts">
			 <placeholder name="Scripts Placeholder"/>
			 <separator name="After Scripts"/>
			 <menuitem name="Open Scripts Folder" action="Open Scripts Folder"/>
		</menu>
	</placeholder>
	<separator name="Clipboard separator"/>
	<placeholder name="File Clipboard Actions">
		<menuitem name="Cut" action="Cut"/>
		<menuitem name="Copy" action="Copy"/>
		<menuitem name="Paste Files Into" action="Paste Files Into"/>
	</placeholder>
	<separator name="File actions separator"/>
	<placeholder name="File Actions">
		<menuitem name="Rename" action="Rename"/>
		<menuitem name="Create Link" action="Create Link"/>
		<menu action="CopyToMenu">
			<menuitem name="Copy to next pane" action="Copy to next pane"/>
			<menuitem name="Copy to Home" action="Copy to Home"/>
			<menuitem name="Copy to Desktop" action="Copy to Desktop"/>
		</menu>
		<menu action="MoveToMenu">
			<menuitem name="Move to next pane" action="Move to next pane"/>
			<menuitem name="Copy to Home" action="Move to Home"/>
			<menuitem name="Copy to Desktop" action="Move to Desktop"/>
		</menu>
	</placeholder>
	<separator name="Extension actions separator"/>
	<placeholder name="Extension Actions"/>
	<separator name="Dangerous separator"/>
	<placeholder name="Dangerous File Actions">
		<menuitem name="Trash" action="Trash"/>
		<menuitem name="Delete" action="Delete"/>
		<menuitem name="Restore From Trash" action="Restore From Trash"/>
	</placeholder>
	<separator name="Appearance separator"/>
	<placeholder name="Icon Appearance Items">
	</placeholder>
	<separator name="Removable separator"/>
	<placeholder name="Removable Media Placeholder">
		 <menuitem name="Mount Volume" action="Mount Volume"/>
		 <menuitem name="Unmount Volume" action="Unmount Volume"/>
		 <menuitem name="Eject Volume" action="Eject Volume"/>
		 <menuitem name="Format Volume" action="Format Volume"/>
		 <menuitem name="Start Volume" action="Start Volume"/>
		 <menuitem name="Stop Volume" action="Stop Volume"/>
		 <menuitem name="Poll" action="Poll"/>
        </placeholder>
        <menuitem name="Connect To Server Link" action="Connect To Server Link"/>
        <separator name="Properties Separator"/>
        <menuitem name="Properties" action="Properties"/>
</popup>
''')


def liria_home(staged):
    """The view's extension strip (where the SDE Home overview appears)
    becomes a flat, themable area instead of an etched frame."""
    edit(staged / "src/liria-window-slot.c",
         "    frame = gtk_frame_new (NULL);\n    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_ETCHED_IN);\n",
         "    frame = gtk_frame_new (NULL);\n    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_NONE);\n"
         "    gtk_style_context_add_class (gtk_widget_get_style_context (frame), \"liria-extra-location\");\n")


def liria_sidebar(staged):
    """Sidebar sections read Favorites (Home and the personal folders),
    Devices (the file system and drives), Bookmarks and Network, with
    icons of the theme drawn from a GIcon column by the cell renderer (since
    alpha52 the full-colour icons at the scaled sidebar size, see
    liria_sidebar_pinned)."""
    sidebar = staged / "src/liria-places-sidebar.c"
    edit(sidebar, '    last_iter = add_heading (sidebar, SECTION_COMPUTER,\n                             _("Computer"));',
         '    last_iter = add_heading (sidebar, SECTION_COMPUTER,\n                             _("Favorites"));')
    edit(sidebar, "    display_name = g_filename_display_basename (g_get_home_dir ());\n",
         "    display_name = g_strdup (_(\"Home\"));\n")
    root = """    /* file system root */
    mount_uri = "file:///"; /* No need to strdup */
    icon = g_themed_icon_new (LIRIA_ICON_FILESYSTEM);
    last_iter = add_place (sidebar, PLACES_BUILT_IN,
                           SECTION_COMPUTER,
                           _("File System"), icon,
                           mount_uri, NULL, NULL, NULL, 0,
                           _("Open the contents of the File System"));
    g_object_unref (icon);
    compare_for_selection (sidebar,
                           location, mount_uri, last_uri,
                           &last_iter, &select_path);
"""
    edit(sidebar, root, "")
    edit(sidebar, "    /* first go through all connected drives */\n",
         root.replace("SECTION_COMPUTER", "SECTION_DEVICES") + "\n    /* first go through all connected drives */\n")
    # Symbolic icons.
    edit_once(sidebar, '#include "liria-places-sidebar.h"\n', '#include "liria-places-sidebar.h"\n#include "liria-sde-ui.h"\n')
    edit_once(sidebar, "    PLACES_SIDEBAR_COLUMN_HEADING_TEXT,\n\n    PLACES_SIDEBAR_COLUMN_COUNT",
              "    PLACES_SIDEBAR_COLUMN_HEADING_TEXT,\n    PLACES_SIDEBAR_COLUMN_GICON, /* SDE */\n\n    PLACES_SIDEBAR_COLUMN_COUNT")
    edit_once(sidebar, "                                         G_TYPE_INT,\n                                         G_TYPE_STRING);\n",
              "                                         G_TYPE_INT,\n                                         G_TYPE_STRING,\n"
              "                                         G_TYPE_ICON);\n")
    edit_once(sidebar, """                        PLACES_SIDEBAR_COLUMN_SECTION_TYPE, section_type,
                        -1);

    if (surface != NULL)""", """                        PLACES_SIDEBAR_COLUMN_SECTION_TYPE, section_type,
                        -1);
    {
        GIcon *symbolic = liria_sde_ui_sidebar_icon (icon);

        gtk_list_store_set (sidebar->store, &iter, PLACES_SIDEBAR_COLUMN_GICON, symbolic, -1);
        if (symbolic != NULL)
            g_object_unref (symbolic);
    }

    if (surface != NULL)""")
    # The row icon is rendered at exactly the sidebar size (themes such as
    # Papirus lack some icons at 18 px and would make those rows taller).
    edit_once(sidebar, """    icon_size = liria_get_icon_size_for_stock_size (GTK_ICON_SIZE_MENU);
    icon_scale = gtk_widget_get_scale_factor (GTK_WIDGET (sidebar));
    icon_info = liria_icon_info_lookup (icon, icon_size, icon_scale);
""", """    icon_size = liria_sde_ui_sidebar_icon_pixels (); /* SDE */
    icon_scale = gtk_widget_get_scale_factor (GTK_WIDGET (sidebar));
    {
        GIcon *colour = liria_sde_ui_sidebar_icon (icon);

        icon_info = liria_icon_info_lookup (colour != NULL ? colour : icon, icon_size, icon_scale);
        if (colour != NULL)
            g_object_unref (colour);
    }
""")
    edit_once(sidebar, 'liria_icon_info_lookup_from_name ("media-eject", icon_size, icon_scale);',
              'liria_icon_info_lookup_from_name (gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), '
              '"media-eject-symbolic") ? "media-eject-symbolic" : "media-eject", icon_size, icon_scale);')


def liria_quick_access(staged):
    """alpha50: Quick access on the Home overview is the user's own list of
    pinned folders, files and locations (src/liria-sde-pins.c, an overlay;
    stored in ~/.config/sde/liria-quick-access, one URI per line with an
    optional TAB and label). The window builds the pin list code for the
    sidebar's "Pin to Quick Access" item; the SDE Tools extension builds
    the same file (sde-tools/liria-sde-pins.c includes it) for the file
    menus and the overview, and "make check" runs its unit test."""
    src = staged / "src"
    edit_once(src / "Makefile.am", "\tliria-sde-ui.c \\\n",
              "\tliria-sde-pins.c \\\n\tliria-sde-pins.h \\\n\tliria-sde-ui.c \\\n")
    sidebar = src / "liria-places-sidebar.c"
    edit_once(sidebar, '#include "liria-sde-ui.h"\n', '#include "liria-sde-ui.h"\n#include "liria-sde-pins.h"\n')
    edit_once(sidebar, "    GtkWidget *popup_menu_open_in_new_tab_item;\n",
              "    GtkWidget *popup_menu_open_in_new_tab_item;\n    GtkWidget *popup_menu_sde_pin_item; /* SDE */\n")
    edit_once(sidebar, "    sidebar->popup_menu_remove_item = NULL;\n",
              "    sidebar->popup_menu_remove_item = NULL;\n    sidebar->popup_menu_sde_pin_item = NULL;\n")
    edit_once(sidebar, "static void\nbookmarks_check_popup_sensitivity (LiriaPlacesSidebar *sidebar)\n",
              """/* SDE: the label of an eel image menu item sits in a box next to its icon. */
static void
sde_menu_item_set_text (GtkWidget *item, const char *text)
{
    GtkWidget *box = gtk_bin_get_child (GTK_BIN (item));
    GList *children = GTK_IS_CONTAINER (box) ? gtk_container_get_children (GTK_CONTAINER (box)) : NULL;

    for (GList *l = children; l != NULL; l = l->next)
    {
        if (GTK_IS_LABEL (l->data))
            gtk_label_set_text_with_mnemonic (GTK_LABEL (l->data), text);
    }
    g_list_free (children);
}

static void
bookmarks_check_popup_sensitivity (LiriaPlacesSidebar *sidebar)
""")
    edit_once(sidebar, "    gtk_widget_show (sidebar->popup_menu_open_in_new_tab_item);\n",
              """    gtk_widget_show (sidebar->popup_menu_open_in_new_tab_item);

    /* SDE: Pin to / Unpin from Quick Access on the Home overview. */
    gtk_widget_set_visible (sidebar->popup_menu_sde_pin_item, uri != NULL);
    sde_menu_item_set_text (sidebar->popup_menu_sde_pin_item,
                            uri != NULL && liria_sde_pins_is_pinned (uri) ? _("Unpin from Quick Access")
                                                                          : _("Pin to Quick Access"));
""")
    edit_once(sidebar, "/* Rename the selected bookmark */\n", """/* SDE: pins or unpins the selected place in Quick access. */
static void
sde_pin_shortcut_cb (GtkMenuItem        *item,
                     LiriaPlacesSidebar *sidebar)
{
    GtkTreeIter iter;
    char *uri = NULL;

    if (!get_selected_iter (sidebar, &iter))
    {
        return;
    }
    gtk_tree_model_get (GTK_TREE_MODEL (sidebar->filter_model), &iter,
                        PLACES_SIDEBAR_COLUMN_URI, &uri, -1);
    if (uri != NULL)
    {
        const char *uris[] = { uri, NULL };

        if (liria_sde_pins_is_pinned (uri))
            liria_sde_pins_remove (uris);
        else
            liria_sde_pins_add (uris, -1);
        g_free (uri);
    }
}

/* Rename the selected bookmark */
""")
    edit_once(sidebar, """                      G_CALLBACK (open_shortcut_in_new_window_cb), sidebar);
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);
""", """                      G_CALLBACK (open_shortcut_in_new_window_cb), sidebar);
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);

    item = eel_image_menu_item_new_from_icon ("view-pin-symbolic", _("Pin to Quick Access"));
    sidebar->popup_menu_sde_pin_item = item;
    g_signal_connect (item, "activate",
                      G_CALLBACK (sde_pin_shortcut_cb), sidebar);
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (sidebar->popup_menu), item);
""")


def liria_home_view(staged):
    """alpha52: the Home overview is the Home view. While it is shown the
    slot hides its folder view (liria-sde-ui.c, "sde-home-replaces-view"),
    so the home folder's icons no longer appear a second time below it;
    leaving the home folder restores the view. src/liria-sde-model.c holds
    the preferences (~/.config/sde/liria.conf) and the widget-free
    decisions of the ribbon, the Home view and drops (unit tested)."""
    src = staged / "src"
    edit_once(src / "Makefile.am", "\tliria-sde-pins.h \\\n",
              "\tliria-sde-pins.h \\\n\tliria-sde-model.c \\\n\tliria-sde-model.h \\\n")
    edit_once(src / "liria-window-slot.c", """                           slot->extra_location_widgets);
    gtk_widget_hide (slot->extra_location_frame);
""", """                           slot->extra_location_widgets);
    gtk_widget_hide (slot->extra_location_frame);
    /* SDE: the Home overview may have replaced the folder view. */
    gtk_widget_show (slot->view_box);
    gtk_box_set_child_packing (GTK_BOX (slot->content_box), slot->extra_location_frame,
                               FALSE, FALSE, 0, GTK_PACK_START);
""")


SIDEBAR_PINNED_HELPERS = r'''/* SDE (alpha52): the sidebar as in Windows 11. Home on its own, then
 * "Pinned" (the Quick access pins, ~/.config/sde/liria-quick-access, in
 * their order; a drop on the heading or between two pins pins folders),
 * Bookmarks, "Drives" and Network. Headings collapse on click (stored in
 * ~/.config/sde/liria.conf [sidebar] collapsed) and a pinned row shows a
 * pin while the pointer is over it. Icons are the icon theme's normal
 * full-colour icons, as in the folder view. */
static const char *
sde_section_name (SectionType section)
{
    switch (section)
    {
    case SECTION_COMPUTER:
        return "pinned";
    case SECTION_DEVICES:
        return "drives";
    case SECTION_BOOKMARKS:
        return "bookmarks";
    case SECTION_NETWORK:
        return "network";
    default:
        return NULL;
    }
}

static gboolean
sde_row_visible (GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    PlaceType type = PLACES_BUILT_IN;
    SectionType section = SECTION_HOME;

    gtk_tree_model_get (model, iter, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type,
                        PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section, -1);
    return type == PLACES_HEADING || !liria_sde_ui_sidebar_collapsed (sde_section_name (section));
}

static void
sde_heading_arrow_func (GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model,
                        GtkTreeIter *iter, gpointer data)
{
    PlaceType type = PLACES_BUILT_IN;
    SectionType section = SECTION_HOME;

    gtk_tree_model_get (model, iter, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type,
                        PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section, -1);
    g_object_set (cell, "visible", type == PLACES_HEADING,
                  "icon-name", liria_sde_ui_sidebar_collapsed (sde_section_name (section))
                               ? "pan-end-symbolic" : "pan-down-symbolic", NULL);
}

/* The pin of the pinned row under the pointer. */
static void
sde_pin_marker_func (GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model,
                     GtkTreeIter *iter, gpointer data)
{
    LiriaPlacesSidebar *sidebar = data;
    GtkTreePath *hover = g_object_get_data (G_OBJECT (sidebar), "sde-hover-path");
    gboolean pinned = FALSE, show = FALSE;

    gtk_tree_model_get (model, iter, PLACES_SIDEBAR_COLUMN_SDE_PINNED, &pinned, -1);
    if (pinned && hover != NULL)
    {
        GtkTreePath *path = gtk_tree_model_get_path (model, iter);

        show = gtk_tree_path_compare (path, hover) == 0;
        gtk_tree_path_free (path);
    }
    /* Pinned rows keep the room, so the text does not move on hover. */
    g_object_set (cell, "visible", pinned, "icon-name", show ? "view-pin-symbolic" : NULL, NULL);
}

static gboolean
sde_sidebar_hover (GtkWidget *tree_view, GdkEventMotion *event, LiriaPlacesSidebar *sidebar)
{
    GtkTreePath *path = NULL;

    if (event != NULL)
        gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (tree_view), (int) event->x, (int) event->y,
                                       &path, NULL, NULL, NULL);
    g_object_set_data_full (G_OBJECT (sidebar), "sde-hover-path", path, (GDestroyNotify) gtk_tree_path_free);
    gtk_widget_queue_draw (tree_view);
    return FALSE;
}

static gboolean
sde_sidebar_left (GtkWidget *tree_view, GdkEventCrossing *event, LiriaPlacesSidebar *sidebar)
{
    return sde_sidebar_hover (tree_view, NULL, sidebar);
}

/* A click on a heading collapses or expands its section. */
static gboolean
sde_heading_clicked (LiriaPlacesSidebar *sidebar, GdkEventButton *event)
{
    GtkTreePath *path = NULL;
    GtkTreeIter iter;
    PlaceType type = PLACES_BUILT_IN;
    SectionType section = SECTION_HOME;

    if (event->button != 1 ||
        !gtk_tree_view_get_path_at_pos (sidebar->tree_view, (int) event->x, (int) event->y, &path, NULL, NULL, NULL))
        return FALSE;
    if (gtk_tree_model_get_iter (sidebar->filter_model, &iter, path))
        gtk_tree_model_get (sidebar->filter_model, &iter, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type,
                            PLACES_SIDEBAR_COLUMN_SECTION_TYPE, &section, -1);
    gtk_tree_path_free (path);
    if (type != PLACES_HEADING || sde_section_name (section) == NULL)
        return FALSE;
    liria_sde_ui_sidebar_toggle (sde_section_name (section));
    gtk_tree_model_filter_refilter (GTK_TREE_MODEL_FILTER (sidebar->filter_model));
    gtk_widget_queue_draw (GTK_WIDGET (sidebar->tree_view));
    return TRUE;
}

static GIcon *
sde_pin_icon (const char *uri)
{
    GFile *file;
    char *path;
    GIcon *icon = NULL;
    int i;

    if (g_str_has_prefix (uri, "trash:"))
        return liria_trash_monitor_get_icon ();
    file = g_file_new_for_uri (uri);
    path = g_file_get_path (file);
    if (path != NULL)
    {
        if (g_strcmp0 (path, g_get_home_dir ()) == 0)
            icon = g_themed_icon_new (LIRIA_ICON_HOME);
        for (i = 0; icon == NULL && i < G_USER_N_DIRECTORIES; i++)
        {
            if (g_strcmp0 (path, g_get_user_special_dir (i)) != 0)
                continue;
            icon = i == G_USER_DIRECTORY_DESKTOP ? g_themed_icon_new (LIRIA_ICON_DESKTOP)
                                                 : liria_user_special_directory_get_gicon (i);
        }
        if (icon == NULL && g_file_test (path, G_FILE_TEST_EXISTS) && !g_file_test (path, G_FILE_TEST_IS_DIR))
        {
            char *type = g_content_type_guess (path, NULL, 0, NULL);

            icon = g_content_type_get_icon (type);
            g_free (type);
        }
    }
    if (icon == NULL)
        icon = g_themed_icon_new_with_default_fallbacks (g_file_is_native (file) ? LIRIA_ICON_FOLDER : "folder-remote");
    g_free (path);
    g_object_unref (file);
    return icon;
}

static void
sde_add_pinned_places (LiriaPlacesSidebar *sidebar, const char *location, const char *last_uri,
                       GtkTreePath **select_path)
{
    GPtrArray *pins = liria_sde_pins_load ();
    guint i;

    for (i = 0; i < pins->len; i++)
    {
        LiriaSdePin *pin = pins->pdata[i];
        gboolean trash = g_str_has_prefix (pin->uri, "trash:");
        GFile *file = g_file_new_for_uri (pin->uri);
        char *base = g_file_get_basename (file), *where = g_file_get_parse_name (file), *name;
        GIcon *icon = sde_pin_icon (pin->uri);
        GtkTreeIter iter;

        if (pin->label != NULL)
            name = g_strdup (pin->label);
        else if (trash)
            name = g_strdup (_("Trash"));
        else if (base != NULL && g_strcmp0 (base, "/") != 0)
            name = g_filename_display_name (base);
        else
            name = g_strdup (where);
        iter = add_place (sidebar, PLACES_BUILT_IN, SECTION_COMPUTER, name, icon, pin->uri,
                          NULL, NULL, NULL, (int) i, trash ? _("Open the trash") : where);
        compare_for_selection (sidebar, location, pin->uri, last_uri, &iter, select_path);
        g_object_unref (icon);
        g_free (name);
        g_free (where);
        g_free (base);
        g_object_unref (file);
    }
    g_ptr_array_unref (pins);
}

static gboolean
sde_pins_reload (gpointer data)
{
    LiriaPlacesSidebar *sidebar = data;

    g_object_set_data (G_OBJECT (sidebar), "sde-pins-reload", NULL);
    if (sidebar->tree_view != NULL && sidebar->window != NULL)
        update_places (sidebar);
    return G_SOURCE_REMOVE;
}

static void
sde_pins_changed (LiriaPlacesSidebar *sidebar, GFile *file, GFile *other, GFileMonitorEvent event)
{
    if (event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED ||
        g_object_get_data (G_OBJECT (sidebar), "sde-pins-reload") != NULL)
        return;
    g_object_set_data (G_OBJECT (sidebar), "sde-pins-reload", GINT_TO_POINTER (1));
    g_timeout_add_full (G_PRIORITY_DEFAULT, 100, sde_pins_reload, g_object_ref (sidebar), g_object_unref);
}

static void
sde_watch_pins (LiriaPlacesSidebar *sidebar)
{
    char *path = g_build_filename (g_get_user_config_dir (), "sde", NULL);
    GFile *file;
    GFileMonitor *monitor;

    g_mkdir_with_parents (path, 0700);
    g_free (path);
    path = liria_sde_pins_path ();
    file = g_file_new_for_path (path);
    monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (monitor != NULL)
    {
        g_signal_connect_object (monitor, "changed", G_CALLBACK (sde_pins_changed), sidebar, G_CONNECT_SWAPPED);
        g_object_set_data_full (G_OBJECT (sidebar), "sde-pins-monitor", monitor, g_object_unref);
    }
    g_object_unref (file);
    g_free (path);
}

static void
update_places (LiriaPlacesSidebar *sidebar)
'''


def liria_sidebar_pinned(staged):
    """alpha52: Windows 11 style sidebar: Home, Pinned (the Quick access
    pins, droppable), Bookmarks, Drives, Network; collapsible headings; a
    pin on the hovered pinned row; full-colour theme icons (see
    SIDEBAR_PINNED_HELPERS)."""
    sidebar = staged / "src/liria-places-sidebar.c"
    edit_once(sidebar, "    SECTION_NETWORK,\n} SectionType;", "    SECTION_NETWORK,\n    SECTION_HOME, /* SDE */\n} SectionType;")
    edit_once(sidebar, "    PLACES_SIDEBAR_COLUMN_GICON, /* SDE */\n",
              "    PLACES_SIDEBAR_COLUMN_GICON, /* SDE */\n    PLACES_SIDEBAR_COLUMN_SDE_PINNED, /* SDE */\n")
    edit_once(sidebar, "                                         G_TYPE_STRING,\n                                         G_TYPE_ICON);\n",
              "                                         G_TYPE_STRING,\n                                         G_TYPE_ICON,\n"
              "                                         G_TYPE_BOOLEAN);\n")
    edit_once(sidebar, "\nstatic void\nupdate_places (LiriaPlacesSidebar *sidebar)\n",
              "\nstatic void update_places (LiriaPlacesSidebar *sidebar);\n\n" + SIDEBAR_PINNED_HELPERS)
    # A row of a collapsed section has no visible iter.
    edit_once(sidebar, """    int res;

    res = g_strcmp0 (added_uri, last_uri);
""", """    int res;

    if (iter->stamp == 0) /* SDE: hidden in a collapsed section */
    {
        return;
    }
    res = g_strcmp0 (added_uri, last_uri);
""")
    # Home, then the pins under "Pinned".
    text = sidebar.read_text()
    start = text.index("    /* COMPUTER */\n    last_iter = add_heading (sidebar, SECTION_COMPUTER,\n")
    end = text.index("    /* file system root */\n", start)
    sidebar.write_text(text[:start] + """    /* SDE: Home on its own, then the Quick access pins. */
    mount_uri = liria_get_home_directory_uri ();
    icon = g_themed_icon_new (LIRIA_ICON_HOME);
    last_iter = add_place (sidebar, PLACES_BUILT_IN,
                           SECTION_HOME,
                           _("Home"), icon,
                           mount_uri, NULL, NULL, NULL, 0,
                           _("Open your personal folder"));
    g_object_unref (icon);
    compare_for_selection (sidebar,
                           location, mount_uri, last_uri,
                           &last_iter, &select_path);
    g_free (mount_uri);

    add_heading (sidebar, SECTION_COMPUTER, _("Pinned"));
    sde_add_pinned_places (sidebar, location, last_uri, &select_path);

""" + text[end:])
    edit_once(sidebar, "    char *location, *mount_uri, *name, *desktop_path, *last_uri;\n    const gchar *path;\n",
              "    char *location, *mount_uri, *name, *last_uri;\n")
    edit_once(sidebar, "    GList *network_mounts;\n    GList *xdg_dirs;\n", "    GList *network_mounts;\n")
    edit(sidebar, '            add_heading (sidebar, SECTION_DEVICES,\n                     _("Devices"));',
         '            add_heading (sidebar, SECTION_DEVICES,\n                     _("Drives"));')
    # Pinned rows are marked (the hover pin and drops between them).
    edit_once(sidebar, "        gtk_list_store_set (sidebar->store, &iter, PLACES_SIDEBAR_COLUMN_GICON, symbolic, -1);\n",
              "        gtk_list_store_set (sidebar->store, &iter, PLACES_SIDEBAR_COLUMN_GICON, symbolic,\n"
              "                            PLACES_SIDEBAR_COLUMN_SDE_PINNED, section_type == SECTION_COMPUTER, -1);\n")
    # Drops: on the Pinned heading or between two pins they pin folders.
    edit_once(sidebar, """    if (place_type == PLACES_HEADING &&
        section_type != SECTION_BOOKMARKS &&
        section_type != SECTION_NETWORK) {""", """    if (place_type == PLACES_HEADING &&
        section_type != SECTION_BOOKMARKS &&
        section_type != SECTION_NETWORK &&
        section_type != SECTION_COMPUTER) {""")
    edit_once(sidebar, """    if (place_type == PLACES_HEADING && section_type == SECTION_BOOKMARKS) {
        *pos = GTK_TREE_VIEW_DROP_AFTER;""", """    if (place_type == PLACES_HEADING && (section_type == SECTION_BOOKMARKS || section_type == SECTION_COMPUTER)) {
        *pos = GTK_TREE_VIEW_DROP_AFTER;
    } else if (section_type == SECTION_COMPUTER &&
               (*pos == GTK_TREE_VIEW_DROP_BEFORE || *pos == GTK_TREE_VIEW_DROP_AFTER)) {
        /* SDE: the edge of a pinned row: a new pin goes there */""")
    edit_once(sidebar, """        if (section_type != SECTION_BOOKMARKS &&
            !(section_type == SECTION_NETWORK && place_type == PLACES_HEADING)) {
            goto out;
        }
""", """        if (section_type == SECTION_COMPUTER)
        {
            /* SDE: pin the dropped folders at that place. */
            char **uris = info == TEXT_URI_LIST ? gtk_selection_data_get_uris (selection_data) : NULL;

            if (place_type == PLACES_HEADING)
                position = 0;
            else if (tree_pos == GTK_TREE_VIEW_DROP_AFTER)
                position++;
            success = uris != NULL && liria_sde_pins_add ((const char * const *) uris, position);
            g_strfreev (uris);
            goto out;
        }
        if (section_type != SECTION_BOOKMARKS &&
            !(section_type == SECTION_NETWORK && place_type == PLACES_HEADING)) {
            goto out;
        }
""")
    # Headings: an arrow, a click collapses; the pin of the hovered row.
    edit_once(sidebar, """    gtk_tree_view_column_set_cell_data_func (col, cell,
                         heading_cell_renderer_func,
                         sidebar, NULL);
""", """    gtk_tree_view_column_set_cell_data_func (col, cell,
                         heading_cell_renderer_func,
                         sidebar, NULL);

    /* SDE: the collapse arrow after the heading */
    cell = gtk_cell_renderer_pixbuf_new ();
    g_object_set (cell, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 4, NULL);
    gtk_tree_view_column_pack_start (col, cell, FALSE);
    gtk_tree_view_column_set_cell_data_func (col, cell, sde_heading_arrow_func, sidebar, NULL);
""")
    edit_once(sidebar, """    g_signal_connect (cell, "editing-canceled",
                      G_CALLBACK (bookmarks_editing_canceled), sidebar);
""", """    g_signal_connect (cell, "editing-canceled",
                      G_CALLBACK (bookmarks_editing_canceled), sidebar);

    /* SDE: a pin on the pinned row under the pointer */
    cell = gtk_cell_renderer_pixbuf_new ();
    g_object_set (cell, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 4, "xalign", 1.0,
                  "width", 16 + 2 * 4, NULL);
    gtk_tree_view_column_pack_end (col, cell, FALSE);
    gtk_tree_view_column_set_cell_data_func (col, cell, sde_pin_marker_func, sidebar, NULL);
""")
    edit_once(sidebar, """    sidebar->filter_model = liria_shortcuts_model_filter_new (sidebar,
                            GTK_TREE_MODEL (sidebar->store),
                            NULL);
""", """    sidebar->filter_model = liria_shortcuts_model_filter_new (sidebar,
                            GTK_TREE_MODEL (sidebar->store),
                            NULL);
    gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (sidebar->filter_model),
                                            sde_row_visible, sidebar, NULL);
""")
    edit_once(sidebar, """    g_signal_connect (tree_view, "motion-notify-event",
                      G_CALLBACK (bookmarks_motion_event_cb), sidebar);
""", """    g_signal_connect (tree_view, "motion-notify-event",
                      G_CALLBACK (bookmarks_motion_event_cb), sidebar);
    g_signal_connect (tree_view, "motion-notify-event",
                      G_CALLBACK (sde_sidebar_hover), sidebar);
    g_signal_connect (tree_view, "leave-notify-event",
                      G_CALLBACK (sde_sidebar_left), sidebar);
    sde_watch_pins (sidebar);
""")
    # Until the user drags it, the sidebar is wide enough for its names at
    # every interface scale (176 px at 100 %, grown with Xft/DPI).
    edit_once(staged / "src/liria-navigation-window.c", """        g_settings_get_int (liria_window_state,
                            LIRIA_WINDOW_STATE_SIDEBAR_WIDTH) * scale;
""", """        g_settings_get_int (liria_window_state,
                            LIRIA_WINDOW_STATE_SIDEBAR_WIDTH) * scale;
    {
        GVariant *user = g_settings_get_user_value (liria_window_state, LIRIA_WINDOW_STATE_SIDEBAR_WIDTH);
        gint dpi = -1;

        if (user == NULL)
        {
            g_object_get (gtk_settings_get_default (), "gtk-xft-dpi", &dpi, NULL);
            window->details->side_pane_width = MAX (window->details->side_pane_width, 176 * scale);
            if (dpi > 96 * 1024)
                window->details->side_pane_width = window->details->side_pane_width * (dpi / 1024) / 96;
        }
        else
        {
            g_variant_unref (user);
        }
    }
""")
    edit_once(sidebar, """    if (event->type != GDK_BUTTON_PRESS)
    {
        /* ignore multiple clicks */
        return TRUE;
    }

    if (event->button == 2)""", """    if (event->type != GDK_BUTTON_PRESS)
    {
        /* ignore multiple clicks */
        return TRUE;
    }

    if (sde_heading_clicked (sidebar, event))
    {
        return TRUE;
    }

    if (event->button == 2)""")


def liria_pathbar(staged):
    """alpha52: the path bar's scroll arrows were always shown, even when
    every folder fitted, and then did nothing (Caja computed need_sliders
    as TRUE before measuring); they now appear only when the path is too
    long. A click on the empty part of the path opens the location entry
    (liria-sde-ui.c)."""
    edit_once(staged / "src/liria-pathbar.c", "    need_sliders = TRUE;\n    up_slider_offset = 0;\n",
              "    need_sliders = FALSE; /* SDE: only when the buttons do not fit */\n    up_slider_offset = 0;\n")


def liria_ribbon(staged):
    """alpha52: an optional ribbon under the header of every folder window
    (src/liria-sde-ribbon.c): Home, Share and View tabs, contextual Drive
    and Archive tabs; full, collapsed (Ctrl+F1) or off, remembered in
    ~/.config/sde/liria.conf [ribbon] mode."""
    edit_once(staged / "src/Makefile.am", "\tliria-sde-model.h \\\n",
              "\tliria-sde-model.h \\\n\tliria-sde-ribbon.c \\\n\tliria-sde-ribbon.h \\\n")


def liria_right_drag(staged):
    """alpha52: dragging icons with the right mouse button asks what to do
    (Move Here, Copy Here, Link Here, Cancel), as in Windows; a right click
    without a drag opens the context menu when the button is released.
    Alt+drag is taken by the window manager (it moves windows); the middle
    button keeps asking as before."""
    private = staged / "libliria-private"
    edit_once(private / "liria-icon-private.h", "    guint drag_button;\n",
              "    guint drag_button;\n    GdkEvent *sde_menu_event; /* SDE: right click waiting for a drag */\n")
    container = private / "liria-icon-container.c"
    edit_once(container, """    if (event->button == DRAG_BUTTON
            || event->button == DRAG_MENU_BUTTON)
    {
        details->drag_button = event->button;""", """    if (event->button == DRAG_BUTTON
            || event->button == DRAG_MENU_BUTTON
            || event->button == CONTEXTUAL_MENU_BUTTON)
    {
        details->drag_button = event->button;""")
    edit_once(container, """    if (event->button == CONTEXTUAL_MENU_BUTTON)
    {
        g_signal_emit (container,
                       signals[CONTEXT_CLICK_SELECTION], 0,
                       event);
    }

    return TRUE;
}""", """    if (event->button == CONTEXTUAL_MENU_BUTTON)
    {
        /* SDE: the menu opens on release unless the icons are dragged. */
        g_clear_pointer (&details->sde_menu_event, gdk_event_free);
        details->sde_menu_event = gdk_event_copy ((GdkEvent *) event);
    }

    return TRUE;
}""")
    edit_once(container, """        case DRAG_STATE_MOVE_OR_COPY:
            if (!details->drag_started)
            {
                liria_icon_container_did_not_drag (container, event);
            }""", """        case DRAG_STATE_MOVE_OR_COPY:
            if (!details->drag_started && event->button == CONTEXTUAL_MENU_BUTTON)
            {
                GdkEvent *press = details->sde_menu_event;

                details->sde_menu_event = NULL;
                g_signal_emit (container, signals[CONTEXT_CLICK_SELECTION], 0,
                               press != NULL ? (GdkEventButton *) press : event);
                if (press != NULL)
                    gdk_event_free (press);
            }
            else if (!details->drag_started)
            {
                liria_icon_container_did_not_drag (container, event);
            }""")
    edit_once(container, "                details->drag_started = TRUE;\n                details->drag_state = DRAG_STATE_MOVE_OR_COPY;\n",
              "                details->drag_started = TRUE;\n                details->drag_state = DRAG_STATE_MOVE_OR_COPY;\n"
              "                g_clear_pointer (&details->sde_menu_event, gdk_event_free);\n")
    # GTK suggests "ask" for middle-button drags only. A right-button drag
    # is marked on the display ("sde-drag-ask") until it ends, and every
    # Liria drop target (folder views, sidebar, path bar, Home cards) then
    # asks instead of choosing the default action.
    dnd = private / "liria-icon-dnd.c"
    edit_once(dnd, """void
liria_icon_dnd_begin_drag (LiriaIconContainer *container,""", """static void
sde_right_drag_ended (GtkWidget *widget, GdkDragContext *context, gpointer data)
{
    g_object_set_data (G_OBJECT (gdk_display_get_default ()), "sde-drag-ask", NULL);
    g_signal_handlers_disconnect_by_func (widget, sde_right_drag_ended, data);
}

void
liria_icon_dnd_begin_drag (LiriaIconContainer *container,""")
    edit_once(dnd, """    /* start the drag */
    gtk_drag_begin_with_coordinates (GTK_WIDGET (container),""", """    /* SDE: a right-button drag asks what to do on drop. */
    g_object_set_data (G_OBJECT (gdk_display_get_default ()), "sde-drag-ask", GINT_TO_POINTER (button == 3));
    if (button == 3)
        g_signal_connect (container, "drag-end", G_CALLBACK (sde_right_drag_ended), NULL);

    /* start the drag */
    gtk_drag_begin_with_coordinates (GTK_WIDGET (container),""")
    edit_once(private / "liria-dnd.c", """    if (gdk_drag_context_get_suggested_action (context) == GDK_ACTION_ASK)
    {
        /* Don't override ask */""", """    if (gdk_drag_context_get_suggested_action (context) == GDK_ACTION_ASK ||
        (g_object_get_data (G_OBJECT (gdk_display_get_default ()), "sde-drag-ask") != NULL &&
         (gdk_drag_context_get_actions (context) & GDK_ACTION_ASK)))
    {
        *action = GDK_ACTION_ASK;
        return;
    }
    if (gdk_drag_context_get_suggested_action (context) == GDK_ACTION_ASK)
    {
        /* Don't override ask */""")


def liria_copy_engine(staged):
    """alpha15: reliable copies to USB sticks and other removable or slow
    drives (libliria-private/liria-sde-copy.c, an overlay). Regular files
    are written with bounded dirty data so progress shows what reached the
    device, the operation ends with a visible "Finishing" phase (syncfs),
    names are made valid for FAT/exFAT/NTFS before the first write, and
    files over 4 GiB are refused up front on FAT with a clear message.
    Internal disks keep the normal g_file_copy() path."""
    edit_once(staged / "libliria-private/Makefile.am", "\tliria-file-operations.c \\\n",
         "\tliria-file-operations.c \\\n\tliria-sde-copy.c \\\n\tliria-sde-copy.h \\\n")
    ops = staged / "libliria-private/liria-file-operations.c"
    edit_once(ops, '#include "liria-metadata.h"\n', '#include "liria-metadata.h"\n#include "liria-sde-copy.h"\n')
    edit_once(ops, "\tLiriaCopyCallback  done_callback;\n\tgpointer done_callback_data;\n} CopyMoveJob;",
         "\tLiriaCopyCallback  done_callback;\n\tgpointer done_callback_data;\n"
         "\tLiriaSdeFlush *sde_flush; /* SDE: removable/slow destination */\n"
         "\tgboolean sde_skip_large;\n} CopyMoveJob;\n\n"
         "static void sde_wait_for_device (CommonJob *job, LiriaSdeFlush *flush, const char *path, const char *name);")
    edit_once(ops, "\tint num_files_since_progress;\n\tOpKind op;\n} SourceInfo;",
         "\tint num_files_since_progress;\n\tOpKind op;\n\tint sde_huge_files; /* SDE: files over 4 GiB */\n} SourceInfo;")
    edit_once(ops, "\tsource_info->num_bytes += g_file_info_get_size (info);\n",
         "\tsource_info->num_bytes += g_file_info_get_size (info);\n"
         "\tif (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR &&\n"
         "\t    (guint64) g_file_info_get_size (info) > LIRIA_SDE_FAT_MAX_FILE_SIZE)\n"
         "\t\tsource_info->sde_huge_files++;\n")
    # FAT, exFAT and NTFS (as Windows reads it) reject : * ? " < > | and \ .
    edit_once(ops, '#define FAT_FORBIDDEN_CHARACTERS "/:;*?\\"<>"',
         '#define FAT_FORBIDDEN_CHARACTERS "/:;*?\\"<>|\\\\"')
    edit_once(ops, '\t\tif (!strcmp (dest_fs_type, "fat")  ||\n',
         '\t\tif (!strcmp (dest_fs_type, "exfat") || !strcmp (dest_fs_type, "fuseblk") ||\n'
         '\t\t    !strcmp (dest_fs_type, "ntfs") || !strcmp (dest_fs_type, "ntfs3") ||\n'
         '\t\t    !strcmp (dest_fs_type, "fuse.exfat") || !strcmp (dest_fs_type, "fuse.fusefat") ||\n'
         '\t\t    !strcmp (dest_fs_type, "fat")  ||\n')
    edit_once(ops, '\t\t\tdetails = f (_("There is %" G_GUINT64_FORMAT  " available, but %" G_GOFFSET_FORMAT " is required."), free_size, required_size);',
         '\t\t\tdetails = f (_("There is %S available, but %S is required."), (goffset) free_size, required_size);')

    # copy_move_file: files too large for FAT, then the safe writer.
    edit_once(ops, """		goto out;
	}

 retry:
	error = NULL;
	flags = G_FILE_COPY_NOFOLLOW_SYMLINKS;""", """		goto out;
	}

	/* SDE: FAT cannot hold files of 4 GiB or more; say so clearly. */
	if (copy_job->sde_flush != NULL && liria_sde_flush_too_large (copy_job->sde_flush, src)) {
		if (copy_job->sde_skip_large || job->skip_all_error) {
			goto out;
		}
		primary = f (_("\\"%B\\" is too large for \\"%s\\"."), src, liria_sde_flush_get_name (copy_job->sde_flush));
		secondary = f (_("Drives formatted with FAT32 cannot store files of 4 GB or more. "
				 "Format the drive as exFAT or NTFS to copy this file."));
		response = run_warning (job, primary, secondary, NULL,
					(source_info->num_files - transfer_info->num_files) > 1,
					CANCEL, SKIP_ALL, SKIP, NULL);
		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) {
			copy_job->sde_skip_large = TRUE;
		}
		goto out;
	}

 retry:
	error = NULL;
	flags = G_FILE_COPY_NOFOLLOW_SYMLINKS;""")
    edit_once(ops, """	if (copy_job->is_move) {
		res = g_file_move (src, dest,
				   flags,
				   job->cancellable,
				   copy_file_progress_callback,
				   &pdata,
				   &error);
	} else {""", """	res = FALSE;
	sde_handled = FALSE;
	if (copy_job->sde_flush != NULL && !(copy_job->is_move && same_fs)) {
		/* SDE: removable/slow drive, data written through to the device */
		res = liria_sde_flush_copy_file (copy_job->sde_flush, src, dest, flags,
						 copy_job->is_move, job->cancellable,
						 copy_file_progress_callback, &pdata,
						 &sde_handled, &error);
	}
	if (sde_handled) {
		/* done above */
	} else if (copy_job->is_move) {
		res = g_file_move (src, dest,
				   flags,
				   job->cancellable,
				   copy_file_progress_callback,
				   &pdata,
				   &error);
	} else {""")
    edit_once(ops, "\tint unique_name_nr;\n\tgboolean handled_invalid_filename;\n\n\tjob = (CommonJob *)copy_job;",
         "\tint unique_name_nr;\n\tgboolean handled_invalid_filename;\n\tgboolean sde_handled;\n\n\tjob = (CommonJob *)copy_job;")
    # Valid names for the drive before the first write.
    edit_once(ops, """	dest_fs_type = NULL;
	readonly_source_fs = FALSE;
""", """	dest_fs_type = NULL;
	readonly_source_fs = FALSE;
	if (job->sde_flush != NULL && liria_sde_flush_needs_dos_names (job->sde_flush)) {
		dest_fs_type = g_strdup (liria_sde_flush_get_fs_type (job->sde_flush));
	}
""")
    edit_once(ops, "static gboolean\ncopy_job (GIOSchedulerJob *io_job,", r"""/* SDE: one warning before copying when files cannot fit on FAT. */
static void
sde_check_large_files (CopyMoveJob *job, SourceInfo *source_info)
{
	CommonJob *common = (CommonJob *) job;
	char *primary, *secondary;
	int response;

	if (job->sde_flush == NULL || !liria_sde_flush_has_4g_limit (job->sde_flush) ||
	    source_info->sde_huge_files == 0) {
		return;
	}
	primary = f (ngettext ("%'d file is too large for \"%s\".",
			       "%'d files are too large for \"%s\".",
			       source_info->sde_huge_files),
		     source_info->sde_huge_files, liria_sde_flush_get_name (job->sde_flush));
	secondary = f (_("The drive uses the FAT32 file system, which cannot store files of 4 GB or more. "
			 "Format the drive as exFAT or NTFS to copy them, or skip them and copy the rest."));
	if (source_info->sde_huge_files >= source_info->num_files) {
		run_error (common, primary, secondary, NULL, FALSE, CANCEL, NULL);
		abort_job (common);
		return;
	}
	response = run_warning (common, primary, secondary, NULL, FALSE,
				CANCEL, _("_Skip Large Files"), NULL);
	if (response == 1) {
		job->sde_skip_large = TRUE;
	} else {
		abort_job (common);
	}
}

typedef struct {
	LiriaSdeFlush *flush;
	char *path;
	gint done;
} SdeSync;

static gpointer
sde_sync_thread (gpointer data)
{
	SdeSync *sync = data;

	if (sync->flush != NULL) {
		liria_sde_flush_sync (sync->flush);
	} else {
		liria_sde_sync_path (sync->path);
	}
	g_atomic_int_set (&sync->done, 1);
	return NULL;
}

/* SDE: "Finishing…" until the device has every byte (syncfs). The
 * operation is reported done only after this. */
static void
sde_wait_for_device (CommonJob *job, LiriaSdeFlush *flush, const char *path, const char *name)
{
	SdeSync sync = { flush, (char *) path, 0 };
	GThread *thread;

	liria_progress_info_take_status (job->progress,
					    f (_("Finishing\xE2\x80\xA6 writing data to \"%s\""), name));
	liria_progress_info_take_details (job->progress,
					     f (_("Do not remove the drive until this has finished.")));
	liria_progress_info_pulse_progress (job->progress);
	thread = g_thread_new ("liria-sde-sync", sde_sync_thread, &sync);
	while (!g_atomic_int_get (&sync.done)) {
		g_usleep (150 * 1000);
		liria_progress_info_pulse_progress (job->progress);
	}
	g_thread_join (thread);
}

static void
sde_finish_writing (CopyMoveJob *job)
{
	if (job->sde_flush != NULL) {
		sde_wait_for_device ((CommonJob *) job, job->sde_flush, NULL,
				     liria_sde_flush_get_name (job->sde_flush));
		liria_sde_flush_free (job->sde_flush);
		job->sde_flush = NULL;
	}
}

static gboolean
copy_job (GIOSchedulerJob *io_job,""")
    edit_once(ops, """			    &dest_fs_id,
			    source_info.num_bytes);
	g_object_unref (dest);
	if (job_aborted (common)) {
		goto aborted;
	}

	g_timer_start (job->common.time);
""", """			    &dest_fs_id,
			    source_info.num_bytes);
	if (!job_aborted (common)) {
		job->sde_flush = liria_sde_flush_new (dest);
		sde_check_large_files (job, &source_info);
	}
	g_object_unref (dest);
	if (job_aborted (common)) {
		goto aborted;
	}

	g_timer_start (job->common.time);
""")
    edit_once(ops, """ aborted:

	g_free (dest_fs_id);

	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   copy_job_done,""", """ aborted:
	/* Also after a cancel: what was written must reach the device. */
	sde_finish_writing (job);

	g_free (dest_fs_id);

	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   copy_job_done,""")
    # Moves to another file system are a copy and a delete.
    edit_once(ops, """	/* This moves all files that we can do without copy + delete */
	move_files_prepare (job, dest_fs_id, &dest_fs_type, &fallbacks);""", """	job->sde_flush = liria_sde_flush_new (job->destination);
	if (job->sde_flush != NULL && liria_sde_flush_needs_dos_names (job->sde_flush)) {
		dest_fs_type = g_strdup (liria_sde_flush_get_fs_type (job->sde_flush));
	}

	/* This moves all files that we can do without copy + delete */
	move_files_prepare (job, dest_fs_id, &dest_fs_type, &fallbacks);""")
    edit_once(ops, """	verify_destination (&job->common,
			    job->destination,
			    NULL,
			    source_info.num_bytes);
	if (job_aborted (common)) {
		goto aborted;
	}
""", """	verify_destination (&job->common,
			    job->destination,
			    NULL,
			    source_info.num_bytes);
	if (!job_aborted (common)) {
		sde_check_large_files (job, &source_info);
	}
	if (job_aborted (common)) {
		goto aborted;
	}
""")
    edit_once(ops, """ aborted:
    	g_list_free_full (fallbacks, g_free);
""", """ aborted:
	sde_finish_writing (job);
    	g_list_free_full (fallbacks, g_free);
""")
    overlay_file = staged / "libliria-private/liria-sde-copy.c"
    if not overlay_file.exists():
        raise ValueError("liria-sde-copy.c overlay missing")


def liria_safe_remove(staged):
    """alpha15: Eject, Unmount and Safely Remove first write all cached data
    to the drive (syncfs in a file-operation job, shown in the progress
    window as "Writing data to ..."), then detach it. Unmount no longer
    blocks silently for minutes after a copy."""
    ops = staged / "libliria-private/liria-file-operations.c"
    edit_once(ops, "static gboolean\ndir_has_files (GFile *dir)\n", r"""/* SDE: flush mounted file systems before they are detached. */
typedef struct {
	CommonJob common;
	GList *paths;
	char *name;
	LiriaOpCallback done_callback;
	gpointer done_callback_data;
} SdeSyncJob;

static gboolean
sde_sync_job_done (gpointer user_data)
{
	SdeSyncJob *job = user_data;

	if (job->done_callback) {
		job->done_callback (job->done_callback_data);
	}
	g_list_free_full (job->paths, g_free);
	g_free (job->name);
	finalize_common ((CommonJob *) job);
	return FALSE;
}

static gboolean
sde_sync_job (GIOSchedulerJob *io_job,
	      GCancellable *cancellable,
	      gpointer user_data)
{
	SdeSyncJob *job = user_data;
	GList *l;

	job->common.io_job = io_job;
	liria_progress_info_start (job->common.progress);
	for (l = job->paths; l != NULL; l = l->next) {
		sde_wait_for_device ((CommonJob *) job, NULL, l->data, job->name);
	}
	liria_progress_info_take_status (job->common.progress,
					    f (_("Removing \"%s\"\xE2\x80\xA6"), job->name));
	g_io_scheduler_job_send_to_mainloop_async (io_job, sde_sync_job_done, job, NULL);
	return FALSE;
}

static void
sde_add_mount_path (GList **paths, GMount *mount)
{
	GFile *root = mount ? g_mount_get_root (mount) : NULL;
	char *path = root ? g_file_get_path (root) : NULL;

	if (path != NULL && !g_list_find_custom (*paths, path, (GCompareFunc) g_strcmp0)) {
		*paths = g_list_append (*paths, path);
	} else {
		g_free (path);
	}
	if (root != NULL) {
		g_object_unref (root);
	}
}

void
liria_file_operations_sde_sync_mounts (GtkWindow *parent_window,
				       GMount *mount,
				       GVolume *volume,
				       GDrive *drive,
				       LiriaOpCallback done_callback,
				       gpointer done_callback_data)
{
	SdeSyncJob *job;
	GList *paths = NULL, *volumes, *l;
	char *name = NULL;

	if (mount != NULL) {
		sde_add_mount_path (&paths, mount);
		name = g_mount_get_name (mount);
	} else if (volume != NULL) {
		GMount *m = g_volume_get_mount (volume);

		sde_add_mount_path (&paths, m);
		if (m != NULL) {
			g_object_unref (m);
		}
		name = g_volume_get_name (volume);
	} else if (drive != NULL) {
		volumes = g_drive_get_volumes (drive);
		for (l = volumes; l != NULL; l = l->next) {
			GMount *m = g_volume_get_mount (l->data);

			sde_add_mount_path (&paths, m);
			if (m != NULL) {
				g_object_unref (m);
			}
		}
		g_list_free_full (volumes, g_object_unref);
		name = g_drive_get_name (drive);
	}
	if (paths == NULL) {
		g_free (name);
		if (done_callback) {
			done_callback (done_callback_data);
		}
		return;
	}
	job = op_job_new (SdeSyncJob, parent_window, TRUE, FALSE);
	job->paths = paths;
	job->name = name ? name : g_strdup (_("the drive"));
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	g_io_scheduler_push_job (sde_sync_job, job, NULL, 0, NULL);
}

static void
sde_sync_then_unmount (gpointer user_data)
{
	UnmountData *data = user_data;

	liria_file_operations_sde_sync_mounts (data->parent_window, data->mount, NULL, NULL,
					       (LiriaOpCallback) do_unmount, data);
}

static gboolean
dir_has_files (GFile *dir)
""")
    edit_once(ops, "\t\t\tjob->done_callback = (LiriaOpCallback)do_unmount;",
         "\t\t\tjob->done_callback = (LiriaOpCallback)sde_sync_then_unmount;")
    edit_once(ops, "\tdo_unmount (data);\n}\n\nvoid\nliria_file_operations_unmount_mount (",
         "\tsde_sync_then_unmount (data);\n}\n\nvoid\nliria_file_operations_unmount_mount (")
    edit_once(staged / "libliria-private/liria-file-operations.h",
         "void liria_file_operations_unmount_mount_full (",
         "void liria_file_operations_sde_sync_mounts (GtkWindow *parent_window, GMount *mount,\n"
         "                                            GVolume *volume, GDrive *drive,\n"
         "                                            LiriaOpCallback done_callback,\n"
         "                                            gpointer done_callback_data);\n"
         "void liria_file_operations_unmount_mount_full (")
    sidebar = staged / "src/liria-places-sidebar.c"
    edit_once(sidebar, """static void
do_eject (GMount *mount,
          GVolume *volume,
          GDrive *drive,
          LiriaPlacesSidebar *sidebar)
{
""", """static void
sde_eject_now (GMount *mount,
               GVolume *volume,
               GDrive *drive,
               LiriaPlacesSidebar *sidebar)
{
""")
    edit_once(sidebar, """static void
eject_shortcut_cb (GtkMenuItem           *item,""", """/* SDE: write cached data to the drive (with a progress notice), then
 * eject or stop it. */
typedef struct {
    GMount *mount;
    GVolume *volume;
    GDrive *drive;
    LiriaPlacesSidebar *sidebar;
    gboolean stop;
} SdeEject;

static void drive_stop_cb (GObject *source_object, GAsyncResult *res, gpointer user_data);

static void
sde_eject_synced (gpointer data)
{
    SdeEject *e = data;

    if (!gtk_widget_in_destruction (GTK_WIDGET (e->sidebar)) && e->sidebar->window != NULL)
    {
        if (e->stop)
        {
            GMountOperation *mount_op;

            mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (e->sidebar))));
            g_drive_stop (e->drive, G_MOUNT_UNMOUNT_NONE, mount_op, NULL, drive_stop_cb,
                          g_object_ref (e->sidebar->window));
            g_object_unref (mount_op);
        }
        else
        {
            sde_eject_now (e->mount, e->volume, e->drive, e->sidebar);
        }
    }
    else if (e->sidebar->window != NULL)
    {
        liria_window_info_set_initiated_unmount (e->sidebar->window, FALSE);
    }
    if (e->mount) g_object_unref (e->mount);
    if (e->volume) g_object_unref (e->volume);
    if (e->drive) g_object_unref (e->drive);
    g_object_unref (e->sidebar);
    g_free (e);
}

static void
sde_sync_and_detach (GMount *mount, GVolume *volume, GDrive *drive,
                     LiriaPlacesSidebar *sidebar, gboolean stop)
{
    SdeEject *e = g_new0 (SdeEject, 1);

    e->mount = mount ? g_object_ref (mount) : NULL;
    e->volume = volume ? g_object_ref (volume) : NULL;
    e->drive = drive ? g_object_ref (drive) : NULL;
    e->sidebar = g_object_ref (sidebar);
    e->stop = stop;
    liria_window_info_set_initiated_unmount (sidebar->window, TRUE);
    liria_file_operations_sde_sync_mounts (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (sidebar))),
                                           mount, volume, drive, sde_eject_synced, e);
}

static void
do_eject (GMount *mount,
          GVolume *volume,
          GDrive *drive,
          LiriaPlacesSidebar *sidebar)
{
    sde_sync_and_detach (mount, volume, drive, sidebar, FALSE);
}

static void
eject_shortcut_cb (GtkMenuItem           *item,""")
    edit_once(sidebar, """        GMountOperation *mount_op;

        mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (sidebar))));
        liria_window_info_set_initiated_unmount (sidebar->window, TRUE);
        g_drive_stop (drive, G_MOUNT_UNMOUNT_NONE, mount_op, NULL, drive_stop_cb,
                      g_object_ref (sidebar->window));
        g_object_unref (mount_op);
    }
    g_object_unref (drive);""", """        sde_sync_and_detach (NULL, NULL, drive, sidebar, TRUE);
    }
    if (drive != NULL)
        g_object_unref (drive);""")


def liria_clipboard(staged):
    """alpha15: Copy/Cut/Paste between Liria and other applications. Liria
    offers and accepts x-special/gnome-copied-files (Nautilus, Nemo,
    Thunar, PCManFM, Qt file dialogs) and accepts text/uri-list, besides
    MATE's own target. A drag and drop no longer empties the clipboard
    (the collision check compared the dropped list with itself and blocked
    on the clipboard), and Liria stays running while it owns copied files
    or a file operation runs, so closing the last window neither loses the
    clipboard nor aborts a copy (SDE has no clipboard manager)."""
    clip = staged / "libliria-private/liria-clipboard.c"
    edit_once(clip, """    if (gtk_selection_data_get_data_type (selection_data) != copied_files_atom
            || gtk_selection_data_get_length (selection_data) <= 0)
    {
        items = NULL;
    }""", """    GdkAtom type = gtk_selection_data_get_data_type (selection_data);

    if (gtk_selection_data_get_length (selection_data) <= 0)
    {
        items = NULL;
    }
    else if (type == gdk_atom_intern_static_string ("text/uri-list"))
    {
        /* SDE: plain URI lists from other applications are copied. */
        char **uris = gtk_selection_data_get_uris (selection_data);

        items = NULL;
        for (int i = 0; uris != NULL && uris[i] != NULL; i++)
        {
            char *scheme = g_uri_parse_scheme (uris[i]);

            if (scheme != NULL && g_ascii_strcasecmp (scheme, "http") != 0 &&
                g_ascii_strcasecmp (scheme, "https") != 0)
                items = g_list_prepend (items, g_strdup (uris[i]));
            g_free (scheme);
        }
        items = g_list_reverse (items);
        g_strfreev (uris);
        if (cut != NULL)
            *cut = FALSE;
    }
    else if (type != copied_files_atom &&
             type != gdk_atom_intern_static_string ("x-special/gnome-copied-files"))
    {
        items = NULL;
    }""")
    edit_once(clip, """    collision = FALSE;
    data = gtk_clipboard_wait_for_contents (liria_clipboard_get (widget),
                                            copied_files_atom);
    if (data == NULL) {
        return;
    }

    clipboard_item_uris = liria_clipboard_get_uri_list_from_selection_data (data, NULL,
                          copied_files_atom);

    for (l = (GList *) item_uris; l; l = l->next) {
        if (g_list_find_custom ((GList *) item_uris, l->data,
                                (GCompareFunc) g_strcmp0)) {""", """    LiriaClipboardInfo *info;

    collision = FALSE;
    /* SDE: only Liria's own clipboard can hold these files; do not block
     * waiting for another application's clipboard. */
    info = liria_clipboard_monitor_get_clipboard_info (liria_clipboard_monitor_get ());
    data = NULL;
    if (info == NULL) {
        return;
    }

    clipboard_item_uris = NULL;
    for (l = info->files; l; l = l->next) {
        clipboard_item_uris = g_list_prepend (clipboard_item_uris, liria_file_get_uri (l->data));
    }

    for (l = (GList *) item_uris; l; l = l->next) {
        if (g_list_find_custom (clipboard_item_uris, l->data,
                                (GCompareFunc) g_strcmp0)) {""")
    edit_once(clip, """    if (clipboard_item_uris) {
        g_list_free_full (clipboard_item_uris, g_free);
    }
}""", """    if (clipboard_item_uris) {
        g_list_free_full (clipboard_item_uris, g_free);
    }
    if (data) {
        gtk_selection_data_free (data);
    }
}

/* SDE: the best target for files on the clipboard, or GDK_NONE. */
GdkAtom
liria_clipboard_files_target (GdkAtom *targets, int n_targets, GdkAtom copied_files_atom)
{
    GdkAtom gnome = gdk_atom_intern_static_string ("x-special/gnome-copied-files");
    GdkAtom uri_list = gdk_atom_intern_static_string ("text/uri-list");
    GdkAtom best = GDK_NONE;

    for (int i = 0; targets != NULL && i < n_targets; i++)
    {
        if (targets[i] == copied_files_atom)
            return copied_files_atom;
        if (targets[i] == gnome)
            best = gnome;
        else if (targets[i] == uri_list && best == GDK_NONE)
            best = uri_list;
    }
    return best;
}

typedef struct {
    GdkAtom copied_files_atom;
    GtkClipboardReceivedFunc callback;
    gpointer data;
} SdeFilesRequest;

static void
sde_files_targets_received (GtkClipboard *clipboard, GdkAtom *targets, int n_targets, gpointer user_data)
{
    SdeFilesRequest *request = user_data;
    GdkAtom target = liria_clipboard_files_target (targets, n_targets, request->copied_files_atom);

    gtk_clipboard_request_contents (clipboard, target != GDK_NONE ? target : request->copied_files_atom,
                                    request->callback, request->data);
    g_free (request);
}

/* SDE: requests the clipboard files in Liria's, GNOME's or the plain URI
 * list format, whichever the owner offers. */
void
liria_clipboard_request_files (GtkClipboard *clipboard, GdkAtom copied_files_atom,
                               GtkClipboardReceivedFunc callback, gpointer data)
{
    SdeFilesRequest *request = g_new0 (SdeFilesRequest, 1);

    request->copied_files_atom = copied_files_atom;
    request->callback = callback;
    request->data = data;
    gtk_clipboard_request_targets (clipboard, sde_files_targets_received, request);
}""")
    edit_once(clip, '#include "liria-clipboard.h"\n', '#include "liria-clipboard.h"\n#include "liria-clipboard-monitor.h"\n#include "liria-file.h"\n')
    edit_once(staged / "libliria-private/liria-clipboard.h", "GtkClipboard*",
         "GdkAtom liria_clipboard_files_target (GdkAtom *targets, int n_targets, GdkAtom copied_files_atom);\n"
         "void liria_clipboard_request_files (GtkClipboard *clipboard, GdkAtom copied_files_atom,\n"
         "                                    GtkClipboardReceivedFunc callback, gpointer data);\n"
         "GtkClipboard*")

    view = staged / "src/file-manager/fm-directory-view.c"
    edit_once(view, "        gtk_target_list_add (target_list, copied_files_atom, 0, 0);\n",
         "        gtk_target_list_add (target_list, copied_files_atom, 0, 0);\n"
         "        gtk_target_list_add (target_list, gdk_atom_intern_static_string (\"x-special/gnome-copied-files\"), 0, 0);\n")
    edit_once(view, """	gtk_clipboard_request_contents (liria_clipboard_get (GTK_WIDGET (view)),
					copied_files_atom,
					paste_clipboard_received_callback,
					view);""", """	liria_clipboard_request_files (liria_clipboard_get (GTK_WIDGET (view)),
				       copied_files_atom,
				       paste_clipboard_received_callback,
				       view);""")
    edit_once(view, """	gtk_clipboard_request_contents (liria_clipboard_get (GTK_WIDGET (view)),
					copied_files_atom,
					paste_into_clipboard_received_callback,
					data);""", """	liria_clipboard_request_files (liria_clipboard_get (GTK_WIDGET (view)),
				       copied_files_atom,
				       paste_into_clipboard_received_callback,
				       data);""")
    edit_once(view, """		for (i=0; i < n_targets; i++) {
			if (targets[i] == copied_files_atom) {
				can_paste = TRUE;
			}
		}""", """		can_paste = liria_clipboard_files_target (targets, n_targets, copied_files_atom) != GDK_NONE;
		(void) i;""")

    tree = staged / "src/file-manager/fm-tree-view.c"
    edit_once(tree, "    gtk_target_list_add (target_list, copied_files_atom, 0, 0);\n",
         "    gtk_target_list_add (target_list, copied_files_atom, 0, 0);\n"
         "    gtk_target_list_add (target_list, gdk_atom_intern_static_string (\"x-special/gnome-copied-files\"), 0, 0);\n")

    monitor = staged / "libliria-private/liria-clipboard-monitor.c"
    edit_once(monitor, "    else if (target == copied_files_atom)\n",
         "    else if (target == copied_files_atom ||\n"
         "             target == gdk_atom_intern_static_string (\"x-special/gnome-copied-files\"))\n")
    edit_once(monitor, "        gtk_selection_data_set (selection_data, copied_files_atom, 8, str, len);",
         "        gtk_selection_data_set (selection_data, target, 8, str, len);")
    edit_once(monitor, """    monitor->details->info = liria_clipboard_info_copy (info);
""", """    monitor->details->info = liria_clipboard_info_copy (info);

    /* SDE: keep the process (and so the clipboard) alive while Liria owns
     * copied files, even after its last window closes. */
    {
        static gboolean held = FALSE;
        GApplication *app = g_application_get_default ();

        if (app != NULL && (monitor->details->info != NULL) != held)
        {
            held = monitor->details->info != NULL;
            if (held)
                g_application_hold (app);
            else
                g_application_release (app);
        }
    }
""")
    # A running copy or move keeps Liria alive after its window closes.
    ops = staged / "libliria-private/liria-file-operations.c"
    edit_once(ops, "\tcommon = g_malloc0 (job_size);\n",
         "\tcommon = g_malloc0 (job_size);\n\tif (g_application_get_default () != NULL) {\n"
         "\t\tg_application_hold (g_application_get_default ()); /* SDE */\n\t}\n")
    edit_once(ops, "static void\nfinalize_common (CommonJob *common)\n{\n",
         "static void\nfinalize_common (CommonJob *common)\n{\n\tif (g_application_get_default () != NULL) {\n"
         "\t\tg_application_release (g_application_get_default ()); /* SDE */\n\t}\n")


RECIPES = {"liria": liria}


def apply(record, staged):
    overlay(record["id"], staged)
    # Overlay sources carry the SDE build version as a placeholder.
    for path in staged.rglob("*.c"):
        text = path.read_text(errors="surrogateescape")
        if "@SDE_BUILD_VERSION@" in text:
            path.write_text(text.replace("@SDE_BUILD_VERSION@", record["build_version"]), errors="surrogateescape")
    recipe = RECIPES.get(record["id"])
    if recipe:
        recipe(staged)
