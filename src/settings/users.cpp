// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// Control Center > Users & Avatar (sde-settings --page users).
//
// Your account: the picture in a circle, the full name (AccountsService
// User.RealName, changed with SetRealName: allowed for one's own account),
// the user name and the account type (Administrator for members of the sudo
// group, else Standard). "Change avatar": the stock pictures of
// /usr/share/Avatar, "Choose a picture..." (PNG, JPEG or SVG up to 10 MB,
// cropped to a square) and "Remove avatar" (back to the initials). The work
// is done by the shared backend (src/common/avatar.h) on a worker thread;
// the panel and the lock screen follow through _SDE_AVATAR_SERIAL. Your
// password (passwd(1), which wants the current one) and automatic login.
//
// The users of this computer (accounts.h): every person's account with its
// picture, type and state, and "Add User..." (full name, a user name made
// from it, Standard or Administrator, a password with its strength, or one
// the person chooses at the first login). The card of a chosen account
// changes its name, picture, type, password, automatic login and lock, or
// removes it, keeping or deleting the home folder. Renaming an account
// (its login name and home folder; yours with your own password) is made
// at the next restart by sde-account-rename (account_rename.h): the page
// shows "Will be renamed to ... at the next restart" with Cancel. AccountsService makes
// every change on a worker thread and polkit asks for an administrator's
// password (sde-polkit-agent); its signals keep the list current. Without
// AccountsService the accounts are listed and the page says what to
// install. The forms are cards of the page, so they scroll on small
// screens, and what was typed is kept while the page is built again.
#include <FL/Fl.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <fcntl.h>
#include <pwd.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "account_rename.h"
#include "accounts.h"
#include "avatar.h"
#include "avatar_view.h"
#include "build_config.h"
#include "dbus.h"
#include "glyphs.h"
#include "i18n.h"
#include "settings.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

avatar::Account g_account;
bool g_account_loaded = false;
avatar::Picture g_picture;
bool g_busy = false;           // a change is being made
std::string g_status;          // its outcome
// How the outcome is shown: in the accent color (working, done), in the
// normal text color (done, with a note) or in the error color (failed).
enum class StatusKind { done, note, error };
StatusKind g_status_kind = StatusKind::done;
std::string g_name_draft;      // the Full name field while it is edited
bool g_name_edited = false;
bool g_lookup_running = false, g_lookup_again = false;

// ---------------------------------------------------- the users' state

std::vector<accounts::User> g_users;
bool g_users_loaded = false;   // the first answer came
bool g_service = false;        // AccountsService answered (else: /etc/passwd)
std::string g_service_error;
bool g_users_loading = false, g_users_again = false;
std::string g_selected;        // user name of the account whose card is open
bool g_reveal_selected = false;  // scroll to its card once the list has it
bool g_changing = false;       // a change to an account is being made
std::string g_change_status;   // its outcome, in the list's card
StatusKind g_change_kind = StatusKind::done;
std::string g_own_status;      // ...or in the card of your account
StatusKind g_own_kind = StatusKind::done;
std::map<std::string, std::unique_ptr<avatar::Picture>> g_user_pictures;

// Renames waiting for the next restart (account_rename.h), read again
// whenever the page is built.
std::vector<account_rename::Request> g_renames;
// The account the rename form is for.
struct RenameTarget {
  uid_t uid = 0;
  std::string name, home, display;
  bool self = false;
};
RenameTarget g_rename_target;

void load_renames() { g_renames = account_rename::load_requests(account_rename::default_dir()); }

const account_rename::Request *rename_request(uid_t uid) {
  for (const auto &r : g_renames)
    if (r.uid == uid) return &r;
  return nullptr;
}

// The form open in the page, and what was typed in it.
enum class Form { none, own_password, add_user, user_password, remove_user, rename };
Form g_form = Form::none;
struct Draft {
  std::string full_name, user_name, current, password, again, other_name;
  bool user_name_edited = false, other_name_edited = false;
  int type = 0;      // the add form: 0 Standard, 1 Administrator
  int later = 0;     // 1: the person chooses a password at the (first or) next login
  std::string message;  // what is wrong with what was typed
  std::string new_login;  // the rename form: the new user name
  int rename_link = 0;    // 1: a link from the old home folder
};
Draft g_draft;
// The control to give the keyboard to once the page is built.
std::string g_focus_next;

void wipe(std::string &s) {
  if (!s.empty()) explicit_bzero(&s[0], s.size());
  s.clear();
}

void wipe_secrets() {
  wipe(g_draft.current);
  wipe(g_draft.password);
  wipe(g_draft.again);
}

void close_form() {
  wipe_secrets();
  g_form = Form::none;
  g_draft.message.clear();
}

// Without AccountsService's answer yet: the login and GECOS names.
const avatar::Account &local_account() {
  static avatar::Account a;
  static bool made = false;
  if (!made) {
    made = true;
    if (struct passwd *pw = getpwuid(getuid())) {
      a.user = pw->pw_name ? pw->pw_name : "";
      std::string g = pw->pw_gecos ? pw->pw_gecos : "";
      a.real_name = trim(g.substr(0, g.find(',')));
      if (a.real_name == a.user) a.real_name.clear();
    }
    if (a.user.empty()) a.user = env("USER", "user");
  }
  return a;
}

void refresh_account_pages() {
  refresh_if_shown("home");
  refresh_if_shown("users");
}

void set_status(const std::string &text, StatusKind kind) {
  g_status = text;
  g_status_kind = kind;
}

void set_change_status(const std::string &text, StatusKind kind) {
  g_change_status = text;
  g_change_kind = kind;
}

void set_own_status(const std::string &text, StatusKind kind) {
  g_own_status = text;
  g_own_kind = kind;
}

// Runs |work| on a worker thread and |done| from the FLTK event loop once it
// has finished (polled, like avatar::run_async(), without announcing
// anything to the other programs).
template <class T>
void background(std::function<T()> work, std::function<void(T &)> done) {
  struct Job {
    std::thread thread;
    std::atomic<bool> finished{false};
    T result;
    std::function<void(T &)> done;
    static void poll(void *data) {
      auto *job = static_cast<Job *>(data);
      if (!job->finished.load()) {
        Fl::repeat_timeout(0.04, poll, data);
        return;
      }
      job->thread.join();
      auto fn = std::move(job->done);
      T result = std::move(job->result);
      delete job;
      if (fn) fn(result);
    }
  };
  auto *job = new Job;
  job->done = std::move(done);
  job->thread = std::thread([job, work] {
    job->result = work();
    job->finished.store(true);
  });
  Fl::add_timeout(0.04, Job::poll, job);
}

// "sde-retro.png" -> "SDE Retro", "sunrise.png" -> "Sunrise" (the names of
// docs/BRANDING.md): words capitalized, SDE in capitals.
std::string pretty_name(const std::string &path) {
  std::string n = basename_of(path);
  size_t dot = n.rfind('.');
  if (dot != std::string::npos) n.erase(dot);
  std::string out, word;
  auto flush = [&]() {
    if (word.empty()) return;
    std::string lower;
    for (char c : word) lower += char(tolower((unsigned char)c));
    if (lower == "sde") word = "SDE";
    else word[0] = char(toupper((unsigned char)word[0]));
    if (!out.empty()) out += ' ';
    out += word;
    word.clear();
  };
  for (char c : n) {
    if (c == '-' || c == '_' || c == ' ') flush();
    else word += c;
  }
  flush();
  return out;
}

void load_users();

void finished(const avatar::Result &r, const std::string &done_text) {
  g_busy = false;
  if (!r.ok)
    set_status(r.error.empty() ? std::string(_("The change did not work.")) : r.error, StatusKind::error);
  else if (r.note.empty())
    set_status(done_text, StatusKind::done);
  else  // done, but only for SDE (for example without AccountsService)
    set_status(r.note, StatusKind::note);
  reload_account();
  load_users();
  refresh_account_pages();
}

void apply_picture(const std::string &path) {
  if (g_busy) return;
  g_busy = true;
  set_status(_("Changing your picture..."), StatusKind::done);
  refresh_account_pages();
  avatar::set_avatar_async(path, [](const avatar::Result &r) { finished(r, _("Your picture was changed.")); });
}

void remove_picture() {
  if (g_busy) return;
  g_busy = true;
  set_status(_("Removing your picture..."), StatusKind::done);
  refresh_account_pages();
  avatar::clear_avatar_async([](const avatar::Result &r) {
    finished(r, _("Your picture was removed: your initials are shown instead."));
  });
}

void save_name() {
  if (g_busy) return;
  std::string name = trim(g_name_draft);
  std::string why;
  if (!avatar::valid_real_name(name, &why)) {
    set_status(why, StatusKind::error);
    refresh_if_shown("users");
    return;
  }
  g_busy = true;
  set_status(_("Changing your name..."), StatusKind::done);
  refresh_if_shown("users");
  avatar::set_real_name_async(name, [](const avatar::Result &r) {
    g_name_edited = false;
    finished(r, _("Your name was changed."));
  });
}

// ------------------------------------------------------------ the users

const accounts::User *find_user(const std::string &name) {
  for (const auto &u : g_users)
    if (u.name == name) return &u;
  return nullptr;
}

const accounts::User *self_user() {
  for (const auto &u : g_users)
    if (u.self) return &u;
  return nullptr;
}

std::vector<std::string> taken_names() {
  std::vector<std::string> out;
  for (const auto &u : g_users) out.push_back(u.name);
  return out;
}

struct Listing {
  bool ok = false;
  std::vector<accounts::User> users;
  std::string error;
};

void load_users() {
  if (g_users_loading) {
    g_users_again = true;
    return;
  }
  g_users_loading = true;
  background<Listing>(
      [] {
        Listing l;
        l.ok = accounts::backend().list(&l.users, &l.error);
        if (!l.ok) l.users = accounts::local_users();
        return l;
      },
      [](Listing &l) {
        g_users_loading = false;
        g_users = std::move(l.users);
        g_service = l.ok;
        g_service_error = l.error;
        g_users_loaded = true;
        if (!g_selected.empty() && !find_user(g_selected)) {
          g_selected.clear();
          if (g_form == Form::user_password || g_form == Form::remove_user) close_form();
        }
        if (g_reveal_selected && find_user(g_selected) && current_settings_page() == "users") {
          g_reveal_selected = false;
          open_settings_page("users", "user");
        } else {
          refresh_if_shown("users");
        }
        if (g_users_again) {
          g_users_again = false;
          load_users();
        }
      });
}

void load_users_later(void *) { load_users(); }

// AccountsService announces accounts added, removed and changed elsewhere
// (another administrator, adduser, the login screen).
void watch_users() {
  static bool watching = false;
  if (watching) return;
  watching = true;
  dbus::Bus *bus = dbus::Bus::system();
  if (!bus) return;
  bus->add_signal_handler(
      "type='signal',sender='org.freedesktop.Accounts'", [](DBusMessage *) { return true; },
      [](DBusMessage *) {
        Fl::remove_timeout(load_users_later);
        Fl::add_timeout(0.3, load_users_later);
      });
}

// A change to an account on a worker thread: |working| is shown meanwhile,
// then |done_text| or the reason it failed, in the list's card or, for
// |own| changes, in the card of your account (they are announced to the
// panel and the lock screen too).
struct Outcome {
  bool ok = false;
  std::string error;
};
void change(const std::string &working, const std::string &done_text, std::function<bool(std::string *)> work,
            std::function<void(bool ok, const std::string &error)> after = nullptr, bool own = false) {
  if (g_changing) return;
  g_changing = true;
  auto report = own ? set_own_status : set_change_status;
  report(working, StatusKind::done);
  refresh_if_shown("users");
  background<Outcome>(
      [work] {
        Outcome o;
        o.ok = work(&o.error);
        return o;
      },
      [done_text, after, own, report](Outcome &o) {
        g_changing = false;
        if (o.ok)
          report(done_text, StatusKind::done);
        else
          report(o.error.empty() ? std::string(_("The change did not work.")) : o.error, StatusKind::error);
        if (after) after(o.ok, o.error);
        if (own && o.ok) {
          avatar::notify_changed();
          reload_account();
        }
        load_users();
        refresh_account_pages();
      });
}

// "sam · Administrator · Locked · Logged in"
std::string user_detail(const accounts::User &u) {
  std::string d = u.name + "  ·  " + accounts::type_label(u.type);
  if (u.locked) d += "  ·  " + std::string(_("Locked"));
  if (u.password_mode == accounts::PasswordMode::set_at_login)
    d += "  ·  " + std::string(_("Password at next login"));
  if (u.auto_login) d += "  ·  " + std::string(_("Automatic login"));
  if (u.logged_in) d += "  ·  " + std::string(_("Logged in"));
  if (const account_rename::Request *r = rename_request(u.uid))
    if (r->state == "pending") d += "  ·  " + format(_("Renamed to %s at the next restart"), r->new_name.c_str());
  return d;
}

// The picture of |u|: yours (~/.face or AccountsService's), else
// AccountsService's IconFile; nullptr draws the initials.
cairo_surface_t *user_picture(const accounts::User &u, int size) {
  if (u.self) return g_picture.at(size);
  auto &pic = g_user_pictures[u.name];
  if (!pic) pic.reset(new avatar::Picture);
  pic->set_path(u.icon);
  return pic->at(size);
}

// ------------------------------------------------------------- widgets

class AvatarCircle : public Fl_Widget {
 public:
  AvatarCircle(int x, int y, int d) : Fl_Widget(x, y, d, d) {}
  void draw() override {
    const Theme &t = settings_theme();
    cairo_surface_t *pic = g_picture.at(w());
    {
      Painter p(window()->w(), window()->h());
      p.circle(x() + w() / 2.0, y() + h() / 2.0, w() / 2.0 + Sd(2), t.border);
      avatar::paint_circle(p, pic, x(), y(), w(), t.accent);
    }
    if (!pic) avatar::draw_initials(settings_account().display_name(), x(), y(), w(), t.on_accent);
  }
};

// One stock picture: click to use it.
class StockChoice : public Fl_Widget {
 public:
  StockChoice(int x, int y, int d, std::string path)
      : Fl_Widget(x, y, d, d), path_(std::move(path)), name_(pretty_name(path_)) {
    tooltip(name_.c_str());
    pic_.set_path(path_);
    if (g_busy) deactivate();
  }
  bool current() const { return avatar::same_picture(path_, g_picture.path()); }
  void draw() override {
    const Theme &t = settings_theme();
    int inset = S(5), d = w() - 2 * inset;
    bool on = current();
    Painter p(window()->w(), window()->h());
    double cx = x() + w() / 2.0, cy = y() + h() / 2.0;
    if (on || hot_ || Fl::focus() == this) {
      cairo_t *cr = p.cr();
      cairo_new_path(cr);
      cairo_arc(cr, cx, cy, w() / 2.0 - Sd(1.5), 0, 2 * M_PI);
      p.set(on ? t.accent : t.text_dim.alpha(0.6));
      cairo_set_line_width(cr, on ? Sd(3) : Sd(1.5));
      cairo_stroke(cr);
    }
    avatar::paint_circle(p, pic_.at(d), x() + inset, y() + inset, d, t.surface_alt);
    if (!active_r()) p.circle(cx, cy, d / 2.0, t.surface.alpha(0.5));
    if (on) {
      double bx = x() + w() - Sd(12), by = y() + Sd(12);
      p.circle(bx, by, Sd(9), t.accent);
      p.line(bx - Sd(4), by, bx - Sd(1), by + Sd(3), t.on_accent, Sd(2));
      p.line(bx - Sd(1), by + Sd(3), bx + Sd(4), by - Sd(3), t.on_accent, Sd(2));
    }
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        if (active_r()) take_focus();
        return 1;
      case FL_RELEASE:
        if (active_r() && Fl::event_inside(this)) apply_picture(path_);
        return 1;
      case FL_ENTER:
      case FL_LEAVE:
        hot_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          apply_picture(path_);
          return 1;
        }
        return 0;
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  std::string path_, name_;
  avatar::Picture pic_;
  bool hot_ = false;
};

// A line of text in the color of its StatusKind.
class StatusLine : public Fl_Widget {
 public:
  StatusLine(int x, int y, int w, int h, std::string text, StatusKind kind)
      : Fl_Widget(x, y, w, h), text_(std::move(text)), kind_(kind) {}
  void set(const std::string &text, StatusKind kind) {
    text_ = text;
    kind_ = kind;
    redraw();
  }
  void draw() override {
    const Theme &t = settings_theme();
    text_font(t.font_size);
    fl_color((kind_ == StatusKind::error ? t.error
              : kind_ == StatusKind::note ? t.text
                                          : t.accent_text)
                 .fl());
    fl_draw(text_.c_str(), x(), y(), w(), h(),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  }

 private:
  std::string text_;
  StatusKind kind_;
};

// A text field in the theme's colors.
void style_input(Fl_Input *in) {
  const Theme &t = settings_theme();
  in->box(FL_DOWN_BOX);
  in->color(t.surface_alt.fl());
  in->textcolor(t.text.fl());
  in->cursor_color(t.accent.fl());
  in->selection_color(t.accent.fl());
  in->textsize(t.font_size + 1);
}

class NameInput : public Fl_Input {
 public:
  NameInput(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {
    style_input(this);
    when(FL_WHEN_CHANGED);
    tooltip(_("Your full name"));
    callback([](Fl_Widget *w, void *) {
      g_name_draft = static_cast<Fl_Input *>(w)->value();
      g_name_edited = true;
    });
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      Fl::add_timeout(0, [](void *) { save_name(); });
      return 1;
    }
    return Fl_Input::handle(e);
  }
};

// A field of a form, kept in |draft| (it survives the page being built
// again). |changed| runs after each edit and Enter runs |submit|. Password
// fields show dots and overwrite their text when they go away.
class DraftInput : public Fl_Input {
 public:
  DraftInput(int x, int y, int w, int h, std::string *draft, const std::string &name, bool secret,
             std::function<void()> changed, std::function<void()> submit)
      : Fl_Input(x, y, w, h), draft_(draft), name_(name), secret_(secret), changed_(std::move(changed)),
        submit_(std::move(submit)) {
    style_input(this);
    if (secret) type(FL_SECRET_INPUT);
    tooltip(name_.c_str());
    value(draft->c_str());
    when(FL_WHEN_CHANGED);
    callback([](Fl_Widget *w, void *) {
      auto *self = static_cast<DraftInput *>(w);
      if (self->secret_) wipe(*self->draft_);
      *self->draft_ = self->value();
      if (self->changed_) self->changed_();
    });
  }
  ~DraftInput() override {
    // value(text, n) copies over the field's own buffer: the typed password
    // does not stay in freed memory.
    if (secret_)
      if (int n = size()) value(std::string(size_t(n), ' ').c_str(), n);
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) && submit_) {
      auto fn = submit_;
      Fl::add_timeout(0, [](void *d) {
        auto *f = static_cast<std::function<void()> *>(d);
        (*f)();
        delete f;
      }, new std::function<void()>(fn));
      return 1;
    }
    return Fl_Input::handle(e);
  }

 private:
  std::string *draft_;
  std::string name_;
  bool secret_;
  std::function<void()> changed_, submit_;
};

// How strong a new password is: four bars and a word, updated as it is
// typed.
class StrengthMeter : public Fl_Widget {
 public:
  StrengthMeter(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void set(const std::string &password, const std::string &user, const std::string &real_name) {
    empty_ = password.empty();
    strength_ = accounts::password_strength(password, user, real_name);
    redraw();
  }
  void draw() override {
    const Theme &t = settings_theme();
    Color c = strength_ <= 1 ? t.error : strength_ == 2 ? t.warning : t.success;
    int label_w = S(110), gap = S(4), bars = std::max(S(40), w() - label_w - S(10));
    int bw = (bars - 3 * gap) / 4, bh = S(6), by = y() + (h() - bh) / 2;
    {
      Painter p(window()->w(), window()->h());
      for (int i = 0; i < 4; ++i) {
        bool lit = !empty_ && (i < strength_ || (i == 0 && strength_ == 0));
        p.fill_rounded(x() + i * (bw + gap), by, bw, bh, bh / 2.0, lit ? c : t.border);
      }
    }
    text_font(t.font_size - 1, true);
    std::string label = empty_ ? std::string(_("Password strength")) : accounts::strength_label(strength_);
    draw_text(label, x() + bars + S(10), y(), label_w, h(), empty_ ? t.text_dim : c);
  }

 private:
  int strength_ = 0;
  bool empty_ = true;
};

// Opens the card of account |name| below the list (yours is the top of the
// page).
void open_user(const std::string &name) {
  const accounts::User *u = find_user(name);
  if (!u || u->self) {
    open_settings_page("users");
    return;
  }
  if (g_selected != name) {
    g_selected = name;
    g_draft.other_name_edited = false;
    if (g_form == Form::user_password || g_form == Form::remove_user) close_form();
  }
  open_settings_page("users", "user");
}

// A row of the list of accounts: picture, name and state; a click opens the
// account's card below the list (yours is the top of the page).
class UserRow : public Fl_Widget {
 public:
  UserRow(int x, int y, int w, int h, const accounts::User &u)
      : Fl_Widget(x, y, w, h), user_(u), name_("user:" + u.name) {
    tooltip(name_.c_str());
  }
  bool selected() const { return !user_.self && user_.name == g_selected; }
  void draw() override {
    const Theme &t = settings_theme();
    int d = S(36), ax = x() + S(10), ay = y() + (h() - d) / 2;
    cairo_surface_t *pic = user_picture(user_, d);
    {
      Painter p(window()->w(), window()->h());
      if (selected() || hot_)
        p.fill_rounded(x(), y(), w(), h(), S(9), selected() ? t.accent.alpha(0.13) : t.hover);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(9));
      avatar::paint_circle(p, pic, ax, ay, d, user_.locked ? t.text_dim : t.accent);
      if (!user_.self) glyph::chevron(p, x() + w() - S(26), y() + h() / 2.0 - S(7), S(14), true, t.text_dim);
    }
    if (!pic) avatar::draw_initials(user_.display_name(), ax, ay, d, t.on_accent);
    int tx = ax + d + S(14), tw = w() - (tx - x()) - S(44);
    if (user_.self) {
      // "You" in a small pill at the right.
      std::string you = _("You");
      text_font(t.font_size - 1, true);
      int pw = text_width(you) + S(16), px = x() + w() - pw - S(10), ph = S(22);
      {
        Painter p(window()->w(), window()->h());
        p.fill_rounded(px, y() + (h() - ph) / 2, pw, ph, ph / 2.0, t.accent.alpha(0.15));
      }
      draw_text(you, px, y() + (h() - ph) / 2, pw, ph, t.accent_text, 1);
      tw = px - tx - S(8);
    }
    text_font(t.font_size + 1, true);
    draw_text(user_.display_name(), tx, y() + h() / 2 - S(21), tw, S(22), t.text);
    text_font(t.font_size - 1);
    draw_text(user_detail(user_), tx, y() + h() / 2 + S(1), tw, S(20), t.text_dim);
  }
  // Shows the account's card (from the event loop: this row goes away
  // when the page is built again).
  void open() {
    Fl::add_timeout(0, [](void *d) {
      std::unique_ptr<std::string> name(static_cast<std::string *>(d));
      open_user(*name);
    }, new std::string(user_.name));
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        take_focus();
        return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) open();
        return 1;
      case FL_ENTER:
      case FL_LEAVE:
        hot_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          open();
          return 1;
        }
        return 0;
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  accounts::User user_;
  std::string name_;
  bool hot_ = false;
};

// A picture of an account in its card.
class UserPicture : public Fl_Widget {
 public:
  UserPicture(int x, int y, int d, const accounts::User &u) : Fl_Widget(x, y, d, d), user_(u) {}
  void draw() override {
    const Theme &t = settings_theme();
    cairo_surface_t *pic = user_picture(user_, w());
    {
      Painter p(window()->w(), window()->h());
      p.circle(x() + w() / 2.0, y() + h() / 2.0, w() / 2.0 + Sd(2), t.border);
      avatar::paint_circle(p, pic, x(), y(), w(), user_.locked ? t.text_dim : t.accent);
    }
    if (!pic) avatar::draw_initials(user_.display_name(), x(), y(), w(), t.on_accent);
  }

 private:
  accounts::User user_;
};

void reload_later(void *) { reload_account(); }

// _SDE_AVATAR_SERIAL: another program (the Start menu) changed the picture.
int g_avatar_atom_handler(int) {
  if (!fl_xevent || fl_xevent->type != PropertyNotify) return 0;
  static Atom serial = XInternAtom(fl_display, avatar::kSerialAtom, False);
  if (fl_xevent->xproperty.window == RootWindow(fl_display, fl_screen) &&
      fl_xevent->xproperty.atom == serial) {
    Fl::remove_timeout(reload_later);
    Fl::add_timeout(0.15, reload_later);
  }
  return 0;
}

// ------------------------------------------------------------ the forms

StrengthMeter *g_meter = nullptr;
StatusLine *g_form_message = nullptr;
DraftInput *g_user_name_input = nullptr;
Label *g_user_name_hint = nullptr;

// True while the widgets above belong to the page shown.
bool form_shown() { return current_settings_page() == "users"; }

void show_message(const std::string &text) {
  g_draft.message = text;
  if (g_form_message && form_shown()) g_form_message->set(text, StatusKind::error);
}

// The meter and the match of the two passwords, as they are typed: the
// confirmation is wrong once it stops being the start of the password.
void password_typed(const std::string &user, const std::string &real_name) {
  if (!form_shown()) return;
  if (g_meter) g_meter->set(g_draft.password, user, real_name);
  bool differ = !g_draft.again.empty() && g_draft.password.compare(0, g_draft.again.size(), g_draft.again) != 0;
  show_message(differ ? std::string(_("The passwords do not match.")) : std::string());
}

// The user name follows the full name until it is typed by hand.
void full_name_typed() {
  if (g_draft.user_name_edited || !form_shown()) return;
  g_draft.user_name = accounts::suggest_user_name(g_draft.full_name, taken_names());
  if (g_user_name_input) g_user_name_input->value(g_draft.user_name.c_str());
  if (g_user_name_hint)
    g_user_name_hint->set(g_draft.user_name.empty() ? std::string(_("Type a user name: lowercase letters, digits, - and _."))
                                                    : std::string(_("Used to log in.")));
}

void user_name_typed() {
  g_draft.user_name_edited = true;
  if (!g_user_name_hint || !form_shown()) return;
  std::string why = accounts::user_name_problem(g_draft.user_name, taken_names());
  g_user_name_hint->set(why.empty() ? std::string(_("Used to log in.")) : why);
}

// Cancel and the form's own button, side by side.
void form_buttons(Page *page, const std::string &label, const std::string &action,
                  std::function<void()> submit, bool danger = false) {
  auto *fn = keep(new std::function<void()>(std::move(submit)));
  page->row(label, S(300), S(34), [&](int x, int y, int w, int h) {
    auto *g = new Fl_Group(x, y, w, h);
    int bw = (w - S(10)) / 2;
    auto *cancel = new FlatButton(x, y, bw, h, _("Cancel"));
    cancel->copy_label(_("Cancel"));
    cancel->callback([](Fl_Widget *, void *) {
      Fl::add_timeout(0, [](void *) {
        close_form();
        refresh_if_shown("users");
      });
    });
    auto *ok = new FlatButton(x + bw + S(10), y, w - bw - S(10), h, action, !danger);
    ok->copy_label(action.c_str());
    ok->callback([](Fl_Widget *, void *d) {
      auto *f = static_cast<std::function<void()> *>(d);
      Fl::add_timeout(0, [](void *dd) { (*static_cast<std::function<void()> *>(dd))(); }, f);
    }, fn);
    if (g_changing) {
      cancel->deactivate();
      ok->deactivate();
    }
    g->end();
    return g;
  });
}

// A field row of a form.
DraftInput *field(Page *page, const std::string &label, const std::string &name, std::string *draft, bool secret,
                  std::function<void()> changed, std::function<void()> submit, const std::string &hint = "") {
  DraftInput *in = nullptr;
  page->row(label, S(300), S(34), [&](int x, int y, int w, int h) {
    in = new DraftInput(x, y, w, h, draft, name, secret, std::move(changed), std::move(submit));
    if (g_changing) in->deactivate();
    return in;
  }, hint);
  return in;
}

// The strength meter, right under the password field.
void meter_row(Page *page) {
  int x, w;
  page->control_box(S(300), &x, &w);
  g_meter = new StrengthMeter(x, page->cursor(), w, S(22));
  page->advance(S(30));
}

void message_row(Page *page) {
  int x = page->left() + S(20), w = page->width() - S(40);
  int h = std::max(S(22), Label::measure(g_draft.message.empty() ? "M" : g_draft.message, w, Label::NORMAL) + S(4));
  g_form_message = new StatusLine(x, page->cursor(), w, h, g_draft.message, StatusKind::error);
  page->advance(h + S(4));
}

void submit_own_password() {
  if (g_changing) return;
  std::string why = g_draft.current.empty() ? std::string(_("Type your current password."))
                                            : accounts::password_problem(g_draft.password, g_draft.again,
                                                                         settings_account().user);
  if (why.empty() && g_draft.password == g_draft.current) why = _("The new password is the same as the current one.");
  if (!why.empty()) {
    show_message(why);
    return;
  }
  auto cur = std::make_shared<std::string>(g_draft.current);
  auto pw = std::make_shared<std::string>(g_draft.password);
  wipe_secrets();
  g_draft.message.clear();
  change(_("Changing your password..."), _("Your password was changed."),
         [cur, pw](std::string *error) {
           bool ok = accounts::change_own_password(*cur, *pw, *pw, error);
           wipe(*cur);
           wipe(*pw);
           return ok;
         },
         [](bool ok, const std::string &error) {
           if (ok) {
             close_form();
           } else {
             g_draft.message = error;  // the form stays open: type them again
             g_focus_next = _("Current password");
             set_own_status("", StatusKind::done);
           }
         },
         true);
}

void submit_add_user() {
  if (g_changing) return;
  accounts::NewUser n;
  n.name = trim(g_draft.user_name);
  n.real_name = trim(g_draft.full_name);
  n.type = g_draft.type == 1 ? accounts::Type::administrator : accounts::Type::standard;
  n.password_at_login = g_draft.later == 1;
  n.password = g_draft.password;
  n.again = g_draft.again;
  std::string why = n.real_name.empty() ? std::string(_("Type the full name of the person."))
                                        : accounts::user_name_problem(n.name, taken_names());
  if (why.empty()) avatar::valid_real_name(n.real_name, &why);
  if (why.empty() && !n.password_at_login) why = accounts::password_problem(n.password, n.again, n.name, n.real_name);
  if (!why.empty()) {
    wipe(n.password);
    wipe(n.again);
    show_message(why);
    return;
  }
  auto job = std::make_shared<accounts::NewUser>(n);
  wipe(n.password);
  wipe(n.again);
  std::vector<accounts::User> all = g_users;
  std::string name = n.name;
  auto created = std::make_shared<bool>(false);
  change(format(_("Adding the account \"%s\"..."), name.c_str()),
         format(_("The account of %s was added."), n.real_name.c_str()),
         [job, all, created](std::string *error) {
           bool ok = accounts::add_user(*job, all, error, created.get());
           wipe(job->password);
           wipe(job->again);
           return ok;
         },
         [name, created](bool ok, const std::string &error) {
           wipe_secrets();
           if (ok || *created) {
             // The account exists (its card sets the password when that failed).
             g_form = Form::none;
             g_draft = Draft();
             g_selected = name;
             g_reveal_selected = true;
           } else {
             g_draft.message = error;  // shown in the form, which stays open
             set_change_status("", StatusKind::done);
           }
         });
}

void submit_user_password(const accounts::User &u) {
  if (g_changing) return;
  bool later = g_draft.later == 1;
  if (!later) {
    std::string why = accounts::password_problem(g_draft.password, g_draft.again, u.name, u.real_name);
    if (!why.empty()) {
      show_message(why);
      return;
    }
  }
  auto pw = std::make_shared<std::string>(later ? std::string() : g_draft.password);
  wipe_secrets();
  g_draft.message.clear();
  change(later ? format(_("Asking %s for a new password at the next login..."), u.display_name().c_str())
               : format(_("Setting the password of %s..."), u.display_name().c_str()),
         later ? format(_("%s chooses a new password at the next login."), u.display_name().c_str())
               : format(_("The password of %s was set."), u.display_name().c_str()),
         [u, later, pw](std::string *error) {
           bool ok = accounts::set_password(u, later, *pw, *pw, error);
           wipe(*pw);
           return ok;
         },
         [](bool ok, const std::string &error) {
           if (ok) {
             close_form();
           } else {
             g_draft.message = error;  // shown in the form, which stays open
             set_change_status("", StatusKind::done);
           }
         });
}

void remove_user(const accounts::User &u, bool files) {
  if (g_changing) return;
  change(format(_("Removing the account of %s..."), u.display_name().c_str()),
         files ? format(_("The account of %s and its home folder were removed."), u.display_name().c_str())
               : format(_("The account of %s was removed; its home folder was kept."), u.display_name().c_str()),
         [u, files](std::string *error) { return accounts::backend().remove(u, files, error); },
         [](bool ok, const std::string &) {
           close_form();
           if (ok) g_selected.clear();
         });
}

void choose_user_picture(const accounts::User &u) {
  static const char *filter = "Pictures (*.{png,jpg,jpeg,svg,PNG,JPG,JPEG,SVG})";
  std::string dir = user_dir("PICTURES");
  if (dir.empty() || !dir_exists(dir)) dir = home_dir();
  const char *f = fl_file_chooser(format(_("A picture for %s"), u.display_name().c_str()).c_str(), filter,
                                  dir.c_str());
  if (!f || !*f) return;
  std::string path = f;
  change(format(_("Changing the picture of %s..."), u.display_name().c_str()),
         format(_("The picture of %s was changed."), u.display_name().c_str()),
         [u, path](std::string *error) { return accounts::set_picture(u, path, error); });
}

// A button of a row; |action| runs from the event loop (the page may be
// built again meanwhile). |why_not| disables it and says why (tooltip);
// the label also names it in the QA dump.
Fl_Widget *button(int x, int y, int w, int h, const std::string &label, std::function<void()> action,
                  bool primary = false, const std::string &why_not = "", bool enabled = true) {
  auto *b = new FlatButton(x, y, w, h, label, primary);
  b->copy_label(label.c_str());
  b->callback([](Fl_Widget *, void *d) {
    Fl::add_timeout(0, [](void *dd) { (*static_cast<std::function<void()> *>(dd))(); }, d);
  }, keep(new std::function<void()>(std::move(action))));
  if (!why_not.empty() || !enabled || g_changing) b->deactivate();
  if (!why_not.empty()) b->tooltip(keep(new std::string(why_not))->c_str());
  return b;
}

void open_form(Form f, const std::string &anchor, const std::string &focus) {
  close_form();
  if (f == Form::add_user) {
    g_draft.full_name.clear();
    g_draft.user_name.clear();
    g_draft.user_name_edited = false;
    g_draft.type = 0;
  }
  g_form = f;
  g_focus_next = focus;
  g_draft.later = 0;
  open_settings_page("users", anchor);
}

// ------------------------------------------------------- renaming

// Whether account |uid| is a person's (UID_MIN to UID_MAX of
// /etc/login.defs): only those are renamed here.
bool renamable(uid_t uid) {
  uid_t min = 0, max = 0;
  account_rename::uid_range("/", &min, &max);
  return uid >= min && uid <= max;
}

// Runs sde-account-rename with |args| through pkexec (polkit asks for the
// password), or SDE_ACCOUNT_RENAME_HELPER directly in the tests, with
// SDE_ACCOUNT_RENAME_DIR and SDE_ACCOUNT_RENAME_ROOT as its --dir and
// --root. Blocks: run it on a worker thread.
bool run_rename_helper(std::vector<std::string> args, std::string *error) {
  std::vector<std::string> argv;
  std::string test = env("SDE_ACCOUNT_RENAME_HELPER");
  if (!test.empty()) {
    argv = {test};
    std::string dir = env("SDE_ACCOUNT_RENAME_DIR"), root = env("SDE_ACCOUNT_RENAME_ROOT");
    if (!dir.empty()) args.insert(args.end(), {"--dir", dir});
    if (!root.empty()) args.insert(args.end(), {"--root", root});
  } else {
    argv = {"pkexec", std::string(SDE_LIBEXECDIR) + "/sde/sde-account-rename"};
  }
  argv.insert(argv.end(), args.begin(), args.end());
  std::vector<char *> a;
  for (auto &s : argv) a.push_back(&s[0]);
  a.push_back(nullptr);
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    *error = strerror(errno);
    return false;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  pid_t pid = -1;
  int err = posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, a.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    *error = format(_("%s cannot be started: %s"), argv[0].c_str(), strerror(err));
    return false;
  }
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (out.size() < 65536) out.append(buf, size_t(n));
  }
  close(fds[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  if (code == 0) return true;
  if (test.empty() && code == 126) {
    *error = _("Nothing was changed: the password was not given.");
    return false;
  }
  if (test.empty() && code == 127) {
    *error = _("Nothing was changed: you are not allowed to do this.");
    return false;
  }
  // "ERROR CODE text"
  for (const auto &line : split(out, '\n'))
    if (starts_with(line, "ERROR ")) {
      size_t sp = line.find(' ', 6);
      *error = sp == std::string::npos ? line.substr(6) : line.substr(sp + 1);
      return false;
    }
  *error = trim(out).empty() ? std::string(_("The change did not work.")) : trim(out);
  return false;
}

// What is wrong with the new name ("" when nothing).
std::string rename_problem() {
  const RenameTarget &t = g_rename_target;
  std::string name = trim(g_draft.new_login);
  if (name.empty()) return _("Type the new user name: lowercase letters, digits, - and _.");
  if (name == t.name) return format(_("The account is called %s already."), name.c_str());
  std::vector<std::string> taken = taken_names();
  for (const auto &r : g_renames)
    if (r.uid != t.uid) taken.push_back(r.new_name);
  return accounts::user_name_problem(name, taken);
}

// The problem, or what becomes of the home folder.
std::string rename_hint() {
  const RenameTarget &t = g_rename_target;
  std::string why = rename_problem();
  if (!why.empty()) return why;
  std::string name = trim(g_draft.new_login);
  std::string home = account_rename::new_home_for(t.home, t.name, name);
  return home != t.home ? format(_("The home folder %s becomes %s."), t.home.c_str(), home.c_str())
                        : std::string(_("The home folder keeps its name."));
}

Label *g_rename_hint = nullptr;

void rename_typed() {
  if (!form_shown()) return;
  if (g_rename_hint) g_rename_hint->set(rename_hint());
  if (!g_draft.message.empty()) show_message("");  // the hint says what is wrong now
}

void submit_rename() {
  if (g_changing) return;
  RenameTarget t = g_rename_target;
  std::string name = trim(g_draft.new_login);
  std::string why = rename_problem();
  if (!why.empty()) {
    show_message(why);
    return;
  }
  std::vector<std::string> args = {t.self ? "schedule-self" : "schedule", "--uid", std::to_string(t.uid), "--old",
                                   t.name, "--new", name};
  if (g_draft.rename_link && account_rename::new_home_for(t.home, t.name, name) != t.home) args.push_back("--link");
  close_form();
  g_draft.new_login.clear();
  change(format(_("Asking to rename %s..."), t.name.c_str()),
         format(_("%s will be renamed to %s at the next restart."), t.name.c_str(), name.c_str()),
         [args](std::string *error) { return run_rename_helper(args, error); },
         [](bool, const std::string &) { load_renames(); }, t.self);
}

void cancel_rename(const RenameTarget &t, bool failed) {
  if (g_changing) return;
  std::vector<std::string> args = {t.self ? "cancel-self" : "cancel", "--uid", std::to_string(t.uid)};
  change(format(_("Cancelling the rename of %s..."), t.name.c_str()),
         failed ? std::string(_("Done.")) : format(_("%s keeps its name."), t.name.c_str()),
         [args](std::string *error) { return run_rename_helper(args, error); },
         [](bool, const std::string &) { load_renames(); }, t.self);
}

void open_rename(const RenameTarget &t) {
  g_rename_target = t;
  g_draft.new_login.clear();
  g_draft.rename_link = 0;
  open_form(Form::rename, t.self ? "own-rename" : "user", _("New user name"));
}

// The form (under the account's User name row).
void rename_form(Page *page) {
  const RenameTarget &t = g_rename_target;
  page->note(t.self ? std::string(_("Your user name changes at the next restart of the computer, before anyone logs "
                                    "in; until then you keep using your current name. Your password, files and "
                                    "settings stay as they are."))
                    : format(_("The user name of %s changes at the next restart of the computer, before anyone logs "
                               "in. Their password, files and settings stay as they are."),
                             t.display.c_str()));
  field(page, _("New user name"), _("New user name"), &g_draft.new_login, false, rename_typed, submit_rename);
  {
    int x = page->left() + S(20), w = page->width() - S(40);
    std::string hint = rename_hint();
    int h = std::max(S(22), Label::measure(hint, w, Label::DIM) + S(4));
    g_rename_hint = new Label(x, page->cursor(), w, h, hint, Label::DIM, true);
    page->advance(h + S(4));
  }
  page->switch_row(
          _("Link from the old folder"), g_draft.rename_link == 1, [](bool on) { g_draft.rename_link = on ? 1 : 0; },
          _("Programs that remember the old folder path keep finding the files. The old folder name stays taken."))
      ->tooltip(_("Link from the old folder"));
  message_row(page);
  form_buttons(page, "", _("Rename at Restart"), submit_rename);
}

// The User name row of an account's card: the name and Rename..., or the
// rename waiting for the restart with Cancel (also a failed one).
void user_name_row(Page *page, const RenameTarget &t, const std::string &why_not) {
  const account_rename::Request *r = rename_request(t.uid);
  std::string hint;
  if (r && r->state == "failed")
    hint = format(_("Renaming to %s failed at the last start: %s"), r->new_name.c_str(), r->error.c_str());
  else if (r)
    hint = format(_("Will be renamed to %s at the next restart."), r->new_name.c_str()) +
           (r->new_home != r->old_home ? " " + format(_("The home folder becomes %s."), r->new_home.c_str())
                                       : std::string());
  else
    hint = why_not.empty() ? std::string(_("Used to log in. A new name is taken at the next restart.")) : why_not;
  if (r) qa_line("@rename:" + r->old_name + ":" + r->new_name + ":" + r->state, 0, 0, 0, 0);
  page->row(_("User name"), S(320), S(34), [&](int x, int y, int w, int h) {
    auto *g = new Fl_Group(x, y, w, h);
    int bw = std::min(S(150), w / 2);
    new Label(x, y, w - bw - S(10), h, t.name, Label::BOLD);
    RenameTarget copy = t;
    if (r) {
      bool failed = r->state == "failed";
      auto *b = button(x + w - bw, y, bw, h, failed ? _("Dismiss") : _("Cancel Rename"),
                       [copy, failed] { cancel_rename(copy, failed); });
      b->tooltip(failed ? _("Forget the failed rename") : _("Cancel the rename"));
    } else {
      auto *b = button(x + w - bw, y, bw, h, _("Rename..."), [copy] { open_rename(copy); }, false, why_not,
                       !(g_form == Form::rename && g_rename_target.uid == t.uid));
      if (why_not.empty()) b->tooltip(t.self ? _("Rename your account") : _("Rename this account"));
    }
    g->end();
    return g;
  }, hint);
  if (g_form == Form::rename && g_rename_target.uid == t.uid && !r && why_not.empty()) rename_form(page);
}

// ------------------------------------------------------------ the cards

// The password form of your account (Change Password...).
void own_password_form(Page *page) {
  std::string user = settings_account().user, real = settings_account().real_name;
  page->anchor("password");
  page->note(_("Your current password is checked first; the new one must also meet the password rules "
               "of this system."));
  field(page, _("Current password"), _("Current password"), &g_draft.current, true, nullptr, submit_own_password);
  field(page, _("New password"), _("New password"), &g_draft.password, true,
        [user, real] { password_typed(user, real); }, submit_own_password);
  meter_row(page);
  field(page, _("Confirm new password"), _("Confirm new password"), &g_draft.again, true,
        [user, real] { password_typed(user, real); }, submit_own_password);
  message_row(page);
  form_buttons(page, "", _("Change Password"), submit_own_password);
  if (g_meter) g_meter->set(g_draft.password, user, real);
}

void own_account_card(Page *page, const avatar::Account &a) {
  const accounts::User *me = g_service ? self_user() : nullptr;
  bool admin = me ? me->admin() : a.admin;
  page->begin_card(_("Your account"));
  page->row(
      _("Full name"), S(320), S(34),
      [&a](int x, int y, int w, int h) {
        int bw = S(92);
        auto *in = new NameInput(x, y, w - bw - S(8), h);
        in->value((g_name_edited ? g_name_draft : a.real_name).c_str());
        auto *save = new FlatButton(x + w - bw, y, bw, h, _("Save"), true);
        save->tooltip(_("Save your name"));
        save->callback([](Fl_Widget *, void *) { save_name(); });
        if (!a.accounts_service || g_busy) {
          in->deactivate();
          save->deactivate();
        }
        return in;
      },
      a.accounts_service ? _("Shown in the Start menu, on the lock screen and when you log in.")
                         : _("Changing the name needs AccountsService (package accountsservice)."));
  {
    RenameTarget t;
    t.uid = getuid();
    t.name = a.user;
    struct passwd *pw = getpwuid(t.uid);
    t.home = pw && pw->pw_dir ? pw->pw_dir : home_dir();
    t.display = a.display_name();
    t.self = true;
    page->anchor("own-rename");
    user_name_row(page, t,
                  t.uid == 0         ? std::string(_("The administrator's account (root) keeps its name."))
                  : !renamable(t.uid) ? std::string(_("Only the accounts of people can be renamed here."))
                                      : std::string());
  }
  page->row(
      _("Account type"), S(220), S(30),
      [admin](int x, int y, int w, int h) {
        return new Label(x, y, w, h, admin ? _("Administrator") : _("Standard"), Label::BOLD);
      },
      admin ? _("A member of the sudo group: can install software and change the system.")
            : _("Not in the sudo group: an administrator's password is needed for system changes."));
  page->row(
      _("Password"), S(200), S(34),
      [](int x, int y, int w, int h) {
        return button(x, y, w, h, _("Change Password..."),
                      [] { open_form(Form::own_password, "password", _("Current password")); }, false, "",
                      g_form != Form::own_password);
      },
      _("Your current password is asked for."));
  if (g_form == Form::own_password) own_password_form(page);
  if (!g_own_status.empty()) {
    int x = page->left() + S(20), w = page->width() - S(40);
    int h = std::max(S(24), Label::measure(g_own_status, w, Label::NORMAL) + S(4));
    new StatusLine(x, page->cursor(), w, h, g_own_status, g_own_kind);
    page->advance(h + S(4));
  }
  if (me) {
    page->switch_row(
        _("Automatic login"), me->auto_login,
        [](bool on) {
          const accounts::User *u = self_user();
          if (!u) return;
          accounts::User copy = *u;
          change(on ? _("Turning automatic login on...") : _("Turning automatic login off..."),
                 on ? _("You are logged in automatically when the computer starts.")
                    : _("Automatic login is off."),
                 [copy, on](std::string *error) { return accounts::backend().set_auto_login(copy, on, error); },
                 nullptr, true);
        },
        _("Start your session without the login screen when the computer starts. Anyone who switches "
          "it on can use your account."));
  }
  page->end_card();
}

void add_user_form(Page *page) {
  page->anchor("add-user");
  page->begin_card(_("Add a user"));
  field(page, _("Full name"), _("New user's full name"), &g_draft.full_name, false, full_name_typed, submit_add_user,
        _("The name of the person, as it is shown on the login screen."));
  {
    std::string why = g_draft.user_name.empty() ? std::string(_("Type a user name: lowercase letters, digits, - and _."))
                                                : accounts::user_name_problem(g_draft.user_name, taken_names());
    std::string hint = why.empty() ? std::string(_("Used to log in.")) : why;
    page->row(_("User name"), S(300), S(34), [&](int x, int y, int w, int h) {
      g_user_name_input = new DraftInput(x, y, w, h, &g_draft.user_name, _("New user name"), false, user_name_typed,
                                         submit_add_user);
      if (g_changing) g_user_name_input->deactivate();
      return g_user_name_input;
    });
    int x = page->left() + S(20), w = page->width() - S(40);
    g_user_name_hint = new Label(x, page->cursor(), w, S(22), hint, Label::DIM);
    page->advance(S(26));
  }
  page->choice_row(
      _("Account type"), {_("Standard"), _("Administrator")}, g_draft.type,
      [](int v) { g_draft.type = v == 1 ? 1 : 0; },
      _("Administrators (the sudo group) install software and change the system."))
      ->tooltip(_("New user's account type"));
  page->choice_row(
      _("Password"), {_("Set a password now"), _("Let them choose one at the first login")}, g_draft.later,
      [](int v) {
        g_draft.later = v == 1 ? 1 : 0;
        Fl::add_timeout(0, [](void *) { refresh_if_shown("users"); });
      })
      ->tooltip(_("When the password is chosen"));
  if (g_draft.later == 0) {
    auto typed = [] { password_typed(g_draft.user_name, g_draft.full_name); };
    field(page, _("New password"), _("New user's password"), &g_draft.password, true, typed, submit_add_user);
    meter_row(page);
    field(page, _("Confirm password"), _("Confirm the new user's password"), &g_draft.again, true, typed,
          submit_add_user);
    if (g_meter) g_meter->set(g_draft.password, g_draft.user_name, g_draft.full_name);
  } else {
    page->note(_("The account has no password until its first login, when the person sets one. Anyone at "
                 "the computer can log in to it until then."));
  }
  message_row(page);
  form_buttons(page, "", _("Add User"), submit_add_user);
  page->end_card();
}

void user_password_form(Page *page, const accounts::User &u) {
  page->anchor("user-password");
  page->choice_row(
      _("New password"), {_("Set a password now"), _("Ask for one at the next login")}, g_draft.later,
      [](int v) {
        g_draft.later = v == 1 ? 1 : 0;
        Fl::add_timeout(0, [](void *) { refresh_if_shown("users"); });
      })
      ->tooltip(_("When the password is chosen"));
  if (g_draft.later == 0) {
    std::string name = u.name, real = u.real_name;
    auto typed = [name, real] { password_typed(name, real); };
    auto submit = [u] { submit_user_password(u); };
    field(page, _("Password"), format(_("Password of %s"), u.name.c_str()), &g_draft.password, true, typed, submit);
    meter_row(page);
    field(page, _("Confirm password"), format(_("Confirm the password of %s"), u.name.c_str()), &g_draft.again, true,
          typed, submit);
    if (g_meter) g_meter->set(g_draft.password, u.name, u.real_name);
  } else {
    page->note(format(_("%s is asked for a new password at the next login and can log in without one "
                        "until then."),
                      u.display_name().c_str()));
  }
  message_row(page);
  form_buttons(page, "", _("Set Password"), [u] { submit_user_password(u); });
}

void remove_form(Page *page, const accounts::User &u) {
  page->anchor("remove-user");
  std::string home = u.home.empty() ? "/home/" + u.name : u.home;
  page->note(format(_("Remove the account of %s (%s)? The person can no longer log in. Their home folder "
                      "%s holds their documents, pictures and settings: keep it, or delete it with the "
                      "account."),
                    u.display_name().c_str(), u.name.c_str(), home.c_str()));
  if (u.logged_in)
    page->note(format(_("%s is logged in now: their programs keep running until they log out."),
                      u.display_name().c_str()));
  page->row("", S(420), S(34), [&](int x, int y, int w, int h) {
    auto *g = new Fl_Group(x, y, w, h);
    int bw = (w - S(20)) / 3;
    button(x, y, bw, h, _("Cancel"), [] {
      close_form();
      refresh_if_shown("users");
    });
    button(x + bw + S(10), y, bw, h, _("Keep Files"), [u] { remove_user(u, false); });
    button(x + 2 * (bw + S(10)), y, w - 2 * (bw + S(10)), h, _("Delete Files"), [u] { remove_user(u, true); }, true);
    g->end();
    return g;
  });
}

// The card of the account chosen in the list.
void user_card(Page *page, const accounts::User &u) {
  page->anchor("user");
  page->begin_card();
  {
    int x = page->left() + S(20), y = page->cursor() + S(4), d = S(64);
    new UserPicture(x, y, d, u);
    int tx = x + d + S(18), tw = page->width() - (tx - page->left()) - S(20);
    new Label(tx, y + S(4), tw, S(32), u.display_name(), Label::TITLE);
    new Label(tx, y + S(38), tw, S(22), user_detail(u), Label::DIM);
    page->advance(d + S(14));
  }
  if (u.logged_in)
    page->note(format(_("%s is logged in now. Most changes apply at their next login."), u.display_name().c_str()));
  page->row(
      _("Full name"), S(320), S(34),
      [&u](int x, int y, int w, int h) {
        int bw = S(92);
        if (!g_draft.other_name_edited) g_draft.other_name = u.real_name;
        accounts::User copy = u;
        auto save = [copy] {
          std::string name = trim(g_draft.other_name), why;
          if (!avatar::valid_real_name(name, &why) || name.empty()) {
            set_change_status(why.empty() ? std::string(_("Type the full name of the person.")) : why, StatusKind::error);
            refresh_if_shown("users");
            return;
          }
          change(format(_("Changing the name of %s..."), copy.name.c_str()),
                 format(_("The name of %s was changed."), copy.name.c_str()),
                 [copy, name](std::string *error) { return accounts::backend().set_real_name(copy, name, error); },
                 [](bool ok, const std::string &) {
                   if (ok) g_draft.other_name_edited = false;
                 });
        };
        auto *in = new DraftInput(x, y, w - bw - S(8), h, &g_draft.other_name,
                                  format(_("Full name of %s"), u.name.c_str()), false,
                                  [] { g_draft.other_name_edited = true; }, save);
        button(x + w - bw, y, bw, h, _("Save Name"), save, true);
        if (g_changing) in->deactivate();
        return in;
      });
  {
    RenameTarget t;
    t.uid = u.uid;
    t.name = u.name;
    t.home = u.home.empty() ? "/home/" + u.name : u.home;
    t.display = u.display_name();
    user_name_row(page, t, renamable(u.uid) ? std::string() : std::string(_("Only the accounts of people can be renamed here.")));
  }
  page->row(_("Picture"), S(260), S(34), [&u](int x, int y, int w, int h) {
    auto *g = new Fl_Group(x, y, w, h);
    int bw = (w - S(10)) / 2;
    accounts::User copy = u;
    button(x, y, bw, h, _("Choose..."), [copy] { choose_user_picture(copy); });
    button(x + bw + S(10), y, w - bw - S(10), h, _("Remove"),
           [copy] {
             change(format(_("Removing the picture of %s..."), copy.name.c_str()),
                    format(_("The picture of %s was removed."), copy.name.c_str()),
                    [copy](std::string *error) { return accounts::set_picture(copy, "", error); });
           },
           false, copy.icon.empty() ? std::string(_("No picture")) : std::string());
    g->end();
    return g;
  }, _("Shown on the login screen and in the list of users."));
  {
    std::string why = accounts::why_not_set_type(g_users, u, u.admin() ? accounts::Type::standard
                                                                      : accounts::Type::administrator);
    accounts::User copy = u;
    auto *ch = page->choice_row(
        _("Account type"), {_("Standard"), _("Administrator")}, u.admin() ? 1 : 0,
        [copy](int v) {
          accounts::Type t = v == 1 ? accounts::Type::administrator : accounts::Type::standard;
          if (t == copy.type) return;
          change(format(_("Changing the account type of %s..."), copy.name.c_str()),
                 t == accounts::Type::administrator
                     ? format(_("%s is an administrator now."), copy.display_name().c_str())
                     : format(_("%s has a standard account now."), copy.display_name().c_str()),
                 [copy, t](std::string *error) { return accounts::backend().set_type(copy, t, error); });
        },
        why.empty() ? std::string(_("Administrators (the sudo group) install software and change the system."))
                    : why);
    ch->tooltip(_("Account type"));
    if (!why.empty() || g_changing) ch->deactivate();
  }
  {
    std::string state = u.password_mode == accounts::PasswordMode::set_at_login
                            ? std::string(_("Chooses a new password at the next login."))
                        : u.password_mode == accounts::PasswordMode::none
                            ? std::string(_("No password: anyone at the computer can log in to this account."))
                            : std::string(_("Set a new password, or let the person choose one at the next login."));
    page->row(_("Password"), S(200), S(34), [&u](int x, int y, int w, int h) {
      return button(x, y, w, h, _("Set Password..."),
                    [] { open_form(Form::user_password, "user-password", ""); }, false, "",
                    g_form != Form::user_password);
    }, state);
    if (g_form == Form::user_password) user_password_form(page, u);
  }
  {
    accounts::User copy = u;
    auto *sw = page->switch_row(
        _("Automatic login"), u.auto_login,
        [copy](bool on) {
          change(format(_("Changing the automatic login of %s..."), copy.name.c_str()),
                 on ? format(_("%s is logged in automatically when the computer starts."), copy.display_name().c_str())
                    : std::string(_("Automatic login is off.")),
                 [copy, on](std::string *error) { return accounts::backend().set_auto_login(copy, on, error); });
        },
        _("Only one account logs in automatically; turning it on here turns it off for the others."));
    sw->tooltip(_("Automatic login"));
    if (g_changing) sw->deactivate();
  }
  {
    std::string why = u.locked ? std::string() : accounts::why_not_lock(g_users, u);
    accounts::User copy = u;
    auto *sw = page->switch_row(
        _("Lock account"), u.locked,
        [copy](bool on) {
          change(format(on ? _("Locking the account of %s...") : _("Unlocking the account of %s..."),
                        copy.name.c_str()),
                 on ? format(_("%s cannot log in now."), copy.display_name().c_str())
                    : format(_("%s can log in again."), copy.display_name().c_str()),
                 [copy, on](std::string *error) { return accounts::backend().set_locked(copy, on, error); });
        },
        why.empty() ? std::string(_("A locked account cannot log in; its files are kept.")) : why);
    sw->tooltip(_("Lock account"));
    if (!why.empty() || g_changing) sw->deactivate();
  }
  {
    std::string why = accounts::why_not_remove(g_users, u);
    page->row(_("Remove this account"), S(200), S(34), [&why](int x, int y, int w, int h) {
      return button(x, y, w, h, _("Remove User..."),
                    [] { open_form(Form::remove_user, "remove-user", ""); }, false, why,
                    g_form != Form::remove_user);
    }, why.empty() ? std::string(_("You choose next whether the home folder is kept.")) : why);
    if (g_form == Form::remove_user && why.empty()) remove_form(page, u);
  }
  page->end_card();
}

void users_card(Page *page) {
  page->anchor("accounts");
  page->begin_card(_("Users of this computer"));
  if (!g_users_loaded) {
    page->note(_("Looking for the accounts of this computer..."));
    page->end_card();
    return;
  }
  if (!g_service)
    page->note(format(_("Adding and changing accounts needs AccountsService. Install the package "
                        "accountsservice (sudo apt install accountsservice), then open this page again. (%s)"),
                      g_service_error.c_str()));
  {
    int x = page->left() + S(10), w = page->width() - S(20), rh = S(56);
    for (const auto &u : g_users) {
      auto *row = new UserRow(x, page->cursor(), w, rh, u);
      if (!g_service && !u.self) row->deactivate();
      page->advance(rh + S(2));
    }
  }
  page->advance(S(6));
  if (!g_change_status.empty()) {
    int x = page->left() + S(20), w = page->width() - S(40);
    int h = std::max(S(24), Label::measure(g_change_status, w, Label::NORMAL) + S(4));
    new StatusLine(x, page->cursor(), w, h, g_change_status, g_change_kind);
    page->advance(h + S(4));
  }
  if (g_service)
    page->row(
        _("New account"), S(200), S(34),
        [](int x, int y, int w, int h) {
          return button(x, y, w, h, _("Add User..."),
                        [] { open_form(Form::add_user, "add-user", _("New user's full name")); }, true, "",
                        g_form != Form::add_user);
        },
        _("Changes to other accounts need the password of an administrator."));
  page->end_card();
}

}  // namespace

const avatar::Account &settings_account() { return g_account_loaded ? g_account : local_account(); }
avatar::Picture &settings_avatar() { return g_picture; }

void reload_account() {
  if (g_lookup_running) {
    g_lookup_again = true;
    return;
  }
  g_lookup_running = true;
  avatar::lookup_async([](const avatar::Account &a, const std::string &path) {
    g_lookup_running = false;
    bool changed = !g_account_loaded || a.user != g_account.user ||
                   a.real_name != g_account.real_name || a.admin != g_account.admin ||
                   a.accounts_service != g_account.accounts_service;
    g_account = a;
    g_account_loaded = true;
    if (g_picture.set_path(path) || g_picture.check()) changed = true;
    if (!g_name_edited) g_name_draft = a.real_name;
    if (changed && !g_busy) refresh_account_pages();
    if (g_lookup_again) {
      g_lookup_again = false;
      reload_account();
    }
  });
}

void watch_account() {
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  Fl::add_handler(g_avatar_atom_handler);
  g_name_draft = local_account().real_name;
  reload_account();
}

void choose_avatar_picture() {
  if (g_busy) return;
  static const char *filter = "Pictures (*.{png,jpg,jpeg,svg,PNG,JPG,JPEG,SVG})";
  std::string dir = user_dir("PICTURES");
  if (dir.empty() || !dir_exists(dir)) dir = home_dir();
  const char *f = fl_file_chooser(_("Choose a picture"), filter, dir.c_str());
  if (!f || !*f) return;
  apply_picture(f);
}

void build_users_page(Page *page) {
  const avatar::Account &a = settings_account();
  g_meter = nullptr;
  g_form_message = nullptr;
  g_user_name_input = nullptr;
  g_user_name_hint = nullptr;
  if (!g_users_loaded && !g_users_loading) load_users();
  watch_users();
  load_renames();
  g_rename_hint = nullptr;
  page->heading(_("Users & Avatar"), _("Your picture and account, and the other users of this computer"));

  // Who: the picture, the name and the kind of account.
  page->begin_card();
  {
    const accounts::User *me = g_service ? self_user() : nullptr;
    bool admin = me ? me->admin() : a.admin;
    int x = page->left() + S(20), y = page->cursor() + S(4), d = S(96);
    new AvatarCircle(x, y, d);
    int tx = x + d + S(22), tw = page->width() - (tx - page->left()) - S(20);
    new Label(tx, y + S(8), tw, S(34), a.display_name(), Label::TITLE);
    std::string kind = admin ? _("Administrator") : _("Standard account");
    new Label(tx, y + S(44), tw, S(22), a.user + "  ·  " + kind, Label::DIM);
    if (!g_status.empty()) {
      int sh = std::max(S(24), Label::measure(g_status, tw, Label::NORMAL) + S(4));
      new StatusLine(tx, y + S(70), tw, sh, g_status, g_status_kind);
      page->advance(std::max(d + S(8), S(70) + sh + S(4)));
    } else {
      page->advance(d + S(8));
    }
  }
  page->end_card();

  // Change avatar.
  page->anchor("avatar");
  page->begin_card(_("Change avatar"));
  {
    int x = page->left() + S(20), avail = page->width() - S(40);
    std::vector<std::string> stock = avatar::stock_avatars();
    if (stock.empty()) {
      page->note(format(_("No pictures are installed in %s."), avatar::stock_dir().c_str()));
    } else {
      int cell = S(76), gap = S(10);
      int cols = std::max(1, (avail + gap) / (cell + gap));
      int y = page->cursor();
      for (size_t i = 0; i < stock.size(); ++i)
        new StockChoice(x + int(i % size_t(cols)) * (cell + gap), y + int(i / size_t(cols)) * (cell + gap),
                        cell, stock[i]);
      int rows = int((stock.size() + size_t(cols) - 1) / size_t(cols));
      page->advance(rows * (cell + gap) + S(4));
    }
    int bw = std::min(S(210), (avail - S(10)) / 2), y = page->cursor();
    auto *choose = new FlatButton(x, y, bw, S(34), _("Choose a picture…"), true);
    choose->callback([](Fl_Widget *, void *) { Fl::add_timeout(0, [](void *) { choose_avatar_picture(); }); });
    auto *remove = new FlatButton(x + bw + S(10), y, bw, S(34), _("Remove avatar"));
    remove->callback([](Fl_Widget *, void *) { remove_picture(); });
    if (g_picture.empty()) remove->deactivate();
    if (g_busy) {
      choose->deactivate();
      remove->deactivate();
    }
    page->advance(S(44));
    page->note(_("PNG, JPEG or SVG pictures of up to 10 MB. The middle square is kept and saved as "
                 "a 256 x 256 picture for SDE (~/.face) and the login screen."));
  }
  page->end_card();

  own_account_card(page, a);
  users_card(page);
  if (g_form == Form::add_user && g_service) add_user_form(page);
  if (const accounts::User *u = find_user(g_selected))
    if (!u->self) user_card(page, *u);
  page->finish();

  // The first field of a form that was just opened takes the keyboard.
  if (!g_focus_next.empty()) {
    std::string name = g_focus_next;
    g_focus_next.clear();
    std::function<Fl_Widget *(Fl_Group *)> find = [&](Fl_Group *g) -> Fl_Widget * {
      for (int i = 0; i < g->children(); ++i) {
        Fl_Widget *c = g->child(i);
        if (c->tooltip() && name == c->tooltip() && c->active_r()) return c;
        if (Fl_Group *sub = c->as_group())
          if (Fl_Widget *w = find(sub)) return w;
      }
      return nullptr;
    };
    if (Fl_Widget *w = find(page)) w->take_focus();
  }
}

}  // namespace sde
