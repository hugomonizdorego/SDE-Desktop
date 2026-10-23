// SPDX-License-Identifier: GPL-3.0-or-later
// Software Updater (sde-update): native UI over a mintupdate-derived model,
// Debian PackageKit/APT and Flatpak.
//
//   sde-update [--page NAME]        the window
//   sde-update --notifier [--once]  background checks ([update] in sde.conf)
//   sde-update --check | --history | --sources   helper protocols (read-only)
//   sde-update --plan ACTION [PKG]  print a transaction's fixed argv
//   sde-update --transaction refresh|upgrade     interactive, in a terminal
#include <FL/Fl.H>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "about.h"
#include "app.h"
#include "build_config.h"
#include "config.h"
#include "dbus.h"
#include "i18n.h"
#include "model.h"
#include "process.h"
#include "spawn.h"
#include "util.h"

using namespace sde;
using namespace sde::update;

namespace {

const char *kPython = "/usr/bin/python3";

// ------------------------------------------------- interactive transaction

// No package names, options, repository URLs or shell commands are accepted.
// PackageKit handles dependency solving, locks and the system's polkit policy.
std::vector<std::string> terminal_transaction(const std::string &action) {
  // PackageKit 1.3.4+ ships pkgcli in the packagekit binary package;
  // Debian 12/13 and older Ubuntu releases use packagekit-tools/pkcon.
  if (is_executable("/usr/bin/pkgcli")) {
    if (action == "refresh") return {"/usr/bin/pkgcli", "refresh"};
    if (action == "upgrade") return {"/usr/bin/pkgcli", "update"};
    return {};
  }
  if (action == "refresh") return {"/usr/bin/pkcon", "refresh", "force"};
  if (action == "upgrade") return {"/usr/bin/pkcon", "update"};
  return {};
}

int run_terminal_transaction(const std::string &action) {
  auto command = terminal_transaction(action);
  if (command.empty()) return 2;
  // Unlike the metadata helpers, a terminal transaction needs stdin and
  // stderr intact for confirmation, authentication and useful diagnostics.
  pid_t child = fork();
  if (child < 0) return 1;
  if (child == 0) {
    std::vector<char *> args;
    for (auto &arg : command) args.push_back(const_cast<char *>(arg.c_str()));
    args.push_back(nullptr);
    execv(args[0], args.data());
    perror("Software Updater");
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return 1;
  }
  int result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  printf("\nSoftware Updater: %s (exit %d). Press Enter to close.\n", result == 0 ? "completed" : "not completed",
         result);
  fflush(stdout);
  if (isatty(STDIN_FILENO)) {
    char line[8];
    if (!fgets(line, sizeof line, stdin)) {
    }
  }
  return result;
}

int exec_helper(const std::string &name, const std::vector<std::string> &extra = {}) {
  std::string path = helper(name);
  std::vector<std::string> argv = {"python3", path};
  argv.insert(argv.end(), extra.begin(), extra.end());
  std::vector<char *> args;
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  execv(kPython, args.data());
  perror("sde-update");
  return 127;
}

// ---------------------------------------------------------------- notifier

NotifierState load_state() { return load_notifier_state(); }
void save_state(const NotifierState &s) { save_notifier_state(s); }

uint32_t g_notification = 0;

void notify(const std::string &summary, const std::string &body) {
  auto *bus = dbus::Bus::session();
  if (!bus) {
    if (!find_program("notify-send").empty())
      run_sync({"notify-send", "-a", _("Software Updater"), "-i", "system-software-update", summary, body});
    return;
  }
  DBusMessage *msg = dbus::method("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                  "org.freedesktop.Notifications", "Notify");
  {
    dbus::Writer w(msg);
    w.str(_("Software Updater")).u32(g_notification).str("system-software-update").str(summary).str(body);
    w.strings({"default", _("Open Software Updater")});
    { dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}"); }
    w.i32(-1);
  }
  bus->call_async(msg, [](DBusMessage *reply) {
    if (!reply) return;
    auto args = dbus::read_args(reply);
    if (!args.empty()) g_notification = uint32_t(args[0].num());
  });
}

class Notifier {
 public:
  explicit Notifier(bool once) : once_(once) {}
  void start(double delay) {
    if (auto *bus = dbus::Bus::session())
      bus->add_signal_handler(
          "type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'",
          [](DBusMessage *) { return true; },
          [](DBusMessage *m) {
            auto args = dbus::read_args(m);
            if (args.size() >= 2 && g_notification && uint32_t(args[0].num()) == g_notification &&
                args[1].str() == "default")
              spawn_detached({"sde-update"});
          });
    Fl::add_timeout(delay, wake, this);
  }
  bool done = false;

 private:
  static void wake(void *data) { static_cast<Notifier *>(data)->check(); }
  void later(double seconds) {
    if (once_) {
      done = true;
      return;
    }
    Fl::add_timeout(seconds, wake, this);
  }
  void check() {
    Config cfg;
    cfg.load();
    int hours = cfg.get_int("update", "check_interval", 24);
    if (hours <= 0 && !once_) return later(900);  // disabled; look again later
    NotifierState state = load_state();
    std::time_t now = std::time(nullptr);
    std::time_t due = state.last_check + std::time_t(std::max(1, hours)) * 3600;
    // Remind Me Later just ended: check (and notify) again now.
    bool remind_over = state.remind_until && now >= state.remind_until && state.last_check < state.remind_until;
    if (!once_ && !remind_over && state.last_check && now < due)
      return later(std::min<double>(double(due - now), 900));
    bool refresh = cfg.get_bool("update", "auto_refresh", true) && is_executable("/usr/bin/pkcon");
    if (refresh) {
      // PackageKit lets the active session refresh without a password.
      bool ok = run_async({"/usr/bin/pkcon", "-p", "refresh"},
                          [this](const std::string &, int) { read_updates(); });
      if (ok) return;
    }
    read_updates();
  }
  void read_updates() {
    run_async({kPython, helper("check.py")}, [this](const std::string &out, int status) {
      Config cfg;
      cfg.load();
      NotifierState state = load_state();
      state.last_check = std::time(nullptr);
      UpdateList list = parse_updates(out);
      if (status == 0 && list.valid) {
        std::string signature;
        for (auto &u : list.updates) signature += u.name + "=" + u.new_version + ";";
        signature = std::to_string(std::hash<std::string>()(signature));
        int count = int(list.updates.size()), security = list.count(Group::Security),
            critical = list.count(Group::Critical);
        bool snoozed = state.remind_until && std::time(nullptr) < state.remind_until;
        bool wanted = cfg.get_bool("update", "notify", true) && !snoozed &&
                      (!cfg.get_bool("update", "notify_security_only", false) || security + critical > 0);
        if (count && wanted && signature != state.notified) {
          std::string summary = count == 1 ? std::string(_("1 software update is available"))
                                           : format(_("%d software updates are available"), count);
          std::string body;
          if (critical && security)
            body = format(_("%d critical and %d security updates. Open Software Updater to install them."),
                          critical, security);
          else if (critical)
            body = format(_("%d of them are critical updates. Open Software Updater to install them."), critical);
          else if (security)
            body = format(_("%d of them are security updates. Open Software Updater to install them."), security);
          else
            body = format(_("%s to download. Open Software Updater to review them."),
                          format_size(list.total_size()).c_str());
          notify(summary, body);
          state.notified = signature;
          state.remind_until = 0;
        }
        if (!count) state.notified.clear();
        printf("sde-update: %d updates (%d security)\n", count, security);
        fflush(stdout);
      }
      save_state(state);
      int hours = cfg.get_int("update", "check_interval", 24);
      double next = hours > 0 ? hours * 3600.0 : 900;
      // Wake up when Remind Me Later ends.
      std::time_t now = std::time(nullptr);
      if (state.remind_until > now) next = std::min(next, double(state.remind_until - now) + 5);
      later(next);
    });
  }
  bool once_;
};

int run_notifier(bool once) {
  // One notifier per user session.
  std::string lock = join_path(runtime_dir(), "sde-update-notifier.lock");
  int fd = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (!once) return 0;
  }
  Notifier notifier(once);
  // Let the session settle (and the network come up) before the first check.
  notifier.start(once ? 0 : 120);
  while (!notifier.done) Fl::wait(1e6);
  // Give a pending notification call time to go out.
  for (int i = 0; i < 20; ++i) Fl::wait(0.05);
  return 0;
}

void usage(FILE *out) {
  fprintf(out, "%s",
          _("Usage: sde-update [--page NAME] [--version]\n"
            "       sde-update --check | --history | --sources\n"
            "       sde-update --plan refresh|upgrade|autoremove|clean|repair [PACKAGE...]\n"
            "       sde-update --plan flatpak system|user:REF...\n"
            "       sde-update --transaction refresh|upgrade\n"
            "       sde-update --notifier [--once]\n\n"
            "  --page NAME     open updates, history, sources, maintenance, settings or about\n"
            "  --check         print the read-only update list (SDE-UPDATES protocol)\n"
            "  --history       print the APT history (SDE-HISTORY protocol)\n"
            "  --sources       print the package sources (SDE-SOURCES protocol)\n"
            "  --plan          print the fixed command of a transaction without running it\n"
            "  --transaction   run an interactive PackageKit transaction in this terminal\n"
            "  --notifier      check for updates in the background and notify\n"));
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string first = args.empty() ? "" : args[0];
  if (first == "--version" && args.size() == 1) return print_version("sde-update", SDE_UPDATE_VERSION);
  if (first == "--help") {
    usage(stdout);
    return 0;
  }
  if (first == "--check" && args.size() == 1) return exec_helper("check.py");
  if (first == "--history" && args.size() == 1) return exec_helper("history.py");
  if (first == "--sources" && args.size() == 1) return exec_helper("sources.py");
  if (first == "--plan" && args.size() >= 2) {
    std::vector<std::string> extra = {"--plan"};
    extra.insert(extra.end(), args.begin() + 1, args.end());
    return exec_helper("transaction.py", extra);
  }
  if (first == "--transaction" && args.size() == 2) return run_terminal_transaction(args[1]);
  if (first == "--notifier" && (args.size() == 1 || (args.size() == 2 && args[1] == "--once")))
    return run_notifier(args.size() == 2);
  if (args.empty()) return run_app("updates");
  if (first == "--page" && args.size() == 2) return run_app(args[1]);
  usage(stderr);
  return 2;
}
