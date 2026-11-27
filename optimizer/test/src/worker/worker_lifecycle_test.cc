// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Worker lifecycle, connection handling and daemon internals tests
//
// - Initialization, shutdown, mmap cache directory and cache corruption
//   recovery
// - Connection limits, health sockets, thread pool dispatch, accessors
// - HTTP API, WebSocket, Web Bot Auth timer and startup artifacts
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#ifndef _WIN32
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/shared_config.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

TEST_F(WorkerTest, InitializeAndShutdown) {
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  EXPECT_TRUE(worker.Initialize());
  EXPECT_EQ(worker.socket_path(), socket_path_);

  // Socket/pipe endpoint should exist
  EXPECT_TRUE(pagespeed::test::PipeEndpointExists(socket_path_));

  worker.Shutdown();
}

TEST_F(WorkerTest, InitializeWithCache) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  EXPECT_TRUE(worker.Initialize());
  EXPECT_NE(worker.cache(), nullptr);

  worker.Shutdown();
}

// =============================================================================
// Rapid-fire notification test
// =============================================================================
//
// Phase 6 learning: The worker's OnClientRead had a buffer.clear() bug
// that discarded any data beyond the first notification in a read.
// While the current architecture uses one connection per notification,
// this test verifies the worker handles rapid sequential notifications
// without losing any.

TEST_F(WorkerTest, RapidFireNotificationsAllProcessed) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with 3 different CSS files
  std::string url1 = "http://example.com/a.css";
  std::string css1 = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url1, css1);

  std::string url2 = "http://example.com/b.css";
  std::string css2 = "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url2, css2);

  std::string url3 = "http://example.com/c.css";
  std::string css3 = "p  {  line-height:  1.5;  margin:  10px;  }\n";
  CacheOriginal(worker.cache(), url3, css3);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send all 3 notifications in rapid succession (separate connections)
  CacheNotification n1, n2, n3;
  n1.url = url1;
  n1.scheme = "https";
  n1.content_type = ContentType::kCss;
  n1.capability_mask = CapabilityMask().Encode();

  n2.url = url2;
  n2.scheme = "https";
  n2.content_type = ContentType::kCss;
  n2.capability_mask = CapabilityMask().Encode();

  n3.url = url3;
  n3.scheme = "https";
  n3.content_type = ContentType::kCss;
  n3.capability_mask = CapabilityMask().Encode();

  SendNotification(n1);
  SendNotification(n2);
  SendNotification(n3);

  // Poll until all 3 variants appear at Desktop/Identity (normalized).
  CapabilityMask mask;
  std::string v1, v2, v3;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (v1.empty()) v1 = ReadVariant(worker.cache(), url1, mask);
    if (v2.empty()) v2 = ReadVariant(worker.cache(), url2, mask);
    if (v3.empty()) v3 = ReadVariant(worker.cache(), url3, mask);
    if (!v1.empty() && !v2.empty() && !v3.empty()) break;
  }

  EXPECT_FALSE(v1.empty()) << "CSS a.css variant missing";
  EXPECT_FALSE(v2.empty()) << "CSS b.css variant missing";
  EXPECT_FALSE(v3.empty()) << "CSS c.css variant missing";

  // Each minified variant should be smaller than its original
  if (!v1.empty()) {
    EXPECT_LT(v1.size(), css1.size());
  }
  if (!v2.empty()) {
    EXPECT_LT(v2.size(), css2.size());
  }
  if (!v3.empty()) {
    EXPECT_LT(v3.size(), css3.size());
  }

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Worker with mmap directory enabled
// =============================================================================
//
// Phase 6 learning: The worker MUST open the cache with
// enable_mmap_directory = true for cross-process sharing with nginx.

TEST_F(WorkerTest, WorkerCacheUsesMmapDirectory) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  // Write via the worker's cache using sentinel (simulates original content).
  std::string url = "/mmap_test";
  std::string value = "shared_value";
  CacheOriginal(worker.cache(), url, value);

  // Open a second PageSpeedCache on the same path (multi-process sharing).
  PageSpeedCacheConfig reader_config;
  reader_config.volume_path = cache_path_;
  reader_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  auto reader_result = PageSpeedCache::Create(reader_config);
  ASSERT_TRUE(reader_result.has_value()) << "Reader cache creation failed";
  auto& reader = *reader_result.value();

  // The reader should see the worker's write via ReadBestAlternate.
  auto read = reader.ReadBestAlternate(url, "", "https", CapabilityMask());
  ASSERT_TRUE(read.has_value())
      << "Reader cannot see worker's write — "
         "worker cache must use multi-process sharing";
  auto content = read->content();
  std::string read_value(reinterpret_cast<const char*>(content.data()),
                         content.size());
  EXPECT_EQ(read_value, value);

  worker.Shutdown();
}

TEST_F(WorkerTest, MaxConnectionsEnforced) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_connections = 2;
  config.connection_timeout_ms = 10000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Open max_connections connections and keep them alive
  intptr_t fd1 = ConnectToSocket(socket_path_);
  ASSERT_GE(fd1, static_cast<intptr_t>(0)) << "First connection failed";
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  intptr_t fd2 = ConnectToSocket(socket_path_);
  ASSERT_GE(fd2, static_cast<intptr_t>(0)) << "Second connection failed";
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // The third connection should be accepted but immediately closed
  // by the server (rejected).  We detect this by trying to read:
  // the server closes the socket, so read returns 0.
  intptr_t fd3 = ConnectToSocket(socket_path_);
  ASSERT_GE(fd3, static_cast<intptr_t>(0)) << "Third connect() failed";

  // Give the worker time to reject
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read on the rejected socket should return 0 (EOF on Unix) or -1
  // (ERROR_BROKEN_PIPE on Windows). Both indicate the server closed it.
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd3, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Rejected connection should be closed by server";

  pagespeed::test::ClosePipe(fd1);
  pagespeed::test::ClosePipe(fd2);
  pagespeed::test::ClosePipe(fd3);

  // Let the worker process the closes
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WorkerTest, BufferOverflowDisconnects) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_buffer_size = 64;  // Very small buffer limit
  config.connection_timeout_ms = 10000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect and send data exceeding max_buffer_size
  intptr_t fd = ConnectToSocket(socket_path_);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Send 128 bytes of junk (exceeds 64 byte limit)
  std::string junk(128, 'X');
  ssize_t written = pagespeed::test::PipeWrite(fd, junk.data(), junk.size());
  // Write may or may not fully succeed depending on timing
  (void)written;

  // Give worker time to process and close
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Subsequent read should return 0 (EOF) or write should fail
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Connection should be closed after overflow";

  pagespeed::test::ClosePipe(fd);
}

TEST_F(WorkerTest, ConnectionTimeoutDisconnects) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.connection_timeout_ms = 100;  // 100ms timeout

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect but don't send any data
  intptr_t fd = ConnectToSocket(socket_path_);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Wait for timeout to fire (100ms + margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Connection should be closed by the server
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Connection should be closed after timeout";

  pagespeed::test::ClosePipe(fd);
}

TEST_F(WorkerTest, GracefulShutdownDrainsConnections) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.connection_timeout_ms = 10000;
  config.shutdown_timeout_ms = 2000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Open a connection
  intptr_t fd = ConnectToSocket(socket_path_);
  // No WorkerStopper at this site (#1354): the teardown below is the behavior
  // under test — Shutdown() is issued from a second thread mid-body and the
  // worker_thread.join() is itself the assertion — so a guard would
  // double-join. Clean up explicitly before the fatal assert instead, so a
  // failed connect does not return with worker_thread still joinable.
  if (fd < 0) {
    worker.Shutdown();
    worker_thread.join();
  }
  ASSERT_GE(fd, static_cast<intptr_t>(0));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(worker.active_connections(), 1);

  // Initiate shutdown from another thread
  std::thread shutdown_thread([&worker]() {
    // Give a tiny delay so the main thread can observe
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    worker.Shutdown();
  });

  // Close our connection to let the worker drain
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  pagespeed::test::ClosePipe(fd);

  // Worker should exit cleanly
  worker_thread.join();
  shutdown_thread.join();

  // After shutdown, active connections should be zero
  EXPECT_EQ(worker.active_connections(), 0);
}

TEST_F(WorkerTest, HealthCheckResponds) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_connections = 128;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to health socket
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  // Read response
  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';

  std::string response(buf, n);

  // Should start with "OK"
  EXPECT_EQ(response.substr(0, 2), "OK");
  // Should contain connection info like "0/128"
  EXPECT_NE(response.find("/128"), std::string::npos)
      << "Response: " << response;
  // Should end with newline
  EXPECT_EQ(response.back(), '\n');

  pagespeed::test::ClosePipe(fd);
}

// =============================================================================
// Production Hardening Tests
// =============================================================================

// =============================================================================
// Thread Pool Dispatch Tests
// =============================================================================

TEST_F(WorkerTest, HealthResponsiveDuringProcessing) {
  // Verify the health endpoint responds promptly even when the worker
  // is processing a notification in the thread pool.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content
  std::string url = "http://example.com/tp-health.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Immediately query health endpoint - should respond within 1 second
  auto start = std::chrono::steady_clock::now();
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  auto elapsed = std::chrono::steady_clock::now() - start;
  pagespeed::test::ClosePipe(fd);

  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';
  std::string response(buf, n);
  EXPECT_EQ(response.substr(0, 2), "OK");
  EXPECT_NE(response.find("inflight="), std::string::npos)
      << "Response: " << response;

  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  EXPECT_LT(elapsed_ms, 1000)
      << "Health response took too long: " << elapsed_ms << "ms";

  // Wait for processing to finish
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_F(WorkerTest, ConcurrentNotificationsProcessed) {
  // Send 3 CSS notifications rapidly and verify all 3 variants appear.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate 3 CSS files
  std::string url1 = "http://example.com/tp1.css";
  std::string css1 = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url1, css1);

  std::string url2 = "http://example.com/tp2.css";
  std::string css2 = "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url2, css2);

  std::string url3 = "http://example.com/tp3.css";
  std::string css3 = "p  {  line-height:  1.5;  margin:  10px;  }\n";
  CacheOriginal(worker.cache(), url3, css3);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send all 3 rapidly
  CacheNotification n1, n2, n3;
  n1.url = url1;
  n1.scheme = "https";
  n1.content_type = ContentType::kCss;
  n1.capability_mask = CapabilityMask().Encode();
  n2.url = url2;
  n2.scheme = "https";
  n2.content_type = ContentType::kCss;
  n2.capability_mask = CapabilityMask().Encode();
  n3.url = url3;
  n3.scheme = "https";
  n3.content_type = ContentType::kCss;
  n3.capability_mask = CapabilityMask().Encode();

  SendNotification(n1);
  SendNotification(n2);
  SendNotification(n3);

  // Poll until all 3 variants appear
  CapabilityMask mask = CapabilityMask::Decode(CapabilityMask().Encode());
  std::string v1, v2, v3;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (v1.empty()) v1 = ReadVariant(worker.cache(), url1, mask);
    if (v2.empty()) v2 = ReadVariant(worker.cache(), url2, mask);
    if (v3.empty()) v3 = ReadVariant(worker.cache(), url3, mask);
    if (!v1.empty() && !v2.empty() && !v3.empty()) break;
  }

  EXPECT_FALSE(v1.empty()) << "tp1.css variant missing";
  EXPECT_FALSE(v2.empty()) << "tp2.css variant missing";
  EXPECT_FALSE(v3.empty()) << "tp3.css variant missing";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ShutdownWaitsForInFlightWork) {
  // Send a notification and immediately shut down; the variant should
  // still be written because shutdown waits for in-flight work.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/tp-shutdown.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Brief pause to ensure notification is dispatched to thread pool
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Immediately shut down
  worker.Shutdown();
  worker_thread.join();

  // Verify the variant was written despite immediate shutdown
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty())
      << "Variant should be written before shutdown completes";
}

TEST_F(WorkerTest, InFlightCounterAccuracy) {
  // Verify that in_flight_work() eventually returns to 0 after
  // all notifications are processed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content
  std::string url = "http://example.com/tp-inflight.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(worker.in_flight_work(), 0);

  // Send notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing to complete
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.in_flight_work() == 0 &&
        worker.stats().notifications_received.load() >= 1) {
      break;
    }
  }

  EXPECT_EQ(worker.in_flight_work(), 0)
      << "In-flight counter should return to 0 after processing";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, StatsThreadPoolInfoInStats) {
  // Verify STATS response includes thread_pool info.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains and "thread_pool" sits near its end, so a fixed-buffer single
  // read truncates it silently once the document outgrows the buffer (see
  // ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"thread_pool\""), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("\"inflight\""), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("\"size\""), std::string::npos)
      << "Response: " << response;
}

TEST_F(WorkerTest, MetricsIncludesThreadPoolGauge) {
  // Verify METRICS response includes thread pool inflight gauge.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the metrics document no longer fits one
  // bufferful (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("pagespeed_thread_pool_inflight"), std::string::npos)
      << "Response: " << response;
}

// =============================================================================
// Timing Counters Tests (Feature 7)
// =============================================================================

TEST_F(WorkerTest, TimingCountersIncrementAfterProcessing) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Timing should start at zero.
  EXPECT_EQ(worker.stats().total_processing_time_us.load(), 0u);
  EXPECT_EQ(worker.stats().css_processing_time_us.load(), 0u);

  // Pre-populate CSS content.
  std::string css_url = "http://example.com/timing.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), css_url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing — poll until both timing counters are non-zero
  // instead of a fixed sleep (slow shards can sit at the pipe read past a
  // fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().total_processing_time_us.load() > 0u &&
        worker.stats().css_processing_time_us.load() > 0u) {
      break;
    }
  }

  // Timing counters should be non-zero after processing.
  EXPECT_GT(worker.stats().total_processing_time_us.load(), 0u);
  EXPECT_GT(worker.stats().css_processing_time_us.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Phase 3.1: Worker Daemon Internals Tests
// =============================================================================

// --- Health check socket tests ---

TEST_F(WorkerTest, HealthSocketExists) {
  // Verify that the .health socket file is created after Initialize().
  WorkerConfig config;
  config.socket_path = socket_path_;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string health_path = worker.health_socket_path();
  EXPECT_TRUE(pagespeed::test::PipeEndpointExists(health_path))
      << "Health socket/pipe endpoint should exist after Initialize()";

  worker.Shutdown();
}

TEST_F(WorkerTest, HealthSocketReturnsOk) {
  // Connect to health socket, read response, verify it starts with "OK ".
  WorkerConfig config;
  config.socket_path = socket_path_;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_TRUE(response.starts_with("OK "))
      << "Health response should start with 'OK ', got: " << response;
}

TEST_F(WorkerTest, HealthSocketShowsConnectionCount) {
  // Verify active connection info appears in health response.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_connections = 64;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Open a notification socket connection to bump active_connections.
  int held_fd = ConnectToSocket(socket_path_);
  ASSERT_GE(held_fd, 0) << "Failed to open held connection";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Now query health endpoint.
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  // Response should contain connection count info (e.g., "1/64").
  EXPECT_NE(response.find("/64"), std::string::npos)
      << "Health response should show max connections, got: " << response;

  pagespeed::test::ClosePipe(held_fd);
}

// --- end ported adversarial probes ---

// --- Stats counter tests ---

TEST_F(WorkerTest, NotificationCounterIncrements) {
  // Verify notifications_received increments for each notification.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);

  // Pre-populate two distinct CSS resources.
  std::string url1 = "http://example.com/counter1.css";
  std::string url2 = "http://example.com/counter2.css";
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url1, css);
  CacheOriginal(worker.cache(), url2, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification n1;
  n1.url = url1;
  n1.scheme = "https";
  n1.content_type = ContentType::kCss;
  n1.capability_mask = CapabilityMask().Encode();
  SendNotification(n1);

  // Wait for first notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);

  CacheNotification n2;
  n2.url = url2;
  n2.scheme = "https";
  n2.content_type = ContentType::kCss;
  n2.capability_mask = CapabilityMask().Encode();
  SendNotification(n2);

  // Wait for second notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().notifications_received.load(), 2u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, VariantWrittenCounterAccurate) {
  // Verify that variants_written reflects actual cache writes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  // Pre-populate CSS content.
  std::string url = "http://example.com/counter-variants.css";
  std::string css = "body { margin: 0; } h1 { color: green; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().variants_written.load() >= 1) break;
  }

  uint64_t variants = worker.stats().variants_written.load();
  EXPECT_GE(variants, 1u)
      << "At least one variant should be written for CSS processing";

  // Verify a variant actually exists in cache (identity mask for CSS).
  CapabilityMask mask;  // Desktop/Identity = default
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty())
      << "Cache should contain written variant for CSS";
  // Minified CSS should be smaller or equal to original.
  EXPECT_LE(variant.size(), css.size())
      << "Minified CSS should not be larger than original";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Config Accessor Tests
// =============================================================================

TEST_F(WorkerTest, ApiPortDefaultZero) {
  // Without an API server, api_port() returns 0.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.api_port(), 0);

  worker.Shutdown();
}

TEST_F(WorkerTest, GetConfigReturnsLiveConfig) {
  // GetConfig() returns the current live configuration snapshot.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.jpeg_quality = 42;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  auto live = worker.GetConfig();
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(live->jpeg_quality, 42);

  worker.Shutdown();
}

TEST_F(WorkerTest, UpdateConfigChangesLiveConfig) {
  // UpdateConfig() atomically replaces the live config.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.jpeg_quality = 85;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  auto before = worker.GetConfig();
  ASSERT_NE(before, nullptr);
  EXPECT_EQ(before->jpeg_quality, 85);

  // Update to a new config.
  auto new_config = std::make_shared<WorkerConfig>(*before);
  new_config->jpeg_quality = 50;
  worker.UpdateConfig(new_config);

  auto after = worker.GetConfig();
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->jpeg_quality, 50);

  // The old snapshot should still have the original value.
  EXPECT_EQ(before->jpeg_quality, 85);

  worker.Shutdown();
}

TEST_F(WorkerTest, SocketPathAccessor) {
  // socket_path() returns the configured path.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.socket_path(), socket_path_);

  worker.Shutdown();
}

TEST_F(WorkerTest, CacheAccessor) {
  // cache() returns non-null when cache is configured.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_NE(worker.cache(), nullptr);

  worker.Shutdown();
}

TEST_F(WorkerTest, CacheAccessorNullWithoutPath) {
  // cache() returns null when no cache_path is configured.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_EQ(worker.cache(), nullptr);

  worker.Shutdown();
}

TEST_F(WorkerTest, StatsAccessor) {
  // stats() provides access to WorkerStats with default zeros.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  EXPECT_EQ(worker.stats().errors.load(), 0u);

  worker.Shutdown();
}

TEST_F(WorkerTest, ActiveConnectionsAndInFlightDefaults) {
  // active_connections() and in_flight_work() default to 0.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.active_connections(), 0);
  EXPECT_EQ(worker.in_flight_work(), 0);

  worker.Shutdown();
}

// =============================================================================
// InvalidateUrl with no cache returns 0
// =============================================================================

TEST_F(WorkerTest, InvalidateUrlNoCacheReturnsZero) {
  // When no cache is configured, InvalidateUrl returns 0.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  int deleted = worker.InvalidateUrl("http://example.com/no-cache", "");
  EXPECT_EQ(deleted, 0);

  worker.Shutdown();
}

// =============================================================================
// Health socket path construction
// =============================================================================

TEST_F(WorkerTest, HealthSocketPathSuffix) {
  // Health socket path should be the main socket path + ".health".
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.health_socket_path(), socket_path_ + ".health");

  worker.Shutdown();
}

TEST_F(WorkerTest, MgmtSocketPathSuffix) {
  // Management socket path should be the main socket path + ".mgmt".
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.mgmt_socket_path(), socket_path_ + ".mgmt");

  worker.Shutdown();
}

// =============================================================================
// Notification without cache does not crash
// =============================================================================

TEST_F(WorkerTest, NotificationWithoutCacheHandledGracefully) {
  // Notifications when cache is not configured should not crash.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the notification to be received instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // not-processed invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1u) break;
  }

  // Should have received the notification but not processed it.
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HttpApiEnabledWorker) {
  // Initialize worker with api_port=-1 (OS picks a free port).
  // Covers the HTTP server init, route registration, the WebSocket upgrade
  // handler and the console route.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;  // OS picks a free port

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // api_port() should return the actual bound port (> 0).
  EXPECT_GT(worker.api_port(), 0) << "HTTP API should be bound to a real port";

  // Run briefly to exercise the event loop with HTTP server active.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Verify we can connect to the HTTP API port via TCP.
  int sock = pagespeed::test::ConnectTcp(worker.api_port());
  EXPECT_GE(sock, 0) << "Could not connect to HTTP API port "
                     << worker.api_port();
  if (sock >= 0) pagespeed::test::CloseSocket(sock);

  // WebSocket manager should be available.
  EXPECT_NE(worker.ws_manager(), nullptr);

  worker.Shutdown();
  worker_thread.join();
}

// Regression: /v1/health's connections block must report the connection cap
// the management API listener actually enforces
// (HttpServerConfig::max_connections, default 32), not the notification
// listener's separate WorkerConfig::max_connections (default 128).
TEST_F(WorkerTest, HttpApiHealthReportsHttpListenerConnectionCap) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;  // OS picks a free port

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_GT(worker.api_port(), 0);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int sock = pagespeed::test::ConnectTcp(worker.api_port());
  ASSERT_GE(sock, 0) << "Failed to connect to API port " << worker.api_port();

  const std::string request =
      "GET /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n\r\n";
  ssize_t written =
      pagespeed::test::SocketWrite(sock, request.data(), request.size());
  ASSERT_EQ(written, static_cast<ssize_t>(request.size()));

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
  }
  pagespeed::test::CloseSocket(sock);

  EXPECT_NE(response.find("200 OK"), std::string::npos) << response;
  EXPECT_NE(response.find("\"max\":32"), std::string::npos)
      << "connections.max must be the HTTP listener's cap: " << response;
  EXPECT_EQ(response.find("\"max\":128"), std::string::npos)
      << "the notification listener's cap must not leak into /v1/health: "
      << response;
}

TEST_F(WorkerTest, WorkerWithBrowserAnalysisEnabled) {
  // Initialize worker with browser_analysis.enabled = true.
  // Covers lines 503-513 (BrowserAnalysisManager creation and init).
  // Chrome is unlikely to be available in the test environment, so the
  // Initialize() call should succeed but browser_manager_ may be reset
  // after the BrowserAnalysisManager fails to start Chrome.
  // Also covers lines 896-898 (browser_manager shutdown) during teardown.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // The test binary runs as root in the build container,
  // where the Chrome sandbox is (correctly) unavailable and `require`
  // would refuse to construct the manager.  These tests seed profiles
  // directly and never spawn Chrome, so take the documented opt-out.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  // Use a non-existent binary to trigger graceful failure.
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";

  NullMessageHandler handler;
  Worker worker(config, &handler);
  // Initialize should succeed even if browser analysis init fails
  // (it falls back to heuristics only).
  ASSERT_TRUE(worker.Initialize());

  // The worker should still function: cache should be available.
  ASSERT_NE(worker.cache(), nullptr);

  // Run briefly to exercise the event loop.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send a CSS notification to verify the worker still processes normally.
  std::string url = "http://example.com/browser-analysis-test.css";
  std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for CSS processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "Worker should still process notifications after browser init failure";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Web Bot Auth Refresh Timer Tests
// =============================================================================

TEST_F(WorkerTest, WebBotAuthTimerSetupAndSharedConfigPublication) {
  // When --web-bot-auth is on and a key directory is configured, the worker
  // starts the refresh timer and publishes the toggle + directory hosts to
  // nginx via pagespeed-shared.conf.  The first refresh fires only after a
  // 5s initial delay, so this test never touches the network.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.web_bot_auth = true;
  config.web_bot_auth_key_directories = {
      "https://keys.example.com/.well-known/"
      "http-message-signatures-directory"};
  config.web_bot_auth_verified_bots = "kid1=crawler";

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Shared config carries the observe-only toggle end-to-end.
  SharedConfig shared = ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
  EXPECT_TRUE(shared.web_bot_auth);
  EXPECT_EQ(shared.web_bot_auth_verified_bots, "kid1=crawler");
  EXPECT_EQ(shared.web_bot_auth_directory_hosts, "keys.example.com");

  // Run briefly to exercise the event loop with the refresh timer active.
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_NE(worker.cache(), nullptr);
}

TEST_F(WorkerTest, WebBotAuthTimerNotSetWhenDisabled) {
  // Default off: no timer, and the shared config publishes web_bot_auth =
  // false so nginx does no signature work.  Negative control for the setup
  // test above.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // web_bot_auth stays false (default); directories configured but ignored.
  config.web_bot_auth_key_directories = {"https://keys.example.com/dir"};

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  SharedConfig shared = ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
  EXPECT_FALSE(shared.web_bot_auth);

  ASSERT_NE(worker.cache(), nullptr);
  worker.Shutdown();
}

// =============================================================================
// No license state, no telemetry identity
// =============================================================================

TEST_F(WorkerTest, InitializeWritesNoLicenseOrInstanceIdArtifacts) {
  // A 2.0 daemon persisted a license token (pagespeed.license) and a
  // telemetry instance id (pagespeed.instance-id) beside the cache, and
  // published license_key / license_valid / license_checked_once into
  // pagespeed-shared.conf.  A 2.1 daemon must create none of that, and the
  // daemon-authored file set must still be recognised so the cache-dir
  // ownership check keeps working.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::filesystem::path dir =
      std::filesystem::path(cache_path_).parent_path();
  EXPECT_FALSE(std::filesystem::exists(dir / "pagespeed.license"));
  EXPECT_FALSE(std::filesystem::exists(dir / "pagespeed.instance-id"));

  std::ifstream f(SharedConfigFilePath(cache_path_));
  ASSERT_TRUE(f.is_open());
  std::string raw((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  // Line-anchored: rsl_cap_requested_license= (RSL-CAP, unrelated) is a
  // legitimate key that contains the substring.
  for (const char* key :
       {"\nlicense_key=", "\nlicense_valid=", "\nlicense_checked_once="}) {
    EXPECT_EQ(raw.find(key), std::string::npos) << key << "\n" << raw;
  }
  EXPECT_EQ(raw.find("instance"), std::string::npos) << raw;

  ASSERT_NE(worker.cache(), nullptr);
  worker.Shutdown();
}

TEST_F(WorkerTest, AgentOptimizeServeTogglesFollowOperatorFlagsOnly) {
  // The agent_optimize markdown variant and the /llms.txt index
  // are switched on by their operator flags alone — there is no license
  // state anywhere for them to depend on.  The wire key keeps its historical
  // name (agent_optimize_entitled) so a 2.0-era reader keeps working.
  {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.browser_analysis.agent_optimize = true;
    config.browser_analysis.agent_optimize_llms_txt = true;

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());
    SharedConfig shared =
        ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
    EXPECT_TRUE(shared.agent_optimize_entitled);
    EXPECT_TRUE(shared.agent_optimize_llms_txt_enabled);
    worker.Shutdown();
  }
  {
    // Negative control: flags off → toggles off.  Same cache dir, so this
    // also proves the toggle is rewritten (not sticky) on every start.
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());
    SharedConfig shared =
        ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
    EXPECT_FALSE(shared.agent_optimize_entitled);
    EXPECT_FALSE(shared.agent_optimize_llms_txt_enabled);
    worker.Shutdown();
  }
}

// =============================================================================
// Cache Corruption Recovery Tests
// =============================================================================

// The file on disk is the structural-fingerprint sibling of the configured
// path, not the configured path itself (#428).  Whether that file is ever
// deleted by the recovery depends on who holds it:
//
// With no other process holding the volume, a damaged volume never reaches
// the recovery at all: the storage layer resets it in place inside Create()
// and the worker starts healthy.  This is what #428 looks like at this pin,
// and it needs no lock support, so the test below runs on every platform.
TEST_F(WorkerTest, DamagedVolumeWithNoPeerIsResetInPlace) {
  const uint64_t volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  std::string volume_file;
  {
    PageSpeedCacheConfig create_config;
    create_config.volume_path = cache_path_;
    create_config.volume_size = volume_size;
    auto created = PageSpeedCache::Create(create_config);
    ASSERT_TRUE(created.has_value());
    volume_file = (*created)->VolumeFilePath();
    ASSERT_FALSE(volume_file.empty());
    ASSERT_NE(volume_file, cache_path_)
        << "expected the fingerprinted volume path, not the configured one";
  }  // Closed: no peer holds the volume.

  // Truncate the real volume file to less than its 64-byte header: the
  // shape a full disk or a power loss leaves behind.
  std::error_code ec;
  std::filesystem::resize_file(volume_file, 16, ec);
  ASSERT_FALSE(ec) << "truncate failed: " << ec.message();
  ASSERT_EQ(std::filesystem::file_size(volume_file),
            static_cast<std::uintmax_t>(16));

  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = volume_size;

  NullMessageHandler handler;
  Worker worker(config, &handler);

  // The open resets the damaged volume in place: healthy start, no
  // recovery involved.
  EXPECT_TRUE(worker.Initialize())
      << "the worker should start healthy on a damaged volume with no peer";
  EXPECT_NE(worker.cache(), nullptr)
      << "the damaged volume should have been reset in place";

  // Full size again: the same file, re-initialized.
  EXPECT_EQ(std::filesystem::file_size(volume_file),
            static_cast<std::uintmax_t>(volume_size));

  // The reset cache is functional.
  std::string url = "http://example.com/recovery-test";
  std::string content = "recovered content";
  CacheOriginal(worker.cache(), url, content);
  CapabilityMask default_mask;
  std::string read_back = ReadVariant(worker.cache(), url, default_mask);
  EXPECT_EQ(read_back, content) << "the reset cache should be fully functional";

  worker.Shutdown();
}

// With a live peer holding the damaged volume, the storage layer needs to
// reset the volume and another process holds it, so it refuses on purpose
// (ResetRefusedLivePeer): the cure it names is to drain the peer, not to
// touch the volume.  The worker must start degraded and delete nothing: a
// delete-and-recreate would put the two processes on different volume files
// with no reset signal, and the peer (normally the web-server module) would
// keep writing an inode the worker never reads, silently losing its writes
// until the web server restarts (#428).
//
// The peer here is an ordinary PageSpeedCache on the same path, standing in
// for the web-server process that holds the volume while the worker
// restarts; its shared lifetime lock is what turns the worker's open of the
// truncated volume into the refusal.
//
// POSIX with OFD locks only: macOS has no F_OFD_SETLK, so the storage
// layer's reset gate degrades there and resets the damaged volume in place
// even under a peer.  No Windows CI lane runs worker_test today, and
// truncating a file another process has mapped is refused on Windows in any
// case, so the _WIN32 arm is deliberately absent.
#if defined(F_OFD_SETLK)
TEST_F(WorkerTest, CacheOpenUnderLivePeerStartsDegradedAndDeletesNothing) {
  const uint64_t volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  {
    PageSpeedCacheConfig peer_config;
    peer_config.volume_path = cache_path_;
    peer_config.volume_size = volume_size;
    auto peer = PageSpeedCache::Create(peer_config);
    ASSERT_TRUE(peer.has_value());
    const std::string volume_file = (*peer)->VolumeFilePath();
    ASSERT_FALSE(volume_file.empty());
    ASSERT_NE(volume_file, cache_path_)
        << "expected the fingerprinted volume path, not the configured one";

    // Truncate the real volume file to less than its 64-byte header: the
    // shape a full disk or a power loss leaves behind.
    std::error_code ec;
    std::filesystem::resize_file(volume_file, 16, ec);
    ASSERT_FALSE(ec) << "truncate failed: " << ec.message();
    ASSERT_EQ(std::filesystem::file_size(volume_file),
              static_cast<std::uintmax_t>(16));

    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = volume_size;

    NullMessageHandler handler;
    Worker worker(config, &handler);

    // Degraded, nothing deleted: the volume file is still the peer's,
    // untouched at its truncated size.
    EXPECT_TRUE(worker.Initialize())
        << "a live peer still starts the worker, degraded";
    EXPECT_EQ(worker.cache(), nullptr)
        << "a volume another process holds must not be recovered by deletion";
    EXPECT_TRUE(std::filesystem::exists(volume_file))
        << "the volume file a live peer holds was deleted";
    EXPECT_EQ(std::filesystem::file_size(volume_file),
              static_cast<std::uintmax_t>(16))
        << "the volume file a live peer holds was replaced";

    worker.Shutdown();
  }  // The peer closes last, on its own inode.
}
#endif  // defined(F_OFD_SETLK)

#ifndef _WIN32
// A symlink at the volume path is a refusal, not damage: the open reaches
// the mode check (which opens the path O_NOFOLLOW and sees ELOOP), Create()
// reports IoError, and the recovery must not delete anything -- a file this
// process was refused is not this process's to remove (#428).  POSIX only:
// the refusal is detected through O_NOFOLLOW/ELOOP and planting a symlink
// on Windows needs privileges.
TEST_F(WorkerTest, CacheOpenRefusalDoesNotDeleteTheVolumeFiles) {
  const uint64_t volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  std::string volume_file;
  {
    PageSpeedCacheConfig create_config;
    create_config.volume_path = cache_path_;
    create_config.volume_size = volume_size;
    auto created = PageSpeedCache::Create(create_config);
    ASSERT_TRUE(created.has_value());
    volume_file = (*created)->VolumeFilePath();
  }
  ASSERT_FALSE(volume_file.empty());

  // Replace the volume with a symlink to a copy of itself: the storage
  // layer opens the target through the link (a valid volume), and the mode
  // check refuses the symlink.
  const std::string decoy = volume_file + ".target";
  std::error_code ec;
  std::filesystem::copy_file(volume_file, decoy, ec);
  ASSERT_FALSE(ec) << "copy failed: " << ec.message();
  ASSERT_EQ(::unlink(volume_file.c_str()), 0);
  ASSERT_EQ(::symlink(decoy.c_str(), volume_file.c_str()), 0);

  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = volume_size;

  NullMessageHandler handler;
  Worker worker(config, &handler);

  // Degraded mode, not recovery: a refusal must not be "fixed" by deleting
  // the volume files.
  EXPECT_TRUE(worker.Initialize())
      << "a refusal still starts the worker, degraded";
  EXPECT_EQ(worker.cache(), nullptr)
      << "a refused volume must not be recovered by deletion";

  // Nothing was deleted: the symlink is still in place (a recovery would
  // have unlinked the name) and so is its target.
  EXPECT_TRUE(std::filesystem::is_symlink(volume_file))
      << "the symlinked volume name was deleted for a refusal";
  EXPECT_TRUE(std::filesystem::exists(decoy))
      << "the symlink target was deleted for a refusal";

  worker.Shutdown();
}
#endif  // !_WIN32

#ifndef _WIN32
// POSIX only: the cache-directory check is uid/mode-based, and on Windows
// (where ACLs are the access model and the service story is a separate arc)
// ValidateCacheDir always reports OK, so there is no refusal to observe.
TEST_F(WorkerTest, CacheDirectoryMissingRefusesToStart) {
  // A cache DIRECTORY that is missing, unwritable, or holds
  // content this daemon does not own is a refusal to start, not a degrade.
  // Nothing is chowned, nothing falls back to another location, and the
  // process exits non-zero so the supervisor's backoff and the operator's
  // monitoring both see it.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = "/nonexistent_dir_for_test/cache";
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);

  EXPECT_FALSE(worker.Initialize())
      << "A missing cache directory must be a refusal, never a silent "
         "cache-off start";
  EXPECT_EQ(worker.cache(), nullptr);
}
#endif  // !_WIN32

TEST_F(WorkerTest, CacheCorruptionRecoveryStartsDegradedWhenPathUnusable) {
  // When the cache VOLUME cannot be created even after the retry — the cache
  // directory itself being present, writable and owned — Initialize returns
  // true (degraded mode) with a null cache. Covers the degraded-mode startup
  // path, which survives the cache-directory check above: that check is about
  // the directory, this is about the volume inside it.
  //
  // A name longer than NAME_MAX makes both the initial Create and the retry
  // fail deterministically, without needing a directory the daemon may not
  // enter.
  WorkerConfig config;
  config.socket_path = socket_path_;
#ifdef _WIN32
  // Windows has no uid/mode directory check to satisfy, so the original
  // nonexistent-parent path still lands squarely in the degraded path.
  config.cache_path = "/nonexistent_dir_for_test/cache";
#else
  config.cache_path = temp_dir_ + "/" + std::string(300, 'x');
#endif
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);

  // Initialize should succeed in degraded mode: event loop still starts.
  EXPECT_TRUE(worker.Initialize())
      << "Worker should start in degraded mode when cache is unusable";
  EXPECT_EQ(worker.cache(), nullptr) << "Cache should be null in degraded mode";

  // Health should report DEGRADED
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket in degraded mode";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_TRUE(response.starts_with("DEGRADED "))
      << "Expected DEGRADED health, got: " << response;
  EXPECT_NE(response.find("cache=unavailable"), std::string::npos)
      << "Response should mention cache=unavailable: " << response;
}

// =============================================================================
// WebSocket Invalid Endpoint Tests
// =============================================================================

TEST_F(WorkerTest, WebSocketInvalidEndpointClosesConnection) {
  // When a WebSocket upgrade request arrives for an unknown endpoint
  // (not /v1/ws/stats, /v1/ws/events, or /v1/ws/logs), the worker
  // should close the connection.  Covers lines 738-750 (the else branch
  // of the WebSocket endpoint routing in the upgrade handler).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;  // OS picks a free port

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_GT(worker.api_port(), 0) << "HTTP API should be available";

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send a WebSocket upgrade request to an invalid endpoint path.
  std::string request =
      "GET /v1/ws/invalid-endpoint HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

  int sock = pagespeed::test::ConnectTcp(worker.api_port());
  ASSERT_GE(sock, 0) << "Failed to connect to API port " << worker.api_port();

  ssize_t written =
      pagespeed::test::SocketWrite(sock, request.data(), request.size());
  EXPECT_EQ(written, static_cast<ssize_t>(request.size()));

  // Wait for the server to process the upgrade and close the handle.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Try to read from the socket.  The server should have closed
  // the connection (invalid endpoint -> uv_close).  We expect either
  // 0 bytes (clean close) or an error.
  char buf[256];
  ssize_t nread = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
  // nread == 0 means the peer closed the connection (expected).
  // nread < 0 means error (also acceptable -- connection was reset).
  // nread > 0 would mean the server sent something, which is unexpected
  // for an invalid WS endpoint.
  //
  // Note: The HTTP server sends a 101 Switching Protocols response
  // before passing the handle to the upgrade handler.  So we may
  // receive the 101 response before the close.  Read it and then
  // check for close.
  if (nread > 0) {
    // We received the 101 response -- read again for the close.
    nread = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
  }
  EXPECT_LE(nread, 0) << "Connection should be closed for invalid WS endpoint";

  pagespeed::test::CloseSocket(sock);
}

TEST_F(WorkerTest, WebSocketValidEndpointsAccepted) {
  // Verify that the three valid WebSocket endpoints are recognized
  // and don't trigger the close-on-invalid-endpoint path.
  // Covers lines 740-745 (the three valid endpoint checks).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_GT(worker.api_port(), 0);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Test each valid WebSocket endpoint.
  std::vector<std::string> valid_endpoints = {"/v1/ws/stats", "/v1/ws/events",
                                              "/v1/ws/logs"};

  for (const auto& endpoint : valid_endpoints) {
    std::string request = "GET " + endpoint +
                          " HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: Upgrade\r\n"
                          "Upgrade: websocket\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";

    // ConnectTcp sets a 10s SO_RCVTIMEO by default; override to 1s.
    int sock =
        pagespeed::test::ConnectTcp(worker.api_port(), /*timeout_sec=*/1);
    ASSERT_GE(sock, 0) << "Failed to connect for endpoint " << endpoint;

    ssize_t written =
        pagespeed::test::SocketWrite(sock, request.data(), request.size());
    EXPECT_EQ(written, static_cast<ssize_t>(request.size()));

    // Wait briefly for the server to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // For valid endpoints, the server hands off to WsManager which
    // should accept the connection (send a 101 Switching Protocols).
    // We just verify the connection is not immediately closed.
    char buf[512];
    ssize_t nread = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
    // For valid endpoints we expect either data (101 response) or
    // a timeout (connection still open, no data yet).  An immediate
    // close (nread == 0) would indicate the endpoint was rejected.
    // Note: nread == -1 with EAGAIN/EWOULDBLOCK means timeout (good).
    if (nread == 0) {
      ADD_FAILURE() << "Valid endpoint " << endpoint
                    << " should not be immediately closed";
    }

    pagespeed::test::CloseSocket(sock);
  }
}

// With a token and --api-read-open, the worker's log handshake still
// succeeds -- a browser cannot carry Authorization on it -- but the stream
// behind it withholds every line until the token arrives in-band, exactly
// as WsManager::AcceptUpgrade enforces without read-open. The other stream
// stays pre-authenticated.
TEST_F(WorkerTest, ReadOpenLogStreamKeepsTheToken) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;  // OS picks a free port
  config.api_token = "worker-read-open-token";
  config.api_read_open = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_GT(worker.api_port(), 0);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Connects, sends the handshake, and returns {socket, response head} --
  // the caller keeps the socket open to watch what happens next (unlike a
  // helper that closes it, this test needs to keep reading from one path).
  auto handshake = [&worker](const std::string& path) {
    std::string request = "GET " + path +
                          " HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: Upgrade\r\n"
                          "Upgrade: websocket\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";
    int sock =
        pagespeed::test::ConnectTcp(worker.api_port(), /*timeout_sec=*/5);
    if (sock < 0) return std::make_pair(sock, std::string());
    pagespeed::test::SocketWrite(sock, request.data(), request.size());
    std::string head;
    char c;
    while (head.find("\r\n\r\n") == std::string::npos &&
           pagespeed::test::SocketRead(sock, &c, 1) == 1) {
      head.push_back(c);
    }
    return std::make_pair(sock, head);
  };

  // The anonymous log handshake succeeds: the gate does not ask for the
  // bearer header on this path, under read-open or otherwise.
  auto [logs_sock, logs_head] = handshake("/v1/ws/logs");
  ASSERT_GE(logs_sock, 0);
  EXPECT_NE(logs_head.find("101"), std::string::npos) << logs_head;

  // No in-band {"auth":...} is sent: once the auth timeout elapses
  // (WsConfig::auth_timeout_ms, 2000ms by default -- the Worker wires no
  // override) the only bytes on the wire are one close frame with code 4001
  // (unmasked, FIN + opcode 8, two-byte payload), then end of stream. Any
  // text frame would be log content reaching an unauthenticated visitor.
  std::this_thread::sleep_for(std::chrono::milliseconds(2200));
  char buf[256];
  std::string received;
  ssize_t nread;
  while ((nread = pagespeed::test::SocketRead(logs_sock, buf, sizeof(buf))) >
         0) {
    received.append(buf, static_cast<size_t>(nread));
  }
  const std::string close_4001("\x88\x02\x0f\xa1", 4);
  EXPECT_EQ(received, close_4001)
      << "the log stream sent more than the auth-timeout close frame to an "
         "unauthenticated read-open visitor ("
      << received.size() << " bytes)";
  pagespeed::test::CloseSocket(logs_sock);

  // The other stream stays pre-authenticated under read-open: its handshake
  // succeeds and a snapshot follows with no in-band auth needed.
  auto [stats_sock, stats_head] = handshake("/v1/ws/stats");
  ASSERT_GE(stats_sock, 0);
  EXPECT_NE(stats_head.find("101"), std::string::npos) << stats_head;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ssize_t stats_nread =
      pagespeed::test::SocketRead(stats_sock, buf, sizeof(buf));
  EXPECT_GT(stats_nread, 0)
      << "the stats stream sent no snapshot under read-open";
  pagespeed::test::CloseSocket(stats_sock);
}

}  // namespace
}  // namespace pagespeed
