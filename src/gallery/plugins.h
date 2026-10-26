// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Plugins of Photo and Gallery. Each program has
//
//  * built-in plugins: features that its Plugins window switches on and off
//    (Photo: collages, frames, watermarks...; Gallery: places, duplicates,
//    printing...), and
//  * action plugins: small files that run a program on the chosen photos.
//    They are read from
//
//      ~/.local/share/sde/<program>/plugins/*.plugin   (the user's)
//      /usr/share/sde/<program>/plugins/*.plugin       ($XDG_DATA_DIRS)
//
//    where <program> is "photo" or "gallery"; a file of the user replaces a
//    system file of the same name. The format follows desktop entries:
//
//      [SDE Plugin]
//      Type=Action
//      Name=Optimize for the Web
//      Name[id]=Optimalkan untuk Web
//      Comment=Makes JPEG files smaller without visible loss (jpegoptim)
//      Icon=applications-graphics
//      Exec=jpegoptim --strip-all --max=85 %F
//      TryExec=jpegoptim
//      MimeType=image/jpeg;
//      Files=multiple        single: one photo at a time; multiple (default)
//      Changes=true          the program rewrites the photos: show them again
//      Terminal=false
//
//    Exec= is split into arguments like a desktop entry's, and no shell is
//    involved: %f is a photo, %F all of them, %u and %U the same as file://
//    URIs, %% a percent sign. Field codes are only allowed as whole
//    arguments, so a file name never becomes part of another argument (a
//    shell command line, for one): a plugin that breaks the rule is listed
//    as invalid and never run. With %f (or %u) and several photos the
//    program runs once per photo.
//
// Whether a plugin is on is kept in sde.conf: [<program>] plugin_<id> for
// built-in plugins, action_<id> for action plugins (true or false).
// Nothing here needs a display.
#pragma once

#include <string>
#include <vector>

namespace sde {
class Config;
}

namespace gallery {

struct BuiltinPlugin {
  std::string id;           // "collage"; the setting is plugin_<id>
  std::string name;         // translated
  std::string description;  // translated, one or two sentences
  int glyph = 0;            // a line icon (icon::Kind)
  bool default_on = true;
};

struct ActionPlugin {
  std::string id;    // the file name without ".plugin"; the setting is action_<id>
  std::string file;  // absolute path of the .plugin file
  std::string name, comment, icon, exec, try_exec;
  std::vector<std::string> mime_types;  // empty: every image
  bool multiple = true;  // Files=multiple (several photos at once) or single
  bool changes = false;  // Changes=true: the photos are rewritten
  bool terminal = false;
  std::string problem;   // why the plugin cannot be used ("" when it can)
  bool valid() const { return problem.empty(); }
  // TryExec= and the program of Exec= are installed.
  bool available() const;
  // True when every type in |mime_types_of_files| is one the plugin takes.
  bool accepts(const std::vector<std::string> &mime_types_of_files) const;
  // The programs to run for |files| (absolute paths): one argument vector,
  // or one per file when Exec= has %f or %u. Empty when invalid.
  std::vector<std::vector<std::string>> commands(const std::vector<std::string> &files) const;
};

// Parses the text of a .plugin file. |locales| are the Name[xx] keys to try,
// most specific first (see plugin_locales()). Returns false when the text
// has no [SDE Plugin] group or is not an Action; a plugin whose Exec= breaks
// the rules above is returned with |problem| set.
bool parse_action_plugin(const std::string &text, const std::string &id, ActionPlugin &out,
                         const std::vector<std::string> &locales = {});
// Name[xx] keys for the current locale: ll_CC@modifier, ll_CC, ll@modifier, ll.
std::vector<std::string> plugin_locales();
// The folders searched for action plugins of |program|, the user's first
// ($SDE_PHOTO_DATA_DIR/plugins or $SDE_GALLERY_DATA_DIR/plugins, a source
// tree's data/<program>, next).
std::vector<std::string> plugin_dirs(const std::string &program);
// The user's folder (~/.local/share/sde/<program>/plugins).
std::string user_plugin_dir(const std::string &program);
// The action plugins of |program|, sorted by name.
std::vector<ActionPlugin> load_action_plugins(const std::string &program);

bool plugin_enabled(const sde::Config &cfg, const std::string &program, const std::string &id, bool default_on);
void set_plugin_enabled(sde::Config &cfg, const std::string &program, const std::string &id, bool on);
bool action_enabled(const sde::Config &cfg, const std::string &program, const std::string &id);
void set_action_enabled(sde::Config &cfg, const std::string &program, const std::string &id, bool on);

// The MIME type of a file, from its content and name ("image/jpeg").
std::string mime_type_of(const std::string &path);
// True when |type| matches |pattern| ("image/jpeg", "image/*", "*/*").
bool mime_matches(const std::string &pattern, const std::string &type);

}  // namespace gallery
