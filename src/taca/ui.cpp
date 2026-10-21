// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Small user interface pieces: themed text fields, option chips, worker
// jobs, URI lists and file choosers.
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <set>

#include <unistd.h>

#include <cerrno>
#include <ctime>

#include "config.h"
#include "i18n.h"
#include "taca.h"
#include "util.h"

using namespace sde;

namespace taca {

void style_input(Fl_Input *in, const Theme &t) {
  in->box(FL_FLAT_BOX);
  in->color(t.surface_alt.fl());
  in->selection_color(t.accent.fl());
  in->textcolor(t.text.fl());
  in->cursor_color(t.accent.fl());
  in->textsize(t.font_size);
  in->textfont(FL_HELVETICA);
}

void draw_field(Painter &p, Fl_Widget *in, const Theme &t, int pad_left) {
  if (!in->visible()) return;
  int x = in->x() - S(8) - pad_left, y = in->y() - S(4), w = in->w() + S(16) + pad_left, h = in->h() + S(8);
  bool focus = Fl::focus() == in;
  p.fill_rounded(x, y, w, h, S(8), in->active_r() ? t.surface_alt : t.surface_alt.mix(t.background, 0.5));
  p.stroke_rounded(x + 0.5, y + 0.5, w - 1, h - 1, S(8), focus ? t.accent : t.border, focus ? Sd(1.5) : 1);
}

// ------------------------------------------------------------------ chips

Chips::Chips(int x, int y, int w, int h, std::vector<std::string> labels)
    : Fl_Widget(x, y, w, h), labels_(std::move(labels)) {}

void Chips::enable(int i, bool on) {
  if (on) off_.erase(i);
  else off_.insert(i);
  redraw();
}

std::vector<Chips::Cell> Chips::layout() const {
  std::vector<Cell> cells;
  text_font(active_theme().font_size);
  int cx = x(), cy = y(), ch = S(30), gap = S(6);
  for (const auto &l : labels_) {
    int cw = std::max(S(46), text_width(l) + S(24));
    if (cx + cw > x() + w() && cx > x()) {
      cx = x();
      cy += ch + gap;
    }
    cells.push_back({cx, cy, cw, ch});
    cx += cw + gap;
  }
  return cells;
}

int Chips::rows_needed(int width) const {
  text_font(active_theme().font_size);
  int cx = 0, rows = 1;
  for (const auto &l : labels_) {
    int cw = std::max(S(46), text_width(l) + S(24));
    if (cx + cw > width && cx > 0) {
      cx = 0;
      ++rows;
    }
    cx += cw + S(6);
  }
  return rows;
}

void Chips::draw() {
  const Theme &t = active_theme();
  auto cells = layout();
  {
    Painter p(window()->w(), window()->h());
    for (size_t i = 0; i < cells.size(); ++i) {
      const auto &c = cells[i];
      bool on = int(i) == value_, dis = off_.count(int(i)) || !active_r();
      Color bg = on ? t.accent : (int(i) == hover_ && !dis ? t.hover : t.surface_alt);
      p.fill_rounded(c.x, c.y, c.w, c.h, c.h / 2.0, dis && !on ? bg.alpha(0.5) : bg);
      if (!on) p.stroke_rounded(c.x + 0.5, c.y + 0.5, c.w - 1, c.h - 1, c.h / 2.0, t.border);
      if (Fl::focus() == this && int(i) == focus_) focus_ring(p, c.x, c.y, c.w, c.h, c.h / 2.0);
    }
  }
  text_font(t.font_size, false);
  for (size_t i = 0; i < cells.size(); ++i) {
    const auto &c = cells[i];
    bool on = int(i) == value_, dis = off_.count(int(i)) || !active_r();
    Color fg = on ? t.on_accent : dis ? t.text_dim.alpha(0.6) : t.text;
    if (dis && !on) fg = t.text_dim.mix(t.surface_alt, 0.4);
    draw_text(labels_[i], c.x, c.y, c.w, c.h, fg, 1);
  }
}

int Chips::handle(int event) {
  auto cells = layout();
  auto at = [&](int mx, int my) {
    for (size_t i = 0; i < cells.size(); ++i)
      if (mx >= cells[i].x && mx < cells[i].x + cells[i].w && my >= cells[i].y && my < cells[i].y + cells[i].h)
        return int(i);
    return -1;
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int h = at(Fl::event_x(), Fl::event_y());
      if (h != hover_) {
        hover_ = h;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      int i = at(Fl::event_x(), Fl::event_y());
      if (i >= 0 && enabled(i)) {
        value_ = focus_ = i;
        if (Fl::visible_focus()) Fl::focus(this);
        do_callback();
        redraw();
      }
      return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
      focus_ = value_;
      redraw();
      return 1;
    case FL_KEYBOARD: {
      int k = Fl::event_key(), n = int(labels_.size());
      int step = k == FL_Left || k == FL_Up ? -1 : k == FL_Right || k == FL_Down ? 1 : 0;
      if (step) {
        int i = value_;
        for (int tries = 0; tries < n; ++tries) {
          i = (i + step + n) % n;
          if (enabled(i)) break;
        }
        value_ = focus_ = i;
        do_callback();
        redraw();
        return 1;
      }
      return 0;
    }
  }
  return Fl_Widget::handle(event);
}

// ------------------------------------------------------------------- jobs

namespace {

volatile sig_atomic_t g_terminate = 0;
std::set<Job *> g_jobs;  // main thread only

void on_signal(int) { g_terminate = 1; }

// SIGTERM or SIGINT: cancel running work (which removes its temporary
// files) and leave once nothing runs any more.
void poll_terminate(void *) {
  if (g_terminate) {
    if (g_jobs.empty()) std::exit(143);
    for (Job *j : g_jobs) j->cancel();
  }
  Fl::repeat_timeout(0.2, poll_terminate);
}

}  // namespace

void install_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
  Fl::add_timeout(0.2, poll_terminate);
}

Job::Job(const std::string &title, Work work, Done done)
    : title_(title), work_(std::move(work)), done_(std::move(done)) {
  g_jobs.insert(this);
  thread_ = std::thread([this] {
    result_ = work_(progress_);
    Fl::awake(awake_cb, this);
  });
}

Job::~Job() {
  g_jobs.erase(this);
  progress_.cancel = true;
  if (thread_.joinable()) thread_.join();
}

void Job::awake_cb(void *p) {
  auto *job = static_cast<Job *>(p);
  // The job may have been cancelled and deleted before this ran.
  if (!g_jobs.count(job)) return;
  if (job->thread_.joinable()) job->thread_.join();
  job->finished_ = true;
  g_jobs.erase(job);
  if (g_terminate) std::exit(143);
  // |done| may destroy the job: copy what it needs first.
  Done done = job->done_;
  Result result = job->result_;
  if (done) done(result);
}

// ------------------------------------------------------------------ URIs

std::string uri_to_path(const std::string &uri) {
  std::string s = trim(uri);
  if (starts_with(s, "file://")) {
    s = s.substr(7);
    size_t slash = s.find('/');
    if (slash == std::string::npos) return "";
    s = s.substr(slash);  // drop the host name
  } else if (s.empty() || s[0] != '/') {
    return "";
  }
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string path_to_uri(const std::string &path) {
  static const char *hex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : path) {
    if (isalnum(c) || strchr("/-_.~", c)) {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::vector<std::string> parse_uri_list(const std::string &text) {
  std::vector<std::string> out;
  for (auto line : split(text, '\n')) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    std::string p = uri_to_path(line);
    if (!p.empty()) out.push_back(p);
  }
  return out;
}

// --------------------------------------------------------------- choosers

std::string choose_folder(const std::string &title, const std::string &start) {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
  chooser.title(title.c_str());
  chooser.directory(start.c_str());
  if (chooser.show() != 0 || !chooser.filename()) return "";
  return chooser.filename();
}

std::vector<std::string> choose_files(const std::string &title, const std::string &start, bool folders,
                                      bool archives) {
  Fl_Native_File_Chooser chooser;
  chooser.type(folders ? Fl_Native_File_Chooser::BROWSE_MULTI_DIRECTORY : Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
  chooser.title(title.c_str());
  chooser.directory(start.c_str());
  if (archives)
    chooser.filter(_("Archives\t*.{zip,rar,7z,tar,tgz,gz,bz2,xz,zst,lz4,lzma,lz,Z,iso,cab,cpio,deb,rpm,xar,jar,war,"
                     "apk,epub,cbz,cbr,a,ar,lha,lzh,001}\nAll Files\t*"));
  std::vector<std::string> out;
  if (chooser.show() != 0) return out;
  for (int i = 0; i < chooser.count(); ++i)
    if (chooser.filename(i)) out.push_back(chooser.filename(i));
  return out;
}

// ------------------------------------------------------------- settings

std::string favorites_file() { return config_home() + "/sde/taca-favorites"; }

Settings Settings::load() {
  Settings s;
  Config cfg;
  cfg.load();
  std::string f = cfg.get("taca", "format", "zip");
  for (const auto &fi : all_formats())
    if (f == fi.id && fi.format != Format::Unknown) s.format = fi.format;
  s.level = std::max(0, std::min(9, cfg.get_int("taca", "level", 6)));
  s.icons = cfg.get("taca", "view", "details") == "icons";
  s.hidden = cfg.get_bool("taca", "hidden", false);
  s.theme = cfg.get("taca", "theme", "follow");
  if (s.theme != "light" && s.theme != "dark") s.theme = "follow";
  s.open_after = cfg.get_bool("taca", "open_after", true);
  return s;
}

void Settings::save() const {
  Config cfg;
  cfg.load();
  cfg.set("taca", "format", format_info(format).id);
  cfg.set("taca", "level", std::to_string(level));
  cfg.set("taca", "view", icons ? "icons" : "details");
  cfg.set("taca", "hidden", hidden ? "true" : "false");
  cfg.set("taca", "theme", theme);
  cfg.set("taca", "open_after", open_after ? "true" : "false");
  cfg.save();
}

// ------------------------------------------------------------------ trash

namespace {

std::string trash_escape(const std::string &path) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : path) {
    if (isalnum(c) || strchr("/-_.~", c)) {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

}  // namespace

std::vector<std::string> move_to_trash(const std::vector<std::string> &paths, std::string &error) {
  std::vector<std::string> failed;
  std::string trash = data_home() + "/Trash";
  make_dirs(trash + "/files");
  make_dirs(trash + "/info");
  for (const auto &p : paths) {
    std::string base = basename_of(p), name = base;
    for (int i = 2; access((trash + "/info/" + name + ".trashinfo").c_str(), F_OK) == 0 ||
                    access((trash + "/files/" + name).c_str(), F_OK) == 0;
         ++i)
      name = base + "." + std::to_string(i);
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char date[32];
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
    std::string info = trash + "/info/" + name + ".trashinfo";
    if (!write_file_atomic(info, "[Trash Info]\nPath=" + trash_escape(p) + "\nDeletionDate=" + date + "\n")) {
      error = strerror(errno);
      failed.push_back(p);
      continue;
    }
    if (rename(p.c_str(), (trash + "/files/" + name).c_str()) != 0) {
      if (errno != EXDEV) error = strerror(errno);
      unlink(info.c_str());
      failed.push_back(p);
    }
  }
  return failed;
}

}  // namespace taca
