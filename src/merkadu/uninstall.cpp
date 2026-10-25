// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// `merkadu --uninstall FILE`: the Start menu's "Uninstall..." of an app,
// given the absolute path of its desktop file. A small window finds where
// the app comes from (the Debian package that owns the file, the Flatpak
// of X-Flatpak=, the snap of X-SnapInstanceName= or a launcher in the
// user's home), asks (icon, name, origin, space freed, the other packages
// removed with it), removes it with the animated SDE logo and reports how
// it went. Apps of SDE's own packages are not removed on their own, nor
// apps whose removal would take a protected package (the desktop) away.
//
// Exit status: 0 removed, 1 failed or refused, 2 cancelled.
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "about.h"
#include "buffered_window.h"
#include "i18n.h"
#include "icons.h"
#include "image_cache.h"
#include "logo_anim.h"
#include "store.h"
#include "ui.h"
#include "util.h"
#include "window.h"
#include "wm.h"

namespace sde {
namespace merkadu {

namespace {

enum class Phase { Checking, Confirm, Refused, Working, Done, Failed };

struct State {
  Phase phase = Phase::Checking;
  Launcher launcher;
  std::string title;     // the app's name
  std::string icon;
  Source source = Source::Debian;
  std::string target;    // package, "system:REF" or snap name
  std::string origin;    // "Debian package gimp"
  long long freed = -1;
  std::vector<std::string> also;  // other packages removed with it
  std::string message;   // refusal or error
  int job = 0;
  int status = 2;        // exit status
  bool user_launcher = false;
};

State g;
class Dialog;
Dialog *g_dialog = nullptr;

int dialog_w() { return S(500); }

class Dialog : public BufferedWindow {
 public:
  Dialog(int w, int h) : BufferedWindow(w, h, _("Uninstall")) {}

  void draw() override;
  int handle(int e) override;
  void layout();
  void qa_dump();

  Button *cancel = nullptr, *ok = nullptr;
  LogoAnimation *logo = nullptr;
};

std::string headline() {
  switch (g.phase) {
    case Phase::Checking: return format(_("Checking %s\xE2\x80\xA6"), g.title.c_str());
    case Phase::Confirm:
      return g.user_launcher ? format(_("Remove the launcher of %s?"), g.title.c_str())
                             : format(_("Uninstall %s?"), g.title.c_str());
    case Phase::Refused: return format(_("%s cannot be uninstalled"), g.title.c_str());
    case Phase::Working: return format(_("Uninstalling %s\xE2\x80\xA6"), g.title.c_str());
    case Phase::Done:
      return g.user_launcher ? format(_("The launcher of %s was removed."), g.title.c_str())
                             : format(_("%s was uninstalled."), g.title.c_str());
    case Phase::Failed: return format(_("%s could not be uninstalled"), g.title.c_str());
  }
  return "";
}

// The text below the headline.
std::string body() {
  std::string out;
  switch (g.phase) {
    case Phase::Checking:
      return _("Finding out where it was installed from.");
    case Phase::Confirm:
      if (g.user_launcher)
        return _("It was not installed from a package: only its launcher in your home folder is removed. Files "
                 "it may have elsewhere stay.");
      out = g.origin;
      if (g.freed > 0) out += "\n" + format(_("This frees %s of disk space."), format_size(g.freed).c_str());
      if (!g.also.empty()) {
        out += "\n\n" + std::string(_("These packages are removed with it:")) + "\n";
        size_t shown = std::min<size_t>(g.also.size(), 10);
        for (size_t i = 0; i < shown; ++i) out += g.also[i] + (i + 1 < shown ? ", " : "");
        if (g.also.size() > shown) out += format(_(" and %d more"), int(g.also.size() - shown));
      }
      return out;
    case Phase::Refused:
    case Phase::Failed:
      return g.message;
    case Phase::Working:
      return g.origin;
    case Phase::Done:
      return g.freed > 0 ? format(_("%s of disk space were freed."), format_size(g.freed).c_str()) : "";
  }
  return "";
}

int body_h(int w) { return TextBlock::measure(body(), w, 0); }

void Dialog::layout() {
  int W = dialog_w(), pad = S(24);
  bool working = g.phase == Phase::Working || g.phase == Phase::Checking;
  int text_w = W - 2 * pad - (working ? 0 : S(56) + S(16));
  int top = working ? S(20) + S(96) + S(16) : pad;
  int h = top + S(30) + S(8) + body_h(text_w) + S(12);
  if (g.phase == Phase::Working) h += S(28);
  h = std::max(h, pad + S(56) + S(12)) + S(34) + pad;
  if (h != this->h()) size(W, h);
  logo->resize((W - S(96)) / 2, S(20), S(96), S(96));
  if (working) {
    logo->show();
    logo->running(g.phase == Phase::Working);
  } else {
    logo->running(false);
    logo->hide();
  }
  int by = h - pad - S(34);
  // The buttons of each phase.
  ok->hide();
  cancel->hide();
  switch (g.phase) {
    case Phase::Checking:
      cancel->text(_("Cancel"));
      cancel->show();
      break;
    case Phase::Confirm:
      ok->text(g.user_launcher ? _("Remove Launcher") : _("Uninstall"));
      ok->kind(Button::Danger);
      ok->show();
      cancel->text(_("Cancel"));
      cancel->show();
      break;
    case Phase::Working: {
      const Job *j = Store::get().job(g.job);
      if (j && (j->cancellable || j->state == Job::Queued)) {
        cancel->text(_("Cancel"));
        cancel->show();
      }
      break;
    }
    default:
      ok->text(_("Close"));
      ok->kind(Button::Primary);
      ok->show();
      break;
  }
  int okw = Button::fit(ok->text()), cw = Button::fit(cancel->text());
  ok->resize(W - pad - okw, by, okw, S(34));
  cancel->resize(ok->visible() ? ok->x() - S(10) - cw : W - pad - cw, by, cw, S(34));
  redraw();
}

void Dialog::draw() {
  const Theme &t = active_theme();
  int W = w(), pad = S(24);
  bool working = g.phase == Phase::Working || g.phase == Phase::Checking;
  int tx = working ? pad : pad + S(56) + S(16), tw = W - tx - pad;
  int top = working ? S(20) + S(96) + S(16) : pad;
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.background);
    if (!working) {
      std::string fallback = "application-x-executable";
      p.image(images::icon(g.icon, S(56), fallback), pad, pad, S(56));
      if (g.phase == Phase::Done) {
        double cs = S(22);
        p.circle(pad + S(56) - cs / 2 + S(2), pad + S(56) - cs / 2 + S(2), cs / 2, t.success);
        paint_symbol(p, Symbol::Check, pad + S(56) - cs + S(5), pad + S(56) - cs + S(5), cs - S(6),
                     Color::hex(0xFFFFFF));
      } else if (g.phase == Phase::Failed || g.phase == Phase::Refused) {
        double cs = S(22);
        p.circle(pad + S(56) - cs / 2 + S(2), pad + S(56) - cs / 2 + S(2), cs / 2, t.warning);
        paint_symbol(p, Symbol::Warning, pad + S(56) - cs + S(5), pad + S(56) - cs + S(4), cs - S(6),
                     Color::hex(0xFFFFFF));
      }
    }
    if (g.phase == Phase::Working) {
      const Job *j = Store::get().job(g.job);
      int pct = j ? j->percent : -1;
      int bh = top + S(30) + S(8) + body_h(tw) + S(10);
      paint_progress(p, tx, bh, tw, S(8), pct, 0);
    }
  }
  text_font(t.font_size + 4, true);
  draw_text(headline(), tx, top, tw, S(30), t.text, working ? 1 : 0);
  std::string b = body();
  text_font(t.font_size);
  fl_color((g.phase == Phase::Failed ? readable(t.error, t.background) : t.text_dim).fl());
  int bh = body_h(tw);
  fl_draw(b.c_str(), tx, top + S(38), tw, bh,
          Fl_Align((working ? FL_ALIGN_TOP : FL_ALIGN_TOP_LEFT) | FL_ALIGN_WRAP | FL_ALIGN_INSIDE), nullptr, 0);
  if (g.phase == Phase::Working) {
    const Job *j = Store::get().job(g.job);
    std::string st = j ? (j->state == Job::Queued ? std::string(_("Waiting")) : j->status) : "";
    if (j && j->percent >= 0) st += format("  %d%%", j->percent);
    text_font(t.font_size - 1);
    draw_text(st, tx, top + S(38) + bh + S(22), tw, S(20), t.text_dim, 1);
  }
  draw_children();
  qa_dump();
}

void Dialog::qa_dump() {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  if (!dir || !*dir) return;
  static const char *names[] = {"checking", "confirm", "refused", "working", "done", "failed"};
  std::string text = std::string("@state:") + names[int(g.phase)] + "\t0\t0\t0\t0\t1\t0\n";
  text += "@source:" + std::string(source_id(g.source)) + ":" + g.target + "\t0\t0\t0\t0\t1\t0\n";
  for (auto &a : g.also) text += "@also:" + a + "\t0\t0\t0\t0\t1\t0\n";
  if (g.freed >= 0) text += "@freed:" + std::to_string(g.freed) + "\t0\t0\t0\t0\t1\t0\n";
  for (Button *b : {ok, cancel})
    if (b && b->visible())
      text += b->text() + "\t" + std::to_string(b->x()) + "\t" + std::to_string(b->y()) + "\t" +
              std::to_string(b->w()) + "\t" + std::to_string(b->h()) + "\t1\t0\n";
  if (logo && logo->visible())
    text += "logo\t" + std::to_string(logo->x()) + "\t" + std::to_string(logo->y()) + "\t" +
            std::to_string(logo->w()) + "\t" + std::to_string(logo->h()) + "\t1\t0\n";
  std::string path = std::string(dir) + "/Merkadu-Uninstall.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

void finish(int status) {
  g.status = status;
  if (g_dialog) g_dialog->hide();
}

void set_phase(Phase p) {
  g.phase = p;
  if (!g_dialog) return;
  g_dialog->layout();
  // The keyboard starts on the safe button: Cancel while asking, Close after.
  bool closing = p == Phase::Done || p == Phase::Failed || p == Phase::Refused;
  if (closing && g_dialog->ok->visible())
    g_dialog->ok->take_focus();
  else if (g_dialog->cancel->visible())
    g_dialog->cancel->take_focus();
}

void refuse(const std::string &message) {
  g.message = message;
  set_phase(Phase::Refused);
}

void ok_clicked() {
  switch (g.phase) {
    case Phase::Confirm:
      if (g.user_launcher) {
        if (unlink(g.launcher.path.c_str()) == 0) {
          set_phase(Phase::Done);
        } else {
          g.message = format(_("The launcher could not be removed: %s"), strerror(errno));
          set_phase(Phase::Failed);
        }
        return;
      }
      {
        Job j;
        j.action = Action::Remove;
        j.source = g.source;
        j.title = g.title;
        j.icon = g.icon;
        j.targets = {g.target};
        g.job = Store::get().enqueue(j);
      }
      if (!g.job) {
        g.message = _("The source of this app cannot remove software now.");
        set_phase(Phase::Failed);
        return;
      }
      set_phase(Phase::Working);
      return;
    case Phase::Done:
      finish(0);
      return;
    case Phase::Failed:
    case Phase::Refused:
      finish(1);
      return;
    default:
      return;
  }
}

void cancel_clicked() {
  if (g.phase == Phase::Working) {
    Store::get().cancel(g.job);
    return;
  }
  finish(2);
}

int Dialog::handle(int e) {
  bool key = e == FL_KEYBOARD || e == FL_SHORTCUT;
  if (key && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) &&
      (g.phase == Phase::Done || g.phase == Phase::Failed || g.phase == Phase::Refused)) {
    ok_clicked();  // Close is the default button
    return 1;
  }
  if (key && Fl::event_key() == FL_Escape) {
    if (g.phase == Phase::Working) return 1;
    if (g.phase == Phase::Done) finish(0);
    else if (g.phase == Phase::Failed || g.phase == Phase::Refused) finish(1);
    else finish(2);
    return 1;
  }
  int r = BufferedWindow::handle(e);
  if (!r && handle_about_event(e, about_info())) return 1;
  return r;
}

// Debian: the package that owns the launcher, what goes with it.
void check_package() {
  Store &st = Store::get();
  std::string path = g.launcher.path;
  st.backend(Source::Debian)->owners({path}, [path](std::map<std::string, std::vector<std::string>> found) {
    auto it = found.find(path);
    if (it == found.end() || it->second.empty()) {
      refuse(_("No installed package contains this launcher, so Merkadu cannot uninstall it. It may have been "
               "installed by hand or by another program."));
      return;
    }
    std::string pkg = it->second.front();
    g.source = Source::Debian;
    g.target = pkg;
    Origin o;
    o.source = Source::Debian;
    o.id = pkg;
    g.origin = origin_line(o);
    auto prot = Store::get().settings().protected_packages;
    if (prot.count(pkg)) {
      refuse(format(_("%s is part of the Selviana Desktop Environment (package %s). It is installed and removed "
                      "together with the desktop, not on its own."),
                    g.title.c_str(), pkg.c_str()));
      return;
    }
    Store::get().backend(Source::Debian)->plan_remove({pkg}, [pkg, prot](std::vector<std::string> removed,
                                                                          long long freed, std::string error) {
      if (!error.empty()) {
        g.message = error;
        set_phase(Phase::Failed);
        return;
      }
      RemovalPlan plan = plan_removal(pkg, removed, prot);
      if (!plan.allowed) {
        refuse(format(_("Removing %s would also remove %s, which the Selviana Desktop Environment needs."),
                      g.title.c_str(), plan.protected_package.c_str()));
        return;
      }
      g.also = plan.also_removed;
      g.freed = freed;
      set_phase(Phase::Confirm);
    });
  });
}

// Flatpak and Snap: the installed ref or snap.
void check_bundle() {
  Store &st = Store::get();
  Source src = g.launcher.kind == LauncherKind::Flatpak ? Source::Flatpak : Source::Snap;
  if (!st.installed_loaded(src)) return;  // called again when it is loaded
  g.source = src;
  const Origin *found = nullptr;
  for (auto &a : st.catalog().apps())
    for (auto &o : a.origins) {
      if (!o.installed || o.source != src) continue;
      bool match = src == Source::Flatpak
                       ? o.flatpak_app_id() == g.launcher.id &&
                             (o.installation.empty() ? "system" : o.installation) == g.launcher.flatpak_installation
                       : o.id == g.launcher.id;
      if (match && !found) found = &o;
    }
  if (!found) {
    refuse(src == Source::Flatpak
               ? format(_("The Flatpak %s is not installed (any more)."), g.launcher.id.c_str())
               : format(_("The snap %s is not installed (any more)."), g.launcher.id.c_str()));
    return;
  }
  g.target = src == Source::Flatpak
                 ? std::string(found->installation.empty() ? "system" : found->installation) + ":" + found->id
                 : found->id;
  g.origin = origin_line(*found);
  g.freed = found->installed_size;
  set_phase(Phase::Confirm);
}

}  // namespace

int run_uninstall(const std::string &desktop_file) {
  std::string text;
  bool readable_file = read_file(desktop_file, text);
  g.launcher = classify_launcher(desktop_file, text, home_dir());
  g.title = !g.launcher.name.empty() ? g.launcher.name : basename_of(desktop_file);
  g.icon = g.launcher.icon;
  Store &st = Store::get();
  st.start(false);

  auto *win = new Dialog(dialog_w(), S(220));
  g_dialog = win;
  win->logo = new LogoAnimation(0, 0, S(96), S(96));
  win->logo->tooltip(_("Working"));
  win->cancel = new Button(0, 0, S(100), S(34), _("Cancel"));
  win->cancel->on_click = cancel_clicked;
  win->ok = new Button(0, 0, S(100), S(34), _("Uninstall"), Button::Danger);
  win->ok->on_click = ok_clicked;
  win->end();
  win->xclass("merkadu");
  win->callback([](Fl_Widget *, void *) {
    // The close button of the frame: like Cancel or Close.
    if (g.phase == Phase::Working) {
      Store::get().cancel(g.job);
      return;
    }
    finish(g.phase == Phase::Done ? 0 : g.phase == Phase::Failed || g.phase == Phase::Refused ? 1 : 2);
  });
  win->layout();
  int wx = 0, wy = 0, ww = win->w(), wh = win->h();
  fit_to_work_area(wx, wy, ww, wh);
  win->position(wx, wy);

  st.on_job_done = [](const Job &j) {
    if (j.id != g.job) return;
    if (j.state == Job::Done) {
      set_phase(Phase::Done);
    } else if (j.state == Job::Cancelled) {
      finish(2);
    } else {
      g.message = j.error.empty() ? std::string(_("The removal failed.")) : j.error;
      set_phase(Phase::Failed);
    }
  };
  st.listen([](Event e) {
    if (!g_dialog) return;
    if (e == Event::Progress || e == Event::Jobs) {
      if (g.phase == Phase::Working) g_dialog->layout();
      g_dialog->redraw();
    }
    if (e == Event::Catalog && g.phase == Phase::Checking &&
        (g.launcher.kind == LauncherKind::Flatpak || g.launcher.kind == LauncherKind::Snap))
      check_bundle();
  });

  if (!readable_file) {
    g.message = format(_("The file %s cannot be read."), desktop_file.c_str());
    set_phase(Phase::Failed);
  } else {
    switch (g.launcher.kind) {
      case LauncherKind::Invalid:
        g.message = format(_("%s is not an application launcher (a .desktop file)."), desktop_file.c_str());
        set_phase(Phase::Failed);
        break;
      case LauncherKind::UserLauncher:
        g.user_launcher = true;
        g.origin = desktop_file;
        set_phase(Phase::Confirm);
        break;
      case LauncherKind::Package:
        check_package();
        break;
      default:
        check_bundle();
        break;
    }
  }
  win->show();
  while (win->shown()) Fl::wait();
  return g.status;
}

}  // namespace merkadu
}  // namespace sde
