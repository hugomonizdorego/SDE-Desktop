// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Gallery main window: library state, sources, header and actions.
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "build_config.h"
#include "config.h"
#include "exif.h"
#include "gallery.h"
#include "link.h"
#include "i18n.h"
#include "process.h"
#include "spawn.h"
#include "util.h"
#include "x11.h"

using namespace sde;

namespace gallery {

GalleryWindow *GalleryWindow::instance_ = nullptr;

namespace {

std::string thumb_key(const std::string &path, bool large) {
  return path + (large ? "\n1" : "\n0");
}

enum MenuId {
  M_OPEN_FILE = 1, M_OPEN_FOLDER, M_IMPORT, M_ADD_FOLDER, M_REFRESH, M_QUIT,
  M_SELECT_ALL, M_SELECT_NONE, M_FAVORITE, M_TRASH, M_COPY_PATH, M_OPEN_PHOTO, M_WALLPAPER, M_SHOW_FILES,
  M_SORT_NEWEST, M_SORT_OLDEST, M_SORT_NAME, M_ZOOM_IN, M_ZOOM_OUT, M_SIDEBAR, M_PREFER_PHOTO,
  M_FULLSCREEN, M_SHORTCUTS, M_ABOUT
};

// The Gallery icon (see sde-gallery.svg), for the About window.
void draw_app_icon(Painter &p, double x, double y, double s) {
  cairo_t *cr = p.cr();
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + s, y + s);
  cairo_pattern_add_color_stop_rgb(g, 0, 0x2B / 255.0, 0xB4 / 255.0, 0xD3 / 255.0);
  cairo_pattern_add_color_stop_rgb(g, 1, 0x0E / 255.0, 0x56 / 255.0, 0x6C / 255.0);
  p.rounded_rect(x, y, s, s, s * 0.26);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  icon::draw(p, icon::Picture, x + s * 0.18, y + s * 0.18, s * 0.64, Color::hex(0xFFFFFF));
}

}  // namespace

Album View::album() const {
  switch (kind) {
    case ViewKind::All: return {AlbumKind::All, ""};
    case ViewKind::Favorites: return {AlbumKind::Favorites, ""};
    case ViewKind::Recent: return {AlbumKind::Recent, ""};
    case ViewKind::Screenshots: return {AlbumKind::Screenshots, ""};
    case ViewKind::Folder: return {AlbumKind::Folder, folder};
    case ViewKind::Device: return {AlbumKind::Device, folder};
    case ViewKind::Browse: return {AlbumKind::Folder, folder};
  }
  return {};
}

GalleryWindow::GalleryWindow(int w, int h) : BufferedWindow(w, h, _("Gallery")) {
  instance_ = this;
  theme_ = active_theme();
  pictures_ = pictures_dir();
  extra_formats_ = extra_formats();
  reload_config();
  service_.reset(new ThumbnailService([this] { Fl::awake(awake_cb, this); },
                                      int(std::max(2u, std::thread::hardware_concurrency() / 2))));

  begin();
  menubar_ = new MenuBar(0, 0, w, S(30));
  build_menu();
  sidebar_ = new Sidebar(this, 0, 0, S(232), h);
  search_ = new Fl_Input(0, 0, S(200), S(30));
  search_->when(FL_WHEN_CHANGED);
  search_->callback([](Fl_Widget *, void *p) {
    static_cast<GalleryWindow *>(p)->rebuild_view();
  }, this);
  sort_ctl_ = new Segmented(0, 0, S(222), S(32), {_("Newest"), _("Oldest"), _("Name")});
  sort_ctl_->value(int(sort_));
  sort_ctl_->callback([](Fl_Widget *w, void *p) {
    auto *self = static_cast<GalleryWindow *>(p);
    self->sort_ = Sort(static_cast<Segmented *>(w)->value());
    self->save_settings();
    self->rebuild_view();
  }, this);
  zoom_ctl_ = new Slider(0, 0, S(120), S(28));
  zoom_ctl_->bounds(96, 320);
  zoom_ctl_->step(4);
  zoom_ctl_->value(zoom_);
  zoom_ctl_->tooltip(_("Thumbnail size"));
  zoom_ctl_->callback([](Fl_Widget *w, void *p) {
    static_cast<GalleryWindow *>(p)->set_zoom(int(static_cast<Slider *>(w)->value()));
  }, this);
  sel_fav_ = new FlatButton(0, 0, S(100), S(32), _("Favorite"));
  sel_fav_->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<GalleryWindow *>(p);
    std::vector<std::string> paths;
    for (size_t i : self->grid_->selection()) paths.push_back(self->view_[i].path);
    self->toggle_favorite(paths);
  }, this);
  sel_trash_ = new FlatButton(0, 0, S(130), S(32), _("Move to Trash"));
  sel_trash_->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<GalleryWindow *>(p);
    std::vector<std::string> paths;
    for (size_t i : self->grid_->selection()) paths.push_back(self->view_[i].path);
    self->trash(paths);
  }, this);
  import_btn_ = new FlatButton(0, 0, S(120), S(32), _("Import…"), true);
  import_btn_->callback([](Fl_Widget *, void *p) { static_cast<GalleryWindow *>(p)->import_dialog(); }, this);
  import_btn_->hide();
  sel_clear_ = new FlatButton(0, 0, S(84), S(32), _("Done"), true);
  sel_clear_->callback([](Fl_Widget *, void *p) {
    static_cast<GalleryWindow *>(p)->grid_->clear_selection();
  }, this);
  grid_ = new Grid(this, S(232), S(64), w - S(232), h - S(64));
  viewer_ = new Viewer(this, 0, 0, w, h);
  viewer_->hide();
  end();
  resizable(nullptr);
  size_range(S(560), S(400));
  xclass("SDE-Gallery");
  apply_theme();
  layout();
  start_scan();
  refresh_devices();
  // The kernel flags /proc/self/mountinfo as exceptional when mounts change.
  mounts_fd_ = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
  if (mounts_fd_ >= 0) Fl::add_fd(mounts_fd_, FL_EXCEPT, mounts_cb, this);
  Fl::add_timeout(5.0, mounts_timer_cb, this);
  Fl::add_handler(x_event_handler);
}

GalleryWindow::~GalleryWindow() {
  Fl::remove_handler(x_event_handler);
  Fl::remove_timeout(mounts_timer_cb, this);
  if (mounts_fd_ >= 0) {
    Fl::remove_fd(mounts_fd_);
    close(mounts_fd_);
  }
  if (ext_thread_.joinable()) ext_thread_.join();
  Fl::remove_timeout(toast_cb, this);
  if (Fl::has_timeout(save_cb, this)) save_settings();
  Fl::remove_timeout(save_cb, this);
  instance_ = nullptr;
  if (scan_thread_.joinable()) scan_thread_.join();
  service_.reset();  // joins the workers before the surfaces go away
  for (auto &kv : thumbs_)
    if (kv.second.surface) cairo_surface_destroy(kv.second.surface);
}

void GalleryWindow::hide() {
  if (viewer_) viewer_->stop_slideshow();
  BufferedWindow::hide();
}

// ------------------------------------------------------------- settings

void GalleryWindow::reload_config() {
  Config cfg;
  cfg.load();
  user_folders_.clear();
  for (auto &f : decode_list(cfg.get("gallery", "folders", ""))) {
    while (f.size() > 1 && f.back() == '/') f.pop_back();
    if (f != pictures_ && f[0] == '/') user_folders_.push_back(f);
  }
  favorites_.clear();
  for (auto &f : decode_list(cfg.get("gallery", "favorites", ""))) favorites_.insert(f);
  zoom_ = std::max(96, std::min(320, cfg.get_int("gallery", "thumbnail_size", 168)));
  std::string s = cfg.get("gallery", "sort", "newest");
  sort_ = s == "oldest" ? Sort::Oldest : s == "name" ? Sort::Name : Sort::Newest;
  prefer_photo_ = cfg.get_bool("gallery", "open_in_photo", false);
  show_sidebar_ = cfg.get_bool("gallery", "sidebar", true);
}

LibrarySettings GalleryWindow::library_settings() const {
  LibrarySettings s;
  s.pictures = pictures_;
  s.user_folders = user_folders_;
  s.favorites = favorites_;
  s.sort = sort_;
  return s;
}

void GalleryWindow::save_settings() {
  Config cfg;
  cfg.load();
  cfg.set("gallery", "folders", encode_list(user_folders_));
  cfg.set("gallery", "favorites",
          encode_list(std::vector<std::string>(favorites_.begin(), favorites_.end())));
  cfg.set("gallery", "thumbnail_size", std::to_string(zoom_));
  cfg.set("gallery", "sort", sort_name(sort_));
  cfg.set("gallery", "open_in_photo", prefer_photo_ ? "true" : "false");
  cfg.set("gallery", "sidebar", show_sidebar_ ? "true" : "false");
  cfg.save();
}

void GalleryWindow::apply_theme() {
  Config cfg;
  cfg.load();
  double scale = ui_scale();
  theme_ = Theme::from_config(cfg);
  // Every size was computed with the scale at start-up; keep it.
  set_ui_scale(scale);
  theme_.scale = scale;
  theme_.apply_to_fltk();
  search_->box(FL_FLAT_BOX);
  search_->color(theme_.surface_alt.fl());
  search_->selection_color(theme_.accent.fl());
  search_->textcolor(theme_.text.fl());
  search_->cursor_color(theme_.accent.fl());
  search_->textsize(theme_.font_size);
  search_->textfont(FL_HELVETICA);
  if (menubar_) menubar_->apply_theme();
  color(theme_.background.fl());
  redraw();
}

int GalleryWindow::x_event_handler(int) {
  if (!instance_ || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == events_atom() && e.xproperty.state == PropertyNewValue) {
    instance_->handle_events();
    return 0;
  }
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    auto *self = instance_;
    std::set<std::string> old_favorites = self->favorites_;
    auto old_folders = self->user_folders_;
    self->reload_config();
    self->apply_theme();
    self->sort_ctl_->value(int(self->sort_));
    self->zoom_ctl_->value(self->zoom_);
    self->update_menu();
    self->layout();
    if (old_folders != self->user_folders_) self->start_scan();
    else if (old_favorites != self->favorites_) self->rebuild_view(true);
    self->grid_->relayout();
    return 0;  // other handlers may want it too
  }
  return 0;
}

// ---------------------------------------------------------------- library

void GalleryWindow::start_scan() {
  if (scanning_) {
    rescan_wanted_ = true;
    return;
  }
  scanning_ = true;
  rescan_wanted_ = false;
  std::vector<std::string> roots{pictures_};
  for (auto &f : user_folders_) roots.push_back(f);
  if (scan_thread_.joinable()) scan_thread_.join();
  auto extra = extra_formats_;
  scan_thread_ = std::thread([this, roots, extra] {
    auto found = scan_folders(roots, extra);
    {
      std::lock_guard<std::mutex> lock(scan_mu_);
      scan_result_ = std::move(found);
    }
    Fl::awake(scan_awake_cb, this);
  });
  redraw();
}

void GalleryWindow::rescan() { start_scan(); }

void GalleryWindow::scan_awake_cb(void *p) {
  if (instance_ == p) static_cast<GalleryWindow *>(p)->scan_finished();
}

void GalleryWindow::scan_finished() {
  {
    std::lock_guard<std::mutex> lock(scan_mu_);
    all_ = std::move(scan_result_);
    scan_result_.clear();
  }
  scanning_ = false;
  // Forget thumbnails of files that changed.
  rebuild_view(true);
  if (rescan_wanted_) start_scan();
  else if (!pending_select_.empty()) {
    std::string p = pending_select_;
    pending_select_.clear();
    select_path(p);
  }
}

bool GalleryWindow::searching() const { return search_ && search_->size() > 0; }

size_t GalleryWindow::count_for(const View &v) const {
  if (v.external()) return v == view_kind_ && !ext_scanning_ ? ext_photos_.size() : 0;
  std::string key = std::to_string(int(v.kind)) + v.folder;
  auto cached = counts_.find(key);
  if (cached != counts_.end()) return cached->second;
  LibrarySettings lib = library_settings();
  Album album = v.album();
  time_t now = time(nullptr);
  size_t n = 0;
  for (const auto &p : all_)
    if (album_contains(album, p, lib, now)) ++n;
  counts_[key] = n;
  return n;
}

std::string GalleryWindow::view_title(const View &v) const {
  switch (v.kind) {
    case ViewKind::All: return _("All Photos");
    case ViewKind::Favorites: return _("Favorites");
    case ViewKind::Recent: return _("Recent");
    case ViewKind::Screenshots: return _("Screenshots");
    case ViewKind::Folder: return v.folder == pictures_ ? std::string(_("Pictures")) : basename_of(v.folder);
    case ViewKind::Device:
      for (const auto &d : devices_)
        if (d.mount_point == v.folder) return d.label();
      return basename_of(v.folder);
    case ViewKind::Browse: return basename_of(v.folder).empty() ? v.folder : basename_of(v.folder);
  }
  return "";
}

void GalleryWindow::rebuild_view(bool keep_scroll) {
  counts_.clear();
  std::string query = search_ ? search_->value() : "";
  std::string focus_path;
  if (grid_ && grid_->focus_index() >= 0 && grid_->focus_index() < int(view_.size()))
    focus_path = view_[size_t(grid_->focus_index())].path;
  // The same filter and order as Photo uses for this album (album.h).
  view_ = album_photos(source(), view_kind_.album(), library_settings(), sort_, time(nullptr), query);
  groups_ = group_photos(view_, sort_);
  if (grid_) {
    if (keep_scroll) grid_->relayout();
    else grid_->reset();
    if (!focus_path.empty())
      for (size_t i = 0; i < view_.size(); ++i)
        if (view_[i].path == focus_path) grid_->set_focus_index(int(i));
  }
  if (viewer_open()) viewer_->photos_changed();
  update_header_widgets();
  update_menu();
  redraw();
}

void GalleryWindow::set_view(const View &v) {
  view_kind_ = v;
  if (v.external() && (ext_root_ != v.folder || ext_photos_.empty())) {
    ext_photos_.clear();
    start_external_scan(v.folder);
  }
  rebuild_view(false);
  layout();
}

// ------------------------------------------------------ devices and browsing

void GalleryWindow::refresh_devices() {
  auto now = current_removable_mounts();
  bool same = now.size() == devices_.size();
  for (size_t i = 0; same && i < now.size(); ++i) same = now[i].mount_point == devices_[i].mount_point;
  if (same) return;
  for (const auto &d : now) {
    bool known = false;
    for (const auto &o : devices_) known |= o.mount_point == d.mount_point;
    if (!known && !devices_.empty()) toast(format(_("“%s” is connected"), d.label().c_str()));
  }
  devices_ = now;
  // Leave a device view whose medium was removed.
  if (view_kind_.kind == ViewKind::Device) {
    bool present = false;
    for (const auto &d : devices_) present |= d.mount_point == view_kind_.folder;
    if (!present) {
      if (viewer_open()) close_viewer();
      ext_photos_.clear();
      set_view(View{});
    }
  }
  sidebar_->redraw();
  update_menu();
  redraw();
}

void GalleryWindow::mounts_cb(int, void *p) {
  if (instance_ == p) static_cast<GalleryWindow *>(p)->refresh_devices();
}

void GalleryWindow::mounts_timer_cb(void *p) {
  if (instance_ == p) static_cast<GalleryWindow *>(p)->refresh_devices();
  Fl::repeat_timeout(5.0, mounts_timer_cb, p);
}

void GalleryWindow::start_external_scan(const std::string &root) {
  ext_wanted_ = root;
  if (ext_scanning_) return;
  ext_scanning_ = true;
  ext_root_ = root;
  if (ext_thread_.joinable()) ext_thread_.join();
  auto extra = extra_formats_;
  ext_thread_ = std::thread([this, root, extra] {
    auto found = scan_folders({root}, extra, 50000);
    {
      std::lock_guard<std::mutex> lock(scan_mu_);
      ext_result_ = std::move(found);
      ext_result_root_ = root;
    }
    Fl::awake(ext_awake_cb, this);
  });
  redraw();
}

void GalleryWindow::ext_awake_cb(void *p) {
  if (instance_ == p) static_cast<GalleryWindow *>(p)->external_scan_finished();
}

void GalleryWindow::external_scan_finished() {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(scan_mu_);
    root = ext_result_root_;
    if (view_kind_.external() && view_kind_.folder == root) ext_photos_ = std::move(ext_result_);
    ext_result_.clear();
  }
  ext_scanning_ = false;
  if (ext_wanted_ != root && view_kind_.external() && view_kind_.folder == ext_wanted_) {
    start_external_scan(ext_wanted_);
    return;
  }
  rebuild_view(true);
  sidebar_->redraw();
  if (!pending_select_.empty()) {
    std::string p = pending_select_;
    pending_select_.clear();
    select_path(p);
  }
}

void GalleryWindow::browse(const std::string &dir_in) {
  std::string dir = dir_in;
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  if (!dir_exists(dir)) return;
  for (const auto &d : devices_)
    if (d.mount_point == dir) {
      set_view(View{ViewKind::Device, dir});
      return;
    }
  if (dir == pictures_ || std::find(user_folders_.begin(), user_folders_.end(), dir) != user_folders_.end()) {
    set_view(View{ViewKind::Folder, dir});
    return;
  }
  set_view(View{ViewKind::Browse, dir});
}

void GalleryWindow::open_folder_dialog() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
  chooser.title(_("Open a Folder"));
  chooser.directory((devices_.empty() ? pictures_ : devices_[0].mount_point).c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  browse(chooser.filename());
}

void GalleryWindow::open_file_dialog() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.title(_("Open a Photo"));
  chooser.filter(_("Images\t*.{jpg,jpeg,png,gif,bmp,svg,webp,tif,tiff}"));
  chooser.directory(pictures_.c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  std::string path = chooser.filename();
  std::string dir = path.substr(0, path.rfind('/'));
  // Photo steps through the photos of that folder.
  if (!open_in_photo(path, Album{AlbumKind::Directory, dir.empty() ? "/" : dir}, Sort::Name)) {
    pending_select_ = path;
    browse(dir);
  }
}

void GalleryWindow::eject(const std::string &mount_point) {
  std::string label = basename_of(mount_point);
  std::vector<std::string> argv;
  if (!find_program("gio").empty()) argv = {"gio", "mount", "-u", mount_point};
  else if (!find_program("umount").empty()) argv = {"umount", mount_point};
  else {
    toast(_("Ejecting needs the gio program (libglib2.0-bin)"));
    return;
  }
  if (view_kind_.kind == ViewKind::Device && view_kind_.folder == mount_point) {
    if (viewer_open()) close_viewer();
    set_view(View{});
  }
  run_async(argv, [this, label](const std::string &, int status) {
    if (instance_ != this) return;
    toast(status == 0 ? format(_("“%s” can be removed safely"), label.c_str())
                      : format(_("“%s” is busy and could not be ejected"), label.c_str()));
    refresh_devices();
  });
}

void GalleryWindow::import_dialog() {
  if (importing_) {
    toast(_("An import is already running"));
    return;
  }
  if (view_kind_.kind == ViewKind::Device) {
    for (const auto &d : devices_)
      if (d.mount_point == view_kind_.folder) {
        std::vector<Photo> only;
        for (size_t i : grid_->selection()) only.push_back(view_[i]);
        import_from(d, only);
        return;
      }
  }
  if (devices_.empty()) {
    fl_message_title(_("Import from Device"));
    fl_message("%s", _("No camera, SD card or USB drive is connected.\n\n"
                       "Connect one, and it appears under Devices in the sidebar."));
    return;
  }
  if (devices_.size() == 1) {
    import_from(devices_[0]);
    return;
  }
  // Several devices: let the user pick one.
  std::vector<Fl_Menu_Item> items(devices_.size() + 1);
  for (auto &it : items) memset(&it, 0, sizeof it);
  std::vector<std::string> labels;
  for (const auto &d : devices_) labels.push_back(d.label());
  for (size_t i = 0; i < devices_.size(); ++i) {
    items[i].text = labels[i].c_str();
    items[i].labelsize(theme_.font_size);
    items[i].labelcolor(theme_.text.fl());
  }
  const Fl_Menu_Item *picked = items.data()->popup(w() / 2 - S(80), top_ + S(80), _("Import from"));
  if (picked) import_from(devices_[size_t(picked - items.data())]);
}

void GalleryWindow::import_from(const MountEntry &device, const std::vector<Photo> &only) {
  std::string label = device.label();
  std::vector<Photo> photos = only;
  if (photos.empty()) {
    if (view_kind_.kind == ViewKind::Device && view_kind_.folder == device.mount_point && !ext_scanning_)
      photos = ext_photos_;
    else
      photos = scan_folders({device.mount_point}, extra_formats_, 50000);
  }
  if (photos.empty()) {
    toast(format(_("No photos found on “%s”"), label.c_str()));
    return;
  }
  std::string dest = import_root(pictures_);
  auto plan = plan_import(photos, dest, [](const std::string &p) {
    struct stat st {};
    return stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1LL;
  });
  size_t todo = 0;
  for (const auto &i : plan) todo += i.skip ? 0 : 1;
  if (todo == 0) {
    toast(format(_("All photos on “%s” were imported already"), label.c_str()));
    return;
  }
  std::string question = format(_("Copy %zu photos from “%s” to %s?"), todo, label.c_str(), dest.c_str());
  if (plan.size() > todo) question += "\n" + format(_("%zu photos were imported before and are skipped."), plan.size() - todo);
  fl_message_title(_("Import Photos"));
  if (fl_choice("%s", _("Cancel"), _("Import"), nullptr, question.c_str()) != 1) return;
  importing_ = true;
  toast(format(_("Importing %zu photos…"), todo));
  // Copy on a worker thread; report back through the event loop.
  struct Job {
    GalleryWindow *self;
    size_t done = 0, failed = 0, total = 0;
    std::string error;
  };
  auto *job = new Job{this, 0, 0, todo, ""};
  std::thread([job, plan] {
    for (const auto &i : plan) {
      if (i.skip) continue;
      std::string err;
      if (copy_file(i.source, i.dest, &err)) ++job->done;
      else {
        ++job->failed;
        job->error = err;
      }
    }
    Fl::awake([](void *p) {
      auto *job = static_cast<Job *>(p);
      GalleryWindow *self = job->self;
      if (instance_ == self) {
        self->importing_ = false;
        if (job->failed)
          self->toast(format(_("Imported %zu photos; %zu could not be copied (%s)"), job->done, job->failed,
                             job->error.c_str()));
        else
          self->toast(format(job->done == 1 ? _("Imported %zu photo") : _("Imported %zu photos"), job->done));
        self->start_scan();
      }
      delete job;
    }, job);
  }).detach();
}

void GalleryWindow::select_all() {
  if (viewer_open() || view_.empty()) return;
  Fl::focus(grid_);
  grid_->handle_select_all();
}

void GalleryWindow::toggle_sidebar() {
  show_sidebar_ = !show_sidebar_;
  save_settings();
  layout();
  update_menu();
  redraw();
}

void GalleryWindow::toggle_fullscreen() {
  if (fullscreen_active()) fullscreen_off();
  else fullscreen();
  update_menu();
}

void GalleryWindow::show_about_dialog() {
  AboutInfo info;
  info.name = _("Gallery");
  info.program = "sde-gallery";
  info.version = SDE_VERSION;
  info.description = _("Browse the photos in your Pictures folder, your own folders and on cameras, "
                       "SD cards and USB drives. Open them in Photo to view details and edit them.");
  info.logo = draw_app_icon;
  info.credits.push_back(format(_("Built with SDE-FLTK %s and cairo"), SDE_FLTK_VERSION));
  show_about(info);
}

void GalleryWindow::show_shortcuts_dialog() {
  show_shortcuts(_("Gallery Keyboard Shortcuts"),
                 {{"", _("Browsing")},
                  {_("Arrows"), _("Move between photos")},
                  {_("Enter"), _("Open the photo")},
                  {_("Space"), _("Select or deselect")},
                  {_("Shift+Arrows"), _("Extend the selection")},
                  {_("Ctrl+A"), _("Select all")},
                  {_("Esc"), _("Clear the selection")},
                  {_("Ctrl+F"), _("Search")},
                  {_("Ctrl+wheel, + −"), _("Thumbnail size")},
                  {"", _("Photos")},
                  {_("Ctrl+D"), _("Add to or remove from Favorites")},
                  {_("Ctrl+E"), _("Open in Photo")},
                  {_("Ctrl+C"), _("Copy the path")},
                  {_("Delete"), _("Move to the Trash")},
                  {"", _("Library")},
                  {_("Ctrl+O"), _("Open a photo")},
                  {_("Ctrl+Shift+O"), _("Open a folder")},
                  {_("Ctrl+I"), _("Import from a device")},
                  {_("F5"), _("Refresh")},
                  {"", _("Window")},
                  {_("F9"), _("Show or hide the sidebar")},
                  {_("F11"), _("Full screen")},
                  {_("F1"), _("Keyboard shortcuts")},
                  {_("Ctrl+Q"), _("Quit")}});
}

std::vector<std::string> GalleryWindow::selected_or_focused() const {
  std::vector<std::string> paths;
  for (size_t i : grid_->selection()) paths.push_back(view_[i].path);
  if (paths.empty() && grid_->focus_index() >= 0 && grid_->focus_index() < int(view_.size()))
    paths.push_back(view_[size_t(grid_->focus_index())].path);
  return paths;
}

// ----------------------------------------------------------------- menus

void GalleryWindow::build_menu() {
  auto add = [this](const char *label, int shortcut, long id, int flags = 0) {
    menubar_->add(label, shortcut, menu_cb, reinterpret_cast<void *>(id), flags);
  };
  std::string f = std::string(_("&File")) + "/", e = std::string(_("&Edit")) + "/",
              v = std::string(_("&View")) + "/", h = std::string(_("&Help")) + "/";
  auto L = [](const std::string &menu, const char *item) {
    // A slash in a translated label would start a submenu.
    std::string it = item;
    for (auto &c : it)
      if (c == '/') c = '|';
    return menu + it;
  };
  add(L(f, _("Open File…")).c_str(), FL_CTRL + 'o', M_OPEN_FILE);
  add(L(f, _("Open Folder…")).c_str(), FL_CTRL + FL_SHIFT + 'o', M_OPEN_FOLDER);
  add(L(f, _("Import from Device…")).c_str(), FL_CTRL + 'i', M_IMPORT, FL_MENU_DIVIDER);
  add(L(f, _("Add Folder to Library…")).c_str(), 0, M_ADD_FOLDER);
  add(L(f, _("Refresh")).c_str(), FL_F + 5, M_REFRESH, FL_MENU_DIVIDER);
  add(L(f, _("Quit")).c_str(), FL_CTRL + 'q', M_QUIT);
  add(L(e, _("Select All")).c_str(), FL_CTRL + 'a', M_SELECT_ALL);
  add(L(e, _("Select None")).c_str(), FL_CTRL + FL_SHIFT + 'a', M_SELECT_NONE, FL_MENU_DIVIDER);
  add(L(e, _("Add to Favorites")).c_str(), FL_CTRL + 'd', M_FAVORITE);
  add(L(e, _("Copy Path")).c_str(), FL_CTRL + 'c', M_COPY_PATH);
  add(L(e, _("Move to Trash…")).c_str(), 0, M_TRASH, FL_MENU_DIVIDER);
  add(L(e, _("Open in Photo")).c_str(), FL_CTRL + 'e', M_OPEN_PHOTO);
  add(L(e, _("Set as Wallpaper")).c_str(), 0, M_WALLPAPER);
  add(L(e, _("Show in Files")).c_str(), 0, M_SHOW_FILES);
  add(L(v, _("Newest First")).c_str(), 0, M_SORT_NEWEST, FL_MENU_RADIO);
  add(L(v, _("Oldest First")).c_str(), 0, M_SORT_OLDEST, FL_MENU_RADIO);
  add(L(v, _("By Name")).c_str(), 0, M_SORT_NAME, FL_MENU_RADIO | FL_MENU_DIVIDER);
  add(L(v, _("Larger Thumbnails")).c_str(), FL_CTRL + '=', M_ZOOM_IN);
  add(L(v, _("Smaller Thumbnails")).c_str(), FL_CTRL + '-', M_ZOOM_OUT, FL_MENU_DIVIDER);
  add(L(v, _("Show Sidebar")).c_str(), FL_F + 9, M_SIDEBAR, FL_MENU_TOGGLE);
  add(L(v, _("Open Photos in Photo")).c_str(), 0, M_PREFER_PHOTO, FL_MENU_TOGGLE | FL_MENU_DIVIDER);
  add(L(v, _("Full Screen")).c_str(), FL_F + 11, M_FULLSCREEN, FL_MENU_TOGGLE);
  add(L(h, _("Keyboard Shortcuts")).c_str(), FL_F + 1, M_SHORTCUTS);
  add(L(h, _("About Gallery")).c_str(), 0, M_ABOUT);
  update_menu();
}

void GalleryWindow::update_menu() {
  if (!menubar_) return;
  for (int i = 0; i < menubar_->size(); ++i) {
    Fl_Menu_Item &it = const_cast<Fl_Menu_Item &>(menubar_->menu()[i]);
    if (!it.text || it.submenu()) continue;
    long id = reinterpret_cast<long>(it.user_data());
    auto state = [&](bool on) { on ? it.set() : it.clear(); };
    switch (id) {
      case M_SORT_NEWEST: state(sort_ == Sort::Newest); break;
      case M_SORT_OLDEST: state(sort_ == Sort::Oldest); break;
      case M_SORT_NAME: state(sort_ == Sort::Name); break;
      case M_SIDEBAR: state(show_sidebar_); break;
      case M_PREFER_PHOTO: state(prefer_photo_); break;
      case M_FULLSCREEN: state(shown() && fullscreen_active()); break;
      // Items that depend on the selection stay active: the menu cannot
      // follow every focus change, and the actions check for themselves.
    }
  }
}

void GalleryWindow::menu_cb(Fl_Widget *, void *data) {
  auto *self = instance_;
  if (!self) return;
  long id = reinterpret_cast<long>(data);
  auto paths = self->selected_or_focused();
  switch (id) {
    case M_OPEN_FILE: self->open_file_dialog(); break;
    case M_OPEN_FOLDER: self->open_folder_dialog(); break;
    case M_IMPORT: self->import_dialog(); break;
    case M_ADD_FOLDER: self->add_folder(); break;
    case M_REFRESH: self->rescan(); break;
    case M_QUIT: self->hide(); break;
    case M_SELECT_ALL: self->select_all(); break;
    case M_SELECT_NONE: self->grid_->clear_selection(); break;
    case M_FAVORITE:
    case M_COPY_PATH:
    case M_TRASH:
      if (paths.empty()) {
        self->toast(_("Select a photo first"));
        break;
      }
      if (id == M_FAVORITE) self->toggle_favorite(paths);
      else if (id == M_COPY_PATH) self->copy_paths(paths);
      else self->trash(paths);
      break;
    case M_OPEN_PHOTO:
    case M_WALLPAPER:
    case M_SHOW_FILES:
      if (paths.size() != 1) {
        self->toast(paths.empty() ? _("Select a photo first") : _("Select only one photo"));
        break;
      }
      if (id == M_OPEN_PHOTO) self->edit_in_photo(paths[0]);
      else if (id == M_WALLPAPER) self->set_wallpaper(paths[0]);
      else self->show_in_files(paths[0]);
      break;
    case M_SORT_NEWEST:
    case M_SORT_OLDEST:
    case M_SORT_NAME:
      self->sort_ = id == M_SORT_NEWEST ? Sort::Newest : id == M_SORT_OLDEST ? Sort::Oldest : Sort::Name;
      self->sort_ctl_->value(int(self->sort_));
      self->save_settings();
      self->rebuild_view();
      break;
    case M_ZOOM_IN: self->set_zoom(self->zoom_ + 24); break;
    case M_ZOOM_OUT: self->set_zoom(self->zoom_ - 24); break;
    case M_SIDEBAR: self->toggle_sidebar(); break;
    case M_PREFER_PHOTO:
      self->prefer_photo_ = !self->prefer_photo_;
      self->save_settings();
      break;
    case M_FULLSCREEN: self->toggle_fullscreen(); break;
    case M_SHORTCUTS: self->show_shortcuts_dialog(); break;
    case M_ABOUT: self->show_about_dialog(); break;
  }
  self->update_menu();
}

// --------------------------------------------------------- Photo events

void GalleryWindow::forget_thumbnails(const std::string &path) {
  for (bool large : {false, true}) {
    auto it = thumbs_.find(thumb_key(path, large));
    if (it != thumbs_.end()) {
      if (it->second.surface) cairo_surface_destroy(it->second.surface);
      thumbs_.erase(it);
    }
  }
  failed_.erase(path);
  dims_.erase(path);
}

void GalleryWindow::handle_events() {
  auto events = take_gallery_events();
  if (events.empty()) return;
  bool changed = false, favorites = false;
  std::string select;
  auto update = [&](std::vector<Photo> &list, const std::string &path, bool remove) {
    for (size_t i = 0; i < list.size(); ++i) {
      if (list[i].path != path) continue;
      if (remove) {
        list.erase(list.begin() + long(i));
      } else {
        struct stat st {};
        if (stat(path.c_str(), &st) == 0) {
          list[i].mtime = st.st_mtime;
          list[i].size = st.st_size;
        }
      }
      return true;
    }
    return false;
  };
  for (const auto &e : events) {
    if (e.verb == "select") {
      select = e.path;
    } else if (e.verb == "present") {
      select = "";
      x11::activate_window(fl_xid(this));
    } else if (e.verb == "changed" || e.verb == "removed" || e.verb == "added") {
      bool remove = e.verb == "removed" || !file_exists(e.path);
      forget_thumbnails(e.path);
      bool known = update(all_, e.path, remove);
      known |= update(ext_photos_, e.path, remove);
      if (remove && favorites_.erase(e.path)) favorites = true;
      if (!known && !remove) {
        // A new file (Save As): add it when it belongs to what is shown.
        auto ph = photos_for_paths({e.path});
        if (!ph.empty() && is_image_name(ph[0].name, extra_formats_)) {
          bool in_library = false;
          for (const auto &r : library_settings().roots()) in_library |= path_under(e.path, r);
          if (in_library) all_.push_back(ph[0]);
          if (view_kind_.external() && path_under(e.path, view_kind_.folder)) ext_photos_.push_back(ph[0]);
        }
      }
      changed = true;
    }
  }
  if (favorites) save_settings();
  if (changed) rebuild_view(true);
  if (!select.empty()) select_path(select);
}

void GalleryWindow::select_path(const std::string &path) {
  auto find = [&]() { return index_of(view_, path); };
  int i = find();
  if (i < 0 && index_of(all_, path) >= 0) {
    search_->value("");
    set_view(View{});
    i = find();
  }
  if (i < 0) {
    // Not in the library: show its folder.
    std::string dir = path.substr(0, path.rfind('/'));
    if (view_kind_.external() && view_kind_.folder == dir && ext_scanning_) {
      pending_select_ = path;
    } else if (!(view_kind_.external() && path_under(path, view_kind_.folder))) {
      pending_select_ = path;
      if (scanning_) {
        // The library scan may still find it.
        return;
      }
      browse(dir.empty() ? "/" : dir);
      return;
    }
  }
  if (viewer_open()) close_viewer();
  if (i >= 0) {
    grid_->clear_selection();
    grid_->set_focus_index(i);
    grid_->select_index(i);
    grid_->ensure_visible(i);
    Fl::focus(grid_);
    layout();
  }
  if (shown()) x11::activate_window(fl_xid(this));
  redraw();
}

void GalleryWindow::open_photo(int index) {
  if (index < 0 || index >= int(view_.size())) return;
  if (prefer_photo_ && !find_program("sde-view").empty()) edit_in_photo(view_[size_t(index)].path);
  else open_viewer(index);
}

void GalleryWindow::set_zoom(int z) {
  z = std::max(96, std::min(320, z));
  if (z == zoom_) return;
  zoom_ = z;
  zoom_ctl_->value(z);
  grid_->relayout();
  Fl::remove_timeout(save_cb, this);
  Fl::add_timeout(1.0, save_cb, this);
  redraw();
}

// ------------------------------------------------------------- thumbnails

cairo_surface_t *GalleryWindow::thumbnail(const Photo &p, bool large, bool *failed) {
  if (failed) *failed = failed_.count(p.path) != 0;
  auto it = thumbs_.find(thumb_key(p.path, large));
  if (it != thumbs_.end()) {
    it->second.used = frame_;
    return it->second.surface;
  }
  it = thumbs_.find(thumb_key(p.path, !large));
  if (it != thumbs_.end()) {
    it->second.used = frame_;
    return it->second.surface;
  }
  return nullptr;
}

void GalleryWindow::request_thumbnails(const std::vector<std::pair<const Photo *, bool>> &wanted) {
  ++frame_;
  std::vector<ThumbnailService::Request> jobs;
  for (const auto &w : wanted) {
    if (failed_.count(w.first->path) || thumbs_.count(thumb_key(w.first->path, w.second))) continue;
    jobs.push_back({w.first->path, w.first->mtime, w.second ? JobKind::Large : JobKind::Normal});
  }
  service_->replace_queue(std::move(jobs));
}

void GalleryWindow::awake_cb(void *p) {
  if (instance_ == p) static_cast<GalleryWindow *>(p)->results_ready();
}

void GalleryWindow::results_ready() {
  bool changed = false;
  for (auto &r : service_->take_results()) {
    if (r.kind == JobKind::Full) {
      viewer_->full_loaded(r);  // takes ownership of the surface
      continue;
    }
    std::string key = thumb_key(r.path, r.kind == JobKind::Large);
    if (r.width > 0 && r.height > 0) dims_[r.path] = {r.width, r.height};
    if (!r.surface) {
      failed_.insert(r.path);
    } else {
      auto &slot = thumbs_[key];
      if (slot.surface) cairo_surface_destroy(slot.surface);
      slot.surface = r.surface;
      slot.used = frame_;
    }
    changed = true;
  }
  // Keep memory bounded: drop the least recently drawn thumbnails.
  const size_t limit = 1500;
  if (thumbs_.size() > limit) {
    std::vector<std::pair<unsigned long long, std::string>> order;
    for (auto &kv : thumbs_) order.push_back({kv.second.used, kv.first});
    std::sort(order.begin(), order.end());
    for (size_t i = 0; i < thumbs_.size() - limit * 3 / 4 && i < order.size(); ++i) {
      if (order[i].first + 2 >= frame_) break;  // still on screen
      auto it = thumbs_.find(order[i].second);
      cairo_surface_destroy(it->second.surface);
      thumbs_.erase(it);
    }
  }
  if (changed) {
    grid_->redraw();
    if (viewer_open()) viewer_->redraw();
  }
}

void GalleryWindow::dimensions(const std::string &path, int &w, int &h) const {
  auto it = dims_.find(path);
  w = it == dims_.end() ? 0 : it->second.first;
  h = it == dims_.end() ? 0 : it->second.second;
}

// --------------------------------------------------------------- actions

void GalleryWindow::toggle_favorite(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  bool all = std::all_of(paths.begin(), paths.end(), [this](const std::string &p) { return is_favorite(p); });
  for (const auto &p : paths) {
    if (all) favorites_.erase(p);
    else favorites_.insert(p);
  }
  save_settings();
  counts_.clear();
  if (all) toast(paths.size() == 1 ? _("Removed from Favorites") : format(_("%zu photos removed from Favorites"), paths.size()));
  else toast(paths.size() == 1 ? _("Added to Favorites") : format(_("%zu photos added to Favorites"), paths.size()));
  if (view_kind_.kind == ViewKind::Favorites && !viewer_open()) rebuild_view(true);
  redraw();
}

void GalleryWindow::add_folder() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
  chooser.title(_("Add a Folder to Gallery"));
  chooser.directory(home_dir().c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  add_folder(chooser.filename());
}

void GalleryWindow::add_folder(const std::string &chosen) {
  std::string dir = chosen;
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  if (dir.empty() || !dir_exists(dir)) return;
  if (dir != pictures_ && std::find(user_folders_.begin(), user_folders_.end(), dir) == user_folders_.end())
    user_folders_.push_back(dir);
  save_settings();
  view_kind_ = View{ViewKind::Folder, dir};
  start_scan();
  rebuild_view(false);
}

void GalleryWindow::remove_folder(const std::string &folder) {
  auto it = std::find(user_folders_.begin(), user_folders_.end(), folder);
  if (it == user_folders_.end()) return;
  user_folders_.erase(it);
  save_settings();
  if (view_kind_.kind == ViewKind::Folder && view_kind_.folder == folder) view_kind_ = View{};
  toast(format(_("“%s” was removed from Gallery. No files were deleted."), basename_of(folder).c_str()));
  start_scan();
  rebuild_view(false);
}

void GalleryWindow::edit_in_photo(const std::string &path) {
  Album album = view_kind_.album();
  if (searching()) {
    // Photo steps through the search results: hand them over as a list.
    std::vector<std::string> paths;
    for (const auto &p : view_) paths.push_back(p.path);
    std::string file = runtime_dir() + "/sde-gallery/results-" + std::to_string(getpid()) + ".list";
    if (write_album_list(file, paths)) album = Album{AlbumKind::List, file};
  }
  if (!open_in_photo(path, album, sort_)) toast(_("Photo (sde-view) is not installed"));
}

void GalleryWindow::open_with(const std::string &path) {
  if (find_program("xdg-open").empty() || !spawn_detached({"xdg-open", path}))
    toast(_("xdg-open is not available"));
}

void GalleryWindow::set_wallpaper(const std::string &path) {
  Config cfg;
  cfg.load();
  cfg.set("appearance", "wallpaper", path);
  if (!cfg.save()) {
    toast(_("Could not save the wallpaper setting"));
    return;
  }
  x11::notify_config_changed();
  toast(_("Set as wallpaper"));
}

void GalleryWindow::show_in_files(const std::string &path) {
  std::string dir = path.substr(0, path.rfind('/'));
  if (dir.empty()) dir = "/";
  if (!find_program("liria").empty() && spawn_detached({"liria", "--no-desktop", dir})) return;
  if (!open_uri(dir)) toast(_("No file manager is available"));
}

void GalleryWindow::copy_paths(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  std::string text;
  for (const auto &p : paths) text += (text.empty() ? "" : "\n") + p;
  Fl::copy(text.c_str(), int(text.size()), 1);
  Fl::copy(text.c_str(), int(text.size()), 0);
  toast(paths.size() == 1 ? _("Path copied") : format(_("%zu paths copied"), paths.size()));
}

void GalleryWindow::trash(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  std::string question =
      paths.size() == 1 ? format(_("Move “%s” to the Trash?"), basename_of(paths[0]).c_str())
                        : format(_("Move %zu photos to the Trash?"), paths.size());
  fl_message_title(_("Move to Trash"));
  if (fl_choice("%s", _("Cancel"), _("Move to Trash"), nullptr, question.c_str()) != 1) return;
  size_t removed = 0;
  std::string error;
  for (const auto &p : paths) {
    std::string err;
    if (!move_to_trash(p, &err)) {
      error = err;
      continue;
    }
    ++removed;
    favorites_.erase(p);
    forget_thumbnails(p);
    for (auto *list : {&all_, &ext_photos_})
      list->erase(std::remove_if(list->begin(), list->end(), [&](const Photo &ph) { return ph.path == p; }),
                  list->end());
  }
  if (removed) save_settings();
  rebuild_view(true);
  if (removed < paths.size())
    toast(format(_("Some files could not be moved to the Trash: %s"), error.c_str()));
  else
    toast(removed == 1 ? _("Moved to the Trash") : format(_("%zu photos moved to the Trash"), removed));
}

void GalleryWindow::open_viewer(int index) {
  if (index < 0 || index >= int(view_.size())) return;
  menubar_->hide();
  import_btn_->hide();
  sidebar_->hide();
  grid_->hide();
  search_->hide();
  sort_ctl_->hide();
  zoom_ctl_->hide();
  sel_fav_->hide();
  sel_trash_->hide();
  sel_clear_->hide();
  viewer_->resize(0, 0, w(), h());
  viewer_->show();
  viewer_->open(index);
  Fl::focus(viewer_);
  redraw();
}

void GalleryWindow::close_viewer() {
  int index = viewer_->index();
  viewer_->close();
  viewer_->hide();
  menubar_->show();
  if (show_sidebar_) sidebar_->show();
  grid_->show();
  search_->show();
  sort_ctl_->show();
  layout();
  if (index >= 0 && index < int(view_.size())) {
    grid_->set_focus_index(index);
    grid_->ensure_visible(index);
  }
  if (view_kind_.kind == ViewKind::Favorites) rebuild_view(true);
  Fl::focus(grid_);
  redraw();
}

bool GalleryWindow::viewer_open() const { return viewer_ && viewer_->visible(); }

void GalleryWindow::toast(const std::string &message) {
  toast_ = message;
  Fl::remove_timeout(toast_cb, this);
  Fl::add_timeout(2.8, toast_cb, this);
  redraw();
}

void GalleryWindow::save_cb(void *p) { static_cast<GalleryWindow *>(p)->save_settings(); }

void GalleryWindow::toast_cb(void *p) {
  auto *self = static_cast<GalleryWindow *>(p);
  self->toast_.clear();
  self->redraw();
}

void GalleryWindow::focus_search() {
  if (viewer_open()) return;
  Fl::focus(search_);
  search_->position(search_->size(), 0);
  redraw();
}

// ----------------------------------------------------------------- layout

void GalleryWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void GalleryWindow::layout() {
  header_h_ = S(64);
  top_ = viewer_open() ? 0 : S(30);
  menubar_->resize(0, 0, w(), S(30));
  int sw = w() >= S(900) ? S(232) : S(68);
  sidebar_->resize(0, top_, sw, h() - top_);
  if (!show_sidebar_) {
    sw = 0;
    sidebar_->hide();
  } else if (!viewer_open()) {
    sidebar_->show();
  }
  grid_->resize(sw, top_ + header_h_, w() - sw, h() - top_ - header_h_);
  if (viewer_open()) viewer_->resize(0, 0, w(), h());
  update_header_widgets();
}

void GalleryWindow::update_header_widgets() {
  if (!grid_ || viewer_open()) return;
  int sw = sidebar_->visible() ? sidebar_->w() : 0;
  int right = w() - S(20), cy = top_ + header_h_ / 2;
  if (view_kind_.kind == ViewKind::Device && !devices_.empty()) {
    import_btn_->resize(right - S(120), cy - S(16), S(120), S(32));
    import_btn_->show();
    right -= S(120) + S(12);
  } else {
    import_btn_->hide();
  }
  int avail = right - sw - S(24);
  // Leave the view title room before adding the optional controls.
  text_font(theme_.font_size + S(5), true);
  int title_w = std::max(S(150), std::min(S(320), text_width(view_title(view_kind_)) + S(24)));
  bool show_zoom = avail - title_w > S(180) + S(234) + S(220), show_sort = avail - title_w > S(234) + S(170);
  if (show_zoom) {
    zoom_ctl_->resize(right - S(120) - S(22), cy - S(14), S(120), S(28));
    zoom_ctl_->show();
    right -= S(120) + S(44) + S(16);
  } else {
    zoom_ctl_->hide();
  }
  if (show_sort) {
    sort_ctl_->resize(right - S(222), cy - S(16), S(222), S(32));
    sort_ctl_->show();
    right -= S(222) + S(12);
  } else {
    sort_ctl_->hide();
  }
  int used = (show_zoom ? S(180) : 0) + (show_sort ? S(234) : 0);
  int search_w = std::max(S(140), std::min(S(260), avail - title_w - used));
  // The text field sits inside a rounded pill drawn by the window.
  search_->resize(right - search_w + S(34), cy - S(13), search_w - S(44), S(26));
  size_t selected = grid_->selection().size();
  bool sel = selected > 0;
  // While photos are selected, the search field makes room for actions.
  if (sel) search_->hide();
  else search_->show();
  int bx = sw + S(24) + S(190);
  int bw_fav = S(96), bw_trash = S(128), bw_done = S(76);
  if (sel && bx + bw_fav + bw_trash + bw_done + S(16) <= right) {
    sel_fav_->resize(bx, cy - S(16), bw_fav, S(32));
    sel_trash_->resize(bx + bw_fav + S(8), cy - S(16), bw_trash, S(32));
    sel_clear_->resize(bx + bw_fav + bw_trash + S(16), cy - S(16), bw_done, S(32));
    sel_fav_->show();
    sel_trash_->show();
    sel_clear_->show();
  } else if (sel) {
    sel_fav_->hide();
    sel_trash_->hide();
    sel_clear_->resize(std::min(bx, right - bw_done), cy - S(16), bw_done, S(32));
    sel_clear_->show();
  } else {
    sel_fav_->hide();
    sel_trash_->hide();
    sel_clear_->hide();
  }
}

void GalleryWindow::draw_header(Painter &p) {
  int sw = sidebar_->visible() ? sidebar_->w() : 0;
  p.fill_rect(sw, top_, w() - sw, header_h_, theme_.background);
  p.line(sw, top_ + header_h_ - 0.5, w(), top_ + header_h_ - 0.5, theme_.border.alpha(0.7));
  // Search pill.
  if (search_->visible()) {
  int sx = search_->x() - S(34), sy = search_->y() - S(3), sww = search_->w() + S(44), sh = S(32);
  p.fill_rounded(sx, sy, sww, sh, sh / 2.0, theme_.surface_alt);
  p.stroke_rounded(sx + 0.5, sy + 0.5, sww - 1, sh - 1, sh / 2.0,
                   Fl::focus() == search_ ? theme_.accent : theme_.border, Fl::focus() == search_ ? Sd(1.5) : 1);
  icon::draw(p, icon::Search, sx + S(10), sy + S(7), S(18), theme_.text_dim);
  }
  if (zoom_ctl_->visible()) {
    icon::draw(p, icon::Small, zoom_ctl_->x() - S(22), top_ + header_h_ / 2.0 - S(9), S(18), theme_.text_dim);
    icon::draw(p, icon::Large, zoom_ctl_->x() + zoom_ctl_->w() + S(4), top_ + header_h_ / 2.0 - S(10), S(20),
               theme_.text_dim);
  }
}

void GalleryWindow::draw_header_text() {
  int sw = sidebar_->visible() ? sidebar_->w() : 0, x = sw + S(24), ty = top_;
  size_t selected = grid_->selection().size();
  int limit = (search_->x() - S(44)) - x;
  if (sel_clear_->visible()) limit = (sel_fav_->visible() ? sel_fav_->x() : sel_clear_->x()) - x - S(10);
  if (selected) {
    text_font(theme_.font_size + S(5), true);
    draw_text(format(_("%zu selected"), selected), x, ty + S(10), limit, S(28), theme_.text);
    text_font(theme_.font_size - S(1));
    draw_text(_("Press Esc to clear"), x, ty + S(36), limit, S(18), theme_.text_dim);
  } else {
    text_font(theme_.font_size + S(5), true);
    draw_text(view_title(view_kind_), x, ty + S(10), limit, S(28), theme_.text);
    text_font(theme_.font_size - S(1));
    std::string sub;
    if (view_kind_.external() && ext_scanning_) sub = _("Looking for photos…");
    else if (scanning_ && all_.empty() && !view_kind_.external()) sub = _("Looking for photos…");
    else if (searching()) sub = format(view_.size() == 1 ? _("%zu result") : _("%zu results"), view_.size());
    else sub = format(view_.size() == 1 ? _("%zu photo") : _("%zu photos"), view_.size());
    if (scanning_ && !all_.empty() && !view_kind_.external()) sub += "  ·  " + std::string(_("Updating…"));
    if (view_kind_.kind == ViewKind::Device) sub += "  ·  " + std::string(_("on removable media"));
    else if (view_kind_.kind == ViewKind::Browse) sub += "  ·  " + view_kind_.folder;
    draw_text(sub, x, ty + S(36), limit, S(18), theme_.text_dim);
  }
  if (search_->visible() && search_->size() == 0) {
    text_font(theme_.font_size);
    draw_text(_("Search photos"), search_->x() + S(3), search_->y(), search_->w(), search_->h(),
              theme_.text_dim);
  }
}

void GalleryWindow::draw() {
  if (viewer_open()) {
    draw_children();
    return;
  }
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    draw_header(p);
  }
  draw_children();
  draw_header_text();
  if (!toast_.empty()) {
    text_font(theme_.font_size);
    int tw = std::min(text_width(toast_) + S(40), w() - S(40));
    int th = S(40);
    int tx = (w() - tw) / 2, ty = h() - th - S(28);
    Color bg = theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.12) : Color::hex(0x1F2630);
    Color fg = theme_.dark ? theme_.text : Color::hex(0xF5F7FA);
    {
      Painter p(w(), h());
      p.shadow(tx, ty, tw, th, th / 2.0, theme_.shadow, S(8));
      p.fill_rounded(tx, ty, tw, th, th / 2.0, bg);
    }
    draw_text(toast_, tx + S(20), ty, tw - S(40), th, fg, 1);
  }
}

int GalleryWindow::handle(int event) {
  if (event == FL_SHORTCUT || event == FL_KEYBOARD) {
    int key = Fl::event_key();
    bool ctrl = Fl::event_state() & FL_CTRL;
    if (ctrl && key == 'w') {
      hide();
      return 1;
    }
    if (!viewer_open()) {
      if (ctrl && key == 'f') {
        focus_search();
        return 1;
      }
      if (Fl::focus() == search_ && (key == FL_Escape || key == FL_Down || key == FL_Enter ||
                                     key == FL_KP_Enter)) {
        if (key == FL_Escape) {
          if (search_->size() == 0) {
            Fl::focus(grid_);
          } else {
            search_->value("");
            rebuild_view();
          }
        } else {
          Fl::focus(grid_);
          if (grid_->focus_index() < 0 && !view_.empty()) grid_->set_focus_index(0);
        }
        redraw();
        return 1;
      }
    }
    if (key == FL_Escape) return 1;  // never close the window on Escape
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_FOCUS || event == FL_UNFOCUS || event == FL_PUSH) redraw();
  return r;
}

}  // namespace gallery
