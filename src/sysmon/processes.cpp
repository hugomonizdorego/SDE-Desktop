// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE System Monitor: the Processes page (table, grouping, tree, actions).
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <set>

#include "apps.h"
#include "buffered_window.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "process.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sysmon {

namespace {

enum Column { kName, kPid, kUser, kCpu, kMemory, kRead, kWrite, kThreads, kState, kColumns };

struct ColumnSpec {
  const char *title;
  int width;  // unscaled; 0 = takes the remaining width
  int align;  // 0 left, 2 right
  int priority;  // lower stays longer when the table is narrow
};

const ColumnSpec kSpecs[kColumns] = {
    {N_("Name"), 0, 0, 0},       {N_("PID"), 70, 2, 2},         {N_("User"), 96, 0, 5},
    {N_("CPU"), 74, 2, 1},       {N_("Memory"), 96, 2, 1},      {N_("Disk read"), 96, 2, 4},
    {N_("Disk write"), 96, 2, 4}, {N_("Threads"), 72, 2, 6},    {N_("State"), 90, 0, 3},
};

struct Row {
  int proc = -1;  // index into the snapshot's process list
  int depth = 0;
  int group = -1;  // >= 0 for group header rows
  int count = 0;   // processes in the group
  bool app = false;
  std::string name, icon;
  Window window = 0;
};

std::string lower_copy(std::string s) {
  for (auto &c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

const char *signal_name(int sig) {
  switch (sig) {
    case SIGTERM: return "TERM";
    case SIGKILL: return "KILL";
    case SIGSTOP: return "STOP";
    case SIGCONT: return "CONT";
    default: return "TERM";
  }
}

std::string read_link(const std::string &path) {
  char buf[4096];
  ssize_t n = readlink(path.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return "";
  buf[n] = 0;
  return buf;
}

// ------------------------------------------------------------ Properties

class PropertiesWindow : public sde::BufferedWindow {
 public:
  PropertiesWindow(const ProcInfo &p, const std::string &display_name, uint64_t boot_time)
      : sde::BufferedWindow(S(560), S(470), _("Process Properties")), name_(display_name) {
    const sde::Theme &t = sde::active_theme();
    std::string base = "/proc/" + std::to_string(p.pid);
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    exe_ = read_link(base + "/exe");
    std::string cwd = read_link(base + "/cwd");
    int fds = -1;
    if (DIR *d = opendir((base + "/fd").c_str())) {
      fds = 0;
      while (dirent *e = readdir(d))
        if (e->d_name[0] != '.') ++fds;
      closedir(d);
    }
    std::string status, vm_size, vm_swap;
    if (read_text(base + "/status", status)) {
      for (auto &line : sde::split(status, '\n')) {
        if (sde::starts_with(line, "VmSize:")) vm_size = sde::trim(line.substr(7));
        if (sde::starts_with(line, "VmSwap:")) vm_swap = sde::trim(line.substr(7));
      }
    }
    char started[64] = "";
    time_t start = time_t(boot_time + p.start_ticks / uint64_t(hz));
    struct tm tm;
    if (boot_time && localtime_r(&start, &tm)) strftime(started, sizeof started, "%Y-%m-%d %H:%M:%S", &tm);
    auto unknown = [](const std::string &s) { return s.empty() ? std::string(_("Not available")) : s; };
    rows_ = {
        {_("Process ID"), std::to_string(p.pid)},
        {_("Parent ID"), std::to_string(p.ppid)},
        {_("User"), p.user},
        {_("State"), _(state_name(p.state))},
        {_("Priority"), sde::format(_("Nice %ld"), p.nice)},
        {_("Threads"), std::to_string(p.threads)},
        {_("Memory"), format_bytes(p.memory)},
        {_("Virtual memory"), unknown(vm_size)},
        {_("Swap"), unknown(vm_swap)},
        {_("CPU time"), format_duration(double(p.cpu_ticks) / double(hz))},
        {_("Started"), unknown(started)},
        {_("Open files"), fds >= 0 ? std::to_string(fds) : std::string(_("Not available"))},
        {_("Executable"), unknown(exe_)},
        {_("Working folder"), unknown(cwd)},
    };
    cmdline_ = p.cmdline.empty() ? "[" + p.comm + "]" : p.cmdline;
    // Close is created first so that it has the initial keyboard focus.
    auto *close = new sde::FlatButton(w() - S(120), h() - S(54), S(100), S(34), _("Close"), true);
    open_ = new sde::FlatButton(S(20), h() - S(54), S(180), S(34), _("Open File Location"));
    open_->callback([](Fl_Widget *, void *d) {
      auto *self = static_cast<PropertiesWindow *>(d);
      std::string dir = self->exe_.substr(0, self->exe_.rfind('/'));
      if (!dir.empty()) open_folder(dir);
    }, this);
    if (exe_.empty() || exe_.find('/') == std::string::npos) open_->deactivate();
    close->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    end();
    set_non_modal();
    xclass("sde-system-monitor");
    callback([](Fl_Widget *w, void *) { w->hide(); });
    (void)t;
  }

  void draw() override {
    const sde::Theme &t = sde::active_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      p.fill_rounded(S(16), S(64), w() - S(32), h() - S(64) - S(70), Sd(12), t.surface);
      p.stroke_rounded(S(16) + 0.5, S(64) + 0.5, w() - S(32) - 1, h() - S(64) - S(70) - 1, Sd(12), t.border);
    }
    sde::text_font(t.font_size + S(5), true);
    sde::draw_text(name_, S(20), S(14), w() - S(40), S(30), t.text);
    sde::text_font(t.font_size - S(1));
    fl_color(t.text_dim.fl());
    fl_draw(cmdline_.c_str(), S(20), S(42), w() - S(40), S(20), FL_ALIGN_LEFT | FL_ALIGN_CLIP | FL_ALIGN_INSIDE);
    int y = S(74), rh = S(22);
    for (auto &r : rows_) {
      sde::text_font(t.font_size);
      sde::draw_text(r.first, S(32), y, S(140), rh, t.text_dim);
      sde::draw_text(r.second, S(176), y, w() - S(208), rh, t.text);
      y += rh;
    }
    draw_children();
  }

 private:
  std::string name_, cmdline_, exe_;
  std::vector<std::pair<std::string, std::string>> rows_;
  sde::FlatButton *open_;
};

}  // namespace

// ------------------------------------------------------------ the table

class ProcessTable : public Fl_Widget {
 public:
  std::function<void(int pid)> on_select, on_activate;
  std::function<void(int pid, int x, int y)> on_menu;

  ProcessTable(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_NO_BOX); }

  void set_model(std::shared_ptr<const Snapshot> snap, std::vector<Row> rows) {
    snap_ = std::move(snap);
    rows_ = std::move(rows);
    clamp_scroll();
    redraw();
  }
  const Snapshot *snapshot() const { return snap_.get(); }
  int selected() const { return selected_pid_; }
  const Row *selected_row() const {
    for (auto &r : rows_)
      if (r.proc >= 0 && snap_->procs[r.proc].pid == selected_pid_) return &r;
    return nullptr;
  }
  const ProcInfo *selected_proc() const {
    const Row *r = selected_row();
    return r ? &snap_->procs[r->proc] : nullptr;
  }

  int sort_column = kCpu;
  bool sort_descending = true;
  std::set<int> collapsed;
  std::function<void()> on_sort, on_toggle_group;

  void draw() override {
    const sde::Theme &t = sde::active_theme();
    layout_columns();
    fl_push_clip(x(), y(), w(), h());
    int hh = header_h(), rh = row_h();
    int body_y = y() + hh, body_h = h() - hh;
    int first = top_, last = std::min<int>(int(rows_.size()), top_ + body_h / rh + 2);
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), t.background);
      p.fill_rounded(x(), y(), w(), h(), Sd(12), t.surface);
      // Header band.
      cairo_save(p.cr());
      p.rounded_rect(x(), y(), w(), h(), Sd(12));
      cairo_clip(p.cr());
      p.fill_rect(x(), y(), w(), hh, t.surface_alt.mix(t.surface, 0.35));
      p.line(x(), y() + hh - 0.5, x() + w(), y() + hh - 0.5, t.border);
      cairo_restore(p.cr());
      for (int i = first; i < last; ++i) {
        int ry = body_y + (i - top_) * rh;
        const Row &r = rows_[i];
        bool sel = r.proc >= 0 && snap_->procs[r.proc].pid == selected_pid_;
        if (sel)
          p.fill_rounded(x() + S(6), ry + 1, w() - S(12) - S(10), rh - 2, Sd(6), t.accent.alpha(t.dark ? 0.30 : 0.16));
        else if (i == hover_)
          p.fill_rounded(x() + S(6), ry + 1, w() - S(12) - S(10), rh - 2, Sd(6), t.hover);
        if (r.group >= 0) {
          // Chevron: right when collapsed, down when open.
          cairo_t *cr = p.cr();
          double cx = x() + S(20), cy = ry + rh / 2.0, a = Sd(3.5);
          cairo_save(cr);
          cairo_translate(cr, cx, cy);
          if (!collapsed.count(r.group)) cairo_rotate(cr, M_PI / 2);
          cairo_move_to(cr, -a / 2, -a);
          cairo_line_to(cr, a / 2, 0);
          cairo_line_to(cr, -a / 2, a);
          cairo_restore(cr);
          p.set(t.text_dim);
          cairo_set_line_width(cr, Sd(1.6));
          cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
          cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
          cairo_stroke(cr);
          continue;
        }
        const ProcInfo &pi = snap_->procs[r.proc];
        // Name cell: icon, then text drawn below.
        double ix = col_x_[kName] + S(10) + r.depth * S(18);
        double isz = S(18), iy = ry + (rh - isz) / 2.0;
        if (r.depth > 0) {
          Color guide = t.border;
          p.line(ix - S(11) + 0.5, ry, ix - S(11) + 0.5, ry + rh / 2.0, guide);
          p.line(ix - S(11), ry + rh / 2.0 + 0.5, ix - S(3), ry + rh / 2.0 + 0.5, guide);
        }
        cairo_surface_t *icon = nullptr;
        if (!r.icon.empty()) icon = sde::IconLoader::get({r.icon, "application-x-executable"}, int(isz));
        if (!icon && r.window) icon = cached_window_icon(r.window, int(isz));
        if (icon) {
          p.image(icon, ix, iy, isz);
        } else if (r.app) {
          p.fill_rounded(ix, iy, isz, isz, Sd(5), t.accent.alpha(0.85));
        } else {
          Color c = t.text_dim.alpha(0.65);
          p.stroke_rounded(ix + Sd(2.5), iy + Sd(2.5), isz - Sd(5), isz - Sd(5), Sd(3), c, Sd(1.3));
          p.fill_rect(ix + Sd(5.5), iy + Sd(6.5), isz - Sd(11), Sd(1.3), c);
          p.fill_rect(ix + Sd(5.5), iy + Sd(9.5), isz - Sd(13), Sd(1.3), c);
        }
        // CPU share as a thin bar under the value.
        if (col_w_[kCpu] > 0 && pi.cpu >= 1.0) {
          double bw = (col_w_[kCpu] - S(16)) * std::min(1.0, pi.cpu / 100.0);
          p.fill_rounded(col_x_[kCpu] + col_w_[kCpu] - S(8) - bw, ry + rh - S(7), std::max(Sd(2), bw), Sd(2.5),
                         Sd(1.2), palette().cpu.alpha(0.85));
        }
      }
      // Scroll indicator.
      int total = int(rows_.size()) * rh;
      if (total > body_h) {
        double track_h = body_h - S(8);
        double th = std::max(double(S(28)), track_h * body_h / total);
        double ty = body_y + S(4) + (track_h - th) * double(top_) / std::max(1, max_top());
        p.fill_rounded(x() + w() - S(10), ty, S(5), th, Sd(2.5), dragging_ ? t.text_dim : t.text_dim.alpha(0.45));
      }
      // Sort arrow.
      int sc = sort_column;
      if (col_w_[sc] > 0) {
        double ax = kSpecs[sc].align == 2 ? col_x_[sc] + col_w_[sc] - S(10) - text_w(sc) - S(14)
                                          : col_x_[sc] + S(10) + text_w(sc) + S(6);
        double ay = y() + hh / 2.0;
        cairo_t *cr = p.cr();
        p.set(t.accent_text);
        double a = Sd(4);
        if (sort_descending) {
          cairo_move_to(cr, ax, ay - a / 2), cairo_line_to(cr, ax + 2 * a, ay - a / 2), cairo_line_to(cr, ax + a, ay + a / 2 + 1);
        } else {
          cairo_move_to(cr, ax, ay + a / 2), cairo_line_to(cr, ax + 2 * a, ay + a / 2), cairo_line_to(cr, ax + a, ay - a / 2 - 1);
        }
        cairo_close_path(cr);
        cairo_fill(cr);
      }
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(12), t.border);
    }
    // Text pass.
    for (int c = 0; c < kColumns; ++c) {
      if (col_w_[c] <= 0) continue;
      sde::text_font(t.font_size - S(1), c == sort_column);
      sde::draw_text(_(kSpecs[c].title), col_x_[c] + S(10), y(), col_w_[c] - S(20), hh,
                     c == sort_column ? t.text : t.text_dim, kSpecs[c].align);
    }
    fl_push_clip(x(), body_y, w(), body_h);
    for (int i = first; i < last; ++i) {
      int ry = body_y + (i - top_) * rh;
      const Row &r = rows_[i];
      if (r.group >= 0) {
        sde::text_font(t.font_size, true);
        sde::draw_text(r.name + "  (" + std::to_string(r.count) + ")", x() + S(34), ry, w() / 2, rh, t.text);
        continue;
      }
      const ProcInfo &pi = snap_->procs[r.proc];
      bool sel = pi.pid == selected_pid_;
      Color fg = sel ? (t.dark ? t.text : t.accent_text.mix(t.text, 0.4)) : t.text;
      for (int c = 0; c < kColumns; ++c) {
        if (col_w_[c] <= 0) continue;
        std::string v;
        Color cc = fg;
        int inset = S(10);
        switch (c) {
          case kName: v = r.name; inset = S(10) + r.depth * S(18) + S(26); break;
          case kPid: v = std::to_string(pi.pid); cc = t.text_dim; break;
          case kUser: v = pi.user; cc = t.text_dim; break;
          case kCpu:
            v = sde::format("%.1f %%", pi.cpu);
            if (pi.cpu < 0.05) cc = t.text_dim;
            break;
          case kMemory: v = format_bytes(pi.memory); break;
          case kRead:
            v = pi.read_rate < 0 ? "–" : pi.read_rate < 1 ? "0" : format_rate(pi.read_rate);
            if (pi.read_rate < 1) cc = t.text_dim;
            break;
          case kWrite:
            v = pi.write_rate < 0 ? "–" : pi.write_rate < 1 ? "0" : format_rate(pi.write_rate);
            if (pi.write_rate < 1) cc = t.text_dim;
            break;
          case kThreads: v = std::to_string(pi.threads); cc = t.text_dim; break;
          case kState: v = _(state_name(pi.state));
            cc = pi.state == 'R' ? palette().disk : pi.state == 'T' || pi.state == 'Z' ? palette().warning : t.text_dim;
            break;
        }
        sde::text_font(t.font_size, c == kName && r.app);
        int right_pad = c == last_col_ ? S(18) : S(10);
        sde::draw_text(v, col_x_[c] + inset, ry, col_w_[c] - inset - right_pad, rh, cc, kSpecs[c].align);
      }
    }
    if (rows_.empty()) {
      sde::text_font(t.font_size);
      sde::draw_text(_("No matching processes"), x(), body_y + S(20), w(), S(30), t.text_dim, 1);
    }
    fl_pop_clip();
    fl_pop_clip();
  }

  int handle(int e) override {
    int hh = header_h(), rh = row_h();
    switch (e) {
      case FL_ENTER: return 1;
      case FL_LEAVE: set_hover(-1); return 1;
      case FL_FOCUS: case FL_UNFOCUS: return 1;
      case FL_MOVE: {
        int ey = Fl::event_y() - y() - hh;
        set_hover(ey >= 0 ? top_ + ey / rh : -1);
        return 1;
      }
      case FL_MOUSEWHEEL:
        scroll_to(top_ + Fl::event_dy() * 3);
        return 1;
      case FL_PUSH: {
        take_focus();
        int ex = Fl::event_x(), ey = Fl::event_y();
        if (ex >= x() + w() - S(16) && ey > y() + hh && int(rows_.size()) * rh > h() - hh) {
          dragging_ = true;
          drag_to(ey);
          return 1;
        }
        if (ey < y() + hh) {
          for (int c = 0; c < kColumns; ++c) {
            if (col_w_[c] > 0 && ex >= col_x_[c] && ex < col_x_[c] + col_w_[c]) {
              if (sort_column == c) sort_descending = !sort_descending;
              else sort_column = c, sort_descending = c != kName && c != kUser && c != kState;
              if (on_sort) on_sort();
            }
          }
          return 1;
        }
        int i = top_ + (ey - y() - hh) / rh;
        if (i < 0 || i >= int(rows_.size())) return 1;
        const Row &r = rows_[i];
        if (r.group >= 0) {
          if (collapsed.count(r.group)) collapsed.erase(r.group);
          else collapsed.insert(r.group);
          if (on_toggle_group) on_toggle_group();
          return 1;
        }
        select_pid(snap_->procs[r.proc].pid);
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          if (on_menu) on_menu(selected_pid_, ex, ey);
        } else if (Fl::event_clicks() > 0) {
          Fl::event_clicks(0);
          if (on_activate) on_activate(selected_pid_);
        }
        return 1;
      }
      case FL_DRAG:
        if (dragging_) drag_to(Fl::event_y());
        return 1;
      case FL_RELEASE:
        if (dragging_) dragging_ = false, redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        int page = std::max(1, (h() - hh) / rh - 1);
        if (k == FL_Up) move_selection(-1);
        else if (k == FL_Down) move_selection(1);
        else if (k == FL_Page_Up) move_selection(-page);
        else if (k == FL_Page_Down) move_selection(page);
        else if (k == FL_Home) move_selection(-1000000);
        else if (k == FL_End) move_selection(1000000);
        else if ((k == FL_Enter || k == FL_KP_Enter) && on_activate && selected_pid_) on_activate(selected_pid_);
        else if (k == FL_Menu && on_menu && selected_pid_) on_menu(selected_pid_, x() + S(60), y() + hh + S(40));
        else return 0;
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }

  void select_pid(int pid) {
    if (pid == selected_pid_) return;
    selected_pid_ = pid;
    redraw();
    if (on_select) on_select(pid);
  }

 private:
  int header_h() const { return S(34); }
  int row_h() const { return S(30); }
  int max_top() const { return std::max(0, int(rows_.size()) - (h() - header_h()) / row_h()); }
  void clamp_scroll() { top_ = std::max(0, std::min(top_, max_top())); }
  void scroll_to(int top) {
    int old = top_;
    top_ = top;
    clamp_scroll();
    if (old != top_) redraw();
  }
  void drag_to(int ey) {
    int body = h() - header_h();
    double f = double(ey - y() - header_h()) / std::max(1, body);
    scroll_to(int(std::round(f * max_top())));
    redraw();
  }
  void set_hover(int i) {
    if (i >= int(rows_.size())) i = -1;
    if (i == hover_) return;
    hover_ = i;
    redraw();
  }
  void move_selection(int delta) {
    if (rows_.empty()) return;
    int cur = -1;
    for (size_t i = 0; i < rows_.size(); ++i)
      if (rows_[i].proc >= 0 && snap_->procs[rows_[i].proc].pid == selected_pid_) cur = int(i);
    int i = cur < 0 ? (delta > 0 ? -1 : int(rows_.size())) : cur;
    int step = delta > 0 ? 1 : -1;
    int target = std::max(0, std::min(int(rows_.size()) - 1, i + delta));
    while (target >= 0 && target < int(rows_.size()) && rows_[target].proc < 0) target += step;
    if (target < 0 || target >= int(rows_.size())) return;
    select_pid(snap_->procs[rows_[target].proc].pid);
    int visible = (h() - header_h()) / row_h();
    if (target < top_) scroll_to(target);
    else if (target >= top_ + visible) scroll_to(target - visible + 1);
  }
  int text_w(int c) const {
    sde::text_font(sde::active_theme().font_size - S(1), true);
    return sde::text_width(_(kSpecs[c].title));
  }
  void layout_columns() {
    // Drop low-priority columns until the name keeps a useful width.
    int avail = w() - S(12);
    for (int level = 7; level >= 0; --level) {
      int fixed = 0;
      for (int c = 1; c < kColumns; ++c)
        if (kSpecs[c].priority < level) fixed += S(kSpecs[c].width);
      if (avail - fixed >= S(230) || level == 0) {
        int cx = x() + S(6);
        last_col_ = kName;
        for (int c = 0; c < kColumns; ++c) {
          bool on = c == kName || kSpecs[c].priority < level;
          col_w_[c] = !on ? 0 : c == kName ? avail - fixed : S(kSpecs[c].width);
          col_x_[c] = cx;
          cx += col_w_[c];
          if (on) last_col_ = c;
        }
        return;
      }
    }
  }

  std::shared_ptr<const Snapshot> snap_;
  std::vector<Row> rows_;
  int top_ = 0, hover_ = -1, selected_pid_ = 0, last_col_ = kState;
  bool dragging_ = false;
  int col_x_[kColumns] = {}, col_w_[kColumns] = {};
};

// ------------------------------------------------------------ the page

class ProcessesPage : public Page {
 public:
  ProcessesPage(int x, int y, int w, int h, Sampler &sampler)
      : Page(x, y, w, h, _("Processes")), sampler_(sampler) {
    int ty = y + header_height(), bh = S(36);
    search_ = new SearchField(x + S(28), ty, S(300), bh, _("Search by name, user, PID or command"));
    search_->input()->callback([](Fl_Widget *, void *d) { static_cast<ProcessesPage *>(d)->rebuild(); }, this);
    tree_ = new Chip(x + S(340), ty, S(120), bh, _("Tree view"));
    tree_->callback([](Fl_Widget *, void *d) { static_cast<ProcessesPage *>(d)->rebuild(); }, this);
    int bx = x + w - S(28);
    end_ = new sde::FlatButton(bx - S(120), ty, S(120), bh, _("End Task"), true);
    bx -= S(130);
    props_ = new sde::FlatButton(bx - S(112), ty, S(112), bh, _("Properties"));
    bx -= S(122);
    more_ = new sde::FlatButton(bx - S(104), ty, S(104), bh, _("Actions…"));
    end_->callback([](Fl_Widget *, void *d) { static_cast<ProcessesPage *>(d)->signal_selected(SIGTERM); }, this);
    props_->callback([](Fl_Widget *, void *d) { static_cast<ProcessesPage *>(d)->show_properties(); }, this);
    more_->callback([](Fl_Widget *w, void *d) {
      auto *self = static_cast<ProcessesPage *>(d);
      self->context_menu(w->x(), w->y() + w->h());
    }, this);
    table_ = new ProcessTable(x + S(28), ty + bh + S(14), w - S(56), h - header_height() - bh - S(14) - S(24));
    table_->on_select = [this](int) { update_buttons(); };
    table_->on_activate = [this](int) { show_properties(); };
    table_->on_menu = [this](int, int mx, int my) { context_menu(mx, my); };
    table_->on_sort = [this] { rebuild(); };
    table_->on_toggle_group = [this] { rebuild(); };
    resizable(table_);
    end();
    update_buttons();
    AppIndex::instance().load_async();
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    // Keep the toolbar buttons anchored to the right edge.
    int ty = Y + header_height(), bh = S(36), bx = X + W - S(28);
    end_->resize(bx - S(120), ty, S(120), bh);
    bx -= S(130);
    props_->resize(bx - S(112), ty, S(112), bh);
    bx -= S(122);
    more_->resize(bx - S(104), ty, S(104), bh);
    search_->resize(X + S(28), ty, S(300), bh);
    search_->input()->resize(X + S(28) + S(34), ty + S(5), S(300) - S(44), bh - S(10));
    tree_->resize(X + S(340), ty, S(120), bh);
    table_->resize(X + S(28), ty + bh + S(14), W - S(56), H - header_height() - bh - S(14) - S(24));
    subtitle_->resize(X + S(28), Y + S(48), W - S(56), S(22));
  }

  void update(const std::shared_ptr<const Snapshot> &snap, bool visible) override {
    if (!visible || !snap->has_procs) return;
    snap_ = snap;
    if (++window_scan_ % 2 == 1 || windows_.empty()) {
      windows_ = scan_windows();
      forget_window_icons(windows_);
    }
    rebuild();
    int threads = 0;
    for (auto &p : snap->procs) threads += p.threads;
    subtitle_->text(sde::format(_("%d processes · %d threads · CPU %s · Memory %s of %s"), int(snap->procs.size()),
                                threads, format_percent(snap->cpu_total).c_str(), format_bytes(snap->mem.used()).c_str(),
                                format_bytes(snap->mem.total).c_str()));
  }

  void activated() override { sampler_.poke(); }
  void focus_search() override { search_->input()->take_focus(); }

 private:
  bool matches(const ProcInfo &p, const std::string &name, const std::string &needle) const {
    if (needle.empty()) return true;
    return lower_copy(name).find(needle) != std::string::npos ||
           lower_copy(p.cmdline).find(needle) != std::string::npos ||
           lower_copy(p.user).find(needle) != std::string::npos || std::to_string(p.pid) == needle;
  }

  void rebuild() {
    if (!snap_) return;
    const auto &procs = snap_->procs;
    std::string needle = lower_copy(sde::trim(search_->value()));
    std::vector<Resolved> info(procs.size());
    std::map<int, int> index;  // pid -> proc index
    for (size_t i = 0; i < procs.size(); ++i) {
      auto w = windows_.find(procs[i].pid);
      info[i] = resolve_process(procs[i], w == windows_.end() ? nullptr : &w->second);
      index[procs[i].pid] = int(i);
    }
    std::map<int, std::vector<int>> children;
    for (size_t i = 0; i < procs.size(); ++i) children[procs[i].ppid].push_back(int(i));

    int col = table_->sort_column;
    bool desc = table_->sort_descending;
    auto less = [&](int a, int b) {
      const ProcInfo &pa = procs[a], &pb = procs[b];
      auto cmp = [&]() -> int {
        switch (col) {
          case kName: {
            std::string na = lower_copy(info[a].name), nb = lower_copy(info[b].name);
            return na < nb ? -1 : na > nb ? 1 : 0;
          }
          case kPid: return pa.pid < pb.pid ? -1 : pa.pid > pb.pid;
          case kUser: return pa.user < pb.user ? -1 : pa.user > pb.user;
          case kCpu: return pa.cpu < pb.cpu ? -1 : pa.cpu > pb.cpu;
          case kMemory: return pa.memory < pb.memory ? -1 : pa.memory > pb.memory;
          case kRead: return pa.read_rate < pb.read_rate ? -1 : pa.read_rate > pb.read_rate;
          case kWrite: return pa.write_rate < pb.write_rate ? -1 : pa.write_rate > pb.write_rate;
          case kThreads: return pa.threads < pb.threads ? -1 : pa.threads > pb.threads;
          case kState: return pa.state < pb.state ? -1 : pa.state > pb.state;
        }
        return 0;
      }();
      if (cmp == 0) return pa.pid < pb.pid;
      return desc ? cmp > 0 : cmp < 0;
    };
    auto make_row = [&](int i, int depth) {
      Row r;
      r.proc = i;
      r.depth = depth;
      r.app = info[i].app;
      r.name = info[i].name;
      r.icon = info[i].icon;
      r.window = info[i].window;
      return r;
    };

    std::vector<Row> rows;
    if (tree_->value() && needle.empty()) {
      std::vector<int> roots;
      for (size_t i = 0; i < procs.size(); ++i)
        if (!index.count(procs[i].ppid) || procs[i].ppid == procs[i].pid) roots.push_back(int(i));
      std::function<void(int, int)> walk = [&](int i, int depth) {
        rows.push_back(make_row(i, std::min(depth, 12)));
        auto it = children.find(procs[i].pid);
        if (it == children.end()) return;
        std::vector<int> kids = it->second;
        std::sort(kids.begin(), kids.end(), less);
        for (int k : kids)
          if (k != i) walk(k, depth + 1);
      };
      std::sort(roots.begin(), roots.end(), less);
      for (int r : roots) walk(r, 0);
    } else {
      // Apps first, each with the helper processes it started; the rest is
      // background work.
      std::vector<int> apps, background;
      std::vector<bool> used(procs.size(), false);
      for (size_t i = 0; i < procs.size(); ++i)
        if (info[i].app) apps.push_back(int(i)), used[i] = true;
      std::map<int, std::vector<int>> helpers;
      for (int a : apps) {
        std::vector<int> stack{a};
        while (!stack.empty()) {
          int cur = stack.back();
          stack.pop_back();
          auto it = children.find(procs[cur].pid);
          if (it == children.end()) continue;
          for (int k : it->second) {
            if (used[k]) continue;
            used[k] = true;
            helpers[a].push_back(k);
            stack.push_back(k);
          }
        }
      }
      for (size_t i = 0; i < procs.size(); ++i)
        if (!used[i]) background.push_back(int(i));
      std::sort(apps.begin(), apps.end(), less);
      std::sort(background.begin(), background.end(), less);
      auto group = [&](int id, const char *title, const std::vector<int> &members, bool with_helpers) {
        std::vector<Row> body;
        int count = 0;
        for (int m : members) {
          bool self = matches(procs[m], info[m].name, needle);
          std::vector<Row> sub;
          if (with_helpers) {
            auto h = helpers[m];
            std::sort(h.begin(), h.end(), less);
            for (int k : h)
              if (self || matches(procs[k], info[k].name, needle)) sub.push_back(make_row(k, 1));
          }
          if (!self && sub.empty()) continue;
          body.push_back(make_row(m, 0));
          count += 1 + int(sub.size());
          body.insert(body.end(), sub.begin(), sub.end());
        }
        if (body.empty()) return;
        Row header;
        header.group = id;
        header.name = title;
        header.count = count;
        rows.push_back(header);
        if (!table_->collapsed.count(id)) rows.insert(rows.end(), body.begin(), body.end());
      };
      if (tree_->value()) {
        // Tree view while searching: matching processes only, flat.
        std::vector<int> all;
        for (size_t i = 0; i < procs.size(); ++i)
          if (matches(procs[i], info[i].name, needle)) all.push_back(int(i));
        std::sort(all.begin(), all.end(), less);
        for (int i : all) rows.push_back(make_row(i, 0));
      } else {
        group(0, _("Apps"), apps, true);
        group(1, _("Background processes"), background, false);
      }
    }
    table_->set_model(snap_, std::move(rows));
    update_buttons();
  }

  void update_buttons() {
    bool any = table_->selected_proc() != nullptr;
    for (Fl_Widget *b : {(Fl_Widget *)end_, (Fl_Widget *)props_, (Fl_Widget *)more_}) {
      if (any && !b->active()) b->activate(), b->redraw();
      if (!any && b->active()) b->deactivate(), b->redraw();
    }
  }

  std::string display_name(const ProcInfo &p) {
    const Row *r = table_->selected_row();
    return r && r->proc >= 0 && snap_->procs[r->proc].pid == p.pid ? r->name : p.name;
  }

  void signal_selected(int sig) {
    const ProcInfo *p = table_->selected_proc();
    if (!p) return;
    std::string name = display_name(*p);
    int pid = p->pid;
    if (pid == getpid() && (sig == SIGSTOP)) return;  // would freeze this window
    if (sig == SIGKILL &&
        !fl_choice(_("Kill \"%s\" (PID %d) immediately?\nUnsaved work in it will be lost."), _("Cancel"),
                   _("Kill"), nullptr, name.c_str(), pid))
      return;
    if (kill(pid, sig) == 0) {
      sampler_.poke();
      return;
    }
    if (errno == ESRCH) {
      sampler_.poke();
      return;
    }
    if (errno != EPERM) return;
    // Another user's process: ask polkit through pkexec.
    std::string killer = sde::find_program("kill");
    if (killer.empty() || sde::find_program("pkexec").empty()) {
      fl_alert(_("\"%s\" belongs to %s. Administrator rights are required, but pkexec is not installed."),
               name.c_str(), p->user.c_str());
      return;
    }
    if (!fl_choice(_("\"%s\" belongs to %s.\nAuthenticate as an administrator to continue?"), _("Cancel"),
                   _("Authenticate"), nullptr, name.c_str(), p->user.c_str()))
      return;
    sde::run_async({"pkexec", killer, "-s", signal_name(sig), std::to_string(pid)},
                   [this](const std::string &, int) { sampler_.poke(); });
  }

  void renice_selected(int value) {
    const ProcInfo *p = table_->selected_proc();
    if (!p) return;
    int pid = p->pid;
    if (setpriority(PRIO_PROCESS, id_t(pid), value) == 0) {
      sampler_.poke();
      return;
    }
    if (errno != EPERM && errno != EACCES) return;
    std::string renice = sde::find_program("renice");
    if (renice.empty() || sde::find_program("pkexec").empty()) {
      fl_alert("%s", _("Raising the priority requires administrator rights, but pkexec is not installed."));
      return;
    }
    sde::run_async({"pkexec", renice, "-n", std::to_string(value), "-p", std::to_string(pid)},
                   [this](const std::string &, int) { sampler_.poke(); });
  }

  void open_location() {
    const ProcInfo *p = table_->selected_proc();
    if (!p) return;
    std::string exe = read_link("/proc/" + std::to_string(p->pid) + "/exe");
    if (exe.empty()) {
      // Other users' executables cannot be resolved; try the command path.
      std::string first = p->cmdline.substr(0, p->cmdline.find(' '));
      exe = first.find('/') == 0 ? first : sde::find_program(first);
    }
    if (exe.empty()) {
      fl_alert("%s", _("The location of this program is not available."));
      return;
    }
    if (sde::ends_with(exe, " (deleted)")) exe = exe.substr(0, exe.size() - 10);
    open_folder(exe.substr(0, std::max<size_t>(1, exe.rfind('/'))));
  }

  void show_properties() {
    const ProcInfo *p = table_->selected_proc();
    if (!p) return;
    auto *w = new PropertiesWindow(*p, display_name(*p), snap_->boot_time);
    w->position(window()->x() + (window()->w() - w->w()) / 2, window()->y() + (window()->h() - w->h()) / 2);
    w->show();
  }

  void context_menu(int mx, int my) {
    const ProcInfo *p = table_->selected_proc();
    if (!p) return;
    bool stopped = p->state == 'T';
    std::vector<Fl_Menu_Item> items;
    auto add = [&](const char *text, intptr_t id, int flags) {
      Fl_Menu_Item it;
      std::memset(&it, 0, sizeof it);
      it.text = text;
      it.user_data_ = (void *)id;
      it.flags = flags;
      it.labelsize_ = uchar(sde::active_theme().font_size);
      it.labelcolor_ = sde::active_theme().text.fl();
      items.push_back(it);
    };
    auto finish = [&] { add(nullptr, 0, 0); };
    auto radio = [&](bool on) { return FL_MENU_RADIO | (on ? FL_MENU_VALUE : 0); };
    add(_("End Task"), 1, 0);
    add(_("Kill"), 2, FL_MENU_DIVIDER);
    add(_("Stop"), 3, stopped ? FL_MENU_INACTIVE : 0);
    add(_("Continue"), 4, (stopped ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER);
    add(_("Change Priority"), 0, FL_SUBMENU | FL_MENU_DIVIDER);
    add(_("Very high (-20)"), 10, radio(p->nice <= -15));
    add(_("High (-5)"), 11, radio(p->nice > -15 && p->nice < 0));
    add(_("Normal (0)"), 12, radio(p->nice == 0));
    add(_("Low (5)"), 13, radio(p->nice > 0 && p->nice < 15));
    add(_("Very low (19)"), 14, radio(p->nice >= 15));
    finish();
    add(_("Open File Location"), 20, 0);
    add(_("Properties"), 21, 0);
    finish();
    const Fl_Menu_Item *m = items.data()->popup(mx, my);
    if (!m || !m->user_data()) return;
    switch (intptr_t(m->user_data())) {
      case 1: signal_selected(SIGTERM); break;
      case 2: signal_selected(SIGKILL); break;
      case 3: signal_selected(SIGSTOP); break;
      case 4: signal_selected(SIGCONT); break;
      case 10: renice_selected(-20); break;
      case 11: renice_selected(-5); break;
      case 12: renice_selected(0); break;
      case 13: renice_selected(5); break;
      case 14: renice_selected(19); break;
      case 20: open_location(); break;
      case 21: show_properties(); break;
    }
  }

  Sampler &sampler_;
  std::shared_ptr<const Snapshot> snap_;
  std::map<int, WindowOwner> windows_;
  int window_scan_ = 0;
  SearchField *search_;
  Chip *tree_;
  sde::FlatButton *end_, *props_, *more_;
  ProcessTable *table_;
};

Page *make_processes_page(int x, int y, int w, int h, Sampler &sampler) {
  return new ProcessesPage(x, y, w, h, sampler);
}

}  // namespace sysmon
