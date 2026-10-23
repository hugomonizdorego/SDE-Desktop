// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny command palette: ">" runs commands, ":" goes to a line, and plain
// text quickly opens documents, files of the current folder and recent files.
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <set>

#include "i18n.h"
#include "niny.h"
#include "textops.h"
#include "util.h"

using namespace sde;

namespace niny {

namespace {
constexpr int kMaxVisible = 9;
}

Palette::Palette(App *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  box(FL_NO_BOX);
  input_ = new Field(0, 0, 100, 30);
  input_->when(FL_WHEN_CHANGED);
  input_->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<Palette *>(p);
    self->sel_ = 0;
    self->scroll_ = 0;
    self->rebuild();
  }, this);
  input_->on_key = [this](int key, int) {
    int n = int(items_.size());
    switch (key) {
      case FL_Escape: close(true); return 1;
      case FL_Enter:
      case FL_KP_Enter: run_selected(); return 1;
      case FL_Down: sel_ = n ? (sel_ + 1) % n : 0; break;
      case FL_Up: sel_ = n ? (sel_ + n - 1) % n : 0; break;
      case FL_Page_Down: sel_ = std::min(n - 1, sel_ + kMaxVisible); break;
      case FL_Page_Up: sel_ = std::max(0, sel_ - kMaxVisible); break;
      case FL_Tab: return 1;
      default: return 0;
    }
    if (sel_ < scroll_) scroll_ = sel_;
    if (sel_ >= scroll_ + kMaxVisible) scroll_ = sel_ - kMaxVisible + 1;
    redraw();
    return 1;
  };
  end();
  hide();
}

int Palette::list_top() const { return y() + S(58); }
int Palette::item_h() const { return S(36); }

void Palette::relayout() {
  style_input(input_, true);
  input_->resize(x() + S(46), y() + S(14), w() - S(62), S(30));
}

void Palette::open(const std::string &prefix) {
  EditorView *ed = app_->editor();
  origin_cursor_ = ed && app_->active() ? ed->insert_position() : -1;
  origin_top_ = ed ? ed->top_line() : 1;
  input_->value(prefix.c_str());
  input_->position(int(prefix.size()));
  sel_ = scroll_ = 0;
  show();
  rebuild();
  app_->layout();
  input_->take_focus();
  app_->redraw();
}

void Palette::close(bool revert) {
  if (!visible()) return;
  std::string v = input_->value();
  if (revert && !v.empty() && v[0] == ':' && origin_cursor_ >= 0 && app_->active()) {
    app_->editor()->insert_position(origin_cursor_);
    app_->editor()->scroll(origin_top_, 0);
  }
  hide();
  if (app_->active()) app_->editor()->take_focus();
  app_->redraw();
}

void Palette::rebuild() {
  std::string v = input_->value();
  items_.clear();
  if (!v.empty() && v[0] == ':') {
    mode_ = ":";
    std::string rest = trim(v.substr(1));
    int line = 0, col = 0;
    if (!rest.empty() && sscanf(rest.c_str(), "%d:%d", &line, &col) >= 1 && line > 0) {
      Document *d = app_->active();
      int total = d ? d->line_count() : 0;
      line = std::min(line, std::max(1, total));
      PaletteItem it;
      it.label = col > 0 ? format(_("Go to line %d, column %d"), line, col) : format(_("Go to line %d"), line);
      it.detail = format(_("%d lines"), total);
      int l = line, c = col;
      it.run = [this, l, c] { app_->goto_line(l, c); };
      items_.push_back(it);
      // Live preview.
      if (d) app_->goto_line(line, col);
    } else {
      PaletteItem it;
      Document *d = app_->active();
      it.label = _("Type a line number, or line:column");
      it.detail = d ? format(_("%d lines"), d->line_count()) : "";
      items_.push_back(it);
    }
  } else if (!v.empty() && v[0] == '>') {
    mode_ = ">";
    std::string q = trim(v.substr(1));
    for (const Command &c : app_->commands()) {
      int sc = q.empty() ? 0 : std::max(fuzzy_score(q, c.label), fuzzy_score(q, c.group + " " + c.label) - 50);
      if (sc < 0) continue;
      PaletteItem it;
      it.label = c.label;
      it.detail = c.group;
      it.hint = c.shortcut_text;
      std::string id = c.id;
      it.run = [this, id] { app_->run_command(id); };
      it.score = sc;
      items_.push_back(it);
    }
  } else {
    mode_ = "";
    std::string q = trim(v);
    std::set<std::string> seen;
    auto add = [&](const std::string &label, const std::string &detail, const Language *lang,
                   std::function<void()> fn, int bonus) {
      int sc = q.empty() ? bonus : fuzzy_score(q, label);
      if (sc < 0) return;
      PaletteItem it;
      it.label = label;
      it.detail = detail;
      it.badge = lang;
      it.run = std::move(fn);
      it.score = sc + (q.empty() ? 0 : bonus);
      items_.push_back(it);
    };
    const auto &docs = app_->docs();
    for (size_t i = 0; i < docs.size(); ++i) {
      Document *d = docs[i].get();
      if (!d->path.empty()) seen.insert(d->path);
      int idx = int(i);
      add(d->display_name(), d->path.empty() ? _("Open document") : home_relative(d->folder()), d->lang,
          [this, idx] { app_->activate(idx); }, 300);
    }
    std::string folder = app_->sidebar()->folder();
    if (!folder.empty()) {
      if (DIR *dir = opendir(folder.c_str())) {
        int count = 0;
        while (dirent *e = readdir(dir)) {
          if (e->d_name[0] == '.' || ++count > 2000) continue;
          std::string p = folder + "/" + e->d_name;
          struct stat st;
          if (seen.count(p) || stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
          seen.insert(p);
          add(e->d_name, home_relative(folder), detect_language(p), [this, p] { app_->open_file(p); }, 100);
        }
        closedir(dir);
      }
    }
    for (auto &r : app_->recent()) {
      if (seen.count(r)) continue;
      seen.insert(r);
      add(basename_of(r), home_relative(r.substr(0, r.rfind('/'))), detect_language(r),
          [this, r] { app_->open_file(r); }, 0);
    }
    if (q.empty() && items_.empty()) {
      PaletteItem it;
      it.label = _("Type > for commands, : to go to a line");
      items_.push_back(it);
    }
  }
  std::stable_sort(items_.begin(), items_.end(),
                   [](const PaletteItem &a, const PaletteItem &b) { return a.score > b.score; });
  int shown = std::min(kMaxVisible, std::max(1, int(items_.size())));
  size(w(), S(66) + shown * item_h());
  relayout();
  if (window()) window()->redraw();
}

void Palette::run_selected() {
  if (sel_ < 0 || sel_ >= int(items_.size()) || !items_[size_t(sel_)].run) {
    if (mode_ != ":") return;
    close(false);
    return;
  }
  auto fn = items_[size_t(sel_)].run;
  close(false);
  fn();
}

void Palette::draw() {
  const Look &L = look();
  int top = list_top(), ih = item_h();
  int n = int(items_.size());
  {
    Painter p(window()->w(), window()->h());
    double r = Sd(L.radius + 4);
    p.shadow(x(), y(), w(), h(), r, L.shadow, S(18));
    p.fill_rounded(x(), y(), w(), h(), r, L.card);
    if (L.glass) p.glass_sheen(x(), y(), w(), h() * 0.5, r, L.dark);
    p.stroke_rounded(x(), y(), w(), h(), r, L.card_border);
    icon::draw(p, mode_ == ">" ? icon::Command : mode_ == ":" ? icon::ChevronRight : icon::Search, x() + S(16),
               y() + S(19), S(20), L.dim);
    p.line(x() + S(12), top - S(6) + 0.5, x() + w() - S(12), top - S(6) + 0.5, L.border, 1);
    for (int i = scroll_; i < n && i < scroll_ + kMaxVisible; ++i) {
      int ry = top + (i - scroll_) * ih;
      if (i == sel_ && items_[size_t(i)].run)
        p.fill_rounded(x() + S(8), ry, w() - S(16), ih - S(2), Sd(L.radius), L.accent.alpha(L.dark ? 0.30 : 0.14));
      else if (i == hover_ && items_[size_t(i)].run)
        p.fill_rounded(x() + S(8), ry, w() - S(16), ih - S(2), Sd(L.radius), L.hover);
      if (items_[size_t(i)].badge) draw_badge(p, items_[size_t(i)].badge, x() + S(18), ry + (ih - S(22)) / 2, S(22));
    }
  }
  draw_children();
  for (int i = scroll_; i < n && i < scroll_ + kMaxVisible; ++i) {
    const PaletteItem &it = items_[size_t(i)];
    int ry = top + (i - scroll_) * ih;
    int lx = x() + S(20);
    if (it.badge) {
      draw_badge_text(it.badge, x() + S(18), ry + (ih - S(22)) / 2.0, S(22));
      lx = x() + S(50);
    }
    int right = x() + w() - S(20);
    text_font(L.theme.font_size - S(1));
    if (!it.hint.empty()) {
      int hw = text_width(it.hint) + S(14);
      {
        Painter p(window()->w(), window()->h());
        p.fill_rounded(right - hw, ry + (ih - S(22)) / 2.0, hw, S(22), Sd(5), L.field);
      }
      text_font(L.theme.font_size - S(1));
      draw_text(it.hint, right - hw, ry, hw, ih - S(2), L.dim, 1);
      right -= hw + S(10);
    }
    text_font(L.theme.font_size, i == sel_);
    int lw = std::min(text_width(it.label) + S(4), right - lx);
    draw_text(it.label, lx, ry, lw, ih - S(2), it.run ? L.text : L.dim);
    if (!it.detail.empty()) {
      text_font(L.theme.font_size - S(1));
      draw_text(it.detail, lx + lw + S(10), ry, right - lx - lw - S(10), ih - S(2), L.dim);
    }
  }
}

int Palette::handle(int event) {
  int my = Fl::event_y(), mx = Fl::event_x();
  int top = list_top();
  auto item_at = [&]() {
    if (mx < x() || mx >= x() + w() || my < top) return -1;
    int i = scroll_ + (my - top) / item_h();
    return i < int(items_.size()) && i < scroll_ + kMaxVisible ? i : -1;
  };
  switch (event) {
    case FL_MOVE:
    case FL_ENTER: {
      int h = item_at();
      if (h != hover_) {
        hover_ = h;
        redraw();
      }
      break;
    }
    case FL_PUSH:
      if (!Fl::event_inside(this)) {
        close(true);
        return 0;
      }
      if (item_at() >= 0) {
        sel_ = item_at();
        run_selected();
        return 1;
      }
      break;
    case FL_MOUSEWHEEL: {
      int n = int(items_.size());
      scroll_ = std::max(0, std::min(scroll_ + Fl::event_dy(), std::max(0, n - kMaxVisible)));
      redraw();
      return 1;
    }
  }
  return Fl_Group::handle(event);
}

}  // namespace niny
