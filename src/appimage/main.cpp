// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-appimage: runs AppImages with a double-click, after it verified them
// (docs/APPIMAGE.md, man sde-appimage). Liria and the desktop open AppImages
// with it (sde-appimage.desktop, NoDisplay), also executable ones.
//
//   sde-appimage FILE [ARGUMENTS...]  verify FILE; run it when it can be
//                                     trusted, else explain why and offer
//                                     to delete it
//   sde-appimage --check FILE         print the verdict (no window)
//   sde-appimage --forget FILE        forget that FILE was verified or accepted
//   sde-appimage --version | --help
//
// A trusted AppImage replaces this process (fexecve of the file that was
// verified, so no other file can take its place in between); its exit
// status is the application's. Otherwise: 1 not run (kept, deleted or an
// error), 2 usage.
#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <dirent.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "about.h"
#include "appimage.h"
#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "crash.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

extern "C" char **environ;

using namespace sde;
using namespace sde::appimage;

namespace {

AppInfo about_info() {
  AppInfo info;
  info.name = _("AppImage Runner");
  info.program = "sde-appimage";
  info.version = SDE_VERSION;
  info.icon = "sde-appimage";
  info.description = _("Runs AppImages with a double-click after it checked their signature, and keeps those it "
                       "cannot verify from running.");
  info.credits = {toolkit_credit(), _("Signatures checked with gpgv (GnuPG) or sqv (Sequoia)"),
                  _("The AppImage format: the AppImage project (appimage.org)")};
  return info;
}

std::string usage() {
  return _("sde-appimage: run an AppImage after verifying it.\n\n"
           "Usage: sde-appimage FILE [ARGUMENTS...]\n"
           "       sde-appimage --check FILE\n"
           "       sde-appimage --forget FILE\n"
           "       sde-appimage --version\n\n"
           "An AppImage runs when its signature is valid (and made with the same key as the earlier\n"
           "versions of the application) or when it was verified before. Otherwise a window explains\n"
           "why and offers to delete it. The administrator's policy is /etc/sde/appimage.conf.\n");
}

// ----------------------------------------------------------------- the log

// ~/.local/state/sde/appimage.log: what was run or refused (the previous
// megabyte in appimage.log.1).
void log_line(const std::string &text) {
  std::string dir = state_home() + "/sde", path = dir + "/appimage.log";
  make_dirs(dir);
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && st.st_size > (1 << 20)) rename(path.c_str(), (path + ".1").c_str());
  if (FILE *f = fopen(path.c_str(), "ae")) {
    char when[32];
    time_t t = time(nullptr);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&t));
    fprintf(f, "%s %s\n", when, text.c_str());
    fclose(f);
  }
}

std::string origin_url(const std::string &path) {
  char buf[1024];
  ssize_t n = getxattr(path.c_str(), "user.xdg.origin.url", buf, sizeof buf - 1);
  if (n <= 0) return "";
  std::string url(buf, size_t(n));
  for (unsigned char c : url)
    if (c < 0x21 || c == 0x7F) return "";
  return starts_with(url, "https://") || starts_with(url, "http://") ? url : "";
}

std::string format_size(unsigned long long bytes) {
  if (bytes < 1000) return format(_("%llu bytes"), bytes);
  const char *units[] = {"kB", "MB", "GB", "TB"};
  double v = double(bytes);
  int u = -1;
  while (v >= 1000 && u < 3) {
    v /= 1000;
    ++u;
  }
  return format(v >= 100 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

// "ABCD 1234 ..." in groups of four, as GnuPG prints fingerprints.
std::string group(const std::string &fpr, size_t n = 4) {
  std::string out;
  for (size_t i = 0; i < fpr.size(); i += n) out += (i ? " " : "") + fpr.substr(i, n);
  return out;
}

// --------------------------------------------------------------- running

// The C library directory of this architecture (libfuse.so.2 lives there).
std::vector<std::string> library_dirs() {
  std::string a = host_arch();
  std::string triplet = a == "x86_64"    ? "x86_64-linux-gnu"
                        : a == "aarch64" ? "aarch64-linux-gnu"
                        : a == "armhf"   ? "arm-linux-gnueabihf"
                        : a == "i386"    ? "i386-linux-gnu"
                        : a == "riscv64" ? "riscv64-linux-gnu"
                        : a == "ppc64el" ? "powerpc64le-linux-gnu"
                                         : a + "-linux-gnu";
  return {"/lib/" + triplet, "/usr/lib/" + triplet, "/lib64", "/usr/lib64", "/lib", "/usr/lib"};
}

// Whether the AppImage can mount itself with FUSE: /dev/fuse, fusermount
// and, for runtimes before 2023 (dynamically linked), libfuse.so.2.
bool fuse_usable(int fd, const Info &info, std::string *why) {
  if (env("SDE_APPIMAGE_NO_FUSE") == "1") {
    *why = "disabled";
    return false;
  }
  if (access("/dev/fuse", R_OK | W_OK) != 0) {
    *why = "no /dev/fuse";
    return false;
  }
  if (find_program("fusermount3").empty() && find_program("fusermount").empty()) {
    *why = "no fusermount";
    return false;
  }
  // The runtime part names the library it loads.
  size_t n = size_t(std::min<uint64_t>(info.elf_size ? info.elf_size : info.size, 4u << 20));
  std::string runtime(n, '\0');
  ssize_t got = pread(fd, &runtime[0], n, 0);
  if (got > 0 && runtime.find("libfuse.so.2") != std::string::npos) {
    for (const auto &d : library_dirs())
      if (file_exists(d + "/libfuse.so.2")) return true;
    *why = "no libfuse.so.2";
    return false;
  }
  return true;
}

// The directory of the private copies (AppImages on drives that run no
// programs, or that others may change).
std::string copies_dir() { return data_home() + "/sde/appimages"; }

// The file to execute: |fd| itself when it may run where it is (made
// executable for its owner when it is not yet), else a copy in
// copies_dir(). -1 with *error when it cannot run.
int runnable(int fd, const std::string &path, const Check &c, TrustStore &store, std::string *copy,
             std::string *error) {
  struct stat st;
  struct statvfs vfs;
  if (fstat(fd, &st) != 0) {
    *error = strerror(errno);
    return -1;
  }
  bool noexec = fstatvfs(fd, &vfs) == 0 && (vfs.f_flag & ST_NOEXEC);
  // What was verified is what runs (the open file, not its name), unless
  // someone else may write to it between now and then: then a copy runs.
  bool others_write = (st.st_mode & S_IWOTH) || ((st.st_mode & S_IWGRP) && st.st_gid != getgid());
  bool safe = (st.st_uid == getuid() || st.st_uid == 0) && !others_write;
  if (!noexec && safe) {
    if (st.st_uid == getuid() ? (st.st_mode & S_IXUSR) : (st.st_mode & S_IXOTH)) return fd;
    // Verified: it may run. Only its owner gets the executable bit.
    if (st.st_uid == getuid() && fchmod(fd, (st.st_mode & 07777) | S_IXUSR) == 0) {
      store.add_seen(seen_of(fd, path, c.sha256));
      return fd;
    }
  }
  // A drive that runs no programs (noexec), or a file of another user: a
  // private copy that is checked again.
  std::string dir = copies_dir();
  make_dirs(data_home() + "/sde");
  if (!private_dir(dir)) {
    *error = format(_("%s cannot be created."), dir.c_str());
    return -1;
  }
  struct statvfs home;
  if (statvfs(dir.c_str(), &home) == 0 && (home.f_flag & ST_NOEXEC)) {
    *error = _("This drive does not allow running programs (it is mounted with \"noexec\"), and neither does "
               "your home folder.");
    return -1;
  }
  *copy = dir + "/" + c.sha256.substr(0, 12) + "-" + basename_of(path);
  CheckOptions o;
  o.tmpdir = runtime_dir();
  // A copy made before (it may be running): used when it is still the same.
  int old = open(copy->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (old >= 0) {
    struct stat os;
    Check again = fstat(old, &os) == 0 && os.st_uid == getuid() && !(os.st_mode & 022)
                      ? check(old, *copy, store, o)
                      : Check();
    if (again.verdict == Verdict::trusted && again.sha256 == c.sha256 && (os.st_mode & S_IXUSR)) return old;
    close(old);
    unlink(copy->c_str());
  }
  int out = open(copy->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0700);
  if (out < 0) {
    *error = format(_("The copy cannot be made: %s"), strerror(errno));
    return -1;
  }
  char buf[1 << 16];
  off_t off = 0;
  bool ok = true;
  for (;;) {
    ssize_t n = pread(fd, buf, sizeof buf, off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = n == 0;
      break;
    }
    if (write(out, buf, size_t(n)) != n) {
      ok = false;
      break;
    }
    off += n;
  }
  close(out);
  int cfd = ok ? open(copy->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
  if (cfd < 0) {
    *error = _("The copy cannot be made: is your home folder full?");
    unlink(copy->c_str());
    return -1;
  }
  // The copy must be what was verified.
  o.use_seen = false;
  Check again = check(cfd, *copy, store, o);
  if (again.verdict != Verdict::trusted || again.sha256 != c.sha256) {
    close(cfd);
    unlink(copy->c_str());
    *error = _("The copy is not the AppImage that was verified.");
    return -1;
  }
  return cfd;
}

// Replaces this process with the AppImage. Returns only on failure.
void launch(int fd, const std::string &path, const Check &c, TrustStore &store,
            const std::vector<std::string> &args, std::string *error) {
  std::string copy;
  int exe = runnable(fd, path, c, store, &copy, error);
  if (exe < 0) return;
  std::string why;
  bool fuse = fuse_usable(exe, c.info, &why);
  if (!fuse) setenv("APPIMAGE_EXTRACT_AND_RUN", "1", 1);
  store.save();
  log_line(format("run %s sha256 %s app %s %s%s%s", path.c_str(), c.sha256.c_str(), c.app.c_str(),
                  c.how.c_str(), c.fingerprint.empty() ? "" : (" key " + c.fingerprint).c_str(),
                  fuse ? "" : (" extract-and-run (" + why + ")").c_str()));
  std::vector<std::string> argv = {copy.empty() ? path : copy};
  argv.insert(argv.end(), args.begin(), args.end());
  std::vector<char *> a;
  for (auto &s : argv) a.push_back(&s[0]);
  a.push_back(nullptr);
  // Nothing of this program (the X connection) goes to the application.
  if (fl_display) {
    XCloseDisplay(fl_display);
    fl_display = nullptr;
  }
  for (int f = 3; f < 1024; ++f)
    if (f != exe) fcntl(f, F_SETFD, FD_CLOEXEC);
  fexecve(exe, a.data(), environ);
  *error = format(_("It cannot be started: %s"), strerror(errno));
}

// ---------------------------------------------------------------- dialog

enum class Choice { none, keep, remove, run };

struct Texts {
  std::string title, body;
  std::string window;              // the window title
  std::vector<std::string> meta;   // for the QA dump ("verdict:unsigned" ...)
};

Texts explain(const Check &c, Policy policy, const std::string &path) {
  Texts t;
  std::string app = display_name(path);
  t.window = format(_("%s — AppImage Runner"), app.c_str());
  t.meta = {std::string("verdict:") + verdict_id(c.verdict),
            std::string("policy:") + (policy == Policy::ask ? "ask" : "verified-only")};
  if (!c.fingerprint.empty()) t.meta.push_back("key:" + c.fingerprint);
  if (!c.previous.empty()) t.meta.push_back("previous:" + c.previous);
  if (!c.error.empty()) t.meta.push_back("error:" + c.error);
  switch (c.verdict) {
    case Verdict::unsigned_image:
      t.title = _("This AppImage has no signature");
      t.body = _("Nobody vouches for it: SDE cannot check who made it and that nobody changed it since. Most "
                 "AppImages on the web are published this way; run only those whose maker signs them.");
      break;
    case Verdict::bad_signature:
      t.title = _("The signature of this AppImage is not valid");
      t.body = _("Its contents do not match its signature: it was changed after it was signed, or the signature is "
                 "forged. Do not run it.");
      if (c.error == "expired") t.body = _("It was signed with a key that has expired.");
      if (c.error == "revoked") t.body = _("It was signed with a key that its owner has revoked. Do not run it.");
      break;
    case Verdict::no_key:
      t.title = _("This AppImage cannot be verified");
      t.body = c.error == "no-verifier"
                   ? std::string(_("The signature checker (gpgv, package gpgv) is not installed."))
                   : std::string(_("It carries a signature, but not the key that made it, so the signature cannot be "
                                   "checked."));
      break;
    case Verdict::key_changed:
      t.title = _("This AppImage is signed with another key");
      t.body = format(_("Earlier versions of %s were signed with another key. Someone else may have made this "
                        "one. If its maker announced a new key, forget the old one with \"sde-appimage --forget\" "
                        "and open it again."),
                      app.c_str());
      t.body += "\n" + format(_("Key before: %s"), group(c.previous).c_str());
      t.body += "\n" + format(_("Key now: %s"), group(c.fingerprint).c_str());
      break;
    case Verdict::type1:
      t.title = _("This old AppImage cannot be verified");
      t.body = _("It is an AppImage of the first type, which cannot carry a signature.");
      break;
    case Verdict::wrong_arch:
      t.title = _("This AppImage is made for another kind of computer");
      t.body = format(_("It is made for %s computers; this computer is %s. Download the version for this computer."),
                      c.info.arch.c_str(), host_arch().c_str());
      break;
    case Verdict::not_appimage:
      t.title = _("This file is not an AppImage");
      t.body = _("It is not an AppImage, or it is damaged (perhaps the download did not finish).");
      break;
    case Verdict::unreadable:
      t.title = _("This file cannot be read");
      t.body = c.error;
      break;
    case Verdict::trusted:
      t.title = _("This AppImage is verified");
      t.body = c.signer.empty() ? std::string() : format(_("Signed by %s"), c.signer.c_str());
      break;
  }
  t.body += "\n\n" + format(_("File: %s"), basename_of(path).c_str());
  if (c.info.size) t.body += "  (" + format_size(c.info.size) + ")";
  std::string folder = path.substr(0, path.rfind('/'));
  t.body += "\n" + format(_("Folder: %s"), folder.empty() ? "/" : folder.c_str());
  std::string url = origin_url(path);
  if (!url.empty()) t.body += "\n" + format(_("Downloaded from: %s"), url.c_str());
  if (!c.sha256.empty()) t.body += "\n" + format(_("SHA-256: %s"), group(c.sha256, 8).c_str());
  bool may_run = policy == Policy::ask && (c.verdict == Verdict::unsigned_image || c.verdict == Verdict::type1 ||
                                           c.verdict == Verdict::no_key);
  t.body += "\n\n";
  t.body += may_run ? _("Your administrator allows running AppImages that cannot be verified after this warning. "
                        "Delete it unless you are sure where it came from.")
                    : _("SDE runs only AppImages it can verify. Delete it unless you are sure where it came from; "
                        "Keep leaves it where it is, and it does not run.");
  return t;
}

class Dialog : public BufferedWindow {
 public:
  Dialog(int w, int h, Texts texts, bool can_run)
      : BufferedWindow(w, h, nullptr), texts_(std::move(texts)) {
    copy_label(texts_.window.c_str());
    int pad = S(24), bh = S(36);
    auto add = [&](const std::string &label, const std::string &tip, Choice what, bool primary) {
      text_font(active_theme().font_size + 1);
      int bw = std::max(S(104), text_width(label) + S(40));
      auto *b = new FlatButton(0, 0, bw, bh, label, primary);
      b->copy_label(label.c_str());
      b->tooltip(tip.empty() ? nullptr : strdup(tip.c_str()));
      b->callback([](Fl_Widget *wd, void *d) {
        auto *self = static_cast<Dialog *>(wd->window());
        self->choice = Choice(reinterpret_cast<intptr_t>(d));
        self->hide();
      }, reinterpret_cast<void *>(intptr_t(what)));
      return b;
    };
    remove_ = add(_("Delete"), _("Move the AppImage to the trash"), Choice::remove, true);
    keep_ = add(_("Keep"), _("Keep the AppImage without running it"), Choice::keep, false);
    if (can_run) run_ = add(_("Run Anyway"), _("Run the AppImage although it cannot be verified"), Choice::run, false);
    end();
    int y = h - pad - bh;
    remove_->position(w - pad - remove_->w(), y);
    keep_->position(remove_->x() - S(10) - keep_->w(), y);
    if (run_) run_->position(pad, y);
  }
  // The width of the text in a window |w| wide.
  static int text_area(int w) { return w - 2 * S(24) - S(64) - S(18); }
  // Breaks the words of |text| wider than |width| (folders, file names)
  // after a '/' when it can, anywhere else when not: FLTK wraps only at
  // spaces.
  static std::string fit_words(const std::string &text, int width) {
    text_font(active_theme().font_size + 1);
    std::string out, word;
    auto flush = [&] {
      while (!word.empty() && fl_width(word.c_str(), int(word.size())) > width) {
        size_t fit = 0, slash = 0;
        for (size_t i = 1; i <= word.size(); ++i) {
          if (i < word.size() && (word[i] & 0xC0) == 0x80) continue;  // inside a UTF-8 character
          if (fl_width(word.c_str(), int(i)) > width) break;
          fit = i;
          if (word[i - 1] == '/') slash = i;
        }
        size_t cut = slash ? slash : std::max<size_t>(fit, 1);
        out += word.substr(0, cut) + "\n";
        word.erase(0, cut);
      }
      out += word;
      word.clear();
    };
    for (char ch : text) {
      if (ch == ' ' || ch == '\n') {
        flush();
        out += ch;
      } else {
        word += ch;
      }
    }
    flush();
    return out;
  }
  static int measure(const Texts &t, int w) {
    int pad = S(24);
    text_font(active_theme().font_size + 1);
    int ww = text_area(w), hh = 0;
    fl_measure(t.body.c_str(), ww, hh, 0);
    return pad + S(34) + S(10) + hh + S(20) + S(36) + pad;
  }
  void draw() override {
    const Theme &t = active_theme();
    int pad = S(24), isz = S(64);
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      p.image(IconLoader::get({"sde-appimage", "application-x-executable"}, isz), pad, pad, isz);
      double cs = S(26), cx = pad + isz - cs / 2 + S(4), cy = pad + isz - cs / 2 + S(4);
      p.circle(cx, cy, cs / 2, t.warning);
      p.line(cx, cy - Sd(6), cx, cy + Sd(1.5), Color::hex(0xFFFFFF), Sd(2.4));
      p.circle(cx, cy + Sd(5.5), Sd(1.6), Color::hex(0xFFFFFF));
    }
    int tx = pad + isz + S(18), tw = w() - tx - pad;
    text_font(t.font_size + 5, true);
    draw_text(texts_.title, tx, pad, tw, S(34), t.text);
    text_font(t.font_size + 1);
    fl_color(t.text.fl());
    fl_draw(texts_.body.c_str(), tx, pad + S(44), tw, h() - pad - S(44) - S(36) - pad,
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
    draw_children();
    qa_dump();
  }
  int handle(int e) override {
    if ((e == FL_SHORTCUT || e == FL_KEYBOARD) && Fl::event_key() == FL_Escape) {
      choice = Choice::keep;
      hide();
      return 1;
    }
    int r = BufferedWindow::handle(e);
    if (!r && handle_about_event(e, about_info())) return 1;
    return r;
  }
  void qa_dump() {
    const char *dir = getenv("SDE_PANEL_QA_DIR");
    if (!dir || !*dir) return;
    std::string text = "@title:" + texts_.title + "\t0\t0\t0\t0\t1\t0\n";
    for (const auto &m : texts_.meta) text += "@" + m + "\t0\t0\t0\t0\t1\t0\n";
    for (Fl_Widget *b : {static_cast<Fl_Widget *>(remove_), static_cast<Fl_Widget *>(keep_), static_cast<Fl_Widget *>(run_)})
      if (b)
        text += std::string(b->label()) + "\t" + std::to_string(b->x()) + "\t" + std::to_string(b->y()) + "\t" +
                std::to_string(b->w()) + "\t" + std::to_string(b->h()) + "\t1\t0\n";
    if (Fl::focus() && Fl::focus()->label()) text += std::string("@focus:") + Fl::focus()->label() + "\t0\t0\t0\t0\t1\t0\n";
    std::string path = std::string(dir) + "/AppImage.txt", tmp = path + ".tmp";
    if (FILE *f = fopen(tmp.c_str(), "w")) {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
      rename(tmp.c_str(), path.c_str());
    }
  }
  Choice choice = Choice::none;
  FlatButton *remove_ = nullptr, *keep_ = nullptr, *run_ = nullptr;

 private:
  Texts texts_;
};

void open_ui() {
  static bool open = false;
  if (open) return;
  open = true;
  Config cfg;
  cfg.load();
  fl_open_display();
  Theme theme = Theme::from_config(cfg);
  theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  Fl::visible_focus(1);
}

Choice ask(Texts texts, bool can_run) {
  open_ui();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int W = std::min(S(600), sw - S(24));
  texts.body = Dialog::fit_words(texts.body, Dialog::text_area(W));
  int H = std::min(Dialog::measure(texts, W), sh - S(48));
  Dialog d(W, H, texts, can_run);
  d.xclass("sde-appimage");
  d.position(sx + (sw - W) / 2, sy + (sh - H) / 3);
  d.set_modal();
  d.show();
  d.remove_->take_focus();  // Delete is the default: Enter deletes
  while (d.shown()) Fl::wait();
  return d.choice == Choice::none ? Choice::keep : d.choice;
}

void message(const std::string &title, const std::string &body) {
  fprintf(stderr, "sde-appimage: %s: %s\n", title.c_str(), body.c_str());
  const char *display = getenv("DISPLAY");
  if (!display || !*display) return;
  open_ui();
  fl_alert("%s\n\n%s", title.c_str(), body.c_str());
}

// Moves |path| to the trash (GIO); false when the drive has no trash.
bool trash(const std::string &path, std::string *error) {
  GFile *f = g_file_new_for_path(path.c_str());
  GError *err = nullptr;
  bool ok = g_file_trash(f, nullptr, &err);
  if (!ok && err) *error = err->message;
  g_clear_error(&err);
  g_object_unref(f);
  return ok;
}

// Keeping an AppImage that cannot be verified: it does not run by accident.
void take_exec_bits(int fd) {
  struct stat st;
  if (fstat(fd, &st) == 0 && st.st_uid == getuid() && (st.st_mode & 0111)) fchmod(fd, st.st_mode & ~mode_t(0111));
}

int run(std::string path, const std::vector<std::string> &args) {
  if (char *real = realpath(path.c_str(), nullptr)) {
    path = real;
    free(real);
  }
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  TrustStore store;
  store.load();
  Policy policy = load_policy();
  Check c;
  if (fd < 0) {
    c.verdict = Verdict::unreadable;
    c.error = strerror(errno);
  } else {
    fcntl(fd, F_SETFL, 0);
    std::string tmp = runtime_dir() + "/sde-appimage-XXXXXX";
    CheckOptions o;
    if (mkdtemp(&tmp[0])) o.tmpdir = tmp;
    c = check(fd, path, store, o);
    if (!o.tmpdir.empty()) rmdir(o.tmpdir.c_str());
  }
  if (c.verdict == Verdict::trusted) {
    std::string error;
    launch(fd, path, c, store, args, &error);
    log_line("failed " + path + ": " + error);
    message(_("The AppImage cannot be started"), error);
    return 1;
  }
  log_line(format("refused %s %s sha256 %s", path.c_str(), verdict_id(c.verdict),
                  c.sha256.empty() ? "-" : c.sha256.c_str()));
  if (c.verdict == Verdict::unreadable && fd < 0) {
    message(_("This file cannot be read"), path + ": " + c.error);
    return 1;
  }
  bool can_run = policy == Policy::ask && (c.verdict == Verdict::unsigned_image || c.verdict == Verdict::type1 ||
                                           (c.verdict == Verdict::no_key && c.error != "no-verifier"));
  Texts texts = explain(c, policy, path);
  const char *display = getenv("DISPLAY");
  if (!display || !*display) {
    // From a terminal without a desktop: the explanation, and nothing runs.
    fprintf(stderr, "sde-appimage: %s\n\n%s\n", texts.title.c_str(), texts.body.c_str());
    return 1;
  }
  Choice choice = ask(texts, can_run);
  if (choice == Choice::run && can_run) {
    accept(c, path, store);
    c.how = "accepted";
    log_line("accepted " + path + " sha256 " + c.sha256);
    std::string error;
    launch(fd, path, c, store, args, &error);
    message(_("The AppImage cannot be started"), error);
    return 1;
  }
  if (choice == Choice::remove) {
    std::string error;
    if (trash(path, &error)) {
      log_line("deleted (trash) " + path);
    } else if (fl_choice(_("This drive has no trash. Delete \"%s\" for good?"), _("Keep"), _("Delete"), nullptr,
                         basename_of(path).c_str()) == 1) {
      if (unlink(path.c_str()) == 0) log_line("deleted " + path);
      else message(_("The AppImage could not be deleted"), strerror(errno));
    }
    return 1;
  }
  if (fd >= 0) take_exec_bits(fd);
  log_line("kept " + path);
  return 1;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  sde::set_default_window_icon({"sde-appimage", "application-x-executable"});
  install_crash_handler("sde-appimage");
  if (argc < 2) {
    fprintf(stderr, "%s", usage().c_str());
    return 2;
  }
  std::string a = argv[1];
  if (a == "--version") return print_version(about_info());
  if (a == "--help" || a == "-h") {
    printf("%s", usage().c_str());
    return 0;
  }
  if (a == "--check" || a == "--forget") {
    if (argc != 3) {
      fprintf(stderr, "%s", usage().c_str());
      return 2;
    }
    std::string path = argv[2];
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
      fprintf(stderr, "sde-appimage: %s: %s\n", path.c_str(), strerror(errno));
      return 1;
    }
    TrustStore store;
    store.load();
    std::string tmp = runtime_dir() + "/sde-appimage-XXXXXX";
    CheckOptions o;
    o.use_seen = a == "--check";
    if (mkdtemp(&tmp[0])) o.tmpdir = tmp;
    Check c = check(fd, path, store, o);
    if (!o.tmpdir.empty()) rmdir(o.tmpdir.c_str());
    if (a == "--forget") {
      if (!c.sha256.empty()) {
        store.forget(c.sha256);
        // Its private copies go too.
        if (DIR *d = opendir(copies_dir().c_str())) {
          while (struct dirent *e = readdir(d))
            if (starts_with(e->d_name, c.sha256.substr(0, 12) + "-")) unlink((copies_dir() + "/" + e->d_name).c_str());
          closedir(d);
        }
      }
      // The key of its application too: the next version is "first use".
      store.forget_key(c.app);
      store.save();
      printf("forgot %s\n", path.c_str());
      return 0;
    }
    printf("%s\n", verdict_id(c.verdict));
    if (!c.sha256.empty()) printf("sha256 %s\n", c.sha256.c_str());
    if (!c.app.empty()) printf("app %s\n", c.app.c_str());
    if (!c.fingerprint.empty()) printf("key %s\n", c.fingerprint.c_str());
    if (!c.signer.empty()) printf("signer %s\n", c.signer.c_str());
    if (!c.previous.empty()) printf("previous %s\n", c.previous.c_str());
    if (!c.how.empty()) printf("how %s%s\n", c.how.c_str(), c.cached ? " (cached)" : "");
    if (!c.error.empty()) printf("error %s\n", c.error.c_str());
    if (c.verdict == Verdict::trusted) store.save();
    return c.verdict == Verdict::trusted ? 0 : 1;
  }
  int first = a == "--" ? 2 : 1;
  if (first >= argc) {
    fprintf(stderr, "%s", usage().c_str());
    return 2;
  }
  std::string path = argv[first];
  if (starts_with(path, "file://")) path = path.substr(7);
  std::vector<std::string> args(argv + first + 1, argv + argc);
  return run(path, args);
}
