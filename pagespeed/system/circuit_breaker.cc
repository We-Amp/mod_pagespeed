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

#include "pagespeed/system/circuit_breaker.h"

#include "pagespeed/kernel/base/timer.h"

namespace net_instaweb {

CircuitBreaker::CircuitBreaker(const Config& config, Timer* timer)
    : config_(config), timer_(timer) {}

bool CircuitBreaker::AllowRequest() {
  State current_state = state_.load(std::memory_order_acquire);

  switch (current_state) {
    case State::CLOSED:
      return true;

    case State::OPEN: {
      // Check if timeout has elapsed.
      int64 now_ms = timer_->NowMs();
      int64 open_time = open_time_ms_.load(std::memory_order_acquire);
      if (now_ms - open_time >= config_.timeout_ms) {
        // Timeout has elapsed, try to transition to HALF_OPEN.
        // Use compare_exchange to ensure only one thread makes this transition.
        State expected = State::OPEN;
        if (state_.compare_exchange_strong(expected, State::HALF_OPEN,
                                           std::memory_order_acq_rel)) {
          // Successfully transitioned to HALF_OPEN, reset success counter.
          success_count_.store(0, std::memory_order_release);
          return true;
        }
        // Another thread beat us to the transition.
        // Check the new state - if it's HALF_OPEN or CLOSED, allow the request.
        current_state = state_.load(std::memory_order_acquire);
        return current_state != State::OPEN;
      }
      return false;
    }

    case State::HALF_OPEN:
      // Allow test requests in HALF_OPEN state.
      return true;
  }

  // Should never reach here, but return false to be safe.
  return false;
}

void CircuitBreaker::RecordSuccess() {
  State current_state = state_.load(std::memory_order_acquire);

  switch (current_state) {
    case State::CLOSED:
      // Reset failure count on success.
      failure_count_.store(0, std::memory_order_release);
      break;

    case State::HALF_OPEN: {
      // Increment success count and check if we should close the circuit.
      int new_count =
          success_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (new_count >= config_.success_threshold) {
        TransitionToClosed();
      }
      break;
    }

    case State::OPEN:
      // Shouldn't happen in normal operation - requests blocked in OPEN state.
      // No-op.
      break;
  }
}

void CircuitBreaker::RecordFailure() {
  State current_state = state_.load(std::memory_order_acquire);

  switch (current_state) {
    case State::CLOSED: {
      // Increment failure count and check if we should open the circuit.
      int new_count =
          failure_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (new_count >= config_.failure_threshold) {
        TransitionToOpen();
      }
      break;
    }

    case State::HALF_OPEN:
      // Any failure in HALF_OPEN immediately opens the circuit.
      TransitionToOpen();
      break;

    case State::OPEN:
      // Shouldn't happen in normal operation - requests blocked in OPEN state.
      // No-op.
      break;
  }
}

CircuitBreaker::State CircuitBreaker::GetState() const {
  return state_.load(std::memory_order_acquire);
}

int CircuitBreaker::GetFailureCount() const {
  return failure_count_.load(std::memory_order_acquire);
}

int CircuitBreaker::GetSuccessCount() const {
  return success_count_.load(std::memory_order_acquire);
}

void CircuitBreaker::TransitionToOpen() {
  // Record the time when the circuit opened.
  open_time_ms_.store(timer_->NowMs(), std::memory_order_release);
  state_.store(State::OPEN, std::memory_order_release);
  // Reset failure count for next cycle.
  failure_count_.store(0, std::memory_order_release);
}

void CircuitBreaker::TransitionToClosed() {
  failure_count_.store(0, std::memory_order_release);
  success_count_.store(0, std::memory_order_release);
  state_.store(State::CLOSED, std::memory_order_release);
}

void CircuitBreaker::TransitionToHalfOpen() {
  success_count_.store(0, std::memory_order_release);
  state_.store(State::HALF_OPEN, std::memory_order_release);
}

}  // namespace net_instaweb
