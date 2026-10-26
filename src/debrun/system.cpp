// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "system.h"

#include <fcntl.h>
#include <poll.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "i18n.h"
#include "util.h"

extern "C" char **environ;

namespace sde {
namespace debrun {

namespace {

// The architecture the dependencies of |p| must be built for.
std::string dependency_arch(const Package &p, const System &s) {
  return p.architecture == "all" || p.architecture.empty() ? s.native_arch : p.architecture;
}

bool arch_compatible(const Installed &i, const Relation &r, const std::string &want) {
  if (i.arch == "all" || i.arch == want || want.empty()) return true;
  if (i.multi_arch == "foreign") return true;
  return r.arch_qualifier == "any" && i.multi_arch == "allowed";
}

// "pkg : Depends: x (>= 2) but it is not installable" -> the part after the
// name of |r| ("but it is not installable"), "" when |r| is not named.
std::string unmet_reason(const std::vector<std::string> &unmet, const std::string &name) {
  for (const auto &u : unmet) {
    size_t colon = u.find(" : ");
    std::string rest = colon == std::string::npos ? u : u.substr(colon + 3);
    for (size_t pos = rest.find(name); pos != std::string::npos; pos = rest.find(name, pos + 1)) {
      bool start = pos == 0 || rest[pos - 1] == ' ';
      size_t end = pos + name.size();
      bool stop = end >= rest.size() || rest[end] == ' ' || rest[end] == ':';
      if (!start || !stop) continue;
      size_t but = rest.find(" but ", end);
      size_t next_or = rest.find(" or ", end);
      if (but != std::string::npos && (next_or == std::string::npos || but < next_or))
        return trim(rest.substr(but + 1, (next_or == std::string::npos ? rest.size() : next_or) - but - 1));
      return "missing";
    }
  }
  return "";
}

const AptItem *planned(const AptPlan &plan, const std::string &name) {
  for (const auto &i : plan.install)
    if (i.name == name || starts_with(i.name, name + ":")) return &i;
  return nullptr;
}

bool removed(const AptPlan &plan, const std::string &name) {
  for (const auto &i : plan.remove)
    if (i.name == name || starts_with(i.name, name + ":")) return true;
  return false;
}

// "Inst sl [1.0] (5.02-1 Ubuntu:24.04/noble [amd64])"
bool parse_item(const std::string &line, AptItem *out) {
  std::string rest = trim(line.substr(5));
  size_t sp = rest.find(' ');
  out->name = rest.substr(0, sp);
  rest = sp == std::string::npos ? "" : trim(rest.substr(sp + 1));
  if (!rest.empty() && rest[0] == '[') {
    size_t close = rest.find(']');
    if (close == std::string::npos) return false;
    out->old_version = rest.substr(1, close - 1);
    rest = trim(rest.substr(close + 1));
  }
  if (!rest.empty() && rest[0] == '(') {
    size_t close = rest.rfind(')');
    if (close == std::string::npos) return false;
    std::vector<std::string> words;
    for (auto &w : split(rest.substr(1, close - 1), ' '))
      if (!w.empty()) words.push_back(w);
    if (!words.empty()) {
      out->version = words.front();
      words.erase(words.begin());
    }
    if (!words.empty() && words.back().size() > 2 && words.back().front() == '[' && words.back().back() == ']') {
      out->arch = words.back().substr(1, words.back().size() - 2);
      words.pop_back();
    }
    for (auto &w : words) out->origin += (out->origin.empty() ? "" : " ") + w;
  }
  return !out->name.empty();
}

}  // namespace

// ================================================================ dpkg

std::vector<const Installed *> System::find(const std::string &name) const {
  std::vector<const Installed *> out;
  auto range = by_name.equal_range(name);
  for (auto it = range.first; it != range.second; ++it)
    if (packages[it->second].present()) out.push_back(&packages[it->second]);
  return out;
}

void parse_status(const std::string &text, System *out) {
  out->packages.clear();
  out->by_name.clear();
  out->providers.clear();
  for (const Paragraph &p : parse_paragraphs(text)) {
    Installed i;
    i.name = p.get("Package");
    if (i.name.empty()) continue;
    i.version = p.get("Version");
    i.arch = p.get("Architecture");
    i.multi_arch = p.get("Multi-Arch");
    std::vector<std::string> status;
    for (auto &w : split(p.get("Status"), ' '))
      if (!w.empty()) status.push_back(w);
    if (status.size() == 3) {
      i.want = status[0];
      i.state = status[2];
    }
    i.essential = lower(p.get("Essential")) == "yes";
    i.is_protected = lower(p.get("Protected")) == "yes";
    std::vector<Alternatives> provides;
    parse_relations(p.get("Provides"), &provides);
    for (auto &alts : provides)
      for (auto &r : alts) i.provides.push_back(r);
    out->packages.push_back(std::move(i));
  }
  for (size_t n = 0; n < out->packages.size(); ++n) {
    out->by_name.emplace(out->packages[n].name, n);
    for (const auto &r : out->packages[n].provides) out->providers.emplace(r.name, n);
  }
}

bool load_system(System *out, std::string *error) {
  std::string path = env("DEBRUN_DPKG_STATUS", "/var/lib/dpkg/status"), text;
  out->loaded = read_file(path, text);
  if (!out->loaded) {
    if (error) *error = format(_("The list of installed packages (%s) cannot be read: is this a Debian system?"),
                               path.c_str());
    return false;
  }
  parse_status(text, out);
  std::string archs = env("DEBRUN_ARCHITECTURES");
  if (archs.empty()) {
    int st = 0;
    archs = trim(run_capture({"dpkg", "--print-architecture"}, &st, 10));
    if (st != 0) archs.clear();
    std::string foreign = run_capture({"dpkg", "--print-foreign-architectures"}, &st, 10);
    if (st == 0) archs += " " + foreign;
  }
  std::vector<std::string> list;
  for (auto &a : split(archs, ' '))
    for (auto &b : split(a, '\n'))
      if (valid_architecture(trim(b))) list.push_back(trim(b));
  out->native_arch = list.empty() ? std::string() : list[0];
  out->foreign_archs.assign(list.empty() ? list.end() : list.begin() + 1, list.end());
  return true;
}

std::string arch_label(const std::string &arch) {
  if (arch == "amd64") return _("64-bit PC (amd64)");
  if (arch == "i386") return _("32-bit PC (i386)");
  if (arch == "arm64") return _("64-bit ARM (arm64)");
  if (arch == "armhf") return _("32-bit ARM (armhf)");
  if (arch == "armel") return _("older 32-bit ARM (armel)");
  if (arch == "riscv64") return _("64-bit RISC-V (riscv64)");
  if (arch == "ppc64el") return _("64-bit POWER (ppc64el)");
  if (arch == "s390x") return _("IBM Z (s390x)");
  if (arch == "all") return _("all computers (all)");
  return arch;
}

// ============================================================== the fit

Fit fit_package(const Package &p, const System &s) {
  Fit f;
  if (p.architecture != "all" && !s.native_arch.empty() && p.architecture != s.native_arch) {
    f.arch_foreign = std::find(s.foreign_archs.begin(), s.foreign_archs.end(), p.architecture) !=
                     s.foreign_archs.end();
    f.arch_ok = f.arch_foreign;
    if (!f.arch_ok) {
      f.problem = format(_("This package is made for %s computers, but this computer is a %s."),
                         arch_label(p.architecture).c_str(), arch_label(s.native_arch).c_str());
      if (p.architecture == "i386" && s.native_arch == "amd64")
        f.problem += " " + std::string(_("32-bit packages can be installed after an administrator adds the "
                                         "architecture with \"dpkg --add-architecture i386\" and refreshes the "
                                         "package lists. Look for a 64-bit (amd64) package first."));
      else
        f.problem += " " + std::string(_("Download the package made for this computer instead."));
    }
  }
  // The installed package of that name: the same architecture first.
  for (const Installed *i : s.find(p.name)) {
    bool same = i->arch == p.architecture || i->arch == "all" || p.architecture == "all";
    if (!f.current || same) f.current = i;
    if (same) break;
  }
  if (f.current) {
    int c = compare_versions(p.version, f.current->version);
    f.action = c > 0 ? Action::upgrade : c == 0 ? Action::reinstall : Action::downgrade;
  }
  return f;
}

// ========================================================== APT's plan

AptPlan parse_apt_output(const std::string &output, int status) {
  AptPlan plan;
  bool in_unmet = false;
  for (const auto &raw : split(output, '\n')) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (starts_with(line, "Inst ")) {
      AptItem i;
      if (parse_item(line, &i)) plan.install.push_back(i);
      in_unmet = false;
    } else if (starts_with(line, "Remv ")) {
      AptItem i;
      if (parse_item(line, &i)) plan.remove.push_back(i);
      in_unmet = false;
    } else if (starts_with(line, "E: ")) {
      plan.errors.push_back(trim(line.substr(3)));
      in_unmet = false;
    } else if (line.find("have unmet dependencies") != std::string::npos) {
      in_unmet = true;
    } else if (in_unmet && !line.empty() && line[0] == ' ') {
      std::string t = trim(line);
      size_t colon = t.find(": ");
      bool field = colon != std::string::npos && colon < 24 && isupper((unsigned char)t[0]) &&
                   t.substr(0, colon).find(' ') == std::string::npos;
      if (line.size() > 1 && line[1] != ' ' && t.find(" : ") != std::string::npos) {
        plan.unmet.push_back(t);  // " pkg : Depends: ..."
      } else if (!plan.unmet.empty() && field) {
        // Another relation of the same package ("Recommends: ...").
        const std::string &last = plan.unmet.back();
        plan.unmet.push_back(last.substr(0, last.find(" : ") + 3) + t);
      } else if (!plan.unmet.empty()) {
        plan.unmet.back() += " " + t;  // the alternatives after "or"
      }
    } else if (!line.empty() && line[0] != ' ') {
      in_unmet = false;
    }
  }
  plan.ran = status >= 0 && (status == 0 || !plan.errors.empty() || !plan.unmet.empty());
  plan.ok = status == 0 && plan.errors.empty() && plan.unmet.empty();
  if (!plan.ok) {
    std::string all = lower(output);
    if (!plan.unmet.empty())
      plan.problem = AptPlan::Problem::unmet;
    else if (all.find("held packages were changed") != std::string::npos ||
             all.find("you have held broken packages") != std::string::npos)
      plan.problem = AptPlan::Problem::held;
    else if (all.find("essential") != std::string::npos || all.find("potentially harmful") != std::string::npos ||
             all.find("system-critical") != std::string::npos || all.find("protected") != std::string::npos)
      plan.problem = AptPlan::Problem::essential;
    else if (all.find("could not get lock") != std::string::npos || all.find("unable to lock") != std::string::npos ||
             all.find("unable to acquire") != std::string::npos)
      plan.problem = AptPlan::Problem::lock;
    else if (all.find("downgrade") != std::string::npos)
      plan.problem = AptPlan::Problem::downgrade;
    else if (all.find("not a valid deb") != std::string::npos ||
             all.find("invalid archive signature") != std::string::npos ||
             all.find("unsupported file") != std::string::npos)
      plan.problem = AptPlan::Problem::damaged;
    else
      plan.problem = AptPlan::Problem::other;
  }
  return plan;
}

std::string apt_get_program() { return env("DEBRUN_APT_GET", "apt-get"); }

std::vector<std::string> simulate_install_argv(const std::string &deb, bool reinstall, bool downgrade) {
  std::vector<std::string> argv = {apt_get_program(), "--simulate", "-q", "-o", "Debug::NoLocking=true",
                                   "-o", "APT::Get::Show-User-Simulation-Note=false", "install"};
  if (reinstall) argv.push_back("--reinstall");
  if (downgrade) argv.push_back("--allow-downgrades");
  argv.push_back(deb);
  return argv;
}

std::vector<std::string> simulate_remove_argv(const std::string &name) {
  return {apt_get_program(), "--simulate", "-q", "-o", "Debug::NoLocking=true",
          "-o", "APT::Get::Show-User-Simulation-Note=false", "remove", name};
}

std::string run_capture(const std::vector<std::string> &argv, int *status, int timeout_seconds) {
  if (status) *status = -1;
  std::string path = argv.empty() ? std::string() : find_program(argv[0]);
  if (argv.empty() || path.empty()) return "";
  // Our environment with the C locale: the output is parsed.
  std::vector<std::string> envs;
  for (char **e = environ; e && *e; ++e)
    if (strncmp(*e, "LC_", 3) != 0 && strncmp(*e, "LANG=", 5) != 0 && strncmp(*e, "LANGUAGE=", 9) != 0)
      envs.push_back(*e);
  envs.push_back("LC_ALL=C.UTF-8");
  std::vector<char *> envp, args;
  for (auto &e : envs) envp.push_back(&e[0]);
  envp.push_back(nullptr);
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return "";
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t none, dflt;
  sigemptyset(&none);
  sigemptyset(&dflt);
  sigaddset(&dflt, SIGPIPE);
  sigaddset(&dflt, SIGCHLD);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setsigdefault(&attr, &dflt);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  pid_t pid = -1;
  int err = posix_spawn(&pid, path.c_str(), &fa, &attr, args.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    return "";
  }
  std::string out;
  time_t deadline = time(nullptr) + timeout_seconds;
  bool timed_out = false;
  for (;;) {
    int left = int(deadline - time(nullptr));
    if (left <= 0) {
      timed_out = true;
      break;
    }
    struct pollfd p = {fds[0], POLLIN, 0};
    int r = poll(&p, 1, 1000);
    if (r < 0 && errno == EINTR) continue;
    if (r == 0) continue;
    char buf[8192];
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (out.size() < (16u << 20)) out.append(buf, size_t(n));
  }
  close(fds[0]);
  if (timed_out) kill(pid, SIGKILL);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (status) *status = !timed_out && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return out;
}

// ============================================================ relations

std::vector<DepCheck> check_relations(const Package &p, const System &s, const AptPlan &plan) {
  std::vector<DepCheck> out;
  std::string want_arch = dependency_arch(p, s);
  const std::pair<const char *, const std::string *> needs[] = {
      {"Pre-Depends", &p.pre_depends}, {"Depends", &p.depends}, {"Recommends", &p.recommends}};
  for (const auto &field : needs) {
    std::vector<Alternatives> groups;
    parse_relations(*field.second, &groups);
    for (const auto &alts : groups) {
      DepCheck d;
      d.field = field.first;
      d.alts = alts;
      bool done = false;
      for (const Relation &r : alts) {
        for (const Installed *i : s.find(r.name))
          if (arch_compatible(*i, r, want_arch) && version_satisfies(i->version, r.op, r.version)) {
            d.state = DepCheck::State::installed;
            d.detail = i->version;
            done = true;
            break;
          }
        if (done) break;
        auto range = s.providers.equal_range(r.name);
        for (auto it = range.first; it != range.second && !done; ++it) {
          const Installed &i = s.packages[it->second];
          if (!i.present() || !arch_compatible(i, r, want_arch)) continue;
          for (const Relation &prov : i.provides)
            if (prov.name == r.name &&
                (r.op.empty() || (prov.op == "=" && version_satisfies(prov.version, r.op, r.version)))) {
              d.state = DepCheck::State::provided;
              d.detail = i.name;
              done = true;
              break;
            }
        }
        if (done) break;
      }
      if (!done && plan.ran)
        for (const Relation &r : alts)
          if (const AptItem *i = planned(plan, r.name)) {
            d.state = DepCheck::State::will_install;
            d.detail = i->version + (i->origin.empty() ? "" : " (" + i->origin + ")");
            done = true;
            break;
          }
      if (!done && plan.ok) {
        // A package APT chose provides it.
        d.state = DepCheck::State::ok;
        done = true;
      }
      if (!done) {
        d.state = DepCheck::State::missing;
        for (const Relation &r : alts) {
          std::string why = unmet_reason(plan.unmet, r.name);
          if (!why.empty() && why != "missing") {
            d.detail = why;
            break;
          }
        }
      }
      out.push_back(d);
    }
  }
  // Conflicts and Breaks that hit an installed package.
  const std::pair<const char *, const std::string *> against[] = {{"Conflicts", &p.conflicts},
                                                                   {"Breaks", &p.breaks}};
  for (const auto &field : against) {
    std::vector<Alternatives> groups;
    parse_relations(*field.second, &groups);
    for (const auto &alts : groups)
      for (const Relation &r : alts) {
        std::vector<const Installed *> hits;
        for (const Installed *i : s.find(r.name))
          if (i->name != p.name && version_satisfies(i->version, r.op, r.version)) hits.push_back(i);
        auto range = s.providers.equal_range(r.name);
        for (auto it = range.first; it != range.second; ++it) {
          const Installed &i = s.packages[it->second];
          if (i.present() && i.name != p.name && r.op.empty() &&
              std::find(hits.begin(), hits.end(), &i) == hits.end())
            hits.push_back(&i);
        }
        for (const Installed *i : hits) {
          DepCheck d;
          d.field = field.first;
          d.alts = {r};
          d.state = removed(plan, i->name) ? DepCheck::State::removed : DepCheck::State::conflict;
          d.detail = i->name + " " + i->version;
          out.push_back(d);
        }
      }
  }
  return out;
}

}  // namespace debrun
}  // namespace sde
