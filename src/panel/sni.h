// SPDX-License-Identifier: GPL-3.0-or-later
// StatusNotifierItem / AppIndicator support for the panel's tray.
//
// The panel runs the org.kde.StatusNotifierWatcher service (or registers as
// a host with an existing watcher), reads each item's properties, follows
// its change signals and shows its com.canonical.dbusmenu menu. XEmbed tray
// icons keep working next to these items (see tray.h).
#pragma once

#include <cairo.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sde {
namespace dbus {
class Bus;
struct Value;
}  // namespace dbus
}  // namespace sde

class SniHost {
 public:
  struct Item {
    std::string service;   // as registered (well-known or unique name)
    std::string owner;     // unique bus name, used to match signals
    std::string path = "/StatusNotifierItem";
    std::string id, title, status = "Active", tooltip;
    std::string icon_name, attention_icon_name, icon_theme_path;
    std::string menu_path;
    bool item_is_menu = false;
    cairo_surface_t *pixmap = nullptr;            // owned
    cairo_surface_t *attention_pixmap = nullptr;  // owned
    bool loaded = false;
  };

  explicit SniHost(std::function<void()> changed);
  ~SniHost();

  // Connects to the session bus. Returns false when D-Bus is unavailable.
  bool start();

  // Items that should be shown (Status != Passive, properties loaded, not
  // hidden as a duplicate of a native SDE indicator).
  std::vector<const Item *> visible_items() const;

  // Hides items whose Id, Title or bus name matches one of |names|
  // (case-insensitive), e.g. nm-applet next to SDE's own network indicator.
  void set_hidden(std::vector<std::string> names);
  bool is_hidden(const Item &item) const;
  // Plain XKB layout applets (gxkb, xxkb...) repeat SDE's own keyboard
  // indicator: hidden while it is shown.
  void set_hide_layout_applets(bool hide) { hide_layout_applets_ = hide; }
  // An input method framework (IBus, Fcitx...) shows an item.
  bool has_input_method() const;

  // The name shown for |item| (tooltip, accessibility): its Title, else its
  // ToolTip title, Id or bus name; always printable text, never empty.
  static std::string display_name(const Item &item);

  // Icon for |item| at |size| pixels (not owned; may be nullptr).
  cairo_surface_t *icon(const Item &item, int size) const;

  // Mouse actions. Coordinates are root coordinates.
  void activate(const Item &item, int x, int y);
  void secondary_activate(const Item &item, int x, int y);
  void context_menu(const Item &item, int x, int y);
  void scroll(const Item &item, int delta);

 private:
  void add_item(const std::string &service, const std::string &sender);
  void remove_owner(const std::string &owner);
  void drop_duplicates();
  void refresh(Item &item);
  void apply_properties(Item &item, const sde::dbus::Value &props);
  bool show_dbus_menu(const Item &item, int x, int y);
  void notify_changed();
  void emit_watcher_signal(const char *member, const std::string &arg);

  std::function<void()> changed_;
  sde::dbus::Bus *bus_ = nullptr;
  bool own_watcher_ = false;
  std::vector<std::unique_ptr<Item>> items_;
  std::vector<std::string> hidden_;
  bool hide_layout_applets_ = false;
};
