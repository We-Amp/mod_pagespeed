/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/nginx/ngx_daemon_record_completion.h"

#include <atomic>
#include <chrono>
#include <utility>

#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/ipro_recorder.h"

namespace net_instaweb {

const char kIproDaemonRecordDropped[] = "ipro_daemon_record_dropped";
const char kIproDaemonNotifyDropped[] = "ipro_daemon_notify_dropped";

namespace {

// Process-wide queue state.  The counts cover completions RESERVED, not
// completions running: a reservation is taken before the closure is handed
// to the pool and released by the closure's destructor, so a completion the
// pool cancelled at shutdown still releases its room.
struct DaemonCompletionQueue {
  std::atomic<int64> queued_bytes{0};
  std::atomic<int> queued_items{0};
  std::atomic<int64> last_open_attempt_ms{0};
};

// No-destructor function-local singletons: the queue may be touched from a
// pool thread while the process exits, and a destroyed-at-exit singleton
// would make that a use-after-free instead of a harmless late decrement.
DaemonCompletionQueue& Queue() {
  static DaemonCompletionQueue* queue = new DaemonCompletionQueue();
  return *queue;
}

int64 SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

DaemonNowMsFn& NowMsSlot() {
  static DaemonNowMsFn* fn = new DaemonNowMsFn(&SteadyNowMs);
  return *fn;
}

void NoteDrop(Variable* dropped) {
  if (dropped != nullptr) {
    dropped->Add(1);
  }
}

// The open attempt posted by PostDaemonOpenAttempt.  Cancel is a no-op:
// nothing was opened, so there is nothing to undo.
class DaemonOpenAttempt : public Function {
 public:
  explicit DaemonOpenAttempt(DaemonAdapter* adapter) : adapter_(adapter) {}
  void Run() override { adapter_->RecordCache(); }
  void Cancel() override {}

 private:
  DaemonAdapter* adapter_;
};

}  // namespace

void ps_daemon_record_completion_init_stats(Statistics* statistics) {
  if (statistics == nullptr) {
    return;
  }
  statistics->AddVariable(kIproDaemonRecordDropped);
  statistics->AddVariable(kIproDaemonNotifyDropped);
}

bool TryQueueDaemonCompletion(int64 body_bytes, Variable* dropped) {
  if (body_bytes > kDaemonCompletionQueueMaxBytes) {
    NoteDrop(dropped);
    return false;
  }
  DaemonCompletionQueue& queue = Queue();
  const int64 bytes =
      queue.queued_bytes.fetch_add(body_bytes, std::memory_order_relaxed);
  const int items = queue.queued_items.fetch_add(1, std::memory_order_relaxed);
  if (bytes + body_bytes > kDaemonCompletionQueueMaxBytes ||
      items + 1 > kDaemonCompletionQueueMaxItems) {
    queue.queued_bytes.fetch_sub(body_bytes, std::memory_order_relaxed);
    queue.queued_items.fetch_sub(1, std::memory_order_relaxed);
    NoteDrop(dropped);
    return false;
  }
  return true;
}

bool TryQueueDaemonNotification(Variable* dropped) {
  DaemonCompletionQueue& queue = Queue();
  const int items = queue.queued_items.fetch_add(1, std::memory_order_relaxed);
  if (items + 1 > kDaemonNotifyQueueMaxItems) {
    queue.queued_items.fetch_sub(1, std::memory_order_relaxed);
    NoteDrop(dropped);
    return false;
  }
  return true;
}

void DaemonCompletionExited(int64 body_bytes) {
  DaemonCompletionQueue& queue = Queue();
  queue.queued_bytes.fetch_sub(body_bytes, std::memory_order_relaxed);
  queue.queued_items.fetch_sub(1, std::memory_order_relaxed);
}

bool ShouldPostDaemonOpenAttempt() {
  const int64 now = NowMsSlot()();
  std::atomic<int64>& last = Queue().last_open_attempt_ms;
  int64 seen = last.load(std::memory_order_relaxed);
  while (seen == 0 || now - seen >= kDaemonOpenAttemptIntervalMs) {
    if (last.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

void PostDaemonOpenAttempt(QueuedWorkerPool::Sequence* sequence,
                           DaemonAdapter* adapter) {
  sequence->Add(new DaemonOpenAttempt(adapter));
}

void SetDaemonNowMsForTesting(DaemonNowMsFn fn) {
  NowMsSlot() = (fn == nullptr) ? DaemonNowMsFn(&SteadyNowMs) : std::move(fn);
}

void ResetDaemonCompletionQueueForTesting() {
  DaemonCompletionQueue& queue = Queue();
  queue.queued_bytes.store(0, std::memory_order_relaxed);
  queue.queued_items.store(0, std::memory_order_relaxed);
  queue.last_open_attempt_ms.store(0, std::memory_order_relaxed);
}

DaemonRecordCompletion::DaemonRecordCompletion(IproRecorder* recorder,
                                               ResponseHeaders* headers,
                                               bool complete, int64 body_bytes)
    : recorder_(recorder),
      headers_(headers),
      complete_(complete),
      body_bytes_(body_bytes) {}

DaemonRecordCompletion::~DaemonRecordCompletion() {
  delete headers_;
  DaemonCompletionExited(body_bytes_);
}

void DaemonRecordCompletion::Run() {
  recorder_->DoneAndSetHeaders(headers_, complete_);
  // DoneAndSetHeaders deletes the recorder but does not take the headers.
  delete headers_;
  headers_ = nullptr;
  recorder_ = nullptr;
}

void DaemonRecordCompletion::Cancel() {
  if (recorder_ != nullptr) {
    recorder_->DoneAndSetHeaders(nullptr, false);
    recorder_ = nullptr;
  }
}

}  // namespace net_instaweb
