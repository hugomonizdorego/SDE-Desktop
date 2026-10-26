// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The apt backend: debrun-helper through pkexec (helper.h), its output read
// line by line from the FLTK loop. pkexec exits with 126 when the password
// dialog was dismissed and 127 when the authorization failed; otherwise the
// helper's own status counts.
#include <FL/Fl.H>
#include <fcntl.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "backend.h"
#include "build_config.h"
#include "config.h"
#include "deb.h"
#include "helper.h"
#include "i18n.h"
#include "util.h"

extern "C" char **environ;

namespace sde {
namespace debrun {

namespace {

std::string helper_path() { return std::string(SDE_LIBEXECDIR) + "/sde/debrun-helper"; }

class HelperBackend : public Backend {
 public:
  ~HelperBackend() override {
    if (fd_ >= 0) {
      Fl::remove_fd(fd_);
      close(fd_);
    }
  }
  std::string name() const override { return "APT"; }
  bool running() const override { return pid_ > 0; }

  void install(const Package &p, bool reinstall, bool downgrade, const std::vector<std::string> &removals,
               Callbacks cb) override {
    helper::Request r;
    r.action = "install";
    r.file = p.path;
    r.sha256 = p.sha256;
    r.package = p.name;
    r.version = p.version;
    r.arch = p.architecture;
    r.reinstall = reinstall;
    r.downgrade = downgrade;
    r.allow_remove = removals;
    start(r, std::move(cb));
  }
  void remove(const std::string &name, const std::vector<std::string> &also, Callbacks cb) override {
    helper::Request r;
    r.action = "remove";
    r.package = name;
    r.allow_remove = also;
    start(r, std::move(cb));
  }
  void refresh(Callbacks cb) override {
    helper::Request r;
    r.action = "refresh";
    start(r, std::move(cb));
  }

 private:
  void start(const helper::Request &r, Callbacks cb) {
    cb_ = std::move(cb);
    hello_ = false;
    result_.clear();
    code_.clear();
    text_.clear();
    output_.clear();
    std::vector<std::string> argv;
    std::string test = env("DEBRUN_TEST_HELPER");
    if (!test.empty()) {
      argv = {test};  // tests: without pkexec, so it gains nothing
    } else {
      if (find_program("pkexec").empty()) {
        fail("backend", _("pkexec is not installed (package pkexec): Debrun cannot ask for permission."));
        return;
      }
      argv = {"pkexec", helper_path()};
    }
    for (auto &a : helper::request_args(r)) argv.push_back(a);
    std::string path = find_program(argv[0]);
    int fds[2];
    if (path.empty() || pipe2(fds, O_CLOEXEC) != 0) {
      fail("backend", format(_("%s cannot be started."), argv[0].c_str()));
      return;
    }
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
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
    int err = posix_spawn(&pid, path.c_str(), &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (err != 0) {
      close(fds[0]);
      fail("backend", format(_("%s cannot be started: %s"), argv[0].c_str(), strerror(err)));
      return;
    }
    pid_ = pid;
    fd_ = fds[0];
    fcntl(fd_, F_SETFL, O_NONBLOCK);
    Step s;
    s.phase = "auth";
    if (cb_.step) cb_.step(s);
    Fl::add_fd(fd_, FL_READ, readable, this);
  }

  static void readable(int fd, void *data) {
    auto *self = static_cast<HelperBackend *>(data);
    char buf[4096];
    for (;;) {
      ssize_t n = read(fd, buf, sizeof buf);
      if (n > 0) {
        self->buffer_.append(buf, size_t(n));
        size_t nl;
        while ((nl = self->buffer_.find('\n')) != std::string::npos) {
          std::string line = self->buffer_.substr(0, nl);
          self->buffer_.erase(0, nl + 1);
          self->line(line);
        }
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == EAGAIN) return;
      // End of output: the helper (or pkexec) is done.
      Fl::remove_fd(fd);
      close(fd);
      self->fd_ = -1;
      if (!self->buffer_.empty()) self->line(self->buffer_);
      self->buffer_.clear();
      reap(self);
      return;
    }
  }

  static void reap(void *data) {
    auto *self = static_cast<HelperBackend *>(data);
    int st = 0;
    pid_t r = waitpid(self->pid_, &st, WNOHANG);
    if (r == 0) {
      Fl::add_timeout(0.05, reap, data);
      return;
    }
    self->pid_ = -1;
    int code = r > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    self->finished(code);
  }

  void line(const std::string &raw) {
    helper::Event e = helper::parse_event(raw);
    switch (e.type) {
      case helper::Event::Type::hello: hello_ = true; break;
      case helper::Event::Type::phase:
      case helper::Event::Type::progress: {
        Step s;
        s.percent = e.percent;
        s.phase = e.word;
        s.package = e.package;
        if (cb_.step) cb_.step(s);
        break;
      }
      case helper::Event::Type::log:
        if (output_.size() < (1u << 20)) output_ += e.text + "\n";
        if (cb_.log) cb_.log(e.text);
        break;
      case helper::Event::Type::error:
        code_ = e.word;
        text_ = e.text;
        break;
      case helper::Event::Type::result: result_ = e.word; break;
      case helper::Event::Type::none:
        // pkexec's own messages ("Error executing command as another user").
        if (!trim(raw).empty()) {
          if (output_.size() < (1u << 20)) output_ += raw + "\n";
          if (cb_.log) cb_.log(raw);
        }
        break;
    }
  }

  void finished(int status) {
    Outcome o;
    if (result_ == "ok" && status == 0) {
      o.ok = true;
    } else if (!hello_ && (status == 126 || status == 127)) {
      // pkexec: 126 the dialog was dismissed, 127 not authorized.
      o.cancelled = status == 126;
      o.code = "auth";
      o.message = status == 126 ? _("The installation was cancelled: the administrator password was not given.")
                                : _("Permission was refused: the password was wrong, or this account may not "
                                    "install software.");
      o.detail = trim(output_);
    } else {
      o.code = code_.empty() ? "failed" : code_;
      o.detail = !text_.empty() ? text_ : trim(output_);
      o.message = error_message(o.code, text_);
    }
    Callbacks cb = std::move(cb_);
    cb_ = Callbacks();
    if (cb.done) cb.done(o);
  }

  void fail(const std::string &code, const std::string &message) {
    Outcome o;
    o.code = code;
    o.message = message;
    Callbacks cb = std::move(cb_);
    cb_ = Callbacks();
    if (cb.done) cb.done(o);
  }

  Callbacks cb_;
  pid_t pid_ = -1;
  int fd_ = -1;
  std::string buffer_, output_, result_, code_, text_;
  bool hello_ = false;
};

}  // namespace

std::unique_ptr<Backend> make_helper_backend() { return std::unique_ptr<Backend>(new HelperBackend()); }

std::unique_ptr<Backend> make_backend() {
  Config cfg;
  cfg.load();
  std::string choice = env("DEBRUN_BACKEND", cfg.get("debrun", "backend", "auto"));
  if (choice == "packagekit") return make_packagekit_backend();
  if (choice == "apt") return make_helper_backend();
  bool helper = !env("DEBRUN_TEST_HELPER").empty() ||
                (!find_program("pkexec").empty() && is_executable(helper_path()));
  return helper ? make_helper_backend() : make_packagekit_backend();
}

std::string error_message(const std::string &code, const std::string &detail) {
  std::string d = trim(detail);
  if (code == "open")
    return d.empty() ? std::string(_("The package file cannot be read."))
                     : format(_("The package file cannot be read: %s."), d.c_str());
  if (code == "changed")
    return _("The package file changed after Debrun showed it. Nothing was installed: open the file again to "
             "check it.");
  if (code == "arch") return _("This package is not made for the architecture of this computer.");
  if (code == "unmet")
    return d.empty() ? std::string(_("The packages it needs cannot be installed from the package sources."))
                     : format(_("The packages it needs cannot be installed from the package sources (%s)."),
                              d.c_str());
  if (code == "held")
    return _("A package it involves is held at its version (apt-mark hold). Nothing was changed; an administrator "
             "can release it with \"apt-mark unhold\".");
  if (code == "essential")
    return d.empty() ? std::string(_("This would remove a package the system or the desktop needs. Nothing was "
                                     "changed."))
                     : format(_("This would remove a package the system or the desktop needs (%s). Nothing was "
                                "changed."),
                              d.c_str());
  if (code == "removals")
    return _("The change would now also remove packages that were not shown. Nothing was changed: open the package "
             "again to see the new plan.");
  if (code == "lock")
    return _("Another program is installing or removing software. Try again when it has finished.");
  if (code == "space") return _("There is not enough free disk space.");
  if (code == "usage" || code == "root")
    return format(_("The helper of Debrun refused the request (%s)."), d.c_str());
  return d.empty() ? std::string(_("The package system reported an error."))
                   : format(_("The package system reported an error: %s"), d.c_str());
}

}  // namespace debrun
}  // namespace sde
