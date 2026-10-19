// SPDX-License-Identifier: GPL-3.0-or-later
#include "suite.h"
#include "suite_data.h"
#include "util.h"
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include "spawn.h"
#include "theme.h"
#include "widgets.h"
namespace sde {
const std::vector<SuiteApp> &suite_apps() {
  static const std::vector<SuiteApp> apps = [] {
    std::vector<SuiteApp> result;
    for (const auto &line : split(kSuiteManifest, '\n')) {
      if (line.empty() || line[0] == '#') continue;
      auto f = split(line, '\t');
      if (f.size() == 10) result.push_back({f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]});
    }
    return result;
  }();
  return apps;
}
void show_app_about(const std::string &id) {
  for (const auto &app : suite_apps()) {
    if (app.id != id) continue;
    auto *window = new Fl_Double_Window(S(490), S(320));
    window->copy_label(("About " + app.name).c_str());
    window->color(active_theme().background.fl());
    auto *text = new Fl_Box(S(20), S(16), S(450), S(120));
    const auto description = app.name + " " + app.version + "\n" + app.role +
        "\n\nSDE developer: Hugo Moniz do Rego\nOriginal authors and copyright notices are retained.";
    text->copy_label(description.c_str());
    text->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    text->labelcolor(active_theme().text.fl());
    text->labelsize(S(14));
    auto link = [](int y, const std::string &label, const std::string &uri) {
      auto *button = new FlatButton(S(20), y, S(450), S(34), label);
      // Window close schedules deletion of the complete widget tree below.
      button->copy_tooltip(uri.c_str());
      button->callback([](Fl_Widget *w, void *) { open_uri(w->tooltip()); });
    };
    link(S(148), "Hugo Moniz do Rego", "https://github.com/hugomonizdorego");
    link(S(190), app.upstream, app.url);
    link(S(232), "SDE source", "https://github.com/hugomonizdorego/SDE-Desktop");
    link(S(274), "Copyright and license", "/usr/share/doc/" + app.package + "/copyright");
    window->end();
    window->callback([](Fl_Widget *w, void *) { w->hide(); Fl::delete_widget(w); });
    window->show();
    return;
  }
}
}  // namespace sde
