// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Taca's dialogs: password, messages, rename, extract options, new archive
// and properties. All are themed SDE windows drawn with cairo.
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "taca.h"
#include "util.h"

using namespace sde;

namespace taca {

Dialog::Dialog(int w, int h, const std::string &title, const std::string &subtitle)
    : BufferedWindow(w, h, title.c_str()), theme_(active_theme()), title_(title), subtitle_(subtitle) {
  copy_label(title.c_str());
  xclass("Taca");
  set_modal();
  color(theme_.background.fl());
  callback([](Fl_Widget *w, void *) { static_cast<Dialog *>(w)->finish(0); });
}

void Dialog::finish(int code) {
  if (code != 0 && accept && !accept(code)) return;
  code_ = code;
  done_ = true;
  hide();
}

void Dialog::buttons(const std::vector<std::string> &labels, int primary) {
  begin();
  int bh = S(34), gap = S(10), right = w() - S(22), y = h() - bh - S(20);
  std::vector<int> widths;
  text_font(theme_.font_size);
  for (const auto &l : labels) widths.push_back(std::max(S(96), text_width(l) + S(36)));
  int x = right;
  for (size_t i = labels.size(); i-- > 0;) {
    x -= widths[i];
    auto *b = new FlatButton(x, y, widths[i], bh, labels[i], int(i) == primary);
    b->callback([](Fl_Widget *w, void *data) {
      static_cast<Dialog *>(w->window())->finish(int(reinterpret_cast<long>(data)));
    }, reinterpret_cast<void *>(long(i)));
    buttons_.insert(buttons_.begin(), b);
    x -= gap;
  }
  end();
}

void Dialog::label(int x, int y, int w, const std::string &text, bool dim, bool bold) {
  labels_.push_back({x, y, w, text, dim, bold});
}

void Dialog::draw() {
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    p.fill_rect(0, h() - S(74), w(), S(74), theme_.surface_alt.alpha(0.6));
    p.line(0, h() - S(74) + 0.5, w(), h() - S(74) + 0.5, theme_.border.alpha(0.7));
    for (auto *f : fields_) draw_field(p, f, theme_);
    if (extra_paint) extra_paint(p);
  }
  text_font(theme_.font_size + S(5), true);
  draw_text(title_, S(24), S(18), w() - S(48), S(30), theme_.text);
  if (!subtitle_.empty()) {
    text_font(theme_.font_size);
    draw_text(subtitle_, S(24), S(48), w() - S(48), S(22), theme_.text_dim);
  }
  for (const auto &l : labels_) {
    text_font(theme_.font_size - (l.dim ? S(1) : 0), l.bold);
    draw_text(l.text, l.x, l.y, l.w, S(22), l.dim ? theme_.text_dim : theme_.text);
  }
  draw_children();
  if (extra_text) extra_text();
}

int Dialog::handle(int event) {
  if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
    int k = Fl::event_key();
    if (k == FL_Escape) {
      finish(0);
      return 1;
    }
    if ((k == FL_Enter || k == FL_KP_Enter) && !buttons_.empty()) {
      for (size_t i = 0; i < buttons_.size(); ++i)
        if (Fl::focus() == buttons_[i]) {
          finish(int(i));
          return 1;
        }
      finish(int(buttons_.size()) - 1);
      return 1;
    }
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_PUSH || event == FL_KEYBOARD || event == FL_RELEASE) {
    if (on_change) on_change();
    redraw();
  }
  return r;
}

int Dialog::run(Fl_Window *parent) {
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  if (parent && parent->shown())
    position(parent->x() + (parent->w() - w()) / 2, parent->y() + std::max(0, (parent->h() - h()) / 3));
  else
    position(sx + (sw - w()) / 2, sy + (sh - h()) / 3);
  // Keep the whole dialog (and its title bar) on the screen.
  int nx = std::max(sx, std::min(x(), sx + sw - w()));
  int ny = std::max(sy + 24, std::min(y(), sy + sh - h() - 4));
  position(nx, std::max(sy, ny));
  show();
  while (!done_ && shown()) Fl::wait();
  return code_;
}

FitScale::FitScale(int logical_h, int logical_w) {
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  double scale = ui_scale();
  double fit = std::min((sh - 64.0) / logical_h, logical_w > 0 ? (sw - 16.0) / logical_w : 99.0);
  if (fit >= scale) return;
  fit = std::max(0.75, fit);
  saved_ = active_theme();
  old_ = scale;
  Theme t = saved_;
  t.font_size = std::max(9, int(std::lround(saved_.font_size * fit / scale)));
  t.scale = fit;
  set_ui_scale(fit);
  t.apply_to_fltk();
  active_ = true;
}

FitScale::~FitScale() {
  if (!active_) return;
  set_ui_scale(old_);
  saved_.apply_to_fltk();
}

// --------------------------------------------------------------- simple

bool ask_password(Fl_Window *parent, const std::string &archive, bool wrong, std::string &out) {
  FitScale fit(232, 440);
  Dialog d(S(440), S(232), wrong ? _("Wrong password") : _("Password required"),
           format(wrong ? _("Try again to unlock “%s”.") : _("“%s” is encrypted."), basename_of(archive).c_str()));
  d.begin();
  auto *in = new Fl_Secret_Input(S(32), S(104), d.w() - S(64), S(28));
  style_input(in, d.theme());
  d.field(in);
  d.end();
  d.label(S(24), S(76), d.w() - S(48), _("Password"), true);
  d.buttons({_("Cancel"), _("Unlock")}, 1);
  d.accept = [in](int) { return in->size() > 0; };
  Fl::focus(in);
  if (d.run(parent) != 1) return false;
  out = in->value();
  return true;
}

int message(Fl_Window *parent, const std::string &title, const std::string &text,
            const std::vector<std::string> &buttons, int primary, bool warning) {
  int w = S(480);
  fl_font(FL_HELVETICA, active_theme().font_size);
  int mw = w - S(48 + 40), mh = 0;
  fl_measure(text.c_str(), mw, mh, 0);
  if (mw > w - S(88)) mw = w - S(88);
  int h = std::max(S(190), S(70) + mh + S(110));
  Dialog d(w, std::min(h, S(560)), title);
  std::string body = text;
  const Theme &t = d.theme();
  d.extra_paint = [warning, &t](Painter &p) {
    icon::draw(p, warning ? icon::Warning : icon::Info, S(24), S(62), S(26), warning ? Color::hex(0xD9822B) : t.accent_text);
  };
  d.extra_text = [&d, body, &t]() {
    fl_font(FL_HELVETICA, t.font_size);
    fl_color(t.text.fl());
    fl_draw(body.c_str(), S(64), S(62), d.w() - S(88), d.h() - S(62) - S(86), FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
  };
  d.buttons(buttons, primary < 0 ? int(buttons.size()) - 1 : primary);
  return d.run(parent);
}

bool ask_name(Fl_Window *parent, const std::string &title, const std::string &current, std::string &out) {
  Dialog d(S(460), S(200), title);
  d.begin();
  auto *in = new Fl_Input(S(32), S(76), d.w() - S(64), S(28));
  style_input(in, d.theme());
  in->value(current.c_str());
  // Select the name without its extension.
  size_t dot = current.find('.', 1);
  in->insert_position(0, int(dot == std::string::npos ? current.size() : dot));
  d.field(in);
  d.end();
  d.buttons({_("Cancel"), _("Rename")}, 1);
  d.accept = [in](int) {
    std::string v = trim(in->value());
    return !v.empty() && v.find('/') == std::string::npos && v != "." && v != "..";
  };
  Fl::focus(in);
  if (d.run(parent) != 1) return false;
  out = trim(in->value());
  return true;
}

// -------------------------------------------------------------- extract

bool ask_extract(Fl_Window *parent, const std::string &archive, size_t selected, ExtractChoice &c) {
  FitScale fit(486, 560);
  Dialog d(S(560), S(486), _("Extract"), format(_("From “%s”"), basename_of(archive).c_str()));
  int left = S(24), fw = d.w() - S(48);
  d.begin();
  d.label(left, S(80), fw, _("Destination"), true);
  auto *dest = new Fl_Input(left + S(8), S(110), fw - S(130), S(28));
  style_input(dest, d.theme());
  dest->value(c.dest.c_str());
  d.field(dest);
  auto *browse = new FlatButton(left + fw - S(108), S(106), S(108), S(34), _("Browse…"));
  browse->callback([](Fl_Widget *w, void *p) {
    auto *in = static_cast<Fl_Input *>(p);
    std::string dir = choose_folder(_("Extract To"), in->value());
    if (!dir.empty()) in->value(dir.c_str());
    w->window()->redraw();
  }, dest);
  d.label(left, S(156), fw, _("Files"), true);
  auto *what = new Chips(left, S(182), fw, S(30),
                         {_("Everything"), selected ? format(_("Selected (%zu)"), selected) : std::string(_("Selected"))});
  what->value(c.selected_only && selected ? 1 : 0);
  if (!selected) what->enable(1, false);
  d.label(left, S(226), fw, _("When a file already exists"), true);
  auto *exists = new Chips(left, S(252), fw, S(30), {_("Keep both"), _("Replace"), _("Skip")});
  exists->value(c.overwrite == Overwrite::Rename ? 0 : c.overwrite == Overwrite::Replace ? 1 : 2);
  auto *keep = new Switch(left, S(304), S(44), S(24));
  keep->value(c.keep_structure);
  d.label(left + S(56), S(305), fw - S(56), _("Keep the folder structure"));
  auto *open = new Switch(left, S(344), S(44), S(24));
  open->value(c.open_after);
  d.label(left + S(56), S(345), fw - S(56), _("Show the files when done"));
  d.end();
  d.buttons({_("Cancel"), _("Extract")}, 1);
  d.accept = [dest, &d](int) {
    if (dir_exists(dest->value())) return true;
    if (!make_dirs(dest->value())) {
      message(&d, _("Cannot use this folder"), format(_("“%s” could not be created."), dest->value()), {_("OK")}, 0, true);
      return false;
    }
    return true;
  };
  if (d.run(parent) != 1) return false;
  c.dest = dest->value();
  c.selected_only = what->value() == 1;
  c.overwrite = exists->value() == 0 ? Overwrite::Rename : exists->value() == 1 ? Overwrite::Replace : Overwrite::Skip;
  c.keep_structure = keep->value();
  c.open_after = open->value();
  return true;
}

// ---------------------------------------------------------- new archive

bool ask_create(Fl_Window *parent, const std::vector<std::string> &files, CreateChoice &c) {
  auto formats = creatable_formats();
  std::vector<std::string> labels;
  for (Format f : formats) labels.push_back(format_info(f).label);
  std::string sub = files.empty() ? std::string(_("Choose what goes inside after creating it."))
                    : files.size() == 1 ? format(_("Pack “%s”"), basename_of(files[0]).c_str())
                                        : format(_("Pack %zu items"), files.size());
  FitScale fit(560, 600);
  int w = S(600);
  Dialog d(w, S(560), _("New Archive"), sub);
  int left = S(24), fw = w - S(48);
  d.begin();
  int nw = fw * 2 / 5;
  d.label(left, S(80), nw, _("Name"), true);
  auto *name = new Fl_Input(left + S(8), S(110), nw - S(16), S(28));
  style_input(name, d.theme());
  d.field(name);
  d.label(left + nw + S(12), S(80), fw - nw - S(12), _("Location"), true);
  auto *where = new Fl_Input(left + nw + S(20), S(110), fw - nw - S(20) - S(60), S(28));
  style_input(where, d.theme());
  d.field(where);
  auto *browse = new FlatButton(left + fw - S(44), S(106), S(44), S(34), "…");
  browse->tooltip(_("Browse…"));
  browse->callback([](Fl_Widget *w, void *p) {
    auto *in = static_cast<Fl_Input *>(p);
    std::string dir = choose_folder(_("Save the Archive In"), in->value());
    if (!dir.empty()) in->value(dir.c_str());
    w->window()->redraw();
  }, where);
  d.label(left, S(150), fw, _("Format"), true);
  auto *chips = new Chips(left, S(176), fw, S(66), labels);
  int rows = chips->rows_needed(fw);
  chips->size(fw, rows * S(36));
  int y = S(176) + rows * S(36) + S(10);
  d.label(left, y, fw, _("Compression"), true);
  auto *level = new Slider(left + S(150), y + S(28), fw - S(300), S(26));
  level->bounds(0, 9);
  level->step(1);
  level->value(c.options.level);
  y += S(70);
  d.label(left, y, fw / 2, _("Password (optional)"), true);
  auto *pw = new Fl_Secret_Input(left + S(8), y + S(30), fw / 2 - S(30), S(28));
  style_input(pw, d.theme());
  d.field(pw);
  auto *names = new Switch(left + fw / 2 + S(10), y + S(32), S(44), S(24));
  d.label(left + fw / 2 + S(64), y + S(33), fw / 2 - S(64), _("Encrypt file names"));
  y += S(70);
  auto *split = new Switch(left, y, S(44), S(24));
  d.label(left + S(56), y + S(1), S(180), _("Split into volumes of"));
  auto *vol = new Fl_Input(left + S(250), y - S(2), S(90), S(28));
  style_input(vol, d.theme());
  vol->value("100");
  d.field(vol);
  d.label(left + S(352), y + S(1), S(40), "MB");
  d.end();

  // Defaults.
  std::string dir = !c.folder.empty() ? c.folder
                    : files.empty()     ? home_dir()
                                        : files[0].substr(0, std::max<size_t>(1, files[0].rfind('/')));
  std::string base = files.size() == 1 ? basename_of(files[0]) : basename_of(dir);
  if (base.empty() || base == "/") base = _("Archive");
  where->value(dir.c_str());
  int chosen = 0;
  for (size_t i = 0; i < formats.size(); ++i)
    if (formats[i] == c.options.format) chosen = int(i);
  chips->value(chosen);
  name->value((base + format_info(formats[size_t(chosen)]).extension).c_str());
  std::string note;
  auto update = [&]() {
    Format f = formats[size_t(chips->value())];
    Capabilities caps = capabilities(f);
    bool single = format_info(f).single_file;
    if (caps.password) pw->activate();
    else pw->deactivate();
    if (caps.encrypt_names && pw->size() > 0) names->activate();
    else names->deactivate();
    if (caps.volumes) {
      split->activate();
      if (split->value()) vol->activate();
      else vol->deactivate();
    } else {
      split->deactivate();
      vol->deactivate();
    }
    if (f == Format::Tar || f == Format::Iso || f == Format::Cpio) level->deactivate();
    else level->activate();
    note = caps.note;
    if (single && files.size() != 1) note = _("This format holds a single file: choose TAR.GZ, TAR.XZ or another TAR format");
    if (!caps.password) note = note.empty() ? std::string(_("This format does not support passwords")) : note;
  };
  int last_format = chosen;
  d.on_change = [&]() {
    if (chips->value() != last_format) {
      // Swap the extension in the name field.
      std::string n = name->value();
      std::string stem = strip_archive_extension(n);
      if (stem.empty()) stem = base;
      name->value((stem + format_info(formats[size_t(chips->value())]).extension).c_str());
      last_format = chips->value();
    }
    update();
  };
  update();
  // Key presses go straight to the focused field, not through the dialog.
  auto changed = [](Fl_Widget *, void *p) {
    auto *dlg = static_cast<Dialog *>(p);
    if (dlg->on_change) dlg->on_change();
    dlg->redraw();
  };
  for (Fl_Input *in : {static_cast<Fl_Input *>(pw), name, vol}) {
    in->when(FL_WHEN_CHANGED);
    in->callback(changed, &d);
  }
  chips->callback(changed, &d);
  split->callback(changed, &d);
  const Theme &t = d.theme();
  d.extra_text = [&, level]() {
    text_font(t.font_size - S(1));
    int lv = int(level->value());
    draw_text(_("Faster"), left, level->y(), S(140), level->h(), t.text_dim, 2);
    draw_text(_("Smaller"), level->x() + level->w() + S(10), level->y(), S(70), level->h(), t.text_dim);
    std::string what = lv == 0 ? _("Store only") : lv <= 3 ? _("Fast") : lv <= 6 ? _("Normal") : _("Maximum");
    draw_text(format("%d · %s", lv, what.c_str()), level->x() + level->w() + S(80), level->y(), S(140),
              level->h(), t.text);
    if (!note.empty()) draw_text(note, left, d.h() - S(108), d.w() - S(48), S(22), t.text_dim);
  };
  d.buttons({_("Cancel"), _("Create")}, 1);
  d.accept = [&](int) {
    std::string n = trim(name->value());
    Format f = formats[size_t(chips->value())];
    if (n.empty() || n.find('/') != std::string::npos) {
      message(&d, _("Choose a name"), _("The archive needs a name without slashes."), {_("OK")}, 0, true);
      return false;
    }
    if (format_info(f).single_file && files.size() != 1) {
      message(&d, _("Choose another format"), note, {_("OK")}, 0, true);
      return false;
    }
    if (!dir_exists(where->value())) {
      message(&d, _("Choose a location"), format(_("The folder “%s” does not exist."), where->value()), {_("OK")}, 0, true);
      return false;
    }
    std::string path = join_path(where->value(), n);
    if (file_exists(path) &&
        message(&d, _("Replace the archive?"), format(_("“%s” already exists. Replace it?"), n.c_str()),
                {_("Cancel"), _("Replace")}, 1, true) != 1)
      return false;
    return true;
  };
  Fl::focus(name);
  if (d.run(parent) != 1) return false;
  Format f = formats[size_t(chips->value())];
  c.path = join_path(where->value(), trim(name->value()));
  c.options.format = f;
  c.options.level = int(level->value());
  c.options.password = pw->active() ? pw->value() : "";
  c.options.encrypt_names = names->active() && names->value();
  c.options.volume_size = split->active() && split->value() ? int64_t(std::max(1.0, atof(vol->value())) * 1000 * 1000) : 0;
  return true;
}

// ----------------------------------------------------------- properties

void show_properties(Fl_Window *parent, const ArchiveInfo &info, const std::string &password_note) {
  struct stat st;
  time_t mtime = stat(info.path.c_str(), &st) == 0 ? st.st_mtime : 0;
  char date[64] = "—";
  if (mtime) {
    struct tm tm;
    localtime_r(&mtime, &tm);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
  }
  double ratio = info.total_size > 0 ? double(info.archive_size) / double(info.total_size) : -1;
  std::vector<std::pair<std::string, std::string>> rows = {
      {_("Location"), info.path.substr(0, info.path.rfind('/'))},
      {_("Format"), std::string(format_info(info.format).label) +
                        (info.engine_format.empty() || info.backend == "libarchive" ? ""
                                                                                     : "  (" + info.engine_format + ")")},
      {_("Archive size"), human_size(info.archive_size)},
      {_("Contents"), format("%s (%s bytes)", human_size(info.total_size).c_str(), group_digits(info.total_size).c_str())},
      {_("Packed"), info.total_packed >= 0 ? human_size(info.total_packed) : std::string("—")},
      {_("Ratio"), ratio >= 0 ? format("%d%%", int(std::lround(ratio * 100))) : std::string("—")},
      {_("Files"), format(_("%zu files in %zu folders"), info.files, info.folders)},
      {_("Encryption"), info.header_encrypted ? _("Contents and file names")
                        : info.encrypted       ? _("Contents (file names are visible)")
                                               : _("None")},
      {_("Modified"), date},
      {_("Read with"), info.backend.empty() ? "libarchive" : info.backend},
  };
  if (info.unsafe) rows.push_back({_("Warning"), format(_("%zu unsafe names will not be extracted"), info.unsafe)});
  if (!password_note.empty()) rows.push_back({_("Password"), password_note});
  std::vector<std::string> comment_lines;
  for (const auto &l : split(info.comment, '\n', true))
    if (comment_lines.size() < 6) comment_lines.push_back(l);
  while (!comment_lines.empty() && trim(comment_lines.back()).empty()) comment_lines.pop_back();
  if (comment_lines.empty() && !info.comment.empty()) comment_lines.push_back(info.comment);
  FitScale fit(84 + int(rows.size()) * 30 + (comment_lines.empty() ? 0 : 36 + int(comment_lines.size()) * 20) + 96, 580);
  int comment_h = comment_lines.empty() ? 0 : S(36) + int(comment_lines.size()) * S(20);
  Dialog d(S(580), S(84) + int(rows.size()) * S(30) + comment_h + S(96), basename_of(info.path),
           describe_format(info.format, info.path));
  const Theme &t = d.theme();
  d.extra_paint = [&](Painter &p) {
    // Compression ring.
    double cx = d.w() - S(96), cy = S(150), r = S(54);
    cairo_t *cr = p.cr();
    cairo_set_line_width(cr, Sd(12));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    p.set(t.border.alpha(0.6));
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);
    if (ratio >= 0) {
      double saved = std::max(0.0, std::min(1.0, 1 - ratio));
      p.set(t.accent);
      cairo_new_sub_path(cr);
      cairo_arc(cr, cx, cy, r, -M_PI / 2, -M_PI / 2 + 2 * M_PI * std::max(0.01, saved));
      cairo_stroke(cr);
    }
  };
  d.extra_text = [&]() {
    int y = S(84);
    for (const auto &row : rows) {
      text_font(t.font_size - S(1));
      draw_text(row.first, S(24), y, S(120), S(24), t.text_dim);
      text_font(t.font_size);
      draw_text(row.second, S(150), y, d.w() - S(150) - S(190), S(24), t.text);
      y += S(30);
    }
    if (!comment_lines.empty()) {
      y += S(8);
      text_font(t.font_size - S(1), true);
      draw_text(_("Comment"), S(24), y, S(200), S(24), t.text_dim);
      y += S(26);
      text_font(t.font_size);
      for (const auto &l : comment_lines) {
        draw_text(l, S(32), y, d.w() - S(56), S(20), t.text);
        y += S(20);
      }
    }
    double cx = d.w() - S(96), cy = S(150);
    text_font(t.font_size + S(6), true);
    std::string pct = ratio >= 0 ? format("%d%%", int(std::lround(std::max(0.0, 1 - ratio) * 100))) : "—";
    draw_text(pct, int(cx - S(50)), int(cy - S(18)), S(100), S(28), t.text, 1);
    text_font(t.font_size - S(2));
    draw_text(_("smaller"), int(cx - S(50)), int(cy + S(8)), S(100), S(18), t.text_dim, 1);
  };
  d.buttons({_("Close")}, 0);
  d.run(parent);
}

}  // namespace taca
