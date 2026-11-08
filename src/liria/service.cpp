// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "service.h"

#include <FL/Fl.H>

#include <cstring>
#include <memory>

#include "dbus.h"
#include "file_chooser_model.h"
#include "location.h"
#include "ui.h"
#include "util.h"

namespace sde {
namespace liria {

const char *const kBusName = "org.debion.Liria";

namespace {

const char *kPath = "/org/debion/Liria";
const char *kIface = "org.debion.Liria";
const char *kFm = "org.freedesktop.FileManager1";
const char *kFmPath = "/org/freedesktop/FileManager1";

const char *kLiriaXml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.debion.Liria\">\n"
    "    <method name=\"Open\"><arg type=\"as\" name=\"locations\" direction=\"in\"/>"
    "<arg type=\"as\" name=\"select\" direction=\"in\"/></method>\n"
    "    <method name=\"Quit\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\"><arg type=\"s\" name=\"xml\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "</node>\n";

const char *kFmXml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.FileManager1\">\n"
    "    <method name=\"ShowFolders\"><arg type=\"as\" name=\"URIs\" direction=\"in\"/>"
    "<arg type=\"s\" name=\"StartupId\" direction=\"in\"/></method>\n"
    "    <method name=\"ShowItems\"><arg type=\"as\" name=\"URIs\" direction=\"in\"/>"
    "<arg type=\"s\" name=\"StartupId\" direction=\"in\"/></method>\n"
    "    <method name=\"ShowItemProperties\"><arg type=\"as\" name=\"URIs\" direction=\"in\"/>"
    "<arg type=\"s\" name=\"StartupId\" direction=\"in\"/></method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\"><arg type=\"s\" name=\"xml\" direction=\"out\"/></method>\n"
    "  </interface>\n"
    "</node>\n";

std::vector<std::string> strings_of(const dbus::Value &v) {
  std::vector<std::string> out;
  for (const auto &item : v.items)
    if (item.is_string() && !item.s.empty()) out.push_back(item.s);
  return out;
}

// What a call asked for, done after the reply is sent (it may open dialogs).
struct Request {
  enum Kind { Folders, Items, Properties, Quit } kind = Folders;
  std::vector<std::string> locations, select;
};

void perform(void *data) {
  std::unique_ptr<Request> r(static_cast<Request *>(data));
  App &app = App::get();
  switch (r->kind) {
    case Request::Quit:
      for (MainWindow *w : std::vector<MainWindow *>(app.windows)) w->hide();
      break;
    case Request::Folders:
      app.open_window(r->locations, r->select);
      break;
    case Request::Items:
      app.open_window({}, r->select);
      break;
    case Request::Properties: {
      std::vector<Item> items;
      for (const auto &p : r->select) {
        Item it;
        if (chooser::entry_for_path(p, it)) items.push_back(it);
      }
      if (items.empty()) break;
      MainWindow *w = app.active_window();
      if (!w) w = app.open_window({chooser::parent_dir(items[0].path)}, {items[0].path});
      show_properties(w, items);
      break;
    }
  }
}

DBusHandlerResult reply_ok(dbus::Bus *bus, DBusMessage *msg) {
  if (!dbus_message_get_no_reply(msg)) bus->send(dbus_message_new_method_return(msg));
  return DBUS_HANDLER_RESULT_HANDLED;
}

DBusHandlerResult introspect(dbus::Bus *bus, DBusMessage *msg, const char *xml) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  dbus::Writer(reply).str(xml);
  bus->send(reply);
  return DBUS_HANDLER_RESULT_HANDLED;
}

std::vector<std::string> paths_of(const std::vector<std::string> &uris) {
  std::vector<std::string> out;
  for (const auto &u : uris) {
    std::string loc = normalize_location(u);
    if (!loc.empty()) out.push_back(loc);
  }
  return out;
}

DBusHandlerResult on_call(dbus::Bus *bus, DBusMessage *msg) {
  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *iface = dbus_message_get_interface(msg);
  const char *member = dbus_message_get_member(msg);
  const char *path = dbus_message_get_path(msg);
  if (!member || !path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  bool fm = strcmp(path, kFmPath) == 0;
  std::string i = iface ? iface : "";
  std::string m = member;
  if (m == "Introspect" && (i.empty() || i == "org.freedesktop.DBus.Introspectable"))
    return introspect(bus, msg, fm ? kFmXml : kLiriaXml);
  std::vector<dbus::Value> args = dbus::read_args(msg);
  auto *r = new Request;
  if (fm && (i.empty() || i == kFm)) {
    std::vector<std::string> uris = args.empty() ? std::vector<std::string>() : strings_of(args[0]);
    if (m == "ShowFolders") {
      r->kind = Request::Folders;
      r->locations = paths_of(uris);
    } else if (m == "ShowItems") {
      r->kind = Request::Items;
      r->select = paths_of(uris);
    } else if (m == "ShowItemProperties") {
      r->kind = Request::Properties;
      r->select = paths_of(uris);
    } else {
      delete r;
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
  } else if (!fm && (i.empty() || i == kIface)) {
    if (m == "Open") {
      r->kind = Request::Folders;
      if (args.size() > 0) r->locations = paths_of(strings_of(args[0]));
      if (args.size() > 1) r->select = paths_of(strings_of(args[1]));
    } else if (m == "Quit") {
      r->kind = Request::Quit;
    } else {
      delete r;
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
  } else {
    delete r;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  reply_ok(bus, msg);
  Fl::add_timeout(0.0, perform, r);
  return DBUS_HANDLER_RESULT_HANDLED;
}

}  // namespace

std::string location_of_argument(const std::string &arg, const std::string &cwd) {
  if (arg.empty()) return "";
  if (arg[0] == '/' || arg[0] == '~') return normalize_location(chooser::expand_location(arg, cwd));
  std::string loc = normalize_location(arg);
  if (!loc.empty() && !is_local(loc)) return loc;
  // A path relative to the folder Liria was started in.
  std::string p = chooser::expand_location(arg, cwd);
  return p.empty() ? arg : p;
}

bool hand_off(const std::vector<std::string> &locations, const std::vector<std::string> &select) {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus || !bus->name_has_owner(kBusName)) return false;
  DBusMessage *m = dbus::method(kBusName, kPath, kIface, "Open");
  if (!m) return false;
  {
    dbus::Writer w(m);
    w.strings(locations);
    w.strings(select);
  }
  DBusMessage *reply = bus->call(m, 5000);
  bool ok = reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
  if (reply) dbus_message_unref(reply);
  return ok;
}

bool ask_running_to_quit() {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus || !bus->name_has_owner(kBusName)) return false;
  DBusMessage *m = dbus::method(kBusName, kPath, kIface, "Quit");
  DBusMessage *reply = m ? bus->call(m, 5000) : nullptr;
  bool ok = reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
  if (reply) dbus_message_unref(reply);
  return ok;
}

bool start_dbus_service() {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return true;  // no session bus: Liria works alone
  if (!bus->request_name(kBusName)) return false;
  bus->export_object(kPath, [bus](DBusMessage *msg) { return on_call(bus, msg); });
  bus->export_object(kFmPath, [bus](DBusMessage *msg) { return on_call(bus, msg); });
  // Browsers ask org.freedesktop.FileManager1 for "Show in folder"; another
  // file manager that has it keeps it.
  bus->request_name(kFm);
  return true;
}

}  // namespace liria
}  // namespace sde
