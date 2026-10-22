// SPDX-License-Identifier: GPL-3.0-or-later
// sde-notifyd: the SDE notification service.
//
// Implements org.freedesktop.Notifications (Desktop Notifications
// Specification 1.2): popups with icon, summary, body, action buttons and
// images, urgency levels and a Do Not Disturb mode that only lets critical
// notifications through.
//
// alpha52: a history of the notifications (org.sde.Notifications on the same
// object) for the panel's notifications button and flyout: GetHistory,
// MarkRead, Dismiss, DismissApp, Clear, InvokeAction and the HistoryChanged
// signal (unread, total). Expired and silenced (Do Not Disturb) notifications
// stay in the history until dismissed there; transient ones are not kept.
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <X11/extensions/shape.h>
#include <cmath>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "crash.h"
#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "dbus.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "theme.h"
#include "util.h"
#include "x11.h"

using namespace sde;

namespace {

const char *kPath = "/org/freedesktop/Notifications";
const char *kIface = "org.freedesktop.Notifications";
const char *kHistoryIface = "org.sde.Notifications";

enum CloseReason : uint32_t { EXPIRED = 1, DISMISSED = 2, CLOSED_BY_CALL = 3 };

Config g_cfg;
Theme g_theme;
dbus::Bus *g_bus = nullptr;

// Removes the small HTML subset allowed by the specification (<b>, <i>,
// <u>, <a>, <img>) and decodes the basic XML entities.
std::string strip_markup(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '<') {
      size_t end = in.find('>', i);
      if (end == std::string::npos) {
        out += in.substr(i);
        break;
      }
      std::string tag = lower(in.substr(i + 1, end - i - 1));
      if (tag == "br" || tag == "br/" || tag == "br /") out += '\n';
      i = end;
      continue;
    }
    if (c == '&') {
      static const std::pair<const char *, const char *> entities[] = {
          {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"}};
      bool done = false;
      for (auto &e : entities) {
        size_t n = strlen(e.first);
        if (in.compare(i, n, e.first) == 0) {
          out += e.second;
          i += n - 1;
          done = true;
          break;
        }
      }
      if (done) continue;
    }
    out += c;
  }
  return trim(out);
}

void emit_closed(uint32_t id, uint32_t reason) {
  DBusMessage *s = dbus::signal(kPath, kIface, "NotificationClosed");
  dbus::Writer(s).u32(id).u32(reason);
  g_bus->send(s);
}

void emit_action(uint32_t id, const std::string &key) {
  DBusMessage *s = dbus::signal(kPath, kIface, "ActionInvoked");
  dbus::Writer(s).u32(id).str(key);
  g_bus->send(s);
}

struct Notification;
void close_notification(uint32_t id, uint32_t reason);
void relayout();

// Flat button in the notification card.
class ActionButton : public Fl_Button {
 public:
  ActionButton(int x, int y, int w, int h, const std::string &label)
      : Fl_Button(x, y, w, h), text_(label) {
    box(FL_NO_BOX);
    clear_visible_focus();
  }
  void draw() override {
    {
      Painter p(window()->w(), window()->h());
      bool hot = Fl::belowmouse() == this;
      p.fill_rounded(x(), y(), w(), h(), S(8), hot ? g_theme.hover : g_theme.surface_alt);
    }
    text_font(g_theme.font_size, true);
    draw_text(text_, x() + S(6), y(), w() - S(12), h(), g_theme.accent_text, 1);
  }
  int handle(int e) override {
    if (e == FL_ENTER || e == FL_LEAVE) {
      redraw();
      return 1;
    }
    return Fl_Button::handle(e);
  }

 private:
  std::string text_;
};

class Popup : public BufferedWindow {
 public:
  Popup(Notification *n);
  void draw() override;
  int handle(int e) override;
  int wanted_height() const { return height_; }
  void rebuild();

 private:
  Notification *n_;
  int height_ = 0;
  int body_h_ = 0;
  bool hover_close_ = false;
  std::vector<std::unique_ptr<std::pair<uint32_t, std::string>>> keys_;  // button user data
};

struct Notification {
  uint32_t id = 0;
  std::string app, icon, summary, body, sender;
  std::vector<std::pair<std::string, std::string>> actions;  // key, label
  int urgency = 1;  // 0 low, 1 normal, 2 critical
  int timeout_ms = -1;
  bool resident = false;
  cairo_surface_t *image = nullptr;
  std::unique_ptr<Popup> popup;
  bool hovered = false;

  ~Notification() {
    if (image) cairo_surface_destroy(image);
  }
};

std::vector<std::unique_ptr<Notification>> g_notes;
uint32_t g_next_id = 1;

// The notification history (oldest first), shown by the panel.
struct HistoryEntry {
  uint32_t id = 0;
  std::string app, icon, summary, body;
  std::vector<std::pair<std::string, std::string>> actions;
  uint32_t time = 0;
  uint32_t urgency = 1;
  bool unread = true;
};
std::vector<HistoryEntry> g_history;
constexpr size_t kHistoryMax = 100;

void emit_history_changed() {
  uint32_t unread = 0;
  for (const auto &h : g_history) unread += h.unread ? 1 : 0;
  DBusMessage *s = dbus::signal(kPath, kHistoryIface, "HistoryChanged");
  dbus::Writer(s).u32(unread).u32(uint32_t(g_history.size()));
  g_bus->send(s);
}

bool history_remove(uint32_t id) {
  auto it = std::find_if(g_history.begin(), g_history.end(), [&](auto &h) { return h.id == id; });
  if (it == g_history.end()) return false;
  g_history.erase(it);
  return true;
}

Notification *find_note(uint32_t id) {
  for (auto &n : g_notes)
    if (n->id == id) return n.get();
  return nullptr;
}

void expire_cb(void *data) {
  uint32_t id = uint32_t(reinterpret_cast<uintptr_t>(data));
  Notification *n = find_note(id);
  if (!n) return;
  if (n->hovered) {
    // Keep it while the pointer is over it.
    Fl::repeat_timeout(1.0, expire_cb, data);
    return;
  }
  close_notification(id, EXPIRED);
}

void schedule_expiry(Notification *n) {
  void *key = reinterpret_cast<void *>(uintptr_t(n->id));
  Fl::remove_timeout(expire_cb, key);
  int ms = n->timeout_ms;
  if (n->urgency == 2 && ms <= 0) return;  // critical: until dismissed
  if (ms < 0) ms = g_cfg.get_int("notifications", "timeout", 6) * 1000;
  if (ms == 0) return;  // never expires
  Fl::add_timeout(ms / 1000.0, expire_cb, key);
}

Popup::Popup(Notification *n) : BufferedWindow(0, 0, 10, 10, "Notification"), n_(n) {
  end();
  set_override();
  border(0);
  rebuild();
}

// alpha51 compact popups: 340 px wide, 12 px padding, 32 px icon and soft
// rounded corners, like the panel's flyouts.
constexpr int kPopupW = 340, kPad = 12, kIcon = 32, kRadius = 10;

void round_corners(Window xid, int w, int h) {
  int event, error, r = std::min({S(kRadius), w / 4, h / 4});
  if (!xid || r <= 0 || !XShapeQueryExtension(fl_display, &event, &error)) return;
  std::vector<XRectangle> rows;
  for (int row = 0; row < h; ++row) {
    double dy = row < r ? r - row - 0.5 : row >= h - r ? row - (h - r) + 0.5 : 0;
    int inset = dy > 0 ? int(std::ceil(r - std::sqrt(std::max(0.0, r * r - dy * dy)))) : 0;
    rows.push_back({short(inset), short(row), static_cast<unsigned short>(std::max(1, w - 2 * inset)), 1});
  }
  XShapeCombineRectangles(fl_display, xid, ShapeBounding, 0, 0, rows.data(), int(rows.size()),
                          ShapeSet, YXBanded);
}

void Popup::rebuild() {
  clear();  // note: also resets resizable()
  resizable(nullptr);  // keep child positions when the card changes height
  keys_.clear();
  begin();
  int W = S(kPopupW), pad = S(kPad), icon = S(kIcon);
  int text_x = pad + icon + S(12), text_w = W - text_x - pad - S(20);
  text_font(g_theme.font_size);
  int bw = text_w, bh = 0;
  if (!n_->body.empty()) {
    fl_measure(n_->body.c_str(), bw, bh, 0);
    bh = std::min(bh, fl_height() * 5);
  }
  body_h_ = bh;
  int y = pad + S(22) + (bh ? bh + S(6) : 0);
  y = std::max(y, pad + icon);
  std::vector<std::pair<std::string, std::string>> buttons;
  for (auto &a : n_->actions)
    if (a.first != "default") buttons.push_back(a);
  if (!buttons.empty()) {
    y += S(10);
    int n = std::min<int>(3, int(buttons.size()));
    int gap = S(8), bw2 = (W - 2 * pad - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
      auto *b = new ActionButton(pad + i * (bw2 + gap), y, bw2, S(28), buttons[size_t(i)].second);
      std::string key = buttons[size_t(i)].first;
      uint32_t id = n_->id;
      b->callback(
          [](Fl_Widget *, void *data) {
            auto *pair = static_cast<std::pair<uint32_t, std::string> *>(data);
            uint32_t nid = pair->first;
            std::string k = pair->second;
            emit_action(nid, k);
            Notification *note = find_note(nid);
            if (note && !note->resident) {
              if (history_remove(nid)) emit_history_changed();
              close_notification(nid, DISMISSED);
            }
          },
          keys_.emplace_back(new std::pair<uint32_t, std::string>(id, key)).get());
    }
    y += S(28);
  }
  height_ = y + pad;
  end();
  size(W, height_);
  redraw();
}

void Popup::draw() {
  const Theme &t = g_theme;
  int W = w(), H = h(), pad = S(kPad), icon = S(kIcon);
  int text_x = pad + icon + S(12), text_w = W - text_x - pad - S(20);
  {
    Painter p(W, H);
    // The window is shaped by round_corners(); theme colors (alpha51).
    p.fill_rect(0, 0, W, H, t.menu_bg);
    p.stroke_rounded(0.5, 0.5, W - 1, H - 1, std::min({S(kRadius), W / 4, H / 4}), t.border);
    if (n_->urgency == 2) p.fill_rounded(S(4), S(10), S(4), H - S(20), S(2), t.error);
    cairo_surface_t *img = n_->image;
    if (!img) img = IconLoader::get({n_->icon, "dialog-information", "preferences-desktop-notification"}, icon);
    p.image(img, pad, pad, icon);
    // Close button.
    Color cc = hover_close_ ? t.text : t.text_dim;
    int cx = W - pad - S(8), cy = pad + S(8), r = S(5);
    if (hover_close_) p.circle(cx, cy, S(11), t.hover);
    p.line(cx - r, cy - r, cx + r, cy + r, cc, Sd(1.6));
    p.line(cx - r, cy + r, cx + r, cy - r, cc, Sd(1.6));
  }
  text_font(t.font_size + 1, true);
  std::string title = n_->summary.empty() ? n_->app : n_->summary;
  draw_text(title, text_x, pad - S(2), text_w, S(22), t.text);
  if (!n_->body.empty()) {
    text_font(t.font_size);
    fl_color(t.text_dim.fl());
    fl_push_clip(text_x, pad + S(22), text_w, body_h_);
    fl_draw(n_->body.c_str(), text_x, pad + S(22), text_w, body_h_,
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
    fl_pop_clip();
  }
  draw_children();
}

int Popup::handle(int e) {
  int pad = S(kPad);
  bool on_close = Fl::event_x() >= w() - pad - S(22) && Fl::event_y() < pad + S(24);
  switch (e) {
    case FL_ENTER:
      n_->hovered = true;
      [[fallthrough]];
    case FL_MOVE:
      if (on_close != hover_close_) {
        hover_close_ = on_close;
        redraw();
      }
      BufferedWindow::handle(e);
      return 1;
    case FL_LEAVE:
      n_->hovered = false;
      hover_close_ = false;
      redraw();
      BufferedWindow::handle(e);
      return 1;
    case FL_PUSH:
      if (BufferedWindow::handle(e)) return 1;
      return 1;
    case FL_RELEASE: {
      if (BufferedWindow::handle(e)) return 1;
      uint32_t id = n_->id;
      if (on_close || Fl::event_button() != FL_LEFT_MOUSE) {
        close_notification(id, DISMISSED);
        return 1;
      }
      bool has_default = false;
      for (auto &a : n_->actions)
        if (a.first == "default") has_default = true;
      if (has_default) {
        emit_action(id, "default");
        // Opened: it leaves the history too.
        if (history_remove(id)) emit_history_changed();
      }
      close_notification(id, DISMISSED);
      return 1;
    }
    default:
      return BufferedWindow::handle(e);
  }
}

// Stacks visible popups in the corner next to the panel.
void relayout() {
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, 0);
  bool top = g_theme.panel_top;
  int margin = S(12), gap = S(10);
  int y = top ? sy + g_theme.panel_height + margin : sy + sh - g_theme.panel_height - margin;
  int shown = 0;
  // Newest first, closest to the panel.
  for (auto it = g_notes.rbegin(); it != g_notes.rend(); ++it) {
    Notification *n = it->get();
    if (!n->popup) continue;
    Popup *p = n->popup.get();
    int h = p->wanted_height();
    bool fits = top ? y + h < sy + sh - margin : y - h > sy + margin;
    if (shown >= 4 || !fits) {
      p->hide();
      continue;
    }
    int px = sx + sw - p->w() - margin;
    int py = top ? y : y - h;
    p->resize(px, py, p->w(), h);
    if (!p->shown()) p->show();
    round_corners(fl_xid(p), p->w(), h);
    y = top ? y + h + gap : y - h - gap;
    ++shown;
  }
}

void close_notification(uint32_t id, uint32_t reason) {
  Fl::remove_timeout(expire_cb, reinterpret_cast<void *>(uintptr_t(id)));
  auto it = std::find_if(g_notes.begin(), g_notes.end(), [&](auto &n) { return n->id == id; });
  if (it == g_notes.end()) return;
  if ((*it)->popup) (*it)->popup->hide();
  // Delete later: we may be inside one of the popup's callbacks.
  Notification *raw = it->release();
  g_notes.erase(it);
  Fl::add_timeout(0, [](void *d) { delete static_cast<Notification *>(d); }, raw);
  emit_closed(id, reason);
  relayout();
}

cairo_surface_t *image_from_hint(const dbus::Value &v, int size) {
  const dbus::Value &s = v.inner();
  if (s.type != DBUS_TYPE_STRUCT || s.items.size() < 7) return nullptr;
  int w = int(s.items[0].num()), h = int(s.items[1].num()), stride = int(s.items[2].num());
  bool alpha = s.items[3].boolean();
  int channels = int(s.items[5].num());
  auto bytes = s.items[6].bytes();
  return rgba_pixels(bytes.data(), bytes.size(), w, h, stride, alpha, channels, size);
}

std::string icon_from_path(std::string icon) {
  if (starts_with(icon, "file://")) icon = icon.substr(7);
  return icon;
}

DBusHandlerResult on_notify(DBusMessage *msg) {
  auto args = dbus::read_args(msg);
  if (args.size() < 8) {
    g_bus->send(dbus::Bus::error(msg, DBUS_ERROR_INVALID_ARGS, "Notify expects (susssasa{sv}i)"));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  uint32_t replaces = uint32_t(args[1].num());
  Notification *n = replaces ? find_note(replaces) : nullptr;
  std::unique_ptr<Notification> fresh;
  // A replacement of a notification whose bubble is gone takes its place
  // in the history (progress updates, a chat's latest message).
  if (!n && replaces) history_remove(replaces);
  if (!n) {
    fresh = std::make_unique<Notification>();
    fresh->id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;
    n = fresh.get();
  }
  n->app = args[0].str();
  n->icon = icon_from_path(args[2].str());
  n->summary = strip_markup(args[3].str());
  n->body = strip_markup(args[4].str());
  n->sender = dbus_message_get_sender(msg) ? dbus_message_get_sender(msg) : "";
  n->actions.clear();
  const auto &acts = args[5].items;
  for (size_t i = 0; i + 1 < acts.size(); i += 2) n->actions.push_back({acts[i].s, acts[i + 1].s});
  const dbus::Value &hints = args[6];
  n->urgency = 1;
  if (const dbus::Value *u = hints.get("urgency")) n->urgency = int(u->num());
  n->resident = false;
  if (const dbus::Value *r = hints.get("resident")) n->resident = r->boolean();
  if (n->image) {
    cairo_surface_destroy(n->image);
    n->image = nullptr;
  }
  for (const char *key : {"image-data", "image_data", "icon_data"}) {
    if (const dbus::Value *img = hints.get(key)) {
      n->image = image_from_hint(*img, S(kIcon));
      if (n->image) break;
    }
  }
  if (!n->image) {
    for (const char *key : {"image-path", "image_path"}) {
      if (const dbus::Value *p = hints.get(key)) {
        n->icon = icon_from_path(p->str());
        break;
      }
    }
  }
  if (n->icon.empty()) {
    if (const dbus::Value *e = hints.get("desktop-entry")) {
      DesktopEntry entry;
      if (find_application(e->str() + ".desktop", entry)) n->icon = entry.icon;
    }
  }
  n->timeout_ms = int(args[7].num());
  uint32_t id = n->id;

  DBusMessage *reply = dbus_message_new_method_return(msg);
  dbus::Writer(reply).u32(id);
  g_bus->send(reply);

  bool transient = false;
  if (const dbus::Value *t = hints.get("transient")) transient = t->boolean();
  if (!transient) {
    auto it = std::find_if(g_history.begin(), g_history.end(), [&](auto &h) { return h.id == id; });
    if (it == g_history.end()) {
      g_history.emplace_back();
      it = g_history.end() - 1;
    }
    it->id = id;
    it->app = n->app;
    it->icon = n->icon;
    it->summary = n->summary;
    it->body = n->body;
    it->actions = n->actions;
    it->time = uint32_t(::time(nullptr));
    it->urgency = uint32_t(n->urgency);
    it->unread = true;
    while (g_history.size() > kHistoryMax) g_history.erase(g_history.begin());
    emit_history_changed();
  }

  bool dnd = g_cfg.get_bool("notifications", "do_not_disturb", false);
  if (fresh) g_notes.push_back(std::move(fresh));
  if (dnd && n->urgency < 2) {
    // Silenced: tell the sender it is gone so it does not wait for it.
    close_notification(id, EXPIRED);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (!n->popup) n->popup = std::make_unique<Popup>(n);
  else n->popup->rebuild();
  relayout();
  schedule_expiry(n);
  return DBUS_HANDLER_RESULT_HANDLED;
}

// org.sde.Notifications: the history for the panel.
DBusHandlerResult on_history(DBusMessage *msg, const std::string &m) {
  auto args = dbus::read_args(msg);
  DBusMessage *r = dbus_message_new_method_return(msg);
  bool changed = false;
  if (m == "GetHistory") {
    dbus::Writer w(r);
    {
      dbus::Writer list(w.iter(), DBUS_TYPE_ARRAY, "(ussssasuub)");
      for (const auto &h : g_history) {
        dbus::Writer st(list.iter(), DBUS_TYPE_STRUCT, nullptr);
        std::vector<std::string> acts;
        for (const auto &a : h.actions) {
          acts.push_back(a.first);
          acts.push_back(a.second);
        }
        st.u32(h.id).str(h.app).str(h.icon).str(h.summary).str(h.body).strings(acts).u32(h.time)
            .u32(h.urgency).boolean(h.unread);
      }
    }
  } else if (m == "MarkRead") {
    for (auto &h : g_history)
      if (h.unread) {
        h.unread = false;
        changed = true;
      }
  } else if (m == "Dismiss" && !args.empty()) {
    uint32_t id = uint32_t(args[0].num());
    changed = history_remove(id);
    close_notification(id, DISMISSED);
  } else if (m == "DismissApp" && !args.empty()) {
    std::string app = args[0].str();
    std::vector<uint32_t> ids;
    for (const auto &h : g_history)
      if (h.app == app) ids.push_back(h.id);
    for (uint32_t id : ids) {
      history_remove(id);
      close_notification(id, DISMISSED);
    }
    changed = !ids.empty();
  } else if (m == "Clear") {
    std::vector<uint32_t> ids;
    for (const auto &h : g_history) ids.push_back(h.id);
    g_history.clear();
    for (uint32_t id : ids) close_notification(id, DISMISSED);
    changed = !ids.empty();
  } else if (m == "InvokeAction" && args.size() >= 2) {
    uint32_t id = uint32_t(args[0].num());
    emit_action(id, args[1].str());
    changed = history_remove(id);
    close_notification(id, DISMISSED);
  } else {
    dbus_message_unref(r);
    g_bus->send(dbus::Bus::error(msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method " + m));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  g_bus->send(r);
  if (changed) emit_history_changed();
  return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult on_message(DBusMessage *msg) {
  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *member = dbus_message_get_member(msg);
  if (!member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  std::string m = member;
  if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
    static const char *xml =
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
        "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node><interface name=\"org.freedesktop.Notifications\">"
        "<method name=\"GetCapabilities\"><arg type=\"as\" direction=\"out\"/></method>"
        "<method name=\"Notify\"><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
        "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
        "<arg type=\"as\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/>"
        "<arg type=\"u\" direction=\"out\"/></method>"
        "<method name=\"CloseNotification\"><arg type=\"u\" direction=\"in\"/></method>"
        "<method name=\"GetServerInformation\"><arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
        "<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/></method>"
        "<signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
        "<signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal>"
        "</interface><interface name=\"org.sde.Notifications\">"
        "<method name=\"GetHistory\"><arg type=\"a(ussssasuub)\" direction=\"out\"/></method>"
        "<method name=\"MarkRead\"/>"
        "<method name=\"Dismiss\"><arg type=\"u\" direction=\"in\"/></method>"
        "<method name=\"DismissApp\"><arg type=\"s\" direction=\"in\"/></method>"
        "<method name=\"Clear\"/>"
        "<method name=\"InvokeAction\"><arg type=\"u\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
        "<signal name=\"HistoryChanged\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
        "</interface></node>";
    DBusMessage *r = dbus_message_new_method_return(msg);
    dbus::Writer(r).str(xml);
    g_bus->send(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_has_interface(msg, kHistoryIface)) return on_history(msg, m);
  if (!dbus_message_has_interface(msg, kIface)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  if (m == "Notify") return on_notify(msg);
  DBusMessage *r = dbus_message_new_method_return(msg);
  if (m == "GetCapabilities") {
    dbus::Writer(r).strings({"actions", "body", "body-markup", "icon-static", "persistence"});
  } else if (m == "GetServerInformation") {
    dbus::Writer(r).str("SDE Notifications").str("Selviana Desktop Environment").str(SDE_VERSION).str("1.2");
  } else if (m == "CloseNotification") {
    auto args = dbus::read_args(msg);
    if (!args.empty()) {
      // Withdrawn by the application: gone from the history as well.
      if (history_remove(uint32_t(args[0].num()))) emit_history_changed();
      close_notification(uint32_t(args[0].num()), CLOSED_BY_CALL);
    }
  } else {
    dbus_message_unref(r);
    g_bus->send(dbus::Bus::error(msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method " + m));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  g_bus->send(r);
  return DBUS_HANDLER_RESULT_HANDLED;
}

void load_theme() {
  g_cfg.load();
  g_theme = Theme::from_config(g_cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(g_cfg.get("appearance", "icon_theme", "auto"));
  IconLoader::clear_cache();
}

int x_event_handler(int) {
  if (!fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    load_theme();
    for (auto &n : g_notes)
      if (n->popup) n->popup->rebuild();
    relayout();
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  install_crash_handler("sde-notifyd");
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) {
      printf("sde-notifyd %s (%s) - DebION Dev-Team\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    }
    printf("%s", _("Usage: sde-notifyd [--version]\n\n"
                   "Shows desktop notifications (org.freedesktop.Notifications).\n"));
    return strcmp(argv[i], "--help") ? 2 : 0;
  }
  fl_open_display();
  load_theme();
  g_bus = dbus::Bus::session();
  if (!g_bus) return 1;
  if (!g_bus->request_name(kIface)) {
    fprintf(stderr, "sde-notifyd: %s\n",
            _("another notification service is already running; exiting"));
    return 0;
  }
  g_bus->export_object(kPath, on_message);

  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  Fl::add_handler(x_event_handler);
  // Run without any window shown.
  for (;;) Fl::wait(1e20);
}
