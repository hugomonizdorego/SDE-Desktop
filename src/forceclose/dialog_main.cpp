// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-force-close: "Wait / Force Close" for a program that stopped
// responding or never opened, Force Close from the taskbar, and the
// notification after a crash (0.2.0 Alpha 3.1, docs/FORCE-CLOSE.md).
//
//   sde-force-close --window XID [--reason frozen|closing]
//       The dialog for a window that does not answer _NET_WM_PING
//       (sde-watchdog after the timeout, SDE WM after a close attempt).
//   sde-force-close --launch PID
//       The dialog for a program SDE started that has shown no window.
//   sde-force-close --kill-window XID
//       Force Close at once (the taskbar button's menu).
//   sde-force-close --check-window XID | --check-pid PID
//       Says whether Force Close would end it, and why not; ends nothing.
//   sde-force-close --crashed --name NAME [--icon ICON] [--desktop-id ID]
//                   [--signal N] [-- COMMAND...]
//       "<App> closed unexpectedly" with Open Again and Details (sde-session).
//
// The dialog closes by itself when the program answers again, when the
// window goes away, or when the program opens a window. Exit status: 0
// forced to close, 1 Wait, 2 resolved by itself, 3 already asked (the
// dialog that is open was raised), 4 refused (the program may not be ended:
// a part of the session, another user's...), 64 usage.
#define FL_INTERNALS 1
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "buffered_window.h"
#include "config.h"
#include "crash.h"
#include "dbus.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "launches.h"
#include "paint.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"
#include "x11.h"
#include "xtarget.h"

using namespace sde;

namespace {

enum Exit { FORCED = 0, WAITED = 1, RESOLVED = 2, ALREADY = 3, REFUSED = 4, USAGE = 64 };

Theme g_theme;
forceclose::Context g_ctx;

struct Target {
  bool launch = false;
  Window win = 0;
  bool closing = false;
  bool marked = false;  // _SDE_NOT_RESPONDING was set when the dialog opened
  launches::Record record;
  std::string name;
  cairo_surface_t *icon = nullptr;
  unsigned long ping_id = 0;
  double started = 0;
} g;

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Window parse_window(const char *s) { return Window(strtoul(s, nullptr, 0)); }

// " - [Not Responding]" and " - [Killing...]": SDE WM's (Openbox's) title suffixes.
std::string plain_title(std::string t) {
  for (const char *suffix : {" - [Not Responding]", " - [Killing...]"}) {
    std::string s = suffix;
    if (t.size() > s.size() && t.compare(t.size() - s.size(), s.size(), s) == 0) t.resize(t.size() - s.size());
  }
  return t;
}

// The application of a window: its desktop file (by WM_CLASS, as the
// panel's taskbar finds it), else its title and icon.
void identify_window(Window w, int size) {
  XClassHint hint;
  std::string res_name, res_class;
  if (XGetClassHint(fl_display, w, &hint)) {
    res_name = hint.res_name ? hint.res_name : "";
    res_class = hint.res_class ? hint.res_class : "";
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
  }
  DesktopEntry entry;
  bool found = false;
  for (const std::string &n : {lower(res_name), lower(res_class)})
    if (!n.empty() && find_application(n + ".desktop", entry)) {
      found = true;
      break;
    }
  if (!found && (!res_name.empty() || !res_class.empty())) {
    std::string cls = lower(res_class), name = lower(res_name);
    for (const auto &e : load_applications("SDE")) {
      std::string wm = lower(e.startup_wm_class);
      auto cmd = e.command();
      std::string exec = cmd.empty() ? "" : lower(basename_of(cmd[0]));
      if ((!wm.empty() && (wm == cls || wm == name)) || (!exec.empty() && (exec == name || exec == cls)) ||
          (!cls.empty() && ends_with(lower(e.id), "." + cls + ".desktop"))) {
        entry = e;
        found = true;
        break;
      }
    }
  }
  if (found) {
    g.name = entry.name;
    g.icon = IconLoader::get({IconLoader::app_icon(entry, size), "application-x-executable"}, size);
  }
  if (g.name.empty()) g.name = plain_title(x11::window_title(w));
  if (g.name.empty()) g.name = res_class.empty() ? std::string(_("The application")) : res_class;
  if (!g.icon) g.icon = x11::window_icon(w, size);
  if (!g.icon) g.icon = IconLoader::get({"application-x-executable", "application-default-icon"}, size);
}

void identify_launch(int size) {
  DesktopEntry entry;
  if (!g.record.desktop_id.empty() && find_application(g.record.desktop_id, entry)) {
    g.name = entry.name;
    g.icon = IconLoader::get({IconLoader::app_icon(entry, size), "application-x-executable"}, size);
  }
  if (g.name.empty()) g.name = g.record.name;
  if (g.name.empty() && !g.record.argv.empty()) g.name = basename_of(g.record.argv[0]);
  if (!g.icon && !g.record.icon.empty())
    g.icon = IconLoader::get({g.record.icon, "application-x-executable"}, size);
  if (!g.icon) g.icon = IconLoader::get({"application-x-executable", "application-default-icon"}, size);
}

std::string heading() {
  return format(g.launch ? _("%s has not opened yet") : _("%s is not responding"), g.name.c_str());
}

std::string body() {
  if (!g.launch)
    return _("You can wait for it or force it to close; unsaved changes will be lost.");
  long age = std::max(0L, long(time(nullptr) - g.record.time));
  std::string started =
      age < 90 ? format(ngettext("It started %ld second ago but has not shown a window.",
                                 "It started %ld seconds ago but has not shown a window.",
                                 (unsigned long)age),
                        age)
               : format(ngettext("It started %ld minute ago but has not shown a window.",
                                 "It started %ld minutes ago but has not shown a window.",
                                 (unsigned long)(age / 60)),
                        age / 60);
  return started + " " + _("You can wait for it or force it to close.");
}

std::vector<std::string> wrap(const std::string &text, int width) {
  std::vector<std::string> lines;
  std::string line, word;
  auto flush_word = [&] {
    if (word.empty()) return;
    std::string next = line.empty() ? word : line + " " + word;
    if (!line.empty() && text_width(next) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = next;
    }
    word.clear();
  };
  for (char c : text) {
    if (c == ' ') flush_word();
    else word += c;
  }
  flush_word();
  if (!line.empty()) lines.push_back(line);
  return lines;
}

// Force Close: the theme's error colour, white (or dark) text.
class DangerButton : public Fl_Button {
 public:
  DangerButton(int x, int y, int w, int h, const std::string &text) : Fl_Button(x, y, w, h), text_(text) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    copy_tooltip(text.c_str());
  }
  void draw() override {
    const Theme &t = active_theme();
    bool hot = Fl::belowmouse() == this, down = value() != 0;
    Color bg = t.error;
    if (hot) bg = bg.mix(t.dark ? Color::hex(0xFFFFFF) : Color::hex(0x000000), 0.10);
    if (down) bg = bg.mix(Color::hex(0x000000), 0.18);
    Color white = Color::hex(0xFFFFFF);
    Color fg = white.contrast(t.error) >= 3.0 ? white : Color::hex(0x101820);
    double r = t.radius_for(h());
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), r, bg);
      p.surface_edge(x(), y(), w(), h(), r, bg.mix(Color::hex(0x000000), 0.16), down ? 0.0 : 0.25, false);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
    }
    text_font(t.font_size, true);
    draw_text(text_, x() + S(8), y(), w() - S(16), h(), fg, 1);
  }
  int handle(int e) override {
    if (e == FL_ENTER || e == FL_LEAVE || e == FL_FOCUS || e == FL_UNFOCUS) redraw();
    if (e == FL_KEYBOARD && Fl::focus() == this &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      do_callback();
      return 1;
    }
    return Fl_Button::handle(e);
  }

 private:
  std::string text_;
};

class Dialog : public BufferedWindow {
 public:
  Dialog(int w, int h, const std::string &title) : BufferedWindow(w, h) { copy_label(title.c_str()); }
  void draw() override {
    const Theme &t = g_theme;
    int pad = S(20), icon = S(48), tx = pad + icon + S(16), tw = w() - tx - pad;
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      if (g.icon) p.image(g.icon, pad, pad, icon);
    }
    text_font(t.font_size + S(3), true);
    int y = pad;
    for (const auto &line : wrap(heading(), tw)) {
      draw_text(line, tx, y, tw, S(26), t.text);
      y += S(26);
    }
    y += S(4);
    text_font(t.font_size);
    for (const auto &line : wrap(body(), tw)) {
      draw_text(line, tx, y, tw, S(20), t.text_dim);
      y += S(20);
    }
    draw_children();
    qa_dump();
  }
  int handle(int e) override {
    if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
      finish(WAITED);
      return 1;
    }
    return BufferedWindow::handle(e);
  }
  // The controls for the tests ($SDE_FORCE_CLOSE_QA_DIR/force-close.txt, in
  // the panel's QA format: name, x, y, w, h, active).
  void qa_dump() {
    const char *dir = getenv("SDE_FORCE_CLOSE_QA_DIR");
    if (!dir || !*dir) return;
    std::string text = "@title:" + std::string(label()) + "\t0\t0\t0\t0\t1\n";
    text += std::string("@mode:") + (g.launch ? "launch" : g.closing ? "closing" : "frozen") + "\t0\t0\t0\t0\t1\n";
    for (int i = 0; i < children(); ++i) {
      Fl_Widget *c = child(i);
      if (!c->tooltip()) continue;
      text += std::string(c->tooltip()) + "\t" + std::to_string(c->x()) + "\t" + std::to_string(c->y()) + "\t" +
              std::to_string(c->w()) + "\t" + std::to_string(c->h()) + "\t1\n";
    }
    write_file_atomic(std::string(dir) + "/force-close.txt", text);
  }
  static void finish(int code);
};

Dialog *g_dialog = nullptr;
Window g_owner = 0;  // the single-instance selection's window

void Dialog::finish(int code) {
  if (g_dialog) g_dialog->hide();
  XFlush(fl_display);
  exit(code);
}

void force_close(Fl_Widget *, void *) {
  int code = REFUSED;
  if (g.launch) {
    // The program and everything it started: it never got as far as a window.
    auto fam = forceclose::family(g.record.pid);
    int killed = 0;
    for (long pid : fam) {
      forceclose::Verdict v = forceclose::kill_now(pid, g_ctx, pid == g.record.pid ? g.record.start : 0);
      if (v == forceclose::Verdict::OK) ++killed;
      else if (pid == g.record.pid) fprintf(stderr, "sde-force-close: %ld: %s\n", pid, forceclose::verdict_name(v));
    }
    code = killed ? FORCED : REFUSED;
  } else {
    long pid = 0;
    forceclose::Verdict v = forceclose::force_close_window(fl_display, g.win, g_ctx, false, &pid);
    if (v != forceclose::Verdict::OK)
      fprintf(stderr, "sde-force-close: window 0x%lx (process %ld): %s\n", g.win, pid, forceclose::verdict_name(v));
    code = v == forceclose::Verdict::OK ? FORCED : REFUSED;
  }
  Dialog::finish(code);
}

void wait_cb(Fl_Widget *, void *) { Dialog::finish(WAITED); }

Atom atom(const char *name) { return XInternAtom(fl_display, name, False); }

bool window_exists(Window w) {
  XSync(fl_display, False);
  XErrorHandler old = XSetErrorHandler([](Display *, XErrorEvent *) { return 0; });
  XWindowAttributes a;
  bool ok = XGetWindowAttributes(fl_display, w, &a) != 0;
  XSync(fl_display, False);
  XSetErrorHandler(old);
  return ok;
}

bool marked(Window w) { return !x11::get_cardinals(w, forceclose::kNotResponding).empty(); }

// Resolved by itself? Checked every second; a window is pinged every two.
void watch(void *) {
  if (g.launch) {
    if (!launches::alive(g.record) || forceclose::family_shows_window(fl_display, g.record.pid))
      Dialog::finish(RESOLVED);
  } else {
    if (!window_exists(g.win)) Dialog::finish(RESOLVED);
    // The watchdog or the window manager saw it answer.
    if (g.marked && !marked(g.win)) Dialog::finish(RESOLVED);
    if (!g.marked && marked(g.win)) g.marked = true;
    static int ticks = 0;
    if (++ticks % 2 == 0) {
      g.ping_id = 0x5DF00000UL | ((g.ping_id + 1) & 0xFFFFFUL);
      forceclose::send_ping(fl_display, g.win, g.ping_id);
    }
  }
  XFlush(fl_display);
  Fl::repeat_timeout(1.0, watch);
}

int on_event(void *event, void *) {
  const XEvent &e = *static_cast<const XEvent *>(event);
  Window w = 0;
  // Any answer from the window: its program handles events again.
  if (!g.launch && forceclose::is_pong(fl_display, e, &w, nullptr) && w == g.win) Dialog::finish(RESOLVED);
  // The dialog that is open already is asked to come forward.
  if (e.type == SelectionRequest || e.type == SelectionClear) return 0;
  return 0;
}

std::string selection_name() {
  char buf[64];
  if (g.launch) snprintf(buf, sizeof buf, "_SDE_FORCE_CLOSE_P%ld", g.record.pid);
  else snprintf(buf, sizeof buf, "_SDE_FORCE_CLOSE_W%lx", g.win);
  return buf;
}

// One dialog per window (or program): a selection owned by a small window
// of this process, which names the dialog's own window.
bool claim() {
  Atom sel = atom(selection_name().c_str());
  Window other = XGetSelectionOwner(fl_display, sel);
  if (other != None) {
    Window dialog = x11::get_window(other, "_SDE_FORCE_CLOSE_DIALOG");
    if (dialog) x11::activate_window(dialog);
    XFlush(fl_display);
    return false;
  }
  g_owner = XCreateSimpleWindow(fl_display, DefaultRootWindow(fl_display), -10, -10, 1, 1, 0, 0, 0);
  XSetSelectionOwner(fl_display, sel, g_owner, CurrentTime);
  return XGetSelectionOwner(fl_display, sel) == g_owner;
}

int show_dialog() {
  fl_open_display();
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  g_ctx = forceclose::current_context();
  int icon = S(48);
  if (g.launch) {
    if (!launches::alive(g.record)) return RESOLVED;
    forceclose::Verdict v = forceclose::check(g.record.pid, g_ctx);
    if (v != forceclose::Verdict::OK) {
      fprintf(stderr, "sde-force-close: %ld: %s\n", g.record.pid, forceclose::verdict_name(v));
      return REFUSED;
    }
    identify_launch(icon);
  } else {
    if (!window_exists(g.win)) return RESOLVED;
    // Nothing to offer for a window Force Close may not end (the panel,
    // a part of the session, another user's program).
    long pid = 0;
    forceclose::Verdict v = forceclose::force_close_window(fl_display, g.win, g_ctx, true, &pid);
    if (v != forceclose::Verdict::OK) {
      fprintf(stderr, "sde-force-close: window 0x%lx (process %ld): %s\n", g.win, pid, forceclose::verdict_name(v));
      return REFUSED;
    }
    g.marked = marked(g.win);
    identify_window(g.win, icon);
  }
  if (!claim()) return ALREADY;

  int W = S(480), pad = S(20), bh = S(34), bw = S(124);
  // The height follows the text (at the dialog's width).
  text_font(g_theme.font_size + S(3), true);
  int tw = W - 2 * pad - S(48) - S(16);
  int lines_h = int(wrap(heading(), tw).size()) * S(26) + S(4);
  text_font(g_theme.font_size);
  lines_h += int(wrap(body(), tw).size()) * S(20);
  int H = pad + std::max(S(48), lines_h) + S(20) + bh + pad;
  auto *win = new Dialog(W, H, heading());
  g_dialog = win;
  auto *force = new DangerButton(W - pad - bw, H - pad - bh, bw, bh, _("Force Close"));
  force->callback(force_close);
  auto *wait = new FlatButton(W - pad - 2 * bw - S(10), H - pad - bh, bw, bh, _("Wait"));
  wait->copy_tooltip(_("Wait"));
  wait->callback(wait_cb);
  win->end();
  win->size_range(W, H, W, H);  // a fixed dialog: no maximize
  win->xclass("sde-force-close");
  // Over the program's window, else in the middle of the screen.
  int sx, sy, sw, sh, screen = 0;
  if (!g.launch) {
    Window child;
    int rx = 0, ry = 0;
    XWindowAttributes a;
    if (XGetWindowAttributes(fl_display, g.win, &a) &&
        XTranslateCoordinates(fl_display, g.win, DefaultRootWindow(fl_display), 0, 0, &rx, &ry, &child))
      screen = Fl::screen_num(rx + a.width / 2, ry + a.height / 2);
  }
  Fl::screen_work_area(sx, sy, sw, sh, screen);
  win->position(sx + (sw - W) / 2, sy + (sh - H) / 3);
  Fl_X::before_map = [](Fl_Window *w, Window xid) {
    if (w != g_dialog) return;
    x11::set_window_type(xid, "_NET_WM_WINDOW_TYPE_DIALOG");
    x11::set_atoms(xid, "_NET_WM_STATE", {"_NET_WM_STATE_ABOVE"});
  };
  Fl::visible_focus(1);
  win->show();
  wait->take_focus();
  if (fl_xid(win) && g_owner) {
    long id = long(fl_xid(win));
    XChangeProperty(fl_display, g_owner, atom("_SDE_FORCE_CLOSE_DIALOG"), XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<unsigned char *>(&id), 1);
  }
  // The window's answers to pings arrive at the root window.
  Window root = DefaultRootWindow(fl_display);
  XWindowAttributes ra;
  XGetWindowAttributes(fl_display, root, &ra);
  XSelectInput(fl_display, root, ra.your_event_mask | SubstructureNotifyMask);
  if (!g.launch) XSelectInput(fl_display, g.win, PropertyChangeMask | StructureNotifyMask);
  Fl::add_system_handler(on_event, nullptr);
  Fl::add_timeout(1.0, watch);
  // A first ping at once.
  if (!g.launch) {
    g.ping_id = 0x5DF00001UL;
    forceclose::send_ping(fl_display, g.win, g.ping_id);
  }
  g.started = now();
  Fl::run();
  return WAITED;
}

// -------------------------------------------------------------- crashes

struct Crash {
  std::string name, icon, desktop_id;
  int signal = 0;
  std::vector<std::string> argv;
  uint32_t id = 0;
} g_crash;

void reopen() {
  DesktopEntry entry;
  if (!g_crash.desktop_id.empty() && find_application(g_crash.desktop_id, entry)) launch_entry(entry);
  else if (!g_crash.argv.empty()) spawn_detached(g_crash.argv);
}

int notify_crash() {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return 1;
  bool reports = !find_program("sde-system-reports").empty();
  std::string summary = format(_("%s closed unexpectedly"), g_crash.name.c_str());
  std::string why = g_crash.signal > 0 ? strsignal(g_crash.signal) : "";
  std::string text = why.empty() ? std::string(_("It stopped because of an error."))
                                 : format(_("It stopped because of an error (%s)."), why.c_str());
  if (reports) text += std::string(" ") + _("System Reports has the details.");
  DBusMessage *m = dbus::method("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                "org.freedesktop.Notifications", "Notify");
  {
    dbus::Writer w(m);
    std::vector<std::string> actions = {"reopen", _("Open Again")};
    if (reports) actions.insert(actions.end(), {"details", _("Details")});
    w.str("SDE").u32(0).str(g_crash.icon.empty() ? "dialog-warning" : g_crash.icon).str(summary).str(text).strings(actions);
    {
      dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
      if (!g_crash.desktop_id.empty()) {
        std::string id = g_crash.desktop_id;
        if (ends_with(id, ".desktop")) id.resize(id.size() - 8);
        dbus::Writer entry(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        entry.str("desktop-entry").variant_str(id);
      }
    }
    w.i32(-1);
  }
  DBusMessage *reply = bus->call(m, 5000);
  if (!reply) return 1;
  auto args = dbus::read_args(reply);
  dbus_message_unref(reply);
  g_crash.id = args.empty() ? 0 : uint32_t(args[0].num());
  if (!g_crash.id) return 1;
  auto ours = [](DBusMessage *msg) {
    auto a = dbus::read_args(msg);
    return !a.empty() && uint32_t(a[0].num()) == g_crash.id;
  };
  bus->add_signal_handler("type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'", ours,
                          [](DBusMessage *msg) {
                            auto a = dbus::read_args(msg);
                            std::string key = a.size() > 1 ? a[1].str() : "";
                            if (key == "reopen") reopen();
                            if (key == "details") spawn_detached({"sde-system-reports"});
                            exit(0);
                          });
  bus->add_signal_handler("type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'",
                          ours, [](DBusMessage *) { exit(0); });
  // The notification may wait in the notification center for a while.
  Fl::add_timeout(3 * 3600, [](void *) { exit(0); });
  for (;;) Fl::wait(1e20);
}

void usage(FILE *out) {
  fprintf(out, "%s",
          _("Usage: sde-force-close --window XID [--reason frozen|closing]\n"
            "       sde-force-close --launch PID\n"
            "       sde-force-close --kill-window XID\n"
            "       sde-force-close --check-window XID | --check-pid PID\n"
            "       sde-force-close --crashed --name NAME [--icon ICON] [--desktop-id ID]\n"
            "                       [--signal N] [-- COMMAND...]\n\n"
            "Asks whether to wait for a program that stopped responding or never opened,\n"
            "or to force it to close at once (SIGKILL). Programs of other users and the\n"
            "parts of the session are never ended.\n"));
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  install_crash_handler("sde-force-close");
  sde::set_default_window_icon({"sde-force-close", "dialog-warning"});
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty()) {
    usage(stderr);
    return USAGE;
  }
  std::string mode = a[0];
  std::string arg = a.size() > 1 ? a[1] : "";
  if (mode == "--version") return print_version("sde-force-close", SDE_VERSION);
  if (mode == "--help" || mode == "-h") {
    usage(stdout);
    return 0;
  }
  if (mode == "--window" && !arg.empty()) {
    g.win = parse_window(arg.c_str());
    g.closing = a.size() > 3 && a[2] == "--reason" && a[3] == "closing";
    return g.win ? show_dialog() : USAGE;
  }
  if (mode == "--launch" && !arg.empty()) {
    if (!launches::load(atol(arg.c_str()), g.record)) {
      fprintf(stderr, "sde-force-close: no record of the start of %s\n", arg.c_str());
      return RESOLVED;
    }
    g.launch = true;
    return show_dialog();
  }
  if ((mode == "--kill-window" || mode == "--check-window") && !arg.empty()) {
    fl_open_display();
    forceclose::Context ctx = forceclose::current_context();
    long pid = 0;
    forceclose::Verdict v =
        forceclose::force_close_window(fl_display, parse_window(arg.c_str()), ctx, mode == "--check-window", &pid);
    printf("%s %ld\n", forceclose::verdict_name(v), pid);
    XFlush(fl_display);
    return v == forceclose::Verdict::OK ? 0 : REFUSED;
  }
  if (mode == "--check-pid" && !arg.empty()) {
    forceclose::Verdict v = forceclose::check(atol(arg.c_str()), forceclose::current_context());
    printf("%s\n", forceclose::verdict_name(v));
    return v == forceclose::Verdict::OK ? 0 : REFUSED;
  }
  if (mode == "--crashed") {
    for (size_t i = 1; i < a.size(); ++i) {
      std::string next = i + 1 < a.size() ? a[i + 1] : "";
      if (a[i] == "--name") g_crash.name = next, ++i;
      else if (a[i] == "--icon") g_crash.icon = next, ++i;
      else if (a[i] == "--desktop-id") g_crash.desktop_id = next, ++i;
      else if (a[i] == "--signal") g_crash.signal = atoi(next.c_str()), ++i;
      else if (a[i] == "--") {
        g_crash.argv.assign(a.begin() + long(i) + 1, a.end());
        break;
      }
    }
    if (g_crash.name.empty()) return USAGE;
    return notify_crash();
  }
  usage(stderr);
  return USAGE;
}
