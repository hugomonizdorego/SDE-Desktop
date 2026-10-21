// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "about.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/sde_fltk.h>
#include <FL/x.H>

#include <algorithm>
#include <cstring>
#include <memory>

#include "buffered_window.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "spawn.h"
#include "theme.h"
#include "widgets.h"

namespace sde {

const char *const kDeveloper = "DebION Dev-Team";
const char *const kDeveloperId = "org.debion";
const char *const kCopyright = "\xC2\xA9 2026 DebION Dev-Team";
const char *const kProjectWebsite = "https://github.com/hugomonizdorego/SDE-Desktop";

std::string developer_line() { return format(_("Developer: %s"), kDeveloper); }

std::string license_name(const std::string &spdx) {
  if (spdx == "GPL-3.0-or-later" || spdx == "GPL-3.0+") return _("GNU GPL version 3 or later");
  if (spdx == "GPL-3.0-only" || spdx == "GPL-3.0") return _("GNU GPL version 3");
  if (spdx == "GPL-2.0-or-later" || spdx == "GPL-2.0+") return _("GNU GPL version 2 or later");
  if (spdx == "GPL-2.0-only" || spdx == "GPL-2.0") return _("GNU GPL version 2");
  if (spdx == "LGPL-2.1-or-later") return _("GNU LGPL version 2.1 or later");
  if (spdx == "MIT") return _("MIT License");
  return spdx;
}

std::string toolkit_credit() {
  return format(_("Built with SDE-FLTK %s, based on FLTK by Bill Spitzak and others"),
                SDE_FLTK_VERSION);
}

namespace {

std::vector<std::string> wrap(const std::string &text, int width) {
  std::vector<std::string> lines;
  std::string line, word;
  auto flush_word = [&] {
    if (word.empty()) return;
    std::string cand = line.empty() ? word : line + " " + word;
    if (!line.empty() && text_width(cand) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = cand;
    }
    word.clear();
  };
  for (char c : text) {
    if (c == '\n') {
      flush_word();
      lines.push_back(line);
      line.clear();
    } else if (c == ' ') {
      flush_word();
    } else {
      word += c;
    }
  }
  flush_word();
  if (!line.empty()) lines.push_back(line);
  return lines;
}

struct Row {
  std::string label, value;
  bool link = false;
};

class AboutWindow : public BufferedWindow {
 public:
  AboutWindow(int w, int h, const char *title, const AppInfo &info)
      : BufferedWindow(0, 0, w, h, title), info_(info) {}

  std::vector<std::string> desc_, credits_;
  std::vector<Row> rows_;
  int desc_y = 0, card_y = 0, card_h = 0, credits_y = 0;

  void draw() override {
    const Theme &t = active_theme().font.empty() ? fallback() : active_theme();
    int W = w();
    int row_h = S(26);
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      if (t.glass) p.glass_sheen(0, 0, w(), S(150), 0, t.dark);
      double ls = S(72);
      double lx = (W - ls) / 2, ly = S(26);
      if (info_.logo) {
        info_.logo(p, lx, ly, ls);
      } else {
        std::vector<std::string> names;
        if (!info_.icon.empty()) names.push_back(info_.icon);
        if (!info_.program.empty()) names.push_back(info_.program);
        names.push_back("application-x-executable");
        std::string path;
        for (const auto &n : names)
          if (!(path = IconLoader::lookup(n, int(ls))).empty()) break;
        cairo_surface_t *img = path.empty() ? nullptr : IconLoader::get({path}, int(ls));
        if (img) {
          p.image(img, lx, ly, ls);
        } else {
          // No icon theme: a tile with the initial.
          p.fill_gradient(lx, ly, ls, ls, S(18), t.accent.mix(t.on_accent, 0.15), t.accent);
          p.flush();
          text_font(int(ls * 0.5), true);
          draw_text(info_.name.substr(0, 1), int(lx), int(ly), int(ls), int(ls), t.on_accent, 1);
        }
      }
      p.fill_rounded(S(24), card_y, W - S(48), card_h, S(10), t.surface_alt);
      for (size_t i = 1; i < rows_.size(); ++i) {
        double yy = card_y + S(8) + row_h * double(i) + 0.5;
        p.line(S(36), yy, W - S(36), yy, t.border.alpha(0.7));
      }
    }
    int y = S(108);
    text_font(t.font_size + S(7), true);
    draw_text(info_.name, S(16), y, W - S(32), S(34), t.text, 1);
    y += S(34);
    text_font(t.font_size - S(1));
    std::string ver = info_.version.empty() ? info_.program
                                            : format(_("Version %s"), info_.version.c_str());
    draw_text(ver, S(16), y, W - S(32), S(20), t.text_dim, 1);
    text_font(t.font_size);
    y = desc_y;
    for (const auto &l : desc_) {
      draw_text(l, S(32), y, W - S(64), S(22), t.text, 1);
      y += S(22);
    }
    y = card_y + S(8);
    for (const auto &r : rows_) {
      text_font(t.font_size - S(1));
      draw_text(r.label, S(40), y, S(130), row_h, t.text_dim);
      text_font(t.font_size - S(1), r.label == _("Developer"));
      draw_text(r.value, S(40) + S(130), y, W - S(80) - S(130), row_h,
                r.link ? t.accent_text : t.text, 2);
      y += row_h;
    }
    if (!credits_.empty()) {
      y = credits_y;
      text_font(t.font_size - S(1), true);
      draw_text(_("Credits"), S(32), y, W - S(64), S(22), t.text);
      y += S(24);
      text_font(t.font_size - S(1));
      for (const auto &l : credits_) {
        draw_text(l, S(32), y, W - S(64), S(20), t.text_dim);
        y += S(20);
      }
    }
    draw_children();
  }

  int handle(int event) override {
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        (Fl::event_key() == FL_Escape || Fl::event_key() == FL_Enter ||
         Fl::event_key() == FL_KP_Enter)) {
      hide();
      return 1;
    }
    return BufferedWindow::handle(event);
  }

 private:
  AppInfo info_;
  static const Theme &fallback() {
    static Theme t;
    static bool init = false;
    if (!init) {
      Config cfg;
      cfg.load();
      double s = ui_scale();
      t = Theme::from_config(cfg);
      set_ui_scale(s);
      init = true;
    }
    return t;
  }
};

}  // namespace

void show_about(const AppInfo &info) {
  fl_open_display();
  const Theme &t = active_theme();
  int W = S(480);
  std::string title = format(_("About %s"), info.name.c_str());
  auto *win = new AboutWindow(W, S(100), title.c_str(), info);
  win->copy_label(title.c_str());

  int y = S(108) + S(34) + S(28);
  text_font(t.font_size);
  win->desc_ = wrap(info.description, W - S(64));
  win->desc_y = y;
  y += S(22) * int(win->desc_.size()) + S(16);

  win->rows_.push_back({_("Developer"), kDeveloper});
  if (!info.version.empty()) win->rows_.push_back({_("Version"), info.version});
  win->rows_.push_back({_("License"), license_name(info.license)});
  win->rows_.push_back({_("Copyright"), kCopyright});
  if (!info.website.empty()) {
    std::string site = info.website;
    for (const char *pre : {"https://", "http://"})
      if (site.rfind(pre, 0) == 0) site = site.substr(strlen(pre));
    win->rows_.push_back({_("Website"), site, true});
  }
  win->card_y = y;
  win->card_h = S(26) * int(win->rows_.size()) + S(16);
  y += win->card_h + S(18);

  if (!info.credits.empty()) {
    text_font(t.font_size - S(1));
    for (const auto &c : info.credits)
      for (const auto &l : wrap(c, W - S(64))) win->credits_.push_back(l);
    win->credits_y = y;
    y += S(24) + S(20) * int(win->credits_.size()) + S(14);
  }
  int H = y + S(34) + S(22);
  win->size(W, H);

  win->begin();
  int bw = S(110), bh = S(34), by = H - bh - S(18);
  auto *close = new FlatButton(W - S(24) - bw, by, bw, bh, _("Close"), true);
  close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  if (!info.website.empty()) {
    auto *site = new FlatButton(S(24), by, S(130), bh, _("Website"));
    static std::string url;
    url = info.website;
    site->callback([](Fl_Widget *, void *) { open_uri(url); });
  }
  win->end();
  win->color(t.surface.fl());

  Fl_Window *parent = nullptr;
  for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w))
    if (w != win && !w->menu_window() && !w->tooltip_window() && w->visible()) {
      parent = w;
      break;
    }
  if (parent)
    win->position(parent->x() + (parent->w() - W) / 2, parent->y() + (parent->h() - H) / 3);
  else
    win->position((Fl::w() - W) / 2, (Fl::h() - H) / 3);
  win->set_modal();
  win->show();
  close->take_focus();
  while (win->shown()) Fl::wait();
  Fl::delete_widget(win);
}

int handle_about_event(int event, const AppInfo &info) {
  if ((event == FL_SHORTCUT || event == FL_KEYBOARD) && Fl::event_key() == FL_F + 1) {
    show_about(info);
    return 1;
  }
  if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
    ContextMenu menu;
    MenuItemOptions o;
    o.icon = "info";
    o.shortcut = FL_F + 1;
    AppInfo app = info;
    menu.item(format(_("About %s"), info.name.c_str()), [app] { show_about(app); }, o);
    menu.popup();
    return 1;
  }
  return 0;
}

std::string version_line(const AppInfo &info) {
  return info.program + " " + info.version + " (" + kDeveloper + ")";
}

}  // namespace sde
