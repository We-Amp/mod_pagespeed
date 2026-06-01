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
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/headers.h"

namespace Envoy {
namespace Http {
class HttpPageSpeedDecoderFilter;
}
}  // namespace Envoy

namespace net_instaweb {

enum PreserveCachingHeaders {
  kPreserveAllCachingHeaders,  // Cache-Control, ETag, Last-Modified, etc
  kPreserveOnlyCacheControl,   // Only Cache-Control.
  kDontPreserveHeaders,
};

class EnvoyBaseFetch : public AsyncFetch {
 public:
  EnvoyBaseFetch(StringPiece url, EnvoyServerContext* server_context,
                 const RequestContextPtr& request_ctx,
                 PreserveCachingHeaders preserve_caching_headers,
                 const RewriteOptions* options,
                 Envoy::Http::HttpPageSpeedDecoderFilter* decoder);

  // Called by Envoy to decrement the refcount.
  // Returns the new reference count.
  int DecrementRefCount();
  // Called by pagespeed to increment the refcount.
  // Returns the new reference count.
  int IncrementRefCount();
  bool IsCachedResultValid(const ResponseHeaders& headers) override;
  // Called when the filter is being destroyed to prevent use-after-free.
  // Must be called from the dispatcher thread before the filter is destroyed.
  void DetachDecoder() { decoder_.store(nullptr, std::memory_order_release); }

 private:
  bool HandleWrite(const StringPiece& sp, MessageHandler* handler) override;
  bool HandleFlush(MessageHandler* handler) override;
  void HandleHeadersComplete() override;
  void HandleDone(bool success) override;

  // Decrements reference count and deletes if zero. Returns new count.
  int DecrefAndDeleteIfUnreferenced();

  GoogleString url_;
  GoogleString buffer_;
  EnvoyServerContext* server_context_{nullptr};
  const RewriteOptions* options_{nullptr};
  // Reference count using std::atomic for proper thread-safety.
  // Starts at 2: one for the Envoy filter, one for PageSpeed.
  // Uses memory_order_acq_rel for modifications to ensure visibility
  // across threads, and memory_order_acquire for the final read before delete.
  std::atomic<int> references_{2};
  PreserveCachingHeaders preserve_caching_headers_;
  // Set to true just before the Envoy side releases its reference
  bool have_ipro_response_{false};
  // Atomic pointer to avoid race between worker threads and dispatcher thread.
  // Use memory_order_acquire for loads, memory_order_release for stores.
  std::atomic<Envoy::Http::HttpPageSpeedDecoderFilter*> decoder_{nullptr};

  DISALLOW_COPY_AND_ASSIGN(EnvoyBaseFetch);
};

}  // namespace net_instaweb
