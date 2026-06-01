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

#include "pagespeed/iis/iis_perf_counters.h"

#include <string>

#include "base/logging.h"
#include "pagespeed/kernel/base/string_util.h"

#ifdef _WIN32
#include <perflib.h>

// PERF_DETAIL_* constants from winperf.h (not in perflib.h)
#ifndef PERF_DETAIL_NOVICE
#define PERF_DETAIL_NOVICE      100
#define PERF_DETAIL_ADVANCED    200
#define PERF_DETAIL_EXPERT      300
#define PERF_DETAIL_WIZARD      400
#endif
#endif

namespace net_instaweb {

// Static member initialization
bool IisPerfCounters::initialized_ = false;
std::atomic<int64> IisPerfCounters::requests_processed_(0);
std::atomic<int64> IisPerfCounters::html_rewrites_(0);
std::atomic<int64> IisPerfCounters::cache_hits_(0);
std::atomic<int64> IisPerfCounters::cache_misses_(0);
std::atomic<int64> IisPerfCounters::ipro_requests_(0);
std::atomic<int64> IisPerfCounters::ipro_recordings_(0);
std::atomic<int64> IisPerfCounters::errors_(0);
std::atomic<int64> IisPerfCounters::total_rewrite_time_ms_(0);
std::atomic<int64> IisPerfCounters::rewrite_count_(0);

#ifdef _WIN32
HANDLE IisPerfCounters::perf_provider_handle_ = nullptr;
PPERF_COUNTERSET_INSTANCE IisPerfCounters::perf_instance_ = nullptr;

// PageSpeed Performance Counter Provider GUID
// {E3F17D2E-A7B3-4C5F-9D21-3B8E6A1F4C9D}
static const GUID kPageSpeedProviderGuid = {
    0xe3f17d2e, 0xa7b3, 0x4c5f,
    {0x9d, 0x21, 0x3b, 0x8e, 0x6a, 0x1f, 0x4c, 0x9d}};

// PageSpeed Counter Set GUID
// {F4A28D3E-B8C4-5D6A-AE32-4C9F7B2E5D0E}
static const GUID kPageSpeedCounterSetGuid = {
    0xf4a28d3e, 0xb8c4, 0x5d6a,
    {0xae, 0x32, 0x4c, 0x9f, 0x7b, 0x2e, 0x5d, 0x0e}};

// Counter IDs - must match the order in the counter set template
enum PageSpeedCounterId : ULONG {
  kCounterRequestsProcessed = 0,
  kCounterHtmlRewrites = 1,
  kCounterCacheHits = 2,
  kCounterCacheMisses = 3,
  kCounterIproRequests = 4,
  kCounterErrors = 5,
  kCounterTotalRewriteTimeMs = 6,
  kCounterRewriteCount = 7,
  kCounterCount = 8  // Total number of counters
};

// Counter set template structure for programmatic registration.
// This is a variable-length structure: PERF_COUNTERSET_INFO followed by
// an array of PERF_COUNTER_INFO for each counter.
#pragma pack(push, 8)
struct PageSpeedCounterSetTemplate {
  PERF_COUNTERSET_INFO info;
  PERF_COUNTER_INFO counters[kCounterCount];
};
#pragma pack(pop)

// Helper function to update Windows performance counter value
static void UpdateWindowsCounter(ULONG counter_id, ULONGLONG value) {
  if (IisPerfCounters::perf_provider_handle_ != nullptr &&
      IisPerfCounters::perf_instance_ != nullptr) {
    PerfSetULongLongCounterValue(IisPerfCounters::perf_provider_handle_,
                                  IisPerfCounters::perf_instance_, counter_id,
                                  value);
  }
}
#endif

bool IisPerfCounters::Initialize() {
  if (initialized_) {
    return true;
  }

#ifdef _WIN32
  // Register with Windows Performance Counter infrastructure
  // Note: Full performance counter integration requires:
  // 1. A counter manifest (.man) file defining the counter set
  // 2. Registration via lodctr.exe or programmatically
  // 3. Installing the provider DLL
  //
  // For now, we just initialize our atomic counters and optionally
  // register with Windows if the manifest is available.
  if (RegisterCounterSet()) {
    LOG(INFO) << "PageSpeed: Windows performance counters registered";
  } else {
    LOG(INFO) << "PageSpeed: Using internal performance counters only";
  }
#endif

  initialized_ = true;
  LOG(INFO) << "PageSpeed: Performance counters initialized";
  return true;
}

void IisPerfCounters::Shutdown() {
  if (!initialized_) {
    return;
  }

#ifdef _WIN32
  UnregisterCounterSet();
#endif

  initialized_ = false;
  LOG(INFO) << "PageSpeed: Performance counters shut down";
}

void IisPerfCounters::IncrementRequestsProcessed() {
  int64 new_value =
      requests_processed_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
  UpdateWindowsCounter(kCounterRequestsProcessed,
                       static_cast<ULONGLONG>(new_value));
#endif
}

void IisPerfCounters::IncrementHtmlRewrites() {
  int64 new_value = html_rewrites_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
  UpdateWindowsCounter(kCounterHtmlRewrites, static_cast<ULONGLONG>(new_value));
#endif
}

void IisPerfCounters::IncrementCacheHits() {
  int64 new_value = cache_hits_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
  UpdateWindowsCounter(kCounterCacheHits, static_cast<ULONGLONG>(new_value));
#endif
}

void IisPerfCounters::IncrementCacheMisses() {
  int64 new_value = cache_misses_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
  UpdateWindowsCounter(kCounterCacheMisses, static_cast<ULONGLONG>(new_value));
#endif
}

void IisPerfCounters::IncrementIproRequests() {
  int64 new_value = ipro_requests_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
  UpdateWindowsCounter(kCounterIproRequests, static_cast<ULONGLONG>(new_value));
#endif
}

void IisPerfCounters::IncrementIproRecordings() {
  ipro_recordings_.fetch_add(1, std::memory_order_relaxed);
  // Note: No Windows counter for this yet - could add if needed
}

void IisPerfCounters::IncrementErrors() {
  int64 new_value = errors_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
  UpdateWindowsCounter(kCounterErrors, static_cast<ULONGLONG>(new_value));
#endif
}

void IisPerfCounters::RecordRewriteTime(int64 time_ms) {
  int64 new_total =
      total_rewrite_time_ms_.fetch_add(time_ms, std::memory_order_relaxed) +
      time_ms;
  int64 new_count = rewrite_count_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
  UpdateWindowsCounter(kCounterTotalRewriteTimeMs,
                       static_cast<ULONGLONG>(new_total));
  UpdateWindowsCounter(kCounterRewriteCount, static_cast<ULONGLONG>(new_count));
#endif
}

void IisPerfCounters::ResetCounters() {
  requests_processed_.store(0, std::memory_order_relaxed);
  html_rewrites_.store(0, std::memory_order_relaxed);
  cache_hits_.store(0, std::memory_order_relaxed);
  cache_misses_.store(0, std::memory_order_relaxed);
  ipro_requests_.store(0, std::memory_order_relaxed);
  ipro_recordings_.store(0, std::memory_order_relaxed);
  errors_.store(0, std::memory_order_relaxed);
  total_rewrite_time_ms_.store(0, std::memory_order_relaxed);
  rewrite_count_.store(0, std::memory_order_relaxed);

#ifdef _WIN32
  // Also reset Windows performance counters
  if (perf_provider_handle_ != nullptr && perf_instance_ != nullptr) {
    for (ULONG i = 0; i < kCounterCount; ++i) {
      PerfSetULongLongCounterValue(perf_provider_handle_, perf_instance_, i, 0);
    }
  }
#endif
}

bool IisPerfCounters::IsWindowsPerfCounterActive() {
#ifdef _WIN32
  return perf_provider_handle_ != nullptr && perf_instance_ != nullptr;
#else
  return false;
#endif
}

GoogleString IisPerfCounters::GetProviderGuidString() {
#ifdef _WIN32
  // Format: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
  char buf[64];
  snprintf(buf, sizeof(buf),
           "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
           kPageSpeedProviderGuid.Data1,
           kPageSpeedProviderGuid.Data2,
           kPageSpeedProviderGuid.Data3,
           kPageSpeedProviderGuid.Data4[0],
           kPageSpeedProviderGuid.Data4[1],
           kPageSpeedProviderGuid.Data4[2],
           kPageSpeedProviderGuid.Data4[3],
           kPageSpeedProviderGuid.Data4[4],
           kPageSpeedProviderGuid.Data4[5],
           kPageSpeedProviderGuid.Data4[6],
           kPageSpeedProviderGuid.Data4[7]);
  return GoogleString(buf);
#else
  return "(not available on this platform)";
#endif
}

GoogleString IisPerfCounters::GetCountersSummary() {
  int64 requests = requests_processed_.load(std::memory_order_relaxed);
  int64 rewrites = html_rewrites_.load(std::memory_order_relaxed);
  int64 hits = cache_hits_.load(std::memory_order_relaxed);
  int64 misses = cache_misses_.load(std::memory_order_relaxed);
  int64 ipro = ipro_requests_.load(std::memory_order_relaxed);
  int64 ipro_rec = ipro_recordings_.load(std::memory_order_relaxed);
  int64 errs = errors_.load(std::memory_order_relaxed);
  int64 total_time = total_rewrite_time_ms_.load(std::memory_order_relaxed);
  int64 count = rewrite_count_.load(std::memory_order_relaxed);

  double avg_time = count > 0 ? static_cast<double>(total_time) / count : 0.0;
  double hit_rate = (hits + misses) > 0
      ? 100.0 * hits / (hits + misses)
      : 0.0;

  GoogleString summary = StrCat(
      "PageSpeed Performance Counters:\n",
      "  Requests Processed: ", Integer64ToString(requests), "\n",
      "  HTML Rewrites: ", Integer64ToString(rewrites), "\n",
      "  Cache Hits: ", Integer64ToString(hits), "\n",
      "  Cache Misses: ", Integer64ToString(misses), "\n",
      "  Cache Hit Rate: ", std::to_string(hit_rate), "%\n",
      "  IPRO Requests: ", Integer64ToString(ipro), "\n",
      "  IPRO Recordings: ", Integer64ToString(ipro_rec), "\n",
      "  Errors: ", Integer64ToString(errs), "\n",
      "  Average Rewrite Time: ", std::to_string(avg_time), " ms\n");

  // Add Windows Performance Counter status
  StrAppend(&summary,
            "  Windows PerfMon: ",
            IsWindowsPerfCounterActive() ? "Active" : "Inactive",
            "\n");
  if (IsWindowsPerfCounterActive()) {
    StrAppend(&summary, "  Provider GUID: ", GetProviderGuidString(), "\n");
  }

  return summary;
}

#ifdef _WIN32

bool IisPerfCounters::RegisterCounterSet() {
  // Windows Performance Counter V2 API integration.
  //
  // This implementation uses programmatic registration which allows counter
  // values to be tracked internally and accessed via GetCountersSummary()
  // and the admin handler even without a pre-installed counter manifest.
  //
  // PERFMON.EXE VISIBILITY:
  // For counters to appear in Windows Performance Monitor (perfmon.exe),
  // the counter manifest must be registered with the system:
  //
  //   1. Copy pagespeed_iis.dll to the install location
  //      (e.g., %SystemRoot%\System32\inetsrv\)
  //   2. Edit pagespeed_counters.man to set the correct applicationIdentity path
  //   3. Run as Administrator:
  //        lodctr /m:pagespeed_counters.man
  //   4. Restart PLA service or reboot:
  //        net stop pla && net start pla
  //
  // To unregister:
  //   unlodctr /m:pagespeed_counters.man
  //
  // The manifest file is located at: pagespeed/iis/pagespeed_counters.man
  //
  // PROGRAMMATIC ACCESS:
  // Even without manifest registration, counter values are available via:
  //   - IisPerfCounters::GetCountersSummary() for text summary
  //   - Admin handler at /pagespeed_admin?counters
  //   - The Windows V2 API calls below publish counters that can be
  //     read by PDH APIs using the provider GUID

  ULONG status;

  // Step 1: Register the provider with PerfStartProviderEx
  // Using PerfStartProvider (simpler form without callbacks)
  // Need a non-const copy of the GUID for the API
  GUID providerGuid = kPageSpeedProviderGuid;
  status = PerfStartProvider(&providerGuid, nullptr,
                             &perf_provider_handle_);

  if (status != ERROR_SUCCESS) {
    LOG(WARNING) << "PageSpeed: PerfStartProvider failed with error " << status;
    perf_provider_handle_ = nullptr;
    return false;
  }

  // Step 2: Build the counter set template
  // The template describes the counter set and its counters
  PageSpeedCounterSetTemplate counterSetTemplate;
  memset(&counterSetTemplate, 0, sizeof(counterSetTemplate));

  // Counter set info
  counterSetTemplate.info.CounterSetGuid = kPageSpeedCounterSetGuid;
  counterSetTemplate.info.ProviderGuid = kPageSpeedProviderGuid;
  counterSetTemplate.info.NumCounters = kCounterCount;
  counterSetTemplate.info.InstanceType = PERF_COUNTERSET_SINGLE_INSTANCE;

  // Define each counter
  // Counter type PERF_COUNTER_LARGE_RAWCOUNT (0x00010100) = 64-bit raw value
  // From winperf.h: PERF_SIZE_LARGE | PERF_TYPE_NUMBER | PERF_NUMBER_DECIMAL
  const ULONG kPerfCounterLargeRawcount = 0x00010100;

  // Counter 0: Requests Processed
  counterSetTemplate.counters[0].CounterId = kCounterRequestsProcessed;
  counterSetTemplate.counters[0].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[0].Attrib = 0;
  counterSetTemplate.counters[0].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[0].DetailLevel = PERF_DETAIL_NOVICE;
  counterSetTemplate.counters[0].Scale = 0;
  counterSetTemplate.counters[0].Offset = 0;

  // Counter 1: HTML Rewrites
  counterSetTemplate.counters[1].CounterId = kCounterHtmlRewrites;
  counterSetTemplate.counters[1].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[1].Attrib = 0;
  counterSetTemplate.counters[1].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[1].DetailLevel = PERF_DETAIL_NOVICE;
  counterSetTemplate.counters[1].Scale = 0;
  counterSetTemplate.counters[1].Offset = sizeof(ULONGLONG);

  // Counter 2: Cache Hits
  counterSetTemplate.counters[2].CounterId = kCounterCacheHits;
  counterSetTemplate.counters[2].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[2].Attrib = 0;
  counterSetTemplate.counters[2].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[2].DetailLevel = PERF_DETAIL_NOVICE;
  counterSetTemplate.counters[2].Scale = 0;
  counterSetTemplate.counters[2].Offset = 2 * sizeof(ULONGLONG);

  // Counter 3: Cache Misses
  counterSetTemplate.counters[3].CounterId = kCounterCacheMisses;
  counterSetTemplate.counters[3].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[3].Attrib = 0;
  counterSetTemplate.counters[3].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[3].DetailLevel = PERF_DETAIL_NOVICE;
  counterSetTemplate.counters[3].Scale = 0;
  counterSetTemplate.counters[3].Offset = 3 * sizeof(ULONGLONG);

  // Counter 4: IPRO Requests
  counterSetTemplate.counters[4].CounterId = kCounterIproRequests;
  counterSetTemplate.counters[4].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[4].Attrib = 0;
  counterSetTemplate.counters[4].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[4].DetailLevel = PERF_DETAIL_NOVICE;
  counterSetTemplate.counters[4].Scale = 0;
  counterSetTemplate.counters[4].Offset = 4 * sizeof(ULONGLONG);

  // Counter 5: Errors
  counterSetTemplate.counters[5].CounterId = kCounterErrors;
  counterSetTemplate.counters[5].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[5].Attrib = 0;
  counterSetTemplate.counters[5].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[5].DetailLevel = PERF_DETAIL_NOVICE;
  counterSetTemplate.counters[5].Scale = 0;
  counterSetTemplate.counters[5].Offset = 5 * sizeof(ULONGLONG);

  // Counter 6: Total Rewrite Time (ms)
  counterSetTemplate.counters[6].CounterId = kCounterTotalRewriteTimeMs;
  counterSetTemplate.counters[6].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[6].Attrib = 0;
  counterSetTemplate.counters[6].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[6].DetailLevel = PERF_DETAIL_ADVANCED;
  counterSetTemplate.counters[6].Scale = 0;
  counterSetTemplate.counters[6].Offset = 6 * sizeof(ULONGLONG);

  // Counter 7: Rewrite Count
  counterSetTemplate.counters[7].CounterId = kCounterRewriteCount;
  counterSetTemplate.counters[7].Type = kPerfCounterLargeRawcount;
  counterSetTemplate.counters[7].Attrib = 0;
  counterSetTemplate.counters[7].Size = sizeof(ULONGLONG);
  counterSetTemplate.counters[7].DetailLevel = PERF_DETAIL_ADVANCED;
  counterSetTemplate.counters[7].Scale = 0;
  counterSetTemplate.counters[7].Offset = 7 * sizeof(ULONGLONG);

  // Step 3: Register the counter set with the provider
  status = PerfSetCounterSetInfo(
      perf_provider_handle_,
      reinterpret_cast<PPERF_COUNTERSET_INFO>(&counterSetTemplate),
      sizeof(counterSetTemplate));

  if (status != ERROR_SUCCESS) {
    LOG(WARNING) << "PageSpeed: PerfSetCounterSetInfo failed with error "
                 << status;
    PerfStopProvider(perf_provider_handle_);
    perf_provider_handle_ = nullptr;
    return false;
  }

  // Step 4: Create the counter set instance
  // For a single instance counter set, we create one instance named "PageSpeed"
  perf_instance_ = PerfCreateInstance(perf_provider_handle_,
                                      &kPageSpeedCounterSetGuid, L"PageSpeed",
                                      0);  // Instance ID 0

  if (perf_instance_ == nullptr) {
    DWORD last_error = GetLastError();
    LOG(WARNING) << "PageSpeed: PerfCreateInstance failed with error "
                 << last_error;
    PerfStopProvider(perf_provider_handle_);
    perf_provider_handle_ = nullptr;
    return false;
  }

  // Step 5: Initialize all counter values to 0
  for (ULONG i = 0; i < kCounterCount; ++i) {
    PerfSetULongLongCounterValue(perf_provider_handle_, perf_instance_, i, 0);
  }

  LOG(INFO) << "PageSpeed: Windows V2 performance counters registered "
            << "successfully";
  return true;
}

void IisPerfCounters::UnregisterCounterSet() {
  // Delete the instance first
  if (perf_instance_ != nullptr) {
    PerfDeleteInstance(perf_provider_handle_, perf_instance_);
    perf_instance_ = nullptr;
    LOG(INFO) << "PageSpeed: Performance counter instance deleted";
  }

  // Then stop the provider
  if (perf_provider_handle_ != nullptr) {
    ULONG status = PerfStopProvider(perf_provider_handle_);
    if (status != ERROR_SUCCESS) {
      LOG(WARNING) << "PageSpeed: PerfStopProvider failed with error "
                   << status;
    } else {
      LOG(INFO) << "PageSpeed: Performance counter provider stopped";
    }
    perf_provider_handle_ = nullptr;
  }
}

#endif  // _WIN32

}  // namespace net_instaweb
