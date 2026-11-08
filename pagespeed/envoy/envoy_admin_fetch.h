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

#pragma once

#include <atomic>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace Envoy {
namespace Event {
class Dispatcher;
}
namespace Http {
class HttpPageSpeedDecoderFilter;
}
}  // namespace Envoy

namespace net_instaweb {

class EnvoyServerContext;

// AsyncFetch implementation for admin/statistics pages.
// Buffers the response body from AdminSite handlers and sends
// the complete response via Envoy's sendLocalReply().
//
// The reply is always sent on the dispatcher of the worker that owns the
// request: a completion that arrives on any other thread (a fetcher, cache or
// scheduler thread) posts the reply to that dispatcher. The fetch registers
// itself with its filter, which detaches it when the filter is destroyed; a
// reply that arrives after that is dropped.
//
// Lifetime: two references, one held by the filter (released by
// DetachDecoder() followed by DecrementRefCount()) and one by the completion
// (released once the reply has been sent or dropped). The fetch deletes
// itself when both are gone.
class EnvoyAdminFetch : public AsyncFetch {
 public:
  EnvoyAdminFetch(EnvoyServerContext* server_context,
                  Envoy::Http::HttpPageSpeedDecoderFilter* decoder);
  ~EnvoyAdminFetch() override;

  // Called by the filter, on its dispatcher thread, before it is destroyed.
  void DetachDecoder() { decoder_.store(nullptr, std::memory_order_release); }
  // Releases one reference; deletes the fetch when none remain.
  void DecrementRefCount();

 protected:
  bool HandleWrite(const StringPiece& sp, MessageHandler* handler) override;
  bool HandleFlush(MessageHandler* handler) override;
  void HandleHeadersComplete() override;
  void HandleDone(bool success) override;

 private:
  // Sends the buffered reply through the filter if it is still attached.
  // Runs on the dispatcher thread only.
  void SendReplyOnDispatcher();

  std::atomic<Envoy::Http::HttpPageSpeedDecoderFilter*> decoder_{nullptr};
  // The dispatcher of the worker that owns the request; outlives the filter.
  Envoy::Event::Dispatcher* dispatcher_{nullptr};
  std::atomic<int> references_{2};
  GoogleString buffer_;

  EnvoyAdminFetch(const EnvoyAdminFetch&) = delete;
  EnvoyAdminFetch& operator=(const EnvoyAdminFetch&) = delete;
};

}  // namespace net_instaweb
