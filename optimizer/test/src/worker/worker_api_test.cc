// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Worker HTTP API Integration Test
//
// Tests that the HTTP management API is correctly wired into the Worker
// process: server starts, routes respond, and graceful shutdown works.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "src/worker/serve_stats.h"
#include "src/worker/worker.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

class WorkerApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    handler_ = std::make_unique<NullMessageHandler>();

    // Create temp directory for sockets and cache.
    tmp_dir_ = test::MakeTempDir();
    std::filesystem::create_directories(tmp_dir_);

    cache_path_ = (tmp_dir_ / "test.cache").string();
#ifdef _WIN32
    // Windows Named Pipes need simple identifiers, not full filesystem paths.
    // Include \\.\pipe\ prefix so Worker::Initialize() doesn't double-prefix.
    socket_path_ = "\\\\.\\pipe\\ps_api_" +
                   std::to_string(std::hash<std::string>{}(cache_path_));
#else
    socket_path_ = (tmp_dir_ / "test.sock").string();
#endif

    // Create a small console directory for static file test.
    console_dir_ = (tmp_dir_ / "console").string();
    std::filesystem::create_directories(console_dir_);
    {
      std::ofstream ofs(console_dir_ + "/index.html", std::ios::binary);
      ofs << "<html>Test Console</html>";
    }
  }

  // Stops and joins the worker a test left running.  A fatal assertion
  // returns from the test body before its StopWorker() call; without this
  // the worker would be destroyed under its own running loop and the still
  // joinable thread would end the whole binary in std::terminate.
  void TearDown() override {
    StopWorker(worker_.get());
    worker_.reset();
    std::filesystem::remove_all(tmp_dir_);
  }

  // Start a Worker with the HTTP API enabled on a random port.  The fixture
  // owns it (one per test); the pointer is null if it failed to initialize.
  Worker* StartWorker(bool with_console = false, int num_threads = 0) {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.api_port = -1;  // Negative = OS picks a random port.
    // A tokenless API is fail-closed unless it was opened
    // deliberately.  main() refuses to start on the combination these tests
    // want; Worker itself does not, so say it here explicitly -- the API
    // under test is the loopback, deliberately-unauthenticated one.
    config.api_no_auth = true;
    config.num_threads = num_threads;
    if (with_console) {
      config.console_dir = console_dir_;
    }

    worker_ = std::make_unique<Worker>(config, handler_.get());
    if (!worker_->Initialize()) {
      worker_.reset();
      return nullptr;
    }

    // Run the event loop in a background thread.
    loop_thread_ = std::thread([&w = *worker_] { w.Run(); });
    // Wait for the API server to be ready (port bound).
    for (int i = 0; i < 20; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (worker_->api_port() > 0) break;
    }
    return worker_.get();
  }

  // Shuts the worker down and joins its loop thread.  Safe to call again
  // (TearDown does); the worker itself lives until TearDown.
  void StopWorker(Worker* worker) {
    if (worker) {
      worker->Shutdown();
      if (loop_thread_.joinable()) loop_thread_.join();
    }
  }

  std::string SendRequest(int port, const std::string& request) {
    // Retry connect — on Windows the server may not be accepting yet.
    int sock = -1;
    for (int i = 0; i < 5 && sock < 0; ++i) {
      sock = test::ConnectTcp(port, 2);
      if (sock < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, request.data(), request.size());
    if (sent != static_cast<ssize_t>(request.size())) {
      test::CloseSocket(sock);
      return "";
    }
    std::string response;
    char buf[4096];
    while (true) {
      ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
      if (n <= 0) break;
      response.append(buf, n);
    }
    test::CloseSocket(sock);
    return response;
  }

  std::filesystem::path tmp_dir_;
  std::string socket_path_;
  std::string cache_path_;
  std::string console_dir_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<Worker> worker_;
  std::thread loop_thread_;
};

TEST_F(WorkerApiTest, ApiDisabledByDefault) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  Worker worker(config, handler_.get());
  ASSERT_TRUE(worker.Initialize());
  EXPECT_EQ(worker.api_port(), 0);
  worker.Shutdown();
}

TEST_F(WorkerApiTest, HealthEndpoint) {
  Worker* worker = StartWorker();
  ASSERT_NE(worker, nullptr);
  int port = worker->api_port();
  ASSERT_GT(port, 0);

  auto resp =
      SendRequest(port, "GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("\"status\""), std::string::npos);

  StopWorker(worker);
}

TEST_F(WorkerApiTest, StatsEndpoint) {
  Worker* worker = StartWorker();
  ASSERT_NE(worker, nullptr);
  int port = worker->api_port();

  auto resp =
      SendRequest(port, "GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("\"received\""), std::string::npos);

  StopWorker(worker);
}

TEST_F(WorkerApiTest, MetricsEndpoint) {
  Worker* worker = StartWorker();
  ASSERT_NE(worker, nullptr);
  int port = worker->api_port();

  auto resp =
      SendRequest(port, "GET /v1/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("pagespeed_"), std::string::npos);

  StopWorker(worker);
}

TEST_F(WorkerApiTest, CacheAlternatesEndpoint) {
  Worker* worker = StartWorker();
  ASSERT_NE(worker, nullptr);
  int port = worker->api_port();

  auto resp = SendRequest(
      port,
      "GET /v1/cache/alternates?url=http%3A%2F%2Ftest.com%2F&hostname="
      "test.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  // No cached alternates → 404 is expected, but the endpoint must respond.
  EXPECT_FALSE(resp.empty()) << "Empty response from cache endpoint";
  EXPECT_NE(resp.find("HTTP/1.1"), std::string::npos);

  StopWorker(worker);
}

TEST_F(WorkerApiTest, ConsoleRoute) {
  Worker* worker = StartWorker(/*with_console=*/true);
  ASSERT_NE(worker, nullptr);
  int port = worker->api_port();

  auto resp =
      SendRequest(port, "GET /console/ HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("Test Console"), std::string::npos);

  StopWorker(worker);
}

TEST_F(WorkerApiTest, EventLoopLag) {
  Worker* worker = StartWorker();
  ASSERT_NE(worker, nullptr);

  // After a brief wait, event_loop_lag_us() should be available.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  // Lag should be very small (< 100ms) on a non-loaded system.
  EXPECT_LT(worker->event_loop_lag_us(), 100000u);

  StopWorker(worker);
}

TEST_F(WorkerApiTest, GracefulShutdown) {
  Worker* worker = StartWorker();
  ASSERT_NE(worker, nullptr);
  int port = worker->api_port();
  ASSERT_GT(port, 0);

  // Verify the server is up.
  auto resp =
      SendRequest(port, "GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  // Shutdown should complete cleanly.
  StopWorker(worker);

  // After shutdown, the port should be unbound.
  int sock = test::ConnectTcp(port, 2);
  EXPECT_LT(sock, 0);  // Connection should be refused.
  if (sock >= 0) test::CloseSocket(sock);
}

// The worker is a WRITER of the shared counting surface as of serve-stats v8:
// it stamps its pool width and samples its own backlog into the saturation
// group.  Read the file the way an out-of-process consumer would.
TEST_F(WorkerApiTest, ServeStatsSaturationBlockIsWrittenByTheWorker) {
  Worker* worker = StartWorker(/*with_console=*/false, /*num_threads=*/4);
  ASSERT_NE(worker, nullptr);

  const std::string stats_path = ServeStatsPath(cache_path_);
  ServeStats* observed = OpenServeStats(stats_path);
  ASSERT_NE(observed, nullptr) << "worker should have created " << stats_path;

  // Configuration is stamped at startup, before any sample exists — that is
  // what lets a reader tell "no samples yet" from "nobody writes this block".
  EXPECT_EQ(observed->worker_pool_threads, 4u);

  // Samples ride the worker's existing periodic timer (one per second), so
  // wait for the count to advance rather than assuming a cadence.
  uint64_t count = 0;
  for (int i = 0; i < 60 && count < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    count = observed->saturation_sample_count;
  }
  EXPECT_GE(count, 2u) << "the worker stopped sampling its backlog";

  // An idle worker samples a zero backlog: the count advances, the accumulator
  // and the high-water mark do not.  A count that advances while the load is
  // zero is exactly the signal a reader needs to trust an idle window.
  EXPECT_EQ(observed->saturation_sample_accum, 0u);
  EXPECT_EQ(observed->saturation_hwm, 0u);

  // Observe-only: no serve classification happens in this build, so the
  // serve-class counters stay at zero while the saturation group advances.
  EXPECT_EQ(observed->serve_optimized_total, 0u);
  EXPECT_EQ(observed->serve_original_pending_total, 0u);
  EXPECT_EQ(observed->notify_suppressed_total, 0u);

  CloseServeStats(observed);
  StopWorker(worker);
}

}  // namespace
}  // namespace pagespeed
