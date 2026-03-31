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

#ifndef PAGESPEED_SYSTEM_CIRCUIT_BREAKER_FETCHER_H_
#define PAGESPEED_SYSTEM_CIRCUIT_BREAKER_FETCHER_H_

#include <memory>

#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/system/circuit_breaker.h"

namespace net_instaweb {

class AsyncFetch;
class MessageHandler;

// CircuitBreakerFetcher wraps another UrlAsyncFetcher and applies circuit
// breaker logic to prevent cascade failures.
//
// When the circuit is OPEN, fetches are immediately failed with a 503 status.
// When CLOSED or HALF_OPEN, fetches are forwarded to the underlying fetcher.
// The circuit breaker is updated based on fetch success/failure.
//
// Thread-safety: This class is thread-safe. The underlying CircuitBreaker
// uses atomic operations for state management.
class CircuitBreakerFetcher : public UrlAsyncFetcher {
 public:
  // Takes ownership of base_fetcher. circuit_breaker must outlive this object.
  CircuitBreakerFetcher(UrlAsyncFetcher* base_fetcher,
                        CircuitBreaker* circuit_breaker,
                        MessageHandler* handler);

  ~CircuitBreakerFetcher() override;

  // UrlAsyncFetcher interface.
  void Fetch(const GoogleString& url, MessageHandler* message_handler,
             AsyncFetch* fetch) override;

  bool SupportsHttps() const override { return base_fetcher_->SupportsHttps(); }

  // Returns the current circuit breaker state for monitoring.
  CircuitBreaker::State GetCircuitState() const {
    return circuit_breaker_->GetState();
  }

 private:
  // Callback that wraps the original fetch callback to record success/failure.
  class CircuitBreakerCallback;

  std::unique_ptr<UrlAsyncFetcher> base_fetcher_;
  CircuitBreaker* circuit_breaker_;  // Not owned
  MessageHandler* handler_;

  CircuitBreakerFetcher(const CircuitBreakerFetcher&) = delete;
  CircuitBreakerFetcher& operator=(const CircuitBreakerFetcher&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_CIRCUIT_BREAKER_FETCHER_H_
