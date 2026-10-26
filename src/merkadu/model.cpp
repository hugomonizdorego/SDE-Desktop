// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "model.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "i18n.h"
#include "json.h"
#include "packages.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

std::string g_distribution;
bool g_distribution_set = false;

bool contains(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

void add_unique(std::vector<std::string> &v, const std::string &s) {
  if (!s.empty() && !contains(v, s)) v.push_back(s);
}

std::string lower_ascii(std::string s) {
  for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool word_char(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

// Lower-case words of |text| (letters, digits and UTF-8 sequences).
std::vector<std::string> words_of(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : text) {
    if (word_char(c)) {
      cur += char(std::tolower(c));
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

// ------------------------------------------------------------- sources

const char *source_id(Source s) {
  switch (s) {
    case Source::Flatpak: return "flatpak";
    case Source::Snap: return "snap";
    default: return "debian";
  }
}

bool source_from_id(const std::string &id, Source *out) {
  std::string l = lower_ascii(id);
  if (l == "debian" || l == "deb" || l == "apt" || l == "packagekit")
    *out = Source::Debian;
  else if (l == "flatpak" || l == "flathub")
    *out = Source::Flatpak;
  else if (l == "snap" || l == "snapd")
    *out = Source::Snap;
  else
    return false;
  return true;
}

void set_distribution_name(const std::string &name) {
  g_distribution = name;
  g_distribution_set = true;
}

std::string source_label(Source s) {
  switch (s) {
    case Source::Flatpak: return "Flatpak";
    case Source::Snap: return "Snap";
    default: break;
  }
  if (!g_distribution_set) {
    // os-release NAME without "GNU/Linux": "Debian", "DebION".
    std::string name = trim(os_release("NAME"));
    for (const char *suffix : {" GNU/Linux", " Linux"})
      if (ends_with(name, suffix)) name = name.substr(0, name.size() - std::strlen(suffix));
    set_distribution_name(name);
  }
  return g_distribution.empty() ? std::string(_("System")) : g_distribution;
}

std::string origin_kind_label(Source s) {
  switch (s) {
    case Source::Flatpak: return _("Flatpak app");
    case Source::Snap: return _("Snap");
    default: return format(_("%s package"), source_label(s).c_str());
  }
}

// ---------------------------------------------------------------- apps

std::string Origin::key() const {
  std::string k = std::string(source_id(source)) + ":";
  if (source == Source::Flatpak) k += (installation.empty() ? std::string("system") : installation) + ":";
  return k + id;
}

std::string Origin::flatpak_app_id() const {
  auto parts = split(id, '/', true);
  return parts.size() >= 2 ? parts[1] : id;
}

bool App::installed() const { return installed_origin() != nullptr; }

bool App::has_update() const {
  for (auto &o : origins)
    if (o.installed && o.update) return true;
  return false;
}

bool App::has_source(Source s) const { return origin(s) != nullptr; }

const Origin *App::installed_origin() const {
  for (auto &o : origins)
    if (o.installed) return &o;
  return nullptr;
}

const Origin *App::main_origin() const {
  if (const Origin *o = installed_origin()) return o;
  return origins.empty() ? nullptr : &origins.front();
}

const Origin *App::origin(Source s) const {
  for (auto &o : origins)
    if (o.source == s) return &o;
  return nullptr;
}

Origin *App::origin(Source s) {
  for (auto &o : origins)
    if (o.source == s) return &o;
  return nullptr;
}

std::time_t App::newest_release() const {
  std::time_t newest = 0;
  for (auto &r : releases) newest = std::max(newest, r.timestamp);
  return newest;
}

bool App::is_app() const { return kind == "desktop-application" || kind == "web-application" || kind.empty(); }

void add_icon(App &app, const IconRef &icon) {
  if (icon.ref.empty()) return;
  if (std::find(app.icons.begin(), app.icons.end(), icon) != app.icons.end()) return;
  app.icons.push_back(icon);
  if (app.icon.empty()) app.icon = icon.ref;
}

std::vector<std::string> icon_candidates(const App &app, int px) {
  auto is_name = [](const std::string &r) {
    return !r.empty() && r[0] != '/' && !starts_with(r, "https://") && !starts_with(r, "http://") &&
           !starts_with(r, "snap-icon:");
  };
  auto is_file = [](const std::string &r) { return !r.empty() && r[0] == '/'; };
  std::vector<IconRef> all = app.icons;
  if (!app.icon.empty() && std::none_of(all.begin(), all.end(), [&](const IconRef &i) { return i.ref == app.icon; }))
    all.insert(all.begin(), IconRef{app.icon, 0});
  std::vector<std::string> out;
  for (auto &i : all)
    if (is_name(i.ref)) add_unique(out, i.ref);
  // Large enough (90 % of the size is fine), smallest first; files first.
  std::vector<IconRef> big, small;
  for (auto &i : all) {
    if (is_name(i.ref)) continue;
    int size = i.size > 0 ? i.size : 64;
    (size * 10 >= px * 9 ? big : small).push_back(IconRef{i.ref, size});
  }
  std::stable_sort(big.begin(), big.end(), [&](const IconRef &a, const IconRef &b) {
    if (a.size != b.size) return a.size < b.size;
    return is_file(a.ref) && !is_file(b.ref);
  });
  std::stable_sort(small.begin(), small.end(), [&](const IconRef &a, const IconRef &b) {
    if (a.size != b.size) return a.size > b.size;
    return is_file(a.ref) && !is_file(b.ref);
  });
  for (auto &i : big) add_unique(out, i.ref);
  for (auto &i : small) add_unique(out, i.ref);
  return out;
}

std::vector<std::string> fallback_icon_names(const App &app) {
  std::vector<std::string> out;
  if (app.kind == "font") out = {"font-x-generic", "preferences-desktop-font"};
  else if (app.kind == "addon" || app.kind == "codec") out = {"application-x-addon", "extension"};
  else if (app.kind == "console-application") out = {"utilities-terminal", "terminal"};
  // The main categories of the Desktop Menu Specification, most specific first.
  static const std::pair<const char *, const char *> categories[] = {
      {"Game", "applications-games"},           {"Graphics", "applications-graphics"},
      {"AudioVideo", "applications-multimedia"}, {"Audio", "applications-multimedia"},
      {"Video", "applications-multimedia"},      {"WebBrowser", "web-browser"},
      {"Email", "internet-mail"},                {"Network", "applications-internet"},
      {"Office", "applications-office"},          {"Development", "applications-development"},
      {"Science", "applications-science"},        {"Education", "applications-education"},
      {"Settings", "preferences-desktop"},        {"System", "applications-system"},
      {"Utility", "applications-utilities"},      {"Accessibility", "preferences-desktop-accessibility"},
  };
  for (auto &c : categories)
    if (contains(app.categories, c.first)) add_unique(out, c.second);
  if (!app.section.empty())
    for (auto &n : section_icon_names(section_base(app.section))) add_unique(out, n);
  for (const char *n : {"application-x-executable", "package-x-generic"}) add_unique(out, n);
  return out;
}

std::string normalize_id(const std::string &appstream_id) {
  std::string id = lower_ascii(trim(appstream_id));
  if (ends_with(id, ".desktop")) id.resize(id.size() - 8);
  return id;
}

std::string description_text(const std::string &markup) {
  // Plain text (snaps, some catalogs) keeps its own lines.
  if (markup.find('<') == std::string::npos) return trim(markup);
  std::string out, para;
  bool in_list = false, ordered = false;
  int number = 0;
  auto flush = [&] {
    std::string t = trim(para);
    para.clear();
    if (t.empty()) return;
    if (!out.empty()) out += in_list ? "\n" : "\n\n";
    out += t;
  };
  size_t i = 0;
  while (i < markup.size()) {
    char c = markup[i];
    if (c == '<') {
      size_t end = markup.find('>', i);
      if (end == std::string::npos) break;
      std::string tag = lower_ascii(markup.substr(i + 1, end - i - 1));
      i = end + 1;
      bool closing = !tag.empty() && tag[0] == '/';
      std::string name = closing ? tag.substr(1) : tag;
      size_t sp = name.find_first_of(" \t/");
      if (sp != std::string::npos) name.resize(sp);
      if (name == "p") {
        flush();
      } else if (name == "ul" || name == "ol") {
        flush();
        if (!closing) {
          if (!out.empty()) out += "\n";
          in_list = true;
          ordered = name == "ol";
          number = 0;
        } else {
          in_list = false;
        }
      } else if (name == "li") {
        if (!closing) {
          flush();
          para = ordered ? std::to_string(++number) + ". " : "\xE2\x80\xA2 ";
        } else {
          flush();
        }
      } else if (name == "br") {
        para += ' ';
      }
      continue;
    }
    if (c == '&') {
      size_t end = markup.find(';', i);
      if (end != std::string::npos && end - i <= 8) {
        std::string ent = markup.substr(i + 1, end - i - 1);
        const char *rep = nullptr;
        if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos") rep = "'";
        if (rep) {
          para += rep;
          i = end + 1;
          continue;
        }
      }
    }
    // Whitespace inside a paragraph collapses to one space.
    if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    if (c == ' ' && (para.empty() || para.back() == ' ')) {
      ++i;
      continue;
    }
    para += c;
    ++i;
  }
  flush();
  return out;
}

void merge_origin(App &app, const Origin &o) {
  for (auto &cur : app.origins) {
    if (cur.source != o.source) continue;
    // Flatpak: one origin per installation and ref (branches and the user
    // installation are origins of their own).
    if (o.source == Source::Flatpak &&
        ((cur.installation.empty() ? "system" : cur.installation) !=
             (o.installation.empty() ? "system" : o.installation) ||
         cur.id != o.id))
      continue;
    if (!o.id.empty()) cur.id = o.id;
    if (!o.remote.empty()) cur.remote = o.remote;
    if (!o.version.empty()) cur.version = o.version;
    if (o.installed) {
      cur.installed = true;
      if (!o.installed_version.empty()) cur.installed_version = o.installed_version;
    }
    if (o.update) cur.update = true;
    if (o.download_size >= 0) cur.download_size = o.download_size;
    if (o.installed_size >= 0) cur.installed_size = o.installed_size;
    if (!o.package_id.empty()) cur.package_id = o.package_id;
    if (!o.publisher.empty()) cur.publisher = o.publisher;
    if (o.verified) cur.verified = true;
    if (o.classic) cur.classic = true;
    if (!o.license.empty()) cur.license = o.license;
    return;
  }
  app.origins.push_back(o);
}

App &Catalog::add(App app) {
  if (app.id.empty()) app.id = normalize_id(app.appstream_id);
  auto it = by_id_.find(app.id);
  if (it == by_id_.end()) {
    apps_.push_back(std::move(app));
    index(apps_.size() - 1);
    return apps_.back();
  }
  App &cur = apps_[it->second];
  auto fill = [](std::string &dst, const std::string &src) {
    if (dst.empty()) dst = src;
  };
  fill(cur.appstream_id, app.appstream_id);
  fill(cur.name, app.name);
  fill(cur.summary, app.summary);
  fill(cur.description, app.description);
  // A local icon beats a remote one.
  if (cur.icon.empty() || (starts_with(cur.icon, "http") && !app.icon.empty() && !starts_with(app.icon, "http")))
    cur.icon = app.icon;
  for (auto &i : app.icons)
    if (std::find(cur.icons.begin(), cur.icons.end(), i) == cur.icons.end()) cur.icons.push_back(i);
  if (cur.kind.empty()) cur.kind = app.kind;
  fill(cur.developer, app.developer);
  fill(cur.license, app.license);
  fill(cur.homepage, app.homepage);
  fill(cur.bugtracker, app.bugtracker);
  fill(cur.help, app.help);
  fill(cur.donation, app.donation);
  fill(cur.section, app.section);
  for (auto &c : app.categories) add_unique(cur.categories, c);
  for (auto &k : app.keywords) add_unique(cur.keywords, k);
  for (auto &p : app.pkgnames) add_unique(cur.pkgnames, p);
  for (auto &d : app.desktop_ids) add_unique(cur.desktop_ids, d);
  if (cur.screenshots.empty()) cur.screenshots = app.screenshots;
  if (cur.releases.empty()) cur.releases = app.releases;
  for (auto &o : app.origins) merge_origin(cur, o);
  index(it->second);
  return cur;
}

void Catalog::index(size_t i) {
  const App &a = apps_[i];
  by_id_[a.id] = i;
  for (auto &o : a.origins) {
    std::string key = std::string(source_id(o.source)) + ":" +
                      (o.source == Source::Flatpak ? lower_ascii(o.flatpak_app_id()) : o.id);
    by_origin_[key] = i;
  }
  for (auto &d : a.desktop_ids) by_desktop_[normalize_id(d)] = i;
}

App *Catalog::find(const std::string &id) {
  auto it = by_id_.find(id);
  if (it == by_id_.end()) it = by_id_.find(normalize_id(id));
  return it == by_id_.end() ? nullptr : &apps_[it->second];
}

const App *Catalog::find(const std::string &id) const {
  return const_cast<Catalog *>(this)->find(id);
}

App *Catalog::find_origin(Source source, const std::string &id) {
  std::string key = std::string(source_id(source)) + ":";
  if (source == Source::Flatpak) {
    Origin o;
    o.source = source;
    o.id = id;
    key += lower_ascii(o.flatpak_app_id());
  } else {
    key += id;
  }
  auto it = by_origin_.find(key);
  if (it == by_origin_.end()) return nullptr;
  App &app = apps_[it->second];
  return app.has_source(source) ? &app : nullptr;
}

App *Catalog::find_desktop(const std::string &desktop_id) {
  auto it = by_desktop_.find(normalize_id(desktop_id));
  return it == by_desktop_.end() ? nullptr : &apps_[it->second];
}

void Catalog::reset_installed(Source source) {
  for (auto &a : apps_)
    for (auto &o : a.origins)
      if (o.source == source) {
        o.installed = false;
        o.update = false;
        o.installed_version.clear();
      }
}

void Catalog::prune(Source source, const std::set<std::string> &keep) {
  std::vector<App> kept;
  kept.reserve(apps_.size());
  for (auto &a : apps_) {
    a.origins.erase(std::remove_if(a.origins.begin(), a.origins.end(),
                                   [&](const Origin &o) {
                                     return o.source == source && !o.installed && !keep.count(o.key());
                                   }),
                    a.origins.end());
    if (!a.origins.empty()) kept.push_back(std::move(a));
  }
  apps_ = std::move(kept);
  by_id_.clear();
  by_origin_.clear();
  by_desktop_.clear();
  for (size_t i = 0; i < apps_.size(); ++i) index(i);
}

void Catalog::remove_empty() {
  bool any = false;
  for (auto &a : apps_) any = any || a.origins.empty();
  if (!any) return;
  apps_.erase(std::remove_if(apps_.begin(), apps_.end(), [](const App &a) { return a.origins.empty(); }),
              apps_.end());
  by_id_.clear();
  by_origin_.clear();
  by_desktop_.clear();
  for (size_t i = 0; i < apps_.size(); ++i) index(i);
}

void Catalog::clear() {
  apps_.clear();
  by_id_.clear();
  by_origin_.clear();
  by_desktop_.clear();
}

void Catalog::sort_origins(const std::vector<Source> &priority) {
  auto rank = [&](const Origin &o) {
    int r = int(priority.size());
    for (size_t i = 0; i < priority.size(); ++i)
      if (priority[i] == o.source) r = int(i);
    return (o.installed ? 0 : 100) + r * 2 + (o.installation == "user" ? 1 : 0);
  };
  for (auto &a : apps_)
    std::stable_sort(a.origins.begin(), a.origins.end(),
                     [&](const Origin &x, const Origin &y) { return rank(x) < rank(y); });
}

// ---------------------------------------------------------------- search

std::vector<std::string> query_words(const std::string &query) { return words_of(query); }

int match_score(const App &app, const std::vector<std::string> &words) {
  if (words.empty()) return 0;
  std::string name = lower_ascii(app.name);
  std::vector<std::string> name_words = words_of(app.name);
  std::string summary = lower_ascii(app.summary);
  std::string description;  // lowered only when needed
  int total = 0;
  for (const std::string &w : words) {
    int best = 0;
    if (name == w) {
      best = 100;
    } else if (starts_with(name, w)) {
      best = 70;
    } else {
      for (auto &nw : name_words)
        if (starts_with(nw, w)) best = std::max(best, 55);
      if (!best && name.find(w) != std::string::npos) best = 40;
    }
    if (best < 35) {
      if (app.id.find(w) != std::string::npos) best = std::max(best, 35);
      for (auto &k : app.keywords)
        if (starts_with(lower_ascii(k), w)) best = std::max(best, 32);
      for (auto &p : app.pkgnames)
        if (p.find(w) != std::string::npos) best = std::max(best, p == w ? 45 : 28);
      for (auto &o : app.origins)
        if (o.source == Source::Snap && o.id.find(w) != std::string::npos)
          best = std::max(best, o.id == w ? 45 : 28);
    }
    if (best < 15 && summary.find(w) != std::string::npos) best = 15;
    if (best < 5 && w.size() >= 3) {
      if (description.empty()) description = lower_ascii(app.description);
      if (description.find(w) != std::string::npos) best = 5;
    }
    if (best == 0) return 0;
    total += best;
  }
  // Applications before fonts, add-ons and tools; installed ones a little first.
  if (app.is_app()) total += 8;
  if (app.installed()) total += 2;
  return total;
}

void sort_by_name(std::vector<const App *> &apps) {
  std::stable_sort(apps.begin(), apps.end(), [](const App *a, const App *b) {
    std::string x = lower_ascii(a->name), y = lower_ascii(b->name);
    if (x != y) return x < y;
    return a->id < b->id;
  });
}

std::vector<const App *> search(const Catalog &catalog, const std::string &query, bool all_kinds) {
  std::vector<std::string> words = query_words(query);
  std::vector<std::pair<int, const App *>> scored;
  for (const App &a : catalog.apps()) {
    if (!all_kinds && !a.is_app()) continue;
    int s = match_score(a, words);
    if (s > 0) scored.push_back({s, &a});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const std::pair<int, const App *> &x,
                                                    const std::pair<int, const App *> &y) {
    if (x.first != y.first) return x.first > y.first;
    std::string a = lower_ascii(x.second->name), b = lower_ascii(y.second->name);
    if (a != b) return a < b;
    return x.second->id < y.second->id;
  });
  std::vector<const App *> out;
  out.reserve(scored.size());
  for (auto &s : scored) out.push_back(s.second);
  return out;
}

// ------------------------------------------------------------- packages

bool PackageId::installed() const {
  return data == "installed" || starts_with(data, "installed:") || starts_with(data, "auto:") ||
         starts_with(data, "manual:") || data == "auto" || data == "manual";
}

PackageId parse_package_id(const std::string &id) {
  PackageId p;
  auto f = split(id, ';', true);
  if (f.size() != 4 || f[0].empty()) return p;
  p.name = f[0];
  p.version = f[1];
  p.arch = f[2];
  p.data = f[3];
  p.valid = true;
  return p;
}

bool valid_package_name(const std::string &name) {
  size_t colon = name.find(':');
  if (colon != std::string::npos) {
    std::string arch = name.substr(colon + 1);
    if (arch.empty() || arch.size() > 32) return false;
    for (unsigned char c : arch)
      if (!(std::islower(c) || std::isdigit(c) || c == '-')) return false;
    return valid_package_name(name.substr(0, colon));
  }
  if (name.size() < 2 || name.size() > 128) return false;
  if (!std::islower(static_cast<unsigned char>(name[0])) && !std::isdigit(static_cast<unsigned char>(name[0])))
    return false;
  for (unsigned char c : name)
    if (!(std::islower(c) || std::isdigit(c) || c == '+' || c == '-' || c == '.')) return false;
  return true;
}

bool valid_snap_name(const std::string &name) {
  std::string base = name, key;
  size_t us = name.find('_');
  if (us != std::string::npos) {
    base = name.substr(0, us);
    key = name.substr(us + 1);
    if (key.empty() || key.size() > 10) return false;
    for (unsigned char c : key)
      if (!(std::islower(c) || std::isdigit(c))) return false;
  }
  if (base.empty() || base.size() > 40 || base.front() == '-' || base.back() == '-') return false;
  if (base.find("--") != std::string::npos) return false;
  bool letter = false;
  for (unsigned char c : base) {
    if (std::islower(c))
      letter = true;
    else if (!(std::isdigit(c) || c == '-'))
      return false;
  }
  return letter;
}

bool valid_flatpak_ref(const std::string &ref) {
  auto parts = split(ref, '/', true);
  if (parts.size() != 4 || (parts[0] != "app" && parts[0] != "runtime")) return false;
  if (ref.find("..") != std::string::npos) return false;
  for (size_t i = 1; i < 4; ++i) {
    if (parts[i].empty() || parts[i].size() > 255) return false;
    for (unsigned char c : parts[i])
      if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  // The id has at least three elements ("org.gimp.GIMP").
  return std::count(parts[1].begin(), parts[1].end(), '.') >= 2;
}

// --------------------------------------------------------------- history

std::time_t parse_local_time(const std::string &stamp) {
  struct tm tm = {};
  int y, mo, d, h, mi, s;
  if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 ||
      s > 60)
    return 0;
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  tm.tm_isdst = -1;
  std::time_t t = mktime(&tm);
  return t < 0 ? 0 : t;
}

std::time_t parse_rfc3339(const std::string &stamp) {
  int y, mo, d, h, mi, s;
  if (std::sscanf(stamp.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  struct tm tm = {};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  std::time_t t = timegm(&tm);
  // The zone after the seconds (and their fraction): Z or +hh:mm.
  size_t p = stamp.find_first_of("Zz+-", 19);
  if (p != std::string::npos && (stamp[p] == '+' || stamp[p] == '-')) {
    int zh = 0, zm = 0;
    if (std::sscanf(stamp.c_str() + p + 1, "%d:%d", &zh, &zm) >= 1) {
      long offset = zh * 3600L + zm * 60L;
      t += stamp[p] == '+' ? -offset : offset;
    }
  }
  return t;
}

namespace {

// "gimp:amd64 (2.10.38-1, automatic)" -> name, version, automatic.
struct AptItem {
  std::string name, version;
  bool automatic = false;
};

std::vector<AptItem> apt_items(const std::string &value, bool upgrade) {
  std::vector<AptItem> out;
  size_t i = 0;
  while (i < value.size()) {
    size_t open = value.find('(', i);
    if (open == std::string::npos) break;
    size_t close = value.find(')', open);
    if (close == std::string::npos) close = value.size();
    AptItem item;
    item.name = trim(value.substr(i, open - i));
    if (!item.name.empty() && item.name[0] == ',') item.name = trim(item.name.substr(1));
    size_t colon = item.name.find(':');
    if (colon != std::string::npos) item.name.resize(colon);
    std::vector<std::string> parts;
    for (auto &p : split(value.substr(open + 1, close - open - 1), ',')) parts.push_back(trim(p));
    if (!parts.empty() && parts.back() == "automatic") {
      item.automatic = true;
      parts.pop_back();
    }
    if (!parts.empty()) item.version = upgrade && parts.size() >= 2 ? parts[1] : parts[0];
    if (!item.name.empty()) out.push_back(item);
    i = close + 1;
  }
  return out;
}

void flush_apt_stanza(std::map<std::string, std::string> &st, std::vector<HistoryEntry> &out) {
  if (st.empty()) return;
  std::time_t when = parse_local_time(st["Start-Date"]);
  if (!when) {
    st.clear();
    return;
  }
  std::string requested = st["Requested-By"];
  size_t paren = requested.find(" (");
  if (paren != std::string::npos) requested.resize(paren);
  struct Kind {
    const char *key;
    Change change;
    bool upgrade;
  };
  const Kind kinds[] = {{"Install", Change::Install, false},  {"Reinstall", Change::Install, false},
                        {"Upgrade", Change::Update, true},    {"Downgrade", Change::Update, true},
                        {"Remove", Change::Remove, false},    {"Purge", Change::Remove, false}};
  // One entry per kind of change; Install and Reinstall (and Remove and
  // Purge) of one transaction are one entry.
  std::map<int, HistoryEntry> entries;
  for (auto &k : kinds) {
    auto it = st.find(k.key);
    if (it == st.end()) continue;
    HistoryEntry &e = entries[int(k.change)];
    e.when = when;
    e.source = Source::Debian;
    e.change = k.change;
    e.requested_by = requested;
    e.tool = st["Commandline"];
    if (st.count("Error")) {
      e.failed = true;
      e.error = st["Error"];
    }
    for (auto &item : apt_items(it->second, k.upgrade)) {
      if (item.automatic) {
        ++e.automatic;
        continue;
      }
      e.items.push_back(item.name);
      e.versions.push_back(item.version);
    }
  }
  for (auto &e : entries) out.push_back(std::move(e.second));
  st.clear();
}

}  // namespace

void parse_apt_history(const std::string &text, std::vector<HistoryEntry> &out) {
  std::map<std::string, std::string> stanza;
  for (const std::string &raw : split(text, '\n', true)) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line).empty()) {
      flush_apt_stanza(stanza, out);
      continue;
    }
    size_t colon = line.find(": ");
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    // A new Start-Date without a blank line starts a new stanza.
    if (key == "Start-Date" && stanza.count("Start-Date")) flush_apt_stanza(stanza, out);
    stanza[key] = line.substr(colon + 2);
  }
  flush_apt_stanza(stanza, out);
}

namespace {

// "%b %e %T" in the C locale ("Oct 20 10:15:42", "Oct  5 08:01:02"), in
// the current year unless that would be in the future.
std::time_t parse_flatpak_time(const std::string &stamp, std::time_t now) {
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char mon[8] = "";
  int d, h, mi, s;
  if (std::sscanf(stamp.c_str(), "%3s %d %d:%d:%d", mon, &d, &h, &mi, &s) != 5) return 0;
  int m = -1;
  for (int i = 0; i < 12; ++i)
    if (strcasecmp(mon, months[i]) == 0) m = i;
  if (m < 0) return 0;
  struct tm local = {};
  localtime_r(&now, &local);
  for (int back = 0; back < 2; ++back) {
    struct tm tm = {};
    tm.tm_year = local.tm_year - back;
    tm.tm_mon = m;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    std::time_t t = mktime(&tm);
    if (t <= now + 86400) return t;
  }
  return 0;
}

}  // namespace

void parse_flatpak_history(const std::string &text, std::vector<HistoryEntry> &out) {
  std::time_t now = std::time(nullptr);
  for (const std::string &line : split(text, '\n')) {
    auto f = split(line, '\t', true);
    if (f.size() < 4) continue;
    std::string change = trim(f[1]), ref = trim(f[2]);
    Change c;
    if (change == "deploy install")
      c = Change::Install;
    else if (change == "deploy update")
      c = Change::Update;
    else if (change == "uninstall")
      c = Change::Remove;
    else
      continue;
    if (!starts_with(ref, "app/")) continue;  // runtimes come and go with the apps
    HistoryEntry e;
    e.when = parse_flatpak_time(trim(f[0]), now);
    if (!e.when) continue;
    e.source = Source::Flatpak;
    e.change = c;
    e.items.push_back(ref);
    e.versions.push_back("");
    if (f.size() > 5) e.requested_by = trim(f[5]);
    if (f.size() > 6) e.tool = trim(f[6]);
    out.push_back(std::move(e));
  }
}

bool parse_snap_changes(const std::string &json, std::vector<HistoryEntry> &out) {
  Json root;
  if (!parse_json(json, root) || !root.is_object()) return false;
  const Json *result = root.get("result");
  if (!result || !result->is_array()) return false;
  for (const Json &ch : result->items) {
    std::string kind = ch.text("kind");
    Change c;
    if (kind == "install-snap")
      c = Change::Install;
    else if (kind == "refresh-snap" || kind == "auto-refresh")
      c = Change::Update;
    else if (kind == "remove-snap")
      c = Change::Remove;
    else
      continue;
    HistoryEntry e;
    e.source = Source::Snap;
    e.change = c;
    e.when = parse_rfc3339(ch.text("spawn-time"));
    if (!e.when) continue;
    std::string status = ch.text("status");
    if (status == "Error" || status == "Undone" || status == "Hold") {
      e.failed = true;
      e.error = ch.text("err");
    } else if (status != "Done") {
      continue;  // still running
    }
    const Json *data = ch.get("data");
    std::vector<std::string> names = data ? data->strings("snap-names") : std::vector<std::string>();
    if (names.empty()) {
      // "Install "firefox" snap": the name between the quotes of the summary.
      std::string summary = ch.text("summary");
      size_t a = summary.find('"'), b = summary.find('"', a + 1);
      if (a != std::string::npos && b != std::string::npos) names.push_back(summary.substr(a + 1, b - a - 1));
    }
    for (auto &n : names) {
      e.items.push_back(n);
      e.versions.push_back("");
    }
    if (e.items.empty()) continue;
    e.tool = kind == "auto-refresh" ? "snapd" : "";
    out.push_back(std::move(e));
  }
  return true;
}

void sort_history(std::vector<HistoryEntry> &entries) {
  std::stable_sort(entries.begin(), entries.end(),
                   [](const HistoryEntry &a, const HistoryEntry &b) { return a.when > b.when; });
}

long local_day(std::time_t when) {
  struct tm tm = {};
  localtime_r(&when, &tm);
  // Days since 1970-01-01 of the local date (proleptic Gregorian).
  int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

std::string day_label(std::time_t when, std::time_t now) {
  long day = local_day(when), today = local_day(now);
  if (day == today) return _("Today");
  if (day == today - 1) return _("Yesterday");
  struct tm tm = {}, now_tm = {};
  localtime_r(&when, &tm);
  localtime_r(&now, &now_tm);
  char buf[128];
  // The current week (from Monday): the day of the week.
  int weekday = (now_tm.tm_wday + 6) % 7;
  if (day < today && day >= today - weekday) {
    strftime(buf, sizeof buf, "%A", &tm);
    return buf;
  }
  // TRANSLATORS: a date in the current year (strftime format).
  if (tm.tm_year == now_tm.tm_year) strftime(buf, sizeof buf, _("%B %e"), &tm);
  // TRANSLATORS: a date in another year (strftime format).
  else strftime(buf, sizeof buf, _("%B %e, %Y"), &tm);
  std::string out = buf;
  // %e pads single digits with a space.
  size_t dbl = out.find("  ");
  if (dbl != std::string::npos) out.erase(dbl, 1);
  return out;
}

std::string time_label(std::time_t when) {
  struct tm tm = {};
  localtime_r(&when, &tm);
  char buf[32];
  strftime(buf, sizeof buf, "%H:%M", &tm);
  return buf;
}

// -------------------------------------------------------------- uninstall

namespace {

std::string desktop_key(const std::string &text, const std::string &key) {
  bool in_entry = false;
  for (const std::string &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      in_entry = line == "[Desktop Entry]";
      continue;
    }
    if (!in_entry) continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
  }
  return "";
}

bool has_desktop_entry(const std::string &text) {
  for (const std::string &raw : split(text, '\n'))
    if (trim(raw) == "[Desktop Entry]") return true;
  return false;
}

}  // namespace

Launcher classify_launcher(const std::string &path, const std::string &text, const std::string &home) {
  Launcher l;
  l.path = path;
  if (path.empty() || path[0] != '/' || !ends_with(path, ".desktop") || !has_desktop_entry(text)) return l;
  l.name = desktop_key(text, "Name");
  l.icon = desktop_key(text, "Icon");
  std::string flatpak = desktop_key(text, "X-Flatpak");
  std::string snap = desktop_key(text, "X-SnapInstanceName");
  std::string base = basename_of(path);
  std::string h = home;
  while (h.size() > 1 && h.back() == '/') h.pop_back();
  bool in_home = !h.empty() && h != "/" && starts_with(path, h + "/");
  if (!flatpak.empty()) {
    l.kind = LauncherKind::Flatpak;
    l.id = flatpak;
    l.flatpak_installation = in_home ? "user" : "system";
  } else if (!snap.empty() || starts_with(path, "/var/lib/snapd/desktop/applications/")) {
    l.kind = LauncherKind::Snap;
    if (snap.empty()) {
      // NAME_APP.desktop
      size_t us = base.find('_');
      snap = us == std::string::npos ? base.substr(0, base.size() - 8) : base.substr(0, us);
    }
    l.id = snap;
  } else if (in_home) {
    l.kind = LauncherKind::UserLauncher;
  } else {
    l.kind = LauncherKind::Package;
  }
  return l;
}

std::vector<std::string> parse_dpkg_owners(const std::string &output, const std::string &path) {
  std::vector<std::string> out;
  for (const std::string &line : split(output, '\n')) {
    if (starts_with(line, "diversion by")) continue;
    size_t sep = line.rfind(": ");
    if (sep == std::string::npos || trim(line.substr(sep + 2)) != path) continue;
    for (auto &p : split(line.substr(0, sep), ',')) {
      std::string name = trim(p);
      size_t colon = name.find(':');  // "libfoo:amd64"
      if (colon != std::string::npos) name.resize(colon);
      if (valid_package_name(name)) add_unique(out, name);
    }
  }
  return out;
}

long long parse_installed_sizes(const std::string &output) {
  long long kib = 0;
  for (const std::string &line : split(output, '\n')) {
    std::string t = trim(line);
    char *end = nullptr;
    long long v = std::strtoll(t.c_str(), &end, 10);
    if (!t.empty() && end && *end == '\0' && v > 0) kib += v;
  }
  return kib * 1024;
}

RemovalPlan plan_removal(const std::string &package, const std::vector<std::string> &removed,
                         const std::set<std::string> &protected_packages,
                         const std::set<std::string> &system_packages) {
  RemovalPlan plan;
  auto refuse = [&](const std::string &p) {
    plan.protected_package = p;
    plan.system = !protected_packages.count(p);
    plan.also_removed.clear();
    return plan;
  };
  if (protected_packages.count(package) || system_packages.count(package)) return refuse(package);
  for (const std::string &p : removed) {
    if (protected_packages.count(p) || system_packages.count(p)) return refuse(p);
    if (p != package) add_unique(plan.also_removed, p);
  }
  std::sort(plan.also_removed.begin(), plan.also_removed.end());
  plan.allowed = true;
  return plan;
}

std::string refusal_text(const RemovalPlan &plan, const std::string &name, const std::string &package) {
  const std::string &p = plan.protected_package;
  if (plan.system) {
    if (p == package)
      return format(_("%s (package %s) is part of the base system: the computer may not start or work without "
                      "it."),
                    name.c_str(), p.c_str());
    return format(_("Removing %s would also remove %s, a part of the base system that the computer needs to "
                    "start and work."),
                  name.c_str(), p.c_str());
  }
  if (p == package)
    return format(_("%s is part of the Selviana Desktop Environment (package %s). It is installed and removed "
                    "together with the desktop, not on its own."),
                  name.c_str(), p.c_str());
  return format(_("Removing %s would also remove %s, which the Selviana Desktop Environment needs."), name.c_str(),
                p.c_str());
}

std::set<std::string> default_protected_packages() { return {"sde", "sde-desktop", "merkadu"}; }

void parse_removal_query(const std::string &output, long long *freed, std::set<std::string> *system) {
  long long kib = 0;
  bool any = false;
  for (const std::string &line : split(output, '\n')) {
    auto f = split(line, '\t', true);
    if (f.size() < 4 || !valid_package_name(trim(f[0]))) continue;
    char *end = nullptr;
    long long v = std::strtoll(trim(f[1]).c_str(), &end, 10);
    if (end && *end == '\0' && v > 0) {
      kib += v;
      any = true;
    }
    if (lower_ascii(trim(f[2])) == "yes" || lower_ascii(trim(f[3])) == "required") system->insert(trim(f[0]));
  }
  *freed = any ? kib * 1024 : -1;
}

std::set<std::string> parse_system_packages(const std::string &output) {
  std::set<std::string> out;
  for (const std::string &line : split(output, '\n')) {
    auto f = split(line, '\t', true);
    if (f.size() < 3) continue;
    std::string name = trim(f[0]);
    if (!valid_package_name(name)) continue;
    if (lower_ascii(trim(f[1])) == "yes" || lower_ascii(trim(f[2])) == "required") out.insert(name);
  }
  return out;
}

std::string running_kernel_package(const std::string &release) {
  std::string r = trim(release);
  if (r.empty() || r.find('/') != std::string::npos) return "";
  std::string name = "linux-image-" + r;
  return valid_package_name(name) ? name : "";
}

// --------------------------------------------------------------- config

std::string data_dir() { return env("MERKADU_DATA_DIR", MERKADU_DATADIR); }

void KeyFile::parse(const std::string &text) {
  std::string section;
  for (const std::string &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line[0] == '[') {
      size_t end = line.find(']');
      section = end == std::string::npos ? "" : trim(line.substr(1, end - 1));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos || section.empty()) continue;
    values_[lower_ascii(section)][lower_ascii(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
  }
}

std::vector<std::string> KeyFile::keys(const std::string &section) const {
  std::vector<std::string> out;
  auto s = values_.find(lower_ascii(section));
  if (s != values_.end())
    for (auto &kv : s->second) out.push_back(kv.first);
  return out;
}

bool KeyFile::has(const std::string &section, const std::string &key) const {
  auto s = values_.find(lower_ascii(section));
  return s != values_.end() && s->second.count(lower_ascii(key));
}

std::string KeyFile::get(const std::string &section, const std::string &key, const std::string &fallback) const {
  auto s = values_.find(lower_ascii(section));
  if (s == values_.end()) return fallback;
  auto k = s->second.find(lower_ascii(key));
  return k == s->second.end() ? fallback : k->second;
}

std::vector<std::string> KeyFile::list(const std::string &section, const std::string &key) const {
  return split_list(get(section, key));
}

int KeyFile::get_int(const std::string &section, const std::string &key, int fallback) const {
  std::string v = get(section, key);
  char *end = nullptr;
  long n = std::strtol(v.c_str(), &end, 10);
  return !v.empty() && end && *end == '\0' ? int(n) : fallback;
}

Settings Settings::from(const KeyFile &kf) {
  Settings s;
  // [Highlights] and [Popular]; [Featured] and [Top] of 0.2 Alpha 2 still count.
  s.featured = kf.has("Highlights", "apps") ? kf.list("Highlights", "apps") : kf.list("Featured", "apps");
  s.top = kf.has("Popular", "apps") ? kf.list("Popular", "apps") : kf.list("Top", "apps");
  for (auto &key : kf.keys("Recommended")) s.recommended[key] = kf.list("Recommended", key);
  s.new_count = std::max(0, std::min(48, kf.get_int("New", "count", s.new_count)));
  auto flag = [&](const char *section, const char *key, bool fallback) {
    std::string v = lower_ascii(kf.get(section, key));
    if (v == "false" || v == "0" || v == "no") return false;
    if (v == "true" || v == "1" || v == "yes") return true;
    return fallback;
  };
  s.refresh_on_start = flag("Refresh", "on-start", s.refresh_on_start);
  s.refresh_minutes = std::max(0, std::min(24 * 60, kf.get_int("Refresh", "min-age", s.refresh_minutes)));
  s.setup_flathub = flag("Flatpak", "setup-flathub", s.setup_flathub);
  if (kf.has("Sources", "order")) {
    std::vector<Source> order;
    for (auto &id : kf.list("Sources", "order")) {
      Source src;
      if (source_from_id(id, &src) && std::find(order.begin(), order.end(), src) == order.end())
        order.push_back(src);
    }
    for (Source src : {Source::Debian, Source::Flatpak, Source::Snap})
      if (std::find(order.begin(), order.end(), src) == order.end()) order.push_back(src);
    s.order = order;
  }
  s.protected_packages = default_protected_packages();
  for (auto &p : kf.list("Uninstall", "protected"))
    if (valid_package_name(p)) s.protected_packages.insert(p);
  std::string hide = lower_ascii(kf.get("Snap", "hide-home-folder", "true"));
  s.hidden_snap_folder = !(hide == "false" || hide == "0" || hide == "no");
  std::string url = kf.get("Flatpak", "flathub-url");
  if (starts_with(url, "https://")) s.flathub_url = url;
  return s;
}

Settings load_settings() {
  KeyFile kf;
  std::string text;
  std::string fixed = env("MERKADU_CONFIG");
  if (!fixed.empty()) {
    if (read_file(fixed, text)) kf.parse(text);
    return Settings::from(kf);
  }
  std::vector<std::string> files = {join_path(data_dir(), "merkadu.conf")};
  std::vector<std::string> dirs = config_dirs();
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) files.push_back(*it + "/merkadu/merkadu.conf");
  files.push_back("/etc/merkadu/merkadu.conf");
  std::set<std::string> seen;
  for (auto &f : files) {
    if (!seen.insert(f).second) continue;
    if (read_file(f, text)) kf.parse(text);
  }
  return Settings::from(kf);
}

const App *resolve_reference(const Catalog &catalog, const std::string &ref) {
  auto &cat = const_cast<Catalog &>(catalog);
  std::string r = trim(ref);
  if (r.find('|') != std::string::npos) {
    for (auto &alt : split(r, '|'))
      if (const App *a = resolve_reference(catalog, alt)) return a;
    return nullptr;
  }
  if (starts_with(r, "deb:")) return cat.find_origin(Source::Debian, r.substr(4));
  if (starts_with(r, "snap:")) {
    if (const App *a = cat.find(r)) return a;
    return cat.find_origin(Source::Snap, r.substr(5));
  }
  if (starts_with(r, "flatpak:")) {
    std::string id = r.substr(8);
    if (const App *a = cat.find(id)) return a;
    return cat.find_origin(Source::Flatpak, "app/" + id + "/x/x");
  }
  return cat.find(r);
}

// ------------------------------------------------------------- formatting

std::string format_size(long long bytes) {
  if (bytes < 0) return _("Unknown");
  if (bytes < 1000) return format(_("%lld bytes"), bytes);
  const char *units[] = {"kB", "MB", "GB", "TB"};
  double v = double(bytes);
  int u = -1;
  while (v >= 1000 && u < 3) {
    v /= 1000;
    ++u;
  }
  return format(v >= 100 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

std::string format_count(long long n) {
  std::string digits = std::to_string(n < 0 ? -n : n), out;
  int k = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (k && k % 3 == 0) out.insert(out.begin(), ',');
    out.insert(out.begin(), *it);
    ++k;
  }
  return n < 0 ? "-" + out : out;
}

std::string apps_count(size_t n) {
  return n == 1 ? std::string(_("1 app")) : format(_("%s apps"), format_count(static_cast<long long>(n)).c_str());
}

std::vector<std::string> split_list(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ';' || c == ',') {
      if (!trim(cur).empty()) out.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
  return out;
}

std::string clean_text(const std::string &s, size_t max) {
  std::string out;
  out.reserve(std::min(s.size(), max));
  for (unsigned char c : s) {
    if (c < 0x20 && c != '\n' && c != '\t') continue;
    if (c == 0x7F) continue;
    out += char(c);
  }
  if (out.size() > max) {
    size_t cut = max;
    // Do not split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
    out += "\xE2\x80\xA6";
  }
  return out;
}

}  // namespace merkadu
}  // namespace sde
