// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/config_persister.h"

#include <cstdint>
#include <system_error>
#include <utility>

#include "lib/base/message_handler.h"

namespace pagespeed {

namespace {

// Thread-safe errno text (strerror may use a shared buffer).
std::string ErrnoText(int err) {
  if (err == 0) return "unknown error";
  return std::error_code(err, std::generic_category()).message();
}

}  // namespace

ConfigPersister::ConfigPersister(uv_loop_t* loop, MessageHandler* handler,
                                 std::function<void()> before_write,
                                 std::chrono::milliseconds drain_timeout)
    : loop_(loop),
      handler_(handler),
      drain_timeout_(drain_timeout),
      core_(std::make_shared<Core>()) {
  core_->before_write = std::move(before_write);
  auto* async = new uv_async_t;
  async->data = this;
  if (uv_async_init(loop_, async, OnAsync) != 0) {
    delete async;
    core_->stop = true;  // Never start the thread; Submit() fails fast.
    return;
  }
  core_->async = async;
  thread_ = std::thread([core = core_] { ThreadMain(core); });
}

ConfigPersister::~ConfigPersister() { Shutdown(); }

void ConfigPersister::Submit(Job job, Done done) {
  {
    std::lock_guard<std::mutex> lock(core_->mu);
    if (!core_->stop) {
      const uint64_t gen = ++core_->next_gen;
      if (core_->pending.has_value()) {
        // Supersede the waiting job: its state is contained in this one.
        job.shared_dirty = job.shared_dirty || core_->pending->shared_dirty;
      }
      core_->pending = std::move(job);
      core_->pending_gen = gen;
      core_->waiters.push_back(Waiter{gen, std::move(done)});
      core_->cv.notify_one();
      return;
    }
  }
  Result failed;
  failed.json_ok = false;
  failed.json_error = "configuration persistence is shut down";
  if (done) done(failed);
}

std::vector<ConfigPersister::Waiter> ConfigPersister::TakeWaiters(
    uint64_t gen) {
  std::vector<Waiter> taken;
  std::lock_guard<std::mutex> lock(core_->mu);
  while (!core_->waiters.empty() && core_->waiters.front().gen <= gen) {
    taken.push_back(std::move(core_->waiters.front()));
    core_->waiters.pop_front();
  }
  return taken;
}

void ConfigPersister::ThreadMain(std::shared_ptr<Core> core) {
  for (;;) {
    Job job;
    uint64_t gen;
    {
      std::unique_lock<std::mutex> lock(core->mu);
      core->cv.wait(lock,
                    [&] { return core->pending.has_value() || core->stop; });
      if (!core->pending.has_value()) break;  // stop and nothing left to write.
      job = std::move(*core->pending);
      core->pending.reset();
      gen = core->pending_gen;
    }

    if (core->before_write) core->before_write();
    Result result;
    if (job.write_json) {
      if (const int err = job.write_json(); err != 0) {
        result.json_ok = false;
        result.json_error = ErrnoText(err);
      }
    }
    if (job.shared_dirty && job.write_shared) {
      result.shared_attempted = true;
      if (const int err = job.write_shared(); err != 0) {
        result.shared_ok = false;
        result.shared_error = ErrnoText(err);
      }
    }

    std::lock_guard<std::mutex> lock(core->mu);
    core->completions.push_back(Completion{gen, std::move(result)});
    // Sent under the lock: Shutdown() nulls `async` under it before closing.
    if (core->async != nullptr) uv_async_send(core->async);
  }
  std::lock_guard<std::mutex> lock(core->mu);
  core->exited = true;
  core->exit_cv.notify_all();
}

void ConfigPersister::OnAsync(uv_async_t* handle) {
  static_cast<ConfigPersister*>(handle->data)->DeliverCompletions();
}

void ConfigPersister::DeliverCompletions() {
  for (;;) {
    Completion c;
    {
      std::lock_guard<std::mutex> lock(core_->mu);
      if (core_->completions.empty()) return;
      c = std::move(core_->completions.front());
      core_->completions.pop_front();
    }
    if (handler_ != nullptr) {
      if (!c.result.json_ok) {
        handler_->Warning("config: failed to write pagespeed.json: %s",
                          c.result.json_error.c_str());
      }
      if (c.result.shared_attempted && !c.result.shared_ok) {
        handler_->Warning("config: failed to write pagespeed-shared.conf: %s",
                          c.result.shared_error.c_str());
      }
    }
    for (auto& w : TakeWaiters(c.gen)) {
      if (w.done) w.done(c.result);
    }
  }
}

void ConfigPersister::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(core_->mu);
    if (core_->stop && !thread_.joinable() && core_->async == nullptr) return;
    core_->stop = true;
  }
  core_->cv.notify_one();
  if (thread_.joinable()) {
    bool exited;
    {
      std::unique_lock<std::mutex> lock(core_->mu);
      exited = core_->exit_cv.wait_for(lock, drain_timeout_,
                                       [&] { return core_->exited; });
    }
    if (exited) {
      thread_.join();
    } else {
      // A hung disk must not hang shutdown: leave the thread to finish (it
      // holds its own reference to the shared state) and fail what waits.
      if (handler_ != nullptr) {
        handler_->Warning(
            "config: configuration save did not finish within %lld ms at "
            "shutdown; continuing without waiting for it",
            static_cast<long long>(drain_timeout_.count()));
      }
      thread_.detach();
    }
  }

  // Everything the thread wrote is now in completions; deliver it.
  if (core_->async != nullptr) DeliverCompletions();

  // Anything still waiting was never written (after a drain timeout, or never
  // started): fail it rather than leave it hanging.
  std::vector<Waiter> rest = TakeWaiters(UINT64_MAX);
  Result failed;
  failed.json_ok = false;
  failed.json_error = "configuration persistence is shut down";
  for (auto& w : rest) {
    if (w.done) w.done(failed);
  }

  uv_async_t* async = nullptr;
  {
    std::lock_guard<std::mutex> lock(core_->mu);
    async = core_->async;
    core_->async = nullptr;
  }
  if (async != nullptr) {
    uv_close(reinterpret_cast<uv_handle_t*>(async),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_async_t*>(h); });
  }
}

}  // namespace pagespeed
