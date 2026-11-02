// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's changes and their queue: one change runs at a time, the others
// wait; each can be paused, resumed and cancelled while its source allows
// it (0.2 Alpha 3.1).
//
//   Pause    stops a change safely while it downloads, and runs it again on
//            Resume: the sources keep what they downloaded (APT's
//            /var/cache/apt/archives and its partial/ downloads, the objects
//            OSTree already fetched for Flatpak, snapd's partial download).
//            A queued change can be paused before it starts.
//   Cancel   stops a running change (PackageKit's Cancel, a GCancellable for
//            libflatpak, snapd's abort) or takes a waiting or paused one
//            out of the queue.
//   Neither  while packages are being installed or removed (dpkg runs):
//            stopping then could leave the system half changed; the buttons
//            are off and say why.
//
// Display-free, for the unit tests; the Store runs the queue.
#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "model.h"

namespace sde {
namespace merkadu {

enum class Action { Install = 0, Remove = 1, Update = 2 };

// What a change does now, as its source reports it.
enum class JobPhase {
  Unknown,
  Waiting,      // for authorization, another program, the network, the source to start
  Preparing,    // resolving dependencies, reading the lists
  Downloading,  // stopping keeps what was downloaded
  Applying,     // changing the system in a way the source undoes when stopped (snapd, a Flatpak deploy)
  Changing,     // installing, updating or removing packages (dpkg): not safe to stop
};

struct Progress {
  int percent = -1;  // -1: unknown
  std::string status;
  bool cancellable = false;  // the source allows stopping it now
  JobPhase phase = JobPhase::Unknown;
};

// A change the user asked for; the queue runs them one after the other.
struct Job {
  int id = 0;
  Action action = Action::Install;
  Source source = Source::Debian;
  std::string app_id;   // the app it is about ("" for several apps)
  std::string title;    // what the progress view shows ("GIMP", "3 apps")
  std::string icon;
  // Packages, Flatpak items ("system:app/org.gimp.GIMP/x86_64/stable") or
  // snap names.
  std::vector<std::string> targets;
  std::string remote;   // Flatpak remote to install from; snap channel
  bool classic = false; // a snap with classic confinement
  int needs = 0;        // a job that must succeed first (else this one is cancelled)
  // Progress, updated while it runs.
  enum State { Queued, Running, Paused, Done, Failed, Cancelled };
  State state = Queued;
  int percent = -1;
  std::string status;
  std::string error;
  bool cancellable = false;
  JobPhase phase = JobPhase::Unknown;
  // A pause or a cancel is on its way to the source.
  enum Stop { NoStop, Pausing, Cancelling };
  Stop stopping = NoStop;
  int runs = 0;  // how often it was started (more than once after a pause)
  std::time_t finished = 0;
  bool active() const { return state == Queued || state == Running || state == Paused; }
};

// What the user can do with a job now; |why_not| says why Pause or Cancel
// is not possible while it runs ("" when they are).
struct JobControls {
  bool pause = false, resume = false, cancel = false;
  std::string why_not;
};
JobControls job_controls(const Job &j);

// The changes: one runs at a time, in the order they were asked for; a job
// that needs another waits for it and is cancelled when that one does not
// succeed.
class JobQueue {
 public:
  // Adds |j| (Queued) and returns its id.
  int add(Job j);
  Job *find(int id);
  const Job *find(int id) const;
  const std::vector<Job> &jobs() const { return jobs_; }
  bool running() const;
  // Jobs that are not finished (queued, running or paused).
  int active() const;
  // The job to start now: the first queued one whose dependency is done,
  // nullptr while one runs or none can. Jobs whose dependency failed or
  // was cancelled are cancelled on the way (their ids in |cancelled|).
  Job *next(std::time_t now, std::vector<int> *cancelled = nullptr);
  void start(int id);
  void progress(int id, const Progress &p);
  // The source is done with |id|. A job stopped for a pause is Paused (it
  // runs again after resume()), otherwise Done, Cancelled or Failed.
  void finished(int id, bool ok, bool cancelled, const std::string &error, std::time_t now);

  // The user's requests. true: the source must be asked to stop the
  // running job (Store: Backend::cancel()).
  bool pause(int id);
  bool cancel(int id, std::time_t now);
  // A paused job waits in the queue again; false when it is not paused.
  bool resume(int id);
  void clear_finished();

 private:
  std::vector<Job> jobs_;
  int next_id_ = 1;
};

// The job that installs |app| from |source| (its origin of that source),
// false when that is not possible (no such origin, installed already, not
// a valid package, ref or snap). The Flatpak remote is the origin's: the
// remote whose catalog lists the app.
bool make_install_job(const App &app, Source source, Job &out);

// "Installing", "Removing", "Updating" (translated).
std::string action_progress_label(Action a);
// "Install", "Remove", "Update".
std::string action_label(Action a);

}  // namespace merkadu
}  // namespace sde
