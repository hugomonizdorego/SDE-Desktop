// SPDX-License-Identifier: GPL-3.0-or-later
// sde-desktop: wallpaper and desktop icons (Home, Trash and ~/Desktop).
#include <FL/Fl.H>
#include "buffered_window.h"
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "desktop_entry.h"
#include "icons.h"
#include "paint.h"
#include "spawn.h"
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
};

std::string icon_for_file(const std::string &path) {
  if (dir_exists(path)) return "folder";
  std::string ext = lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size()
                                                                                    : path.find_last_of('.')));
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

class Desktop : public sde::BufferedWindow {
 public:
  Desktop() : sde::BufferedWindow(0, 0, 100, 100, "SDE Desktop") {
    end();
    border(0);
    load();
  }

  ~Desktop() override {
    for (auto *s : wallpapers_) cairo_surface_destroy(s);
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

  void load() {
    cfg_.load();
    theme_ = Theme::from_config(cfg_);
    theme_.apply_to_fltk();
    IconLoader::set_theme(cfg_.get("appearance", "icon_theme", "auto"));
    IconLoader::clear_cache();
    int sw = DisplayWidth(fl_display, fl_screen), sh = DisplayHeight(fl_display, fl_screen);
    sde::BufferedWindow::resize(0, 0, sw, sh);
    for (auto *s : wallpapers_) cairo_surface_destroy(s);
    wallpapers_.clear();
    reload_items();
    redraw();
  }

  void reload_items() {
    items_.clear();
    std::string home = home_dir();
    if (cfg_.get_bool("desktop", "show_home", true))
      items_.push_back({"Home", home, home, {"user-home", "folder-home", "folder"}, false});
    if (cfg_.get_bool("desktop", "show_trash", false))
      items_.push_back({"Trash", "", "trash:///", {"user-trash", "user-trash-full"}, false});
    if (cfg_.get_bool("desktop", "show_icons", true)) {
      desktop_dir_ = user_dir("DESKTOP");
      if (desktop_dir_ != home) {
        for (auto &name : list_dir(desktop_dir_)) {
          if (name[0] == '.') continue;
          std::string full = desktop_dir_ + "/" + name;
          Item it{name, full, full, {icon_for_file(full)}, false};
          if (ends_with(name, ".desktop")) {
            DesktopEntry e;
            if (e.load(full) && !e.name.empty()) {
              it.label = e.name;
              it.icons = {e.icon, "application-x-executable"};
              it.is_launcher = true;
            }
          }
          items_.push_back(it);
        }
      }
    }
    selected_ = -1;
    layout_items();
  }

  void layout_items() {
    int mx, my, mw, mh;
    Fl::screen_xywh(mx, my, mw, mh, 0);
    int usable = mh - theme_.panel_height - 24;
    int rows = std::max(1, usable / cell_h_);
    for (size_t i = 0; i < items_.size(); ++i) {
      items_[i].x = mx + 12 + int(i) / rows * cell_w_;
      items_[i].y = my + 12 + int(i) % rows * cell_h_;
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
    if (dir_exists(user_dir("DESKTOP")))
      watch_ = inotify_add_watch(inotify_fd_, user_dir("DESKTOP").c_str(),
                                 IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);
  }

  static void on_inotify(int fd, void *data) {
    char buf[4096];
    while (read(fd, buf, sizeof buf) > 0) {
    }
    // Coalesce bursts of events (e.g. a file copy) into one refresh.
    Fl::remove_timeout(delayed_reload, data);
    Fl::add_timeout(0.3, delayed_reload, data);
  }

  static void delayed_reload(void *data) {
    auto *d = static_cast<Desktop *>(data);
    d->reload_items();
    d->redraw();
  }

  std::string wallpaper_path() const {
    std::string wp = cfg_.get("appearance", "wallpaper", "auto");
    if (wp.empty() || wp == "auto" || !file_exists(wp)) {
      std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
      wp = base + "/backgrounds/sde/" + (theme_.dark ? "selviana-dark.svg" : "selviana-light.svg");
    }
    return wp;
  }

  void draw() override {
    int n = Fl::screen_count();
    if (int(wallpapers_.size()) != n) {
      for (auto *s : wallpapers_) cairo_surface_destroy(s);
      wallpapers_.clear();
      std::string wp = wallpaper_path();
      std::string mode = cfg_.get("appearance", "wallpaper_mode", "zoom");
      for (int i = 0; i < n; ++i) {
        int sx, sy, sw, sh;
        Fl::screen_xywh(sx, sy, sw, sh, i);
        Color bg = theme_.dark ? Color::hex(0x10161D) : Color::hex(0x1C6F8A);
        wallpapers_.push_back(load_wallpaper(wp, sw, sh, mode, bg.r, bg.g, bg.b));
      }
    }
    {
      Painter p(w(), h());
      for (int i = 0; i < n; ++i) {
        int sx, sy, sw, sh;
        Fl::screen_xywh(sx, sy, sw, sh, i);
        cairo_set_source_surface(p.cr(), wallpapers_[size_t(i)], sx, sy);
        cairo_paint(p.cr());
      }
      for (size_t i = 0; i < items_.size(); ++i) {
        const Item &it = items_[i];
        if (int(i) == selected_)
          p.fill_rounded(it.x + 4, it.y + 2, cell_w_ - 8, cell_h_ - 6, 10, theme_.accent.alpha(0.45));
        else if (int(i) == hover_)
          p.fill_rounded(it.x + 4, it.y + 2, cell_w_ - 8, cell_h_ - 6, 10, Color::hex(0xFFFFFF, 0.16));
        p.image(IconLoader::get(it.icons, 48), it.x + (cell_w_ - 48) / 2.0, it.y + 8, 48);
      }
    }
    text_font(theme_.font_size);
    for (auto &it : items_) {
      std::string label = ellipsize(it.label, cell_w_ - 12);
      draw_text(label, it.x + 7, it.y + 61, cell_w_ - 12, 22, Color::hex(0x000000, 1), 1);
      draw_text(label, it.x + 6, it.y + 60, cell_w_ - 12, 22, Color::hex(0xFFFFFF), 1);
    }
  }

  int item_at(int x, int y) const {
    for (size_t i = 0; i < items_.size(); ++i)
      if (x >= items_[i].x + 4 && x < items_[i].x + cell_w_ - 4 && y >= items_[i].y &&
          y < items_[i].y + cell_h_ - 4)
        return int(i);
    return -1;
  }

  void open_item(int i) {
    if (i < 0 || i >= int(items_.size())) return;
    Item it = items_[size_t(i)];
    if (it.is_launcher) {
      // Launchers copied to the desktop must be marked executable (trusted)
      // before they run, like in other desktops.
      if (access(it.path.c_str(), X_OK) != 0) {
        if (fl_choice("\"%s\" is not marked as trusted.\nDo you want to trust and launch it?",
                      "Cancel", "Trust and Launch", nullptr, it.label.c_str()) != 1)
          return;
        struct stat st;
        if (stat(it.path.c_str(), &st) == 0) chmod(it.path.c_str(), st.st_mode | S_IXUSR);
      }
      DesktopEntry e;
      if (e.load(it.path)) launch_entry(e);
      return;
    }
    if (!open_uri(it.uri)) fl_alert("No application is available to open \"%s\".", it.label.c_str());
  }

  void context_menu(int i) {
    if (i >= 0) {
      Fl_Menu_Item items[] = {{"Open", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                              {"Move to Trash", 0, nullptr, nullptr,
                               items_[size_t(i)].path.empty() || i < first_file_index() ? FL_MENU_INACTIVE : 0,
                               0, 0, 0, 0},
                              {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
      const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
      if (m == &items[0]) open_item(i);
      else if (m == &items[1]) spawn_detached({"gio", "trash", items_[size_t(i)].path});
      return;
    }
    Fl_Menu_Item items[] = {
        {"Open Terminal", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {"Open Desktop Folder", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {"New Folder...", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
        {"Change Wallpaper...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {"Desktop Settings", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
        {"Refresh", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
    std::string desk = user_dir("DESKTOP");
    if (m == &items[0]) open_terminal({}, dir_exists(desk) ? desk : home_dir());
    else if (m == &items[1]) {
      make_dirs(desk);
      open_uri(desk);
    } else if (m == &items[2]) {
      const char *name = fl_input("Folder name:", "New Folder");
      if (name && *name && !strchr(name, '/')) {
        make_dirs(desk);
        if (mkdir((desk + "/" + name).c_str(), 0755) != 0) fl_alert("Could not create the folder.");
      }
    } else if (m == &items[3]) {
      spawn_detached({"sde-settings", "--page", "appearance"});
    } else if (m == &items[4]) {
      spawn_detached({"sde-settings", "--page", "desktop"});
    } else if (m == &items[5]) {
      load();
    }
  }

  int first_file_index() const {
    int n = 0;
    for (auto &it : items_)
      if (it.path.empty() || it.path == home_dir()) ++n;
    return n;
  }

  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        int i = item_at(Fl::event_x(), Fl::event_y());
        if (i != selected_) {
          selected_ = i;
          redraw();
        }
        if (Fl::event_button() == FL_RIGHT_MOUSE) context_menu(i);
        else if (Fl::event_clicks() && i >= 0) open_item(i);
        return 1;
      }
      case FL_ENTER:
      case FL_MOVE: {
        int i = item_at(Fl::event_x(), Fl::event_y());
        if (i != hover_) {
          hover_ = i;
          redraw();
        }
        return 1;
      }
      case FL_LEAVE:
        hover_ = -1;
        redraw();
        return 1;
      case FL_KEYBOARD:
        if ((Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) && selected_ >= 0) {
          open_item(selected_);
          return 1;
        }
        return 0;
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
  std::vector<Item> items_;
  std::vector<cairo_surface_t *> wallpapers_;
  std::string desktop_dir_;
  int selected_ = -1, hover_ = -1;
  int cell_w_ = 96, cell_h_ = 96;
  int inotify_fd_ = -1, watch_ = -1;
};

Desktop *Desktop::instance_ = nullptr;

int ignore_x_errors(Display *, XErrorEvent *) { return 0; }

}  // namespace

int main(int argc, char **argv) {
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
