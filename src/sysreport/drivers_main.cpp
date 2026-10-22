// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Driver Manager (sde-drivers): Debian's non-free firmware, proprietary
// and DKMS drivers for the devices of this computer. Since 0.1.0~alpha51 a
// single calm window after Linux Mint's Driver Manager, only more minimal:
// one row per device with its driver packages as radio choices and "Do not
// use the device", a status line and Revert / Apply Changes at the bottom.
// Applying runs one "pkexec apt-get install -y A B- ..." transaction (plus
// the software sources change when one is needed) with a slim progress
// line. The concept follows Linux Mint's Driver Manager; the code is SDE's
// own. Detection and the selection model live in drivers.{h,cpp}.
//
// QA: SDE_DRIVERS_DEMO=1 shows example devices (an NVIDIA card, a Realtek
// Wi-Fi card on rtw89-dkms, Intel Bluetooth); SDE_DRIVERS_DEMO=empty shows
// the empty state. Nothing is installed in demo mode.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "about.h"
#include "buffered_window.h"
#include "build_config.h"
#include "drivers.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "process.h"
#include "shell.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

using namespace sde;
using drivers::Device;
using drivers::DeviceKind;
using drivers::DriverOption;

namespace {

// ------------------------------------------------------------------ state

drivers::Scan g_scan;
bool g_scanned = false;
bool g_keyboard = false;  // the last input was a key: show focus rings
bool g_demo = false;
std::vector<int> g_initial, g_selection;

bool g_busy = false;
int g_stream = 0;
double g_progress = 0;     // 0-100 while busy
std::string g_progress_text;
std::deque<std::string> g_output;  // last lines of apt, for errors
int g_exit_status = -1;
std::string g_tmp_sources;  // rewritten sources file to install, removed after
// The message after the last apply ("" shows the proprietary driver count).
std::string g_message;
bool g_message_error = false;

const Theme &theme() { return shell::theme(); }

std::vector<std::string> icons_for(DeviceKind k) {
  switch (k) {
    case DeviceKind::Graphics: return {"video-display", "preferences-desktop-display", "computer"};
    case DeviceKind::Wifi: return {"network-wireless", "network-wireless-signal-excellent", "network-wireless-symbolic"};
    case DeviceKind::Bluetooth: return {"bluetooth", "preferences-system-bluetooth", "bluetooth-active", "network-wireless"};
    case DeviceKind::Network: return {"network-wired", "network-wired-symbolic", "network-wireless"};
    case DeviceKind::Audio: return {"audio-card", "audio-speakers", "multimedia-volume-control"};
    case DeviceKind::Processor: return {"cpu", "computer", "application-x-firmware"};
    default: return {"application-x-firmware", "drive-harddisk", "computer"};
  }
}

std::string state_text(drivers::DeviceState s) {
  switch (s) {
    case drivers::DeviceState::Recommended: return _("This device is using the recommended driver.");
    case drivers::DeviceState::Alternative: return _("This device is using an alternative driver.");
    default: return _("This device is using no additional driver.");
  }
}

std::string tag_text(const DriverOption &o) {
  std::string tag = o.free_software ? _("open-source") : _("proprietary");
  if (o.recommended) tag += std::string(", ") + _("recommended");
  return "(" + tag + ")";
}

bool selection_dirty() { return g_selection != g_initial; }

void demo_scan(bool empty) {
  g_scan = drivers::Scan();
  g_scan.non_free_firmware = g_scan.contrib_non_free = true;
  g_scan.sources_file = "/etc/apt/sources.list.d/debian.sources";
  if (empty) return;
  std::vector<drivers::Recommendation> recs = {
      {"nvidia-driver", "NVIDIA GeForce RTX 3060 Mobile / Max-Q", "", "non-free", false, true},
      {"firmware-nvidia-graphics", "NVIDIA GeForce RTX 3060 Mobile / Max-Q", "", "non-free-firmware", true, false},
      {"firmware-realtek", "Realtek Wi-Fi", "", "non-free-firmware", false, true},
      {"rtw89-dkms", "Realtek Wi-Fi", "", "main", true, false},
      {"firmware-iwlwifi", "Intel Bluetooth", "", "non-free-firmware", true, true},
  };
  std::vector<drivers::PciDevice> pci(1);
  pci[0].class_id = "028000";
  pci[0].vendor_id = "10ec";
  pci[0].vendor = "Realtek Semiconductor Co., Ltd.";
  pci[0].model = "RTL8852BE PCIe 802.11ax Wireless Network Controller";
  std::map<std::string, drivers::AptVersions> v = {
      {"nvidia-driver", {"", "535.247.01-1"}},
      {"firmware-nvidia-graphics", {"20250410-1", "20250410-1"}},
      {"firmware-realtek", {"", "20250410-1"}},
      {"rtw89-dkms", {"7.3+klbkresolute", "7.3+klbkresolute"}},
      {"firmware-iwlwifi", {"20250410-1", "20250410-1"}},
  };
  std::map<std::string, std::string> s = {
      {"nvidia-driver", "NVIDIA metapackage"},
      {"firmware-nvidia-graphics", "Binary firmware for Nvidia GPUs (used by nouveau)"},
      {"firmware-realtek", "Binary firmware for Realtek wired/wifi/BT adapters"},
      {"rtw89-dkms", "rtw89 DKMS modules for Debian"},
      {"firmware-iwlwifi", "Binary firmware for Intel Wireless cards"},
  };
  g_scan.items = recs;
  g_scan.devices = drivers::group_devices(recs, pci, v, s);
}

void do_scan() {
  const char *demo = getenv("SDE_DRIVERS_DEMO");
  g_demo = demo && *demo;
  if (g_demo) demo_scan(std::string(demo) == "empty");
  else g_scan = drivers::scan_system();
  g_initial = drivers::initial_selection(g_scan.devices);
  g_selection = g_initial;
  g_scanned = true;
}

// Archive areas the chosen packages need that the sources lack.
std::vector<std::string> missing_components(const drivers::Changes &c) {
  bool need_firmware = false, need_nonfree = false;
  for (const auto &p : c.install)
    for (const auto &r : g_scan.items)
      if (r.package == p) {
        if (r.component == "non-free-firmware") need_firmware = true;
        if (r.component == "non-free" || r.component == "contrib") need_nonfree = true;
      }
  std::vector<std::string> out;
  if (need_firmware && !g_scan.non_free_firmware) out.push_back("non-free-firmware");
  if (need_nonfree && !g_scan.contrib_non_free) {
    out.push_back("contrib");
    out.push_back("non-free");
  }
  return out;
}

// ------------------------------------------------------------- the window

class DriversWindow;
DriversWindow *g_win = nullptr;
void selection_changed();

int header_h() { return S(40); }
int bar_h() { return S(60); }

// One device: icon, name, state and its radio choices.
class DeviceRow : public Fl_Widget {
 public:
  DeviceRow(int x, int y, int w, size_t index) : Fl_Widget(x, y, w, measure(index)), index_(index) {}

  static int name_h() { return S(48); }
  static int option_h(const DriverOption &) { return S(64); }
  static int none_h() { return S(30); }
  static int measure(size_t index) {
    const Device &d = g_scan.devices[index];
    int h = name_h();
    for (auto &o : d.options) h += option_h(o);
    return h + none_h();
  }
  // Number of choices (options + "Do not use the device").
  int choices() const { return int(g_scan.devices[index_].options.size()) + 1; }
  // Choice index (0..n) <-> selection value (option or kNoDriver).
  int choice_of(int sel) const { return sel == drivers::kNoDriver ? choices() - 1 : sel; }
  int sel_of(int choice) const { return choice == choices() - 1 ? drivers::kNoDriver : choice; }

  int choice_y(int choice) const {
    const Device &d = g_scan.devices[index_];
    int yy = y() + name_h();
    for (int i = 0; i < choice && i < int(d.options.size()); ++i) yy += option_h(d.options[size_t(i)]);
    return yy;
  }
  int choice_at(int ey) const {
    for (int c = 0; c < choices(); ++c) {
      int top = choice_y(c);
      int h = c == choices() - 1 ? none_h() : option_h(g_scan.devices[index_].options[size_t(c)]);
      if (ey >= top && ey < top + h) return c;
    }
    return -1;
  }

  void draw() override {
    const Theme &t = theme();
    const Device &d = g_scan.devices[index_];
    int sel = choice_of(g_selection[index_]);
    int rx = x() + S(64);  // radio column
    bool focused = Fl::focus() == this && g_keyboard;  // the ring is for keyboard users
    {
      Painter p(window()->w(), window()->h());
      p.image(IconLoader::get(icons_for(d.kind), S(40)), x() + S(4), y() + S(2), S(40),
              g_busy ? 0.5 : 1.0);
      for (int c = 0; c < choices(); ++c) {
        double cy = choice_y(c) + S(11);
        bool on = c == sel;
        Color ring = on ? t.accent : t.text_dim.mix(t.background, 0.35);
        if (g_busy) ring = ring.alpha(0.5);
        p.circle(rx + S(8), cy, Sd(8), ring);
        if (!on) p.circle(rx + S(8), cy, Sd(6.5), t.surface);
        else p.circle(rx + S(8), cy, Sd(3.2), t.on_accent);
        if (focused && c == focus_) {
          int h = c == choices() - 1 ? none_h() : option_h(d.options[size_t(c)]);
          focus_ring(p, rx - S(6), choice_y(c) - S(4), w() - (rx - x()) + S(2), h - S(2), Sd(8));
        }
      }
    }
    Color text = g_busy ? t.text_dim : t.text;
    int tx = x() + S(56);
    text_font(t.font_size + 1);
    draw_text(d.name, tx, y(), w() - (tx - x()), S(24), text);
    text_font(t.font_size - 1);
    draw_text(state_text(drivers::device_state(d, g_initial[index_])), tx, y() + S(22), w() - (tx - x()),
              S(18), t.text_dim);
    int lx = rx + S(26), lw = w() - (lx - x());
    Color tag = t.accent_text.mix(t.text_dim, 0.4);
    for (size_t i = 0; i < d.options.size(); ++i) {
      const DriverOption &o = d.options[i];
      int oy = choice_y(int(i));
      text_font(t.font_size, true);
      draw_text(o.package, lx, oy, lw, S(22), text);
      int pw = std::min(lw, text_width(o.package));
      text_font(t.font_size - 1, true);
      draw_text(tag_text(o), lx + pw + S(6), oy + S(1), std::max(0, lw - pw - S(6)), S(22), tag);
      text_font(t.font_size - 1);
      std::string ver = !o.version.empty() ? format(_("Version %s"), o.version.c_str())
                        : o.installed      ? _("Installed")
                                           : _("Not available from the enabled software sources");
      draw_text(ver, lx, oy + S(21), lw, S(18), o.version.empty() && !o.installed ? t.warning.mix(t.text, 0.4) : text);
      draw_text(o.summary, lx, oy + S(38), lw, S(18), t.text_dim);
    }
    text_font(t.font_size);
    draw_text(_("Do not use the device"), lx, choice_y(choices() - 1), lw, S(22), text);
  }

  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        if (g_busy) return 1;
        int c = choice_at(Fl::event_y());
        if (c < 0) return 0;
        if (Fl::visible_focus()) take_focus();
        choose(c);
        return 1;
      }
      case FL_FOCUS:
        g_keyboard = Fl::event() == FL_KEYBOARD;
        focus_ = choice_of(g_selection[index_]);
        redraw();
        return 1;
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD: {
        if (g_busy) return 0;
        int k = Fl::event_key();
        if (k == FL_Up || k == FL_Down) {
          int n = focus_ + (k == FL_Down ? 1 : -1);
          if (n < 0 || n >= choices()) return 0;  // let FLTK move to the next row
          focus_ = n;
          choose(n);
          return 1;
        }
        if (k == ' ') {
          choose(focus_);
          return 1;
        }
        return 0;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  void choose(int c) {
    focus_ = c;
    int sel = sel_of(c);
    if (g_selection[index_] != sel) {
      g_selection[index_] = sel;
      g_message.clear();
      selection_changed();
    }
    redraw();
  }

  size_t index_;
  int focus_ = 0;
};

// The notice above the devices when Debian's non-free areas are off.
class SourcesNotice : public Fl_Widget {
 public:
  SourcesNotice(int x, int y, int w, int h, std::string text) : Fl_Widget(x, y, w, h), text_(std::move(text)) {}
  void draw() override {
    const Theme &t = theme();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), Sd(8), t.surface_alt);
      p.stroke_rounded(x(), y(), w(), h(), Sd(8), t.border);
      p.circle(x() + S(18), y() + h() / 2.0, Sd(4), t.warning);
    }
    text_font(t.font_size - 1);
    fl_color(t.text.fl());
    fl_draw(text_.c_str(), x() + S(32), y(), w() - S(32) - S(110), h(),
            Fl_Align(FL_ALIGN_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  }

 private:
  std::string text_;
};

void apply_changes();
void revert_changes();

class DriversWindow : public BufferedWindow {
 public:
  DriversWindow(int w, int h, const char *title) : BufferedWindow(w, h, title) {
    end();
    begin();
    revert_ = new FlatButton(0, 0, S(96), S(34), _("Revert"));
    apply_ = new FlatButton(0, 0, S(132), S(34), _("Apply Changes"), true);
    revert_->callback([](Fl_Widget *, void *) { revert_changes(); });
    apply_->callback([](Fl_Widget *, void *) { apply_changes(); });
    end();
    resizable(nullptr);
    place_buttons();
    update_buttons();
  }

  void resize(int X, int Y, int W, int H) override {
    bool sized = W != w() || H != h();
    BufferedWindow::resize(X, Y, W, H);
    if (sized) {
      place_buttons();
      rebuild();
    }
  }

  void place_buttons() {
    int by = h() - bar_h() + (bar_h() - S(34)) / 2;
    apply_->resize(w() - S(16) - S(132), by, S(132), S(34));
    revert_->resize(apply_->x() - S(8) - S(96), by, S(96), S(34));
  }

  void update_buttons() {
    bool on = g_scanned && !g_busy && selection_dirty();
    if (on) {
      revert_->activate();
      apply_->activate();
    } else {
      revert_->deactivate();
      apply_->deactivate();
    }
    redraw();
  }

  // (Re)creates the device list.
  void rebuild() {
    int scroll = 0;
    if (page_) {
      scroll = page_->yposition();
      remove(page_);
      Fl::delete_widget(page_);
      page_ = nullptr;
    }
    if (g_scanned && !g_scan.devices.empty()) {
      begin();
      page_ = new Page(0, header_h(), w(), h() - header_h() - bar_h());
      int x = S(20), width = w() - S(40) - page_->scrollbar_size();
      int y = page_->y() + S(4);
      drivers::Changes all;
      for (size_t i = 0; i < g_scan.devices.size(); ++i)
        for (auto &o : g_scan.devices[i].options)
          if (!o.installed) all.install.push_back(o.package);
      std::vector<std::string> comps = missing_components(all);
      if (!comps.empty()) {
        std::string list;
        for (auto &c : comps) list += (list.empty() ? "" : ", ") + c;
        std::string text = format(_("Some drivers come from Debian's %s area, which is not enabled in "
                                    "%s. Apply Changes enables it first."),
                                  list.c_str(), g_scan.sources_file.c_str());
        text_font(theme().font_size - 1);
        int nh = std::max(S(44), Label::measure(text, width - S(150), Label::DIM) + S(16));
        new SourcesNotice(x, y, width, nh, text);
        y += nh + S(18);
      }
      for (size_t i = 0; i < g_scan.devices.size(); ++i) {
        auto *row = new DeviceRow(x, y, width, i);
        y += row->h() + S(20);
      }
      if (!g_scan.kernel_log_readable) {
        std::string note = _("The kernel log could not be read, so missing firmware may not be listed. "
                             "Members of the \"adm\" group can read it.");
        int nh = Label::measure(note, width, Label::DIM) + S(4);
        new Label(x, y, width, nh, note, Label::DIM, true);
        y += nh;
      }
      auto *spacer = new Fl_Box(x, y, 1, S(8));
      spacer->box(FL_NO_BOX);
      page_->end();
      end();
      if (scroll) page_->scroll_to(0, std::min(scroll, std::max(0, y - page_->y() - page_->h())));
    }
    update_buttons();
    redraw();
  }

  void draw() override {
    const Theme &t = theme();
    int bar_y = h() - bar_h();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      // Separator above the bottom bar; while applying it is the progress line.
      p.fill_rect(S(16), bar_y, w() - S(32), std::max(1, S(1)), t.border);
      if (g_busy) {
        double pw = (w() - S(32)) * std::max(0.02, std::min(1.0, g_progress / 100.0));
        p.fill_rounded(S(16), bar_y - Sd(1), pw, Sd(3), Sd(1.5), t.accent);
      }
      if (g_scanned && g_scan.devices.empty()) {
        p.image(IconLoader::get({"emblem-ok-symbolic", "emblem-ok", "object-select"}, S(40)),
                (w() - S(40)) / 2.0, header_h() + (bar_y - header_h()) / 2.0 - S(64), S(40), 0.55);
      }
    }
    text_font(t.font_size - 1);
    draw_text(_("Additional and proprietary drivers"), S(16), S(8), w() - S(32), S(24), t.text_dim, 1);
    if (!g_scanned) {
      text_font(t.font_size + 1);
      draw_text(_("Looking for drivers..."), 0, header_h(), w(), bar_y - header_h(), t.text_dim, 1);
    } else if (g_scan.devices.empty()) {
      int cy = header_h() + (bar_y - header_h()) / 2;
      text_font(t.font_size + 3);
      draw_text(_("No additional drivers are needed."), S(16), cy - S(14), w() - S(32), S(28), t.text, 1);
      text_font(t.font_size - 1);
      draw_text(_("Every device of this computer works with the free drivers of Debian."), S(16), cy + S(16),
                w() - S(32), S(20), t.text_dim, 1);
    }
    // Status on the left of the bottom bar.
    std::string status;
    Color sc = t.text;
    if (g_busy) {
      status = g_progress_text.empty() ? _("Applying changes...") : g_progress_text;
      sc = t.text_dim;
    } else if (!g_message.empty()) {
      status = g_message;
      sc = g_message_error ? t.error.mix(t.text, 0.3) : t.text;
    } else if (g_scanned) {
      int n = drivers::proprietary_in_use(g_scan.devices);
      status = n == 0 ? std::string(_("No proprietary drivers are in use."))
                      : format(ngettext("%d proprietary driver in use.", "%d proprietary drivers in use.", n), n);
    }
    text_font(t.font_size);
    draw_text(status, S(16), bar_y, revert_->x() - S(28), bar_h(), sc);
    draw_children();
  }

  int handle(int e) override {
    if (e == FL_PUSH) g_keyboard = false;
    if (e == FL_KEYBOARD && Fl::event_key() == FL_Escape) {
      do_callback();
      return 1;
    }
    int r = BufferedWindow::handle(e);
    if (!r && handle_about_event(e, about)) return 1;
    return r;
  }

  AppInfo about;

 private:
  Page *page_ = nullptr;
  FlatButton *revert_ = nullptr, *apply_ = nullptr;
};

void selection_changed() {
  if (g_win) g_win->update_buttons();
}

void revert_changes() {
  g_selection = g_initial;
  g_message.clear();
  if (g_win) g_win->rebuild();
}

void finish_apply() {
  g_busy = false;
  g_stream = 0;
  if (!g_tmp_sources.empty()) {
    unlink(g_tmp_sources.c_str());
    g_tmp_sources.clear();
  }
  int status = g_exit_status;
  if (status == 0) {
    g_message = _("Changes applied. Restart the computer to use the new drivers.");
    g_message_error = false;
  } else if (status < 0 || status == 126 || status == 127) {
    g_message = _("Nothing was changed: the administrator password was not given.");
    g_message_error = false;
  } else {
    g_message = format(_("Applying the changes failed (status %d)."), status);
    g_message_error = true;
    std::string tail;
    for (auto &l : g_output) tail += l + "\n";
    fl_alert(_("Installing or removing the drivers failed (status %d).\n\n%s"), status, tail.c_str());
  }
  if (status == 0 && !g_demo) {
    std::string keep = g_message;
    do_scan();
    g_message = keep;
  } else if (status == 0) {
    g_initial = g_selection;  // demo: pretend it worked
  }
  if (g_win) g_win->rebuild();
}

void demo_tick(void *) {
  bool hold = getenv("SDE_DRIVERS_DEMO_HOLD") != nullptr;
  g_progress = hold ? 42 : g_progress + 12;
  g_progress_text = format("%s  %d%%", "Unpacking nvidia-driver (535.247.01-1)", int(g_progress));
  if (g_win) g_win->redraw();
  if (g_progress >= 100) {
    g_exit_status = 0;
    finish_apply();
  } else if (!hold) {
    Fl::repeat_timeout(0.3, demo_tick);
  }
}

void apply_changes() {
  drivers::Changes c = drivers::plan_changes(g_scan.devices, g_initial, g_selection);
  if (c.empty() || g_busy) return;
  std::vector<std::string> comps = missing_components(c);
  std::string sources_path, sources_tmp;
  if (!comps.empty() && !g_demo) {
    bool deb822 = false;
    sources_path = drivers::main_sources_file(&deb822);
    std::string text;
    if (!read_file(sources_path, text)) {
      fl_alert(_("Cannot read %s."), sources_path.c_str());
      return;
    }
    std::string updated = drivers::enable_components(text, deb822, comps);
    if (updated == text) {
      fl_message(_("%s has no Debian entry to change. Enable the non-free areas in a terminal, for "
                   "example with \"sudo apt edit-sources\"."),
                 sources_path.c_str());
      return;
    }
    // Keep a copy of the current file; the new one is installed by root.
    std::string backup_dir = state_home() + "/sde/apt-backup";
    make_dirs(backup_dir);
    char stamp[32];
    time_t now = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    write_file_atomic(join_path(backup_dir, basename_of(sources_path) + "." + stamp), text);
    sources_tmp = join_path(runtime_dir(), "sde-drivers-" + std::to_string(getpid()) + "-" + basename_of(sources_path));
    if (!write_file_atomic(sources_tmp, updated)) {
      fl_alert(_("Cannot write %s."), sources_tmp.c_str());
      return;
    }
  }
  g_busy = true;
  g_progress = 0;
  g_progress_text = sources_tmp.empty() ? _("Waiting for the administrator password...")
                                        : _("Enabling the software sources...");
  g_output.clear();
  g_exit_status = -1;
  g_message.clear();
  g_tmp_sources = sources_tmp;
  if (g_win) g_win->rebuild();

  if (g_demo) {
    // Fake progress for screenshots (SDE_DRIVERS_DEMO_HOLD stops at 42 %).
    Fl::add_timeout(0.3, demo_tick);
    return;
  }

  if (find_program("pkexec").empty()) {
    g_busy = false;
    fl_alert("%s", _("pkexec (polkit) is not installed, so SDE cannot ask for the administrator "
                     "password. Run apt-get in a terminal with sudo instead."));
    if (g_win) g_win->rebuild();
    return;
  }
  // One authorization: the sources change (when needed) and one apt
  // transaction; "pkg-" removes pkg. The paths and package names are
  // arguments, never pasted into the script.
  static const char *script =
      "exec 2>&1\n"
      "src=\"$1\"; dst=\"$2\"; shift 2\n"
      "rc=0\n"
      "if [ -n \"$src\" ]; then install -m 0644 -- \"$src\" \"$dst\" && apt-get update || rc=$?; fi\n"
      "if [ $rc -eq 0 ]; then DEBIAN_FRONTEND=noninteractive apt-get -o APT::Status-Fd=1 "
      "-o Dpkg::Use-Pty=0 \"$@\" || rc=$?; fi\n"
      "echo \"sde-status:$rc\"\n";
  std::vector<std::string> argv = {"pkexec", "/bin/sh", "-c", script, "sh", sources_tmp, sources_path};
  for (auto &a : drivers::apt_arguments(c)) argv.push_back(a);
  g_stream = stream_lines(
      argv,
      [](const std::string &line) {
        double pct;
        std::string text;
        if (starts_with(line, "sde-status:")) {
          g_exit_status = atoi(line.c_str() + 11);
          return;
        }
        if (drivers::parse_apt_status(line, &pct, &text)) {
          g_progress = std::max(g_progress, pct);
          g_progress_text = format("%s  %d%%", text.c_str(), int(g_progress));
        } else if (!trim(line).empty()) {
          g_output.push_back(line);
          if (g_output.size() > 30) g_output.pop_front();
          if (g_progress <= 0) g_progress_text = _("Refreshing the package lists...");
        }
        if (g_win) g_win->damage_rect(0, g_win->h() - bar_h() - S(2), g_win->w(), bar_h() + S(2));
      },
      [] { Fl::add_timeout(0, [](void *) { finish_apply(); }); });
  if (!g_stream) {
    g_exit_status = 127;
    finish_apply();
  }
}

AppInfo app_info() {
  AppInfo a;
  a.name = _("Driver Manager");
  a.program = "sde-drivers";
  a.version = SDE_VERSION;
  a.icon = "jockey";
  a.description = _("Finds hardware that needs non-free drivers or firmware and installs Debian's "
                    "packages for it.");
  a.credits = {_("Concept after Linux Mint's Driver Manager (no code used)"), toolkit_credit()};
  return a;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") {
      printf("sde-drivers %s (%s) - %s\n", SDE_VERSION, SDE_VERSION_LABEL, kDeveloper);
      return 0;
    }
    if (a == "--list") {
      // Text report for terminals and bug reports.
      drivers::Scan s = drivers::scan_system();
      for (auto &g : s.gpus) printf("graphics: %s\n", g.c_str());
      printf("sources: %s (non-free-firmware %s)\n", s.sources_file.c_str(),
             s.non_free_firmware ? "enabled" : "disabled");
      for (auto &r : s.items)
        printf("%s\t%s\t%s\t%s\n", r.package.c_str(), r.installed ? "installed" : "recommended",
               r.component.c_str(), r.device.c_str());
      auto initial = drivers::initial_selection(s.devices);
      for (size_t i = 0; i < s.devices.size(); ++i) {
        const Device &d = s.devices[i];
        printf("device: %s\n", d.name.c_str());
        for (size_t j = 0; j < d.options.size(); ++j) {
          const DriverOption &o = d.options[j];
          printf("  %s %s %s%s %s\n", int(j) == initial[i] ? "(*)" : "( )", o.package.c_str(),
                 o.free_software ? "open-source" : "proprietary", o.recommended ? ",recommended" : "",
                 o.version.c_str());
        }
        printf("  %s do-not-use\n", initial[i] == drivers::kNoDriver ? "(*)" : "( )");
      }
      if (!s.kernel_log_readable) printf("note: the kernel log could not be read\n");
      return 0;
    }
    printf("Usage: sde-drivers [--list] [--version]\n\n"
           "  --list     print the devices and driver packages instead of opening the window\n");
    return a == "--help" ? 0 : 2;
  }
  shell::init();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int W = std::min(S(680), sw - S(16)), H = std::min(S(540), sh - S(40));
  g_win = new DriversWindow(W, H, _("Driver Manager"));
  g_win->about = app_info();
  g_win->xclass("sde-drivers");
  g_win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  g_win->size_range(std::min(W, S(440)), std::min(H, S(320)));
  g_win->resizable(g_win);
  g_win->callback([](Fl_Widget *w, void *) {
    if (g_busy) {
      fl_message("%s", _("Please wait until the changes are applied."));
      return;
    }
    w->hide();
  });
  g_win->show();
  Fl::add_timeout(0.05, [](void *) {
    do_scan();
    g_win->rebuild();
  });
  return Fl::run();
}
