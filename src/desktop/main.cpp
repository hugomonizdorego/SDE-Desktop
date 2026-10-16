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
#include "i18n.h"
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
  // For sorting.
  bool is_dir = false;
  long long size = 0;
  time_t modified = 0;
};

// Picks a name that does not exist yet: "New Folder", "New Folder 2", ...
std::string unique_path(const std::string &dir, const std::string &base, const std::string &ext = "") {
  std::string path = dir + "/" + base + ext;
  for (int n = 2; file_exists(path) || dir_exists(path); ++n)
    path = dir + "/" + base + " " + std::to_string(n) + ext;
  return path;
}

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
      items_.push_back({_("Home"), home, home, {"user-home", "folder-home", "folder"}, false});
    if (cfg_.get_bool("desktop", "show_trash", false))
      items_.push_back({_("Trash"), "", "trash:///", {"user-trash", "user-trash-full"}, false});
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
          struct stat st;
          if (stat(full.c_str(), &st) == 0) {
            it.is_dir = S_ISDIR(st.st_mode);
            it.size = st.st_size;
            it.modified = st.st_mtime;
          }
          items_.push_back(it);
        }
      }
    }
    sort_items();
    selected_ = -1;
    layout_items();
  }

  // Home and Trash stay first; files follow the "Sort by" choice, with
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
    icon_ = size == "small" ? S(32) : size == "large" ? S(64) : S(48);
    cell_w_ = icon_ + S(48);
    cell_h_ = icon_ + S(48);
    int usable = mh - theme_.panel_height - S(24);
    int rows = std::max(1, usable / cell_h_);
    for (size_t i = 0; i < items_.size(); ++i) {
      items_[i].x = mx + S(12) + int(i) / rows * cell_w_;
      items_[i].y = my + S(12) + (theme_.panel_top ? theme_.panel_height : 0) + int(i) % rows * cell_h_;
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
          p.fill_rounded(it.x + S(4), it.y + S(2), cell_w_ - S(8), cell_h_ - S(6), S(10), theme_.accent.alpha(0.45));
        else if (int(i) == hover_)
          p.fill_rounded(it.x + 4, it.y + 2, cell_w_ - 8, cell_h_ - 6, 10, Color::hex(0xFFFFFF, 0.16));
        p.image(IconLoader::get(it.icons, icon_), it.x + (cell_w_ - icon_) / 2.0, it.y + S(8), icon_);
      }
    }
    text_font(theme_.font_size);
    for (auto &it : items_) {
      std::string label = ellipsize(it.label, cell_w_ - S(12));
      draw_text(label, it.x + S(6) + 1, it.y + icon_ + S(12) + 1, cell_w_ - S(12), S(22), Color::hex(0x000000, 1), 1);
      draw_text(label, it.x + S(6), it.y + icon_ + S(12), cell_w_ - S(12), S(22), Color::hex(0xFFFFFF), 1);
    }
  }

  int item_at(int x, int y) const {
    for (size_t i = 0; i < items_.size(); ++i)
      if (x >= items_[i].x + S(4) && x < items_[i].x + cell_w_ - S(4) && y >= items_[i].y &&
          y < items_[i].y + cell_h_ - S(4))
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
    if (!open_uri(it.uri)) fl_alert(_("No application is available to open \"%s\"."), it.label.c_str());
  }

  bool is_file_item(int i) const { return i >= first_file_index() && i < int(items_.size()); }

  void rename_item(int i) {
    if (!is_file_item(i)) return;
    const Item &it = items_[size_t(i)];
    std::string old_name = basename_of(it.path);
    const char *name = fl_input("%s", old_name.c_str(), _("New name:"));
    if (!name || !*name || old_name == name) return;
    if (strchr(name, '/')) {
      fl_alert("%s", _("A name cannot contain \"/\"."));
      return;
    }
    std::string target = desktop_dir_ + "/" + name;
    if (file_exists(target) || dir_exists(target)) {
      fl_alert(_("\"%s\" already exists."), name);
      return;
    }
    if (rename(it.path.c_str(), target.c_str()) != 0) fl_alert("%s", _("Could not rename the item."));
  }

  void trash_item(int i) {
    if (!is_file_item(i)) return;
    if (!spawn_detached({"gio", "trash", items_[size_t(i)].path}))
      fl_alert("%s", _("Moving to the trash needs the gio program (package libglib2.0-bin)."));
  }

  void new_folder() {
    std::string desk = user_dir("DESKTOP");
    make_dirs(desk);
    std::string path = unique_path(desk, _("New Folder"));
    const char *name = fl_input("%s", basename_of(path).c_str(), _("Folder name:"));
    if (!name || !*name) return;
    if (strchr(name, '/') || mkdir((desk + "/" + name).c_str(), 0755) != 0)
      fl_alert("%s", _("Could not create the folder."));
  }

  void new_document() {
    std::string desk = user_dir("DESKTOP");
    make_dirs(desk);
    std::string path = unique_path(desk, _("New Document"), ".txt");
    const char *name = fl_input("%s", basename_of(path).c_str(), _("Document name:"));
    if (!name || !*name || strchr(name, '/')) return;
    std::string target = desk + "/" + name;
    if (file_exists(target)) {
      fl_alert(_("\"%s\" already exists."), name);
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
      Fl_Menu_Item items[] = {
          {_("Open"), FL_Enter, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
          {_("Rename..."), FL_F + 2, nullptr, nullptr, fflag, 0, 0, 0, 0},
          {_("Copy Location"), 0, nullptr, nullptr, fflag | FL_MENU_DIVIDER, 0, 0, 0, 0},
          {_("Move to Trash"), FL_Delete, nullptr, nullptr, fflag, 0, 0, 0, 0},
          {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
      const Fl_Menu_Item *m = items->popup(mx, my);
      if (m == &items[0]) open_item(i);
      else if (m == &items[1]) rename_item(i);
      else if (m == &items[2]) {
        const std::string &path = items_[size_t(i)].path;
        Fl::copy(path.c_str(), int(path.size()), 1);  // clipboard
        Fl::copy(path.c_str(), int(path.size()), 0);  // primary selection
      } else if (m == &items[3]) trash_item(i);
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
      case 11: load(); break;
      case 13: new_folder(); break;
      case 14: new_document(); break;
      case 16: open_terminal({}, dir_exists(desk) ? desk : home_dir()); break;
      case 17:
        make_dirs(desk);
        open_uri(desk);
        break;
      case 18: spawn_detached({"sde-settings", "--page", "display"}); break;
      case 19: spawn_detached({"sde-settings", "--page", "appearance"}); break;
      case 20: spawn_detached({"sde-settings", "--page", "desktop"}); break;
      default: break;
    }
  }

  // Keyboard shortcuts (the same keys shown in the context menu).
  int key(int k, int state) {
    bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT;
    int n = int(items_.size());
    if (k == FL_F + 5) {
      load();
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
    selected_ = s;
    redraw();
    return 1;
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
        if (Fl::event_button() == FL_RIGHT_MOUSE) context_menu(i, Fl::event_x(), Fl::event_y());
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
  int cell_w_ = 96, cell_h_ = 96, icon_ = 48;
  int inotify_fd_ = -1, watch_ = -1;
};

Desktop *Desktop::instance_ = nullptr;

int ignore_x_errors(Display *, XErrorEvent *) { return 0; }

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
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
