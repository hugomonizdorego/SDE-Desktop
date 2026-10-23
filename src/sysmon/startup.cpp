// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE System Monitor: the Startup apps page (XDG autostart entries).
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sysmon {

namespace {

struct StartupEntry {
  std::string file, name, comment, icon, user_path, system_path;
  bool enabled = true;
};

std::string user_autostart_dir() { return sde::join_path(sde::config_home(), "autostart"); }

bool shown_in_sde(const sde::DesktopEntry &e) {
  auto has = [](const std::vector<std::string> &v) {
    return std::find(v.begin(), v.end(), "SDE") != v.end();
  };
  if (!e.only_show_in.empty() && !has(e.only_show_in)) return false;
  return !has(e.not_show_in);
}

std::vector<StartupEntry> load_entries() {
  std::map<std::string, StartupEntry> by_file;
  // System directories first; the user's copies override them.
  std::vector<std::string> dirs;
  auto system = sde::config_dirs();
  for (auto it = system.rbegin(); it != system.rend(); ++it) dirs.push_back(sde::join_path(*it, "autostart"));
  dirs.push_back(user_autostart_dir());
  for (size_t d = 0; d < dirs.size(); ++d) {
    bool user = d + 1 == dirs.size();
    for (auto &file : sde::list_dir(dirs[d])) {
      if (!sde::ends_with(file, ".desktop")) continue;
      std::string path = sde::join_path(dirs[d], file);
      sde::DesktopEntry e;
      std::string text;
      if (!e.load(path, file) || !sde::read_file(path, text)) continue;
      StartupEntry &s = by_file[file];
      s.file = file;
      if (user) s.user_path = path;
      else s.system_path = path;
      if (!e.name.empty()) s.name = e.name;
      if (!e.comment.empty()) s.comment = e.comment;
      if (!e.icon.empty()) s.icon = e.icon;
      s.enabled = autostart_enabled(text);
      if (!shown_in_sde(e)) s.file.clear();  // not started in SDE: hide
    }
  }
  std::vector<StartupEntry> out;
  for (auto &kv : by_file)
    if (!kv.second.file.empty()) out.push_back(kv.second);
  std::sort(out.begin(), out.end(), [](const StartupEntry &a, const StartupEntry &b) {
    return sde::lower(a.name) < sde::lower(b.name);
  });
  return out;
}

// Writes the user's copy with Hidden= set; system entries get an override.
bool set_enabled(const StartupEntry &e, bool on) {
  std::string source = e.user_path.empty() ? e.system_path : e.user_path, text;
  if (!sde::read_file(source, text)) return false;
  std::string dir = user_autostart_dir();
  if (!sde::make_dirs(dir)) return false;
  return sde::write_file_atomic(sde::join_path(dir, e.file), set_autostart_hidden(text, !on));
}

class StartupList : public Fl_Widget {
 public:
  std::function<void()> on_change;
  StartupList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_NO_BOX); }

  void reload() {
    items_ = load_entries();
    clamp();
    redraw();
  }
  const std::vector<StartupEntry> &items() const { return items_; }

  void draw() override {
    const sde::Theme &t = sde::active_theme();
    fl_push_clip(x(), y(), w(), h());
    int rh = row_h();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), t.background);
      int total = int(items_.size()) * rh;
      if (!items_.empty()) {
        int ch = std::min(total, h()) ;
        p.fill_rounded(x() + 0.5, y() + 0.5 - (offset_ > 0 ? S(20) : 0), w() - 1, ch - 1 + (offset_ > 0 ? S(20) : 0), Sd(14), t.surface);
      }
      for (size_t i = 0; i < items_.size(); ++i) {
        int ry = y() + int(i) * rh - offset_;
        if (ry + rh < y() || ry > y() + h()) continue;
        const StartupEntry &e = items_[i];
        if (int(i) == hover_) p.fill_rounded(x() + S(6), ry + S(3), w() - S(12), rh - S(6), Sd(10), t.hover);
        if (i) p.line(x() + S(70), ry + 0.5, x() + w() - S(18), ry + 0.5, t.border.alpha(0.7));
        cairo_surface_t *icon = sde::IconLoader::get({e.icon, "application-x-executable"}, S(32));
        if (icon) p.image(icon, x() + S(22), ry + (rh - S(32)) / 2.0, S(32));
        else draw_icon(p, Icon::Startup, x() + S(26), ry + (rh - S(24)) / 2.0, S(24), t.text_dim);
        // Switch.
        double sw = S(42), sh = S(24), sx = x() + w() - S(24) - sw, sy = ry + (rh - sh) / 2;
        p.fill_rounded(sx, sy, sw, sh, sh / 2, e.enabled ? t.accent : t.border);
        p.circle(e.enabled ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh / 2 - S(3), e.enabled ? t.on_accent : t.surface);
      }
      if (!items_.empty()) p.stroke_rounded(x() + 0.5, y() + 0.5 - (offset_ > 0 ? S(20) : 0), w() - 1, std::min(total, h()) - 1 + (offset_ > 0 ? S(20) : 0), Sd(14), t.border);
    }
    for (size_t i = 0; i < items_.size(); ++i) {
      int ry = y() + int(i) * rh - offset_;
      if (ry + rh < y() || ry > y() + h()) continue;
      const StartupEntry &e = items_[i];
      int tx = x() + S(70), tw = w() - S(70) - S(220);
      sde::text_font(t.font_size, true);
      sde::draw_text(e.name.empty() ? e.file : e.name, tx, ry + S(10), tw, S(22), t.text);
      sde::text_font(t.font_size - S(1));
      sde::draw_text(e.comment.empty() ? e.file : e.comment, tx, ry + S(32), tw, S(18), t.text_dim);
      std::string origin = e.user_path.empty() ? _("System") : e.system_path.empty() ? _("Yours") : _("System, changed");
      sde::draw_text(origin, x() + w() - S(220), ry, S(140), rh, t.text_dim, 2);
    }
    if (items_.empty()) {
      sde::text_font(t.font_size);
      sde::draw_text(_("No apps start automatically."), x(), y() + S(20), w(), S(30), t.text_dim, 1);
    }
    fl_pop_clip();
  }

  int handle(int e) override {
    switch (e) {
      case FL_ENTER: return 1;
      case FL_LEAVE: set_hover(-1); return 1;
      case FL_MOVE: set_hover(hit(Fl::event_y())); return 1;
      case FL_MOUSEWHEEL:
        offset_ += Fl::event_dy() * S(40);
        clamp();
        redraw();
        return 1;
      case FL_PUSH: {
        int i = hit(Fl::event_y());
        if (i < 0) return 1;
        // The whole row toggles, not just the switch: a bigger target.
        StartupEntry &entry = items_[i];
        if (!set_enabled(entry, !entry.enabled)) {
          fl_alert(_("Cannot change \"%s\" in %s."), entry.name.c_str(), user_autostart_dir().c_str());
          return 1;
        }
        reload();
        if (on_change) on_change();
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }

 private:
  int row_h() const { return S(62); }
  int hit(int ey) const {
    int rel = ey - y() + offset_;
    if (rel < 0) return -1;
    int i = rel / row_h();
    return i < int(items_.size()) ? i : -1;
  }
  void set_hover(int i) {
    if (i == hover_) return;
    hover_ = i;
    redraw();
  }
  void clamp() { offset_ = std::max(0, std::min(offset_, int(items_.size()) * row_h() - h())); }
  std::vector<StartupEntry> items_;
  int offset_ = 0, hover_ = -1;
};

class StartupPage : public Page {
 public:
  StartupPage(int x, int y, int w, int h) : Page(x, y, w, h, _("Startup Apps")) {
    folder_ = new sde::FlatButton(x + w - S(28) - S(150), y + S(18), S(150), S(36), _("Open Folder"));
    folder_->callback([](Fl_Widget *, void *) {
      sde::make_dirs(user_autostart_dir());
      open_folder(user_autostart_dir());
    });
    list_ = new StartupList(x + S(28), y + header_height(), w - S(56), h - header_height() - S(20));
    list_->on_change = [this] { summarize(); };
    resizable(list_);
    end();
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    subtitle_->resize(X + S(28), Y + S(48), W - S(56) - S(160), S(22));
    folder_->resize(X + W - S(28) - S(150), Y + S(18), S(150), S(36));
    list_->resize(X + S(28), Y + header_height(), W - S(56), H - header_height() - S(20));
  }
  void activated() override {
    list_->reload();
    summarize();
  }
  void update(const std::shared_ptr<const Snapshot> &, bool) override {}

 private:
  void summarize() {
    int on = 0;
    for (auto &e : list_->items()) on += e.enabled;
    subtitle_->text(sde::format(_("%d of %d apps start when you log in · click a row to switch it"), on,
                                int(list_->items().size())));
  }
  sde::FlatButton *folder_;
  StartupList *list_;
};

}  // namespace

Page *make_startup_page(int x, int y, int w, int h) { return new StartupPage(x, y, w, h); }

}  // namespace sysmon
