// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Management socket tests
//
// - Management socket commands: STATS, METRICS, PURGE and their auth rules
// - Stats and metrics output that reports worker counters
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <cstdint>
#include <span>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// =============================================================================
// Management Socket Tests (Feature 5)
// =============================================================================

TEST_F(WorkerTest, MgmtSocketExists) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Management socket/pipe endpoint should exist.
  std::string mgmt_path = worker.mgmt_socket_path();
  EXPECT_TRUE(pagespeed::test::PipeEndpointExists(mgmt_path));

  worker.Shutdown();
}

TEST_F(WorkerTest, MgmtStatsCommand) {
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

  // Connect to management socket and send STATS command.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read response
  char buf[2048];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from management socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);

  // Should be JSON with expected fields.
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("\"notifications\""), std::string::npos);
  // Both heal counters, as numbers, closing the notifications object.
  EXPECT_NE(response.find(R"("dedup_healed":0,"missing_copy_healed":0})"),
            std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("\"variants\""), std::string::npos);
  EXPECT_NE(response.find("\"by_type\""), std::string::npos);
  EXPECT_NE(response.find("\"by_format\""), std::string::npos);
  EXPECT_NE(response.find("\"timing_us\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsCommand) {
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

  // Connect to management socket and send METRICS command.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the metrics document no longer fits one
  // bufferful (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  // Should contain Prometheus text exposition format with expected metrics.
  EXPECT_NE(response.find("pagespeed_notifications_total"), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("pagespeed_variants_written_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_errors_total"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_cache_entries"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_connections_active"), std::string::npos);
  // Should contain TYPE annotations.
  EXPECT_NE(response.find("# TYPE pagespeed_notifications_total counter"),
            std::string::npos);
  EXPECT_NE(response.find("# TYPE pagespeed_cache_entries gauge"),
            std::string::npos);
  // Should contain format-labeled metrics.
  EXPECT_NE(response.find("pagespeed_format_generated_total{format=\"webp\"}"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_processing_seconds_total{type=\"image\"}"),
            std::string::npos);
}

TEST_F(WorkerTest, MgmtPurgeCommand) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable proactive variants to simplify: only generates requested format.
  config.proactive_image_variants = false;

  // PURGE requires authentication (fail-closed).
  setenv("PAGESPEED_PURGE_TOKEN", "purge-test-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS.  Use a hostname so PURGE can target it.
  std::string url = "/purge.css";
  std::string hostname = "example.com";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             css.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(css.data(), css.size())));
    (void)wh->close_sync();
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification to create a variant.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for variant to appear.
  CapabilityMask default_mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, default_mask, hostname);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "CSS variant not written before purge";

  // AUTH + PURGE on same connection (AUTH keeps connection alive).
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Send AUTH first.
  std::string auth_cmd = "AUTH purge-test-token\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());

  // Read AUTH response.
  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_EQ(std::string(buf, n), "OK\n") << "AUTH should succeed";

  // Send PURGE on the same authenticated connection.
  std::string cmd = "PURGE " + hostname + " " + url + "\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read PURGE response.
  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("OK"), std::string::npos) << "Response: " << response;

  // Wait for purge to take effect
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // The variant should be gone after purge.
  EXPECT_NE(response.find("deleted"), std::string::npos);

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeAuthCorrectToken) {
  // AUTH with correct token should allow PURGE on the same connection.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  // Set the purge token via environment variable.
  setenv("PAGESPEED_PURGE_TOKEN", "test-secret-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Write some content to purge.
  std::string url = "/auth-purge.css";
  std::string hostname = "auth.example.com";
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kOther;
    std::string data = "body { margin: 0; }";
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(data.data(), data.size())));
    (void)wh->close_sync();
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // AUTH keeps the connection alive, so AUTH+PURGE works on one connection.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Send AUTH with correct token.
  std::string auth_cmd = "AUTH test-secret-token\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_EQ(std::string(buf, n), "OK\n")
      << "AUTH with correct token should succeed";

  // PURGE on the same authenticated connection.
  std::string purge_cmd = "PURGE " + hostname + " " + url + "\n";
  pagespeed::test::PipeWrite(fd, purge_cmd.data(), purge_cmd.size());

  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("OK"), std::string::npos)
      << "PURGE after AUTH should succeed: " << response;
  EXPECT_NE(response.find("deleted"), std::string::npos)
      << "Response should include deletion count: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeAuthWrongToken) {
  // AUTH with wrong token should be rejected.
  WorkerConfig config;
  config.socket_path = socket_path_;

  setenv("PAGESPEED_PURGE_TOKEN", "correct-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "AUTH wrong-token\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR invalid token"), std::string::npos)
      << "Wrong token should be rejected: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeWithoutAuthRejected) {
  // PURGE without AUTH when token is configured should be rejected.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  setenv("PAGESPEED_PURGE_TOKEN", "some-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Try PURGE directly without AUTH.
  std::string cmd = "PURGE example.com /some-url\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR authentication required"), std::string::npos)
      << "PURGE without AUTH should fail: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtStatsWithoutAuth) {
  // STATS should work without AUTH even when token is configured.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  setenv("PAGESPEED_PURGE_TOKEN", "some-token", 1);

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
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[2048];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "STATS should work without AUTH: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeNoTokenConfigured) {
  // When no token is configured, PURGE should be rejected (fail-closed).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  // Ensure no token is set.
  unsetenv("PAGESPEED_PURGE_TOKEN");

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // PURGE without any token configured should be rejected.
  std::string cmd = "PURGE example.com /no-token-url\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR"), std::string::npos)
      << "PURGE should be rejected when no token configured: " << response;
}

TEST_F(WorkerTest, MgmtPurgeUrlTooLong) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_url_length = 32;  // Very short limit for testing.

  setenv("PAGESPEED_PURGE_TOKEN", "url-long-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // AUTH first.
  std::string auth_cmd = "AUTH url-long-token\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());
  char auth_buf[64];
  ssize_t auth_n =
      pagespeed::test::PipeRead(fd, auth_buf, sizeof(auth_buf) - 1);
  ASSERT_GT(auth_n, 0);

  // URL exceeding max_url_length should be rejected.
  std::string long_url(64, 'x');
  std::string cmd = "PURGE example.com /" + long_url + "\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR URL too long"), std::string::npos)
      << "Should reject long URL: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtUnknownCommand) {
  WorkerConfig config;
  config.socket_path = socket_path_;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "FOOBAR\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR"), std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsIncludesCompressedVariants) {
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
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the metrics document no longer fits one
  // bufferful (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(
      response.find("pagespeed_compressed_variants_total{encoding=\"gzip\"}"),
      std::string::npos)
      << "Missing gzip metric in: " << response;
  EXPECT_NE(
      response.find("pagespeed_compressed_variants_total{encoding=\"brotli\"}"),
      std::string::npos)
      << "Missing brotli metric in: " << response;
}

TEST_F(WorkerTest, MgmtStatsIncludesCompressedVariants) {
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
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"gzip\":"), std::string::npos)
      << "Missing gzip in STATS: " << response;
  EXPECT_NE(response.find("\"brotli\":"), std::string::npos)
      << "Missing brotli in STATS: " << response;
}

// =============================================================================
// Management Socket: BROWSER-STATUS command
// =============================================================================

TEST_F(WorkerTest, MgmtBrowserStatusDisabled) {
  // When browser analysis is not enabled, BROWSER-STATUS returns
  // {"enabled":false}.
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
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "BROWSER-STATUS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  char buf[1024];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from management socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("\"enabled\":false"), std::string::npos)
      << "Response: " << response;
}

// =============================================================================
// Management socket: multiple commands on same connection (keep-alive)
// =============================================================================

TEST_F(WorkerTest, MgmtStatsIncludesSvgCounters) {
  // The STATS JSON response includes SVG vectorization counters.
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
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"svg\""), std::string::npos)
      << "STATS should include SVG section. Response: " << response;
  EXPECT_NE(response.find("\"candidates_evaluated\""), std::string::npos);
  EXPECT_NE(response.find("\"vectorized\""), std::string::npos);
  EXPECT_NE(response.find("\"written\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtStatsIncludesAlternateWriteCounters) {
  // STATS JSON includes alternate write/failure counters and selector
  // invocations.
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
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"alternates\""), std::string::npos)
      << "STATS should include alternates section. Response: " << response;
  EXPECT_NE(response.find("\"write_failures\""), std::string::npos);
  EXPECT_NE(response.find("\"selector_invocations\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtStatsIncludesContentAnalysis) {
  // STATS JSON includes content analysis classification counters.
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
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"content_analysis\""), std::string::npos)
      << "STATS should include content_analysis section";
  EXPECT_NE(response.find("\"photo\""), std::string::npos);
  EXPECT_NE(response.find("\"screenshot\""), std::string::npos);
  EXPECT_NE(response.find("\"illustration\""), std::string::npos);
  EXPECT_NE(response.find("\"denoised\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtStatsIncludesSsimulacra2) {
  // STATS JSON includes SSIMULACRA2 quality verification counters.
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
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"ssimulacra2\""), std::string::npos)
      << "STATS should include ssimulacra2 section";
  EXPECT_NE(response.find("\"checks\""), std::string::npos);
  EXPECT_NE(response.find("\"reencodes\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsIncludesSvgCounters) {
  // The METRICS Prometheus response includes SVG-related counters.
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

  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty());
  EXPECT_NE(response.find("pagespeed_svg_candidates_evaluated_total"),
            std::string::npos)
      << "METRICS should include SVG counters";
  EXPECT_NE(response.find("pagespeed_svg_vectorized_total"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_svg_written_total"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_svg_bytes_saved_total"),
            std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsIncludesHtmlAssembly) {
  // METRICS includes html assembly outcome counters.
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

  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty());
  EXPECT_NE(response.find("pagespeed_html_assembly_total"), std::string::npos)
      << "METRICS should include html assembly counters";
  EXPECT_NE(response.find("pagespeed_alternate_writes_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_selector_invocations_total"),
            std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsIncludesContentAnalysis) {
  // METRICS includes content analysis classification counters.
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

  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty());
  EXPECT_NE(response.find("pagespeed_content_analysis_total"),
            std::string::npos)
      << "METRICS should include content analysis counters";
  EXPECT_NE(response.find("pagespeed_images_denoised_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_ssimulacra2_checks_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_ssimulacra2_reencodes_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_ssimulacra2_declines_total"),
            std::string::npos)
      << "a decline is a decision an operator must be able to see (#1381)";
  EXPECT_NE(response.find("pagespeed_ssimulacra2_decline_tombstone_hits_total"),
            std::string::npos)
      << "a tombstone skip is a decision an operator must be able to see "
         "(#1382)";
}

// =============================================================================
// MGMT PURGE with bad syntax
// =============================================================================

TEST_F(WorkerTest, MgmtPurgeBadSyntax) {
  // Authenticated PURGE with bad syntax returns an error.
  // Set env var BEFORE Initialize so the token is picked up.
  setenv("PAGESPEED_PURGE_TOKEN", "test-token-bad-syntax", 1);

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

  // AUTH first.
  std::string auth_cmd = "AUTH test-token-bad-syntax\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());

  char buf[1024];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_EQ(std::string(buf, n), "OK\n") << "AUTH should succeed";

  // Send PURGE with no arguments (bad syntax).
  std::string purge_cmd = "PURGE \n";
  pagespeed::test::PipeWrite(fd, purge_cmd.data(), purge_cmd.size());

  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string purge_response(buf, n);
  EXPECT_NE(purge_response.find("ERR"), std::string::npos)
      << "Bad PURGE syntax should return error. Response: " << purge_response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtAuthWithoutToken) {
  // AUTH when no purge token is configured should return
  // "ERR no token configured".
  WorkerConfig config;
  config.socket_path = socket_path_;

  // Ensure no token is set.
  unsetenv("PAGESPEED_PURGE_TOKEN");

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "AUTH sometoken\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No response from management socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR no token configured"), std::string::npos)
      << "AUTH without configured token should return error. Got: " << response;
}

// =============================================================================
// Management Socket Error Handling Tests
// =============================================================================

TEST_F(WorkerTest, MgmtSocketAbruptClose) {
  // Covers lines 3279-3289 (OnMgmtRead nread < 0 path).
  // When a client connects to the management socket and immediately
  // disconnects without sending data, the worker should handle the
  // EOF/error gracefully (nread < 0) and clean up the connection.
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

  // Connect and immediately close without sending anything.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";
  pagespeed::test::ClosePipe(fd);

  // Allow the worker to process the disconnect.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Worker should still be functional: connect again and send a command.
  fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0)
      << "Worker should still accept connections after abrupt close";

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  char buf[2048];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "Worker should still respond after handling abrupt close";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "Response: " << response;
}

TEST_F(WorkerTest, MgmtSocketBufferOverflow) {
  // Covers lines 3304-3313 (buffer > 16384 bytes without newline).
  // When a client sends more than 16384 bytes without a newline,
  // the management socket should forcibly disconnect the client
  // to prevent memory exhaustion.
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
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  // Send >16384 bytes of data without any newline to trigger overflow guard.
  std::string overflow_data(17000, 'A');
  ssize_t written = pagespeed::test::PipeWrite(fd, overflow_data.data(),
                                               overflow_data.size());
  // The write may succeed partially or fully depending on kernel buffers.
  EXPECT_GT(written, 0);

  // Give the worker time to process the data and close the connection.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Try to read - should get EOF or error since server closed connection.
  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf));
  EXPECT_LE(n, 0) << "Connection should be closed after buffer overflow";
  pagespeed::test::ClosePipe(fd);

  // Worker should still be functional after the overflow disconnect.
  fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Worker should still accept connections after overflow";

  std::string cmd = "STATS\n";
  written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "Worker should still respond after overflow disconnect";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "Response: " << response;
}

}  // namespace
}  // namespace pagespeed
