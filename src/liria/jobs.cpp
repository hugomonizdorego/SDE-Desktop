// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// File operations on worker threads with SDE's copy dialog
// (src/common/transfer_dialog.h: the penguin that carries the papers, Pause,
// Cancel, the speed and the time left; one window for every operation of
// Liria). The dialog appears when an operation takes longer than half a
// second; on a removable drive an operation ends only when its data is on
// the drive ("The files are on SANDISK - you can remove it."). Each
// finished operation can be undone (Ctrl+Z), and the windows that show the
// folders involved select what was pasted.
#include <FL/Fl.H>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "clipboard.h"
#include "file_chooser_model.h"
#include "i18n.h"
#include "location.h"
#include "transfer_dialog.h"
#include "ui.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {

class Job;
std::map<int, std::shared_ptr<desk::FileProgress>> &dialog_jobs() {
  static std::map<int, std::shared_ptr<desk::FileProgress>> m;
  return m;
}
int g_running = 0;

std::string folder_name(const std::string &dir) {
  if (dir.empty()) return "";
  return location_title(dir);
}

class Job {
 public:
  using Work = std::function<int(desk::FileProgress *, std::vector<std::string> *)>;
  using Done = std::function<void(int failed, bool cancelled, const std::vector<std::string> &errors)>;

  Job(TransferDialog::Operation op, std::string source, std::string dest, Work work, Done done)
      : op_(op), source_(std::move(source)), dest_(std::move(dest)), done_(std::move(done)) {
    ++g_running;
    auto progress = progress_;
    auto finished = finished_;
    auto errors = errors_;
    auto failed = failed_;
    std::thread([progress, finished, errors, failed, work] {
      std::vector<std::string> errs;
      int f = work ? work(progress.get(), &errs) : 0;
      {
        std::lock_guard<std::mutex> lock(progress->mutex);
        *errors = errs;
      }
      *failed = f;
      *finished = true;
    }).detach();
    started_ = now_ms();
    Fl::add_timeout(0.05, poll, this);
  }
  ~Job() { Fl::remove_timeout(poll, this); }

 private:
  static void poll(void *data) {
    auto *job = static_cast<Job *>(data);
    if (*job->finished_) {
      std::vector<std::string> errors;
      {
        std::lock_guard<std::mutex> lock(job->progress_->mutex);
        errors = *job->errors_;
      }
      bool cancelled = job->progress_->cancel;
      errors.erase(std::remove(errors.begin(), errors.end(), std::string("Cancelled")), errors.end());
      if (!errors.empty() && job->id_ < 0) job->show_dialog();
      if (job->id_ >= 0) {
        job->update_dialog();
        auto *dialog = TransferDialog::instance();
        dialog->finish(job->id_,
                       cancelled && errors.empty() ? TransferDialog::Result::Cancelled
                       : errors.empty()            ? TransferDialog::Result::Done
                                                   : TransferDialog::Result::Failed,
                       errors);
        dialog_jobs().erase(job->id_);
      }
      Done done = std::move(job->done_);
      int failed = *job->failed_;
      delete job;
      --g_running;
      if (done) done(failed, cancelled, errors);
      App::get().maybe_quit();
      return;
    }
    if (job->id_ < 0 && now_ms() - job->started_ > 500) job->show_dialog();
    if (job->id_ >= 0) job->update_dialog();
    Fl::repeat_timeout(0.1, poll, data);
  }

  void show_dialog() {
    auto *dialog = TransferDialog::instance();
    if (!dialog->on_pause) {
      dialog->on_pause = [](int id, bool paused) {
        auto it = dialog_jobs().find(id);
        if (it != dialog_jobs().end()) it->second->pause = paused;
      };
      dialog->on_cancel = [](int id) {
        auto it = dialog_jobs().find(id);
        if (it == dialog_jobs().end()) return;
        it->second->cancel = true;
        it->second->pause = false;
      };
    }
    id_ = dialog->add(op_, source_, dest_);
    dialog_jobs()[id_] = progress_;
    update_dialog();
    if (!dialog->shown()) dialog->show();
  }

  void update_dialog() {
    TransferState s;
    s.bytes_done = progress_->done;
    s.bytes_total = progress_->total;
    s.files_done = progress_->files_done;
    s.files_total = progress_->files_total;
    s.finishing = progress_->phase == desk::FileProgress::FINISHING;
    s.removable = progress_->removable;
    s.fat = progress_->fat;
    {
      std::lock_guard<std::mutex> lock(progress_->mutex);
      s.current = basename_of(progress_->current);
      s.drive = progress_->drive;
    }
    TransferDialog::instance()->update(id_, s);
  }

  TransferDialog::Operation op_;
  std::string source_, dest_;
  Done done_;
  std::shared_ptr<desk::FileProgress> progress_ = std::make_shared<desk::FileProgress>();
  std::shared_ptr<std::atomic<bool>> finished_ = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<int>> failed_ = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<std::vector<std::string>> errors_ = std::make_shared<std::vector<std::string>>();
  double started_ = 0;
  int id_ = -1;
};

// The window if it still exists.
MainWindow *alive(MainWindow *w) {
  auto &ws = App::get().windows;
  return std::find(ws.begin(), ws.end(), w) != ws.end() ? w : nullptr;
}

std::string common_folder(const std::vector<std::string> &paths) {
  std::string dir;
  for (size_t i = 0; i < paths.size(); ++i) {
    std::string d = chooser::parent_dir(paths[i]);
    if (i == 0) dir = d;
    else if (d != dir) return "";
  }
  return dir;
}

void report(MainWindow *w, const std::vector<std::string> &errors) {
  if (!w || errors.empty()) return;
  w->say(errors.front() + (errors.size() > 1 ? format(_(" (and %d more problems)"), int(errors.size()) - 1) : ""),
         true);
}

}  // namespace

int App::running_jobs() const { return g_running; }

void App::transfer(const std::vector<std::string> &sources, const std::string &dest, Transfer kind, MainWindow *from,
                   bool select_results) {
  if (sources.empty() || dest.empty()) return;
  Plan plan = plan_transfer(sources, dest, kind, [from, kind](const Clash &c, bool &all) {
    return ask_clash(from, c, all, kind);
  });
  if (!plan.errors.empty()) report(from, plan.errors);
  if (plan.cancelled || plan.empty()) return;
  auto shared = std::make_shared<Plan>(plan);
  TransferDialog::Operation op = kind == Transfer::Move ? TransferDialog::Operation::Move : TransferDialog::Operation::Copy;
  std::string src_dir = common_folder(sources);
  bool was_cut = clipboard && clipboard->cut() && kind == Transfer::Move;
  new Job(op, folder_name(src_dir), folder_name(dest),
          [shared](desk::FileProgress *p, std::vector<std::string> *errors) { return run_plan(*shared, p, errors); },
          [this, shared, from, select_results, was_cut, dest, src_dir](int failed, bool cancelled,
                                                                       const std::vector<std::string> &errors) {
            MainWindow *w = alive(from);
            UndoStep step;
            step.kind = shared->kind == Transfer::Move ? UndoStep::Move : UndoStep::Copy;
            for (const auto &pr : shared->moved)
              if (desk::path_exists(pr.first)) step.pairs.push_back(pr);
            if (!cancelled || !step.pairs.empty()) undo.push(step);
            if (was_cut && !failed) clipboard->clear();
            for (auto *win : windows) {
              win->folder_changed(dest, {});
              if (!src_dir.empty()) win->folder_changed(src_dir, {});
            }
            if (w && select_results && w->tab().location == dest) {
              Tab &t = w->tab();
              t.select_when_loaded = shared->results.empty() ? std::string() : shared->results.front();
              t.also_select.insert(shared->results.begin(), shared->results.end());
              w->folder_changed(dest, {});
            }
            report(w, errors);
          });
}

void App::trash(const std::vector<std::string> &paths, MainWindow *from) {
  std::vector<std::string> trashable, other;
  for (const auto &p : paths) (can_trash(p) ? trashable : other).push_back(p);
  if (!other.empty()) {
    std::string text = other.size() == 1
                           ? format(_("“%s” cannot go to the Trash (it is on a drive or share without one). Delete it "
                                      "permanently? This cannot be undone."),
                                    basename_of(other[0]).c_str())
                           : format(_("%d items cannot go to the Trash (they are on a drive or share without one). "
                                      "Delete them permanently? This cannot be undone."),
                                    int(other.size()));
    if (confirm(from, _("Delete permanently?"), text, _("Delete")))
      new Job(TransferDialog::Operation::Delete, folder_name(common_folder(other)), "",
              [other](desk::FileProgress *p, std::vector<std::string> *errors) { return delete_paths(other, p, errors); },
              [this, other, from](int, bool, const std::vector<std::string> &errors) {
                for (auto *win : windows) win->folder_changed(common_folder(other), {});
                report(alive(from), errors);
              });
  }
  if (trashable.empty()) return;
  if (prefs.confirm_trash) {
    std::string text = trashable.size() == 1 ? format(_("Move “%s” to the Trash?"), basename_of(trashable[0]).c_str())
                                             : format(_("Move these %d items to the Trash?"), int(trashable.size()));
    if (!confirm(from, _("Move to the Trash?"), text, _("Move to Trash"), false)) return;
  }
  auto trashed = std::make_shared<std::vector<TrashItem>>();
  new Job(TransferDialog::Operation::Trash, folder_name(common_folder(trashable)), _("Trash"),
          [trashable, trashed](desk::FileProgress *p, std::vector<std::string> *errors) {
            return trash_paths(trashable, p, trashed.get(), errors);
          },
          [this, trashable, trashed, from](int, bool, const std::vector<std::string> &errors) {
            UndoStep step;
            step.kind = UndoStep::Trash;
            step.trashed = *trashed;
            undo.push(step);
            std::string dir = common_folder(trashable);
            for (auto *win : windows) {
              win->folder_changed(dir, {});
              win->folder_changed(kTrash, {});
            }
            MainWindow *w = alive(from);
            if (w && errors.empty() && !trashed->empty())
              w->say(trashed->size() == 1 ? format(_("Moved “%s” to the Trash. Undo with Ctrl+Z."),
                                                   basename_of(trashable[0]).c_str())
                                          : format(_("Moved %d items to the Trash. Undo with Ctrl+Z."),
                                                   int(trashed->size())));
            report(w, errors);
            places_changed();
          });
}

void App::delete_permanently(const std::vector<std::string> &paths, MainWindow *from) {
  if (paths.empty()) return;
  std::string text = paths.size() == 1
                         ? format(_("Permanently delete “%s”? This cannot be undone."), basename_of(paths[0]).c_str())
                         : format(_("Permanently delete these %d items? This cannot be undone."), int(paths.size()));
  if (!confirm(from, _("Delete permanently?"), text, _("Delete"))) return;
  // Items of the Trash leave their info files too.
  std::vector<TrashItem> items;
  std::vector<std::string> plain;
  for (const auto &p : paths) {
    TrashItem it;
    if (p.find("/files/") != std::string::npos && trash_item_of(p, it) &&
        file_exists(it.info_path()))
      items.push_back(it);
    else
      plain.push_back(p);
  }
  new Job(TransferDialog::Operation::Delete, folder_name(common_folder(paths)), "",
          [items, plain](desk::FileProgress *p, std::vector<std::string> *errors) {
            int failed = delete_paths(plain, p, errors);
            for (const auto &it : items) {
              std::string e;
              if (!erase_from_trash(it, &e)) {
                ++failed;
                errors->push_back(e);
              }
            }
            return failed;
          },
          [this, paths, from](int, bool, const std::vector<std::string> &errors) {
            for (auto *win : windows) {
              win->folder_changed(common_folder(paths), {});
              win->folder_changed(kTrash, {});
            }
            report(alive(from), errors);
            places_changed();
          });
}

void App::restore(const std::vector<TrashItem> &items, MainWindow *from) {
  if (items.empty()) return;
  auto restored = std::make_shared<std::vector<std::string>>();
  new Job(TransferDialog::Operation::Move, _("Trash"), "",
          [items, restored](desk::FileProgress *p, std::vector<std::string> *errors) {
            return restore_items(items, p, restored.get(), errors);
          },
          [this, restored, from](int, bool, const std::vector<std::string> &errors) {
            for (auto *win : windows) {
              win->folder_changed(kTrash, {});
              for (const auto &r : *restored) win->folder_changed(chooser::parent_dir(r), {});
            }
            MainWindow *w = alive(from);
            if (w && errors.empty())
              w->say(restored->size() == 1 ? format(_("Restored “%s”."), basename_of((*restored)[0]).c_str())
                                           : format(_("Restored %d items."), int(restored->size())));
            report(w, errors);
            places_changed();
          });
}

void App::empty_trash(MainWindow *from) {
  if (trash_is_empty()) return;
  if (!confirm(from, _("Empty the Trash?"),
               _("Everything in the Trash is deleted for good, also on the drives that are plugged in. This cannot be "
                 "undone."),
               _("Empty Trash")))
    return;
  new Job(TransferDialog::Operation::Delete, _("Trash"), "",
          [](desk::FileProgress *p, std::vector<std::string> *errors) { return empty_all_trash(p, errors); },
          [this, from](int, bool, const std::vector<std::string> &errors) {
            for (auto *win : windows) win->folder_changed(kTrash, {});
            report(alive(from), errors);
            places_changed();
          });
}

void App::undo_last(MainWindow *from) {
  // The operation still running is not on the list yet: Ctrl+Z pressed
  // during it (or right after a drop) would undo the one before.
  if (running_jobs() > 0) {
    if (from) from->say(_("Wait until the file operation has ended, then undo it."));
    return;
  }
  if (undo.empty()) {
    if (from) from->say(_("There is nothing to undo."));
    return;
  }
  UndoStep step = undo.pop();
  auto shared = std::make_shared<UndoStep>(step);
  new Job(step.kind == UndoStep::Trash ? TransferDialog::Operation::Move : step.kind == UndoStep::Copy ||
                                                                                   step.kind == UndoStep::Create
                                                                               ? TransferDialog::Operation::Trash
                                                                               : TransferDialog::Operation::Move,
          "", "",
          [shared](desk::FileProgress *p, std::vector<std::string> *errors) { return undo_step(*shared, p, errors); },
          [this, shared, from](int, bool, const std::vector<std::string> &errors) {
            std::set<std::string> dirs;
            for (const auto &pr : shared->pairs) {
              dirs.insert(chooser::parent_dir(pr.first));
              if (!pr.second.empty()) dirs.insert(chooser::parent_dir(pr.second));
            }
            for (const auto &it : shared->trashed) dirs.insert(chooser::parent_dir(it.original));
            dirs.insert(kTrash);
            for (auto *win : windows)
              for (const auto &d : dirs) win->folder_changed(d, {});
            MainWindow *w = alive(from);
            if (w && errors.empty()) w->say(_("Undone."));
            report(w, errors);
            places_changed();
          });
}

}  // namespace liria
}  // namespace sde
