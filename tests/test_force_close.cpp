// SPDX-License-Identifier: GPL-3.0-or-later
// 0.2.0 Alpha 3.1 Force Close (docs/FORCE-CLOSE.md), without a display:
//  * which processes Force Close may end (src/forceclose/process_rules.h),
//    against a made-up /proc: PID 1, this process and its ancestors, other
//    users (real, effective and saved ids), the parts of the session by
//    their program and command names, the session's own sde-session,
//    zombies and processes that cannot be identified; families of a launch
//  * the watchdog's decisions (src/forceclose/watch_model.h): pings,
//    "Not Responding", the dialog after the timeout or a close attempt,
//    Wait, recovery, one dialog per program, launches
//  * the launch records (src/common/launches.h)
// It never sends a signal: kill_now() is exercised only on a process this
// test starts itself (a child that sleeps), never on anything else.
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/common/config.h"
#include "../src/common/launches.h"
#include "../src/common/util.h"
#include "../src/forceclose/process_rules.h"
#include "../src/forceclose/watch_model.h"

using namespace forceclose;

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

namespace {

std::string g_proc;

// A process in the made-up /proc. |exe| "" leaves out the exe link (a
// kernel thread, or a program that cannot be read).
void fake(long pid, long ppid, long session, const std::string &comm, const std::string &exe, long uid,
          char state = 'S', unsigned long long start = 1000, long euid = -1, long suid = -1) {
  std::string dir = g_proc + "/" + std::to_string(pid);
  sde::make_dirs(dir);
  char stat[512];
  snprintf(stat, sizeof stat,
           "%ld (%s) %c %ld %ld %ld 0 -1 4194304 1 0 0 0 0 0 0 0 20 0 1 0 %llu 1000 100 0 0\n", pid,
           comm.c_str(), state, ppid, pid, session, start);
  sde::write_file_atomic(dir + "/stat", stat);
  sde::write_file_atomic(dir + "/comm", comm + "\n");
  char status[256];
  snprintf(status, sizeof status, "Name:\t%s\nUid:\t%ld\t%ld\t%ld\t%ld\n", comm.c_str(), uid,
           euid < 0 ? uid : euid, suid < 0 ? uid : suid, uid);
  sde::write_file_atomic(dir + "/status", status);
  unlink((dir + "/exe").c_str());
  if (!exe.empty() && symlink(exe.c_str(), (dir + "/exe").c_str()) != 0) perror("symlink");
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/sde-force-close-test-XXXXXX";
  std::string root = mkdtemp(tmpl);
  g_proc = root + "/proc";
  const long me = 1000, other = 1001;

  // ---------------------------------------------------------- the rules
  fake(1, 0, 1, "systemd", "/usr/lib/systemd/systemd", 0);
  fake(2, 0, 0, "kthreadd", "", 0);
  fake(400, 1, 400, "lightdm", "/usr/sbin/lightdm", 0);
  fake(500, 400, 500, "sde-session", "/usr/bin/sde-session", me);
  fake(510, 500, 500, "sde-wm", "/usr/bin/sde-wm", me);
  fake(511, 500, 500, "sde-panel", "/usr/bin/sde-panel (deleted)", me);  // upgraded while running
  fake(512, 500, 500, "sde-polkit-agen", "/usr/libexec/sde/sde-polkit-agent", me);
  fake(513, 500, 500, "Xorg", "/usr/lib/xorg/Xorg", me);  // a rootless X server
  fake(514, 500, 500, "startsde", "/usr/bin/dash", me);   // a script: the command names it
  fake(520, 500, 520, "sde-watchdog", "/usr/bin/sde-watchdog", me);
  fake(530, 520, 520, "sde-force-close", "/usr/bin/sde-force-close", me);  // "this process"
  fake(600, 500, 600, "merkadu", "/usr/bin/merkadu", me);                 // a frozen application
  fake(601, 600, 600, "merkadu-helper", "/usr/lib/merkadu/helper", me);
  fake(602, 1, 600, "merkadu-daemon", "/usr/lib/merkadu/daemon", me);  // reparented, same session
  fake(700, 500, 700, "firefox", "/usr/lib/firefox/firefox", other);
  fake(701, 500, 701, "passwd", "/usr/bin/passwd", me, 'S', 1000, 0, 0);  // setuid: euid 0
  fake(702, 500, 702, "defunct", "/usr/bin/defunct", me, 'Z');
  fake(703, 500, 703, "python3", "", me);  // exe not readable

  Context ctx;
  ctx.proc = g_proc;
  ctx.self = 530;
  ctx.uid = me;
  ctx.protect = ancestors(530, g_proc);
  ctx.protect.push_back(500);  // the session's pid file
  CHECK((ancestors(530, g_proc) == std::vector<long>{530, 520, 500, 400}));

  Process p = read_process(600, g_proc);
  CHECK(p.exists && p.ppid == 500 && p.session == 600 && p.comm == "merkadu" && p.exe == "merkadu");
  CHECK(p.ruid == me && p.euid == me && p.suid == me && p.start == 1000 && p.state == 'S');
  CHECK(read_process(511, g_proc).exe == "sde-panel");
  CHECK(!read_process(9999, g_proc).exists);

  CHECK(check(600, ctx) == Verdict::OK);
  CHECK(check(601, ctx) == Verdict::OK);
  CHECK(check(1, ctx) == Verdict::PID1);
  CHECK(check(0, ctx) == Verdict::NO_PROCESS);
  CHECK(check(-5, ctx) == Verdict::NO_PROCESS);
  CHECK(check(9999, ctx) == Verdict::NO_PROCESS);
  CHECK(check(2, ctx) == Verdict::OTHER_USER);  // root's kernel thread
  CHECK(check(400, ctx) == Verdict::SELF);      // an ancestor (the display manager)
  CHECK(check(500, ctx) == Verdict::SELF);      // sde-session
  CHECK(check(520, ctx) == Verdict::SELF);
  CHECK(check(530, ctx) == Verdict::SELF);
  CHECK(check(510, ctx) == Verdict::SESSION);
  CHECK(check(511, ctx) == Verdict::SESSION);
  CHECK(check(512, ctx) == Verdict::SESSION);  // comm cut to 15 characters
  CHECK(check(513, ctx) == Verdict::SESSION);
  CHECK(check(514, ctx) == Verdict::SESSION);  // by its command name
  CHECK(check(700, ctx) == Verdict::OTHER_USER);
  CHECK(check(701, ctx) == Verdict::OTHER_USER);  // setuid program
  CHECK(check(702, ctx) == Verdict::ZOMBIE);
  CHECK(check(703, ctx) == Verdict::UNKNOWN);
  // The session's programs by name, and nothing else.
  for (const char *n : {"sde-session", "sde-wm", "openbox", "sde-panel", "sde-desktop", "sde-notifyd",
                        "sde-compositor", "picom", "sde-lock", "sde-power", "Xorg", "Xvfb", "Xwayland",
                        "dbus-daemon", "systemd", "sde-watchdog", "sde-force-close", "sde-polkit-agen"})
    CHECK(session_program(n));
  for (const char *n : {"merkadu", "firefox", "liria", "niny", "sde-settings", "calculator", "", "sde-"})
    CHECK(!session_program(n));
  // Without a known user nothing may be ended.
  Context nobody = ctx;
  nobody.uid = -1;
  CHECK(check(600, nobody) == Verdict::OTHER_USER);

  // Families: the program, its children, and what stays in its session.
  auto fam = family(600, g_proc);
  std::sort(fam.begin(), fam.end());
  CHECK((fam == std::vector<long>{600, 601, 602}));
  CHECK(in_family(601, 600, g_proc) && in_family(602, 600, g_proc) && in_family(600, 600, g_proc));
  CHECK(!in_family(700, 600, g_proc) && !in_family(500, 600, g_proc));
  CHECK(family(1, g_proc).empty());

  // kill_now() on a child of this test only: refused for a start time that
  // does not match (a recycled number), then sent.
  {
    // A plain "sleep": this test's own name, cut to 15 characters, is
    // "sde-force-close", which is a part of the session.
    pid_t child = fork();
    if (child == 0) {
      execl("/bin/sleep", "sleep", "300", static_cast<char *>(nullptr));
      _exit(127);
    }
    Context real = current_context();
    long start = 0;
    for (int i = 0; i < 100; ++i) {  // until it runs sleep
      start = long(sde::launches::start_time(child));
      if (read_process(child, "/proc").exe == "sleep") break;
      usleep(10000);
    }
    CHECK(start > 0);
    CHECK(check(child, real) == Verdict::OK);
    CHECK(kill_now(child, real, (unsigned long long)start + 1) == Verdict::NO_PROCESS);
    CHECK(kill(child, 0) == 0);  // still there
    Verdict v = kill_now(child, real, (unsigned long long)start);
    CHECK(v == Verdict::OK);
    if (v != Verdict::OK) kill(child, SIGKILL);  // never left behind
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    // This process, its parent and PID 1 are never ended.
    CHECK(kill_now(getpid(), real) == Verdict::SELF);
    CHECK(kill_now(getppid(), real) == Verdict::SELF);
    CHECK(kill_now(1, real) == Verdict::PID1);
  }

  // ----------------------------------------------------- the watchdog
  {
    watchdog::Settings s;
    s.mark_after = 5;
    s.timeout = 120;
    s.repeat = 2;
    watchdog::Model m(s);
    // Focus: a ping, none again while it is pending (before |repeat|).
    CHECK(m.use(0x10, 600, 100.0));
    CHECK(!m.use(0x10, 600, 100.5));
    CHECK(m.use(0x10, 600, 102.1));  // pending long enough: again
    // It answers: forgotten.
    CHECK(m.answered(0x10).empty());
    CHECK(!m.find(0x10));
    // It stops answering: marked after 5 s, pinged again meanwhile.
    CHECK(m.use(0x10, 600, 200.0));
    m.use(0x11, 600, 200.0);  // a second window of the same program
    auto d = m.tick(203.0);
    CHECK(d.mark.empty() && d.dialog.empty() && d.ping.size() == 2);
    CHECK(m.next(203.0) > 0 && m.next(203.0) <= 2.0);
    d = m.tick(205.1);
    CHECK((d.mark == std::vector<unsigned long>{0x10, 0x11}));
    CHECK(m.find(0x10)->marked);
    // The dialog after 120 s, once per program.
    d = m.tick(319.0);
    CHECK(d.dialog.empty());
    d = m.tick(320.2);
    CHECK(d.dialog.size() == 1 && d.dialog[0] == 0x10);
    d = m.tick(321.0);
    CHECK(d.dialog.empty());
    // Wait: not asked again for another timeout.
    m.dialog_closed(0x10, true, 330.0);
    d = m.tick(400.0);
    CHECK(d.dialog.empty());
    d = m.tick(451.0);
    CHECK(d.dialog.size() == 1);
    if (!d.dialog.empty()) m.dialog_closed(d.dialog[0], false, 452.0);
    // An answer from either window: both unmarked, forgotten.
    auto unmark = m.answered(0x11);
    std::sort(unmark.begin(), unmark.end());
    CHECK((unmark == std::vector<unsigned long>{0x10, 0x11}));
    CHECK(m.size() == 0);
    // A close attempt: the dialog as soon as it is marked, despite Wait.
    CHECK(m.use(0x20, 700, 500.0));
    d = m.tick(506.0);
    CHECK(d.mark.size() == 1);
    CHECK(d.dialog.empty());
    m.closing(0x20, 700, 507.0);
    d = m.tick(507.0);
    CHECK(d.dialog.size() == 1 && d.dialog[0] == 0x20);
    // The window goes away: forgotten.
    m.gone(0x20);
    CHECK(m.size() == 0 && m.next(600.0) < 0);
    // Turned off: nothing is due.
    watchdog::Settings off = s;
    off.enabled = false;
    watchdog::Model quiet(off);
    quiet.use(0x30, 1, 0.0);
    d = quiet.tick(1000.0);
    CHECK(d.mark.empty() && d.dialog.empty() && d.ping.empty());
  }
  {
    // [force_close] in sde.conf.
    sde::Config cfg;
    watchdog::Settings s = watchdog::settings(cfg);
    CHECK(s.enabled && s.mark_after == 5 && s.timeout == 120);
    cfg.set("force_close", "timeout", "1");
    cfg.set("force_close", "mark_after", "0");
    cfg.set("force_close", "enabled", "false");
    s = watchdog::settings(cfg);
    CHECK(!s.enabled && s.timeout == 2 && s.mark_after == 1);  // at least 2 and 1 s
  }
  {
    watchdog::Launches l;
    l.add(4242, 1000.0, 10.0, 120.0);  // started 10 s ago
    CHECK(l.has(4242) && l.due(1100.0).empty());
    CHECK(l.next(1000.0) == 110.0);
    CHECK((l.due(1110.5) == std::vector<long>{4242}));
    l.asked(4242);
    CHECK(l.due(2000.0).empty() && l.next(2000.0) < 0);
    l.forget(4242);
    CHECK(!l.has(4242) && l.size() == 0);
  }

  // ------------------------------------------------- the launch records
  {
    setenv("SDE_LAUNCHES_DIR", (root + "/launches").c_str(), 1);
    CHECK(sde::launches::enabled());
    sde::launches::Record r;
    r.pid = getpid();
    r.start = sde::launches::start_time(getpid());
    r.time = 1793500000;
    r.name = "Merkadu\nStore";
    r.icon = "merkadu";
    r.desktop_id = "merkadu.desktop";
    r.argv = {"merkadu", "--page", "a\\b"};
    CHECK(sde::launches::save(r));
    sde::launches::Record back;
    CHECK(sde::launches::load(getpid(), back));
    CHECK(back.pid == r.pid && back.start == r.start && back.time == r.time && back.name == r.name &&
          back.icon == r.icon && back.desktop_id == r.desktop_id && back.argv == r.argv);
    CHECK(sde::launches::alive(back));
    back.start += 1;  // another process with the same number
    CHECK(!sde::launches::alive(back));
    sde::write_file_atomic(root + "/launches/notes.txt", "pid=77\n");  // not a record
    CHECK(sde::launches::all().size() == 1);
    sde::launches::remove(getpid());
    CHECK(sde::launches::all().empty());
    CHECK(!sde::launches::parse("name=x\n", back));
    CHECK(sde::launches::start_time(999999999) == 0);
  }

  std::string cmd = "rm -rf '" + root + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", root.c_str());
  if (failures) fprintf(stderr, "%d failure(s)\n", failures);
  else printf("force close: all checks passed\n");
  return failures ? 1 : 0;
}
