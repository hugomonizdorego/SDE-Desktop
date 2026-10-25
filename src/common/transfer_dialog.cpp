// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_dialog.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <glib.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "i18n.h"
#include "legal.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

const double kFrame = 1.0 / 30.0;  // animation frame interval, seconds

SdeTransferRgb rgb(const Color &c) {
  SdeTransferRgb out;
  out.r = c.r;
  out.g = c.g;
  out.b = c.b;
  return out;
}

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

cairo_surface_t *render_svg_data(const std::string &svg, int size) {
  GError *err = nullptr;
  RsvgHandle *handle = rsvg_handle_new_from_data(reinterpret_cast<const guint8 *>(svg.data()), svg.size(), &err);
  if (!handle) {
    if (err) g_error_free(err);
    return nullptr;
  }
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(s);
  RsvgRectangle viewport{0, 0, double(size), double(size)};
  bool ok = rsvg_handle_render_document(handle, cr, &viewport, nullptr);
  cairo_destroy(cr);
  g_object_unref(handle);
  if (!ok) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  return s;
}

// Pause (two bars) or resume (a triangle) and cancel (a cross), drawn in
// the text colour on a round hover background.
void draw_glyph_button(Painter &p, Fl_Widget *b, bool hover, bool down, int glyph) {
  const Theme &t = active_theme();
  double x = b->x(), y = b->y(), w = b->w(), h = b->h();
  if (hover || down) p.fill_rounded(x, y, w, h, t.radius_small, down ? t.hover.mix(t.text, 0.12) : t.hover);
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
  } else {  // cancel
    cairo_set_line_width(cr, Sd(1.6));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - s, cy - s);
    cairo_line_to(cr, cx + s, cy + s);
    cairo_move_to(cr, cx + s, cy - s);
    cairo_line_to(cr, cx - s, cy + s);
    cairo_stroke(cr);
  }
}

}  // namespace

// Layout, in unscaled pixels.
static const int kWidth = 480, kPad = 18, kLogo = 72, kButton = 32;
static const int kCollapsed = 166, kExpanded = 340, kGraphTop = 104, kGraphH = 104;

TransferDialog::TransferDialog(Operation op, const std::string &source, const std::string &destination)
    : BufferedWindow(S(kWidth), S(kCollapsed)), op_(op), source_(source), destination_(destination) {
  started_ = clock();
  sde_transfer_init(&model_, started_);
  motion_ = !reduced_motion();
  pause_ = new DrawnButton(0, 0, S(kButton), S(kButton), [this](Painter &p, bool hover, bool down) {
    draw_glyph_button(p, pause_, hover, down, paused_ ? 1 : 0);
  });
  pause_->tooltip(_("Pause"));
  pause_->callback([](Fl_Widget *, void *d) {
    auto *self = static_cast<TransferDialog *>(d);
    self->set_paused(!self->paused_);
    if (self->on_pause) self->on_pause(self->paused_);
  }, this);
  cancel_ = new DrawnButton(0, 0, S(kButton), S(kButton), [this](Painter &p, bool hover, bool down) {
    draw_glyph_button(p, cancel_, hover, down, 2);
  });
  cancel_->tooltip(_("Cancel"));
  cancel_->callback([](Fl_Widget *, void *d) {
    auto *self = static_cast<TransferDialog *>(d);
    self->cancel_->deactivate();
    self->pause_->deactivate();
    if (self->on_cancel) self->on_cancel();
  }, this);
  toggle_ = new DrawnButton(0, 0, S(150), S(26), [this](Painter &p, bool hover, bool down) {
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
    draw_text(details_ ? _("Fewer details") : _("More details"), toggle_->x() + S(26), toggle_->y(),
              toggle_->w() - S(28), toggle_->h(), t.text);
  });
  toggle_->callback([](Fl_Widget *, void *d) {
    auto *self = static_cast<TransferDialog *>(d);
    self->set_details(!self->details_);
  }, this);
  end();
  xclass("sde-transfer");
  title_ = percent_text();
  copy_label(title_.c_str());
  size_range(w(), h(), w(), h());
  callback([](Fl_Widget *w, void *) {
    // Closing the window cancels, like the Cancel button.
    auto *self = static_cast<TransferDialog *>(w);
    if (self->on_cancel && self->cancel_->active()) {
      self->cancel_->deactivate();
      self->on_cancel();
    }
  });
  layout_buttons();
  load_logo(S(kLogo));
}

TransferDialog::~TransferDialog() {
  stop_animation();
  free_logo();
}

Fl_Widget *TransferDialog::pause_button() const { return pause_; }
Fl_Widget *TransferDialog::cancel_button() const { return cancel_; }
Fl_Widget *TransferDialog::details_button() const { return toggle_; }

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

void TransferDialog::free_logo() {
  if (logo_.under) cairo_surface_destroy(logo_.under);
  for (auto *s : logo_.stripe)
    if (s) cairo_surface_destroy(s);
  if (logo_.over) cairo_surface_destroy(logo_.over);
  logo_ = SdeTransferLogo{};
  if (logo_flat_) cairo_surface_destroy(logo_flat_);
  logo_flat_ = nullptr;
}

// The mark in layers (transfer_model.h); a PNG-only installation or a
// mark without stripes is drawn still.
void TransferDialog::load_logo(int size) {
  free_logo();
  std::string path = legal::branding_path("sde-mark"), svg;
  if (ends_with(path, ".svg") && read_file(path, svg)) {
    cairo_surface_t *layers[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    bool ok = true;
    for (int i = 0; i < 5 && ok; ++i) {
      char *part = sde_transfer_logo_layer_svg(svg.c_str(), i);
      ok = part != nullptr;
      if (ok) layers[i] = render_svg_data(part, size);
      free(part);
      ok = ok && layers[i];
    }
    if (ok) {
      logo_.under = layers[0];
      for (int i = 0; i < 3; ++i) logo_.stripe[i] = layers[i + 1];
      logo_.over = layers[4];
      logo_.size = size;
      return;
    }
    for (auto *s : layers)
      if (s) cairo_surface_destroy(s);
  }
  logo_flat_ = legal::load_mark(size, active_theme().dark);
}

void TransferDialog::layout_buttons() {
  int bx = w() - S(kPad) - S(kButton);
  cancel_->resize(bx, S(52), S(kButton), S(kButton));
  pause_->resize(bx - S(6) - S(kButton), S(52), S(kButton), S(kButton));
  toggle_->resize(S(kPad) - S(6), h() - S(kPad) - S(26) + S(4), S(160), S(26));
}

void TransferDialog::set_details(bool details) {
  if (details == details_) return;
  details_ = details;
  int height = S(details ? kExpanded : kCollapsed);
  if (shown()) size(w(), height);
  else resize(x(), y(), w(), height);
  size_range(w(), height, w(), height);
  layout_buttons();
  redraw();
}

void TransferDialog::set_paused(bool paused) {
  if (paused == paused_) return;
  paused_ = paused;
  sde_transfer_set_paused(&model_, std::max(clock(), model_.now), paused ? 1 : 0);
  pause_->tooltip(paused ? _("Resume") : _("Pause"));
  if (paused) stop_animation();
  else start_animation();
  title_ = percent_text();
  copy_label(title_.c_str());
  redraw();
}

void TransferDialog::update(const TransferState &state, double now) {
  state_ = state;
  finishing_ = state.finishing;
  current_ = state.current;
  sde_transfer_update(&model_, now >= 0 ? now : std::max(clock(), model_.now), double(state.bytes_done),
                      double(state.bytes_total), state.files_done, state.files_total);
  std::string title = percent_text();
  if (title != title_) {
    title_ = title;
    copy_label(title_.c_str());
  }
  if (shown()) {
    // Everything but the mark, which the animation repaints on its own.
    damage_rect(S(kPad) + S(kLogo), 0, w() - S(kPad) - S(kLogo), h());
    damage_rect(0, S(kPad) + S(kLogo), S(kPad) + S(kLogo), h() - S(kPad) - S(kLogo));
  }
  start_animation();
}

void TransferDialog::start_animation() {
  bool want = motion_ && !paused_ && !finishing_ && shown() && logo_.under;
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
  if (shown()) damage_rect(S(kPad), S(kPad), S(kLogo), S(kLogo));
}

void TransferDialog::tick(void *data) {
  auto *self = static_cast<TransferDialog *>(data);
  if (!self->animating_) return;
  self->damage_rect(S(kPad), S(kPad), S(kLogo), S(kLogo));
  Fl::repeat_timeout(kFrame, tick, data);
}

void TransferDialog::hide() {
  stop_animation();
  BufferedWindow::hide();
}

int TransferDialog::handle(int event) {
  int r = BufferedWindow::handle(event);
  if (event == FL_SHOW) start_animation();
  return r;
}

std::string TransferDialog::format_speed(double bytes_per_second) {
  return size_text((long long)std::llround(bytes_per_second)) + "/s";
}

std::string TransferDialog::format_remaining(double seconds) {
  int value = 0, minutes = 0;
  switch (sde_transfer_eta_parts(seconds, &value, &minutes)) {
    case SDE_TRANSFER_ETA_UNKNOWN: return _("Calculating…");
    case SDE_TRANSFER_ETA_FEW_SECONDS: return _("A few seconds");
    case SDE_TRANSFER_ETA_SECONDS: return format(ngettext("About %d second", "About %d seconds", value), value);
    case SDE_TRANSFER_ETA_MINUTES: return format(ngettext("About %d minute", "About %d minutes", value), value);
    case SDE_TRANSFER_ETA_HOURS:
      if (minutes == 0) return format(ngettext("About %d hour", "About %d hours", value), value);
      return format(ngettext("About %d hour and %d minutes", "About %d hours and %d minutes", value), value,
                    minutes);
  }
  return std::string();
}

std::string TransferDialog::headline() const {
  long n = model_.files_total;
  bool move = op_ == Operation::Move;
  if (n <= 0) return move ? format(_("Moving to %s"), destination_.c_str()) : format(_("Copying to %s"), destination_.c_str());
  std::string count = count_text(n);
  if (source_.empty())
    return format(move ? ngettext("Moving %s item to %s", "Moving %s items to %s", n)
                       : ngettext("Copying %s item to %s", "Copying %s items to %s", n),
                  count.c_str(), destination_.c_str());
  return format(move ? ngettext("Moving %s item from %s to %s", "Moving %s items from %s to %s", n)
                     : ngettext("Copying %s item from %s to %s", "Copying %s items from %s to %s", n),
                count.c_str(), source_.c_str(), destination_.c_str());
}

std::string TransferDialog::percent_text() const {
  double p = sde_transfer_progress(&model_);
  int percent = p < 0 ? 0 : int(std::floor(p * 100.0));
  if (finishing_) return _("Finishing…");
  if (paused_) return format(_("Paused - %d%% complete"), percent);
  return format(_("%d%% complete"), percent);
}

std::string TransferDialog::speed_text() const {
  if (model_.speed <= 0) return std::string();
  return format(_("Speed: %s"), format_speed(model_.speed).c_str());
}

std::string TransferDialog::remaining_text() const {
  if (paused_) return _("Paused");
  return format_remaining(sde_transfer_remaining(&model_));
}

std::string TransferDialog::items_text() const {
  long items = model_.files_total - model_.files_done;
  if (items < 0) items = 0;
  long long bytes = (long long)(model_.bytes_total - model_.bytes_done);
  if (model_.files_total <= 0) return _("Calculating…");
  return format("%s (%s)", count_text(items).c_str(), size_text(bytes < 0 ? 0 : bytes).c_str());
}

void TransferDialog::draw() {
  const Theme &t = active_theme();
  int pad = S(kPad), tx = pad + S(kLogo) + S(14), tw = w() - tx - pad - 2 * S(kButton) - S(12);
  double seconds = clock() - started_;
  double line_y = -1;
  int gy = S(kGraphTop), gh = S(kGraphH), gx = pad, gw = w() - 2 * pad;
  Color bar = paused_ ? t.warning : t.accent;
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.background);
    if (logo_.under) sde_transfer_draw_logo(p.cr(), &logo_, pad, pad, seconds, animating_ ? 1 : 0);
    else if (logo_flat_) p.image(logo_flat_, pad, pad, S(kLogo));
    if (details_) {
      line_y = sde_transfer_draw_graph(p.cr(), gx, gy, gw, gh, &model_, rgb(bar), rgb(t.text));
    } else {
      double prog = sde_transfer_progress(&model_);
      double by = gy + S(4), bh = S(8);
      p.fill_rounded(gx, by, gw, bh, bh / 2, t.text.alpha(0.10));
      if (prog > 0) p.fill_rounded(gx, by, std::max(bh, gw * prog), bh, bh / 2, bar);
    }
  }
  text_font(t.font_size);
  draw_text(headline(), tx, pad + S(4), w() - tx - pad, S(22), t.text_dim);
  text_font(t.font_size + S(6));
  draw_text(percent_text(), tx, S(50), tw, S(36), t.text);
  if (details_) {
    std::string speed = speed_text();
    if (!speed.empty()) {
      // Over the current speed line, at the right, inside the graph.
      text_font(t.font_size - S(1), true);
      int lw = text_width(speed) + S(10);
      int ly = int(line_y) - S(20);
      if (ly < gy + S(2)) ly = int(line_y) + S(3);
      draw_text(speed, gx + gw - lw, ly, lw - S(4), S(18), t.text, 2);
    }
    int y = gy + gh + S(10), rh = S(24), lw = S(130);
    const std::pair<std::string, std::string> rows[] = {
        {_("Name:"), current_.empty() ? std::string("—") : current_},
        {_("Time remaining:"), remaining_text()},
        {_("Items remaining:"), items_text()},
    };
    for (auto &r : rows) {
      text_font(t.font_size);
      draw_text(r.first, gx, y, lw, rh, t.text_dim);
      draw_text(r.second, gx + lw, y, gw - lw, rh, t.text);
      y += rh;
    }
  }
  draw_children();
}

}  // namespace sde
