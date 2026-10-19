/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 *
 * Photo Effects: presets (filters) and adjustments that are previewed live
 * on the open picture, in a side pane page. The original file is never
 * changed; "Save Copy..." writes the edited picture to a new file.
 * Plugin structure follows the bundled reload plugin by Lucas Rocha.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "sdeview-effects-plugin.h"

#include <math.h>
#include <string.h>

#include <gmodule.h>
#include <glib/gi18n-lib.h>
#include <libpeas/peas-activatable.h>

#include <sdeview-debug.h>
#include <sdeview-image.h>
#include <sdeview-sidebar.h>
#include <sdeview-window.h>
#include <sdeview-window-activatable.h>

static void sdeview_window_activatable_iface_init (SdeviewWindowActivatableInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (SdeviewEffectsPlugin,
                                sdeview_effects_plugin,
                                PEAS_TYPE_EXTENSION_BASE,
                                0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (SDEVIEW_TYPE_WINDOW_ACTIVATABLE,
                                                               sdeview_window_activatable_iface_init))

enum {
	PROP_0,
	PROP_WINDOW
};

typedef struct {
	const gchar *id;
	const gchar *label;
	/* brightness, contrast, saturation, warmth, fade, vignette (slider units) */
	gdouble values[6];
} Preset;

static const Preset presets[] = {
	{ "original", N_("Original"), { 0, 0, 100, 0, 0, 0 } },
	{ "vivid",    N_("Vivid"),    { 4, 18, 145, 6, 0, 10 } },
	{ "warm",     N_("Warm"),     { 4, 6, 110, 45, 6, 0 } },
	{ "cool",     N_("Cool"),     { 0, 8, 95, -45, 0, 0 } },
	{ "mono",     N_("Mono"),     { 0, 12, 0, 0, 0, 0 } },
	{ "noir",     N_("Noir"),     { -6, 45, 0, 0, 0, 35 } },
	{ "sepia",    N_("Sepia"),    { 4, 4, 100, 0, 10, 15 } },
	{ "vintage",  N_("Vintage"),  { 6, -12, 80, 30, 35, 40 } },
	{ "matte",    N_("Matte"),    { 6, -18, 90, 8, 45, 0 } },
	{ "invert",   N_("Invert"),   { 0, 0, 100, 0, 0, 0 } },
};

static inline gdouble
clamp01 (gdouble v)
{
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static gdouble
smoothstep (gdouble a, gdouble b, gdouble x)
{
	gdouble t = clamp01 ((x - a) / (b - a));
	return t * t * (3.0 - 2.0 * t);
}

static gdouble
value_of (GtkWidget *scale)
{
	return gtk_range_get_value (GTK_RANGE (scale));
}

/* Separable box blur in place, used by "Soften". */
static void
box_blur (guchar *pixels, gint width, gint height, gint stride, gint channels, gint radius)
{
	gint x, y, c, i, len = MAX (width, height);
	guint *acc = g_new (guint, channels);
	guchar *line = g_new (guchar, (gsize) len * channels);

	for (y = 0; y < height; y++) {
		guchar *row = pixels + (gsize) y * stride;
		memcpy (line, row, (gsize) width * channels);
		for (x = 0; x < width; x++) {
			gint n = 0;
			memset (acc, 0, sizeof (guint) * channels);
			for (i = MAX (0, x - radius); i <= MIN (width - 1, x + radius); i++, n++)
				for (c = 0; c < channels; c++) acc[c] += line[i * channels + c];
			for (c = 0; c < channels; c++) row[x * channels + c] = (guchar) (acc[c] / n);
		}
	}
	for (x = 0; x < width; x++) {
		for (y = 0; y < height; y++)
			memcpy (line + y * channels, pixels + (gsize) y * stride + x * channels, channels);
		for (y = 0; y < height; y++) {
			gint n = 0;
			memset (acc, 0, sizeof (guint) * channels);
			for (i = MAX (0, y - radius); i <= MIN (height - 1, y + radius); i++, n++)
				for (c = 0; c < channels; c++) acc[c] += line[i * channels + c];
			for (c = 0; c < channels; c++)
				pixels[(gsize) y * stride + x * channels + c] = (guchar) (acc[c] / n);
		}
	}
	g_free (line);
	g_free (acc);
}

/* Renders |src| into |dst| (same size and layout) with the current settings. */
static void
render (SdeviewEffectsPlugin *plugin, GdkPixbuf *src, GdkPixbuf *dst)
{
	gint width = gdk_pixbuf_get_width (src), height = gdk_pixbuf_get_height (src);
	gint channels = gdk_pixbuf_get_n_channels (src);
	gint sstride = gdk_pixbuf_get_rowstride (src), dstride = gdk_pixbuf_get_rowstride (dst);
	const guchar *in = gdk_pixbuf_read_pixels (src);
	guchar *out = gdk_pixbuf_get_pixels (dst);
	gdouble brightness = value_of (plugin->brightness) / 200.0;
	gdouble contrast = 1.0 + value_of (plugin->contrast) / 100.0;
	gdouble saturation = value_of (plugin->saturation) / 100.0;
	gdouble warmth = value_of (plugin->warmth) / 1000.0;
	gdouble fade = value_of (plugin->fade) / 100.0 * 0.22;
	gdouble vignette = value_of (plugin->vignette) / 100.0 * 0.75;
	gint soften = (gint) value_of (plugin->soften);
	gboolean sepia = g_strcmp0 (plugin->preset, "sepia") == 0 || g_strcmp0 (plugin->preset, "vintage") == 0;
	gboolean invert = g_strcmp0 (plugin->preset, "invert") == 0;
	gdouble cx = width / 2.0, cy = height / 2.0, radius = sqrt (cx * cx + cy * cy);
	gint x, y;

	contrast = contrast < 0.0 ? 0.0 : contrast;
	for (y = 0; y < height; y++) {
		const guchar *p = in + (gsize) y * sstride;
		guchar *q = out + (gsize) y * dstride;
		gdouble dy = (y - cy) / radius;
		for (x = 0; x < width; x++, p += channels, q += channels) {
			gdouble r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0, l;
			if (invert) {
				r = 1.0 - r; g = 1.0 - g; b = 1.0 - b;
			}
			if (sepia) {
				gdouble sr = 0.393 * r + 0.769 * g + 0.189 * b;
				gdouble sg = 0.349 * r + 0.686 * g + 0.168 * b;
				gdouble sb = 0.272 * r + 0.534 * g + 0.131 * b;
				r = sr; g = sg; b = sb;
			}
			r += brightness; g += brightness; b += brightness;
			r = (r - 0.5) * contrast + 0.5;
			g = (g - 0.5) * contrast + 0.5;
			b = (b - 0.5) * contrast + 0.5;
			l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
			r = l + (r - l) * saturation;
			g = l + (g - l) * saturation;
			b = l + (b - l) * saturation;
			r += warmth; b -= warmth;
			r = fade + r * (1.0 - fade);
			g = fade + g * (1.0 - fade);
			b = fade + b * (1.0 - fade);
			if (vignette > 0.0) {
				gdouble dx = (x - cx) / radius;
				gdouble k = 1.0 - vignette * smoothstep (0.45, 1.0, sqrt (dx * dx + dy * dy) * 1.414);
				r *= k; g *= k; b *= k;
			}
			q[0] = (guchar) lround (clamp01 (r) * 255.0);
			q[1] = (guchar) lround (clamp01 (g) * 255.0);
			q[2] = (guchar) lround (clamp01 (b) * 255.0);
			if (channels == 4) q[3] = p[3];
		}
	}
	if (soften > 0)
		box_blur (out, width, height, dstride, channels, soften);
}

static gboolean
is_default (SdeviewEffectsPlugin *plugin)
{
	return g_strcmp0 (plugin->preset, "original") == 0 && value_of (plugin->brightness) == 0 &&
	       value_of (plugin->contrast) == 0 && value_of (plugin->saturation) == 100 &&
	       value_of (plugin->warmth) == 0 && value_of (plugin->fade) == 0 &&
	       value_of (plugin->vignette) == 0 && value_of (plugin->soften) == 0;
}

static void
set_status (SdeviewEffectsPlugin *plugin, const gchar *text)
{
	gtk_label_set_text (GTK_LABEL (plugin->status), text ? text : "");
}

/* Puts the untouched pixels back into the picture and forgets it. */
static void
restore_original (SdeviewEffectsPlugin *plugin)
{
	if (plugin->image != NULL && plugin->original != NULL) {
		GdkPixbuf *pixbuf = sdeview_image_get_pixbuf (plugin->image);
		if (pixbuf != NULL) {
			if (gdk_pixbuf_get_width (pixbuf) == gdk_pixbuf_get_width (plugin->original) &&
			    gdk_pixbuf_get_height (pixbuf) == gdk_pixbuf_get_height (plugin->original)) {
				gdk_pixbuf_copy_area (plugin->original, 0, 0,
				                      gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf),
				                      pixbuf, 0, 0);
				sdeview_image_modified (plugin->image);
			}
			g_object_unref (pixbuf);
		}
	}
	g_clear_object (&plugin->original);
	g_clear_object (&plugin->image);
}

/* Returns the editable pixbuf of the current picture, or NULL. */
static GdkPixbuf *
current_pixbuf (SdeviewEffectsPlugin *plugin)
{
	SdeviewImage *image = sdeview_window_get_image (plugin->window);
	GdkPixbuf *pixbuf;

	if (image == NULL || sdeview_image_is_animation (image)
#ifdef HAVE_RSVG
	    || sdeview_image_is_svg (image)
#endif
	    ) {
		set_status (plugin, _("Effects are not available for this picture."));
		return NULL;
	}
	if (image != plugin->image)
		restore_original (plugin);
	pixbuf = sdeview_image_get_pixbuf (image);
	if (pixbuf == NULL || gdk_pixbuf_get_colorspace (pixbuf) != GDK_COLORSPACE_RGB ||
	    gdk_pixbuf_get_bits_per_sample (pixbuf) != 8 || gdk_pixbuf_get_n_channels (pixbuf) < 3) {
		if (pixbuf != NULL) g_object_unref (pixbuf);
		set_status (plugin, _("Effects are not available for this picture."));
		return NULL;
	}
	if (plugin->original == NULL) {
		plugin->original = gdk_pixbuf_copy (pixbuf);
		plugin->image = g_object_ref (image);
	}
	return pixbuf;
}

static gboolean
apply_now (gpointer data)
{
	SdeviewEffectsPlugin *plugin = data;
	GdkPixbuf *pixbuf;

	plugin->pending = 0;
	pixbuf = current_pixbuf (plugin);
	if (pixbuf == NULL)
		return G_SOURCE_REMOVE;
	render (plugin, plugin->original, pixbuf);
	g_object_unref (pixbuf);
	sdeview_image_modified (plugin->image);
	set_status (plugin, is_default (plugin) ? "" : _("Preview. The original file is unchanged."));
	return G_SOURCE_REMOVE;
}

static void
schedule_apply (SdeviewEffectsPlugin *plugin)
{
	if (plugin->syncing)
		return;
	if (plugin->pending)
		g_source_remove (plugin->pending);
	plugin->pending = g_timeout_add (90, apply_now, plugin);
}

static void
set_sliders (SdeviewEffectsPlugin *plugin, const gdouble *v, gdouble soften)
{
	plugin->syncing = TRUE;
	gtk_range_set_value (GTK_RANGE (plugin->brightness), v[0]);
	gtk_range_set_value (GTK_RANGE (plugin->contrast), v[1]);
	gtk_range_set_value (GTK_RANGE (plugin->saturation), v[2]);
	gtk_range_set_value (GTK_RANGE (plugin->warmth), v[3]);
	gtk_range_set_value (GTK_RANGE (plugin->fade), v[4]);
	gtk_range_set_value (GTK_RANGE (plugin->vignette), v[5]);
	gtk_range_set_value (GTK_RANGE (plugin->soften), soften);
	plugin->syncing = FALSE;
}

static void
preset_clicked (GtkButton *button, SdeviewEffectsPlugin *plugin)
{
	const Preset *preset = g_object_get_data (G_OBJECT (button), "sde-preset");

	g_free (plugin->preset);
	plugin->preset = g_strdup (preset->id);
	set_sliders (plugin, preset->values, 0);
	schedule_apply (plugin);
}

static void
slider_changed (GtkRange *range, SdeviewEffectsPlugin *plugin)
{
	schedule_apply (plugin);
}

static void
reset_clicked (GtkButton *button, SdeviewEffectsPlugin *plugin)
{
	g_free (plugin->preset);
	plugin->preset = g_strdup ("original");
	set_sliders (plugin, presets[0].values, 0);
	restore_original (plugin);
	set_status (plugin, "");
}

static const gchar *
format_for (const gchar *path)
{
	gchar *lower = g_ascii_strdown (path, -1);
	const gchar *format = "png";

	if (g_str_has_suffix (lower, ".jpg") || g_str_has_suffix (lower, ".jpeg"))
		format = "jpeg";
	else if (g_str_has_suffix (lower, ".tif") || g_str_has_suffix (lower, ".tiff"))
		format = "tiff";
	else if (g_str_has_suffix (lower, ".bmp"))
		format = "bmp";
	g_free (lower);
	return format;
}

static void
save_clicked (GtkButton *button, SdeviewEffectsPlugin *plugin)
{
	GtkWidget *dialog;
	GdkPixbuf *pixbuf;
	gchar *base, *name, *dot;
	GFile *file;

	if (plugin->pending)
		apply_now (plugin);
	pixbuf = current_pixbuf (plugin);
	if (pixbuf == NULL)
		return;

	dialog = gtk_file_chooser_dialog_new (_("Save Edited Copy"), GTK_WINDOW (plugin->window),
	                                      GTK_FILE_CHOOSER_ACTION_SAVE,
	                                      _("_Cancel"), GTK_RESPONSE_CANCEL,
	                                      _("_Save"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);
	file = sdeview_image_get_file (plugin->image);
	if (file != NULL) {
		GFile *parent = g_file_get_parent (file);
		if (parent != NULL) {
			gtk_file_chooser_set_current_folder_file (GTK_FILE_CHOOSER (dialog), parent, NULL);
			g_object_unref (parent);
		}
		base = g_file_get_basename (file);
		g_object_unref (file);
	} else {
		base = g_strdup ("photo.png");
	}
	dot = strrchr (base, '.');
	name = dot ? g_strdup_printf ("%.*s-edited%s", (int) (dot - base), base, dot)
	           : g_strdup_printf ("%s-edited.png", base);
	gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), name);
	g_free (name);
	g_free (base);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
		gchar *path = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
		const gchar *format = format_for (path);
		GError *error = NULL;
		gboolean ok;

		if (g_str_equal (format, "jpeg"))
			ok = gdk_pixbuf_save (pixbuf, path, format, &error, "quality", "92", NULL);
		else
			ok = gdk_pixbuf_save (pixbuf, path, format, &error, NULL);
		if (ok) {
			gchar *text = g_strdup_printf (_("Saved %s"), path);
			set_status (plugin, text);
			g_free (text);
		} else {
			set_status (plugin, error ? error->message : _("The picture could not be saved."));
			g_clear_error (&error);
		}
		g_free (path);
	}
	gtk_widget_destroy (dialog);
	g_object_unref (pixbuf);
}

static GtkWidget *
add_slider (SdeviewEffectsPlugin *plugin, GtkGrid *grid, gint row, const gchar *label,
            gdouble min, gdouble max, gdouble value)
{
	GtkWidget *text = gtk_label_new (label);
	GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, min, max, 1);

	gtk_label_set_xalign (GTK_LABEL (text), 0.0);
	gtk_style_context_add_class (gtk_widget_get_style_context (text), "dim-label");
	gtk_range_set_value (GTK_RANGE (scale), value);
	gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
	gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
	gtk_scale_set_digits (GTK_SCALE (scale), 0);
	gtk_scale_add_mark (GTK_SCALE (scale), value, GTK_POS_BOTTOM, NULL);
	gtk_widget_set_hexpand (scale, TRUE);
	g_signal_connect (scale, "value-changed", G_CALLBACK (slider_changed), plugin);
	gtk_grid_attach (grid, text, 0, row * 2, 1, 1);
	gtk_grid_attach (grid, scale, 0, row * 2 + 1, 1, 1);
	return scale;
}

static GtkWidget *
build_page (SdeviewEffectsPlugin *plugin)
{
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
	GtkWidget *title = gtk_label_new (NULL);
	GtkWidget *flow = gtk_flow_box_new ();
	GtkWidget *grid = gtk_grid_new ();
	GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *reset = gtk_button_new_with_mnemonic (_("_Reset"));
	GtkWidget *save = gtk_button_new_with_mnemonic (_("Save _Copy..."));
	GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
	guint i;

	gtk_style_context_add_class (gtk_widget_get_style_context (box), "sdeview-effects");
	gtk_label_set_markup (GTK_LABEL (title), _("<b>Filters</b>"));
	gtk_label_set_xalign (GTK_LABEL (title), 0.0);
	gtk_box_pack_start (GTK_BOX (box), title, FALSE, FALSE, 0);

	gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 3);
	gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 2);
	gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 4);
	gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 4);
	gtk_style_context_add_class (gtk_widget_get_style_context (flow), "preset");
	for (i = 0; i < G_N_ELEMENTS (presets); i++) {
		GtkWidget *button = gtk_button_new_with_label (_(presets[i].label));
		g_object_set_data (G_OBJECT (button), "sde-preset", (gpointer) &presets[i]);
		g_signal_connect (button, "clicked", G_CALLBACK (preset_clicked), plugin);
		gtk_container_add (GTK_CONTAINER (flow), button);
	}
	gtk_box_pack_start (GTK_BOX (box), flow, FALSE, FALSE, 0);

	title = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (title), _("<b>Adjust</b>"));
	gtk_label_set_xalign (GTK_LABEL (title), 0.0);
	gtk_box_pack_start (GTK_BOX (box), title, FALSE, FALSE, 0);

	gtk_grid_set_row_spacing (GTK_GRID (grid), 2);
	plugin->brightness = add_slider (plugin, GTK_GRID (grid), 0, _("Brightness"), -100, 100, 0);
	plugin->contrast = add_slider (plugin, GTK_GRID (grid), 1, _("Contrast"), -100, 100, 0);
	plugin->saturation = add_slider (plugin, GTK_GRID (grid), 2, _("Saturation"), 0, 200, 100);
	plugin->warmth = add_slider (plugin, GTK_GRID (grid), 3, _("Warmth"), -100, 100, 0);
	plugin->fade = add_slider (plugin, GTK_GRID (grid), 4, _("Fade"), 0, 100, 0);
	plugin->vignette = add_slider (plugin, GTK_GRID (grid), 5, _("Vignette"), 0, 100, 0);
	plugin->soften = add_slider (plugin, GTK_GRID (grid), 6, _("Soften"), 0, 8, 0);
	gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

	gtk_box_pack_start (GTK_BOX (buttons), reset, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (buttons), save, TRUE, TRUE, 0);
	gtk_style_context_add_class (gtk_widget_get_style_context (save), "suggested-action");
	g_signal_connect (reset, "clicked", G_CALLBACK (reset_clicked), plugin);
	g_signal_connect (save, "clicked", G_CALLBACK (save_clicked), plugin);
	gtk_box_pack_start (GTK_BOX (box), buttons, FALSE, FALSE, 0);

	plugin->status = gtk_label_new (NULL);
	gtk_label_set_line_wrap (GTK_LABEL (plugin->status), TRUE);
	gtk_label_set_xalign (GTK_LABEL (plugin->status), 0.0);
	gtk_style_context_add_class (gtk_widget_get_style_context (plugin->status), "dim-label");
	gtk_box_pack_start (GTK_BOX (box), plugin->status, FALSE, FALSE, 0);

	gtk_container_set_border_width (GTK_CONTAINER (box), 10);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add (GTK_CONTAINER (scroll), box);
	gtk_widget_show_all (scroll);
	return scroll;
}

static void
selection_changed (GtkIconView *view, SdeviewEffectsPlugin *plugin)
{
	/* Another picture: the previous one gets its own pixels back. */
	if (plugin->pending) {
		g_source_remove (plugin->pending);
		plugin->pending = 0;
	}
	if (plugin->image != NULL && plugin->image != sdeview_window_get_image (plugin->window)) {
		restore_original (plugin);
		reset_clicked (NULL, plugin);
	}
}

static void
show_effects (GtkAction *action, SdeviewEffectsPlugin *plugin)
{
	GtkUIManager *manager = sdeview_window_get_ui_manager (plugin->window);
	GList *groups;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	for (groups = gtk_ui_manager_get_action_groups (manager); groups; groups = groups->next) {
		GtkAction *toggle = gtk_action_group_get_action (groups->data, "ViewSidebar");
		if (toggle != NULL && GTK_IS_TOGGLE_ACTION (toggle))
			gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (toggle), TRUE);
	}
	G_GNUC_END_IGNORE_DEPRECATIONS;
	sdeview_sidebar_set_page (SDEVIEW_SIDEBAR (sdeview_window_get_sidebar (plugin->window)), plugin->page);
}

static const gchar *const ui_definition = "<ui><menubar name=\"MainMenu\">"
	"<menu name=\"ToolsMenu\" action=\"Tools\"><separator/>"
	"<menuitem name=\"SdeviewPluginEffects\" action=\"SdeviewPluginRunEffects\"/>"
	"<separator/></menu></menubar>"
	"<popup name=\"ViewPopup\"><separator/>"
	"<menuitem action=\"SdeviewPluginRunEffects\"/><separator/>"
	"</popup></ui>";

static const GtkActionEntry action_entries[] = {
	{ "SdeviewPluginRunEffects", "applications-graphics", N_("_Effects and Filters"), "E",
	  N_("Edit the picture with filters and adjustments"), G_CALLBACK (show_effects) }
};

static void
sdeview_effects_plugin_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	SdeviewEffectsPlugin *plugin = SDEVIEW_EFFECTS_PLUGIN (object);

	switch (prop_id) {
	case PROP_WINDOW:
		plugin->window = SDEVIEW_WINDOW (g_value_dup_object (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
sdeview_effects_plugin_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	SdeviewEffectsPlugin *plugin = SDEVIEW_EFFECTS_PLUGIN (object);

	switch (prop_id) {
	case PROP_WINDOW:
		g_value_set_object (value, plugin->window);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
sdeview_effects_plugin_init (SdeviewEffectsPlugin *plugin)
{
	plugin->preset = g_strdup ("original");
}

static void
sdeview_effects_plugin_dispose (GObject *object)
{
	SdeviewEffectsPlugin *plugin = SDEVIEW_EFFECTS_PLUGIN (object);

	if (plugin->pending) {
		g_source_remove (plugin->pending);
		plugin->pending = 0;
	}
	g_clear_object (&plugin->original);
	g_clear_object (&plugin->image);
	g_clear_object (&plugin->window);
	g_clear_pointer (&plugin->preset, g_free);
	G_OBJECT_CLASS (sdeview_effects_plugin_parent_class)->dispose (object);
}

static void
sdeview_effects_plugin_activate (SdeviewWindowActivatable *activatable)
{
	SdeviewEffectsPlugin *plugin = SDEVIEW_EFFECTS_PLUGIN (activatable);
	GtkUIManager *manager = sdeview_window_get_ui_manager (plugin->window);
	GtkWidget *sidebar = sdeview_window_get_sidebar (plugin->window);
	GtkWidget *thumbs = sdeview_window_get_thumb_view (plugin->window);

	sdeview_debug (DEBUG_PLUGINS);
	plugin->page = build_page (plugin);
	sdeview_sidebar_add_page (SDEVIEW_SIDEBAR (sidebar), _("Effects"), plugin->page);
	if (thumbs != NULL)
		plugin->selection_id = g_signal_connect (thumbs, "selection-changed",
		                                         G_CALLBACK (selection_changed), plugin);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	plugin->ui_action_group = gtk_action_group_new ("SdeviewEffectsPluginActions");
#ifdef ENABLE_NLS
	gtk_action_group_set_translation_domain (plugin->ui_action_group, GETTEXT_PACKAGE);
#endif
	gtk_action_group_add_actions (plugin->ui_action_group, action_entries,
	                              G_N_ELEMENTS (action_entries), plugin);
	gtk_ui_manager_insert_action_group (manager, plugin->ui_action_group, -1);
	plugin->ui_id = gtk_ui_manager_add_ui_from_string (manager, ui_definition, -1, NULL);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_warn_if_fail (plugin->ui_id != 0);
}

static void
sdeview_effects_plugin_deactivate (SdeviewWindowActivatable *activatable)
{
	SdeviewEffectsPlugin *plugin = SDEVIEW_EFFECTS_PLUGIN (activatable);
	GtkUIManager *manager = sdeview_window_get_ui_manager (plugin->window);
	GtkWidget *thumbs = sdeview_window_get_thumb_view (plugin->window);

	sdeview_debug (DEBUG_PLUGINS);
	restore_original (plugin);
	if (thumbs != NULL && plugin->selection_id) {
		g_signal_handler_disconnect (thumbs, plugin->selection_id);
		plugin->selection_id = 0;
	}
	sdeview_sidebar_remove_page (SDEVIEW_SIDEBAR (sdeview_window_get_sidebar (plugin->window)), plugin->page);
	plugin->page = NULL;
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_ui_manager_remove_ui (manager, plugin->ui_id);
	gtk_ui_manager_remove_action_group (manager, plugin->ui_action_group);
	gtk_ui_manager_ensure_update (manager);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_clear_object (&plugin->ui_action_group);
}

static void
sdeview_effects_plugin_class_init (SdeviewEffectsPluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = sdeview_effects_plugin_dispose;
	object_class->set_property = sdeview_effects_plugin_set_property;
	object_class->get_property = sdeview_effects_plugin_get_property;
	g_object_class_override_property (object_class, PROP_WINDOW, "window");
}

static void
sdeview_effects_plugin_class_finalize (SdeviewEffectsPluginClass *klass)
{
}

static void
sdeview_window_activatable_iface_init (SdeviewWindowActivatableInterface *iface)
{
	iface->activate = sdeview_effects_plugin_activate;
	iface->deactivate = sdeview_effects_plugin_deactivate;
}

G_MODULE_EXPORT void
peas_register_types (PeasObjectModule *module)
{
	sdeview_effects_plugin_register_type (G_TYPE_MODULE (module));
	peas_object_module_register_extension_type (module,
	                                            SDEVIEW_TYPE_WINDOW_ACTIVATABLE,
	                                            SDEVIEW_TYPE_EFFECTS_PLUGIN);
}
