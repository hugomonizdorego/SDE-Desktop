// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Clock & Weather (alpha52, sde-settings --page clock; the panel
// clock's right-click menu opens it): the clock style shared by the panel
// and the Action Center's clock hero (digital, blinking colon, LED digits,
// analog), 12/24-hour time, seconds and the date on the panel; the weather
// in the Action Center, its units, place and animation. The panel follows
// every change at once (the config serial).
#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Window.H>
#include <langinfo.h>

#include <ctime>
#include <string>
#include <vector>

#include "../panel/clockface.h"
#include "../panel/weather.h"
#include "config.h"
#include "i18n.h"
#include "paint.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

const clockface::Style kStyles[] = {clockface::Style::Digital, clockface::Style::Blink,
                                    clockface::Style::Led, clockface::Style::Analog};

// The four clock styles as selectable cards, each with a preview of the
// current time in that style.
class StylePicker : public Fl_Widget {
 public:
  StylePicker(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    label(_("Clock style"));
    labeltype(FL_NO_LABEL);
    selected_ = int(clockface::Settings::from_config(settings_config()).style);
    Fl::add_timeout(1.0, tick, this);
  }
  ~StylePicker() override { Fl::remove_timeout(tick, this); }
  void draw() override {
    const Theme &t = settings_theme();
    clockface::Settings cs = clockface::Settings::from_config(settings_config());
    struct tm lt = clockface::local_now();
    int n = 4, gap = S(10), cw = (w() - (n - 1) * gap) / n, ph = h() - S(28);
    static const char *names[] = {N_("Digital"), N_("Blink"), N_("LED"), N_("Analog")};
    for (int i = 0; i < n; ++i) {
      int cx = x() + i * (cw + gap);
      bool sel = i == selected_, hot = i == hover_;
      {
        Painter p(window()->w(), window()->h());
        // A small night-blue screen for every preview.
        p.fill_gradient(cx, y(), cw, ph, S(10), Color::hex(0x1B2A4A), Color::hex(0x31477A));
        if (sel)
          p.stroke_rounded(cx + 1, y() + 1, cw - 2, ph - 2, S(10), t.accent, S(3));
        else if (hot)
          p.stroke_rounded(cx + .5, y() + .5, cw - 1, ph - 1, S(10), t.border, S(1));
        Color ink = Color::hex(0xFFFFFF);
        if (kStyles[i] == clockface::Style::Led) {
          std::string d = clockface::time_digits(lt, cs.h24, false);
          double lh = ph * .38, lw = clockface::led_width(d, lh);
          clockface::draw_led(p, d, cx + (cw - lw) / 2, y() + (ph - lh) / 2, lh,
                              t.accent.mix(ink, .55), lt.tm_sec % 2 == 0, true);
        } else if (kStyles[i] == clockface::Style::Analog) {
          double r = ph * .36;
          clockface::draw_analog(p, cx + cw / 2.0, y() + ph / 2.0, r, lt, Color::hex(0xFFFFFF, .16),
                                 Color::hex(0xE6EDF6), ink, t.accent.mix(ink, .25), cs.seconds,
                                 true);
        }
        if (Fl::focus() == this && sel) focus_ring(p, cx, y(), cw, ph, S(10));
      }
      if (kStyles[i] == clockface::Style::Digital || kStyles[i] == clockface::Style::Blink) {
        text_font(std::max(S(14), ph / 4), true);
        std::string s = clockface::format_time(lt, cs.h24, false);
        bool colon = kStyles[i] == clockface::Style::Digital || lt.tm_sec % 2 == 0;
        clockface::draw_blink_text(s, cx, y(), cw, ph, Color::hex(0xFFFFFF), colon);
      }
      text_font(t.font_size, sel);
      draw_text(_(names[i]), cx, y() + ph + S(4), cw, S(22), sel ? t.accent_text : t.text, 1);
    }
  }
  int handle(int event) override {
    switch (event) {
      case FL_ENTER:
      case FL_MOVE: {
        int c = cell_at(Fl::event_x());
        if (c != hover_) {
          hover_ = c;
          redraw();
        }
        return 1;
      }
      case FL_LEAVE:
        hover_ = -1;
        redraw();
        return 1;
      case FL_PUSH: {
        int c = cell_at(Fl::event_x());
        if (c >= 0) choose(c);
        take_focus();
        return 1;
      }
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == FL_Left && selected_ > 0) return choose(selected_ - 1), 1;
        if (Fl::event_key() == FL_Right && selected_ < 3) return choose(selected_ + 1), 1;
        return 0;
    }
    return Fl_Widget::handle(event);
  }

 private:
  int cell_at(int ex) const {
    int gap = S(10), cw = (w() - 3 * gap) / 4;
    int i = (ex - x()) / (cw + gap);
    return i >= 0 && i < 4 && (ex - x()) - i * (cw + gap) < cw ? i : -1;
  }
  void choose(int i) {
    selected_ = i;
    settings_config().set("clock", "style", clockface::style_name(kStyles[i]));
    apply_change();
    redraw();
  }
  static void tick(void *d) {
    auto *self = static_cast<StylePicker *>(d);
    self->redraw();
    Fl::repeat_timeout(1.0, tick, d);
  }
  int selected_ = 0, hover_ = -1;
};

// [clock] format, kept in step with the older [panel] clock_24h (the lock
// screen and other programs read it).
void set_format(int v) {
  Config &cfg = settings_config();
  static const char *values[] = {"auto", "24h", "12h"};
  cfg.set("clock", "format", values[v < 0 || v > 2 ? 0 : v]);
  const char *t_fmt = nl_langinfo(T_FMT);
  bool h24 = v == 1 || (v == 0 && !clockface::format_is_12h(t_fmt ? t_fmt : ""));
  cfg.set("panel", "clock_24h", h24 ? "true" : "false");
  apply_change();
}

bool g_city_mode = false, g_city_init = false;

}  // namespace

void build_clock_page(Page *page) {
  Config &cfg = settings_config();
  page->heading(_("Clock & Weather"),
                _("The clock on the panel and in the Action Center, and the weather behind it"));
  page->begin_card(_("Clock"));
  {
    int x = page->left() + S(20), y = page->cursor(), w = page->width() - S(40), h = S(124);
    new StylePicker(x, y + S(8), w, h);
    page->advance(h + S(18));
  }
  std::string format = lower(trim(cfg.get("clock", "format", "auto")));
  page->choice_row(_("Time format"), {_("Automatic"), _("24-hour"), _("12-hour")},
                   format == "24h" ? 1 : format == "12h" ? 2 : 0, [](int v) { set_format(v); },
                   _("Automatic follows the language and region."));
  page->switch_row(_("Show seconds"), cfg.get_bool("clock", "seconds", false), [](bool on) {
    settings_config().set("clock", "seconds", on ? "true" : "false");
    apply_change();
  });
  page->switch_row(_("Show the date on the panel"), cfg.get_bool("panel", "show_date", true),
                   [](bool on) {
                     settings_config().set("panel", "show_date", on ? "true" : "false");
                     apply_change();
                   });
  page->note(_("Blink flashes the colon every second; LED draws seven-segment digits. "
               "Right-click the panel clock to come back here."));
  page->end_card();

  weather::Settings ws = weather::Settings::from_config(cfg);
  page->begin_card(_("Weather"));
  page->switch_row(_("Show the weather in the Action Center"), ws.enabled, [](bool on) {
    settings_config().set("weather", "enabled", on ? "true" : "false");
    apply_change();
  });
  std::string unit = lower(trim(cfg.get("weather", "unit", "auto")));
  page->choice_row(_("Temperature"),
                   {_("Automatic"), _("Celsius (°C)"), _("Fahrenheit (°F)"), _("Kelvin (K)")},
                   unit == "celsius" || unit == "c"      ? 1
                   : unit == "fahrenheit" || unit == "f" ? 2
                   : unit == "kelvin" || unit == "k"     ? 3
                                                         : 0,
                   [](int v) {
                     static const char *values[] = {"auto", "celsius", "fahrenheit", "kelvin"};
                     settings_config().set("weather", "unit", values[v < 0 || v > 3 ? 0 : v]);
                     apply_change();
                   });
  std::string wind = lower(trim(cfg.get("weather", "wind_unit", "auto")));
  page->choice_row(_("Wind speed"), {_("Automatic"), "km/h", "mph", "m/s", _("Knots")},
                   wind == "kmh"     ? 1
                   : wind == "mph"   ? 2
                   : wind == "ms"    ? 3
                   : wind == "knots" ? 4
                                     : 0,
                   [](int v) {
                     static const char *values[] = {"auto", "kmh", "mph", "ms", "knots"};
                     settings_config().set("weather", "wind_unit", values[v < 0 || v > 4 ? 0 : v]);
                     apply_change();
                   });
  if (!g_city_init) {
    g_city_init = true;
    g_city_mode = ws.location != "auto";
  }
  page->choice_row(_("Location"), {_("Automatic"), _("A city or place")}, g_city_mode ? 1 : 0,
                   [](int v) {
                     g_city_mode = v == 1;
                     if (!g_city_mode) {
                       settings_config().set("weather", "location", "auto");
                       apply_change();
                     }
                     Fl::add_timeout(0, [](void *) { refresh_page(); });
                   },
                   _("Automatic finds the place from your internet address."));
  if (g_city_mode) {
    std::string place = ws.location == "auto" ? "" : ws.location;
    page->row(_("City"), S(240), S(32), [place](int x, int y, int w, int h) {
      const Theme &t = settings_theme();
      auto *in = new Fl_Input(x, y, w, h);
      in->box(FL_BORDER_BOX);
      in->color(t.surface_alt.fl());
      in->textcolor(t.text.fl());
      in->cursor_color(t.text.fl());
      in->selection_color(t.accent.fl());
      in->textsize(t.font_size + 1);
      in->value(place.c_str());
      in->tooltip(_("City, or latitude,longitude"));
      in->when(FL_WHEN_ENTER_KEY_ALWAYS | FL_WHEN_RELEASE);
      in->callback([](Fl_Widget *w, void *) {
        std::string v = trim(static_cast<Fl_Input *>(w)->value());
        settings_config().set("weather", "location", v.empty() ? "auto" : v);
        apply_change();
      });
      return in;
    }, _("Press Enter to save. \"lat,lon\" works too."));
  }
  page->switch_row(_("Animated sky"), ws.animate, [](bool on) {
    settings_config().set("weather", "animation", on ? "true" : "false");
    apply_change();
  }, _("Clouds, rain, snow and stars move gently; off shows a still picture."));
  page->note(_("Weather by Open-Meteo, shown only while the computer is online. Without it, the "
               "Action Center shows the day or night sky."));
  page->end_card();
  page->finish();
}

}  // namespace sde
