// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Path safety for extraction and the external tool runner.
//
// An archive is untrusted input. Member names such as "../../.bashrc",
// "/etc/passwd" or "C:\\x", and symbolic links that point out of the
// destination, are never written. Every extraction path in Taca goes
// through sanitize_member_path() and, for links, symlink_stays_inside().
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "engine.h"
#include "util.h"

namespace taca {

bool sanitize_member_path(const std::string &name, std::string &out) {
  out.clear();
  if (name.empty()) return false;
  for (unsigned char c : name)
    if (c < 0x20 || c == 0x7f) return false;
  // Windows style names from zip and rar archives.
  std::string n = name;
  if (n.size() >= 2 && n[1] == ':' && isalpha(static_cast<unsigned char>(n[0]))) return false;
  if (n[0] == '/' || n[0] == '\\') return false;
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= n.size(); ++i) {
    if (i == n.size() || n[i] == '/') {
      std::string part = n.substr(start, i - start);
      start = i + 1;
      if (part.empty() || part == ".") continue;
      if (part == "..") return false;
      // "..\\x" or "a\\..\\..\\x" written on Windows.
      std::string bs = part;
      size_t s2 = 0;
      for (size_t j = 0; j <= bs.size(); ++j) {
        if (j == bs.size() || bs[j] == '\\') {
          if (bs.substr(s2, j - s2) == "..") return false;
          s2 = j + 1;
        }
      }
      parts.push_back(part);
    }
  }
  if (parts.empty()) return false;
  for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
  return true;
}

bool symlink_stays_inside(const std::string &where, const std::string &target) {
  if (target.empty() || target[0] == '/') return false;
  if (target.size() >= 2 && target[1] == ':') return false;
  // Depth of the folder that holds the link, below the destination.
  std::string clean;
  if (!sanitize_member_path(where, clean)) return false;
  int depth = 0;
  for (char c : clean)
    if (c == '/') ++depth;
  size_t start = 0;
  for (size_t i = 0; i <= target.size(); ++i) {
    if (i == target.size() || target[i] == '/' || target[i] == '\\') {
      std::string part = target.substr(start, i - start);
      start = i + 1;
      if (part.empty() || part == ".") continue;
      if (part == "..") {
        if (--depth < 0) return false;
      } else {
        ++depth;
      }
    }
  }
  return true;
}

bool has_symlink_component(const std::string &root, const std::string &rel) {
  std::string path = root;
  size_t start = 0;
  for (size_t i = 0; i <= rel.size(); ++i) {
    if (i == rel.size() || rel[i] == '/') {
      std::string part = rel.substr(start, i - start);
      start = i + 1;
      if (part.empty()) continue;
      path += "/" + part;
      struct stat st;
      if (lstat(path.c_str(), &st) != 0) return false;  // the rest does not exist yet
      if (S_ISLNK(st.st_mode)) return true;
    }
  }
  return false;
}

std::string unique_path(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return path;
  size_t slash = path.rfind('/');
  std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  std::string stem = base, ext;
  size_t dot = base.find('.', 1);
  // Keep compound extensions together: "a.tar.gz" -> "a (2).tar.gz".
  if (dot != std::string::npos && dot > 0) {
    stem = base.substr(0, dot);
    ext = base.substr(dot);
  }
  for (int i = 2; i < 100000; ++i) {
    std::string candidate = dir + stem + " (" + std::to_string(i) + ")" + ext;
    if (lstat(candidate.c_str(), &st) != 0) return candidate;
  }
  return path + ".new";
}

int run_tool(const std::vector<std::string> &argv, const std::string &cwd,
             const std::function<void(const std::string &)> &on_line, const std::atomic<bool> *cancel,
             std::string *output) {
  if (argv.empty()) return -1;
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    setpgid(0, 0);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) dup2(devnull, 0);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    std::vector<char *> args;
    for (const auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(fds[1]);
  std::string line;
  bool killed = false;
  char buf[8192];
  for (;;) {
    if (cancel && cancel->load() && !killed) {
      kill(-pid, SIGTERM);
      kill(pid, SIGTERM);
      killed = true;
    }
    struct pollfd p{fds[0], POLLIN, 0};
    int r = poll(&p, 1, 100);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) continue;
    ssize_t got = read(fds[0], buf, sizeof buf);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    if (output && output->size() < (8u << 20)) output->append(buf, size_t(got));
    for (ssize_t i = 0; i < got; ++i) {
      char c = buf[i];
      if (c == '\n' || c == '\r' || c == '\b') {
        if (!line.empty() && on_line) on_line(line);
        line.clear();
      } else {
        line += c;
      }
    }
  }
  if (!line.empty() && on_line) on_line(line);
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (killed) return -2;
  if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
  return 255;
}

}  // namespace taca
