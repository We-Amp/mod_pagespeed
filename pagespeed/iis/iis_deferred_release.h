// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_IIS_IIS_DEFERRED_RELEASE_H_
#define PAGESPEED_IIS_IIS_DEFERRED_RELEASE_H_

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace net_instaweb {

// Runs tasks on one dedicated thread, in the order they were posted. The IIS
// port uses it to retire a replaced site state: the last reference to it can
// be dropped on one of that state's own worker threads, which must never be
// the thread that shuts its pools down (it would wait for itself), and
// nobody should wait for that shutdown while holding a request or global
// lock.
//
// The thread starts on the first Post, never at construction, so creating a
// queue in a DLL is safe under the loader lock. Header-only and free of
// IIS/Windows headers so it builds on any platform.
class DeferredReleaseQueue {
 public:
  DeferredReleaseQueue() : stopping_(false) {}
  ~DeferredReleaseQueue() { StopAndDrain(); }

  DeferredReleaseQueue(const DeferredReleaseQueue&) = delete;
  DeferredReleaseQueue& operator=(const DeferredReleaseQueue&) = delete;

  // Runs |task| on the release thread, starting it on first use. After
  // StopAndDrain() has begun, or if the thread cannot be created, it runs
  // |task| inline on the caller. Never throws from thread creation.
  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!stopping_) {
        bool ok = true;
        if (!thread_.joinable()) {
          try {
            thread_ = std::thread([this] { Run(); });
          } catch (...) {
            ok = false;
          }
        }
        if (ok) {
          tasks_.push_back(std::move(task));
          wake_.notify_one();
          return;
        }
        static std::atomic<bool> logged(false);
        if (!logged.exchange(true)) {
          std::fputs("DeferredReleaseQueue: cannot start the release thread; "
                     "releasing inline\n", stderr);
        }
      }
    }
    task();
  }

  // Runs every queued task (including tasks posted by tasks), then joins the
  // thread. Idempotent. Must not be called from a task.
  void StopAndDrain() {
    std::thread to_join;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      to_join = std::move(thread_);
    }
    wake_.notify_all();
    if (to_join.joinable()) {
      to_join.join();
    }
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          return;  // stopping, and nothing left to run
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      // Outside the mutex: a task may Post another task.
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> tasks_;
  std::thread thread_;
  bool stopping_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_DEFERRED_RELEASE_H_
