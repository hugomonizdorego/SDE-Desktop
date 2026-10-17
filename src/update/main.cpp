// SPDX-License-Identifier: GPL-3.0-or-later
// SDE Update: native UI over a mintupdate-derived model and Debian PackageKit.
#include <FL/Fl.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/fl_ask.H>
#include <FL/x.H>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <algorithm>

#include <cstdio>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "paint.h"
#include "process.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

using namespace sde;
namespace {

// No package names, options, repository URLs or shell commands are accepted.
// PackageKit handles dependency solving, locks and the system's polkit policy.
std::vector<std::string> transaction(const std::string &action) {
  if (action == "refresh") return {"/usr/bin/pkcon", "refresh", "force"};
  if (action == "upgrade") return {"/usr/bin/pkcon", "update"};
  return {};
}

int run_transaction(const std::string &action, bool plan) {
  auto command = transaction(action);
  if (command.empty()) return 2;
  if (plan) {
    for (auto &arg : command) printf("%s\n", arg.c_str());
    return 0;
  }
  // Unlike the metadata process helpers, a terminal transaction needs stdin
  // and stderr intact for confirmation, authentication and useful diagnostics.
  pid_t child = fork();
  if (child < 0) return 1;
  if (child == 0) {
    std::vector<char *> args;
    for (auto &arg : command) args.push_back(const_cast<char *>(arg.c_str()));
    args.push_back(nullptr);
    execv(args[0], args.data());
    perror("SDE Update");
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return 1;
  }
  int result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  printf("\nSDE Update: %s (exit %d). Press Enter to close.\n", result == 0 ? "completed" : "not completed",
         result);
  fflush(stdout);
  if (isatty(STDIN_FILENO)) {
    char line[8];
    if (!fgets(line, sizeof line, stdin)) {
    }
  }
  return result;
}

std::string browser_text(std::string value) {
  std::string result;
  for (char c : value) {
    if (c == '@') result += '@';
    result += c;
  }
  return result;
}

class UpdateWindow : public BufferedWindow {
 public:
  UpdateWindow(int w, int h) : BufferedWindow(w, h, _("SDE Update")), theme_(active_theme()) {
    int gap = S(16), bh = S(34), bw = (w - 4 * gap) / 3;
    check_ = new FlatButton(gap, S(78), bw, bh, _("Check Again"));
    refresh_ = new FlatButton(2 * gap + bw, S(78), bw, bh, _("Refresh Sources"));
    install_ = new FlatButton(3 * gap + 2 * bw, S(78), bw, bh, _("Install Updates"), true);
    check_->callback([](Fl_Widget *, void *p) { static_cast<UpdateWindow *>(p)->load(); }, this);
    refresh_->callback([](Fl_Widget *, void *p) { static_cast<UpdateWindow *>(p)->start("refresh"); }, this);
    install_->callback([](Fl_Widget *, void *p) { static_cast<UpdateWindow *>(p)->start("upgrade"); }, this);
    list_ = new Fl_Hold_Browser(gap, S(164), w - 2 * gap, h - S(238));
    list_->box(FL_FLAT_BOX);
    list_->color(theme_.surface.fl());
    list_->selection_color(theme_.accent.fl());
    list_->textcolor(theme_.text.fl());
    list_->textsize(theme_.font_size);
    list_->textfont(FL_HELVETICA);
    auto *source = new FlatButton(gap, h - S(51), S(110), S(30), "mintupdate");
    source->callback([](Fl_Widget *, void *) { open_uri("https://github.com/linuxmint/mintupdate"); });
    end();
    resizable(nullptr);
    xclass("sde-update");
    callback(
        [](Fl_Widget *w, void *p) {
          // Keep callback targets alive while asynchronous jobs finish.
          if (!static_cast<UpdateWindow *>(p)->busy_) w->hide();
        },
        this);
  }

  void draw() override {
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), theme_.background);
    }
    text_font(theme_.font_size + S(8), true);
    draw_text(_("SDE Update"), S(16), S(14), w() - S(32), S(31), theme_.text);
    text_font(theme_.font_size);
    draw_text(_("DebION GNU/Linux · System updates"), S(16), S(45), w() - S(32), S(24), theme_.text_dim);
    draw_text(summary_, S(16), S(122), w() - S(32), S(32), theme_.text);
    text_font(theme_.font_size - S(1));
    draw_text(_("Review the final package changes in the terminal."), S(142), h() - S(60), w() - S(158),
              S(24), theme_.text_dim);
    draw_text(_("Authentication is handled by the system."), S(142), h() - S(37), w() - S(158), S(24),
              theme_.text_dim);
    draw_children();
  }

  void load() {
    set_busy(true);
    install_->deactivate();
    summary_ = _("Reading local APT metadata…");
    redraw();
    bool started = run_async(
        {"/usr/bin/python3", std::string(SDE_DATADIR) + "/update/check.py"},
        [this](const std::string &output, int status) {
          list_->clear();
          auto lines = split(output, '\n');
          int count = 0;
          bool valid = status == 0 && !lines.empty() && lines[0] == "SDE-UPDATES\t1";
          for (size_t i = 1; valid && i < lines.size(); ++i) {
            if (lines[i].empty()) continue;
            auto fields = split(lines[i], '\t');
            if (fields.size() != 6) {
              valid = false;
              break;
            }
            list_->add(("@b" + browser_text(fields[1] + "  [" + fields[0] + "]")).c_str());
            list_->add(browser_text("  " + fields[2] + " -> " + fields[3]).c_str());
            list_->add(browser_text("  " + fields[5] + "  (" + fields[4] + " bytes)").c_str());
            list_->add("");
            ++count;
          }
          if (!valid) {
            list_->clear();
            summary_ = _("Cannot read APT metadata. Check python3-apt and your package sources.");
          } else {
            summary_ = count
                           ? format(_("%d source packages have updates. Held packages are excluded."), count)
                           : _("No updates in the local cache. Refresh Sources to check the repositories.");
          }
          set_busy(false);
          if (valid && count) install_->activate();
          redraw();
        });
    if (!started) {
      summary_ = _("Python 3 is missing. Reinstall the sde package and its dependencies.");
      set_busy(false);
      redraw();
    }
  }

 private:
  void set_busy(bool busy) {
    busy_ = busy;
    if (busy) {
      check_->deactivate();
      refresh_->deactivate();
      install_->deactivate();
    } else {
      check_->activate();
      refresh_->activate();
    }
  }
  void start(const std::string &action) {
    if (busy_) return;
    if (find_program("xterm").empty() || !is_executable("/usr/bin/pkcon")) {
      fl_alert("%s", _("Install xterm and packagekit-tools to run update transactions."));
      return;
    }
    set_busy(true);
    summary_ = _("Complete or cancel the transaction in the terminal, then press Enter there.");
    redraw();
    run_async({"xterm", "-T", "SDE Update", "-e", "sde-update", "--transaction", action},
              [this](const std::string &, int) { load(); });
  }
  Theme theme_;
  FlatButton *check_, *refresh_, *install_;
  Fl_Hold_Browser *list_;
  bool busy_ = false;
  std::string summary_;
};
}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  if (argc == 2 && std::string(argv[1]) == "--version") {
    printf("sde-update %s\n", SDE_VERSION);
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--check") {
    execl("/usr/bin/python3", "python3", (std::string(SDE_DATADIR) + "/update/check.py").c_str(),
          static_cast<char *>(nullptr));
    perror("sde-update");
    return 127;
  }
  if (argc == 3 && (std::string(argv[1]) == "--transaction" || std::string(argv[1]) == "--plan"))
    return run_transaction(argv[2], std::string(argv[1]) == "--plan");
  if (argc != 1) {
    printf(
        "Usage: sde-update [--check | --version | --plan refresh|upgrade | --transaction refresh|upgrade]\n");
    return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 2;
  }
  Config config;
  config.load();
  fl_open_display();
  auto theme = Theme::from_config(config);
  theme.apply_to_fltk();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int w = std::min(S(860), sw - S(24)), h = std::min(S(600), sh - S(60));
  auto *window = new UpdateWindow(w, h);
  window->position(sx + (sw - w) / 2, sy + (sh - h) / 2);
  window->show();
  window->load();
  return Fl::run();
}
