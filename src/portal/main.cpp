// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// xdg-desktop-portal-sde: SDE's backend of the FileChooser portal
// (org.freedesktop.impl.portal.FileChooser: OpenFile, SaveFile, SaveFiles).
// xdg-desktop-portal hands it the file dialogs of sandboxed programs
// (Flatpak) and of programs that use the portal (Firefox, Chromium, GTK 4
// and Qt programs), so they show the SDE file chooser
// (src/common/file_chooser.h) like every SDE program. D-Bus starts it on
// demand (org.freedesktop.impl.portal.desktop.sde), data/portals/sde.portal
// declares it and sde-portals.conf chooses it for SDE sessions; it ends a
// minute after its last dialog closed.
//
// Options understood: accept_label, multiple, directory, filters,
// current_filter, choices (their defaults are returned; the dialog shows
// none), current_name, current_folder, current_file, files (SaveFiles) and
// the parent window "x11:<XID>" (the dialog is transient for it). modal is
// always true. Results: uris, current_filter, choices and writable.
// Request.Close on the request's handle closes its dialog.
#include <FL/Fl.H>
#include <X11/Xlib.h>
#include <dbus/dbus.h>
#include <glib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "dbus.h"
#include "file_chooser.h"
#include "file_chooser_model.h"
#include "i18n.h"
#include "icons.h"
#include "theme.h"
#include "util.h"

using namespace sde;

namespace {

const char *kName = "org.freedesktop.impl.portal.desktop.sde";
const char *kPath = "/org/freedesktop/portal/desktop";
const char *kFileChooser = "org.freedesktop.impl.portal.FileChooser";
const char *kRequest = "org.freedesktop.impl.portal.Request";
const char *kRequestPrefix = "/org/freedesktop/portal/desktop/request";

// A filter as the portal describes it: a name and (0: glob, 1: MIME type) rules.
struct PortalFilter {
  std::string name;
  std::vector<std::pair<uint32_t, std::string>> rules;
};

struct Choice {
  std::string id, value;
};

struct Request {
  DBusMessage *call = nullptr;  // referenced until the reply is sent
  std::string method, handle, app_id, parent, title;
  dbus::Value options;
  FileChooser *chooser = nullptr;  // while its dialog is open
  bool closed = false;             // Request.Close came first
};

dbus::Bus *g_bus = nullptr;
std::map<std::string, std::shared_ptr<Request>> g_requests;  // handle -> request
int g_open = 0;
bool g_quit = false;

std::string take_gstr(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

// An "ay" path (NUL-terminated by the portal).
std::string byte_path(const dbus::Value *v) {
  if (!v) return "";
  auto b = v->bytes();
  std::string s(b.begin(), b.end());
  size_t nul = s.find('\0');
  if (nul != std::string::npos) s.resize(nul);
  return s;
}

PortalFilter read_filter(const dbus::Value &v) {
  PortalFilter f;
  const dbus::Value &s = v.inner();
  if (s.items.size() < 2) return f;
  f.name = s.items[0].str();
  for (const auto &rule : s.items[1].inner().items)
    if (rule.items.size() >= 2) f.rules.push_back({uint32_t(rule.items[0].num()), rule.items[1].str()});
  return f;
}

std::vector<PortalFilter> read_filters(const dbus::Value *v) {
  std::vector<PortalFilter> out;
  if (!v) return out;
  for (const auto &item : v->inner().items) {
    PortalFilter f = read_filter(item);
    if (!f.name.empty() || !f.rules.empty()) out.push_back(f);
  }
  return out;
}

bool same_filter(const PortalFilter &a, const PortalFilter &b) { return a.name == b.name && a.rules == b.rules; }

void add_filter(FileChooser &chooser, const PortalFilter &f) {
  std::string patterns;
  std::vector<std::string> types;
  for (const auto &r : f.rules) {
    if (r.first == 0) patterns += (patterns.empty() ? "" : " ") + r.second;
    else types.push_back(r.second);
  }
  chooser.add_filter(f.name.empty() ? patterns : f.name, patterns, types);
}

std::string uri_of(const std::string &path) {
  gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  return uri ? take_gstr(uri) : "file://" + path;
}

void write_filter(DBusMessageIter *variant_parent, const PortalFilter &f) {
  dbus::Writer v(variant_parent, DBUS_TYPE_VARIANT, "(sa(us))");
  dbus::Writer st(v.iter(), DBUS_TYPE_STRUCT, nullptr);
  st.str(f.name);
  dbus::Writer rules(st.iter(), DBUS_TYPE_ARRAY, "(us)");
  for (const auto &r : f.rules) {
    dbus::Writer rule(rules.iter(), DBUS_TYPE_STRUCT, nullptr);
    rule.u32(r.first).str(r.second);
  }
}

void send_reply(Request &r, uint32_t response, const std::vector<std::string> &uris,
                const PortalFilter *filter, const std::vector<Choice> &choices, bool writable) {
  if (!r.call) return;
  DBusMessage *reply = dbus_message_new_method_return(r.call);
  {
    dbus::Writer w(reply);
    w.u32(response);
    dbus::Writer results(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
    if (response == 0) {
      {
        dbus::Writer e(results.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("uris").variant_strings(uris);
      }
      if (filter) {
        dbus::Writer e(results.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("current_filter");
        write_filter(e.iter(), *filter);
      }
      if (!choices.empty()) {
        dbus::Writer e(results.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("choices");
        dbus::Writer v(e.iter(), DBUS_TYPE_VARIANT, "a(ss)");
        dbus::Writer list(v.iter(), DBUS_TYPE_ARRAY, "(ss)");
        for (const auto &c : choices) {
          dbus::Writer pair(list.iter(), DBUS_TYPE_STRUCT, nullptr);
          pair.str(c.id).str(c.value);
        }
      }
      if (writable) {
        dbus::Writer e(results.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("writable").variant_bool(true);
      }
    }
  }
  g_bus->send(reply);
  dbus_message_unref(r.call);
  r.call = nullptr;
}

// Quits a minute after the last dialog closed (D-Bus starts it again).
void idle_quit(void *) {
  if (g_open == 0 && g_requests.empty()) g_quit = true;
}

void schedule_idle_quit() {
  Fl::remove_timeout(idle_quit);
  const char *t = getenv("SDE_PORTAL_IDLE_SECONDS");
  Fl::add_timeout(t && *t ? atof(t) : 60.0, idle_quit);
}

// The theme of the SDE configuration as it is now.
void load_theme() {
  Config cfg;
  cfg.load();
  Theme::from_config(cfg).apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
}

void run_request(void *data) {
  auto *holder = static_cast<std::shared_ptr<Request> *>(data);
  std::shared_ptr<Request> r = *holder;
  delete holder;
  if (r->closed) {
    send_reply(*r, 2, {}, nullptr, {}, false);
    g_requests.erase(r->handle);
    schedule_idle_quit();
    return;
  }
  const dbus::Value &o = r->options;
  bool save = r->method == "SaveFile", save_many = r->method == "SaveFiles";
  bool directory = o.get("directory") && o.get("directory")->boolean();
  bool multiple = o.get("multiple") && o.get("multiple")->boolean();
  FileChooser::Mode mode = save ? FileChooser::Mode::Save
                           : save_many ? FileChooser::Mode::Folder
                           : directory ? (multiple ? FileChooser::Mode::Folders : FileChooser::Mode::Folder)
                           : multiple ? FileChooser::Mode::OpenMultiple
                                      : FileChooser::Mode::Open;
  load_theme();
  FileChooser chooser(mode);
  if (!r->title.empty()) chooser.title(r->title);
  if (const dbus::Value *v = o.get("accept_label")) {
    // GTK mnemonics ("_Open") are not shown.
    std::string label;
    for (char c : v->str())
      if (c != '_') label += c;
    if (!label.empty()) chooser.accept_label(label);
  }
  std::vector<PortalFilter> filters = save_many ? std::vector<PortalFilter>() : read_filters(o.get("filters"));
  PortalFilter current;
  bool has_current = false;
  if (const dbus::Value *v = o.get("current_filter")) {
    current = read_filter(*v);
    has_current = !current.rules.empty();
  }
  if (has_current && filters.empty()) filters.push_back(current);
  int first = -1;
  for (size_t i = 0; i < filters.size(); ++i) {
    add_filter(chooser, filters[i]);
    if (has_current && first < 0 && same_filter(filters[i], current)) first = int(i);
  }
  if (first >= 0) chooser.filter_index(first);
  if (!filters.empty() && !save) chooser.all_files(false);  // the program lists what it wants
  std::string folder = byte_path(o.get("current_folder"));
  std::string file = byte_path(o.get("current_file"));
  if (save && !file.empty()) {
    chooser.file_name(file);  // a path: its folder and name
  } else if (save) {
    if (const dbus::Value *v = o.get("current_name")) chooser.file_name(v->str());
  }
  if (!folder.empty()) chooser.folder(folder);
  else if (save || save_many) chooser.default_folder(user_dir("DOCUMENTS"));
  if (starts_with(r->parent, "x11:")) chooser.parent_xid(strtoul(r->parent.c_str() + 4, nullptr, 16));
  std::string app = r->app_id.empty() ? std::string("xdg-desktop-portal") : r->app_id;
  chooser.program(app);
  std::vector<Choice> choices;
  if (const dbus::Value *v = o.get("choices"))
    for (const auto &c : v->inner().items)
      if (c.items.size() >= 4) choices.push_back({c.items[0].str(), c.items[3].str()});
  r->chooser = &chooser;
  ++g_open;
  bool ok = chooser.run();
  --g_open;
  r->chooser = nullptr;
  if (!ok) {
    send_reply(*r, r->closed ? 2 : 1, {}, nullptr, {}, false);  // 1: the user cancelled
  } else {
    std::vector<std::string> uris;
    if (save_many) {
      // SaveFiles: the program's names in the chosen folder.
      std::string dir = chooser.path();
      if (const dbus::Value *files = o.get("files"))
        for (const auto &f : files->inner().items) {
          std::string name = basename_of(byte_path(&f));
          if (!name.empty()) uris.push_back(uri_of(dir + "/" + name));
        }
    } else {
      for (const auto &p : chooser.paths()) uris.push_back(uri_of(p));
    }
    int fi = chooser.filter_index();
    const PortalFilter *used = fi >= 0 && fi < int(filters.size()) ? &filters[size_t(fi)] : nullptr;
    send_reply(*r, 0, uris, used, choices, r->method == "OpenFile");
  }
  g_requests.erase(r->handle);
  schedule_idle_quit();
}

DBusHandlerResult on_file_chooser(DBusMessage *msg) {
  const char *iface = dbus_message_get_interface(msg);
  const char *member = dbus_message_get_member(msg);
  if (!member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  if (iface && strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0 && strcmp(member, "Introspect") == 0) {
    static const char *xml =
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
        "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node><interface name=\"org.freedesktop.impl.portal.FileChooser\">"
        "<method name=\"OpenFile\"><arg type=\"o\" name=\"handle\" direction=\"in\"/>"
        "<arg type=\"s\" name=\"app_id\" direction=\"in\"/><arg type=\"s\" name=\"parent_window\" direction=\"in\"/>"
        "<arg type=\"s\" name=\"title\" direction=\"in\"/><arg type=\"a{sv}\" name=\"options\" direction=\"in\"/>"
        "<arg type=\"u\" name=\"response\" direction=\"out\"/><arg type=\"a{sv}\" name=\"results\" direction=\"out\"/>"
        "</method>"
        "<method name=\"SaveFile\"><arg type=\"o\" name=\"handle\" direction=\"in\"/>"
        "<arg type=\"s\" name=\"app_id\" direction=\"in\"/><arg type=\"s\" name=\"parent_window\" direction=\"in\"/>"
        "<arg type=\"s\" name=\"title\" direction=\"in\"/><arg type=\"a{sv}\" name=\"options\" direction=\"in\"/>"
        "<arg type=\"u\" name=\"response\" direction=\"out\"/><arg type=\"a{sv}\" name=\"results\" direction=\"out\"/>"
        "</method>"
        "<method name=\"SaveFiles\"><arg type=\"o\" name=\"handle\" direction=\"in\"/>"
        "<arg type=\"s\" name=\"app_id\" direction=\"in\"/><arg type=\"s\" name=\"parent_window\" direction=\"in\"/>"
        "<arg type=\"s\" name=\"title\" direction=\"in\"/><arg type=\"a{sv}\" name=\"options\" direction=\"in\"/>"
        "<arg type=\"u\" name=\"response\" direction=\"out\"/><arg type=\"a{sv}\" name=\"results\" direction=\"out\"/>"
        "</method></interface></node>\n";
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus::Writer(reply).str(xml);
    g_bus->send(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (!iface || strcmp(iface, kFileChooser) != 0) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  if (strcmp(member, "OpenFile") != 0 && strcmp(member, "SaveFile") != 0 && strcmp(member, "SaveFiles") != 0)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  auto args = dbus::read_args(msg);
  if (args.size() < 5) {
    g_bus->send(dbus::Bus::error(msg, DBUS_ERROR_INVALID_ARGS, "expected (o handle, s app_id, s parent, s title, "
                                                               "a{sv} options)"));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  auto r = std::make_shared<Request>();
  r->call = dbus_message_ref(msg);
  r->method = member;
  r->handle = args[0].str();
  r->app_id = args[1].str();
  r->parent = args[2].str();
  r->title = args[3].str();
  r->options = args[4];
  Fl::remove_timeout(idle_quit);
  g_requests[r->handle] = r;
  // Shown from the event loop, not inside libdbus's dispatch: the dialog
  // runs its own loop, and further calls (Close) must get through.
  Fl::add_timeout(0, run_request, new std::shared_ptr<Request>(r));
  return DBUS_HANDLER_RESULT_HANDLED;
}

// org.freedesktop.impl.portal.Request.Close on a request's handle.
DBusHandlerResult on_request(DBusConnection *, DBusMessage *msg, void *) {
  if (!dbus_message_is_method_call(msg, kRequest, "Close")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *path = dbus_message_get_path(msg);
  auto it = path ? g_requests.find(path) : g_requests.end();
  if (it != g_requests.end()) {
    Request &r = *it->second;
    r.closed = true;
    if (r.chooser) r.chooser->close();
  }
  g_bus->send(dbus_message_new_method_return(msg));
  return DBUS_HANDLER_RESULT_HANDLED;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") {
      AppInfo info;
      info.name = "xdg-desktop-portal-sde";
      info.program = "xdg-desktop-portal-sde";
      info.component = "sde";
      info.version = SDE_VERSION;
      print_version(info);
      return 0;
    }
    if (a == "--help" || a == "-h") {
      printf("Usage: xdg-desktop-portal-sde [--replace]\n"
             "The SDE file chooser for xdg-desktop-portal (org.freedesktop.impl.portal.FileChooser).\n"
             "D-Bus starts it when a program asks the portal for a file dialog.\n");
      return 0;
    }
  }
  bool replace = argc > 1 && strcmp(argv[1], "--replace") == 0;
  if (!getenv("DISPLAY") || !*getenv("DISPLAY")) {
    fprintf(stderr, "xdg-desktop-portal-sde: no X display (DISPLAY is not set)\n");
    return 1;
  }
  g_bus = dbus::Bus::session();
  if (!g_bus) return 1;
  g_bus->export_object(kPath, on_file_chooser);
  static DBusObjectPathVTable requests = {nullptr, on_request, nullptr, nullptr, nullptr, nullptr};
  dbus_connection_register_fallback(g_bus->conn(), kRequestPrefix, &requests, nullptr);
  if (!g_bus->request_name(kName, replace)) {
    fprintf(stderr, "xdg-desktop-portal-sde: %s is taken\n", kName);
    return 1;
  }
  load_theme();
  schedule_idle_quit();
  while (!g_quit) Fl::wait(1e20);
  return 0;
}
