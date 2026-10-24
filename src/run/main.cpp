// SPDX-License-Identifier: GPL-3.0-or-later
// sde-run: "Run Command" dialog (Alt+F2). The command is split into
// arguments like a desktop file Exec= line and started without a shell.
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include "buffered_window.h"
#include <FL/Fl_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "widgets.h"
#include "paint.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"

using namespace sde;

namespace {

Theme g_theme;
Fl_Input *g_input = nullptr;
std::vector<std::string> g_history;
size_t g_history_pos = 0;

AppInfo app_info() {
  AppInfo info;
  info.name = _("Run Command");
  info.program = "sde-run";
  info.version = SDE_VERSION;
  info.icon = "sde-run";
  info.description = _("Starts a program, a command or a web address (Alt+F2).");
  info.credits = {toolkit_credit()};
  return info;
}

std::string history_path() { return state_home() + "/sde/run-history"; }

void load_history() {
  std::string text;
  if (!read_file(history_path(), text)) return;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line))
    if (!trim(line).empty()) g_history.push_back(line);
  g_history_pos = g_history.size();
}

void save_history(const std::string &cmd) {
  std::vector<std::string> out;
  for (auto &h : g_history)
    if (h != cmd) out.push_back(h);
  out.push_back(cmd);
  if (out.size() > 50) out.erase(out.begin(), out.end() - 50);
  std::string text;
  for (auto &h : out) text += h + "\n";
  write_file_atomic(history_path(), text);
}

class RunInput : public Fl_Input {
 public:
  RunInput(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {}
  int handle(int e) override {
    if (e == FL_KEYBOARD && !g_history.empty()) {
      if (Fl::event_key() == FL_Up && g_history_pos > 0) {
        value(g_history[--g_history_pos].c_str());
        position(size());
        return 1;
      }
      if (Fl::event_key() == FL_Down) {
        if (g_history_pos + 1 < g_history.size()) value(g_history[++g_history_pos].c_str());
        else {
          g_history_pos = g_history.size();
          value("");
        }
        position(size());
        return 1;
      }
    }
    return Fl_Input::handle(e);
  }
};

class RunWindow : public sde::BufferedWindow {
 public:
  RunWindow(int w, int h) : sde::BufferedWindow(w, h, _("Run Command")) {}
  int handle(int e) override {
    int r = sde::BufferedWindow::handle(e);
    if (!r || (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && !Fl::event_inside(g_input)))
      if (handle_about_event(e, app_info())) return 1;
    return r;
  }
  void draw() override {
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), g_theme.background);
      p.image(IconLoader::get({"system-run", "utilities-terminal", "application-x-executable"}, S(40)), S(20),
              S(20), S(40));
      p.fill_rounded(S(76), S(52), w() - S(96), S(40), S(10), g_theme.surface);
      p.stroke_rounded(S(76), S(52), w() - S(96), S(40), S(10), g_theme.accent, Sd(1.5));
    }
    text_font(g_theme.font_size + 2, true);
    draw_text(_("Run a command"), S(76), S(16), w() - S(96), S(28), g_theme.text);
    draw_children();
    text_font(g_theme.font_size - 1);
    draw_text(_("Enter a program name or a full command. Ctrl+Enter runs it in a terminal."), S(76), S(98),
              w() - S(96), S(22), g_theme.text_dim);
  }
};

bool run(const std::string &cmd, bool in_terminal) {
  auto argv = split_command(cmd);
  if (argv.empty()) return false;
  // Plain URLs and paths open with the default application.
  if (argv.size() == 1 && (argv[0].find("://") != std::string::npos ||
                           (argv[0][0] == '/' && !is_executable(argv[0])) ||
                           (argv[0][0] == '~'))) {
    std::string target = argv[0][0] == '~' ? home_dir() + argv[0].substr(1) : argv[0];
    return open_uri(target);
  }
  if (in_terminal) return open_terminal(argv);
  if (find_program(argv[0]).empty()) {
    fl_alert(_("Command not found: %s"), argv[0].c_str());
    return false;
  }
  return spawn_detached(argv);
}

void on_run(Fl_Widget *, void *data) {
  std::string cmd = trim(g_input->value());
  if (cmd.empty()) return;
  bool term = Fl::event_state(FL_CTRL) != 0;
  if (run(cmd, term)) {
    save_history(cmd);
    static_cast<Fl_Window *>(data)->hide();
  }
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  sde::set_default_window_icon({"sde-run"});  // title bar and taskbar
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) return print_version(app_info());
    printf("Usage: sde-run [--version]\n");
    return strcmp(argv[i], "--help") ? 2 : 0;
  }
  fl_open_display();
  Fl::visible_focus(1);
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  load_history();

  int W = S(560);
  auto *win = new RunWindow(W, S(172));
  g_input = new RunInput(S(86), S(57), W - S(116), S(30));
  g_input->box(FL_FLAT_BOX);
  g_input->color(g_theme.surface.fl());
  g_input->textcolor(g_theme.text.fl());
  g_input->textsize(g_theme.font_size + 2);
  g_input->cursor_color(g_theme.accent.fl());
  g_input->when(FL_WHEN_ENTER_KEY_ALWAYS);
  g_input->callback(on_run, win);
  auto *cancel = new FlatButton(W - S(236), S(126), S(104), S(34), _("Cancel"));
  cancel->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  auto *ok = new FlatButton(W - S(124), S(126), S(104), S(34), _("Run"), true);
  ok->callback(on_run, win);
  win->end();
  win->xclass("sde-run");
  // Center on the primary screen.
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, 0);
  win->position(sx + (sw - win->w()) / 2, sy + sh / 3 - win->h() / 2);
  win->show();
  g_input->take_focus();
  return Fl::run();
}
