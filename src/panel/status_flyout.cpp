// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_flyout.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Scroll.H>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>

#include "controls.h"
#include "glyphs.h"
#include "i18n.h"
#include "panel.h"
#include "panel_widgets.h"
#include "popup_style.h"
#include "quick.h"
#include "spawn.h"
#include "status_model.h"
#include "touchpad_monitor.h"
#include "tray_icons.h"
#include "util.h"
#include "widgets.h"

using namespace sde;

namespace {
using IconFn = std::function<void(Painter &, double, double, double, const Color &)>;
using TextFn = std::function<std::string()>;

void later(std::function<void()> fn) {
  auto *heap = new std::function<void()>(std::move(fn));
  Fl::add_timeout(0, [](void *d) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(d));
    (*f)();
  }, heap);
}

const Theme &theme() { return Panel::instance->theme(); }

Color led_color(NetLed led) {
  return led == NetLed::GREEN    ? Color::hex(0x22C55E)
         : led == NetLed::YELLOW ? Color::hex(0xEAB308)
                                 : Color::hex(0xEF4444);
}

// A group of rows. alpha51: plain rows with a thin separator above every
// group but the first (no card behind them).
class FlyCard : public Fl_Group {
 public:
  FlyCard(int x, int y, int w, bool rule = false) : Fl_Group(x, y, w, S(40)), rule_(rule) {
    box(FL_NO_BOX);
    resizable(nullptr);
  }
  void draw() override {
    const Theme &t = theme();
    if (rule_) {
      Painter p(window()->w(), window()->h());
      p.line(x(), y() + 0.5, x() + w(), y() + 0.5, t.border);
    }
    draw_children();
  }

 private:
  bool rule_;
};

// Text; the label (not drawn) names it for window inspection and UI tests.
class FlyLabel : public Fl_Widget {
 public:
  FlyLabel(int x, int y, int w, int h, TextFn text, int style = 0)
      : Fl_Widget(x, y, w, h), text_(std::move(text)), style_(style) {
    copy_label(text_().c_str());
    labeltype(FL_NO_LABEL);
    clear_visible_focus();
  }
  void draw() override {
    const Theme &t = theme();
    text_font(t.font_size, style_ == 1);
    Color c = style_ == 2 ? t.text_dim : style_ == 3 ? t.accent_text : t.text;
    draw_text(text_(), x(), y(), w(), h(), c);
  }

 private:
  TextFn text_;
  int style_;  // 0 text, 1 title, 2 dim, 3 accent
};

// A clickable row: icon, title and an optional dim second line; with
// |on| it ends with an on/off switch (the whole row toggles).
class FlyRow : public Fl_Button {
 public:
  FlyRow(int x, int y, int w, int h, std::string title, IconFn icon, std::function<void()> action,
         TextFn sub = nullptr, std::function<bool()> on = nullptr)
      : Fl_Button(x, y, w, h),
        title_(std::move(title)),
        icon_(std::move(icon)),
        action_(std::move(action)),
        sub_(std::move(sub)),
        on_(std::move(on)) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    copy_tooltip(title_.c_str());
    callback([](Fl_Widget *w, void *) {
      auto *r = static_cast<FlyRow *>(w);
      if (r->action_) {
        auto fn = r->action_;
        later(fn);  // the action may rebuild the flyout and delete this row
      }
    });
  }
  bool is_switch() const { return bool(on_); }
  void draw() override {
    const Theme &t = theme();
    bool hot = Fl::belowmouse() == this && active_r();
    Color ink = active_r() ? t.text : t.text_dim;
    double sw = Sd(36), sh = Sd(20);
    {
      Painter p(window()->w(), window()->h());
      if (hot || value()) p.fill_rounded(x(), y(), w(), h(), S(6), value() ? t.accent.alpha(0.2) : t.hover);
      double size = S(popup_style::kIcon);
      if (icon_) icon_(p, x() + S(8), y() + (h() - size) / 2.0, size, ink);
      if (on_) {
        bool on = on_();
        double sx = x() + w() - sw - S(8), sy = y() + (h() - sh) / 2;
        Color track = on ? t.accent : t.border;
        if (!active_r()) track = track.alpha(0.5);
        p.fill_rounded(sx, sy, sw, sh, sh / 2, track);
        p.circle(on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh / 2 - Sd(3),
                 on ? Color::hex(0xFFFFFF) : t.surface);
      }
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(6));
    }
    int tx = x() + (icon_ ? S(32) : S(8)), tw = w() - (tx - x()) - (on_ ? int(sw) + S(16) : S(8));
    std::string sub = sub_ ? sub_() : "";
    text_font(t.font_size);
    if (sub.empty()) {
      draw_text(title_, tx, y(), tw, h(), ink);
    } else {
      draw_text(title_, tx, y() + S(2), tw, h() / 2 - S(1), ink);
      text_font(std::max(S(9), t.font_size - 1));
      draw_text(sub, tx, y() + h() / 2 - S(1), tw, h() / 2 - S(2), t.text_dim);
    }
  }
  int handle(int event) override {
    if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS || event == FL_UNFOCUS) {
      redraw();
      return 1;
    }
    if (event == FL_KEYBOARD && Fl::focus() == this &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter ||
         Fl::event_key() == ' ')) {
      do_callback();
      return 1;
    }
    return Fl_Button::handle(event);
  }

 private:
  std::string title_;
  IconFn icon_;
  std::function<void()> action_;
  TextFn sub_;
  std::function<bool()> on_;
};

// Small square button with an icon; |tip| names it.
class FlyIconButton : public Fl_Button {
 public:
  FlyIconButton(int x, int y, int size, const std::string &tip, IconFn icon,
                std::function<void()> action)
      : Fl_Button(x, y, size, size), icon_(std::move(icon)), action_(std::move(action)) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    copy_tooltip(tip.c_str());
    callback([](Fl_Widget *w, void *) {
      auto fn = static_cast<FlyIconButton *>(w)->action_;
      later(fn);
    });
  }
  void draw() override {
    const Theme &t = theme();
    Painter p(window()->w(), window()->h());
    bool hot = Fl::belowmouse() == this;
    if (hot || value()) p.fill_rounded(x(), y(), w(), h(), S(6), t.hover);
    double s = std::min<double>(std::min(w(), h()) * 0.6, S(popup_style::kIcon));
    icon_(p, x() + (w() - s) / 2, y() + (h() - s) / 2, s, t.text);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(6));
  }
  int handle(int event) override {
    if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS || event == FL_UNFOCUS) {
      redraw();
      return 1;
    }
    if (event == FL_KEYBOARD && Fl::focus() == this &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      do_callback();
      return 1;
    }
    return Fl_Button::handle(event);
  }

 private:
  IconFn icon_;
  std::function<void()> action_;
};

class FlyButton : public FlatButton {
 public:
  FlyButton(int x, int y, int w, int h, const std::string &text, const std::string &name,
            std::function<void()> fn, bool primary = false)
      : FlatButton(x, y, w, h, text, primary), fn_(std::move(fn)) {
    copy_tooltip(name.c_str());
    callback([](Fl_Widget *w, void *) {
      auto fn = static_cast<FlyButton *>(w)->fn_;
      later(fn);
    });
  }

 private:
  std::function<void()> fn_;
};

// The network header: icon with the LED, connection name and state.
class NetworkHeader : public Fl_Widget {
 public:
  NetworkHeader(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    clear_visible_focus();
    copy_label(SystemStatus::get().network.status_text().c_str());
    labeltype(FL_NO_LABEL);
  }
  void draw() override {
    const Theme &t = theme();
    const auto &n = SystemStatus::get().network;
    double size = S(20), ix = x() + S(6), iy = y() + (h() - size) / 2;
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(ix - S(6), iy - S(6), size + S(12), size + S(12), S(8),
                     t.accent.alpha(t.dark ? 0.22 : 0.12));
      if (n.kind == "ethernet")
        tray_icon::wired(p, ix, iy, size, true, t.text);
      else if (n.kind == "wifi")
        tray_icon::wifi(p, ix, iy, size, std::max(1, (n.signal + 24) / 25), true, t.text);
      else
        tray_icon::offline(p, ix, iy, size, t.text);
      NetLed led = network_led(n.connected(), n.connectivity, false, PanelControls::get().net_verdict);
      double r = Sd(3.5);
      p.circle(ix + size + Sd(1), iy + size - Sd(1), r + Sd(1.5), t.surface_alt);
      p.circle(ix + size + Sd(1), iy + size - Sd(1), r, led_color(led));
    }
    int tx = x() + int(size) + S(20), tw = w() - (tx - x());
    std::string name = n.connected() ? n.connection
                       : n.available ? std::string(_("Not connected"))
                                     : std::string(_("Network unavailable"));
    std::string state;
    if (!n.available)
      state = _("NetworkManager is not running");
    else if (!n.connected())
      state = _("No network connection");
    else if (!network_verdict_text(PanelControls::get().net_verdict).empty())
      state = _(network_verdict_text(PanelControls::get().net_verdict).c_str());
    else if (n.connectivity == NetworkState::FULL)
      state = std::string(n.kind == "wifi" ? _("Wi-Fi") : _("Ethernet")) + " · " + _("Internet access");
    else if (n.portal())
      state = _("Sign in to network");
    else if (n.no_internet())
      state = _("Local network only · no internet");
    else
      state = n.kind == "wifi" ? _("Wi-Fi · connected") : _("Ethernet · connected");
    text_font(t.font_size, true);
    draw_text(name, tx, y() + S(2), tw, h() / 2 - S(1), t.text);
    text_font(std::max(S(9), t.font_size - 1));
    draw_text(state, tx, y() + h() / 2, tw, h() / 2 - S(2), t.text_dim);
  }
};

// The mini touchpad of the flyout: follows the finger like the panel icon.
class TouchpadPreview : public Fl_Widget {
 public:
  TouchpadPreview(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    clear_visible_focus();
    copy_label("Touchpad preview");
    labeltype(FL_NO_LABEL);
  }
  void draw() override {
    const Theme &t = theme();
    Painter p(window()->w(), window()->h());
    double pw = std::min(double(w()), h() * 1.6), ph = h();
    draw_touchpad(p, x() + (w() - pw) / 2, y(), pw, ph, TouchpadMonitor::get().state(),
                  TouchpadMonitor::get().enabled(), true, t.text, t.accent);
  }
};

IconFn wifi_icon(int strength) {
  return [strength](Painter &p, double x, double y, double s, const Color &c) {
    tray_icon::wifi(p, x, y, s, strength, true, c);
  };
}
IconFn wired_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    tray_icon::wired(p, x, y, s, true, c);
  };
}
IconFn gear_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) { glyph::gear(p, x, y, s, c); };
}
IconFn dots_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    for (int i = 0; i < 3; ++i) p.circle(x + s * (0.2 + 0.3 * i), y + s / 2, s * 0.09, c);
  };
}
IconFn refresh_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    cairo_t *cr = p.cr();
    p.set(c);
    cairo_set_line_width(cr, std::max(1.0, s / 12));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + s / 2, y + s / 2, s * 0.34, 0.6, 2 * M_PI - 0.2);
    cairo_stroke(cr);
    double ax = x + s / 2 + s * 0.34 * std::cos(0.6), ay = y + s / 2 + s * 0.34 * std::sin(0.6);
    cairo_move_to(cr, ax + s * 0.14, ay - s * 0.02);
    cairo_line_to(cr, ax, ay);
    cairo_line_to(cr, ax - s * 0.03, ay - s * 0.15);
    cairo_stroke(cr);
  };
}
IconFn drive_icon(bool card) {
  return [card](Painter &p, double x, double y, double s, const Color &c) {
    tray_icon::drive(p, x, y, s, card, c);
  };
}
IconFn touchpad_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    TouchpadMonitor::State idle;
    draw_touchpad(p, x, y + s * 0.15, s, s * 0.7, idle, true, false, c, c);
  };
}
}  // namespace

struct FlyBuilder {
  StatusFlyout *fly;
  int pad, inner, y;
  FlyCard *card = nullptr;
  int groups = 0;
  void start_card(const std::string &title = "") {
    card = new FlyCard(pad, y, inner, groups > 0);
    y += groups > 0 ? S(6) : 0;
    ++groups;
    if (!title.empty()) {
      new FlyLabel(pad + S(8), y, inner - S(16), S(popup_style::kSection), [title] { return title; }, 1);
      y += S(popup_style::kSection);
    }
  }
  void end_card() {
    card->size(inner, y - card->y());
    card->end();
    y += S(4);
    card = nullptr;
  }
  // A separator before a row that follows the groups (the settings link).
  void rule() {
    auto *r = new FlyCard(pad, y, inner, groups > 0);
    r->size(inner, S(1));
    r->end();
    y += S(6);
  }
  FlyRow *row(const std::string &title, IconFn icon, std::function<void()> fn, TextFn sub = nullptr,
              std::function<bool()> on = nullptr, bool enabled = true) {
    int h = sub ? S(popup_style::kRowSub) : S(popup_style::kRow);
    auto *r = new FlyRow(pad, y, inner, h, title, std::move(icon), std::move(fn),
                         std::move(sub), std::move(on));
    if (!enabled) r->deactivate();
    y += h + S(2);
    return r;
  }
  void info(const std::string &text, int style = 2) {
    new FlyLabel(pad + S(8), y, inner - S(16), S(20), [text] { return text; }, style);
    y += S(20);
  }
  std::function<void()> launch(std::vector<std::string> cmd) {
    StatusFlyout *f = fly;
    return [f, cmd] {
      f->close();
      spawn_detached(cmd);
    };
  }
};

// ----------------------------------------------------------------- window

StatusFlyout::StatusFlyout() : PanelPopup("Network") { end(); }

bool StatusFlyout::handles(const std::string &section) {
  return section == "network" || section == "drives" || section == "touchpad";
}

void StatusFlyout::select_section(const std::string &section) {
  section_ = handles(section) ? section : "network";
  // Untranslated window titles, like the other flyouts.
  copy_label(section_ == "drives" ? "Drives" : section_ == "touchpad" ? "Touchpad" : "Network");
}

void StatusFlyout::prepare() {
  rebuild();
  if (section_ == "network") {
    PanelControls::get().check_network();
    SystemStatus::get().check_connectivity();
    SystemStatus::get().refresh_network();
  }
  if (section_ == "drives") PanelControls::get().refresh_drives();
  if (section_ == "touchpad") TouchpadMonitor::get().want_motion(true);
}

void StatusFlyout::closed() {
  Fl::remove_timeout(rebuild_timeout, this);
  rebuild_pending_ = false;
  if (section_ == "touchpad") Panel::instance->status_icon_changed("touchpad");
  // Drives that were removed safely leave the list with the flyout.
  auto &tasks = PanelControls::get().drive_tasks;
  for (auto it = tasks.begin(); it != tasks.end();)
    it = it->second.state == "removed" || it->second.state == "error" ? tasks.erase(it) : ++it;
  PanelControls::get().message.clear();
  SystemStatus::get().message.clear();
}

std::string StatusFlyout::structure() const {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  std::string sig = section_ + "|" + s.message + "|" + c.message;
  if (section_ == "network") {
    sig += std::to_string(s.network.available) + std::to_string(s.network.has_wifi) +
           std::to_string(s.network.wifi_enabled) + s.network.kind + s.network.connection +
           std::to_string(s.network.connectivity) + std::to_string(c.airplane) + c.net_verdict;
    for (const auto &w : s.wifi) sig += "|" + w.ssid + std::to_string(w.active) + std::to_string(w.signal / 25);
    for (const auto &d : c.devices) sig += "|" + d.device + d.state + d.connection + d.ip;
  } else if (section_ == "drives") {
    for (const auto &d : c.drives) {
      sig += "|" + d.path + d.label;
      for (const auto &p : d.parts) sig += p.mountpoint;
    }
    for (const auto &t : c.drive_tasks) sig += "|" + t.first + t.second.state + t.second.text;
    for (const auto &f : c.drive_free) sig += "|" + std::to_string(f.second / 1000000);
  } else {
    sig += std::to_string(TouchpadMonitor::get().present()) +
           std::to_string(TouchpadMonitor::get().enabled());
  }
  return sig;
}

void StatusFlyout::status_changed() {
  if (!shown()) return;
  if (structure() == structure_) {
    redraw();
    return;
  }
  if (rebuild_pending_) return;
  rebuild_pending_ = true;
  Fl::add_timeout(0.05, rebuild_timeout, this);
}

void StatusFlyout::rebuild_timeout(void *data) {
  auto *self = static_cast<StatusFlyout *>(data);
  self->rebuild_pending_ = false;
  if (!self->shown()) return;
  std::string focus = focused_name(self);
  int scroll = self->body_ ? self->body_->yposition() : 0;
  self->rebuild();
  if (self->body_)
    self->body_->scroll_to(0, std::max(0, std::min(scroll, self->content_height_ - self->body_->h())));
  self->reposition();
  restore_focus(self, focus);
  self->redraw();
}

void StatusFlyout::rebuild() {
  structure_ = structure();
  clear();
  resizable(nullptr);
  const auto &t = Panel::instance->theme();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int W = std::min(S(popup_style::kFlyoutW), mw - S(16));
  int pad = S(popup_style::kPad), scrollbar = S(6), inner = W - 2 * pad - scrollbar;
  auto *panel = Panel::instance;
  int available =
      std::max(S(120), (t.panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my) - S(16));
  header_ = S(popup_style::kHeader);
  begin();
  body_ = new Fl_Scroll(S(2), header_, W - S(4), std::max(S(40), available - header_));
  body_->type(Fl_Scroll::VERTICAL);
  body_->resizable(nullptr);
  body_->scrollbar_size(scrollbar);
  body_->box(FL_FLAT_BOX);
  body_->color(t.surface.fl());
  new Fl_Box(body_->x(), body_->y(), 1, 1);
  FlyBuilder b{this, pad, inner, header_ + S(6)};
  if (section_ == "network")
    network_section(b);
  else if (section_ == "drives")
    drives_section(b);
  else
    touchpad_section(b);
  content_height_ = b.y - header_ + S(4);
  new Fl_Box(pad, b.y, inner, S(4));
  body_->end();
  int H = std::min(available, header_ + content_height_);
  body_->size(W - S(4), H - header_);
  begin();
  std::string title = section_ == "drives"     ? _("Drives")
                      : section_ == "touchpad" ? _("Touchpad")
                                               : _("Network");
  new FlyLabel(pad + S(8), S(4), W - 2 * pad - S(44), header_ - S(4), [title] { return title; }, 1);
  if (section_ == "network")
    new FlyIconButton(W - pad - S(30), (header_ - S(30)) / 2, S(30), _("More network options"), dots_icon(),
                      [this] { network_menu(); });
  end();
  resizable(nullptr);
  init_sizes();
  size(W, H);
}

void StatusFlyout::draw() {
  const auto &t = Panel::instance->theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    if (t.glass) p.fill_rect(0, 0, w(), header_, t.surface_alt.mix(t.surface, 0.55));
    stroke_outline(p, t.border);
    p.line(S(popup_style::kPad), header_ - 0.5, w() - S(popup_style::kPad), header_ - 0.5, t.border);
  }
  draw_children();
  if (qa_dump_enabled() && body_)
    qa_dump(this, "@body\t" + std::to_string(body_->x()) + "\t" + std::to_string(body_->y()) +
                      "\t" + std::to_string(body_->w()) + "\t" + std::to_string(body_->h()) +
                      "\t1\n");
}

int StatusFlyout::handle(int event) {
  if (event == FL_MOUSEWHEEL && body_ && Fl::event_inside(body_)) {
    auto *target = Fl::belowmouse();
    if (target && Fl::event_inside(target) && dynamic_cast<Fl_Slider *>(target))
      return PanelPopup::handle(event);
    int limit = std::max(0, content_height_ - body_->h());
    body_->scroll_to(0, std::max(0, std::min(limit, body_->yposition() + Fl::event_dy() * S(36))));
    redraw();
    return 1;
  }
  return PanelPopup::handle(event);
}

// ---------------------------------------------------------------- network

void StatusFlyout::network_menu() {
  const auto &n = SystemStatus::get().network;
  bool airplane = PanelControls::get().airplane;
  std::vector<std::pair<std::string, std::function<void()>>> actions;
  actions.push_back({_("Troubleshoot"), [] { SystemStatus::get().troubleshoot_network(); }});
  actions.push_back({_("Check internet connection"), [] {
                       auto &s = SystemStatus::get();
                       s.message = _("Checking the internet connection...");
                       s.changed_now();
                       s.check_connectivity(true, [](int) {
                         auto &st = SystemStatus::get();
                         st.message = st.network.status_text();
                         st.changed_now();
                       });
                     }});
  if (n.portal())
    actions.push_back({_("Open sign-in page"), [this] {
                         close();
                         SystemStatus::get().open_portal();
                       }});
  actions.push_back({airplane ? _("Turn airplane mode off") : _("Airplane mode"), [airplane] {
                       PanelControls::get().wireless_off(!airplane);
                     }});
  actions.push_back({_("Network settings"), [this] {
                       close();
                       spawn_detached({"sde-settings", "--page", "network"});
                     }});
  std::vector<Fl_Menu_Item> items;
  for (const auto &a : actions) items.push_back({a.first.c_str(), 0, nullptr, nullptr, 0, 0, 0, 0, 0});
  items.push_back({nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0});
  // Below the "⋯" button, right-aligned with the flyout's content.
  int mx = w() - S(12) - S(220), my = S(42);
  int ox = Fl::event_x_root() - Fl::event_x(), oy = Fl::event_y_root() - Fl::event_y();
  int dx = x() - ox, dy = y() - oy;  // this window relative to the event's window
  const Fl_Menu_Item *m = items.front().popup(mx + dx, my + dy);
  if (!m) return;
  size_t index = size_t(m - items.data());
  if (index < actions.size()) later(actions[index].second);
}

void StatusFlyout::network_section(FlyBuilder &b) {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  const auto &n = s.network;
  b.start_card();
  new NetworkHeader(b.pad + S(4), b.y, b.inner - S(8), S(40));
  b.y += S(44);
  std::string message = !c.message.empty() ? c.message : s.message;
  if (!message.empty()) b.info(message);
  if (c.net_verdict == "proxy-required" || c.net_verdict == "no-dns" || c.net_verdict == "clock")
    b.row(c.net_verdict == "proxy-required" ? _("Proxy settings")
          : c.net_verdict == "clock"        ? _("Date and time settings")
                                            : _("DNS settings"),
          gear_icon(), b.launch({"sde-settings", "--page", c.net_verdict == "clock" ? "system" : "network"}),
          [] { return std::string(_(network_verdict_text(PanelControls::get().net_verdict).c_str())); });
  if (n.portal())
    b.row(_("Open sign-in page"), wifi_icon(4), [this] {
      close();
      SystemStatus::get().open_portal();
    });
  if (n.available && n.has_wifi)
    b.row(_("Wi-Fi"), wifi_icon(n.wifi_enabled ? 4 : 0),
          [] {
            auto &st = SystemStatus::get();
            st.set_wifi_enabled(!st.network.wifi_enabled);
          },
          [] {
            const auto &nn = SystemStatus::get().network;
            if (!nn.wifi_enabled) return std::string(_("Off"));
            if (nn.kind == "wifi") return std::string(_("Connected")) + " · " + nn.connection;
            // Wired first in NetworkState, Wi-Fi connected as well.
            for (const auto &w : SystemStatus::get().wifi)
              if (w.active) return std::string(_("Connected")) + " · " + w.ssid;
            return std::string(_("On · not connected"));
          },
          [] { return SystemStatus::get().network.wifi_enabled; }, !c.airplane);
  int wired = 0;
  for (const auto &d : c.devices) wired += d.kind == "ethernet";
  for (const auto &d : c.devices) {
    if (d.kind != "ethernet") continue;
    std::string device = d.device;
    bool plugged = d.state != "unavailable";
    std::string title = wired > 1 ? std::string(_("Ethernet")) + " · " + device : _("Ethernet");
    b.row(title, wired_icon(),
          [device] {
            auto &pc = PanelControls::get();
            for (const auto &dd : pc.devices)
              if (dd.device == device)
                pc.network_device(device, !starts_with(dd.state, "connect"));
          },
          [device] {
            for (const auto &dd : PanelControls::get().devices) {
              if (dd.device != device) continue;
              if (dd.state == "unavailable") return std::string(_("Cable unplugged"));
              if (starts_with(dd.state, "connecting")) return std::string(_("Connecting..."));
              if (dd.state == "connected") {
                std::string text = std::string(_("Connected")) + " · " + dd.connection;
                return text;
              }
              return std::string(_("Off"));
            }
            return std::string();
          },
          [device] {
            for (const auto &dd : PanelControls::get().devices)
              if (dd.device == device) return starts_with(dd.state, "connect");
            return false;
          },
          plugged);
    if (d.state == "connected" && !d.ip.empty()) {
      std::string line = "IP " + d.ip + (d.gateway.empty() ? "" : " · " + std::string(_("Gateway")) + " " + d.gateway);
      new FlyLabel(b.pad + S(32), b.y - S(4), b.inner - S(40), S(18), [line] { return line; }, 2);
      b.y += S(16);
    }
  }
  if (c.airplane) b.info(_("Airplane mode is on."));
  b.end_card();
  if (n.available && n.has_wifi && n.wifi_enabled && !c.airplane) {
    b.start_card();
    new FlyLabel(b.pad + S(8), b.y, b.inner - S(52), S(28), [] { return std::string(_("Wi-Fi networks")); }, 1);
    new FlyIconButton(b.pad + b.inner - S(32), b.y, S(28), _("Scan Wi-Fi networks"), refresh_icon(), [] {
      auto &st = SystemStatus::get();
      st.message = _("Looking for Wi-Fi networks...");
      st.changed_now();
      st.refresh_network(true);
    });
    b.y += S(30);
    if (s.wifi.empty()) b.info(_("No Wi-Fi networks found."));
    size_t shown = 0;
    for (const auto &w : s.wifi) {
      if (++shown > 12) break;
      std::string name = w.ssid + (w.secure ? " · " + std::string(_("Secured")) : "");
      WifiNetwork net = w;
      b.row(name, wifi_icon(std::max(1, (w.signal + 24) / 25)),
            [net] {
              if (!net.active) SystemStatus::get().connect_wifi(net);
            },
            w.active ? TextFn([] { return std::string(_("Connected")); }) : TextFn());
    }
    b.end_card();
  }
  b.rule();
  b.row(_("Network settings"), gear_icon(), b.launch({"sde-settings", "--page", "network"}));
}

// ----------------------------------------------------------------- drives

void StatusFlyout::drives_section(FlyBuilder &b) {
  const auto &c = PanelControls::get();
  static const std::map<std::string, const char *> kinds = {{"usb", N_("USB flash drive")},
                                                            {"card", N_("Memory card")},
                                                            {"hdd", N_("External hard disk")},
                                                            {"ssd", N_("External SSD")}};
  if (c.drives.empty()) b.info(_("No removable drives are plugged in."));
  int gap = S(8);
  for (const auto &d : c.drives) {
    b.start_card();
    std::string name = d.name, label = d.label;
    auto task = c.drive_tasks.find(name);
    bool working = task != c.drive_tasks.end() &&
                   (task->second.state == "ejecting" || task->second.state == "mounting");
    {
      auto *icon = new FlyRow(b.pad, b.y, b.inner, S(popup_style::kRowSub), label, drive_icon(d.kind == "card"),
                              nullptr, [d] {
                                auto k = kinds.find(d.kind);
                                return std::string(_(k == kinds.end() ? N_("Drive") : k->second)) +
                                       " · " + human_size(d.size);
                              });
      icon->clear_visible_focus();
      icon->copy_tooltip(("Drive " + label).c_str());
      b.y += S(popup_style::kRowSub) + S(2);
    }
    for (const auto &p : d.parts) {
      std::string line;
      std::string part = p.label.empty() ? p.path : p.label;
      if (!p.mountpoint.empty()) {
        auto free = c.drive_free.find(p.mountpoint);
        line = free != c.drive_free.end() && p.size > 0
                   ? format(_("%s free of %s"), human_size(free->second).c_str(),
                            human_size(p.size).c_str())
                   : std::string(_("Mounted"));
        line += " · " + p.mountpoint;
      } else {
        line = std::string(_("Not mounted")) + (p.fstype.empty() ? "" : " · " + p.fstype);
      }
      if (d.parts.size() > 1) line = part + ": " + line;
      b.info(line);
    }
    if (task != c.drive_tasks.end() && !task->second.text.empty()) {
      // One sentence per line, so "in use by ..." and the advice both show.
      int style = task->second.state == "busy" || task->second.state == "error" ? 3 : 2;
      std::string text = task->second.text;
      size_t at;
      while ((at = text.find(". ")) != std::string::npos) {
        b.info(text.substr(0, at + 1), style);
        text = text.substr(at + 2);
      }
      if (!trim(text).empty()) b.info(text, style);
    }
    int bw = (b.inner - S(16) - gap) / 2;
    auto *open = new FlyButton(b.pad + S(8), b.y + S(4), bw, S(28), _("Open"), "Open " + label, [this, name] {
      close();
      PanelControls::get().drive_open(name);
    });
    auto *eject = new FlyButton(b.pad + S(8) + bw + gap, b.y + S(4), bw, S(28),
                                d.kind == "hdd" || d.kind == "ssd" ? _("Safely remove") : _("Eject"),
                                "Eject " + label, [name] { PanelControls::get().drive_eject(name, true); },
                                true);
    if (working) {
      open->deactivate();
      eject->deactivate();
    }
    b.y += S(36);
    b.end_card();
  }
  // Drives removed safely stay listed until the flyout closes.
  for (const auto &t : c.drive_tasks) {
    if (t.second.state != "removed") continue;
    bool present = std::any_of(c.drives.begin(), c.drives.end(),
                               [&](const RemovableDrive &d) { return d.name == t.first; });
    if (present) continue;
    b.start_card();
    b.info(t.second.text, 0);
    b.end_card();
  }
}

// --------------------------------------------------------------- touchpad

void StatusFlyout::touchpad_section(FlyBuilder &b) {
  auto &tp = TouchpadMonitor::get();
  if (!tp.present()) {
    b.info(_("No touchpad was found."));
    b.row(_("Touchpad settings..."), gear_icon(), b.launch({"sde-settings", "--page", "touchpad"}));
    return;
  }
  b.start_card();
  new TouchpadPreview(b.pad + S(8), b.y, b.inner - S(16), S(64));
  b.y += S(68);
  b.info(tp.enabled() ? _("Move a finger, tap or scroll to try it.") : _("The touchpad is off."));
  b.end_card();
  b.start_card();
  auto options = [] { return load_touchpad_options(Panel::instance->config()); };
  b.row(_("Touchpad"), touchpad_icon(), [] {
    auto &m = TouchpadMonitor::get();
    m.set_enabled(!m.enabled());
  }, [] { return std::string(TouchpadMonitor::get().enabled() ? _("On") : _("Off")); },
        [] { return TouchpadMonitor::get().enabled(); });
  b.row(_("Tap to click"), nullptr, [options] {
    auto o = options();
    o.tap = !o.tap;
    TouchpadMonitor::get().apply(o);
  }, nullptr, [options] { return options().tap; }, tp.enabled());
  b.row(_("Natural scrolling"), nullptr, [options] {
    auto o = options();
    o.natural = !o.natural;
    TouchpadMonitor::get().apply(o);
  }, nullptr, [options] { return options().natural; }, tp.enabled());
  new FlyLabel(b.pad + S(8), b.y, b.inner - S(16), S(22), [] { return std::string(_("Pointer speed")); }, 0);
  b.y += S(22);
  auto *speed = new Slider(b.pad + S(8), b.y, b.inner - S(16), S(28));
  speed->bounds(-100, 100);
  speed->step(5);
  speed->value(options().speed);
  speed->copy_tooltip(_("Pointer speed"));
  speed->when(FL_WHEN_RELEASE | FL_WHEN_CHANGED);
  speed->callback([](Fl_Widget *w, void *) {
    // Applied when the slider is released (or moved with keys/wheel).
    if (Fl::event() == FL_DRAG) return;
    auto o = load_touchpad_options(Panel::instance->config());
    o.speed = int(static_cast<Slider *>(w)->value());
    TouchpadMonitor::get().apply(o);
  });
  if (!tp.enabled()) speed->deactivate();
  b.y += S(30);
  b.end_card();
  b.rule();
  b.row(_("Touchpad settings..."), gear_icon(), b.launch({"sde-settings", "--page", "touchpad"}));
}
