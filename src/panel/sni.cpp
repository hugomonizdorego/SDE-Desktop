// SPDX-License-Identifier: GPL-3.0-or-later
#include "sni.h"
#include "status_model.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>

#include "dbus.h"
#include "icons.h"
#include "panel.h"
#include "util.h"

using namespace sde;

namespace {

const char *kWatcherName = "org.kde.StatusNotifierWatcher";
const char *kWatcherPath = "/StatusNotifierWatcher";
const char *kWatcherIface = "org.kde.StatusNotifierWatcher";
const char *kItemIface = "org.kde.StatusNotifierItem";

// Picks the largest pixmap of an a(iiay) icon list.
cairo_surface_t *pixmap_from(const dbus::Value &v) {
  const dbus::Value &list = v.inner();
  const dbus::Value *best = nullptr;
  int best_w = 0;
  for (auto &p : list.items) {
    if (p.items.size() < 3) continue;
    int w = int(p.items[0].num());
    if (w > best_w && w <= 256) {
      best = &p;
      best_w = w;
    }
  }
  if (!best) return nullptr;
  int w = int(best->items[0].num()), h = int(best->items[1].num());
  auto bytes = best->items[2].bytes();
  return argb_network_pixels(bytes.data(), bytes.size(), w, h, std::max(w, h));
}

// "_File" -> "File", "__" -> "_"; '&' is FLTK's shortcut marker.
std::string menu_label(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '_') {
      if (i + 1 < in.size() && in[i + 1] == '_') {
        out += '_';
        ++i;
      }
      continue;
    }
    if (in[i] == '&') out += '&';
    out += in[i];
  }
  return out;
}

}  // namespace

SniHost::SniHost(std::function<void()> changed) : changed_(std::move(changed)) {}

SniHost::~SniHost() {
  for (auto &i : items_) {
    if (i->pixmap) cairo_surface_destroy(i->pixmap);
    if (i->attention_pixmap) cairo_surface_destroy(i->attention_pixmap);
  }
}

void SniHost::notify_changed() {
  if (changed_) changed_();
}

void SniHost::emit_watcher_signal(const char *member, const std::string &arg) {
  DBusMessage *s = dbus::signal(kWatcherPath, kWatcherIface, member);
  if (arg.empty() && std::string(member) == "StatusNotifierHostRegistered") {
    bus_->send(s);
    return;
  }
  dbus::Writer(s).str(arg);
  bus_->send(s);
}

bool SniHost::start() {
  bus_ = dbus::Bus::session();
  if (!bus_) return false;

  // Item property changes.
  bus_->add_signal_handler(
      std::string("type='signal',interface='") + kItemIface + "'",
      [](DBusMessage *m) { return dbus_message_has_interface(m, kItemIface); },
      [this](DBusMessage *m) {
        const char *sender = dbus_message_get_sender(m);
        const char *path = dbus_message_get_path(m);
        if (!sender || !path) return;
        for (auto &i : items_)
          if (i->owner == sender && i->path == path) refresh(*i);
      });
  // Items disappear with their owners.
  bus_->add_signal_handler(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
      "member='NameOwnerChanged'",
      [](DBusMessage *m) {
        return dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged");
      },
      [this](DBusMessage *m) {
        auto args = dbus::read_args(m);
        if (args.size() == 3 && args[2].s.empty() && !args[1].s.empty()) remove_owner(args[1].s);
      });

  own_watcher_ = bus_->request_name(kWatcherName);
  bus_->request_name("org.kde.StatusNotifierHost-" + std::to_string(getpid()));
  if (own_watcher_) {
    bus_->export_object(kWatcherPath, [this](DBusMessage *msg) -> DBusHandlerResult {
      if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      std::string member = dbus_message_get_member(msg) ? dbus_message_get_member(msg) : "";
      const char *sender = dbus_message_get_sender(msg);
      auto args = dbus::read_args(msg);
      DBusMessage *reply = dbus_message_new_method_return(msg);
      if (dbus_message_has_interface(msg, kWatcherIface) && member == "RegisterStatusNotifierItem" &&
          !args.empty()) {
        add_item(args[0].s, sender ? sender : "");
        bus_->send(reply);
        emit_watcher_signal("StatusNotifierItemRegistered", args[0].s);
        return DBUS_HANDLER_RESULT_HANDLED;
      }
      if (dbus_message_has_interface(msg, kWatcherIface) && member == "RegisterStatusNotifierHost") {
        bus_->send(reply);
        emit_watcher_signal("StatusNotifierHostRegistered", "");
        return DBUS_HANDLER_RESULT_HANDLED;
      }
      if (dbus_message_has_interface(msg, "org.freedesktop.DBus.Properties")) {
        std::vector<std::string> registered;
        for (auto &i : items_) registered.push_back(i->owner + i->path);
        std::string prop = args.size() >= 2 ? args[1].s : "";
        if (member == "Get") {
          dbus::Writer w(reply);
          if (prop == "RegisteredStatusNotifierItems") w.variant_strings(registered);
          else if (prop == "IsStatusNotifierHostRegistered") w.variant_bool(true);
          else if (prop == "ProtocolVersion") w.variant_i32(0);
          else {
            dbus_message_unref(reply);
            bus_->send(dbus::Bus::error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property " + prop));
            return DBUS_HANDLER_RESULT_HANDLED;
          }
        } else if (member == "GetAll") {
          dbus::Writer w(reply);
          dbus::Writer dict(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
          {
            dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
            e.str("RegisteredStatusNotifierItems").variant_strings(registered);
          }
          {
            dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
            e.str("IsStatusNotifierHostRegistered").variant_bool(true);
          }
          {
            dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
            e.str("ProtocolVersion").variant_i32(0);
          }
        } else {
          dbus_message_unref(reply);
          return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        bus_->send(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
      }
      if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        dbus::Writer(reply).str(
            "<node><interface name=\"org.kde.StatusNotifierWatcher\">"
            "<method name=\"RegisterStatusNotifierItem\"><arg type=\"s\" direction=\"in\"/></method>"
            "<method name=\"RegisterStatusNotifierHost\"><arg type=\"s\" direction=\"in\"/></method>"
            "<property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>"
            "<property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>"
            "<property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>"
            "<signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>"
            "<signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>"
            "<signal name=\"StatusNotifierHostRegistered\"/>"
            "</interface></node>");
        bus_->send(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
      }
      dbus_message_unref(reply);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    });
    emit_watcher_signal("StatusNotifierHostRegistered", "");
  } else {
    // Another watcher runs (e.g. from another panel): be a host for it.
    fprintf(stderr, "sde-panel: using the existing StatusNotifierWatcher\n");
    bus_->add_signal_handler(
        std::string("type='signal',interface='") + kWatcherIface + "'",
        [](DBusMessage *m) { return dbus_message_has_interface(m, kWatcherIface); },
        [this](DBusMessage *m) {
          auto args = dbus::read_args(m);
          if (args.empty()) return;
          if (dbus_message_has_member(m, "StatusNotifierItemRegistered")) add_item(args[0].s, "");
        });
    DBusMessage *reg = dbus::method(kWatcherName, kWatcherPath, kWatcherIface, "RegisterStatusNotifierHost");
    dbus::Writer(reg).str(bus_->unique_name());
    bus_->call_async(reg, nullptr);
    DBusMessage *get = dbus::method(kWatcherName, kWatcherPath, "org.freedesktop.DBus.Properties", "Get");
    dbus::Writer(get).str(kWatcherIface).str("RegisteredStatusNotifierItems");
    bus_->call_async(get, [this](DBusMessage *reply) {
      auto args = dbus::read_args(reply);
      if (args.empty()) return;
      for (auto &s : args[0].inner().items) add_item(s.s, "");
    });
  }
  return true;
}

void SniHost::add_item(const std::string &service, const std::string &sender) {
  auto item = std::make_unique<Item>();
  if (!service.empty() && service[0] == '/') {
    // libappindicator registers its object path; the bus name is the sender.
    item->service = sender;
    item->path = service;
  } else {
    size_t slash = service.find('/');
    item->service = service.substr(0, slash);
    if (slash != std::string::npos) item->path = service.substr(slash);
  }
  if (item->service.empty()) return;
  if (item->service[0] == ':') item->owner = item->service;
  else if (!sender.empty()) item->owner = sender;
  // An application can register the same item twice: once by object path
  // (libappindicator) and once by a well-known name, or again after the
  // watcher restarts. Both point at the same owner and object path.
  for (auto &i : items_) {
    if (i->service == item->service && i->path == item->path) return;
    if (!item->owner.empty() && i->owner == item->owner && i->path == item->path) return;
  }
  Item *raw = item.get();
  items_.push_back(std::move(item));
  if (raw->owner.empty()) {
    DBusMessage *m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                  "GetNameOwner");
    dbus::Writer(m).str(raw->service);
    std::string service_copy = raw->service, path_copy = raw->path;
    bus_->call_async(m, [this, service_copy, path_copy](DBusMessage *reply) {
      auto args = dbus::read_args(reply);
      for (auto &i : items_)
        if (i->service == service_copy && i->path == path_copy && !args.empty()) i->owner = args[0].s;
      drop_duplicates();
    });
  }
  refresh(*raw);
}

void SniHost::remove_owner(const std::string &owner) {
  bool changed = false;
  for (auto it = items_.begin(); it != items_.end();) {
    if ((*it)->owner == owner || (*it)->service == owner) {
      if (own_watcher_) emit_watcher_signal("StatusNotifierItemUnregistered", (*it)->owner + (*it)->path);
      if ((*it)->pixmap) cairo_surface_destroy((*it)->pixmap);
      if ((*it)->attention_pixmap) cairo_surface_destroy((*it)->attention_pixmap);
      it = items_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) notify_changed();
}

void SniHost::drop_duplicates() {
  bool changed = false;
  for (size_t a = 0; a < items_.size(); ++a)
    for (size_t b = a + 1; b < items_.size();) {
      const Item &x = *items_[a], &y = *items_[b];
      bool same = !x.owner.empty() && x.owner == y.owner && x.path == y.path;
      if (!same) {
        ++b;
        continue;
      }
      if (items_[b]->pixmap) cairo_surface_destroy(items_[b]->pixmap);
      if (items_[b]->attention_pixmap) cairo_surface_destroy(items_[b]->attention_pixmap);
      items_.erase(items_.begin() + long(b));
      changed = true;
    }
  if (changed) notify_changed();
}

void SniHost::set_hidden(std::vector<std::string> names) {
  for (auto &n : names) n = lower(n);
  hidden_ = std::move(names);
}

bool SniHost::has_input_method() const {
  for (auto &i : items_) {
    if (!i->loaded || i->status == "Passive") continue;
    for (const std::string &v : {i->id, i->title, i->service})
      if (keyboard_applet(v) == KeyboardApplet::INPUT_METHOD) return true;
  }
  return false;
}

bool SniHost::is_hidden(const Item &item) const {
  if (hide_layout_applets_)
    for (const std::string &v : {item.id, item.title})
      if (keyboard_applet(v) == KeyboardApplet::LAYOUT) return true;
  if (hidden_.empty()) return false;
  for (const std::string &value : {item.id, item.title, item.service}) {
    std::string v = lower(value);
    if (v.empty()) continue;
    for (const auto &h : hidden_)
      if (!h.empty() && (v == h || v.rfind(h + "-", 0) == 0)) return true;
  }
  return false;
}

void SniHost::refresh(Item &item) {
  std::string service = item.service, path = item.path;
  dbus::get_all_properties(bus_, service, path, kItemIface, [this, service, path](const dbus::Value &props) {
    for (auto &i : items_) {
      if (i->service != service || i->path != path) continue;
      if (!props.valid()) return;
      apply_properties(*i, props);
      notify_changed();
      return;
    }
  });
}

void SniHost::apply_properties(Item &item, const dbus::Value &props) {
  auto str = [&](const char *key) {
    const dbus::Value *v = props.get(key);
    return v ? v->str() : std::string();
  };
  item.id = str("Id");
  item.title = str("Title");
  item.status = str("Status");
  if (item.status.empty()) item.status = "Active";
  item.icon_name = str("IconName");
  item.attention_icon_name = str("AttentionIconName");
  item.icon_theme_path = str("IconThemePath");
  item.menu_path = str("Menu");
  const dbus::Value *menu_flag = props.get("ItemIsMenu");
  item.item_is_menu = menu_flag && menu_flag->boolean();
  if (item.pixmap) cairo_surface_destroy(item.pixmap);
  if (item.attention_pixmap) cairo_surface_destroy(item.attention_pixmap);
  item.pixmap = nullptr;
  item.attention_pixmap = nullptr;
  if (const dbus::Value *p = props.get("IconPixmap")) item.pixmap = pixmap_from(*p);
  if (const dbus::Value *p = props.get("AttentionIconPixmap")) item.attention_pixmap = pixmap_from(*p);
  item.tooltip = item.title;
  if (const dbus::Value *tt = props.get("ToolTip")) {
    const dbus::Value &t = tt->inner();
    if (t.items.size() >= 3 && !t.items[2].s.empty()) item.tooltip = t.items[2].s;
  }
  item.loaded = true;
}

std::string SniHost::display_name(const Item &item) {
  return tray_item_name(item.title, item.tooltip, item.id, item.service);
}

std::vector<const SniHost::Item *> SniHost::visible_items() const {
  std::vector<const Item *> out;
  for (auto &i : items_)
    if (i->loaded && i->status != "Passive" && !is_hidden(*i)) out.push_back(i.get());
  return out;
}

cairo_surface_t *SniHost::icon(const Item &item, int size) const {
  bool attention = item.status == "NeedsAttention";
  std::string name = attention && !item.attention_icon_name.empty() ? item.attention_icon_name : item.icon_name;
  cairo_surface_t *pix = attention && item.attention_pixmap ? item.attention_pixmap : item.pixmap;
  if (!name.empty()) {
    if (name[0] == '/') return IconLoader::get({name}, size);
    if (!item.icon_theme_path.empty()) {
      // Applications may ship their own icons in IconThemePath.
      static std::map<std::string, cairo_surface_t *> cache;
      std::string key = item.icon_theme_path + "|" + name + "|" + std::to_string(size);
      auto it = cache.find(key);
      if (it != cache.end()) return it->second;
      for (const char *ext : {".svg", ".png"}) {
        std::vector<std::string> candidates{item.icon_theme_path + "/" + name + ext};
        for (const char *sz : {"scalable", "256x256", "128x128", "64x64", "48x48", "32x32", "24x24", "22x22", "16x16"})
          candidates.push_back(item.icon_theme_path + "/hicolor/" + sz + "/apps/" + name + ext);
        for (auto &c : candidates) {
          if (!file_exists(c)) continue;
          cairo_surface_t *s = load_image(c, size, size);
          cache[key] = s;
          if (s) return s;
        }
      }
    }
    if (!IconLoader::lookup(name, size).empty()) return IconLoader::get({name}, size);
  }
  if (pix) return pix;
  return IconLoader::get({name, "application-x-executable"}, size);
}

void SniHost::activate(const Item &item, int x, int y) {
  if (item.item_is_menu && !item.menu_path.empty()) {
    context_menu(item, x, y);
    return;
  }
  DBusMessage *m = dbus::method(item.service, item.path, kItemIface, "Activate");
  dbus::Writer(m).i32(x).i32(y);
  std::string service = item.service, path = item.path;
  bus_->call_async(m, [this, service, path, x, y](DBusMessage *reply) {
    if (reply) return;
    // No Activate method (common with AppIndicators): show the menu instead.
    for (auto &i : items_)
      if (i->service == service && i->path == path) {
        context_menu(*i, x, y);
        return;
      }
  });
}

void SniHost::secondary_activate(const Item &item, int x, int y) {
  DBusMessage *m = dbus::method(item.service, item.path, kItemIface, "SecondaryActivate");
  dbus::Writer(m).i32(x).i32(y);
  bus_->call_async(m, nullptr);
}

void SniHost::scroll(const Item &item, int delta) {
  DBusMessage *m = dbus::method(item.service, item.path, kItemIface, "Scroll");
  dbus::Writer(m).i32(delta).str("vertical");
  bus_->call_async(m, nullptr);
}

void SniHost::context_menu(const Item &item, int x, int y) {
  if (!item.menu_path.empty() && item.menu_path != "/" && show_dbus_menu(item, x, y)) return;
  DBusMessage *m = dbus::method(item.service, item.path, kItemIface, "ContextMenu");
  dbus::Writer(m).i32(x).i32(y);
  bus_->call_async(m, nullptr);
}

bool SniHost::show_dbus_menu(const Item &item, int x, int y) {
  const char *iface = "com.canonical.dbusmenu";
  DBusMessage *about = dbus::method(item.service, item.menu_path, iface, "AboutToShow");
  dbus::Writer(about).i32(0);
  if (DBusMessage *r = bus_->call(about, 500)) dbus_message_unref(r);

  DBusMessage *get = dbus::method(item.service, item.menu_path, iface, "GetLayout");
  dbus::Writer(get).i32(0).i32(-1).strings(
      {"type", "label", "enabled", "visible", "toggle-type", "toggle-state", "children-display"});
  DBusMessage *reply = bus_->call(get, 1500);
  if (!reply) return false;
  auto args = dbus::read_args(reply);
  dbus_message_unref(reply);
  if (args.size() < 2) return false;

  std::deque<std::string> labels;  // stable storage for menu labels
  std::vector<Fl_Menu_Item> menu;
  std::function<void(const dbus::Value &)> add_children = [&](const dbus::Value &node) {
    if (node.items.size() < 3) return;
    for (auto &child_v : node.items[2].items) {
      const dbus::Value &child = child_v.inner();
      if (child.items.size() < 3) continue;
      int id = int(child.items[0].num());
      const dbus::Value &props = child.items[1];
      auto prop = [&](const char *k) { return props.get(k); };
      if (prop("visible") && !prop("visible")->boolean()) continue;
      if (prop("type") && prop("type")->str() == "separator") {
        if (!menu.empty()) menu.back().flags |= FL_MENU_DIVIDER;
        continue;
      }
      labels.push_back(menu_label(prop("label") ? prop("label")->str() : ""));
      Fl_Menu_Item mi{};
      mi.text = labels.back().c_str();
      mi.user_data_ = reinterpret_cast<void *>(intptr_t(id));
      if (prop("enabled") && !prop("enabled")->boolean()) mi.flags |= FL_MENU_INACTIVE;
      std::string toggle = prop("toggle-type") ? prop("toggle-type")->str() : "";
      if (toggle == "checkmark") mi.flags |= FL_MENU_TOGGLE;
      if (toggle == "radio") mi.flags |= FL_MENU_RADIO;
      if (!toggle.empty() && prop("toggle-state") && prop("toggle-state")->num() == 1) mi.flags |= FL_MENU_VALUE;
      bool submenu = !child.items[2].items.empty() ||
                     (prop("children-display") && prop("children-display")->str() == "submenu");
      if (submenu) {
        mi.flags |= FL_SUBMENU;
        menu.push_back(mi);
        add_children(child);
        menu.push_back(Fl_Menu_Item{});
      } else {
        menu.push_back(mi);
      }
    }
  };
  add_children(args[1].inner());
  if (menu.empty()) return false;
  menu.push_back(Fl_Menu_Item{});

  (void)y;
  const Fl_Menu_Item *picked = Panel::instance->popup_menu(menu.data(), x);
  if (!picked || (picked->flags & FL_SUBMENU)) return true;
  DBusMessage *ev = dbus::method(item.service, item.menu_path, iface, "Event");
  {
    dbus::Writer w(ev);
    w.i32(int32_t(reinterpret_cast<intptr_t>(picked->user_data_))).str("clicked").variant_i32(0).u32(0);
  }
  dbus_message_set_no_reply(ev, TRUE);
  bus_->send(ev);
  return true;
}
