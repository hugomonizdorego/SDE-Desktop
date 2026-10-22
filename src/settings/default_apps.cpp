// SPDX-License-Identifier: GPL-3.0-or-later
#include "default_apps.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "desktop_entry.h"
#include "util.h"

namespace sde {

// "N_" strings: translated where they are shown.
const std::vector<AppCategory> &app_categories() {
  static const std::vector<AppCategory> list = {
      {"browser",
       "Web browser",
       {"web-browser", "applications-internet", "internet-web-browser"},
       {"x-scheme-handler/http", "x-scheme-handler/https", "text/html", "application/xhtml+xml",
        "application/x-extension-htm", "application/x-extension-html", "application/x-extension-xhtml"},
       {"x-scheme-handler/http", "x-scheme-handler/https"},
       ""},
      {"mail",
       "Mail",
       {"mail-client", "internet-mail", "emblem-mail", "mail-message-new"},
       {"x-scheme-handler/mailto", "message/rfc822"},
       {"x-scheme-handler/mailto"},
       ""},
      {"music",
       "Music",
       {"multimedia-audio-player", "audio-x-generic", "applications-multimedia"},
       {"audio/mpeg", "audio/mp4", "audio/x-m4a", "audio/flac", "audio/x-flac", "audio/ogg",
        "audio/x-vorbis+ogg", "audio/vorbis", "audio/opus", "audio/x-opus+ogg", "audio/wav", "audio/x-wav",
        "audio/aac", "audio/x-aac", "audio/x-ms-wma", "audio/webm", "audio/x-mpegurl", "audio/x-scpls",
        "audio/x-matroska"},
       {},
       ""},
      {"video",
       "Video",
       {"multimedia-video-player", "video-x-generic", "applications-multimedia"},
       {"video/mp4", "video/x-matroska", "video/webm", "video/mpeg", "video/x-msvideo", "video/quicktime",
        "video/ogg", "video/x-flv", "video/3gpp", "video/x-ms-wmv", "video/mp2t", "video/x-ogm+ogg"},
       {},
       ""},
      {"image",
       "Image viewer",
       {"image-viewer", "image-x-generic", "applications-graphics"},
       {"image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp", "image/tiff", "image/svg+xml",
        "image/avif", "image/heif", "image/x-icon", "image/vnd.microsoft.icon", "image/x-portable-pixmap"},
       {},
       ""},
      {"text",
       "Text editor",
       {"accessories-text-editor", "text-editor", "text-x-generic"},
       {"text/plain", "text/markdown", "text/x-markdown", "text/x-log", "text/x-readme"},
       {"text/plain"},
       ""},
      {"document",
       "Office documents",
       {"x-office-document", "libreoffice-writer", "application-vnd.oasis.opendocument.text"},
       {"application/vnd.oasis.opendocument.text", "application/msword",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "application/rtf",
        "text/rtf", "application/vnd.oasis.opendocument.text-template"},
       {},
       ""},
      {"spreadsheet",
       "Spreadsheet",
       {"x-office-spreadsheet", "libreoffice-calc", "application-vnd.oasis.opendocument.spreadsheet"},
       {"application/vnd.oasis.opendocument.spreadsheet", "application/vnd.ms-excel",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "text/csv",
        "application/vnd.oasis.opendocument.spreadsheet-template"},
       {"application/vnd.oasis.opendocument.spreadsheet", "application/vnd.ms-excel",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
       ""},
      {"presentation",
       "Presentation",
       {"x-office-presentation", "libreoffice-impress", "application-vnd.oasis.opendocument.presentation"},
       {"application/vnd.oasis.opendocument.presentation", "application/vnd.ms-powerpoint",
        "application/vnd.openxmlformats-officedocument.presentationml.presentation",
        "application/vnd.oasis.opendocument.presentation-template"},
       {},
       ""},
      {"pdf",
       "PDF documents",
       {"application-pdf", "x-office-document", "document-viewer"},
       {"application/pdf", "application/x-pdf", "application/postscript"},
       {"application/pdf"},
       ""},
      {"archive",
       "Archives",
       {"package-x-generic", "application-x-archive", "file-roller"},
       {"application/zip", "application/x-tar", "application/gzip", "application/x-gzip",
        "application/x-compressed-tar", "application/x-bzip2", "application/x-bzip-compressed-tar",
        "application/x-xz", "application/x-xz-compressed-tar", "application/x-7z-compressed", "application/vnd.rar",
        "application/x-rar", "application/zstd", "application/x-zstd-compressed-tar"},
       {"application/zip", "application/x-tar", "application/x-7z-compressed", "application/vnd.rar",
        "application/x-compressed-tar"},
       ""},
      {"files",
       "File manager",
       {"system-file-manager", "folder", "inode-directory"},
       {"inode/directory"},
       {},
       ""},
      {"terminal",
       "Terminal",
       {"utilities-terminal", "terminal", "org.gnome.Terminal"},
       {},
       {},
       "TerminalEmulator"},
      {"calendar",
       "Calendar",
       {"x-office-calendar", "office-calendar", "evolution-calendar"},
       {"text/calendar", "x-scheme-handler/webcal"},
       {"text/calendar"},
       ""},
  };
  return list;
}

const AppCategory *find_category(const std::string &id) {
  for (auto &c : app_categories())
    if (c.id == id) return &c;
  return nullptr;
}

bool MimeApp::supports(const std::string &mime) const {
  return std::find(mime_types.begin(), mime_types.end(), mime) != mime_types.end();
}

std::vector<std::string> application_dirs() {
  std::vector<std::string> dirs;
  for (auto &d : data_dirs()) dirs.push_back(d + "/applications");
  // Flatpak exports (normally in $XDG_DATA_DIRS through /etc/profile.d).
  for (std::string d : {data_home() + "/flatpak/exports/share/applications",
                        std::string("/var/lib/flatpak/exports/share/applications")})
    if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
  return dirs;
}

namespace {

void scan_dir(const std::string &dir, const std::string &prefix, std::map<std::string, std::string> &found) {
  for (auto &name : list_dir(dir)) {
    std::string full = dir + "/" + name;
    if (dir_exists(full)) scan_dir(full, prefix + name + "-", found);
    else if (ends_with(name, ".desktop") && !found.count(prefix + name)) found[prefix + name] = full;
  }
}

}  // namespace

std::vector<MimeApp> scan_mime_apps(const std::vector<std::string> &dirs) {
  std::map<std::string, std::string> found;
  for (auto &d : dirs) scan_dir(d, "", found);
  std::vector<MimeApp> out;
  for (auto &kv : found) {
    DesktopEntry e;
    if (!e.load(kv.second, kv.first)) continue;
    // Hidden=true deletes the entry (and masks the system one).
    if (e.hidden || e.type != "Application" || e.name.empty() || e.exec.empty()) continue;
    if (!e.only_show_in.empty() || !e.not_show_in.empty() || !e.try_exec.empty())
      if (!e.visible_in("SDE")) continue;
    MimeApp a;
    a.id = kv.first;
    a.name = e.name;
    a.icon = e.icon;
    a.path = kv.second;
    a.mime_types = e.mime_types;
    a.categories = e.categories;
    a.hidden = e.no_display;
    out.push_back(std::move(a));
  }
  std::sort(out.begin(), out.end(), [](const MimeApp &a, const MimeApp &b) {
    std::string la = lower(a.name), lb = lower(b.name);
    return la != lb ? la < lb : a.id < b.id;
  });
  return out;
}

std::vector<const MimeApp *> apps_for_category(const AppCategory &cat, const std::vector<MimeApp> &apps) {
  std::vector<const MimeApp *> out;
  const std::vector<std::string> &match = cat.match.empty() ? cat.types : cat.match;
  for (auto &a : apps) {
    if (a.hidden) continue;
    bool ok = false;
    if (!cat.desktop_category.empty())
      ok = std::find(a.categories.begin(), a.categories.end(), cat.desktop_category) != a.categories.end();
    for (auto &t : match)
      if (a.supports(t)) ok = true;
    if (ok) out.push_back(&a);
  }
  return out;
}

std::vector<const MimeApp *> apps_for_type(const std::string &mime, const std::vector<MimeApp> &apps) {
  std::vector<const MimeApp *> out;
  for (auto &a : apps)
    if (!a.hidden && a.supports(mime)) out.push_back(&a);
  return out;
}

std::vector<std::string> known_mime_types(const std::vector<MimeApp> &apps) {
  std::set<std::string> all;
  for (auto &a : apps)
    for (auto &t : a.mime_types)
      if (t.find('/') != std::string::npos) all.insert(t);
  return std::vector<std::string>(all.begin(), all.end());
}

// ------------------------------------------------------------ mimeapps.list

void MimeAppsList::parse(const std::string &text) {
  lines_.clear();
  changed_ = false;
  std::string section;
  std::istringstream in(text);
  std::string raw;
  while (std::getline(in, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    Line l;
    l.text = raw;
    std::string t = trim(raw);
    if (!t.empty() && t[0] == '[' && t.back() == ']') {
      section = t.substr(1, t.size() - 2);
      l.header = true;
    } else if (!t.empty() && t[0] != '#') {
      size_t eq = t.find('=');
      if (eq != std::string::npos) l.key = trim(t.substr(0, eq));
    }
    l.section = section;
    lines_.push_back(l);
  }
}

std::string MimeAppsList::format() const {
  std::string out;
  for (auto &l : lines_) out += l.text + "\n";
  return out;
}

bool MimeAppsList::load(const std::string &path) {
  std::string text;
  if (!read_file(path, text)) {
    parse("");
    return false;
  }
  parse(text);
  return true;
}

bool MimeAppsList::save(const std::string &path) const { return write_file_atomic(path, format()); }

std::vector<std::string> MimeAppsList::get(const std::string &section, const std::string &mime) const {
  for (auto &l : lines_)
    if (!l.header && l.section == section && l.key == mime) {
      std::string v = l.text.substr(l.text.find('=') + 1);
      std::vector<std::string> out;
      for (auto &p : split(v, ';')) {
        std::string t = trim(p);
        if (!t.empty()) out.push_back(t);
      }
      return out;
    }
  return {};
}

bool MimeAppsList::has(const std::string &section, const std::string &mime) const {
  for (auto &l : lines_)
    if (!l.header && l.section == section && l.key == mime) return true;
  return false;
}

void MimeAppsList::remove_key(const std::string &section, const std::string &mime) {
  for (size_t i = 0; i < lines_.size(); ++i)
    if (!lines_[i].header && lines_[i].section == section && lines_[i].key == mime) {
      lines_.erase(lines_.begin() + long(i));
      changed_ = true;
      return;
    }
}

void MimeAppsList::set_list(const std::string &section, const std::string &mime,
                            const std::vector<std::string> &apps) {
  std::string text = mime + "=";
  for (auto &a : apps) text += a + ";";
  for (auto &l : lines_)
    if (!l.header && l.section == section && l.key == mime) {
      if (l.text != text) changed_ = true;
      l.text = text;
      return;
    }
  changed_ = true;
  Line nl;
  nl.text = text;
  nl.key = mime;
  nl.section = section;
  // After the last key of the section (comments and blank lines that
  // follow it stay where they are).
  long header = -1, last = -1;
  for (size_t i = 0; i < lines_.size(); ++i) {
    if (lines_[i].header && lines_[i].section == section && header < 0) header = long(i);
    if (lines_[i].section == section && !lines_[i].key.empty()) last = long(i);
  }
  if (header < 0) {
    if (!lines_.empty() && !trim(lines_.back().text).empty()) lines_.push_back(Line{"", "", lines_.back().section});
    Line h;
    h.text = "[" + section + "]";
    h.section = section;
    h.header = true;
    lines_.push_back(h);
    lines_.push_back(nl);
    return;
  }
  long at = (last >= 0 ? last : header) + 1;
  lines_.insert(lines_.begin() + at, nl);
}

void MimeAppsList::set_default(const std::string &mime, const std::string &app) {
  set_list("Default Applications", mime, {app});
  std::vector<std::string> removed = get("Removed Associations", mime);
  auto it = std::find(removed.begin(), removed.end(), app);
  if (it != removed.end()) {
    removed.erase(it);
    if (removed.empty()) remove_key("Removed Associations", mime);
    else set_list("Removed Associations", mime, removed);
  }
}

void MimeAppsList::unset_default(const std::string &mime) { remove_key("Default Applications", mime); }

// ------------------------------------------------------------ lookup

std::vector<std::string> mimeapps_paths(const std::string &config_home, const std::vector<std::string> &config_dirs,
                                        const std::vector<std::string> &data_dirs, const std::string &desktops) {
  std::vector<std::string> names;
  for (auto &d : split(desktops, ':'))
    if (!trim(d).empty()) names.push_back(lower(trim(d)) + "-mimeapps.list");
  names.push_back("mimeapps.list");
  std::vector<std::string> out;
  auto add_dir = [&](const std::string &dir) {
    if (dir.empty()) return;
    for (auto &n : names) {
      std::string p = dir + "/" + n;
      if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
  };
  add_dir(config_home);
  for (auto &d : config_dirs) add_dir(d);
  for (auto &d : data_dirs) add_dir(d + "/applications");
  return out;
}

std::vector<std::string> mimeapps_paths() {
  return mimeapps_paths(config_home(), config_dirs(), data_dirs(), env("XDG_CURRENT_DESKTOP", "SDE"));
}

std::string resolve_default(const std::string &mime, const std::vector<MimeAppsList> &lists,
                            const std::vector<MimeApp> &apps) {
  std::set<std::string> installed;
  for (auto &a : apps) installed.insert(a.id);
  std::set<std::string> removed;
  // Removed Associations of a file apply to it and to every file after it.
  std::vector<std::set<std::string>> removed_upto;
  for (auto &l : lists) {
    for (auto &r : l.get("Removed Associations", mime)) removed.insert(r);
    removed_upto.push_back(removed);
  }
  for (size_t i = 0; i < lists.size(); ++i)
    for (auto &id : lists[i].get("Default Applications", mime))
      if (installed.count(id) && !removed_upto[i].count(id)) return id;
  for (size_t i = 0; i < lists.size(); ++i)
    for (auto &id : lists[i].get("Added Associations", mime))
      if (installed.count(id) && !removed_upto[i].count(id)) return id;
  for (auto &a : apps)
    if (a.supports(mime) && !removed.count(a.id)) return a.id;
  return "";
}

void apply_category(MimeAppsList &user, const AppCategory &cat, const MimeApp &app) {
  for (size_t i = 0; i < cat.types.size(); ++i) {
    const std::string &t = cat.types[i];
    if (i == 0 || app.supports(t)) user.set_default(t, app.id);
    else user.unset_default(t);
  }
}

void reset_category(MimeAppsList &user, const AppCategory &cat) {
  for (auto &t : cat.types) user.unset_default(t);
}

}  // namespace sde
