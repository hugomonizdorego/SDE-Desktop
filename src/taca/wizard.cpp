// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// More of Taca's dialogs: find, repair, the wizard, convert, settings,
// favorites, text preview, keyboard shortcuts and About.
#include <FL/Fl.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_draw.H>
#include <archive.h>

#include <algorithm>

#include "about.h"
#include "build_config.h"
#include "i18n.h"
#include "taca.h"
#include "util.h"

using namespace sde;

namespace taca {

// ------------------------------------------------------------------- find

bool ask_find(Fl_Window *parent, const std::string &where, FindChoice &c) {
  FitScale fit(c.in_archive ? 262 : 342, 520);
  Dialog d(S(520), c.in_archive ? S(262) : S(342), _("Find Files"), format(_("Look in %s"), where.c_str()));
  int left = S(24), fw = d.w() - S(48);
  d.begin();
  d.label(left, S(80), fw, _("File names to find"), true);
  auto *pat = new Fl_Input(left + S(8), S(110), fw - S(16), S(28));
  style_input(pat, d.theme());
  pat->value(c.pattern.c_str());
  pat->position(0, pat->size());
  d.field(pat);
  d.label(left, S(144), fw, _("Wildcards such as *.txt; separate several patterns with “;”"), true);
  Switch *sub = nullptr, *inside = nullptr;
  if (!c.in_archive) {
    sub = new Switch(left, S(186), S(44), S(24));
    sub->value(c.recursive);
    d.label(left + S(56), S(187), fw - S(56), _("Look in sub-folders"));
    inside = new Switch(left, S(222), S(44), S(24));
    inside->value(c.inside_archives);
    d.label(left + S(56), S(223), fw - S(56), _("Look inside archives too"));
  }
  d.end();
  d.buttons({_("Cancel"), _("Find")}, 1);
  Fl::focus(pat);
  if (d.run(parent) != 1) return false;
  c.pattern = trim(pat->value());
  if (c.pattern.empty()) c.pattern = "*";
  if (sub) c.recursive = sub->value();
  if (inside) c.inside_archives = inside->value();
  return true;
}

// ----------------------------------------------------------------- repair

bool ask_repair(Fl_Window *parent, const std::string &archive, const RepairPlan &plan, std::string &dest_dir) {
  FitScale fit(360, 580);
  Dialog d(S(580), S(360), _("Repair Archive"), format(_("“%s”"), basename_of(archive).c_str()));
  int left = S(24), fw = d.w() - S(48);
  d.begin();
  d.label(left, S(152), fw, _("Save the repaired copy in"), true);
  auto *dest = new Fl_Input(left + S(8), S(182), fw - S(130), S(28));
  style_input(dest, d.theme());
  dest->value(dest_dir.c_str());
  d.field(dest);
  auto *browse = new FlatButton(left + fw - S(108), S(178), S(108), S(34), _("Browse…"));
  browse->callback([](Fl_Widget *w, void *p) {
    auto *in = static_cast<Fl_Input *>(p);
    std::string dir = choose_folder(_("Save the Repaired Archive In"), in->value());
    if (!dir.empty()) in->value(dir.c_str());
    w->window()->redraw();
  }, dest);
  d.end();
  const Theme &t = d.theme();
  std::string name = repaired_name(archive, plan.output_format);
  d.extra_paint = [&](Painter &p) { icon::tool(p, icon::TRepair, left, S(82), S(40), t.dark, true); };
  d.extra_text = [&]() {
    fl_font(FL_HELVETICA, t.font_size);
    fl_color(t.text.fl());
    fl_draw(plan.summary.c_str(), left + S(56), S(80), fw - S(56), S(60),
            FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
    text_font(t.font_size - S(1));
    draw_text(format(_("New file: %s"), name.c_str()), left, S(222), fw, S(24), t.text_dim);
    draw_text(_("The damaged archive itself is never changed."), left, S(246), fw, S(24), t.text_dim);
  };
  d.buttons({_("Cancel"), _("Repair")}, 1);
  d.accept = [&](int) {
    if (dir_exists(dest->value())) return true;
    message(&d, _("Choose a folder"), format(_("The folder “%s” does not exist."), dest->value()), {_("OK")}, 0, true);
    return false;
  };
  if (d.run(parent) != 1) return false;
  dest_dir = dest->value();
  return true;
}

// ----------------------------------------------------------------- wizard

int ask_wizard_action(Fl_Window *parent) {
  FitScale fit(372, 560);
  Dialog d(S(560), S(372), _("Wizard"), _("What would you like to do?"));
  const Theme &t = d.theme();
  int choice = 0;
  struct Option {
    icon::Tool pic;
    const char *title, *text;
  } options[] = {
      {icon::TAdd, _("Create a new archive"), _("Choose files and folders and pack them.")},
      {icon::TExtract, _("Extract an archive"), _("Choose an archive and unpack it into a folder.")},
      {icon::TWizard, _("Convert an archive"), _("Repack an archive in another format, e.g. RAR to 7Z.")},
  };
  d.begin();
  std::vector<DrawnButton *> rows;
  for (int i = 0; i < 3; ++i) {
    int y = S(84) + i * S(70);
    auto *b = new DrawnButton(
        S(24), y, d.w() - S(48), S(62),
        [&, i](Painter &p, bool hover, bool) {
          DrawnButton *self = rows[size_t(i)];
          bool on = choice == i;
          p.fill_rounded(self->x(), self->y(), self->w(), self->h(), S(10),
                         on ? t.accent.alpha(t.dark ? 0.3 : 0.14) : hover ? t.hover : t.surface_alt);
          p.stroke_rounded(self->x() + 0.5, self->y() + 0.5, self->w() - 1, self->h() - 1, S(10),
                           on ? t.accent : t.border, on ? Sd(1.5) : 1);
          icon::tool(p, options[i].pic, self->x() + S(14), self->y() + S(13), S(36), t.dark, true);
        },
        [&, i](bool) {
          DrawnButton *self = rows[size_t(i)];
          text_font(t.font_size, true);
          draw_text(options[i].title, self->x() + S(64), self->y() + S(8), self->w() - S(76), S(24), t.text);
          text_font(t.font_size - S(1));
          draw_text(options[i].text, self->x() + S(64), self->y() + S(30), self->w() - S(76), S(22), t.text_dim);
        },
        S(10));
    rows.push_back(b);
  }
  for (int i = 0; i < 3; ++i) {
    rows[size_t(i)]->callback(
        [](Fl_Widget *w, void *data) {
          auto *pair = static_cast<std::pair<int *, int> *>(data);
          *pair->first = pair->second;
          if (Fl::event_clicks()) static_cast<Dialog *>(w->window())->finish(1);
          w->window()->redraw();
        },
        new std::pair<int *, int>(&choice, i));
  }
  d.end();
  d.buttons({_("Cancel"), _("Next")}, 1);
  int r = d.run(parent);
  for (auto *b : rows) delete static_cast<std::pair<int *, int> *>(b->user_data());
  return r == 1 ? choice + 1 : 0;
}

bool ask_convert(Fl_Window *parent, const std::string &archive, std::string &dest, CreateOptions &o) {
  auto formats = creatable_formats();
  std::vector<std::string> labels;
  for (Format f : formats) labels.push_back(format_info(f).label);
  FitScale fit(420, 580);
  int w = S(580);
  Dialog d(w, S(420), _("Convert Archive"), format(_("Repack “%s” in another format"), basename_of(archive).c_str()));
  int left = S(24), fw = w - S(48);
  d.begin();
  d.label(left, S(80), fw, _("New format"), true);
  auto *chips = new Chips(left, S(106), fw, S(66), labels);
  int rows = chips->rows_needed(fw);
  chips->size(fw, rows * S(36));
  int y = S(106) + rows * S(36) + S(10);
  d.label(left, y, fw, _("Save as"), true);
  auto *name = new Fl_Input(left + S(8), y + S(30), fw - S(16), S(28));
  style_input(name, d.theme());
  d.field(name);
  y += S(70);
  d.label(left, y, fw, _("Compression"), true);
  auto *level = new Slider(left + S(150), y + S(28), fw - S(300), S(26));
  level->bounds(0, 9);
  level->step(1);
  level->value(o.level);
  d.end();
  std::string dir = archive.substr(0, archive.rfind('/'));
  std::string stem = strip_archive_extension(basename_of(archive));
  Format src = detect_format(archive);
  int chosen = 0;
  for (size_t i = 0; i < formats.size(); ++i)
    if (formats[i] == o.format && formats[i] != src) chosen = int(i);
  if (formats[size_t(chosen)] == src) chosen = chosen + 1 < int(formats.size()) ? chosen + 1 : 0;
  chips->value(chosen);
  auto set_name = [&]() {
    name->value(join_path(dir, stem + format_info(formats[size_t(chips->value())]).extension).c_str());
  };
  set_name();
  chips->callback([](Fl_Widget *, void *p) { (*static_cast<std::function<void()> *>(p))(); }, new std::function<void()>(set_name));
  const Theme &t = d.theme();
  d.extra_text = [&]() {
    text_font(t.font_size - S(1));
    draw_text(_("Faster"), left, level->y(), S(140), level->h(), t.text_dim, 2);
    draw_text(_("Smaller"), level->x() + level->w() + S(10), level->y(), S(140), level->h(), t.text_dim);
  };
  d.buttons({_("Cancel"), _("Convert")}, 1);
  d.accept = [&](int) {
    std::string n = trim(name->value());
    if (n.empty() || n[0] != '/') {
      message(&d, _("Choose a name"), _("Enter the full path of the new archive."), {_("OK")}, 0, true);
      return false;
    }
    if (n == archive) {
      message(&d, _("Choose another name"), _("The new archive cannot replace the one being converted."), {_("OK")}, 0, true);
      return false;
    }
    if (file_exists(n) && message(&d, _("Replace the archive?"), format(_("“%s” already exists. Replace it?"), basename_of(n).c_str()),
                                  {_("Cancel"), _("Replace")}, 1, true) != 1)
      return false;
    return true;
  };
  int r = d.run(parent);
  delete static_cast<std::function<void()> *>(chips->user_data());
  if (r != 1) return false;
  dest = trim(name->value());
  o.format = formats[size_t(chips->value())];
  o.level = int(level->value());
  return true;
}

// --------------------------------------------------------------- settings

bool ask_settings(Fl_Window *parent, Settings &s) {
  auto formats = creatable_formats();
  std::vector<std::string> labels;
  for (Format f : formats) labels.push_back(format_info(f).label);
  FitScale fit(490, 600);
  int w = S(600);
  Dialog d(w, S(490), _("Settings"), _("Defaults for new archives and how Taca looks"));
  int left = S(24), fw = w - S(48);
  d.begin();
  d.label(left, S(80), fw, _("Default archive format"), true);
  auto *fmt = new Chips(left, S(106), fw, S(66), labels);
  int rows = fmt->rows_needed(fw);
  fmt->size(fw, rows * S(36));
  for (size_t i = 0; i < formats.size(); ++i)
    if (formats[i] == s.format) fmt->value(int(i));
  int y = S(106) + rows * S(36) + S(10);
  d.label(left, y, fw, _("Compression level"), true);
  auto *level = new Slider(left + S(150), y + S(28), fw - S(300), S(26));
  level->bounds(0, 9);
  level->step(1);
  level->value(s.level);
  y += S(70);
  int vw = S(190);
  d.label(left, y, vw, _("View"), true);
  auto *view = new Chips(left, y + S(26), vw, S(30), {_("Details"), _("Icons")});
  view->value(s.icons ? 1 : 0);
  d.label(left + vw + S(10), y, fw - vw - S(10), _("Appearance"), true);
  auto *theme = new Chips(left + vw + S(10), y + S(26), fw - vw - S(10), S(30), {_("Follow SDE"), _("Light"), _("Dark")});
  theme->value(s.theme == "light" ? 1 : s.theme == "dark" ? 2 : 0);
  y += S(74);
  auto *hidden = new Switch(left, y, S(44), S(24));
  hidden->value(s.hidden);
  d.label(left + S(56), y + S(1), fw - S(56), _("Show hidden files"));
  y += S(36);
  auto *open = new Switch(left, y, S(44), S(24));
  open->value(s.open_after);
  d.label(left + S(56), y + S(1), fw - S(56), _("Show the files when an extraction finishes"));
  d.end();
  const Theme &t = d.theme();
  d.extra_text = [&]() {
    text_font(t.font_size - S(1));
    int lv = int(level->value());
    draw_text(_("Faster"), left, level->y(), S(140), level->h(), t.text_dim, 2);
    std::string what = lv == 0 ? _("Store only") : lv <= 3 ? _("Fast") : lv <= 6 ? _("Normal") : _("Maximum");
    draw_text(format("%d · %s", lv, what.c_str()), level->x() + level->w() + S(16), level->y(), S(130), level->h(), t.text);
  };
  d.buttons({_("Cancel"), _("Save")}, 1);
  if (d.run(parent) != 1) return false;
  s.format = formats[size_t(fmt->value())];
  s.level = int(level->value());
  s.icons = view->value() == 1;
  s.theme = theme->value() == 1 ? "light" : theme->value() == 2 ? "dark" : "follow";
  s.hidden = hidden->value();
  s.open_after = open->value();
  return true;
}

// -------------------------------------------------------------- favorites

namespace {

void style_browser(Fl_Browser *b, const Theme &t) {
  b->box(FL_FLAT_BOX);
  b->color(t.surface_alt.fl());
  b->selection_color(t.accent.fl());
  b->textcolor(t.text.fl());
  b->textsize(t.font_size);
  b->textfont(FL_HELVETICA);
}

}  // namespace

bool organize_favorites(Fl_Window *parent, std::vector<std::string> &list) {
  FitScale fit(440, 600);
  Dialog d(S(600), S(440), _("Organize Favorites"), _("Folders and archives you open often"));
  std::vector<std::string> work = list;
  d.begin();
  auto *browser = new Fl_Hold_Browser(S(24), S(84), d.w() - S(48) - S(130), S(250));
  style_browser(browser, d.theme());
  auto fill = [&](int select) {
    browser->clear();
    for (const auto &f : work) browser->add(f.c_str());
    if (select > 0 && select <= browser->size()) browser->value(select);
  };
  fill(work.empty() ? 0 : 1);
  struct Ctx {
    std::vector<std::string> *work;
    Fl_Hold_Browser *browser;
    std::function<void(int)> fill;
  } ctx{&work, browser, fill};
  int bx = d.w() - S(24) - S(118);
  auto *remove = new FlatButton(bx, S(84), S(118), S(34), _("Remove"));
  auto *up = new FlatButton(bx, S(126), S(118), S(34), _("Move Up"));
  auto *down = new FlatButton(bx, S(168), S(118), S(34), _("Move Down"));
  remove->callback([](Fl_Widget *w, void *p) {
    auto *c = static_cast<Ctx *>(p);
    int v = c->browser->value();
    if (v <= 0) return;
    c->work->erase(c->work->begin() + (v - 1));
    c->fill(std::min(v, int(c->work->size())));
    w->window()->redraw();
  }, &ctx);
  up->callback([](Fl_Widget *w, void *p) {
    auto *c = static_cast<Ctx *>(p);
    int v = c->browser->value();
    if (v <= 1) return;
    std::swap((*c->work)[size_t(v - 1)], (*c->work)[size_t(v - 2)]);
    c->fill(v - 1);
    w->window()->redraw();
  }, &ctx);
  down->callback([](Fl_Widget *w, void *p) {
    auto *c = static_cast<Ctx *>(p);
    int v = c->browser->value();
    if (v <= 0 || v >= int(c->work->size())) return;
    std::swap((*c->work)[size_t(v - 1)], (*c->work)[size_t(v)]);
    c->fill(v + 1);
    w->window()->redraw();
  }, &ctx);
  d.end();
  if (work.empty()) d.label(S(36), S(96), S(300), _("No favorites yet"), true);
  d.buttons({_("Cancel"), _("Save")}, 1);
  if (d.run(parent) != 1 || work == list) return false;
  list = work;
  return true;
}

// ------------------------------------------------------------------- text

void show_text(Fl_Window *parent, const std::string &title, const std::string &text) {
  int sw = Fl::w(), sh = Fl::h();
  Dialog d(std::min(S(820), sw - S(40)), std::min(S(620), sh - S(80)), title);
  const Theme &t = d.theme();
  auto *buf = new Fl_Text_Buffer();
  buf->text(text.c_str());
  d.begin();
  auto *view = new Fl_Text_Display(S(16), S(60), d.w() - S(32), d.h() - S(60) - S(86));
  view->buffer(buf);
  view->box(FL_FLAT_BOX);
  view->color(t.surface.fl());
  view->textcolor(t.text.fl());
  view->selection_color(t.accent.fl());
  view->textfont(FL_COURIER);
  view->textsize(t.font_size);
  view->scrollbar_width(S(14));
  d.end();
  d.resizable(view);
  d.buttons({_("Close")}, 0);
  d.run(parent);
  view->buffer(nullptr);
  delete buf;
}

// ------------------------------------------------------ shortcuts & about

void show_shortcuts(Fl_Window *parent) {
  static const std::vector<std::pair<const char *, const char *>> rows = {
      {"", "Archives"},
      {"Alt+A", "Add files to an archive"},
      {"Alt+E", "Extract to a folder"},
      {"Alt+W", "Extract here"},
      {"Alt+T", "Test the archive"},
      {"Alt+V", "View the selected file"},
      {"Alt+I", "Information"},
      {"Alt+R", "Repair an archive"},
      {"Alt+Z", "Wizard"},
      {"Alt+Q", "Convert an archive"},
      {"Del", "Delete (files go to the trash)"},
      {"F2", "Rename"},
      {"", "Browsing"},
      {"Enter", "Open the folder, archive or file"},
      {"Backspace", "Up one level"},
      {"Ctrl+L", "Type a location"},
      {"F3, Ctrl+F", "Find files"},
      {"F5", "Refresh"},
      {"Ctrl+A", "Select all"},
      {"Ctrl+H", "Show hidden files"},
      {"Ctrl+B", "Add to favorites"},
      {"Ctrl+O, Ctrl+N", "Open or create an archive"},
      {"Ctrl+W, Ctrl+Q", "Close the archive, quit"},
  };
  FitScale fit(84 + int(rows.size()) * 22 + 90, 520);
  int h = S(84) + int(rows.size()) * S(22) + S(90);
  Dialog d(S(520), h, _("Keyboard Shortcuts"));
  const Theme &t = d.theme();
  d.extra_text = [&]() {
    int y = S(64);
    for (const auto &r : rows) {
      if (!*r.first) {
        y += S(6);
        text_font(t.font_size, true);
        draw_text(_(r.second), S(24), y, S(300), S(22), t.accent_text);
      } else {
        text_font(t.font_size - S(1), true);
        draw_text(r.first, S(32), y, S(150), S(22), t.text);
        text_font(t.font_size - S(1));
        draw_text(_(r.second), S(190), y, d.w() - S(214), S(22), t.text_dim);
      }
      y += S(22);
    }
  };
  d.buttons({_("Close")}, 0);
  d.run(parent);
}

sde::AppInfo about_info() {
  sde::AppInfo info;
  info.name = "Taca";
  info.program = "taca";
  info.version = SDE_APP_VERSION;
  info.icon = "taca";
  info.description = _("The archive manager of the Selviana Desktop Environment. Browse, create, extract, "
                       "test and repair archives.");
  std::string engines = std::string(archive_version_string());
  if (!seven_zip_program().empty()) engines += " · 7-Zip";
  if (!unrar_program().empty()) engines += " · unrar";
  if (!rar_program().empty()) engines += " · rar";
  info.credits = {format(_("Engines: %s"), engines.c_str()), sde::toolkit_credit()};
  // Taca's own badge when the "taca" icon is not installed.
  info.logo = [](Painter &p, double x, double y, double size) {
    icon::badge(p, x, y, size, active_theme().accent);
  };
  return info;
}

void show_about(Fl_Window *) { sde::show_about(about_info()); }

}  // namespace taca
