# SPDX-License-Identifier: GPL-3.0-or-later
"""SDE alpha10 redesign of the native suite forks.

Applied by tools/prepare-native-suite.py after the namespace rename. Every
change is a small, checked text edit or an added file from suite/overlays;
upstream copyright notices, authors and About credits are kept unchanged.
A missing anchor stops preparation instead of silently skipping a change.
"""
import re
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
OVERLAYS = HERE / "overlays"


def inputs():
    """Files whose content changes the prepared sources (for the lock hash)."""
    return [Path(__file__)] + sorted(p for p in OVERLAYS.rglob("*") if p.is_file())


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


def overlay(record_id, staged):
    source = OVERLAYS / record_id
    if not source.is_dir():
        return
    for path in source.rglob("*"):
        if path.is_file():
            target = staged / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def photo(staged):
    """SDE View becomes Photo, with a live Effects side pane plugin."""
    window = staged / "src/sdeview-window.c"
    edit(window, '"comments",_("Photo is a simple graphics viewer for the MATE Desktop Environment."),',
         '"comments",_("Photo shows, browses and lightly edits pictures in SDE.\\n'
         'Based on Eye of MATE by the MATE developers."),')
    plugins = staged / "plugins/Makefile.am"
    edit(plugins, "plugins_LTLIBRARIES = \\\n\tfullscreen/libfullscreen.la  \\\n",
         "plugins_LTLIBRARIES = \\\n\teffects/libeffects.la  \\\n\tfullscreen/libfullscreen.la  \\\n")
    edit(plugins, "plugins_in_files = \\\n\tfullscreen/fullscreen.plugin.desktop.in  \\\n",
         "plugins_in_files = \\\n\teffects/effects.plugin.desktop.in  \\\n\tfullscreen/fullscreen.plugin.desktop.in  \\\n")
    edit(plugins, "# Fullscreen plugin\n",
         "# Effects plugin (SDE)\n"
         "effects_libeffects_la_SOURCES = \\\n"
         "\teffects/sdeview-effects-plugin.h  \\\n"
         "\teffects/sdeview-effects-plugin.c\n\n"
         "effects_libeffects_la_LIBADD = $(SDEVIEW_LIBS) -lm\n\n"
         "# Fullscreen plugin\n")
    edit(staged / "configure.ac", "plugins/fullscreen/fullscreen.plugin.desktop.in\n",
         "plugins/effects/effects.plugin.desktop.in\nplugins/fullscreen/fullscreen.plugin.desktop.in\n")


def liria(staged):
    """Compact icon-only Back/Forward buttons (tooltips keep the names), so
    the command bar reads as one glass strip instead of Caja's labels; and
    the built-in SDE Tools extension (suite/overlays/liria/sde-tools)."""
    edit(staged / "Makefile.am", "\tsrc \\\n", "\tsrc \\\n\tsde-tools \\\n")
    edit(staged / "configure.ac", "src/file-manager/Makefile\n", "src/file-manager/Makefile\nsde-tools/Makefile\n")
    menus = staged / "src/liria-navigation-window-menus.c"
    for direction in ("BACK", "FORWARD"):
        edit(menus, '"direction", LIRIA_NAVIGATION_DIRECTION_%s,\n                           "is_important", TRUE,' % direction,
             '"direction", LIRIA_NAVIGATION_DIRECTION_%s,\n                           "is_important", FALSE,' % direction)
    liria_command_bar(staged)
    liria_home(staged)
    liria_sidebar(staged)
    liria_copy_engine(staged)
    liria_safe_remove(staged)
    liria_clipboard(staged)


def liria_command_bar(staged):
    """alpha14: one command row. Back, Forward, Up and Reload, then the
    breadcrumb path (with its search field) stretched across the window,
    then View As, Search, extension buttons and a single main-menu button
    that holds the classic File/Edit/View/Go/Bookmarks/Help menus. Every
    action, shortcut and menu stays available; only the layout changes."""
    edit(staged / "src/liria-navigation-window-ui.xml",
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
         '\t<toolitem name="Reload" action="Reload"/>\n'
         '\t<toolitem name="ViewAs" action="ViewAs"/>\n')
    window = staged / "src/liria-navigation-window.c"
    edit(window, "static void\nliria_navigation_window_init (LiriaNavigationWindow *window)\n{",
         """/* SDE: opens a location in this window's active tab. The SDE Home
 * overview (an extension) finds it as the "sde-go-to-uri" object data. */
static void
sde_go_to_uri (GtkWidget *widget, const char *uri)
{
    GFile *location = g_file_new_for_uri (uri);

    liria_window_go_to (LIRIA_WINDOW (widget), location);
    g_object_unref (location);
}

/* SDE: the first pane's path bar row moves into the command bar, right
 * after Reload; it is destroyed together with its pane. */
static void
sde_merge_location_row (GtkWidget *toolbar, GtkUIManager *ui_manager, LiriaNavigationWindowPane *pane)
{
    GtkWidget *reload = gtk_ui_manager_get_widget (ui_manager, "/Toolbar/Reload");
    GtkToolItem *item = gtk_tool_item_new ();
    GtkWidget *row = pane->location_bar;
    gint index = -1;

    if (reload != NULL && gtk_widget_get_parent (reload) == toolbar)
        index = gtk_toolbar_get_item_index (GTK_TOOLBAR (toolbar), GTK_TOOL_ITEM (reload)) + 1;
    g_object_ref (row);
    gtk_container_remove (GTK_CONTAINER (gtk_widget_get_parent (row)), row);
    gtk_container_add (GTK_CONTAINER (item), row);
    g_object_unref (row);
    gtk_container_set_border_width (GTK_CONTAINER (row), 0);
    gtk_widget_set_margin_start (row, 8);
    gtk_widget_set_margin_end (row, 8);
    gtk_tool_item_set_expand (item, TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (item)), "sde-location-item");
    gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, index);
    gtk_widget_show (GTK_WIDGET (item));
    g_signal_connect_object (pane->widget, "destroy", G_CALLBACK (gtk_widget_destroy), item, G_CONNECT_SWAPPED);
}

/* SDE: the menu bar's menus move into one main-menu button at the end of
 * the command bar. F10 opens it, like the menu bar before. */
static void
sde_merge_menu_bar (GtkWidget *toolbar, GtkWidget *menubar)
{
    GtkToolItem *item = gtk_tool_item_new ();
    GtkWidget *bar = gtk_menu_bar_new ();
    GtkWidget *button = gtk_menu_item_new ();
    GtkWidget *menu = gtk_menu_new ();
    GtkWidget *image;
    GList *children, *l;
    const char *icon = "open-menu-symbolic";

    if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), icon))
        icon = "open-menu";
    if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), icon))
        icon = "emblem-system";
    image = gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_container_add (GTK_CONTAINER (button), image);
    gtk_widget_set_tooltip_text (button, _("Menu"));
    children = gtk_container_get_children (GTK_CONTAINER (menubar));
    for (l = children; l != NULL; l = l->next)
    {
        GtkWidget *child = l->data;

        g_object_ref (child);
        gtk_container_remove (GTK_CONTAINER (menubar), child);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), child);
        g_object_unref (child);
    }
    g_list_free (children);
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (button), menu);
    gtk_menu_shell_append (GTK_MENU_SHELL (bar), button);
    gtk_style_context_add_class (gtk_widget_get_style_context (bar), "sde-main-menu");
    gtk_container_add (GTK_CONTAINER (item), bar);
    gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);
    gtk_widget_show_all (GTK_WIDGET (item));
    gtk_widget_hide (menubar);
    gtk_widget_set_no_show_all (menubar, TRUE);
}

static void
liria_navigation_window_init (LiriaNavigationWindow *window)
{""")
    edit(window, """    gtk_widget_show (toolbar);

    liria_navigation_window_initialize_toolbars (window);
""", """    gtk_widget_show (toolbar);

    /* SDE: one command row with icon buttons, the path and the menu. */
    gtk_toolbar_set_style (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_show_arrow (GTK_TOOLBAR (toolbar), FALSE);
    sde_merge_location_row (toolbar, ui_manager, pane);
    sde_merge_menu_bar (toolbar, win->details->menubar);
    g_object_set_data (G_OBJECT (window), "sde-go-to-uri", sde_go_to_uri);

    liria_navigation_window_initialize_toolbars (window);
""")


def liria_home(staged):
    """The view's extension strip (where the SDE Home overview appears)
    becomes a flat, themable area instead of an etched frame."""
    edit(staged / "src/liria-window-slot.c",
         "    frame = gtk_frame_new (NULL);\n    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_ETCHED_IN);\n",
         "    frame = gtk_frame_new (NULL);\n    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_NONE);\n"
         "    gtk_style_context_add_class (gtk_widget_get_style_context (frame), \"liria-extra-location\");\n")


def liria_sidebar(staged):
    """Sidebar sections read Favorites (Home and the personal folders),
    Drives (the file system and devices), Bookmarks and Network."""
    sidebar = staged / "src/liria-places-sidebar.c"
    edit(sidebar, '    last_iter = add_heading (sidebar, SECTION_COMPUTER,\n                             _("Computer"));',
         '    last_iter = add_heading (sidebar, SECTION_COMPUTER,\n                             _("Favorites"));')
    edit(sidebar, '            add_heading (sidebar, SECTION_DEVICES,\n                     _("Devices"));',
         '            add_heading (sidebar, SECTION_DEVICES,\n                     _("Drives"));')
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


RECIPES = {"sde-view": photo, "liria": liria}


def apply(record, staged):
    overlay(record["id"], staged)
    recipe = RECIPES.get(record["id"])
    if recipe:
        recipe(staged)
