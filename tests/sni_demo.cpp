// SPDX-License-Identifier: GPL-3.0-or-later
// sde-sni-demo: a minimal StatusNotifierItem with a dbusmenu, used to test
// the panel's AppIndicator support. Not installed.
//
//   sde-sni-demo [icon-name [id [title]]]
#include <FL/Fl.H>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "dbus.h"

using namespace sde;

namespace {

dbus::Bus *g_bus;
std::string g_icon = "utilities-terminal", g_id = "sde-sni-demo", g_title = "SDE tray demo";

void add_prop(dbus::Writer &dict, const char *key, const std::string &value, char type = 's') {
  dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(key);
  if (type == 'o') e.variant_path(value);
  else e.variant_str(value);
}

void add_bool(dbus::Writer &dict, const char *key, bool value) {
  dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(key).variant_bool(value);
}

void menu_node(dbus::Writer &parent, int id, const char *label, bool separator) {
  dbus::Writer v(parent.iter(), DBUS_TYPE_VARIANT, "(ia{sv}av)");
  dbus::Writer s(v.iter(), DBUS_TYPE_STRUCT, nullptr);
  s.i32(id);
  {
    dbus::Writer props(s.iter(), DBUS_TYPE_ARRAY, "{sv}");
    if (separator) add_prop(props, "type", "separator");
    else add_prop(props, "label", label);
  }
  dbus::Writer children(s.iter(), DBUS_TYPE_ARRAY, "v");
}

DBusHandlerResult on_item(DBusMessage *m) {
  if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  std::string member = dbus_message_get_member(m);
  DBusMessage *r = dbus_message_new_method_return(m);
  if (dbus_message_has_interface(m, "org.freedesktop.DBus.Properties") && member == "GetAll") {
    dbus::Writer w(r);
    dbus::Writer dict(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
    add_prop(dict, "Category", "ApplicationStatus");
    add_prop(dict, "Id", g_id);
    add_prop(dict, "Title", g_title);
    add_prop(dict, "Status", "Active");
    add_prop(dict, "IconName", g_icon);
    add_prop(dict, "Menu", "/MenuBar", 'o');
    add_bool(dict, "ItemIsMenu", false);
  } else if (member == "Activate" || member == "SecondaryActivate" || member == "ContextMenu" ||
             member == "Scroll") {
    printf("item: %s\n", member.c_str());
    fflush(stdout);
  } else {
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  g_bus->send(r);
  return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult on_menu(DBusMessage *m) {
  if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  std::string member = dbus_message_get_member(m);
  DBusMessage *r = dbus_message_new_method_return(m);
  if (member == "GetLayout") {
    dbus::Writer w(r);
    w.u32(1);
    dbus::Writer root(w.iter(), DBUS_TYPE_STRUCT, nullptr);
    root.i32(0);
    {
      dbus::Writer props(root.iter(), DBUS_TYPE_ARRAY, "{sv}");
      add_prop(props, "children-display", "submenu");
    }
    dbus::Writer children(root.iter(), DBUS_TYPE_ARRAY, "v");
    menu_node(children, 1, "_Open Demo Window", false);
    menu_node(children, 2, "", true);
    menu_node(children, 3, "_Quit", false);
  } else if (member == "AboutToShow") {
    dbus::Writer(r).boolean(false);
  } else if (member == "Event") {
    auto args = dbus::read_args(m);
    printf("menu event: id=%lld %s\n", args.empty() ? -1LL : (long long)args[0].num(),
           args.size() > 1 ? args[1].s.c_str() : "");
    fflush(stdout);
    if (!args.empty() && args[0].num() == 3) exit(0);
  } else {
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  g_bus->send(r);
  return DBUS_HANDLER_RESULT_HANDLED;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc > 1) g_icon = argv[1];
  if (argc > 2) g_id = argv[2];
  if (argc > 3) g_title = argv[3];
  g_bus = dbus::Bus::session();
  if (!g_bus) return 1;
  std::string name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
  g_bus->request_name(name);
  g_bus->export_object("/StatusNotifierItem", on_item);
  g_bus->export_object("/MenuBar", on_menu);
  DBusMessage *reg = dbus::method("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                                  "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem");
  dbus::Writer(reg).str(name);
  DBusMessage *reply = g_bus->call(reg, 2000);
  printf("registered: %s\n", reply ? "yes" : "no (no watcher)");
  fflush(stdout);
  if (reply) dbus_message_unref(reply);
  for (;;) Fl::wait(1e20);
}
