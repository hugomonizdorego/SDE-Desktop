// SPDX-License-Identifier: GPL-3.0-or-later
// sde-sni-demo: a minimal StatusNotifierItem with a dbusmenu, used to test
// the panel's AppIndicator support. Not installed.
//
//   sde-sni-demo [icon-name [id [title]]]
#include <FL/Fl.H>
#include <cairo.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

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

// A 16x16 PNG for the icon-data of a menu item.
std::vector<unsigned char> demo_png() {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 0.86, 0.15, 0.15);
  cairo_arc(cr, 8, 8, 7, 0, 6.2832);
  cairo_fill(cr);
  cairo_destroy(cr);
  std::vector<unsigned char> out;
  cairo_surface_write_to_png_stream(
      s,
      [](void *closure, const unsigned char *data, unsigned int n) {
        auto *o = static_cast<std::vector<unsigned char> *>(closure);
        o->insert(o->end(), data, data + n);
        return CAIRO_STATUS_SUCCESS;
      },
      &out);
  cairo_surface_destroy(s);
  return out;
}

void add_bytes(dbus::Writer &dict, const char *key, const std::vector<unsigned char> &bytes) {
  dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(key);
  dbus::Writer v(e.iter(), DBUS_TYPE_VARIANT, "ay");
  DBusMessageIter arr;
  dbus_message_iter_open_container(v.iter(), DBUS_TYPE_ARRAY, "y", &arr);
  const unsigned char *data = bytes.data();
  dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE, &data, int(bytes.size()));
  dbus_message_iter_close_container(v.iter(), &arr);
}

// |icon|: an icon-name; "@png": icon-data (0.2: the panel shows both).
void menu_node(dbus::Writer &parent, int id, const char *label, bool separator,
               const char *icon = nullptr) {
  dbus::Writer v(parent.iter(), DBUS_TYPE_VARIANT, "(ia{sv}av)");
  dbus::Writer s(v.iter(), DBUS_TYPE_STRUCT, nullptr);
  s.i32(id);
  {
    dbus::Writer props(s.iter(), DBUS_TYPE_ARRAY, "{sv}");
    if (separator) add_prop(props, "type", "separator");
    else add_prop(props, "label", label);
    if (icon && std::string(icon) == "@png") add_bytes(props, "icon-data", demo_png());
    else if (icon) add_prop(props, "icon-name", icon);
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
    menu_node(children, 1, "_Open Demo Window", false, "window-new");
    menu_node(children, 2, "", true);
    menu_node(children, 3, "_Quit", false, "@png");
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
