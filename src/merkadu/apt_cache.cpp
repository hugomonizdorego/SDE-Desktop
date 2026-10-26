// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "apt_cache.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "i18n.h"
#include "model.h"
#include "util.h"

#ifdef MERKADU_HAVE_APT
#include <apt-pkg/algorithms.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/depcache.h>
#include <apt-pkg/error.h>
#include <apt-pkg/init.h>
#include <apt-pkg/pkgcache.h>
#include <apt-pkg/pkgrecords.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/policy.h>
#endif

namespace sde {
namespace merkadu {
namespace apt {

namespace {

// --------------------------------------------------------------- thread

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<std::function<void()>> g_work;
bool g_thread = false;

void worker() {
  for (;;) {
    std::function<void()> fn;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_wake.wait(lock, [] { return !g_work.empty(); });
      fn = std::move(g_work.front());
      g_work.pop_front();
    }
    fn();
  }
}

}  // namespace

void queue(std::function<void()> work) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_work.push_back(std::move(work));
  if (!g_thread) {
    g_thread = true;
    std::thread(worker).detach();
  }
  g_wake.notify_one();
}

std::time_t lists_time() {
  std::string dir = env("MERKADU_APT_LISTS", "/var/lib/apt/lists");
  std::time_t newest = 0;
  for (auto &name : list_dir(dir)) {
    if (!ends_with(name, "_Release") && !ends_with(name, "_InRelease")) continue;
    struct stat st;
    if (stat(join_path(dir, name).c_str(), &st) == 0) newest = std::max(newest, std::time_t(st.st_mtime));
  }
  return newest;
}

#ifdef MERKADU_HAVE_APT

namespace {

bool g_init = false;
std::unique_ptr<pkgCacheFile> g_cache;

// The errors APT collected (warnings are dropped), one per line.
std::string take_errors() {
  std::string out;
  while (!_error->empty(GlobalError::NOTICE)) {
    std::string msg;
    bool error = _error->PopMessage(msg);
    if (error && !msg.empty()) out += (out.empty() ? "" : "\n") + msg;
  }
  _error->Discard();
  return out;
}

pkgCacheFile *cache(std::string *error) {
  if (g_cache) return g_cache.get();
  if (!g_init) {
    if (!pkgInitConfig(*_config) || !pkgInitSystem(*_config, _system)) {
      if (error) *error = take_errors();
      return nullptr;
    }
    g_init = true;
  }
  auto file = std::make_unique<pkgCacheFile>();
  // Read-only: no lock, so that it works as a user and while PackageKit
  // works. Without write access to /var/cache/apt, APT builds the cache in
  // memory.
  if (!file->Open(nullptr, false)) {
    if (error) *error = take_errors();
    if (error && error->empty()) *error = _("The package lists could not be read.");
    return nullptr;
  }
  take_errors();
  g_cache = std::move(file);
  return g_cache.get();
}

std::string str(const char *s) { return s ? s : ""; }

// "Debian testing/main" for a version's package file; "" for the dpkg status.
std::string file_origin(pkgCache::PkgFileIterator f) {
  std::string archive = str(f.Archive());
  if (archive == "now") return "";
  std::string origin = str(f.Origin());
  if (origin.empty()) origin = str(f.Label());
  if (origin.empty()) origin = str(f.Site());
  if (origin.empty()) origin = _("Local");
  std::string out = origin;
  if (!archive.empty()) out += " " + archive;
  std::string comp = str(f.Component());
  if (!comp.empty()) out += "/" + comp;
  return out;
}

std::string version_origin(pkgCache::VerIterator v) {
  for (pkgCache::VerFileIterator vf = v.FileList(); !vf.end(); ++vf) {
    std::string o = file_origin(vf.File());
    if (!o.empty()) return o;
  }
  return "";
}

const char *dep_type(unsigned char type) {
  switch (type) {
    case pkgCache::Dep::Depends: return "Depends";
    case pkgCache::Dep::PreDepends: return "Pre-Depends";
    case pkgCache::Dep::Recommends: return "Recommends";
    case pkgCache::Dep::Suggests: return "Suggests";
    case pkgCache::Dep::Conflicts: return "Conflicts";
    case pkgCache::Dep::DpkgBreaks: return "Breaks";
    case pkgCache::Dep::Replaces: return "Replaces";
    case pkgCache::Dep::Enhances: return "Enhances";
    case pkgCache::Dep::Obsoletes: return "Obsoletes";
    default: return "";
  }
}

std::string dep_text(pkgCache::DepIterator d) {
  std::string out = d.TargetPkg().FullName(true);
  if (d.TargetVer()) out += std::string(" (") + str(d.CompType()) + " " + str(d.TargetVer()) + ")";
  return out;
}

// What the long description says, as paragraphs: " ." lines separate them,
// lines indented further are kept as they are (lists, examples).
std::string description_paragraphs(const std::string &desc) {
  std::vector<std::string> lines = split(desc, '\n', true);
  std::string out, para;
  auto flush = [&] {
    if (para.empty()) return;
    out += (out.empty() ? "" : "\n\n") + para;
    para.clear();
  };
  for (size_t i = 1; i < lines.size(); ++i) {  // the first line is the summary
    const std::string &l = lines[i];
    if (trim(l) == ".") {
      flush();
    } else if (starts_with(l, "  ")) {
      para += (para.empty() ? "" : "\n") + trim(l);
    } else {
      std::string t = trim(l);
      if (t.empty()) continue;
      bool bullet = starts_with(t, "* ") || starts_with(t, "- ") || starts_with(t, "o ");
      para += para.empty() ? t : (bullet ? "\n" : " ") + t;
    }
  }
  flush();
  return out;
}

}  // namespace

bool available() { return true; }

void reload() { g_cache.reset(); }

Snapshot snapshot() {
  auto t0 = std::chrono::steady_clock::now();
  Snapshot snap;
  snap.lists_time = lists_time();
  pkgCacheFile *file = cache(&snap.error);
  if (!file) return snap;
  pkgCache *pc = file->GetPkgCache();
  pkgDepCache *dc = file->GetDepCache();
  pkgPolicy *policy = file->GetPolicy();
  if (!pc || !dc || !policy) {
    snap.error = take_errors();
    return snap;
  }
  pkgRecords records(*file);
  std::string native = _config->Find("APT::Architecture");
  snap.packages.reserve(pc->HeaderP->PackageCount);
  // The descriptions are read afterwards in the order of their files:
  // seeking in compressed lists means decompressing them again.
  std::vector<std::pair<size_t, pkgCache::DescFileIterator>> descriptions;
  for (pkgCache::PkgIterator p = pc->PkgBegin(); !p.end(); ++p) {
    pkgCache::VerIterator cur = p.CurrentVer();
    pkgCache::VerIterator cand = policy->GetCandidateVer(p);
    bool config_only = p->CurrentState == pkgCache::State::ConfigFiles;
    if (cur.end() && cand.end() && !config_only) continue;  // virtual, or nothing to install
    // Other architectures only when something of them is installed.
    if (str(p.Arch()) != native && cur.end() && !config_only) continue;
    pkgCache::VerIterator v = !cand.end() ? cand : !cur.end() ? cur : p.VersionList();
    if (v.end()) continue;
    Package pk;
    pk.name = p.FullName(true);
    pk.arch = str(v.Arch());
    pk.section = str(v.Section());
    pk.priority = v->Priority;
    pk.essential = (p->Flags & pkgCache::Flag::Essential) != 0;
    pk.installed = !cur.end();
    pk.config_only = config_only;
    if (!cur.end()) {
      pk.installed_version = cur.VerStr();
      pk.current_size = (long long)cur->InstalledSize;
    }
    pkgDepCache::StateCache &state = (*dc)[p];
    if (!cand.end()) {
      pk.candidate_version = cand.VerStr();
      pk.download_size = (long long)cand->Size;
      pk.installed_size = (long long)cand->InstalledSize;
      pk.origin = version_origin(cand);
      pk.upgradable = !cur.end() && cur != cand && state.Upgradable();
    }
    pk.automatic = (state.Flags & pkgCache::Flag::Auto) != 0;
    pk.broken = !cur.end() && state.NowBroken();
    pkgCache::DescIterator d = v.TranslatedDescription();
    if (!d.end()) descriptions.push_back({snap.packages.size(), d.FileList()});
    snap.packages.push_back(std::move(pk));
  }
  std::sort(descriptions.begin(), descriptions.end(), [](const auto &a, const auto &b) {
    if (a.second->File != b.second->File) return a.second->File < b.second->File;
    return a.second->Offset < b.second->Offset;
  });
  for (auto &d : descriptions)
    snap.packages[d.first].summary = clean_text(records.Lookup(d.second).ShortDesc(), 300);
  take_errors();
  sort_packages(snap.packages);
  snap.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return snap;
}

Details details(const std::string &name) {
  Details out;
  std::string error;
  pkgCacheFile *file = cache(&error);
  if (!file) return out;
  pkgCache *pc = file->GetPkgCache();
  pkgPolicy *policy = file->GetPolicy();
  pkgCache::PkgIterator p = pc->FindPkg(name);
  if (p.end()) return out;
  pkgCache::VerIterator cur = p.CurrentVer(), cand = policy->GetCandidateVer(p);
  pkgCache::VerIterator v = !cand.end() ? cand : !cur.end() ? cur : p.VersionList();
  if (v.end()) return out;
  out.found = true;
  out.name = p.FullName(true);
  out.version = v.VerStr();
  pkgRecords records(*file);
  pkgCache::DescIterator d = v.TranslatedDescription();
  if (!d.end()) {
    pkgRecords::Parser &rec = records.Lookup(d.FileList());
    out.description = clean_text(description_paragraphs(rec.LongDesc()), 16384);
  }
  pkgRecords::Parser &rec = records.Lookup(v.FileList());
  out.homepage = rec.Homepage();
  out.maintainer = rec.Maintainer();
  out.source_package = rec.SourcePkg();
  if (out.source_package.empty()) out.source_package = str(v.SourcePkgName());
  // Dependencies, or-groups on one line.
  for (pkgCache::DepIterator dep = v.DependsList(); !dep.end();) {
    pkgCache::DepIterator start, end;
    dep.GlobOr(start, end);  // moves |dep| past the group
    std::string type = dep_type(start->Type);
    if (type.empty()) continue;
    std::string text;
    for (pkgCache::DepIterator x = start;; ++x) {
      text += (text.empty() ? "" : " | ") + dep_text(x);
      if (x == end) break;
    }
    out.dependencies.push_back({type, text});
  }
  // Installed packages that need it.
  for (pkgCache::DepIterator rd = p.RevDependsList(); !rd.end() && out.required_by.size() < 200; ++rd) {
    if (rd->Type != pkgCache::Dep::Depends && rd->Type != pkgCache::Dep::PreDepends) continue;
    pkgCache::PkgIterator parent = rd.ParentPkg();
    if (parent.CurrentVer().end() || rd.ParentVer() != parent.CurrentVer()) continue;
    std::string n = parent.FullName(true);
    if (std::find(out.required_by.begin(), out.required_by.end(), n) == out.required_by.end())
      out.required_by.push_back(n);
  }
  std::sort(out.required_by.begin(), out.required_by.end());
  for (pkgCache::PrvIterator pr = v.ProvidesList(); !pr.end(); ++pr) out.provides.push_back(str(pr.Name()));
  for (pkgCache::VerIterator vi = p.VersionList(); !vi.end(); ++vi) {
    VersionInfo info;
    info.version = vi.VerStr();
    info.origin = version_origin(vi);
    if (info.origin.empty()) info.origin = _("installed");
    info.pin = policy->GetPriority(vi);
    info.installed = vi == cur;
    info.candidate = vi == cand;
    out.versions.push_back(info);
  }
  take_errors();
  return out;
}

ChangePlan simulate(const ChangeSet &marks) {
  ChangePlan plan;
  std::string error;
  pkgCacheFile *file = cache(&error);
  if (!file) {
    plan.error = error;
    return plan;
  }
  pkgCache *pc = file->GetPkgCache();
  pkgDepCache *dc = file->GetDepCache();
  {
    pkgDepCache::ActionGroup group(*dc);
    pkgProblemResolver fix(dc);
    // Like Synaptic's markings: each one protected, then APT's resolver
    // removes what the removals leave broken and adds what is missing.
    for (auto &kv : marks.all()) {
      pkgCache::PkgIterator p = pc->FindPkg(kv.first);
      if (p.end()) {
        plan.error = format(_("The package %s is not in the package lists."), kv.first.c_str());
        continue;
      }
      plan.marked.insert(p.FullName(true));
      fix.Clear(p);
      fix.Protect(p);
      if (kv.second == Mark::Remove) {
        fix.Remove(p);
        dc->MarkDelete(p, false);
      } else {
        dc->MarkInstall(p, true);
      }
    }
    if (dc->BrokenCount() > 0 && !fix.Resolve(true)) {
      std::string e = take_errors();
      if (plan.error.empty()) plan.error = e.empty() ? std::string(_("The dependencies cannot be resolved.")) : e;
    }
  }
  for (pkgCache::PkgIterator p = pc->PkgBegin(); !p.end(); ++p) {
    pkgDepCache::StateCache &s = (*dc)[p];
    std::string n = p.FullName(true);
    if (s.NewInstall())
      plan.install.push_back(n);
    else if (s.Upgrade())
      plan.upgrade.push_back(n);
    else if (s.Downgrade())
      plan.downgrade.push_back(n);
    else if (s.Delete())
      plan.remove.push_back(n);
  }
  if (dc->BrokenCount() > 0 && plan.error.empty()) {
    std::string broken;
    for (pkgCache::PkgIterator p = pc->PkgBegin(); !p.end(); ++p)
      if ((*dc)[p].InstBroken()) broken += (broken.empty() ? "" : ", ") + p.FullName(true);
    plan.error = format(_("These packages would be left with unmet dependencies: %s"), broken.c_str());
  }
  plan.download = (long long)dc->DebSize();
  plan.disk = (long long)dc->UsrSize();
  plan.valid = true;
  // Forget the simulation: the next one starts from what is installed.
  dc->Init(nullptr);
  take_errors();
  return plan;
}

#else  // without libapt-pkg

bool available() { return false; }
void reload() {}
Snapshot snapshot() {
  Snapshot s;
  s.error = _("This build of Merkadu cannot read the package lists (it was built without libapt-pkg).");
  return s;
}
Details details(const std::string &) { return Details(); }
ChangePlan simulate(const ChangeSet &) {
  ChangePlan p;
  p.error = _("This build of Merkadu cannot read the package lists (it was built without libapt-pkg).");
  return p;
}

#endif

}  // namespace apt
}  // namespace merkadu
}  // namespace sde
