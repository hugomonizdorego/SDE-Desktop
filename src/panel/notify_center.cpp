// SPDX-License-Identifier: GPL-3.0-or-later
#include "notify_center.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <ctime>
#include <map>
#include <memory>

#include "dbus.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "panel.h"
#include "popup_style.h"
#include "spawn.h"
#include "util.h"
#include "widgets.h"

using namespace sde;

namespace {
const char *kService = "org.freedesktop.Notifications";
const char *kPath = "/org/freedesktop/Notifications";
const char *kIface = "org.sde.Notifications";

void later(std::function<void()> fn) {
  auto *heap = new std::function<void()>(std::move(fn));
  Fl::add_timeout(0, [](void *d) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(d));
    (*f)();
  }, heap);
}

// A method call without a reply; the HistoryChanged signal follows.
void send_call(const char *member, const std::function<void(dbus::Writer &)> &args = nullptr) {
  dbus::Bus *bus = Panel::safe_mode ? nullptr : dbus::Bus::session();
  if (!bus) return;
  DBusMessage *m = dbus::method(kService, kPath, kIface, member);
  if (args) {
    dbus::Writer w(m);
    args(w);
  }
  dbus_message_set_no_reply(m, TRUE);
  bus->send(m);
}

const Theme &theme() { return Panel::instance->theme(); }

std::string time_label(uint32_t t) {
  time_t now = time(nullptr);
  long age = long(now) - long(t);
  if (age < 60) return _("Now");
  if (age < 3600) return format(_("%d min ago"), int(age / 60));
  struct tm a, b;
  time_t tt = time_t(t);
  localtime_r(&tt, &a);
  localtime_r(&now, &b);
  char buf[32];
  bool h24 = Panel::instance->config().get_bool("panel", "clock_24h", true);
  if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year)
    strftime(buf, sizeof buf, h24 ? "%H:%M" : "%I:%M %p", &a);
  else
    strftime(buf, sizeof buf, "%e %b", &a);
  return trim(buf);
}

void cross(Painter &p, double x, double y, double s, const Color &c) {
  double r = s * 0.3, cx = x + s / 2, cy = y + s / 2;
  p.line(cx - r, cy - r, cx + r, cy + r, c, Sd(1.4));
  p.line(cx - r, cy + r, cx + r, cy - r, c, Sd(1.4));
}

using IconFn = std::function<void(Painter &, double, double, double, const Color &)>;

// Small square button with a drawn icon; |tip| names it.
class IconButton : public Fl_Button {
 public:
  IconButton(int x, int y, int size, const std::string &tip, IconFn icon, std::function<void()> fn,
             std::function<bool()> on = nullptr)
      : Fl_Button(x, y, size, size), icon_(std::move(icon)), fn_(std::move(fn)), on_(std::move(on)) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    copy_tooltip(tip.c_str());
    callback([](Fl_Widget *w, void *) { later(static_cast<IconButton *>(w)->fn_); });
  }
  void draw() override {
    const Theme &t = theme();
    Painter p(window()->w(), window()->h());
    bool hot = Fl::belowmouse() == this, on = on_ && on_();
    if (on)
      p.fill_rounded(x(), y(), w(), h(), S(6), t.accent.alpha(0.22));
    else if (hot || value())
      p.fill_rounded(x(), y(), w(), h(), S(6), t.hover);
    double s = std::min<double>(std::min(w(), h()) * 0.62, S(popup_style::kIcon));
    icon_(p, x() + (w() - s) / 2, y() + (h() - s) / 2, s, on ? t.accent_text : t.text);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(6));
  }
  int handle(int event) override {
    if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS || event == FL_UNFOCUS) {
      redraw();
      return 1;
    }
    if (event == FL_KEYBOARD && Fl::focus() == this &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter || Fl::event_key() == ' ')) {
      do_callback();
      return 1;
    }
    return Fl_Button::handle(event);
  }

 private:
  IconFn icon_;
  std::function<void()> fn_;
  std::function<bool()> on_;
};

class TextButton : public FlatButton {
 public:
  TextButton(int x, int y, int w, int h, const std::string &text, const std::string &name,
             std::function<void()> fn)
      : FlatButton(x, y, w, h, text), fn_(std::move(fn)) {
    copy_tooltip(name.c_str());
    callback([](Fl_Widget *w, void *) { later(static_cast<TextButton *>(w)->fn_); });
  }

 private:
  std::function<void()> fn_;
};

// Plain text; the label (not drawn) names it for the UI tests.
class Caption : public Fl_Widget {
 public:
  Caption(int x, int y, int w, int h, const std::string &text, int style)
      : Fl_Widget(x, y, w, h), text_(text), style_(style) {
    copy_label(text.c_str());
    labeltype(FL_NO_LABEL);
    clear_visible_focus();
  }
  void draw() override {
    const Theme &t = theme();
    text_font(style_ == 2 ? std::max(S(9), t.font_size - 1) : t.font_size, style_ != 2);
    draw_text(text_, x(), y(), w(), h(), style_ == 2 ? t.text_dim : t.text, style_ == 3 ? 1 : 0);
  }

 private:
  std::string text_;
  int style_;  // 1 title, 2 dim caption, 3 centered
};

// The bell of the empty list.
class EmptyBell : public Fl_Widget {
 public:
  EmptyBell(int x, int y, int w, int h, bool dnd) : Fl_Widget(x, y, w, h), dnd_(dnd) {
    clear_visible_focus();
  }
  void draw() override {
    const Theme &t = theme();
    Painter p(window()->w(), window()->h());
    double s = std::min(w(), h());
    glyph::bell(p, x() + (w() - s) / 2, y(), s, dnd_, t.text_dim);
  }

 private:
  bool dnd_;
};

// One notification: icon, summary, time, body (up to three lines), its
// actions and a dismiss button. Clicking it runs the default action.
class Card : public Fl_Group {
 public:
  static constexpr int kLines = 3;
  Card(int x, int y, int w, const NotifyEntry &e) : Fl_Group(x, y, w, S(40)), e_(e) {
    box(FL_NO_BOX);
    resizable(nullptr);
    copy_label(("Notification " + (e.summary.empty() ? e.app : e.summary)).c_str());
    labeltype(FL_NO_LABEL);
    const Theme &t = theme();
    int pad = S(8), icon = S(24);
    text_x_ = x + pad + icon + S(10);
    text_w_ = w - (text_x_ - x) - pad - S(26);
    text_font(t.font_size);
    int bh = 0;
    if (!e.body.empty()) {
      int bw = text_w_;
      fl_measure(e.body.c_str(), bw, bh, 0);
      bh = std::min(bh, fl_height() * kLines);
    }
    body_h_ = bh;
    int yy = y + pad + S(20) + (bh ? bh + S(2) : 0);
    yy = std::max(yy, y + pad + icon);
    uint32_t id = e.id;
    begin();
    new IconButton(x + w - pad - S(22), y + S(6), S(22),
                   std::string(_("Dismiss")) + " " + (e.summary.empty() ? e.app : e.summary),
                   cross, [id] { NotifyCenter::get().dismiss(id); });
    std::vector<std::pair<std::string, std::string>> buttons;
    for (const auto &a : e.actions)
      if (a.first != "default" && !a.second.empty()) buttons.push_back(a);
    if (!buttons.empty()) {
      yy += S(6);
      int n = std::min<int>(3, int(buttons.size()));
      int gap = S(6), avail = w - (text_x_ - x) - pad, bw = (avail - gap * (n - 1)) / n;
      for (int i = 0; i < n; ++i) {
        std::string key = buttons[size_t(i)].first;
        new TextButton(text_x_ + i * (bw + gap), yy, bw, S(26), buttons[size_t(i)].second,
                       buttons[size_t(i)].second,
                       [id, key] { NotifyCenter::get().invoke(id, key); });
      }
      yy += S(26);
    }
    end();
    size(w, yy + pad - y);
  }
  void draw() override {
    const Theme &t = theme();
    int pad = S(8), icon = S(24);
    {
      Painter p(window()->w(), window()->h());
      bool hot = Fl::belowmouse() == this || (Fl::belowmouse() && Fl::belowmouse()->parent() == this);
      p.fill_rounded(x(), y(), w(), h(), S(8), hot ? t.hover : t.surface_alt);
      if (e_.urgency >= 2) p.fill_rounded(x() + S(2), y() + S(8), S(3), h() - S(16), Sd(1.5), t.error);
      cairo_surface_t *img = IconLoader::get(
          {e_.icon, lower(e_.app), "dialog-information", "preferences-desktop-notification"}, icon);
      p.image(img, x() + pad, y() + pad, icon);
      // Not seen yet: a dot on the icon's corner.
      if (e_.unread) {
        p.circle(x() + pad + icon - Sd(1), y() + pad + Sd(1), Sd(4.5), hot ? t.hover : t.surface_alt);
        p.circle(x() + pad + icon - Sd(1), y() + pad + Sd(1), Sd(3), t.accent);
      }
    }
    std::string stamp = time_label(e_.time);
    text_font(std::max(S(9), t.font_size - 1));
    int tw = text_width(stamp);
    text_font(t.font_size, true);
    draw_text(e_.summary.empty() ? e_.app : e_.summary, text_x_, y() + pad - S(1),
              text_w_ - tw - S(12), S(20), t.text);
    text_font(std::max(S(9), t.font_size - 1));
    draw_text(stamp, text_x_ + text_w_ - tw, y() + pad - S(1), tw, S(20), t.text_dim);
    if (!e_.body.empty()) {
      text_font(t.font_size);
      fl_color(t.text_dim.fl());
      fl_push_clip(text_x_, y() + pad + S(20), text_w_, body_h_);
      fl_draw(e_.body.c_str(), text_x_, y() + pad + S(20), text_w_, body_h_,
              Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
      fl_pop_clip();
    }
    draw_children();
  }
  int handle(int event) override {
    if (Fl_Group::handle(event)) return 1;
    switch (event) {
      case FL_ENTER:
      case FL_LEAVE:
        redraw();
        return 1;
      case FL_PUSH:
        return Fl::event_button() == FL_LEFT_MOUSE ? 1 : 0;
      case FL_RELEASE:
        if (Fl::event_inside(this) && e_.has_default()) {
          uint32_t id = e_.id;
          later([id] { NotifyCenter::get().invoke(id, "default"); });
        }
        return 1;
      default:
        return 0;
    }
  }

 private:
  NotifyEntry e_;
  int text_x_ = 0, text_w_ = 0, body_h_ = 0;
};
}  // namespace

bool NotifyEntry::has_default() const {
  for (const auto &a : actions)
    if (a.first == "default") return true;
  return false;
}

// ------------------------------------------------------------------ model

NotifyCenter &NotifyCenter::get() {
  static NotifyCenter c;
  return c;
}

void NotifyCenter::start(std::function<void()> changed) {
  changed_ = std::move(changed);
  if (started_ || Panel::safe_mode) return;
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  started_ = true;
  bus->add_signal_handler(
      std::string("type='signal',interface='") + kIface + "',member='HistoryChanged'",
      [](DBusMessage *m) { return bool(dbus_message_is_signal(m, kIface, "HistoryChanged")); },
      [this](DBusMessage *) { refresh(); });
  bus->add_signal_handler(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
      "member='NameOwnerChanged',arg0='org.freedesktop.Notifications'",
      [](DBusMessage *m) {
        return bool(dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged"));
      },
      [this](DBusMessage *) { refresh(); });
  refresh();
}

int NotifyCenter::unread() const {
  int n = 0;
  for (const auto &e : entries_) n += e.unread ? 1 : 0;
  return n;
}

void NotifyCenter::changed() {
  if (changed_) changed_();
}

void NotifyCenter::refresh() {
  dbus::Bus *bus = started_ ? dbus::Bus::session() : nullptr;
  if (!bus) return;
  if (pending_) {
    again_ = true;
    return;
  }
  pending_ = true;
  DBusMessage *m = dbus::method(kService, kPath, kIface, "GetHistory");
  bus->call_async(m, [this](DBusMessage *reply) {
    pending_ = false;
    std::vector<NotifyEntry> list;
    if (reply) {
      auto args = dbus::read_args(reply);
      if (!args.empty())
        for (const auto &v : args[0].items) {
          const auto &f = v.inner().items;
          if (f.size() < 9) continue;
          NotifyEntry e;
          e.id = uint32_t(f[0].num());
          e.app = f[1].str();
          e.icon = f[2].str();
          e.summary = f[3].str();
          e.body = f[4].str();
          for (size_t i = 0; i + 1 < f[5].items.size(); i += 2)
            e.actions.push_back({f[5].items[i].str(), f[5].items[i + 1].str()});
          e.time = uint32_t(f[6].num());
          e.urgency = uint32_t(f[7].num());
          e.unread = f[8].boolean();
          list.push_back(std::move(e));
        }
    }
    std::reverse(list.begin(), list.end());  // newest first
    available_ = reply != nullptr;
    entries_ = std::move(list);
    changed();
    if (again_) {
      again_ = false;
      refresh();
    }
  }, 2000);
}

void NotifyCenter::mark_read() {
  bool any = false;
  for (auto &e : entries_) {
    any = any || e.unread;
    e.unread = false;
  }
  send_call("MarkRead");
  if (any) changed();
}

void NotifyCenter::dismiss(uint32_t id) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const NotifyEntry &e) { return e.id == id; }),
                 entries_.end());
  send_call("Dismiss", [id](dbus::Writer &w) { w.u32(id); });
  changed();
}

void NotifyCenter::dismiss_app(const std::string &app) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const NotifyEntry &e) { return e.app == app; }),
                 entries_.end());
  send_call("DismissApp", [app](dbus::Writer &w) { w.str(app); });
  changed();
}

void NotifyCenter::clear() {
  entries_.clear();
  send_call("Clear");
  changed();
}

void NotifyCenter::invoke(uint32_t id, const std::string &key) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const NotifyEntry &e) { return e.id == id; }),
                 entries_.end());
  send_call("InvokeAction", [id, key](dbus::Writer &w) { w.u32(id).str(key); });
  changed();
  if (Panel::instance) Panel::instance->close_notifications();
}

// ----------------------------------------------------------------- flyout

NotifyFlyout::NotifyFlyout() : PanelPopup("Notifications") { end(); }

void NotifyFlyout::prepare() {
  NotifyCenter::get().refresh();
  rebuild();
}

void NotifyFlyout::closed() {
  Fl::remove_timeout(rebuild_timeout, this);
  rebuild_pending_ = false;
  // Seen: the badge clears.
  NotifyCenter::get().mark_read();
}

std::string NotifyFlyout::structure() const {
  std::string sig = Panel::instance->config().get("notifications", "do_not_disturb");
  for (const auto &e : NotifyCenter::get().entries())
    sig += "|" + std::to_string(e.id) + e.summary + e.body + std::to_string(e.actions.size());
  return sig;
}

void NotifyFlyout::status_changed() {
  if (!shown()) return;
  if (structure() == structure_) {
    redraw();
    return;
  }
  if (rebuild_pending_) return;
  rebuild_pending_ = true;
  Fl::add_timeout(0.05, rebuild_timeout, this);
}

void NotifyFlyout::rebuild_timeout(void *data) {
  auto *self = static_cast<NotifyFlyout *>(data);
  self->rebuild_pending_ = false;
  if (!self->shown()) return;
  int scroll = self->body_ ? self->body_->yposition() : 0;
  self->rebuild();
  if (self->body_)
    self->body_->scroll_to(0, std::max(0, std::min(scroll, self->content_height_ - self->body_->h())));
  self->reposition();
  self->redraw();
}

void NotifyFlyout::rebuild() {
  structure_ = structure();
  clear();
  resizable(nullptr);
  auto *panel = Panel::instance;
  const auto &t = panel->theme();
  const auto &center = NotifyCenter::get();
  bool dnd = panel->config().get_bool("notifications", "do_not_disturb", false);
  int mx, my, mw, mh;
  panel->monitor(mx, my, mw, mh);
  int W = std::min(S(360), mw - S(16));
  int pad = S(popup_style::kPad), scrollbar = S(6), inner = W - 2 * pad - scrollbar;
  int available = std::max(S(160), (t.panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my) - S(16));
  available = std::min(available, S(popup_style::kCenterMaxH));
  header_ = S(popup_style::kHeader);
  int footer = S(40);
  begin();
  body_ = new Fl_Scroll(S(2), header_, W - S(4), std::max(S(40), available - header_ - footer));
  body_->type(Fl_Scroll::VERTICAL);
  body_->resizable(nullptr);
  body_->scrollbar_size(scrollbar);
  body_->box(FL_FLAT_BOX);
  body_->color(t.surface.fl());
  new Fl_Box(body_->x(), body_->y(), 1, 1);
  int y = header_ + S(8);
  const auto &entries = center.entries();
  if (entries.empty()) {
    new EmptyBell(pad, y + S(10), inner, S(32), dnd);
    y += S(50);
    new Caption(pad, y, inner, S(22), _("No new notifications"), 3);
    y += S(22);
    if (dnd) {
      new Caption(pad, y, inner, S(20), _("Do Not Disturb is on: notifications are silenced."), 2);
      y += S(20);
    }
    y += S(12);
  } else {
    // Applications in the order of their newest notification.
    std::vector<std::string> apps;
    for (const auto &e : entries)
      if (std::find(apps.begin(), apps.end(), e.app) == apps.end()) apps.push_back(e.app);
    for (const auto &app : apps) {
      std::string name = app.empty() ? std::string(_("Notifications")) : app;
      new Caption(pad + S(4), y, inner - S(40), S(24), name, 2);
      new IconButton(pad + inner - S(24), y + S(1), S(22),
                     std::string(_("Clear")) + " " + name, cross,
                     [app] { NotifyCenter::get().dismiss_app(app); });
      y += S(26);
      for (const auto &e : entries) {
        if (e.app != app) continue;
        auto *card = new Card(pad, y, inner, e);
        y += card->h() + S(6);
      }
      y += S(4);
    }
  }
  content_height_ = y - header_ + S(4);
  new Fl_Box(pad, y, inner, S(4));
  body_->end();
  int H = std::min(available, header_ + content_height_ + footer);
  body_->size(W - S(4), H - header_ - footer);
  begin();
  new Caption(pad + S(8), S(4), W - 2 * pad - S(130), header_ - S(4), _("Notifications"), 1);
  new IconButton(W - pad - S(30), (header_ - S(30)) / 2, S(30), _("Do Not Disturb"),
                 [](Painter &p, double x, double y, double s, const Color &c) {
                   glyph::bell(p, x, y, s, true, c);
                 },
                 [] {
                   auto *pn = Panel::instance;
                   pn->set_do_not_disturb(!pn->config().get_bool("notifications", "do_not_disturb", false));
                 },
                 [] { return Panel::instance->config().get_bool("notifications", "do_not_disturb", false); });
  auto *clear_all = new TextButton(W - pad - S(30) - S(92), (header_ - S(28)) / 2, S(86), S(28),
                                   _("Clear all"), _("Clear all"), [] { NotifyCenter::get().clear(); });
  if (entries.empty()) clear_all->deactivate();
  new TextButton(pad, H - footer + S(6), inner + scrollbar, S(28), _("Notification settings..."),
                 _("Notification settings..."), [this] {
                   close();
                   spawn_detached({"sde-settings", "--page", "notifications"});
                 });
  end();
  resizable(nullptr);
  init_sizes();
  size(W, H);
}

void NotifyFlyout::draw() {
  const auto &t = Panel::instance->theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    if (t.glass) p.fill_rect(0, 0, w(), header_, t.surface_alt.mix(t.surface, 0.55));
    stroke_outline(p, t.border);
    p.line(S(popup_style::kPad), header_ - 0.5, w() - S(popup_style::kPad), header_ - 0.5, t.border);
    p.line(S(popup_style::kPad), h() - S(40) + 0.5, w() - S(popup_style::kPad), h() - S(40) + 0.5, t.border);
  }
  draw_children();
  if (qa_dump_enabled() && body_) {
    const auto &c = NotifyCenter::get();
    qa_dump(this, "@body\t" + std::to_string(body_->x()) + "\t" + std::to_string(body_->y()) + "\t" +
                      std::to_string(body_->w()) + "\t" + std::to_string(body_->h()) + "\t1\n" +
                      "@count:" + std::to_string(c.total()) + "\t0\t0\t0\t0\t1\n");
  }
}

int NotifyFlyout::handle(int event) {
  if (event == FL_MOUSEWHEEL && body_ && Fl::event_inside(body_)) {
    int limit = std::max(0, content_height_ - body_->h());
    body_->scroll_to(0, std::max(0, std::min(limit, body_->yposition() + Fl::event_dy() * S(36))));
    redraw();
    return 1;
  }
  return PanelPopup::handle(event);
}
