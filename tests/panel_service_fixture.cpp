// SPDX-License-Identifier: GPL-3.0-or-later
// Isolated QA services, never installed. All names/data are explicit fixtures.
#include <FL/Fl.H>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cairo.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "dbus.h"
using namespace sde;
namespace {
dbus::Bus* bus;
Display* display;
Window icon;
std::string log_path;
bool powered = true, discovering = false, paired = true, connected = false, adapter_present = true,
     device_present = true, control = true;
std::string playback = "Playing";
// MPRIS position (microseconds) and the album art written next to the log.
long long position = 42000000;
std::string cover;
const char* track = "/org/sde/qa/track/1";
// NetworkManager connectivity: 1 none, 2 portal, 3 limited, 4 full.
uint32_t connectivity = 4;
const std::string nm = "/org/freedesktop/NetworkManager";
const std::string adapter = "/org/bluez/hci0", device = adapter + "/dev_00_11_22_33_44_55",
                  player = "/org/mpris/MediaPlayer2";
void log(const std::string& s) { std::ofstream(log_path, std::ios::app) << s << '\n'; }
// SDE_FIXTURE_NAMES=showcase (tools/showcase.py): everyday sample names
// instead of the QA ones, and no XEmbed tray icon. The checks of
// panel-smoke use the QA names (the default).
bool showcase = false;
const char* pick(const char* qa, const char* sample) { return showcase ? sample : qa; }
std::string stick_label() { return pick("QA-STICK", "ALEX-USB"); }
void str(dbus::Writer& w, const char* k, const std::string& v, bool path = false) {
  dbus::Writer e(w.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(k);
  if (path)
    e.variant_path(v);
  else
    e.variant_str(v);
}
void flag(dbus::Writer& w, const char* k, bool v) {
  dbus::Writer e(w.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(k).variant_bool(v);
}
void adapter_props(dbus::Writer& w) {
  str(w, "Alias", pick("Bluetooth QA adapter", "alex-laptop"));
  flag(w, "Powered", powered);
  flag(w, "Discovering", discovering);
}
void device_props(dbus::Writer& w) {
  str(w, "Alias", pick("QA headphones", "Wireless Headphones"));
  str(w, "Address", "00:11:22:33:44:55");
  str(w, "Adapter", adapter, true);
  flag(w, "Connected", connected);
  flag(w, "Paired", paired);
  flag(w, "Trusted", false);
}
void changed(const std::string& path, const char* iface) {
  auto* m = dbus::signal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
  {
    dbus::Writer w(m);
    w.str(iface);
    { dbus::Writer d(w.iter(), DBUS_TYPE_ARRAY, "{sv}"); }
    w.strings({});
  }
  bus->send(m);
}
void nm_props(dbus::Writer& w) {
  {
    dbus::Writer e(w.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
    e.str("Connectivity").variant_u32(connectivity);
  }
  flag(w, "ConnectivityCheckEnabled", true);
  str(w, "ConnectivityCheckUri", "http://nmcheck.example.test/check_network_status.txt");
}
void nm_changed() {
  auto* m = dbus::signal(nm, "org.freedesktop.DBus.Properties", "PropertiesChanged");
  {
    dbus::Writer w(m);
    w.str("org.freedesktop.NetworkManager");
    {
      dbus::Writer d(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
      dbus::Writer e(d.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      e.str("Connectivity").variant_u32(connectivity);
    }
    w.strings({});
  }
  bus->send(m);
}

// ------------------------------------------------------------- UDisks2
// One USB stick "QA-STICK" (sdq, partition sdq1, vfat), unmounted until
// Mount; PowerOff removes it. Modes: drive-busy (Unmount fails with
// DeviceBusy), drive-remove, drive-insert.
const std::string ud = "/org/freedesktop/UDisks2", ud_drive = ud + "/drives/QA_Stick",
                  ud_disk = ud + "/block_devices/sdq", ud_part = ud + "/block_devices/sdq1";
bool drive_present = true, drive_busy = false;
std::string mount_point;
void bytes(dbus::Writer& w, const char* k, const std::string& v) {
  dbus::Writer e(w.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(k);
  dbus::Writer var(e.iter(), DBUS_TYPE_VARIANT, "ay");
  dbus::Writer a(var.iter(), DBUS_TYPE_ARRAY, "y");
  for (char c : v + std::string(1, '\0')) {
    unsigned char b = (unsigned char)c;
    dbus_message_iter_append_basic(a.iter(), DBUS_TYPE_BYTE, &b);
  }
}
void u64(dbus::Writer& w, const char* k, uint64_t v) {
  dbus::Writer e(w.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(k);
  dbus::Writer var(e.iter(), DBUS_TYPE_VARIANT, "t");
  dbus_message_iter_append_basic(var.iter(), DBUS_TYPE_UINT64, &v);
}
void block_props(dbus::Writer& w, const std::string& dev, uint64_t size, const std::string& label,
                 const std::string& type) {
  bytes(w, "Device", dev);
  bytes(w, "PreferredDevice", dev);
  str(w, "Drive", ud_drive, true);
  u64(w, "Size", size);
  str(w, "IdLabel", label);
  str(w, "IdType", type);
  str(w, "IdUsage", type.empty() ? "" : "filesystem");
  flag(w, "HintIgnore", false);
  flag(w, "HintSystem", false);
  flag(w, "HintAuto", true);
  str(w, "HintName", "");
  str(w, "CryptoBackingDevice", "/", true);
}
void iface_begin(dbus::Writer& interfaces, const char* name,
                 const std::function<void(dbus::Writer&)>& props) {
  dbus::Writer iface(interfaces.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  iface.str(name);
  dbus::Writer p(iface.iter(), DBUS_TYPE_ARRAY, "{sv}");
  props(p);
}
void ud_object(dbus::Writer& objects, const std::string& path) {
  dbus::Writer object(objects.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  object.path(path);
  dbus::Writer interfaces(object.iter(), DBUS_TYPE_ARRAY, "{sa{sv}}");
  if (path == ud_drive) {
    iface_begin(interfaces, "org.freedesktop.UDisks2.Drive", [](dbus::Writer& p) {
      str(p, "Vendor", pick("QA", "Generic"));
      str(p, "Model", pick("Flash Stick", "USB Flash Drive"));
      str(p, "ConnectionBus", "usb");
      str(p, "Media", "");
      u64(p, "Size", 8000000000ULL);
      flag(p, "Removable", true);
      flag(p, "MediaRemovable", true);
      flag(p, "Ejectable", false);
      flag(p, "CanPowerOff", true);
      dbus::Writer e(p.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      e.str("RotationRate").variant_i32(0);
    });
  } else if (path == ud_disk) {
    iface_begin(interfaces, "org.freedesktop.UDisks2.Block",
                [](dbus::Writer& p) { block_props(p, "/dev/sdq", 8000000000ULL, "", ""); });
    iface_begin(interfaces, "org.freedesktop.UDisks2.PartitionTable",
                [](dbus::Writer& p) { str(p, "Type", "dos"); });
  } else {
    iface_begin(interfaces, "org.freedesktop.UDisks2.Block",
                [](dbus::Writer& p) { block_props(p, "/dev/sdq1", 7990000000ULL, stick_label(), "vfat"); });
    iface_begin(interfaces, "org.freedesktop.UDisks2.Partition",
                [](dbus::Writer& p) { str(p, "Table", ud_disk, true); });
    iface_begin(interfaces, "org.freedesktop.UDisks2.Filesystem", [](dbus::Writer& p) {
      dbus::Writer e(p.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      e.str("MountPoints");
      dbus::Writer var(e.iter(), DBUS_TYPE_VARIANT, "aay");
      dbus::Writer a(var.iter(), DBUS_TYPE_ARRAY, "ay");
      if (!mount_point.empty()) {
        dbus::Writer m(a.iter(), DBUS_TYPE_ARRAY, "y");
        for (char c : mount_point + std::string(1, '\0')) {
          unsigned char b = (unsigned char)c;
          dbus_message_iter_append_basic(m.iter(), DBUS_TYPE_BYTE, &b);
        }
      }
    });
  }
}
void ud_signal(const char* member, const std::string& path) {
  auto* m = dbus::signal(ud, "org.freedesktop.DBus.ObjectManager", member);
  {
    dbus::Writer w(m);
    w.path(path);
    if (std::string(member) == "InterfacesAdded") {
      dbus::Writer interfaces(w.iter(), DBUS_TYPE_ARRAY, "{sa{sv}}");
    } else {
      w.strings({"org.freedesktop.UDisks2.Block"});
    }
  }
  bus->send(m);
}
void ud_changed() { changed(ud_part, "org.freedesktop.UDisks2.Filesystem"); }

// ------------------------------------------------------ AccountsService
// The account picture and real name (src/common/avatar.h): every user is
// "QA Avatar Tester" ("Alex Tester" in the showcase) until SetRealName;
// SetIconFile copies the picture next to the log, as accountsservice copies
// it to /var/lib/AccountsService.
const std::string accounts = "/org/freedesktop/Accounts", account_user = accounts + "/User4242";
std::string real_name = "QA Avatar Tester", icon_file, account_name;
void account_props(dbus::Writer& w) {
  str(w, "RealName", real_name);
  str(w, "IconFile", icon_file);
  str(w, "UserName", account_name);
  dbus::Writer e(w.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str("AccountType").variant_i32(0);
}
void set_icon(const std::string& file) {
  std::string dir = log_path.substr(0, log_path.rfind('/') + 1) + "accounts-icons";
  if (file.empty()) {
    if (!icon_file.empty()) unlink(icon_file.c_str());
    icon_file.clear();
    return;
  }
  mkdir(dir.c_str(), 0755);
  std::ifstream in(file, std::ios::binary);
  std::stringstream data;
  data << in.rdbuf();
  icon_file = dir + "/" + (account_name.empty() ? std::string("user") : account_name);
  std::ofstream(icon_file, std::ios::binary) << data.str();
}
DBusMessage* ud_error(DBusMessage* m, const char* name, const char* text) {
  return dbus_message_new_error(m, name, text);
}
DBusHandlerResult handle(DBusMessage* m) {
  if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const std::string path = dbus_message_get_path(m), method = dbus_message_get_member(m);
  auto args = dbus::read_args(m);
  auto* r = dbus_message_new_method_return(m);
  if (path == ud && method == "GetManagedObjects") {
    dbus::Writer w(r);
    dbus::Writer objects(w.iter(), DBUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    if (drive_present)
      for (const auto& p : {ud_drive, ud_disk, ud_part}) ud_object(objects, p);
  } else if (path == ud_part && method == "Mount") {
    log("UDISKS Mount");
    mount_point = "/tmp/sde-qa-media/" + stick_label();
    dbus::Writer(r).str(mount_point);
    ud_changed();
  } else if (path == ud_part && method == "Unmount") {
    log("UDISKS Unmount");
    if (drive_busy) {
      dbus_message_unref(r);
      r = ud_error(m, "org.freedesktop.UDisks2.Error.DeviceBusy", "target is busy");
    } else {
      mount_point.clear();
      ud_changed();
    }
  } else if (path == ud_drive && (method == "PowerOff" || method == "Eject")) {
    log("UDISKS " + method);
    drive_present = false;
    mount_point.clear();
    ud_signal("InterfacesRemoved", ud_part);
  } else if (path == nm && method == "GetAll") {
    dbus::Writer w(r);
    dbus::Writer props(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
    nm_props(props);
  } else if (path == nm && method == "CheckConnectivity") {
    log("NM CheckConnectivity");
    dbus::Writer(r).u32(connectivity);
  } else if (path == "/" && method == "GetManagedObjects") {
    dbus::Writer w(r);
    dbus::Writer objects(w.iter(), DBUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    if (adapter_present) {
      dbus::Writer object(objects.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      object.path(adapter);
      dbus::Writer interfaces(object.iter(), DBUS_TYPE_ARRAY, "{sa{sv}}");
      dbus::Writer iface(interfaces.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      iface.str("org.bluez.Adapter1");
      dbus::Writer props(iface.iter(), DBUS_TYPE_ARRAY, "{sv}");
      adapter_props(props);
    }
    if (adapter_present && device_present) {
      dbus::Writer object(objects.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      object.path(device);
      dbus::Writer interfaces(object.iter(), DBUS_TYPE_ARRAY, "{sa{sv}}");
      dbus::Writer iface(interfaces.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      iface.str("org.bluez.Device1");
      dbus::Writer props(iface.iter(), DBUS_TYPE_ARRAY, "{sv}");
      device_props(props);
    }
  } else if (path == player && method == "GetAll") {
    dbus::Writer w(r);
    dbus::Writer props(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
    if (!args.empty() && args[0].str() == "org.mpris.MediaPlayer2") {
      str(props, "Identity", pick("SDE Music · QA fixture", "Music"));
      str(props, "DesktopEntry", "sde-qa-music");
      flag(props, "CanRaise", true);
    } else {
      str(props, "PlaybackStatus", playback);
      flag(props, "CanControl", control);
      flag(props, "CanSeek", true);
      {
        dbus::Writer e(props.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("Position");
        dbus::Writer v(e.iter(), DBUS_TYPE_VARIANT, "x");
        dbus_int64_t pos = position;
        dbus_message_iter_append_basic(v.iter(), DBUS_TYPE_INT64, &pos);
      }
      flag(props, "CanPlay", true);
      flag(props, "CanPause", true);
      flag(props, "CanGoNext", true);
      flag(props, "CanGoPrevious", false);
      dbus::Writer entry(props.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      entry.str("Metadata");
      dbus::Writer v(entry.iter(), DBUS_TYPE_VARIANT, "a{sv}");
      dbus::Writer metadata(v.iter(), DBUS_TYPE_ARRAY, "{sv}");
      str(metadata, "xesam:title", pick("Alpha test track", "Morning Light"));
      str(metadata, "xesam:album", pick("QA Sessions", "Showcase Sessions"));
      str(metadata, "mpris:trackid", track, true);
      if (!cover.empty()) str(metadata, "mpris:artUrl", "file://" + cover);
      {
        dbus::Writer e(metadata.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("mpris:length");
        dbus::Writer v(e.iter(), DBUS_TYPE_VARIANT, "x");
        dbus_int64_t len = 187000000;
        dbus_message_iter_append_basic(v.iter(), DBUS_TYPE_INT64, &len);
      }
      dbus::Writer artist(metadata.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      artist.str("xesam:artist").variant_strings({pick("SDE QA", "Selviana Ensemble")});
    }
  } else if (path == player && method == "SetPosition" && args.size() == 2) {
    position = args[1].num();
    log("MPRIS SetPosition " + args[0].str() + " " + std::to_string(position / 1000000));
    changed(player, "org.mpris.MediaPlayer2.Player");
  } else if (path == player && (method == "PlayPause" || method == "Next" || method == "Previous" ||
                                method == "Raise")) {
    log("MPRIS " + method);
    if (method == "PlayPause") playback = playback == "Playing" ? "Paused" : "Playing";
    changed(player, "org.mpris.MediaPlayer2.Player");
  } else if (path == adapter && method == "Set" && args.size() == 3) {
    powered = args[2].boolean();
    if (!powered) discovering = false;
    log(std::string("BT Powered ") + (powered ? "on" : "off"));
    changed(adapter, "org.bluez.Adapter1");
  } else if (path == adapter && (method == "StartDiscovery" || method == "StopDiscovery")) {
    discovering = method == "StartDiscovery";
    log("BT " + method);
    changed(adapter, "org.bluez.Adapter1");
  } else if (path == adapter && method == "RemoveDevice") {
    paired = false;
    connected = false;
    log("BT RemoveDevice");
    changed(device, "org.bluez.Device1");
  } else if (path == device &&
             (method == "Pair" || method == "Connect" || method == "Disconnect")) {
    if (method == "Pair")
      paired = true;
    else
      connected = method == "Connect";
    log("BT " + method);
    changed(device, "org.bluez.Device1");
  } else if (path == accounts && method == "FindUserByName" && !args.empty()) {
    account_name = args[0].str();
    log("ACCOUNTS FindUserByName " + account_name);
    dbus::Writer(r).path(account_user);
  } else if (path == account_user && method == "GetAll") {
    dbus::Writer w(r);
    dbus::Writer props(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
    account_props(props);
  } else if (path == account_user && method == "SetIconFile" && !args.empty()) {
    log("ACCOUNTS SetIconFile " + args[0].str());
    set_icon(args[0].str());
    changed(account_user, "org.freedesktop.Accounts.User");
  } else if (path == account_user && method == "SetRealName" && !args.empty()) {
    log("ACCOUNTS SetRealName " + args[0].str());
    real_name = args[0].str();
    changed(account_user, "org.freedesktop.Accounts.User");
  } else if (path == "/SDETest" && method == "Mode" && !args.empty()) {
    auto mode = args[0].str();
    adapter_present = mode != "no-adapter";
    device_present = mode != "no-device";
    control = mode != "no-control";
    if (mode == "unpaired") paired = false;
    if (mode == "drive-busy") drive_busy = true;
    if (mode == "drive-free") drive_busy = false;
    if (mode == "drive-mounted") {
      mount_point = "/tmp/sde-qa-media/" + stick_label();
      ud_changed();
    }
    if (mode == "drive-remove" || mode == "drive-insert") {
      drive_present = mode == "drive-insert";
      mount_point.clear();
      ud_signal(drive_present ? "InterfacesAdded" : "InterfacesRemoved", ud_part);
    }
    if (mode.rfind("nm-", 0) == 0) {
      connectivity = mode == "nm-none" ? 1 : mode == "nm-portal" ? 2 : mode == "nm-limited" ? 3 : 4;
      nm_changed();
    }
    changed(adapter, "org.bluez.Adapter1");
    changed(player, "org.mpris.MediaPlayer2.Player");
    log("MODE " + mode);
  } else {
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  bus->send(r);
  return DBUS_HANDLER_RESULT_HANDLED;
}
void poll(void*) {
  while (XPending(display)) {
    XEvent e;
    XNextEvent(display, &e);
    if (e.type == ButtonPress) log("XEMBED click");
    if (e.type == Expose) {
      GC gc = XCreateGC(display, icon, 0, nullptr);
      XSetForeground(display, gc, 0x167d9a);
      XFillRectangle(display, icon, gc, 0, 0, 64, 64);
      XSetForeground(display, gc, 0xffffff);
      XDrawString(display, icon, gc, 3, 15, "Q", 1);
      XFreeGC(display, gc);
    }
  }
  XFlush(display);
  Fl::repeat_timeout(.02, poll);
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  log_path = argv[1];
  const char* names = getenv("SDE_FIXTURE_NAMES");
  showcase = names && std::string(names) == "showcase";
  real_name = pick("QA Avatar Tester", "Alex Tester");
  {
    // A small album cover next to the log.
    cover = log_path.substr(0, log_path.rfind('/') + 1) + "qa-cover.png";
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 96, 96);
    cairo_t* cr = cairo_create(s);
    cairo_pattern_t* g = cairo_pattern_create_linear(0, 0, 96, 96);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.93, 0.36, 0.44);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.33, 0.24, 0.78);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
    cairo_arc(cr, 48, 48, 26, 0, 6.2832);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.33, 0.24, 0.78);
    cairo_arc(cr, 48, 48, 6, 0, 6.2832);
    cairo_fill(cr);
    cairo_destroy(cr);
    if (cairo_surface_write_to_png(s, cover.c_str()) != CAIRO_STATUS_SUCCESS) cover.clear();
    cairo_surface_destroy(s);
  }
  bus = dbus::Bus::session();
  if (!bus || !bus->request_name("org.mpris.MediaPlayer2.sdeqa") || !bus->request_name("org.bluez") ||
      !bus->request_name("org.freedesktop.NetworkManager") ||
      !bus->request_name("org.freedesktop.UDisks2") || !bus->request_name("org.freedesktop.Accounts"))
    return 1;
  for (const auto& path : {std::string("/"), adapter, device, player, std::string("/SDETest"), nm,
                           ud, ud_drive, ud_disk, ud_part, accounts, account_user})
    bus->export_object(path, handle);
  display = XOpenDisplay(nullptr);
  if (!display) return 1;
  auto root = DefaultRootWindow(display);
  icon = XCreateSimpleWindow(display, root, 0, 0, 22, 22, 0, 0, 0x167d9a);
  XStoreName(display, icon, "SDE XEmbed fixture");
  XSelectInput(display, icon, ButtonPressMask | ExposureMask);
  Atom info = XInternAtom(display, "_XEMBED_INFO", False);
  unsigned long values[] = {0, 1};
  XChangeProperty(display, icon, info, info, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(values), 2);
  Window owner = XGetSelectionOwner(display, XInternAtom(display, "_NET_SYSTEM_TRAY_S0", False));
  if (!owner && !showcase) return 1;
  if (!showcase) {  // the showcase has no "Q" test icon in the tray
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = owner;
    event.xclient.message_type = XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);
    event.xclient.format = 32;
    event.xclient.data.l[1] = 0;
    event.xclient.data.l[2] = long(icon);
    XSendEvent(display, owner, False, NoEventMask, &event);
  }
  XFlush(display);
  log("READY");
  Fl::add_timeout(.02, poll);
  while (true) Fl::wait(.1);
}
