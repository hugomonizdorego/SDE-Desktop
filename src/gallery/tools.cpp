// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Gallery's plugins and "Edit with": the built-in plugins and the Plugins
// window, action plugins, editing in Photo or in another installed image
// editor, and the folder watch that shows what other programs save.
#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>

#include "config.h"
#include "gallery.h"
#include "i18n.h"
#include "link.h"
#include "menubar.h"
#include "process.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

namespace gallery {

const std::vector<BuiltinPlugin> &gallery_plugins() {
  static std::vector<BuiltinPlugin> list;
  if (list.empty()) {
    list = {
        {"collage", _("Collage"), _("Makes a collage of the selected photos in Photo, in ten designs."), icon::Photos},
        {"slideshow", _("Slideshow Transitions"),
         _("Crossfades, slides and a slow Ken Burns zoom between photos, with the speed and order you choose."),
         icon::Play},
        {"places", _("Places"),
         _("Groups photos by where they were taken, from their GPS data and a built-in list of places; works offline."),
         icon::MapPin},
        {"duplicates", _("Find Duplicates"),
         _("Finds identical copies and very similar photos, so that you can keep one of each."), icon::Copy},
        {"rename", _("Batch Rename"), _("Renames many photos at once by date, number and name, with a preview."),
         icon::Edit},
        {"convert", _("Convert Format"),
         _("Converts photos to JPEG, PNG, WebP or TIFF, with the quality and size you choose."), icon::Resize},
        {"print", _("Print"),
         _("Prints photos one per page, several per page or as a contact sheet, or saves them as a PDF."), icon::Print},
        {"email", _("Send by Email"), _("Attaches photos to a new email, as they are or as smaller copies."),
         icon::OpenWith},
        {"devices", _("Cameras and Phones"),
         _("Imports from cameras and phones connected with a USB cable (PTP and MTP, through GIO or gphoto2)."),
         icon::Camera},
    };
  }
  return list;
}

bool GalleryWindow::plugin_on(const std::string &id) const {
  auto it = plugins_.find(id);
  return it == plugins_.end() || it->second;
}

void GalleryWindow::load_plugins() {
  Config cfg;
  cfg.load();
  plugins_.clear();
  for (const auto &p : gallery_plugins()) plugins_[p.id] = plugin_enabled(cfg, "gallery", p.id, p.default_on);
  actions_.clear();
  for (auto &a : load_action_plugins("gallery"))
    if (a.valid() && a.available() && action_enabled(cfg, "gallery", a.id)) actions_.push_back(a);
}

void GalleryWindow::show_plugins() {
  std::vector<PluginRow> rows;
  for (const auto &p : gallery_plugins()) {
    PluginRow r;
    r.id = p.id;
    r.name = p.name;
    r.description = p.description;
    r.glyph = p.glyph;
    r.on = plugin_on(p.id);
    if (p.id == "slideshow") r.settings = [this] { slideshow_settings(); };
    if (p.id == "email") r.settings = [this] { email_settings(); };
    rows.push_back(r);
  }
  Config cfg;
  cfg.load();
  for (const auto &a : load_action_plugins("gallery")) {
    PluginRow r;
    r.id = a.id;
    r.action = true;
    r.name = a.name;
    r.description = a.comment;
    r.icon = a.icon;
    r.usable = a.valid() && a.available();
    r.on = r.usable && action_enabled(cfg, "gallery", a.id);
    r.detail = !a.valid() ? format(_("Cannot be used: %s"), a.problem.c_str())
               : !a.available() ? format(_("Needs a program that is not installed (%s)"),
                                         (a.try_exec.empty() ? a.exec : a.try_exec).c_str())
                                : a.file;
    rows.push_back(r);
  }
  bool changed = show_plugins_dialog(
      _("Gallery Plugins"),
      _("Plugins add tools to Gallery. Switch off the ones you do not need; they leave the menus."), rows,
      user_plugin_dir("gallery"));
  if (!changed) return;
  cfg.load();
  for (const auto &r : rows) {
    if (r.action) set_action_enabled(cfg, "gallery", r.id, r.on);
    else set_plugin_enabled(cfg, "gallery", r.id, r.on);
  }
  cfg.save();
  load_plugins();
  rebuild_menu();
  if (view_kind_.kind == ViewKind::Places && !plugin_on("places")) set_view(View{});
  sidebar_->redraw();
  layout();
  redraw();
  toast(_("Plugins updated"));
}

void GalleryWindow::add_action_items(ContextMenu &m, const std::vector<std::string> &paths) {
  if (actions_.empty() || paths.empty()) return;
  std::vector<std::string> types;
  for (const auto &p : paths) types.push_back(mime_type_of(p));
  ContextMenu sub;
  for (size_t i = 0; i < actions_.size(); ++i)
    sub.item(actions_[i].name, [this, i, paths] { run_action(i, paths); },
             {actions_[i].icon, 0, actions_[i].accepts(types)});
  m.submenu(_("Plugins"), std::move(sub), "settings");
}

void GalleryWindow::run_action(size_t index, const std::vector<std::string> &paths) {
  if (index >= actions_.size() || paths.empty()) return;
  ActionPlugin a = actions_[index];
  std::vector<std::string> types;
  for (const auto &p : paths) types.push_back(mime_type_of(p));
  if (!a.accepts(types)) {
    toast(format(_("“%s” does not work with these files"), a.name.c_str()));
    return;
  }
  auto commands = a.commands(paths);
  if (commands.empty()) return;
  if (a.terminal) {
    for (const auto &argv : commands)
      if (!open_terminal(argv)) {
        toast(_("No terminal is available"));
        break;
      }
    return;
  }
  std::string name = a.name;
  toast(format(_("Running “%s”…"), name.c_str()));
  bool changes = a.changes;
  // The programs run one after the other; the photos are shown again
  // when the last one is done.
  struct Run {
    std::vector<std::vector<std::string>> commands;
    size_t next = 0, failed = 0;
  };
  auto run = std::make_shared<Run>();
  run->commands = commands;
  auto step = std::make_shared<std::function<void()>>();
  *step = [this, run, step, name, paths, changes] {
    if (run->next >= run->commands.size()) {
      if (instance_ != this) return;
      if (changes) files_changed(paths);
      if (run->failed) toast(format(_("“%s” failed for %zu of %zu photos"), name.c_str(), run->failed,
                                    run->commands.size()));
      else toast(format(_("“%s” is done"), name.c_str()));
      return;
    }
    const auto &argv = run->commands[run->next++];
    bool ok = run_async(argv, [run, step](const std::string &, int status) {
      if (status != 0) ++run->failed;
      (*step)();
    });
    if (!ok) {
      ++run->failed;
      (*step)();
    }
  };
  (*step)();
}

void GalleryWindow::create_collage(const std::vector<std::string> &paths) {
  if (find_program("sde-view").empty()) {
    toast(_("Photo (sde-view) is not installed"));
    return;
  }
  std::vector<std::string> argv{"sde-view", "--collage", "--"};
  argv.insert(argv.end(), paths.begin(), paths.end());
  if (!spawn_detached(argv)) toast(_("Photo (sde-view) could not be started"));
  else toast(paths.size() > 1 ? format(_("A collage of %zu photos opens in Photo"), paths.size())
                              : std::string(_("The collage opens in Photo")));
}

// --------------------------------------------------------------- edit with

const std::vector<DesktopEntry> &GalleryWindow::applications() {
  std::string stamp = applications_stamp();
  if (stamp != apps_stamp_ || apps_.empty()) {
    apps_ = load_applications();
    apps_stamp_ = stamp;
  }
  return apps_;
}

std::string GalleryWindow::default_editor_name() const {
  if (editor_.empty()) return _("Photo");
  for (const auto &e : apps_)
    if (e.id == editor_) return e.name;
  return _("Photo");
}

void GalleryWindow::set_default_editor(const std::string &app_id) {
  editor_ = app_id == "sde-view.desktop" ? "" : app_id;
  save_settings();
  rebuild_menu();
  toast(format(_("Ctrl+E edits photos in %s"), default_editor_name().c_str()));
}

void GalleryWindow::edit_with_default(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  applications();
  bool installed = false;
  for (const auto &e : apps_) installed |= e.id == editor_;
  edit_with(paths, editor_.empty() || !installed ? "" : editor_);
}

void GalleryWindow::edit_with(const std::vector<std::string> &paths, const std::string &app_id) {
  if (paths.empty()) return;
  if (app_id.empty() || app_id == "sde-view.desktop") {
    if (paths.size() == 1) {
      edit_in_photo(paths[0]);
      return;
    }
    // Photo steps through exactly these photos.
    std::string file = runtime_dir() + "/sde-gallery/edit-" + std::to_string(getpid()) + ".list";
    if (!write_album_list(file, paths) || !open_in_photo(paths[0], Album{AlbumKind::List, file}, sort_))
      toast(_("Photo (sde-view) is not installed"));
    return;
  }
  const DesktopEntry *app = nullptr;
  for (const auto &e : applications())
    if (e.id == app_id) app = &e;
  if (!app) {
    toast(_("That application is no longer installed"));
    return;
  }
  // Programs with %f take one photo: start one for each.
  bool one = app->exec.find("%f") != std::string::npos || app->exec.find("%u") != std::string::npos;
  bool ok = true;
  if (one) {
    for (const auto &p : paths) ok = launch_entry(*app, {p}) && ok;
  } else {
    ok = launch_entry(*app, paths);
  }
  if (!ok) {
    toast(format(_("%s could not be started"), app->name.c_str()));
    return;
  }
  // What it saves shows up here.
  for (const auto &p : paths) watch_folder(p.substr(0, p.rfind('/')));
  toast(paths.size() == 1 ? format(_("Editing “%s” in %s"), basename_of(paths[0]).c_str(), app->name.c_str())
                          : format(_("Editing %zu photos in %s"), paths.size(), app->name.c_str()));
}

ContextMenu GalleryWindow::edit_with_menu(const std::vector<std::string> &paths) {
  ContextMenu m;
  if (paths.empty()) return m;
  std::string mime = mime_type_of(paths[0]);
  auto editors = image_editors(applications(), mime);
  bool photo = !find_program("sde-view").empty();
  std::string def = editor_;
  bool def_installed = def.empty();
  for (const auto &e : editors) def_installed |= e.id == def;
  if (!def_installed) def.clear();
  if (photo)
    m.item(_("Photo"), [this, paths] { edit_with(paths, ""); },
           {"sde-photo", def.empty() ? FL_CTRL + 'e' : 0, true, editors.empty()});
  for (size_t i = 0; i < editors.size(); ++i) {
    std::string id = editors[i].id;
    std::string icon = editors[i].icon.empty() ? "applications-graphics" : editors[i].icon;
    m.item(editors[i].name, [this, paths, id] { edit_with(paths, id); },
           {icon, def == id ? FL_CTRL + 'e' : 0, true, i + 1 == editors.size()});
  }
  m.item(_("Other Application…"), [this, paths] { edit_with_other(paths); }, {"open", 0, true, true});
  // The default editor: Photo or one of the editors.
  ContextMenu defaults;
  if (photo) defaults.radio(_("Photo"), def.empty(), [this] { set_default_editor(""); });
  for (const auto &e : editors) {
    std::string id = e.id;
    defaults.radio(e.name, def == id, [this, id] { set_default_editor(id); });
  }
  m.submenu(_("Default Editor"), std::move(defaults), "settings");
  return m;
}

void GalleryWindow::edit_with_popup(const std::vector<std::string> &paths, int x, int y) {
  if (paths.empty()) {
    toast(_("Select a photo first"));
    return;
  }
  ContextMenu m = edit_with_menu(paths);
  if (x >= 0) m.popup(x, y);
  else m.popup();
  redraw();
}

void GalleryWindow::edit_with_other(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  std::string mime = mime_type_of(paths[0]);
  auto apps = applications_for(applications(), mime);
  if (apps.empty()) {
    toast(_("No other application opens this kind of image"));
    return;
  }
  bool make_default = false;
  int i = choose_application(_("Edit With"),
                             paths.size() == 1 ? format(_("Choose an application to edit “%s”."),
                                                        basename_of(paths[0]).c_str())
                                               : format(_("Choose an application to edit %zu photos."), paths.size()),
                             apps, &make_default);
  if (i < 0) return;
  std::string id = apps[size_t(i)].id;
  edit_with(paths, id);
  if (make_default) set_default_editor(id);
}

// ---------------------------------------------------------- folder watch

void GalleryWindow::watch_folder(const std::string &dir) {
  if (dir.empty()) return;
  if (!watch_) {
    watch_.reset(new FolderWatch());
    if (watch_->fd() < 0) return;
    auto extra = extra_formats_;
    watch_->set_filter([extra](const std::string &name) { return is_image_name(name, extra); });
    Fl::add_fd(watch_->fd(), FL_READ, watch_cb, this);
  }
  watch_->watch(dir);
}

void GalleryWindow::watch_cb(int, void *p) {
  auto *self = static_cast<GalleryWindow *>(p);
  if (instance_ != self || !self->watch_) return;
  for (const auto &c : self->watch_->take()) self->watch_pending_.push_back({c.second ? "removed" : "changed", c.first});
  // Programs often write a file in several steps: wait a moment.
  Fl::remove_timeout(watch_timer_cb, self);
  Fl::add_timeout(0.4, watch_timer_cb, self);
}

void GalleryWindow::watch_timer_cb(void *p) {
  auto *self = static_cast<GalleryWindow *>(p);
  if (instance_ != self) return;
  std::vector<GalleryEvent> events;
  events.swap(self->watch_pending_);
  self->file_events(events);
}

}  // namespace gallery
