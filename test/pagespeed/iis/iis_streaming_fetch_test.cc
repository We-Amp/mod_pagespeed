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

// Exhaustive unit tests for IisStreamingFetch class.
//
// IisStreamingFetch supports incremental HTML flushing using HTTP_DATA_CHUNK
// structures. It accumulates output in blocks (4KB min, 64KB max) and signals
// when flush is requested via condition variable, allowing the IIS thread to
// write chunks incrementally.
//
// Test Categories:
// 1. Basic Operations - empty writes, small/large writes, sequential writes
// 2. Chunk Management - creation, sizing, taking chunks
// 3. Flush Behavior - HandleFlush, flush with empty buffer, disabled mode
// 4. Completion - HandleDone, success/failure status, flushing remaining data
// 5. Thread Safety - concurrent writes, write during flush, blocking behavior
// 6. Statistics - bytes_written, bytes_flushed tracking
// 7. Edge Cases - zero-length content, very large content, rapid flush cycles

#include "pagespeed/iis/iis_streaming_fetch.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/opt/http/request_context.h"

namespace net_instaweb {

namespace {

// Helper to create a test request context
RequestContextPtr CreateTestRequestContext() {
  return RequestContextPtr(new RequestContext(
      new NullMutex(), nullptr /* timer */));
}

// Helper to generate test data of a specific size
GoogleString GenerateTestData(size_t size, char fill = 'X') {
  return GoogleString(size, fill);
}

// Helper to extract all data from a vector of chunks
GoogleString ExtractChunkData(const std::vector<HTTP_DATA_CHUNK*>& chunks) {
  GoogleString result;
  for (const HTTP_DATA_CHUNK* chunk : chunks) {
    if (chunk != nullptr && chunk->FromMemory.pBuffer != nullptr) {
      result.append(static_cast<const char*>(chunk->FromMemory.pBuffer),
                    chunk->FromMemory.BufferLength);
    }
  }
  return result;
}

// Helper to free chunks after use
void FreeChunks(std::vector<HTTP_DATA_CHUNK*>& chunks) {
  for (HTTP_DATA_CHUNK* chunk : chunks) {
    if (chunk != nullptr) {
      delete[] static_cast<char*>(chunk->FromMemory.pBuffer);
      delete chunk;
    }
  }
  chunks.clear();
}

// Helper to calculate total chunk size
size_t TotalChunkSize(const std::vector<HTTP_DATA_CHUNK*>& chunks) {
  size_t total = 0;
  for (const HTTP_DATA_CHUNK* chunk : chunks) {
    if (chunk != nullptr) {
      total += chunk->FromMemory.BufferLength;
    }
  }
  return total;
}

}  // namespace

// ============================================================================
// Test Fixture
// ============================================================================

class IisStreamingFetchTest : public testing::Test {
 protected:
  void SetUp() override {
    request_ctx_ = CreateTestRequestContext();
  }

  void TearDown() override {
    request_ctx_.clear();
  }

  // Create a streaming fetch with flush enabled (default)
  std::unique_ptr<IisStreamingFetch> CreateFetch(bool flush_enabled = true) {
    return std::make_unique<IisStreamingFetch>(request_ctx_, flush_enabled);
  }

  // Create a streaming fetch with custom memory allocator
  std::unique_ptr<IisStreamingFetch> CreateFetchWithAllocator(
      bool flush_enabled,
      IisStreamingFetch::MemoryAllocator allocator) {
    return std::make_unique<IisStreamingFetch>(
        request_ctx_, flush_enabled, nullptr, nullptr, allocator);
  }

  RequestContextPtr request_ctx_;
};

// ============================================================================
// 1. Basic Operations Tests
// ============================================================================

// Empty writes should succeed and not create chunks
TEST_F(IisStreamingFetchTest, EmptyWriteHandling) {
  auto fetch = CreateFetch();

  // Empty write should succeed
  EXPECT_TRUE(fetch->HandleWrite("", nullptr));

  // No bytes should be written
  EXPECT_EQ(0u, fetch->bytes_written());

  // No pending chunks
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Flush should work even with no data
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_FALSE(fetch->HasPendingChunks());
}

// Small write accumulates in current block
TEST_F(IisStreamingFetchTest, SingleSmallWrite) {
  auto fetch = CreateFetch();
  const GoogleString small_data = "Hello, World!";

  EXPECT_TRUE(fetch->HandleWrite(small_data, nullptr));

  // Bytes written should match
  EXPECT_EQ(small_data.size(), fetch->bytes_written());

  // No chunks yet (data is in accumulation buffer)
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Flush creates a chunk
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_TRUE(fetch->HasPendingChunks());

  // Take and verify chunks
  auto chunks = fetch->TakePendingChunks();
  EXPECT_EQ(1u, chunks.size());
  EXPECT_EQ(small_data, ExtractChunkData(chunks));

  FreeChunks(chunks);
}

// Write larger than kMaxBlockSize creates multiple chunks
TEST_F(IisStreamingFetchTest, SingleLargeWrite) {
  auto fetch = CreateFetch();

  // Write data larger than kMaxBlockSize (64KB)
  const size_t large_size = IisStreamingFetch::kMaxBlockSize * 2 + 1024;
  GoogleString large_data = GenerateTestData(large_size);

  EXPECT_TRUE(fetch->HandleWrite(large_data, nullptr));
  EXPECT_EQ(large_size, fetch->bytes_written());

  // Flush to finalize
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks = fetch->TakePendingChunks();

  // Should have multiple chunks due to size limits
  EXPECT_GE(chunks.size(), 2u);

  // Total data should match
  EXPECT_EQ(large_data, ExtractChunkData(chunks));

  // Each chunk should not exceed kMaxBlockSize
  for (const HTTP_DATA_CHUNK* chunk : chunks) {
    EXPECT_LE(chunk->FromMemory.BufferLength, IisStreamingFetch::kMaxBlockSize);
  }

  FreeChunks(chunks);
}

// Multiple writes accumulate until flush
TEST_F(IisStreamingFetchTest, MultipleSequentialWrites) {
  auto fetch = CreateFetch();

  GoogleString data1 = "First part. ";
  GoogleString data2 = "Second part. ";
  GoogleString data3 = "Third part.";

  EXPECT_TRUE(fetch->HandleWrite(data1, nullptr));
  EXPECT_TRUE(fetch->HandleWrite(data2, nullptr));
  EXPECT_TRUE(fetch->HandleWrite(data3, nullptr));

  EXPECT_EQ(data1.size() + data2.size() + data3.size(), fetch->bytes_written());

  // No chunks yet without flush
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Flush creates chunk(s) with all accumulated data
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks = fetch->TakePendingChunks();
  GoogleString expected = data1 + data2 + data3;
  EXPECT_EQ(expected, ExtractChunkData(chunks));

  FreeChunks(chunks);
}

// ============================================================================
// 2. Chunk Management Tests
// ============================================================================

// Flush finalizes current block into chunk
TEST_F(IisStreamingFetchTest, ChunkCreationOnFlush) {
  auto fetch = CreateFetch();

  GoogleString data = "Test data for chunk creation";
  EXPECT_TRUE(fetch->HandleWrite(data, nullptr));

  // Before flush: no pending chunks
  EXPECT_FALSE(fetch->HasPendingChunks());

  // After flush: chunk created
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_TRUE(fetch->HasPendingChunks());

  auto chunks = fetch->TakePendingChunks();
  EXPECT_EQ(1u, chunks.size());
  EXPECT_EQ(data.size(), chunks[0]->FromMemory.BufferLength);

  // Verify chunk type
  EXPECT_EQ(HttpDataChunkFromMemory, chunks[0]->DataChunkType);

  FreeChunks(chunks);
}

// Blocks are at least kMinBlockSize (4KB) when allocated
TEST_F(IisStreamingFetchTest, ChunkSizingMinimum) {
  auto fetch = CreateFetch();

  // Write small amount of data
  GoogleString small_data = "tiny";
  EXPECT_TRUE(fetch->HandleWrite(small_data, nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks = fetch->TakePendingChunks();
  EXPECT_EQ(1u, chunks.size());

  // The chunk contains only the written data (not padded to block size)
  EXPECT_EQ(small_data.size(), chunks[0]->FromMemory.BufferLength);

  FreeChunks(chunks);
}

// Blocks don't exceed kMaxBlockSize (64KB)
TEST_F(IisStreamingFetchTest, ChunkSizingMaximum) {
  auto fetch = CreateFetch();

  // Write exactly kMaxBlockSize bytes
  GoogleString max_data = GenerateTestData(IisStreamingFetch::kMaxBlockSize);
  EXPECT_TRUE(fetch->HandleWrite(max_data, nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks = fetch->TakePendingChunks();

  // All chunks should be <= kMaxBlockSize
  for (const HTTP_DATA_CHUNK* chunk : chunks) {
    EXPECT_LE(chunk->FromMemory.BufferLength, IisStreamingFetch::kMaxBlockSize);
  }

  // Total should match
  EXPECT_EQ(IisStreamingFetch::kMaxBlockSize, TotalChunkSize(chunks));

  FreeChunks(chunks);
}

// TakePendingChunks returns and clears the queue
TEST_F(IisStreamingFetchTest, TakePendingChunksClearsQueue) {
  auto fetch = CreateFetch();

  GoogleString data = "Test data";
  EXPECT_TRUE(fetch->HandleWrite(data, nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  EXPECT_TRUE(fetch->HasPendingChunks());

  auto chunks1 = fetch->TakePendingChunks();
  EXPECT_EQ(1u, chunks1.size());

  // Queue should be cleared
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Second call returns empty
  auto chunks2 = fetch->TakePendingChunks();
  EXPECT_TRUE(chunks2.empty());

  FreeChunks(chunks1);
}

// ============================================================================
// 3. Flush Behavior Tests
// ============================================================================

// HandleFlush finalizes block from current data
TEST_F(IisStreamingFetchTest, HandleFlushFinalizesBlock) {
  auto fetch = CreateFetch();

  GoogleString data = "Data before flush";
  EXPECT_TRUE(fetch->HandleWrite(data, nullptr));

  // bytes_flushed should be 0 before TakePendingChunks
  EXPECT_EQ(0u, fetch->bytes_flushed());

  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks = fetch->TakePendingChunks();

  // bytes_flushed should be updated after TakePendingChunks
  EXPECT_EQ(data.size(), fetch->bytes_flushed());

  FreeChunks(chunks);
}

// HandleFlush signals condition variable (tested via WaitForFlushOrDone)
TEST_F(IisStreamingFetchTest, HandleFlushSignalsCV) {
  auto fetch = CreateFetch();

  std::atomic<bool> wait_started{false};
  std::atomic<bool> wait_returned{false};
  bool flush_result = false;

  // Start a thread that waits for flush
  std::thread waiter([&]() {
    wait_started = true;
    flush_result = fetch->WaitForFlushOrDone();
    wait_returned = true;
  });

  // Wait for waiter thread to start waiting
  while (!wait_started) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Waiter should be blocked
  EXPECT_FALSE(wait_returned.load());

  // Write and flush
  fetch->HandleWrite("data", nullptr);
  fetch->HandleFlush(nullptr);

  // Wait for waiter to return
  waiter.join();

  // WaitForFlushOrDone should return true for flush
  EXPECT_TRUE(flush_result);
  EXPECT_TRUE(wait_returned.load());
}

// Multiple flushes work correctly
TEST_F(IisStreamingFetchTest, MultipleFlushesInSequence) {
  auto fetch = CreateFetch();

  // First write + flush
  GoogleString data1 = "First batch";
  EXPECT_TRUE(fetch->HandleWrite(data1, nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks1 = fetch->TakePendingChunks();
  EXPECT_EQ(1u, chunks1.size());
  EXPECT_EQ(data1, ExtractChunkData(chunks1));

  // Second write + flush
  GoogleString data2 = "Second batch";
  EXPECT_TRUE(fetch->HandleWrite(data2, nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks2 = fetch->TakePendingChunks();
  EXPECT_EQ(1u, chunks2.size());
  EXPECT_EQ(data2, ExtractChunkData(chunks2));

  // Third write + flush
  GoogleString data3 = "Third batch";
  EXPECT_TRUE(fetch->HandleWrite(data3, nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  auto chunks3 = fetch->TakePendingChunks();
  EXPECT_EQ(1u, chunks3.size());
  EXPECT_EQ(data3, ExtractChunkData(chunks3));

  EXPECT_EQ(data1.size() + data2.size() + data3.size(), fetch->bytes_written());
  EXPECT_EQ(data1.size() + data2.size() + data3.size(), fetch->bytes_flushed());

  FreeChunks(chunks1);
  FreeChunks(chunks2);
  FreeChunks(chunks3);
}

// Flush with empty buffer is no-op
TEST_F(IisStreamingFetchTest, FlushWithEmptyBuffer) {
  auto fetch = CreateFetch();

  // Flush without any writes
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_FALSE(fetch->HasPendingChunks());
  EXPECT_EQ(0u, fetch->bytes_written());
  EXPECT_EQ(0u, fetch->bytes_flushed());

  // Multiple empty flushes should be fine
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_FALSE(fetch->HasPendingChunks());
}

// Flush is no-op when flush_enabled=false
TEST_F(IisStreamingFetchTest, FlushDisabledMode) {
  auto fetch = CreateFetch(false /* flush_enabled */);

  GoogleString data = "Data with flush disabled";
  EXPECT_TRUE(fetch->HandleWrite(data, nullptr));

  // Flush should succeed but not create chunks
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Data is still in the buffer, will be flushed on Done
  EXPECT_EQ(data.size(), fetch->bytes_written());
  EXPECT_EQ(0u, fetch->bytes_flushed());

  // Done finalizes everything
  fetch->HandleDone(true);

  auto chunks = fetch->TakePendingChunks();
  EXPECT_EQ(data, ExtractChunkData(chunks));

  FreeChunks(chunks);
}

// ============================================================================
// 4. Completion Tests
// ============================================================================

// HandleDone sets flags and signals
TEST_F(IisStreamingFetchTest, HandleDoneSignalsCompletion) {
  auto fetch = CreateFetch();

  EXPECT_FALSE(fetch->IsDone());

  fetch->HandleDone(true);

  EXPECT_TRUE(fetch->IsDone());
}

// Success/failure status is preserved
TEST_F(IisStreamingFetchTest, SuccessStatusPropagation) {
  // Test success case
  {
    auto fetch = CreateFetch();
    fetch->HandleDone(true);
    EXPECT_TRUE(fetch->success());
  }

  // Test failure case
  {
    auto fetch = CreateFetch();
    fetch->HandleDone(false);
    EXPECT_FALSE(fetch->success());
  }
}

// HandleDone finalizes any pending data
TEST_F(IisStreamingFetchTest, DoneFlushesRemainingData) {
  auto fetch = CreateFetch();

  GoogleString data = "Data written before done";
  EXPECT_TRUE(fetch->HandleWrite(data, nullptr));

  // No flush called, but Done should finalize
  EXPECT_FALSE(fetch->HasPendingChunks());

  fetch->HandleDone(true);

  // Data should now be available
  auto chunks = fetch->TakePendingChunks();
  EXPECT_EQ(data, ExtractChunkData(chunks));

  FreeChunks(chunks);
}

// ============================================================================
// 5. Thread Safety Tests
// ============================================================================

// Concurrent writes from multiple threads
TEST_F(IisStreamingFetchTest, ConcurrentWritesFromMultipleThreads) {
  auto fetch = CreateFetch();

  const int kNumThreads = 4;
  const int kWritesPerThread = 100;
  const GoogleString kThreadData = "Thread data block. ";

  std::vector<std::thread> threads;
  std::atomic<int> successful_writes{0};

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < kWritesPerThread; ++j) {
        if (fetch->HandleWrite(kThreadData, nullptr)) {
          ++successful_writes;
        }
      }
    });
  }

  // Wait for all writers to finish
  for (auto& t : threads) {
    t.join();
  }

  // All writes should succeed
  EXPECT_EQ(kNumThreads * kWritesPerThread, successful_writes.load());

  // Total bytes should match
  EXPECT_EQ(kNumThreads * kWritesPerThread * kThreadData.size(),
            fetch->bytes_written());

  // Finalize and verify
  fetch->HandleDone(true);

  auto chunks = fetch->TakePendingChunks();
  size_t total_size = TotalChunkSize(chunks);
  EXPECT_EQ(kNumThreads * kWritesPerThread * kThreadData.size(), total_size);

  FreeChunks(chunks);
}

// Write and flush concurrently
TEST_F(IisStreamingFetchTest, WriteAndFlushConcurrent) {
  auto fetch = CreateFetch();

  const int kIterations = 50;
  std::atomic<bool> stop{false};
  std::atomic<int> flush_count{0};

  // Writer thread
  std::thread writer([&]() {
    for (int i = 0; i < kIterations && !stop; ++i) {
      fetch->HandleWrite("data", nullptr);
      std::this_thread::yield();
    }
  });

  // Flusher thread
  std::thread flusher([&]() {
    for (int i = 0; i < kIterations && !stop; ++i) {
      fetch->HandleFlush(nullptr);
      ++flush_count;
      std::this_thread::yield();
    }
  });

  writer.join();
  flusher.join();

  // Finalize
  fetch->HandleDone(true);

  // Should not crash and data should be intact
  auto chunks = fetch->TakePendingChunks();
  size_t total_size = TotalChunkSize(chunks);

  // At least some data should have been written
  EXPECT_GT(total_size, 0u);

  FreeChunks(chunks);
}

// WaitForFlushOrDone blocks until signaled
TEST_F(IisStreamingFetchTest, WaitForFlushOrDoneBlocks) {
  auto fetch = CreateFetch();

  std::atomic<bool> wait_started{false};
  std::atomic<bool> wait_finished{false};

  std::thread waiter([&]() {
    wait_started = true;
    fetch->WaitForFlushOrDone();
    wait_finished = true;
  });

  // Wait for waiter to start
  while (!wait_started) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Give some time to ensure waiter is blocked
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Waiter should still be blocked
  EXPECT_FALSE(wait_finished.load());

  // Signal completion
  fetch->HandleDone(true);

  waiter.join();
  EXPECT_TRUE(wait_finished.load());
}

// Wait returns true on flush
TEST_F(IisStreamingFetchTest, WaitReturnsOnFlush) {
  auto fetch = CreateFetch();

  std::atomic<bool> wait_started{false};
  std::atomic<bool> result{false};

  std::thread waiter([&]() {
    wait_started = true;
    result = fetch->WaitForFlushOrDone();
  });

  while (!wait_started) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // Trigger flush
  fetch->HandleFlush(nullptr);

  waiter.join();

  // Should return true for flush (not done)
  EXPECT_TRUE(result.load());
  EXPECT_FALSE(fetch->IsDone());
}

// Wait returns false on done
TEST_F(IisStreamingFetchTest, WaitReturnsFalseOnDone) {
  auto fetch = CreateFetch();

  std::atomic<bool> wait_started{false};
  std::atomic<bool> result{true};  // Initialize to true to verify it changes

  std::thread waiter([&]() {
    wait_started = true;
    result = fetch->WaitForFlushOrDone();
  });

  while (!wait_started) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // Trigger done
  fetch->HandleDone(true);

  waiter.join();

  // Should return false for done
  EXPECT_FALSE(result.load());
  EXPECT_TRUE(fetch->IsDone());
}

// ============================================================================
// 6. Statistics Tests
// ============================================================================

// bytes_written tracks correctly
TEST_F(IisStreamingFetchTest, BytesWrittenTracked) {
  auto fetch = CreateFetch();

  EXPECT_EQ(0u, fetch->bytes_written());

  GoogleString data1 = "First";
  EXPECT_TRUE(fetch->HandleWrite(data1, nullptr));
  EXPECT_EQ(data1.size(), fetch->bytes_written());

  GoogleString data2 = "Second";
  EXPECT_TRUE(fetch->HandleWrite(data2, nullptr));
  EXPECT_EQ(data1.size() + data2.size(), fetch->bytes_written());

  GoogleString data3 = "Third";
  EXPECT_TRUE(fetch->HandleWrite(data3, nullptr));
  EXPECT_EQ(data1.size() + data2.size() + data3.size(), fetch->bytes_written());

  // Flush doesn't change bytes_written
  fetch->HandleFlush(nullptr);
  EXPECT_EQ(data1.size() + data2.size() + data3.size(), fetch->bytes_written());
}

// bytes_flushed counts after TakePendingChunks
TEST_F(IisStreamingFetchTest, BytesFlushedTracked) {
  auto fetch = CreateFetch();

  EXPECT_EQ(0u, fetch->bytes_flushed());

  GoogleString data1 = "First batch";
  fetch->HandleWrite(data1, nullptr);
  fetch->HandleFlush(nullptr);

  // bytes_flushed is 0 until TakePendingChunks
  EXPECT_EQ(0u, fetch->bytes_flushed());

  auto chunks1 = fetch->TakePendingChunks();
  EXPECT_EQ(data1.size(), fetch->bytes_flushed());

  GoogleString data2 = "Second batch";
  fetch->HandleWrite(data2, nullptr);
  fetch->HandleFlush(nullptr);

  auto chunks2 = fetch->TakePendingChunks();
  EXPECT_EQ(data1.size() + data2.size(), fetch->bytes_flushed());

  FreeChunks(chunks1);
  FreeChunks(chunks2);
}

// ============================================================================
// 7. Edge Cases Tests
// ============================================================================

// Zero-length writes are handled
TEST_F(IisStreamingFetchTest, ZeroLengthContent) {
  auto fetch = CreateFetch();

  // Multiple zero-length writes
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(fetch->HandleWrite("", nullptr));
    EXPECT_TRUE(fetch->HandleWrite(StringPiece(), nullptr));
  }

  EXPECT_EQ(0u, fetch->bytes_written());

  // Flush should work
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Done should work
  fetch->HandleDone(true);
  EXPECT_TRUE(fetch->IsDone());
}

// Very large content (10MB+) works
TEST_F(IisStreamingFetchTest, VeryLargeContent) {
  auto fetch = CreateFetch();

  const size_t kLargeSize = 10 * 1024 * 1024;  // 10MB
  GoogleString large_data = GenerateTestData(kLargeSize);

  EXPECT_TRUE(fetch->HandleWrite(large_data, nullptr));
  EXPECT_EQ(kLargeSize, fetch->bytes_written());

  fetch->HandleDone(true);

  auto chunks = fetch->TakePendingChunks();

  // Should have many chunks
  EXPECT_GT(chunks.size(), 100u);

  // Total should match
  EXPECT_EQ(kLargeSize, TotalChunkSize(chunks));

  // All chunks should respect size limits
  for (const HTTP_DATA_CHUNK* chunk : chunks) {
    EXPECT_LE(chunk->FromMemory.BufferLength, IisStreamingFetch::kMaxBlockSize);
  }

  FreeChunks(chunks);
}

// Many rapid flush cycles work
TEST_F(IisStreamingFetchTest, RapidFlushCycles) {
  auto fetch = CreateFetch();

  const int kCycles = 100;
  size_t total_written = 0;

  for (int i = 0; i < kCycles; ++i) {
    GoogleString data = "Cycle " + IntegerToString(i) + " data. ";
    EXPECT_TRUE(fetch->HandleWrite(data, nullptr));
    EXPECT_TRUE(fetch->HandleFlush(nullptr));
    total_written += data.size();

    auto chunks = fetch->TakePendingChunks();
    // Should have chunk for each cycle
    EXPECT_FALSE(chunks.empty());
    FreeChunks(chunks);
  }

  EXPECT_EQ(total_written, fetch->bytes_written());
  EXPECT_EQ(total_written, fetch->bytes_flushed());
}

// headers_complete flag is set correctly
TEST_F(IisStreamingFetchTest, HeadersCompleteFlag) {
  auto fetch = CreateFetch();

  // Initially false
  EXPECT_FALSE(fetch->headers_complete());

  // After HandleHeadersComplete
  fetch->HandleHeadersComplete();
  EXPECT_TRUE(fetch->headers_complete());

  // Stays true
  fetch->HandleWrite("data", nullptr);
  EXPECT_TRUE(fetch->headers_complete());

  fetch->HandleDone(true);
  EXPECT_TRUE(fetch->headers_complete());
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

// Binary data with null bytes is handled correctly
TEST_F(IisStreamingFetchTest, BinaryDataWithNullBytes) {
  auto fetch = CreateFetch();

  GoogleString binary_data;
  binary_data.push_back('\x00');
  binary_data.push_back('\x01');
  binary_data.push_back('\x00');
  binary_data.append("text");
  binary_data.push_back('\x00');
  binary_data.push_back('\xFF');

  EXPECT_TRUE(fetch->HandleWrite(binary_data, nullptr));
  fetch->HandleFlush(nullptr);

  auto chunks = fetch->TakePendingChunks();
  EXPECT_EQ(binary_data, ExtractChunkData(chunks));
  EXPECT_EQ(binary_data.size(), TotalChunkSize(chunks));

  FreeChunks(chunks);
}

// Data exactly at block boundaries
TEST_F(IisStreamingFetchTest, DataAtBlockBoundaries) {
  auto fetch = CreateFetch();

  // Write exactly kDefaultBlockSize (8KB)
  GoogleString default_block = GenerateTestData(
      IisStreamingFetch::kDefaultBlockSize, 'A');
  EXPECT_TRUE(fetch->HandleWrite(default_block, nullptr));

  // The block should be full and finalized automatically
  fetch->HandleFlush(nullptr);
  auto chunks1 = fetch->TakePendingChunks();
  EXPECT_EQ(IisStreamingFetch::kDefaultBlockSize, TotalChunkSize(chunks1));
  FreeChunks(chunks1);

  // Write exactly kMinBlockSize (4KB)
  GoogleString min_block = GenerateTestData(
      IisStreamingFetch::kMinBlockSize, 'B');
  EXPECT_TRUE(fetch->HandleWrite(min_block, nullptr));
  fetch->HandleFlush(nullptr);
  auto chunks2 = fetch->TakePendingChunks();
  EXPECT_EQ(IisStreamingFetch::kMinBlockSize, TotalChunkSize(chunks2));
  FreeChunks(chunks2);

  // Write exactly kMaxBlockSize (64KB)
  GoogleString max_block = GenerateTestData(
      IisStreamingFetch::kMaxBlockSize, 'C');
  EXPECT_TRUE(fetch->HandleWrite(max_block, nullptr));
  fetch->HandleFlush(nullptr);
  auto chunks3 = fetch->TakePendingChunks();
  EXPECT_EQ(IisStreamingFetch::kMaxBlockSize, TotalChunkSize(chunks3));
  FreeChunks(chunks3);
}

// Interleaved headers, writes, flushes, done
TEST_F(IisStreamingFetchTest, InterleavedOperations) {
  auto fetch = CreateFetch();

  // Headers first
  fetch->HandleHeadersComplete();
  EXPECT_TRUE(fetch->headers_complete());

  // Write some data
  fetch->HandleWrite("Part 1", nullptr);

  // Flush
  fetch->HandleFlush(nullptr);
  auto chunks1 = fetch->TakePendingChunks();
  EXPECT_EQ("Part 1", ExtractChunkData(chunks1));
  FreeChunks(chunks1);

  // More writes
  fetch->HandleWrite("Part 2", nullptr);
  fetch->HandleWrite("Part 3", nullptr);

  // Flush
  fetch->HandleFlush(nullptr);
  auto chunks2 = fetch->TakePendingChunks();
  EXPECT_EQ("Part 2Part 3", ExtractChunkData(chunks2));
  FreeChunks(chunks2);

  // Final write without explicit flush
  fetch->HandleWrite("Part 4", nullptr);

  // Done should finalize
  fetch->HandleDone(true);
  auto chunks3 = fetch->TakePendingChunks();
  EXPECT_EQ("Part 4", ExtractChunkData(chunks3));
  FreeChunks(chunks3);

  EXPECT_TRUE(fetch->IsDone());
  EXPECT_TRUE(fetch->success());
}

// Custom memory allocator is used
TEST_F(IisStreamingFetchTest, CustomMemoryAllocator) {
  int allocation_count = 0;

  auto allocator = [&](size_t size) -> void* {
    ++allocation_count;
    return new char[size];
  };

  auto fetch = CreateFetchWithAllocator(true, allocator);

  // Write data that requires allocation
  fetch->HandleWrite("Test data", nullptr);

  // Should have used our allocator
  EXPECT_GT(allocation_count, 0);

  fetch->HandleDone(true);
}

// Multiple wait cycles with flush/take/write pattern
TEST_F(IisStreamingFetchTest, MultipleWaitCycles) {
  auto fetch = CreateFetch();

  const int kCycles = 5;

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    std::atomic<bool> flush_received{false};

    // Consumer thread
    std::thread consumer([&]() {
      bool result = fetch->WaitForFlushOrDone();
      flush_received = true;
      if (result) {
        auto chunks = fetch->TakePendingChunks();
        FreeChunks(chunks);
      }
    });

    // Give consumer time to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Producer writes and flushes
    GoogleString data = "Cycle " + IntegerToString(cycle);
    fetch->HandleWrite(data, nullptr);
    fetch->HandleFlush(nullptr);

    consumer.join();
    EXPECT_TRUE(flush_received.load());
  }

  // Final done
  fetch->HandleDone(true);
  EXPECT_TRUE(fetch->IsDone());
}

// Done after multiple flushes without taking chunks
TEST_F(IisStreamingFetchTest, DoneWithUntakenChunks) {
  auto fetch = CreateFetch();

  // Write and flush multiple times without taking
  for (int i = 0; i < 5; ++i) {
    fetch->HandleWrite("data", nullptr);
    fetch->HandleFlush(nullptr);
  }

  // Final write without flush
  fetch->HandleWrite("final", nullptr);

  // Done should finalize everything
  fetch->HandleDone(true);

  // Should have all chunks
  auto chunks = fetch->TakePendingChunks();
  EXPECT_EQ(6u, chunks.size());  // 5 flushed + 1 finalized on done

  FreeChunks(chunks);
}

// IsDone is initially false
TEST_F(IisStreamingFetchTest, IsDoneInitialState) {
  auto fetch = CreateFetch();
  EXPECT_FALSE(fetch->IsDone());
}

// HasPendingChunks reflects actual state
TEST_F(IisStreamingFetchTest, HasPendingChunksReflectsState) {
  auto fetch = CreateFetch();

  // Initially no chunks
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Write without flush - still no pending chunks
  fetch->HandleWrite("data", nullptr);
  EXPECT_FALSE(fetch->HasPendingChunks());

  // After flush - has pending chunks
  fetch->HandleFlush(nullptr);
  EXPECT_TRUE(fetch->HasPendingChunks());

  // After take - no more pending chunks
  auto chunks = fetch->TakePendingChunks();
  EXPECT_FALSE(fetch->HasPendingChunks());

  FreeChunks(chunks);
}

}  // namespace net_instaweb
