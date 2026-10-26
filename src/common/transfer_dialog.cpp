// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_dialog.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <glib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

const double kFrame = 1.0 / 30.0;   // animation frame interval, seconds
const double kDoneSeconds = 3.0;    // a finished operation stays this long
const double kDoneDrive = 6.0;      // ... and when it says the drive may be removed

// Layout, in unscaled pixels.
const int kWidth = 520, kPad = 20, kScene = 104, kButton = 32, kSmallButton = 28, kRow = 44;

std::string size_text(long long bytes) {
  char *s = g_format_size(guint64(bytes < 0 ? 0 : bytes));
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

// A number with the thousands separator of the locale ("1,234").
std::string count_text(long n) {
  char *s = g_strdup_printf("%'ld", n);
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

SdeSceneRgb scene_rgb(const Color &c) {
  SdeSceneRgb out;
  out.r = c.r;
  out.g = c.g;
  out.b = c.b;
  return out;
}

// Pause (two bars) or resume (a triangle) and cancel or close (a cross),
// drawn in the text colour on a round hover background.
void draw_glyph_button(Painter &p, Fl_Widget *b, bool hover, bool down, int glyph) {
  const Theme &t = active_theme();
  double x = b->x(), y = b->y(), w = b->w(), h = b->h();
  if (hover || down) p.fill_rounded(x, y, w, h, std::min(w, h) / 2, down ? t.hover.mix(t.text, 0.12) : t.hover);
  Color c = b->active() ? t.text : t.text_dim;
  cairo_t *cr = p.cr();
  double cx = x + w / 2, cy = y + h / 2, s = std::min(w, h) * 0.22;
  p.set(c);
  if (glyph == 0) {  // pause
    cairo_rectangle(cr, cx - s * 0.9, cy - s, s * 0.6, s * 2);
    cairo_rectangle(cr, cx + s * 0.3, cy - s, s * 0.6, s * 2);
    cairo_fill(cr);
  } else if (glyph == 1) {  // resume
    cairo_move_to(cr, cx - s * 0.7, cy - s);
    cairo_line_to(cr, cx + s, cy);
    cairo_line_to(cr, cx - s * 0.7, cy + s);
    cairo_close_path(cr);
    cairo_fill(cr);
  } else {  // cancel, close
    cairo_set_line_width(cr, Sd(1.6));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - s, cy - s);
    cairo_line_to(cr, cx + s, cy + s);
    cairo_move_to(cr, cx + s, cy - s);
    cairo_line_to(cr, cx - s, cy + s);
    cairo_stroke(cr);
  }
}

// A slim bar: the track and the part done.
void draw_bar(Painter &p, double x, double y, double w, double h, double progress, const Color &fill) {
  const Theme &t = active_theme();
  p.fill_rounded(x, y, w, h, h / 2, t.text.alpha(0.10));
  if (progress > 0) p.fill_rounded(x, y, std::max(h, w * std::min(progress, 1.0)), h, h / 2, fill);
}

}  // namespace

TransferDialog *TransferDialog::instance() {
  static TransferDialog *dialog = nullptr;
  if (!dialog) dialog = new TransferDialog();
  return dialog;
}

TransferDialog::TransferDialog() : BufferedWindow(S(kWidth), S(240)) {
  motion_ = !reduced_motion();
  toggle_ = new DrawnButton(0, 0, S(120), S(28), [this](Painter &p, bool hover, bool down) {
    const Theme &t = active_theme();
    if (hover || down) p.fill_rounded(toggle_->x(), toggle_->y(), toggle_->w(), toggle_->h(), t.radius_small, t.hover);
    // A chevron: up when the details are shown.
    double cx = toggle_->x() + Sd(14), cy = toggle_->y() + toggle_->h() / 2.0, s = Sd(4);
    cairo_t *cr = p.cr();
    p.set(t.text_dim);
    cairo_set_line_width(cr, Sd(1.5));
    cairo_move_to(cr, cx - s, cy + (details_ ? s / 2 : -s / 2));
    cairo_line_to(cr, cx, cy + (details_ ? -s / 2 : s / 2));
    cairo_line_to(cr, cx + s, cy + (details_ ? s / 2 : -s / 2));
    cairo_stroke(cr);
  }, [this](bool) {
    const Theme &t = active_theme();
    text_font(t.font_size);
    draw_text(_("Details"), toggle_->x() + S(26), toggle_->y(), toggle_->w() - S(28), toggle_->h(), t.text_dim);
  });
  toggle_->tooltip(_("Show or hide the details"));
  toggle_->callback([](Fl_Widget *, void *d) {
    auto *self = static_cast<TransferDialog *>(d);
    self->set_details(!self->details_);
  }, this);
  end();
  xclass("sde-transfer");
  copy_label(_("File Transfers"));
  callback([](Fl_Widget *w, void *) {
    // Closing the window cancels what still runs and closes the rest.
    auto *self = static_cast<TransferDialog *>(w);
    std::vector<int> ids;
    for (Op *op : self->ops_) ids.push_back(op->id);
    for (int id : ids) {
      Op *op = self->find(id);
      if (!op) continue;
      if (op->phase != Op::Running) {
        self->remove(id);
      } else if (op->cancel->active()) {
        op->cancel->deactivate();
        op->pause->deactivate();
        if (self->on_cancel) self->on_cancel(id);
      }
    }
    if (self->ops_.empty()) self->hide();
  });
  relayout();
}

TransferDialog::~TransferDialog() {
  stop_animation();
  Fl::remove_timeout(expire, this);
  for (Op *op : ops_) delete op;
}

double TransferDialog::clock() const {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool TransferDialog::reduced_motion() {
  const char *forced = getenv("SDE_REDUCED_MOTION");
  if (forced && *forced) return std::strcmp(forced, "0") != 0;
  Config cfg;
  cfg.load();
  return !cfg.get_bool("aero", "animations", true) || cfg.get_bool("appearance", "low_resource", false);
}

TransferDialog::Op *TransferDialog::find(int id) const {
  for (Op *op : ops_)
    if (op->id == id) return op;
  return nullptr;
}

bool TransferDialog::has(int id) const { return find(id) != nullptr; }

bool TransferDialog::paused(int id) const {
  Op *op = find(id);
  return op && op->paused;
}

const SdeTransfer *TransferDialog::model(int id) const {
  Op *op = find(id);
  return op ? &op->model : nullptr;
}

Fl_Widget *TransferDialog::pause_button(int id) const {
  Op *op = find(id);
  return op ? op->pause : nullptr;
}

Fl_Widget *TransferDialog::cancel_button(int id) const {
  Op *op = find(id);
  return op ? op->cancel : nullptr;
}

Fl_Widget *TransferDialog::details_button() const { return toggle_; }

void TransferDialog::scene_box(int &x, int &y, int &w, int &h) const {
  x = scene_x_;
  y = scene_y_;
  w = scene_w_;
  h = scene_h_;
}

int TransferDialog::add(Operation kind, const std::string &source, const std::string &destination) {
  Op *op = new Op;
  op->id = next_id_++;
  op->kind = kind;
  op->source = source;
  op->destination = destination;
  op->started = op->phase_started = clock();
  sde_transfer_init(&op->model, op->started);
  int id = op->id;
  begin();
  op->pause = new DrawnButton(0, 0, S(kButton), S(kButton), [this, id](Painter &p, bool hover, bool down) {
    Op *o = find(id);
    if (o) draw_glyph_button(p, o->pause, hover, down, o->paused ? 1 : 0);
  });
  op->pause->tooltip(_("Pause"));
  op->pause->callback([](Fl_Widget *w, void *d) {
    auto *self = static_cast<TransferDialog *>(w->window());
    int id = int(reinterpret_cast<intptr_t>(d));
    Op *o = self->find(id);
    if (!o || o->phase != Op::Running) return;
    self->set_paused(id, !o->paused);
    if (self->on_pause) self->on_pause(id, o->paused);
  }, reinterpret_cast<void *>(intptr_t(id)));
  op->cancel = new DrawnButton(0, 0, S(kButton), S(kButton), [this, id](Painter &p, bool hover, bool down) {
    Op *o = find(id);
    if (o) draw_glyph_button(p, o->cancel, hover, down, 2);
  });
  op->cancel->tooltip(_("Cancel"));
  op->cancel->callback([](Fl_Widget *w, void *d) {
    auto *self = static_cast<TransferDialog *>(w->window());
    int id = int(reinterpret_cast<intptr_t>(d));
    Op *o = self->find(id);
    if (!o) return;
    if (o->phase != Op::Running) {
      // Done or failed: the cross closes it.
      self->remove(id);
      return;
    }
    o->cancel->deactivate();
    o->pause->deactivate();
    if (self->on_cancel) self->on_cancel(id);
  }, reinterpret_cast<void *>(intptr_t(id)));
  end();
  ops_.push_back(op);
  relayout();
  start_animation();
  return id;
}

void TransferDialog::update(int id, const TransferState &state, double now) {
  Op *op = find(id);
  if (!op || op->phase != Op::Running) return;
  if (state.finishing != op->state.finishing && op->cancel->active() == !state.finishing) {
    // The last data must reach the drive: nothing to pause or cancel.
    if (state.finishing) {
      op->pause->deactivate();
      op->cancel->deactivate();
    } else {
      op->pause->activate();
      op->cancel->activate();
    }
  }
  op->state = state;
  sde_transfer_update(&op->model, now >= 0 ? now : std::max(clock(), op->model.now), double(state.bytes_done),
                      double(state.bytes_total), state.files_done, state.files_total);
  refresh_title();
  if (shown()) {
    // Everything but the picture, which the animation repaints on its own.
    if (op == ops_.front()) {
      damage_rect(0, scene_y_ + scene_h_, w(), h() - scene_y_ - scene_h_);
      if (!animating_) damage_rect(scene_x_, scene_y_, scene_w_, scene_h_);
    } else {
      damage_rect(0, scene_y_ + scene_h_, w(), h() - scene_y_ - scene_h_);
    }
  }
  start_animation();
}

void TransferDialog::set_paused(int id, bool paused) {
  Op *op = find(id);
  if (!op || op->paused == paused) return;
  op->paused = paused;
  sde_transfer_set_paused(&op->model, std::max(clock(), op->model.now), paused ? 1 : 0);
  op->pause->tooltip(paused ? _("Resume") : _("Pause"));
  op->phase_started = clock();
  refresh_title();
  start_animation();
  redraw();
}

void TransferDialog::finish(int id, Result result, const std::vector<std::string> &errors) {
  Op *op = find(id);
  if (!op) return;
  if (result == Result::Cancelled) {
    remove(id);
    return;
  }
  op->phase = result == Result::Done ? Op::Done : Op::Failed;
  op->errors = errors;
  op->paused = false;
  op->state.finishing = false;
  op->phase_started = clock();
  // The bar at its end; the cross closes the operation now.
  sde_transfer_update(&op->model, std::max(clock(), op->model.now), double(op->state.bytes_total),
                      double(op->state.bytes_total), op->state.files_total, op->state.files_total);
  op->pause->hide();
  op->cancel->activate();
  op->cancel->tooltip(_("Close"));
  if (result == Result::Done) {
    Fl::remove_timeout(expire, this);
    Fl::add_timeout(0.5, expire, this);
  }
  refresh_title();
  relayout();
  start_animation();
  redraw();
}

void TransferDialog::expire(void *data) {
  auto *self = static_cast<TransferDialog *>(data);
  double now = self->clock();
  std::vector<int> gone;
  bool waiting = false;
  for (Op *op : self->ops_) {
    if (op->phase != Op::Done) continue;
    double keep = op->state.removable || !op->state.drive.empty() ? kDoneDrive : kDoneSeconds;
    if (now - op->phase_started >= keep) gone.push_back(op->id);
    else waiting = true;
  }
  for (int id : gone) self->remove(id);
  if (waiting) Fl::repeat_timeout(0.5, expire, data);
}

void TransferDialog::remove(int id) {
  Op *op = find(id);
  if (!op) return;
  ops_.erase(std::find(ops_.begin(), ops_.end(), op));
  Fl_Group::remove(op->pause);
  Fl_Group::remove(op->cancel);
  // The buttons may be in their own callback: deleted later.
  Fl::delete_widget(op->pause);
  Fl::delete_widget(op->cancel);
  delete op;
  if (ops_.empty()) {
    stop_animation();
    hide();
    return;
  }
  relayout();
  refresh_title();
  start_animation();
  redraw();
}

void TransferDialog::set_details(bool details) {
  if (details == details_) return;
  details_ = details;
  relayout();
  redraw();
}

void TransferDialog::relayout() {
  int pad = S(kPad), inner = w() - 2 * pad;
  int y = S(14);
  scene_x_ = pad;
  scene_y_ = y;
  scene_w_ = inner;
  scene_h_ = S(kScene);
  y += scene_h_ + S(8);
  if (!ops_.empty()) {
    Op *main = ops_.front();
    main->cancel->resize(w() - pad - S(kButton) + S(4), y - S(2), S(kButton), S(kButton));
    main->pause->resize(main->cancel->x() - S(4) - S(kButton), y - S(2), S(kButton), S(kButton));
  }
  y += S(30);       // headline
  y += S(6 + 8);    // bar
  y += S(8 + 20);   // status
  y += S(20) + S(6);  // note
  toggle_->resize(pad - S(6), y, S(120), S(28));
  y += S(28);
  if (details_) y += S(4) + 3 * S(22) + S(4);
  if (ops_.size() > 1) {
    y += S(10) + S(24);  // the line and "2 more operations"
    for (size_t i = 1; i < ops_.size(); ++i) {
      Op *op = ops_[i];
      int by = y + (S(kRow) - S(kSmallButton)) / 2;
      op->cancel->resize(w() - pad - S(kSmallButton) + S(4), by, S(kSmallButton), S(kSmallButton));
      op->pause->resize(op->cancel->x() - S(2) - S(kSmallButton), by, S(kSmallButton), S(kSmallButton));
      y += S(kRow);
    }
  }
  y += S(14);
  if (y != h()) {
    if (shown()) size(w(), y);
    else resize(x(), this->y(), w(), y);
    size_range(w(), y, w(), y);
  }
}

void TransferDialog::refresh_title() {
  std::string title = _("File Transfers");
  if (!ops_.empty()) {
    Op *op = ops_.front();
    double p = sde_transfer_progress(&op->model);
    if (op->phase == Op::Running && p >= 0)
      title = format("%d%% · %s", int(std::floor(p * 100.0)), headline(op->id).c_str());
    else
      title = headline(op->id);
  }
  if (title != title_) {
    title_ = title;
    copy_label(title_.c_str());
  }
}

SdeScene TransferDialog::scene_of(const Op &op) const {
  const Theme &t = active_theme();
  SdeScene s;
  std::memset(&s, 0, sizeof s);
  s.kind = op.kind == Operation::Copy ? SDE_SCENE_COPY
           : op.kind == Operation::Move ? SDE_SCENE_MOVE
                                         : SDE_SCENE_DELETE;
  if (op.phase == Op::Done) s.state = SDE_SCENE_DONE;
  else if (op.phase == Op::Failed) s.state = SDE_SCENE_ERROR;
  else if (op.paused) s.state = SDE_SCENE_PAUSED;
  else if (op.state.finishing) s.state = SDE_SCENE_FINISHING;
  else s.state = SDE_SCENE_RUNNING;
  s.progress = sde_transfer_progress(&op.model);
  double now = clock();
  s.seconds = now - op.started;
  s.state_seconds = now - op.phase_started;
  s.animate = motion_ ? 1 : 0;
  s.dark = t.dark ? 1 : 0;
  s.ink = scene_rgb(t.text);
  return s;
}

bool TransferDialog::moving() const {
  if (ops_.empty() || !motion_) return false;
  SdeScene s = scene_of(*ops_.front());
  return sde_scene_moving(&s) != 0;
}

void TransferDialog::start_animation() {
  bool want = moving() && shown() && visible();
  if (want && !animating_) {
    animating_ = true;
    Fl::add_timeout(kFrame, tick, this);
  } else if (!want) {
    stop_animation();
  }
}

void TransferDialog::stop_animation() {
  if (animating_) Fl::remove_timeout(tick, this);
  animating_ = false;
  if (shown()) damage_rect(scene_x_, scene_y_, scene_w_, scene_h_);
}

void TransferDialog::tick(void *data) {
  auto *self = static_cast<TransferDialog *>(data);
  if (!self->animating_) return;
  self->damage_rect(self->scene_x_, self->scene_y_, self->scene_w_, self->scene_h_);
  // Minimized or nothing moves any more (the jump is over): stop.
  if (!self->visible() || !self->moving()) {
    self->animating_ = false;
    return;
  }
  Fl::repeat_timeout(kFrame, tick, data);
}

void TransferDialog::hide() {
  stop_animation();
  BufferedWindow::hide();
}

int TransferDialog::handle(int event) {
  // No button has the keyboard focus (and its ring) until the keyboard is
  // used: then Tab reaches the first one, as usual.
  if (event == FL_FOCUS && !keyboard_) {
    Fl::focus(this);
    return 1;
  }
  if (event == FL_KEYBOARD && Fl::focus() == this) {
    int key = Fl::event_key();
    keyboard_ = true;
    if (key == FL_Tab || key == FL_Right || key == FL_Left || key == FL_Down || key == FL_Up) {
      bool back = key == FL_Left || key == FL_Up || (key == FL_Tab && Fl::event_shift());
      for (int i = 0; i < children(); ++i)
        if (child(back ? children() - 1 - i : i)->take_focus()) return 1;
    }
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_SHOW) start_animation();
  if (event == FL_HIDE) stop_animation();
  return r;
}

std::string TransferDialog::format_speed(double bytes_per_second) {
  return size_text((long long)std::llround(bytes_per_second)) + "/s";
}

std::string TransferDialog::format_left(double seconds) {
  int value = 0, minutes = 0;
  switch (sde_transfer_eta_parts(seconds, &value, &minutes)) {
    case SDE_TRANSFER_ETA_UNKNOWN: return _("Calculating…");
    case SDE_TRANSFER_ETA_FEW_SECONDS: return _("A few seconds left");
    case SDE_TRANSFER_ETA_SECONDS:
      return format(ngettext("About %d second left", "About %d seconds left", value), value);
    case SDE_TRANSFER_ETA_MINUTES:
      return format(ngettext("About %d minute left", "About %d minutes left", value), value);
    case SDE_TRANSFER_ETA_HOURS:
      if (minutes == 0) return format(ngettext("About %d hour left", "About %d hours left", value), value);
      return format(ngettext("About %d hour and %d minutes left", "About %d hours and %d minutes left", value),
                    value, minutes);
  }
  return std::string();
}

std::string TransferDialog::headline(int id) const {
  Op *op = find(id);
  if (!op) return std::string();
  long n = std::max(op->model.files_total, op->state.files_total);
  std::string count = count_text(n), dest = op->destination;
  if (op->phase == Op::Failed) {
    switch (op->kind) {
      case Operation::Copy: return format(_("Not everything could be copied to %s"), dest.c_str());
      case Operation::Move: return format(_("Not everything could be moved to %s"), dest.c_str());
      case Operation::Delete: return _("Not everything could be deleted");
      case Operation::Trash: return _("Not everything could be moved to the Trash");
    }
  }
  if (op->phase == Op::Done) {
    switch (op->kind) {
      case Operation::Copy:
        return format(ngettext("Copied %s item to %s", "Copied %s items to %s", n), count.c_str(), dest.c_str());
      case Operation::Move:
        return format(ngettext("Moved %s item to %s", "Moved %s items to %s", n), count.c_str(), dest.c_str());
      case Operation::Delete: return format(ngettext("Deleted %s item", "Deleted %s items", n), count.c_str());
      case Operation::Trash:
        return format(ngettext("Moved %s item to the Trash", "Moved %s items to the Trash", n), count.c_str());
    }
  }
  switch (op->kind) {
    case Operation::Copy:
      if (n <= 0) return format(_("Copying to %s"), dest.c_str());
      return format(ngettext("Copying %s item to %s", "Copying %s items to %s", n), count.c_str(), dest.c_str());
    case Operation::Move:
      if (n <= 0) return format(_("Moving to %s"), dest.c_str());
      return format(ngettext("Moving %s item to %s", "Moving %s items to %s", n), count.c_str(), dest.c_str());
    case Operation::Delete:
      if (n <= 0) return _("Deleting");
      return format(ngettext("Deleting %s item", "Deleting %s items", n), count.c_str());
    case Operation::Trash:
      if (n <= 0) return _("Moving to the Trash");
      return format(ngettext("Moving %s item to the Trash", "Moving %s items to the Trash", n), count.c_str());
  }
  return std::string();
}

std::string TransferDialog::status(int id) const {
  Op *op = find(id);
  if (!op) return std::string();
  const SdeTransfer &m = op->model;
  const std::string &drive = op->state.drive;
  if (op->phase == Op::Done) {
    bool removed = op->kind == Operation::Delete || op->kind == Operation::Trash;
    if (!drive.empty() && op->state.removable)
      return removed ? format(_("%s is up to date — you can remove it."), drive.c_str())
                     : format(_("The files are on %s — you can remove it."), drive.c_str());
    if (!drive.empty() && !removed) return format(_("The files are on %s."), drive.c_str());
    return _("Done");
  }
  if (op->phase == Op::Failed) return op->errors.empty() ? std::string(_("Something went wrong.")) : op->errors.front();
  if (op->state.finishing)
    return drive.empty() ? std::string(_("Writing the last data…")) : format(_("Writing the last data to %s…"), drive.c_str());
  std::string text;
  if (m.bytes_total > 0)
    text = format(_("%s of %s"), size_text((long long)m.bytes_done).c_str(), size_text((long long)m.bytes_total).c_str());
  else if (m.files_total > 0)
    text = format(ngettext("%s of %s item", "%s of %s items", m.files_total), count_text(m.files_done).c_str(),
                  count_text(m.files_total).c_str());
  else
    return _("Preparing…");
  const char *dot = " · ";
  if (op->paused) return text + dot + _("Paused");
  if (m.speed > 0 && m.bytes_total > 0) text += dot + format_speed(m.speed);
  if (m.bytes_total > 0) text += dot + format_left(sde_transfer_remaining(&m));
  return text;
}

std::string TransferDialog::note(int id) const {
  Op *op = find(id);
  if (!op) return std::string();
  if (op->phase == Op::Done)
    return op->state.fat && op->state.removable
               ? _("Windows may offer to scan it as it was not ejected; the files are complete.")
               : std::string();
  if (op->phase == Op::Failed) {
    int more = int(op->errors.size()) - 1;
    return more > 0 ? format(ngettext("and %d more problem (Details)", "and %d more problems (Details)", more), more)
                    : std::string();
  }
  if (op->state.finishing) return _("Do not remove the drive yet.");
  return op->state.current;
}

std::string TransferDialog::percent_text(int id) const {
  Op *op = find(id);
  if (!op) return std::string();
  double p = sde_transfer_progress(&op->model);
  return format("%d%%", p < 0 ? 0 : int(std::floor(p * 100.0)));
}

void TransferDialog::draw() {
  const Theme &t = active_theme();
  int pad = S(kPad), inner = w() - 2 * pad;
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.background);
    if (!ops_.empty()) {
      Op &main = *ops_.front();
      SdeScene scene = scene_of(main);
      if (!animating_) scene.animate = scene.animate && sde_scene_moving(&scene);
      sde_scene_draw(p.cr(), scene_x_, scene_y_, scene_w_, scene_h_, &scene);
      Color fill = main.phase == Op::Failed ? t.error : main.phase == Op::Done ? t.success
                   : main.paused                ? t.warning
                                                : t.accent;
      int by = scene_y_ + scene_h_ + S(8) + S(30) + S(6);
      draw_bar(p, pad, by, inner, S(8), main.phase == Op::Running ? sde_transfer_progress(&main.model) : 1.0, fill);
      if (ops_.size() > 1) {
        int ly = toggle_->y() + toggle_->h() + (details_ ? S(4) + 3 * S(22) + S(4) : 0) + S(4);
        p.fill_rect(pad, ly, inner, 1, t.text.alpha(0.14));
        int y = ly + S(6) + S(24);
        for (size_t i = 1; i < ops_.size(); ++i, y += S(kRow)) {
          Op &op = *ops_[i];
          int bw = inner - S(2 * kSmallButton + 6) - S(52);
          Color f = op.phase == Op::Failed ? t.error : op.phase == Op::Done ? t.success
                    : op.paused              ? t.warning
                                             : t.accent;
          draw_bar(p, pad, y + S(28), bw, S(5), op.phase == Op::Running ? sde_transfer_progress(&op.model) : 1.0, f);
        }
      }
    }
  }
  if (!ops_.empty()) {
    Op &main = *ops_.front();
    int y = scene_y_ + scene_h_ + S(8);
    int text_w = inner - 2 * S(kButton) - S(12);
    text_font(t.font_size + S(2), true);
    draw_text(headline(main.id), pad, y, text_w, S(28), t.text);
    y += S(30) + S(6 + 8);
    text_font(t.font_size);
    draw_text(status(main.id), pad, y + S(4), inner, S(20), main.phase == Op::Failed ? t.error : t.text);
    y += S(8 + 20);
    text_font(t.font_size - S(1));
    draw_text(note(main.id), pad, y, inner, S(20), t.text_dim);
    if (details_) {
      int dy = toggle_->y() + toggle_->h() + S(4), lw = S(70);
      std::string to = main.destination;
      if (!main.state.drive.empty() && main.kind != Operation::Delete && main.kind != Operation::Trash)
        to = format(_("%s on %s"), main.destination.c_str(), main.state.drive.c_str());
      std::string items = main.model.files_total > 0
                              ? format(_("%s of %s"), count_text(main.model.files_done).c_str(),
                                       count_text(main.model.files_total).c_str())
                              : std::string("—");
      std::string from = main.source.empty() ? std::string("—") : main.source;
      // A failed operation lists its problems instead.
      std::vector<std::pair<std::string, std::string>> rows = {
          {_("From"), from}, {_("To"), to}, {_("Items"), items}};
      if (main.phase == Op::Failed && main.errors.size() > 1) {
        rows = {{_("Problems"), main.errors[1]}};
        if (main.errors.size() > 2) rows.push_back({"", main.errors[2]});
        if (main.errors.size() > 3)
          rows.push_back({"", format(ngettext("and %d more", "and %d more", int(main.errors.size()) - 3),
                                     int(main.errors.size()) - 3)});
      }
      for (auto &r : rows) {
        text_font(t.font_size);
        draw_text(r.first, pad, dy, lw, S(22), t.text_dim);
        draw_text(r.second, pad + lw, dy, inner - lw, S(22), t.text);
        dy += S(22);
      }
    }
    if (ops_.size() > 1) {
      int ly = toggle_->y() + toggle_->h() + (details_ ? S(4) + 3 * S(22) + S(4) : 0) + S(4);
      int n = int(ops_.size()) - 1;
      text_font(t.font_size - S(1), true);
      draw_text(format(ngettext("%d more operation", "%d more operations", n), n), pad, ly + S(6), inner, S(22),
                t.text_dim);
      int y = ly + S(6) + S(24);
      for (size_t i = 1; i < ops_.size(); ++i, y += S(kRow)) {
        Op &op = *ops_[i];
        int bw = inner - S(2 * kSmallButton + 6) - S(52);
        text_font(t.font_size);
        draw_text(headline(op.id), pad, y + S(2), bw, S(22), t.text);
        text_font(t.font_size - S(1));
        std::string right = op.phase == Op::Running ? (op.paused ? std::string(_("Paused")) : percent_text(op.id))
                                                    : std::string();
        draw_text(right, pad + bw + S(6), y + S(21), S(50), S(14), t.text_dim);
      }
    }
  }
  draw_children();
}

}  // namespace sde
