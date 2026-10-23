// SPDX-License-Identifier: GPL-3.0-or-later
// sde-desktop: wallpaper and desktop icons (Computer, Home, Trash and
// ~/Desktop).
//
// Refresh and config changes are cheap (alpha 50):
//  * ~/Desktop is watched with inotify and changed names are applied one by
//    one (no full rescan); Refresh (F5) rescans but reuses every entry whose
//    size and modification time did not change (no .desktop re-parsing);
//  * the wallpaper is composed on a worker thread, at most once per
//    (file, mtime, mode, screen size), and kept across Refresh and config
//    changes; large JPEGs are scaled down by libjpeg while decoding;
//  * icons are loaded a few at a time from an idle callback, so the first
//    paint never waits for dozens of SVG icons; the icon cache is kept
//    unless the icon theme changes;
//  * repaints draw only the icons inside the damaged area.
// SDE_PERF_STATS=1 prints how long a refresh takes.
//
// Drag and drop (alpha 52):
//  * icons are placed freely on a grid; positions are kept per file name in
//    ~/.config/sde/desktop-positions.ini (they follow renames) and new
//    files take the first free cell; Arrange Icons sorts them again;
//  * click, Ctrl+click and rubber-band selection; a selection is dragged as
//    a whole, onto another cell, a folder, the Trash or a launcher;
//  * icons dragged to other programs are offered as text/uri-list and drops
//    from them are accepted (xdnd.cpp); files are moved, copied or linked
//    on a worker thread (file_ops.cpp), never overwriting anything.
#include <FL/Fl.H>
#include "about.h"
#include "crash.h"
#include "buffered_window.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Progress.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "desktop_entry.h"
#include "desktop_model.h"
#include "file_ops.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "perf.h"
#include "spawn.h"
#include "thumbnails.h"
#include "theme.h"
#include "util.h"
#include "x11.h"
#include "xdnd.h"

using namespace sde;
using desk::DropAction;
using desk::TargetKind;

namespace {

struct Item {
  std::string label;
  std::string path;       // file or folder; empty for virtual items
  std::string uri;        // what to open
  std::vector<std::string> icons;
  bool is_launcher = false;
  int x = 0, y = 0;
  // For sorting.
  bool is_dir = false;
  long long size = 0;
  time_t modified = 0;
  // Change detection for incremental rescans.
  long long ctime_ns = 0;
  // Filled lazily (see Desktop::load_icons_idle); never freed (IconLoader).
  cairo_surface_t *icon = nullptr;
  bool icon_ready = false;
  std::string shown;  // ellipsized label for the current cell width
};

// Picks a name that does not exist yet: "New Folder", "New Folder 2", ...
std::string unique_path(const std::string &dir, const std::string &base, const std::string &ext = "") {
  std::string path = dir + "/" + base + ext;
  for (int n = 2; file_exists(path) || dir_exists(path); ++n)
    path = dir + "/" + base + " " + std::to_string(n) + ext;
  return path;
}

std::string icon_for_file(const std::string &path, bool is_dir) {
  if (is_dir) return "folder";
  size_t dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? std::string() : lower(path.substr(dot));
  static const std::vector<std::pair<std::vector<std::string>, std::string>> map = {
      {{".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".bmp"}, "image-x-generic"},
      {{".mp3", ".ogg", ".flac", ".wav", ".m4a", ".opus"}, "audio-x-generic"},
      {{".mp4", ".mkv", ".webm", ".avi", ".mov"}, "video-x-generic"},
      {{".pdf"}, "application-pdf"},
      {{".odt", ".doc", ".docx", ".rtf"}, "x-office-document"},
      {{".ods", ".xls", ".xlsx", ".csv"}, "x-office-spreadsheet"},
      {{".odp", ".ppt", ".pptx"}, "x-office-presentation"},
      {{".zip", ".tar", ".gz", ".xz", ".bz2", ".7z", ".zst"}, "package-x-generic"},
      {{".deb"}, "application-x-deb"},
      {{".html", ".htm"}, "text-html"},
      {{".sh", ".py"}, "text-x-script"},
  };
  for (auto &m : map)
    if (std::find(m.first.begin(), m.first.end(), ext) != m.first.end()) return m.second;
  return "text-x-generic";
}

// Builds the entry for ~/Desktop/<name>. One stat(); .desktop files are
// parsed only here, so unchanged launchers are never read again.
bool make_file_item(const std::string &dir, const std::string &name, Item &it) {
  std::string full = dir + "/" + name;
  struct stat st;
  if (stat(full.c_str(), &st) != 0) return false;
  it = Item();
  it.label = name;
  it.path = full;
  it.uri = full;
  it.is_dir = S_ISDIR(st.st_mode);
  it.size = st.st_size;
  it.modified = st.st_mtime;
  it.ctime_ns = (long long)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
  it.icons = {icon_for_file(full, it.is_dir)};
  if (!it.is_dir && ends_with(name, ".desktop")) {
    DesktopEntry e;
    if (e.load(full) && !e.name.empty()) {
      it.label = e.name;
      it.icons = {e.icon, "application-x-executable"};
      it.is_launcher = true;
    }
  }
  return true;
}

// What a composed wallpaper depends on; it is recomposed only when this changes.
struct WallpaperKey {
  std::string path, mode;
  long long mtime_ns = 0, size = 0;
  int w = 0, h = 0;
  double r = 0, g = 0, b = 0;
  bool operator==(const WallpaperKey &o) const {
    return path == o.path && mode == o.mode && mtime_ns == o.mtime_ns && size == o.size && w == o.w &&
           h == o.h && r == o.r && g == o.g && b == o.b;
  }
  bool operator!=(const WallpaperKey &o) const { return !(*this == o); }
};

// Formats that the shared loader (SVG, PNG, JPEG, GIF, BMP) cannot read
// but the Gallery decoder can (WebP, TIFF, AVIF ... through gdk-pixbuf).
bool is_svg(const std::string &path) {
  std::string l = lower(path);
  return ends_with(l, ".svg") || ends_with(l, ".svgz");
}

// Runs on a worker thread: only cairo image surfaces, rsvg and the
// (thread-safe) Gallery decoder; no FLTK or X calls.
cairo_surface_t *compose_wallpaper(const WallpaperKey &k) {
  int w = k.w, h = k.h;
  const std::string &mode = k.mode;
  if (is_svg(k.path)) return load_wallpaper(k.path, w, h, mode, k.r, k.g, k.b);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cairo_t *cr = cairo_create(out);
  cairo_set_source_rgb(cr, k.r, k.g, k.b);
  cairo_paint(cr);
  // Tile and center show pixels 1:1; the other modes never need more than
  // about twice the screen size, so libjpeg can scale while decoding.
  bool natural = mode == "tile" || mode == "center";
  int max_dim = natural ? 0 : 2 * std::max(w, h);
  int ow = 0, oh = 0;
  cairo_surface_t *img = gallery::decode_image(k.path, max_dim, &ow, &oh);
  if (img && !natural && mode != "stretch") {
    // A very wide or tall picture in "zoom" may need its short side at full
    // size: decode again without the limit if the reduced copy is too small.
    double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    double need = mode == "fit" ? std::fmin(w / iw, h / ih) : std::fmax(w / iw, h / ih);
    if (need > 1.0 && (ow > iw || oh > ih)) {
      cairo_surface_destroy(img);
      img = gallery::decode_image(k.path, 0);
    }
  }
  if (img) {
    double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    if (mode == "tile") {
      cairo_pattern_t *pat = cairo_pattern_create_for_surface(img);
      cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
      cairo_set_source(cr, pat);
      cairo_paint(cr);
      cairo_pattern_destroy(pat);
    } else {
      double sx = w / iw, sy = h / ih;
      if (mode == "fit") sx = sy = std::fmin(sx, sy);
      else if (mode == "center") sx = sy = 1;
      else if (mode != "stretch") sx = sy = std::fmax(sx, sy);  // zoom
      cairo_translate(cr, (w - iw * sx) / 2, (h - ih * sy) / 2);
      cairo_scale(cr, sx, sy);
      cairo_set_source_surface(cr, img, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
      cairo_paint(cr);
    }
    cairo_surface_destroy(img);
  }
  cairo_destroy(cr);
  cairo_surface_flush(out);
  return out;
}

// Wallpapers composed off the main thread. Results come back through a
// pipe watched with Fl::add_fd, so no FLTK locking is needed.
class WallpaperWorker {
 public:
  using Done = void (*)(void *);
  WallpaperWorker(Done done, void *data) : done_(done), data_(data) {
    if (pipe2(fds_, O_CLOEXEC | O_NONBLOCK) == 0) Fl::add_fd(fds_[0], FL_READ, on_pipe, this);
  }
  ~WallpaperWorker() {
    // A worker still running owns |job| (shared_ptr) and writes to a pipe
    // that stays open until exit, so nothing dangles.
    if (fds_[0] >= 0) Fl::remove_fd(fds_[0]);
  }

  // Starts composing |keys| (one per screen) unless that is already running.
  void request(const std::vector<WallpaperKey> &keys) {
    if (pending_ && pending_keys_ == keys) return;
    pending_keys_ = keys;
    pending_ = true;
    auto job = std::make_shared<Job>();
    job->keys = keys;
    job->serial = ++serial_;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current_ = job;
    }
    int fd = fds_[1];
    std::thread([job, fd] {
      for (const auto &k : job->keys) job->surfaces.push_back(compose_wallpaper(k));
      job->done = true;
      char b = 1;
      if (fd >= 0) (void)!write(fd, &b, 1);
    }).detach();
  }

  // Takes the finished result for the newest request, or returns false.
  bool take(std::vector<WallpaperKey> &keys, std::vector<cairo_surface_t *> &surfaces) {
    std::shared_ptr<Job> job;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job = current_;
    }
    if (!job || job->serial != serial_ || !job->done) return false;
    keys = job->keys;
    surfaces = job->surfaces;
    job->surfaces.clear();
    pending_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    current_.reset();
    return true;
  }

  bool pending() const { return pending_; }

 private:
  struct Job {
    std::vector<WallpaperKey> keys;
    std::vector<cairo_surface_t *> surfaces;
    unsigned long serial = 0;
    std::atomic<bool> done{false};
    ~Job() {
      for (auto *s : surfaces) cairo_surface_destroy(s);  // superseded results
    }
  };
  static void on_pipe(int fd, void *data) {
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0) {
    }
    auto *self = static_cast<WallpaperWorker *>(data);
    self->done_(self->data_);
  }
  Done done_;
  void *data_;
  int fds_[2] = {-1, -1};
  std::mutex mutex_;
  std::shared_ptr<Job> current_;
  std::vector<WallpaperKey> pending_keys_;
  unsigned long serial_ = 0;
  bool pending_ = false;
};

// File operations from drops run on a worker thread; a small progress
// window appears when they take longer than half a second.
class FileJob {
 public:
  FileJob(std::vector<desk::FileOp> ops, std::function<void()> done) : ops_(std::move(ops)), done_(std::move(done)) {
    auto progress = progress_;
    auto ops_copy = ops_;
    auto finished = finished_;
    auto errors = errors_;
    std::thread([progress, ops_copy, finished, errors] {
      std::vector<std::string> errs;
      desk::run_file_ops(ops_copy, progress.get(), &errs);
      {
        std::lock_guard<std::mutex> lock(progress->mutex);
        *errors = errs;
      }
      *finished = true;
    }).detach();
    started_ = perf::now_ms();
    Fl::add_timeout(0.1, poll, this);
  }
  ~FileJob() {
    Fl::remove_timeout(poll, this);
    delete window_;
  }

 private:
  static void poll(void *data) {
    auto *job = static_cast<FileJob *>(data);
    if (*job->finished_) {
      std::vector<std::string> errors;
      {
        std::lock_guard<std::mutex> lock(job->progress_->mutex);
        errors = *job->errors_;
      }
      if (job->window_) job->window_->hide();
      bool cancelled = job->progress_->cancel;
      auto done = job->done_;
      std::string text;
      for (size_t i = 0; i < errors.size() && i < 5; ++i) text += errors[i] + "\n";
      if (errors.size() > 5) text += format(_("... and %d more"), int(errors.size() - 5));
      delete job;  // before any dialog: nothing refers to it any more
      if (done) done();
      if (!text.empty() && !(cancelled && text.find("Cancelled") != std::string::npos))
        fl_alert("%s\n\n%s", _("Some items could not be copied or moved:"), text.c_str());
      return;
    }
    if (!job->window_ && perf::now_ms() - job->started_ > 500) job->show_window();
    if (job->window_) job->update_window();
    Fl::repeat_timeout(0.1, poll, data);
  }

  void show_window() {
    int W = S(380), H = S(118);
    window_ = new Fl_Double_Window(W, H, _("Copying Files"));
    label_ = new Fl_Box(S(16), S(12), W - S(32), S(40));
    label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP | FL_ALIGN_WRAP);
    bar_ = new Fl_Progress(S(16), S(54), W - S(32), S(16));
    bar_->minimum(0);
    bar_->maximum(1);
    bar_->selection_color(FL_SELECTION_COLOR);
    auto *cancel = new Fl_Button(W - S(112), H - S(40), S(96), S(30), _("Cancel"));
    cancel->callback([](Fl_Widget *, void *d) { static_cast<FileJob *>(d)->progress_->cancel = true; }, this);
    window_->end();
    window_->set_non_modal();
    window_->show();
  }

  void update_window() {
    long long total = progress_->total, done = progress_->done;
    std::string current;
    {
      std::lock_guard<std::mutex> lock(progress_->mutex);
      current = progress_->current;
    }
    label_text_ = current.empty() ? std::string(_("Preparing...")) : basename_of(current);
    label_->label(label_text_.c_str());
    bar_->value(total > 0 ? float(double(done) / double(total)) : 0.f);
    bar_text_ = total > 0 ? format("%d %%", int(100.0 * double(done) / double(total))) : std::string();
    bar_->label(bar_text_.c_str());
  }

  std::vector<desk::FileOp> ops_;
  std::function<void()> done_;
  std::shared_ptr<desk::FileProgress> progress_ = std::make_shared<desk::FileProgress>();
  std::shared_ptr<std::atomic<bool>> finished_ = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::vector<std::string>> errors_ = std::make_shared<std::vector<std::string>>();
  double started_ = 0;
  Fl_Double_Window *window_ = nullptr;
  Fl_Box *label_ = nullptr;
  Fl_Progress *bar_ = nullptr;
  std::string label_text_, bar_text_;
};

class Desktop : public sde::BufferedWindow {
 public:
  Desktop() : sde::BufferedWindow(0, 0, 100, 100, "SDE Desktop"), worker_(on_wallpaper_ready, this) {
    end();
    border(0);
    positions_.load(desk::default_positions_path());
    load();
  }

  ~Desktop() override {
    for (auto *s : wallpapers_) cairo_surface_destroy(s);
    Fl::remove_idle(load_icons_idle, this);
    Fl::remove_timeout(apply_inotify, this);
    if (inotify_fd_ >= 0) {
      Fl::remove_fd(inotify_fd_);
      close(inotify_fd_);
    }
  }

  void setup() {
    Window xid = fl_xid(this);
    x11::set_window_type(xid, "_NET_WM_WINDOW_TYPE_DESKTOP");
    x11::set_atoms(xid, "_NET_WM_STATE",
                   {"_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER",
                    "_NET_WM_STATE_BELOW"});
    x11::set_cardinals(xid, "_NET_WM_DESKTOP", {long(0xFFFFFFFF)});
    XClassHint hint{const_cast<char *>("sde-desktop"), const_cast<char *>("SDE-Desktop")};
    XSetClassHint(fl_display, xid, &hint);
    x11::remap(xid);
    XMoveResizeWindow(fl_display, xid, 0, 0, unsigned(w()), unsigned(h()));

    Window root = RootWindow(fl_display, fl_screen);
    XWindowAttributes attr;
    XGetWindowAttributes(fl_display, root, &attr);
    XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask | StructureNotifyMask);
    watch_desktop_dir();
    setup_dnd(xid);
  }

  // Config changed (or startup): theme, icon size, places and sort order may
  // differ. The wallpaper and icon caches survive unless what they depend
  // on changed.
  void load() {
    double t0 = perf::enabled() ? perf::now_ms() : 0;
    cfg_.load();
    theme_ = Theme::from_config(cfg_);
    theme_.apply_to_fltk();
    std::string old_icon_theme = IconLoader::theme();
    IconLoader::set_theme(cfg_.get("appearance", "icon_theme", "auto"));  // clears on change
    if (IconLoader::theme() != old_icon_theme) forget_icons();
    int sw = DisplayWidth(fl_display, fl_screen), sh = DisplayHeight(fl_display, fl_screen);
    sde::BufferedWindow::resize(0, 0, sw, sh);
    std::string dir = cfg_.get_bool("desktop", "show_icons", true) ? user_dir("DESKTOP") : std::string();
    if (dir == home_dir()) dir.clear();
    if (dir != desktop_dir_) {
      desktop_dir_ = dir;
      files_.clear();
      rescan_files();
    }
    rebuild_items();
    config_dirty_ = true;  // wallpaper settings or the theme color may differ
    redraw();
    perf_done("load", t0);
  }

  // Right-click > Refresh and F5: picks up files changed behind inotify's
  // back (e.g. on network mounts) and a wallpaper file edited in place.
  void refresh() {
    double t0 = perf::enabled() ? perf::now_ms() : 0;
    rescan_files();
    rebuild_items();
    wallpaper_checked_ = false;  // re-stat the wallpaper file at the next draw
    redraw();
    perf_done("refresh", t0);
  }

  void perf_done(const char *what, double t0) {
    if (!perf::enabled()) return;
    fprintf(stderr, "sde-perf: sde-desktop %s_ms=%.1f items=%zu\n", what, perf::now_ms() - t0, items_.size());
    draw_t0_ = perf::now_ms();
  }

  // Full rescan of ~/Desktop that keeps every unchanged entry.
  void rescan_files() {
    std::map<std::string, Item> next;
    if (!desktop_dir_.empty()) {
      for (auto &name : list_dir(desktop_dir_)) {
        if (name.empty() || name[0] == '.') continue;
        update_file(name, next);
      }
    }
    files_.swap(next);
  }

  // Re-reads ~/Desktop/<name> into |into| (removes it when gone). The old
  // entry is reused when its size, mtime and ctime are unchanged.
  void update_file(const std::string &name, std::map<std::string, Item> &into) {
    std::string full = desktop_dir_ + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) {
      into.erase(name);
      return;
    }
    long long ctime_ns = (long long)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
    auto old = files_.find(name);
    if (old != files_.end() && old->second.ctime_ns == ctime_ns && old->second.size == st.st_size &&
        old->second.modified == st.st_mtime) {
      if (&into != &files_) into[name] = old->second;
      return;
    }
    Item it;
    if (make_file_item(desktop_dir_, name, it)) into[name] = it;
    else into.erase(name);
  }

  // Places first, then the files in the chosen order; keeps the selection.
  void rebuild_items() {
    std::string sel = selected_ >= 0 && selected_ < int(items_.size())
                          ? items_[size_t(selected_)].uri : std::string();
    // Indexes change: a drag in progress ends (a rubber band goes on, it
    // works with uris).
    if (drag_state_ == DRAG_ITEMS || drag_state_ == DRAG_PRESSED) cancel_drag();
    // Places keep their loaded icons across rebuilds.
    std::map<std::string, Item> places;
    for (auto &it : items_)
      if (it.path.empty() || it.path == home_dir()) places[it.uri] = it;
    auto place = [&](const std::string &label, const std::string &path, const std::string &uri,
                     std::vector<std::string> icons) {
      auto p = places.find(uri);
      if (p != places.end() && p->second.label == label) {
        items_.push_back(p->second);
        return;
      }
      Item it;
      it.label = label;
      it.path = path;
      it.uri = uri;
      it.icons = std::move(icons);
      items_.push_back(it);
    };
    items_.clear();
    std::string home = home_dir();
    if (cfg_.get_bool("desktop", "show_computer", false))
      place(_("Computer"), "", "computer:///",
            {"computer", "user-desktop", "drive-harddisk", "drive-harddisk-system"});
    if (cfg_.get_bool("desktop", "show_home", true))
      place(_("Home"), home, home, {"user-home", "folder-home", "folder"});
    if (cfg_.get_bool("desktop", "show_trash", false))
      place(_("Trash"), "", "trash:///", {"user-trash", "user-trash-full"});
    for (auto &kv : files_) items_.push_back(kv.second);
    sort_items();
    selected_ = -1;
    hover_ = -1;
    std::set<std::string> still;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (!sel.empty() && items_[i].uri == sel) selected_ = int(i);
      if (sel_.count(items_[i].uri)) still.insert(items_[i].uri);
    }
    sel_.swap(still);
    layout_items();
    schedule_icons();
  }

  // The loaded icon of each file entry is stored back into files_ so that
  // the next rebuild does not load it again.
  void remember_icon(const Item &it) {
    if (it.path.empty() || it.path == home_dir()) return;
    auto f = files_.find(basename_of(it.path));
    if (f != files_.end() && f->second.path == it.path) {
      f->second.icon = it.icon;
      f->second.icon_ready = it.icon_ready;
    }
  }

  void forget_icons() {
    for (auto &it : items_) it.icon = nullptr, it.icon_ready = false;
    for (auto &kv : files_) kv.second.icon = nullptr, kv.second.icon_ready = false;
  }

  void schedule_icons() {
    for (auto &it : items_)
      if (!it.icon_ready) {
        if (!Fl::has_idle(load_icons_idle, this)) Fl::add_idle(load_icons_idle, this);
        return;
      }
  }

  // Loads icons for a few milliseconds, repaints those cells, and comes back
  // at the next idle moment until every icon is loaded.
  static void load_icons_idle(void *data) {
    auto *d = static_cast<Desktop *>(data);
    double t0 = perf::now_ms();
    bool more = false;
    for (size_t i = 0; i < d->items_.size(); ++i) {
      Item &it = d->items_[i];
      if (it.icon_ready) continue;
      if (perf::now_ms() - t0 > 8) {
        more = true;
        break;
      }
      it.icon = IconLoader::get(it.icons, d->icon_);
      it.icon_ready = true;
      d->remember_icon(it);
      d->damage_item(int(i));
    }
    if (!more) Fl::remove_idle(load_icons_idle, data);
  }

  // Computer, Home and Trash stay first; files follow the "Sort by" choice, with
  // folders before files as in most file managers.
  void sort_items() {
    size_t first = size_t(first_file_index());
    if (first >= items_.size()) return;
    std::string key = cfg_.get("desktop", "sort", "name");
    auto ext = [](const Item &i) {
      size_t dot = i.path.find_last_of('.');
      return dot == std::string::npos ? std::string() : lower(i.path.substr(dot));
    };
    std::stable_sort(items_.begin() + long(first), items_.end(), [&](const Item &a, const Item &b) {
      if (a.is_dir != b.is_dir) return a.is_dir;
      if (key == "size" && a.size != b.size) return a.size > b.size;
      if (key == "modified" && a.modified != b.modified) return a.modified > b.modified;
      if (key == "type" && ext(a) != ext(b)) return ext(a) < ext(b);
      return lower(a.label) < lower(b.label);
    });
  }

  void layout_items() {
    int mx, my, mw, mh;
    Fl::screen_xywh(mx, my, mw, mh, 0);
    std::string size = cfg_.get("desktop", "icon_size", "medium");
    int icon = size == "small" ? S(32) : size == "large" ? S(64) : S(48);
    if (icon != icon_) forget_icons();
    icon_ = icon;
    cell_w_ = icon_ + S(48);
    cell_h_ = icon_ + S(48);
    int usable = mh - theme_.panel_height - S(24);
    rows_ = std::max(1, usable / cell_h_);
    cols_ = std::max(1, (mw - S(24)) / cell_w_);
    grid_x_ = mx + S(12);
    grid_y_ = my + S(12) + (theme_.panel_top ? theme_.panel_height : 0);
    std::vector<std::string> keys;
    for (auto &it : items_) keys.push_back(item_key(it));
    std::vector<desk::Cell> cells = desk::place_icons(keys, cols_, rows_, positions_);
    text_font(theme_.font_size);
    for (size_t i = 0; i < items_.size(); ++i) {
      items_[i].x = grid_x_ + cells[i].col * cell_w_;
      items_[i].y = grid_y_ + cells[i].row * cell_h_;
      items_[i].shown = ellipsize(items_[i].label, cell_w_ - S(12));
    }
    // Tests read where the icons are (tests/test_desktop_dnd.py).
    if (const char *dump = getenv("SDE_DESKTOP_LAYOUT_FILE")) {
      std::string out;
      for (auto &it : items_)
        out += item_key(it) + "\t" + std::to_string(it.x) + "\t" + std::to_string(it.y) + "\t" +
               std::to_string(cell_w_) + "\t" + std::to_string(cell_h_) + "\n";
      write_file_atomic(dump, out);
    }
  }

  // The key of an item in the position store: the file name, or "@home",
  // "@trash", "@computer" for the places.
  std::string item_key(const Item &it) const {
    if (it.uri == "computer:///") return "@computer";
    if (it.uri == "trash:///") return "@trash";
    if (!is_file(it)) return "@home";
    return basename_of(it.path);
  }

  desk::Cell cell_of(const Item &it) const {
    return desk::Cell{std::max(0, (it.x - grid_x_ + cell_w_ / 2) / cell_w_),
                      std::max(0, (it.y - grid_y_ + cell_h_ / 2) / cell_h_)};
  }

  // The grid cell under a point (clamped to the grid).
  desk::Cell cell_at(int x, int y) const {
    int c = (x - grid_x_) / std::max(1, cell_w_), r = (y - grid_y_) / std::max(1, cell_h_);
    return desk::Cell{std::max(0, std::min(cols_ - 1, c)), std::max(0, std::min(rows_ - 1, r))};
  }

  void save_positions() {
    if (!positions_.save()) fprintf(stderr, "sde-desktop: cannot write %s\n", positions_.path().c_str());
  }

  // Remembers where every icon is now, so that new files never push the
  // others around (called before the first manual move).
  void pin_all_positions() {
    for (auto &it : items_) positions_.set(item_key(it), cell_of(it));
  }

  void watch_desktop_dir() {
    if (inotify_fd_ < 0) {
      inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      if (inotify_fd_ < 0) return;
      Fl::add_fd(inotify_fd_, FL_READ, on_inotify, this);
    }
    if (watch_ >= 0) inotify_rm_watch(inotify_fd_, watch_);
    watch_ = -1;
    if (!desktop_dir_.empty() && dir_exists(desktop_dir_))
      watch_ = inotify_add_watch(inotify_fd_, desktop_dir_.c_str(),
                                 IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                                     IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
    watched_dir_ = desktop_dir_;
  }

  // Collects the changed names; they are applied together a moment later so
  // that a burst of events (e.g. a file copy) costs one relayout.
  static void on_inotify(int fd, void *data) {
    auto *d = static_cast<Desktop *>(data);
    alignas(struct inotify_event) char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (char *p = buf; p < buf + n;) {
        auto *ev = reinterpret_cast<struct inotify_event *>(p);
        if (ev->mask & (IN_Q_OVERFLOW | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) d->full_rescan_ = true;
        else if (ev->len && ev->name[0] && ev->name[0] != '.') d->changed_.insert(ev->name);
        // Renames keep the icon where it was.
        if (ev->len && (ev->mask & IN_MOVED_FROM)) d->moved_from_[ev->cookie] = ev->name;
        if (ev->len && (ev->mask & IN_MOVED_TO)) {
          auto from = d->moved_from_.find(ev->cookie);
          if (from != d->moved_from_.end()) {
            if (d->positions_.get(from->second, nullptr)) {
              d->positions_.rename(from->second, ev->name);
              d->positions_dirty_ = true;
            }
            d->moved_from_.erase(from);
          }
        }
        p += sizeof(struct inotify_event) + ev->len;
      }
    }
    Fl::remove_timeout(apply_inotify, data);
    Fl::add_timeout(0.2, apply_inotify, data);
  }

  static void apply_inotify(void *data) {
    auto *d = static_cast<Desktop *>(data);
    if (d->drag_state_ == DRAG_ITEMS || d->drag_state_ == DRAG_PRESSED) {
      Fl::repeat_timeout(0.3, apply_inotify, data);  // after the drag
      return;
    }
    double t0 = perf::enabled() ? perf::now_ms() : 0;
    size_t changes = d->changed_.size();
    if (d->full_rescan_) {
      d->full_rescan_ = false;
      d->rescan_files();
      d->watch_desktop_dir();
    } else {
      for (const auto &name : d->changed_) d->update_file(name, d->files_);
    }
    d->changed_.clear();
    d->moved_from_.clear();
    if (d->positions_dirty_) {
      d->positions_dirty_ = false;
      d->save_positions();
    }
    d->rebuild_items();
    d->redraw();  // positions may shift, so the icon area is repainted
    if (perf::enabled())
      fprintf(stderr, "sde-perf: sde-desktop inotify_ms=%.1f changes=%zu\n", perf::now_ms() - t0, changes);
  }

  // ------------------------------------------------------------ wallpaper

  std::string wallpaper_path() const {
    std::string wp = cfg_.get("appearance", "wallpaper", "auto");
    if (wp.empty() || wp == "auto" || !file_exists(wp)) {
      std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
      std::string name = theme_.dark ? "selviana-dark.svg" : "selviana-light.svg";
      wp = base + "/backgrounds/sde/" + name;
      if (!file_exists(wp) && file_exists("/usr/share/backgrounds/sde/" + name))
        wp = "/usr/share/backgrounds/sde/" + name;
    }
    return wp;
  }

  std::vector<WallpaperKey> wallpaper_keys() const {
    std::vector<WallpaperKey> keys;
    std::string wp = wallpaper_path();
    std::string mode = cfg_.get("appearance", "wallpaper_mode", "zoom");
    struct stat st;
    long long mtime = 0, size = 0;
    if (stat(wp.c_str(), &st) == 0) {
      mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
      size = st.st_size;
    }
    Color bg = theme_.dark ? Color::hex(0x10161D) : Color::hex(0x1C6F8A);
    for (int i = 0; i < Fl::screen_count(); ++i) {
      int sx, sy, sw, sh;
      Fl::screen_xywh(sx, sy, sw, sh, i);
      WallpaperKey k;
      k.path = wp;
      k.mode = mode;
      k.mtime_ns = mtime;
      k.size = size;
      k.w = sw;
      k.h = sh;
      k.r = bg.r;
      k.g = bg.g;
      k.b = bg.b;
      keys.push_back(k);
    }
    return keys;
  }

  static void on_wallpaper_ready(void *data) {
    auto *d = static_cast<Desktop *>(data);
    std::vector<WallpaperKey> keys;
    std::vector<cairo_surface_t *> surfaces;
    if (!d->worker_.take(keys, surfaces)) return;
    for (auto *s : d->wallpapers_) cairo_surface_destroy(s);
    d->wallpapers_ = surfaces;
    d->wallpaper_keys_ = keys;
    d->redraw();
    if (perf::enabled() && d->wallpaper_t0_ > 0)
      fprintf(stderr, "sde-perf: sde-desktop wallpaper_async_ms=%.1f\n", perf::now_ms() - d->wallpaper_t0_);
    d->wallpaper_t0_ = 0;
  }

  // Asks the worker for a new wallpaper when its inputs changed. Until it is
  // ready the previous wallpaper (or the plain color) stays on screen.
  void check_wallpaper() {
    if (wallpaper_checked_ && !config_dirty_) return;
    wallpaper_checked_ = true;
    config_dirty_ = false;
    std::vector<WallpaperKey> keys = wallpaper_keys();
    if (keys == wallpaper_keys_ && wallpapers_.size() == keys.size()) return;
    if (perf::enabled()) wallpaper_t0_ = perf::now_ms();
    worker_.request(keys);
  }

  void draw() override {
    check_wallpaper();
    int n = Fl::screen_count();
    {
      Painter p(w(), h());
      for (int i = 0; i < n; ++i) {
        int sx, sy, sw, sh;
        Fl::screen_xywh(sx, sy, sw, sh, i);
        if (size_t(i) < wallpapers_.size() && wallpapers_[size_t(i)] &&
            cairo_image_surface_get_width(wallpapers_[size_t(i)]) == sw &&
            cairo_image_surface_get_height(wallpapers_[size_t(i)]) == sh) {
          // Uploaded to the X server once, not on every repaint.
          p.surface(wallpapers_[size_t(i)], sx, sy);
        } else {
          p.fill_rect(sx, sy, sw, sh, theme_.dark ? Color::hex(0x10161D) : Color::hex(0x1C6F8A));
        }
      }
      for (size_t i = 0; i < items_.size(); ++i) {
        const Item &it = items_[i];
        if (!fl_not_clipped(it.x, it.y, cell_w_, cell_h_)) continue;
        bool dragged = drag_state_ == DRAG_ITEMS && is_selected(int(i));
        if (int(i) == drop_target_) {
          p.fill_rounded(it.x + S(4), it.y + S(2), cell_w_ - S(8), cell_h_ - S(6), S(10), theme_.accent.alpha(0.35));
          p.stroke_rounded(it.x + S(4), it.y + S(2), cell_w_ - S(8), cell_h_ - S(6), S(10), theme_.accent, Sd(2));
        } else if (is_selected(int(i))) {
          p.fill_rounded(it.x + S(4), it.y + S(2), cell_w_ - S(8), cell_h_ - S(6), S(10), theme_.accent.alpha(0.45));
        } else if (int(i) == hover_) {
          p.fill_rounded(it.x + 4, it.y + 2, cell_w_ - 8, cell_h_ - 6, 10, Color::hex(0xFFFFFF, 0.16));
        }
        if (int(i) == selected_ && Fl::focus() == this && sel_.size() > 1)
          p.stroke_rounded(it.x + S(4), it.y + S(2), cell_w_ - S(8), cell_h_ - S(6), S(10), Color::hex(0xFFFFFF, 0.7), 1);
        if (it.icon_ready && it.icon) {
          if (dragged) cairo_push_group(p.cr());
          p.image(it.icon, it.x + (cell_w_ - icon_) / 2.0, it.y + S(8), icon_);
          if (dragged) {
            cairo_pop_group_to_source(p.cr());
            cairo_paint_with_alpha(p.cr(), 0.5);
          }
        }
      }
      // Where a rearranged icon will land.
      if (drag_state_ == DRAG_ITEMS && drag_local_ && drop_target_ < 0 && !positions_.auto_arrange) {
        int gx = grid_x_ + drag_cell_.col * cell_w_, gy = grid_y_ + drag_cell_.row * cell_h_;
        p.stroke_rounded(gx + S(4), gy + S(2), cell_w_ - S(8), cell_h_ - S(6), S(10), Color::hex(0xFFFFFF, 0.8), Sd(1.5));
      }
      if (drag_state_ == DRAG_BAND) {
        int bx = std::min(band_x0_, band_x1_), by = std::min(band_y0_, band_y1_);
        int bw = std::abs(band_x1_ - band_x0_), bh = std::abs(band_y1_ - band_y0_);
        p.fill_rect(bx, by, bw, bh, theme_.accent.alpha(0.22));
        p.stroke_rounded(bx + 0.5, by + 0.5, std::max(1, bw - 1), std::max(1, bh - 1), 0, theme_.accent, 1);
      }
    }
    text_font(theme_.font_size);
    for (auto &it : items_) {
      if (!fl_not_clipped(it.x, it.y, cell_w_, cell_h_)) continue;
      draw_text(it.shown, it.x + S(6) + 1, it.y + icon_ + S(12) + 1, cell_w_ - S(12), S(22), Color::hex(0x000000, 1), 1);
      draw_text(it.shown, it.x + S(6), it.y + icon_ + S(12), cell_w_ - S(12), S(22), Color::hex(0xFFFFFF), 1);
    }
    if (draw_t0_ > 0) {
      fprintf(stderr, "sde-perf: sde-desktop painted_after_ms=%.1f\n", perf::now_ms() - draw_t0_);
      draw_t0_ = 0;
    }
  }

  // Repaints one icon cell only (hover and selection changes).
  void damage_item(int i) {
    if (i >= 0 && i < int(items_.size()))
      damage_rect(items_[size_t(i)].x, items_[size_t(i)].y, cell_w_, cell_h_);
  }

  int item_at(int x, int y) const {
    for (size_t i = 0; i < items_.size(); ++i)
      if (x >= items_[i].x + S(4) && x < items_[i].x + cell_w_ - S(4) && y >= items_[i].y &&
          y < items_[i].y + cell_h_ - S(4))
        return int(i);
    return -1;
  }

  // Item actions take a copy of the entry: menus and dialogs run nested
  // event loops, during which inotify or a config change may rebuild items_
  // (a reference would dangle and an index could name another file).
  void open_item(int i) {
    if (i >= 0 && i < int(items_.size())) open_item(items_[size_t(i)]);
  }

  void open_item(const Item it) {
    if (it.is_launcher) {
      // Launchers copied to the desktop must be marked executable (trusted)
      // before they run, like in other desktops.
      if (access(it.path.c_str(), X_OK) != 0) {
        if (fl_choice(_("\"%s\" is not marked as trusted.\nDo you want to trust and launch it?"),
                      _("Cancel"), _("Trust and Launch"), nullptr, it.label.c_str()) != 1)
          return;
        struct stat st;
        if (stat(it.path.c_str(), &st) == 0) chmod(it.path.c_str(), st.st_mode | S_IXUSR);
      }
      DesktopEntry e;
      if (e.load(it.path)) launch_entry(e);
      return;
    }
    if (!open_place(it.uri)) fl_alert(_("No application is available to open \"%s\"."), it.label.c_str());
  }

  // computer:/// and trash:/// are Liria (the SDE file manager) places;
  // other file managers get them through xdg-open/gio.
  static bool open_place(const std::string &uri) {
    bool virtual_place = starts_with(uri, "computer:") || starts_with(uri, "trash:");
    if (virtual_place && !find_program("liria").empty()) return spawn_detached({"liria", uri});
    return open_uri(uri);
  }

  // Hides Computer, Home or Trash (the switches in Settings > Desktop).
  void hide_place(const Item &it) {
    const char *key = it.uri == "computer:///" ? "show_computer"
                      : it.uri == "trash:///"  ? "show_trash"
                                               : "show_home";
    set_option(key, "false");
  }

  bool is_file_item(int i) const { return i >= first_file_index() && i < int(items_.size()); }
  static bool is_file(const Item &it) { return !it.path.empty() && it.path != home_dir(); }

  // A new name for a file in |dir|: not empty, no "/", not "." or "..".
  static bool valid_name(const char *name) {
    return name && *name && !strchr(name, '/') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
  }

  void rename_item(int i) {
    if (is_file_item(i)) rename_item(items_[size_t(i)]);
  }

  void rename_item(const Item it) {
    if (!is_file(it)) return;
    std::string old_name = basename_of(it.path);
    std::string dir = it.path.substr(0, it.path.size() - old_name.size() - 1);
    const char *input = fl_input("%s", old_name.c_str(), _("New name:"));
    if (!input) return;
    std::string name = input;  // fl_input's buffer is reused by the next dialog
    if (name.empty() || name == old_name) return;
    if (!valid_name(name.c_str())) {
      fl_alert("%s", _("A name cannot contain \"/\"."));
      return;
    }
    std::string target = dir + "/" + name;
    struct stat st;
    if (lstat(target.c_str(), &st) == 0) {
      fl_alert(_("\"%s\" already exists."), name.c_str());
      return;
    }
    // RENAME_NOREPLACE: never overwrite a file created meanwhile.
    int r = renameat2(AT_FDCWD, it.path.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE);
    if (r != 0 && (errno == EINVAL || errno == ENOSYS))  // file systems without the flag
      r = rename(it.path.c_str(), target.c_str());
    if (r != 0) {
      if (errno == EEXIST) fl_alert(_("\"%s\" already exists."), name.c_str());
      else fl_alert("%s", _("Could not rename the item."));
    } else if (dir == desktop_dir_ && positions_.get(old_name, nullptr)) {
      positions_.rename(old_name, name);
      save_positions();
    }
  }

  void trash_item(int i) {
    if (is_file_item(i)) trash_item(items_[size_t(i)]);
  }

  void trash_item(const Item &it) {
    if (!is_file(it)) return;
    trash_paths({it.path});
  }

  // Selected files (or the item under the keyboard focus).
  std::vector<std::string> selected_paths() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < items_.size(); ++i)
      if (is_file(items_[i]) && (sel_.count(items_[i].uri) || (sel_.empty() && int(i) == selected_)))
        out.push_back(items_[i].path);
    return out;
  }

  // The freedesktop.org home trash for files on the home file system; gio
  // for the others (it knows the per-volume trash folders).
  void trash_paths(const std::vector<std::string> &paths) {
    std::vector<desk::FileOp> ops;
    std::string trash = desk::home_trash_dir();
    make_dirs(trash + "/files");
    for (auto &p : paths) {
      if (desk::same_filesystem(p, trash + "/files")) {
        desk::FileOp op;
        op.kind = desk::FileOp::TRASH;
        op.src = p;
        ops.push_back(op);
      } else if (!spawn_detached({"gio", "trash", p})) {
        fl_alert("%s", _("Moving to the trash needs the gio program (package libglib2.0-bin)."));
        return;
      }
    }
    if (!ops.empty()) start_job(std::move(ops));
  }

  void new_folder() {
    std::string desk = user_dir("DESKTOP");
    make_dirs(desk);
    std::string path = unique_path(desk, _("New Folder"));
    const char *input = fl_input("%s", basename_of(path).c_str(), _("Folder name:"));
    if (!input || !*input) return;
    std::string name = input;
    if (!valid_name(name.c_str()) || mkdir((desk + "/" + name).c_str(), 0755) != 0)
      fl_alert("%s", _("Could not create the folder."));
  }

  void new_document() {
    std::string desk = user_dir("DESKTOP");
    make_dirs(desk);
    std::string path = unique_path(desk, _("New Document"), ".txt");
    const char *input = fl_input("%s", basename_of(path).c_str(), _("Document name:"));
    if (!input || !*input) return;
    std::string name = input;
    if (!valid_name(name.c_str())) {
      fl_alert("%s", _("Could not create the document."));
      return;
    }
    std::string target = desk + "/" + name;
    struct stat st;
    if (lstat(target.c_str(), &st) == 0) {
      fl_alert(_("\"%s\" already exists."), name.c_str());
      return;
    }
    if (!write_file_atomic(target, "")) fl_alert("%s", _("Could not create the document."));
  }

  // Arrange Icons > By ...: forgets the free positions and sorts again.
  void arrange(const char *key) {
    positions_.clear();
    save_positions();
    if (cfg_.get("desktop", "sort", "name") == key) {
      rebuild_items();
      redraw();
    } else {
      set_option("sort", key);  // reloads and rebuilds
    }
  }

  void set_option(const char *key, const char *value) {
    cfg_.set("desktop", key, value);
    cfg_.save();
    x11::notify_config_changed();  // reloads this window too
  }

  // Right-click menu. |i| is the item under the pointer, or -1 for the
  // desktop itself. Combines the familiar Windows entries (View, Sort by,
  // Refresh, New) with the usual Linux ones (terminal, file manager).
  void context_menu(int i, int mx, int my) {
    if (i >= 0) {
      bool file = is_file_item(i);
      int fflag = file ? 0 : FL_MENU_INACTIVE;
      const Item it = items_[size_t(i)];  // a copy: popup() runs a nested event loop
      bool trash = it.uri == "trash:///";
      // Places (Computer, Home, Trash) share the file layout of the menu;
      // entries that do not apply are shown disabled, and the last entry
      // removes the place from the desktop instead of trashing it.
      Fl_Menu_Item items[] = {
          {_("Open"), FL_Enter, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},           // 0
          {_("Rename..."), FL_F + 2, nullptr, nullptr, fflag, 0, 0, 0, 0},                // 1
          {_("Copy Location"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},         // 2
          {file ? _("Move to Trash") : _("Remove from Desktop"), file ? FL_Delete : 0, nullptr,
           nullptr, trash ? FL_MENU_DIVIDER : 0, 0, 0, 0, 0},                              // 3
          {_("Empty Trash"), 0, nullptr, nullptr, trash ? 0 : FL_MENU_INVISIBLE, 0, 0, 0, 0},  // 4
          {_("Desktop Settings"), 0, nullptr, nullptr, file ? FL_MENU_INVISIBLE : 0, 0, 0, 0, 0},  // 5
          {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
      if (!file) items[3].flags |= FL_MENU_DIVIDER;
      const Fl_Menu_Item *m = items->popup(mx, my);
      if (m == &items[0]) open_item(it);
      else if (m == &items[1]) rename_item(it);
      else if (m == &items[2]) {
        const std::string &path = it.path.empty() ? it.uri : it.path;
        Fl::copy(path.c_str(), int(path.size()), 1);  // clipboard
        Fl::copy(path.c_str(), int(path.size()), 0);  // primary selection
      } else if (m == &items[3]) {
        std::vector<std::string> paths = selected_paths();
        if (file && paths.size() > 1) trash_paths(paths);
        else if (file) trash_item(it);
        else hide_place(it);
      } else if (m == &items[4]) {
        if (fl_choice("%s", _("Cancel"), _("Empty Trash"), nullptr,
                      _("Permanently delete all items in the Trash?")) == 1 &&
            !spawn_detached({"gio", "trash", "--empty"}))
          fl_alert("%s", _("Emptying the trash needs the gio program (package libglib2.0-bin)."));
      } else if (m == &items[5]) {
        spawn_detached({"sde-settings", "--page", "desktop"});
      }
      return;
    }
    std::string size = cfg_.get("desktop", "icon_size", "medium");
    std::string sort = cfg_.get("desktop", "sort", "name");
    auto radio = [](bool on) { return FL_MENU_RADIO | (on ? FL_MENU_VALUE : 0); };
    int autoflag = FL_MENU_TOGGLE | (positions_.auto_arrange ? FL_MENU_VALUE : 0);
    Fl_Menu_Item items[] = {
        {_("View"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},                              // 0
        {_("Large Icons"), 0, nullptr, nullptr, radio(size == "large"), 0, 0, 0, 0},            // 1
        {_("Medium Icons"), 0, nullptr, nullptr, radio(size == "medium"), 0, 0, 0, 0},          // 2
        {_("Small Icons"), 0, nullptr, nullptr, radio(size == "small"), 0, 0, 0, 0},            // 3
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},                                          // 4
        {_("Arrange Icons"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},                     // 5
        {_("By Name"), 0, nullptr, nullptr, radio(sort == "name"), 0, 0, 0, 0},                 // 6
        {_("By Size"), 0, nullptr, nullptr, radio(sort == "size"), 0, 0, 0, 0},                 // 7
        {_("By Type"), 0, nullptr, nullptr, radio(sort == "type"), 0, 0, 0, 0},                 // 8
        {_("By Date Modified"), 0, nullptr, nullptr, radio(sort == "modified") | FL_MENU_DIVIDER, 0, 0, 0, 0},  // 9
        {_("Auto-arrange"), 0, nullptr, nullptr, autoflag, 0, 0, 0, 0},                         // 10
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},                                          // 11
        {_("Refresh"), FL_F + 5, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},                // 12
        {_("New"), 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER, 0, 0, 0, 0},              // 13
        {_("Folder"), FL_CTRL + FL_SHIFT + 'n', nullptr, nullptr, 0, 0, 0, 0, 0},               // 14
        {_("Empty Document"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                             // 15
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},                                          // 16
        {_("Open Terminal Here"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                         // 17
        {_("Open in File Manager"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},          // 18
        {_("Display Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                           // 19
        {_("Change Wallpaper..."), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                        // 20
        {_("Desktop Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                           // 21
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    const Fl_Menu_Item *m = items->popup(mx, my);
    if (!m) return;
    long k = m - items;
    std::string desk = user_dir("DESKTOP");
    switch (k) {
      case 1: set_option("icon_size", "large"); break;
      case 2: set_option("icon_size", "medium"); break;
      case 3: set_option("icon_size", "small"); break;
      case 6: arrange("name"); break;
      case 7: arrange("size"); break;
      case 8: arrange("type"); break;
      case 9: arrange("modified"); break;
      case 10:
        positions_.auto_arrange = !positions_.auto_arrange;
        save_positions();
        rebuild_items();
        redraw();
        break;
      case 12: refresh(); break;
      case 14: new_folder(); break;
      case 15: new_document(); break;
      case 17: open_terminal({}, dir_exists(desk) ? desk : home_dir()); break;
      case 18:
        make_dirs(desk);
        open_uri(desk);
        break;
      case 19: spawn_detached({"sde-settings", "--page", "display"}); break;
      case 20: spawn_detached({"sde-settings", "--page", "wallpaper"}); break;
      case 21: spawn_detached({"sde-settings", "--page", "desktop"}); break;
      default: break;
    }
  }

  // Keyboard shortcuts (the same keys shown in the context menu).
  int key(int k, int state) {
    bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT;
    int n = int(items_.size());
    if (k == FL_F + 5) {
      refresh();
      return 1;
    }
    if (ctrl && shift && (k == 'n' || k == 'N')) {
      new_folder();
      return 1;
    }
    if (k == FL_Menu || (shift && k == FL_F + 10)) {
      int mx = w() / 2, my = h() / 2;
      if (selected_ >= 0 && selected_ < n) {
        mx = items_[size_t(selected_)].x + cell_w_ / 2;
        my = items_[size_t(selected_)].y + cell_h_ / 2;
      }
      context_menu(selected_, mx, my);
      return 1;
    }
    if (k == FL_Escape) {
      selected_ = -1;
      sel_.clear();
      redraw();
      return 1;
    }
    if (!n) return 0;
    if (ctrl && (k == 'a' || k == 'A')) {
      for (auto &it : items_) sel_.insert(it.uri);
      redraw();
      return 1;
    }
    if ((k == FL_Enter || k == FL_KP_Enter) && selected_ >= 0) {
      // Every selected icon (copies: opening may run dialogs).
      std::vector<Item> open;
      for (size_t i = 0; i < items_.size(); ++i)
        if (is_selected(int(i)) || int(i) == selected_) open.push_back(items_[i]);
      for (auto &it : open) open_item(it);
      return 1;
    }
    if (k == FL_F + 2 && selected_ >= 0) {
      rename_item(selected_);
      return 1;
    }
    if (k == FL_Delete && (selected_ >= 0 || !sel_.empty())) {
      std::vector<std::string> paths = selected_paths();
      if (!paths.empty()) trash_paths(paths);
      return 1;
    }
    // Arrow keys move to the nearest icon in that direction.
    int s = selected_ < 0 ? 0 : selected_;
    int next = -2;
    if (k == FL_Down) next = neighbor(s, 0, 1);
    else if (k == FL_Up) next = neighbor(s, 0, -1);
    else if (k == FL_Right) next = neighbor(s, 1, 0);
    else if (k == FL_Left) next = neighbor(s, -1, 0);
    else if (k == FL_Home) next = 0;
    else if (k == FL_End) next = n - 1;
    else return 0;
    if (selected_ < 0) next = 0;
    if (next < 0) return 1;
    if (shift) {
      sel_.insert(items_[size_t(next)].uri);
      selected_ = next;
      redraw();
    } else {
      select_only(next);
    }
    return 1;
  }

  int first_file_index() const {
    int n = 0;
    for (auto &it : items_)
      if (it.path.empty() || it.path == home_dir()) ++n;
    return n;
  }

  // ------------------------------------------------------------ selection

  bool is_selected(int i) const {
    return i >= 0 && i < int(items_.size()) && sel_.count(items_[size_t(i)].uri) != 0;
  }

  void select_only(int i) {
    sel_.clear();
    if (i >= 0 && i < int(items_.size())) sel_.insert(items_[size_t(i)].uri);
    selected_ = i;
    redraw();
  }

  // Items whose cell intersects the rubber band (added to what was selected
  // before when Ctrl is held).
  void update_band() {
    int x0 = std::min(band_x0_, band_x1_), x1 = std::max(band_x0_, band_x1_);
    int y0 = std::min(band_y0_, band_y1_), y1 = std::max(band_y0_, band_y1_);
    sel_ = band_base_;
    for (auto &it : items_) {
      int ix0 = it.x + S(4), ix1 = it.x + cell_w_ - S(4), iy0 = it.y + S(2), iy1 = it.y + cell_h_ - S(4);
      if (ix0 < x1 && ix1 > x0 && iy0 < y1 && iy1 > y0) sel_.insert(it.uri);
    }
  }

  static unsigned mods_from_fl(int state) {
    unsigned m = 0;
    if (state & FL_SHIFT) m |= desk::MOD_SHIFT;
    if (state & FL_CTRL) m |= desk::MOD_CTRL;
    if (state & FL_ALT) m |= desk::MOD_ALT;
    return m;
  }

  // ------------------------------------------------------------ drop targets

  // Folders (and Home), the Trash and application launchers take drops; a
  // drop anywhere else lands on the desktop itself.
  TargetKind target_kind(int i) const {
    if (i < 0 || i >= int(items_.size())) return TargetKind::Desktop;
    const Item &it = items_[size_t(i)];
    if (it.uri == "trash:///") return TargetKind::Trash;
    if (it.uri == "computer:///") return TargetKind::NoTarget;
    if (it.is_dir || !is_file(it)) return TargetKind::Folder;
    if (it.is_launcher) return TargetKind::Launcher;
    return TargetKind::Desktop;
  }

  std::string drop_dir() const { return desktop_dir_.empty() ? user_dir("DESKTOP") : desktop_dir_; }

  static std::string parent_of(const std::string &path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
  }

  // Alt-drops and sources that ask: "Move Here / Copy Here / Link Here".
  DropAction ask_action(int x, int y, const desk::ActionSet &allowed) {
    Fl_Menu_Item items[] = {
        {_("Move Here"), 0, nullptr, nullptr, allowed.move ? 0 : FL_MENU_INACTIVE, 0, 0, 0, 0},
        {_("Copy Here"), 0, nullptr, nullptr, allowed.copy ? 0 : FL_MENU_INACTIVE, 0, 0, 0, 0},
        {_("Link Here"), 0, nullptr, nullptr, (allowed.link ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER, 0, 0, 0, 0},
        {_("Cancel"), FL_Escape, nullptr, nullptr, 0, 0, 0, 0, 0},
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    const Fl_Menu_Item *m = items->popup(x, y);
    if (m == &items[0]) return DropAction::Move;
    if (m == &items[1]) return DropAction::Copy;
    if (m == &items[2]) return DropAction::Link;
    return DropAction::NoDrop;
  }

  // 0 skip, 1 keep both, 2 replace.
  int ask_conflict(const std::string &name, bool is_dir) {
    return fl_choice(is_dir ? _("A folder named \"%s\" already exists here.\nReplace it, keep both or skip this item?")
                            : _("A file named \"%s\" already exists here.\nReplace it, keep both or skip this item?"),
                     _("Skip"), _("Keep Both"), _("Replace"), name.c_str());
  }

  void start_job(std::vector<desk::FileOp> ops) {
    if (ops.empty()) return;
    // Deletes itself when done; inotify brings the changes to the icons.
    new FileJob(std::move(ops), [] {
      if (instance_) Fl::add_timeout(0.3, [](void *) {
        if (instance_) instance_->refresh();
      });
    });
  }

  // Gives |names| (about to appear in ~/Desktop) the free cells nearest to
  // |cell|, so that dropped files land where they were dropped.
  void place_new_names(const std::vector<std::string> &names, desk::Cell cell) {
    if (positions_.auto_arrange || names.empty()) return;
    pin_all_positions();
    std::set<desk::Cell> taken;
    for (auto &it : items_)
      if (std::find(names.begin(), names.end(), item_key(it)) == names.end()) taken.insert(cell_of(it));
    for (auto &n : names) {
      desk::Cell c = nearest_free(cell, taken);
      positions_.set(n, c);
      taken.insert(c);
    }
    save_positions();
  }

  desk::Cell nearest_free(desk::Cell want, const std::set<desk::Cell> &taken) const {
    if (!taken.count(want)) return want;
    int limit = std::max(cols_, rows_) + 1;
    for (int r = 1; r <= limit; ++r) {
      desk::Cell best{-1, -1};
      int best_d = 1 << 30;
      for (int dc = -r; dc <= r; ++dc)
        for (int dr = -r; dr <= r; ++dr) {
          if (std::max(std::abs(dc), std::abs(dr)) != r) continue;
          desk::Cell c{want.col + dc, want.row + dr};
          if (c.col < 0 || c.row < 0 || c.col >= cols_ || c.row >= rows_ || taken.count(c)) continue;
          int d = dc * dc * 2 + dr * dr;  // prefer the same column (icons flow down)
          if (d < best_d) best_d = d, best = c;
        }
      if (best.col >= 0) return best;
    }
    // The grid is full: the first column beyond it.
    desk::Cell c{cols_, 0};
    while (taken.count(c)) {
      if (++c.row >= rows_) c.row = 0, ++c.col;
    }
    return c;
  }

  // Drops local |paths| on item |ti| (or the desktop) at (x, y). Returns
  // what was done (None when nothing happened).
  DropAction drop_files(const std::vector<std::string> &paths, int ti, unsigned mods, DropAction suggested,
                        const desk::ActionSet &allowed, int x, int y) {
    if (paths.empty()) return DropAction::NoDrop;
    TargetKind kind = target_kind(ti);
    Item target = ti >= 0 && ti < int(items_.size()) ? items_[size_t(ti)] : Item();  // a copy (dialogs follow)
    if (kind == TargetKind::NoTarget) return DropAction::NoDrop;
    if (kind == TargetKind::Launcher) {
      DesktopEntry e;
      if (e.load(target.path) && e.type == "Application") {
        if (access(target.path.c_str(), X_OK) != 0) {
          if (fl_choice(_("\"%s\" is not marked as trusted.\nDo you want to trust and launch it?"), _("Cancel"),
                        _("Trust and Launch"), nullptr, target.label.c_str()) != 1)
            return DropAction::NoDrop;
          struct stat st;
          if (stat(target.path.c_str(), &st) == 0) chmod(target.path.c_str(), st.st_mode | S_IXUSR);
        }
        return launch_entry(e, paths) ? DropAction::Copy : DropAction::NoDrop;
      }
      kind = TargetKind::Desktop;
      ti = -1;
    }
    if (kind == TargetKind::Trash) {
      trash_paths(paths);
      return DropAction::Move;
    }
    std::string dir = kind == TargetKind::Folder ? target.path : drop_dir();
    make_dirs(dir);
    bool same_fs = desk::same_filesystem(paths[0], dir);
    DropAction a = desk::choose_drop_action(kind, mods, same_fs, suggested, allowed);
    if (a == DropAction::Ask) a = ask_action(x, y, allowed);
    if (a == DropAction::NoDrop) return DropAction::NoDrop;
    std::vector<desk::FileOp> ops;
    std::vector<std::string> names;
    for (auto &src : paths) {
      std::string name = basename_of(src);
      if (name.empty() || !desk::path_exists(src)) continue;
      bool here = parent_of(src) == dir;
      if (here && a == DropAction::Move) continue;  // already there
      if (a != DropAction::Link && desk::path_inside(dir, src)) {
        fl_alert(_("\"%s\" cannot be moved or copied into itself."), name.c_str());
        continue;
      }
      struct stat st;
      bool is_dir = stat(src.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      desk::FileOp op;
      op.kind = a == DropAction::Move ? desk::FileOp::MOVE
                : a == DropAction::Link ? desk::FileOp::LINK
                                        : desk::FileOp::COPY;
      op.src = src;
      auto taken = [](const std::string &n, void *d) {
        return desk::path_exists(*static_cast<std::string *>(d) + "/" + n);
      };
      if (desk::path_exists(dir + "/" + name)) {
        int c = here ? 1 : ask_conflict(name, is_dir);
        if (c == 0) continue;
        if (c == 1) name = desk::keep_both_name(name, is_dir, taken, &dir);
        else op.replace = true;
      }
      op.dst = dir + "/" + name;
      ops.push_back(op);
      names.push_back(name);
    }
    if (ops.empty()) return DropAction::NoDrop;
    if (dir == desktop_dir_ && ti < 0) place_new_names(names, cell_at(x, y));
    start_job(std::move(ops));
    return a;
  }

  // Text and links dropped from other programs: a link (.desktop) per URL,
  // or "Dropped text.txt".
  DropAction drop_other(const desk::DropPayload &payload, int ti, int x, int y) {
    TargetKind kind = target_kind(ti);
    if (kind == TargetKind::Launcher && !payload.urls.empty()) {
      DesktopEntry e;
      if (e.load(items_[size_t(ti)].path) && e.type == "Application")
        return launch_entry(e, payload.urls) ? DropAction::Copy : DropAction::NoDrop;
    }
    if (kind == TargetKind::Launcher) kind = TargetKind::Desktop, ti = -1;
    if (kind != TargetKind::Desktop && kind != TargetKind::Folder) return DropAction::NoDrop;
    std::string dir = kind == TargetKind::Folder ? items_[size_t(ti)].path : drop_dir();
    make_dirs(dir);
    auto taken = [](const std::string &n, void *d) {
      return desk::path_exists(*static_cast<std::string *>(d) + "/" + n);
    };
    std::vector<std::string> names;
    auto write_new = [&](std::string name, const std::string &content, mode_t mode) {
      if (desk::path_exists(dir + "/" + name)) name = desk::keep_both_name(name, false, taken, &dir);
      std::string path = dir + "/" + name;
      int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
      if (fd < 0) return false;
      bool ok = write(fd, content.data(), content.size()) == ssize_t(content.size());
      close(fd);
      if (ok) names.push_back(name);
      return ok;
    };
    bool any = false;
    for (auto &url : payload.urls) {
      std::string file = desk::link_file_name(url);
      // Trusted from the start: the user made it.
      any = write_new(file, desk::link_desktop_entry(url, file.substr(0, file.size() - 8)), 0755) || any;
    }
    if (!payload.text.empty())
      any = write_new(std::string(_("Dropped text")) + ".txt", payload.text, 0644) || any;
    if (!any) return DropAction::NoDrop;
    if (dir == desktop_dir_ && ti < 0) place_new_names(names, cell_at(x, y));
    return DropAction::Copy;
  }

  // ------------------------------------------------------------ dragging

  void setup_dnd(Window xid) {
    desk::Xdnd::Hooks h;
    h.over = [this](int x, int y, unsigned mods, DropAction suggested, const desk::ActionSet &allowed,
                    bool files) {
      int i = item_at(x - this->x(), y - this->y());
      TargetKind k = target_kind(i);
      if (!files && k != TargetKind::Desktop && k != TargetKind::Folder) k = TargetKind::Desktop;
      int hl = (k == TargetKind::Folder || k == TargetKind::Trash || k == TargetKind::Launcher) ? i : -1;
      if (hl != drop_target_) {
        damage_item(drop_target_);
        drop_target_ = hl;
        damage_item(drop_target_);
      }
      if (k == TargetKind::NoTarget) return DropAction::NoDrop;
      if (!files) return allowed.copy ? DropAction::Copy : DropAction::NoDrop;  // text makes a new file
      // The file system is not known before the data arrives: the answer
      // assumes one file system, the drop decides for real.
      return desk::choose_drop_action(k, mods, true, suggested, allowed);
    };
    h.leave = [this] {
      damage_item(drop_target_);
      drop_target_ = -1;
    };
    h.drop = [this](int x, int y, DropAction action, const std::string &data, bool uri_list) {
      desk::DropPayload payload = desk::classify_drop(data, uri_list);
      int lx = x - this->x(), ly = y - this->y();
      int i = item_at(lx, ly);
      damage_item(drop_target_);
      drop_target_ = -1;
      Window r, c;
      int rx, ry, wx, wy;
      unsigned mask = 0;
      XQueryPointer(fl_display, RootWindow(fl_display, fl_screen), &r, &c, &rx, &ry, &wx, &wy, &mask);
      unsigned mods = desk::Xdnd::modifiers_from_state(mask);
      DropAction done = DropAction::NoDrop;
      if (!payload.files.empty())
        done = drop_files(payload.files, i, mods, action == DropAction::Ask ? DropAction::Ask : DropAction::Copy,
                          xdnd_->source_actions(), lx, ly);
      if (!payload.urls.empty() || !payload.text.empty()) {
        DropAction other = drop_other(payload, payload.files.empty() ? i : -1, lx, ly);
        if (done == DropAction::NoDrop) done = other;
      }
      return done;
    };
    h.finished = [](bool accepted, DropAction action) {
      // The target moved the files away (or not): look again soon.
      if (accepted && action == DropAction::Move && instance_)
        Fl::add_timeout(0.5, [](void *) {
          if (instance_) instance_->refresh();
        });
    };
    xdnd_.reset(new desk::Xdnd(fl_display, xid, h));
  }

  void start_drag() {
    if (press_item_ < 0 || press_item_ >= int(items_.size())) return;
    drag_paths_.clear();
    for (auto &it : items_)
      if (sel_.count(it.uri) && is_file(it)) drag_paths_.push_back(it.path);
    drag_state_ = DRAG_ITEMS;
    drag_local_ = true;
    drag_offset_x_ = press_x_ - items_[size_t(press_item_)].x;
    drag_offset_y_ = press_y_ - items_[size_t(press_item_)].y;
    drag_cell_ = cell_of(items_[size_t(press_item_)]);
    // Only files leave the desktop; Home, Trash and Computer are moved
    // around on it but never offered to other programs.
    if (xdnd_ && !drag_paths_.empty())
      xdnd_->begin_drag(drag_paths_, items_[size_t(press_item_)].icon, icon_, int(drag_paths_.size()),
                        fl_event_time);
    redraw();
  }

  void drag_motion(int xr, int yr, int x, int y) {
    unsigned mods = mods_from_fl(Fl::event_state());
    bool local = true;
    if (xdnd_ && xdnd_->dragging()) local = xdnd_->motion(xr, yr, mods, fl_event_time);
    drag_local_ = local;
    drop_target_ = -1;
    if (local) {
      int i = item_at(x, y);
      if (i >= 0 && !is_selected(i) && !drag_paths_.empty()) {
        TargetKind k = target_kind(i);
        if (k == TargetKind::Folder || k == TargetKind::Trash || k == TargetKind::Launcher) drop_target_ = i;
      }
      drag_cell_ = cell_at(x - drag_offset_x_ + cell_w_ / 2, y - drag_offset_y_ + cell_h_ / 2);
      cursor(FL_CURSOR_MOVE);
    } else {
      cursor(xdnd_->remote_accepts() ? FL_CURSOR_MOVE : FL_CURSOR_DEFAULT);
    }
    redraw();
  }

  void cancel_drag() {
    if (xdnd_ && xdnd_->dragging()) xdnd_->cancel();
    drag_state_ = DRAG_NONE;
    drop_target_ = -1;
    cursor(FL_CURSOR_DEFAULT);
    redraw();
  }

  // Moves the selected icons by the offset the pointer moved the pressed one.
  void move_selection_to(desk::Cell target) {
    if (positions_.auto_arrange || press_item_ < 0 || press_item_ >= int(items_.size())) return;
    desk::Cell origin = cell_of(items_[size_t(press_item_)]);
    int dc = target.col - origin.col, dr = target.row - origin.row;
    if (!dc && !dr) return;
    pin_all_positions();
    std::set<desk::Cell> taken;
    std::vector<size_t> moving;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (is_selected(int(i))) moving.push_back(i);
      else taken.insert(cell_of(items_[i]));
    }
    // The pressed icon first: it goes exactly where it was dropped.
    std::stable_sort(moving.begin(), moving.end(),
                     [&](size_t a, size_t b) { return int(a) == press_item_ && int(b) != press_item_; });
    for (size_t i : moving) {
      desk::Cell c = cell_of(items_[i]);
      c.col = std::max(0, std::min(cols_ - 1, c.col + dc));
      c.row = std::max(0, std::min(rows_ - 1, c.row + dr));
      c = nearest_free(c, taken);
      positions_.set(item_key(items_[i]), c);
      taken.insert(c);
    }
    save_positions();
    layout_items();
  }

  void finish_drag(int x, int y) {
    int ti = drop_target_;
    bool local = drag_local_;
    std::vector<std::string> paths = drag_paths_;
    desk::Cell cell = drag_cell_;
    unsigned mods = mods_from_fl(Fl::event_state());
    drag_state_ = DRAG_NONE;
    drop_target_ = -1;
    cursor(FL_CURSOR_DEFAULT);
    if (!local && xdnd_ && xdnd_->dragging()) {
      xdnd_->drop(fl_event_time);
      redraw();
      return;
    }
    if (xdnd_) xdnd_->cancel();
    if (ti >= 0) drop_files(paths, ti, mods, DropAction::Copy, desk::ActionSet(), x, y);
    else move_selection_to(cell);
    redraw();
  }

  // The nearest icon in a direction (icons are placed freely on the grid).
  int neighbor(int from, int dx, int dy) const {
    int best = -1;
    long best_d = 1L << 40;
    for (size_t j = 0; j < items_.size(); ++j) {
      if (int(j) == from) continue;
      long ddx = items_[j].x - items_[size_t(from)].x, ddy = items_[j].y - items_[size_t(from)].y;
      long primary = dx ? ddx * dx : ddy * dy;
      if (primary <= 0) continue;
      long secondary = dx ? std::labs(ddy) : std::labs(ddx);
      long d = primary + 3 * secondary;
      if (d < best_d) best_d = d, best = int(j);
    }
    return best;
  }

  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        int x = Fl::event_x(), y = Fl::event_y();
        int i = item_at(x, y);
        int state = Fl::event_state();
        bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT;
        if (Fl::focus() != this) take_focus();
        if (drag_state_ == DRAG_ITEMS) cancel_drag();
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          if (i < 0 || !is_selected(i)) select_only(i);
          context_menu(i, x, y);
          return 1;
        }
        if (Fl::event_button() != FL_LEFT_MOUSE) return 1;
        if (i < 0) {
          if (!ctrl) sel_.clear();
          band_base_ = sel_;
          selected_ = -1;
          drag_state_ = DRAG_BAND;
          band_x0_ = band_x1_ = x;
          band_y0_ = band_y1_ = y;
          redraw();
          return 1;
        }
        if (Fl::event_clicks() && !ctrl && !shift) {
          drag_state_ = DRAG_NONE;
          open_item(i);
          return 1;
        }
        if (ctrl) {
          if (is_selected(i)) sel_.erase(items_[size_t(i)].uri);
          else sel_.insert(items_[size_t(i)].uri);
        } else if (shift && selected_ >= 0 && selected_ < int(items_.size())) {
          // Everything in the rectangle between the focused icon and this one.
          band_base_.clear();
          band_x0_ = std::min(items_[size_t(selected_)].x, items_[size_t(i)].x) + S(5);
          band_x1_ = std::max(items_[size_t(selected_)].x, items_[size_t(i)].x) + cell_w_ - S(5);
          band_y0_ = std::min(items_[size_t(selected_)].y, items_[size_t(i)].y) + S(3);
          band_y1_ = std::max(items_[size_t(selected_)].y, items_[size_t(i)].y) + cell_h_ - S(5);
          update_band();
        } else if (!is_selected(i)) {
          select_only(i);
        }
        selected_ = i;
        press_item_ = i;
        press_x_ = x;
        press_y_ = y;
        drag_state_ = is_selected(i) ? DRAG_PRESSED : DRAG_NONE;
        redraw();
        return 1;
      }
      case FL_DRAG: {
        int x = Fl::event_x(), y = Fl::event_y();
        if (drag_state_ == DRAG_PRESSED && (std::abs(x - press_x_) > S(6) || std::abs(y - press_y_) > S(6)))
          start_drag();
        if (drag_state_ == DRAG_ITEMS) {
          drag_motion(Fl::event_x_root(), Fl::event_y_root(), x, y);
        } else if (drag_state_ == DRAG_BAND) {
          band_x1_ = x;
          band_y1_ = y;
          update_band();
          redraw();
        }
        return 1;
      }
      case FL_RELEASE: {
        int state = Fl::event_state();
        if (drag_state_ == DRAG_ITEMS) {
          finish_drag(Fl::event_x(), Fl::event_y());
        } else if (drag_state_ == DRAG_PRESSED) {
          // A plain click inside a multiple selection selects that icon only.
          if (!(state & (FL_CTRL | FL_SHIFT)) && sel_.size() > 1) select_only(press_item_);
        } else if (drag_state_ == DRAG_BAND) {
          redraw();
        }
        drag_state_ = DRAG_NONE;
        return 1;
      }
      case FL_ENTER:
      case FL_MOVE: {
        int i = item_at(Fl::event_x(), Fl::event_y());
        if (i != hover_) {
          damage_item(hover_);
          hover_ = i;
          damage_item(hover_);
        }
        return 1;
      }
      case FL_LEAVE:
        damage_item(hover_);
        hover_ = -1;
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == FL_Escape && drag_state_ != DRAG_NONE) {
          cancel_drag();
          return 1;
        }
        return key(Fl::event_key(), Fl::event_state());
      default:
        return sde::BufferedWindow::handle(e);
    }
  }

  static int x_event_handler(int) {
    if (!instance_ || !fl_xevent) return 0;
    const XEvent &e = *fl_xevent;
    Window root = RootWindow(fl_display, fl_screen);
    if (e.type == PropertyNotify && e.xproperty.window == root &&
        e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
      instance_->load();
      instance_->watch_desktop_dir();
      return 1;
    }
    if (e.type == ConfigureNotify && e.xconfigure.window == root) {
      Fl::call_screen_init();
      instance_->config_dirty_ = true;  // screen sizes may have changed
      instance_->load();
      XMoveResizeWindow(fl_display, fl_xid(instance_), 0, 0, unsigned(instance_->w()),
                        unsigned(instance_->h()));
      return 1;
    }
    return 0;
  }

  static Desktop *instance_;

 private:
  Config cfg_;
  Theme theme_;
  std::vector<Item> items_;                 // places, then files in display order
  std::map<std::string, Item> files_;       // ~/Desktop entries by file name
  std::set<std::string> changed_;           // names reported by inotify, not applied yet
  bool full_rescan_ = false;                // inotify overflowed or the folder moved
  std::vector<cairo_surface_t *> wallpapers_;
  std::vector<WallpaperKey> wallpaper_keys_;
  WallpaperWorker worker_;
  bool wallpaper_checked_ = false, config_dirty_ = true;
  std::string desktop_dir_, watched_dir_;
  int selected_ = -1, hover_ = -1;  // keyboard focus and hovered icon
  std::set<std::string> sel_, band_base_;  // selected items (by uri)
  int cell_w_ = 96, cell_h_ = 96, icon_ = 0;
  int rows_ = 1, cols_ = 1, grid_x_ = 0, grid_y_ = 0;
  desk::PositionStore positions_;
  bool positions_dirty_ = false;
  std::map<uint32_t, std::string> moved_from_;  // inotify rename cookies
  enum DragState { DRAG_NONE, DRAG_PRESSED, DRAG_ITEMS, DRAG_BAND };
  DragState drag_state_ = DRAG_NONE;
  int press_item_ = -1, press_x_ = 0, press_y_ = 0, drag_offset_x_ = 0, drag_offset_y_ = 0;
  int band_x0_ = 0, band_y0_ = 0, band_x1_ = 0, band_y1_ = 0;
  int drop_target_ = -1;
  bool drag_local_ = true;
  desk::Cell drag_cell_;
  std::vector<std::string> drag_paths_;
  std::unique_ptr<desk::Xdnd> xdnd_;
  int inotify_fd_ = -1, watch_ = -1;
  double draw_t0_ = 0, wallpaper_t0_ = 0;
};

Desktop *Desktop::instance_ = nullptr;

int ignore_x_errors(Display *, XErrorEvent *) { return 0; }

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  install_crash_handler("sde-desktop");
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) return print_version("sde-desktop", SDE_VERSION);
    printf("Usage: sde-desktop [--version]\n\nDraws the wallpaper and desktop icons.\n");
    return strcmp(argv[i], "--help") ? 2 : 0;
  }
  fl_open_display();
  XSetErrorHandler(ignore_x_errors);
  Desktop *d = new Desktop();
  Desktop::instance_ = d;
  d->show();
  d->setup();
  Fl::add_handler(Desktop::x_event_handler);
  return Fl::run();
}
