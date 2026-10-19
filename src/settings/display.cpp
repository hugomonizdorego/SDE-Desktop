// SPDX-License-Identifier: GPL-3.0-or-later
// Native RandR frontend, using the MIT DisplayProfileManager profile model port.
#include <FL/Fl.H>
#include <FL/Fl_Int_Input.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <cmath>

#include "buffered_window.h"
#include "build_config.h"
#include "i18n.h"
#include "paint.h"
#include "settings.h"
#include "spawn.h"
#include "system.h"
#include "ui.h"
#include "util.h"
namespace sde {
namespace {
std::vector<Output> current, pending;
std::string topology, notice;
int selected = 0, scale = 100, profile_index = 0;
bool loaded = false, applying = false;
void ensure_primary() {
  bool found = false;
  for (auto& o : pending) {
    if (!o.enabled || found) o.primary = false;
    if (o.enabled && o.primary) found = true;
  }
  if (!found)
    for (auto& o : pending)
      if (o.enabled) {
        o.primary = true;
        break;
      }
}
void load_state(bool detect = false) {
  current = query_outputs(detect);
  topology = display_topology(current);
  pending.clear();
  for (const auto& o : current)
    if (o.connected) pending.push_back(o);
  selected = std::min(selected, std::max(0, int(pending.size()) - 1));
  scale = settings_config().get_int("appearance", "scale", 100);
  if (scale != 125 && scale != 150 && scale != 175 && scale != 200) scale = 100;
  ensure_primary();
  loaded = true;
}
void refresh_later() {
  Fl::add_timeout(0, [](void*) { refresh_page(); });
}
void choose_mode(Output& o, const std::string& mode) {
  o.current_mode = mode;
  o.rate = 0;
  for (const auto& m : o.available)
    if (m.name == mode) {
      o.rate = m.rate;
      break;
    }
}
class MonitorMap : public Fl_Widget {
 public:
  MonitorMap(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  struct Rect {
    int x, y, w, h;
    size_t index;
  };
  std::vector<Rect> rectangles() {
    int maxx = 1, maxy = 1;
    for (const auto& o : pending)
      if (o.enabled) {
        maxx = std::max(maxx, o.x + output_width(o));
        maxy = std::max(maxy, o.y + output_height(o));
      }
    factor_ = std::min(double(w() - S(50)) / maxx, double(h() - S(40)) / maxy);
    int left = x() + (w() - int(maxx * factor_)) / 2, top = y() + (h() - int(maxy * factor_)) / 2;
    std::vector<Rect> rects;
    for (size_t i = 0; i < pending.size(); ++i) {
      const auto& o = pending[i];
      if (!o.enabled) continue;
      rects.push_back({left + int(o.x * factor_), top + int(o.y * factor_),
                       std::max(S(36), int(output_width(o) * factor_)),
                       std::max(S(28), int(output_height(o) * factor_)), i});
    }
    return rects;
  }
  void draw() override {
    const auto& t = active_theme();
    auto rects = rectangles();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(5), t.surface_alt);
      for (const auto& r : rects) {
        p.fill_rounded(r.x + S(3), r.y + S(3), r.w - S(6), r.h - S(6), S(4),
                       int(r.index) == selected ? t.accent.alpha(.22) : t.background);
        p.stroke_rounded(r.x + S(3), r.y + S(3), r.w - S(6), r.h - S(6), S(4),
                         int(r.index) == selected ? t.accent : t.border, S(2));
      }
    }
    for (const auto& r : rects) {
      text_font(settings_theme().font_size + 6, true);
      draw_text(std::to_string(r.index + 1) + (pending[r.index].primary ? " *" : ""), r.x,
                r.y + r.h / 2 - S(23), r.w, S(26), t.text, 1);
      text_font(settings_theme().font_size - 1);
      draw_text(pending[r.index].name, r.x + S(4), r.y + r.h / 2 + S(3), r.w - S(8), S(22),
                t.text_dim, 1);
    }
  }
  int handle(int event) override {
    if (event == FL_PUSH) {
      auto rects = rectangles();
      for (auto i = rects.rbegin(); i != rects.rend(); ++i)
        if (Fl::event_x() >= i->x && Fl::event_x() < i->x + i->w && Fl::event_y() >= i->y &&
            Fl::event_y() < i->y + i->h) {
          selected = int(i->index);
          start_x_ = Fl::event_x();
          start_y_ = Fl::event_y();
          old_x_ = pending[i->index].x;
          old_y_ = pending[i->index].y;
          drag_factor_ = factor_;
          dragging_ = true;
          return 1;
        }
      return 1;
    }
    if (event == FL_DRAG && dragging_) {
      auto& o = pending[size_t(selected)];
      o.x = std::max(0, std::min(30000, old_x_ + int((Fl::event_x() - start_x_) / drag_factor_)));
      o.y = std::max(0, std::min(30000, old_y_ + int((Fl::event_y() - start_y_) / drag_factor_)));
      for (size_t i = 0; i < pending.size(); ++i)
        if (int(i) != selected && pending[i].enabled) {
          const auto& q = pending[i];
          for (int edge : {q.x, q.x + output_width(q), q.x - output_width(o)})
            if (std::abs(o.x - edge) < 32) o.x = std::max(0, edge);
          for (int edge : {q.y, q.y + output_height(q), q.y - output_height(o)})
            if (std::abs(o.y - edge) < 32) o.y = std::max(0, edge);
        }
      redraw();
      return 1;
    }
    if (event == FL_RELEASE && dragging_) {
      dragging_ = false;
      refresh_later();
      return 1;
    }
    return Fl_Widget::handle(event);
  }

 private:
  double factor_ = 1, drag_factor_ = 1;
  bool dragging_ = false;
  int start_x_ = 0, start_y_ = 0, old_x_ = 0, old_y_ = 0;
};
class IdentifyWindow : public BufferedWindow {
 public:
  IdentifyWindow(int x, int y, int number, const std::string& name)
      : BufferedWindow(x, y, S(240), S(110), "Identify Display"), number_(number), name_(name) {
    end();
    border(0);
  }
  void draw() override {
    const auto& t = active_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.accent);
    }
    text_font(t.font_size + 20, true);
    draw_text(std::to_string(number_), 0, S(8), w(), S(52), t.on_accent, 1);
    text_font(t.font_size + 1);
    draw_text(name_, S(8), S(66), w() - S(16), S(30), t.on_accent, 1);
  }

 private:
  int number_;
  std::string name_;
};
void identify() {
  int number = 0;
  for (const auto& o : current)
    if (o.connected) {
      ++number;
      if (!o.enabled) continue;
      auto* w = new IdentifyWindow(o.x + std::max(0, (o.width - S(240)) / 2),
                                   o.y + std::max(0, (o.height - S(110)) / 2), number,
                                   o.readable_name.empty() ? o.name : o.readable_name);
      w->show();
      Fl::add_timeout(
          3,
          [](void* d) {
            auto* w = static_cast<IdentifyWindow*>(d);
            w->hide();
            Fl::delete_widget(w);
          },
          w);
    }
}
class ConfirmWindow : public BufferedWindow {
 public:
  ConfirmWindow() : BufferedWindow(S(440), S(180), _("Keep display settings?")) {
    auto* revert = new FlatButton(S(160), S(124), S(120), S(36), _("Revert"));
    auto* keep = new FlatButton(S(292), S(124), S(128), S(36), _("Keep Changes"), true);
    revert->callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    keep->callback([](Fl_Widget* w, void*) {
      auto* self = static_cast<ConfirmWindow*>(w->window());
      self->result_ = true;
      self->hide();
    });
    end();
    set_modal();
  }
  void draw() override {
    const auto& t = active_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
    }
    text_font(t.font_size + 3, true);
    draw_text(_("Keep these display settings?"), S(20), S(18), w() - S(40), S(30), t.text);
    text_font(t.font_size);
    draw_text(format(_("Reverting in %d seconds"), seconds_), S(20), S(64), w() - S(40), S(26),
              t.text_dim);
    draw_children();
  }
  bool run() {
    int x, y, w, h;
    Fl::screen_xywh(x, y, w, h, 0);
    position(x + (w - this->w()) / 2, y + (h - this->h()) / 2);
    show();
    Fl::add_timeout(1, tick, this);
    while (shown()) Fl::wait();
    Fl::remove_timeout(tick, this);
    return result_;
  }

 private:
  static void tick(void* d) {
    auto* self = static_cast<ConfirmWindow*>(d);
    if (--self->seconds_ <= 0) {
      self->hide();
      return;
    }
    self->redraw();
    Fl::repeat_timeout(1, tick, d);
  }
  bool result_ = false;
  int seconds_ = 15;
};
void apply_layout() {
  ensure_primary();
  auto fresh = query_outputs(true);
  std::string error = validate_display_layout(pending, fresh);
  if (!error.empty()) {
    fl_alert("%s", _(error.c_str()));
    return;
  }
  applying = true;
  bool primary = false;
  for (const auto& o : fresh) primary |= o.connected && o.enabled && o.primary;
  if (!primary)
    for (auto& o : fresh)
      if (o.connected && o.enabled) {
        o.primary = true;
        break;
      }
  DisplayPreview preview(fresh);
  if (!preview.ready()) {
    applying = false;
    fl_alert("%s", _("Could not start the display rollback worker."));
    return;
  }
  if (!run_display_layout(pending)) {
    preview.revert();
    applying = false;
    load_state();
    refresh_later();
    fl_alert("%s", _("The graphics driver rejected this layout. Previous settings were restored."));
    return;
  }
  Fl::check();
  ConfirmWindow confirm;
  if (confirm.run() && preview.keep()) {
    settings_config().set("appearance", "scale", std::to_string(scale));
    save_display_layout(settings_config(), pending);
    apply_change();
    notice = _("Display settings saved. GTK applications may need to be reopened to resize fully.");
  } else {
    preview.revert();
    notice = _("Previous display settings restored.");
  }
  applying = false;
  load_state();
  refresh_later();
}
void preset(int mode) {
  if (pending.empty()) return;
  auto before = pending;
  size_t main = 0;
  for (size_t i = 0; i < pending.size(); ++i)
    if (pending[i].primary) main = i;
  if (mode == 2 || mode == 3) {
    auto internal = [](const Output& o) {
      return starts_with(o.name, "eDP") || starts_with(o.name, "LVDS") ||
             starts_with(o.name, "DSI");
    };
    for (auto& o : pending) o.enabled = mode == 2 ? internal(o) : !internal(o);
    if (!std::any_of(pending.begin(), pending.end(), [](const Output& o) { return o.enabled; })) {
      pending = before;
      fl_alert("%s", _("No display matches this layout."));
      return;
    }
  } else
    for (auto& o : pending) o.enabled = true;
  if (mode == 1) {
    std::string common;
    for (const auto& m : pending[main].modes)
      if (std::all_of(pending.begin(), pending.end(), [&](const Output& o) {
            return std::find(o.modes.begin(), o.modes.end(), m) != o.modes.end();
          })) {
        common = m;
        break;
      }
    if (common.empty()) {
      pending = before;
      fl_alert("%s", _("These displays do not share a resolution for mirroring."));
      return;
    }
    for (auto& o : pending) {
      choose_mode(o, common);
      o.rotation = "normal";
      o.x = o.y = 0;
    }
  } else {
    int x = 0;
    for (auto& o : pending)
      if (o.enabled) {
        if (o.current_mode.empty())
          choose_mode(o, o.preferred_mode.empty() ? (o.modes.empty() ? "" : o.modes.front())
                                                  : o.preferred_mode);
        o.x = x;
        o.y = 0;
        x += output_width(o);
      }
  }
  ensure_primary();
  refresh_later();
}
void watch_displays(void*) {
  if (loaded && !applying) {
    auto fresh = query_outputs();
    if (!fresh.empty() && display_topology(fresh) != topology) {
      load_state();
      notice =
          _("Display connection or mode changed. The current hardware layout has been reloaded.");
      refresh_page();
    }
  }
  Fl::repeat_timeout(2, watch_displays);
}
void number_row(Page* page, const std::string& label, int value, std::function<void(int)> changed) {
  page->row(label, S(130), S(32), [value, changed](int x, int y, int w, int h) -> Fl_Widget* {
    auto* input = new Fl_Int_Input(x, y, w, h);
    input->value(std::to_string(value).c_str());
    input->textsize(settings_theme().font_size);
    input->when(FL_WHEN_CHANGED);
    input->callback(
        [](Fl_Widget* w, void* d) {
          long n = strtol(static_cast<Fl_Int_Input*>(w)->value(), nullptr, 10);
          (*static_cast<std::function<void(int)>*>(d))(int(std::max(0L, std::min(30000L, n))));
        },
        keep(new std::function<void(int)>(changed)));
    return input;
  });
}
}  // namespace
void start_display_watch() {
  Fl::remove_timeout(watch_displays);
  Fl::add_timeout(2, watch_displays);
}
void build_display_page(Page* page) {
  if (!loaded) load_state();
  page->heading(_("Display Settings"), _("Arrange monitors, resolution and display profiles"));
  int bw = (page->width() - S(20)) / 3;
  auto* detect = new FlatButton(page->left(), page->cursor(), bw, S(36), _("Detect Displays"));
  detect->callback([](Fl_Widget*, void*) {
    load_state(true);
    notice = _("Display hardware refreshed.");
    refresh_later();
  });
  auto* id = new FlatButton(page->left() + bw + S(10), page->cursor(), bw, S(36), _("Identify"));
  id->callback([](Fl_Widget*, void*) { identify(); });
  auto* apply =
      new FlatButton(page->left() + 2 * (bw + S(10)), page->cursor(), bw, S(36), _("Apply"), true);
  apply->callback([](Fl_Widget*, void*) { apply_layout(); });
  page->advance(S(48));
  if (!notice.empty()) page->note(notice);
  if (pending.empty()) {
    page->note(
        _("No connected displays were reported by XRandR. Check the graphics driver and cable, "
          "then detect displays."));
    page->finish();
    return;
  }
  new MonitorMap(page->left(), page->cursor(), page->width(), S(180));
  page->advance(S(192));
  page->note(
      _("Select or drag a display to arrange it. * marks the main display. Changes are tested for "
        "15 seconds before they are saved."));
  page->begin_card(_("Monitor layout"));
  page->choice_row(_("Quick layout"),
                   {_("Custom"), _("Extend to the right"), _("Mirror displays"),
                    _("Built-in display only"), _("External displays only")},
                   0, [](int v) {
                     if (v > 0) preset(v - 1);
                   });
  std::vector<std::string> names;
  for (size_t i = 0; i < pending.size(); ++i) {
    const auto& o = pending[i];
    names.push_back(std::to_string(i + 1) + " · " +
                    (o.readable_name.empty() ? o.name : o.readable_name + " (" + o.name + ")"));
  }
  page->choice_row(_("Selected display"), names, selected, [](int v) {
    selected = v;
    refresh_later();
  });
  page->end_card();
  auto& o = pending[size_t(selected)];
  page->begin_card(names[size_t(selected)]);
  std::string detail = o.name + " · " +
                       (o.identity.empty() ? _("Connector identity (EDID unavailable)")
                                           : _("EDID hardware identity"));
  if (o.mm_width > 0 && o.mm_height > 0)
    detail += " · " + std::to_string(o.mm_width) + " × " + std::to_string(o.mm_height) + " mm";
  page->note(detail);
  page->switch_row(_("Use this display"), o.enabled, [](bool on) {
    auto& o = pending[size_t(selected)];
    o.enabled = on;
    if (on && o.current_mode.empty())
      choose_mode(o, o.preferred_mode.empty() ? (o.modes.empty() ? "" : o.modes.front())
                                              : o.preferred_mode);
    ensure_primary();
    refresh_later();
  });
  if (o.enabled) {
    page->switch_row(
        _("Main display"), o.primary,
        [](bool on) {
          if (on) {
            for (auto& q : pending) q.primary = false;
            pending[size_t(selected)].primary = true;
          }
          ensure_primary();
          refresh_later();
        },
        _("The panel and new windows appear here."));
    std::vector<std::string> labels;
    int chosen = 0;
    for (size_t i = 0; i < o.modes.size(); ++i) {
      labels.push_back(o.modes[i] + (o.modes[i] == o.preferred_mode
                                         ? " " + std::string(_("(recommended)"))
                                         : ""));
      if (o.modes[i] == o.current_mode) chosen = int(i);
    }
    page->choice_row(_("Resolution"), labels, chosen, [](int v) {
      auto& o = pending[size_t(selected)];
      if (v >= 0 && v < int(o.modes.size())) choose_mode(o, o.modes[size_t(v)]);
      refresh_later();
    });
    std::vector<double> rates;
    labels.clear();
    chosen = 0;
    for (const auto& m : o.available)
      if (m.name == o.current_mode && std::none_of(rates.begin(), rates.end(), [&](double r) {
            return std::fabs(r - m.rate) < .02;
          })) {
        if (std::fabs(m.rate - o.rate) < .02) chosen = int(rates.size());
        rates.push_back(m.rate);
        labels.push_back(m.rate > 0 ? format("%.2f Hz", m.rate) : _("Driver default"));
      }
    page->choice_row(_("Refresh rate"), labels, chosen, [rates](int v) {
      if (v >= 0 && v < int(rates.size())) pending[size_t(selected)].rate = rates[size_t(v)];
    });
    labels.clear();
    chosen = 0;
    for (size_t i = 0; i < o.rotations.size(); ++i) {
      const auto& r = o.rotations[i];
      labels.push_back(r == "normal"  ? _("Landscape")
                       : r == "left"  ? _("Portrait left")
                       : r == "right" ? _("Portrait right")
                                      : _("Landscape inverted"));
      if (r == o.rotation) chosen = int(i);
    }
    page->choice_row(_("Orientation"), labels, chosen, [](int v) {
      auto& o = pending[size_t(selected)];
      if (v >= 0 && v < int(o.rotations.size())) o.rotation = o.rotations[size_t(v)];
      refresh_later();
    });
    number_row(page, _("Horizontal position (px)"), o.x,
               [](int v) { pending[size_t(selected)].x = v; });
    number_row(page, _("Vertical position (px)"), o.y,
               [](int v) { pending[size_t(selected)].y = v; });
  }
  page->end_card();
  page->begin_card(_("Interface scale"));
  std::vector<int> scales = {100, 125, 150, 175, 200};
  int chosen = 0;
  for (size_t i = 0; i < scales.size(); ++i)
    if (scales[i] == scale) chosen = int(i);
  page->choice_row(
      _("Text and interface size"), {"100%", "125%", "150%", "175%", "200%"}, chosen,
      [scales](int v) {
        if (v >= 0 && v < int(scales.size())) scale = scales[size_t(v)];
      },
      _("Shared across all X11 displays. Pixel resolution stays unchanged. Applies to SDE and GTK "
        "text; reopen applications if needed."));
  page->end_card();
  page->begin_card(_("Display profiles"));
  auto profiles = display_profiles();
  if (profile_index >= int(profiles.size())) profile_index = 0;
  page->choice_row(_("Saved profile"), profiles, profile_index, [](int v) { profile_index = v; });
  page->row(_("Save this draft"), S(150), S(34), [](int x, int y, int w, int h) -> Fl_Widget* {
    auto* b = new FlatButton(x, y, w, h, _("Save Profile"));
    b->callback([](Fl_Widget*, void*) {
      const char* name = fl_input("%s", _("New profile"), _("Profile name"));
      if (!name || trim(name).empty()) return;
      std::string unique = unique_profile_name(trim(name), display_profiles());
      if (!validate_display_layout(pending, query_outputs()).empty()) {
        fl_alert("%s", _("Choose a valid display layout before saving a profile."));
        return;
      }
      if (!save_display_profile({unique, scale, pending}))
        fl_alert("%s", _("The profile could not be saved."));
      else {
        notice = _("Profile saved. Use Apply to test and activate it.");
        refresh_later();
      }
    });
    return b;
  });
  if (!profiles.empty()) {
    page->row(
        _("Load selected profile"), S(150), S(34),
        [profiles](int x, int y, int w, int h) -> Fl_Widget* {
          auto* b = new FlatButton(x, y, w, h, _("Load"));
          b->callback(
              [](Fl_Widget*, void* d) {
                auto* list = static_cast<std::vector<std::string>*>(d);
                DisplayProfile p;
                if (profile_index < 0 || profile_index >= int(list->size()) ||
                    !load_display_profile((*list)[size_t(profile_index)], p, query_outputs())) {
                  fl_alert("%s", _("This profile does not match the connected hardware or its "
                                   "supported modes."));
                  return;
                }
                pending = p.outputs;
                scale = p.scale;
                notice = _("Profile loaded as a draft. Apply to test it.");
                refresh_later();
              },
              keep(new std::vector<std::string>(profiles)));
          return b;
        });
    page->row(
        _("Remove selected profile"), S(150), S(34),
        [profiles](int x, int y, int w, int h) -> Fl_Widget* {
          auto* b = new FlatButton(x, y, w, h, _("Delete"));
          b->callback(
              [](Fl_Widget*, void* d) {
                auto* list = static_cast<std::vector<std::string>*>(d);
                if (profile_index < 0 || profile_index >= int(list->size())) return;
                const auto& name = (*list)[size_t(profile_index)];
                if (fl_choice("%s", _("Cancel"), _("Delete"), nullptr,
                              (std::string(_("Delete profile")) + " “" + name + "”?").c_str()) ==
                    1) {
                  delete_display_profile(name);
                  refresh_later();
                }
              },
              keep(new std::vector<std::string>(profiles)));
          return b;
        });
  }
  page->end_card();
  page->begin_card(_("About Display Settings"));
  page->note("Display Settings " + std::string(SDE_DISPLAY_VERSION) + " · SDE Alpha");
  page->note("SDE developer: Hugo Moniz do Rego");
  page->row(_("Developer"), S(180), S(34), [](int x, int y, int w, int h) -> Fl_Widget* {
    auto* b = new FlatButton(x, y, w, h, "Hugo Moniz do Rego");
    b->callback([](Fl_Widget*, void*) { open_uri("https://github.com/hugomonizdorego"); });
    return b;
  });
  page->row(_("Profile model source"), S(180), S(34), [](int x, int y, int w, int h) -> Fl_Widget* {
    auto* b = new FlatButton(x, y, w, h, "DisplayProfileManager");
    b->callback(
        [](Fl_Widget*, void*) { open_uri("https://github.com/zac15987/DisplayProfileManager"); });
    return b;
  });
  page->note(
      _("Copyright (c) 2025 zac15987 · MIT. Native C++ profile port and Linux XRandR integration "
        "by SDE contributors. Windows HDR, audio profiles and hotkeys are not part of this Linux "
        "alpha."));
  page->end_card();
  page->finish();
}
}  // namespace sde
