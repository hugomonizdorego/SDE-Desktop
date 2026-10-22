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
#include <FL/Fl.H>
#include "crash.h"
#include "buffered_window.h"
#include <FL/Fl_Menu_Item.H>
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
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "perf.h"
#include "spawn.h"
#include "thumbnails.h"
#include "theme.h"
#include "util.h"
#include "x11.h"

using namespace sde;

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

class Desktop : public sde::BufferedWindow {
 public:
  Desktop() : sde::BufferedWindow(0, 0, 100, 100, "SDE Desktop"), worker_(on_wallpaper_ready, this) {
    end();
    border(0);
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
    for (size_t i = 0; i < items_.size(); ++i)
      if (!sel.empty() && items_[i].uri == sel) selected_ = int(i);
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
    int rows = std::max(1, usable / cell_h_);
    text_font(theme_.font_size);
    for (size_t i = 0; i < items_.size(); ++i) {
      items_[i].x = mx + S(12) + int(i) / rows * cell_w_;
      items_[i].y = my + S(12) + (theme_.panel_top ? theme_.panel_height : 0) + int(i) % rows * cell_h_;
      items_[i].shown = ellipsize(items_[i].label, cell_w_ - S(12));
    }
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
        p += sizeof(struct inotify_event) + ev->len;
      }
    }
    Fl::remove_timeout(apply_inotify, data);
    Fl::add_timeout(0.2, apply_inotify, data);
  }

  static void apply_inotify(void *data) {
    auto *d = static_cast<Desktop *>(data);
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
        if (int(i) == selected_)
          p.fill_rounded(it.x + S(4), it.y + S(2), cell_w_ - S(8), cell_h_ - S(6), S(10), theme_.accent.alpha(0.45));
        else if (int(i) == hover_)
          p.fill_rounded(it.x + 4, it.y + 2, cell_w_ - 8, cell_h_ - 6, 10, Color::hex(0xFFFFFF, 0.16));
        if (it.icon_ready && it.icon)
          p.image(it.icon, it.x + (cell_w_ - icon_) / 2.0, it.y + S(8), icon_);
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
    }
  }

  void trash_item(int i) {
    if (is_file_item(i)) trash_item(items_[size_t(i)]);
  }

  void trash_item(const Item &it) {
    if (!is_file(it)) return;
    if (!spawn_detached({"gio", "trash", it.path}))
      fl_alert("%s", _("Moving to the trash needs the gio program (package libglib2.0-bin)."));
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
        if (file) trash_item(it);
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
    Fl_Menu_Item items[] = {
        {_("View"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},                              // 0
        {_("Large Icons"), 0, nullptr, nullptr, radio(size == "large"), 0, 0, 0, 0},            // 1
        {_("Medium Icons"), 0, nullptr, nullptr, radio(size == "medium"), 0, 0, 0, 0},          // 2
        {_("Small Icons"), 0, nullptr, nullptr, radio(size == "small"), 0, 0, 0, 0},            // 3
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},                                          // 4
        {_("Sort By"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},                           // 5
        {_("Name"), 0, nullptr, nullptr, radio(sort == "name"), 0, 0, 0, 0},                    // 6
        {_("Size"), 0, nullptr, nullptr, radio(sort == "size"), 0, 0, 0, 0},                    // 7
        {_("Type"), 0, nullptr, nullptr, radio(sort == "type"), 0, 0, 0, 0},                    // 8
        {_("Date Modified"), 0, nullptr, nullptr, radio(sort == "modified"), 0, 0, 0, 0},       // 9
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},                                          // 10
        {_("Refresh"), FL_F + 5, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},                // 11
        {_("New"), 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER, 0, 0, 0, 0},              // 12
        {_("Folder"), FL_CTRL + FL_SHIFT + 'n', nullptr, nullptr, 0, 0, 0, 0, 0},               // 13
        {_("Empty Document"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                             // 14
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},                                          // 15
        {_("Open Terminal Here"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                         // 16
        {_("Open in File Manager"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},          // 17
        {_("Display Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                           // 18
        {_("Change Wallpaper..."), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                        // 19
        {_("Desktop Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},                           // 20
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    const Fl_Menu_Item *m = items->popup(mx, my);
    if (!m) return;
    long k = m - items;
    std::string desk = user_dir("DESKTOP");
    switch (k) {
      case 1: set_option("icon_size", "large"); break;
      case 2: set_option("icon_size", "medium"); break;
      case 3: set_option("icon_size", "small"); break;
      case 6: set_option("sort", "name"); break;
      case 7: set_option("sort", "size"); break;
      case 8: set_option("sort", "type"); break;
      case 9: set_option("sort", "modified"); break;
      case 11: refresh(); break;
      case 13: new_folder(); break;
      case 14: new_document(); break;
      case 16: open_terminal({}, dir_exists(desk) ? desk : home_dir()); break;
      case 17:
        make_dirs(desk);
        open_uri(desk);
        break;
      case 18: spawn_detached({"sde-settings", "--page", "display"}); break;
      case 19: spawn_detached({"sde-settings", "--page", "wallpaper"}); break;
      case 20: spawn_detached({"sde-settings", "--page", "desktop"}); break;
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
      redraw();
      return 1;
    }
    if (!n) return 0;
    if ((k == FL_Enter || k == FL_KP_Enter) && selected_ >= 0) {
      open_item(selected_);
      return 1;
    }
    if (k == FL_F + 2 && selected_ >= 0) {
      rename_item(selected_);
      return 1;
    }
    if (k == FL_Delete && selected_ >= 0) {
      trash_item(selected_);
      return 1;
    }
    // Arrow keys move the selection (icons are laid out in columns).
    int rows = 1;
    while (rows < n && items_[size_t(rows)].x == items_[0].x) ++rows;
    int s = selected_ < 0 ? 0 : selected_;
    if (k == FL_Down) s = std::min(n - 1, s + 1);
    else if (k == FL_Up) s = std::max(0, s - 1);
    else if (k == FL_Right) s = std::min(n - 1, s + rows);
    else if (k == FL_Left) s = std::max(0, s - rows);
    else if (k == FL_Home) s = 0;
    else if (k == FL_End) s = n - 1;
    else return 0;
    damage_item(selected_);
    selected_ = s;
    damage_item(selected_);
    return 1;
  }

  int first_file_index() const {
    int n = 0;
    for (auto &it : items_)
      if (it.path.empty() || it.path == home_dir()) ++n;
    return n;
  }

  // Files dropped from Liria or another application (XDND, alpha52): FLTK
  // delivers them as text, one file:// URI or absolute path per line. On
  // the same drive they are moved to ~/Desktop, from another drive copied,
  // as in a file manager; Shift moves and Ctrl copies. Existing names are
  // never overwritten. The inotify watch then shows the new icons.
  static std::string uri_to_path(const std::string &line) {
    std::string s = line;
    if (s.rfind("file://", 0) == 0) {
      size_t slash = s.find('/', 7);
      if (slash == std::string::npos) return std::string();
      s = s.substr(slash);
      std::string out;
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
          out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
          i += 2;
        } else {
          out += s[i];
        }
      }
      return out;
    }
    return !s.empty() && s[0] == '/' ? s : std::string();
  }

  void drop_files(const char *text, int state) {
    std::string desk = user_dir("DESKTOP");
    struct stat ds {};
    if (!text || desk.empty() || stat(desk.c_str(), &ds) != 0) return;
    std::string all = text;
    size_t start = 0;
    while (start <= all.size()) {
      size_t end = all.find('\n', start);
      std::string line = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
      start = end == std::string::npos ? all.size() + 1 : end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::string path = uri_to_path(line);
      struct stat fs {};
      if (path.empty() || stat(path.c_str(), &fs) != 0) continue;
      std::string parent = path.substr(0, path.find_last_of('/'));
      if (parent == desk || path == desk) continue;  // already on the desktop
      bool move = (state & FL_SHIFT) ? true : (state & FL_CTRL) ? false : fs.st_dev == ds.st_dev;
      if (move)
        spawn_detached({"mv", "-n", "--", path, desk + "/"});
      else
        spawn_detached({"cp", "-a", "-n", "--", path, desk + "/"});
    }
  }

  // Desktop files can be dragged into Liria and other applications: FLTK
  // offers a selection that starts with file:/// and ends with CR LF as
  // text/uri-list. Places (Home, Trash, ...) are not dragged.
  void start_drag(int i) {
    if (i < first_file_index() || i >= int(items_.size()) || items_[size_t(i)].path.empty()) return;
    static const char hex[] = "0123456789ABCDEF";
    std::string uri = "file://";
    for (unsigned char c : items_[size_t(i)].path) {
      if (isalnum(c) || strchr("/-._~", c)) {
        uri += char(c);
      } else {
        uri += '%';
        uri += hex[c >> 4];
        uri += hex[c & 15];
      }
    }
    uri += "\r\n";
    Fl::copy(uri.c_str(), int(uri.size()), 0);
    Fl::dnd();
  }

  int handle(int e) override {
    switch (e) {
      case FL_DRAG:
        if (press_item_ >= 0 && Fl::event_button1() &&
            std::abs(Fl::event_x() - press_x_) + std::abs(Fl::event_y() - press_y_) > 8) {
          int i = press_item_;
          press_item_ = -1;
          start_drag(i);
          return 1;
        }
        return sde::BufferedWindow::handle(e);
      case FL_RELEASE:
        press_item_ = -1;
        return sde::BufferedWindow::handle(e);
      case FL_DND_ENTER:
      case FL_DND_DRAG:
      case FL_DND_LEAVE:
        return 1;
      case FL_DND_RELEASE:
        dnd_state_ = Fl::event_state();
        dnd_pending_ = true;
        return 1;
      case FL_PASTE:
        if (dnd_pending_) {
          dnd_pending_ = false;
          drop_files(Fl::event_text(), dnd_state_);
        }
        return 1;
      case FL_PUSH: {
        int i = item_at(Fl::event_x(), Fl::event_y());
        if (i != selected_) {
          damage_item(selected_);
          selected_ = i;
          damage_item(selected_);
        }
        press_item_ = Fl::event_button() == FL_LEFT_MOUSE && !Fl::event_clicks() ? i : -1;
        press_x_ = Fl::event_x();
        press_y_ = Fl::event_y();
        if (Fl::event_button() == FL_RIGHT_MOUSE) context_menu(i, Fl::event_x(), Fl::event_y());
        else if (Fl::event_clicks() && i >= 0) open_item(i);
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
  int selected_ = -1, hover_ = -1;
  int press_item_ = -1, press_x_ = 0, press_y_ = 0;  // a possible drag start
  int dnd_state_ = 0;          // keyboard state when files were dropped
  bool dnd_pending_ = false;   // FL_PASTE that follows a drop
  int cell_w_ = 96, cell_h_ = 96, icon_ = 0;
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
    if (!strcmp(argv[i], "--version")) {
      printf("sde-desktop %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    }
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
