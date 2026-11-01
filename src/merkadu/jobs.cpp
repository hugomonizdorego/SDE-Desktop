// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "jobs.h"

#include <algorithm>

#include "i18n.h"
#include "util.h"

namespace sde {
namespace merkadu {

std::string action_progress_label(Action a) {
  switch (a) {
    case Action::Remove: return _("Removing");
    case Action::Update: return _("Updating");
    default: return _("Installing");
  }
}

std::string action_label(Action a) {
  switch (a) {
    case Action::Remove: return _("Remove");
    case Action::Update: return _("Update");
    default: return _("Install");
  }
}

JobControls job_controls(const Job &j) {
  JobControls c;
  switch (j.state) {
    case Job::Queued:
      // Not started yet: it can wait, or leave the queue.
      c.pause = c.cancel = true;
      break;
    case Job::Paused:
      c.resume = c.cancel = true;
      break;
    case Job::Running:
      if (j.stopping != Job::NoStop) {
        c.why_not = j.stopping == Job::Pausing ? _("Pausing\xE2\x80\xA6") : _("Cancelling\xE2\x80\xA6");
        break;
      }
      if (j.phase == JobPhase::Changing) {
        // dpkg (or a deploy) is changing the system right now.
        c.why_not = j.action == Action::Remove
                        ? _("The software is being removed: stopping now could leave the system half changed. "
                            "Pause and Cancel come back when this part is done.")
                        : _("The software is being installed: stopping now could leave the system half changed. "
                            "Pause and Cancel come back when this part is done.");
        break;
      }
      c.cancel = j.cancellable;
      // Pausing keeps what was downloaded: only before the system is
      // changed, and only for what downloads (a removal does not).
      c.pause = j.cancellable && j.action != Action::Remove &&
                (j.phase == JobPhase::Downloading || j.phase == JobPhase::Waiting || j.phase == JobPhase::Preparing);
      if (!j.cancellable)
        c.why_not = _("The source of this change does not allow stopping it at the moment.");
      else if (!c.pause && j.phase == JobPhase::Applying)
        c.why_not = _("It is being set up: it can still be cancelled, which undoes it, but not paused.");
      break;
    default:
      break;
  }
  return c;
}

int JobQueue::add(Job j) {
  j.id = next_id_++;
  j.state = Job::Queued;
  j.stopping = Job::NoStop;
  jobs_.push_back(j);
  return jobs_.back().id;
}

Job *JobQueue::find(int id) {
  for (auto &j : jobs_)
    if (j.id == id) return &j;
  return nullptr;
}

const Job *JobQueue::find(int id) const { return const_cast<JobQueue *>(this)->find(id); }

bool JobQueue::running() const {
  return std::any_of(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.state == Job::Running; });
}

int JobQueue::active() const {
  return int(std::count_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.active(); }));
}

Job *JobQueue::next(std::time_t now, std::vector<int> *cancelled) {
  if (running()) return nullptr;
  for (auto &j : jobs_) {
    if (j.state != Job::Queued) continue;
    if (j.needs) {
      const Job *dep = find(j.needs);
      // It waits while what it needs is not finished (also while paused).
      if (dep && dep->active()) continue;
      if (!dep || dep->state != Job::Done) {
        j.state = Job::Cancelled;
        j.error = _("The change it needs did not succeed.");
        j.finished = now;
        if (cancelled) cancelled->push_back(j.id);
        continue;
      }
    }
    return &j;
  }
  return nullptr;
}

void JobQueue::start(int id) {
  Job *j = find(id);
  if (!j) return;
  j->state = Job::Running;
  j->stopping = Job::NoStop;
  j->cancellable = false;
  j->phase = JobPhase::Unknown;
  j->error.clear();
  j->status = action_progress_label(j->action);
  ++j->runs;
}

void JobQueue::progress(int id, const Progress &p) {
  Job *j = find(id);
  if (!j || j->state != Job::Running) return;
  j->percent = p.percent;
  if (!p.status.empty()) j->status = p.status;
  j->cancellable = p.cancellable;
  if (p.phase != JobPhase::Unknown) j->phase = p.phase;
}

void JobQueue::finished(int id, bool ok, bool cancelled, const std::string &error, std::time_t now) {
  Job *j = find(id);
  if (!j) return;
  Job::Stop stopping = j->stopping;
  j->stopping = Job::NoStop;
  j->cancellable = false;
  if (ok) {
    j->state = Job::Done;
    j->percent = 100;
    j->error.clear();
  } else if (cancelled && stopping == Job::Pausing) {
    // Stopped for a pause: it runs again on Resume and continues with what
    // was downloaded.
    j->state = Job::Paused;
    j->status = _("Paused");
    j->error.clear();
    return;
  } else {
    j->state = cancelled ? Job::Cancelled : Job::Failed;
    j->error = error;
  }
  j->finished = now;
}

bool JobQueue::pause(int id) {
  Job *j = find(id);
  if (!j) return false;
  if (j->state == Job::Queued) {
    j->state = Job::Paused;
    j->status = _("Paused");
    return false;
  }
  if (j->state != Job::Running || !job_controls(*j).pause) return false;
  j->stopping = Job::Pausing;
  return true;
}

bool JobQueue::cancel(int id, std::time_t now) {
  Job *j = find(id);
  if (!j) return false;
  if (j->state == Job::Queued || j->state == Job::Paused) {
    j->state = Job::Cancelled;
    j->finished = now;
    return false;
  }
  if (j->state != Job::Running || !job_controls(*j).cancel) return false;
  j->stopping = Job::Cancelling;
  return true;
}

bool JobQueue::resume(int id) {
  Job *j = find(id);
  if (!j || j->state != Job::Paused) return false;
  j->state = Job::Queued;
  j->status.clear();
  return true;
}

void JobQueue::clear_finished() {
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return !j.active(); }), jobs_.end());
}

bool make_install_job(const App &app, Source source, Job &out) {
  const Origin *o = app.origin(source);
  if (!o || o->installed) return false;
  Job j;
  j.action = Action::Install;
  j.source = source;
  j.app_id = app.id;
  j.title = app.name;
  j.icon = app.icon;
  j.remote = o->remote;
  if (source == Source::Debian) {
    if (!valid_package_name(o->id)) return false;
    j.targets = {o->id};
  } else if (source == Source::Flatpak) {
    if (!valid_flatpak_ref(o->id)) return false;
    j.targets = {std::string(o->installation.empty() ? "system" : o->installation) + ":" + o->id};
  } else {
    if (!valid_snap_name(o->id)) return false;
    j.targets = {o->id};
    j.classic = o->classic;
  }
  out = j;
  return true;
}

}  // namespace merkadu
}  // namespace sde
