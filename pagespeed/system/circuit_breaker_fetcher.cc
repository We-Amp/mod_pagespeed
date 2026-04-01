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

#include "pagespeed/system/circuit_breaker_fetcher.h"

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

// Wrapper callback that records fetch success/failure to the circuit breaker.
class CircuitBreakerFetcher::CircuitBreakerCallback : public SharedAsyncFetch {
 public:
  CircuitBreakerCallback(AsyncFetch* base_fetch,
                         CircuitBreaker* circuit_breaker,
                         MessageHandler* handler)
      : SharedAsyncFetch(base_fetch),
        circuit_breaker_(circuit_breaker),
        handler_(handler) {}

  void HandleDone(bool success) override {
    // Record the result to the circuit breaker.
    if (success) {
      // Check if HTTP status indicates success (2xx or 3xx).
      int status_code = response_headers()->status_code();
      if (status_code >= 200 && status_code < 400) {
        circuit_breaker_->RecordSuccess();
      } else if (status_code >= 500) {
        // 5xx errors indicate server-side failures.
        circuit_breaker_->RecordFailure();
        handler_->Message(kWarning,
                          "Circuit breaker: recorded failure for status %d",
                          status_code);
      }
      // 4xx errors are client errors - don't affect circuit breaker.
    } else {
      // Network/connection failure.
      circuit_breaker_->RecordFailure();
      handler_->Message(kWarning,
                        "Circuit breaker: recorded failure (fetch failed)");
    }

    SharedAsyncFetch::HandleDone(success);
    delete this;
  }

 private:
  CircuitBreaker* circuit_breaker_;
  MessageHandler* handler_;
};

CircuitBreakerFetcher::CircuitBreakerFetcher(UrlAsyncFetcher* base_fetcher,
                                             CircuitBreaker* circuit_breaker,
                                             MessageHandler* handler)
    : base_fetcher_(base_fetcher),
      circuit_breaker_(circuit_breaker),
      handler_(handler) {}

CircuitBreakerFetcher::~CircuitBreakerFetcher() {}

void CircuitBreakerFetcher::Fetch(const GoogleString& url,
                                  MessageHandler* message_handler,
                                  AsyncFetch* fetch) {
  // Check if the circuit allows the request.
  if (!circuit_breaker_->AllowRequest()) {
    // Circuit is OPEN - fail immediately with 503.
    handler_->Message(kWarning, "Circuit breaker OPEN: blocking fetch to %s",
                      url.c_str());

    fetch->response_headers()->SetStatusAndReason(HttpStatus::kUnavailable);
    // Add a header to indicate the circuit breaker is open.
    fetch->response_headers()->Add("X-PageSpeed-Circuit-Breaker", "open");
    fetch->HeadersComplete();
    fetch->Done(false);
    return;
  }

  // Circuit is CLOSED or HALF_OPEN - forward the request.
  // Wrap the callback to record the result.
  CircuitBreakerCallback* callback =
      new CircuitBreakerCallback(fetch, circuit_breaker_, handler_);

  base_fetcher_->Fetch(url, message_handler, callback);
}

}  // namespace net_instaweb
