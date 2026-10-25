// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Default Applications: one list per kind of file (web browser,
// mail, music ...). Choices are saved at once to ~/.config/mimeapps.list
// (read by xdg-open, xdg-settings, GLib/GTK, Qt and Liria); the terminal is
// [applications] terminal in sde.conf. "Reset to Defaults" removes the
// user's choices so that the system and SDE defaults
// (/etc/xdg/sde-mimeapps.list) apply again.
#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multi_Label.H>
#include <FL/fl_ask.H>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

#include "config.h"
#include "default_apps.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

struct AppsCache {
  std::string stamp;
  std::vector<MimeApp> apps;
};

AppsCache &cache() {
  static AppsCache c;
  return c;
}

const std::vector<MimeApp> &installed_apps() {
  AppsCache &c = cache();
  std::string stamp = applications_stamp();
  for (auto &d : application_dirs()) stamp += d + (dir_exists(d) ? "+" : "-");
  if (stamp != c.stamp || c.apps.empty()) {
    c.apps = scan_mime_apps(application_dirs());
    c.stamp = stamp;
  }
  return c.apps;
}

std::vector<MimeAppsList> load_lists() {
  std::vector<MimeAppsList> lists;
  for (auto &p : mimeapps_paths()) {
    MimeAppsList l;
    if (l.load(p)) lists.push_back(std::move(l));
  }
  return lists;
}

std::string user_list_path() { return config_home() + "/mimeapps.list"; }

// User files that win over ~/.config/mimeapps.list ("sde-mimeapps.list"
// and the like); their entries for the changed types are removed so that
// the new choice takes effect.
std::vector<std::string> user_desktop_lists() {
  std::vector<std::string> out;
  for (auto &d : split(env("XDG_CURRENT_DESKTOP", "SDE"), ':'))
    if (!trim(d).empty()) out.push_back(config_home() + "/" + lower(trim(d)) + "-mimeapps.list");
  return out;
}

bool save_user(const std::function<void(MimeAppsList &)> &edit, const std::vector<std::string> &types) {
  MimeAppsList user;
  user.load(user_list_path());
  edit(user);
  bool ok = !user.changed() || user.save(user_list_path());
  for (auto &p : user_desktop_lists()) {
    MimeAppsList l;
    if (!l.load(p)) continue;
    for (auto &t : types) l.unset_default(t);
    if (l.changed()) ok = l.save(p) && ok;
  }
  return ok;
}

const MimeApp *app_by_id(const std::string &id) {
  for (auto &a : installed_apps())
    if (a.id == id) return &a;
  return nullptr;
}

// FLTK image of an icon for menu entries.
Fl_RGB_Image *menu_icon(const std::vector<std::string> &names, int size) {
  static std::map<std::string, Fl_RGB_Image *> images;
  std::string key = std::to_string(size);
  for (auto &n : names) key += "|" + n;
  auto it = images.find(key);
  if (it != images.end()) return it->second;
  cairo_surface_t *s = IconLoader::get(names, size);
  Fl_RGB_Image *img = nullptr;
  if (s) {
    cairo_surface_t *flat = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(flat);
    double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
    cairo_scale(cr, size / std::max(1.0, sw), size / std::max(1.0, sh));
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(flat);
    auto *data = new uchar[size_t(size) * size_t(size) * 4];
    unsigned char *src = cairo_image_surface_get_data(flat);
    int stride = cairo_image_surface_get_stride(flat);
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        uint32_t px = *reinterpret_cast<uint32_t *>(src + y * stride + x * 4);
        unsigned a = px >> 24, r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255;
        uchar *d = data + (size_t(y) * size_t(size) + size_t(x)) * 4;
        d[0] = uchar(a ? r * 255 / a : 0);
        d[1] = uchar(a ? g * 255 / a : 0);
        d[2] = uchar(a ? b * 255 / a : 0);
        d[3] = uchar(a);
      }
    cairo_surface_destroy(flat);
    img = new Fl_RGB_Image(data, size, size, 4);
    img->alloc_array = 1;
    // Registers the image label type that Fl_Multi_Label draws with.
    Fl_Menu_Item probe = {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0};
    img->label(&probe);
  }
  images[key] = img;
  return img;
}

std::vector<std::string> app_icons(const MimeApp *a) {
  if (!a) return {"application-x-executable"};
  // As in the Start menu: near matches and the category icon when the
  // icon theme lacks Icon= (icons.h).
  DesktopEntry e;
  e.id = a->id;
  e.path = a->path;
  e.icon = a->icon;
  e.categories = a->categories;
  auto names = app_icon_list(e);
  names.push_back("application-x-executable");
  return names;
}

// A choice that shows the selected application's icon; the menu entries
// show icons too.
class AppChoice : public Fl_Choice {
 public:
  AppChoice(int x, int y, int w, int h) : Fl_Choice(x, y, w, h) { style_choice(this); }
  ~AppChoice() override { restore_labels(); }

  void set_apps(const std::vector<const MimeApp *> &apps, const std::string &current,
                const std::string &none_label) {
    apps_ = apps;
    restore_labels();
    clear();
    labels_.clear();
    texts_.clear();
    if (apps_.empty()) {
      add(menu_escape(none_label).c_str());
      value(0);
      deactivate();
      return;
    }
    labels_.reserve(apps_.size());
    int sel = -1;
    for (size_t i = 0; i < apps_.size(); ++i) {
      add(menu_escape(apps_[i]->name).c_str());
      if (apps_[i]->id == current) sel = int(i);
    }
    // Icons in the menu (Fl_Multi_Label: image, then text).
    labels_.resize(apps_.size());
    texts_.resize(apps_.size(), nullptr);
    for (size_t i = 0; i < apps_.size(); ++i) {
      Fl_RGB_Image *img = menu_icon(app_icons(apps_[i]), S(18));
      if (!img) continue;
      Fl_Menu_Item *item = const_cast<Fl_Menu_Item *>(&menu()[i]);
      texts_[i] = item->text;  // the menu's own copy, freed by Fl_Menu_::clear()
      Fl_Multi_Label &ml = labels_[i];
      ml.labela = reinterpret_cast<const char *>(img);
      ml.typea = _FL_IMAGE_LABEL;
      text_.push_back("  " + apps_[i]->name);
      ml.labelb = strdup(text_.back().c_str());  // lives with the program (pages are rebuilt rarely)
      ml.typeb = FL_NORMAL_LABEL;
      ml.label(item);
    }
    if (sel < 0 && current.empty()) {
      insert(0, menu_escape(_("Choose an application")).c_str(), 0, nullptr);
      apps_.insert(apps_.begin(), nullptr);
      sel = 0;
    } else if (sel < 0) {
      // The default is not one of the offered applications (NoDisplay,
      // or it does not declare the type): show it anyway.
      const MimeApp *a = app_by_id(current);
      std::string n = a ? a->name : current;
      insert(0, menu_escape(n).c_str(), 0, nullptr);
      apps_.insert(apps_.begin(), a);
      extra_id_ = current;
      sel = 0;
    }
    value(sel);
    activate();
  }

  const MimeApp *selected() const {
    int v = value();
    return v >= 0 && v < int(apps_.size()) ? apps_[size_t(v)] : nullptr;
  }

  void draw() override {
    const Theme &t = active_theme();
    bool hot = Fl::belowmouse() == this && active_r();
    const MimeApp *a = selected();
    std::string text = a ? a->name : std::string(mvalue() && mvalue()->label() ? mvalue()->label() : "");
    if (!a && !extra_id_.empty() && value() == 0) text = extra_id_;
    {
      Painter p(window()->w(), window()->h());
      double r = Sd(8);
      p.fill_rounded(x(), y(), w(), h(), r, hot ? t.hover.mix(t.surface_alt, 0.4) : t.surface_alt);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, hot ? t.text_dim.alpha(0.5) : t.border);
      if (a) {
        int is = S(20);
        p.image(IconLoader::get(app_icons(a), is), x() + S(10), y() + (h() - is) / 2.0, is,
                active_r() ? 1.0 : 0.5);
      }
      // Down chevron.
      double cx = x() + w() - S(18), cy = y() + h() / 2.0, k = Sd(4);
      p.line(cx - k, cy - k / 2, cx, cy + k / 2, t.text_dim, Sd(1.5));
      p.line(cx, cy + k / 2, cx + k, cy - k / 2, t.text_dim, Sd(1.5));
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
    }
    text_font(t.font_size + 1);
    draw_text(text, x() + (a ? S(38) : S(12)), y(), w() - S(66), h(), a && active_r() ? t.text : t.text_dim);
  }

 private:
  // Fl_Menu_::clear() (also from the destructor) frees the text of every
  // item, including items whose label became an Fl_Multi_Label of
  // |labels_|: they get their own text back first. Without this, building
  // the page again (a resize of the window) freed a pointer into |labels_|.
  void restore_labels() {
    const Fl_Menu_Item *m = menu();
    if (!m) return;
    for (int i = 0; i < size(); ++i) {
      auto *item = const_cast<Fl_Menu_Item *>(&m[i]);
      if (item->labeltype_ != _FL_MULTI_LABEL) continue;
      for (size_t k = 0; k < labels_.size(); ++k)
        if (item->text == reinterpret_cast<const char *>(&labels_[k]) && texts_[k]) {
          item->label(FL_NORMAL_LABEL, texts_[k]);
          break;
        }
    }
  }
  std::vector<const MimeApp *> apps_;
  std::vector<Fl_Multi_Label> labels_;
  std::vector<const char *> texts_;  // the item texts the labels replaced
  std::vector<std::string> text_;
  std::string extra_id_;
};

class IconBox : public Fl_Widget {
 public:
  IconBox(int x, int y, int s, std::vector<std::string> names) : Fl_Widget(x, y, s, s), names_(std::move(names)) {}
  void draw() override {
    Painter p(window()->w(), window()->h());
    p.image(IconLoader::get(names_, w()), x(), y(), w());
  }

 private:
  std::vector<std::string> names_;
};

std::string current_terminal() {
  std::string id = settings_config().get("applications", "terminal", "auto");
  if (!id.empty() && id != "auto") return id;
  // What "auto" starts: the x-terminal-emulator alternative when it is a
  // known application.
  std::string target = find_program("x-terminal-emulator");
  char buf[512];
  for (int i = 0; i < 4 && !target.empty(); ++i) {
    ssize_t n = readlink(target.c_str(), buf, sizeof buf - 1);
    if (n <= 0) break;
    buf[n] = 0;
    std::string next = buf;
    if (next[0] != '/') next = target.substr(0, target.rfind('/') + 1) + next;
    target = next;
  }
  std::string prog = basename_of(target);
  if (prog.empty()) return "";
  for (auto &a : installed_apps()) {
    if (std::find(a.categories.begin(), a.categories.end(), "TerminalEmulator") == a.categories.end()) continue;
    DesktopEntry e;
    if (e.load(a.path, a.id)) {
      auto cmd = e.command({});
      if (!cmd.empty() && basename_of(cmd[0]) == prog) return a.id;
    }
  }
  return "";
}

std::string current_default(const AppCategory &cat, const std::vector<MimeAppsList> &lists) {
  if (cat.id == "terminal") return current_terminal();
  if (cat.types.empty()) return "";
  return resolve_default(cat.types[0], lists, installed_apps());
}

void choose(const AppCategory *cat, const MimeApp *app) {
  if (!app) return;
  if (cat->id == "terminal") {
    settings_config().set("applications", "terminal", app->id);
    apply_change();
    return;
  }
  if (!save_user([&](MimeAppsList &l) { apply_category(l, *cat, *app); }, cat->types))
    fl_alert(_("Could not save %s."), user_list_path().c_str());
}

void reset_all() {
  if (fl_choice("%s", _("Cancel"), _("Reset"), nullptr,
                _("Use the standard applications of SDE again for every kind of file?")) != 1)
    return;
  std::vector<std::string> types;
  for (auto &c : app_categories()) types.insert(types.end(), c.types.begin(), c.types.end());
  save_user([&](MimeAppsList &l) {
    for (auto &c : app_categories()) reset_category(l, c);
  }, types);
  settings_config().unset("applications", "terminal");
  apply_change();
  Fl::add_timeout(0, [](void *) { refresh_page(); });
}

// ------------------------------------------------------- other file types

class OtherTypesDialog {
 public:
  OtherTypesDialog() {
    int W = std::min(S(620), Fl::w() - S(40)), H = std::min(S(520), Fl::h() - S(80));
    win_ = new Fl_Double_Window(W, H, _("Other File Types"));
    const Theme &t = settings_theme();
    win_->color(t.background.fl());
    int pad = S(16);
    search_ = new Fl_Input(pad, pad, W - 2 * pad, S(32));
    search_->color(t.surface_alt.fl());
    search_->textcolor(t.text.fl());
    search_->textsize(t.font_size + 1);
    search_->box(FL_BORDER_BOX);
    search_->when(FL_WHEN_CHANGED);
    search_->callback([](Fl_Widget *, void *d) { static_cast<OtherTypesDialog *>(d)->fill(); }, this);
    search_->tooltip(_("Search file types"));
    list_ = new Fl_Hold_Browser(pad, pad + S(42), W - 2 * pad, H - S(150));
    list_->color(t.surface.fl());
    list_->textcolor(t.text.fl());
    list_->selection_color(t.accent.fl());
    list_->textsize(t.font_size);
    list_->box(FL_BORDER_BOX);
    static int widths[] = {0, 0};
    widths[0] = (W - 2 * pad) * 11 / 20;
    list_->column_widths(widths);
    list_->column_char('\t');
    list_->callback([](Fl_Widget *, void *d) { static_cast<OtherTypesDialog *>(d)->picked(); }, this);
    int by = H - S(92);
    label_ = new Label(pad, by, W - 2 * pad, S(24), _("Select a file type."), Label::DIM);
    choice_ = new AppChoice(pad, by + S(28), W - 3 * pad - S(110), S(34));
    choice_->callback([](Fl_Widget *, void *d) { static_cast<OtherTypesDialog *>(d)->set(); }, this);
    choice_->deactivate();
    auto *close = new FlatButton(W - pad - S(110), by + S(28), S(110), S(34), _("Close"));
    close->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    win_->end();
    win_->set_modal();
    lists_ = load_lists();
    types_ = known_mime_types(installed_apps());
    fill();
  }
  ~OtherTypesDialog() { delete win_; }

  void run() {
    win_->show();
    search_->take_focus();
    while (win_->shown()) Fl::wait();
  }

 private:
  void fill() {
    std::string q = lower(trim(search_->value()));
    list_->clear();
    shown_.clear();
    for (auto &t : types_) {
      std::string id = resolve_default(t, lists_, installed_apps());
      const MimeApp *a = app_by_id(id);
      std::string name = a ? a->name : id;
      if (!q.empty() && lower(t).find(q) == std::string::npos && lower(name).find(q) == std::string::npos)
        continue;
      shown_.push_back(t);
      std::string line = t + "\t@." + (name.empty() ? std::string(_("(none)")) : name);
      list_->add(line.c_str());
    }
  }

  void picked() {
    int i = list_->value();
    if (i <= 0 || i > int(shown_.size())) {
      choice_->deactivate();
      return;
    }
    mime_ = shown_[size_t(i - 1)];
    label_text_ = format(_("Open %s files with:"), mime_.c_str());
    label_->set(label_text_);
    choice_->set_apps(apps_for_type(mime_, installed_apps()), resolve_default(mime_, lists_, installed_apps()),
                      _("No application declares this type"));
    choice_->redraw();
  }

  void set() {
    const MimeApp *a = choice_->selected();
    if (!a || mime_.empty()) return;
    std::string id = a->id, mime = mime_;
    save_user([&](MimeAppsList &l) { l.set_default(mime, id); }, {mime});
    lists_ = load_lists();
    int line = list_->value();
    fill();
    list_->value(line);
  }

  Fl_Double_Window *win_;
  Fl_Input *search_;
  Fl_Hold_Browser *list_;
  Label *label_;
  AppChoice *choice_;
  std::vector<MimeAppsList> lists_;
  std::vector<std::string> types_, shown_;
  std::string mime_, label_text_;
};

}  // namespace

void build_default_apps_page(Page *page) {
  page->heading(_("Default Applications"), _("Choose the applications that open your files and links"));
  std::vector<MimeAppsList> lists = load_lists();
  struct Group {
    const char *title;
    std::vector<const char *> ids;
  };
  static const std::vector<Group> groups = {
      {N_("Internet"), {"browser", "mail", "calendar"}},
      {N_("Media"), {"music", "video", "image"}},
      {N_("Documents"), {"text", "document", "spreadsheet", "presentation", "pdf"}},
      {N_("System"), {"files", "terminal", "archive"}},
  };
  for (auto &g : groups) {
    page->begin_card(_(g.title));
    for (const char *id : g.ids) {
      const AppCategory *cat = find_category(id);
      if (!cat) continue;
      int x = page->left() + S(20), w = page->width() - S(40), y = page->cursor();
      int cw = std::min(S(300), w * 11 / 20);
      int is = S(28);
      new IconBox(x, y + (S(44) - is) / 2, is, cat->icons);
      new Label(x + is + S(12), y + S(10), w - cw - is - S(24), S(24), _(cat->title.c_str()));
      auto *choice = new AppChoice(x + w - cw, y + S(5), cw, S(34));
      choice->set_apps(apps_for_category(*cat, installed_apps()), current_default(*cat, lists),
                       _("No application installed"));
      choice->tooltip(nullptr);
      choice->callback([](Fl_Widget *wdg, void *d) {
        choose(static_cast<const AppCategory *>(d), static_cast<AppChoice *>(wdg)->selected());
      }, const_cast<AppCategory *>(cat));
      page->advance(S(46));
    }
    page->end_card();
  }
  page->begin_card(_("More"));
  page->row(_("Other file types"), S(150), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Choose..."));
    b->callback([](Fl_Widget *, void *) {
      OtherTypesDialog dialog;
      dialog.run();
      refresh_page();
    });
    return b;
  }, _("Pick the application for any single file type."));
  page->row(_("Reset to defaults"), S(150), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Reset"));
    b->callback([](Fl_Widget *, void *) { reset_all(); });
    return b;
  }, _("Use the standard SDE applications again."));
  page->end_card();
  page->note(format(_("Saved at once to %s, which xdg-open, xdg-settings, Liria and most applications read."),
                    user_list_path().c_str()));
  page->finish();
}

}  // namespace sde
