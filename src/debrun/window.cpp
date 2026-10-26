// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Debrun's window (window.h). The package is read and checked on a worker
// thread (deb.h, system.h: dpkg's status, APT's simulation, apt-cache
// policy); the window shows the result in cards of the SDE UI kit
// (settings/ui.h) and makes the change with the backend (backend.h).
//
// With SDE_PANEL_QA_DIR set, <dir>/Debrun.txt lists the state ("@state:",
// "@action:", "@change:", "@dep:", "@result:" ...) and the named controls
// with their geometry ("name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>active<TAB>
// scrolled"), as the Control Center does, for the UI tests.
#include "window.h"

#include <FL/Fl.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_draw.H>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

#include "async.h"
#include "avatar.h"
#include "backend.h"
#include "buffered_window.h"
#include "build_config.h"
#include "deb.h"
#include "desktop_entry.h"
#include "helper.h"
#include "i18n.h"
#include "icons.h"
#include "../merkadu/logo_anim.h"
#include "paint.h"
#include "spawn.h"
#include "system.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {
namespace debrun {

namespace {

using merkadu::LogoAnimation;

// ------------------------------------------------------------- the state

enum class Phase { empty, loading, error, ready, confirm, working, done, failed };
enum class Job { none, install, remove, refresh };

struct Analysis {
  bool ok = false;
  std::string error;
  Package pkg;
  System sys;
  Fit fit;
  AptPlan plan;         // installing it
  AptPlan remove_plan;  // removing the installed package of that name
  std::vector<DepCheck> deps;
  std::vector<Change> changes;
  Policy policy;           // what the package sources offer of it
  std::string origin_url;  // where the browser downloaded it from
  long long free_kib = -1; // free space for /usr
};

struct State {
  std::string path;
  Phase phase = Phase::empty;
  Analysis a;
  bool analyzing = false;
  Job job = Job::none;
  Job confirming = Job::none;
  Job last_job = Job::none;  // the change that finished last
  Step step;
  Outcome outcome;
  std::string done_text;
  std::vector<std::string> log;
  bool show_log = false, all_files = false;
  std::unique_ptr<Backend> backend;
};

State g;
class DebrunWindow;
DebrunWindow *g_win = nullptr;
std::map<int, cairo_surface_t *> g_icons;  // the package's icon by size

void rebuild();
void relayout();
void analyze_later();
void choose_file();

// --------------------------------------------------------- the analysis

// "user.xdg.origin.url": where a browser (Chromium, Firefox, Liria's
// downloads) saved the file from.
std::string origin_url(const std::string &path) {
  char buf[1024];
  ssize_t n = getxattr(path.c_str(), "user.xdg.origin.url", buf, sizeof buf - 1);
  if (n <= 0) return "";
  std::string url(buf, size_t(n));
  if (!starts_with(url, "https://") && !starts_with(url, "http://")) return "";
  for (unsigned char c : url)
    if (c < 0x21 || c == 0x7F) return "";
  return url;
}

// APT takes a file only by a path ending in ".deb": a private link to the
// package with that name (also keeps any odd characters of the real name
// away from APT's argument parser).
std::string link_for_apt(const std::string &path, std::string *dir) {
  std::string base = runtime_dir();
  if (base.empty()) return "";
  std::string tmpl = base + "/debrun-XXXXXX";
  if (!mkdtemp(&tmpl[0])) return "";
  *dir = tmpl;
  std::string link = tmpl + "/package.deb";
  if (symlink(path.c_str(), link.c_str()) != 0) return "";
  return link;
}

Analysis analyze(const std::string &path) {
  Analysis a;
  if (!read_package(path, &a.pkg, &a.error)) return a;
  a.ok = true;
  std::string error;
  load_system(&a.sys, &error);
  a.fit = fit_package(a.pkg, a.sys);
  a.changes = system_changes(a.pkg);
  a.origin_url = origin_url(path);
  if (a.fit.arch_ok) {
    std::string dir, link = link_for_apt(path, &dir);
    if (!link.empty()) {
      int st = -1;
      std::string out = run_capture(simulate_install_argv(link, a.fit.action == Action::reinstall,
                                                          a.fit.action == Action::downgrade),
                                    &st, 180);
      a.plan = parse_apt_output(out, st);
    }
    if (!dir.empty()) {
      unlink((dir + "/package.deb").c_str());
      rmdir(dir.c_str());
    }
  }
  a.deps = check_relations(a.pkg, a.sys, a.plan);
  if (a.fit.current) {
    int st = -1;
    a.remove_plan = parse_apt_output(run_capture(simulate_remove_argv(a.pkg.name), &st, 120), st);
  }
  int st = -1;
  std::string policy = run_capture({apt_cache_program(), "policy", a.pkg.name}, &st, 60);
  if (st == 0) a.policy = parse_policy(policy);
  if (a.policy.name != a.pkg.name) a.policy = Policy();
  struct statvfs vfs;
  if (statvfs("/usr", &vfs) == 0) a.free_kib = (long long)(vfs.f_bavail) * (long long)vfs.f_frsize / 1024;
  return a;
}

void free_icons() {
  for (auto &i : g_icons)
    if (i.second) cairo_surface_destroy(i.second);
  g_icons.clear();
}

void open_package(const std::string &path) {
  g.path = path;
  g.phase = path.empty() ? Phase::empty : Phase::loading;
  g.a = Analysis();
  g.outcome = Outcome();
  g.done_text.clear();
  g.log.clear();
  g.show_log = g.all_files = false;
  g.confirming = Job::none;
  free_icons();
  if (!path.empty()) analyze_later();
  rebuild();
}

// Reads the package again (also after a change: what is installed now).
void analyze_later() {
  if (g.analyzing || g.path.empty()) return;
  g.analyzing = true;
  std::string path = g.path;
  background<Analysis>([path] { return analyze(path); },
                       [path](Analysis &a) {
                         g.analyzing = false;
                         if (path != g.path) {
                           analyze_later();
                           return;
                         }
                         bool after_change = g.phase == Phase::done || g.phase == Phase::failed;
                         g.a = std::move(a);
                         free_icons();
                         if (!g.a.ok) g.phase = Phase::error;
                         else if (!after_change) g.phase = Phase::ready;
                         rebuild();
                       });
}

// -------------------------------------------------------------- texts

std::string action_label(Action a) {
  switch (a) {
    case Action::upgrade: return _("Upgrade");
    case Action::reinstall: return _("Reinstall");
    case Action::downgrade: return _("Downgrade");
    default: return _("Install");
  }
}

const char *action_id(Action a) {
  switch (a) {
    case Action::upgrade: return "upgrade";
    case Action::reinstall: return "reinstall";
    case Action::downgrade: return "downgrade";
    default: return "install";
  }
}

std::string title_of(const Package &p) { return p.app_name.empty() ? p.name : p.app_name; }

std::string join_names(const std::vector<std::string> &names, size_t max = 6) {
  std::string out;
  for (size_t i = 0; i < names.size() && i < max; ++i) out += (i ? ", " : "") + names[i];
  if (names.size() > max) out += format(_(" and %d more"), int(names.size() - max));
  return out;
}

// The packages APT installs besides this one, and those it removes.
std::vector<std::string> extra_packages(const Analysis &a) {
  std::vector<std::string> out;
  for (const auto &i : a.plan.install)
    if (i.name != a.pkg.name && i.name.substr(0, i.name.find(':')) != a.pkg.name) out.push_back(i.name);
  return out;
}

std::vector<std::string> removed_packages(const AptPlan &plan, const std::string &except = "") {
  std::vector<std::string> out;
  for (const auto &i : plan.remove) {
    std::string n = i.name.substr(0, i.name.find(':'));
    if (n != except && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
  }
  return out;
}

bool can_install(const Analysis &a) { return a.ok && a.fit.arch_ok && (a.plan.ok || !a.plan.ran); }

// Why APT said no, in a sentence.
std::string plan_problem(const AptPlan &plan) {
  switch (plan.problem) {
    case AptPlan::Problem::unmet:
      return _("Packages it needs cannot be installed from the package sources (see Dependencies).");
    case AptPlan::Problem::held: return _("A package it involves is held at its version (apt-mark hold).");
    case AptPlan::Problem::essential: return _("It would remove a package the system needs.");
    case AptPlan::Problem::lock: return _("Another program is installing or removing software right now.");
    case AptPlan::Problem::damaged: return _("APT cannot read the package: it is damaged.");
    default:
      return plan.errors.empty() ? std::string(_("APT cannot install it."))
                                 : format(_("APT cannot install it: %s"), plan.errors.front().c_str());
  }
}

std::string step_text(const Step &s, Job job) {
  const char *pkg = s.package.c_str();
  if (s.phase == "auth") return _("Waiting for permission\xE2\x80\xA6");
  if (s.phase == "copy") return _("Preparing the package\xE2\x80\xA6");
  if (s.phase == "check") return _("Checking the package\xE2\x80\xA6");
  if (s.phase == "wait") return _("Waiting for another program to finish installing software\xE2\x80\xA6");
  if (s.phase == "download") return _("Downloading the packages it needs\xE2\x80\xA6");
  if (s.phase == "refresh") return _("Refreshing the package lists\xE2\x80\xA6");
  if (s.phase == "unpack")
    return s.package.empty() ? std::string(_("Installing\xE2\x80\xA6")) : format(_("Unpacking %s\xE2\x80\xA6"), pkg);
  if (s.phase == "configure")
    return s.package.empty() ? std::string(_("Setting up\xE2\x80\xA6")) : format(_("Setting up %s\xE2\x80\xA6"), pkg);
  if (s.phase == "remove")
    return s.package.empty() ? std::string(_("Removing\xE2\x80\xA6")) : format(_("Removing %s\xE2\x80\xA6"), pkg);
  if (s.phase == "trigger") return _("Finishing\xE2\x80\xA6");
  return job == Job::remove ? _("Removing\xE2\x80\xA6") : job == Job::refresh ? _("Refreshing the package lists\xE2\x80\xA6")
                                                                                : _("Installing\xE2\x80\xA6");
}

enum class Tone { normal, good, warn, bad };

// The sentence of the bottom bar.
std::string bar_text(Tone *tone) {
  *tone = Tone::normal;
  const Analysis &a = g.a;
  switch (g.phase) {
    case Phase::empty: return _("Choose a Debian package (.deb) to see what it contains before you install it.");
    case Phase::loading: return _("Reading the package\xE2\x80\xA6");
    case Phase::error:
      *tone = Tone::bad;
      return _("This file cannot be installed.");
    case Phase::working: return step_text(g.step, g.job);
    case Phase::done:
      *tone = Tone::good;
      return g.done_text;
    case Phase::failed:
      *tone = g.outcome.cancelled ? Tone::warn : Tone::bad;
      return g.outcome.message;
    case Phase::confirm: {
      *tone = Tone::warn;
      if (g.confirming == Job::remove) {
        std::vector<std::string> also = removed_packages(a.remove_plan, a.pkg.name);
        return also.empty() ? format(_("Remove %s from this computer?"), title_of(a.pkg).c_str())
                            : format(_("Remove %s? These packages are removed with it: %s."),
                                     title_of(a.pkg).c_str(), join_names(also).c_str());
      }
      return format(_("Installing %s also removes these packages: %s."), title_of(a.pkg).c_str(),
                    join_names(removed_packages(a.plan)).c_str());
    }
    case Phase::ready: break;
  }
  if (!a.fit.arch_ok) {
    *tone = Tone::bad;
    return format(_("Not for this computer: it is made for %s."), arch_label(a.pkg.architecture).c_str());
  }
  if (a.plan.ran && !a.plan.ok) {
    *tone = Tone::bad;
    return std::string(_("It cannot be installed now.")) + " " + plan_problem(a.plan);
  }
  std::string text;
  const Installed *cur = a.fit.current;
  switch (a.fit.action) {
    case Action::install: text = _("Ready to install."); break;
    case Action::upgrade:
      text = format(_("Ready to upgrade from version %s to %s."), cur->version.c_str(), a.pkg.version.c_str());
      break;
    case Action::reinstall: text = _("This version is installed already. It can be installed again."); break;
    case Action::downgrade:
      *tone = Tone::warn;
      text = format(_("A newer version (%s) is installed: installing this one is a downgrade."), cur->version.c_str());
      break;
  }
  std::vector<std::string> extra = extra_packages(a);
  if (!extra.empty())
    text += " " + format(ngettext("%d more package is installed with it.", "%d more packages are installed with it.",
                                  extra.size()),
                         int(extra.size()));
  std::vector<std::string> gone = removed_packages(a.plan);
  if (!gone.empty()) {
    *tone = Tone::warn;
    text += " " + format(ngettext("It removes %d package.", "It removes %d packages.", gone.size()), int(gone.size()));
  }
  if (!a.plan.ran) text += " " + std::string(_("(APT could not check its dependencies.)"));
  return text;
}

Color tone_color(Tone tone, const Theme &t) {
  switch (tone) {
    case Tone::good: return t.success.mix(t.text, 0.25);
    case Tone::warn: return t.warning.mix(t.text, 0.45);
    case Tone::bad: return t.error;
    default: return t.text;
  }
}

// --------------------------------------------------------------- widgets

int button_w(const std::string &label) {
  text_font(active_theme().font_size + 1);
  return std::max(S(104), text_width(label) + S(40));
}

// A tinted box with a title and a paragraph (the warning that the package
// is not from the distribution, problems).
class Banner : public Fl_Widget {
 public:
  Banner(int x, int y, int w, const std::string &title, const std::string &text, Tone tone)
      : Fl_Widget(x, y, w, 10), title_(title), text_(text), tone_(tone) {
    h(measure(w, title, text));
  }
  static int measure(int w, const std::string &title, const std::string &text) {
    int th = Label::measure(text, w - S(76), Label::NORMAL);
    return S(16) + S(26) + th + S(16);
  }
  void draw() override {
    const Theme &t = active_theme();
    Color c = tone_ == Tone::bad ? t.error : tone_ == Tone::good ? t.success : t.warning;
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), Sd(12), c.alpha(t.dark ? 0.16 : 0.12));
      p.stroke_rounded(x(), y(), w(), h(), Sd(12), c.alpha(0.55));
      double cx = x() + S(30), cy = y() + S(30), r = S(12);
      p.circle(cx, cy, r, c);
      if (tone_ == Tone::good) {
        p.line(cx - Sd(5), cy, cx - Sd(1.5), cy + Sd(4), Color::hex(0xFFFFFF), Sd(2.2));
        p.line(cx - Sd(1.5), cy + Sd(4), cx + Sd(5.5), cy - Sd(4), Color::hex(0xFFFFFF), Sd(2.2));
      } else {
        p.line(cx, cy - Sd(6), cx, cy + Sd(1.5), Color::hex(0xFFFFFF), Sd(2.4));
        p.circle(cx, cy + Sd(5.5), Sd(1.5), Color::hex(0xFFFFFF));
      }
    }
    text_font(t.font_size + 1, true);
    draw_text(title_, x() + S(56), y() + S(14), w() - S(72), S(26), t.text);
    text_font(t.font_size + 1);
    fl_color(t.text.fl());
    fl_draw(text_.c_str(), x() + S(56), y() + S(42), w() - S(76), h() - S(48),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  }

 private:
  std::string title_, text_;
  Tone tone_;
};

// Rows with a colored dot, a name and a detail at the right: dependencies,
// packages installed or removed with it.
struct DotRow {
  Color color;
  std::string text, detail;
};

class DotList : public Fl_Widget {
 public:
  DotList(int x, int y, int w, std::vector<DotRow> rows)
      : Fl_Widget(x, y, w, std::max(1, int(rows.size())) * S(30)), rows_(std::move(rows)) {}
  void draw() override {
    const Theme &t = active_theme();
    int rh = S(30);
    {
      Painter p(window()->w(), window()->h());
      for (size_t i = 0; i < rows_.size(); ++i)
        p.circle(x() + S(8), y() + int(i) * rh + rh / 2.0, S(5), rows_[i].color);
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
      int ry = y() + int(i) * rh;
      text_font(t.font_size);
      int dw = std::min(w() / 2, text_width(rows_[i].detail) + S(8));
      text_font(t.font_size + 1);
      draw_text(rows_[i].text, x() + S(24), ry, w() - S(24) - dw - S(12), rh, t.text);
      text_font(t.font_size);
      draw_text(rows_[i].detail, x() + w() - dw, ry, dw, rh, t.text_dim, 2);
    }
  }

 private:
  std::vector<DotRow> rows_;
};

// The files of the package: one line each, only the visible ones drawn.
class FileList : public Fl_Widget {
 public:
  FileList(int x, int y, int w, const std::vector<Entry> &files, size_t count)
      : Fl_Widget(x, y, w, std::max<size_t>(1, std::min(files.size(), count)) * S(24)),
        files_(&files),
        count_(std::min(files.size(), count)) {}
  void draw() override {
    const Theme &t = active_theme();
    int lh = S(24), cx, cy, cw, ch;
    fl_clip_box(x(), y(), w(), h(), cx, cy, cw, ch);
    size_t first = size_t(std::max(0, (cy - y()) / lh));
    size_t last = std::min(count_, size_t((cy + ch - y()) / lh + 1));
    for (size_t i = first; i < last; ++i) {
      const Entry &e = (*files_)[i];
      int ry = y() + int(i) * lh;
      std::string name = e.path + (e.type == 'd' ? "/" : e.type == 'l' ? "  \xE2\x86\x92 " + e.target : "");
      text_font(t.font_size - 1);
      std::string size = e.type == 'f' ? format_size(e.size) : "";
      int sw = size.empty() ? 0 : text_width(size) + S(8);
      Color c = (e.type == 'f' && (e.mode & 06000)) ? t.error : e.type == 'd' ? t.text_dim : t.text;
      draw_text(name, x(), ry, w() - sw - S(8), lh, c);
      if (sw) draw_text(size, x() + w() - sw, ry, sw, lh, t.text_dim, 2);
    }
  }

 private:
  const std::vector<Entry> *files_;
  size_t count_;
};

// Progress of the change, or a bar sliding back and forth while unknown.
class ProgressBar : public Fl_Widget {
 public:
  ProgressBar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  ~ProgressBar() override { Fl::remove_timeout(tick, this); }
  void value(int percent) {
    percent_ = percent;
    Fl::remove_timeout(tick, this);
    if (percent < 0 && !LogoAnimation::reduced_motion()) Fl::add_timeout(1.0 / 30, tick, this);
    redraw();
  }
  void draw() override {
    const Theme &t = active_theme();
    Painter p(window()->w(), window()->h());
    p.fill_rounded(x(), y(), w(), h(), h() / 2.0, t.border);
    if (percent_ >= 0) {
      double fw = std::max<double>(h(), w() * percent_ / 100.0);
      p.fill_rounded(x(), y(), fw, h(), h() / 2.0, t.accent);
    } else {
      double seg = w() * 0.28, pos = (std::sin(phase_) + 1) / 2 * (w() - seg);
      p.fill_rounded(x() + pos, y(), seg, h(), h() / 2.0, t.accent);
    }
  }

 private:
  static void tick(void *data) {
    auto *self = static_cast<ProgressBar *>(data);
    self->phase_ += 0.09;
    self->redraw();
    Fl::repeat_timeout(1.0 / 30, tick, data);
  }
  int percent_ = -1;
  double phase_ = 0;
};

// ---------------------------------------------------------------- window

int header_h(int H) { return H < S(560) ? S(84) : S(112); }
int bar_h() { return S(78); }

class DebrunWindow : public BufferedWindow {
 public:
  DebrunWindow(int w, int h) : BufferedWindow(w, h, "Debrun") {
    logo = new LogoAnimation(0, 0, S(52), S(52));
    logo->hide();
    progress = new ProgressBar(0, 0, 10, S(6));
    progress->hide();
    for (FlatButton *&b : buttons) {
      b = new FlatButton(0, 0, 10, S(36), "", false);
      b->hide();
    }
    end();
  }

  void draw() override;
  int handle(int e) override;
  void resize(int x, int y, int w, int h) override {
    bool changed = w != this->w() || h != this->h();
    BufferedWindow::resize(x, y, w, h);
    if (changed) Fl::add_timeout(0, [](void *) { rebuild(); });
  }
  void qa_dump();

  Page *page = nullptr;
  LogoAnimation *logo;
  ProgressBar *progress;
  FlatButton *buttons[3];  // right to left: the main action, then the others
  std::vector<std::function<void()>> actions = std::vector<std::function<void()>>(3);
};

// The icon of the package (its own, else a package icon of the theme).
cairo_surface_t *package_icon(int size) {
  if (!g.a.pkg.icon_data.empty()) {
    auto it = g_icons.find(size);
    if (it == g_icons.end()) {
      std::string error;
      it = g_icons.emplace(size, avatar::decode_square(g.a.pkg.icon_data, size, &error)).first;
    }
    if (it->second) return it->second;
  }
  return IconLoader::get({"application-vnd.debian.binary-package", "application-x-deb", "package-x-generic", "debrun"},
                         size);
}

void DebrunWindow::draw() {
  const Theme &t = active_theme();
  int W = w(), H = h(), hh = header_h(H), bh = bar_h();
  bool compact = hh < S(112);
  int isz = compact ? S(52) : S(68), pad = S(24);
  const Package &p = g.a.pkg;
  bool have = g.a.ok && g.phase != Phase::empty && g.phase != Phase::loading && g.phase != Phase::error;
  {
    Painter pt(W, H);
    pt.fill_rect(0, 0, W, H, t.background);
    pt.fill_rect(0, 0, W, hh, t.surface);
    pt.line(0, hh - 0.5, W, hh - 0.5, t.border);
    pt.fill_rect(0, H - bh, W, bh, t.surface);
    pt.line(0, H - bh + 0.5, W, H - bh + 0.5, t.border);
    if (have) pt.image(package_icon(isz), pad, (hh - isz) / 2.0, isz);
    else pt.image(IconLoader::get({"debrun", "package-x-generic"}, isz), pad, (hh - isz) / 2.0, isz);
  }
  int tx = pad + isz + S(18), tw = W - tx - pad;
  int line = compact ? S(24) : S(30);
  int top = (hh - (compact ? S(66) : S(82))) / 2;
  if (have) {
    text_font(t.font_size + (compact ? 5 : 8), true);
    draw_text(title_of(p), tx, top, tw, line, t.text);
    text_font(t.font_size);
    std::string sub = p.name + "  \xC2\xB7  " + p.version + "  \xC2\xB7  " + arch_label(p.architecture);
    draw_text(sub, tx, top + line + S(2), tw, S(22), t.text_dim);
    text_font(t.font_size + 1);
    draw_text(p.app_comment.empty() ? p.summary : p.app_comment, tx, top + line + S(24), tw, S(22), t.text);
  } else {
    text_font(t.font_size + (compact ? 5 : 8), true);
    draw_text(_("Debrun"), tx, top, tw, line, t.text);
    text_font(t.font_size + 1);
    draw_text(_("Install Debian packages downloaded from the website of their makers"), tx, top + line + S(10), tw,
              S(24), t.text_dim);
  }
  // The bar: the logo while working, the sentence, the progress.
  Tone tone;
  std::string text = bar_text(&tone);
  int left = pad + (g.phase == Phase::working ? S(52) + S(14) : 0);
  int right = W - pad;
  for (FlatButton *b : buttons)
    if (b->visible()) right = std::min(right, b->x() - S(16));
  text_font(t.font_size + (g.phase == Phase::working ? 1 : 0), g.phase == Phase::working);
  fl_color(tone_color(tone, t).fl());
  int ty = H - bh + (g.phase == Phase::working ? S(12) : S(10));
  int th = g.phase == Phase::working ? S(26) : bh - S(20);
  fl_draw(text.c_str(), left, ty, std::max(S(40), right - left), th,
          Fl_Align((g.phase == Phase::working ? FL_ALIGN_LEFT : FL_ALIGN_LEFT | FL_ALIGN_WRAP) | FL_ALIGN_INSIDE |
                   FL_ALIGN_CLIP),
          nullptr, 0);
  if (g.phase == Phase::working && g.step.percent >= 0) {
    text_font(t.font_size - 1);
    draw_text(format("%d %%", g.step.percent), progress->x() + progress->w() + S(8), progress->y() - S(8), S(60),
              S(22), t.text_dim);
  }
  draw_children();
  qa_dump();
}

void close_window() {
  if (g.phase == Phase::working) return;  // dpkg must finish; the bar says what happens
  if (g_win) g_win->hide();
}

int DebrunWindow::handle(int e) {
  if (e == FL_SHORTCUT || e == FL_KEYBOARD) {
    int k = Fl::event_key();
    if (k == FL_Escape) {
      if (g.phase == Phase::confirm) {
        g.confirming = Job::none;
        g.phase = Phase::ready;
        rebuild();
      } else {
        close_window();
      }
      return 1;
    }
    if ((Fl::event_state() & FL_CTRL) && (k == 'o' || k == 'q' || k == 'w')) {
      if (k == 'o') {
        Fl::add_timeout(0, [](void *) {
          if (g.phase == Phase::working) return;
          choose_file();
        });
      } else {
        close_window();
      }
      return 1;
    }
  }
  int r = BufferedWindow::handle(e);
  if (!r && handle_about_event(e, about_info())) return 1;
  return r;
}

void DebrunWindow::qa_dump() {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  if (!dir || !*dir) return;
  static const char *phases[] = {"empty", "loading", "error", "ready", "confirm", "working", "done", "failed"};
  auto meta = [](const std::string &s) { return s + "\t0\t0\t0\t0\t1\t0\n"; };
  std::string text = meta(std::string("@state:") + phases[int(g.phase)]);
  const Analysis &a = g.a;
  if (a.ok) {
    text += meta("@package:" + a.pkg.name + " " + a.pkg.version + " " + a.pkg.architecture);
    text += meta(std::string("@action:") + action_id(a.fit.action));
    text += meta(std::string("@plan:") + (a.plan.ok ? "ok" : a.plan.ran ? "refused" : "none"));
    static const char *states[] = {"installed", "provided", "will_install", "missing", "conflict", "removed", "ok"};
    for (const auto &c : a.changes) text += meta(std::string("@change:") + change_id(c.kind));
    for (const auto &d : a.deps)
      text += meta("@dep:" + d.field + ":" + (d.alts.empty() ? "" : d.alts[0].name) + ":" + states[int(d.state)]);
    for (const auto &n : extra_packages(a)) text += meta("@extra:" + n);
    for (const auto &n : removed_packages(a.plan)) text += meta("@remove:" + n);
    if (!a.policy.repo_version().empty()) text += meta("@repo:" + a.policy.repo_version());
    if (!a.pkg.icon_path.empty()) text += meta("@icon:" + a.pkg.icon_path);
  } else if (!a.error.empty()) {
    text += meta("@error:" + a.error);
  }
  if (g.phase == Phase::working) text += meta("@step:" + g.step.phase + " " + std::to_string(g.step.percent));
  if (g.phase == Phase::done) text += meta("@result:ok");
  if (g.phase == Phase::failed)
    text += meta(std::string("@result:") + (g.outcome.cancelled ? "cancelled" : "failed") + " " + g.outcome.code);
  if (g.backend) text += meta("@backend:" + g.backend->name());
  for (FlatButton *b : buttons)
    if (b->visible() && b->label()) text += meta(std::string("@button:") + b->label() + (b->active() ? "" : " (off)"));
  std::function<void(Fl_Widget *, const Fl_Widget *)> collect = [&](Fl_Widget *w, const Fl_Widget *view) {
    if (!w->visible()) return;
    const char *name = w->tooltip() ? w->tooltip() : w->label();
    if (name && *name && w != this) {
      std::string n = name;
      for (char &c : n)
        if (c == '\t' || c == '\n') c = ' ';
      bool shown = !view || (w->y() >= view->y() && w->y() + w->h() <= view->y() + view->h());
      text += n + "\t" + std::to_string(w->x()) + "\t" + std::to_string(w->y()) + "\t" + std::to_string(w->w()) +
              "\t" + std::to_string(w->h()) + "\t" + std::to_string(w->active_r() && shown ? 1 : 0) + "\t" +
              (view ? "1" : "0") + "\n";
    }
    if (auto *grp = w->as_group()) {
      const Fl_Widget *inner = dynamic_cast<Fl_Scroll *>(w) ? w : view;
      for (int i = 0; i < grp->children(); ++i) collect(grp->child(i), inner);
    }
  };
  for (int i = 0; i < children(); ++i) collect(child(i), nullptr);
  std::string path = std::string(dir) + "/Debrun.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

// ------------------------------------------------------------- actions

void log_line(const std::string &line) {
  if (g.log.size() < 4000) g.log.push_back(line);
}

void after_change(Job job, const Outcome &o) {
  g.job = Job::none;
  g.last_job = job;
  g.outcome = o;
  if (o.ok) {
    g.phase = Phase::done;
    const std::string name = title_of(g.a.pkg);
    g.done_text = job == Job::remove    ? format(_("%s was removed."), name.c_str())
                  : job == Job::refresh ? std::string(_("The package lists were refreshed."))
                                        : format(_("%s %s was installed."), name.c_str(), g.a.pkg.version.c_str());
    if (job == Job::refresh) g.phase = Phase::ready;
  } else {
    g.phase = Phase::failed;
  }
  // What is installed now.
  analyze_later();
  rebuild();
}

Callbacks callbacks(Job job) {
  Callbacks cb;
  cb.step = [](const Step &s) {
    // A phase without a number keeps the last one.
    int keep = g.step.percent;
    g.step = s;
    if (s.percent < 0 && (s.phase == "unpack" || s.phase == "configure" || s.phase == "remove")) g.step.percent = keep;
    if (g_win) {
      g_win->progress->value(g.step.percent);
      g_win->redraw();
    }
  };
  cb.log = log_line;
  cb.done = [job](const Outcome &o) { after_change(job, o); };
  return cb;
}

void start_job(Job job) {
  if (!g.backend) g.backend = make_backend();
  if (g.backend->running()) return;
  g.job = job;
  g.confirming = Job::none;
  g.phase = Phase::working;
  g.step = Step();
  g.log.clear();
  g.show_log = false;
  rebuild();
  const Analysis &a = g.a;
  if (job == Job::install) {
    g.backend->install(a.pkg, a.fit.action == Action::reinstall, a.fit.action == Action::downgrade,
                       removed_packages(a.plan), callbacks(job));
  } else if (job == Job::remove) {
    g.backend->remove(a.pkg.name, removed_packages(a.remove_plan, a.pkg.name), callbacks(job));
  } else {
    g.backend->refresh(callbacks(job));
  }
}

void install_clicked() {
  if (!can_install(g.a)) return;
  if (!removed_packages(g.a.plan).empty() && g.phase != Phase::confirm) {
    g.confirming = Job::install;
    g.phase = Phase::confirm;
    rebuild();
    return;
  }
  start_job(Job::install);
}

void remove_clicked() {
  if (g.phase != Phase::confirm) {
    g.confirming = Job::remove;
    g.phase = Phase::confirm;
    rebuild();
    return;
  }
  start_job(Job::remove);
}

void open_app() {
  DesktopEntry e;
  if (!g.a.pkg.app_launcher.empty() && e.load(g.a.pkg.app_launcher) && launch_entry(e)) {
    close_window();
    return;
  }
  g.done_text += " " + std::string(_("It has no launcher to open: find it in the Start menu."));
  rebuild();
}

// The real path of a chosen file (the helper opens no symbolic link).
std::string real_path(const char *f) {
  char *real = realpath(f, nullptr);
  std::string out = real ? real : f;
  free(real);
  return out;
}

void choose_file() {
  std::string dir = user_dir("DOWNLOAD");
  if (dir.empty() || !dir_exists(dir)) dir = home_dir();
  const char *f = fl_file_chooser(_("Choose a Debian package"), "Debian packages (*.deb)", dir.c_str());
  if (f && *f) open_package(real_path(f));
}

// ---------------------------------------------------------------- page

Fl_Widget *link_button(int x, int y, int w, int h, const std::string &label, std::function<void()> run) {
  auto *b = new FlatButton(x, y, w, h, label, false);
  b->copy_label(label.c_str());
  b->callback([](Fl_Widget *, void *d) {
    auto *fn = static_cast<std::function<void()> *>(d);
    Fl::add_timeout(0, [](void *dd) { (*static_cast<std::function<void()> *>(dd))(); }, fn);
  }, keep(new std::function<void()>(std::move(run))));
  return b;
}

void text_row(Page *page, const std::string &label, const std::string &value, bool dim = false) {
  if (value.empty()) return;
  int x, w;
  page->control_box(std::max(S(220), page->width() * 6 / 10 - S(20)), &x, &w);
  int h = std::max(S(24), Label::measure(value, w, dim ? Label::DIM : Label::NORMAL) + S(2));
  page->row(label, w, h, [&](int cx, int cy, int cw, int ch) {
    return new Label(cx, cy, cw, ch, value, dim ? Label::DIM : Label::NORMAL, true);
  });
}

void banner(Page *page, const std::string &title, const std::string &text, Tone tone) {
  new Banner(page->left(), page->cursor(), page->width(), title, text, tone);
  page->advance(Banner::measure(page->width(), title, text) + S(14));
}

// "Not from Debian's package sources": who made it, where it came from,
// and the version the package sources offer.
void origin_card(Page *page) {
  const Analysis &a = g.a;
  std::string distro = os_release("NAME");
  if (distro.empty()) distro = "Debian";
  std::string text = format(_("This package does not come from the package sources of %s, so they did not check "
                              "it. Install it only if you trust its maker, for example when you downloaded it from "
                              "the official website of the software."),
                            distro.c_str());
  if (!a.pkg.maintainer.empty()) text += "\n" + format(_("Made by: %s"), a.pkg.maintainer.c_str());
  if (!a.origin_url.empty()) text += "\n" + format(_("Downloaded from: %s"), a.origin_url.c_str());
  std::string repo = a.policy.repo_version();
  if (!repo.empty())
    text += "\n" + format(_("The package sources also offer %s %s: installed from there, it gets security updates "
                            "with the rest of the system."),
                          a.pkg.name.c_str(), repo.c_str());
  banner(page, format(_("Not from the package sources of %s"), distro.c_str()), text, Tone::warn);
}

void package_card(Page *page) {
  const Analysis &a = g.a;
  const Package &p = a.pkg;
  page->anchor("package");
  page->begin_card(_("Package"));
  text_row(page, _("Name"), p.name);
  text_row(page, _("Version"), p.version);
  text_row(page, _("Architecture"), arch_label(p.architecture));
  text_row(page, _("Maintainer"), p.maintainer);
  if (!p.homepage.empty()) {
    std::string url = p.homepage;
    page->row(_("Homepage"), std::min(page->width() - S(40), std::max(S(200), button_w(url))), S(34),
              [&](int x, int y, int w, int h) {
                Fl_Widget *b = link_button(x, y, w, h, url, [url] { open_uri(url); });
                b->tooltip(keep(new std::string(_("Homepage")))->c_str());
                return b;
              });
  }
  text_row(page, _("Section"), p.section);
  if (p.installed_size >= 0) {
    std::string size = format_size(p.installed_size * 1024);
    if (a.free_kib >= 0 && p.installed_size > a.free_kib)
      size += "  " + format(_("(only %s free)"), format_size(a.free_kib * 1024).c_str());
    text_row(page, _("Installed size"), size);
  }
  text_row(page, _("Download size"), format_size(p.file_size));
  std::string installed = a.fit.current ? a.fit.current->version + (a.fit.current->held() ? "  " + std::string(_("(held)")) : "")
                                        : std::string(_("Not installed"));
  if (a.fit.current && !helper::protected_package(p.name) && !a.fit.current->essential) {
    std::string label = _("Remove...");
    page->row(_("Installed version"), button_w(label), S(34),
              [&](int x, int y, int w, int h) {
                Fl_Widget *b = link_button(x, y, w, h, label, remove_clicked);
                b->tooltip(keep(new std::string(_("Remove the installed package")))->c_str());
                if (g.phase == Phase::working || g.phase == Phase::confirm) b->deactivate();
                return b;
              },
              installed);
  } else {
    text_row(page, _("Installed version"), installed);
  }
  // The digest in groups of eight, to compare with the one the maker publishes.
  std::string digest;
  for (size_t i = 0; i < p.sha256.size(); i += 8) digest += (i ? " " : "") + p.sha256.substr(i, 8);
  text_row(page, _("SHA-256"), digest, true);
  page->end_card();
}

std::string change_title(const Change &c) {
  switch (c.kind) {
    case Change::apt_source: return _("Adds a package source");
    case Change::apt_key: return _("Adds a signing key for package sources");
    case Change::scripts: return _("Runs scripts as administrator");
    case Change::downloads: return _("Downloads files while it is installed");
    case Change::cron: return _("Adds scheduled tasks");
    case Change::service: return _("Adds system services");
    case Change::autostart: return _("Starts programs at every login");
    case Change::setuid: return _("Contains programs that run with administrator rights");
    case Change::users: return _("Creates system users or groups");
    case Change::sudoers: return _("Changes who may use sudo");
    case Change::polkit: return _("Changes who may do administrative tasks");
    case Change::udev: return _("Adds device rules");
    case Change::kernel: return _("Adds kernel modules");
    case Change::pam: return _("Changes how users log in");
    case Change::environment: return _("Changes settings of every login");
    case Change::alternatives: return _("Registers itself as a default program");
    case Change::debconf: return _("May ask questions while it is installed");
    case Change::conffiles: return _("Keeps configuration files in /etc");
    case Change::essential: return _("Marked as essential");
  }
  return "";
}

std::string change_text(const Change &c) {
  switch (c.kind) {
    case Change::apt_source:
      return _("APT will also install updates of this software from its maker, and anything else that source "
               "offers, signed with its key.");
    case Change::apt_key: return _("APT will trust packages signed with this key.");
    case Change::scripts:
      return _("Its maintainer scripts run with full rights while it is installed, upgraded or removed. This is "
               "normal for Debian packages.");
    case Change::downloads: return _("A script fetches files from the internet; what it downloads was not checked.");
    case Change::cron: return _("Programs that run regularly in the background.");
    case Change::service: return _("Programs that start with the computer and run in the background.");
    case Change::autostart: return _("They start in the session of every user.");
    case Change::setuid:
      return _("Set-user-ID programs run with the rights of their owner (usually root), whoever starts them.");
    case Change::users: return _("Accounts for its own services.");
    case Change::sudoers: return _("sudo rules decide who may run what as administrator.");
    case Change::polkit: return _("polkit rules decide who may change the system without a password.");
    case Change::udev: return _("Rules for devices (permissions, names, programs started when a device appears).");
    case Change::kernel: return _("Code that runs inside the operating system (also built with DKMS).");
    case Change::pam: return _("Login, password and screen lock configuration.");
    case Change::environment: return _("Environment variables or library paths for every program.");
    case Change::alternatives: return _("It can become the program the system uses for these roles.");
    case Change::debconf: return _("Debrun answers them with their default values.");
    case Change::conffiles: return _("They stay when the package is removed, until it is purged.");
    case Change::essential: return _("The system cannot work without it.");
  }
  return "";
}

bool serious(const Change &c) {
  switch (c.kind) {
    case Change::apt_source:
    case Change::apt_key:
    case Change::downloads:
    case Change::setuid:
    case Change::sudoers:
    case Change::polkit:
    case Change::kernel:
    case Change::pam:
      return true;
    default:
      return false;
  }
}

void changes_card(Page *page) {
  const Theme &t = active_theme();
  page->anchor("changes");
  page->begin_card(_("What it changes besides its files"));
  if (g.a.changes.empty()) page->note(_("Nothing: it only installs its files."));
  int x = page->left() + S(20), w = page->width() - S(40);
  for (const Change &c : g.a.changes) {
    new DotList(x, page->cursor(), w, {{serious(c) ? t.warning : t.accent, change_title(c), ""}});
    page->advance(S(30));
    int tx = x + S(24), tw = w - S(24);
    std::string text = change_text(c);
    if (!c.details.empty()) {
      std::vector<std::string> d = c.details;
      text += "\n" + join_names(d, 5);
    }
    int h = Label::measure(text, tw, Label::DIM) + S(4);
    new Label(tx, page->cursor(), tw, h, text, Label::DIM, true);
    page->advance(h + S(8));
  }
  page->end_card();
}

void deps_card(Page *page) {
  const Theme &t = active_theme();
  const Analysis &a = g.a;
  page->anchor("dependencies");
  page->begin_card(_("Dependencies"));
  std::vector<DotRow> rows;
  int missing = 0;
  for (const DepCheck &d : a.deps) {
    DotRow r;
    r.text = alternatives_text(d.alts);
    if (d.field == "Recommends") r.text += "  " + std::string(_("(recommended)"));
    if (d.field == "Pre-Depends") r.text += "  " + std::string(_("(needed first)"));
    switch (d.state) {
      case DepCheck::State::installed:
        r.color = t.success;
        r.detail = format(_("installed %s"), d.detail.c_str());
        break;
      case DepCheck::State::provided:
        r.color = t.success;
        r.detail = format(_("provided by %s"), d.detail.c_str());
        break;
      case DepCheck::State::will_install:
        r.color = t.accent;
        r.detail = format(_("will be installed: %s"), d.detail.c_str());
        break;
      case DepCheck::State::ok:
        r.color = t.accent;
        r.detail = _("will be installed");
        break;
      case DepCheck::State::missing:
        r.color = d.field == "Recommends" ? t.warning : t.error;
        r.detail = d.field == "Recommends" ? _("not available (optional)") : _("not available");
        if (starts_with(d.detail, "but ") && d.detail.find("is to be installed") != std::string::npos)
          r.detail = d.detail.substr(4);  // "1.0-1 is to be installed": too old
        if (d.field != "Recommends") ++missing;
        break;
      case DepCheck::State::conflict:
        r.color = t.error;
        r.detail = format(_("conflicts with %s"), d.detail.c_str());
        r.text = d.field + ": " + r.text;
        break;
      case DepCheck::State::removed:
        r.color = t.warning;
        r.detail = format(_("%s will be removed"), d.detail.c_str());
        r.text = d.field + ": " + r.text;
        break;
    }
    rows.push_back(r);
  }
  if (rows.empty()) page->note(_("It needs no other packages."));
  else {
    new DotList(page->left() + S(20), page->cursor(), page->width() - S(40), rows);
    page->advance(int(rows.size()) * S(30) + S(6));
  }
  std::vector<std::string> extra = extra_packages(a), gone = removed_packages(a.plan);
  if (!extra.empty())
    page->note(format(ngettext("Installed with it: %s (%d package).", "Installed with it: %s (%d packages).",
                               extra.size()),
                      join_names(extra, 12).c_str(), int(extra.size())));
  if (!gone.empty())
    page->note(format(_("Removed by it: %s."), join_names(gone, 12).c_str()));
  if (a.plan.ran && !a.plan.ok) {
    for (const auto &u : a.plan.unmet) page->note(u);
    if (a.plan.unmet.empty())
      for (const auto &e : a.plan.errors) page->note(e);
  }
  if (missing || (a.plan.ran && !a.plan.ok && a.plan.problem == AptPlan::Problem::unmet)) {
    std::string label = _("Refresh Package Lists");
    page->row(_("Out of date?"), button_w(label), S(34),
              [&](int x, int y, int w, int h) {
                Fl_Widget *b = link_button(x, y, w, h, label, [] { start_job(Job::refresh); });
                if (g.phase == Phase::working) b->deactivate();
                return b;
              },
              _("Missing packages may be in the package sources already: their lists on this computer can be old."));
  }
  if (!a.plan.ran && a.fit.arch_ok)
    page->note(_("APT could not check the dependencies (is apt-get installed?). The installation resolves them."));
  page->end_card();
}

void files_card(Page *page) {
  const Package &p = g.a.pkg;
  page->anchor("files");
  page->begin_card(_("Files"));
  page->note(format(ngettext("%zu entry, %s in all.", "%zu entries, %s in all.", p.file_count), p.file_count,
                    format_size(p.data_bytes).c_str()));
  size_t shown = g.all_files ? std::min<size_t>(p.files.size(), 20000) : std::min<size_t>(p.files.size(), 60);
  new FileList(page->left() + S(20), page->cursor(), page->width() - S(40), p.files, shown);
  page->advance(int(std::max<size_t>(1, shown)) * S(24) + S(6));
  if (shown < p.file_count && !g.all_files) {
    std::string label = format(_("Show All %zu"), p.file_count);
    page->row("", button_w(label), S(34), [&](int x, int y, int w, int h) {
      Fl_Widget *b = link_button(x, y, w, h, label, [] {
        g.all_files = true;
        rebuild();
      });
      b->tooltip(keep(new std::string(_("Show all files")))->c_str());
      return b;
    });
  } else if (shown < p.file_count) {
    page->note(format(_("The first %zu are shown."), shown));
  }
  page->end_card();
}

void log_card(Page *page) {
  page->anchor("log");
  page->begin_card(_("Details"));
  if (!g.outcome.detail.empty()) page->note(g.outcome.detail);
  std::string text;
  size_t from = g.log.size() > 60 ? g.log.size() - 60 : 0;
  for (size_t i = from; i < g.log.size(); ++i) text += g.log[i] + "\n";
  if (!text.empty()) page->note(trim(text));
  if (text.empty() && g.outcome.detail.empty()) page->note(_("Nothing more was reported."));
  page->end_card();
}

void build_page(Page *page) {
  const Analysis &a = g.a;
  switch (g.phase) {
    case Phase::empty:
      page->begin_card(_("Install a downloaded package"));
      page->note(_("Makers of software often offer it as a Debian package (.deb) on their website. Debrun shows "
                   "what such a package contains and changes before it installs it, and installs what it needs "
                   "from the package sources of this system."));
      page->note(_("Open a package with a double-click in Liria or on the desktop, or choose one."));
      page->end_card();
      return;
    case Phase::loading:
      page->begin_card();
      page->note(format(_("Reading %s\xE2\x80\xA6"), g.path.c_str()));
      page->end_card();
      return;
    case Phase::error:
      banner(page, _("This file cannot be installed"), a.error + "\n" + g.path, Tone::bad);
      return;
    default: break;
  }
  if (g.phase == Phase::failed && g.show_log) log_card(page);
  if (!a.fit.problem.empty()) banner(page, _("Not for this computer"), a.fit.problem, Tone::bad);
  if (g.phase == Phase::done && !g.done_text.empty()) banner(page, _("Done"), g.done_text, Tone::good);
  // Trust first, then what it does to the system, then the details.
  origin_card(page);
  changes_card(page);
  deps_card(page);
  if (!a.pkg.description.empty()) {
    page->begin_card(_("Description"));
    page->note(a.pkg.description);
    page->end_card();
  }
  package_card(page);
  files_card(page);
}

// ---------------------------------------------------------- the buttons

struct ButtonDef {
  std::string label, tip;
  std::function<void()> run;
  bool primary = false, enabled = true;
};

std::vector<ButtonDef> bar_buttons() {
  std::vector<ButtonDef> out;  // right to left
  const Analysis &a = g.a;
  switch (g.phase) {
    case Phase::empty: out.push_back({_("Choose Package..."), "", choose_file, true}); break;
    case Phase::loading: out.push_back({_("Close"), "", close_window}); break;
    case Phase::error:
      out.push_back({_("Close"), "", close_window});
      out.push_back({_("Choose Another..."), "", choose_file});
      break;
    case Phase::ready:
      out.push_back({action_label(a.fit.action), _("Install the package"), install_clicked, true, can_install(a)});
      out.push_back({_("Close"), "", close_window});
      break;
    case Phase::confirm:
      if (g.confirming == Job::remove)
        out.push_back({_("Remove"), _("Confirm the removal"), remove_clicked, true});
      else
        out.push_back({_("Install and Remove"), _("Install the package"), install_clicked, true});
      out.push_back({_("Cancel"), "", [] {
                       g.confirming = Job::none;
                       g.phase = Phase::ready;
                       rebuild();
                     }});
      break;
    case Phase::working: break;
    case Phase::done:
      if (g.last_job == Job::install && !a.pkg.app_launcher.empty())
        out.push_back({_("Open"), _("Open the application"), open_app, true});
      out.push_back({_("Close"), "", close_window, true, true});
      if (out.size() == 2) out[1].primary = false;
      break;
    case Phase::failed:
      out.push_back({_("Close"), "", close_window, true});
      out.push_back({g.show_log ? _("Hide Details") : _("Show Details"), "", [] {
                       g.show_log = !g.show_log;
                       rebuild();
                     }});
      break;
  }
  return out;
}

void place_buttons() {
  DebrunWindow *w = g_win;
  std::vector<ButtonDef> defs = bar_buttons();
  int x = w->w() - S(24), bh = S(36), y = w->h() - bar_h() + (bar_h() - bh) / 2;
  for (int i = 0; i < 3; ++i) {
    FlatButton *b = w->buttons[i];
    if (i >= int(defs.size())) {
      b->hide();
      continue;
    }
    const ButtonDef &d = defs[size_t(i)];
    int bw = button_w(d.label);
    x -= bw;
    // A new button, so that the primary look and the label are right.
    w->remove(b);
    Fl::delete_widget(b);
    w->begin();
    b = new FlatButton(x, y, bw, bh, d.label, d.primary);
    w->end();
    b->copy_label(d.label.c_str());
    if (!d.tip.empty()) b->tooltip(keep(new std::string(d.tip))->c_str());
    w->actions[size_t(i)] = d.run;
    b->callback([](Fl_Widget *, void *data) {
      size_t index = size_t(reinterpret_cast<intptr_t>(data));
      Fl::add_timeout(0, [](void *dd) {
        size_t n = size_t(reinterpret_cast<intptr_t>(dd));
        if (g_win && g_win->actions[n]) {
          auto fn = g_win->actions[n];
          fn();
        }
      }, data);
      (void)index;
    }, reinterpret_cast<void *>(intptr_t(i)));
    if (!d.enabled) b->deactivate();
    w->buttons[i] = b;
    x -= S(10);
  }
}

void relayout() {
  DebrunWindow *w = g_win;
  if (!w) return;
  int W = w->w(), H = w->h();
  place_buttons();
  bool working = g.phase == Phase::working;
  int pad = S(24);
  w->logo->resize(pad, H - bar_h() + (bar_h() - S(52)) / 2, S(52), S(52));
  if (working) {
    w->logo->show();
    w->logo->running(true);
    int px = pad + S(52) + S(14);
    w->progress->resize(px, H - bar_h() + S(48), std::max(S(60), W - px - pad - S(70)), S(6));
    w->progress->show();
    w->progress->value(g.step.percent);
  } else {
    w->logo->running(false);
    w->logo->hide();
    w->progress->hide();
    w->progress->value(0);
  }
  // The safe button takes the keyboard.
  for (FlatButton *b : w->buttons)
    if (b->visible() && b->active() && b->label() &&
        (std::string(b->label()) == _("Close") || std::string(b->label()) == _("Cancel")))
      b->take_focus();
  w->redraw();
}

void rebuild() {
  DebrunWindow *w = g_win;
  if (!w) return;
  int scroll = 0;
  if (w->page) {
    scroll = w->page->yposition();
    w->remove(w->page);
    Fl::delete_widget(w->page);
  }
  int hh = header_h(w->h());
  w->begin();
  w->page = new Page(0, hh, w->w(), w->h() - hh - bar_h());
  w->page->max_width(S(820));
  build_page(w->page);
  w->page->finish();
  w->end();
  w->insert(*w->page, 0);  // under the bar's widgets
  if (scroll && g.phase != Phase::loading) w->page->scroll_to(0, std::min(scroll, w->page->max_scroll()));
  std::string title = g.a.ok ? title_of(g.a.pkg) + " \xE2\x80\x94 Debrun" : std::string("Debrun");
  w->copy_label(title.c_str());
  relayout();
}

}  // namespace

AppInfo about_info() {
  AppInfo info;
  info.name = _("Debrun");
  info.program = "debrun";
  info.version = SDE_VERSION;
  info.icon = "debrun";
  info.description = _("Installs Debian packages downloaded from the websites of their makers, the Debian way: "
                       "it shows what a package contains and changes, and APT installs what it needs from the "
                       "package sources.");
  info.credits = {toolkit_credit(), _("Packages read with libarchive; installed with APT and dpkg (or PackageKit)"),
                  _("The idea of a package installer after GDebi")};
  info.trademarks = {"debian"};
  return info;
}

int run_window(const std::string &path) {
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int W = std::min(S(800), sw - S(24)), H = std::min(S(700), sh - S(48));
  g_win = new DebrunWindow(W, H);
  g_win->xclass("Debrun");
  g_win->size_range(std::min(S(520), W), std::min(S(380), H));
  g_win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  g_win->callback([](Fl_Widget *, void *) { close_window(); });
  open_package(path);
  g_win->show();
  int r = Fl::run();
  free_icons();
  return r;
}

}  // namespace debrun
}  // namespace sde
