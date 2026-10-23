// SPDX-License-Identifier: GPL-3.0-or-later
#include "account.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "avatar.h"
#include "avatar_view.h"
#include "glyphs.h"
#include "i18n.h"
#include "paint.h"
#include "panel.h"
#include "popup.h"
#include "popup_style.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

// ================================================================ account

namespace panel_account {

namespace {

std::string g_name, g_user, g_path;
bool g_admin = false, g_started = false, g_running = false, g_again = false;

avatar::Picture &current() {
  static avatar::Picture p;
  return p;
}

// Before AccountsService answers: the GECOS name and ~/.face.
void start() {
  if (g_started) return;
  g_started = true;
  if (struct passwd *pw = getpwuid(getuid())) g_user = pw->pw_name ? pw->pw_name : "";
  if (g_user.empty()) g_user = env("USER", "user");
  g_name = user_name();
  if (g_name == g_user) g_name.clear();
  std::string face = avatar::face_path();
  if (access(face.c_str(), R_OK) == 0) {
    g_path = face;
    current().set_path(face);
  }
  reload();
}

void repaint() {
  for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w)) w->redraw();
}

}  // namespace

const std::string &name() {
  start();
  return g_name.empty() ? g_user : g_name;
}
const std::string &user() {
  start();
  return g_user;
}
bool admin() {
  start();
  return g_admin;
}
const std::string &picture_path() {
  start();
  return g_path;
}
cairo_surface_t *picture(int size) {
  start();
  return current().at(size);
}

void check() {
  start();
  if (current().check()) repaint();
}

void reload() {
  g_started = true;
  if (g_running) {
    g_again = true;
    return;
  }
  g_running = true;
  avatar::lookup_async([](const avatar::Account &a, const std::string &path) {
    g_running = false;
    g_name = a.real_name;
    g_user = a.user;
    g_admin = a.admin;
    g_path = path;
    if (!current().set_path(path)) current().check();
    repaint();
    if (g_again) {
      g_again = false;
      reload();
    }
  });
}

}  // namespace panel_account

// ================================================================= popup

namespace {

// Decoded stock pictures, kept while the panel runs.
avatar::Picture &stock_picture(const std::string &path) {
  static std::map<std::string, std::unique_ptr<avatar::Picture>> cache;
  auto &slot = cache[path];
  if (!slot) {
    slot.reset(new avatar::Picture);
    slot->set_path(path);
  }
  return *slot;
}

void picture_glyph(Painter &p, double x, double y, double s, const Color &c) {
  p.stroke_rounded(x + s * 0.1, y + s * 0.18, s * 0.8, s * 0.64, s * 0.1, c, std::max(1.1, s * 0.08));
  cairo_t *cr = p.cr();
  cairo_new_path(cr);
  cairo_move_to(cr, x + s * 0.16, y + s * 0.74);
  cairo_line_to(cr, x + s * 0.4, y + s * 0.48);
  cairo_line_to(cr, x + s * 0.58, y + s * 0.66);
  cairo_line_to(cr, x + s * 0.7, y + s * 0.56);
  cairo_line_to(cr, x + s * 0.86, y + s * 0.72);
  p.set(c);
  cairo_set_line_width(cr, std::max(1.1, s * 0.08));
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);
  p.circle(x + s * 0.66, y + s * 0.36, s * 0.07, c);
}

std::string file_name(const std::string &path) { return basename_of(path); }

}  // namespace

AccountPopup::~AccountPopup() = default;

void AccountPopup::open(Fl_Window *host, int ax, int ay, int ah) {
  host_ = host;
  anchor_x_ = ax;
  anchor_y_ = ay;
  anchor_h_ = ah;
  open_ = true;
  hover_ = focus_ = -1;
  keyboard_ = false;
  if (!busy_) status_.clear();
  stock_ = avatar::stock_avatars();
  panel_account::check();
  layout();
  host_->redraw();
}

void AccountPopup::close() {
  if (!open_) return;
  open_ = false;
  if (host_) host_->redraw();
}

void AccountPopup::layout() {
  using namespace popup_style;
  int W = host_->w(), H = host_->h(), pad = S(kPad);
  w_ = std::max(S(200), std::min(S(300), W - anchor_x_ - S(14)));
  int cell = S(46), gap = S(8);
  int inner = w_ - 2 * pad;
  int cols = std::max(1, (inner + gap) / (cell + gap));
  int rows = int((stock_.size() + size_t(cols) - 1) / size_t(cols));
  int header = S(48);
  int h = pad + header + S(10) + S(kSection);
  h += stock_.empty() ? S(22) : rows * (cell + gap);
  if (!status_.empty()) h += S(36);
  h += S(4) + 2 * S(kRow) + pad;
  h_ = std::min(h, H - S(12));
  x_ = anchor_x_ + S(6);
  if (x_ + w_ > W - S(6)) x_ = std::max(S(6), W - S(6) - w_);
  y_ = std::max(S(6), std::min(anchor_y_ + anchor_h_ - h_, H - h_ - S(6)));

  items_.clear();
  int y = y_ + pad + header + S(10) + S(kSection);
  int gx = x_ + pad;
  for (size_t i = 0; i < stock_.size(); ++i) {
    Item it;
    it.kind = AVATAR;
    it.index = int(i);
    it.x = gx + int(i % size_t(cols)) * (cell + gap);
    it.y = y + int(i / size_t(cols)) * (cell + gap);
    it.w = it.h = cell;
    items_.push_back(it);
  }
  y += stock_.empty() ? S(22) : rows * (cell + gap);
  if (!status_.empty()) y += S(36);
  y += S(4);
  for (Kind k : {CHOOSE, SETTINGS}) {
    Item it;
    it.kind = k;
    it.x = x_ + S(4);
    it.y = y;
    it.w = w_ - S(8);
    it.h = S(kRow);
    items_.push_back(it);
    y += S(kRow);
  }
}

int AccountPopup::item_at(int px, int py) const {
  for (size_t i = 0; i < items_.size(); ++i)
    if (items_[i].contains(px, py)) return int(i);
  return -1;
}

void AccountPopup::pick(const std::string &path) {
  if (busy_) return;
  busy_ = true;
  status_ = _("Changing your picture...");
  status_error_ = false;
  layout();
  host_->redraw();
  avatar::set_avatar_async(path, [this](const avatar::Result &r) {
    busy_ = false;
    if (!r.ok) {
      status_ = r.error.empty() ? std::string(_("The picture could not be changed.")) : r.error;
      status_error_ = true;
    } else {
      status_ = r.note;  // the login screen kept its picture (no AccountsService)
      status_error_ = false;
    }
    panel_account::reload();
    if (open_) {
      layout();
      host_->redraw();
    }
  });
}

void AccountPopup::activate(int i) {
  if (i < 0 || i >= int(items_.size())) return;
  const Item &it = items_[size_t(i)];
  switch (it.kind) {
    case AVATAR:
      if (it.index >= 0 && it.index < int(stock_.size())) pick(stock_[size_t(it.index)]);
      return;
    case CHOOSE:
    case SETTINGS: {
      std::vector<std::string> argv = it.kind == CHOOSE
                                          ? std::vector<std::string>{"sde-settings", "--choose-avatar"}
                                          : std::vector<std::string>{"sde-settings", "--page", "users"};
      close();
      if (auto *menu = dynamic_cast<PanelPopup *>(host_)) menu->close();
      spawn_detached(argv);
      return;
    }
    default: return;
  }
}

bool AccountPopup::handle(int event) {
  if (!open_ || !host_) return false;
  int ex = Fl::event_x(), ey = Fl::event_y();
  bool inside = ex >= x_ && ex < x_ + w_ && ey >= y_ && ey < y_ + h_;
  switch (event) {
    case FL_PUSH: {
      // Outside the menu: the menu closes (and this with it).
      if (ex < 0 || ey < 0 || ex >= host_->w() || ey >= host_->h()) {
        open_ = false;
        return false;
      }
      if (!inside) {
        close();
        return true;
      }
      pressed_ = item_at(ex, ey);
      return true;
    }
    case FL_RELEASE: {
      int i = item_at(ex, ey);
      if (i >= 0 && i == pressed_) activate(i);
      pressed_ = -1;
      return true;
    }
    case FL_MOVE:
    case FL_DRAG: {
      int h = inside ? item_at(ex, ey) : -1;
      if (h != hover_) {
        hover_ = h;
        keyboard_ = false;
        host_->redraw();
      }
      return true;
    }
    case FL_MOUSEWHEEL:
      return true;
    case FL_KEYBOARD:
    case FL_SHORTCUT: {
      int k = Fl::event_key(), n = int(items_.size());
      if (k == FL_Escape) {
        close();
        return true;
      }
      if (n == 0) return true;
      if (k == FL_Tab || k == FL_Down || k == FL_Right) {
        focus_ = focus_ < 0 ? 0 : (focus_ + 1) % n;
      } else if (k == FL_Up || k == FL_Left) {
        focus_ = focus_ <= 0 ? n - 1 : focus_ - 1;
      } else if (k == FL_Enter || k == FL_KP_Enter || k == ' ') {
        activate(focus_ >= 0 ? focus_ : hover_);
        return true;
      } else {
        return true;
      }
      keyboard_ = true;
      host_->redraw();
      return true;
    }
    default:
      return false;
  }
}

void AccountPopup::draw_shapes(const Theme &t) {
  if (!open_ || !host_) return;
  layout();
  using namespace popup_style;
  int pad = S(kPad);
  Painter p(host_->w(), host_->h());
  double r = radius();
  p.shadow(x_, y_, w_, h_, r, t.shadow, S(12));
  p.fill_rounded(x_, y_, w_, h_, r, t.menu_bg);
  p.stroke_rounded(x_ + 0.5, y_ + 0.5, w_ - 1, h_ - 1, r, t.menu_border);
  avatar::paint_circle(p, panel_account::picture(S(48)), x_ + pad, y_ + pad, S(48), t.accent);
  const std::string &current = panel_account::picture_path();
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item &it = items_[i];
    bool hot = int(i) == hover_ || (keyboard_ && int(i) == focus_);
    if (it.kind == AVATAR) {
      const std::string &path = stock_[size_t(it.index)];
      bool on = avatar::same_picture(path, current);
      double cx = it.x + it.w / 2.0, cy = it.y + it.h / 2.0;
      if (on || hot) {
        cairo_t *cr = p.cr();
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, it.w / 2.0 - Sd(1.2), 0, 2 * M_PI);
        p.set(on ? t.accent : t.text_dim.alpha(0.7));
        cairo_set_line_width(cr, on ? Sd(2.5) : Sd(1.4));
        cairo_stroke(cr);
      }
      int d = it.w - S(8);
      avatar::paint_circle(p, stock_picture(path).at(d), it.x + S(4), it.y + S(4), d, t.surface_alt);
      if (busy_) p.circle(cx, cy, d / 2.0, t.menu_bg.alpha(0.45));
    } else {
      if (hot) p.fill_rounded(it.x, it.y, it.w, it.h, t.radius_small, t.menu_hover);
      Color ink = hot ? t.menu_hover_text : t.menu_text;
      double g = S(kIcon), gx = it.x + S(8), gy = it.y + (it.h - g) / 2.0;
      if (it.kind == CHOOSE)
        picture_glyph(p, gx, gy, g, ink);
      else
        glyph::gear(p, gx, gy, g, ink);
    }
  }
}

void AccountPopup::draw_text(const Theme &t) {
  if (!open_ || !host_) return;
  using namespace popup_style;
  int pad = S(kPad), d = S(48);
  if (!panel_account::picture(d))
    avatar::draw_initials(panel_account::name(), x_ + pad, y_ + pad, d, t.on_accent);
  int tx = x_ + pad + d + S(10), tw = w_ - (tx - x_) - pad;
  text_font(t.font_size + 1, true);
  sde::draw_text(panel_account::name(), tx, y_ + pad + S(3), tw, S(22), t.menu_text);
  text_font(t.font_size - 1);
  std::string kind = panel_account::admin() ? _("Administrator") : _("Standard account");
  sde::draw_text(panel_account::user() + "  ·  " + kind, tx, y_ + pad + S(25), tw, S(20),
                 t.menu_text.mix(t.menu_bg, 0.35));
  int y = y_ + pad + d + S(10);
  text_font(t.font_size - 1, true);
  sde::draw_text(_("Choose your avatar"), x_ + pad, y, w_ - 2 * pad, S(kSection),
                 t.menu_text.mix(t.menu_bg, 0.35));
  y += S(kSection);
  if (stock_.empty()) {
    text_font(t.font_size - 1);
    sde::draw_text(format(_("No pictures in %s"), avatar::stock_dir().c_str()), x_ + pad, y,
                   w_ - 2 * pad, S(20), t.menu_text.mix(t.menu_bg, 0.35));
  }
  if (!status_.empty()) {
    int sy = 0;
    for (auto &it : items_)
      if (it.kind == CHOOSE) sy = it.y - S(4) - S(36);
    text_font(t.font_size - 1);
    fl_color((status_error_ ? t.error : t.menu_text.mix(t.menu_bg, 0.2)).fl());
    fl_draw(status_.c_str(), x_ + pad, sy + S(2), w_ - 2 * pad, S(34),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  }
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item &it = items_[i];
    if (it.kind != CHOOSE && it.kind != SETTINGS) continue;
    bool hot = int(i) == hover_ || (keyboard_ && int(i) == focus_);
    text_font(t.font_size);
    sde::draw_text(it.kind == CHOOSE ? _("Choose a picture…") : _("Account settings…"),
                   it.x + S(8) + S(kIcon) + S(10), it.y, it.w - S(40), it.h,
                   hot ? t.menu_hover_text : t.menu_text);
  }
}

std::string AccountPopup::qa_lines() const {
  if (!open_) return "";
  auto line = [](const std::string &name, int x, int y, int w, int h) {
    return name + "\t" + std::to_string(x) + "\t" + std::to_string(y) + "\t" + std::to_string(w) +
           "\t" + std::to_string(h) + "\t1\t0\n";
  };
  std::string out = line("@account-popup", x_, y_, w_, h_);
  out += line("account:name:" + panel_account::name(), x_, y_, w_, S(48));
  const std::string &current = panel_account::picture_path();
  for (const Item &it : items_) {
    if (it.kind == AVATAR) {
      const std::string &path = stock_[size_t(it.index)];
      out += line("account:avatar:" + file_name(path), it.x, it.y, it.w, it.h);
      if (avatar::same_picture(path, current)) out += line("@account-current:" + file_name(path), 0, 0, 0, 0);
    } else if (it.kind == CHOOSE) {
      out += line("account:choose", it.x, it.y, it.w, it.h);
    } else if (it.kind == SETTINGS) {
      out += line("account:settings", it.x, it.y, it.w, it.h);
    }
  }
  if (!status_.empty()) {
    std::string s = status_;
    for (char &c : s)
      if (c == '\t' || c == '\n') c = ' ';
    out += line("@account-status:" + s, 0, 0, 0, 0);
  }
  return out;
}
