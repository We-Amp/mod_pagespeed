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

#ifndef PAGESPEED_SYSTEM_CIRCUIT_BREAKER_H_
#define PAGESPEED_SYSTEM_CIRCUIT_BREAKER_H_

#include <atomic>

#include "pagespeed/kernel/base/basictypes.h"

namespace net_instaweb {

class Timer;

// CircuitBreaker implements the circuit breaker pattern for resource fetching.
//
// The circuit breaker has three states:
//   - CLOSED: Normal operation. Requests are allowed through.
//   - OPEN: Circuit is tripped. Requests are blocked to allow recovery.
//   - HALF_OPEN: Testing recovery. Limited requests are allowed through.
//
// State transitions:
//   CLOSED -> OPEN: After failure_threshold consecutive failures.
//   OPEN -> HALF_OPEN: After timeout_ms has elapsed.
//   HALF_OPEN -> CLOSED: After success_threshold consecutive successes.
//   HALF_OPEN -> OPEN: After 1 failure.
//
// Thread-safety: All public methods are thread-safe using atomic operations.
class CircuitBreaker {
 public:
  // Configuration for the circuit breaker.
  struct Config {
    // Number of consecutive failures before opening the circuit.
    int failure_threshold = 5;
    // Number of consecutive successes in HALF_OPEN before closing the circuit.
    int success_threshold = 2;
    // Time in milliseconds to wait in OPEN state before transitioning to
    // HALF_OPEN.
    int64 timeout_ms = 30000;
  };

  enum class State { CLOSED, OPEN, HALF_OPEN };

  // Creates a circuit breaker with the given configuration.
  // timer is used to track time for the OPEN -> HALF_OPEN transition.
  // timer must outlive the CircuitBreaker.
  explicit CircuitBreaker(const Config& config, Timer* timer);

  // Returns true if a request should be allowed through.
  // In CLOSED state, always returns true.
  // In OPEN state, returns false unless timeout_ms has elapsed, in which case
  //   it transitions to HALF_OPEN and returns true.
  // In HALF_OPEN state, returns true (allowing test requests).
  bool AllowRequest();

  // Records a successful request.
  // In CLOSED state, resets the failure count.
  // In HALF_OPEN state, increments the success count and transitions to CLOSED
  //   if success_threshold is reached.
  // In OPEN state, this is a no-op (shouldn't happen in normal operation).
  void RecordSuccess();

  // Records a failed request.
  // In CLOSED state, increments the failure count and transitions to OPEN
  //   if failure_threshold is reached.
  // In HALF_OPEN state, immediately transitions to OPEN.
  // In OPEN state, this is a no-op (shouldn't happen in normal operation).
  void RecordFailure();

  // Returns the current state of the circuit breaker.
  State GetState() const;

  // Returns the current failure count (for testing/monitoring).
  int GetFailureCount() const;

  // Returns the current success count in HALF_OPEN state (for testing).
  int GetSuccessCount() const;

 private:
  // Atomically transitions to OPEN state and records the time.
  void TransitionToOpen();

  // Atomically transitions to CLOSED state and resets counters.
  void TransitionToClosed();

  // Atomically transitions to HALF_OPEN state and resets success counter.
  void TransitionToHalfOpen();

  std::atomic<State> state_{State::CLOSED};
  std::atomic<int> failure_count_{0};
  std::atomic<int> success_count_{0};
  std::atomic<int64> open_time_ms_{0};
  Config config_;
  Timer* timer_;

  CircuitBreaker(const CircuitBreaker&) = delete;
  CircuitBreaker& operator=(const CircuitBreaker&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_CIRCUIT_BREAKER_H_
