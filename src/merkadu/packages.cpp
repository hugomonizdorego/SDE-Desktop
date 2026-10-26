// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "packages.h"

#include <algorithm>
#include <cctype>

#include "i18n.h"
#include "model.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

std::string lower_ascii(std::string s) {
  for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Debian's sections (packages.debian.org/sections), their names and icons.
struct SectionInfo {
  const char *id;
  const char *title;
  const char *icons;  // ';' separated
};
const SectionInfo kSections[] = {
    {"admin", N_("System Administration"), "applications-system;preferences-system"},
    {"cli-mono", N_("Mono/CLI"), "applications-development;text-x-script"},
    {"comm", N_("Communication"), "internet-chat;applications-internet"},
    {"database", N_("Databases"), "office-database;x-office-spreadsheet;applications-office"},
    {"debian-installer", N_("Installer Components"), "system-software-install;package-x-generic"},
    {"debug", N_("Debug Symbols"), "applications-development;text-x-script"},
    {"devel", N_("Development"), "applications-development;text-x-script"},
    {"doc", N_("Documentation"), "help-browser;x-office-document"},
    {"editors", N_("Editors"), "accessories-text-editor;text-editor"},
    {"education", N_("Education"), "applications-education;applications-science"},
    {"electronics", N_("Electronics"), "applications-electronics;applications-engineering;applications-science"},
    {"embedded", N_("Embedded Software"), "applications-electronics;applications-engineering;applications-system"},
    {"fonts", N_("Fonts"), "font-x-generic;preferences-desktop-font"},
    {"games", N_("Games"), "applications-games"},
    {"gnome", N_("GNOME"), "preferences-desktop;user-desktop"},
    {"gnu-r", N_("GNU R"), "applications-science;applications-development"},
    {"gnustep", N_("GNUstep"), "preferences-desktop;user-desktop"},
    {"golang", N_("Go"), "applications-development;text-x-script"},
    {"graphics", N_("Graphics"), "applications-graphics"},
    {"hamradio", N_("Amateur Radio"), "applications-electronics;applications-internet"},
    {"haskell", N_("Haskell"), "applications-development;text-x-script"},
    {"httpd", N_("Web Servers"), "network-server;applications-internet"},
    {"interpreters", N_("Interpreters"), "applications-development;text-x-script"},
    {"introspection", N_("GObject Introspection"), "applications-development;text-x-script"},
    {"java", N_("Java"), "applications-development;text-x-script"},
    {"javascript", N_("JavaScript"), "applications-development;text-x-script"},
    {"kde", N_("KDE"), "preferences-desktop;user-desktop"},
    {"kernel", N_("Kernels and Modules"), "applications-system;cpu"},
    {"libdevel", N_("Libraries - Development"), "applications-development;application-x-sharedlib"},
    {"libs", N_("Libraries"), "application-x-sharedlib;package-x-generic"},
    {"lisp", N_("Lisp"), "applications-development;text-x-script"},
    {"localization", N_("Localization"), "preferences-desktop-locale;preferences-desktop-keyboard"},
    {"mail", N_("Email"), "internet-mail;applications-internet"},
    {"math", N_("Mathematics"), "accessories-calculator;applications-science"},
    {"metapackages", N_("Metapackages"), "package-x-generic;applications-other"},
    {"misc", N_("Miscellaneous"), "applications-other;package-x-generic"},
    {"net", N_("Networking"), "network-workgroup;applications-internet"},
    {"news", N_("Newsgroups"), "internet-news-reader;applications-internet"},
    {"ocaml", N_("OCaml"), "applications-development;text-x-script"},
    {"oldlibs", N_("Old Libraries"), "application-x-sharedlib;package-x-generic"},
    {"otherosfs", N_("Other Systems and File Systems"), "drive-harddisk;applications-system"},
    {"perl", N_("Perl"), "applications-development;text-x-script"},
    {"php", N_("PHP"), "applications-development;text-x-script"},
    {"python", N_("Python"), "applications-development;text-x-script"},
    {"ruby", N_("Ruby"), "applications-development;text-x-script"},
    {"rust", N_("Rust"), "applications-development;text-x-script"},
    {"science", N_("Science"), "applications-science"},
    {"shells", N_("Shells"), "utilities-terminal;terminal"},
    {"sound", N_("Sound"), "audio-x-generic;applications-multimedia"},
    {"tasks", N_("Tasks"), "package-x-generic;applications-other"},
    {"tex", N_("TeX"), "x-office-document;accessories-text-editor"},
    {"text", N_("Text Processing"), "accessories-text-editor;x-office-document"},
    {"utils", N_("Utilities"), "applications-utilities;applications-accessories"},
    {"vcs", N_("Version Control"), "applications-development;text-x-script"},
    {"video", N_("Video"), "video-x-generic;applications-multimedia"},
    {"virtual", N_("Virtual Packages"), "package-x-generic"},
    {"web", N_("Web Software"), "applications-internet;web-browser"},
    {"x11", N_("X Window System"), "preferences-desktop-display;video-display"},
    {"xfce", N_("Xfce"), "preferences-desktop;user-desktop"},
    {"zope", N_("Zope/Plone"), "applications-internet"},
};

const SectionInfo *section_info(const std::string &base) {
  for (auto &s : kSections)
    if (base == s.id) return &s;
  return nullptr;
}

bool word_char(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

std::vector<std::string> words_of(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : text) {
    // Package names keep their '+', '-' and '.': "g++", "python3-gi".
    if (word_char(c) || c == '+' || c == '-' || c == '.') {
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

// --------------------------------------------------------------- sections

std::string section_base(const std::string &section) {
  size_t slash = section.rfind('/');
  return slash == std::string::npos ? section : section.substr(slash + 1);
}

std::string section_component(const std::string &section) {
  size_t slash = section.rfind('/');
  return slash == std::string::npos ? std::string("main") : section.substr(0, slash);
}

std::string section_title(const std::string &base) {
  if (base.empty()) return _("Other");
  if (const SectionInfo *s = section_info(base)) return _(s->title);
  return base;
}

std::vector<std::string> section_icon_names(const std::string &base) {
  if (const SectionInfo *s = section_info(base)) return split(s->icons, ';');
  return {"package-x-generic"};
}

// ----------------------------------------------------------------- marks

void ChangeSet::mark(const std::string &name, Mark m) {
  if (m == Mark::Unmarked)
    marks_.erase(name);
  else
    marks_[name] = m;
}

Mark ChangeSet::get(const std::string &name) const {
  auto it = marks_.find(name);
  return it == marks_.end() ? Mark::Unmarked : it->second;
}

size_t ChangeSet::count(Mark m) const {
  size_t n = 0;
  for (auto &kv : marks_) n += kv.second == m;
  return n;
}

std::vector<std::string> ChangeSet::names(Mark m) const {
  std::vector<std::string> out;
  for (auto &kv : marks_)
    if (kv.second == m) out.push_back(kv.first);
  return out;
}

std::vector<std::pair<std::string, bool>> refused_removals(const ChangePlan &plan,
                                                           const std::set<std::string> &system,
                                                           const std::set<std::string> &protected_packages) {
  std::vector<std::pair<std::string, bool>> out;
  for (auto &p : plan.remove) {
    std::string base = p.substr(0, p.find(':'));
    if (protected_packages.count(p) || protected_packages.count(base))
      out.push_back({p, false});
    else if (system.count(p) || system.count(base))
      out.push_back({p, true});
  }
  return out;
}

std::string plan_counts(const ChangePlan &plan) {
  std::vector<std::string> parts;
  if (!plan.install.empty())
    parts.push_back(format(_("%d to install"), int(plan.install.size())));
  if (!plan.upgrade.empty())
    parts.push_back(format(_("%d to update"), int(plan.upgrade.size())));
  if (!plan.downgrade.empty())
    parts.push_back(
        format(_("%d to downgrade"), int(plan.downgrade.size())));
  if (!plan.remove.empty())
    parts.push_back(format(_("%d to remove"), int(plan.remove.size())));
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
  return out;
}

// ---------------------------------------------------------------- filters

const char *package_status_id(PackageStatus s) {
  switch (s) {
    case PackageStatus::Installed: return "installed";
    case PackageStatus::Upgradable: return "updates";
    case PackageStatus::NotInstalled: return "available";
    case PackageStatus::Marked: return "marked";
    case PackageStatus::ConfigOnly: return "config";
    default: return "all";
  }
}

bool package_status_from_id(const std::string &id, PackageStatus *out) {
  for (int i = 0; i < kPackageStatusCount; ++i)
    if (id == package_status_id(PackageStatus(i))) {
      *out = PackageStatus(i);
      return true;
    }
  return false;
}

bool status_matches(const Package &p, PackageStatus s, const ChangeSet &marks) {
  switch (s) {
    case PackageStatus::Installed: return p.installed;
    case PackageStatus::Upgradable: return p.installed && p.upgradable;
    case PackageStatus::NotInstalled: return !p.installed && !p.candidate_version.empty();
    case PackageStatus::Marked: return marks.get(p.name) != Mark::Unmarked;
    case PackageStatus::ConfigOnly: return p.config_only;
    default: return true;
  }
}

int package_match_score(const Package &p, const std::vector<std::string> &words) {
  if (words.empty()) return 0;
  std::string summary;  // lowered only when needed
  int total = 0;
  for (const std::string &w : words) {
    int best = 0;
    if (p.name == w)
      best = 100;
    else if (starts_with(p.name, w))
      best = 60;
    else if (p.name.find(w) != std::string::npos)
      best = 35;
    if (best < 10) {
      if (summary.empty()) summary = lower_ascii(p.summary);
      if (summary.find(w) != std::string::npos) best = 10;
    }
    if (best == 0) return 0;
    total += best;
  }
  if (p.installed) total += 1;
  return total;
}

std::vector<size_t> filter_packages(const std::vector<Package> &packages, const PackageFilter &f,
                                    const ChangeSet &marks) {
  std::vector<std::string> words = words_of(f.query);
  std::vector<std::pair<int, size_t>> scored;
  std::vector<size_t> out;
  for (size_t i = 0; i < packages.size(); ++i) {
    const Package &p = packages[i];
    if (!f.section.empty() && section_base(p.section) != f.section) continue;
    if (!status_matches(p, f.status, marks)) continue;
    if (words.empty()) {
      out.push_back(i);
      continue;
    }
    int s = package_match_score(p, words);
    if (s > 0) scored.push_back({s, i});
  }
  if (words.empty()) return out;
  std::stable_sort(scored.begin(), scored.end(),
                   [](const std::pair<int, size_t> &a, const std::pair<int, size_t> &b) { return a.first > b.first; });
  out.reserve(scored.size());
  for (auto &s : scored) out.push_back(s.second);
  return out;
}

std::vector<SectionCount> section_counts(const std::vector<Package> &packages, PackageStatus status,
                                         const ChangeSet &marks) {
  std::map<std::string, size_t> counts;
  for (auto &p : packages)
    if (status_matches(p, status, marks)) ++counts[section_base(p.section)];
  std::vector<SectionCount> out;
  for (auto &kv : counts) out.push_back({kv.first, section_title(kv.first), kv.second});
  std::sort(out.begin(), out.end(), [](const SectionCount &a, const SectionCount &b) {
    std::string x = lower_ascii(a.title), y = lower_ascii(b.title);
    return x != y ? x < y : a.id < b.id;
  });
  return out;
}

void sort_packages(std::vector<Package> &packages) {
  std::sort(packages.begin(), packages.end(), [](const Package &a, const Package &b) {
    if (a.name != b.name) return a.name < b.name;
    return a.arch < b.arch;
  });
}

}  // namespace merkadu
}  // namespace sde
