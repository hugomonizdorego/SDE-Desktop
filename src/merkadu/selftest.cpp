// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// `merkadu --self-test`: loads what the window would show, without a
// display, and says what it found: the AppStream catalog (and what is
// missing for it: the appstream package, a refresh of the package lists,
// the unpacked icons), the Flatpak remotes, every package APT knows, and
// whether the apps named with --expect are there with an icon. CI runs it
// on Debian testing with the real archive and Flathub.
//
//   merkadu --self-test [--expect REF[,REF...]] [--expect-flatpak N] [--list]
//                       [--simulate install:PKG,remove:PKG,update:PKG]
//
// REF is an app reference as in merkadu.conf (AppStream id, deb:PACKAGE,
// flatpak:APPID; alternatives with '|'). Exit status 0 when everything
// expected was found, 1 otherwise.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>

#include "appstream_catalog.h"
#include "apt_cache.h"
#include "build_config.h"
#include "i18n.h"
#include "icons.h"
#include "model.h"
#include "packages.h"
#include "util.h"
#include "window.h"

#ifdef MERKADU_HAVE_FLATPAK
#include <flatpak.h>
#endif

namespace sde {
namespace merkadu {

namespace {

std::string when(std::time_t t) {
  if (t <= 0) return "never";
  char buf[64];
  struct tm tm = {};
  localtime_r(&t, &tm);
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

// The icon the window would draw first, and what kind it is; "" when the
// app would get a generic icon.
std::string usable_icon(const App &a, std::string *kind) {
  for (auto &ref : icon_candidates(a, 64)) {
    if (ref[0] == '/') {
      if (file_exists(ref)) {
        *kind = "file";
        return ref;
      }
    } else if (starts_with(ref, "https://")) {
      *kind = "url";
      return ref;
    } else if (!starts_with(ref, "snap-icon:") && !IconLoader::lookup(ref, 64).empty()) {
      *kind = "theme";
      return ref;
    }
  }
  return "";
}

std::vector<std::string> flatpak_remotes() {
  std::vector<std::string> out;
#ifdef MERKADU_HAVE_FLATPAK
  GError *err = nullptr;
  FlatpakInstallation *inst = flatpak_installation_new_system(nullptr, &err);
  g_clear_error(&err);
  if (!inst) return out;
  if (GPtrArray *remotes = flatpak_installation_list_remotes(inst, nullptr, &err)) {
    for (guint i = 0; i < remotes->len; ++i) {
      FlatpakRemote *r = FLATPAK_REMOTE(g_ptr_array_index(remotes, i));
      if (!flatpak_remote_get_disabled(r)) out.push_back(flatpak_remote_get_name(r));
    }
    g_ptr_array_unref(remotes);
  }
  g_clear_error(&err);
  g_object_unref(inst);
#endif
  return out;
}

}  // namespace

int run_self_test(const std::vector<std::string> &args) {
  std::vector<std::string> expect;
  int expect_flatpak = 0;
  bool list = false;
  ChangeSet marks;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--expect" && i + 1 < args.size())
      for (auto &r : split_list(args[++i])) expect.push_back(r);
    else if (args[i] == "--expect-flatpak" && i + 1 < args.size())
      expect_flatpak = std::atoi(args[++i].c_str());
    else if (args[i] == "--list")
      list = true;
    else if (args[i] == "--simulate" && i + 1 < args.size())
      for (auto &m : split_list(args[++i])) {
        // install:NAME, remove:NAME, update:NAME
        size_t colon = m.find(':');
        if (colon == std::string::npos) continue;
        std::string what = m.substr(0, colon), name = m.substr(colon + 1);
        marks.mark(name, what == "remove" ? Mark::Remove : what == "update" ? Mark::Upgrade : Mark::Install);
      }
  }
  int problems = 0;
  IconLoader::set_theme("auto", false);
  printf("Merkadu %s self-test\n\n", SDE_VERSION);

  CatalogData data = load_appstream();
  const CatalogSources &s = data.sources;
  Catalog catalog;
  for (auto &a : data.apps) catalog.add(a);
  printf("AppStream catalog: %zu apps (%d Debian, %d Flatpak components) in %.1f s\n", catalog.size(), data.debian,
         data.flatpak, data.seconds);
  if (!data.error.empty()) printf("  error: %s\n", data.error.c_str());
  printf("  APT downloads the catalog (appstream's 50appstream): %s\n", s.appstream_apt ? "yes" : "NO");
  printf("  DEP-11 catalogs: %d downloaded, %d read by AppStream, %zu read from Merkadu's own links\n", s.dep11,
         s.dep11_linked, s.unlinked.size());
  printf("  icon archives downloaded: %d; catalogs with unpacked icons: %d; other catalog files: %d\n", s.dep11_icons,
         s.icon_origins, s.other_catalogs);
  printf("  last refresh of the package lists: %s\n", when(s.lists_time).c_str());
  std::string remotes, with_data;
  for (auto &r : flatpak_remotes()) remotes += (remotes.empty() ? "" : ", ") + r;
  for (auto &r : s.flatpak_remotes) with_data += (with_data.empty() ? "" : ", ") + r;
  printf("  Flatpak remotes: %s; with AppStream data: %s\n", remotes.empty() ? "none" : remotes.c_str(),
         with_data.empty() ? "none" : with_data.c_str());
  int files = 0, urls = 0, themed = 0, none = 0, flatpak_apps = 0, flatpak_icons = 0;
  for (auto &a : catalog.apps()) {
    std::string kind;
    std::string icon = usable_icon(a, &kind);
    if (kind == "file") ++files;
    else if (kind == "url") ++urls;
    else if (kind == "theme") ++themed;
    else ++none;
    if (a.has_source(Source::Flatpak)) {
      ++flatpak_apps;
      if (!icon.empty()) ++flatpak_icons;
    }
    if (list)
      printf("  %s\t%s\t%s\t%s\n", a.id.c_str(), a.name.c_str(), a.has_source(Source::Flatpak) ? "flatpak" : "debian",
             icon.empty() ? "-" : icon.c_str());
  }
  printf("  icons: %d files, %d downloaded from the catalog's media, %d of the icon theme, %d generic\n", files, urls,
         themed, none);

  if (apt::available()) {
    apt::Snapshot snap = apt::snapshot();
    size_t installed = 0, updates = 0;
    for (auto &p : snap.packages) {
      installed += p.installed;
      updates += p.upgradable;
    }
    size_t sections = section_counts(snap.packages, PackageStatus::All, ChangeSet()).size();
    printf("\nPackages (libapt-pkg): %zu packages, %zu installed, %zu with an update, %zu sections in %.1f s\n",
           snap.packages.size(), installed, updates, sections, snap.seconds);
    if (!snap.error.empty()) {
      printf("  error: %s\n", snap.error.c_str());
      ++problems;
    }
    if (!marks.empty()) {
      // What APT would do with the marked changes (All Software's summary).
      ChangePlan plan = apt::simulate(marks);
      auto list_of = [](const std::vector<std::string> &v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 30; ++i) s += (i ? " " : "") + v[i];
        if (v.size() > 30) s += " ...";
        return s;
      };
      printf("  simulation: %s; download %s, disk %s%s\n", plan_counts(plan).c_str(), format_size(plan.download).c_str(),
             plan.disk < 0 ? ("-" + format_size(-plan.disk)).c_str() : format_size(plan.disk).c_str(),
             plan.error.empty() ? "" : ("; error: " + plan.error).c_str());
      if (!plan.install.empty()) printf("    install: %s\n", list_of(plan.install).c_str());
      if (!plan.upgrade.empty()) printf("    update: %s\n", list_of(plan.upgrade).c_str());
      if (!plan.remove.empty()) printf("    remove: %s\n", list_of(plan.remove).c_str());
      std::set<std::string> system;
      for (auto &p : snap.packages)
        if (p.system() && p.installed) system.insert(p.name);
      for (auto &r : refused_removals(plan, system, default_protected_packages()))
        printf("    refused: %s (%s)\n", r.first.c_str(), r.second ? "the system needs it" : "the desktop needs it");
    }
  } else {
    printf("\nPackages: this build has no libapt-pkg\n");
  }

  if (!expect.empty() || expect_flatpak > 0) printf("\nExpected:\n");
  for (auto &ref : expect) {
    const App *a = resolve_reference(catalog, ref);
    if (!a) {
      printf("  %s: MISSING\n", ref.c_str());
      ++problems;
      continue;
    }
    std::string kind, icon = usable_icon(*a, &kind);
    std::string from;
    for (auto &o : a->origins) from += (from.empty() ? "" : ", ") + std::string(source_id(o.source)) + ":" + o.id;
    printf("  %s: %s (%s) from %s, icon: %s\n", ref.c_str(), a->name.c_str(), a->appstream_id.c_str(), from.c_str(),
           icon.empty() ? "NONE" : (kind + " " + icon).c_str());
    if (icon.empty()) ++problems;
  }
  if (expect_flatpak > 0) {
    bool ok = flatpak_apps >= expect_flatpak && flatpak_icons > 0;
    printf("  Flatpak apps: %d with %d icons (at least %d expected)%s\n", flatpak_apps, flatpak_icons, expect_flatpak,
           ok ? "" : ": MISSING");
    if (!ok) ++problems;
  }
  printf("\nResult: %s\n", problems ? format("%d problems", problems).c_str() : "OK");
  return problems ? 1 : 0;
}

}  // namespace merkadu
}  // namespace sde
