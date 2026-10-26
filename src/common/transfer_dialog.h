// SPDX-License-Identifier: GPL-3.0-or-later
// The copy and move dialog of SDE (0.2.0 Alpha 3), shared by the desktop's
// file operations (drops and Paste) and available to every SDE-FLTK
// program. Liria's file operations window has the same design, the same
// picture and the same model (src/common/transfer_scene.h,
// transfer_model.h):
//
//   [a penguin takes a paper out of one folder, carries it on its head to
//    the other folder and drops it in; a bin for Delete and the Trash]
//   Copying 3 items to Backup                         [Pause] [Cancel]
//   [==================>                         ]
//   780 MB of 1.2 GB · 22 MB/s · About 40 seconds left
//   holiday.mov
//   (v) Details        From  Videos / To  Backup on SANDISK / Items  2 of 3
//   ----
//   Moving 12 items to Documents   [====>    ] 35 %   [Pause] [Cancel]
//
// One window holds every operation of the program: the first with the
// picture, the others as lines under it (the queue). An operation that
// ends well shows the penguin's little jump and "Copied 3 items to Backup"
// with, for a removable drive, "The files are on SANDISK - you can remove
// it." (the copy is on the drive then: src/common/safe_copy.h), and goes
// after a few seconds; one that failed stays with a puzzled penguin and
// its errors until it is closed. While the last data is written the status
// says "Writing the last data to SANDISK...".
//
// The picture moves at about 30 frames per second while it is visible (only
// it is repainted) and stands still when paused, done, minimized and with
// reduced motion ([aero] animations=false or [appearance] low_resource=true
// in sde.conf, or SDE_REDUCED_MOTION=1). Closing the window cancels what is
// still running.
//
// The caller owns the operations: it calls update() a few times a second
// from the FLTK thread and reacts to on_pause and on_cancel.
#pragma once

#include <cairo.h>

#include <functional>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "transfer_model.h"
#include "transfer_scene.h"

namespace sde {

class DrawnButton;

// What a running operation reports.
struct TransferState {
  long long bytes_done = 0, bytes_total = 0;  // bytes_total 0: still counting
  long files_done = 0, files_total = 0;       // items (files, folders, links)
  std::string current;                        // name of the item being copied
  bool finishing = false;                     // writing the last data to |drive|
  std::string drive;                          // the destination drive ("SANDISK"), or ""
  bool removable = false;                     // |drive| may be unplugged once done
  bool fat = false;                           // |drive| has a FAT file system
};

class TransferDialog : public BufferedWindow {
 public:
  enum class Operation { Copy, Move, Delete, Trash };
  enum class Result { Done, Failed, Cancelled };

  TransferDialog();
  ~TransferDialog() override;

  // The dialog of this program, made on first use (not shown).
  static TransferDialog *instance();

  // |source| and |destination|: display names of the folders ("Documents");
  // |source| may be empty. Returns the operation's id.
  int add(Operation op, const std::string &source, const std::string &destination);
  // |now|: the time of the report on a monotonic clock in seconds (tests
  // and demos replay a copy faster); < 0 is the current time.
  void update(int id, const TransferState &state, double now = -1);
  void set_paused(int id, bool paused);
  // The end of an operation: Done shows the finished picture for a few
  // seconds, Failed shows |errors| until the operation is closed,
  // Cancelled removes it at once. The window hides when nothing is left.
  void finish(int id, Result result, const std::vector<std::string> &errors = {});
  bool has(int id) const;
  int count() const { return int(ops_.size()); }
  // The operation shown with the picture, or -1.
  int main_id() const { return ops_.empty() ? -1 : ops_.front()->id; }

  bool paused(int id) const;
  void set_details(bool details);
  bool details() const { return details_; }
  bool animating() const { return animating_; }
  // Buttons, for tests: of an operation, and More/Fewer details.
  Fl_Widget *pause_button(int id) const;
  Fl_Widget *cancel_button(int id) const;
  Fl_Widget *details_button() const;
  // Where the picture is (window coordinates), for tests.
  void scene_box(int &x, int &y, int &w, int &h) const;

  std::function<void(int id, bool paused)> on_pause;
  std::function<void(int id)> on_cancel;

  // The texts as shown, also for tests.
  std::string headline(int id) const;  // "Copying 3 items to Backup"
  std::string status(int id) const;    // "780 MB of 1.2 GB · 22 MB/s · About 40 seconds left"
  std::string note(int id) const;      // the item being copied, or a hint when done
  std::string percent_text(int id) const;  // "42%"
  const SdeTransfer *model(int id) const;

  // "About 2 minutes left", "About 35 seconds left", "A few seconds left",
  // "Calculating..."
  static std::string format_left(double seconds);
  // "85.3 MB/s"
  static std::string format_speed(double bytes_per_second);
  // Whether reduced motion is asked for (sde.conf, SDE_REDUCED_MOTION).
  static bool reduced_motion();

  void draw() override;
  int handle(int event) override;
  void hide() override;

 private:
  struct Op {
    int id = 0;
    Operation kind = Operation::Copy;
    std::string source, destination;
    SdeTransfer model;
    TransferState state;
    bool paused = false;
    enum Phase { Running, Done, Failed } phase = Running;
    std::vector<std::string> errors;
    double started = 0, phase_started = 0;
    DrawnButton *pause = nullptr, *cancel = nullptr;
  };

  Op *find(int id) const;
  void remove(int id);
  void relayout();
  void start_animation();
  void stop_animation();
  void refresh_title();
  bool moving() const;
  SdeScene scene_of(const Op &op) const;
  static void tick(void *data);
  static void expire(void *data);
  double clock() const;
  void draw_main(Op &op);
  void draw_queue_row(Op &op, int y);

  std::vector<Op *> ops_;
  int next_id_ = 1;
  bool details_ = false, animating_ = false, motion_ = true, keyboard_ = false;
  DrawnButton *toggle_ = nullptr;
  std::string title_;
  int scene_x_ = 0, scene_y_ = 0, scene_w_ = 0, scene_h_ = 0;
};

}  // namespace sde
