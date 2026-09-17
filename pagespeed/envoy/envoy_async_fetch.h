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

// EnvoyAsyncFetch is an AsyncFetch adapter that receives ProxyFetch output
// and sends it to the client via Envoy's filter callbacks.
//
// Threading Model:
// ----------------
// ProxyFetch runs HTML parsing and rewriting on worker threads (html_workers).
// When rewriting completes, it calls HandleWrite/HandleDone on this adapter
// from the worker thread. EnvoyAsyncFetch must marshal these calls back to
// Envoy's dispatcher thread before accessing any Envoy APIs.
//
// The threading is:
//   Worker Thread                         Dispatcher Thread
//   -------------                         -----------------
//   HandleWrite() ----buffer---->         (accumulated in base class)
//   HandleFlush()  ----(no-op)---->       (no-op in base class)
//   HandleDone()   ----post---->          SendFinalResponse()
//
// Lifecycle Management:
// ---------------------
// The Envoy filter may be destroyed while ProxyFetch is still running on
// worker threads. EnvoyAsyncFetch uses:
// 1. std::shared_ptr ownership (enable_shared_from_this) for safe capture
// 2. Atomic filter_destroyed_ flag checked before accessing filter
// 3. Mutex protection for state accessed from both threads (via base class)
//
// When the filter is destroyed, it calls MarkFilterDestroyed(). After that:
// - Any pending worker callbacks will check the flag and no-op
// - HandleDone() will clean up without sending to Envoy

#ifndef PAGESPEED_ENVOY_ENVOY_ASYNC_FETCH_H_
#define PAGESPEED_ENVOY_ENVOY_ASYNC_FETCH_H_

#include <atomic>
#include <memory>

#include "envoy/event/dispatcher.h"
#include "net/instaweb/http/public/buffering_async_fetch.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"

namespace Envoy {
namespace Http {
class HttpPageSpeedDecoderFilter;
}
}  // namespace Envoy

namespace net_instaweb {

class EnvoyServerContext;

// AsyncFetch adapter for Envoy that receives ProxyFetch output.
// Thread-safe: HandleWrite/HandleFlush/HandleDone may be called from
// worker threads; this class posts work to the dispatcher thread.
//
// Extends BufferingAsyncFetch for body accumulation and completion tracking.
class EnvoyAsyncFetch : public BufferingAsyncFetch,
                        public std::enable_shared_from_this<EnvoyAsyncFetch> {
 public:
  // Creates an adapter for sending ProxyFetch output to the Envoy filter.
  // Does NOT take ownership of filter.
  // dispatcher is used to post work from worker threads to the main thread.
  EnvoyAsyncFetch(const RequestContextPtr& request_ctx,
                  Envoy::Http::HttpPageSpeedDecoderFilter* filter,
                  Envoy::Event::Dispatcher& dispatcher);

  ~EnvoyAsyncFetch() override;

  // Called by the Envoy filter when it's being destroyed.
  // After this call, no further Envoy APIs will be accessed.
  // Thread-safe: can be called from any thread.
  void MarkFilterDestroyed();

  // Called when HTML rewriting starts via ProxyFetch.
  // This creates a self-reference that keeps the object alive until
  // HandleDone() is called, preventing use-after-free when the filter
  // is destroyed while worker threads are still processing HTML.
  void StartHtmlRewriting();

 protected:
  // AsyncFetch interface implementation.
  void HandleHeadersComplete() override;

  // Called when fetch completes - posts to dispatcher thread.
  void OnDone(bool success) override;

 private:
  // Posts a function to run on the dispatcher thread.
  // The function captures this shared_ptr to prevent use-after-free.
  void PostToDispatcher(std::function<void()> fn);

  // Called on dispatcher thread to send the final response.
  void SendFinalResponse(bool success);

  // Check if the filter is still valid (not destroyed).
  bool IsFilterValid() const {
    return !filter_destroyed_.load(std::memory_order_acquire);
  }

  // Not owned - may outlive us if properly destroyed via MarkFilterDestroyed.
  Envoy::Http::HttpPageSpeedDecoderFilter* filter_;
  Envoy::Event::Dispatcher& dispatcher_;

  // State flags.
  std::atomic<bool> filter_destroyed_{false};
  std::atomic<bool> headers_complete_{false};

  // Self-reference that keeps this object alive during HTML rewriting.
  // Set by StartHtmlRewriting(), cleared in OnDone().
  // This prevents use-after-free when the filter is destroyed while
  // worker threads are still processing HTML via ProxyFetch.
  std::shared_ptr<EnvoyAsyncFetch> self_reference_;

  EnvoyAsyncFetch(const EnvoyAsyncFetch&) = delete;
  EnvoyAsyncFetch& operator=(const EnvoyAsyncFetch&) = delete;
};

// Shared pointer type for EnvoyAsyncFetch.
using EnvoyAsyncFetchPtr = std::shared_ptr<EnvoyAsyncFetch>;

}  // namespace net_instaweb

#endif  // PAGESPEED_ENVOY_ENVOY_ASYNC_FETCH_H_
