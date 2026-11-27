// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_TEST_UTIL_WORKER_QUIESCENCE_H_
#define TEST_TEST_UTIL_WORKER_QUIESCENCE_H_

// Waits for a running Worker to finish the notifications a test sent it,
// decided by what the worker reports rather than by how long it took.
//
// Why this exists: a test that sends a notification and then polls a
// processing counter for a fixed number of seconds is really asserting "the
// optimization finished within N seconds on this host". Image optimization in
// an unoptimized build takes seconds per variant combination on an idle
// machine and many times that on a host whose cores are shared with other
// jobs or under a sanitizer, so every such window is eventually too short,
// and a window that is long enough for the slowest host makes a genuine
// regression cost that long to report. The question the tests actually ask is
// "once the worker has finished with this notification, what did it do?" --
// and the worker can answer when it has finished.

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "gtest/gtest.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"

namespace pagespeed::test {

// True in AddressSanitizer and ThreadSanitizer builds, which run the image
// encoders roughly an order of magnitude slower; tests scale their silence
// budgets by it.  Both the GCC-style define and the Clang __has_feature form
// are checked.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
inline constexpr bool kSanitizerBuild = true;
#elif defined(__clang__)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
inline constexpr bool kSanitizerBuild = true;
#else
inline constexpr bool kSanitizerBuild = false;
#endif
#else
inline constexpr bool kSanitizerBuild = false;
#endif

// True in ThreadSanitizer builds only.  TSan additionally serialises and
// shadows every thread, which pushes codec-bound tests (whole-output
// byte-size rules of the image encoders) past their shard budget without
// adding any race coverage; tests use it to skip such cases in the TSan
// lane while kSanitizerBuild keeps them in the plain and AddressSanitizer
// lanes.  Same detection forms as kSanitizerBuild above.
#if defined(__SANITIZE_THREAD__)
inline constexpr bool kThreadSanitizerBuild = true;
#elif defined(__clang__)
#if __has_feature(thread_sanitizer)
inline constexpr bool kThreadSanitizerBuild = true;
#else
inline constexpr bool kThreadSanitizerBuild = false;
#endif
#else
inline constexpr bool kThreadSanitizerBuild = false;
#endif

// The worker's dispatch state as its own event loop reports it.
struct WorkerLoopState {
  uint64_t notifications_received = 0;
  int in_flight_work = 0;
};

// Asks the worker's health endpoint for its dispatch state.
//
// The reply is composed in one event-loop callback, and the two numbers it
// carries are only ever modified in event-loop callbacks (a notification is
// counted and its work item registered in the same callback; a finished work
// item is dropped from the in-flight set and the count decremented in the
// same callback). Callbacks do not interleave, so the pair is a consistent
// snapshot. Reading the two atomics from the test thread is not: it can land
// between "counted" and "registered" and see a notification that looks
// finished before it has started.
//
// Returns nullopt when the endpoint cannot be reached or the reply does not
// carry both fields; callers retry.
inline std::optional<WorkerLoopState> ReadWorkerLoopState(
    const Worker& worker) {
  intptr_t fd = ConnectPipe(worker.health_socket_path());
  if (fd < 0) return std::nullopt;
  std::string reply;
  char buf[512];
  while (reply.find('\n') == std::string::npos) {
    ssize_t n = PipeRead(fd, buf, sizeof(buf));
    if (n <= 0) break;
    reply.append(buf, static_cast<size_t>(n));
  }
  ClosePipe(fd);

  // A reply cut short of its newline could end inside a number.
  if (reply.empty() || reply.back() != '\n') return std::nullopt;

  auto field = [&reply](std::string_view name) -> std::optional<uint64_t> {
    size_t pos = reply.find(name);
    if (pos == std::string::npos) return std::nullopt;
    const char* begin = reply.data() + pos + name.size();
    const char* end = reply.data() + reply.size();
    uint64_t value = 0;
    if (std::from_chars(begin, end, value).ec != std::errc()) {
      return std::nullopt;
    }
    return value;
  };
  auto notifications = field(" notifs=");
  auto in_flight = field(" inflight=");
  if (!notifications || !in_flight) return std::nullopt;
  return WorkerLoopState{*notifications, static_cast<int>(*in_flight)};
}

// Blocks until the worker has received at least `notifications`
// notifications (a count over the worker's lifetime) and has no work item in
// flight -- that is, until everything the test sent so far has been
// processed, skipped or refused, and its in-flight entry is gone, so a repeat
// notification for the same URL is not turned away as "already running".
// After it returns, the worker's counters are final for those notifications
// and can be asserted on directly.
//
// There is no deadline on the work itself. The only time limit is on
// silence: the wait fails when none of the worker's progress counters has
// moved for `stall_budget`. Nearly every processed variant combination moves
// at least one of them (a combination whose transcode fails outright moves
// none, which only lengthens the silent stretch by one step), so the budget
// has to cover the slowest single step (one combination's encodes), not the
// whole job, and a slow host that keeps making progress is never cut off.
inline ::testing::AssertionResult WaitForNotificationsRetired(
    const Worker& worker, uint64_t notifications,
    std::chrono::milliseconds stall_budget) {
  using Clock = std::chrono::steady_clock;
  const WorkerStats& stats = worker.stats();
  auto relaxed = [](const std::atomic<uint64_t>& counter) {
    return counter.load(std::memory_order_relaxed);
  };
  // Counters are monotonic, so their sum changes whenever any one does.
  auto progress = [&] {
    return relaxed(stats.notifications_received) +
           relaxed(stats.alternate_writes) + relaxed(stats.variants_written) +
           relaxed(stats.dedup_writes_skipped) +
           relaxed(stats.ssimulacra2_checks) +
           relaxed(stats.ssimulacra2_declines) +
           relaxed(stats.ssimulacra2_decline_tombstone_hits) +
           relaxed(stats.ssimulacra2_reencodes) +
           relaxed(stats.image_no_savings_skipped) +
           relaxed(stats.cache_read_retries) +
           relaxed(stats.cache_read_failures) + relaxed(stats.errors);
  };

  uint64_t last_progress = progress();
  int last_in_flight = worker.in_flight_work();
  Clock::time_point last_change = Clock::now();
  for (;;) {
    // The atomics are a cheap filter; the event-loop snapshot decides.
    if (relaxed(stats.notifications_received) >= notifications &&
        worker.in_flight_work() == 0) {
      std::optional<WorkerLoopState> state = ReadWorkerLoopState(worker);
      if (state && state->notifications_received >= notifications &&
          state->in_flight_work == 0) {
        return ::testing::AssertionSuccess();
      }
    }

    const uint64_t now_progress = progress();
    const int now_in_flight = worker.in_flight_work();
    const Clock::time_point now = Clock::now();
    if (now_progress != last_progress || now_in_flight != last_in_flight) {
      last_progress = now_progress;
      last_in_flight = now_in_flight;
      last_change = now;
    } else if (now - last_change > stall_budget) {
      return ::testing::AssertionFailure()
             << "worker made no progress for "
             << std::chrono::duration_cast<std::chrono::seconds>(stall_budget)
                    .count()
             << " s while waiting for " << notifications
             << " notification(s) to retire (received "
             << relaxed(stats.notifications_received) << ", in flight "
             << now_in_flight << ")";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace pagespeed::test

#endif  // TEST_TEST_UTIL_WORKER_QUIESCENCE_H_
