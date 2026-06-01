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

#include "pagespeed/envoy/envoy_base_fetch.h"

#include <memory>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/envoy/http_filter.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

EnvoyBaseFetch::EnvoyBaseFetch(StringPiece url,
                               EnvoyServerContext* server_context,
                               const RequestContextPtr& request_ctx,
                               PreserveCachingHeaders preserve_caching_headers,
                               const RewriteOptions* options,
                               Envoy::Http::HttpPageSpeedDecoderFilter* decoder)
    : AsyncFetch(request_ctx),
      url_(url.data(), url.size()),
      server_context_(server_context),
      options_(options),
      preserve_caching_headers_(preserve_caching_headers) {
  decoder_.store(decoder, std::memory_order_release);
}

bool EnvoyBaseFetch::HandleWrite(const StringPiece& sp, MessageHandler*) {
  buffer_.append(sp.data(), sp.size());
  return true;
}

void EnvoyBaseFetch::HandleHeadersComplete() {
  int status_code = response_headers()->status_code();
  bool continue_decoding = false;

  // Load the decoder pointer once with acquire semantics to ensure we see
  // all writes made before any DetachDecoder() call.
  auto* decoder = decoder_.load(std::memory_order_acquire);

  // Check if decoder is still valid before accessing it.
  // It may have been detached if the filter was destroyed.
  if (decoder == nullptr) {
    return;
  }

  if (status_code == CacheUrlAsyncFetcher::kNotInCacheStatus) {
    // Increment the refcount and hand ownership of the matching decrement to
    // a shared_ptr with a custom deleter. Capturing the shared_ptr by value
    // into the lambda ensures DecrementRefCount() fires whether the lambda
    // runs or is destroyed while still queued (e.g. dispatcher tear-down),
    // preventing leaks.
    IncrementRefCount();
    std::shared_ptr<EnvoyBaseFetch> self(
        this, [](EnvoyBaseFetch* f) { f->DecrementRefCount(); });
    // Post prepareForIproRecording to the dispatcher thread since this
    // callback may be running on a PageSpeed worker thread, but the filter
    // state must only be accessed from the main Envoy thread.
    decoder->decoderCallbacks()->dispatcher().post([this, self]() {
      // Re-check decoder_ inside the callback in case the filter was
      // destroyed between posting and execution.
      auto* dec = decoder_.load(std::memory_order_acquire);
      if (dec != nullptr) {
        dec->prepareForIproRecording();
        dec->decoderCallbacks()->continueDecoding();
      }
    });
  } else {
    bool ipro = !(status_code < 0 || status_code >= 400);
    have_ipro_response_.store(ipro, std::memory_order_release);
    continue_decoding = !ipro;

    // Track 404s for .pagespeed. resource fetches in statistics.
    if (status_code == HttpStatus::kNotFound) {
      server_context_->rewrite_stats()->resource_404_count()->Add(1);
    }

    if (continue_decoding) {
      IncrementRefCount();
      std::shared_ptr<EnvoyBaseFetch> self(
          this, [](EnvoyBaseFetch* f) { f->DecrementRefCount(); });
      decoder->decoderCallbacks()->dispatcher().post([this, self]() {
        auto* dec = decoder_.load(std::memory_order_acquire);
        if (dec != nullptr) {
          dec->decoderCallbacks()->continueDecoding();
        }
      });
    }
  }
}

bool EnvoyBaseFetch::HandleFlush(MessageHandler*) { return true; }

int EnvoyBaseFetch::DecrementRefCount() {
  return DecrefAndDeleteIfUnreferenced();
}

int EnvoyBaseFetch::IncrementRefCount() {
  // Use memory_order_acq_rel to ensure visibility of any writes made before
  // the increment is visible to other threads, and that we see any writes
  // made by other threads before their increments.
  return references_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

int EnvoyBaseFetch::DecrefAndDeleteIfUnreferenced() {
  // Use memory_order_acq_rel for the decrement to ensure:
  // 1. All writes before the decrement are visible to other threads (release)
  // 2. We see all writes made by other threads before their decrements (acquire)
  int old_val = references_.fetch_sub(1, std::memory_order_acq_rel);
  int new_val = old_val - 1;
  if (new_val == 0) {
    // Use acquire fence to ensure we see all writes made by other threads
    // before we delete the object.
    std::atomic_thread_fence(std::memory_order_acquire);
    delete this;
  }
  return new_val;
}

void EnvoyBaseFetch::HandleDone(bool success) {
  // Load the decoder pointer once with acquire semantics to ensure we see
  // all writes made before any DetachDecoder() call.
  auto* decoder = decoder_.load(std::memory_order_acquire);

  // Check if decoder is still valid before accessing it.
  if (decoder == nullptr) {
    DecrefAndDeleteIfUnreferenced();
    return;
  }

  if (have_ipro_response_.load(std::memory_order_acquire)) {
    IncrementRefCount();
    std::shared_ptr<EnvoyBaseFetch> self(
        this, [](EnvoyBaseFetch* f) { f->DecrementRefCount(); });
    if (!success) {
      decoder->decoderCallbacks()->dispatcher().post([this, self]() {
        auto* dec = decoder_.load(std::memory_order_acquire);
        if (dec != nullptr) {
          dec->decoderCallbacks()->continueDecoding();
        }
      });
    } else {
      decoder->decoderCallbacks()->dispatcher().post([this, self]() {
        auto* dec = decoder_.load(std::memory_order_acquire);
        if (dec != nullptr) {
          dec->sendReply(response_headers(), buffer_);
        }
      });
    }
  }

  DecrefAndDeleteIfUnreferenced();
}

bool EnvoyBaseFetch::IsCachedResultValid(const ResponseHeaders& headers) {
  return OptionsAwareHTTPCacheCallback::IsCacheValid(
      url_, *options_, request_context(), headers);
}

}  // namespace net_instaweb
