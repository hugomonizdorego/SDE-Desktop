// SPDX-License-Identifier: GPL-3.0-or-later
#include "process.h"

#include <FL/Fl.H>
#include <fcntl.h>
#include <poll.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>

#include "util.h"

extern "C" char **environ;

namespace sde {

namespace {

struct Job {
  int id = 0;
  pid_t pid = -1;
  int fd = -1;
  std::string buffer;
  DoneFn done;
  LineFn on_line;
  std::function<void()> on_exit;
};

std::map<int, std::unique_ptr<Job>> g_jobs;
int g_next_id = 1;

// The child environment: ours with LC_ALL=C, since helper output is parsed
// and must not be translated. Built once; SDE programs do not change it later.
char **child_environment() {
  static std::vector<std::string> storage;
  static std::vector<char *> envp;
  if (envp.empty()) {
    for (char **e = environ; e && *e; ++e)
      if (strncmp(*e, "LC_ALL=", 7) != 0) storage.push_back(*e);
    storage.push_back("LC_ALL=C");
    for (auto &v : storage) envp.push_back(const_cast<char *>(v.c_str()));
    envp.push_back(nullptr);
  }
  return envp.data();
}

// posix_spawn (vfork-like in glibc) instead of fork(): the panel and the
// other SDE programs map cairo, fonts and icon caches, and copying their page
// tables for every nmcli/pactl call made helper starts noticeably slower.
pid_t start(const std::vector<std::string> &argv, int &out_fd) {
  out_fd = -1;
  if (argv.empty()) return -1;
  std::string path = find_program(argv[0]);
  if (path.empty()) return -1;
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return -1;
  std::vector<char *> args;
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  sigset_t none, dflt;
  sigemptyset(&none);
  sigemptyset(&dflt);
  sigaddset(&dflt, SIGPIPE);
  sigaddset(&dflt, SIGCHLD);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setsigdefault(&attr, &dflt);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  pid_t pid = -1;
  int err = posix_spawn(&pid, path.c_str(), &fa, &attr, args.data(), child_environment());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    return -1;
  }
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  out_fd = fds[0];
  return pid;
}

void finish(int id);

void reap_later(void *data) {
  int id = int(reinterpret_cast<intptr_t>(data));
  finish(id);
}

void finish(int id) {
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return;
  Job &job = *it->second;
  int status = 0;
  pid_t r = waitpid(job.pid, &status, WNOHANG);
  if (r == 0) {
    // Output closed but the process is still exiting.
    Fl::add_timeout(0.05, reap_later, reinterpret_cast<void *>(intptr_t(id)));
    return;
  }
  int code = (r == job.pid && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
  std::unique_ptr<Job> owned = std::move(it->second);
  g_jobs.erase(it);
  if (owned->on_line && !owned->buffer.empty()) owned->on_line(owned->buffer);
  if (owned->done) owned->done(owned->buffer, code);
  if (owned->on_exit) owned->on_exit();
}

void on_readable(int fd, void *data) {
  int id = int(reinterpret_cast<intptr_t>(data));
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) {
    Fl::remove_fd(fd);
    return;
  }
  Job &job = *it->second;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
      job.buffer.append(buf, size_t(n));
      if (job.on_line) {
        size_t nl;
        while ((nl = job.buffer.find('\n')) != std::string::npos) {
          std::string line = job.buffer.substr(0, nl);
          job.buffer.erase(0, nl + 1);
          job.on_line(line);
          if (!g_jobs.count(id)) return;  // stopped from the callback
        }
      }
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    // EOF or error.
    Fl::remove_fd(fd);
    close(fd);
    job.fd = -1;
    finish(id);
    return;
  }
}

int add_job(const std::vector<std::string> &argv, DoneFn done, LineFn on_line,
            std::function<void()> on_exit) {
  int fd;
  pid_t pid = start(argv, fd);
  if (pid < 0) return 0;
  auto job = std::make_unique<Job>();
  job->id = g_next_id++;
  job->pid = pid;
  job->fd = fd;
  job->done = std::move(done);
  job->on_line = std::move(on_line);
  job->on_exit = std::move(on_exit);
  int id = job->id;
  g_jobs[id] = std::move(job);
  Fl::add_fd(fd, FL_READ, on_readable, reinterpret_cast<void *>(intptr_t(id)));
  return id;
}

}  // namespace

bool run_async(const std::vector<std::string> &argv, DoneFn done) {
  if (add_job(argv, done, nullptr, nullptr)) return true;
  bool installed = !argv.empty() && !find_program(argv[0]).empty();
  if (installed && done) done("", -1);
  return installed;
}

int stream_lines(const std::vector<std::string> &argv, LineFn on_line,
                 std::function<void()> on_exit) {
  return add_job(argv, nullptr, std::move(on_line), std::move(on_exit));
}

void stop_stream(int id) {
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return;
  Job &job = *it->second;
  if (job.fd >= 0) {
    Fl::remove_fd(job.fd);
    close(job.fd);
  }
  kill(job.pid, SIGTERM);
  waitpid(job.pid, nullptr, 0);
  g_jobs.erase(it);
}

bool signal_stream(int id, int sig) {
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return false;
  return kill(it->second->pid, sig) == 0;
}

std::string run_sync(const std::vector<std::string> &argv, int *status, int timeout_ms) {
  if (status) *status = -1;
  int fd;
  pid_t pid = start(argv, fd);
  if (pid < 0) return "";
  std::string out;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
    if (elapsed >= timeout_ms) {
      kill(pid, SIGKILL);
      break;
    }
    struct pollfd p{fd, POLLIN, 0};
    int r = poll(&p, 1, int(timeout_ms - elapsed));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) continue;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
      out.append(buf, size_t(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
    break;
  }
  close(fd);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (status && WIFEXITED(st)) *status = WEXITSTATUS(st);
  return out;
}

}  // namespace sde
