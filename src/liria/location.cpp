// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "location.h"

#include <glib.h>

#include <algorithm>
#include <map>

#include "file_chooser_model.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace liria {

const char *const kHome = "liria:home";
const char *const kTrash = "trash:///";
const char *const kNetwork = "network:///";

namespace {

std::string take(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

// The scheme of a URI ("smb"), or "".
std::string scheme_of(const std::string &s) {
  if (s.empty() || !g_ascii_isalpha(s[0])) return "";
  size_t i = 1;
  while (i < s.size() && (g_ascii_isalnum(s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) ++i;
  if (i >= s.size() || s[i] != ':') return "";
  return lower(s.substr(0, i));
}

std::string gvfs_base() {
  std::string runtime = env("XDG_RUNTIME_DIR");
  return runtime.empty() ? std::string() : runtime + "/gvfs";
}

// "smb-share:server=nas,share=docs" -> {"smb-share", {server: nas, share: docs}}.
bool split_gvfs(const std::string &name, std::string &kind, std::map<std::string, std::string> &kv) {
  size_t colon = name.find(':');
  if (colon == std::string::npos || colon == 0) return false;
  kind = name.substr(0, colon);
  for (const auto &pair : split(name.substr(colon + 1), ',')) {
    size_t eq = pair.find('=');
    if (eq == std::string::npos) continue;
    kv[pair.substr(0, eq)] = take(g_uri_unescape_string(pair.substr(eq + 1).c_str(), nullptr));
  }
  return true;
}

}  // namespace

bool is_local(const std::string &loc) { return !loc.empty() && loc[0] == '/'; }

bool is_special(const std::string &loc) { return loc == kHome || loc == kTrash || loc == kNetwork; }

bool is_remote(const std::string &loc) { return !is_local(loc) && !is_special(loc) && !scheme_of(loc).empty(); }

std::string normalize_location(const std::string &in) {
  std::string loc = trim(in);
  if (loc.empty()) return "";
  if (loc[0] == '/') return chooser::normalize_path(loc);
  std::string scheme = scheme_of(loc);
  if (scheme == "file") return uri_to_path(loc);
  if (scheme == "liria" || scheme == "home") return kHome;
  if (scheme == "trash") return kTrash;
  if (scheme == "network") return kNetwork;
  if (scheme.empty()) return "";
  return loc;
}

std::string parent_location(const std::string &loc) {
  if (is_local(loc)) return chooser::parent_dir(loc);
  if (!is_remote(loc)) return "";
  // scheme://authority/a/b/ -> scheme://authority/a/
  size_t start = loc.find("://");
  if (start == std::string::npos) return "";
  size_t root = loc.find('/', start + 3);
  if (root == std::string::npos) return "";
  std::string path = loc.substr(root);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  if (path == "/") return "";
  size_t slash = path.rfind('/');
  return loc.substr(0, root) + path.substr(0, slash + 1);
}

std::string path_to_uri(const std::string &path) {
  gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  return uri ? take(uri) : "file://" + path;
}

std::string uri_to_path(const std::string &uri) {
  if (uri.compare(0, 5, "file:") != 0) return "";
  gchar *p = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
  return p ? chooser::normalize_path(take(p)) : "";
}

std::string uri_basename(const std::string &uri) {
  std::string u = uri;
  size_t q = u.find_first_of("?#");
  if (q != std::string::npos) u.erase(q);
  while (!u.empty() && u.back() == '/') u.pop_back();
  size_t start = u.find("://");
  size_t slash = u.rfind('/');
  if (slash == std::string::npos || (start != std::string::npos && slash < start + 3)) {
    // Only the server: its name.
    std::string host = start == std::string::npos ? u : u.substr(start + 3);
    size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    return host;
  }
  return take(g_uri_unescape_string(u.substr(slash + 1).c_str(), nullptr));
}

std::string gvfs_label(const std::string &name) {
  std::string kind;
  std::map<std::string, std::string> kv;
  if (!split_gvfs(name, kind, kv)) return name;
  std::string host = kv.count("server") ? kv["server"] : kv.count("host") ? kv["host"] : "";
  if (host.empty()) return name;
  if (kv.count("share")) return format(_("%s on %s"), kv["share"].c_str(), host.c_str());
  if (kv.count("user")) return format(_("%s on %s"), kv["user"].c_str(), host.c_str());
  return host;
}

std::string gvfs_uri_of(const std::string &path, const std::string &base_in) {
  std::string base = base_in.empty() ? gvfs_base() : base_in;
  if (base.empty() || !starts_with(path, base + "/")) return "";
  std::string rest = path.substr(base.size() + 1);
  size_t slash = rest.find('/');
  std::string mount = rest.substr(0, slash);
  std::string tail = slash == std::string::npos ? "" : rest.substr(slash + 1);
  std::string kind;
  std::map<std::string, std::string> kv;
  if (!split_gvfs(mount, kind, kv)) return "";
  std::string scheme = kind;
  size_t dash = scheme.find('-');
  if (dash != std::string::npos) scheme = scheme.substr(0, dash);  // smb-share -> smb
  std::string host = kv.count("server") ? kv["server"] : kv.count("host") ? kv["host"] : "";
  if (host.empty()) return "";
  std::string uri = scheme + "://";
  if (kv.count("user")) uri += take(g_uri_escape_string(kv["user"].c_str(), "!$&'()*+,;=", TRUE)) + "@";
  uri += host.find(':') != std::string::npos ? "[" + host + "]" : host;
  if (kv.count("port")) uri += ":" + kv["port"];
  uri += "/";
  if (kv.count("share")) uri += take(g_uri_escape_string(kv["share"].c_str(), "!$&'()*+,;=:@", TRUE)) + "/";
  if (kv.count("prefix")) {
    std::string prefix = kv["prefix"];
    while (!prefix.empty() && prefix[0] == '/') prefix.erase(0, 1);
    if (!prefix.empty()) uri += take(g_uri_escape_string(prefix.c_str(), "!$&'()*+,;=:@/", TRUE)) + "/";
  }
  if (!tail.empty()) uri += take(g_uri_escape_string(tail.c_str(), "!$&'()*+,;=:@/", TRUE));
  return uri;
}

std::string location_title(const std::string &loc) {
  if (loc == kHome) return _("Home");
  if (loc == kTrash) return _("Trash");
  if (loc == kNetwork) return _("Network");
  if (is_local(loc)) {
    if (loc == "/") return _("File System");
    std::string base = gvfs_base();
    if (!base.empty() && starts_with(loc, base + "/") && loc.find('/', base.size() + 1) == std::string::npos)
      return gvfs_label(loc.substr(base.size() + 1));
    return take(g_filename_display_basename(loc.c_str()));
  }
  std::string name = uri_basename(loc);
  return name.empty() ? loc : name;
}

std::vector<std::string> location_icons(const std::string &loc) {
  if (loc == kHome) return {"user-home", "go-home", "folder"};
  if (loc == kTrash) return {"user-trash", "user-trash-full", "folder"};
  if (loc == kNetwork) return {"network-workgroup", "folder-remote", "network-server", "folder"};
  if (is_remote(loc)) return {"folder-remote", "network-server", "folder"};
  if (loc == "/") return {"drive-harddisk-system", "drive-harddisk", "folder"};
  std::string home = home_dir();
  if (loc == home) return {"user-home", "folder"};
  static const struct {
    const char *key, *icon;
  } dirs[] = {{"DESKTOP", "user-desktop"},       {"DOCUMENTS", "folder-documents"}, {"DOWNLOAD", "folder-download"},
              {"MUSIC", "folder-music"},         {"PICTURES", "folder-pictures"},   {"VIDEOS", "folder-videos"},
              {"TEMPLATES", "folder-templates"}, {"PUBLICSHARE", "folder-publicshare"}};
  for (const auto &d : dirs) {
    std::string p = chooser::normalize_path(user_dir(d.key));
    if (!p.empty() && p != home && p == loc) return {d.icon, "folder"};
  }
  std::string base = gvfs_base();
  if (!base.empty() && starts_with(loc, base + "/")) return {"folder-remote", "network-server", "folder"};
  return {"folder"};
}

std::vector<std::string> crumb_chain(const std::string &loc, const std::vector<std::string> &anchors) {
  std::vector<std::string> out;
  if (is_local(loc)) {
    std::string anchor = "/";
    std::string home = home_dir();
    if (!home.empty() && home != "/" && (loc == home || starts_with(loc, home + "/"))) anchor = home;
    for (const auto &a : anchors)
      if (a.size() > anchor.size() && (loc == a || starts_with(loc, a == "/" ? "/" : a + "/"))) anchor = a;
    std::string base = gvfs_base();
    if (!base.empty() && starts_with(loc, base + "/")) {
      size_t end = loc.find('/', base.size() + 1);
      std::string mount = end == std::string::npos ? loc : loc.substr(0, end);
      if (mount.size() > anchor.size()) anchor = mount;
    }
    for (const auto &p : chooser::path_chain(loc))
      if (p.size() >= anchor.size()) out.push_back(p);
    return out;
  }
  if (is_remote(loc)) {
    // scheme://server/ then each folder.
    size_t start = loc.find("://");
    size_t root = start == std::string::npos ? std::string::npos : loc.find('/', start + 3);
    if (root == std::string::npos) return {loc};
    std::string cur = loc.substr(0, root + 1);
    out.push_back(cur);
    std::string path = loc.substr(root + 1);
    for (const auto &part : split(path, '/')) {
      if (part.empty()) continue;
      cur += part + "/";
      out.push_back(cur);
    }
    return out;
  }
  return {loc};
}

std::string name_problem(const std::string &name) {
  if (trim(name).empty()) return _("Type a name.");
  if (name.find('/') != std::string::npos) return _("A name cannot contain “/”.");
  if (name.find('\0') != std::string::npos) return _("A name cannot contain that character.");
  if (name == "." || name == "..") return _("“.” and “..” are not names a file can have.");
  if (name.size() > 255) return _("The name is too long.");
  return "";
}

std::string home_relative(const std::string &path) {
  std::string home = home_dir();
  if (path == home) return "~";
  if (!home.empty() && home != "/" && starts_with(path, home + "/")) return "~" + path.substr(home.size());
  return path;
}

}  // namespace liria
}  // namespace sde
