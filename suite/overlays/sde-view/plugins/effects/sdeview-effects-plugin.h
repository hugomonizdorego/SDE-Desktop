/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SDE contributors.
 * Photo Effects: live filters and adjustments for SDE Photo.
 */
#ifndef __SDEVIEW_EFFECTS_PLUGIN_H__
#define __SDEVIEW_EFFECTS_PLUGIN_H__

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <libpeas/peas-extension-base.h>
#include <libpeas/peas-object-module.h>

#include <sdeview-window.h>

G_BEGIN_DECLS

#define SDEVIEW_TYPE_EFFECTS_PLUGIN (sdeview_effects_plugin_get_type ())
#define SDEVIEW_EFFECTS_PLUGIN(o) \
	(G_TYPE_CHECK_INSTANCE_CAST ((o), SDEVIEW_TYPE_EFFECTS_PLUGIN, SdeviewEffectsPlugin))

typedef struct _SdeviewEffectsPlugin SdeviewEffectsPlugin;
typedef struct _SdeviewEffectsPluginClass SdeviewEffectsPluginClass;

struct _SdeviewEffectsPlugin {
	PeasExtensionBase parent_instance;

	SdeviewWindow *window;
	GtkWidget *page;
	GtkWidget *brightness, *contrast, *saturation, *warmth, *fade, *vignette, *soften;
	GtkWidget *status;
	GtkActionGroup *ui_action_group;
	guint ui_id;

	SdeviewImage *image;   /* image whose pixels are being edited */
	GdkPixbuf *original;   /* untouched copy of those pixels */
	gchar *preset;
	guint pending;         /* debounce for slider drags */
	gulong selection_id;
	gboolean syncing;
};

struct _SdeviewEffectsPluginClass {
	PeasExtensionBaseClass parent_class;
};

GType sdeview_effects_plugin_get_type (void) G_GNUC_CONST;

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

G_END_DECLS

#endif /* __SDEVIEW_EFFECTS_PLUGIN_H__ */
