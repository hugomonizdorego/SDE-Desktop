// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "model.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

#include "build_config.h"
#include "config.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace welcome {

bool show_at_startup(const Config &cfg) { return cfg.get_bool("welcome", "show", false); }

void set_show_at_startup(Config &cfg, bool show) { cfg.set("welcome", "show", show ? "true" : "false"); }

// ---- live session

namespace {

std::vector<std::string> words_of(const std::string &s) {
  std::istringstream in(s);
  std::vector<std::string> out;
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}

struct Mount {
  std::string source, target, type, options;
};

// /proc/mounts escapes spaces and tabs as \040 and \011.
std::string unescape_mount(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
      out += char(std::stoi(s.substr(i + 1, 3), nullptr, 8));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::vector<Mount> parse_mounts(const std::string &text) {
  std::vector<Mount> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    auto w = words_of(line);
    if (w.size() < 4) continue;
    out.push_back({unescape_mount(w[0]), unescape_mount(w[1]), w[2], w[3]});
  }
  return out;
}

// Value of "key=value" in a comma separated option list ("" when missing).
std::string mount_option(const std::string &options, const std::string &key) {
  for (auto &o : split(options, ','))
    if (starts_with(o, key + "=")) return o.substr(key.size() + 1);
  return "";
}

bool under(const std::string &path, const std::string &dir) {
  if (dir == "/") return true;
  return path == dir || starts_with(path, dir + "/");
}

std::string root_reason(const std::string &mounts_text) {
  auto mounts = parse_mounts(mounts_text);
  const Mount *root = nullptr;
  for (const auto &m : mounts)
    if (m.target == "/") root = &m;  // the last mount on / is the visible one
  if (!root) return "";
  std::vector<const Mount *> squash;
  for (const auto &m : mounts)
    if (m.type == "squashfs") squash.push_back(&m);
  if (root->type == "squashfs") return "root file system: squashfs";
  if (root->type == "overlay") {
    // lowerdir=/run/live/rootfs/filesystem.squashfs/ (several joined by ':').
    for (auto &dir : split(mount_option(root->options, "lowerdir"), ':')) {
      std::string d = dir;
      while (d.size() > 1 && d.back() == '/') d.pop_back();
      for (const Mount *s : squash)
        if (under(d, s->target)) return "root file system: overlay on squashfs (" + s->target + ")";
    }
    return "";
  }
  if ((root->type == "tmpfs" || root->type == "rootfs") && !squash.empty())
    return "root file system: " + root->type + " with a squashfs image";
  return "";
}

std::string live_path(const std::string &path) { return env("SDE_LIVE_ROOT") + path; }

}  // namespace

LiveProbe probe_live_session() {
  LiveProbe p;
  read_file(env("SDE_PROC_CMDLINE", "/proc/cmdline"), p.cmdline);
  read_file(env("SDE_PROC_MOUNTS", "/proc/mounts"), p.mounts);
  p.run_live = dir_exists(live_path("/run/live"));
  p.live_medium = dir_exists(live_path("/lib/live/mount/medium")) || dir_exists(live_path("/run/live/medium"));
  std::string etc_live = live_path("/etc/live");
  for (auto &name : list_dir(etc_live))
    if (starts_with(name, "config")) p.live_config = true;
  p.user = user_name();
  std::string desktop = env("SDE_DESKTOP_DIR");
  if (desktop.empty()) desktop = env("SDE_PROC_CMDLINE").empty() ? user_dir("DESKTOP") : "";
  if (!desktop.empty()) {
    for (auto &name : list_dir(desktop)) {
      std::string n = lower(name);
      if (!ends_with(n, ".desktop")) continue;
      if (n.find("calamares") != std::string::npos || n.find("install-debian") != std::string::npos ||
          n.find("debian-installer") != std::string::npos || n.find("install-debion") != std::string::npos)
        p.installer_launcher = true;
    }
  }
  return p;
}

std::string live_reason(const LiveProbe &p) {
  auto words = words_of(p.cmdline);
  bool toram = false;
  std::string live_word;
  for (const auto &w : words) {
    if (w == "boot=live" || w == "boot=casper" || w == "rd.live.image" || starts_with(w, "live-media=") ||
        starts_with(w, "live-media-path=") || w == "casper") {
      if (live_word.empty()) live_word = w;
    }
    if (w == "toram" || starts_with(w, "toram=")) toram = true;
  }
  if (!live_word.empty()) return "kernel command line: " + live_word + (toram ? " toram" : "");
  if (p.run_live) return "/run/live exists";
  if (p.live_medium) return "live medium mounted";
  std::string root = root_reason(p.mounts);
  if (!root.empty()) return root;
  if ((p.user == "user" || p.user == "live") && p.live_config) return "live user with live-config";
  if (p.installer_launcher) return "installer launcher on the desktop";
  return "";
}

bool is_live_session(const LiveProbe &probe) { return !live_reason(probe).empty(); }

bool is_live_session() { return is_live_session(probe_live_session()); }

std::string done_stamp_path() { return join_path(config_home(), "sde/welcome-done"); }

bool welcome_done() {
  if (file_exists(done_stamp_path())) return true;
  std::string conf;
  if (!read_file(Config::user_path(), conf)) return false;
  for (auto &line : split(conf, '\n'))
    if (trim(line) == "[welcome]") return true;
  return false;
}

bool mark_welcome_done() {
  std::string path = done_stamp_path();
  if (file_exists(path)) return true;
  make_dirs(join_path(config_home(), "sde"));
  return write_file_atomic(path, "SDE Welcome was shown to this user. Delete this file to see it at the next login.\n");
}

bool should_autostart(bool live, bool done, bool show_setting) {
  if (live) return false;
  if (!done) return true;
  return show_setting;
}

bool ufw_conf_enabled(const std::string &ufw_conf) {
  std::istringstream in(ufw_conf);
  std::string line;
  bool enabled = false;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (starts_with(line, "ENABLED=")) {
      std::string v = lower(trim(line.substr(8)));
      if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
      enabled = v == "yes";
    }
  }
  return enabled;
}

std::string parse_ufw_status(const std::string &output) {
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    line = lower(trim(line));
    if (starts_with(line, "status:")) {
      std::string v = trim(line.substr(7));
      if (v == "active" || v == "inactive") return v;
    }
  }
  return "";
}

const std::vector<std::string> &codec_packages() {
  static const std::vector<std::string> list = {"gstreamer1.0-plugins-good", "gstreamer1.0-plugins-bad",
                                                "gstreamer1.0-plugins-ugly", "gstreamer1.0-libav"};
  return list;
}

std::vector<std::string> missing_packages(const std::vector<std::string> &wanted,
                                          const std::vector<std::string> &installed) {
  std::vector<std::string> out;
  for (const auto &w : wanted)
    if (std::find(installed.begin(), installed.end(), w) == installed.end()) out.push_back(w);
  return out;
}

// ---- content

namespace {

// Language keys to try for Name[xx], most specific first: ll_CC@modifier,
// ll_CC, ll@modifier, ll (as in desktop_entry.cpp).
std::vector<std::string> language_keys() {
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
  lang = lang.substr(0, lang.find('.'));
  std::string ll = lang.substr(0, lang.find('_'));
  if (!modifier.empty()) keys.push_back(lang + modifier);
  keys.push_back(lang);
  if (!modifier.empty()) keys.push_back(ll + modifier);
  if (ll != lang) keys.push_back(ll);
  return keys;
}

// Desktop Entry escapes: \s \n \t \r \\ (and \; inside lists).
std::string unescape_value(const std::string &v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char n = v[++i];
      out += n == 's' ? ' ' : n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
    } else {
      out += v[i];
    }
  }
  return out;
}

// "a;b\;c;" -> {"a", "b;c"}
std::vector<std::string> list_of(const std::string &raw) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == ';') {
      cur += ';';
      ++i;
    } else if (raw[i] == ';') {
      if (!trim(cur).empty()) out.push_back(unescape_value(trim(cur)));
      cur.clear();
    } else {
      cur += raw[i];
    }
  }
  if (!trim(cur).empty()) out.push_back(unescape_value(trim(cur)));
  return out;
}

const std::string kEntry = "Desktop Entry";

// A value naming a file relative to the content folder ("icons/logo.svg",
// "scripts/setup.sh") becomes the first such file that exists.
std::string resolve(const std::string &value, const std::vector<std::string> &dirs) {
  if (value.empty() || value[0] == '/' || value.find('/') == std::string::npos) return value;
  for (const auto &d : dirs)
    if (file_exists(join_path(d, value))) return join_path(d, value);
  return value;
}

bool installed(const std::string &program) {
  if (program.empty()) return false;
  if (program[0] == '/') return access(program.c_str(), X_OK) == 0;
  return !find_program(program).empty();
}

}  // namespace

std::vector<std::string> content_dirs() {
  std::vector<std::string> dirs;
  // The unit tests replace the system folders (colon separated).
  std::string only = env("SDE_WELCOME_DIRS");
  if (!only.empty()) {
    for (auto &d : split(only, ':'))
      if (!d.empty()) dirs.push_back(d);
    return dirs;
  }
  std::string own = env("SDE_WELCOME_DIR");
  if (!own.empty()) dirs.push_back(own);
  dirs.push_back(join_path(SDE_SYSCONFDIR, "SDE-Welcome"));
  dirs.push_back(SDE_WELCOME_DATADIR);
  return dirs;
}

bool KeyFile::load(const std::string &path) {
  std::string text;
  if (!read_file(path, text)) return false;
  parse(text);
  return true;
}

void KeyFile::parse(const std::string &text) {
  std::istringstream in(text);
  std::string line;
  decltype(groups)::value_type *group = nullptr;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[' && line.back() == ']') {
      std::string name = line.substr(1, line.size() - 2);
      group = nullptr;
      for (auto &g : groups)
        if (g.first == name) group = &g;
      if (!group) {
        groups.push_back({name, {}});
        group = &groups.back();
      }
      continue;
    }
    size_t eq = line.find('=');
    if (!group || eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    bool replaced = false;
    for (auto &kv : group->second)
      if (kv.first == key) {
        kv.second = value;
        replaced = true;
      }
    if (!replaced) group->second.push_back({key, value});
  }
}

bool KeyFile::has(const std::string &group, const std::string &key) const {
  for (const auto &g : groups)
    if (g.first == group)
      for (const auto &kv : g.second)
        if (kv.first == key) return true;
  return false;
}

static const std::string *raw_value(const KeyFile &kf, const std::string &group, const std::string &key) {
  for (const auto &g : kf.groups)
    if (g.first == group)
      for (const auto &kv : g.second)
        if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string KeyFile::get(const std::string &group, const std::string &key, const std::string &fallback) const {
  const std::string *v = raw_value(*this, group, key);
  return v ? unescape_value(*v) : fallback;
}

std::string KeyFile::localized(const std::string &group, const std::string &key) const {
  static const std::vector<std::string> langs = language_keys();
  for (const auto &l : langs) {
    std::string v = get(group, key + "[" + l + "]");
    if (!v.empty()) return v;
  }
  std::string v = get(group, key);
  std::string domain = get(group, "X-SDE-Gettext-Domain");
  if (!v.empty() && !domain.empty()) return dgettext(domain.c_str(), v.c_str());
  return v;
}

void KeyFile::merge(const KeyFile &other) {
  for (const auto &g : other.groups) {
    auto mine = std::find_if(groups.begin(), groups.end(), [&](const auto &x) { return x.first == g.first; });
    if (mine == groups.end()) {
      groups.push_back(g);
      continue;
    }
    for (const auto &kv : g.second) {
      auto it = std::find_if(mine->second.begin(), mine->second.end(),
                             [&](const auto &x) { return x.first == kv.first; });
      if (it != mine->second.end()) it->second = kv.second;
      else mine->second.push_back(kv);
    }
  }
}

Content load_content(const std::string &os_name) {
  auto dirs = content_dirs();
  KeyFile kf;
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {  // /etc and $SDE_WELCOME_DIR win
    KeyFile one;
    if (one.load(join_path(*it, "welcome.conf"))) kf.merge(one);
  }
  const std::string g = "Welcome";
  Content c;
  std::string title = kf.localized(g, "Title"), subtitle = kf.localized(g, "Subtitle");
  if (title.empty()) {
    c.title = os_name.empty() ? std::string(_("Welcome")) : format(_("Welcome to %s"), os_name.c_str());
  } else if (title.find("%OS%") != std::string::npos && os_name.empty()) {
    c.title = _("Welcome");
  } else {
    for (size_t at; (at = title.find("%OS%")) != std::string::npos;) title.replace(at, 4, os_name);
    c.title = title;
  }
  c.subtitle = kf.has(g, "Subtitle") ? subtitle : _("A few first steps to make this computer yours.");
  c.logo = resolve(kf.get(g, "Logo"), dirs);
  c.desktop_line = kf.get(g, "ShowDesktopLine", "true") != "false";
  auto number = [&](const char *key, int fallback, int lo, int hi) {
    std::string v = kf.get(g, key);
    int n = v.empty() ? fallback : atoi(v.c_str());
    return n <= 0 ? fallback : std::max(lo, std::min(hi, n));
  };
  c.width = number("Width", c.width, 420, 1600);
  c.height = number("Height", c.height, 360, 1200);
  c.columns = number("Columns", c.columns, 1, 4);
  return c;
}

std::vector<Step> load_steps() {
  // File name -> (content folder, path); the first folder that has a name wins.
  std::map<std::string, std::pair<std::string, std::string>> files;
  for (const auto &dir : content_dirs())
    for (const auto &name : list_dir(join_path(dir, "steps")))
      if (ends_with(name, ".desktop") && !files.count(name)) files[name] = {dir, join_path(dir, "steps/" + name)};

  // A step as shown, or false when it cannot be (hidden, not installed).
  auto make = [&](const std::string &name, Step &out, std::string &fallback, bool &no_display) {
    auto it = files.find(name);
    if (it == files.end()) return false;
    KeyFile kf;
    if (!kf.load(it->second.second) || !kf.has(kEntry, "Name")) return false;
    fallback = kf.get(kEntry, "X-SDE-Welcome-Fallback");
    no_display = kf.get(kEntry, "NoDisplay") == "true";
    if (kf.get(kEntry, "Hidden") == "true") {
      fallback.clear();
      return false;
    }
    std::vector<std::string> here = {it->second.first};
    out = Step();
    out.file = name;
    out.name = kf.localized(kEntry, "Name");
    out.comment = kf.localized(kEntry, "Comment");
    std::string icon = kf.get(kEntry, "Icon");
    if (!icon.empty()) out.icons.push_back(resolve(icon, here));
    for (const auto &i : list_of(kf.get(kEntry, "X-SDE-Fallback-Icons"))) out.icons.push_back(resolve(i, here));
    if (kf.get(kEntry, "Type") == "Link") {
      out.url = kf.get(kEntry, "URL");
      return !out.url.empty();
    }
    for (auto &a : split_command(kf.get(kEntry, "Exec"))) {
      if (a.size() == 2 && a[0] == '%' && std::string("fFuUick").find(a[1]) != std::string::npos) continue;
      out.argv.push_back(a == "%%" ? "%" : a);
    }
    if (out.argv.empty()) return false;
    out.argv[0] = resolve(out.argv[0], here);
    std::string try_exec = resolve(kf.get(kEntry, "TryExec"), here);
    return installed(try_exec.empty() ? out.argv[0] : try_exec);
  };

  std::vector<Step> steps;
  for (const auto &f : files) {
    Step step;
    std::string fallback;
    bool no_display = false;
    bool ok = make(f.first, step, fallback, no_display);
    if (no_display) continue;  // only used as another step's fallback
    // Follow the fallbacks (at most a few, they could loop).
    for (int depth = 0; !ok && !fallback.empty() && depth < 8; ++depth) {
      std::string next = fallback;
      bool ignored = false;
      ok = make(next, step, fallback, ignored);
    }
    if (ok) steps.push_back(step);
  }
  return steps;
}

const std::vector<Layout> &panel_layouts() {
  static const std::vector<Layout> list = {
      {"full", N_("Traditional"), N_("A full-width panel along the edge, like most desktops.")},
      {"short", N_("Centered"), N_("A shorter panel in the middle of the edge.")},
      {"floating", N_("Floating"), N_("A rounded panel that floats above the edge.")},
  };
  return list;
}

}  // namespace welcome
}  // namespace sde
