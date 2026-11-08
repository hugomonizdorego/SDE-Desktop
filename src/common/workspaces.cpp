// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "workspaces.h"

#include <X11/Xatom.h>

#include <algorithm>
#include <cstring>

#include "config.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace workspaces {

namespace {

std::string xml_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

Atom atom(Display *dpy, const char *name) { return XInternAtom(dpy, name, False); }

std::vector<unsigned long> cardinals(Display *dpy, Window w, const char *prop, Atom type) {
  std::vector<unsigned long> out;
  Atom actual;
  int format;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy, w, atom(dpy, prop), 0, 4096, False, type, &actual, &format, &n, &after,
                         &data) == Success &&
      data) {
    if (format == 32) {
      auto *v = reinterpret_cast<unsigned long *>(data);
      out.assign(v, v + n);
    }
    XFree(data);
  }
  return out;
}

std::string utf8(Display *dpy, Window w, const char *prop) {
  std::string out;
  Atom actual;
  int format;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy, w, atom(dpy, prop), 0, 8192, False, atom(dpy, "UTF8_STRING"), &actual, &format, &n,
                         &after, &data) == Success &&
      data) {
    if (format == 8) out.assign(reinterpret_cast<char *>(data), n);
    XFree(data);
  }
  return out;
}

void root_message(Display *dpy, Window w, const char *type, long l0, long l1 = 0, long l2 = 0) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = w;
  e.xclient.message_type = atom(dpy, type);
  e.xclient.format = 32;
  e.xclient.data.l[0] = l0;
  e.xclient.data.l[1] = l1;
  e.xclient.data.l[2] = l2;
  XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

// The text between <tag> and </tag> after |from| and before |limit|.
bool find_element(const std::string &text, const std::string &tag, size_t from, size_t limit, size_t &a,
                  size_t &b) {
  a = text.find("<" + tag + ">", from);
  if (a == std::string::npos || a >= limit) return false;
  b = text.find("</" + tag + ">", a);
  if (b == std::string::npos || b > limit) return false;
  a += tag.size() + 2;
  return true;
}

}  // namespace

int clamp_count(int n) { return std::max(kMin, std::min(kMax, n)); }

std::string default_name(int index) { return format(_("Workspace %d"), index + 1); }

Settings Settings::from_config(const Config &cfg) {
  Settings s;
  s.count = clamp_count(cfg.get_int("workspaces", "count", kDefault));
  for (int i = 0; i < kMax; ++i) s.names[size_t(i)] = trim(cfg.get("workspaces", "name" + std::to_string(i + 1)));
  s.wrap = cfg.get_bool("workspaces", "wrap", false);
  s.taskbar_all = cfg.get_bool("workspaces", "taskbar_all", false);
  s.alt_tab_all = cfg.get_bool("workspaces", "alt_tab_all", false);
  return s;
}

void Settings::save(Config &cfg) const {
  cfg.set("workspaces", "count", std::to_string(clamp_count(count)));
  for (int i = 0; i < kMax; ++i) {
    std::string n = i < int(names.size()) ? trim(names[size_t(i)]) : "";
    // A name that is the default one stays empty, so that it follows the
    // language and the position.
    if (n == default_name(i)) n.clear();
    cfg.set("workspaces", "name" + std::to_string(i + 1), n);
  }
  cfg.set("workspaces", "wrap", wrap ? "true" : "false");
  cfg.set("workspaces", "taskbar_all", taskbar_all ? "true" : "false");
  cfg.set("workspaces", "alt_tab_all", alt_tab_all ? "true" : "false");
}

std::string Settings::name(int index) const {
  if (index >= 0 && index < int(names.size()) && !trim(names[size_t(index)]).empty())
    return trim(names[size_t(index)]);
  return default_name(index);
}

std::vector<std::string> Settings::shown_names() const {
  std::vector<std::string> out;
  for (int i = 0; i < clamp_count(count); ++i) out.push_back(name(i));
  return out;
}

bool sync_openbox_desktops(std::string &rc, const Settings &s) {
  const std::string before = rc;
  std::string names;
  for (const auto &n : s.shown_names()) names += "<name>" + xml_escape(n) + "</name>";
  const std::string number = std::to_string(clamp_count(s.count));
  size_t d = rc.find("<desktops>"), e = rc.find("</desktops>");
  if (d == std::string::npos || e == std::string::npos || e < d) {
    std::string block = "  <desktops>\n    <number>" + number + "</number>\n    <firstdesk>1</firstdesk>\n" +
                        "    <names>" + names + "</names>\n    <popupTime>600</popupTime>\n  </desktops>\n";
    size_t at = rc.find("  <resize>");
    if (at == std::string::npos) at = rc.find("</openbox_config>");
    if (at == std::string::npos) return false;
    rc.insert(at, block);
    return true;
  }
  size_t a, b;
  if (find_element(rc, "number", d, e, a, b)) {
    rc.replace(a, b - a, number);
  } else {
    rc.insert(d + 10, "\n    <number>" + number + "</number>");
  }
  e = rc.find("</desktops>", d);
  if (find_element(rc, "names", d, e, a, b)) {
    rc.replace(a, b - a, names);
  } else {
    rc.insert(e, "  <names>" + names + "</names>\n  ");
  }
  return rc != before;
}

Live read_live(Display *dpy) {
  Live l;
  if (!dpy) return l;
  Window root = DefaultRootWindow(dpy);
  auto n = cardinals(dpy, root, "_NET_NUMBER_OF_DESKTOPS", XA_CARDINAL);
  auto c = cardinals(dpy, root, "_NET_CURRENT_DESKTOP", XA_CARDINAL);
  if (!n.empty()) l.count = int(n[0]);
  if (!c.empty()) l.current = int(c[0]);
  l.names = split(utf8(dpy, root, "_NET_DESKTOP_NAMES"), '\0', true);
  if (l.names.size() > size_t(std::max(0, l.count))) l.names.resize(size_t(std::max(0, l.count)));
  return l;
}

void apply_live(Display *dpy, const Settings &s) {
  if (!dpy) return;
  Window root = DefaultRootWindow(dpy);
  int count = clamp_count(s.count);
  Live live = read_live(dpy);
  if (live.count != count) root_message(dpy, root, "_NET_NUMBER_OF_DESKTOPS", count);
  std::string value;
  for (const auto &n : s.shown_names()) {
    value += n;
    value.push_back('\0');
  }
  XChangeProperty(dpy, root, atom(dpy, "_NET_DESKTOP_NAMES"), atom(dpy, "UTF8_STRING"), 8, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(value.data()), int(value.size()));
  XSync(dpy, False);
}

std::vector<WindowInfo> windows(Display *dpy) {
  std::vector<WindowInfo> out;
  if (!dpy) return out;
  Window root = DefaultRootWindow(dpy);
  Atom hidden = atom(dpy, "_NET_WM_STATE_HIDDEN");
  for (unsigned long w : cardinals(dpy, root, "_NET_CLIENT_LIST", XA_WINDOW)) {
    WindowInfo i;
    i.win = Window(w);
    auto d = cardinals(dpy, i.win, "_NET_WM_DESKTOP", XA_CARDINAL);
    if (d.empty()) continue;
    i.desktop = d[0] == 0xFFFFFFFFul ? -1 : long(d[0]);
    for (unsigned long st : cardinals(dpy, i.win, "_NET_WM_STATE", XA_ATOM))
      if (Atom(st) == hidden) i.minimized = true;
    out.push_back(i);
  }
  return out;
}

void switch_to(Display *dpy, int index) {
  if (!dpy || index < 0) return;
  root_message(dpy, DefaultRootWindow(dpy), "_NET_CURRENT_DESKTOP", index, CurrentTime);
  XFlush(dpy);
}

void move_window(Display *dpy, Window w, long desktop) {
  if (!dpy || !w) return;
  root_message(dpy, w, "_NET_WM_DESKTOP", desktop < 0 ? long(0xFFFFFFFFul) : desktop, 2);
  XFlush(dpy);
}

long moved_desktop(long desktop, int removed, int count) {
  (void)count;
  if (desktop < 0 || desktop < removed) return desktop;
  if (desktop == removed) return removed > 0 ? removed - 1 : 0;
  return desktop - 1;
}

std::vector<std::string> names_after_close(const Settings &s, int removed) {
  std::vector<std::string> out = s.names;
  out.resize(size_t(kMax));
  if (removed >= 0 && removed < kMax) {
    out.erase(out.begin() + removed);
    out.push_back("");
  }
  return out;
}

int shown_after_close(int current, int removed, int count) {
  (void)count;
  if (current < removed) return current;
  if (current == removed) return removed > 0 ? removed - 1 : 0;
  return current - 1;
}

bool add(Display *dpy, Config &cfg) {
  Settings s = Settings::from_config(cfg);
  Live live = read_live(dpy);
  int count = live.count > 0 ? live.count : s.count;
  if (count >= kMax) return false;
  s.count = count + 1;
  s.save(cfg);
  apply_live(dpy, s);
  switch_to(dpy, s.count - 1);
  XSync(dpy, False);
  return true;
}

bool close(Display *dpy, Config &cfg, int index) {
  Settings s = Settings::from_config(cfg);
  Live live = read_live(dpy);
  int count = live.count > 0 ? live.count : s.count;
  if (count <= kMin) return false;
  int removed = index < 0 ? live.current : index;
  if (removed < 0 || removed >= count) return false;
  int shown = shown_after_close(live.current, removed, count);
  // Closing the workspace in view: show its left neighbour first, which
  // keeps its number, so that the windows arrive where one looks.
  if (removed == live.current && removed > 0) switch_to(dpy, removed - 1);
  for (const auto &w : windows(dpy)) {
    long to = moved_desktop(w.desktop, removed, count);
    if (to != w.desktop) move_window(dpy, w.win, to);
  }
  s.names = names_after_close(s, removed);
  s.count = count - 1;
  s.save(cfg);
  apply_live(dpy, s);
  switch_to(dpy, shown);
  XSync(dpy, False);
  return true;
}

}  // namespace workspaces
}  // namespace sde
