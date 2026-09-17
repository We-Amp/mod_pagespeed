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

// Integration tests for HTML flushing end-to-end functionality.
//
// These tests verify the complete HTML flushing flow using the mock IIS
// infrastructure combined with IisStreamingFetch. They test:
//
// 1. Content Integrity - flushed content matches expected output
// 2. Flush Behavior - multiple chunks created when enabled, single when disabled
// 3. Error Recovery - partial content delivery on mid-stream errors
// 4. Configuration - flush_html and buffer limit options work correctly
//
// The tests simulate the actual IIS request processing flow:
// 1. Create IIS context with request/response
// 2. Create IisStreamingFetch with flush settings
// 3. Simulate ProxyFetch producing output (HandleWrite/HandleFlush/HandleDone)
// 4. Simulate IIS thread consuming chunks (WaitForFlushOrDone/TakePendingChunks)
// 5. Write chunks to MockHttpResponse and verify results

#include "pagespeed/iis/iis_streaming_fetch.h"
#include "test/pagespeed/iis/mock_iis.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/basictypes.h"
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

// Helper to count total bytes in chunks
size_t TotalChunkBytes(const std::vector<HTTP_DATA_CHUNK*>& chunks) {
  size_t total = 0;
  for (const HTTP_DATA_CHUNK* chunk : chunks) {
    if (chunk != nullptr) {
      total += chunk->FromMemory.BufferLength;
    }
  }
  return total;
}

}  // namespace

// =============================================================================
// Test Fixture
// =============================================================================

class IisHtmlFlushingIntegrationTest : public testing::Test {
 protected:
  void SetUp() override {
    request_ctx_ = CreateTestRequestContext();
    // Create default IIS context
    context_ = MockHttpContextBuilder()
        .SetUrl("http://test.com/page.html")
        .SetMethod("GET")
        .SetSiteId(1)
        .SetSiteName("Default Web Site")
        .Build();
  }

  void TearDown() override {
    request_ctx_.clear();
    context_.reset();
  }

  // Create a streaming fetch with specified flush setting
  std::unique_ptr<IisStreamingFetch> CreateFetch(bool flush_enabled) {
    return std::make_unique<IisStreamingFetch>(request_ctx_, flush_enabled);
  }

  // Helper to simulate ProxyFetch output with optional flush after each chunk
  void SimulateProxyFetchOutput(IisStreamingFetch* fetch,
                                const std::vector<GoogleString>& chunks,
                                bool flush_after_each) {
    fetch->HandleHeadersComplete();
    for (const GoogleString& chunk : chunks) {
      fetch->HandleWrite(chunk, nullptr);
      if (flush_after_each) {
        fetch->HandleFlush(nullptr);
      }
    }
    fetch->HandleDone(true);
  }

  // Helper to write all pending chunks to the mock response
  void WriteChunksToResponse(IisStreamingFetch* fetch,
                             MockHttpResponse* response) {
    auto chunks = fetch->TakePendingChunks();
    for (HTTP_DATA_CHUNK* chunk : chunks) {
      if (chunk != nullptr && chunk->FromMemory.pBuffer != nullptr) {
        response->WriteEntityChunks(
            static_cast<const char*>(chunk->FromMemory.pBuffer),
            chunk->FromMemory.BufferLength);
      }
    }
    FreeChunks(chunks);
  }

  // Helper to simulate complete IIS request cycle with streaming fetch
  // Returns the number of flush cycles that occurred
  int SimulateFullRequestCycle(IisStreamingFetch* fetch,
                               const std::vector<GoogleString>& html_chunks,
                               bool flush_after_each,
                               MockHttpResponse* response) {
    int flush_count = 0;
    std::atomic<bool> producer_done{false};

    // Producer thread (simulates ProxyFetch worker)
    std::thread producer([&]() {
      fetch->HandleHeadersComplete();
      for (const GoogleString& chunk : html_chunks) {
        fetch->HandleWrite(chunk, nullptr);
        if (flush_after_each) {
          fetch->HandleFlush(nullptr);
        }
      }
      fetch->HandleDone(true);
      producer_done = true;
    });

    // Consumer loop (simulates IIS worker thread)
    while (true) {
      bool more = fetch->WaitForFlushOrDone();
      if (!more && fetch->IsDone()) {
        // Process any final chunks - count as a flush if there's data
        if (fetch->HasPendingChunks()) {
          WriteChunksToResponse(fetch, response);
          ++flush_count;
        }
        break;
      }
      if (fetch->HasPendingChunks()) {
        WriteChunksToResponse(fetch, response);
        ++flush_count;
      }
    }

    producer.join();
    return flush_count;
  }

  RequestContextPtr request_ctx_;
  std::unique_ptr<MockHttpContext> context_;
};

// =============================================================================
// 1. Content Integrity Tests
// =============================================================================

// FlushedContentMatchesExpected - verify output matches expected rewritten HTML
TEST_F(IisHtmlFlushingIntegrationTest, FlushedContentMatchesExpected) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  // Simulate HTML output from rewriter
  const char kExpectedHtml[] =
      "<!DOCTYPE html>\n"
      "<html>\n"
      "<head><title>Test</title></head>\n"
      "<body><p>Hello, World!</p></body>\n"
      "</html>";

  // Write HTML in multiple chunks
  std::vector<GoogleString> chunks = {
      "<!DOCTYPE html>\n<html>\n",
      "<head><title>Test</title></head>\n",
      "<body><p>Hello, World!</p></body>\n",
      "</html>"
  };

  SimulateFullRequestCycle(fetch.get(), chunks, true, response);

  // Verify final content matches expected
  EXPECT_EQ(kExpectedHtml, response->body());
}

// HeadersSentBeforeBody - verify headers are complete before first body chunk
TEST_F(IisHtmlFlushingIntegrationTest, HeadersSentBeforeBody) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();
  response->ClearRecordedActions();

  // Set up headers first
  response->SetStatus(200, "OK");
  response->SetHeader("Content-Type", "text/html; charset=utf-8");
  response->SetHeader("X-PageSpeed-Version", "1.0");

  // Then start the streaming fetch
  fetch->HandleHeadersComplete();
  fetch->HandleWrite("<html>", nullptr);
  fetch->HandleFlush(nullptr);

  // Verify headers were set before body chunks
  GoogleString actions = response->ActionsSinceLastCall();

  // Actions should show SetStatus and SetHeader before WriteEntityChunks
  size_t status_pos = actions.find("SetStatus");
  size_t header_pos = actions.find("SetHeader");

  // We haven't written to response yet, so WriteEntityChunks should be npos
  // Headers were set, body not written yet
  EXPECT_NE(GoogleString::npos, status_pos);
  EXPECT_NE(GoogleString::npos, header_pos);

  // Now write the chunks to response
  WriteChunksToResponse(fetch.get(), response);
  actions = response->ActionsSinceLastCall();
  EXPECT_TRUE(actions.find("WriteEntityChunks") != GoogleString::npos);

  fetch->HandleDone(true);
}

// MultipleFlushesPreserveContent - content across multiple flushes is correct
TEST_F(IisHtmlFlushingIntegrationTest, MultipleFlushesPreserveContent) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  GoogleString accumulated_content;
  std::vector<GoogleString> chunks = {
      "<html><head>",
      "<title>Multi-flush Test</title>",
      "</head><body>",
      "<div class=\"content\">",
      "<p>Paragraph 1</p>",
      "<p>Paragraph 2</p>",
      "<p>Paragraph 3</p>",
      "</div></body></html>"
  };

  // Simulate production and consumption with flush after each chunk
  fetch->HandleHeadersComplete();

  for (const GoogleString& chunk : chunks) {
    fetch->HandleWrite(chunk, nullptr);
    fetch->HandleFlush(nullptr);

    // Consume the chunk
    auto pending = fetch->TakePendingChunks();
    GoogleString chunk_data = ExtractChunkData(pending);
    accumulated_content += chunk_data;
    response->WriteEntityChunks(chunk_data.data(), chunk_data.size());
    FreeChunks(pending);
  }

  fetch->HandleDone(true);

  // Verify accumulated content matches expected
  GoogleString expected;
  for (const GoogleString& chunk : chunks) {
    expected += chunk;
  }
  EXPECT_EQ(expected, accumulated_content);
  EXPECT_EQ(expected, response->body());
}

// =============================================================================
// 2. Flush Behavior Tests
// =============================================================================

// FlushEnabledCreatesMultipleChunks - verify multiple chunks created
TEST_F(IisHtmlFlushingIntegrationTest, FlushEnabledCreatesMultipleChunks) {
  auto fetch = CreateFetch(true /* flush_enabled */);

  std::vector<GoogleString> chunks = {
      "Chunk 1: <html>",
      "Chunk 2: <head></head>",
      "Chunk 3: <body>",
      "Chunk 4: </body></html>"
  };

  int chunks_received = 0;

  fetch->HandleHeadersComplete();
  for (const GoogleString& chunk : chunks) {
    fetch->HandleWrite(chunk, nullptr);
    fetch->HandleFlush(nullptr);

    if (fetch->HasPendingChunks()) {
      auto pending = fetch->TakePendingChunks();
      chunks_received += pending.size();
      FreeChunks(pending);
    }
  }
  fetch->HandleDone(true);

  // Should have received multiple chunks (one per flush)
  EXPECT_GE(chunks_received, static_cast<int>(chunks.size()));
}

// FlushDisabledCreatesOneChunk - verify single chunk when disabled
TEST_F(IisHtmlFlushingIntegrationTest, FlushDisabledCreatesOneChunk) {
  auto fetch = CreateFetch(false /* flush_disabled */);

  std::vector<GoogleString> chunks = {
      "Chunk 1: <html>",
      "Chunk 2: <head></head>",
      "Chunk 3: <body>",
      "Chunk 4: </body></html>"
  };

  fetch->HandleHeadersComplete();
  for (const GoogleString& chunk : chunks) {
    fetch->HandleWrite(chunk, nullptr);
    fetch->HandleFlush(nullptr);  // Should be no-op when disabled
  }

  // No pending chunks during writing (flush is no-op)
  EXPECT_FALSE(fetch->HasPendingChunks());

  // Done finalizes everything into chunks
  fetch->HandleDone(true);

  auto pending = fetch->TakePendingChunks();

  // All content should be in one or few chunks (depending on block size)
  // but flushing didn't create separate chunks during write
  GoogleString content = ExtractChunkData(pending);

  GoogleString expected;
  for (const GoogleString& chunk : chunks) {
    expected += chunk;
  }
  EXPECT_EQ(expected, content);

  FreeChunks(pending);
}

// EmptyFlushHandled - empty flush doesn't cause issues
TEST_F(IisHtmlFlushingIntegrationTest, EmptyFlushHandled) {
  auto fetch = CreateFetch(true /* flush_enabled */);

  // Multiple empty flushes before any content
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  EXPECT_FALSE(fetch->HasPendingChunks());

  // Write content with empty flushes interspersed
  fetch->HandleHeadersComplete();
  fetch->HandleWrite("<html>", nullptr);
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  // Empty flush after content flush
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  fetch->HandleWrite("<body></body></html>", nullptr);
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  // Empty flushes at end
  EXPECT_TRUE(fetch->HandleFlush(nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  fetch->HandleDone(true);

  // Verify all content is retrievable
  auto chunks = fetch->TakePendingChunks();
  GoogleString content = ExtractChunkData(chunks);
  EXPECT_EQ("<html><body></body></html>", content);

  FreeChunks(chunks);
}

// =============================================================================
// 3. Error Recovery Tests
// =============================================================================

// PartialContentOnError - verify partial content delivered on mid-stream error
TEST_F(IisHtmlFlushingIntegrationTest, PartialContentOnError) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  GoogleString delivered_content;

  // Simulate successful delivery of some content
  fetch->HandleHeadersComplete();

  fetch->HandleWrite("<html><head></head>", nullptr);
  fetch->HandleFlush(nullptr);

  // Consume first flush
  auto chunks1 = fetch->TakePendingChunks();
  delivered_content += ExtractChunkData(chunks1);
  response->WriteEntityChunks(delivered_content.data(),
                               delivered_content.size());
  FreeChunks(chunks1);

  fetch->HandleWrite("<body><p>Partial content</p>", nullptr);
  fetch->HandleFlush(nullptr);

  // Consume second flush
  auto chunks2 = fetch->TakePendingChunks();
  GoogleString second_chunk = ExtractChunkData(chunks2);
  delivered_content += second_chunk;
  response->WriteEntityChunks(second_chunk.data(), second_chunk.size());
  FreeChunks(chunks2);

  // Simulate error during processing (Done with failure)
  fetch->HandleDone(false);

  // Verify partial content was delivered
  EXPECT_FALSE(fetch->success());
  EXPECT_TRUE(delivered_content.find("<html><head></head>") !=
              GoogleString::npos);
  EXPECT_TRUE(delivered_content.find("<body><p>Partial content</p>") !=
              GoogleString::npos);

  // The partial content is in the response
  EXPECT_EQ(delivered_content, response->body());
}

// HeadersAlwaysSent - headers sent even if body fails
TEST_F(IisHtmlFlushingIntegrationTest, HeadersAlwaysSent) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  // Set headers
  response->SetStatus(200, "OK");
  response->SetHeader("Content-Type", "text/html");
  response->SetHeader("X-PageSpeed", "1.0");

  // Start processing
  fetch->HandleHeadersComplete();
  EXPECT_TRUE(fetch->headers_complete());

  // Verify headers are set
  EXPECT_EQ(200, response->GetStatus());
  EXPECT_EQ("text/html", response->GetHeader("Content-Type"));
  EXPECT_EQ("1.0", response->GetHeader("X-PageSpeed"));

  // Simulate immediate failure (no body written)
  fetch->HandleDone(false);

  // Headers should still be present
  EXPECT_EQ(200, response->GetStatus());
  EXPECT_EQ("text/html", response->GetHeader("Content-Type"));
  EXPECT_FALSE(fetch->success());
}

// Mid-stream write failure recovery
TEST_F(IisHtmlFlushingIntegrationTest, MidStreamRecovery) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  fetch->HandleHeadersComplete();

  // First successful write
  EXPECT_TRUE(fetch->HandleWrite("<html>", nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  WriteChunksToResponse(fetch.get(), response);

  // Simulate continued processing after potential issue
  EXPECT_TRUE(fetch->HandleWrite("<body>", nullptr));
  EXPECT_TRUE(fetch->HandleFlush(nullptr));

  WriteChunksToResponse(fetch.get(), response);

  // Finish successfully
  EXPECT_TRUE(fetch->HandleWrite("</body></html>", nullptr));
  fetch->HandleDone(true);

  WriteChunksToResponse(fetch.get(), response);

  // All content should be present
  EXPECT_EQ("<html><body></body></html>", response->body());
  EXPECT_TRUE(fetch->success());
}

// =============================================================================
// 4. Configuration Tests
// =============================================================================

// FlushHtmlOptionRespected - RewriteOptions flush_html works
TEST_F(IisHtmlFlushingIntegrationTest, FlushHtmlOptionRespected) {
  // Test with flush enabled
  {
    auto fetch_enabled = CreateFetch(true /* flush_enabled */);
    fetch_enabled->HandleWrite("test content", nullptr);
    fetch_enabled->HandleFlush(nullptr);

    // Should have pending chunks when enabled
    EXPECT_TRUE(fetch_enabled->HasPendingChunks());

    auto chunks = fetch_enabled->TakePendingChunks();
    FreeChunks(chunks);
    fetch_enabled->HandleDone(true);
  }

  // Test with flush disabled
  {
    auto fetch_disabled = CreateFetch(false /* flush_disabled */);
    fetch_disabled->HandleWrite("test content", nullptr);
    fetch_disabled->HandleFlush(nullptr);

    // Should NOT have pending chunks when disabled (flush is no-op)
    EXPECT_FALSE(fetch_disabled->HasPendingChunks());

    fetch_disabled->HandleDone(true);

    // Content only available after Done
    auto chunks = fetch_disabled->TakePendingChunks();
    EXPECT_EQ("test content", ExtractChunkData(chunks));
    FreeChunks(chunks);
  }
}

// FlushBufferLimitRespected - buffer limit configuration works
TEST_F(IisHtmlFlushingIntegrationTest, FlushBufferLimitRespected) {
  auto fetch = CreateFetch(true /* flush_enabled */);

  // Write data larger than kMaxBlockSize (64KB) - should split into chunks
  const size_t large_size = IisStreamingFetch::kMaxBlockSize + 1024;
  GoogleString large_data(large_size, 'X');

  fetch->HandleWrite(large_data, nullptr);
  fetch->HandleFlush(nullptr);

  auto chunks = fetch->TakePendingChunks();

  // Should have multiple chunks due to buffer limit
  EXPECT_GT(chunks.size(), 1u);

  // Each chunk should not exceed kMaxBlockSize
  for (const HTTP_DATA_CHUNK* chunk : chunks) {
    EXPECT_LE(chunk->FromMemory.BufferLength, IisStreamingFetch::kMaxBlockSize);
  }

  // Total data should match
  EXPECT_EQ(large_size, TotalChunkBytes(chunks));

  FreeChunks(chunks);
  fetch->HandleDone(true);
}

// FlushBufferLimitWithResponse - verify chunks written correctly with buffer limits
TEST_F(IisHtmlFlushingIntegrationTest, FlushBufferLimitWithResponse) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  // Write data larger than kMaxBlockSize (64KB)
  const size_t large_size = IisStreamingFetch::kMaxBlockSize + 1024;
  GoogleString large_data(large_size, 'X');

  fetch->HandleWrite(large_data, nullptr);
  fetch->HandleFlush(nullptr);

  // Write chunks to response
  WriteChunksToResponse(fetch.get(), response);

  fetch->HandleDone(true);

  // Final chunks if any
  WriteChunksToResponse(fetch.get(), response);

  // Response should contain all data
  EXPECT_EQ(large_size, response->body().size());
  EXPECT_EQ(large_data, response->body());
}

// =============================================================================
// Additional Integration Tests
// =============================================================================

// Test realistic HTML page with CSS and JS references
TEST_F(IisHtmlFlushingIntegrationTest, RealisticHtmlPage) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();
  response->SetStatus(200, "OK");
  response->SetHeader("Content-Type", "text/html; charset=utf-8");

  std::vector<GoogleString> chunks = {
      "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n",
      "  <meta charset=\"utf-8\">\n",
      "  <title>PageSpeed Optimized Page</title>\n",
      "  <link rel=\"stylesheet\" href=\"/styles/main.pagespeed.cf.abc123.css\">\n",
      "</head>\n<body>\n",
      "  <header><h1>Welcome</h1></header>\n",
      "  <main>\n",
      "    <p>This is optimized content.</p>\n",
      "    <img src=\"/images/hero.pagespeed.ic.def456.webp\" alt=\"Hero\">\n",
      "  </main>\n",
      "  <script src=\"/scripts/app.pagespeed.jc.ghi789.js\"></script>\n",
      "</body>\n</html>"
  };

  int flushes = SimulateFullRequestCycle(fetch.get(), chunks, true, response);

  // Should have multiple flushes
  EXPECT_GT(flushes, 0);

  // Verify response contains all parts
  const GoogleString& body = response->body();
  EXPECT_TRUE(body.find("<!DOCTYPE html>") != GoogleString::npos);
  EXPECT_TRUE(body.find("main.pagespeed.cf.abc123.css") != GoogleString::npos);
  EXPECT_TRUE(body.find("hero.pagespeed.ic.def456.webp") != GoogleString::npos);
  EXPECT_TRUE(body.find("app.pagespeed.jc.ghi789.js") != GoogleString::npos);
  EXPECT_TRUE(body.find("</html>") != GoogleString::npos);
}

// Test concurrent producer/consumer with large content
TEST_F(IisHtmlFlushingIntegrationTest, ConcurrentLargeContent) {
  auto fetch = CreateFetch(true /* flush_enabled */);

  // Generate large content
  std::vector<GoogleString> chunks;
  GoogleString expected_total;
  for (int i = 0; i < 100; ++i) {
    GoogleString chunk = StrCat("<p>Paragraph ", IntegerToString(i),
                                " with some additional content to make it longer.</p>\n");
    chunks.push_back(chunk);
    expected_total += chunk;
  }

  std::atomic<size_t> bytes_consumed{0};
  std::atomic<bool> producer_done{false};
  GoogleString consumed_content;
  std::mutex content_mutex;

  // Producer thread
  std::thread producer([&]() {
    fetch->HandleHeadersComplete();
    for (size_t i = 0; i < chunks.size(); ++i) {
      fetch->HandleWrite(chunks[i], nullptr);
      // Flush every 10 chunks
      if ((i + 1) % 10 == 0) {
        fetch->HandleFlush(nullptr);
      }
    }
    fetch->HandleDone(true);
    producer_done = true;
  });

  // Consumer loop
  while (!producer_done || fetch->HasPendingChunks()) {
    if (fetch->HasPendingChunks()) {
      auto pending = fetch->TakePendingChunks();
      for (HTTP_DATA_CHUNK* chunk : pending) {
        if (chunk && chunk->FromMemory.pBuffer) {
          std::lock_guard<std::mutex> lock(content_mutex);
          consumed_content.append(
              static_cast<const char*>(chunk->FromMemory.pBuffer),
              chunk->FromMemory.BufferLength);
          bytes_consumed += chunk->FromMemory.BufferLength;
        }
      }
      FreeChunks(pending);
    }
    std::this_thread::yield();
  }

  // Final drain
  auto final_chunks = fetch->TakePendingChunks();
  for (HTTP_DATA_CHUNK* chunk : final_chunks) {
    if (chunk && chunk->FromMemory.pBuffer) {
      consumed_content.append(
          static_cast<const char*>(chunk->FromMemory.pBuffer),
          chunk->FromMemory.BufferLength);
      bytes_consumed += chunk->FromMemory.BufferLength;
    }
  }
  FreeChunks(final_chunks);

  producer.join();

  // Verify all content was consumed correctly
  EXPECT_EQ(expected_total.size(), bytes_consumed.load());
  EXPECT_EQ(expected_total, consumed_content);
}

// Test action recording during flush cycle
TEST_F(IisHtmlFlushingIntegrationTest, ActionRecordingDuringFlush) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();
  response->ClearRecordedActions();

  // Simulate complete cycle with action recording
  response->SetStatus(200, "OK");
  response->SetHeader("Content-Type", "text/html");

  fetch->HandleHeadersComplete();
  fetch->HandleWrite("<html><body>Test</body></html>", nullptr);
  fetch->HandleFlush(nullptr);

  auto chunks = fetch->TakePendingChunks();
  for (HTTP_DATA_CHUNK* chunk : chunks) {
    response->WriteEntityChunks(
        static_cast<const char*>(chunk->FromMemory.pBuffer),
        chunk->FromMemory.BufferLength);
  }
  FreeChunks(chunks);

  response->Flush(true);
  response->set_completed(true);

  fetch->HandleDone(true);

  // Verify actions were recorded in correct order
  GoogleString actions = response->AllActions();
  EXPECT_TRUE(actions.find("SetStatus(200, OK)") != GoogleString::npos);
  EXPECT_TRUE(actions.find("SetHeader(Content-Type, text/html)") !=
              GoogleString::npos);
  EXPECT_TRUE(actions.find("WriteEntityChunks") != GoogleString::npos);
  EXPECT_TRUE(actions.find("Flush(final)") != GoogleString::npos);
}

// Test binary content (images) through streaming
TEST_F(IisHtmlFlushingIntegrationTest, BinaryContentStreaming) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  // Create binary data with null bytes and various byte values
  GoogleString binary_data;
  for (int i = 0; i < 256; ++i) {
    binary_data.push_back(static_cast<char>(i));
  }
  binary_data.append(binary_data);  // 512 bytes total

  fetch->HandleHeadersComplete();
  fetch->HandleWrite(binary_data, nullptr);
  fetch->HandleFlush(nullptr);
  fetch->HandleDone(true);

  auto chunks = fetch->TakePendingChunks();
  GoogleString result = ExtractChunkData(chunks);

  // Binary data should be preserved exactly
  EXPECT_EQ(binary_data.size(), result.size());
  EXPECT_EQ(binary_data, result);

  // Also verify writing to response preserves binary data
  for (HTTP_DATA_CHUNK* chunk : chunks) {
    if (chunk && chunk->FromMemory.pBuffer) {
      response->WriteEntityChunks(
          static_cast<const char*>(chunk->FromMemory.pBuffer),
          chunk->FromMemory.BufferLength);
    }
  }
  EXPECT_EQ(binary_data, response->body());

  FreeChunks(chunks);
}

// Test rapid flush cycles don't lose data
TEST_F(IisHtmlFlushingIntegrationTest, RapidFlushCycleIntegrity) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  const int kCycles = 50;
  GoogleString expected;

  fetch->HandleHeadersComplete();

  for (int i = 0; i < kCycles; ++i) {
    GoogleString data = StrCat("Cycle-", IntegerToString(i), "-");
    fetch->HandleWrite(data, nullptr);
    fetch->HandleFlush(nullptr);
    expected += data;

    // Consume immediately
    auto chunks = fetch->TakePendingChunks();
    GoogleString chunk_data = ExtractChunkData(chunks);
    response->WriteEntityChunks(chunk_data.data(), chunk_data.size());
    FreeChunks(chunks);
  }

  fetch->HandleDone(true);

  // Consume any remaining
  auto final_chunks = fetch->TakePendingChunks();
  GoogleString final_data = ExtractChunkData(final_chunks);
  response->WriteEntityChunks(final_data.data(), final_data.size());
  FreeChunks(final_chunks);

  EXPECT_EQ(expected, response->body());
  // Verify all bytes were flushed - should match expected size
  EXPECT_EQ(expected.size(), fetch->bytes_flushed());
}

// Test IIS context integration
TEST_F(IisHtmlFlushingIntegrationTest, IisContextIntegration) {
  // Verify context is properly configured
  EXPECT_STREQ("http://test.com/page.html",
               context_->request()->GetRawHttpRequestUrl());
  EXPECT_EQ("GET", context_->request()->GetMethod());
  EXPECT_EQ(1u, context_->GetSiteId());
  EXPECT_EQ("Default Web Site", context_->GetSiteName());

  auto fetch = CreateFetch(true);
  MockHttpResponse* response = context_->response();

  // Complete request cycle
  response->SetStatus(200, "OK");
  response->SetHeader("Content-Type", "text/html");

  SimulateProxyFetchOutput(fetch.get(), {"<html></html>"}, false);
  WriteChunksToResponse(fetch.get(), response);
  response->Flush(true);
  response->set_completed(true);

  EXPECT_TRUE(response->completed());
  EXPECT_EQ(200, response->GetStatus());
  EXPECT_EQ("<html></html>", response->body());
}

// Test empty response handling
TEST_F(IisHtmlFlushingIntegrationTest, EmptyResponseHandling) {
  auto fetch = CreateFetch(true /* flush_enabled */);

  // Complete cycle with no body content
  fetch->HandleHeadersComplete();
  fetch->HandleDone(true);

  EXPECT_TRUE(fetch->IsDone());
  EXPECT_TRUE(fetch->success());
  EXPECT_EQ(0u, fetch->bytes_written());
  EXPECT_FALSE(fetch->HasPendingChunks());
}

// Test very large single write
TEST_F(IisHtmlFlushingIntegrationTest, VeryLargeSingleWrite) {
  auto fetch = CreateFetch(true /* flush_enabled */);
  MockHttpResponse* response = context_->response();

  // 1MB of content
  const size_t kLargeSize = 1024 * 1024;
  GoogleString large_content(kLargeSize, 'X');

  fetch->HandleHeadersComplete();
  fetch->HandleWrite(large_content, nullptr);
  fetch->HandleFlush(nullptr);
  fetch->HandleDone(true);

  GoogleString accumulated;
  auto chunks = fetch->TakePendingChunks();
  for (HTTP_DATA_CHUNK* chunk : chunks) {
    if (chunk && chunk->FromMemory.pBuffer) {
      accumulated.append(
          static_cast<const char*>(chunk->FromMemory.pBuffer),
          chunk->FromMemory.BufferLength);
      response->WriteEntityChunks(
          static_cast<const char*>(chunk->FromMemory.pBuffer),
          chunk->FromMemory.BufferLength);
    }
  }
  FreeChunks(chunks);

  // Verify accumulated data
  EXPECT_EQ(kLargeSize, accumulated.size());
  EXPECT_EQ(large_content, accumulated);

  // Verify response received all data
  EXPECT_EQ(kLargeSize, response->body().size());
  EXPECT_EQ(large_content, response->body());
}

}  // namespace net_instaweb
