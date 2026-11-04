// SPDX-License-Identifier: GPL-3.0-or-later
// StatusNotifierItem / AppIndicator support for the panel's tray.
//
// The panel runs the org.kde.StatusNotifierWatcher service (or registers as
// a host with an existing watcher), reads each item's properties, follows
// its change signals and shows its com.canonical.dbusmenu menu. XEmbed tray
// icons keep working next to these items (see tray.h).
//
// Every call to an item is asynchronous (0.2.0 Alpha 4): a program that does
// not answer cannot block the panel, and a menu is shown from the event loop,
// never from inside a D-Bus handler (docs/PANEL.md, "Tray icons").
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
    // A GetAll is on its way; |stale|: a change signal came meanwhile, so the
    // properties are read once more when it answers (a program that sends
    // NewIcon a thousand times costs two calls, not a thousand).
    bool reading = false, stale = false;
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

  // A menu of an item is open, or on its way (QA and tests).
  bool menu_pending() const { return menu_open_ || bool(pending_menu_); }

 private:
  struct PendingMenu;
  void add_item(const std::string &service, const std::string &sender);
  void remove_owner(const std::string &owner);
  void drop_duplicates();
  void refresh(Item &item);
  void apply_properties(Item &item, const sde::dbus::Value &props);
  // Asks for the item's dbusmenu (AboutToShow, GetLayout); the menu opens
  // from the event loop once the layout arrives (show_pending_menu). Without
  // an answer the program is asked to show its own menu (ContextMenu).
  void request_menu(const Item &item, int x, int y);
  static void show_pending_menu(void *data);
  void notify_changed();
  void emit_watcher_signal(const char *member, const std::string &arg);

  std::function<void()> changed_;
  std::shared_ptr<PendingMenu> pending_menu_;
  unsigned menu_serial_ = 0;
  bool menu_open_ = false;
  sde::dbus::Bus *bus_ = nullptr;
  bool own_watcher_ = false;
  std::vector<std::unique_ptr<Item>> items_;
  std::vector<std::string> hidden_;
  bool hide_layout_applets_ = false;
};
