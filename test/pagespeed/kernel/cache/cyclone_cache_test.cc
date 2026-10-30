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

// Unit tests for CycloneCache.

#include "pagespeed/kernel/cache/cyclone_cache.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/null_statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/cache/cache_test_base.h"

// Allocation-failure injection for the exception-barrier tests below.  The
// replacement global operator new is inert unless a test arms it for the
// calling thread: then the next allocation on that thread throws
// std::bad_alloc, once.  Sanitizers interpose operator new themselves, so
// the replacement (and the tests that need it) is left out of those builds.
// CYCLONE_CACHE_TEST_SANITIZERS marks the build for that exclusion and for
// the sanitizer-scaled watchdogs in the fork tests below.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define CYCLONE_CACHE_TEST_SANITIZERS 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CYCLONE_CACHE_TEST_SANITIZERS 1
#endif
#ifdef CYCLONE_CACHE_TEST_SANITIZERS
#define CYCLONE_CACHE_TEST_NO_ALLOC_HOOK 1
#endif

namespace {
thread_local bool t_fail_next_allocation = false;
// When >= 0: let this many allocations on the thread succeed, then fail the
// next one, once (the hook then disarms itself).
thread_local int t_allocations_before_failure = -1;
thread_local int t_failed_allocations = 0;
}  // namespace

#ifndef CYCLONE_CACHE_TEST_NO_ALLOC_HOOK
void* operator new(size_t size) {
  if (t_fail_next_allocation) {
    t_fail_next_allocation = false;
    ++t_failed_allocations;
    throw std::bad_alloc();
  }
  // Countdown idiom: the post-decrement both tests and consumes the budget.
  // NOLINTNEXTLINE(bugprone-inc-dec-in-conditions)
  if (t_allocations_before_failure >= 0 && t_allocations_before_failure-- == 0) {
    ++t_failed_allocations;
    throw std::bad_alloc();
  }
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
#endif  // CYCLONE_CACHE_TEST_NO_ALLOC_HOOK

// Test-only seams exported by the Cyclone wrapper (cyclone_wrapper.cc),
// not part of the vendored C ABI header: the next read, or the next commit
// of a write, fails with the selected library error without touching the
// volume.
extern "C" void cyclone_cache_test_fail_next_read(int kind);
extern "C" void cyclone_cache_test_fail_next_commit(int kind);
constexpr int kSeamBusy = 1;
constexpr int kSeamIoError = 2;
constexpr int kSeamCorrupted = 3;

namespace net_instaweb {

class CycloneCacheTest : public CacheTestBase {
 protected:
  CycloneCacheTest()
      : stats_(new NullStatistics()),
        handler_(new GoogleMessageHandler()) {
    CycloneCache::InitStats(stats_.get());
  }

  // Root directory for this test's volume files.  Bazel's TEST_TMPDIR is
  // unique per test and per shard, so concurrent runs never collide on a
  // volume path, and it is the directory a job-level tmpfs would land on:
  // Cyclone's volume create and teardown fsync/msync once per stripe, and on
  // a busy disk every such flush waits behind unrelated writers (#321).
  // Fall back to the shared gtest temp dir when the binary runs outside
  // Bazel.  Both roots exist on entry; Cyclone creates the leaf directory.
  static GoogleString VolumeRoot() {
    const char* test_tmpdir = getenv("TEST_TMPDIR");
    if (test_tmpdir != nullptr && *test_tmpdir != '\0') {
      GoogleString dir = test_tmpdir;
      // Bazel on Windows hands out backslash paths; normalize like
      // GTestTempDir() does so the volume code sees forward slashes.
      std::replace(dir.begin(), dir.end(), '\\', '/');
      return dir;
    }
    return GTestTempDir();
  }

  void SetUp() override {
    // Create a unique cache path for each test to avoid conflicts
    GoogleString test_name = ::testing::UnitTest::GetInstance()
                                 ->current_test_info()
                                 ->name();
    cache_path_ = StrCat(VolumeRoot(), "/cyclone_cache_test_", test_name);

    CycloneCache::Config config;
    config.cache_path = cache_path_;
    config.cache_size_bytes = 10 * 1024 * 1024;  // 10 MB
    // TODO(cyclone): RAM cache disabled due to bug where deleted/overwritten
    // entries are not properly invalidated. Enable once Cyclone fixes this.
    config.ram_cache_size_bytes = 0;
    config.enable_checksum = true;
    config.num_segments = 0;

    cache_ = std::make_unique<CycloneCache>(config, stats_.get(),
                                            handler_.get());
  }

  void TearDown() override {
    if (cache_ != nullptr) {
      cache_->ShutDown();
      cache_.reset();
    }
  }

  CacheInterface* Cache() override { return cache_.get(); }

  // Creates a second cache (own path) with a small-object tier carve-out.
  std::unique_ptr<CycloneCache> MakeTieredCache(int64 size_bytes,
                                                int small_tier_percent,
                                                const char* suffix) {
    CycloneCache::Config config;
    config.cache_path = StrCat(cache_path_, "_", suffix);
    config.cache_size_bytes = size_bytes;
    config.ram_cache_size_bytes = 0;
    config.enable_checksum = true;
    config.num_segments = 0;
    config.small_tier_percent = small_tier_percent;
    return std::make_unique<CycloneCache>(config, stats_.get(),
                                          handler_.get());
  }

  bool IsHealthyTest() {
    return cache_ != nullptr && cache_->IsHealthy();
  }

  // A cache of its own (own path, own statistics and message handler) for
  // tests that count statistics or log lines.
  std::unique_ptr<CycloneCache> MakeCountingCache(Statistics* stats,
                                                  MessageHandler* handler,
                                                  const char* suffix) {
    CycloneCache::Config config;
    config.cache_path = StrCat(cache_path_, "_", suffix);
    config.cache_size_bytes = 10 * 1024 * 1024;
    config.ram_cache_size_bytes = 0;
    config.enable_checksum = true;
    config.num_segments = 0;
    return std::make_unique<CycloneCache>(config, stats, handler);
  }

  std::unique_ptr<NullStatistics> stats_;
  std::unique_ptr<GoogleMessageHandler> handler_;
  std::unique_ptr<CycloneCache> cache_;
  GoogleString cache_path_;
};

// Basic test: verify the cache is healthy after initialization
TEST_F(CycloneCacheTest, IsHealthy) {
  // Note: This test may fail if the Cyclone library is not available
  // or if the cache path is not writable.
  if (IsHealthyTest()) {
    EXPECT_TRUE(cache_->IsHealthy());
    EXPECT_TRUE(cache_->IsBlocking());
    EXPECT_EQ("CycloneCache", cache_->Name());
  } else {
    // If cache failed to initialize, skip the rest of the tests
    GTEST_SKIP() << "CycloneCache failed to initialize - "
                 << "Cyclone library may not be available";
  }
}

// Test basic put/get operations
TEST_F(CycloneCacheTest, PutGet) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckPut("key1", "value1");
  CheckGet("key1", "value1");
}

// Test that getting a non-existent key returns not found
TEST_F(CycloneCacheTest, NotFound) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckNotFound("nonexistent_key");
}

// Test delete operation
TEST_F(CycloneCacheTest, Delete) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckPut("key1", "value1");
  CheckGet("key1", "value1");
  CheckDelete("key1");
  CheckNotFound("key1");
}

// Test overwriting an existing key
TEST_F(CycloneCacheTest, Overwrite) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckPut("key1", "value1");
  CheckGet("key1", "value1");
  CheckPut("key1", "value2");
  CheckGet("key1", "value2");
}

// Test-only seam exported by the Cyclone wrapper's C++23 translation unit
// (cyclone_wrapper.cc) and deliberately not part of the vendored C ABI
// header: arms a one-shot override that makes the next write onto an
// existing key behave as if its pre-write remove had returned Busy (the
// cross-process locks held by another process past the library's wait
// cap), leaving the old entry in place.
extern "C" void cyclone_cache_test_busy_next_pre_write_remove(void);

// A write whose pre-write remove cannot complete must be dropped, not
// stored over the live entry: overwriting in place is exactly what the
// remove-first step exists to prevent.  Contention is not a cache fault, so
// the dropped write is skipped (not counted as a failure), and the next
// uncontended write succeeds.
TEST_F(CycloneCacheTest, BusyPreWriteRemoveDropsWrite) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);

  CycloneCache::Config config;
  config.cache_path = StrCat(cache_path_, "_busy_remove");
  config.cache_size_bytes = 10 * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = true;
  config.num_segments = 0;
  CycloneCache cache(config, &stats, handler_.get());
  ASSERT_TRUE(cache.IsHealthy());

  CheckPut(&cache, "busy_key", "old_value");
  CheckGet(&cache, "busy_key", "old_value");
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kFailures)->Get());

  // The next pre-write remove reports Busy, as a contended peer would
  // cause; the remove is not performed and the entry stays live.
  cyclone_cache_test_busy_next_pre_write_remove();
  SharedString new_value("new_value");
  cache.Put("busy_key", new_value);

  // The write was dropped and skipped, and reads still see the old entry.
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kFailures)->Get());
  CheckGet(&cache, "busy_key", "old_value");

  // The seam is one-shot: the next overwrite takes the normal
  // remove-then-write path and succeeds.
  CheckPut(&cache, "busy_key", "new_value");
  CheckGet(&cache, "busy_key", "new_value");
  EXPECT_EQ(2, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kFailures)->Get());

  cache.ShutDown();
}

// Second test-only seam from the same translation unit (see the comment
// above): arms a one-shot override that makes the next write behave as if
// its pre-write presence check had returned Busy -- presence unknown, as
// a writer holding the key's directory bucket past the seqlock wait
// budget would cause.
extern "C" void cyclone_cache_test_busy_next_pre_write_exists(void);

// A write whose pre-write presence check cannot answer must also be
// dropped, not stored blind: with presence unknown the slot may hold a
// live entry, and overwriting in place is what the remove-first step
// exists to prevent.  Skipped like any contended write (not counted as a
// failure); the next uncontended write succeeds.
TEST_F(CycloneCacheTest, BusyPreWriteExistsDropsWrite) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);

  CycloneCache::Config config;
  config.cache_path = StrCat(cache_path_, "_busy_exists");
  config.cache_size_bytes = 10 * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = true;
  config.num_segments = 0;
  CycloneCache cache(config, &stats, handler_.get());
  ASSERT_TRUE(cache.IsHealthy());

  CheckPut(&cache, "busy_key", "old_value");
  CheckGet(&cache, "busy_key", "old_value");
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kFailures)->Get());

  // The next pre-write presence check reports Busy, as a contended writer
  // would cause; presence is unknown and the write must be dropped.
  cyclone_cache_test_busy_next_pre_write_exists();
  SharedString new_value("new_value");
  cache.Put("busy_key", new_value);

  // The write was dropped and skipped, and reads still see the old entry.
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kFailures)->Get());
  CheckGet(&cache, "busy_key", "old_value");

  // The seam is one-shot: the next overwrite takes the normal
  // exists-remove-write path and succeeds.
  CheckPut(&cache, "busy_key", "new_value");
  CheckGet(&cache, "busy_key", "new_value");
  EXPECT_EQ(2, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kFailures)->Get());

  cache.ShutDown();
}

// Cache keys embed request URLs, so the write-failure diagnostic must escape
// the key: control characters in it must not forge extra log lines or emit
// terminal escapes.
TEST_F(CycloneCacheTest, WriteFailureLogEscapesKey) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);
  MockMessageHandler handler(new NullMutex);

  CycloneCache::Config config;
  config.cache_path = StrCat(cache_path_, "_key_escape");
  config.cache_size_bytes = 10 * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = true;
  config.num_segments = 0;
  CycloneCache cache(config, &stats, &handler);
  ASSERT_TRUE(cache.IsHealthy());

  const GoogleString key = "key-with-\r\nforged-line-and-\x1b" "escape";
  cyclone_cache_test_fail_next_commit(kSeamIoError);
  SharedString value("value");
  cache.Put(key, value);
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kFailures)->Get());

  GoogleString log;
  StringWriter writer(&log);
  handler.Dump(&writer);
  // No control characters from the key may reach the log verbatim; the key
  // must appear only in escaped form.
  EXPECT_EQ(GoogleString::npos, log.find('\r')) << log;
  EXPECT_EQ(GoogleString::npos, log.find('\x1b')) << log;
  EXPECT_NE(GoogleString::npos, log.find(CEscape(key))) << log;

  cache.ShutDown();
}

// A write whose commit reports contention stored nothing, but it is a
// skipped write, not a cache fault: not counted as a failure and not
// logged, however often it happens.
TEST_F(CycloneCacheTest, BusyCommitIsSkippedWrite) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);
  MockMessageHandler handler(new NullMutex);
  std::unique_ptr<CycloneCache> cache =
      MakeCountingCache(&stats, &handler, "busy_commit");
  ASSERT_TRUE(cache->IsHealthy());
  int warnings_before = handler.MessagesOfType(kWarning);

  for (int i = 0; i < 3; ++i) {
    cyclone_cache_test_fail_next_commit(kSeamBusy);
    SharedString value("value");
    cache->Put("contended_key", value);
  }

  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kFailures)->Get());
  EXPECT_EQ(warnings_before, handler.MessagesOfType(kWarning));
  CheckNotFound(cache.get(), "contended_key");

  // The next uncontended write is stored.
  CheckPut(cache.get(), "contended_key", "value");
  CheckGet(cache.get(), "contended_key", "value");
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kInserts)->Get());

  cache->ShutDown();
}

// A commit that fails for any other reason is a real write failure:
// counted and (sampled) logged.
TEST_F(CycloneCacheTest, FailedCommitIsCountedFailure) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);
  MockMessageHandler handler(new NullMutex);
  std::unique_ptr<CycloneCache> cache =
      MakeCountingCache(&stats, &handler, "failed_commit");
  ASSERT_TRUE(cache->IsHealthy());
  int warnings_before = handler.MessagesOfType(kWarning);

  cyclone_cache_test_fail_next_commit(kSeamIoError);
  SharedString value("value");
  cache->Put("key", value);

  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kInserts)->Get());
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kFailures)->Get());
  EXPECT_EQ(warnings_before + 1, handler.MessagesOfType(kWarning));
  CheckNotFound(cache.get(), "key");

  cache->ShutDown();
}

// Every failed read is a miss for the caller.  Contention is a silent miss
// like a plain one; a damaged entry or an unreadable disk is a miss too,
// but logged, because it points at the cache file or the device.
TEST_F(CycloneCacheTest, ReadErrorsAreMisses) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);
  MockMessageHandler handler(new NullMutex);
  std::unique_ptr<CycloneCache> cache =
      MakeCountingCache(&stats, &handler, "read_errors");
  ASSERT_TRUE(cache->IsHealthy());

  CheckPut(cache.get(), "key", "value");
  CheckGet(cache.get(), "key", "value");
  int warnings = handler.MessagesOfType(kWarning);
  int64 misses = stats.GetVariable(CycloneCache::kMisses)->Get();

  // Plain miss: silent.
  CheckNotFound(cache.get(), "absent");
  EXPECT_EQ(++misses, stats.GetVariable(CycloneCache::kMisses)->Get());
  EXPECT_EQ(warnings, handler.MessagesOfType(kWarning));

  // Contention: a silent miss, although the entry is there.
  cyclone_cache_test_fail_next_read(kSeamBusy);
  CheckNotFound(cache.get(), "key");
  EXPECT_EQ(++misses, stats.GetVariable(CycloneCache::kMisses)->Get());
  EXPECT_EQ(warnings, handler.MessagesOfType(kWarning));

  // Damaged entry: a miss, and logged.
  cyclone_cache_test_fail_next_read(kSeamCorrupted);
  CheckNotFound(cache.get(), "key");
  EXPECT_EQ(++misses, stats.GetVariable(CycloneCache::kMisses)->Get());
  EXPECT_EQ(++warnings, handler.MessagesOfType(kWarning));

  // Unreadable disk: a miss, and logged.  Log lines are sampled 1-in-1024
  // per cache, so the second failure on this cache is counted but not
  // logged.
  cyclone_cache_test_fail_next_read(kSeamIoError);
  CheckNotFound(cache.get(), "key");
  EXPECT_EQ(++misses, stats.GetVariable(CycloneCache::kMisses)->Get());
  EXPECT_EQ(warnings, handler.MessagesOfType(kWarning));

  // The seams are one-shot: the entry is served again.
  CheckGet(cache.get(), "key", "value");

  cache->ShutDown();
}

// No exception from the cache library may cross the C API into the
// module: an allocation failure inside a write or a read is reported as a
// failed operation and the cache keeps working.
TEST_F(CycloneCacheTest, AllocationFailureDoesNotEscape) {
#ifdef CYCLONE_CACHE_TEST_NO_ALLOC_HOOK
  GTEST_SKIP() << "operator new is interposed by the sanitizer";
#else
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);
  MockMessageHandler handler(new NullMutex);
  std::unique_ptr<CycloneCache> cache =
      MakeCountingCache(&stats, &handler, "alloc_failure");
  ASSERT_TRUE(cache->IsHealthy());
  CheckPut(cache.get(), "key", "value");

  // A write that runs out of memory is a failed write.
  SharedString value("other value");
  t_failed_allocations = 0;
  t_fail_next_allocation = true;
  cache->Put("other", value);
  t_fail_next_allocation = false;
  EXPECT_EQ(1, t_failed_allocations);
  EXPECT_EQ(1, stats.GetVariable(CycloneCache::kFailures)->Get());

  // A read that runs out of memory is a miss.  The callback is made
  // before arming, so the injected failure lands inside the lookup.
  int64 misses = stats.GetVariable(CycloneCache::kMisses)->Get();
  Callback* callback = NewCallback();
  t_failed_allocations = 0;
  t_fail_next_allocation = true;
  cache->Get("key", callback);
  t_fail_next_allocation = false;
  EXPECT_EQ(1, t_failed_allocations);
  WaitAndCheckNotFound(callback);
  EXPECT_EQ(misses + 1, stats.GetVariable(CycloneCache::kMisses)->Get());

  // Fail each allocation of a write in turn, including the ones made after
  // the write handle is open (the abort-then-report path): every one is a
  // failed write and nothing escapes, until a write needs fewer allocations
  // than were let through and succeeds.
  int64 failures = stats.GetVariable(CycloneCache::kFailures)->Get();
  bool completed = false;
  for (int skip = 0; skip < 256 && !completed; ++skip) {
    GoogleString step_key = StrCat("step", IntegerToString(skip));
    SharedString step_value(GoogleString(4096, 'x'));
    t_failed_allocations = 0;
    t_allocations_before_failure = skip;
    cache->Put(step_key, step_value);
    t_allocations_before_failure = -1;
    if (t_failed_allocations == 0) {
      completed = true;
      CheckGet(cache.get(), step_key, GoogleString(4096, 'x'));
    } else {
      EXPECT_EQ(++failures, stats.GetVariable(CycloneCache::kFailures)->Get())
          << "allocation " << skip;
      CheckNotFound(cache.get(), step_key.c_str());
    }
  }
  EXPECT_TRUE(completed);

  // Nothing was left half-done: both keys work normally afterwards.
  CheckGet(cache.get(), "key", "value");
  CheckPut(cache.get(), "other", "other value");
  CheckGet(cache.get(), "other", "other value");

  cache->ShutDown();
#endif
}

// Test multiple keys
TEST_F(CycloneCacheTest, MultipleKeys) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckPut("key1", "value1");
  CheckPut("key2", "value2");
  CheckPut("key3", "value3");

  CheckGet("key1", "value1");
  CheckGet("key2", "value2");
  CheckGet("key3", "value3");
}

// Test with larger values
TEST_F(CycloneCacheTest, LargeValue) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  // Create a 100KB value
  GoogleString large_value(100 * 1024, 'x');
  CheckPut("large_key", large_value);
  CheckGet("large_key", large_value);
}

// Test with binary data (including null bytes)
TEST_F(CycloneCacheTest, BinaryData) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  GoogleString binary_data;
  for (int i = 0; i < 256; ++i) {
    binary_data.push_back(static_cast<char>(i));
  }
  CheckPut("binary_key", binary_data);
  CheckGet("binary_key", binary_data);
}

// Test shutdown behavior
TEST_F(CycloneCacheTest, ShutDown) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckPut("key1", "value1");
  cache_->ShutDown();

  EXPECT_FALSE(cache_->IsHealthy());

  // After shutdown, gets should return not found
  CheckNotFound("key1");
}

// Request threads may still be calling into the cache while another thread
// shuts it down; every call must either use the live cache or see it as shut
// down, never a destroyed one.
TEST_F(CycloneCacheTest, ShutDownWhileOperationsInFlight) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckPut("key", "value");
  std::atomic<int> started(0);
  std::atomic<bool> stop(false);
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([this, t, &started, &stop]() {
      GoogleString key = StrCat("key", IntegerToString(t));
      SharedString value("value");
      started.fetch_add(1);
      while (!stop.load()) {
        CacheInterface::SynchronousCallback callback;
        cache_->Get("key", &callback);
        EXPECT_TRUE(callback.called());
        cache_->Put(key, value);
        cache_->Delete(key);
        cache_->IsHealthy();
      }
    });
  }
  while (started.load() < 4) {
    std::this_thread::yield();
  }
  cache_->ShutDown();
  EXPECT_FALSE(cache_->IsHealthy());
  stop.store(true);
  for (std::thread& worker : workers) {
    worker.join();
  }
}

// Two writers storing the same key race between the wrapper's pre-write
// remove and its write; a reader must only ever see one complete value, never
// an empty or mixed one.  The RAM tier is on: overwrites with it enabled are
// the case the pre-write remove guards.
TEST_F(CycloneCacheTest, ConcurrentWritersOfOneKey) {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
  // The cache library's directory probes are optimistic seqlock reads that
  // a concurrent insert into the same bucket races by design.
  GTEST_SKIP() << "same-bucket seqlock reads are reported by TSan";
#endif
#endif
  CycloneCache::Config config;
  config.cache_path = StrCat(cache_path_, "_writers");
  config.cache_size_bytes = 10 * 1024 * 1024;
  config.ram_cache_size_bytes = 1024 * 1024;
  config.enable_checksum = true;
  config.num_segments = 0;
  CycloneCache cache(config, stats_.get(), handler_.get());
  if (!cache.IsHealthy()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  const GoogleString value_a(3000, 'a');
  const GoogleString value_b(5000, 'b');
  const int kIterations = 500;
  std::atomic<int> unexpected(0);
  std::atomic<bool> stop(false);
  auto writer = [&cache](const GoogleString& value) {
    SharedString shared(value);
    for (int i = 0; i < kIterations; ++i) {
      cache.Put("contended", shared);
    }
  };
  std::thread reader([&]() {
    while (!stop.load()) {
      CacheInterface::SynchronousCallback callback;
      cache.Get("contended", &callback);
      if (callback.state() == CacheInterface::kAvailable) {
        StringPiece got = callback.value().Value();
        if (got != value_a && got != value_b) {
          unexpected.fetch_add(1);
        }
      }
    }
  });
  std::thread writer_a(writer, value_a);
  std::thread writer_b(writer, value_b);
  writer_a.join();
  writer_b.join();
  stop.store(true);
  reader.join();

  EXPECT_EQ(0, unexpected.load());
  CacheInterface::SynchronousCallback callback;
  cache.Get("contended", &callback);
  ASSERT_EQ(CacheInterface::kAvailable, callback.state());
  StringPiece last = callback.value().Value();
  EXPECT_TRUE(last == value_a || last == value_b);
  cache.ShutDown();
}

// Test empty key
TEST_F(CycloneCacheTest, EmptyKey) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  // Empty keys should work
  CheckPut("", "empty_key_value");
  CheckGet("", "empty_key_value");
}

// Test empty value
TEST_F(CycloneCacheTest, EmptyValue) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  CheckPut("empty_value_key", "");
  CheckGet("empty_value_key", "");
}

// Test MultiGet functionality (inherited from CacheTestBase)
TEST_F(CycloneCacheTest, MultiGet) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  TestMultiGet();
}

// ============================================================================
// Zero-Copy Tests
// ============================================================================

// Custom callback for testing zero-copy behavior
class ZeroCopyTestCallback : public CacheInterface::Callback {
 public:
  ZeroCopyTestCallback() = default;

  void Done(CacheInterface::KeyState state) override {
    state_ = state;
    done_ = true;
  }

  bool done() const { return done_; }
  CacheInterface::KeyState state() const { return state_; }

 private:
  bool done_ = false;
  CacheInterface::KeyState state_ = CacheInterface::kNotFound;
};

// Test that value().is_mapped() returns true for disk cache hits
TEST_F(CycloneCacheTest, ZeroCopyMappedValueAvailable) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  // Put a value
  SharedString value("test_value_for_zero_copy");
  cache_->Put("zero_copy_key", value);

  // Get it back and check if mapped value is available
  ZeroCopyTestCallback callback;
  cache_->Get("zero_copy_key", &callback);

  EXPECT_TRUE(callback.done());
  EXPECT_EQ(CacheInterface::kAvailable, callback.state());

  // For disk cache hits (RAM cache disabled), the value should be memory-mapped
  EXPECT_TRUE(callback.value().is_mapped());

  // The value should contain the correct data
  EXPECT_EQ("test_value_for_zero_copy", callback.value().Value());

  // The convenience method should also return the correct data
  EXPECT_EQ("test_value_for_zero_copy", callback.value_as_string_piece());
}

// Test that the traditional value() method still works
TEST_F(CycloneCacheTest, ZeroCopyBackwardCompatibility) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  SharedString value("backward_compat_value");
  cache_->Put("compat_key", value);

  ZeroCopyTestCallback callback;
  cache_->Get("compat_key", &callback);

  EXPECT_TRUE(callback.done());
  EXPECT_EQ(CacheInterface::kAvailable, callback.state());

  // Traditional value() should still work
  EXPECT_EQ("backward_compat_value", callback.value().Value());
}

// Test zero-copy with large values
TEST_F(CycloneCacheTest, ZeroCopyLargeValue) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  // Create a 100KB value (smaller to avoid test slowdown from large string
  // comparison output on failure)
  GoogleString large_data(100 * 1024, 'L');
  SharedString value(large_data);
  cache_->Put("large_zero_copy_key", value);

  ZeroCopyTestCallback callback;
  cache_->Get("large_zero_copy_key", &callback);

  EXPECT_TRUE(callback.done());
  EXPECT_EQ(CacheInterface::kAvailable, callback.state());
  EXPECT_TRUE(callback.value().is_mapped());

  // Check size first to give better error messages
  EXPECT_EQ(large_data.size(), callback.value().size());

  // Then check content
  EXPECT_EQ(large_data, callback.value().Value());
}

// Test that value is empty for cache misses
TEST_F(CycloneCacheTest, ZeroCopyMissNoMappedValue) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  ZeroCopyTestCallback callback;
  cache_->Get("nonexistent_key", &callback);

  EXPECT_TRUE(callback.done());
  EXPECT_EQ(CacheInterface::kNotFound, callback.state());
  // For misses, the value should be empty
  EXPECT_TRUE(callback.value().empty());
}

// Test that value can be converted to owned SharedString
TEST_F(CycloneCacheTest, ZeroCopyToOwned) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  SharedString value("convert_to_owned_test");
  cache_->Put("to_owned_key", value);

  ZeroCopyTestCallback callback;
  cache_->Get("to_owned_key", &callback);

  EXPECT_TRUE(callback.done());
  EXPECT_EQ(CacheInterface::kAvailable, callback.state());
  EXPECT_TRUE(callback.value().is_mapped());

  // Convert to owned SharedString
  SharedString owned = callback.value().ToOwned();
  EXPECT_EQ("convert_to_owned_test", owned.Value());

  // The owned value should remain valid even after the callback is destroyed
  // (this tests that the copy was made correctly)
}

// Test that the value remains valid while the MappedSharedString is alive
TEST_F(CycloneCacheTest, ZeroCopyValueLifetime) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  SharedString value("lifetime_test_value");
  cache_->Put("lifetime_key", value);

  // Get the value
  std::unique_ptr<ZeroCopyTestCallback> callback =
      std::make_unique<ZeroCopyTestCallback>();
  cache_->Get("lifetime_key", callback.get());

  EXPECT_TRUE(callback->done());
  EXPECT_EQ(CacheInterface::kAvailable, callback->state());
  EXPECT_TRUE(callback->value().is_mapped());

  // Copy the MappedSharedString to keep it alive
  MappedSharedString mapped_copy = callback->value();

  // Verify data is accessible through the copy
  EXPECT_EQ("lifetime_test_value", mapped_copy.Value());

  // Reset the callback (this doesn't affect the mapped copy)
  callback.reset();

  // The mapped copy should still have valid data
  EXPECT_EQ("lifetime_test_value", mapped_copy.Value());
}

// Test is_mapped() returns true for disk-backed values
TEST_F(CycloneCacheTest, ZeroCopyIsMapped) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  SharedString value("is_mapped_test");
  cache_->Put("is_mapped_key", value);

  ZeroCopyTestCallback callback;
  cache_->Get("is_mapped_key", &callback);

  EXPECT_TRUE(callback.done());
  EXPECT_EQ(CacheInterface::kAvailable, callback.state());

  // For disk cache hits, is_mapped() should return true
  EXPECT_TRUE(callback.value().is_mapped());
}

// ============================================================================
// Directory Persistence Tests
// ============================================================================

// Test that cached data survives a full cache shutdown and recreation.
// This verifies the persist_directory feature: with it enabled, the cache
// directory (index) is mmap'd into the data file, so entries written by
// a previous cache instance are still readable after restart.
TEST_F(CycloneCacheTest, DataSurvivesRestart) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  // Step 1: Write several key-value pairs to the cache.
  CheckPut("persist_key1", "persist_value1");
  CheckPut("persist_key2", "persist_value2");
  CheckPut("persist_key3", "persist_value3");

  // Verify they are readable before shutdown.
  CheckGet("persist_key1", "persist_value1");
  CheckGet("persist_key2", "persist_value2");
  CheckGet("persist_key3", "persist_value3");

  // Step 2: Fully shut down and destroy the cache instance.
  cache_->ShutDown();
  cache_.reset();

  // Step 3: Create a NEW cache instance pointing to the same path.
  // The persist_directory default (true) ensures the directory is recovered.
  CycloneCache::Config config;
  config.cache_path = cache_path_;
  config.cache_size_bytes = 10 * 1024 * 1024;  // 10 MB
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = true;
  config.num_segments = 0;
  // persist_directory defaults to true

  cache_ = std::make_unique<CycloneCache>(config, stats_.get(),
                                          handler_.get());
  ASSERT_TRUE(cache_->IsHealthy()) << "Failed to recreate cache";

  // Step 4: Read back the previously-written keys and verify values.
  CheckGet("persist_key1", "persist_value1");
  CheckGet("persist_key2", "persist_value2");
  CheckGet("persist_key3", "persist_value3");
}

// Verify that with persist_directory disabled, data does NOT survive restart.
// This is the inverse test — confirms the feature actually matters.
TEST_F(CycloneCacheTest, DataLostWithoutPersistDirectory) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  // Shut down the default cache (which has persist_directory=true).
  cache_->ShutDown();
  cache_.reset();

  // Create a cache with persist_directory disabled.
  GoogleString volatile_path = StrCat(cache_path_, "_volatile");

  CycloneCache::Config config;
  config.cache_path = volatile_path;
  config.cache_size_bytes = 10 * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = true;
  config.num_segments = 0;
  config.persist_directory = false;

  cache_ = std::make_unique<CycloneCache>(config, stats_.get(),
                                          handler_.get());
  ASSERT_TRUE(cache_->IsHealthy()) << "Failed to create volatile cache";

  // Write data.
  CheckPut("volatile_key1", "volatile_value1");
  CheckPut("volatile_key2", "volatile_value2");
  CheckGet("volatile_key1", "volatile_value1");

  // Destroy and recreate.
  cache_->ShutDown();
  cache_.reset();

  cache_ = std::make_unique<CycloneCache>(config, stats_.get(),
                                          handler_.get());
  ASSERT_TRUE(cache_->IsHealthy()) << "Failed to recreate volatile cache";

  // Data should be gone — the directory was only in RAM.
  CheckNotFound("volatile_key1");
  CheckNotFound("volatile_key2");
}

// With a cache far below the ~256 MB two-volume floor, the small tier must
// report inactive and small-tier operations must fall back to the default
// keyspace -- i.e. the view is always safe to use.
TEST_F(CycloneCacheTest, SmallTierInactiveFallsBackToDefault) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }
  std::unique_ptr<CycloneCache> cache =
      MakeTieredCache(10 * 1024 * 1024, 10, "tier_inactive");
  ASSERT_TRUE(cache->IsHealthy());
  EXPECT_EQ(10, cache->config().small_tier_percent);
  EXPECT_FALSE(cache->small_tier_active());

  CacheInterface* view = cache->small_tier_view();
  EXPECT_EQ("CycloneCache(small_tier)", view->Name());
  EXPECT_EQ(cache.get(), view->Backend());
  EXPECT_TRUE(view->IsBlocking());

  // A write through the view lands in the default keyspace when the tier is
  // inactive: readable through the plain interface and vice versa.
  CheckPut(view, "fallback_key", "fallback_value");
  CheckGet(cache.get(), "fallback_key", "fallback_value");
  CheckPut(cache.get(), "default_key", "default_value");
  CheckGet(view, "default_key", "default_value");

  cache->ShutDown();
}

// With a cache large enough to host both volumes, the tiers are physically
// separate keyspaces: the same key refers to two independent entries.
// The volume files are sparse, so the apparent 320 MB costs only the few
// bytes actually written.
TEST_F(CycloneCacheTest, SmallTierActiveSeparateKeyspace) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }
  std::unique_ptr<CycloneCache> cache = MakeTieredCache(
      static_cast<int64>(320) * 1024 * 1024, 10, "tier_active");
  ASSERT_TRUE(cache->IsHealthy());
  ASSERT_TRUE(cache->small_tier_active());

  CacheInterface* view = cache->small_tier_view();

  // The directory is persistent and the cache file survives reruns, so clear
  // both tiers' copies of the key before asserting on visibility.
  cache->Delete("shared_key");
  view->Delete("shared_key");

  // Entry written through the view is not visible in the default tier.
  CheckPut(view, "shared_key", "small_value");
  CheckGet(view, "shared_key", "small_value");
  CheckNotFound(cache.get(), "shared_key");

  // And the same key written to the default tier stays independent.
  CheckPut(cache.get(), "shared_key", "default_value");
  CheckGet(cache.get(), "shared_key", "default_value");
  CheckGet(view, "shared_key", "small_value");

  // Deletes are tier-scoped, too.
  view->Delete("shared_key");
  CheckNotFound(view, "shared_key");
  CheckGet(cache.get(), "shared_key", "default_value");

  cache->ShutDown();
}

// ============================================================================
// RAM-Tier Statistics Tests
// ============================================================================

// Every hit is attributed to exactly one tier, so cyclone_cache_ram_hits +
// cyclone_cache_disk_hits must equal cyclone_cache_hits.  Whether the RAM
// tier populates on a read-through is a Cyclone policy detail, so only the
// sum is asserted here; the tier-off test below pins the disk side.
TEST_F(CycloneCacheTest, RamTierHitStatsSumToHits) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);

  CycloneCache::Config config;
  config.cache_path = StrCat(cache_path_, "_ram_stats");
  config.cache_size_bytes = 10 * 1024 * 1024;
  config.ram_cache_size_bytes = 1024 * 1024;
  config.enable_checksum = true;
  config.num_segments = 0;
  CycloneCache cache(config, &stats, handler_.get());
  ASSERT_TRUE(cache.IsHealthy());

  CheckPut(&cache, "ram_stats_key", "ram_stats_value");
  const int64 kGets = 5;
  for (int i = 0; i < kGets; ++i) {
    CheckGet(&cache, "ram_stats_key", "ram_stats_value");
  }

  EXPECT_EQ(kGets, stats.GetVariable(CycloneCache::kHits)->Get());
  EXPECT_EQ(kGets, stats.GetVariable(CycloneCache::kRamHits)->Get() +
                       stats.GetVariable(CycloneCache::kDiskHits)->Get());

  cache.ShutDown();
}

// With the RAM tier disabled, every hit is a disk hit: cyclone_cache_ram_hits
// stays 0 and cyclone_cache_disk_hits accounts for all of them.
TEST_F(CycloneCacheTest, RamTierHitStatsZeroWhenTierDisabled) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  CycloneCache::InitStats(&stats);

  CycloneCache::Config config;
  config.cache_path = StrCat(cache_path_, "_no_ram_stats");
  config.cache_size_bytes = 10 * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = true;
  config.num_segments = 0;
  CycloneCache cache(config, &stats, handler_.get());
  ASSERT_TRUE(cache.IsHealthy());

  CheckPut(&cache, "disk_stats_key", "disk_stats_value");
  const int64 kGets = 3;
  for (int i = 0; i < kGets; ++i) {
    CheckGet(&cache, "disk_stats_key", "disk_stats_value");
  }

  EXPECT_EQ(kGets, stats.GetVariable(CycloneCache::kHits)->Get());
  EXPECT_EQ(0, stats.GetVariable(CycloneCache::kRamHits)->Get());
  EXPECT_EQ(kGets, stats.GetVariable(CycloneCache::kDiskHits)->Get());

  cache.ShutDown();
}

// The tier defaults to off in the adapter Config: behavior and reporting are
// identical to a pre-tier cache.
TEST_F(CycloneCacheTest, SmallTierOffByDefault) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }
  EXPECT_EQ(0, cache_->config().small_tier_percent);
  EXPECT_FALSE(cache_->small_tier_active());
}

// ============================================================================
// Small-Tier Retention at Scale (metadata L2 hit-rate reproduction)
// ============================================================================
//
// Production observed the metadata L2 (file_cache_small) retaining almost
// nothing under load: ~128k inserts, 18 hits, ~239k misses, while the small
// volume sat only ~6% full (no eviction).  These tests bisect that: the
// single-process probe below must be GREEN (retention ~100%) for the fault to
// be multi-process; if it is RED, the fault is in the single-process small-
// tier put/get path itself.

// Counting callback: records found/not-found without asserting per-get, so the
// caller can compute an aggregate hit rate.
class HitCountCallback : public CacheInterface::Callback {
 public:
  void Done(CacheInterface::KeyState state) override {
    found_ = (state == CacheInterface::kAvailable);
    done_ = true;
  }
  bool found() const { return found_; }
  bool done() const { return done_; }

 private:
  bool done_ = false;
  bool found_ = false;
};

// Small-object tier (metadata L2) coverage.
//
// These three probes were written while chasing a production signature: small-
// tier writes reported success (cyclone_cache_failures ~ 0) while reads came
// back nearly empty (18 hits / 239k inserts), even though the churning default
// tier stayed visible (276k hits).  Each probe pins one candidate explanation
// -- lost inserts, cross-process write incoherence, cross-process read
// invisibility to a long-lived reader.
//
// All three PASS, and that is the finding: the small tier retains, and is
// coherent and visible across processes, in these configurations.  Plain
// small-tier incoherence therefore does NOT explain the production signature;
// the fault lies elsewhere.  The probes stay as standing regression gates, so
// read each failure message below as "what a RED here would mean", not as a
// prediction that it will fire.

// Write many metadata-sized entries to the small-object tier through
// small_tier_view(), then read them straight back in the same process.  The
// small volume (~128 MB) dwarfs the ~10 MB written, so the ring never wraps:
// every miss is a lost insert, not an eviction.  Retention must be ~100%.
TEST_F(CycloneCacheTest, SmallTierRetentionAtScale) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }
  // 320 MiB total -> one 128 MiB small-tier stripe (65,536 directory slots),
  // still far above the ~10 MB this test writes, so the ring never wraps.
  // The default tier drops from 16 stripes to 6 with it: this test never
  // writes the default tier, and every stripe costs an fsync on create and
  // an msync on teardown (#235).
  std::unique_ptr<CycloneCache> cache = MakeTieredCache(
      static_cast<int64>(320) * 1024 * 1024, 10, "retention_scale");
  ASSERT_TRUE(cache->IsHealthy());
  ASSERT_TRUE(cache->small_tier_active())
      << "small tier must be active for this test to be meaningful";

  CacheInterface* view = cache->small_tier_view();

  const int kN = 4000;
  for (int i = 0; i < kN; ++i) {
    GoogleString key = StrCat("meta/", IntegerToString(i));
    SharedString value(GoogleString(200, static_cast<char>('a' + (i % 26))));
    view->Put(key, value);
  }

  int hits = 0;
  for (int i = 0; i < kN; ++i) {
    GoogleString key = StrCat("meta/", IntegerToString(i));
    HitCountCallback cb;
    view->Get(key, &cb);
    if (cb.found()) {
      ++hits;
    }
  }

  // RED/GREEN gate: near-total retention with no eviction pressure.
  EXPECT_GE(hits, static_cast<int>(0.99 * kN))
      << "small-tier retention " << hits << "/" << kN
      << " with the ring only ~10 MB / ~128 MB full (no eviction) -- inserts "
         "landed but reads miss: single-process small-tier correctness fault";

  cache->ShutDown();
}

#ifndef _WIN32  // fork()/waitpid(): POSIX-only cross-process tests
// A wait status a human can read in a failure message.  The raw integer hid
// the cause in the #321 runs: "wait status 14" was the writer's own watchdog
// (SIGALRM) firing on a starved disk, and was being read as an exit code.
static GoogleString DescribeWaitStatus(int status) {
  if (WIFEXITED(status)) {
    return StrCat("exited with code ", IntegerToString(WEXITSTATUS(status)));
  }
  if (WIFSIGNALED(status)) {
    const int sig = WTERMSIG(status);
    if (sig == SIGALRM) {
      return StrCat("killed by SIGALRM (signal ", IntegerToString(sig),
                    ", the writer watchdog)");
    }
    return StrCat("killed by signal ", IntegerToString(sig));
  }
  return StrCat("unusual wait status ", IntegerToString(status));
}

// The production topology: N independent "worker" processes, each with its own
// CycloneCache attached to the SAME on-disk volume (exactly how Apache
// MPM-event workers share cyclone.dat/.small).  The adapter hardcodes
// multi_process_config.total_processes = 1 for every instance, so no worker
// knows the others exist.  All writes target the small-object tier, which the
// volume dwarfs (~7 MB written into 128 MB, no eviction), so any loss here
// would be cross-process incoherence and not eviction.
//
// Retention is ~100%: writes from unrelated processes ARE coherent even with
// total_processes = 1.  A RED here would mean that stopped being true.
TEST_F(CycloneCacheTest, SmallTierRetentionMultiProcess) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  // Release the SetUp cache so forked children don't inherit its fds/mmaps.
  // This also leaves the parent SINGLE-THREADED at fork() time (ShutDown()
  // joins Cyclone's threads), which is load-bearing: a child forked from a
  // multi-threaded parent may not create threads under TSan -- and, per POSIX,
  // may not safely call anything non-async-signal-safe at all.  Do not move a
  // live CycloneCache above the fork.
  cache_->ShutDown();
  cache_.reset();

  const GoogleString mp_path = StrCat(cache_path_, "_mp");
  auto make_cfg = [&]() {
    CycloneCache::Config config;
    config.cache_path = mp_path;
    // 320 MiB -> one 128 MiB small-tier stripe (16,384 four-slot buckets,
    // 65,536 slots), clearing the 256 MiB + 128 B floor below which the
    // small tier goes inactive (see the visibility test below).  At 10 GiB
    // this volume was 23 stripes, and every stripe costs an fsync on create
    // and an msync on teardown -- that per-stripe cost, on a contended
    // runner disk, is what timed the shard out (#235).
    config.cache_size_bytes = static_cast<int64>(320) * 1024 * 1024;
    config.ram_cache_size_bytes = 0;
    config.enable_checksum = true;
    config.num_segments = 0;
    config.small_tier_percent = 10;
    return config;
  };

  // Parent lays down the volumes first, then closes: children open-existing,
  // isolating steady-state write coherence from any concurrent-creation race.
  {
    CycloneCache seed(make_cfg(), stats_.get(), handler_.get());
    ASSERT_TRUE(seed.IsHealthy());
    ASSERT_TRUE(seed.small_tier_active())
        << "320 MiB cache must host an active small tier";
    seed.ShutDown();
  }

  const int kProcs = 8;
  // 8 x 1000 = 8,000 entries over 16,384 four-slot buckets: a mean of 0.49
  // keys per bucket, so the expected count of buckets overflowing their
  // four slots is under 4 (Poisson) -- under 0.05% of the keys, well inside
  // the 1% tolerance below.  A larger loss points at cross-process
  // incoherence rather than hash-bucket saturation, which is the property
  // the old 10 GiB volume bought (#235).  One more source of loss remains:
  // at 320 MiB all eight writers share one small-tier stripe and its
  // cross-process write lock, and a Put that waits more than 250 ms for
  // that lock is dropped as busy; on a starved host such drops count
  // against the same tolerance.
  const int kPerProc = 1000;

  std::vector<pid_t> kids;
  for (int p = 0; p < kProcs; ++p) {
    pid_t pid = fork();
    if (pid == -1) {
      // A bare ASSERT would return from the test body with the children
      // already started still running against the volume, into the next
      // test.  Reap them first, then fail (#321).
      for (pid_t kid : kids) {
        int status = 0;
        waitpid(kid, &status, 0);
      }
      ASSERT_NE(-1, pid) << "fork failed at child " << p << " of " << kProcs;
    }
    if (pid == 0) {
      // CHILD == one Apache worker process.  The same watchdog as the
      // visibility test below: a wedged child must be killed by SIGALRM and
      // named in the parent's failure message, not hang its waitpid() until
      // the shard timeout.  600 s covers the sanitizer configs' 900 s
      // timeouts, 120 s fits the plain build's.
#ifdef CYCLONE_CACHE_TEST_SANITIZERS
      alarm(600);
#else
      alarm(120);
#endif
      CycloneCache child(make_cfg(), stats_.get(), handler_.get());
      if (!child.IsHealthy()) {
        _exit(2);
      }
      CacheInterface* view = child.small_tier_view();
      for (int j = 0; j < kPerProc; ++j) {
        GoogleString key =
            StrCat("mp/", IntegerToString(p), "/", IntegerToString(j));
        SharedString value(GoogleString(200, static_cast<char>('a' + (j % 26))));
        view->Put(key, value);
      }
      child.ShutDown();
      _exit(0);
    }
    kids.push_back(pid);
  }

  int write_failures = 0;
  GoogleString failure_notes;
  for (pid_t pid : kids) {
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      ++write_failures;
      StrAppend(&failure_notes, failure_notes.empty() ? "" : "; ", "pid ",
                IntegerToString(pid), ": ", DescribeWaitStatus(status));
    }
  }
  ASSERT_EQ(0, write_failures)
      << write_failures << " of " << kProcs << " writer processes failed to "
         "open/attach the shared cache (a distinct fault from write loss): "
      << failure_notes;

  // Parent reopens and reads back every key written by every worker.
  CycloneCache reader(make_cfg(), stats_.get(), handler_.get());
  ASSERT_TRUE(reader.IsHealthy());
  CacheInterface* rview = reader.small_tier_view();

  const int kTotal = kProcs * kPerProc;
  int hits = 0;
  for (int p = 0; p < kProcs; ++p) {
    for (int j = 0; j < kPerProc; ++j) {
      GoogleString key =
          StrCat("mp/", IntegerToString(p), "/", IntegerToString(j));
      HitCountCallback cb;
      rview->Get(key, &cb);
      if (cb.found()) {
        ++hits;
      }
    }
  }

  EXPECT_GE(hits, static_cast<int>(0.99 * kTotal))
      << "cross-process small-tier retention " << hits << "/" << kTotal
      << " -- " << kProcs << " workers wrote a shared small volume only ~7 MB "
         "/ 128 MB full (no eviction); loss here is the total_processes=1 "
         "directory-coherence fault (cyclone_wrapper.cc hardcodes "
         "multi_process_config.total_processes = 1 while Apache runs many "
         "worker processes on the same volume)";

  reader.ShutDown();
}

// The sharpest form of the production hypothesis, and the one the two tests
// above cannot reach: writes SUCCEED in one process, yet a LONG-LIVED reader
// in another process never sees them.  The distinguishing detail vs the
// sequential fork test above (whose reader is opened fresh, after the writes):
// here one reader instance issues a GET for every key -- a MISS -- BEFORE
// another process writes that key, and GETs again through the SAME instance
// AFTER.  A stale in-process negative-lookup cache, or an mmap/phase view that
// only refreshes on ring-wrap (and the tiny small tier never wraps), would
// keep serving the primed miss.
//
// It does not: the reader sees the foreign writes.  So read-visibility to a
// long-lived reader holds too, and this explanation of the production
// signature is refuted along with the other two.
//
// ORDERING, and why the pipe handshake is not incidental: the property under
// test is "reader opened and primed a miss BEFORE the writer's Put, re-read
// through the SAME instance AFTER it".  The writer is forked FIRST -- while
// the parent still holds no cache and is therefore single-threaded -- and then
// parked on a pipe until the parent has opened the reader and primed every
// miss.  Forking after the reader exists would fork a multi-threaded parent
// into a child that then spawns Cyclone's threads: POSIX-illegal, and TSan
// kills the child for it.  So the handshake, not program order, is what
// enforces primed-before-write.  Keep it.
TEST_F(CycloneCacheTest, SmallTierCrossProcessReadVisibility) {
  if (!IsHealthyTest()) {
    GTEST_SKIP() << "CycloneCache not available";
  }

  cache_->ShutDown();
  cache_.reset();

  const GoogleString vis_path = StrCat(cache_path_, "_vis");
  auto make_cfg = [&]() {
    CycloneCache::Config config;
    config.cache_path = vis_path;
    // The smallest volume that keeps both tiers active is 256 MiB + 128 B
    // (Cyclone's compute_small_tier_sizes: a 128 MiB kMinStripeSize stripe
    // plus a 64 B volume header, per tier).  320 MiB clears that floor with
    // one 128 MiB small-tier stripe: 65,536 directory slots, about 3%
    // loaded by this test's 2000 keys, and no wrap, so a miss can only mean
    // cross-process invisibility.  A larger volume only adds stripes, and
    // with them the per-stripe create/sync/teardown cost of issue #228.
    config.cache_size_bytes = static_cast<int64>(320) * 1024 * 1024;
    config.ram_cache_size_bytes = 0;
    config.enable_checksum = true;
    config.num_segments = 0;
    config.small_tier_percent = 10;
    return config;
  };

  const int kN = 2000;
  auto key_of = [](int i) { return StrCat("vis/", IntegerToString(i)); };

  // If the writer child dies before we release it, the parent's write() to a
  // reader-less pipe would raise SIGPIPE and kill the test process outright,
  // hiding the child's real exit status.  Take EPIPE instead, and restore the
  // previous disposition on the way out (this is process-global state).
  class ScopedIgnoreSigPipe {
   public:
    ScopedIgnoreSigPipe() : prev_(signal(SIGPIPE, SIG_IGN)) {}
    ~ScopedIgnoreSigPipe() { signal(SIGPIPE, prev_); }

   private:
    void (*prev_)(int);
  } ignore_sigpipe;

  int go_pipe[2];
  ASSERT_EQ(0, pipe(go_pipe)) << "pipe failed";

  // WRITER: forked BEFORE the reader exists, so the parent is single-threaded
  // here.  The child does nothing until the parent releases it.
  pid_t pid = fork();
  ASSERT_NE(-1, pid) << "fork failed";
  if (pid == 0) {
    close(go_pipe[1]);
    char go = 0;
    ssize_t n;
    do {
      n = read(go_pipe[0], &go, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
      // EOF: the parent aborted before priming.  Write nothing -- a late write
      // to a volume nobody is watching would only corrupt the next run.
      _exit(3);
    }
    close(go_pipe[0]);
    // Never let a wedged child hang the parent's waitpid() to the test
    // timeout.  The watchdog only has to tell a wedged writer from a slow
    // one, and under sanitizers on a loaded runner the slow case is real:
    // the issue #228 run spent 18 s creating its volume and 67 s shutting
    // one down, and its writer outlived the 120 s bound.  The volume has
    // since been shrunk (its size was the root cause), so this is now a
    // backstop; the 600 s bound covers the sanitizer configs that raise
    // the Bazel test timeout (clang-asan and clang-tsan set 900 s in
    // .bazelrc; clang-msan keeps the 300 s default).  Plain builds keep
    // the tight bound under their 300 s timeout.
#ifdef CYCLONE_CACHE_TEST_SANITIZERS
    alarm(600);
#else
    alarm(120);
#endif

    CycloneCache writer(make_cfg(), stats_.get(), handler_.get());
    if (!writer.IsHealthy()) {
      _exit(2);
    }
    CacheInterface* wview = writer.small_tier_view();
    for (int i = 0; i < kN; ++i) {
      SharedString value(GoogleString(200, static_cast<char>('a' + (i % 26))));
      wview->Put(key_of(i), value);
    }
    writer.ShutDown();
    _exit(0);
  }
  close(go_pipe[0]);

  // READER: a long-lived instance, opened BEFORE any write lands (like an
  // Apache worker that started before the request that populates the entry).
  // It is also the process that CREATES the volume; the child open-existings.
  CycloneCache reader(make_cfg(), stats_.get(), handler_.get());
  const bool reader_ok = reader.IsHealthy() && reader.small_tier_active();
  CacheInterface* rview = reader.small_tier_view();

  // Prime a MISS for every key through the reader's own view.
  int primed_miss = 0;
  if (reader_ok) {
    for (int i = 0; i < kN; ++i) {
      HitCountCallback cb;
      rview->Get(key_of(i), &cb);
      if (!cb.found()) {
        ++primed_miss;
      }
    }
  }

  // Release the writer, then reap it.  Nothing above this point may fail the
  // test: a fatal assertion would return from the test body with the child
  // still parked on the pipe, orphaning it.  Assertions come after the reap.
  const char go = 1;
  ssize_t written;
  do {
    written = write(go_pipe[1], &go, 1);
  } while (written < 0 && errno == EINTR);
  close(go_pipe[1]);

  int status = 0;
  pid_t reaped;
  do {
    reaped = waitpid(pid, &status, 0);
  } while (reaped < 0 && errno == EINTR);

  ASSERT_TRUE(reader_ok) << "reader must open with an active small tier";
  ASSERT_EQ(kN, primed_miss) << "keys must start absent";
  ASSERT_EQ(1, static_cast<int>(written)) << "failed to release the writer";
  ASSERT_EQ(pid, reaped) << "waitpid failed for the writer";
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
      << "writer process failed to write the keys ("
      << DescribeWaitStatus(status) << ")";

  // The SAME long-lived reader instance now GETs every key again.  The writes
  // succeeded in another process; a coherent shared cache must surface them.
  int hits = 0;
  for (int i = 0; i < kN; ++i) {
    HitCountCallback cb;
    rview->Get(key_of(i), &cb);
    if (cb.found()) {
      ++hits;
    }
  }

  EXPECT_GE(hits, static_cast<int>(0.99 * kN))
      << "long-lived reader saw " << hits << "/" << kN << " keys written by "
         "another process AFTER it primed a miss for each -- reproduces the "
         "production small-tier read-visibility collapse (writes succeed, "
         "reads miss)";

  reader.ShutDown();
}

#endif  // _WIN32

}  // namespace net_instaweb
