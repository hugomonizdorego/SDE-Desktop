// SPDX-License-Identifier: GPL-3.0-or-later
// The copy and move progress dialog of SDE (0.2.0 Alpha 2), shared by the
// desktop's file operations (drops and Paste) and available to every
// SDE-FLTK program. Liria's file operations window has the same design and
// the same model (src/common/transfer_model.h):
//
//   [SDE mark]  Copying 1,234 items from Documents to Backup
//               42% complete                          [Pause] [Cancel]
//   [speed graph: the part done, the speed along the progress, a line at
//    the current speed with "Speed: 85.3 MB/s"]       (More details only)
//   Name: holiday.mov
//   Time remaining: About 2 minutes
//   Items remaining: 120 (1.2 GB)
//   (v) Fewer details / More details
//
// While the operation runs, a pulse runs along the red, green and blue
// stripes of the SDE mark (about 30 frames per second, only the mark is
// repainted). It stands still when the operation is paused and with reduced
// motion ([aero] animations=false or [appearance] low_resource=true in
// sde.conf, or SDE_REDUCED_MOTION=1). The window title is "42% complete".
// Closing the window cancels, like the Cancel button.
//
// The caller owns the operation: it calls update() a few times a second
// from the FLTK thread and reacts to on_pause and on_cancel.
#pragma once

#include <cairo.h>

#include <functional>
#include <string>

#include "buffered_window.h"
#include "transfer_model.h"

namespace sde {

class DrawnButton;

// What a running copy or move reports.
struct TransferState {
  long long bytes_done = 0, bytes_total = 0;  // bytes_total 0: still counting
  long files_done = 0, files_total = 0;       // items (files, folders, links)
  std::string current;                        // name of the item being copied
  bool finishing = false;                     // writing the last data to the drive
};

class TransferDialog : public BufferedWindow {
 public:
  enum class Operation { Copy, Move };

  // |source| and |destination|: display names of the folders ("Documents").
  TransferDialog(Operation op, const std::string &source, const std::string &destination);
  ~TransferDialog() override;

  // |now|: the time of the report on a monotonic clock in seconds (tests
  // and demos replay a copy faster); < 0 is the current time.
  void update(const TransferState &state, double now = -1);
  void set_paused(bool paused);
  bool paused() const { return paused_; }
  void set_details(bool details);
  bool details() const { return details_; }
  bool animating() const { return animating_; }
  // The buttons, for tests: Pause/Resume, Cancel, More/Fewer details.
  Fl_Widget *pause_button() const;
  Fl_Widget *cancel_button() const;
  Fl_Widget *details_button() const;

  std::function<void(bool paused)> on_pause;
  std::function<void()> on_cancel;

  const SdeTransfer &model() const { return model_; }
  // The texts as shown, also for tests.
  std::string headline() const;     // "Copying 3 items from A to B"
  std::string percent_text() const; // "42% complete", "Paused - 42% complete"
  std::string speed_text() const;   // "Speed: 85.3 MB/s"
  std::string remaining_text() const;
  std::string items_text() const;   // "120 (1.2 GB)"

  // "About 2 minutes", "About 35 seconds", "A few seconds", "Calculating..."
  static std::string format_remaining(double seconds);
  // "85.3 MB/s"
  static std::string format_speed(double bytes_per_second);
  // Whether reduced motion is asked for (sde.conf, SDE_REDUCED_MOTION).
  static bool reduced_motion();

  void draw() override;
  int handle(int event) override;
  void hide() override;

 private:
  void layout_buttons();
  void load_logo(int size);
  void free_logo();
  void start_animation();
  void stop_animation();
  static void tick(void *data);
  double clock() const;

  Operation op_;
  std::string source_, destination_, current_, title_;
  SdeTransfer model_;
  TransferState state_;
  bool paused_ = false, details_ = false, finishing_ = false;
  bool animating_ = false, motion_ = true;
  SdeTransferLogo logo_{};
  cairo_surface_t *logo_flat_ = nullptr;  // PNG-only mark: drawn still
  DrawnButton *pause_ = nullptr, *cancel_ = nullptr, *toggle_ = nullptr;
  double started_ = 0;
};

}  // namespace sde
