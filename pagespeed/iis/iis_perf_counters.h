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

#ifndef PAGESPEED_IIS_IIS_PERF_COUNTERS_H_
#define PAGESPEED_IIS_IIS_PERF_COUNTERS_H_

#include <atomic>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"

#ifdef _WIN32
#include <windows.h>
#include <perflib.h>
#endif

namespace net_instaweb {

// Windows Performance Counter provider for PageSpeed IIS module.
//
// This class exposes PageSpeed performance metrics through Windows
// Performance Monitor (perfmon), allowing system administrators to
// monitor PageSpeed optimization activity in real-time.
//
// Exposed counters:
// - PageSpeed\Requests Processed
// - PageSpeed\HTML Rewrites
// - PageSpeed\Cache Hits
// - PageSpeed\Cache Misses
// - PageSpeed\IPRO Requests
// - PageSpeed\Errors
// - PageSpeed\Average Rewrite Time (ms)
//
// Usage:
//   IisPerfCounters::Initialize();  // Call once during module init
//   IisPerfCounters::IncrementRequestsProcessed();
//   IisPerfCounters::IncrementHtmlRewrites();
//   // etc.
//   IisPerfCounters::Shutdown();    // Call during module termination
class IisPerfCounters {
 public:
  // Initialize the performance counter provider.
  // Must be called once during module initialization.
  static bool Initialize();

  // Shutdown the performance counter provider.
  // Must be called during module termination.
  static void Shutdown();

  // Check if counters are available
  static bool IsInitialized() { return initialized_; }

  // Check if Windows Performance Counter V2 API is active.
  // Returns true if counters are published to Windows perfmon.
  // Returns false on non-Windows platforms or if registration failed.
  static bool IsWindowsPerfCounterActive();

  // Get provider GUID as string (for diagnostics)
  static GoogleString GetProviderGuidString();

  // Counter operations
  static void IncrementRequestsProcessed();
  static void IncrementHtmlRewrites();
  static void IncrementCacheHits();
  static void IncrementCacheMisses();
  static void IncrementIproRequests();
  static void IncrementIproRecordings();
  static void IncrementErrors();
  static void RecordRewriteTime(int64 time_ms);

  // Read current counter values (for testing and admin UI)
  static int64 GetRequestsProcessed() {
    return requests_processed_.load(std::memory_order_relaxed);
  }
  static int64 GetHtmlRewrites() {
    return html_rewrites_.load(std::memory_order_relaxed);
  }
  static int64 GetCacheHits() {
    return cache_hits_.load(std::memory_order_relaxed);
  }
  static int64 GetCacheMisses() {
    return cache_misses_.load(std::memory_order_relaxed);
  }
  static int64 GetIproRequests() {
    return ipro_requests_.load(std::memory_order_relaxed);
  }
  static int64 GetIproRecordings() {
    return ipro_recordings_.load(std::memory_order_relaxed);
  }
  static int64 GetErrors() {
    return errors_.load(std::memory_order_relaxed);
  }
  static int64 GetTotalRewriteTimeMs() {
    return total_rewrite_time_ms_.load(std::memory_order_relaxed);
  }

  // Reset all counters (for testing)
  static void ResetCounters();

  // Get a human-readable summary of all counters
  static GoogleString GetCountersSummary();

 private:
  IisPerfCounters() = delete;
  ~IisPerfCounters() = delete;

  static bool initialized_;

  // Atomic counters for thread-safe updates
  static std::atomic<int64> requests_processed_;
  static std::atomic<int64> html_rewrites_;
  static std::atomic<int64> cache_hits_;
  static std::atomic<int64> cache_misses_;
  static std::atomic<int64> ipro_requests_;
  static std::atomic<int64> ipro_recordings_;
  static std::atomic<int64> errors_;
  static std::atomic<int64> total_rewrite_time_ms_;
  static std::atomic<int64> rewrite_count_;  // For averaging

#ifdef _WIN32
  // Windows performance counter handles
  static HANDLE perf_provider_handle_;
  static PPERF_COUNTERSET_INSTANCE perf_instance_;

  // Counter set registration
  static bool RegisterCounterSet();
  static void UnregisterCounterSet();

  // Allow helper function access to private members
  friend void UpdateWindowsCounter(ULONG counter_id, ULONGLONG value);
#endif

  IisPerfCounters(const IisPerfCounters&) = delete;
  IisPerfCounters& operator=(const IisPerfCounters&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_PERF_COUNTERS_H_
