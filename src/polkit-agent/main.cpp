// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// sde-polkit-agent: the polkit authentication agent of SDE. When a program
// asks polkit for an administrative action (installing updates, changing
// system settings, mounting a disk of another user), polkit calls this agent,
// which asks for the password in an SDE-FLTK dialog. sde-session starts it.
//
// The agent registers a PolkitAgentListener for the current session; each
// request runs a PolkitAgentSession that talks to polkit's setuid helper.
// GLib's main loop drives polkit; FLTK events are handled from it.
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE 1
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/x.H>
#include <glib-unix.h>
#include <polkitagent/polkitagent.h>
#include <pwd.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "glyphs.h"
#include "i18n.h"
#include "paint.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

using namespace sde;

namespace {

GMainLoop *g_loop = nullptr;

// ------------------------------------------------------------------ dialog

struct Request;
void respond(Request *r);
void cancel(Request *r);
void identity_changed(Request *r, int index);

class Dialog : public Fl_Double_Window {
 public:
  Dialog(Request *req, const std::string &message, const std::string &action,
         const std::vector<std::string> &users, int selected);
  void prompt(const std::string &text, bool echo);
  void error(const std::string &text);
  void busy(bool on);
  std::string password() const { return input_->value(); }
  void clear_password() {
    // value(text, n) copies |text| over the input's own buffer (which holds
    // the typed password); value("") alone would leave the password there.
    // (replace() is not used: it keeps the removed text for undo.)
    if (int n = input_->size()) input_->value(std::string(size_t(n), ' ').c_str(), n);
    input_->value("");
  }
  void draw() override;
  int handle(int e) override;

 private:
  Request *req_;
  Fl_Box *message_, *details_, *prompt_, *error_;
  Fl_Choice *user_ = nullptr;
  Fl_Secret_Input *input_;
  FlatButton *ok_, *cancel_;
  std::string message_text_, details_text_, prompt_text_ = "Password:", error_text_;
};

// ----------------------------------------------------------------- request

struct Request {
  GTask *task = nullptr;
  std::string cookie, action, message;
  std::vector<PolkitIdentity *> identities;
  int selected = 0;
  PolkitAgentSession *session = nullptr;
  GCancellable *cancellable = nullptr;
  gulong cancel_handler = 0;
  std::unique_ptr<Dialog> dialog;
  bool cancelled = false;
  int failures = 0;
};

std::deque<Request *> g_queue;
Request *g_active = nullptr;

std::string identity_label(PolkitIdentity *id) {
  if (POLKIT_IS_UNIX_USER(id)) {
    uid_t uid = uid_t(polkit_unix_user_get_uid(POLKIT_UNIX_USER(id)));
    if (struct passwd *pw = getpwuid(uid)) {
      std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
      gecos = gecos.substr(0, gecos.find(','));
      return gecos.empty() ? pw->pw_name : gecos + " (" + pw->pw_name + ")";
    }
  }
  gchar *s = polkit_identity_to_string(id);
  std::string out = s ? s : "?";
  g_free(s);
  return out;
}

void start_next();

void finish(Request *r, bool gained) {
  if (r->session) {
    g_signal_handlers_disconnect_matched(r->session, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, r);
    g_object_unref(r->session);
    r->session = nullptr;
  }
  if (r->cancel_handler) g_cancellable_disconnect(r->cancellable, r->cancel_handler);
  if (r->dialog) {
    // Deleted later: this may run inside one of the dialog's callbacks.
    r->dialog->hide();
    Fl::delete_widget(r->dialog.release());
  }
  if (gained) {
    g_task_return_boolean(r->task, TRUE);
  } else {
    g_task_return_new_error(r->task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "%s",
                            r->cancelled ? "Authentication was cancelled"
                                         : "Authentication failed");
  }
  g_object_unref(r->task);
  for (auto *id : r->identities) g_object_unref(id);
  delete r;
  g_active = nullptr;
  start_next();
}

void on_request(PolkitAgentSession *, const gchar *text, gboolean echo, gpointer data) {
  auto *r = static_cast<Request *>(data);
  std::string t = text ? text : "";
  // PAM asks "Password: "; show it translated when it is that prompt.
  if (starts_with(lower(trim(t)), "password")) t = _("Password:");
  r->dialog->prompt(trim(t), echo);
}
void on_error(PolkitAgentSession *, const gchar *text, gpointer data) {
  static_cast<Request *>(data)->dialog->error(text ? text : "");
}
void on_info(PolkitAgentSession *, const gchar *text, gpointer data) {
  static_cast<Request *>(data)->dialog->error(text ? text : "");
}

void start_session(Request *r);

void on_completed(PolkitAgentSession *, gboolean gained, gpointer data) {
  auto *r = static_cast<Request *>(data);
  if (gained || r->cancelled) {
    finish(r, gained);
    return;
  }
  // Wrong password: ask again with a fresh session.
  ++r->failures;
  g_signal_handlers_disconnect_matched(r->session, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, r);
  g_object_unref(r->session);
  r->session = nullptr;
  r->dialog->error(_("Sorry, that did not work. Please try again."));
  start_session(r);
}

void start_session(Request *r) {
  r->session = polkit_agent_session_new(r->identities[size_t(r->selected)], r->cookie.c_str());
  g_signal_connect(r->session, "request", G_CALLBACK(on_request), r);
  g_signal_connect(r->session, "show-error", G_CALLBACK(on_error), r);
  g_signal_connect(r->session, "show-info", G_CALLBACK(on_info), r);
  g_signal_connect(r->session, "completed", G_CALLBACK(on_completed), r);
  r->dialog->busy(true);
  polkit_agent_session_initiate(r->session);
}

void respond(Request *r) {
  if (!r) exit(0);  // --preview
  if (!r->session) return;
  std::string pw = r->dialog->password();
  r->dialog->clear_password();
  r->dialog->busy(true);
  r->dialog->error("");
  polkit_agent_session_response(r->session, pw.c_str());
  // Overwrite the copy of the password before the string is freed.
  std::fill(pw.begin(), pw.end(), '\0');
}

void cancel(Request *r) {
  if (!r) exit(0);  // --preview
  r->cancelled = true;
  if (r->session) polkit_agent_session_cancel(r->session);
  else finish(r, false);
}

void identity_changed(Request *r, int index) {
  if (!r) return;  // --preview
  if (index < 0 || index >= int(r->identities.size()) || index == r->selected) return;
  r->selected = index;
  if (r->session) {
    g_signal_handlers_disconnect_matched(r->session, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, r);
    polkit_agent_session_cancel(r->session);
    g_object_unref(r->session);
    r->session = nullptr;
  }
  r->dialog->error("");
  start_session(r);
}

void start(Request *r) {
  g_active = r;
  std::vector<std::string> users;
  for (auto *id : r->identities) users.push_back(identity_label(id));
  r->dialog.reset(new Dialog(r, r->message, r->action, users, r->selected));
  r->dialog->show();
  start_session(r);
}

void start_next() {
  if (g_active || g_queue.empty()) return;
  Request *r = g_queue.front();
  g_queue.pop_front();
  start(r);
}

void on_cancelled(GCancellable *, gpointer data) {
  auto *r = static_cast<Request *>(data);
  // polkit withdrew the request (timeout, or it was answered elsewhere).
  if (r == g_active) {
    cancel(r);
  } else {
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
      if (*it == r) {
        g_queue.erase(it);
        r->cancelled = true;
        r->cancel_handler = 0;
        g_task_return_new_error(r->task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "cancelled");
        g_object_unref(r->task);
        for (auto *id : r->identities) g_object_unref(id);
        delete r;
        break;
      }
  }
}

// ---------------------------------------------------------- dialog impl

Dialog::Dialog(Request *req, const std::string &message, const std::string &action,
               const std::vector<std::string> &users, int selected)
    : Fl_Double_Window(S(460), S(users.size() > 1 ? 316 : 272), _("Authentication Required")),
      req_(req) {
  const Theme &t = active_theme();
  color(t.surface.fl());
  int pad = S(24), x = pad + S(56), w = this->w() - x - pad, y = S(22);
  message_text_ = message.empty() ? _("An application wants to make a change that needs your password.")
                                  : message;
  message_ = new Fl_Box(x, y, w, S(48));
  message_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  message_->labelfont(FL_HELVETICA_BOLD);
  message_->labelsize(t.font_size + 1);
  message_->labelcolor(t.text.fl());
  message_->label(message_text_.c_str());
  y += S(52);
  details_text_ = action;
  details_ = new Fl_Box(x, y, w, S(18));
  details_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  details_->labelsize(t.font_size - 2);
  details_->labelcolor(t.text_dim.fl());
  details_->label(details_text_.c_str());
  y += S(30);
  if (users.size() > 1) {
    user_ = new Fl_Choice(x, y, w, S(32));
    for (const auto &u : users) {
      std::string item = u;
      // '/' and '&' are special in Fl_Menu_ labels.
      for (size_t p = 0; (p = item.find('/', p)) != std::string::npos; p += 2) item.insert(p, "\\");
      user_->add(item.c_str());
    }
    user_->value(selected);
    user_->callback([](Fl_Widget *w, void *d) {
      identity_changed(static_cast<Request *>(d), static_cast<Fl_Choice *>(w)->value());
    }, req);
    y += S(44);
  }
  prompt_ = new Fl_Box(x, y, w, S(18));
  prompt_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  prompt_->labelcolor(t.text.fl());
  prompt_->label(prompt_text_.c_str());
  y += S(22);
  input_ = new Fl_Secret_Input(x, y, w, S(34));
  input_->box(FL_FLAT_BOX);
  input_->color(t.surface_alt.fl());
  input_->textcolor(t.text.fl());
  input_->cursor_color(t.accent.fl());
  input_->when(FL_WHEN_ENTER_KEY_ALWAYS);
  input_->callback([](Fl_Widget *, void *d) { respond(static_cast<Request *>(d)); }, req);
  y += S(40);
  error_ = new Fl_Box(x, y, w, S(20));
  error_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  error_->labelcolor(t.error.fl());
  error_->labelsize(t.font_size - 1);
  int by = this->h() - S(56), bw = S(120);
  cancel_ = new FlatButton(this->w() - pad - 2 * bw - S(10), by, bw, S(36), _("Cancel"));
  cancel_->callback([](Fl_Widget *, void *d) { cancel(static_cast<Request *>(d)); }, req);
  ok_ = new FlatButton(this->w() - pad - bw, by, bw, S(36), _("Authenticate"), true);
  ok_->callback([](Fl_Widget *, void *d) { respond(static_cast<Request *>(d)); }, req);
  end();
  set_modal();
  xclass("SDE-Polkit-Agent");
  // Center on the primary screen.
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  position(sx + (sw - this->w()) / 2, sy + (sh - this->h()) / 3);
}

void Dialog::draw() {
  Fl_Double_Window::draw();
  const Theme &t = active_theme();
  Painter p(w(), h());
  // A round badge with a lock: this dialog asks for a password.
  double cx = S(24), cy = S(22), size = S(44);
  p.fill_rounded(cx, cy, size, size, size / 2, t.accent.alpha(0.16));
  glyph::lock(p, cx + S(10), cy + S(10), S(24), t.accent);
  p.line(0, h() - S(72) + 0.5, w(), h() - S(72) + 0.5, t.border);
  p.flush();
}

int Dialog::handle(int e) {
  if (e == FL_KEYBOARD && Fl::event_key() == FL_Escape) {
    cancel(req_);
    return 1;
  }
  return Fl_Double_Window::handle(e);
}

void Dialog::prompt(const std::string &text, bool echo) {
  prompt_text_ = text.empty() ? _("Password:") : text;
  prompt_->label(prompt_text_.c_str());
  // Echo-on prompts (rare, e.g. a one-time code) are shown in plain text.
  input_->type(echo ? FL_NORMAL_INPUT : FL_SECRET_INPUT);
  busy(false);
  input_->take_focus();
  redraw();
}

void Dialog::error(const std::string &text) {
  error_text_ = text;
  error_->label(error_text_.c_str());
  redraw();
}

void Dialog::busy(bool on) {
  if (on) {
    input_->deactivate();
    ok_->deactivate();
  } else {
    input_->activate();
    ok_->activate();
  }
}

// --------------------------------------------------------------- listener

struct SdeListener {
  PolkitAgentListener parent;
};
struct SdeListenerClass {
  PolkitAgentListenerClass parent_class;
};

GType sde_listener_get_type();
G_DEFINE_TYPE(SdeListener, sde_listener, POLKIT_AGENT_TYPE_LISTENER)

void initiate(PolkitAgentListener *listener, const gchar *action_id, const gchar *message,
              const gchar *, PolkitDetails *, const gchar *cookie, GList *identities,
              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
  auto *r = new Request;
  r->task = g_task_new(listener, cancellable, callback, user_data);
  r->cookie = cookie ? cookie : "";
  r->action = action_id ? action_id : "";
  r->message = message ? message : "";
  uid_t me = getuid();
  for (GList *l = identities; l; l = l->next) {
    auto *id = POLKIT_IDENTITY(l->data);
    if (POLKIT_IS_UNIX_USER(id) &&
        uid_t(polkit_unix_user_get_uid(POLKIT_UNIX_USER(id))) == me)
      r->selected = int(r->identities.size());  // prefer the person at the computer
    r->identities.push_back(POLKIT_IDENTITY(g_object_ref(id)));
  }
  if (r->identities.empty()) {
    g_task_return_new_error(r->task, POLKIT_ERROR, POLKIT_ERROR_FAILED, "No identity to authenticate");
    g_object_unref(r->task);
    delete r;
    return;
  }
  if (cancellable) {
    r->cancellable = cancellable;
    r->cancel_handler = g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled), r, nullptr);
  }
  g_queue.push_back(r);
  start_next();
}

gboolean initiate_finish(PolkitAgentListener *, GAsyncResult *res, GError **error) {
  return g_task_propagate_boolean(G_TASK(res), error);
}

void sde_listener_init(SdeListener *) {}
void sde_listener_class_init(SdeListenerClass *klass) {
  auto *c = POLKIT_AGENT_LISTENER_CLASS(klass);
  c->initiate_authentication = initiate;
  c->initiate_authentication_finish = initiate_finish;
}

// FLTK runs inside GLib's loop: X events wake it, and a short timer keeps
// FLTK's own timeouts (cursor blink) going while a dialog is open.
gboolean on_x_event(gint, GIOCondition, gpointer) {
  Fl::check();
  return G_SOURCE_CONTINUE;
}
gboolean on_tick(gpointer) {
  if (g_active) Fl::check();
  return G_SOURCE_CONTINUE;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) {
      printf("sde-polkit-agent %s (DebION Dev-Team)\n", SDE_VERSION);
      return 0;
    }
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      printf("%s", _("Usage: sde-polkit-agent [--version]\n\n"
                     "The polkit authentication agent of SDE. sde-session starts it.\n"));
      return 0;
    }
  }
  Config cfg;
  cfg.load();
  Theme theme = Theme::from_config(cfg);
  theme.apply_to_fltk();
  fl_open_display();
  if (argc > 1 && !strcmp(argv[1], "--preview")) {
    // Shows the dialog without polkit, for screenshots and theme checks.
    Dialog d(nullptr, _("Authentication is required to install software updates."),
             "org.freedesktop.packagekit.system-update", {"DebION User (debion)", "Administrator (root)"}, 0);
    d.show();
    d.prompt(_("Password:"), false);
    d.error(_("Sorry, that did not work. Please try again."));
    return Fl::run();
  }

  GError *error = nullptr;
  PolkitSubject *subject = polkit_unix_session_new_for_process_sync(getpid(), nullptr, &error);
  if (!subject) {
    // No logind session for this process: fall back to the session ID.
    g_clear_error(&error);
    const char *id = g_getenv("XDG_SESSION_ID");
    if (id) subject = polkit_unix_session_new(id);
  }
  if (!subject) {
    fprintf(stderr, "sde-polkit-agent: cannot find the login session\n");
    return 1;
  }
  auto *listener = POLKIT_AGENT_LISTENER(g_object_new(sde_listener_get_type(), nullptr));
  gpointer handle = polkit_agent_listener_register(
      listener, POLKIT_AGENT_REGISTER_FLAGS_NONE, subject,
      "/org/debion/sde/PolicyKit1/AuthenticationAgent", nullptr, &error);
  if (!handle) {
    fprintf(stderr, "sde-polkit-agent: cannot register: %s\n", error ? error->message : "?");
    // Another agent already serves this session: not an error for SDE.
    return 0;
  }
  g_loop = g_main_loop_new(nullptr, FALSE);
  g_unix_fd_add(ConnectionNumber(fl_display), G_IO_IN, on_x_event, nullptr);
  g_timeout_add(50, on_tick, nullptr);
  g_main_loop_run(g_loop);
  polkit_agent_listener_unregister(handle);
  g_object_unref(listener);
  g_object_unref(subject);
  return 0;
}
