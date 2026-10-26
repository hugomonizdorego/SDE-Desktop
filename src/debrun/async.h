// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Work on a worker thread whose result is handed back to the FLTK loop
// (polled, as in the Control Center): reading a package, APT's simulation
// and hashing never block Debrun's window.
#pragma once

#include <FL/Fl.H>

#include <atomic>
#include <functional>
#include <thread>

namespace sde {
namespace debrun {

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
        Fl::repeat_timeout(0.03, poll, data);
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
  Fl::add_timeout(0.03, Job::poll, job);
}

}  // namespace debrun
}  // namespace sde
