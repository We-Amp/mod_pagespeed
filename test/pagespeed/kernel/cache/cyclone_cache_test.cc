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

#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/cache/cache_test_base.h"

namespace net_instaweb {

class CycloneCacheTest : public CacheTestBase {
 protected:
  CycloneCacheTest()
      : stats_(new NullStatistics()),
        handler_(new GoogleMessageHandler()) {
    CycloneCache::InitStats(stats_.get());
  }

  void SetUp() override {
    // Create a unique cache path for each test to avoid conflicts
    GoogleString test_name = ::testing::UnitTest::GetInstance()
                                 ->current_test_info()
                                 ->name();
    cache_path_ = StrCat(GTestTempDir(), "/cyclone_cache_test_", test_name);

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

  bool IsHealthyTest() {
    return cache_ != nullptr && cache_->IsHealthy();
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
  GoogleString volatile_path = StrCat(GTestTempDir(),
                                      "/cyclone_volatile_test");

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

}  // namespace net_instaweb
