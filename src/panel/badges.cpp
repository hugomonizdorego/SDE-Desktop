// SPDX-License-Identifier: GPL-3.0-or-later
#include "badges.h"

#include <FL/Fl.H>
#include <cairo.h>

#include <algorithm>
#include <cmath>

#include "app_catalog.h"
#include "dbus.h"
#include "icons.h"
#include "notify_center.h"
#include "panel.h"
#include "util.h"

using namespace sde;

namespace badges {

namespace {
const char *kLauncherIface = "com.canonical.Unity.LauncherEntry";
// Senders and their entries are kept within these limits (any program on
// the session bus may send Update signals).
const size_t kMaxLaunchers = 256;
}  // namespace

Badges &Badges::get() {
  static Badges b;
  return b;
}

void Badges::start() {
  if (started_ || Panel::safe_mode) return;
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  started_ = true;
  bus->add_signal_handler(
      std::string("type='signal',interface='") + kLauncherIface + "',member='Update'",
      [](DBusMessage *m) { return bool(dbus_message_is_signal(m, kLauncherIface, "Update")); },
      [this](DBusMessage *m) { on_update(m); });
  // A sender that leaves the bus takes its badges with it.
  bus->add_signal_handler(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
      "member='NameOwnerChanged'",
      [](DBusMessage *m) { return bool(dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")); },
      [this](DBusMessage *m) { on_name_lost(m); });
  // As docks do: libunity applications send their counts while a launcher
  // owns this name (Unity, or another dock, may take it).
  bus->request_name("com.canonical.Unity", true);
}

void Badges::subscribe(void (*fn)(void *), void *data) { subscribers_.push_back({fn, data}); }

void Badges::unsubscribe(void *data) {
  subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
                                    [&](const std::pair<void (*)(void *), void *> &s) { return s.second == data; }),
                     subscribers_.end());
}

void Badges::notifications_changed() { schedule(); }

Badge Badges::of(const std::string &id) const {
  auto it = badges_.find(id);
  return it == badges_.end() ? Badge() : it->second;
}

Badge Badges::sum(const std::vector<std::string> &ids) const {
  Badge out;
  for (auto &id : ids) {
    Badge b = of(id);
    out.count += b.count;
    out.urgent = out.urgent || b.urgent;
  }
  return out;
}

void Badges::app_focused(const std::string &id) {
  if (id.empty()) return;
  std::vector<uint32_t> ids;
  for (auto &e : NotifyCenter::get().entries()) {
    if (!e.unread) continue;
    auto it = notes_.find(e.id);
    if (it != notes_.end() && it->second == id) ids.push_back(e.id);
  }
  if (!ids.empty()) NotifyCenter::get().mark_read_ids(ids);
}

void Badges::on_update(DBusMessage *m) {
  const char *sender = dbus_message_get_sender(m), *path = dbus_message_get_path(m);
  auto args = dbus::read_args(m);
  if (!sender || args.size() < 2 || !args[0].inner().is_string()) return;
  std::string uri = args[0].str(), key = std::string(sender) + "\n" + uri;
  auto it = launchers_.find(key);
  bool first = it == launchers_.end();
  if (first && launchers_.size() >= kMaxLaunchers) return;
  Launcher &l = launchers_[key];
  if (first) {
    l.sender = sender;
    l.uri = uri;
  }
  apply(l.state, args[1]);
  if (first) {
    resolve(l);
    // Its first Update may carry only what changed: the rest from Query.
    if (path) query(key, sender, path);
  }
  schedule();
}

void Badges::query(const std::string &key, const std::string &sender, const std::string &path) {
  dbus::Bus *bus = dbus::Bus::session();
  DBusMessage *q = dbus::method(sender, path, kLauncherIface, "Query");
  if (!bus || !q) return;
  bus->call_async(q, [this, key](DBusMessage *reply) {
    if (!reply) return;  // no Query (Telegram, Electron): the signals alone
    auto it = launchers_.find(key);
    if (it == launchers_.end()) return;
    // libunity answers (sa{sv}); an a{sv} alone is read as well.
    for (auto &v : dbus::read_args(reply)) {
      const dbus::Value &in = v.inner();
      if (in.type == DBUS_TYPE_STRUCT && in.items.size() >= 2) {
        apply(it->second.state, in.items[1]);
        break;
      }
      if (in.type == DBUS_TYPE_ARRAY) {
        apply(it->second.state, in);
        break;
      }
    }
    schedule();
  }, 2000);
}

void Badges::resolve(Launcher &l) {
  auto apps = app_catalog::ready() ? app_catalog::current() : nullptr;
  Source src;
  src.desktop_id = app_id_from_uri(l.uri);
  auto known = programs_.find(l.sender);
  if (known != programs_.end()) src.program = known->second;
  l.app = apps ? match_app(src, apps->apps) : src.desktop_id;
  if (!l.app.empty() || known != programs_.end() || !apps) return;
  // Not an installed desktop file (an Electron application that names
  // itself "electron"): the program of the sender's process.
  dbus::Bus *bus = dbus::Bus::session();
  DBusMessage *q = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                "GetConnectionUnixProcessID");
  if (!bus || !q) return;
  std::string sender = l.sender;
  programs_[sender] = "";  // asked once
  dbus::Writer(q).str(sender);
  bus->call_async(q, [this, sender](DBusMessage *reply) {
    std::string program;
    if (reply) {
      auto args = dbus::read_args(reply);
      if (!args.empty()) program = dbus::peer_program(long(args[0].num()));
    }
    auto known = programs_.find(sender);
    if (known == programs_.end()) return;  // gone meanwhile
    known->second = program;
    for (auto &kv : launchers_)
      if (kv.second.sender == sender && kv.second.app.empty()) resolve(kv.second);
    schedule();
  }, 1000);
}

void Badges::on_name_lost(DBusMessage *m) {
  auto args = dbus::read_args(m);
  if (args.size() < 3 || !args[2].str().empty()) return;
  const std::string name = args[0].str();
  if (name.empty() || name[0] != ':') return;
  programs_.erase(name);
  bool any = false;
  for (auto it = launchers_.begin(); it != launchers_.end();)
    if (it->second.sender == name) {
      it = launchers_.erase(it);
      any = true;
    } else {
      ++it;
    }
  if (any) schedule();
}

void Badges::schedule() {
  if (pending_) return;
  pending_ = true;
  Fl::add_timeout(0, recompute, this);
}

// Counts the badges again; the subscribers hear of it only when one changed.
void Badges::recompute(void *data) {
  auto *self = static_cast<Badges *>(data);
  self->pending_ = false;
  // The applications are read on idle after the panel started: until then
  // the notifications cannot be matched.
  if (!app_catalog::ready()) {
    self->pending_ = true;
    Fl::add_timeout(0.5, recompute, data);
    return;
  }
  auto apps = app_catalog::current();
  if (apps->generation != self->generation_) {
    // Installed or removed applications: match everything again.
    self->generation_ = apps->generation;
    self->notes_.clear();
    for (auto &kv : self->launchers_) self->resolve(kv.second);
  }
  std::map<std::string, int> unread;
  std::map<uint32_t, std::string> notes;
  for (auto &e : NotifyCenter::get().entries()) {
    auto it = self->notes_.find(e.id);
    std::string app = it != self->notes_.end()
                          ? it->second
                          : match_app({e.desktop_entry, e.program, e.app, e.icon}, apps->apps);
    notes[e.id] = app;
    if (e.unread && !app.empty()) ++unread[app];
  }
  self->notes_.swap(notes);  // only the notifications still in the history
  // Several senders of one application (two windows, two profiles): their
  // counts added, urgent when one is.
  std::map<std::string, LauncherState> launch;
  for (auto &kv : self->launchers_) {
    const Launcher &l = kv.second;
    if (l.app.empty()) continue;
    auto it = launch.find(l.app);
    if (it == launch.end()) {
      launch[l.app] = l.state;
      continue;
    }
    LauncherState &s = it->second;
    if (l.state.count_visible) {
      s.count = (s.count_visible ? s.count : 0) + l.state.count;
      s.count_visible = true;
    }
    s.urgent = s.urgent || l.state.urgent;
    if (l.state.progress_visible && !s.progress_visible) {
      s.progress = l.state.progress;
      s.progress_visible = true;
    }
  }
  std::map<std::string, Badge> next;
  for (auto &kv : launch) {
    auto u = unread.find(kv.first);
    Badge b = combine(&kv.second, u == unread.end() ? 0 : u->second);
    if (b.shown() || b.progress >= 0) next[kv.first] = b;
  }
  for (auto &kv : unread)
    if (!next.count(kv.first)) next[kv.first] = combine(nullptr, kv.second);
  if (next == self->badges_) return;
  self->badges_.swap(next);
  auto subscribers = self->subscribers_;
  for (auto &s : subscribers) s.first(s.second);
}

Colors Badges::colors(const std::string &id, const Color &surface, const Color &accent) {
  auto apps = app_catalog::ready() ? app_catalog::current() : nullptr;
  const DesktopEntry *e = apps ? apps->find(id) : nullptr;
  const int size = 32;
  std::string icon = e ? IconLoader::app_icon(*e, size) : std::string();
  std::string key = icon + '\n' + surface.to_hex() + accent.to_hex();
  auto it = colors_.find(key);
  if (it != colors_.end()) return it->second;
  Colors c = readable(accent, surface);
  if (cairo_surface_t *src = e ? IconLoader::get({icon, "application-x-executable"}, size) : nullptr) {
    // The icon painted at a fixed size (whatever kind of surface the
    // loader keeps), then its pixels.
    double sw = size, sh = size;
    if (cairo_surface_get_type(src) == CAIRO_SURFACE_TYPE_IMAGE) {
      sw = std::max(1, cairo_image_surface_get_width(src));
      sh = std::max(1, cairo_image_surface_get_height(src));
    }
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(img);
    cairo_scale(cr, size / sw, size / sh);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(img);
    c = colors_from_pixels(cairo_image_surface_get_data(img), size, size, cairo_image_surface_get_stride(img),
                           surface, accent);
    cairo_surface_destroy(img);
  }
  if (colors_.size() >= 512) colors_.clear();
  colors_[key] = c;
  return c;
}

std::string Badges::qa_lines() const {
  std::string out;
  for (auto &kv : badges_) {
    std::string line = "@app-badge:" + kv.first + "=" + std::to_string(kv.second.count);
    if (kv.second.urgent) line += ":urgent";
    if (kv.second.progress >= 0) line += ":progress " + std::to_string(int(std::lround(kv.second.progress * 100)));
    out += line + "\t0\t0\t0\t0\t1\n";
  }
  return out;
}

// ------------------------------------------------------------------ drawing

namespace {
int font_for(int h) { return std::max(S(8), int(std::lround(h * 0.64))); }
}  // namespace

int pill_width(const Badge &b, int h) {
  if (b.count <= 0) return int(std::lround(h * 0.62));  // urgent: a dot
  text_font(font_for(h), true);
  return std::max(h, text_width(count_text(b.count)) + int(std::lround(h * 0.62)));
}

void paint_pill(Painter &p, double right, double top, int h, const Badge &b, const Colors &c, const Color &ring) {
  if (!b.shown()) return;
  double w = pill_width(b, h), x = right - w, r = Sd(1.5);
  if (b.count <= 0) {
    // Only urgent: a dot in the middle of the place of a pill.
    double cx = right - w / 2, cy = top + h / 2.0;
    if (ring.a > 0) p.circle(cx, cy, w / 2 + r, ring);
    p.circle(cx, cy, w / 2, c.fill);
    return;
  }
  if (ring.a > 0) p.fill_rounded(x - r, top - r, w + 2 * r, h + 2 * r, (h + 2 * r) / 2, ring);
  p.fill_rounded(x, top, w, h, h / 2.0, c.fill);
}

void draw_pill_text(int right, int top, int h, const Badge &b, const Colors &c) {
  if (b.count <= 0) return;
  int w = pill_width(b, h);  // sets the font
  draw_text(count_text(b.count), right - w, top, w, h, c.text, 1);
}

void paint_progress(Painter &p, double x, double y, double w, double h, double progress, const Colors &c,
                    const Color &track) {
  if (progress < 0 || w <= 0) return;
  p.fill_rounded(x, y, w, h, h / 2, track);
  double done = std::max(h, w * std::min(1.0, progress));
  p.fill_rounded(x, y, done, h, h / 2, c.fill);
}

}  // namespace badges
