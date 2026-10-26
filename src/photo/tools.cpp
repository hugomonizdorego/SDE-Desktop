// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's plugins: the built-in ones, which the Plugins window switches on
// and off, the Tools menu, and action plugins (see plugins.h).
#include <FL/Fl.H>

#include <algorithm>

#include "collage_ui.h"
#include "config.h"
#include "i18n.h"
#include "link.h"
#include "menubar.h"
#include "photo.h"
#include "process.h"
#include "spawn.h"
#include "util.h"

using namespace sde;
namespace icon = gallery::icon;

namespace photo {

const std::vector<gallery::BuiltinPlugin> &photo_plugins() {
  static std::vector<gallery::BuiltinPlugin> list;
  if (list.empty()) {
    list = {
        {"collage", _("Collage"),
         _("Puts any number of photos together in one picture, in ten designs (File > New Collage)."), icon::Photos},
        {"autoenhance", _("Auto Enhance"),
         _("Corrects the exposure, contrast and colours with one click (the Adjust tab)."), icon::Adjust},
        {"frames", _("Frames & Borders"),
         _("A border, a mat, a print frame, a shadow or rounded corners around the photo, in the colour you choose "
           "(the Effects tab)."),
         icon::Picture},
        {"watermark", _("Watermark"),
         _("Signs the photo with your name or a copyright line before you share it (the Effects tab)."), icon::Edit},
        {"redeye", _("Red-Eye Fix"), _("Click the red eyes of a flash photo to make them dark again (the Effects tab)."),
         icon::Info},
        {"focusblur", _("Background Blur"),
         _("Keeps the subject sharp and blurs the background, round or as a tilt-shift band (the Effects tab)."),
         icon::Filters},
        {"resizepresets", _("Sizes for the Web and Social Media"),
         _("Resizes for 4K, Full HD, the web and email, and crops for square, portrait and story posts and "
           "widescreen (Sizes… in the Crop tab)."),
         icon::Resize},
        {"metadata", _("Metadata & Privacy"),
         _("Shows every camera detail, and removes the location or all details without touching the picture (the "
           "info panel)."),
         icon::MapPin},
    };
  }
  return list;
}

bool PhotoWindow::plugin_on(const std::string &id) const {
  auto it = plugins_.find(id);
  return it == plugins_.end() || it->second;
}

void PhotoWindow::load_plugins() {
  Config cfg;
  cfg.load();
  plugins_.clear();
  for (const auto &p : photo_plugins()) plugins_[p.id] = gallery::plugin_enabled(cfg, "photo", p.id, p.default_on);
  actions_.clear();
  for (auto &a : gallery::load_action_plugins("photo"))
    if (a.valid() && a.available() && gallery::action_enabled(cfg, "photo", a.id)) actions_.push_back(a);
}

void PhotoWindow::show_plugins() {
  std::vector<gallery::PluginRow> rows;
  for (const auto &p : photo_plugins()) {
    gallery::PluginRow r;
    r.id = p.id;
    r.name = p.name;
    r.description = p.description;
    r.glyph = p.glyph;
    r.on = plugin_on(p.id);
    rows.push_back(r);
  }
  Config cfg;
  cfg.load();
  for (const auto &a : gallery::load_action_plugins("photo")) {
    gallery::PluginRow r;
    r.id = a.id;
    r.action = true;
    r.name = a.name;
    r.description = a.comment;
    r.icon = a.icon;
    r.usable = a.valid() && a.available();
    r.on = r.usable && gallery::action_enabled(cfg, "photo", a.id);
    r.detail = !a.valid() ? format(_("Cannot be used: %s"), a.problem.c_str())
               : !a.available() ? format(_("Needs a program that is not installed (%s)"),
                                         (a.try_exec.empty() ? a.exec : a.try_exec).c_str())
                                : a.file;
    rows.push_back(r);
  }
  bool changed = gallery::show_plugins_dialog(
      _("Photo Plugins"), _("Plugins add tools to Photo. Switch off the ones you do not need; they leave the menus."),
      rows, gallery::user_plugin_dir("photo"));
  if (!changed) return;
  cfg.load();
  for (const auto &r : rows) {
    if (r.action) gallery::set_action_enabled(cfg, "photo", r.id, r.on);
    else gallery::set_plugin_enabled(cfg, "photo", r.id, r.on);
  }
  cfg.save();
  load_plugins();
  build_menu();
  panel_->layout();
  panel_->sync();
  layout();
  redraw();
  toast(_("Plugins updated"));
}

void PhotoWindow::run_action(size_t index) {
  const gallery::Photo *p = current();
  if (!p || index >= actions_.size()) return;
  gallery::ActionPlugin a = actions_[index];
  if (!a.accepts({gallery::mime_type_of(p->path)})) {
    toast(format(_("“%s” does not work with this kind of image"), a.name.c_str()));
    return;
  }
  if (modified()) {
    if (gallery::ask(a.name, _("The plugin works on the saved photo.\nSave your edits first?"), {_("Cancel"), _("Save")}) ==
            0 ||
        !save(false))
      return;
    p = current();
    if (!p) return;
  }
  std::string path = p->path, name = a.name;
  auto commands = a.commands({path});
  if (commands.empty()) return;
  if (a.terminal) {
    if (!open_terminal(commands[0])) toast(_("No terminal is available"));
    return;
  }
  toast(format(_("Running “%s”…"), name.c_str()));
  bool changes = a.changes;
  for (const auto &argv : commands) {
    bool started = run_async(argv, [name, path, changes](const std::string &, int status) {
      PhotoWindow *self = PhotoWindow::instance();
      if (!self) return;
      if (status != 0) {
        self->toast(format(_("“%s” failed (exit status %d)"), name.c_str(), status));
        return;
      }
      if (changes) {
        gallery::send_gallery_events({{"changed", path}});
        const gallery::Photo *cur = self->current();
        if (cur && cur->path == path && !self->modified()) self->load_current();
      }
      self->toast(format(_("“%s” is done"), name.c_str()));
    });
    if (!started) toast(format(_("“%s” could not be started"), name.c_str()));
  }
}

}  // namespace photo
