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

#include "pagespeed/envoy/envoy_admin_fetch.h"

#include <memory>

#include "envoy/event/dispatcher.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/envoy/http_filter.h"
#include "pagespeed/kernel/base/message_handler.h"

namespace net_instaweb {

EnvoyAdminFetch::EnvoyAdminFetch(
    EnvoyServerContext* server_context,
    Envoy::Http::HttpPageSpeedDecoderFilter* decoder)
    : AsyncFetch(RequestContextPtr(server_context->NewRequestContext())) {
  // Set HTTP options on request context - required before accessing options()
  request_context()->set_options(
      server_context->global_options()->ComputeHttpOptions());
  decoder_.store(decoder, std::memory_order_release);
  if (decoder != nullptr) {
    // Constructed on the request's dispatcher thread, so this is the
    // dispatcher every reply must be sent on.
    dispatcher_ = &decoder->decoderCallbacks()->dispatcher();
    decoder->AttachAdminFetch(this);
  } else {
    // No filter will release its reference.
    references_.store(1, std::memory_order_release);
  }
}

EnvoyAdminFetch::~EnvoyAdminFetch() {}

bool EnvoyAdminFetch::HandleWrite(const StringPiece& sp,
                                  MessageHandler* handler) {
  sp.AppendToString(&buffer_);
  return true;
}

bool EnvoyAdminFetch::HandleFlush(MessageHandler* handler) { return true; }

void EnvoyAdminFetch::HandleHeadersComplete() {
  // Headers are set before Write calls - nothing to do here
  // as we'll send everything in HandleDone
}

void EnvoyAdminFetch::DecrementRefCount() {
  if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete this;
  }
}

void EnvoyAdminFetch::HandleDone(bool success) {
  if (dispatcher_ != nullptr && !dispatcher_->isThreadSafe()) {
    // Completed off the request's worker thread (fetcher, cache or scheduler
    // thread). Envoy's connection may only be written from its own
    // dispatcher, so post the reply there. The completion's reference is
    // released when the posted callback runs, or when it is destroyed
    // unrun.
    std::shared_ptr<EnvoyAdminFetch> self(
        this, [](EnvoyAdminFetch* fetch) { fetch->DecrementRefCount(); });
    dispatcher_->post([self]() { self->SendReplyOnDispatcher(); });
    return;
  }
  SendReplyOnDispatcher();
  DecrementRefCount();
}

void EnvoyAdminFetch::SendReplyOnDispatcher() {
  // Send the buffered response via Envoy, unless the filter is gone.
  auto* decoder = decoder_.load(std::memory_order_acquire);
  if (decoder != nullptr) {
    // Add security headers for admin endpoints before sending reply.
    // This is an admin endpoint so it should have restrictive security headers.
    response_headers()->Add("X-Frame-Options", "SAMEORIGIN");
    response_headers()->Add("X-Content-Type-Options", "nosniff");
    response_headers()->Add("X-XSS-Protection", "1; mode=block");
    response_headers()->Add(
        "Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'");
    // Set restrictive caching for admin endpoints.
    response_headers()->Replace(HttpAttributes::kCacheControl,
                                "private, no-store, no-cache, must-revalidate");

    decoder->sendReply(response_headers(), std::move(buffer_));
  }
}

}  // namespace net_instaweb
