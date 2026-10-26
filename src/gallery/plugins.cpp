// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "plugins.h"

#include <gio/gio.h>

#include <algorithm>
#include <map>
#include <sstream>

#include "config.h"
#include "desktop_entry.h"
#include "library.h"
#include "util.h"

namespace gallery {

namespace {

std::string unescape(const std::string &v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char n = v[++i];
      switch (n) {
        case 's': out += ' '; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        default: out += n;
      }
    } else {
      out += v[i];
    }
  }
  return out;
}

std::vector<std::string> list_value(const std::string &v) {
  std::vector<std::string> out;
  for (auto &s : sde::split(v, ';')) {
    std::string t = sde::trim(s);
    if (!t.empty()) out.push_back(t);
  }
  return out;
}

bool field_code(const std::string &arg) { return arg == "%f" || arg == "%F" || arg == "%u" || arg == "%U"; }

// The arguments of Exec= (desktop files escape backslashes once more).
std::vector<std::string> exec_args(const std::string &exec) {
  std::string raw;
  for (size_t i = 0; i < exec.size(); ++i) {
    if (exec[i] == '\\' && i + 1 < exec.size() && exec[i + 1] == '\\') {
      raw += '\\';
      ++i;
    } else {
      raw += exec[i];
    }
  }
  return sde::split_command(raw);
}

// Why |exec| cannot be run safely, or "".
std::string check_exec(const std::string &exec) {
  auto args = exec_args(exec);
  if (args.empty()) return "Exec= is empty";
  if (field_code(args[0])) return "Exec= must start with a program";
  for (const auto &a : args) {
    if (field_code(a)) continue;
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i] != '%') continue;
      if (i + 1 < a.size() && a[i + 1] == '%') {
        ++i;
        continue;
      }
      return "Exec= may use %f, %F, %u and %U only as whole arguments";
    }
  }
  return "";
}

std::string percent_unescape(const std::string &a) {
  std::string out;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == '%' && i + 1 < a.size() && a[i + 1] == '%') ++i;
    out += a[i];
  }
  return out;
}

std::string section_of(const std::string &program) { return program == "photo" ? "photo" : "gallery"; }

}  // namespace

std::vector<std::string> plugin_locales() {
  std::string lang = sde::env("LC_ALL");
  if (lang.empty()) lang = sde::env("LC_MESSAGES");
  if (lang.empty()) lang = sde::env("LANG");
  std::vector<std::string> keys;
  if (lang.empty() || lang == "C" || lang == "POSIX" || sde::starts_with(lang, "C.")) return keys;
  std::string modifier;
  size_t at = lang.find('@');
  if (at != std::string::npos) {
    modifier = lang.substr(at);
    lang = lang.substr(0, at);
  }
  size_t dot = lang.find('.');
  if (dot != std::string::npos) lang = lang.substr(0, dot);
  std::string ll = lang.substr(0, lang.find('_'));
  if (!modifier.empty()) keys.push_back(lang + modifier);
  keys.push_back(lang);
  if (!modifier.empty()) keys.push_back(ll + modifier);
  if (ll != lang) keys.push_back(ll);
  return keys;
}

bool parse_action_plugin(const std::string &text, const std::string &id, ActionPlugin &out,
                         const std::vector<std::string> &locales) {
  std::map<std::string, std::string> values;
  std::istringstream in(text);
  std::string line;
  bool in_main = false, seen = false;
  while (std::getline(in, line)) {
    line = sde::trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      in_main = line == "[SDE Plugin]";
      seen = seen || in_main;
      continue;
    }
    if (!in_main) continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    values[sde::trim(line.substr(0, eq))] = sde::trim(line.substr(eq + 1));
  }
  if (!seen) return false;
  auto get = [&](const std::string &k) {
    auto it = values.find(k);
    return it == values.end() ? std::string() : it->second;
  };
  auto localized = [&](const std::string &k) {
    for (const auto &l : locales) {
      auto it = values.find(k + "[" + l + "]");
      if (it != values.end() && !it->second.empty()) return unescape(it->second);
    }
    return unescape(get(k));
  };
  std::string type = get("Type");
  if (!type.empty() && type != "Action") return false;
  out = ActionPlugin();
  out.id = id;
  out.name = localized("Name");
  out.comment = localized("Comment");
  out.icon = unescape(get("Icon"));
  out.exec = get("Exec");
  out.try_exec = unescape(get("TryExec"));
  out.mime_types = list_value(get("MimeType"));
  out.multiple = sde::lower(get("Files")) != "single";
  out.changes = get("Changes") == "true";
  out.terminal = get("Terminal") == "true";
  if (out.name.empty()) out.problem = "Name= is missing";
  else out.problem = check_exec(out.exec);
  return true;
}

bool ActionPlugin::available() const {
  if (!valid()) return false;
  auto program_found = [](const std::string &p) {
    if (p.empty()) return true;
    if (p[0] == '/') return sde::is_executable(p);
    return !sde::find_program(p).empty();
  };
  auto args = exec_args(exec);
  return program_found(try_exec) && !args.empty() && program_found(args[0]);
}

bool ActionPlugin::accepts(const std::vector<std::string> &types) const {
  if (types.empty()) return false;
  // Files=single runs once per photo, so it takes several photos as well.
  for (const auto &t : types) {
    bool ok = false;
    if (mime_types.empty()) ok = sde::starts_with(t, "image/");
    for (const auto &m : mime_types) ok = ok || mime_matches(m, t);
    if (!ok) return false;
  }
  return true;
}

std::vector<std::vector<std::string>> ActionPlugin::commands(const std::vector<std::string> &files) const {
  std::vector<std::vector<std::string>> out;
  if (!valid()) return out;
  auto args = exec_args(exec);
  bool single = !multiple;
  for (const auto &a : args)
    if (a == "%f" || a == "%u") single = true;
  auto build = [&](const std::vector<std::string> &these) {
    std::vector<std::string> argv;
    for (const auto &a : args) {
      if (a == "%f" || a == "%u") {
        if (!these.empty()) argv.push_back(a == "%u" ? file_uri(these[0]) : these[0]);
      } else if (a == "%F" || a == "%U") {
        for (const auto &f : these) argv.push_back(a == "%U" ? file_uri(f) : f);
      } else {
        argv.push_back(percent_unescape(a));
      }
    }
    return argv;
  };
  if (single && files.size() > 1) {
    for (const auto &f : files) out.push_back(build({f}));
  } else {
    out.push_back(build(files));
  }
  return out;
}

std::string user_plugin_dir(const std::string &program) {
  return sde::data_home() + "/sde/" + program + "/plugins";
}

std::vector<std::string> plugin_dirs(const std::string &program) {
  std::vector<std::string> out;
  for (const auto &d : sde::data_dirs()) {
    std::string dir = d + "/sde/" + program + "/plugins";
    if (std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(dir);
  }
  return out;
}

std::vector<ActionPlugin> load_action_plugins(const std::string &program) {
  std::vector<ActionPlugin> out;
  std::map<std::string, bool> seen;
  auto locales = plugin_locales();
  for (const auto &dir : plugin_dirs(program)) {
    auto names = sde::list_dir(dir);
    std::sort(names.begin(), names.end());
    for (const auto &name : names) {
      if (!sde::ends_with(name, ".plugin")) continue;
      std::string id = name.substr(0, name.size() - 7);
      if (id.empty() || seen.count(id)) continue;
      seen[id] = true;  // the user's file hides the system's, even a broken one
      std::string text;
      if (!sde::read_file(dir + "/" + name, text)) continue;
      ActionPlugin p;
      if (!parse_action_plugin(text, id, p, locales)) continue;
      p.file = dir + "/" + name;
      out.push_back(std::move(p));
    }
  }
  std::sort(out.begin(), out.end(),
            [](const ActionPlugin &a, const ActionPlugin &b) { return sde::lower(a.name) < sde::lower(b.name); });
  return out;
}

bool plugin_enabled(const sde::Config &cfg, const std::string &program, const std::string &id, bool default_on) {
  return cfg.get_bool(section_of(program), "plugin_" + id, default_on);
}

void set_plugin_enabled(sde::Config &cfg, const std::string &program, const std::string &id, bool on) {
  cfg.set(section_of(program), "plugin_" + id, on ? "true" : "false");
}

bool action_enabled(const sde::Config &cfg, const std::string &program, const std::string &id) {
  return cfg.get_bool(section_of(program), "action_" + id, true);
}

void set_action_enabled(sde::Config &cfg, const std::string &program, const std::string &id, bool on) {
  cfg.set(section_of(program), "action_" + id, on ? "true" : "false");
}

std::string mime_type_of(const std::string &path) {
  // Content first (a JPEG named .png is a JPEG), the name as a hint.
  std::string head;
  FILE *f = fopen(path.c_str(), "rb");
  if (f) {
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    head.assign(buf, n);
    fclose(f);
  }
  auto guess = [&](const char *name, bool data) {
    gboolean uncertain = FALSE;
    gchar *type = g_content_type_guess(name, data ? reinterpret_cast<const guchar *>(head.data()) : nullptr,
                                       data ? head.size() : 0, &uncertain);
    std::string mime;
    if (type && !(data && uncertain)) {
      gchar *m = g_content_type_get_mime_type(type);
      if (m) mime = m;
      g_free(m);
    }
    g_free(type);
    return mime;
  };
  auto generic = [](const std::string &m) {
    return m.empty() || m == "application/octet-stream" || m == "text/plain";
  };
  std::string out = head.empty() ? std::string() : guess(nullptr, true);
  if (generic(out)) out = guess(path.c_str(), !head.empty());
  if (generic(out)) {
    // Without the shared MIME database: the usual image extensions.
    std::string e = extension_of(path);
    static const std::map<std::string, std::string> by_ext = {
        {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"}, {"jpe", "image/jpeg"},   {"png", "image/png"},
        {"gif", "image/gif"},   {"webp", "image/webp"}, {"tif", "image/tiff"},   {"tiff", "image/tiff"},
        {"bmp", "image/bmp"},   {"svg", "image/svg+xml"}, {"svgz", "image/svg+xml-compressed"},
        {"avif", "image/avif"}, {"heic", "image/heic"}, {"heif", "image/heif"},  {"jxl", "image/jxl"},
        {"cr2", "image/x-canon-cr2"}, {"nef", "image/x-nikon-nef"}, {"arw", "image/x-sony-arw"},
        {"dng", "image/x-adobe-dng"}, {"orf", "image/x-olympus-orf"}, {"raf", "image/x-fuji-raf"},
        {"rw2", "image/x-panasonic-rw2"}};
    auto it = by_ext.find(e);
    if (it != by_ext.end()) out = it->second;
  }
  return out.empty() ? "application/octet-stream" : out;
}

bool mime_matches(const std::string &pattern, const std::string &type) {
  if (pattern == type || pattern == "*/*" || pattern == "*") return true;
  if (sde::ends_with(pattern, "/*")) return sde::starts_with(type, pattern.substr(0, pattern.size() - 1));
  // Common aliases.
  if ((pattern == "image/jpg" || pattern == "image/pjpeg") && type == "image/jpeg") return true;
  if (pattern == "image/x-bmp" && type == "image/bmp") return true;
  return false;
}

}  // namespace gallery
