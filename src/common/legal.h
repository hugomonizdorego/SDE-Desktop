// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Legal data of the Selviana Desktop Environment: the license, copyright
// lines, upstream credits and trademark notices of every SDE program and
// bundled component. The data comes from data/legal/components.json, the
// single source of truth (docs/LEGAL.md), compiled in by tools/legal-data.py
// as generated/legal_data.h.
//
//   const sde::legal::Component *c = sde::legal::find("taca");
//   c->license                       // "GPL-3.0-or-later"
//   sde::legal::license_name(c->license)
//   sde::legal::trademarks_for(c)    // full trademark sentences
#pragma once

#include <cairo.h>

#include <string>
#include <vector>

namespace sde {
namespace legal {

// The project itself (not translated).
extern const char *const kProjectName;       // "Selviana Desktop Environment"
extern const char *const kProjectShortName;  // "SDE"
extern const char *const kDeveloper;         // "Selviana DE Project"
extern const char *const kDeveloperId;       // AppStream developer id
extern const char *const kWebsite;
extern const char *const kCopyright;         // "Copyright © 2026 Selviana DE Project and contributors"
extern const char *const kLicense;           // "GPL-3.0-or-later"
extern const char *const kInspiration;       // the EDE note

struct Upstream {
  std::string project, url, license, copyright;
  std::string relation;  // fork, port, adapted, build or reference
};

struct Component {
  std::string id, name, kind, summary, license, metainfo;
  bool listed = true;  // shown in Control Center > About > Open-source licenses
  std::vector<std::string> programs, desktop, icons, copyright, credits, trademarks, notices;
  std::vector<Upstream> upstream;
};

struct License {
  std::string id, name, short_name, notice, version, url;
  std::vector<std::string> texts;  // installed full texts, best first
};

const std::vector<Component> &components();
// The component with this id, or the one that installs |program|
// ("sde-panel" -> the desktop "sde"); nullptr when unknown.
const Component *find(const std::string &id_or_program);
// Translated name and summary of a component.
std::string display_name(const Component &c);
std::string display_summary(const Component &c);
// "Fork of", "Ported from" ... for an upstream relation (translated).
std::string relation_label(const std::string &relation);

// License of an SPDX id ("GPL-3.0-or-later"), or nullptr.
const License *license(const std::string &spdx);
// "A AND B" -> {"A", "B"}; "X WITH Y" stays one part.
std::vector<std::string> license_parts(const std::string &expression);
// Full name: "GNU General Public License, version 3 or later"; the parts of
// a compound expression are joined ("... and MIT License").
std::string license_name(const std::string &expression);
// Short name: "GNU GPL version 3 or later".
std::string license_short_name(const std::string &expression);
// The standard notice of a license (translated), including the warranty
// disclaimer: the GPL "This program is free software ... WITHOUT ANY
// WARRANTY ..." paragraphs and the equivalent for the other licenses.
std::string license_notice(const std::string &spdx);
// The first installed full text of |spdx| ("/usr/share/common-licenses/GPL-3"),
// or "" when none is installed. SDE_LICENSE_DIR=DIR is searched first for
// files with the same base name (tests).
std::string license_text_path(const std::string &spdx);
// Full text for the license viewer: the installed text, the MIT text when
// no file is installed, or a note with the license's web address.
std::string license_text(const std::string &spdx);
// Everything the license viewer shows for a component: name, copyright,
// each license's full text and the component's own notice files.
std::string component_license_text(const Component &c);

// Trademark sentence for a key ("sde", "linux", "debian", "paypal"), or "".
std::string trademark(const std::string &key);
// The SDE notice first, then the component's notices and |extra| keys,
// without duplicates.
std::vector<std::string> trademarks_for(const Component *c, const std::vector<std::string> &extra = {});
// Keys of the notices that a text needs: "linux" when it names Linux,
// "debian" when it names Debian.
std::vector<std::string> trademark_keys_for_text(const std::string &text);

// The official SDE logo (installed by the branding package files):
// $SDE_BRANDING_DIR, then <data dir>/sde/branding for each XDG data dir, then
// SDE_DATADIR/branding. SVG is preferred over PNG. "" when not installed.
//   names: "sde-logo" (for light backgrounds), "sde-logo-dark" (for dark
//   backgrounds), "sde-mark" (the mark only).
std::string branding_path(const std::string &name);
// The logo for the current background: sde-logo-dark on dark themes.
std::string logo_name(bool dark);
// Renders the first branding file of |names| (or, failing that, the first
// icon of |icons|) to fit a max_w x max_h box, keeping its aspect ratio.
// Caller owns the surface; nullptr when nothing is installed.
cairo_surface_t *load_branding(const std::vector<std::string> &names, int max_w, int max_h,
                               const std::vector<std::string> &icons = {});

// Operating system names from os-release, never hard-coded:
// PRETTY_NAME ("Debian GNU/Linux forky/sid") and NAME ("Debian GNU/Linux").
// Empty when os-release is missing.
std::string os_pretty_name();
std::string os_name();

}  // namespace legal
}  // namespace sde
