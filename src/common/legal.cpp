// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "legal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "build_config.h"
#include "i18n.h"
#include "icons.h"
#include "legal_data.h"
#include "util.h"

namespace sde {
namespace legal {

const char *const kProjectName = legal_data::kProjectName;
const char *const kProjectShortName = legal_data::kProjectShortName;
const char *const kDeveloper = legal_data::kProjectDeveloper;
const char *const kDeveloperId = legal_data::kProjectDeveloperId;
const char *const kWebsite = legal_data::kProjectWebsite;
const char *const kCopyright = legal_data::kProjectCopyright;
const char *const kLicense = legal_data::kProjectLicense;
const char *const kInspiration = legal_data::kProjectInspiration;

namespace {

std::vector<std::string> strings(const char *const *v, int n) {
  std::vector<std::string> out;
  for (int i = 0; i < n; ++i) out.emplace_back(v[i]);
  return out;
}

// Names and summaries of data/legal/components.json, for xgettext. The JSON
// keeps the English text; display_name() translates it at run time.
[[maybe_unused]] const char *const kTranslatable[] = {
    N_("Control Center"),
    N_("Display Settings"),
    N_("Software Updater"),
    N_("Welcome"),
    N_("Driver Manager"),
    N_("System Reports"),
    N_("System Monitor"),
    N_("Photo"),
    N_("Gallery"),
    N_("Calculator"),
    N_("Run Command"),
    N_("Log Out"),
    N_("Screen Lock"),
    N_("Power Manager"),
    N_("SDE themes"),
    N_("SDE artwork"),
    N_("Session, panel, desktop, notifications and system services"),
    N_("Settings and system information"),
    N_("Monitors, resolution, scale and display profiles"),
    N_("Critical, security, main and Flatpak updates"),
    N_("First steps after installation"),
    N_("Drivers and firmware for your hardware"),
    N_("Crash reports and system details"),
    N_("Processes, resources and startup applications"),
    N_("Text editor"),
    N_("Document viewer and PDF editor"),
    N_("Archive manager"),
    N_("Image viewer and editor"),
    N_("Photo library"),
    N_("Standard, scientific and programmer calculator"),
    N_("Run a program or command"),
    N_("Lock, suspend, log out, restart or shut down"),
    N_("Screen locker"),
    N_("Battery, lid, idle and brightness"),
    N_("File manager"),
    N_("User interface toolkit of every SDE program, a fork of FLTK 1.3.11"),
    N_("Window manager, built from Openbox"),
    N_("Compositor, built from picom"),
    N_("SDE Graphite, SDE Fluent and SDE Liquid Glass for GTK 3 and GTK 4"),
    N_("SDE logo, application icons, wallpapers and avatars"),
};

const std::vector<License> &licenses() {
  static const std::vector<License> list = [] {
    std::vector<License> out;
    for (int i = 0; i < legal_data::kLicenseCount; ++i) {
      const auto &l = legal_data::kLicenses[i];
      out.push_back({l.id, l.name, l.short_name, l.notice, l.version, l.url,
                     strings(l.texts, l.text_count)});
    }
    return out;
  }();
  return list;
}

std::string join_names(const std::vector<std::string> &names) {
  if (names.empty()) return "";
  if (names.size() == 1) return names[0];
  std::string out;
  for (size_t i = 0; i + 1 < names.size(); ++i) out += (i ? ", " : "") + names[i];
  return format(_("%s and %s"), out.c_str(), names.back().c_str());
}

bool contains_word(const std::string &text, const std::string &word) {
  size_t pos = 0;
  while ((pos = text.find(word, pos)) != std::string::npos) {
    bool left = pos == 0 || !isalnum(static_cast<unsigned char>(text[pos - 1]));
    size_t end = pos + word.size();
    bool right = end >= text.size() || !isalnum(static_cast<unsigned char>(text[end]));
    if (left && right) return true;
    pos = end;
  }
  return false;
}

// The MIT text without its copyright line, shown when no MIT file is installed.
const char *const kMitText =
    "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
    "of this software and associated documentation files (the \"Software\"), to deal\n"
    "in the Software without restriction, including without limitation the rights\n"
    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
    "copies of the Software, and to permit persons to whom the Software is\n"
    "furnished to do so, subject to the following conditions:\n\n"
    "The above copyright notice and this permission notice shall be included in all\n"
    "copies or substantial portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
    "SOFTWARE.\n";

std::string base_name(const std::string &path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

const std::vector<Component> &components() {
  static const std::vector<Component> list = [] {
    std::vector<Component> out;
    for (int i = 0; i < legal_data::kComponentCount; ++i) {
      const auto &d = legal_data::kComponents[i];
      Component c;
      c.id = d.id;
      c.name = d.name;
      c.kind = d.kind;
      c.summary = d.summary;
      c.license = d.license;
      c.metainfo = d.metainfo;
      c.listed = d.listed;
      c.programs = strings(d.programs, d.program_count);
      c.desktop = strings(d.desktop, d.desktop_count);
      c.icons = strings(d.icons, d.icon_count);
      c.copyright = strings(d.copyright, d.copyright_count);
      c.credits = strings(d.credits, d.credit_count);
      c.trademarks = strings(d.trademarks, d.trademark_count);
      c.notices = strings(d.notices, d.notice_count);
      for (int k = 0; k < d.upstream_count; ++k) {
        const auto &u = d.upstream[k];
        c.upstream.push_back({u.project, u.url, u.license, u.copyright, u.relation});
      }
      out.push_back(std::move(c));
    }
    return out;
  }();
  return list;
}

const Component *find(const std::string &id_or_program) {
  for (const auto &c : components())
    if (c.id == id_or_program) return &c;
  for (const auto &c : components())
    if (std::find(c.programs.begin(), c.programs.end(), id_or_program) != c.programs.end()) return &c;
  return nullptr;
}

std::string display_name(const Component &c) { return _(c.name.c_str()); }
std::string display_summary(const Component &c) { return _(c.summary.c_str()); }

std::string relation_label(const std::string &relation) {
  if (relation == "fork") return _("Fork of");
  if (relation == "port") return _("Ported from");
  if (relation == "adapted") return _("Adapted from");
  if (relation == "build") return _("Built from");
  if (relation == "reference") return _("Design reference");
  return relation;
}

const License *license(const std::string &spdx) {
  std::string id = spdx;
  if (id == "GPL-3.0+") id = "GPL-3.0-or-later";
  if (id == "GPL-2.0+") id = "GPL-2.0-or-later";
  if (id == "GPL-3.0") id = "GPL-3.0-only";
  for (const auto &l : licenses())
    if (l.id == id) return &l;
  return nullptr;
}

std::vector<std::string> license_parts(const std::string &expression) {
  std::vector<std::string> parts;
  std::string e = expression;
  for (const char *op : {" OR ", " or "})
    for (size_t p; (p = e.find(op)) != std::string::npos;) e.replace(p, strlen(op), " AND ");
  size_t start = 0;
  while (start <= e.size()) {
    size_t p = e.find(" AND ", start);
    std::string part = trim(e.substr(start, p == std::string::npos ? std::string::npos : p - start));
    while (!part.empty() && (part.front() == '(' || part.back() == ')')) {
      if (part.front() == '(') part.erase(0, 1);
      if (!part.empty() && part.back() == ')') part.pop_back();
      part = trim(part);
    }
    if (!part.empty()) parts.push_back(part);
    if (p == std::string::npos) break;
    start = p + 5;
  }
  return parts;
}

std::string license_name(const std::string &expression) {
  std::vector<std::string> names;
  for (const auto &part : license_parts(expression)) {
    const License *l = license(part);
    names.push_back(l ? std::string(_(l->name.c_str())) : part);
  }
  return join_names(names);
}

std::string license_short_name(const std::string &expression) {
  std::vector<std::string> names;
  for (const auto &part : license_parts(expression)) {
    const License *l = license(part);
    names.push_back(l ? std::string(_(l->short_name.c_str())) : part);
  }
  return join_names(names);
}

std::string license_notice(const std::string &spdx) {
  const License *l = license(spdx);
  std::string kind = l ? l->notice : "";
  if (kind == "gpl")
    return format(_("This program is free software: you can redistribute it and/or modify it under the "
                    "terms of the GNU General Public License as published by the Free Software "
                    "Foundation, either version %s of the License, or (at your option) any later "
                    "version.\n\n"
                    "This program is distributed in the hope that it will be useful, but WITHOUT ANY "
                    "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A "
                    "PARTICULAR PURPOSE. See the GNU General Public License for more details.\n\n"
                    "You should have received a copy of the GNU General Public License along with this "
                    "program. If not, see <https://www.gnu.org/licenses/>."),
                  l->version.c_str());
  if (kind == "gpl-only")
    return format(_("This program is free software: you can redistribute it and/or modify it under the "
                    "terms of the GNU General Public License, version %s, as published by the Free "
                    "Software Foundation.\n\n"
                    "This program is distributed in the hope that it will be useful, but WITHOUT ANY "
                    "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A "
                    "PARTICULAR PURPOSE. See the GNU General Public License for more details."),
                  l->version.c_str());
  if (kind == "lgpl")
    return format(_("This library is free software; you can redistribute it and/or modify it under the "
                    "terms of the GNU Library General Public License as published by the Free Software "
                    "Foundation; either version %s of the License, or (at your option) any later "
                    "version.\n\n"
                    "This library is distributed in the hope that it will be useful, but WITHOUT ANY "
                    "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A "
                    "PARTICULAR PURPOSE. See the GNU Library General Public License for more details."),
                  l->version.c_str());
  if (kind == "fltk")
    return _("The FLTK library and included programs are provided under the terms of the GNU Library "
             "General Public License (LGPL), version 2, with the FLTK exceptions: changes to support "
             "a platform, widgets subclassed from FLTK widgets and static linking do not constitute "
             "a derivative work. Changes to FLTK itself are provided under the LGPL.\n\n"
             "This library is distributed in the hope that it will be useful, but WITHOUT ANY "
             "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A "
             "PARTICULAR PURPOSE. See the GNU Library General Public License for more details.");
  if (kind == "mpl")
    return _("This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If "
             "a copy of the MPL was not distributed with this file, you can obtain one at "
             "https://mozilla.org/MPL/2.0/.\n\n"
             "Covered Software is provided under this License on an \"as is\" basis, without warranty "
             "of any kind, either expressed, implied, or statutory.");
  if (kind == "mit")
    return _("Permission is hereby granted, free of charge, to any person obtaining a copy of this "
             "software and associated documentation files, to deal in the Software without "
             "restriction, subject to the conditions of the MIT License.\n\n"
             "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.");
  if (kind == "cc")
    return format(_("This work is licensed under the Creative Commons Attribution-ShareAlike %s "
                    "International License. It is provided as is, without warranties of any kind."),
                  l->version.c_str());
  if (kind == "cc0")
    return _("To the extent possible under law, the authors have dedicated this work to the public "
             "domain (CC0 1.0). It is provided as is, without warranties of any kind.");
  return format(_("Distributed under the terms of %s."), spdx.c_str());
}

std::string license_text_path(const std::string &spdx) {
  const License *l = license(spdx);
  if (!l) return "";
  std::string dir = env("SDE_LICENSE_DIR");
  for (const auto &path : l->texts) {
    if (!dir.empty() && file_exists(dir + "/" + base_name(path))) return dir + "/" + base_name(path);
    if (file_exists(path)) return path;
  }
  return "";
}

std::string license_text(const std::string &spdx) {
  std::string path = license_text_path(spdx), text;
  if (!path.empty() && read_file(path, text) && !text.empty()) return text;
  const License *l = license(spdx);
  if (l && l->notice == "mit") return kMitText;
  std::string url = l ? l->url : "";
  return format(_("The full text of the %s is not installed on this computer."),
                l ? _(l->name.c_str()) : spdx.c_str()) +
         (url.empty() ? std::string() : "\n\n" + format(_("Read it at %s"), url.c_str())) + "\n";
}

std::string component_license_text(const Component &c) {
  std::string out = display_name(c) + "\n";
  for (const auto &line : c.copyright) out += line + "\n";
  out += format(_("License: %s"), license_name(c.license).c_str()) + "\n";
  for (const auto &u : c.upstream)
    out += "\n" + relation_label(u.relation) + " " + u.project + " (" + license_short_name(u.license) +
           ")\n" + u.copyright + "\n" + u.url + "\n";
  std::vector<std::string> shown;
  for (const auto &part : license_parts(c.license)) {
    const License *l = license(part);
    out += "\n\n======== " + (l ? std::string(_(l->name.c_str())) : part) + " ========\n\n";
    out += license_text(part);
    shown.push_back(license_text_path(part));
  }
  for (const auto &path : c.notices) {
    if (base_name(path) == "copyright" && path.find("/doc/sde/copyright") != std::string::npos) continue;
    if (std::find(shown.begin(), shown.end(), path) != shown.end()) continue;
    std::string text;
    if (!read_file(path, text) || text.empty()) continue;
    out += "\n\n======== " + path + " ========\n\n" + text;
  }
  return out;
}

std::string trademark(const std::string &key) {
  for (int i = 0; i < legal_data::kTrademarkCount; ++i)
    if (key == legal_data::kTrademarks[i].id) return legal_data::kTrademarks[i].text;
  return "";
}

std::vector<std::string> trademarks_for(const Component *c, const std::vector<std::string> &extra) {
  std::vector<std::string> keys = {"sde"};
  if (c)
    for (const auto &k : c->trademarks) keys.push_back(k);
  for (const auto &k : extra) keys.push_back(k);
  std::vector<std::string> out, seen;
  for (const auto &k : keys) {
    if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
    seen.push_back(k);
    std::string t = trademark(k);
    if (!t.empty()) out.push_back(t);
  }
  return out;
}

std::vector<std::string> trademark_keys_for_text(const std::string &text) {
  std::vector<std::string> keys;
  if (contains_word(text, "Linux")) keys.push_back("linux");
  if (contains_word(text, "Debian")) keys.push_back("debian");
  if (contains_word(text, "PayPal")) keys.push_back("paypal");
  return keys;
}

std::string branding_path(const std::string &name) {
  std::vector<std::string> dirs;
  std::string forced = env("SDE_BRANDING_DIR");
  if (!forced.empty()) dirs.push_back(forced);
  for (const auto &d : data_dirs()) dirs.push_back(d + "/sde/branding");
  dirs.push_back(std::string(SDE_DATADIR) + "/branding");
  for (const auto &d : dirs)
    for (const char *ext : {".svg", ".png"})
      if (file_exists(d + "/" + name + ext)) return d + "/" + name + ext;
  return "";
}

std::string logo_name(bool dark) { return dark ? "sde-logo-dark" : "sde-logo"; }

cairo_surface_t *load_branding(const std::vector<std::string> &names, int max_w, int max_h,
                               const std::vector<std::string> &icons) {
  if (max_w <= 0 || max_h <= 0) return nullptr;
  std::string path;
  for (const auto &n : names)
    if (!(path = branding_path(n)).empty()) break;
  if (path.empty())
    for (const auto &n : icons)
      if (!(path = IconLoader::lookup(n, std::min(max_w, max_h))).empty()) break;
  if (path.empty()) return nullptr;
  cairo_surface_t *src = load_image(path);
  if (!src) return nullptr;
  double iw = cairo_image_surface_get_width(src), ih = cairo_image_surface_get_height(src);
  if (iw <= 0 || ih <= 0) {
    cairo_surface_destroy(src);
    return nullptr;
  }
  double scale = std::min(max_w / iw, max_h / ih);
  int w = std::max(1, int(std::lround(iw * scale))), h = std::max(1, int(std::lround(ih * scale)));
  if (ends_with(lower(path), ".svg")) {
    // Vector logos are rendered again at the final size for sharp edges.
    cairo_surface_t *exact = load_image(path, w, h);
    if (exact) {
      cairo_surface_destroy(src);
      return exact;
    }
  }
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(out);
  cairo_scale(cr, w / iw, h / ih);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(src);
  return out;
}

cairo_surface_t *load_mark(int size, bool dark) {
  if (size <= 0) return nullptr;
  if (size <= 32) {
    int drawn = size >= 32 ? 32 : size >= 24 ? 24 : size >= 22 ? 22 : 16;
    if (cairo_surface_t *s = load_branding({}, drawn, drawn, {"sde-logo"})) return s;
  }
  if (cairo_surface_t *s = load_branding({"sde-mark"}, size, size, {"sde-logo"})) return s;
  return load_branding({logo_name(dark)}, size, size);
}

std::string os_pretty_name() {
  std::string v = os_release("PRETTY_NAME");
  return v.empty() ? os_release("NAME") : v;
}

std::string os_name() {
  std::string v = os_release("NAME");
  return v.empty() ? os_release("PRETTY_NAME") : v;
}

}  // namespace legal
}  // namespace sde
