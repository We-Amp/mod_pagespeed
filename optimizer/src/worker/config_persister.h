// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Configuration persistence thread
//
// Saving the configuration files (write, fsync, rename) can take seconds on a
// busy disk.  Doing that on the worker's event-loop thread stalls every other
// request on the loop, health checks included.  ConfigPersister moves the
// disk work to one dedicated thread while keeping the contract that a
// configuration PATCH is answered only once its change is saved:
//
//   * Submit() (event-loop thread) hands over a job and a completion callback
//     and returns at once.
//   * One thread runs the jobs, one at a time, in submission order.  At most
//     one job waits: a job submitted while another is still pending replaces
//     it (the newer job's state already contains the older change), so a
//     burst of PATCHes becomes one write.  Every PATCH covered by a write is
//     completed with that write's outcome.
//   * Completion callbacks run back on the event-loop thread (uv_async_t).
//
// Lifetime: the persister owns one thread and one uv_async_t on the loop it
// was built with.  Shutdown() must be called on the loop thread before the
// loop stops; it writes any pending job, joins the thread and closes the
// handle.  The destructor calls it if the owner did not.

#ifndef PAGESPEED_SRC_WORKER_CONFIG_PERSISTER_H_
#define PAGESPEED_SRC_WORKER_CONFIG_PERSISTER_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "uv.h"

namespace pagespeed {

class MessageHandler;

class ConfigPersister {
 public:
  // Runs on the persister thread; returns 0 when the file is saved, else the
  // errno-style code of the failure.  The writer reads errno right at the
  // failing call, so later cleanup cannot overwrite the cause.
  using WriteFn = std::function<int()>;

  struct Job {
    // Saves the main configuration file.
    WriteFn write_json;
    // Saves the shared configuration file.  Runs only when `shared_dirty`.
    // Always set it when `shared_dirty` may be true.
    WriteFn write_shared;
    // True when this change touches a shared-file field.  Coalescing ORs it
    // in, so a superseded job's shared change is never lost.
    bool shared_dirty = false;
  };

  struct Result {
    bool json_ok = true;
    std::string json_error;  // Cause text when !json_ok.
    bool shared_attempted = false;
    bool shared_ok = true;
    std::string shared_error;  // Cause text when !shared_ok.
  };

  using Done = std::function<void(const Result&)>;

  // `loop` must outlive the persister's Shutdown().  `handler` (optional) gets
  // a warning for every failed write.  `before_write` (optional, a test seam)
  // runs on the persister thread before each job's writes; a test blocks in it
  // to hold the disk work "in flight" deterministically.  `drain_timeout`
  // bounds how long Shutdown() waits for an in-flight write (a hung disk).
  ConfigPersister(
      uv_loop_t* loop, MessageHandler* handler,
      std::function<void()> before_write = nullptr,
      std::chrono::milliseconds drain_timeout = std::chrono::seconds(30));
  ~ConfigPersister();

  ConfigPersister(const ConfigPersister&) = delete;
  ConfigPersister& operator=(const ConfigPersister&) = delete;

  // The handler warnings go to (may be null).
  MessageHandler* handler() const { return handler_; }

  // Event-loop thread only.  `done` runs later on the loop thread, after the
  // write that covers this job has finished.  After Shutdown() the job is not
  // run and `done` is called at once with a failure.
  void Submit(Job job, Done done);

  // Event-loop thread only.  Writes any pending job, joins the thread,
  // completes the waiters it covers and closes the async handle.  Idempotent.
  // If the thread has not finished within the drain timeout (a hung disk), a
  // warning is logged, the thread is left to finish on its own and every
  // waiting request is failed, so shutdown always proceeds.
  void Shutdown();

 private:
  struct Waiter {
    uint64_t gen;
    Done done;
  };
  struct Completion {
    uint64_t gen;
    Result result;
  };

  // State the persister thread touches.  Shared with the thread so that a
  // thread abandoned after a drain timeout never reads freed memory.
  struct Core {
    std::mutex mu;
    std::condition_variable cv;       // Wakes the thread.
    std::condition_variable exit_cv;  // Signals that the thread has returned.
    bool stop = false;
    bool exited = false;
    uint64_t next_gen = 0;
    std::optional<Job> pending;
    uint64_t pending_gen = 0;
    std::deque<Waiter> waiters;
    std::deque<Completion> completions;
    uv_async_t* async = nullptr;  // Heap; null once closed.  Guarded by mu.
    std::function<void()> before_write;
  };

  static void ThreadMain(std::shared_ptr<Core> core);
  static void OnAsync(uv_async_t* handle);
  void DeliverCompletions();
  // Removes and returns, in order, the waiters with gen <= `gen`.
  std::vector<Waiter> TakeWaiters(uint64_t gen);

  uv_loop_t* loop_;
  MessageHandler* handler_;
  std::chrono::milliseconds drain_timeout_;
  std::shared_ptr<Core> core_;
  std::thread thread_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CONFIG_PERSISTER_H_
