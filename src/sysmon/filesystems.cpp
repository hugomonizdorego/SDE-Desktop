// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE System Monitor: the File systems page (mounted local storage).
#include <FL/fl_draw.H>

#include <algorithm>

#include "i18n.h"
#include "ui.h"

namespace sysmon {

namespace {

class FsList : public Fl_Widget {
 public:
  FsList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_NO_BOX); }

  void set(const std::vector<FsInfo> &items) {
    bool same = items.size() == items_.size();
    for (size_t i = 0; same && i < items.size(); ++i)
      same = items[i].mount_point == items_[i].mount_point && items[i].used == items_[i].used &&
             items[i].total == items_[i].total;
    if (same && loaded_) return;
    loaded_ = true;
    items_ = items;
    clamp();
    redraw();
  }

  void draw() override {
    const sde::Theme &t = sde::active_theme();
    fl_push_clip(x(), y(), w(), h());
    int rh = row_h(), gap = S(12);
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), t.background);
      for (size_t i = 0; i < items_.size(); ++i) {
        int ry = y() + int(i) * (rh + gap) - offset_;
        if (ry + rh < y() || ry > y() + h()) continue;
        const FsInfo &f = items_[i];
        bool hot = int(i) == hover_;
        p.fill_rounded(x() + 0.5, ry + 0.5, w() - 1, rh - 1, Sd(14), hot ? t.surface.mix(t.hover, 0.5) : t.surface);
        p.stroke_rounded(x() + 0.5, ry + 0.5, w() - 1, rh - 1, Sd(14), t.border);
        p.fill_rounded(x() + S(18), ry + S(18), S(40), S(40), Sd(10), palette().disk.alpha(t.dark ? 0.22 : 0.14));
        draw_icon(p, Icon::Drive, x() + S(27), ry + S(27), S(22), palette().disk);
        double frac = f.total ? double(f.used) / double(f.total) : 0;
        int bx = x() + S(76), bw = w() - S(76) - S(160), by = ry + rh - S(28);
        Color bar = frac > 0.9 ? palette().danger : frac > 0.75 ? palette().warning : t.accent;
        p.fill_rounded(bx, by, bw, S(8), Sd(4), t.surface_alt.mix(t.border, 0.5));
        p.fill_rounded(bx, by, std::max(Sd(8), bw * frac), S(8), Sd(4), bar);
        // Open button.
        int ox = x() + w() - S(130), oy = ry + (rh - S(34)) / 2;
        bool ohot = hot && open_hot_;
        p.fill_rounded(ox, oy, S(110), S(34), Sd(8), ohot ? t.hover : t.surface_alt);
        p.stroke_rounded(ox + 0.5, oy + 0.5, S(110) - 1, S(34) - 1, Sd(8), t.border);
        draw_icon(p, Icon::Folder, ox + S(12), oy + S(9), S(16), t.text);
      }
    }
    for (size_t i = 0; i < items_.size(); ++i) {
      int ry = y() + int(i) * (rh + gap) - offset_;
      if (ry + rh < y() || ry > y() + h()) continue;
      const FsInfo &f = items_[i];
      int tx = x() + S(76), tw = w() - S(76) - S(160);
      sde::text_font(t.font_size + S(1), true);
      sde::draw_text(f.mount_point, tx, ry + S(12), tw / 2, S(22), t.text);
      sde::text_font(t.font_size - S(1));
      std::string meta = f.device + " · " + f.type + (f.read_only ? std::string(" · ") + _("read-only") : "");
      sde::draw_text(meta, tx, ry + S(34), tw / 2 + S(40), S(18), t.text_dim);
      double pct = f.total ? 100.0 * double(f.used) / double(f.total) : 0;
      sde::text_font(t.font_size);
      sde::draw_text(sde::format(_("%s free of %s"), format_bytes(f.available).c_str(), format_bytes(f.total).c_str()),
                     tx + tw / 2, ry + S(12), tw / 2, S(22), t.text, 2);
      sde::text_font(t.font_size - S(1));
      sde::draw_text(sde::format(_("%s used (%.0f %%)"), format_bytes(f.used).c_str(), pct), tx + tw / 2, ry + S(34),
                     tw / 2, S(18), t.text_dim, 2);
      sde::text_font(t.font_size);
      sde::draw_text(_("Open"), x() + w() - S(130) + S(34), ry + (rh - S(34)) / 2, S(70), S(34), t.text);
    }
    if (items_.empty() && loaded_) {
      sde::text_font(t.font_size);
      sde::draw_text(_("No local file systems are mounted."), x(), y() + S(20), w(), S(30), t.text_dim, 1);
    }
    fl_pop_clip();
  }

  int handle(int e) override {
    switch (e) {
      case FL_ENTER: return 1;
      case FL_LEAVE: set_hover(-1, false); return 1;
      case FL_MOVE: {
        int i = hit(Fl::event_y());
        set_hover(i, i >= 0 && in_open(i));
        return 1;
      }
      case FL_MOUSEWHEEL:
        offset_ += Fl::event_dy() * S(40);
        clamp();
        redraw();
        return 1;
      case FL_PUSH: {
        int i = hit(Fl::event_y());
        if (i < 0) return 1;
        if (in_open(i) || Fl::event_clicks() > 0) open_folder(items_[i].mount_point);
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }

 private:
  int row_h() const { return S(76); }
  int hit(int ey) const {
    int stride = row_h() + S(12);
    int rel = ey - y() + offset_;
    if (rel < 0) return -1;
    int i = rel / stride;
    if (rel % stride >= row_h() || i >= int(items_.size())) return -1;
    return i;
  }
  bool in_open(int i) const {
    int ry = y() + i * (row_h() + S(12)) - offset_;
    return Fl::event_inside(x() + w() - S(130), ry + (row_h() - S(34)) / 2, S(110), S(34));
  }
  void set_hover(int i, bool open) {
    if (i == hover_ && open == open_hot_) return;
    hover_ = i, open_hot_ = open;
    redraw();
  }
  void clamp() {
    int total = int(items_.size()) * (row_h() + S(12));
    offset_ = std::max(0, std::min(offset_, total - h()));
  }
  std::vector<FsInfo> items_;
  int offset_ = 0, hover_ = -1;
  bool open_hot_ = false, loaded_ = false;
};

class FileSystemsPage : public Page {
 public:
  FileSystemsPage(int x, int y, int w, int h) : Page(x, y, w, h, _("File Systems")) {
    list_ = new FsList(x + S(28), y + header_height(), w - S(56), h - header_height() - S(20));
    resizable(list_);
    end();
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    subtitle_->resize(X + S(28), Y + S(48), W - S(56), S(22));
    list_->resize(X + S(28), Y + header_height(), W - S(56), H - header_height() - S(20));
  }
  void update(const std::shared_ptr<const Snapshot> &snap, bool visible) override {
    if (!visible || !snap->has_fs) return;
    uint64_t total = 0, free = 0;
    for (auto &f : snap->filesystems) total += f.total, free += f.available;
    subtitle_->text(sde::format(_("%d mounted · %s free of %s"), int(snap->filesystems.size()),
                                format_bytes(free).c_str(), format_bytes(total).c_str()));
    list_->set(snap->filesystems);
  }

 private:
  FsList *list_;
};

}  // namespace

Page *make_filesystems_page(int x, int y, int w, int h) { return new FileSystemsPage(x, y, w, h); }

}  // namespace sysmon
