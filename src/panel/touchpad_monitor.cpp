// SPDX-License-Identifier: GPL-3.0-or-later
#include "touchpad_monitor.h"

#include <FL/Fl.H>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "panel.h"
#include "process.h"
#include "x11.h"

using namespace sde;

namespace {
long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

TouchpadMonitor &TouchpadMonitor::get() {
  static TouchpadMonitor m;
  return m;
}

long long TouchpadMonitor::last_activity() const {
  return std::max(state_.moved_at, std::max(state_.click_at, state_.scroll_at));
}

void TouchpadMonitor::start(std::function<void()> activity, std::function<void()> changed) {
  activity_ = std::move(activity);
  changed_ = std::move(changed);
  display_ = XOpenDisplay(nullptr);
  if (!display_) return;
  int event, error, major = 2, minor = 2;
  if (!XQueryExtension(display_, "XInputExtension", &opcode_, &event, &error) ||
      XIQueryVersion(display_, &major, &minor) != Success) {
    XCloseDisplay(display_);
    display_ = nullptr;
    return;
  }
  detect();
  select();
  Fl::add_fd(ConnectionNumber(display_), FL_READ, input, this);
}

// The first libinput touchpad among the slave pointers.
void TouchpadMonitor::detect() {
  int old = id_;
  bool old_enabled = enabled_;
  id_ = 0;
  name_.clear();
  props_.clear();
  scroll_h_ = scroll_v_ = -1;
  Atom tapping = XInternAtom(display_, "libinput Tapping Enabled", True);
  Atom enabled = XInternAtom(display_, "Device Enabled", True);
  if (tapping != None) {
    int count = 0;
    XIDeviceInfo *devices = XIQueryDevice(display_, XIAllDevices, &count);
    for (int i = 0; devices && i < count && !id_; ++i) {
      const XIDeviceInfo &d = devices[i];
      if (d.use != XISlavePointer && d.use != XIFloatingSlave) continue;
      int n = 0;
      Atom *props = XIListProperties(display_, d.deviceid, &n);
      bool touchpad = props && std::find(props, props + n, tapping) != props + n;
      if (touchpad) {
        id_ = d.deviceid;
        name_ = d.name ? d.name : "";
        for (int k = 0; k < n; ++k)
          if (char *pn = XGetAtomName(display_, props[k])) {
            props_.push_back(pn);
            XFree(pn);
          }
        for (int c = 0; c < d.num_classes; ++c)
          if (d.classes[c]->type == XIScrollClass) {
            auto *s = reinterpret_cast<XIScrollClassInfo *>(d.classes[c]);
            (s->scroll_type == XIScrollTypeVertical ? scroll_v_ : scroll_h_) = s->number;
          }
        enabled_ = true;
        if (enabled != None) {
          Atom type;
          int format;
          unsigned long items = 0, after = 0;
          unsigned char *data = nullptr;
          if (XIGetProperty(display_, id_, enabled, 0, 1, False, AnyPropertyType, &type, &format,
                            &items, &after, &data) == Success &&
              data) {
            if (items && format == 8) enabled_ = data[0] != 0;
            XFree(data);
          }
        }
      }
      if (props) XFree(props);
    }
    if (devices) XIFreeDeviceInfo(devices);
  }
  // UI tests (panel-smoke): SDE_PANEL_FAKE_TOUCHPAD=1 lets the XTEST pointer
  // (xdotool) stand in for a touchpad, so the indicator can be exercised.
  const char *fake = getenv("SDE_PANEL_FAKE_TOUCHPAD");
  if (!id_ && fake && *fake == '1') {
    int count = 0;
    XIDeviceInfo *devices = XIQueryDevice(display_, XIAllDevices, &count);
    for (int i = 0; devices && i < count && !id_; ++i)
      if (devices[i].use == XISlavePointer && devices[i].name &&
          strstr(devices[i].name, "XTEST")) {
        id_ = devices[i].deviceid;
        name_ = "QA touchpad (XTEST)";
        props_ = {"Device Enabled", "libinput Tapping Enabled",
                  "libinput Natural Scrolling Enabled", "libinput Accel Speed"};
      }
    if (devices) XIFreeDeviceInfo(devices);
  }
  if ((old != id_ || old_enabled != enabled_) && changed_) changed_();
}

void TouchpadMonitor::want_motion(bool on) {
  if (motion_ == on) return;
  motion_ = on;
  select();
}

void TouchpadMonitor::select() {
  if (!display_) return;
  Window root = DefaultRootWindow(display_);
  // Device changes and property changes (enabled/disabled elsewhere).
  {
    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(bits, XI_HierarchyChanged);
    XISetMask(bits, XI_PropertyEvent);
    XIEventMask mask{XIAllDevices, int(sizeof bits), bits};
    XISelectEvents(display_, root, &mask, 1);
  }
  if (id_) {
    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(bits, XI_RawButtonPress);
    if (motion_) XISetMask(bits, XI_RawMotion);
    XIEventMask mask{id_, int(sizeof bits), bits};
    XISelectEvents(display_, root, &mask, 1);
    motion_selected_ = motion_;
  }
  XFlush(display_);
}

void TouchpadMonitor::input(int, void *data) {
  auto *self = static_cast<TouchpadMonitor *>(data);
  bool redetect = false;
  while (self->display_ && XPending(self->display_)) {
    XEvent e;
    XNextEvent(self->display_, &e);
    if (e.type != GenericEvent || e.xcookie.extension != self->opcode_) continue;
    if (!XGetEventData(self->display_, &e.xcookie)) continue;
    int type = e.xcookie.evtype;
    if (type == XI_HierarchyChanged)
      redetect = true;
    else if (type == XI_PropertyEvent) {
      auto *p = static_cast<XIPropertyEvent *>(e.xcookie.data);
      if (p->deviceid == self->id_) redetect = true;
    } else if (type == XI_RawButtonPress || type == XI_RawMotion) {
      self->handle_raw(type, e.xcookie.data);
    }
    XFreeEventData(self->display_, &e.xcookie);
  }
  if (redetect) {
    int before = self->id_;
    self->detect();
    if (before != self->id_) self->select();
  }
}

void TouchpadMonitor::handle_raw(int type, void *data) {
  auto *raw = static_cast<XIRawEvent *>(data);
  if (raw->deviceid != id_ && raw->sourceid != id_) return;
  long long now = now_ms();
  if (type == XI_RawButtonPress) {
    int b = raw->detail;
    if (b >= 4 && b <= 7) {
      // Emulated wheel buttons (two-finger or edge scrolling).
      state_.scroll_dx = b == 6 ? -1 : b == 7 ? 1 : 0;
      state_.scroll_dy = b == 4 ? -1 : b == 5 ? 1 : 0;
      state_.scroll_at = now;
    } else if (b >= 1 && b <= 3) {
      state_.click = b;
      state_.click_at = now;
    }
  } else {
    double *v = raw->raw_values;
    double dx = 0, dy = 0, sx = 0, sy = 0;
    for (int i = 0; i < raw->valuators.mask_len * 8; ++i) {
      if (!XIMaskIsSet(raw->valuators.mask, i)) continue;
      double value = *v++;
      if (i == scroll_h_)
        sx += value;
      else if (i == scroll_v_)
        sy += value;
      else if (i == 0)
        dx += value;
      else if (i == 1)
        dy += value;
    }
    if (sx != 0 || sy != 0) {
      state_.scroll_dx = sx > 0 ? 1 : sx < 0 ? -1 : 0;
      state_.scroll_dy = sy > 0 ? 1 : sy < 0 ? -1 : 0;
      state_.scroll_at = now;
    }
    if (dx != 0 || dy != 0) {
      // About one touchpad width per 1200 units of unaccelerated motion.
      state_.x = std::max(0.08, std::min(0.92, state_.x + dx / 1200.0));
      state_.y = std::max(0.1, std::min(0.9, state_.y + dy / 900.0));
      state_.moved_at = now;
    }
  }
  if (activity_) activity_();
}

void TouchpadMonitor::apply(const TouchpadOptions &o) {
  if (Panel::instance) {
    Config cfg = Panel::instance->config();
    save_touchpad_options(cfg, o);
    std::vector<std::pair<std::string, std::string>> keys;
    for (const char *key : {"enabled", "tap_drag", "scroll", "click"})
      keys.push_back({"touchpad", key});
    for (const char *key : {"touchpad_tap", "touchpad_natural_scroll",
                            "touchpad_disable_while_typing", "touchpad_speed"})
      keys.push_back({"input", key});
    std::vector<Panel::Setting> list;
    for (const auto &k : keys) list.push_back({k.first, k.second, cfg.get(k.first, k.second)});
    Panel::instance->set_settings(list);
  }
  if (!id_) return;
  for (const auto &prop : touchpad_props(o, props_)) {
    std::vector<std::string> argv{"xinput", "set-prop", std::to_string(id_), prop.first};
    argv.insert(argv.end(), prop.second.begin(), prop.second.end());
    run_async(argv, nullptr);
  }
  if (std::find(props_.begin(), props_.end(), "Device Enabled") != props_.end()) {
    enabled_ = o.enabled;
    if (changed_) changed_();
  }
}

void TouchpadMonitor::set_enabled(bool on) {
  TouchpadOptions o =
      Panel::instance ? load_touchpad_options(Panel::instance->config()) : TouchpadOptions();
  o.enabled = on;
  apply(o);
}

void draw_touchpad(Painter &p, double x, double y, double w, double h,
                   const TouchpadMonitor::State &s, bool enabled, bool live, const Color &ink,
                   const Color &accent) {
  long long now = now_ms();
  double lw = std::max(1.0, std::min(w, h) / 12);
  double r = std::min(w, h) * 0.18;
  double button = h * 0.72;  // where the button area starts
  p.stroke_rounded(x + lw / 2, y + lw / 2, w - lw, h - lw, r, ink, lw);
  // Click flash: the left or right half of the button area (both: middle).
  if (live && s.click && now - s.click_at < 350) {
    double a = 1.0 - double(now - s.click_at) / 350.0;
    Color c = accent.alpha(0.25 + 0.55 * a);
    double bx = x + lw, by = y + button, bw = (w - 2 * lw) / 2, bh = h - button - lw;
    cairo_save(p.cr());
    p.rounded_rect(x + lw, y + lw, w - 2 * lw, h - 2 * lw, r * 0.8);
    cairo_clip(p.cr());
    if (s.click == 1 || s.click == 2) p.fill_rect(bx, by, bw, bh, c);
    if (s.click == 3 || s.click == 2) p.fill_rect(bx + bw, by, bw, bh, c);
    cairo_restore(p.cr());
  }
  p.line(x + lw, y + button, x + w - lw, y + button, ink.alpha(0.6), lw * 0.8);
  p.line(x + w / 2, y + button, x + w / 2, y + h - lw, ink.alpha(0.6), lw * 0.8);
  if (!enabled) {
    p.line(x + w * 0.1, y + h * 0.9, x + w * 0.9, y + h * 0.1, ink, lw * 1.2);
    return;
  }
  if (!live) return;
  // Two-finger scrolling: arrows for 600 ms.
  if (now - s.scroll_at < 600 && (s.scroll_dx || s.scroll_dy)) {
    double a = 1.0 - double(now - s.scroll_at) / 600.0;
    Color c = accent.alpha(0.35 + 0.65 * a);
    double cx = x + w / 2, cy = y + button / 2, len = std::min(w, button) * 0.32;
    double dx = s.scroll_dx, dy = s.scroll_dy;
    for (double off : {-0.18, 0.18}) {
      double ox = dy ? w * off : 0, oy = dx ? button * off : 0;
      double x1 = cx + ox - dx * len / 2, y1 = cy + oy - dy * len / 2;
      double x2 = cx + ox + dx * len / 2, y2 = cy + oy + dy * len / 2;
      p.line(x1, y1, x2, y2, c, lw);
      double hx = -dx * len * 0.35, hy = -dy * len * 0.35;
      p.line(x2, y2, x2 + hx - hy * 0.7, y2 + hy + hx * 0.7, c, lw);
      p.line(x2, y2, x2 + hx + hy * 0.7, y2 + hy - hx * 0.7, c, lw);
    }
  }
  // The finger: a dot that fades out 2 s after the last movement.
  if (now - s.moved_at < 2000) {
    double a = now - s.moved_at < 1200 ? 1.0 : 1.0 - double(now - s.moved_at - 1200) / 800.0;
    double fx = x + lw + (w - 2 * lw) * s.x, fy = y + lw + (button - 2 * lw) * s.y;
    p.circle(fx, fy, std::max(1.5, std::min(w, h) * 0.13), accent.alpha(0.25 * a));
    p.circle(fx, fy, std::max(1.0, std::min(w, h) * 0.075), accent.alpha(a));
  }
}
