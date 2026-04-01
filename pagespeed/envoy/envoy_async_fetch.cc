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

#include "pagespeed/envoy/envoy_async_fetch.h"

#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/envoy/http_filter.h"

namespace net_instaweb {

EnvoyAsyncFetch::EnvoyAsyncFetch(
    const RequestContextPtr& request_ctx,
    Envoy::Http::HttpPageSpeedDecoderFilter* filter,
    Envoy::Event::Dispatcher& dispatcher, EnvoyServerContext* server_context)
    : BufferingAsyncFetch(request_ctx),
      filter_(filter),
      dispatcher_(dispatcher),
      server_context_(server_context) {}

EnvoyAsyncFetch::~EnvoyAsyncFetch() {
  // Nothing to clean up - all resources are either owned elsewhere or
  // cleaned up in OnDone.
}

void EnvoyAsyncFetch::MarkFilterDestroyed() {
  filter_destroyed_.store(true, std::memory_order_release);
}

void EnvoyAsyncFetch::StartHtmlRewriting() {
  // Create a self-reference to keep this object alive during HTML rewriting.
  // This is necessary because ProxyFetch runs on worker threads and may
  // access this object (and its response headers) after the Envoy filter
  // is destroyed. The self-reference is cleared in OnDone().
  std::lock_guard<std::mutex> lock(mutex());
  self_reference_ = shared_from_this();
}

void EnvoyAsyncFetch::HandleHeadersComplete() {
  headers_complete_.store(true, std::memory_order_release);
  // Headers will be sent with the body in OnDone.
  // We don't need to do anything here for streaming since ProxyFetch
  // buffers everything anyway when using CreateNewProxyFetch.
}

void EnvoyAsyncFetch::OnDone(bool success) {
  // Note: mutex() is already held by BufferingAsyncFetch::HandleDone()
  // when this is called.

  // Clear the self-reference now that HTML rewriting is complete.
  // This allows the object to be destroyed after we're done here.
  self_reference_.reset();

  // Check if filter is already destroyed.
  if (!IsFilterValid()) {
    // Filter is gone. The shared_ptr prevents us from being deleted mid-call,
    // but we can't access the filter. Just return.
    return;
  }

  // Post the response sending to the dispatcher thread.
  // Capture shared_from_this() to keep us alive until the lambda runs.
  auto self = shared_from_this();
  dispatcher_.post([self, success]() { self->SendFinalResponse(success); });
}

void EnvoyAsyncFetch::PostToDispatcher(std::function<void()> fn) {
  auto self = shared_from_this();
  dispatcher_.post([self, fn = std::move(fn)]() {
    if (self->IsFilterValid()) {
      fn();
    }
  });
}

void EnvoyAsyncFetch::SendFinalResponse(bool success) {
  // Must be called on dispatcher thread.
  if (!IsFilterValid()) {
    return;
  }

  // Get the accumulated body - use ReleaseBody() which handles locking.
  GoogleString body_content = ReleaseBody();

  // Send the response via the filter.
  // The filter's sendReply method handles converting headers and sending.
  filter_->sendReply(response_headers(), std::move(body_content));

  // Record metrics if this was an HTML rewrite.
  filter_->RecordHtmlRewriteComplete(success, /* timeout= */ false);
}

}  // namespace net_instaweb
