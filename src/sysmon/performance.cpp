// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE System Monitor: the Performance page, a dashboard of live graph cards.
#include <FL/Fl_Box.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "glyphs.h"
#include "i18n.h"
#include "ui.h"

namespace sysmon {

namespace {

struct Stat {
  std::string label, value;
  Color swatch;
  bool has_swatch = false;
};

class GraphCard : public Fl_Widget {
 public:
  GraphCard(Icon icon, const std::string &title, const Color &a, const Color &b)
      : Fl_Widget(0, 0, 10, 10), icon_(icon), title_(title), ca_(a), cb_(b) {
    box(FL_NO_BOX);
  }

  std::string key;
  std::string subtitle, value, value_caption, scale_label;
  Series a, b;
  bool has_b = false;
  double top = 100;  // 0 = scale to the data
  double floor = 1;
  std::vector<Stat> stats;
  bool seen = true;

  void draw() override {
    const sde::Theme &t = sde::active_theme();
    fl_push_clip(x(), y(), w(), h());
    double top_value = top;
    if (top <= 0) top_value = nice_ceiling(std::max(a.max(), has_b ? b.max() : 0.0) * 1.1, floor);
    int pad = S(18);
    int gx = x() + pad, gy = y() + S(66), gw = w() - 2 * pad;
    int stats_rows = int((stats.size() + stats_columns() - 1) / stats_columns());
    int gh = h() - S(66) - pad - stats_rows * S(40) - (stats_rows ? S(6) : 0);
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), t.background);
      p.fill_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(14), t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(14), t.border);
      double isz = S(34);
      p.fill_rounded(x() + pad, y() + S(16), isz, isz, Sd(9), ca_.alpha(t.dark ? 0.22 : 0.14));
      draw_icon(p, icon_, x() + pad + S(8), y() + S(24), S(18), ca_);
      if (gh > S(30)) draw_body(p, gx, gy, gw, gh, top_value);
      // Stat swatches.
      int cols = stats_columns(), cw = gw / std::max(1, cols);
      for (size_t i = 0; i < stats.size(); ++i) {
        if (!stats[i].has_swatch) continue;
        int sx = gx + int(i % cols) * cw, sy = y() + h() - pad - (stats_rows - int(i / cols)) * S(40);
        p.fill_rounded(sx, sy + S(7), S(8), S(8), Sd(2), stats[i].swatch);
      }
      extra_shapes(p);
    }
    sde::text_font(t.font_size + S(2), true);
    int tx = x() + pad + S(46);
    sde::draw_text(title_, tx, y() + S(14), w() / 2 - S(40), S(22), t.text);
    sde::text_font(t.font_size - S(1));
    sde::draw_text(subtitle, tx, y() + S(34), w() - S(260), S(20), t.text_dim);
    sde::text_font(t.font_size + S(8), true);
    int vw = std::min(S(240), w() / 2);
    sde::draw_text(value, x() + w() - pad - vw - extra_right(), y() + S(10), vw, S(30), t.text, 2);
    sde::text_font(t.font_size - S(1));
    sde::draw_text(value_caption, x() + w() - pad - vw - extra_right(), y() + S(38), vw, S(18), t.text_dim, 2);
    if (gh > S(30) && !scale_label.empty() && !per_core_view()) {
      sde::text_font(t.font_size - S(2));
      std::string label = top <= 0 ? format_rate(top_value) : scale_label;
      sde::draw_text(label, gx + S(8), gy + S(4), gw / 2, S(16), t.text_dim);
    }
    int cols = stats_columns(), cw = gw / std::max(1, cols);
    for (size_t i = 0; i < stats.size(); ++i) {
      int sx = gx + int(i % cols) * cw, sy = y() + h() - pad - (stats_rows - int(i / cols)) * S(40);
      int off = stats[i].has_swatch ? S(14) : 0;
      sde::text_font(t.font_size - S(2));
      sde::draw_text(stats[i].label, sx + off, sy, cw - off - S(6), S(20), t.text_dim);
      sde::text_font(t.font_size, true);
      sde::draw_text(stats[i].value, sx, sy + S(18), cw - S(6), S(20), t.text);
    }
    extra_text();
    fl_pop_clip();
  }

 protected:
  virtual void draw_body(Painter &p, int gx, int gy, int gw, int gh, double top_value) {
    draw_graph(p, gx, gy, gw, gh, a, ca_, has_b ? &b : nullptr, cb_, top_value);
  }
  virtual void extra_shapes(Painter &) {}
  virtual void extra_text() {}
  virtual int extra_right() const { return 0; }
  virtual bool per_core_view() const { return false; }
  int stats_columns() const { return w() > S(640) ? 6 : w() > S(420) ? 4 : 3; }

  Icon icon_;
  std::string title_;
  Color ca_, cb_;
};

class CpuCard : public GraphCard {
 public:
  CpuCard() : GraphCard(Icon::Chip, _("Processor"), palette().cpu, palette().cpu) {}
  std::vector<Series> cores;

  int handle(int e) override {
    if (e == FL_ENTER || e == FL_LEAVE) return 1;
    if (e == FL_MOVE) {
      bool hot = in_toggle();
      if (hot != hot_) hot_ = hot, redraw();
      return 1;
    }
    if (e == FL_PUSH && in_toggle()) {
      per_core_ = !per_core_;
      redraw();
      return 1;
    }
    return GraphCard::handle(e);
  }

 protected:
  bool per_core_view() const override { return per_core_ && cores.size() > 1; }
  int extra_right() const override { return toggle_w() + S(14); }
  int toggle_w() const { return S(96); }
  void toggle_box(int &tx, int &ty, int &tw, int &th) const {
    tw = toggle_w(), th = S(28), tx = x() + w() - S(18) - tw, ty = y() + S(18);
  }
  bool in_toggle() const {
    int tx, ty, tw, th;
    toggle_box(tx, ty, tw, th);
    return Fl::event_inside(tx, ty, tw, th);
  }
  void draw_body(Painter &p, int gx, int gy, int gw, int gh, double top_value) override {
    if (!per_core_view()) {
      GraphCard::draw_body(p, gx, gy, gw, gh, top_value);
      return;
    }
    int n = int(cores.size());
    // Pick a grid close to the card's aspect ratio.
    int cols = std::max(1, int(std::ceil(std::sqrt(n * double(gw) / std::max(1, gh) / 2.2))));
    cols = std::min(n, cols);
    int rows = (n + cols - 1) / cols;
    int gap = S(6);
    double cw = (gw - gap * (cols - 1)) / double(cols), ch = (gh - gap * (rows - 1)) / double(rows);
    for (int i = 0; i < n; ++i) {
      double cx = gx + (i % cols) * (cw + gap), cy = gy + (i / cols) * (ch + gap);
      draw_graph(p, cx, cy, cw, ch, cores[i], ca_, nullptr, ca_, 100, false);
    }
  }
  void extra_shapes(Painter &p) override {
    const sde::Theme &t = sde::active_theme();
    int tx, ty, tw, th;
    toggle_box(tx, ty, tw, th);
    Color fill = per_core_ ? t.accent.alpha(t.dark ? 0.28 : 0.16) : hot_ ? t.hover : t.surface_alt;
    p.fill_rounded(tx + 0.5, ty + 0.5, tw - 1, th - 1, th / 2.0, fill);
    p.stroke_rounded(tx + 0.5, ty + 0.5, tw - 1, th - 1, th / 2.0, per_core_ ? t.accent : t.border);
  }
  void extra_text() override {
    const sde::Theme &t = sde::active_theme();
    int tx, ty, tw, th;
    toggle_box(tx, ty, tw, th);
    sde::text_font(t.font_size - S(1), per_core_);
    sde::draw_text(_("Per core"), tx, ty, tw, th, per_core_ ? t.accent_text : t.text, 1);
    if (per_core_view()) {
      // Core numbers in the corners of the small graphs are noise; the
      // caption names the layout instead.
    }
  }

 private:
  bool per_core_ = false, hot_ = false;
};

std::string temperature(double c) { return c > 0 ? sde::format("%.0f °C", c) : std::string(); }

class PerformancePage : public Page {
 public:
  PerformancePage(int x, int y, int w, int h) : Page(x, y, w, h, _("Performance")) {
    const sde::Theme &t = sde::active_theme();
    scroll_ = new Fl_Scroll(x, y + header_height() - S(6), w, h - header_height() + S(6));
    scroll_->type(Fl_Scroll::VERTICAL);
    scroll_->box(FL_FLAT_BOX);
    scroll_->color(t.background.fl());
    scroll_->scrollbar_size(S(12));
    scroll_->scrollbar.color(t.background.fl());
    scroll_->scrollbar.selection_color(t.border.fl());
    scroll_->scrollbar.box(FL_FLAT_BOX);
    scroll_->scrollbar.slider(FL_FLAT_BOX);
    cpu_ = new CpuCard();
    cpu_->key = "cpu";
    memory_ = new GraphCard(Icon::Memory, _("Memory"), palette().memory, palette().swap);
    memory_->key = "memory";
    memory_->has_b = true;
    memory_->scale_label = "100 %";
    cpu_->scale_label = "100 %";
    scroll_->end();
    resizable(scroll_);
    end();
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    subtitle_->resize(X + S(28), Y + S(48), W - S(56), S(22));
    layout();
  }

  void update(const std::shared_ptr<const Snapshot> &snap, bool visible) override {
    const Snapshot &s = *snap;
    for (auto &kv : cards_) kv.second->seen = false;
    bool changed = false;

    // CPU
    cpu_->a.push(s.cpu_total);
    cpu_->cores.resize(s.cpu_cores.size());
    for (size_t i = 0; i < s.cpu_cores.size(); ++i) cpu_->cores[i].push(s.cpu_cores[i]);
    cpu_->subtitle = s.cpu_model.empty() ? sde::format(_("%d logical processors"), s.cpu_count)
                                         : s.cpu_model;
    cpu_->value = format_percent(s.cpu_total);
    cpu_->value_caption = sde::format(_("%d logical processors"), s.cpu_count);
    cpu_->stats = {
        {_("Speed"), s.cpu_mhz > 0 ? sde::format("%.2f GHz", s.cpu_mhz / 1000) : std::string("–"), {}, false},
        {_("Load average"), sde::format("%.2f  %.2f  %.2f", s.load.one, s.load.five, s.load.fifteen), {}, false},
        {_("Threads"), std::to_string(s.load.threads), {}, false},
        {_("Up time"), format_duration(s.uptime), {}, false},
    };
    if (s.cpu_max_mhz > 0) cpu_->stats.insert(cpu_->stats.begin() + 1, {_("Maximum speed"), sde::format("%.2f GHz", s.cpu_max_mhz / 1000), {}, false});
    if (s.cpu_temperature > 0) cpu_->stats.push_back({_("Temperature"), temperature(s.cpu_temperature), {}, false});

    // Memory
    const MemInfo &m = s.mem;
    double mem_pct = m.total ? 100.0 * double(m.used()) / double(m.total) : 0;
    double swap_pct = m.swap_total ? 100.0 * double(m.swap_used()) / double(m.swap_total) : 0;
    memory_->a.push(mem_pct);
    memory_->b.push(swap_pct);
    memory_->subtitle = sde::format(_("%s installed"), format_bytes(m.total).c_str());
    memory_->value = format_bytes(m.used());
    memory_->value_caption = sde::format(_("%s in use"), format_percent(mem_pct).c_str());
    memory_->stats = {
        {_("In use"), format_bytes(m.used()), palette().memory, true},
        {_("Available"), format_bytes(m.available), {}, false},
        {_("Cached"), format_bytes(m.cached + m.reclaimable), {}, false},
        {_("Swap"), m.swap_total ? format_bytes(m.swap_used()) + " / " + format_bytes(m.swap_total) : std::string(_("None")),
         palette().swap, m.swap_total > 0},
    };

    // GPUs
    for (auto &g : s.gpus) {
      GraphCard *c = card("gpu:" + g.name, Icon::Gpu, _("Graphics"), palette().gpu, palette().gpu.mix(palette().memory, 0.5), changed);
      double busy = std::max(0.0, g.busy);
      double vram = g.vram_total ? 100.0 * double(g.vram_used) / double(g.vram_total) : 0;
      c->a.push(busy);
      c->has_b = g.vram_total > 0;
      c->b.push(vram);
      c->scale_label = "100 %";
      c->subtitle = g.name + (g.driver.empty() ? "" : " · " + g.driver);
      c->value = g.busy >= 0 ? format_percent(busy) : "–";
      c->value_caption = _("Utilization");
      c->stats = {{_("Utilization"), format_percent(busy), palette().gpu, true}};
      if (g.vram_total)
        c->stats.push_back({_("Video memory"), format_bytes(g.vram_used) + " / " + format_bytes(g.vram_total),
                            palette().gpu.mix(palette().memory, 0.5), true});
      if (g.temperature > 0) c->stats.push_back({_("Temperature"), temperature(g.temperature), {}, false});
    }

    // Disks
    for (auto &d : s.disks) {
      GraphCard *c = card("disk:" + d.name, Icon::Drive, sde::format(_("Disk %s"), d.name.c_str()), palette().disk,
                          palette().disk_write, changed);
      c->a.push(d.read_rate);
      c->b.push(d.write_rate);
      c->has_b = true;
      c->top = 0;
      c->floor = 1024 * 1024;
      c->scale_label = "auto";
      std::string kind = d.removable ? _("Removable") : d.rotational ? _("Hard disk") : _("Solid state");
      c->subtitle = (d.model.empty() ? kind : d.model + " · " + kind) + " · " + format_bytes(d.size);
      c->value = sde::format("%.0f %%", d.active);
      c->value_caption = _("Active time");
      c->stats = {
          {_("Read"), format_rate(d.read_rate), palette().disk, true},
          {_("Write"), format_rate(d.write_rate), palette().disk_write, true},
          {_("Active time"), sde::format("%.0f %%", d.active), {}, false},
          {_("Capacity"), format_bytes(d.size), {}, false},
      };
    }

    // Networks
    for (auto &n : s.nets) {
      GraphCard *c = card("net:" + n.name, Icon::Network, n.wireless ? _("Wi-Fi") : _("Network"), palette().net,
                          palette().net_send, changed);
      c->a.push(n.rx_rate);
      c->b.push(n.tx_rate);
      c->has_b = true;
      c->top = 0;
      c->floor = 64 * 1024;
      c->scale_label = "auto";
      c->subtitle = n.name + (n.address.empty() ? "" : " · " + n.address) + (n.up ? "" : std::string(" · ") + _("Disconnected"));
      c->value = format_bits_rate(n.rx_rate + n.tx_rate);
      c->value_caption = _("Throughput");
      c->stats = {
          {_("Receive"), format_rate(n.rx_rate), palette().net, true},
          {_("Send"), format_rate(n.tx_rate), palette().net_send, true},
          {_("Received"), format_bytes(n.rx_total), {}, false},
          {_("Sent"), format_bytes(n.tx_total), {}, false},
      };
    }

    // Battery
    if (s.battery_percent >= 0) {
      GraphCard *c = card("battery", Icon::Battery, _("Battery"), palette().battery, palette().battery, changed);
      c->a.push(s.battery_percent);
      c->scale_label = "100 %";
      c->subtitle = s.battery_status;
      c->value = sde::format("%d %%", s.battery_percent);
      c->value_caption = s.battery_status;
      c->stats = {{_("Charge"), sde::format("%d %%", s.battery_percent), palette().battery, true},
                  {_("Status"), s.battery_status.empty() ? "–" : s.battery_status, {}, false}};
    }

    for (auto it = cards_.begin(); it != cards_.end();) {
      if (it->second->seen) {
        ++it;
        continue;
      }
      scroll_->remove(it->second);
      Fl::delete_widget(it->second);
      order_.erase(std::remove(order_.begin(), order_.end(), it->first), order_.end());
      it = cards_.erase(it);
      changed = true;
    }
    if (changed || first_) {
      first_ = false;
      layout();
      if (visible) redraw();
    }
    if (!visible) return;
    subtitle_->text(sde::format(_("Up time %s · Load %.2f, %.2f, %.2f · %d threads"), format_duration(s.uptime).c_str(),
                                s.load.one, s.load.five, s.load.fifteen, s.load.threads));
    // Only cards in view are repainted.
    int top = scroll_->y(), bottom = scroll_->y() + scroll_->h();
    for (Fl_Widget *c : all_cards())
      if (c->y() + c->h() > top && c->y() < bottom) c->redraw();
  }

 private:
  GraphCard *card(const std::string &key, Icon icon, const std::string &title, const Color &a, const Color &b,
                  bool &changed) {
    auto it = cards_.find(key);
    if (it != cards_.end()) {
      it->second->seen = true;
      return it->second;
    }
    auto *c = new GraphCard(icon, title, a, b);
    c->key = key;
    c->seen = true;
    scroll_->add(c);
    cards_[key] = c;
    order_.push_back(key);
    // Keep a stable, readable order: GPU, disks, networks, battery.
    auto rank = [](const std::string &k) {
      return k.compare(0, 4, "gpu:") == 0 ? 0 : k.compare(0, 5, "disk:") == 0 ? 1 : k.compare(0, 4, "net:") == 0 ? 2 : 3;
    };
    std::stable_sort(order_.begin(), order_.end(), [&](const std::string &x, const std::string &y) { return rank(x) < rank(y); });
    changed = true;
    return c;
  }

  std::vector<Fl_Widget *> all_cards() {
    std::vector<Fl_Widget *> out{cpu_, memory_};
    for (auto &k : order_) out.push_back(cards_[k]);
    return out;
  }

  void layout() {
    int sx = scroll_->x(), sy = scroll_->y() - scroll_->yposition();
    int pad = S(28), gap = S(16);
    int width = scroll_->w() - 2 * pad - S(4);
    int y = sy + S(6);
    cpu_->resize(sx + pad, y, width, S(300));
    y += S(300) + gap;
    std::vector<Fl_Widget *> rest{memory_};
    for (auto &k : order_) rest.push_back(cards_[k]);
    int cols = width >= S(760) ? 2 : 1;
    int cw = (width - gap * (cols - 1)) / cols, ch = S(250);
    for (size_t i = 0; i < rest.size(); ++i) {
      int col = int(i) % cols;
      rest[i]->resize(sx + pad + col * (cw + gap), y, cw, ch);
      if (col == cols - 1 || i + 1 == rest.size()) y += ch + gap;
    }
    // Bottom margin inside the scroll area.
    if (!spacer_) {
      spacer_ = new Fl_Box(0, 0, 1, 1);
      scroll_->add(spacer_);
    }
    spacer_->resize(sx, y, 1, S(12));
    scroll_->init_sizes();
    scroll_->redraw();
  }

  Fl_Scroll *scroll_;
  CpuCard *cpu_;
  GraphCard *memory_;
  Fl_Box *spacer_ = nullptr;
  std::map<std::string, GraphCard *> cards_;
  std::vector<std::string> order_;
  bool first_ = true;
};

}  // namespace

Page *make_performance_page(int x, int y, int w, int h) { return new PerformancePage(x, y, w, h); }

}  // namespace sysmon
