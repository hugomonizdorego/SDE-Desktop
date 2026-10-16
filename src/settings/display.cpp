// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Displays: resolution, main display and arrangement (xrandr).
#include <FL/Fl.H>
#include <FL/fl_ask.H>

#include <algorithm>

#include "buffered_window.h"
#include "i18n.h"
#include "paint.h"
#include "process.h"
#include "settings.h"
#include "system.h"
#include "ui.h"
#include "util.h"

namespace sde {

namespace {

enum Placement { RIGHT_OF, LEFT_OF, ABOVE, BELOW, MIRROR };

struct Pending {
  Output out;
  int placement = RIGHT_OF;
};

std::vector<Pending> g_pending;
std::vector<Output> g_current;

int mode_w(const std::string &m) { return atoi(m.c_str()); }
int mode_h(const std::string &m) {
  size_t x = m.find('x');
  return x == std::string::npos ? 0 : atoi(m.c_str() + x + 1);
}

void load_state() {
  g_current = query_outputs();
  g_pending.clear();
  const Output *primary = nullptr;
  for (auto &o : g_current)
    if (o.connected && o.primary) primary = &o;
  for (auto &o : g_current) {
    if (!o.connected) continue;
    Pending p;
    p.out = o;
    if (primary && &o != primary && o.enabled) {
      if (o.x == primary->x && o.y == primary->y) p.placement = MIRROR;
      else if (o.x >= primary->x + primary->width) p.placement = RIGHT_OF;
      else if (o.x + o.width <= primary->x) p.placement = LEFT_OF;
      else if (o.y + o.height <= primary->y) p.placement = ABOVE;
      else p.placement = BELOW;
    }
    g_pending.push_back(p);
  }
  // xrandr does not always report a primary output; use the first enabled.
  bool any_primary = false;
  for (auto &p : g_pending) any_primary = any_primary || p.out.primary;
  if (!any_primary)
    for (auto &p : g_pending)
      if (p.out.enabled) {
        p.out.primary = true;
        break;
      }
}

// Places the outputs: the main display at 0,0, the others around it.
std::vector<Output> layout() {
  std::vector<Output> out;
  Pending *main = nullptr;
  for (auto &p : g_pending)
    if (p.out.primary && p.out.enabled) main = &p;
  if (!main)
    for (auto &p : g_pending)
      if (p.out.enabled) main = &p;
  int mw = main ? mode_w(main->out.current_mode) : 0, mh = main ? mode_h(main->out.current_mode) : 0;
  int right = mw, left = 0, top = 0, bottom = mh;
  for (auto &p : g_pending) {
    Output o = p.out;
    if (&p == main) {
      o.x = o.y = 0;
    } else if (o.enabled) {
      int w = mode_w(o.current_mode), h = mode_h(o.current_mode);
      switch (p.placement) {
        case RIGHT_OF: o.x = right; o.y = 0; right += w; break;
        case LEFT_OF: left -= w; o.x = left; o.y = 0; break;
        case ABOVE: top -= h; o.x = 0; o.y = top; break;
        case BELOW: o.x = 0; o.y = bottom; bottom += h; break;
        default: o.x = 0; o.y = 0; break;
      }
      o.primary = false;
    }
    out.push_back(o);
  }
  // xrandr needs non-negative positions.
  int min_x = 0, min_y = 0;
  for (auto &o : out)
    if (o.enabled) {
      min_x = std::min(min_x, o.x);
      min_y = std::min(min_y, o.y);
    }
  for (auto &o : out) {
    o.x -= min_x;
    o.y -= min_y;
  }
  return out;
}

bool run_xrandr(const std::vector<Output> &outs) {
  std::vector<std::string> argv{"xrandr"};
  auto args = xrandr_args(outs);
  argv.insert(argv.end(), args.begin(), args.end());
  int status = -1;
  run_sync(argv, &status, 15000);
  return status == 0;
}

// "Keep these display settings?" with an automatic revert.
class ConfirmWindow : public BufferedWindow {
 public:
  ConfirmWindow() : BufferedWindow(S(440), S(170), _("Display Settings")) {
    keep_ = new FlatButton(w() - S(140), h() - S(54), S(120), S(36), _("Keep Changes"), true);
    revert_ = new FlatButton(w() - S(270), h() - S(54), S(120), S(36), _("Revert"));
    keep_->callback([](Fl_Widget *wdg, void *) {
      auto *self = static_cast<ConfirmWindow *>(wdg->window());
      self->result_ = true;
      self->hide();
    });
    revert_->callback([](Fl_Widget *wdg, void *) { wdg->window()->hide(); });
    end();
    set_modal();
  }
  void draw() override {
    const Theme &t = active_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
    }
    text_font(t.font_size + 3, true);
    draw_text(_("Keep these display settings?"), S(20), S(18), w() - S(40), S(30), t.text);
    text_font(t.font_size + 1);
    draw_text(format(_("The previous settings return in %d seconds."), seconds_), S(20), S(54),
              w() - S(40), S(26), t.text_dim);
    draw_children();
  }
  bool run() {
    int sx, sy, sw, sh;
    Fl::screen_xywh(sx, sy, sw, sh, 0);
    position(sx + (sw - w()) / 2, sy + (sh - h()) / 2);
    show();
    keep_->take_focus();
    Fl::add_timeout(1, tick, this);
    while (shown()) Fl::wait();
    Fl::remove_timeout(tick, this);
    return result_;
  }

 private:
  static void tick(void *d) {
    auto *self = static_cast<ConfirmWindow *>(d);
    if (--self->seconds_ <= 0) {
      self->hide();
      return;
    }
    self->redraw();
    Fl::repeat_timeout(1, tick, d);
  }
  FlatButton *keep_, *revert_;
  int seconds_ = 15;
  bool result_ = false;
};

void apply_layout() {
  std::vector<Output> previous = g_current;
  std::vector<Output> wanted = layout();
  bool any_enabled = false;
  for (auto &o : wanted) any_enabled = any_enabled || o.enabled;
  if (!any_enabled) {
    fl_alert("%s", _("At least one display must stay on."));
    return;
  }
  if (!run_xrandr(wanted)) {
    fl_alert("%s", _("The display settings could not be applied."));
    run_xrandr(previous);
    return;
  }
  Fl::check();
  ConfirmWindow confirm;
  if (confirm.run()) {
    save_display_layout(settings_config(), wanted);
  } else {
    run_xrandr(previous);
  }
  Fl::add_timeout(0.5, [](void *) { refresh_page(); });
}

}  // namespace

void build_display_page(Page *page) {
  load_state();
  page->heading(_("Displays"), _("Resolution, main display and arrangement"));
  if (g_pending.empty()) {
    page->begin_card();
    page->note(_("No displays were found. Display settings need the xrandr program "
                 "(package x11-xserver-utils)."));
    page->end_card();
    page->finish();
    return;
  }
  bool multi = g_pending.size() > 1;
  for (size_t i = 0; i < g_pending.size(); ++i) {
    Pending &p = g_pending[i];
    std::string title = p.out.name;
    if (p.out.primary) title += " · " + std::string(_("Main display"));
    page->begin_card(title);
    page->note(p.out.enabled ? format(_("%d x %d at position %d, %d"), p.out.width, p.out.height, p.out.x, p.out.y)
                             : std::string(_("Off")));
    if (multi)
      page->switch_row(_("Use this display"), p.out.enabled, [i](bool on) {
        g_pending[i].out.enabled = on;
        if (on && g_pending[i].out.current_mode.empty())
          g_pending[i].out.current_mode = g_pending[i].out.preferred_mode.empty()
                                              ? (g_pending[i].out.modes.empty() ? "" : g_pending[i].out.modes[0])
                                              : g_pending[i].out.preferred_mode;
      });
    std::vector<std::string> labels;
    int current = 0;
    for (size_t m = 0; m < p.out.modes.size(); ++m) {
      std::string label = p.out.modes[m];
      for (auto &c : label)
        if (c == 'x') c = ' ';
      size_t sp = label.find(' ');
      if (sp != std::string::npos) label = label.substr(0, sp) + " × " + label.substr(sp + 1);
      if (p.out.modes[m] == p.out.preferred_mode) label += " " + std::string(_("(recommended)"));
      labels.push_back(label);
      if (p.out.modes[m] == p.out.current_mode) current = int(m);
    }
    page->choice_row(_("Resolution"), labels, current, [i](int v) {
      if (v >= 0 && v < int(g_pending[i].out.modes.size())) g_pending[i].out.current_mode = g_pending[i].out.modes[size_t(v)];
    });
    if (multi) {
      page->switch_row(_("Main display"), p.out.primary, [i](bool on) {
        for (auto &q : g_pending) q.out.primary = false;
        g_pending[i].out.primary = on;
      }, _("The panel and new windows appear here."));
      if (!p.out.primary)
        page->choice_row(_("Position"),
                         {_("Right of the main display"), _("Left of the main display"),
                          _("Above the main display"), _("Below the main display"),
                          _("Same as the main display (mirror)")},
                         p.placement, [i](int v) { g_pending[i].placement = v; });
    }
    page->end_card();
  }
  int bw = S(150), bh = S(38);
  auto *apply = new FlatButton(page->left() + page->width() - bw, page->cursor(), bw, bh, _("Apply"), true);
  apply->callback([](Fl_Widget *, void *) { apply_layout(); });
  auto *detect = new FlatButton(page->left() + page->width() - 2 * bw - S(12), page->cursor(), bw, bh,
                                _("Detect Displays"));
  detect->callback([](Fl_Widget *, void *) { refresh_page(); });
  page->advance(bh + S(16));
  page->note(_("After you apply a change, it is kept only when you confirm it within 15 seconds."));
  page->finish();
}

}  // namespace sde
