// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "openwith.h"

#include <gio/gio.h>

#include <algorithm>
#include <set>

#include "desktop_entry.h"
#include "file_chooser_model.h"
#include "location.h"
#include "spawn.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {

bool is_a(const std::string &mime, const std::string &super) {
  return mime == super || g_content_type_is_a(mime.c_str(), super.c_str());
}

std::vector<MimeAppsList> load_lists() {
  std::vector<MimeAppsList> lists;
  for (const auto &p : mimeapps_paths()) {
    MimeAppsList l;
    if (l.load(p)) lists.push_back(std::move(l));
  }
  return lists;
}

}  // namespace

const std::vector<MimeApp> &Apps::all() {
  if (!scanned_) rescan();
  return apps_;
}

void Apps::rescan() {
  apps_ = scan_mime_apps(application_dirs());
  scanned_ = true;
}

const MimeApp *Apps::find(const std::string &id) {
  for (const auto &a : all())
    if (a.id == id) return &a;
  return nullptr;
}

std::string Apps::default_for(const std::string &mime) {
  std::vector<MimeAppsList> lists = load_lists();
  std::string id = resolve_default(mime, lists, all());
  if (!id.empty()) return id;
  // A kind of text opens like text, and so on.
  for (const char *parent : {"text/plain", "application/xml", "application/zip"})
    if (mime != parent && is_a(mime, parent)) {
      id = resolve_default(parent, lists, all());
      if (!id.empty()) return id;
    }
  return "";
}

std::vector<const MimeApp *> Apps::for_type(const std::string &mime) {
  std::vector<const MimeApp *> exact, kind;
  for (const auto &a : all()) {
    if (a.hidden) continue;
    bool direct = false, related = false;
    for (const auto &m : a.mime_types) {
      if (m == mime) direct = true;
      else if (!m.empty() && m != "application/octet-stream" && is_a(mime, m)) related = true;
    }
    if (direct) exact.push_back(&a);
    else if (related) kind.push_back(&a);
  }
  auto by_name = [](const MimeApp *a, const MimeApp *b) { return a->name < b->name; };
  std::sort(exact.begin(), exact.end(), by_name);
  std::sort(kind.begin(), kind.end(), by_name);
  exact.insert(exact.end(), kind.begin(), kind.end());
  std::string def = default_for(mime);
  auto it = std::find_if(exact.begin(), exact.end(), [&](const MimeApp *a) { return a->id == def; });
  if (it != exact.end()) std::rotate(exact.begin(), it, it + 1);
  else if (const MimeApp *d = find(def)) exact.insert(exact.begin(), d);
  return exact;
}

bool Apps::set_default(const std::string &mime, const std::string &app_id, std::string *error) {
  std::string path = config_home() + "/mimeapps.list";
  MimeAppsList user;
  user.load(path);
  user.set_default(mime, app_id);
  make_dirs(config_home());
  if (!user.save(path)) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  return true;
}

bool Apps::launch(const std::string &app_id, const std::vector<std::string> &files) {
  DesktopEntry e;
  const MimeApp *a = find(app_id);
  bool ok = a ? e.load(a->path, a->id) : find_application(app_id, e);
  if (!ok) return false;
  return launch_entry(e, files);
}

bool forget_recent(const std::string &path) {
  std::string file = chooser::recent_file_path();
  std::string text;
  if (!read_file(file, text)) return false;
  GBookmarkFile *bf = g_bookmark_file_new();
  bool ok = g_bookmark_file_load_from_data(bf, text.data(), text.size(), nullptr);
  if (ok) {
    std::string uri = path_to_uri(path);
    ok = g_bookmark_file_remove_item(bf, uri.c_str(), nullptr);
    if (ok) ok = g_bookmark_file_to_file(bf, file.c_str(), nullptr);
  }
  g_bookmark_file_free(bf);
  return ok;
}

bool clear_recent() {
  std::string file = chooser::recent_file_path();
  if (!file_exists(file)) return true;
  GBookmarkFile *bf = g_bookmark_file_new();
  bool ok = g_bookmark_file_to_file(bf, file.c_str(), nullptr);
  g_bookmark_file_free(bf);
  return ok;
}

}  // namespace liria
}  // namespace sde
