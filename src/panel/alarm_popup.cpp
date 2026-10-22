// SPDX-License-Identifier: GPL-3.0-or-later
#include "alarm_popup.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Scroll.H>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>

#include "alarms.h"
#include "clockface.h"
#include "glyphs.h"
#include "i18n.h"
#include "paint.h"
#include "panel.h"
#include "panel_widgets.h"
#include "popup_style.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

using namespace sde;
using agenda::Alarm;

namespace {

using IconFn = std::function<void(Painter &, double, double, double, const Color &)>;

// [clock] format (alpha52; the older [panel] clock_24h still counts).
bool clock_24h() { return clockface::use_24h(Panel::instance->config()); }

std::string time_text(int hour, int minute) {
  char buf[24];
  if (clock_24h())
    snprintf(buf, sizeof buf, "%02d:%02d", hour, minute);
  else
    snprintf(buf, sizeof buf, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute,
             hour < 12 ? "AM" : "PM");
  return buf;
}

// Stable names (tooltips) of an alarm's controls, for keyboard focus and tests.
std::string alarm_name(const Alarm &a) {
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", a.hour, a.minute);
  return buf;
}

std::string day_name(int index, bool abbreviated) {  // 0 Monday
  struct tm t {};
  t.tm_wday = (index + 1) % 7;
  char buf[32];
  strftime(buf, sizeof buf, abbreviated ? "%a" : "%A", &t);
  return buf;
}

std::string repeat_text(unsigned days) {
  if (days == 0) return _("Once");
  if (days == agenda::EVERY_DAY) return _("Every day");
  if (days == agenda::WEEKDAYS) return _("Weekdays");
  if (days == agenda::WEEKEND) return _("Weekends");
  std::string out;
  for (int i = 0; i < 7; ++i)
    if (days & (1u << i)) out += (out.empty() ? "" : ", ") + day_name(i, true);
  return out;
}

std::string until_text(long long when) {
  long long left = std::max(0LL, when - (long long)time(nullptr));
  int minutes = int((left + 59) / 60);
  if (minutes < 60) return format(_("in %d min"), minutes);
  if (minutes < 24 * 60) return format(_("in %d h %d min"), minutes / 60, minutes % 60);
  return format(_("in %d days"), minutes / (24 * 60));
}

class Label : public Fl_Widget {
 public:
  Label(int x, int y, int w, int h, std::function<std::string()> text, int style = 0)
      : Fl_Widget(x, y, w, h), text_(std::move(text)), style_(style) {}
  void draw() override {
    const Theme &t = Panel::instance->theme();
    int size = t.font_size + (style_ == 3 ? 8 : style_ == 4 ? 14 : 0);
    text_font(size, style_ == 1 || style_ >= 3);
    draw_text(text_(), x(), y(), w(), h(), style_ == 2 ? t.text_dim : t.text, style_ == 5 ? 1 : 0);
  }

 private:
  std::function<std::string()> text_;
  int style_;  // 0 text, 1 title, 2 dim, 3 large, 4 huge, 5 centered
};

class Card : public Fl_Group {
 public:
  Card(int x, int y, int w, bool accent = false) : Fl_Group(x, y, w, S(40)), accent_(accent) {
    box(FL_NO_BOX);
    resizable(nullptr);
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    {
      Painter p(window()->w(), window()->h());
      Color bg = accent_ ? t.surface_alt.mix(t.accent, t.dark ? 0.3 : 0.16) : t.surface_alt;
      p.fill_rounded(x(), y(), w(), h(), S(8), bg);
      if (t.glass || accent_)
        p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(8), accent_ ? t.accent : t.border);
    }
    draw_children();
  }

 private:
  bool accent_;
};

class FnButton : public FlatButton {
 public:
  FnButton(int x, int y, int w, int h, const std::string &text, std::function<void()> fn,
           bool primary = false)
      : FlatButton(x, y, w, h, text, primary), fn_(std::move(fn)) {
    copy_tooltip(text.c_str());
    callback([](Fl_Widget *w, void *) {
      auto fn = static_cast<FnButton *>(w)->fn_;  // the popup may rebuild
      fn();
    });
  }

 private:
  std::function<void()> fn_;
};

class IconButton : public DrawnButton {
 public:
  IconButton(int x, int y, int w, int h, const std::string &name, IconFn icon,
             std::function<void()> fn, std::function<bool()> on = nullptr)
      : DrawnButton(x, y, w, h, nullptr), icon_(std::move(icon)), fn_(std::move(fn)),
        on_(std::move(on)) {
    copy_tooltip(name.c_str());
    callback([](Fl_Widget *w, void *) {
      auto fn = static_cast<IconButton *>(w)->fn_;
      fn();
    });
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    bool hot = Fl::belowmouse() == this, on = on_ && on_();
    Painter p(window()->w(), window()->h());
    Color bg = on ? t.accent : hot ? t.hover : t.surface_alt;
    if (value()) bg = bg.mix(t.text, 0.15);
    double r = std::min(w(), h()) / 2.0;
    p.fill_rounded(x(), y(), w(), h(), r, bg);
    if (!on) p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border);
    double s = std::min(w(), h()) * 0.55;
    icon_(p, x() + (w() - s) / 2, y() + (h() - s) / 2, s, on ? t.on_accent : t.text);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }

 private:
  IconFn icon_;
  std::function<void()> fn_;
  std::function<bool()> on_;
};

IconFn arrow_icon(bool up) {
  return [up](Painter &p, double x, double y, double s, const Color &c) {
    double a = up ? .65 : .35, b = up ? .35 : .65;
    p.line(x + s * .2, y + s * a, x + s * .5, y + s * b, c, s * .12);
    p.line(x + s * .5, y + s * b, x + s * .8, y + s * a, c, s * .12);
  };
}
IconFn cross_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.line(x + s * .25, y + s * .25, x + s * .75, y + s * .75, c, s * .1);
    p.line(x + s * .75, y + s * .25, x + s * .25, y + s * .75, c, s * .1);
  };
}
IconFn calendar_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.stroke_rounded(x + s * .1, y + s * .18, s * .8, s * .72, s * .12, c, s * .08);
    p.line(x + s * .12, y + s * .4, x + s * .88, y + s * .4, c, s * .08);
    for (int i = 0; i < 2; ++i)
      p.line(x + s * (.32 + i * .36), y + s * .06, x + s * (.32 + i * .36), y + s * .28, c,
             s * .08);
  };
}

// The alarm row: time, name and repeat days; a click edits it.
class AlarmRow : public DrawnButton {
 public:
  AlarmRow(int x, int y, int w, int h, const Alarm &a, AlarmPopup *popup)
      : DrawnButton(x, y, w, h, nullptr), alarm_(a), popup_(popup) {
    copy_tooltip(format(_("Edit alarm %s"), alarm_name(a).c_str()).c_str());
    callback([](Fl_Widget *w, void *) {
      auto *r = static_cast<AlarmRow *>(w);
      int id = r->alarm_.id;
      AlarmPopup *p = r->popup_;
      Fl::add_timeout(0, [](void *d) {
        auto *pp = static_cast<std::pair<AlarmPopup *, int> *>(d);
        pp->first->edit(pp->second);
        delete pp;
      }, new std::pair<AlarmPopup *, int>(p, id));
    });
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    {
      Painter p(window()->w(), window()->h());
      if (Fl::belowmouse() == this) p.fill_rounded(x(), y(), w(), h(), S(6), t.hover);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(6));
    }
    Color ink = alarm_.enabled || alarm_.snoozed_until ? t.text : t.text_dim;
    text_font(t.font_size + 6, true);
    draw_text(time_text(alarm_.hour, alarm_.minute), x() + S(8), y() + S(2), w() - S(16), S(26), ink);
    std::string sub = alarm_.label.empty() ? std::string(_("Alarm")) : alarm_.label;
    sub += " · " + repeat_text(alarm_.days);
    if (alarm_.snoozed_until)
      sub += " · " + format(_("snoozed until %s"),
                            time_text(int(localtime_hour(alarm_.snoozed_until)),
                                      int(localtime_minute(alarm_.snoozed_until))).c_str());
    text_font(t.font_size - 1);
    draw_text(sub, x() + S(8), y() + S(27), w() - S(16), S(18), t.text_dim);
  }

 private:
  static int localtime_hour(long long t) {
    time_t tt = time_t(t);
    struct tm lt;
    localtime_r(&tt, &lt);
    return lt.tm_hour;
  }
  static int localtime_minute(long long t) {
    time_t tt = time_t(t);
    struct tm lt;
    localtime_r(&tt, &lt);
    return lt.tm_min;
  }
  Alarm alarm_;
  AlarmPopup *popup_;
};

// Big hour:minute display of the editor; the wheel changes the part under it.
class TimeDisplay : public Fl_Widget {
 public:
  TimeDisplay(int x, int y, int w, int h, AlarmPopup *popup) : Fl_Widget(x, y, w, h), popup_(popup) {
    copy_tooltip(_("Alarm time"));
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    const Alarm &a = popup_->draft();
    char hh[8], mm[8];
    int hour = clock_24h() ? a.hour : (a.hour % 12 ? a.hour % 12 : 12);
    snprintf(hh, sizeof hh, clock_24h() ? "%02d" : "%d", hour);
    snprintf(mm, sizeof mm, "%02d", a.minute);
    text_font(t.font_size + 22, true);
    int half = w() / 2;
    draw_text(hh, x(), y(), half - S(10), h(), t.text, 2);
    draw_text(":", x() + half - S(10), y(), S(20), h(), t.text_dim, 1);
    draw_text(mm, x() + half + S(10), y(), half - S(10), h(), t.text, 0);
    if (!clock_24h()) {
      text_font(t.font_size, true);
      draw_text(a.hour < 12 ? "AM" : "PM", x() + w() - S(40), y(), S(40), h(), t.text_dim, 2);
    }
  }
  int handle(int e) override {
    if (e == FL_MOUSEWHEEL) {
      int step = -Fl::event_dy();
      if (Fl::event_x() < x() + w() / 2)
        popup_->step_hour(step);
      else
        popup_->step_minute(step);
      return 1;
    }
    if (e == FL_ENTER || e == FL_LEAVE || e == FL_MOVE) return 1;
    return Fl_Widget::handle(e);
  }

 private:
  AlarmPopup *popup_;
};

class DayToggle : public DrawnButton {
 public:
  DayToggle(int x, int y, int size, int index, AlarmPopup *popup)
      : DrawnButton(x, y, size, size, nullptr), index_(index), popup_(popup) {
    copy_tooltip(day_name(index, false).c_str());
    callback([](Fl_Widget *w, void *) {
      auto *d = static_cast<DayToggle *>(w);
      d->popup_->toggle_day(1u << d->index_);
      d->redraw();
    });
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    bool on = popup_->draft().days & (1u << index_);
    {
      Painter p(window()->w(), window()->h());
      double r = w() / 2.0;
      p.circle(x() + r, y() + r, r, on ? t.accent : Fl::belowmouse() == this ? t.hover : t.surface);
      if (!on) {
        p.set(t.border);
        cairo_set_line_width(p.cr(), 1);
        cairo_new_sub_path(p.cr());
        cairo_arc(p.cr(), x() + r, y() + r, r - 0.5, 0, 2 * M_PI);
        cairo_stroke(p.cr());
      }
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
    }
    std::string letter = day_name(index_, true);
    // Two characters, UTF-8 aware.
    size_t cut = 0, chars = 0;
    while (cut < letter.size() && chars < 2) {
      ++cut;
      while (cut < letter.size() && (static_cast<unsigned char>(letter[cut]) & 0xC0) == 0x80) ++cut;
      ++chars;
    }
    text_font(t.font_size - 1, on);
    draw_text(letter.substr(0, cut), x(), y(), w(), h(), on ? t.on_accent : t.text, 1);
  }

 private:
  int index_;
  AlarmPopup *popup_;
};

class AlarmSwitch : public Switch {
 public:
  AlarmSwitch(int x, int y, int w, int h, const Alarm &a) : Switch(x, y, w, h), id_(a.id) {
    value(a.enabled ? 1 : 0);
    copy_tooltip(format(_("Alarm %s on"), alarm_name(a).c_str()).c_str());
    callback([](Fl_Widget *w, void *) {
      auto *s = static_cast<AlarmSwitch *>(w);
      int id = s->id_;
      bool on = s->value();
      Fl::add_timeout(0, [](void *d) {
        auto *p = static_cast<std::pair<int, bool> *>(d);
        Alarms::get().set_enabled(p->first, p->second);
        delete p;
      }, new std::pair<int, bool>(id, on));
    });
  }

 private:
  int id_;
};

void later(std::function<void()> fn) {
  auto *heap = new std::function<void()>(std::move(fn));
  Fl::add_timeout(0, [](void *d) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(d));
    (*f)();
  }, heap);
}

const int kSnooze[] = {5, 10, 15, 20, 30};

}  // namespace

AlarmPopup::AlarmPopup() : PanelPopup("Alarms") { end(); }

std::string AlarmPopup::key() const {
  const auto &cfg = Panel::instance->config();
  std::string k = std::to_string(Alarms::get().revision()) + "|" + std::to_string(editing_) + "|" +
                  std::to_string(draft_.id) + "|" + message_;
  for (const auto &r : Alarms::get().ringing()) k += "|" + std::to_string(r.alarm) + r.title;
  k += cfg.get("appearance", "theme") + cfg.get("appearance", "accent") +
       cfg.get("appearance", "style") + cfg.get("appearance", "scale") + cfg.get("panel", "clock_24h") + cfg.get("clock", "format") +
       cfg.get("panel", "position");
  return k;
}

void AlarmPopup::prepare() {
  if (pending_new_) {
    editing_ = true;
    draft_ = Alarm();
    pending_new_ = false;
  } else {
    editing_ = false;
  }
  message_.clear();
  rebuild();
}

void AlarmPopup::closed() {
  editing_ = false;
  pending_new_ = false;
}

void AlarmPopup::changed() {
  if (!shown()) return;
  if (key() == built_) {
    redraw();
    return;
  }
  if (!rebuild_pending_) {
    rebuild_pending_ = true;
    Fl::add_timeout(0, rebuild_timeout, this);
  }
}

void AlarmPopup::rebuild_timeout(void *data) {
  auto *self = static_cast<AlarmPopup *>(data);
  if (!self->shown()) {
    self->rebuild_pending_ = false;
    return;
  }
  if (Fl::pushed() && Fl::pushed()->window() == self) {
    Fl::add_timeout(.1, rebuild_timeout, data);
    return;
  }
  int scroll = self->body_ ? std::max(0, self->body_->yposition()) : 0;
  std::string focus = focused_name(self);
  self->rebuild();
  self->body_->scroll_to(0, std::min(scroll, std::max(0, self->content_height_ - self->body_->h())));
  self->reposition();
  restore_focus(self, focus);
}

void AlarmPopup::edit(int id) {
  const Alarm *a = id ? Alarms::get().alarm(id) : nullptr;
  draft_ = a ? *a : Alarm();
  if (!a) {
    // A new alarm starts at the next full hour.
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    draft_.hour = (lt.tm_hour + 1) % 24;
    draft_.minute = 0;
  }
  editing_ = true;
  message_.clear();
  changed();
}

void AlarmPopup::step_hour(int delta) {
  draft_.hour = ((draft_.hour + delta) % 24 + 24) % 24;
  redraw();
}

void AlarmPopup::step_minute(int delta) {
  draft_.minute = ((draft_.minute + delta) % 60 + 60) % 60;
  redraw();
}

void AlarmPopup::toggle_day(int bit) {
  draft_.days ^= unsigned(bit);
  redraw();
}

void AlarmPopup::save() {
  Alarm a = draft_;
  a.label = trim(a.label);
  Alarms::get().save_alarm(a);
  editing_ = false;
  long long next = Alarms::get().next_alarm();
  message_ = next ? format(_("Alarm set for %s, %s."), time_text(a.hour, a.minute).c_str(),
                           until_text(next).c_str())
                  : "";
  changed();
}

void AlarmPopup::cancel() {
  editing_ = false;
  changed();
}

void AlarmPopup::remove() {
  if (draft_.id) Alarms::get().remove_alarm(draft_.id);
  editing_ = false;
  changed();
}

void AlarmPopup::rebuild() {
  Fl::remove_timeout(rebuild_timeout, this);
  rebuild_pending_ = false;
  built_ = key();
  clear();
  resizable(nullptr);
  const Theme &t = Panel::instance->theme();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  auto *panel = Panel::instance;
  int W = std::min(S(popup_style::kFlyoutW), mw - S(16));
  int pad = S(popup_style::kPad), scrollbar = S(6), inner = W - 2 * pad - scrollbar;
  int available = std::max(S(160), (t.panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my) - S(16));
  header_ = S(58);
  begin();
  body_ = new Fl_Scroll(S(2), header_, W - S(4), std::max(S(40), available - header_));
  body_->type(Fl_Scroll::VERTICAL);
  body_->resizable(nullptr);
  body_->scrollbar_size(scrollbar);
  body_->box(FL_FLAT_BOX);
  body_->color(t.surface.fl());
  new Fl_Box(body_->x(), body_->y(), 1, 1);
  int y = header_ + S(8);
  auto &alarms = Alarms::get();
  auto label = [&](std::function<std::string()> text, int style, int h, int indent = 0) {
    new Label(pad + indent, y, inner - indent, S(h), std::move(text), style);
    y += S(h);
  };
  auto end_card = [&](Card *card) {
    card->size(inner, y - card->y() + S(8));
    card->end();
    y += S(16);
  };

  // Ringing alarms come first.
  for (const auto &r : alarms.ringing()) {
    if (!r.alarm) continue;
    auto *card = new Card(pad, y, inner, true);
    y += S(10);
    std::string title = r.title, body = r.body;
    label([title] { return title; }, 1, 26, S(12));
    label([body] { return body; }, 2, 22, S(12));
    y += S(6);
    int gap = S(8), bw = (inner - S(24) - gap) / 2, id = r.alarm;
    new FnButton(pad + S(12), y, bw, S(34), _("Snooze"), [id] { later([id] { Alarms::get().snooze(id); }); }, true);
    new FnButton(pad + S(12) + bw + gap, y, bw, S(34), _("Dismiss"),
                 [id] { later([id] { Alarms::get().dismiss(id); }); });
    y += S(34);
    end_card(card);
  }

  if (!editing_) {
    label([] { return std::string(_("Alarms")); }, 1, 24);
    if (!message_.empty()) {
      std::string m = message_;
      label([m] { return m; }, 2, 22);
    }
    label(
        [] {
          int id = 0;
          long long next = Alarms::get().next_alarm(&id);
          if (!next) return std::string(_("No alarm is on."));
          time_t tt = time_t(next);
          struct tm lt;
          localtime_r(&tt, &lt);
          char day[32];
          strftime(day, sizeof day, "%a", &lt);
          return format(_("Next alarm: %s %s (%s)"), day, time_text(lt.tm_hour, lt.tm_min).c_str(),
                        until_text(next).c_str());
        },
        2, 22);
    y += S(4);
    for (const auto &a : alarms.alarms()) {
      auto *card = new Card(pad, y, inner);
      int sw = S(40), del = S(28);
      new AlarmRow(pad + S(4), y + S(3), inner - sw - del - S(24), S(46), a, this);
      new AlarmSwitch(pad + inner - sw - del - S(16), y + S(15), sw, S(22), a);
      int id = a.id;
      new IconButton(pad + inner - del - S(8), y + S(12), del, del,
                     format(_("Delete alarm %s"), alarm_name(a).c_str()), cross_icon(),
                     [id] { later([id] { Alarms::get().remove_alarm(id); }); });
      y += S(52);
      card->size(inner, S(52));
      card->end();
      y += S(6);
    }
    if (alarms.alarms().empty()) {
      label([] { return std::string(_("No alarms yet.")); }, 0, 24);
      label([] { return std::string(_("Add one to wake up or to be reminded on time.")); }, 2, 22);
      y += S(8);
    }
    new FnButton(pad, y, inner, S(36), _("Add alarm"), [this] { later([this] { edit(0); }); }, true);
    y += S(44);
  } else {
    label([this] { return std::string(draft_.id ? _("Edit alarm") : _("New alarm")); }, 1, 28);
    auto *card = new Card(pad, y, inner);
    y += S(8);
    int col = S(64), cx = pad + inner / 2;
    int hx = cx - S(10) - col + (col - S(40)) / 2, mx2 = cx + S(10) + (col - S(40)) / 2;
    new IconButton(hx, y, S(40), S(30), _("Hour up"), arrow_icon(true), [this] { step_hour(1); });
    new IconButton(mx2, y, S(40), S(30), _("Minute up"), arrow_icon(true), [this] { step_minute(5); });
    y += S(32);
    new TimeDisplay(cx - S(10) - col, y, 2 * col + S(20), S(52), this);
    y += S(52);
    new IconButton(hx, y, S(40), S(30), _("Hour down"), arrow_icon(false), [this] { step_hour(-1); });
    new IconButton(mx2, y, S(40), S(30), _("Minute down"), arrow_icon(false),
                   [this] { step_minute(-5); });
    y += S(36);
    label([] { return std::string(_("Scroll over the time to change it minute by minute.")); }, 2, 22,
          S(12));
    end_card(card);

    label([] { return std::string(_("Repeat")); }, 1, 26);
    label([this] { return repeat_text(draft_.days); }, 2, 22);
    int size = std::min(S(38), (inner - 6 * S(6)) / 7), gap = (inner - 7 * size) / 6;
    for (int i = 0; i < 7; ++i) new DayToggle(pad + i * (size + gap), y + S(4), size, i, this);
    y += size + S(16);

    label([] { return std::string(_("Name")); }, 1, 26);
    auto *name = new TextField(pad, y, inner, S(34), _("Alarm name"), _("Alarm"),
                               [this](const std::string &s) { set_label(s); });
    name->value(draft_.label.c_str());
    y += S(44);

    label([] { return std::string(_("Snooze time")); }, 1, 26);
    auto *snooze = new Fl_Choice(pad, y, inner, S(32));
    snooze->tooltip(_("Snooze time"));
    snooze->textsize(t.font_size);
    for (int i = 0; i < 5; ++i) {
      snooze->add(format(_("%d minutes"), kSnooze[i]).c_str());
      if (kSnooze[i] == draft_.snooze) snooze->value(i);
    }
    if (snooze->value() < 0) snooze->value(1);
    snooze->callback([](Fl_Widget *w, void *d) {
      int i = static_cast<Fl_Choice *>(w)->value();
      if (i >= 0 && i < 5) static_cast<AlarmPopup *>(d)->set_snooze(kSnooze[i]);
    }, this);
    y += S(44);

    int gap2 = S(8), n = draft_.id ? 3 : 2, bw = (inner - (n - 1) * gap2) / n;
    new FnButton(pad, y, bw, S(36), _("Save"), [this] { later([this] { save(); }); }, true);
    new FnButton(pad + bw + gap2, y, bw, S(36), _("Cancel"), [this] { later([this] { cancel(); }); });
    if (draft_.id)
      new FnButton(pad + 2 * (bw + gap2), y, bw, S(36), _("Delete"),
                   [this] { later([this] { remove(); }); });
    y += S(46);
  }
  content_height_ = y - header_ + S(4);
  new Fl_Box(pad, y, inner, S(4));
  body_->end();
  int H = std::min(available, header_ + content_height_);
  body_->size(W - S(4), H - header_);

  // Fixed header: the time and date, and the calendar link.
  begin();
  new Label(pad + S(4), S(6), W - 2 * pad - S(52), S(28),
            [] {
              time_t now = time(nullptr);
              struct tm lt;
              localtime_r(&now, &lt);
              return time_text(lt.tm_hour, lt.tm_min);
            },
            3);
  new Label(pad + S(4), S(32), W - 2 * pad - S(52), S(20),
            [] {
              time_t now = time(nullptr);
              struct tm lt;
              localtime_r(&now, &lt);
              char date[96];
              strftime(date, sizeof date, "%A, %e %B %Y", &lt);
              return trim(date);
            },
            2);
  new IconButton(W - pad - S(32), (header_ - S(32)) / 2, S(32), S(32), _("Calendar and agenda"),
                 calendar_icon(), [this] {
                   later([this] {
                     close();
                     Panel::instance->open_action_center(true);
                   });
                 });
  end();
  resizable(nullptr);
  init_sizes();
  size(W, H);
  redraw();
}

void AlarmPopup::draw() {
  const Theme &t = Panel::instance->theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    if (t.glass) p.fill_rect(0, 0, w(), header_, t.surface_alt.mix(t.surface, 0.55));
    stroke_outline(p, t.border);
    p.line(S(popup_style::kPad), header_ - 0.5, w() - S(popup_style::kPad), header_ - 0.5, t.border);
  }
  draw_children();
  if (qa_dump_enabled() && body_)
    qa_dump(this, "@body\t" + std::to_string(body_->x()) + "\t" + std::to_string(body_->y()) + "\t" +
                      std::to_string(body_->w()) + "\t" + std::to_string(body_->h()) + "\t1\n");
}

int AlarmPopup::handle(int event) {
  if (event == FL_MOUSEWHEEL && body_ && Fl::event_inside(body_)) {
    auto *target = Fl::belowmouse();
    if (target && Fl::event_inside(target) &&
        (dynamic_cast<TimeDisplay *>(target) || dynamic_cast<Fl_Choice *>(target)))
      return PanelPopup::handle(event);
    int limit = std::max(0, content_height_ - body_->h());
    body_->scroll_to(0, std::max(0, std::min(limit, body_->yposition() + Fl::event_dy() * S(36))));
    redraw();
    return 1;
  }
  return PanelPopup::handle(event);
}
