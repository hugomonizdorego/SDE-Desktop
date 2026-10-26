// SPDX-License-Identifier: GPL-3.0-or-later
// Software Updater (sde-update): C++ side of the helper protocols (src/update/model.cpp), plus
// an end-to-end run of the real Python helpers on fixture files.
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>

#include "model.h"
#include "process.h"
#include "util.h"

using namespace sde;
using namespace sde::update;

int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

static void updates() {
  std::string v2 =
      "SDE-UPDATES\t2\n"
      "U\tpackage\tfirefox-esr\tfirefox-esr\tfirefox-esr\t128.1\t128.2\t70000000\tfirefox-esr\tdebian\t"
      "testing\tdeb.debian.org\tWeb browser\tMozilla Firefox web browser\n"
      "U\tsecurity\topenssl\tlibssl3t64\topenssl\t3.3.1-1\t3.3.1-2\t2500000\tlibssl3t64, openssl\tdebian\t"
      "testing-security\tsecurity.debian.org\tSSL library\tSecure Sockets Layer toolkit\n"
      "U\tkernel\tlinux-signed-amd64\tlinux-image-amd64\tlinux-signed-amd64\t6.10\t6.11\t90000000\t"
      "linux-image-amd64\tdebian\ttesting\tdeb.debian.org\tLinux\tLinux kernel\n"
      "U\tbackport\tmesa\tlibgl1-mesa-dri\tmesa\t24.1\t24.2\t1000\tlibgl1-mesa-dri\tdebian\t"
      "trixie-backports\tdeb.debian.org\tMesa\tMesa drivers\n"
      "H\tpinned-package\n"
      "X\tfuture record\n";
  UpdateList list = parse_updates(v2);
  CHECK(list.valid && list.protocol == 2);
  CHECK(list.updates.size() == 4);
  // Groups: the kernel is Critical, openssl Security, the rest Main (sorted
  // by title: "Mesa" (backport) before "Web browser").
  CHECK(list.updates[0].kind == Kind::Kernel && list.updates[0].group == Group::Critical);
  CHECK(list.updates[0].reboot);
  CHECK(list.updates[1].kind == Kind::Security && list.updates[1].name == "openssl");
  CHECK(list.updates[1].group == Group::Security);
  CHECK(list.updates[2].kind == Kind::Backport && list.updates[2].archive == "trixie-backports");
  CHECK(list.updates[2].group == Group::Main);
  CHECK(list.updates[3].kind == Kind::Software && list.updates[3].summary == "Web browser");
  CHECK(list.updates[3].title() == "Web browser");
  CHECK(list.updates[1].packages.size() == 2 && list.updates[1].packages[1] == "openssl");
  CHECK(list.total_size() == 70000000LL + 2500000 + 90000000 + 1000);
  CHECK(list.count(Kind::Security) == 1);
  CHECK(list.count(Group::Main) == 2 && list.size(Group::Main) == 70001000LL);
  CHECK(list.held.size() == 1 && list.held[0] == "pinned-package");
  CHECK(matches(list.updates[1], "SSL"));
  CHECK(matches(list.updates[1], "libssl3"));
  CHECK(!matches(list.updates[1], "firefox"));

  bool all = false;
  auto pkgs = selected_packages(list.updates, &all);
  CHECK(all && pkgs.size() == 5);
  list.updates[3].selected = false;
  pkgs = selected_packages(list.updates, &all);
  CHECK(!all && pkgs.size() == 4);

  // Protocol 1 still loads; malformed lines invalidate the whole answer.
  UpdateList v1 = parse_updates("SDE-UPDATES\t1\nsecurity\tbash\t5.1\t5.2\t42\tbash, bash-doc\n");
  CHECK(v1.valid && v1.updates.size() == 1 && v1.updates[0].packages.size() == 2);
  CHECK(!parse_updates("SDE-UPDATES\t2\nU\tpackage\tbroken\n").valid);
  CHECK(!parse_updates("garbage").valid);
  CHECK(parse_updates("SDE-UPDATES\t2\n").valid);
}

static Update apt_update(const std::string &name, const std::string &kind = "package") {
  Update u;
  u.kind = kind_from_id(kind);
  u.name = u.source = u.main_package = name;
  u.packages = {name};
  u.archive = "testing";
  return u;
}

static void classifier() {
  // Origins.
  Update sec = apt_update("openssl", "security");
  CHECK(classify(sec) == Group::Security);
  Update suite = apt_update("curl");
  suite.archive = "trixie-security";
  CHECK(classify(suite) == Group::Security);
  Update site = apt_update("curl");
  site.site = "security.debian.org";
  CHECK(classify(site) == Group::Security);
  CHECK(classify(apt_update("firefox-esr")) == Group::Main);
  Update backport = apt_update("mesa", "backport");
  CHECK(classify(backport) == Group::Main);
  // Priorities and urgency.
  Update req = apt_update("coreutils");
  req.priority = "required";
  CHECK(classify(req) == Group::Critical);
  Update imp = apt_update("cron");
  imp.priority = "important";
  CHECK(classify(imp) == Group::Critical);
  Update opt = apt_update("gimp");
  opt.priority = "optional";
  CHECK(classify(opt) == Group::Main);
  Update urgent = apt_update("tool");
  urgent.urgency = "high";
  CHECK(classify(urgent) == Group::Critical);
  // Kernel, libc, systemd, apt/dpkg, firmware: critical even from security.
  Update kernel = apt_update("linux-signed-amd64", "security");
  kernel.packages = {"linux-image-6.11.0-1-amd64", "linux-image-amd64"};
  CHECK(needs_reboot(kernel) && classify(kernel) == Group::Critical);
  Update image = apt_update("linux-image-amd64");
  CHECK(needs_reboot(image) && classify(image) == Group::Critical);
  Update libc = apt_update("glibc");
  libc.packages = {"libc6:amd64", "libc-bin"};
  CHECK(needs_reboot(libc) && classify(libc) == Group::Critical);
  CHECK(classify(apt_update("systemd")) == Group::Critical);
  CHECK(!needs_reboot(apt_update("apt")) && classify(apt_update("apt")) == Group::Critical);
  CHECK(classify(apt_update("dpkg")) == Group::Critical);
  CHECK(classify(apt_update("firmware-nonfree")) == Group::Critical);
  CHECK(classify(apt_update("intel-microcode")) == Group::Critical);
  CHECK(!needs_reboot(apt_update("libreoffice")));
  // linux-libc-dev and linux-headers do not need a restart.
  CHECK(!needs_reboot(apt_update("linux-libc-dev")));

  // Optional Alpha 51 fields of the U record.
  UpdateList l = parse_updates(
      "SDE-UPDATES\t2\n"
      "U\tpackage\tfirefox-esr\tfirefox-esr\tfirefox-esr\t1\t2\t300\tfirefox-esr\tdebian\ttesting\t"
      "deb.debian.org\tMozilla Firefox web browser\tlong\toptional\tweb\t\tFirefox ESR\tfirefox-esr\n"
      "U\tpackage\tbash\tbash\tbash\t5.2\t5.3\t200\tbash\tdebian\ttesting\tdeb.debian.org\t"
      "GNU Bourne Again SHell\tlong\trequired\tshells\tmedium\t\t\n");
  CHECK(l.valid && l.updates.size() == 2);
  CHECK(l.updates[0].name == "bash" && l.updates[0].group == Group::Critical);
  CHECK(l.updates[0].priority == "required" && l.updates[0].urgency == "medium");
  CHECK(l.updates[1].title() == "Firefox ESR" && l.updates[1].icon == "firefox-esr");
  CHECK(l.updates[1].section == "web" && l.updates[1].group == Group::Main);

  // Size totals and the selection.
  CHECK(l.total_size() == 300 + 200);
  CHECK(l.size(Group::Main) == 300 && l.size(Group::Critical) == 200);
  CHECK(l.selected_count() == 2 && l.selected_size() == l.total_size());
  CHECK(l.needs_reboot() == false);

  // Selection -> helper command lines.
  auto cmds = install_commands(l.updates);
  CHECK(cmds.size() == 1 && cmds[0] == std::vector<std::string>({"upgrade"}));
  l.updates[1].selected = false;  // firefox-esr
  cmds = install_commands(l.updates);
  CHECK(cmds.size() == 1 && cmds[0] == std::vector<std::string>({"upgrade", "bash"}));
  CHECK(l.selected_size() == 200 && l.selected_count() == 1);
  for (auto &u : l.updates) u.selected = false;
  CHECK(install_commands(l.updates).empty());
  bool all = true;
  CHECK(selected_packages(l.updates, &all).empty() && !all);
  // No updates: nothing to install, and not "everything selected".
  CHECK(install_commands({}).empty());
  all = true;
  CHECK(selected_packages({}, &all).empty() && !all);
  // Critical, security and main: applications from Flathub and the Snap
  // Store are updated in Merkadu.
  CHECK(kGroupCount == 3 && std::string(group_id(Group::Main)) == "main");
  CHECK(std::string(group_title(Group::Critical)) == "Critical updates");
  CHECK(remind_label(24) == "Tomorrow");
}

static void history() {
  std::vector<HistoryEntry> h;
  CHECK(parse_history(
      "SDE-HISTORY\t1\n"
      "T\t2026-09-18 10:00:00\t2026-09-18 10:00:05\tapt-get upgrade\talice (1000)\t\n"
      "C\tUpgrade\tbash\tamd64\t5.2-2\t5.2-3\t0\n"
      "C\tUpgrade\tlibc6\tamd64\t2.40-1\t2.40-2\t0\n"
      "C\tInstall\tlibnew\tamd64\t\t1.0\t1\n"
      "T\t2026-09-17 02:20:17\t\tapt-get purge old\t\tdpkg failed\n"
      "C\tPurge\told\tamd64\t0.9\t\t0\n",
      h));
  CHECK(h.size() == 2);
  CHECK(h[0].changes.size() == 3 && h[0].count("Upgrade") == 2);
  CHECK(h[0].changes[2].automatic && h[0].changes[2].new_version == "1.0");
  CHECK(h[0].summary() == "Upgraded 2 · Installed 1");
  CHECK(h[1].error == "dpkg failed" && h[1].summary() == "Purged 1");
  CHECK(matches(h[0], "libc"));
  CHECK(!matches(h[1], "libc"));
  CHECK(!parse_history("SDE-UPDATES\t2\n", h));
}

static void sources() {
  std::vector<Source> s;
  CHECK(parse_sources(
      "SDE-SOURCES\t1\n"
      "S\t/etc/apt/sources.list.d/debian.sources\t1\tdeb822\t1\tdeb deb-src\thttp://deb.debian.org/debian\t"
      "testing\tmain\t/usr/share/keyrings/debian-archive-keyring.gpg\n"
      "S\t/etc/apt/sources.list\t3\tone-line\t0\tdeb\thttp://example.org\tstable\tmain\t\n"
      "S\tshort\n",
      s));
  CHECK(s.size() == 2);
  CHECK(s[0].format == "deb822" && s[0].enabled && s[0].types == "deb deb-src");
  CHECK(!s[1].enabled && s[1].line == 3 && s[1].signed_by.empty());
}

static void transactions() {
  Progress p;
  for (const char *line :
       {"SDE-TRANSACTION\t1", "BACKEND\tpackagekit", "CANCELLABLE\t1", "STATUS\tDownloading packages",
        "PERCENT\t30", "ITEM\tDownloading\tbash\t5.2-3", "ITEM\tDownloading\tlibc6\t2.40-2",
        "ITEM\tUpdating\tbash\t5.2-3", "ERROR\tsomething odd", "LOG\tplain text", "PERCENT\t250"})
    apply_transaction_line(p, line);
  CHECK(p.started && !p.finished);
  CHECK(p.backend == "packagekit" && p.cancellable);
  CHECK(p.status == "Downloading packages" && p.percent == 100);
  CHECK(p.items_seen == 2 && p.item_info == "Updating" && p.item_name == "bash");
  CHECK(p.errors.size() == 1 && p.log.back() == "plain text");
  apply_transaction_line(p, "PERCENT\t-1");
  CHECK(p.percent == -1);
  apply_transaction_line(p, "EXIT\t0\tok");
  CHECK(p.finished && p.ok() && p.exit_code == 0);
  Progress q;
  apply_transaction_line(q, "EXIT\t126\tdenied");
  CHECK(q.finished && !q.ok() && q.outcome == "denied");
  CHECK(status_text("Downloading packages") == "Downloading packages");
  CHECK(status_text("Some new state") == "Some new state");
}

static void helpers() {
  auto removals = parse_autoremove(
      "NOTE: This is only a simulation!\n"
      "Remv libold1 [1.0-1]\n"
      "Remv python3-unused [2.0] [python3:amd64 ]\n"
      "Remv Bad;name [1]\n");
  CHECK(removals.size() == 2 && removals[0] == "libold1" && removals[1] == "python3-unused");
  CHECK(valid_package_name("libc6:amd64"));
  CHECK(valid_package_name("g++-14"));
  CHECK(!valid_package_name("-oAPT::X"));
  CHECK(!valid_package_name("Bash"));
  CHECK(!valid_package_name("a b"));
  CHECK(!valid_package_name("pkg:"));
  CHECK(format_size(512) == "512 bytes");
  CHECK(format_size(1500) == "1.5 kB");
  CHECK(format_size(84200000) == "84.2 MB");
  CHECK(format_size(250000000) == "250 MB");
  std::time_t now = 1000000;
  CHECK(format_age(0, now) == "never");
  CHECK(format_age(now - 10, now) == "just now");
  CHECK(format_age(now - 120, now) == "2 minutes ago");
  CHECK(format_age(now - 3600, now) == "1 hour ago");
  CHECK(format_age(now - 86400 * 3, now) == "3 days ago");
  CHECK(format_date("2026-09-17 02:20:17") == "Sep 17, 2026 · 02:20");
  CHECK(format_date("garbage") == "garbage");
  CHECK(interval_hours().front() == 0 && interval_label(24) == "Daily");
}

// Runs the real helpers against fixture files and parses what they print.
static void end_to_end() {
  std::string dir = std::string(SDE_TEST_SOURCE_DIR) + "/data/update";
  std::string tmp = "/tmp/sde-update-test-" + std::to_string(getpid());
  make_dirs(tmp + "/apt/sources.list.d");
  write_file_atomic(tmp + "/apt/sources.list", "deb http://deb.debian.org/debian testing main\n");
  write_file_atomic(tmp + "/apt/sources.list.d/extra.sources",
                    "Types: deb\nURIs: https://repo.example.org\nSuites: stable\nComponents: main\nEnabled: no\n");
  write_file_atomic(tmp + "/history.log",
                    "Start-Date: 2026-10-01  09:00:00\nCommandline: apt-get upgrade\n"
                    "Upgrade: bash:amd64 (5.2-2, 5.2-3)\nEnd-Date: 2026-10-01  09:00:09\n");
  int status = -1;
  std::string out = run_sync({"python3", dir + "/sources.py", tmp + "/apt"}, &status, 20000);
  std::vector<Source> s;
  CHECK(status == 0 && parse_sources(out, s) && s.size() == 2);
  CHECK(s.size() == 2 && s[0].enabled && !s[1].enabled && s[1].format == "deb822");
  out = run_sync({"python3", dir + "/history.py", tmp}, &status, 20000);
  std::vector<HistoryEntry> h;
  CHECK(status == 0 && parse_history(out, h) && h.size() == 1);
  CHECK(!h.empty() && h[0].changes.size() == 1 && h[0].changes[0].new_version == "5.2-3");
  out = run_sync({"python3", dir + "/transaction.py", "--plan", "--backend", "apt", "upgrade", "bash"}, &status,
                 20000);
  CHECK(status == 0 && out.find("--only-upgrade\nbash\n") != std::string::npos);
  run_sync({"python3", dir + "/transaction.py", "--plan", "upgrade", "bad;name"}, &status, 20000);
  CHECK(status == 2);
  // Flatpak apps are Merkadu's: no such action here.
  run_sync({"python3", dir + "/transaction.py", "--plan", "flatpak", "user:app/org.gimp.GIMP/x86_64/stable"}, &status,
           20000);
  CHECK(status == 2);
  run_sync({"rm", "-rf", tmp});
}

int main() {
  setenv("LANGUAGE", "C", 1);
  updates();
  classifier();
  history();
  sources();
  transactions();
  helpers();
  end_to_end();
  if (failures) fprintf(stderr, "%d update model checks failed\n", failures);
  else printf("Software Updater model and protocol checks passed.\n");
  return failures ? 1 : 0;
}
