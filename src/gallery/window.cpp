// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Gallery main window: library state, sources, header and actions.
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "gallery.h"
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

bool under(const std::string &path, const std::string &dir) {
  return path.size() > dir.size() + 1 && starts_with(path, dir) && path[dir.size()] == '/';
}

}  // namespace

GalleryWindow::GalleryWindow(int w, int h) : BufferedWindow(w, h, _("Gallery")) {
  instance_ = this;
  theme_ = active_theme();
  pictures_ = pictures_dir();
  extra_formats_ = extra_formats();
  reload_config();
  service_.reset(new ThumbnailService([this] { Fl::awake(awake_cb, this); },
                                      int(std::max(2u, std::thread::hardware_concurrency() / 2))));

  begin();
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
  Fl::add_handler(x_event_handler);
}

GalleryWindow::~GalleryWindow() {
  Fl::remove_handler(x_event_handler);
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
}

void GalleryWindow::save_settings() {
  Config cfg;
  cfg.load();
  cfg.set("gallery", "folders", encode_list(user_folders_));
  cfg.set("gallery", "favorites",
          encode_list(std::vector<std::string>(favorites_.begin(), favorites_.end())));
  cfg.set("gallery", "thumbnail_size", std::to_string(zoom_));
  cfg.set("gallery", "sort", sort_ == Sort::Oldest ? "oldest" : sort_ == Sort::Name ? "name" : "newest");
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
  color(theme_.background.fl());
  redraw();
}

int GalleryWindow::x_event_handler(int) {
  if (!instance_ || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    auto *self = instance_;
    std::set<std::string> old_favorites = self->favorites_;
    auto old_folders = self->user_folders_;
    self->reload_config();
    self->apply_theme();
    self->sort_ctl_->value(int(self->sort_));
    self->zoom_ctl_->value(self->zoom_);
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
}

bool GalleryWindow::searching() const { return search_ && search_->size() > 0; }

static bool in_view(const Photo &p, const View &v, const GalleryWindow &w,
                    const std::function<bool(const std::string &)> &fav, time_t recent) {
  switch (v.kind) {
    case ViewKind::All: return true;
    case ViewKind::Favorites: return fav(p.path);
    case ViewKind::Recent: return p.mtime >= recent;
    case ViewKind::Screenshots: return under(p.path, w.screenshots_dir());
    case ViewKind::Folder: return under(p.path, v.folder);
  }
  return true;
}

size_t GalleryWindow::count_for(const View &v) const {
  std::string key = std::to_string(int(v.kind)) + v.folder;
  auto cached = counts_.find(key);
  if (cached != counts_.end()) return cached->second;
  time_t recent = time(nullptr) - 30 * 24 * 3600;
  auto fav = [this](const std::string &p) { return is_favorite(p); };
  size_t n = 0;
  for (const auto &p : all_)
    if (in_view(p, v, *this, fav, recent)) ++n;
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
  }
  return "";
}

void GalleryWindow::rebuild_view(bool keep_scroll) {
  counts_.clear();
  time_t recent = time(nullptr) - 30 * 24 * 3600;
  auto fav = [this](const std::string &p) { return is_favorite(p); };
  std::string query = search_ ? search_->value() : "";
  std::string focus_path;
  if (grid_ && grid_->focus_index() >= 0 && grid_->focus_index() < int(view_.size()))
    focus_path = view_[size_t(grid_->focus_index())].path;
  view_.clear();
  for (const auto &p : all_)
    if (in_view(p, view_kind_, *this, fav, recent) && matches_search(p.name, query))
      view_.push_back(p);
  sort_photos(view_, sort_);
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
  redraw();
}

void GalleryWindow::set_view(const View &v) {
  view_kind_ = v;
  rebuild_view(false);
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
  std::string dir = chooser.filename();
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
  if (find_program("sde-view").empty() || !spawn_detached({"sde-view", path}))
    toast(_("Photo (sde-view) is not installed"));
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
  if (find_program("gio").empty()) {
    toast(_("Moving to the Trash needs the gio program (libglib2.0-bin)"));
    return;
  }
  std::string question =
      paths.size() == 1 ? format(_("Move “%s” to the Trash?"), basename_of(paths[0]).c_str())
                        : format(_("Move %zu photos to the Trash?"), paths.size());
  if (fl_choice("%s", _("Cancel"), _("Move to Trash"), nullptr, question.c_str()) != 1) return;
  std::vector<std::string> argv{"gio", "trash", "--"};
  argv.insert(argv.end(), paths.begin(), paths.end());
  bool started = run_async(argv, [this, paths](const std::string &, int status) {
    if (instance_ != this) return;
    size_t removed = 0;
    for (const auto &p : paths) {
      if (file_exists(p)) continue;
      ++removed;
      favorites_.erase(p);
      all_.erase(std::remove_if(all_.begin(), all_.end(), [&](const Photo &ph) { return ph.path == p; }),
                 all_.end());
    }
    if (removed) save_settings();
    rebuild_view(true);
    if (status != 0 && removed < paths.size()) toast(_("Some files could not be moved to the Trash"));
    else toast(removed == 1 ? _("Moved to the Trash") : format(_("%zu photos moved to the Trash"), removed));
  });
  if (!started) toast(_("Could not start gio"));
}

void GalleryWindow::open_viewer(int index) {
  if (index < 0 || index >= int(view_.size())) return;
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
  sidebar_->show();
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
  int sw = w() >= S(900) ? S(232) : S(68);
  sidebar_->resize(0, 0, sw, h());
  grid_->resize(sw, header_h_, w() - sw, h() - header_h_);
  if (viewer_open()) viewer_->resize(0, 0, w(), h());
  update_header_widgets();
}

void GalleryWindow::update_header_widgets() {
  if (!grid_ || viewer_open()) return;
  int sw = sidebar_->w();
  int right = w() - S(20), cy = header_h_ / 2;
  int avail = right - sw - S(24);
  bool show_zoom = avail > S(720), show_sort = avail > S(560);
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
  int search_w = std::max(S(140), std::min(S(260), avail - S(400)));
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
  int sw = sidebar_->w();
  p.fill_rect(sw, 0, w() - sw, header_h_, theme_.background);
  p.line(sw, header_h_ - 0.5, w(), header_h_ - 0.5, theme_.border.alpha(0.7));
  // Search pill.
  if (search_->visible()) {
  int sx = search_->x() - S(34), sy = search_->y() - S(3), sww = search_->w() + S(44), sh = S(32);
  p.fill_rounded(sx, sy, sww, sh, sh / 2.0, theme_.surface_alt);
  p.stroke_rounded(sx + 0.5, sy + 0.5, sww - 1, sh - 1, sh / 2.0,
                   Fl::focus() == search_ ? theme_.accent : theme_.border, Fl::focus() == search_ ? Sd(1.5) : 1);
  icon::draw(p, icon::Search, sx + S(10), sy + S(7), S(18), theme_.text_dim);
  }
  if (zoom_ctl_->visible()) {
    icon::draw(p, icon::Small, zoom_ctl_->x() - S(22), header_h_ / 2.0 - S(9), S(18), theme_.text_dim);
    icon::draw(p, icon::Large, zoom_ctl_->x() + zoom_ctl_->w() + S(4), header_h_ / 2.0 - S(10), S(20),
               theme_.text_dim);
  }
}

void GalleryWindow::draw_header_text() {
  int sw = sidebar_->w(), x = sw + S(24);
  size_t selected = grid_->selection().size();
  int limit = (search_->x() - S(44)) - x;
  if (sel_clear_->visible()) limit = (sel_fav_->visible() ? sel_fav_->x() : sel_clear_->x()) - x - S(10);
  if (selected) {
    text_font(theme_.font_size + S(5), true);
    draw_text(format(_("%zu selected"), selected), x, S(10), limit, S(28), theme_.text);
    text_font(theme_.font_size - S(1));
    draw_text(_("Press Esc to clear"), x, S(36), limit, S(18), theme_.text_dim);
  } else {
    text_font(theme_.font_size + S(5), true);
    draw_text(view_title(view_kind_), x, S(10), limit, S(28), theme_.text);
    text_font(theme_.font_size - S(1));
    std::string sub;
    if (scanning_ && all_.empty()) sub = _("Looking for photos…");
    else if (searching()) sub = format(view_.size() == 1 ? _("%zu result") : _("%zu results"), view_.size());
    else sub = format(view_.size() == 1 ? _("%zu photo") : _("%zu photos"), view_.size());
    if (scanning_ && !all_.empty()) sub += "  ·  " + std::string(_("Updating…"));
    draw_text(sub, x, S(36), limit, S(18), theme_.text_dim);
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
    if (ctrl && (key == 'q' || key == 'w')) {
      hide();
      return 1;
    }
    if (!viewer_open()) {
      if (ctrl && key == 'f') {
        focus_search();
        return 1;
      }
      if (ctrl && key == 'o') {
        add_folder();
        return 1;
      }
      if (key == FL_F + 5) {
        rescan();
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
