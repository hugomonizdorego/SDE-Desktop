// SPDX-License-Identifier: GPL-3.0-or-later
// Control Center > Panel > Start button (0.2.0 Alpha 4): the picture and the
// text of the panel's Start button and which of them it shows, with a
// preview at the panel's real size ([panel] start_style, start_label,
// start_icon; src/common/start_button.h, docs/PANEL.md); and the Task View
// button ([panel] show_task_view). The panel follows every change at once.
#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <string>

#include "file_chooser.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "settings.h"
#include "start_button.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

// The panel's icon size for its height (Panel::relayout()).
int panel_icon(const Theme &t) {
  int H = t.panel_height;
  return H >= S(48) ? S(28) : H >= S(40) ? S(24) : S(20);
}

// The left end of the panel as it will look: its surface, the Start button
// laid out by start_button::layout() at the panel's size, and a hint of
// the search box after it.
class StartPreview;
StartPreview *g_preview = nullptr;

class StartPreview : public Fl_Widget {
 public:
  StartPreview(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    tooltip(_("The Start button as the panel shows it"));
    g_preview = this;
  }
  ~StartPreview() override {
    if (g_preview == this) g_preview = nullptr;
  }
  void draw() override {
    const Theme &t = settings_theme();
    auto s = start_button::from_config(settings_config());
    int H = std::min(h(), t.panel_height), icon = panel_icon(t);
    int px = x(), py = y() + (h() - H) / 2;
    text_font(t.font_size, true);
    auto L = start_button::layout(s, icon, s.shows_text() ? text_width(s.label) : 0, Sd(1));
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(px, py, w(), H, S(6), t.surface);
      p.stroke_rounded(px, py, w(), H, S(6), t.border);
      p.fill_rounded(px + L.box_x, py + S(5), L.box_w, H - S(10), S(6), t.accent);
      if (s.shows_icon()) {
        cairo_surface_t *own = s.icon.empty() ? nullptr : IconLoader::get({s.icon}, icon);
        p.image(own ? own : IconLoader::get({"sde-logo", "start-here", "distributor-logo"}, icon),
                px + L.icon_x, py + (H - icon) / 2.0, icon);
      }
      int sx = px + L.width + S(4), sw = std::min(S(150), x() + w() - sx - S(8));
      if (sw > S(30)) p.fill_rounded(sx, py + S(8), sw, H - S(16), S(4), t.surface_alt);
    }
    if (s.shows_text()) {
      text_font(t.font_size, true);
      draw_text(s.label, px + L.text_x, py, L.text_w + S(2), H, t.on_accent);
    }
    qa_line("@start-preview:" + std::string(start_button::style_name(s.style)) + ":" +
                (s.shows_text() ? s.label : std::string()) + ":" + (s.icon.empty() ? "default" : "picture"),
            px + L.box_x, py, L.box_w, H);
  }
};

void changed() {
  apply_change();
  if (g_preview) g_preview->redraw();
}

// The text is saved a moment after the last key, so the panel does not lay
// itself out for every letter.
std::string g_label;
void save_label(void *) {
  settings_config().set("panel", "start_label", start_button::clean_label(g_label));
  changed();
}

void choose_picture() {
  FileChooser chooser(FileChooser::Mode::Open);
  chooser.title(_("A picture for the Start button"));
  chooser.filters(std::string(_("Pictures")) + "\t*.{png,jpg,jpeg,svg}");
  chooser.all_files(false);
  std::string dir = user_dir("PICTURES");
  chooser.default_folder(dir.empty() || !dir_exists(dir) ? home_dir() : dir);
  chooser.preview(true);
  if (!chooser.run()) return;
  std::string error;
  bool small = false, cropped = false;
  std::string path = start_button::prepare_picture(chooser.path(), &error, &small, &cropped);
  if (path.empty()) {
    fl_alert("%s", error.c_str());
    return;
  }
  Config &cfg = settings_config();
  cfg.set("panel", "start_icon", path);
  if (start_button::parse_style(cfg.get("panel", "start_style", "both")) == start_button::Style::TextOnly)
    cfg.set("panel", "start_style", "both");
  start_button::remove_pictures(path);
  changed();
  if (small)
    fl_message("%s", format(_("The picture is smaller than %d × %d pixels, so it may look blurred on the "
                              "panel. It is used anyway."),
                            start_button::kMinPicture, start_button::kMinPicture)
                         .c_str());
  else if (cropped)
    fl_message("%s", _("The picture was not square: its middle part is used."));
  Fl::add_timeout(0, [](void *) { refresh_page(); });
}

}  // namespace

void build_start_button_card(Page *page) {
  Config &cfg = settings_config();
  const Theme &t = settings_theme();
  auto s = start_button::from_config(cfg);
  page->anchor("start-button");
  page->begin_card(_("Start button"));
  page->row(_("Preview"), S(300), t.panel_height + S(8),
            [](int x, int y, int w, int h) { return new StartPreview(x, y, w, h); });
  page->choice_row(
      _("Show"), {_("Picture and text"), _("Picture only"), _("Text only")},
      s.style == start_button::Style::IconOnly ? 1 : s.style == start_button::Style::TextOnly ? 2 : 0,
      [](int v) {
        static const char *values[] = {"both", "icon", "text"};
        settings_config().set("panel", "start_style", values[v < 0 || v > 2 ? 0 : v]);
        changed();
      });
  std::string label = s.label;
  page->row(_("Text"), S(240), S(32), [label](int x, int y, int w, int h) {
    const Theme &th = settings_theme();
    auto *in = new Fl_Input(x, y, w, h);
    in->box(FL_DOWN_BOX);
    in->color(th.surface_alt.fl());
    in->textcolor(th.text.fl());
    in->cursor_color(th.accent.fl());
    in->selection_color(th.accent.fl());
    in->textsize(th.font_size + 1);
    in->value(label.c_str());
    in->maximum_size(int(start_button::kMaxLabel) * 4);
    in->tooltip(_("The text on the Start button"));
    in->when(FL_WHEN_CHANGED);
    in->callback([](Fl_Widget *w, void *) {
      g_label = static_cast<Fl_Input *>(w)->value();
      Fl::remove_timeout(save_label);
      Fl::add_timeout(0.5, save_label);
    });
    return in;
  }, format(_("Up to %d characters. Leave it empty to show the picture alone."), int(start_button::kMaxLabel)));
  int icon = panel_icon(t);
  page->row(_("Picture"), S(240), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Choose a Picture..."));
    b->copy_label(_("Choose a Picture..."));
    b->callback([](Fl_Widget *, void *) { Fl::add_timeout(0, [](void *) { choose_picture(); }); });
    return b;
  }, format(_("A square picture of at least %d × %d pixels: PNG, JPEG or SVG. Other shapes are cropped "
              "to their middle; the panel shows it at %d × %d pixels."),
            start_button::kMinPicture, start_button::kMinPicture, icon, icon));
  page->row(_("The SDE logo and \"SDE\""), S(240), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Reset to Default"));
    b->copy_label(_("Reset to Default"));
    b->callback([](Fl_Widget *, void *) {
      Config &cfg = settings_config();
      cfg.unset("panel", "start_style");
      cfg.unset("panel", "start_label");
      cfg.unset("panel", "start_icon");
      start_button::remove_pictures("");
      changed();
      Fl::add_timeout(0, [](void *) { refresh_page(); });
    });
    return b;
  });
  page->end_card();

  // The Task View button (sde-taskview: the workspaces and their windows),
  // after the search; off by default.
  page->anchor("task-view");
  page->begin_card(_("Task View"));
  bool installed = !find_program("sde-taskview").empty();
  page->switch_row(
      _("Task View button"), cfg.get_bool("panel", "show_task_view", false),
      [](bool on) {
        settings_config().set("panel", "show_task_view", on ? "true" : "false");
        apply_change();
      },
      installed ? _("Shows your workspaces and their windows, like Super+Tab.")
                : _("Needs SDE Task View, which is not installed: the button appears once it is."));
  page->end_card();
}

}  // namespace sde
