// SPDX-License-Identifier: GPL-3.0-or-later
#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

namespace sde {

bool spawn_detached(const std::vector<std::string> &argv, const std::string &work_dir) {
  if (argv.empty() || find_program(argv[0]).empty()) return false;

  std::vector<char *> args;
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    // First child: new session, then fork again so the grandchild is
    // re-parented to init (or the session's subreaper) and never becomes a
    // zombie of the caller.
    setsid();
    pid_t pid2 = fork();
    if (pid2 != 0) _exit(pid2 < 0 ? 1 : 0);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    // Do not leak the X connection or other descriptors into children.
    for (int fd = 3; fd < 1024; ++fd) close(fd);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, 0);
      if (devnull > 0) close(devnull);
    }
    if (!work_dir.empty() && chdir(work_dir.c_str()) != 0) {
      if (chdir(home_dir().c_str()) != 0) { /* keep the current directory */ }
    }
    execvp(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string terminal_exec_arg(const std::string &program, const DesktopEntry *entry) {
  if (entry && entry->has_exec_arg) return entry->exec_arg;
  std::string p = basename_of(program);
  if (p == "gnome-terminal" || p == "kgx" || p == "ptyxis" || p == "gnome-console") return "--";
  if (p == "xfce4-terminal" || p == "terminator" || p == "mate-terminal") return "-x";
  if (p == "kitty" || p == "foot" || p == "footclient" || p == "st") return "";
  return "-e";
}

bool open_terminal(const std::vector<std::string> &argv, const std::string &work_dir) {
  // The terminal chosen in Settings > Default Applications.
  Config cfg;
  cfg.load();
  std::string id = cfg.get("applications", "terminal", "auto");
  DesktopEntry entry;
  if (!id.empty() && id != "auto" && find_application(id, entry)) {
    std::vector<std::string> cmd = entry.command({});
    if (!cmd.empty() && !find_program(cmd[0]).empty()) {
      if (!argv.empty()) {
        std::string arg = terminal_exec_arg(cmd[0], &entry);
        if (!arg.empty()) cmd.push_back(arg);
        cmd.insert(cmd.end(), argv.begin(), argv.end());
      }
      if (spawn_detached(cmd, work_dir.empty() ? home_dir() : work_dir)) return true;
    }
  }
  static const char *terminals[] = {"x-terminal-emulator", "xfce4-terminal", "lxterminal",
                                    "qterminal", "mate-terminal", "gnome-terminal",
                                    "konsole", "xterm"};
  for (const char *t : terminals) {
    if (find_program(t).empty()) continue;
    std::vector<std::string> cmd{t};
    if (!argv.empty()) {
      // gnome-terminal wants "--", xfce4-terminal "-x", most others "-e".
      std::string arg = terminal_exec_arg(t);
      if (!arg.empty()) cmd.push_back(arg);
      cmd.insert(cmd.end(), argv.begin(), argv.end());
    }
    return spawn_detached(cmd, work_dir.empty() ? home_dir() : work_dir);
  }
  return false;
}

bool open_uri(const std::string &uri) {
  if (!find_program("xdg-open").empty()) return spawn_detached({"xdg-open", uri});
  if (!find_program("gio").empty()) return spawn_detached({"gio", "open", uri});
  return false;
}

bool launch_entry(const DesktopEntry &entry, const std::vector<std::string> &files) {
  if (entry.type == "Link") return !entry.url.empty() && open_uri(entry.url);
  auto argv = entry.command(files);
  if (argv.empty()) return false;
  std::string dir = entry.work_dir.empty() ? home_dir() : entry.work_dir;
  if (entry.terminal) return open_terminal(argv, dir);
  return spawn_detached(argv, dir);
}

}  // namespace sde
