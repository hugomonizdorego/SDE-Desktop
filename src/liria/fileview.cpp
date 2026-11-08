// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The files of a folder: icons with thumbnails (Ctrl + mouse wheel zooms),
// details with sortable columns, or compact (small icons in rows). The
// items are drawn by one canvas inside an Fl_Scroll (the UI kit's scroll
// widget: wheel, scroll bar and touch scrolling), only the ones in view;
// the pointer repaints only the items it leaves and reaches. Rubber band
// and keyboard selection, type-ahead, drag and drop (inside Liria, and with
// the desktop and other programs through XDND), and renaming in place (F2).
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Touch.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <glib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "clipboard.h"
#include "desktop_model.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "location.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {
namespace liria {

namespace {

size_t next_char(const std::string &s, size_t i) {
  if (i >= s.size()) return s.size();
  ++i;
  while (i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80) ++i;
  return i;
}

// A name in at most |lines| lines of |width| pixels (the font must be set).
std::vector<std::string> wrap_name(const std::string &s, int width, int lines = 2) {
  std::vector<std::string> out;
  std::string rest = s;
  while (!rest.empty() && int(out.size()) < lines) {
    if (text_width(rest) <= width || int(out.size()) == lines - 1) {
      out.push_back(int(out.size()) == lines - 1 ? ellipsize(rest, width) : rest);
      break;
    }
    size_t fit = 0, brk = std::string::npos;
    for (size_t i = 0; i < rest.size();) {
      size_t n = next_char(rest, i);
      if (text_width(rest.substr(0, n)) > width) break;
      fit = n;
      char c = rest[i];
      if (c == ' ' || c == '-' || c == '_' || c == '.') brk = n;
      i = n;
    }
    size_t cut = brk != std::string::npos && brk > fit / 3 ? brk : fit;
    if (cut == 0) cut = next_char(rest, 0);
    std::string first = rest.substr(0, cut);
    while (!first.empty() && first.back() == ' ') first.pop_back();
    out.push_back(first);
    rest = rest.substr(cut);
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------------ canvas

class FileCanvas : public Fl_Widget {
 public:
  FileCanvas(FileView *v) : Fl_Widget(0, 0, 10, 10), v_(v) {}
  int hover = -1;
  int press = -1, press_x = 0, press_y = 0;
  bool pressed_selected = false, band = false, dragging = false, dragged_local = false;
  int band_x0 = 0, band_y0 = 0, band_x1 = 0, band_y1 = 0;  // canvas coordinates (scrolled with it)
  std::set<std::string> band_base;
  std::string typed;
  double typed_at = 0;
  std::unordered_map<std::string, std::vector<std::string>> wraps;
  int wraps_w = 0;
  bool drop_here = false;  // a drag over the empty part of the folder
  int drop_item = -1;
  void draw() override;
  int handle(int e) override;
  void damage_item(int k);
  void update_band();
  static void autoscroll_cb(void *d);
  void move_focus(int key, bool shift, bool ctrl);
  void draw_empty();

 private:
  FileView *v_;
};

class FileScroll : public Fl_Scroll {
 public:
  FileScroll(FileView *v) : Fl_Scroll(0, 0, 10, 10), v_(v) {
    type(Fl_Scroll::VERTICAL);
    box(FL_NO_BOX);
  }
  int last_y = -1;
  void draw() override {
    if (yposition() != last_y) {
      last_y = yposition();
      v_->win()->tab().scroll = yposition();
      v_->win()->schedule_thumbnails();
      if (v_->canvas->hover >= 0) v_->canvas->hover = -1;
    }
    Fl_Scroll::draw();
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Scroll::resize(X, Y, W, H);
    v_->sync();
  }

 private:
  FileView *v_;
};

namespace {

class RenameEditor : public Fl_Input {
 public:
  RenameEditor(FileView *v) : Fl_Input(0, 0, 10, 10), v_(v) {
    box(FL_FLAT_BOX);
    hide();
  }
  void draw() override {
    const Theme &t = active_theme();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), Sd(4), t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(4), t.accent, Sd(1.5));
    }
    color(t.surface.fl());
    textcolor(t.text.fl());
    selection_color(t.accent.fl());
    cursor_color(t.accent.fl());
    textfont(FL_HELVETICA);
    textsize(t.font_size);
    drawtext(x() + S(5), y() + S(2), w() - S(10), h() - S(4));
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int key = Fl::event_key();
      if (key == FL_Enter || key == FL_KP_Enter) {
        v_->finish_rename(true);
        return 1;
      }
      if (key == FL_Escape) {
        v_->finish_rename(false);
        return 1;
      }
    }
    if (e == FL_UNFOCUS) {
      int r = Fl_Input::handle(e);
      Fl::add_timeout(0.0, [](void *d) {
        auto *view = static_cast<FileView *>(d);
        if (!view->editing.empty() && Fl::focus() != view->editor) view->finish_rename(true);
      }, v_);
      return r;
    }
    return Fl_Input::handle(e);
  }

 private:
  FileView *v_;
};

}  // namespace

// ------------------------------------------------------------------ FileView

FileView::FileView(MainWindow *w) : Fl_Group(0, 0, 10, 10), w_(w) {
  box(FL_NO_BOX);
  scroll = new FileScroll(this);
  canvas = new FileCanvas(this);
  editor = new RenameEditor(this);
  scroll->end();
  end();
}

void FileView::canvas_focus() { canvas->take_focus(); }
void FileView::reset_hover() { canvas->hover = -1; }
void FileView::forget_wraps() { canvas->wraps.clear(); }

int FileView::thumb_size() const {
  const Tab &t = const_cast<FileView *>(this)->w_->tab();
  if (t.layout != Layout::Icons) return 128;
  return image > 128 ? 256 : 128;
}

void FileView::metrics() {
  const Tab &t = w_->tab();
  const Theme &th = active_theme();
  pad = S(8);
  int sb = Fl::scrollbar_size() ? Fl::scrollbar_size() : S(14);
  int avail = std::max(S(100), scroll->w() - sb - 2 * pad);
  head_h = t.layout == Layout::Details ? S(34) : 0;
  text_font(th.font_size);
  int line = fl_height() + S(1);
  switch (t.layout) {
    case Layout::Icons: {
      image = std::max(S(24), int(std::lround(Sd(t.icon_size))));
      int min_tile = std::max(S(84), image + S(30));
      cols = std::max(1, avail / min_tile);
      tile_w = std::max(min_tile, avail / cols);
      tile_h = image + S(16) + 2 * line + S(8);
      break;
    }
    case Layout::Details:
      image = S(20);
      cols = 1;
      row_h = S(32);
      tile_w = avail;
      tile_h = row_h;
      break;
    case Layout::Compact:
      image = S(20);
      tile_w = S(230);
      cols = std::max(1, avail / tile_w);
      tile_w = std::max(S(180), avail / cols);
      tile_h = S(32);
      break;
  }
  if (tile_w != canvas->wraps_w) {
    canvas->wraps.clear();
    canvas->wraps_w = tile_w;
  }
}

int FileView::content_height() {
  metrics();
  int n = w_->tab().model.count();
  int rows = (n + cols - 1) / cols;
  return std::max(scroll->h(), rows * tile_h + 2 * pad);
}

void FileView::sync() {
  int sb = Fl::scrollbar_size() ? Fl::scrollbar_size() : S(14);
  int ch = content_height();
  int sy = scroll->yposition();
  canvas->resize(scroll->x(), scroll->y() - sy, std::max(10, scroll->w() - sb), ch);
  scroll->scrollbar.linesize(std::max(S(24), tile_h / 2));
  int max_y = std::max(0, ch - scroll->h());
  if (sy > max_y) scroll->scroll_to(0, max_y);
  if (!editing.empty()) {
    int k = w_->tab().model.index(editing);
    if (k < 0) finish_rename(false);
  }
  scroll->redraw();
  redraw();
}

void FileView::item_rect(int k, int &X, int &Y, int &W, int &H) const {
  int col = k % std::max(1, cols), row = k / std::max(1, cols);
  X = canvas->x() + pad + col * tile_w;
  Y = canvas->y() + pad + row * tile_h;
  W = tile_w;
  H = tile_h;
}

int FileView::item_at(int mx, int my) const {
  if (!Fl::event_inside(scroll) && !(mx >= scroll->x() && mx < scroll->x() + scroll->w() && my >= scroll->y() &&
                                     my < scroll->y() + scroll->h()))
    return -1;
  int cx = mx - canvas->x() - pad, cy = my - canvas->y() - pad;
  if (cx < 0 || cy < 0) return -1;
  int col = cx / std::max(1, tile_w), row = cy / std::max(1, tile_h);
  if (col >= cols) return -1;
  int k = row * cols + col;
  int n = const_cast<FileView *>(this)->w_->tab().model.count();
  if (k >= n) return -1;
  // Icons: the pointer must be on the tile's picture or name, not the gap.
  const Tab &t = const_cast<FileView *>(this)->w_->tab();
  if (t.layout == Layout::Compact) {
    int X, Y, W, H;
    item_rect(k, X, Y, W, H);
    if (mx > X + W - S(6)) return -1;
  }
  return k;
}

void FileView::visible_range(int &first, int &last) const {
  int n = const_cast<FileView *>(this)->w_->tab().model.count();
  if (n == 0 || tile_h <= 0) {
    first = 0;
    last = -1;
    return;
  }
  int top = scroll->yposition() - pad, bottom = scroll->yposition() + scroll->h();
  first = std::max(0, top / tile_h * cols);
  last = std::min(n - 1, (bottom / tile_h + 1) * cols - 1);
}

int FileView::scroll_y() const { return scroll->yposition(); }

void FileView::scroll_to_y(int y) {
  int max_y = std::max(0, canvas->h() - scroll->h());
  scroll->scroll_to(0, std::max(0, std::min(y, max_y)));
}

void FileView::ensure_visible(int k) {
  if (k < 0) return;
  metrics();
  int row = k / std::max(1, cols);
  int top = pad + row * tile_h, bottom = top + tile_h;
  int sy = scroll->yposition();
  if (top - S(4) < sy) scroll_to_y(top - S(4));
  else if (bottom + S(4) > sy + scroll->h()) scroll_to_y(bottom + S(4) - scroll->h());
}

std::vector<FileView::Column> FileView::columns() const {
  std::vector<Column> out;
  const Tab &t = const_cast<FileView *>(this)->w_->tab();
  int left = scroll->x() + pad, right = scroll->x() + scroll->w() - (Fl::scrollbar_size() ? Fl::scrollbar_size() : S(14)) - pad;
  int avail = right - left;
  bool trash = t.location == kTrash, search = !t.query.empty();
  std::vector<Column> tail;
  int cx = right;
  auto add = [&](const std::string &id, const std::string &title, int w, chooser::SortKey key, bool rightalign,
                 int min_avail) {
    if (avail < min_avail) return;
    cx -= w;
    Column c;
    c.id = id;
    c.title = title;
    c.x = cx;
    c.w = w;
    c.key = key;
    c.right = rightalign;
    tail.push_back(c);
  };
  add("size", _("Size"), S(96), chooser::SortKey::Size, true, S(300));
  if (trash) {
    add("type", _("Type"), S(140), chooser::SortKey::Type, false, S(700));
    add("deleted", _("Date deleted"), S(160), chooser::SortKey::Modified, false, S(420));
    add("origin", _("Original location"), S(200), chooser::SortKey::Name, false, S(560));
    tail.back().sortable = false;
  } else if (search) {
    add("type", _("Type"), S(140), chooser::SortKey::Type, false, S(760));
    add("modified", _("Date modified"), S(160), chooser::SortKey::Modified, false, S(420));
    add("folder", _("Folder"), S(220), chooser::SortKey::Name, false, S(560));
    tail.back().sortable = false;
  } else {
    add("type", _("Type"), S(150), chooser::SortKey::Type, false, S(560));
    add("modified", _("Date modified"), S(160), chooser::SortKey::Modified, false, S(400));
  }
  Column name;
  name.id = "name";
  name.title = _("Name");
  name.x = left;
  name.w = cx - left;
  name.key = chooser::SortKey::Name;
  out.push_back(name);
  for (auto it = tail.rbegin(); it != tail.rend(); ++it) out.push_back(*it);
  return out;
}

void FileView::resize(int X, int Y, int W, int H) {
  Fl_Widget::resize(X, Y, W, H);
  metrics();
  scroll->resize(X, Y + head_h, W, H - head_h);
}

void FileView::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  const Tab &t = w_->tab();
  const Look &l = look();
  const Theme &th = active_theme();
  metrics();
  if (scroll->y() != y() + head_h) scroll->resize(x(), y() + head_h, w(), h() - head_h);
  if (t.layout == Layout::Details && fl_not_clipped(x(), y(), w(), head_h)) {
    std::vector<Column> cols_ = columns();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), head_h, l.card);
      for (size_t i = 0; i < cols_.size(); ++i) {
        const Column &c = cols_[i];
        if (int(i) == header_hover && c.sortable)
          p.fill_rounded(c.x, y() + S(4), c.w, head_h - S(8), th.radius_for(head_h - S(8)), l.hover);
        if (i > 0) p.line(c.x - 0.5, y() + S(9), c.x - 0.5, y() + head_h - S(9), th.border);
        if (c.sortable && c.key == t.model.order.key && (c.id != "name" || t.model.order.key == chooser::SortKey::Name))
          liria::glyph(p, t.model.order.descending ? "chevron-down" : "chevron-up", c.x + c.w / 2.0 - S(6), y() + S(1),
                       S(12), l.dim, 1.8);
      }
      p.line(x() + pad, y() + head_h - 0.5, x() + w() - pad, y() + head_h - 0.5, th.border);
    }
    for (const auto &c : cols_) {
      text_font(th.font_size - 1);
      draw_text(c.title, c.x + S(10), y(), c.w - S(20), head_h, l.dim, c.right ? 2 : 0);
    }
  }
  draw_child(*scroll);
}

int FileView::handle(int e) {
  const Tab &t = w_->tab();
  if (t.layout == Layout::Details && Fl::event_y() < y() + head_h && Fl::event_y() >= y()) {
    std::vector<Column> cols_ = columns();
    int hc = -1;
    for (size_t i = 0; i < cols_.size(); ++i)
      if (Fl::event_x() >= cols_[i].x && Fl::event_x() < cols_[i].x + cols_[i].w) hc = int(i);
    switch (e) {
      case FL_ENTER:
      case FL_MOVE:
        if (hc != header_hover) {
          header_hover = hc;
          w_->damage_rect(x(), y(), w(), head_h);
        }
        return 1;
      case FL_PUSH:
        if (hc >= 0 && cols_[size_t(hc)].sortable) {
          if (Fl::event_button() == FL_LEFT_MOUSE) {
            chooser::SortKey key = cols_[size_t(hc)].key;
            if (t.model.order.key == key) w_->set_sort_descending(!t.model.order.descending);
            else w_->sort_by(key);
          } else if (Fl::event_button() == FL_RIGHT_MOUSE) {
            w_->sort_menu(Fl::event_x(), Fl::event_y());
          }
        }
        return 1;
    }
  } else if (header_hover >= 0 && (e == FL_MOVE || e == FL_LEAVE || e == FL_ENTER)) {
    header_hover = -1;
    w_->damage_rect(x(), y(), w(), head_h);
  }
  return Fl_Group::handle(e);
}

void FileView::start_rename(const std::string &path) {
  int k = w_->tab().model.index(path);
  if (k < 0) return;
  ensure_visible(k);
  sync();
  const Tab &t = w_->tab();
  const Item &e = t.model.at(k);
  int X, Y, W, H;
  item_rect(k, X, Y, W, H);
  const Theme &th = active_theme();
  int eh = S(28);
  switch (t.layout) {
    case Layout::Icons:
      editor->resize(X + S(2), Y + S(8) + image + S(4), W - S(4), eh);
      break;
    case Layout::Details:
      editor->resize(X + S(36), Y + (H - eh) / 2, std::max(S(120), columns()[0].w - S(44)), eh);
      break;
    case Layout::Compact:
      editor->resize(X + S(34), Y + (H - eh) / 2, W - S(40), eh);
      break;
  }
  editing = path;
  editor->value(e.display.c_str());
  // The name without its extension is selected, as in Windows.
  auto r = e.dir ? std::pair<size_t, size_t>{0, e.display.size()} : chooser::stem_range(e.display);
  editor->textsize(th.font_size);
  editor->show();
  editor->take_focus();
  editor->insert_position(int(r.second), int(r.first));
  scroll->redraw();
  w_->schedule_qa();
}

void FileView::finish_rename(bool keep) {
  if (editing.empty()) return;
  std::string path = editing;
  std::string name = editor->value();
  editing.clear();
  editor->hide();
  canvas->take_focus();
  scroll->redraw();
  if (!keep) return;
  int k = w_->tab().model.index(path);
  if (k < 0) return;
  const Item &e = w_->tab().model.at(k);
  if (name == e.display || name.empty()) return;
  // The display name is UTF-8; on disk the same bytes (GLib's filename
  // encoding is UTF-8 on every current system).
  std::string new_path, error;
  if (!rename_path(path, name, &new_path, &error)) {
    w_->say(error, true);
    editing = path;
    editor->show();
    editor->take_focus();
    return;
  }
  UndoStep step;
  step.kind = UndoStep::Rename;
  step.pairs.push_back({new_path, path});
  App::get().undo.push(step);
  Tab &tab = w_->tab();
  tab.select_when_loaded = new_path;
  App::get().pins.find(path);
  w_->folder_changed(chooser::parent_dir(path), {basename_of(path), basename_of(new_path)});
}

// ------------------------------------------------------------------ drawing

void FileCanvas::damage_item(int k) {
  if (k < 0) return;
  int X, Y, W, H;
  v_->item_rect(k, X, Y, W, H);
  static_cast<MainWindow *>(window())->damage_rect(X - S(4), Y - S(4), W + S(8), H + S(8));
}

void FileCanvas::draw_empty() {
  MainWindow *w = v_->win();
  const Tab &t = w->tab();
  const Look &l = look();
  const Theme &th = active_theme();
  std::string title, detail, icon = "folder";
  if (!t.error.empty()) {
    title = _("This folder cannot be opened");
    detail = t.error;
    icon = "warning";
  } else if (t.loading) {
    if (now_ms() - t.opened_at < 250) return;
    title = _("Loading…");
    icon = "folder";
  } else if (!t.query.empty()) {
    title = t.searching ? _("Searching…") : _("No items match your search.");
    detail = t.searching ? std::string() : format(_("Nothing here or in the folders below is called “%s”."), t.query.c_str());
    icon = "search";
  } else if (t.location == kTrash) {
    title = _("The Trash is empty");
    detail = _("Items you delete stay here until you empty the Trash.");
    icon = "trash";
  } else if (t.model.total() > 0) {
    title = _("Only hidden items are here");
    detail = _("Show them with View > Show > Hidden items (Ctrl+H).");
    icon = "hidden";
  } else {
    title = _("This folder is empty");
  }
  Fl_Scroll *sc = v_->scroll;
  int cy = sc->y() + sc->h() / 2 - S(56);
  {
    Painter p(window()->w(), window()->h());
    double gs = S(52);
    liria::glyph(p, icon, x() + (sc->w() - gs) / 2, cy, gs, l.faint, 1.1);
  }
  text_font(th.font_size + 2, true);
  draw_text(title, x() + S(20), cy + S(62), sc->w() - S(40), S(26), l.text, 1);
  if (!detail.empty()) {
    text_font(th.font_size);
    draw_text(detail, x() + S(20), cy + S(90), sc->w() - S(40), S(22), l.dim, 1);
  }
}

void FileCanvas::draw() {
  MainWindow *w = v_->win();
  const Tab &t = w->tab();
  const Look &l = look();
  const Theme &th = active_theme();
  v_->metrics();
  Fl_Scroll *sc = v_->scroll;
  fl_push_clip(sc->x(), sc->y(), sc->w(), sc->h());
  int n = t.model.count();
  int first, last;
  v_->visible_range(first, last);
  bool focused = Fl::focus() == this;
  const FileClipboard *clip = App::get().clipboard.get();
  std::vector<FileView::Column> cols = t.layout == Layout::Details ? v_->columns() : std::vector<FileView::Column>();
  bool trash = t.location == kTrash;
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(sc->x(), sc->y(), sc->w(), sc->h(), l.card);
    for (int k = first; k <= last && k < n; ++k) {
      const Item &e = t.model.at(k);
      int X, Y, W, H;
      v_->item_rect(k, X, Y, W, H);
      if (!fl_not_clipped(X, Y, W, H)) continue;
      bool sel = t.model.is_selected(e.path), hot = k == hover || k == drop_item;
      bool foc = focused && t.model.focus == e.path;
      double alpha = (clip && clip->is_cut(e.path)) ? 0.45 : e.hidden ? 0.6 : 1.0;
      if (t.layout == Layout::Icons) {
        double bx = X + S(3), by = Y + S(2), bw = W - S(6), bh = H - S(4);
        double r = th.radius_small + S(2);
        if (sel) {
          p.fill_rounded(bx, by, bw, bh, r, l.select);
          p.stroke_rounded(bx + 0.5, by + 0.5, bw - 1, bh - 1, r, l.select_line);
        } else if (hot) {
          p.fill_rounded(bx, by, bw, bh, r, l.hover);
        }
        if (foc && !sel) focus_ring(p, bx + S(1), by + S(1), bw - S(2), bh - S(2), r);
        int img = v_->image;
        double cx = X + W / 2.0, top = Y + S(8);
        MainWindow::Thumb *thumb = e.dir ? nullptr : w->thumbnail(e, v_->thumb_size());
        if (thumb && thumb->surface) {
          double iw = cairo_image_surface_get_width(thumb->surface), ih = cairo_image_surface_get_height(thumb->surface);
          double kx = std::min(img / iw, img / ih);
          if (kx > 1) kx = std::min(kx, 2.0);  // small pictures are not blown up much
          double dw = std::round(iw * kx), dh = std::round(ih * kx);
          double dx = std::round(cx - dw / 2), dy = std::round(top + (img - dh) / 2);
          cairo_t *cr = p.cr();
          cairo_save(cr);
          p.rounded_rect(dx, dy, dw, dh, Sd(4));
          cairo_clip(cr);
          cairo_translate(cr, dx, dy);
          cairo_scale(cr, dw / iw, dh / ih);
          cairo_set_source_surface(cr, p.server_copy(thumb->surface), 0, 0);
          cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
          cairo_paint_with_alpha(cr, alpha);
          cairo_restore(cr);
          p.stroke_rounded(dx, dy, dw, dh, Sd(4), th.text.alpha(th.dark ? 0.18 : 0.12));
        } else {
          draw_icon(p, w->icon_names(e), cx - img / 2.0, top, img, alpha);
        }
        if (e.link) liria::glyph(p, "link", cx - img / 2.0 - S(2), top + img - S(14), S(14), l.accent_text, 2.0);
      } else {
        double r = th.radius_for(H - S(4));
        double bx = X + S(2), bw = W - S(4);
        if (t.layout == Layout::Compact) bw -= S(6);
        if (sel) {
          p.fill_rounded(bx, Y + S(2), bw, H - S(4), r, l.select);
        } else if (hot) {
          p.fill_rounded(bx, Y + S(2), bw, H - S(4), r, l.hover);
        }
        if (sel && t.layout == Layout::Details) p.fill_rounded(bx, Y + S(9), Sd(3), H - S(18), Sd(1.5), l.accent);
        if (foc) focus_ring(p, bx + S(1), Y + S(3), bw - S(2), H - S(6), r);
        int img = v_->image;
        MainWindow::Thumb *thumb = e.dir ? nullptr : w->thumbnail(e, 128);
        if (thumb && thumb->surface) p.image(thumb->surface, X + S(10), Y + (H - img) / 2.0, img, alpha);
        else draw_icon(p, w->icon_names(e), X + S(10), Y + (H - img) / 2.0, img, alpha);
      }
    }
    if (band) {
      int x0 = x() + std::min(band_x0, band_x1), y0 = y() + std::min(band_y0, band_y1);
      int bw = std::abs(band_x1 - band_x0), bh = std::abs(band_y1 - band_y0);
      p.fill_rect(x0, y0, bw, bh, l.accent.alpha(0.18));
      p.stroke_rounded(x0 + 0.5, y0 + 0.5, std::max(1, bw - 1), std::max(1, bh - 1), 0, l.accent, 1.0);
    }
    if (drop_here) p.stroke_rounded(sc->x() + S(3), sc->y() + S(3), sc->w() - S(6), sc->h() - S(6), th.radius, l.accent, Sd(2));
  }
  // Text.
  time_t now = time(nullptr);
  for (int k = first; k <= last && k < n; ++k) {
    const Item &e = t.model.at(k);
    int X, Y, W, H;
    v_->item_rect(k, X, Y, W, H);
    if (!fl_not_clipped(X, Y, W, H)) continue;
    if (!v_->editing.empty() && v_->editing == e.path) continue;
    bool cut = clip && clip->is_cut(e.path);
    Color fg = cut || e.hidden ? l.dim : l.text;
    if (t.layout == Layout::Icons) {
      text_font(th.font_size);
      int lw = W - S(12);
      int line = fl_height() + S(1);
      int ty = Y + S(8) + v_->image + S(6);
      auto it = wraps.find(e.path);
      if (it == wraps.end()) it = wraps.emplace(e.path, wrap_name(e.display, lw)).first;
      for (const auto &ln : it->second) {
        draw_text(ln, X + S(6), ty, lw, line, fg, 1);
        ty += line;
      }
      if (!e.folder.empty() && !trash && it->second.size() < 2) {
        text_font(th.font_size - 1);
        draw_text(location_title(e.folder), X + S(6), ty, lw, line, l.dim, 1);
      }
    } else if (t.layout == Layout::Compact) {
      text_font(th.font_size);
      draw_text(e.display, X + S(38), Y, W - S(48), H, fg);
    } else {
      for (const auto &c : cols) {
        text_font(th.font_size);
        if (c.id == "name") {
          draw_text(e.display, X + S(38), Y, c.x + c.w - X - S(46), H, fg);
          continue;
        }
        std::string s;
        if (c.id == "size") s = e.dir ? std::string() : chooser::size_text(e.size);
        else if (c.id == "modified" || c.id == "deleted") s = e.mtime ? chooser::time_text(e.mtime, now) : std::string();
        else if (c.id == "type") s = w->type_text(e.mime);
        else if (c.id == "origin" || c.id == "folder") s = e.folder.empty() ? std::string() : display_path(e.folder);
        draw_text(s, c.x + S(10), Y, c.w - S(20), H, l.dim, c.right ? 2 : 0);
      }
    }
  }
  if (n == 0) draw_empty();
  fl_pop_clip();
}

// ------------------------------------------------------------------ events

void FileCanvas::update_band() {
  MainWindow *w = v_->win();
  Tab &t = w->tab();
  int x0 = std::min(band_x0, band_x1), x1 = std::max(band_x0, band_x1);
  int y0 = std::min(band_y0, band_y1), y1 = std::max(band_y0, band_y1);
  std::set<std::string> inside;
  int n = t.model.count();
  int cols = std::max(1, v_->cols);
  int row0 = std::max(0, (y0 - v_->pad) / std::max(1, v_->tile_h));
  int row1 = (y1 - v_->pad) / std::max(1, v_->tile_h);
  for (int row = row0; row <= row1; ++row)
    for (int col = 0; col < cols; ++col) {
      int k = row * cols + col;
      if (k >= n) break;
      int ix = v_->pad + col * v_->tile_w, iy = v_->pad + row * v_->tile_h;
      int iw = v_->tile_w, ih = v_->tile_h;
      if (t.layout == Layout::Icons) {
        ix += S(6);
        iw -= S(12);
      } else if (t.layout == Layout::Details) {
        iw = std::min(iw, S(360));
      }
      if (ix < x1 && ix + iw > x0 && iy < y1 && iy + ih > y0) inside.insert(t.model.at(k).path);
    }
  t.model.select_set(inside, band_base);
  w->selection_changed();
}

void FileCanvas::autoscroll_cb(void *d) {
  auto *c = static_cast<FileCanvas *>(d);
  if (!c->band && !c->dragging) return;
  FileView *v = c->v_;
  int my = Fl::event_y_root() - c->window()->y_root();
  int dy = 0;
  if (my < v->scroll->y() + S(20)) dy = -S(24);
  else if (my > v->scroll->y() + v->scroll->h() - S(20)) dy = S(24);
  if (dy) {
    int before = v->scroll_y();
    v->scroll_to_y(before + dy);
    int moved = v->scroll_y() - before;
    if (moved && c->band) {
      c->band_y1 += moved;
      c->update_band();
    }
  }
  Fl::repeat_timeout(0.05, autoscroll_cb, d);
}

void FileCanvas::move_focus(int key, bool shift, bool ctrl) {
  MainWindow *w = v_->win();
  Tab &t = w->tab();
  int n = t.model.count();
  if (n == 0) return;
  v_->metrics();
  int k = t.model.index(t.model.focus);
  int cols = std::max(1, v_->cols);
  int page = std::max(1, v_->scroll->h() / std::max(1, v_->tile_h)) * cols;
  int step = 0;
  switch (key) {
    case FL_Left: step = t.layout == Layout::Details ? 0 : -1; break;
    case FL_Right: step = t.layout == Layout::Details ? 0 : 1; break;
    case FL_Up: step = -cols; break;
    case FL_Down: step = cols; break;
    case FL_Page_Up: step = -page; break;
    case FL_Page_Down: step = page; break;
    case FL_Home: step = -n; break;
    case FL_End: step = n; break;
  }
  if (!step) return;
  int target = k < 0 ? (step > 0 ? 0 : n - 1) : std::max(0, std::min(n - 1, k + step));
  const std::string &p = t.model.at(target).path;
  if (shift) {
    if (t.model.anchor.empty()) t.model.anchor = k >= 0 ? t.model.at(k).path : p;
    std::string anchor = t.model.anchor;
    t.model.select_range(p, ctrl);
    t.model.anchor = anchor;
  } else if (ctrl) {
    t.model.focus = p;
  } else {
    t.model.select_only(p);
  }
  t.model.focus = p;
  v_->ensure_visible(target);
  w->selection_changed();
  redraw();
}

int FileCanvas::handle(int e) {
  MainWindow *w = v_->win();
  Tab &t = w->tab();
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = v_->item_at(mx, my);
      if (k != hover) {
        damage_item(hover);
        hover = k;
        damage_item(hover);
        if (k >= 0) {
          static std::string tip;
          const Item &it = t.model.at(k);
          tip = it.display;
          if (!it.folder.empty()) tip += "\n" + display_path(it.folder);
          if (!it.dir) tip += "\n" + w->type_text(it.mime) + " · " + chooser::size_text(it.size);
          int X, Y, W, H;
          v_->item_rect(k, X, Y, W, H);
          Fl_Tooltip::enter_area(this, X, Y, W, H, tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      damage_item(hover);
      hover = -1;
      return 1;
    case FL_PUSH: {
      if (!v_->editing.empty()) v_->finish_rename(true);
      take_focus();
      int b = Fl::event_button();
      if (b == FL_BACK_MOUSE || b == FL_FORWARD_MOUSE) return 0;
      int k = v_->item_at(mx, my);
      int st = Fl::event_state();
      if (b == FL_RIGHT_MOUSE) {
        if (k >= 0 && !t.model.is_selected(t.model.at(k).path)) {
          t.model.select_only(t.model.at(k).path);
          w->selection_changed();
        }
        if (k >= 0) w->item_menu(mx, my);
        else {
          if (!(st & FL_CTRL)) {
            t.model.clear_selection();
            w->selection_changed();
          }
          w->background_menu(mx, my);
        }
        return 1;
      }
      if (b == FL_MIDDLE_MOUSE) {
        if (k >= 0 && t.model.at(k).dir) w->open_item(t.model.at(k), true);
        return 1;
      }
      if (b != FL_LEFT_MOUSE) return 1;
      press = k;
      press_x = mx;
      press_y = my;
      dragging = false;
      if (k < 0) {
        // The rubber band; a finger scrolls instead (a tap on the empty
        // part only clears the selection, docs/TOUCH.md).
        band = !Fl_Touch::event_is_touch();
        band_x0 = band_x1 = mx - x();
        band_y0 = band_y1 = my - y();
        band_base = (st & (FL_CTRL | FL_SHIFT)) ? t.model.selected() : std::set<std::string>();
        if (!(st & (FL_CTRL | FL_SHIFT))) {
          t.model.clear_selection();
          w->selection_changed();
        }
        if (band) Fl::add_timeout(0.05, autoscroll_cb, this);
        return 1;
      }
      const Item &it = t.model.at(k);
      if (Fl::event_clicks() && t.model.focus == it.path && !(st & (FL_CTRL | FL_SHIFT))) {
        Fl::event_clicks(0);
        press = -1;
        w->open_item(it, false);
        return 1;
      }
      pressed_selected = t.model.is_selected(it.path);
      if (st & FL_CTRL) {
        t.model.toggle(it.path);
        pressed_selected = false;
      } else if (st & FL_SHIFT) {
        t.model.select_range(it.path);
        pressed_selected = false;
      } else if (!pressed_selected) {
        t.model.select_only(it.path);
      }
      t.model.focus = it.path;
      w->selection_changed();
      redraw();
      return 1;
    }
    case FL_DRAG: {
      if (band) {
        band_x1 = mx - x();
        band_y1 = my - y();
        update_band();
        redraw();
        return 1;
      }
      if (press >= 0 && !dragging && (std::abs(mx - press_x) > S(6) || std::abs(my - press_y) > S(6))) {
        std::vector<std::string> paths = w->selected_paths();
        if (!paths.empty() && t.location != kHome) {
          dragging = true;
          w->begin_drag(paths, int(paths.size()), fl_event_time);
        }
      }
      if (dragging) {
        bool over_self = w->drag_motion(Fl::event_x_root(), Fl::event_y_root(), fl_mods(), fl_event_time);
        int k = over_self ? v_->item_at(mx, my) : -1;
        if (k >= 0 && (!t.model.at(k).dir || t.model.is_selected(t.model.at(k).path))) k = -1;
        if (k != drop_item) {
          damage_item(drop_item);
          drop_item = k;
          damage_item(drop_item);
        }
        dragged_local = over_self;
      }
      return 1;
    }
    case FL_RELEASE: {
      Fl::remove_timeout(autoscroll_cb, this);
      if (band) {
        band = false;
        redraw();
        return 1;
      }
      if (dragging) {
        dragging = false;
        int target = drop_item;
        damage_item(drop_item);
        drop_item = -1;
        (void)target;
        // On a folder of this view, another part of the window or another
        // program: the window knows where the pointer is.
        w->drag_drop(fl_event_time);
        press = -1;
        return 1;
      }
      if (press >= 0 && pressed_selected && !(Fl::event_state() & (FL_CTRL | FL_SHIFT)) &&
          press < t.model.count()) {
        t.model.select_only(t.model.at(press).path);
        w->selection_changed();
        redraw();
      }
      press = -1;
      return 1;
    }
    case FL_MOUSEWHEEL:
      if (Fl::event_state() & FL_CTRL) {
        int dy = Fl::event_dy();
        if (dy) w->zoom(dy < 0 ? 1 : -1);
        return 1;
      }
      return 0;
    case FL_ZOOM_GESTURE: {
      // Two fingers change the size of the icons like Ctrl+wheel: one step
      // for every fifth of a pinch (docs/TOUCH.md). A finger's scroll goes
      // to the Fl_Scroll around the canvas.
      static double pinch = 1.0;
      pinch *= Fl_Touch::zoom_factor();
      if (pinch > 1.2 || pinch < 1 / 1.2) {
        w->zoom(pinch > 1 ? 1 : -1);
        pinch = 1.0;
      }
      return 1;
    }
    case FL_FOCUS:
      if (t.model.focus.empty() && t.model.count() > 0) t.model.focus = t.model.at(0).path;
      redraw();
      return 1;
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      int st = Fl::event_state();
      bool shift = st & FL_SHIFT, ctrl = st & FL_CTRL, alt = st & FL_ALT;
      if (w->shortcut(key)) return 1;
      if (alt) return 0;
      switch (key) {
        case FL_Left:
        case FL_Right:
        case FL_Up:
        case FL_Down:
        case FL_Page_Up:
        case FL_Page_Down:
        case FL_Home:
        case FL_End:
          move_focus(key, shift, ctrl);
          return 1;
        case FL_Enter:
        case FL_KP_Enter:
          w->open_selection(ctrl);
          return 1;
        case FL_BackSpace:
          if (!typed.empty() && now_ms() - typed_at < 1000) {
            typed.erase(typed.size() - 1);
            typed_at = now_ms();
          } else {
            w->back();
          }
          return 1;
        case ' ':
          if (ctrl && !t.model.focus.empty()) {
            t.model.toggle(t.model.focus);
            w->selection_changed();
            redraw();
            return 1;
          }
          break;
        case FL_Menu: {
          int k = t.model.index(t.model.focus);
          int X = x() + S(40), Y = v_->scroll->y() + S(40), W = 0, H = 0;
          if (k >= 0) {
            v_->item_rect(k, X, Y, W, H);
            X += W / 2;
            Y += H / 2;
          }
          if (t.model.selected().empty()) w->background_menu(X, Y);
          else w->item_menu(X, Y);
          return 1;
        }
      }
      // Type-ahead: the first item that starts with what was typed.
      const char *text = Fl::event_text();
      if (!ctrl && text && *text && uint8_t(text[0]) >= 0x20 && text[0] != 0x7f) {
        if (now_ms() - typed_at > 1000) typed.clear();
        typed += text;
        typed_at = now_ms();
        std::string want = chooser::fold_text(typed);
        int n = t.model.count();
        int k = t.model.index(t.model.focus);
        int start = typed.size() == strlen(text) && k >= 0 ? k + 1 : std::max(0, k);
        for (int i = 0; i < n; ++i) {
          int c = (start + i) % n;
          if (t.model.at(c).fold.compare(0, want.size(), want) == 0) {
            t.model.select_only(t.model.at(c).path);
            v_->ensure_visible(c);
            w->selection_changed();
            redraw();
            break;
          }
        }
        return 1;
      }
      return 0;
    }
  }
  return Fl_Widget::handle(e);
}

}  // namespace liria
}  // namespace sde
