// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's menus, in the SDE UI kit's ContextMenu (a row of quick actions,
// then grouped items): the menu of the selected items, of the empty part of
// a folder, of a place (sidebar, drives, network), and the command bar's
// New, Share, Sort, View and More. SDE's own items appear only when they
// apply: Install with Debrun for Debian packages, Run for AppImages
// (sde-appimage verifies them first), Extract and Compress with Taca, Edit
// with for pictures, Send to for removable drives, Set as wallpaper.
#include <FL/Fl.H>
#include <FL/platform.H>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "config.h"
#include "file_chooser_model.h"
#include "i18n.h"
#include "integration.h"
#include "location.h"
#include "menu.h"
#include "openwith.h"
#include "spawn.h"
#include "trash.h"
#include "ui.h"
#include "util.h"
#include "x11.h"

namespace sde {
namespace liria {

namespace {

bool is_archive(const std::string &m) {
  static const std::set<std::string> a = {
      "application/zip", "application/x-tar", "application/x-compressed-tar", "application/x-bzip-compressed-tar",
      "application/x-xz-compressed-tar", "application/x-7z-compressed", "application/vnd.rar", "application/x-rar",
      "application/gzip", "application/x-zstd-compressed-tar", "application/x-lzma-compressed-tar",
      "application/x-bzip2-compressed-tar", "application/x-zip-compressed", "application/x-cpio"};
  return a.count(m) != 0;
}

bool is_deb(const std::string &m) {
  return m == "application/vnd.debian.binary-package" || m == "application/x-deb";
}

bool is_appimage(const std::string &m) {
  return m == "application/vnd.appimage" || m == "application/x-iso9660-appimage";
}

void set_wallpaper(MainWindow *w, const std::string &path) {
  Config cfg;
  cfg.load();
  cfg.set("appearance", "wallpaper", path);
  if (!cfg.save()) {
    w->say(_("The wallpaper could not be saved."), true);
    return;
  }
  // Like Gallery and Photo: when the look takes its colours from the
  // wallpaper, the session switches every program to it in one calm step
  // (0.2.0 Alpha 4, integration.h).
  announce_wallpaper_change(fl_display, cfg);
  w->say(_("The picture is the wallpaper now."));
}

// Removable drives that are mounted and writable.
std::vector<Drive> send_targets() {
  std::vector<Drive> out;
  for (const auto &d : App::get().drives.drives())
    if (d.removable && d.mounted && !d.path.empty() && access(d.path.c_str(), W_OK) == 0) out.push_back(d);
  return out;
}

ContextMenu open_with_menu(MainWindow *w, const Item &e) {
  ContextMenu m;
  App &app = App::get();
  auto apps = app.apps.for_type(e.mime);
  std::string def = app.apps.default_for(e.mime);
  int n = 0;
  std::string path = e.path;
  for (const MimeApp *a : apps) {
    if (++n > 12) break;
    std::string id = a->id;
    std::string label = a->id == def ? format(_("%s (default)"), a->name.c_str()) : a->name;
    m.item(label, [w, id, path] {
      if (!App::get().apps.launch(id, {path})) w->say(_("The application could not be started."), true);
    }, {a->icon.empty() ? std::string("application-x-executable") : a->icon});
  }
  if (!apps.empty()) m.separator();
  Item copy = e;
  m.item(_("Choose another app…"), [w, copy] { show_open_with(w, copy); }, {"open"});
  return m;
}

}  // namespace

void MainWindow::show_menu(ContextMenu &m, int x, int y, Fl_Widget *anchor) {
  if (getenv("SDE_PANEL_QA_DIR")) {
    // "Open|Open with ▸|-Paste|..." ('-': disabled), quick actions first.
    menu_qa_.clear();
    for (const Fl_Menu_Item *it = m.items(); it && it->label(); it = it->next()) {
      std::string label = it->label();
      label.erase(std::remove(label.begin(), label.end(), '&'), label.end());
      if (it->submenu()) label += " >";
      menu_qa_ += (menu_qa_.empty() ? "" : "|") + std::string(it->active() ? "" : "-") + label;
    }
    if (menu_qa_.empty()) menu_qa_ = "(empty)";
    schedule_qa();
  }
  if (anchor) m.popup_below(anchor);
  else m.popup(x, y);
  if (!menu_qa_.empty()) {
    menu_qa_.clear();
    schedule_qa();
  }
}

void MainWindow::send_to(const std::string &dir) {
  std::vector<std::string> paths = selected_paths();
  if (!paths.empty()) App::get().transfer(paths, dir, Transfer::Copy, this, false);
}

void MainWindow::item_menu(int mx, int my) {
  Tab &t = tab();
  std::vector<Item> sel = selected_items();
  if (sel.empty()) return;
  App &app = App::get();
  bool trash = t.location == kTrash;
  bool single = sel.size() == 1;
  const Item &first = sel[0];
  bool all_files = std::all_of(sel.begin(), sel.end(), [](const Item &i) { return !i.dir; });
  bool all_dirs = std::all_of(sel.begin(), sel.end(), [](const Item &i) { return i.dir; });
  bool local = std::all_of(sel.begin(), sel.end(), [](const Item &i) { return is_local(i.path); });
  ContextMenu m;
  if (trash) {
    m.item(_("Restore"), [this] { restore_selection(); }, {"undo"});
    m.item(_("Delete permanently"), [this] { remove(true); }, {"delete", FL_Delete});
    m.separator();
    m.item(_("Properties"), [this] { properties(); }, {"properties", FL_ALT + FL_Enter});
    show_menu(m, mx, my);
    return;
  }
  EditActions edit;
  edit.cut = [this] { cut(); };
  edit.copy = [this] { copy(); };
  edit.del = [this] { remove(false); };
  if (all_dirs && single && can_paste()) {
    std::string dir = first.path;
    edit.paste = [this, dir] { paste_into(dir); };
  }
  m.edit_actions(edit);
  if (single) m.quick(_("Rename"), "rename", [this] { rename(); });
  m.quick(_("Copy path"), "copy", [this] { copy_path(); });
  m.item(_("Open"), [this] { open_selection(false); }, {"open", FL_Enter});
  if (all_dirs) {
    m.item(_("Open in new tab"), [this] { open_selection(true); }, {"tab-new"});
    if (single && local)
      m.item(_("Open in new window"), [first] { App::get().open_window({first.path}); }, {"window-new"});
  }
  if (single && !first.dir) m.submenu(_("Open with"), open_with_menu(this, first), "open");
  if (all_files && std::all_of(sel.begin(), sel.end(), [](const Item &i) { return starts_with(i.mime, "image/"); })) {
    // Edit with: Photo first, then the other image editors.
    ContextMenu edit_with;
    std::vector<std::string> paths = selected_paths();
    if (!find_program("sde-view").empty())
      edit_with.item(_("Photo"), [paths] {
        std::vector<std::string> argv{"sde-view"};
        argv.insert(argv.end(), paths.begin(), paths.end());
        spawn_detached(argv);
      }, {"sde-photo"});
    for (const MimeApp *a : app.apps.for_type(first.mime)) {
      bool editor = std::find(a->categories.begin(), a->categories.end(), "RasterGraphics") != a->categories.end() ||
                    std::find(a->categories.begin(), a->categories.end(), "2DGraphics") != a->categories.end();
      if (!editor || a->id == "sde-view.desktop") continue;
      std::string id = a->id;
      edit_with.item(a->name, [id, paths] { App::get().apps.launch(id, paths); }, {a->icon});
    }
    if (!edit_with.empty()) m.submenu(_("Edit with"), std::move(edit_with), "image");
  }
  m.separator();
  // SDE's tools, where they apply.
  if (single && is_deb(first.mime))
    m.item(_("Install with Debrun"), [this] { install_package(); }, {"debrun"});
  if (single && is_appimage(first.mime)) {
    m.item(_("Run (check the AppImage first)"), [this] { run_appimage(); }, {"sde-appimage"});
  }
  if (local && all_files && std::all_of(sel.begin(), sel.end(), [](const Item &i) { return is_archive(i.mime); }))
    m.item(_("Extract all"), [this] { extract_here(); }, {"package-x-generic"});
  if (local) m.item(_("Compress…"), [this] { compress(); }, {"package-x-generic"});
  if (single && !first.dir && starts_with(first.mime, "image/") && local)
    m.item(_("Set as wallpaper"), [this, first] { set_wallpaper(this, first.path); }, {"preferences-desktop-wallpaper"});
  {
    bool all_pinned = std::all_of(sel.begin(), sel.end(), [&](const Item &i) { return app.pins.contains(i.path); });
    std::vector<std::string> paths = selected_paths();
    if (all_pinned)
      m.item(_("Unpin from Quick access"), [paths] {
        for (const auto &p : paths) App::get().pins.remove(p);
        App::get().places_changed();
      }, {"pin"});
    else
      m.item(_("Pin to Quick access"), [paths] {
        for (const auto &p : paths) App::get().pins.add(p);
        App::get().places_changed();
      }, {"pin"});
  }
  std::vector<Drive> drives = send_targets();
  if (local && !drives.empty()) {
    ContextMenu send;
    for (const auto &d : drives) {
      std::string dir = d.path;
      send.item(d.name, [this, dir] { send_to(dir); }, {d.icons.empty() ? "drive-removable-media" : d.icons[0]});
    }
    m.submenu(_("Send to"), std::move(send), "drive-removable-media");
  }
  if (single && first.dir && local) {
    m.item(_("Open in Terminal"), [this] { open_terminal_here(); }, {"utilities-terminal"});
    m.item(_("Open as administrator"), [this, first] { open_as_administrator(first.path); }, {"dialog-password"});
  }
  m.separator();
  m.item(_("Properties"), [this] { properties(); }, {"properties", FL_ALT + FL_Enter});
  show_menu(m, mx, my);
}

void MainWindow::background_menu(int mx, int my) {
  Tab &t = tab();
  App &app = App::get();
  ContextMenu m;
  if (t.location == kHome) {
    m.item(_("Refresh"), [this] { refresh(); }, {"refresh", FL_F + 5});
    m.item(_("Connect to server…"), [this] { connect_to_server(); }, {"network-server"});
    m.separator();
    m.check(_("Show Network locations"), app.prefs.network_shown, [] {
      App::get().prefs.network_shown = !App::get().prefs.network_shown;
      App::get().prefs.save();
      App::get().places_changed();
    });
    m.check(_("Show Recent files"), app.prefs.recent_shown, [] {
      App::get().prefs.recent_shown = !App::get().prefs.recent_shown;
      App::get().prefs.save();
      App::get().places_changed();
    });
    show_menu(m, mx, my);
    return;
  }
  bool folder = t.is_folder(), writable = can_write_here();
  if (folder && writable && can_paste()) {
    EditActions edit;
    edit.paste = [this] { paste(); };
    edit.select_all = [this] { select_all(); };
    m.edit_actions(edit);
  }
  ContextMenu view;
  struct {
    const char *label;
    int size;
  } sizes[] = {{N_("Extra large icons"), 256}, {N_("Large icons"), 128}, {N_("Medium icons"), 72}, {N_("Small icons"), 48}};
  for (const auto &s : sizes) {
    int px = s.size;
    view.radio(_(s.label), t.layout == Layout::Icons && t.icon_size == px, [this, px] { set_icon_size(px); });
  }
  view.radio(_("Compact"), t.layout == Layout::Compact, [this] { set_layout(Layout::Compact); });
  view.radio(_("Details"), t.layout == Layout::Details, [this] { set_layout(Layout::Details); });
  m.submenu(_("View"), std::move(view), "view");
  ContextMenu sort;
  struct {
    const char *label;
    chooser::SortKey key;
  } keys[] = {{N_("Name"), chooser::SortKey::Name},
              {N_("Date modified"), chooser::SortKey::Modified},
              {N_("Type"), chooser::SortKey::Type},
              {N_("Size"), chooser::SortKey::Size}};
  for (const auto &k : keys) {
    chooser::SortKey key = k.key;
    sort.radio(_(k.label), t.model.order.key == key, [this, key] { sort_by(key); });
  }
  sort.separator();
  sort.radio(_("Ascending"), !t.model.order.descending, [this] { set_sort_descending(false); });
  sort.radio(_("Descending"), t.model.order.descending, [this] { set_sort_descending(true); });
  m.submenu(_("Sort by"), std::move(sort), "sort");
  m.item(_("Refresh"), [this] { refresh(); }, {"refresh", FL_F + 5});
  m.separator();
  if (t.location == kTrash) {
    m.item(_("Empty Trash"), [this] { empty_trash(); }, {"user-trash"});
  } else if (folder) {
    if (writable) {
      m.item(_("Paste"), [this] { paste(); }, {"paste", FL_CTRL + 'v', can_paste()});
      if (!app.undo.empty()) m.item(UndoStack::label(app.undo.top()), [this] { undo(); }, {"undo", FL_CTRL + 'z'});
      ContextMenu nw;
      nw.item(_("Folder"), [this] { new_folder(); }, {"folder-new", FL_CTRL + FL_SHIFT + 'n'});
      nw.item(_("Text document"), [this] { new_document(""); }, {"text-x-generic"});
      for (const auto &tp : templates()) {
        std::string path = tp;
        chooser::Entry e;
        std::vector<std::string> icons = chooser::entry_for_path(tp, e) ? chooser::mime_icon_names(e.mime)
                                                                         : std::vector<std::string>{"text-x-generic"};
        std::string label = basename_of(tp);
        size_t dot = label.rfind('.');
        if (dot != std::string::npos && dot > 0) label = label.substr(0, dot);
        nw.item(label, [this, path] { new_document(path); }, {icons.empty() ? "" : icons[0]});
      }
      m.submenu(_("New"), std::move(nw), "new");
      m.separator();
    }
    if (is_local(t.location)) {
      m.item(_("Open in Terminal"), [this] { open_terminal_here(); }, {"utilities-terminal"});
      std::string loc = t.location;
      if (app.pins.contains(loc))
        m.item(_("Unpin from Quick access"), [loc] {
          App::get().pins.remove(loc);
          App::get().places_changed();
        }, {"pin"});
      else
        m.item(_("Pin to Quick access"), [loc] {
          App::get().pins.add(loc);
          App::get().places_changed();
        }, {"pin"});
    }
  }
  m.check(_("Show hidden items"), app.prefs.show_hidden, [this] { toggle_hidden(); });
  m.separator();
  m.item(_("Properties"), [this] { properties(); }, {"properties", FL_ALT + FL_Enter});
  show_menu(m, mx, my);
}

void MainWindow::place_menu(const std::string &location, int mx, int my, const std::string &drive_id) {
  App &app = App::get();
  ContextMenu m;
  std::string loc = location;
  const Drive *drive = nullptr;
  std::vector<Drive> all = app.drives.drives(), net = app.drives.network();
  for (const auto *list : {&all, &net})
    for (const auto &d : *list)
      if (!drive_id.empty() && d.id == drive_id) drive = &d;
  if (!loc.empty()) {
    m.item(_("Open"), [this, loc] {
      if (is_remote(loc)) open_network_location(loc);
      else go(loc);
    }, {"open"});
    if (loc != kHome) {
      m.item(_("Open in new tab"), [this, loc] { new_tab(loc, false); }, {"tab-new"});
      m.item(_("Open in new window"), [loc] { App::get().open_window({loc}); }, {"window-new"});
    }
  }
  if (loc == kTrash) {
    m.separator();
    m.item(_("Empty Trash"), [this] { empty_trash(); }, {"user-trash", 0, !trash_is_empty()});
  }
  if (!loc.empty() && loc != kHome && loc != kNetwork) {
    m.separator();
    if (app.pins.contains(loc))
      m.item(_("Unpin from Quick access"), [loc] {
        App::get().pins.remove(loc);
        App::get().places_changed();
      }, {"pin"});
    else
      m.item(_("Pin to Quick access"), [loc] {
        App::get().pins.add(loc);
        App::get().places_changed();
      }, {"pin"});
  }
  if (drive) {
    std::string id = drive->id;
    m.separator();
    if (!drive->mounted && drive->can_mount)
      m.item(_("Mount"), [this, id] {
        App::get().drives.mount(id, mount_questions(this, ""), [this](bool ok, const std::string &error, const std::string &path) {
          if (ok && !path.empty()) go(path);
          else if (!error.empty()) say(error, true);
        });
      }, {"drive-harddisk"});
    if (drive->mounted && drive->can_unmount && !drive->system)
      m.item(_("Unmount"), [this, id] {
        App::get().drives.unmount(id, [this](bool ok, const std::string &error, const std::string &) {
          if (!ok && !error.empty()) say(error, true);
        });
      }, {"media-eject"});
    if (drive->can_eject)
      m.item(_("Eject"), [this, id] {
        App::get().drives.eject(id, [this](bool ok, const std::string &error, const std::string &) {
          if (!ok && !error.empty()) say(error, true);
          else if (ok) say(_("The drive can be removed now."));
        });
      }, {"media-eject"});
  }
  if (is_local(loc)) {
    m.separator();
    m.item(_("Open in Terminal"), [loc] { sde::open_terminal({}, loc); }, {"utilities-terminal"});
    m.item(_("Copy path"), [loc] {
      Fl::copy(loc.c_str(), int(loc.size()), 1);
      Fl::copy(loc.c_str(), int(loc.size()), 0);
    }, {"copy"});
    m.item(_("Properties"), [this, loc] {
      Item it;
      if (chooser::entry_for_path(loc, it)) show_properties(this, {it});
    }, {"properties"});
  }
  if (!m.empty()) show_menu(m, mx, my);
}

void MainWindow::new_menu(Fl_Widget *anchor) {
  ContextMenu m;
  m.item(_("Folder"), [this] { new_folder(); }, {"folder-new", FL_CTRL + FL_SHIFT + 'n'});
  m.item(_("Text document"), [this] { new_document(""); }, {"text-x-generic"});
  std::vector<std::string> tps = templates();
  if (!tps.empty()) m.separator();
  for (const auto &tp : tps) {
    std::string path = tp;
    chooser::Entry e;
    std::vector<std::string> icons =
        chooser::entry_for_path(tp, e) ? chooser::mime_icon_names(e.mime) : std::vector<std::string>{"text-x-generic"};
    std::string label = basename_of(tp);
    size_t dot = label.rfind('.');
    if (dot != std::string::npos && dot > 0) label = label.substr(0, dot);
    m.item(label, [this, path] { new_document(path); }, {icons.empty() ? "" : icons[0]});
  }
  const CommandBar::Button *b = commandbar_->find("new");
  if (b) show_menu(m, b->x, commandbar_->y() + commandbar_->h() - S(2));
  else show_menu(m, 0, 0, anchor);
}

void MainWindow::share_menu(Fl_Widget *, int x, int y) {
  ContextMenu m;
  m.item(_("Copy path"), [this] { copy_path(); }, {"copy", FL_CTRL + FL_SHIFT + 'c'});
  m.item(_("Send by email"), [this] { email(); }, {"mail-send"});
  std::vector<Drive> drives = send_targets();
  if (!drives.empty()) {
    ContextMenu send;
    for (const auto &d : drives) {
      std::string dir = d.path;
      send.item(d.name, [this, dir] { send_to(dir); }, {d.icons.empty() ? "drive-removable-media" : d.icons[0]});
    }
    m.submenu(_("Send to"), std::move(send), "drive-removable-media");
  }
  show_menu(m, x, y);
}

void MainWindow::sort_menu(int x, int y) {
  Tab &t = tab();
  ContextMenu m;
  struct {
    const char *label;
    chooser::SortKey key;
  } keys[] = {{N_("Name"), chooser::SortKey::Name},
              {N_("Date modified"), chooser::SortKey::Modified},
              {N_("Type"), chooser::SortKey::Type},
              {N_("Size"), chooser::SortKey::Size}};
  for (const auto &k : keys) {
    chooser::SortKey key = k.key;
    m.radio(_(k.label), t.model.order.key == key, [this, key] { sort_by(key); });
  }
  m.separator();
  m.radio(_("Ascending"), !t.model.order.descending, [this] { set_sort_descending(false); });
  m.radio(_("Descending"), t.model.order.descending, [this] { set_sort_descending(true); });
  m.separator();
  m.check(_("Folders first"), t.model.order.folders_first, [this] {
    Tab &tt = tab();
    tt.model.order.folders_first = !tt.model.order.folders_first;
    App::get().prefs.sort.folders_first = tt.model.order.folders_first;
    App::get().prefs.save();
    tt.model.refresh();
    fileview_->sync();
  });
  show_menu(m, x, y);
}

void MainWindow::view_menu(int x, int y) {
  Tab &t = tab();
  App &app = App::get();
  ContextMenu m;
  struct {
    const char *label;
    int size;
  } sizes[] = {{N_("Extra large icons"), 256}, {N_("Large icons"), 128}, {N_("Medium icons"), 72}, {N_("Small icons"), 48}};
  for (const auto &s : sizes) {
    int px = s.size;
    m.radio(_(s.label), t.layout == Layout::Icons && t.icon_size == px, [this, px] { set_icon_size(px); });
  }
  m.radio(_("Compact"), t.layout == Layout::Compact, [this] { set_layout(Layout::Compact); });
  m.radio(_("Details"), t.layout == Layout::Details, [this] { set_layout(Layout::Details); });
  m.separator();
  ContextMenu show;
  show.check(_("Navigation pane"), app.prefs.sidebar_shown, [this] { toggle_sidebar(); });
  show.check(_("Hidden items"), app.prefs.show_hidden, [this] { toggle_hidden(); });
  show.check(_("Network locations on Home"), app.prefs.network_shown, [] {
    App::get().prefs.network_shown = !App::get().prefs.network_shown;
    App::get().prefs.save();
    App::get().places_changed();
  });
  show.check(_("Recent files on Home"), app.prefs.recent_shown, [] {
    App::get().prefs.recent_shown = !App::get().prefs.recent_shown;
    App::get().prefs.save();
    App::get().places_changed();
  });
  m.submenu(_("Show"), std::move(show), "hidden");
  show_menu(m, x, y);
}

void MainWindow::more_menu(int x, int y) {
  Tab &t = tab();
  App &app = App::get();
  ContextMenu m;
  if (!app.undo.empty()) m.item(UndoStack::label(app.undo.top()), [this] { undo(); }, {"undo", FL_CTRL + 'z'});
  bool files = t.is_folder() || t.location == kTrash || t.location == kNetwork;
  m.item(_("Select all"), [this] { select_all(); }, {"select-all", FL_CTRL + 'a', files});
  m.item(_("Select none"), [this] { select_none(); }, {"", 0, files && !t.model.selected().empty()});
  m.item(_("Invert selection"), [this] { invert_selection(); }, {"", FL_CTRL + 'i', files});
  m.separator();
  if (is_local(t.location)) {
    m.item(_("Open in Terminal"), [this] { open_terminal_here(); }, {"utilities-terminal"});
    std::string loc = t.location;
    m.item(_("Open as administrator"), [this, loc] { open_as_administrator(loc); }, {"dialog-password"});
  }
  m.item(_("Connect to server…"), [this] { connect_to_server(); }, {"network-server"});
  if (t.location != kTrash) m.item(_("Empty Trash"), [this] { empty_trash(); }, {"user-trash", 0, !trash_is_empty()});
  m.separator();
  m.check(_("Ask before moving items to the Trash"), app.prefs.confirm_trash, [] {
    App::get().prefs.confirm_trash = !App::get().prefs.confirm_trash;
    App::get().prefs.save();
  });
  m.item(_("Properties"), [this] { properties(); }, {"properties", FL_ALT + FL_Enter});
  m.item(_("About Liria"), [this] { about(); }, {"help-about"});
  show_menu(m, x, y);
}

}  // namespace liria
}  // namespace sde
