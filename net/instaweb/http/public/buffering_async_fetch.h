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

// BufferingAsyncFetch is a base class for AsyncFetch implementations that
// buffer response body content and track completion state with a mutex.
//
// This base class extracts common functionality from EnvoyAsyncFetch and
// IisAsyncFetch, which both:
//   1. Accumulate body content under mutex protection
//   2. Track done/success state
//   3. Return true from HandleFlush (no-op)
//   4. Return true from HandleWrite for empty content
//
// Subclasses must implement:
//   - HandleHeadersComplete() - called when headers are ready
//   - OnDone(bool success) - called when fetch completes (under mutex)
//
// Thread-safety: HandleWrite, HandleFlush, and HandleDone may be called from
// any thread. All access to body_, done_, and success_ is mutex-protected.

#ifndef NET_INSTAWEB_HTTP_PUBLIC_BUFFERING_ASYNC_FETCH_H_
#define NET_INSTAWEB_HTTP_PUBLIC_BUFFERING_ASYNC_FETCH_H_

#include <mutex>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class MessageHandler;

// Base class for AsyncFetch implementations that buffer body content.
// Thread-safe: all operations are mutex-protected.
class BufferingAsyncFetch : public AsyncFetch {
 public:
  explicit BufferingAsyncFetch(const RequestContextPtr& request_ctx);
  ~BufferingAsyncFetch() override;

  // Returns true if HandleDone has been called with success=true.
  bool success() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return success_;
  }

  // Returns true if HandleDone has been called.
  bool done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
  }

  // Returns a copy of the accumulated body content.
  GoogleString body() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return body_;
  }

  // Returns a reference to the body (caller must hold lock or ensure
  // no concurrent access).
  const GoogleString& body_locked() const { return body_; }

  // Release ownership of the body string (move semantics).
  // Only safe to call after done() returns true.
  GoogleString ReleaseBody() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(body_);
  }

 protected:
  // AsyncFetch interface - these are final to enforce consistent behavior.
  // Subclasses use OnDone() for custom completion handling.
  bool HandleWrite(const StringPiece& content,
                   MessageHandler* handler) override final;
  bool HandleFlush(MessageHandler* handler) override final;
  void HandleDone(bool success) override final;

  // Subclasses must implement HandleHeadersComplete.
  // void HandleHeadersComplete() override = 0;

  // Called when the fetch completes, with mutex_ already held.
  // Subclasses can override to add custom completion handling.
  // Default implementation does nothing.
  virtual void OnDone(bool success) {}

  // Accessor for subclasses that need direct mutex access.
  std::mutex& mutex() const { return mutex_; }

  // Direct access to state for subclasses (must be called with mutex held).
  bool done_locked() const { return done_; }
  bool success_locked() const { return success_; }
  GoogleString& body_mutable() { return body_; }

 private:
  mutable std::mutex mutex_;
  GoogleString body_;
  bool done_{false};
  bool success_{false};

  BufferingAsyncFetch(const BufferingAsyncFetch&) = delete;
  BufferingAsyncFetch& operator=(const BufferingAsyncFetch&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_HTTP_PUBLIC_BUFFERING_ASYNC_FETCH_H_
