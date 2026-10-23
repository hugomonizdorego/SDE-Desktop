// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// One Niny window per display: a second "niny file" hands its files to the
// running editor through a UNIX socket in $XDG_RUNTIME_DIR and exits.
#include <FL/Fl.H>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "niny.h"
#include "util.h"

using namespace sde;

namespace niny {

namespace {

int g_listen = -1;
App *g_app = nullptr;
std::string g_path;

std::string socket_path() {
  std::string display = env("DISPLAY", ":0");
  for (char &c : display)
    if (!isalnum((unsigned char)c)) c = '_';
  return runtime_dir() + "/niny-" + display + ".sock";
}

bool fill_addr(sockaddr_un &addr, const std::string &path) {
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) return false;
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  return true;
}

struct Client {
  int fd;
  std::string data;
};

void client_cb(int fd, void *p) {
  auto *c = static_cast<Client *>(p);
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n > 0) {
    c->data.append(buf, size_t(n));
    if (c->data.size() < (1u << 20)) return;
  }
  Fl::remove_fd(fd);
  close(fd);
  // Protocol: "NINY1\n" cwd "\n" then one argument per line.
  auto lines = split(c->data, '\n', true);
  if (lines.size() >= 2 && lines[0] == "NINY1" && g_app) {
    std::string cwd = lines[1];
    std::vector<std::string> args(lines.begin() + 2, lines.end());
    while (!args.empty() && args.back().empty()) args.pop_back();
    g_app->open_files(args, cwd);
    g_app->raise_window();
  }
  delete c;
}

void accept_cb(int fd, void *) {
  int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (c < 0) return;
  Fl::add_fd(c, FL_READ, client_cb, new Client{c, ""});
}

}  // namespace

bool hand_off_to_running(const std::vector<std::string> &args, const std::string &cwd) {
  sockaddr_un addr;
  if (!fill_addr(addr, socket_path())) return false;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
    close(fd);
    return false;
  }
  std::string msg = "NINY1\n" + cwd + "\n";
  for (auto &a : args)
    if (a.find('\n') == std::string::npos) msg += a + "\n";
  size_t off = 0;
  while (off < msg.size()) {
    ssize_t n = write(fd, msg.data() + off, msg.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      close(fd);
      return false;
    }
    off += size_t(n);
  }
  close(fd);
  return true;
}

bool another_instance_running() {
  sockaddr_un addr;
  if (!fill_addr(addr, socket_path())) return false;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  bool ok = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
  close(fd);
  return ok;
}

void listen_for_instances(App *app) {
  // A second window (--new-window) leaves the socket to the first one.
  if (another_instance_running()) return;
  g_app = app;
  g_path = socket_path();
  sockaddr_un addr;
  if (!fill_addr(addr, g_path)) return;
  make_dirs(runtime_dir());
  unlink(g_path.c_str());  // stale socket (the connect above failed)
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  mode_t old = umask(0077);
  int ok = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
  umask(old);
  if (ok != 0 || listen(fd, 8) != 0) {
    close(fd);
    return;
  }
  g_listen = fd;
  Fl::add_fd(fd, FL_READ, accept_cb, nullptr);
}

void stop_listening() {
  if (g_listen >= 0) {
    Fl::remove_fd(g_listen);
    close(g_listen);
    unlink(g_path.c_str());
    g_listen = -1;
  }
  g_app = nullptr;
}

}  // namespace niny
