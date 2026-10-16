// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_entry.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "util.h"

namespace sde {

namespace {

// Locale keys to try for Name[xx], most specific first:
// ll_CC@modifier, ll_CC, ll@modifier, ll.
std::vector<std::string> locale_keys() {
  std::string lang = env("LC_ALL");
  if (lang.empty()) lang = env("LC_MESSAGES");
  if (lang.empty()) lang = env("LANG");
  std::vector<std::string> keys;
  if (lang.empty() || lang == "C" || lang == "POSIX" || starts_with(lang, "C.")) return keys;
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
  for (auto &s : split(v, ';')) {
    std::string t = trim(s);
    if (!t.empty()) out.push_back(t);
  }
  return out;
}

bool desktop_matches(const std::vector<std::string> &list, const std::string &desktop) {
  // $XDG_CURRENT_DESKTOP may contain several names separated by ':'.
  for (auto &d : split(desktop, ':'))
    for (auto &x : list)
      if (lower(x) == lower(d)) return true;
  return false;
}

void scan_applications(const std::string &dir, const std::string &prefix,
                       std::map<std::string, std::string> &found) {
  for (auto &name : list_dir(dir)) {
    std::string full = dir + "/" + name;
    if (dir_exists(full)) {
      scan_applications(full, prefix + name + "-", found);
    } else if (ends_with(name, ".desktop")) {
      std::string id = prefix + name;
      // The first directory in XDG precedence order wins.
      if (!found.count(id)) found[id] = full;
    }
  }
}

}  // namespace

bool DesktopEntry::load(const std::string &file, const std::string &file_id) {
  std::string text;
  if (!read_file(file, text)) return false;
  path = file;
  id = file_id.empty() ? basename_of(file) : file_id;

  static const std::vector<std::string> keys = locale_keys();
  std::map<std::string, std::string> values;
  std::istringstream in(text);
  std::string line;
  bool in_main = false, seen_main = false;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      in_main = (line == "[Desktop Entry]");
      seen_main = seen_main || in_main;
      continue;
    }
    if (!in_main) continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  if (!seen_main) return false;

  auto get = [&](const std::string &k) {
    auto it = values.find(k);
    return it == values.end() ? std::string() : it->second;
  };
  auto localized = [&](const std::string &k) {
    for (auto &l : keys) {
      auto it = values.find(k + "[" + l + "]");
      if (it != values.end() && !it->second.empty()) return unescape(it->second);
    }
    return unescape(get(k));
  };

  type = get("Type");
  name = localized("Name");
  generic_name = localized("GenericName");
  comment = localized("Comment");
  icon = unescape(get("Icon"));
  exec = get("Exec");
  try_exec = unescape(get("TryExec"));
  work_dir = unescape(get("Path"));
  url = unescape(get("URL"));
  keywords = list_value(localized("Keywords"));
  categories = list_value(get("Categories"));
  only_show_in = list_value(get("OnlyShowIn"));
  not_show_in = list_value(get("NotShowIn"));
  terminal = get("Terminal") == "true";
  no_display = get("NoDisplay") == "true";
  hidden = get("Hidden") == "true";
  return true;
}

bool DesktopEntry::visible_in(const std::string &desktop) const {
  if (hidden) return false;
  if (!only_show_in.empty() && !desktop_matches(only_show_in, desktop)) return false;
  if (!not_show_in.empty() && desktop_matches(not_show_in, desktop)) return false;
  if (!try_exec.empty() && find_program(try_exec).empty()) return false;
  return true;
}

std::vector<std::string> split_command(const std::string &cmd) {
  std::vector<std::string> out;
  std::string cur;
  bool in_token = false;
  char quote = 0;
  for (size_t i = 0; i < cmd.size(); ++i) {
    char c = cmd[i];
    if (quote) {
      if (c == quote) {
        quote = 0;
      } else if (c == '\\' && quote == '"' && i + 1 < cmd.size() &&
                 std::string("\"`$\\").find(cmd[i + 1]) != std::string::npos) {
        cur += cmd[++i];
      } else {
        cur += c;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
      in_token = true;
    } else if (c == '\\' && i + 1 < cmd.size()) {
      cur += cmd[++i];
      in_token = true;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      if (in_token) out.push_back(cur);
      cur.clear();
      in_token = false;
    } else {
      cur += c;
      in_token = true;
    }
  }
  if (in_token) out.push_back(cur);
  return out;
}

std::vector<std::string> DesktopEntry::command(const std::vector<std::string> &files) const {
  // Desktop files escape backslashes once more in string values.
  std::string raw;
  for (size_t i = 0; i < exec.size(); ++i) {
    if (exec[i] == '\\' && i + 1 < exec.size() && exec[i + 1] == '\\') {
      raw += '\\';
      ++i;
    } else {
      raw += exec[i];
    }
  }
  std::vector<std::string> out;
  for (auto &arg : split_command(raw)) {
    if (arg == "%f" || arg == "%u") {
      if (!files.empty()) out.push_back(files[0]);
      continue;
    }
    if (arg == "%F" || arg == "%U") {
      out.insert(out.end(), files.begin(), files.end());
      continue;
    }
    if (arg == "%i") {
      if (!icon.empty()) {
        out.push_back("--icon");
        out.push_back(icon);
      }
      continue;
    }
    std::string expanded;
    for (size_t i = 0; i < arg.size(); ++i) {
      if (arg[i] != '%' || i + 1 >= arg.size()) {
        expanded += arg[i];
        continue;
      }
      char code = arg[++i];
      if (code == '%') expanded += '%';
      else if (code == 'c') expanded += name;
      else if (code == 'k') expanded += path;
      // %d %D %n %N %v %m are deprecated and removed; %f/%u inside a word too.
      else if ((code == 'f' || code == 'u') && !files.empty()) expanded += files[0];
    }
    if (!expanded.empty()) out.push_back(expanded);
  }
  return out;
}

const std::vector<std::string> &menu_categories() {
  static const std::vector<std::string> cats = {
      "Accessories", "Education", "Games", "Graphics", "Internet", "Multimedia",
      "Office", "Development", "Science", "Settings", "System", "Other"};
  return cats;
}

std::string DesktopEntry::menu_category() const {
  // Order matters: the most specific main category wins.
  static const std::vector<std::pair<std::string, std::string>> map = {
      {"Settings", "Settings"},     {"Education", "Education"}, {"Game", "Games"},
      {"Development", "Development"}, {"Science", "Science"},   {"Graphics", "Graphics"},
      {"Network", "Internet"},      {"AudioVideo", "Multimedia"}, {"Audio", "Multimedia"},
      {"Video", "Multimedia"},      {"Office", "Office"},       {"System", "System"},
      {"Utility", "Accessories"},
  };
  for (auto &m : map)
    if (std::find(categories.begin(), categories.end(), m.first) != categories.end())
      return m.second;
  return "Other";
}

std::vector<DesktopEntry> load_applications(const std::string &desktop) {
  std::map<std::string, std::string> found;
  for (auto &dir : data_dirs()) scan_applications(dir + "/applications", "", found);
  std::vector<DesktopEntry> out;
  for (auto &kv : found) {
    DesktopEntry e;
    if (!e.load(kv.second, kv.first)) continue;
    if (e.type != "Application" || e.no_display || e.name.empty() || e.exec.empty()) continue;
    if (!e.visible_in(desktop)) continue;
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const DesktopEntry &a, const DesktopEntry &b) {
    return lower(a.name) < lower(b.name);
  });
  return out;
}

bool find_application(const std::string &id, DesktopEntry &out) {
  for (auto &dir : data_dirs()) {
    std::string base = dir + "/applications/";
    // A desktop file ID "foo-bar.desktop" may live in "foo/bar.desktop".
    std::string candidate = id;
    for (;;) {
      if (file_exists(base + candidate)) return out.load(base + candidate, id);
      size_t dash = candidate.find('-');
      if (dash == std::string::npos) break;
      candidate[dash] = '/';
    }
  }
  return false;
}

}  // namespace sde
