// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// GET /v1/logs: a real HttpServer and the real WsManager log ring on one
// event loop, driven over TCP; plus direct reads of the ring on a loop the
// test thread runs itself.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "nlohmann/json.hpp"
#include "src/worker/api_handlers.h"
#include "src/worker/http_server.h"
#include "src/worker/ws_handlers.h"
#include "test/test_util/scoped_thread_join.h"
#include "test/test_util/tcp_client.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

bool IsStreamId(const json& value) {
  if (!value.is_string()) return false;
  const std::string& s = value.get_ref<const std::string&>();
  if (s.size() != 16) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// The status code of a raw "HTTP/1.1 NNN ..." response, or -1.
int StatusCode(const std::string& response) {
  if (response.compare(0, 9, "HTTP/1.1 ") != 0) return -1;
  return std::atoi(response.c_str() + 9);
}

std::string Body(const std::string& response) {
  const size_t start = response.find("\r\n\r\n");
  return start == std::string::npos ? std::string()
                                    : response.substr(start + 4);
}

// A strict parse: invalid UTF-8 or a torn document is "discarded".
json ParseJsonBody(const std::string& response) {
  return json::parse(Body(response), nullptr, /*allow_exceptions=*/false);
}

class LogHandlersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();

    // The ring owner.  PostLog accepts entries only once the manager runs.
    ws_config_.allow_unauthenticated = true;
    ws_config_.max_connections = 8;
    ws_config_.ping_interval_ms = 5000;
    ws_config_.pong_timeout_ms = 2000;
    ws_config_.auth_timeout_ms = 1000;
    ws_config_.stats_interval_ms = 200;
    ws_config_.event_batch_ms = 50;
    manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
    manager_->SetStatsProvider([]() -> json { return json::object(); });
    manager_->Start();

    ctx_ = std::unique_ptr<ApiContext>(new ApiContext{
        .stats = stats_,
        .get_config = [this]() -> std::shared_ptr<const WorkerConfig> {
          return config_;
        },
        .update_config = [](std::shared_ptr<const WorkerConfig>) {},
        .http_active_connections = []() { return 0; },
        .in_flight_work = []() { return 0; },
        .cache_entries = []() { return uint64_t{0}; },
        .cache_bytes = []() { return uint64_t{0}; },
        .cache_degraded = []() { return false; },
        .num_threads = 1,
        .max_connections = 32,
        .start_time = std::chrono::steady_clock::now(),
        .serve_stats = nullptr,
        .browser_manager = nullptr,
        .browser_sandbox_state = nullptr,
        .syscall_filter_state = nullptr,
        .read_logs = [this](uint64_t since, bool has_since,
                            size_t limit) -> json {
          return manager_->BuildLogsResponse(since, has_since, limit);
        },
    });

    HttpServerConfig server_config;
    server_config.port = 0;  // OS picks.
    // An empty token fails closed by design; these fixtures exercise the
    // route, not the credential gate (sub-fixtures override this hook).
    server_config.allow_unauthenticated = true;
    ConfigureServer(server_config);
    server_ =
        std::make_unique<HttpServer>(loop_, server_config, handler_.get());
    RegisterOperationalRoutes(*server_, *ctx_);
    ASSERT_TRUE(server_->Start());
    StartLoopThread();
  }

  void TearDown() override {
    if (loop_thread_.joinable()) StopLoopThread();
    server_->Stop();
    manager_->Stop();
    uv_run(loop_, UV_RUN_DEFAULT);  // runs every pending close callback
    server_.reset();
    manager_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

  // Sub-fixtures change the server's credential settings here -- before the
  // server is built, never on a running loop.
  virtual void ConfigureServer(HttpServerConfig& config) { (void)config; }

  // The loop helpers of ApiHandlersIntegrationTest, exactly: the stop
  // handle is closed after the join, so TearDown's UV_RUN_DEFAULT returns.
  void StartLoopThread() {
    uv_async_init(loop_, &stop_async_,
                  [](uv_async_t* handle) { uv_stop(handle->loop); });
    loop_thread_ = std::thread([this] { uv_run(loop_, UV_RUN_DEFAULT); });
  }

  void StopLoopThread() {
    uv_async_send(&stop_async_);
    if (loop_thread_.joinable()) loop_thread_.join();
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
  }

  // Post n entries ("entry 0".."entry n-1") from the test thread and give
  // the loop time to drain them into the ring (the WS tests' cadence).
  void PostLogs(int n, const char* level = "info") {
    for (int i = 0; i < n; ++i) {
      manager_->PostLog(level, "worker", "worker",
                        "entry " + std::to_string(i));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // Read until the ring's newest entry is `seq`: a wait that ends on
  // progress, bounded by a number of reads rather than by the clock alone.
  // False if the ring never got there, or at once if a read got no answer
  // (each of those already cost the client's whole receive timeout).
  bool WaitForNewestSeq(uint64_t seq) {
    for (int attempt = 0; attempt < 500; ++attempt) {
      const std::string resp = Get("/v1/logs?limit=1");
      if (resp.empty()) return false;
      json j = ParseJsonBody(resp);
      if (j.is_object() && j["entries"].size() == 1 &&
          j["newest_seq"].get<uint64_t>() == seq) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  std::string SendRequest(const std::string& request) {
    return test::SendRequest(server_->bound_port(), request, 5);
  }

  std::string Get(const std::string& target) {
    return SendRequest("GET " + target +
                       " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  }

  uv_loop_t* loop_ = nullptr;
  uv_async_t stop_async_;
  std::thread loop_thread_;
  std::unique_ptr<NullMessageHandler> handler_;
  WsConfig ws_config_;
  std::unique_ptr<WsManager> manager_;
  WorkerStats stats_;
  // A default config, so the other GET routes (e.g. /v1/config) answer.
  std::shared_ptr<const WorkerConfig> config_ =
      std::make_shared<WorkerConfig>();
  std::unique_ptr<ApiContext> ctx_;
  std::unique_ptr<HttpServer> server_;
};

TEST_F(LogHandlersTest, DefaultsReturnNewestPageAscending) {
  PostLogs(3);
  std::string resp = Get("/v1/logs");
  EXPECT_EQ(StatusCode(resp), 200);
  EXPECT_NE(resp.find("application/json"), std::string::npos);
  json j = ParseJsonBody(resp);
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["entries"].size(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    const json& e = j["entries"][i];
    EXPECT_EQ(e.at("seq").get<uint64_t>(), i + 1) << i;
    EXPECT_EQ(e["type"], "log");
    EXPECT_EQ(e["level"], "info");
    EXPECT_EQ(e["source"], "worker");
    EXPECT_EQ(e["module"], "worker");
    EXPECT_EQ(e["message"], "entry " + std::to_string(i));
    EXPECT_TRUE(e.contains("timestamp"));
  }
  EXPECT_EQ(j["next_since"].get<uint64_t>(), 3u);
  EXPECT_EQ(j["oldest_seq"].get<uint64_t>(), 1u);
  EXPECT_EQ(j["newest_seq"].get<uint64_t>(), 3u);
  EXPECT_FALSE(j["gap"].get<bool>());
  EXPECT_FALSE(j["more"].get<bool>());
  EXPECT_EQ(j["shed_total"].get<uint64_t>(), 0u);
  EXPECT_TRUE(IsStreamId(j["stream_id"])) << j["stream_id"];
}

TEST_F(LogHandlersTest, EmptyRing) {
  std::string resp = Get("/v1/logs");
  EXPECT_EQ(StatusCode(resp), 200);
  json j = ParseJsonBody(resp);
  ASSERT_TRUE(j.is_object());
  EXPECT_EQ(j["entries"].size(), 0u);
  EXPECT_EQ(j["next_since"].get<uint64_t>(), 0u);
  EXPECT_EQ(j["oldest_seq"].get<uint64_t>(), 0u);
  EXPECT_EQ(j["newest_seq"].get<uint64_t>(), 0u);
  EXPECT_FALSE(j["gap"].get<bool>());
  EXPECT_FALSE(j["more"].get<bool>());
  EXPECT_TRUE(IsStreamId(j["stream_id"])) << j["stream_id"];
}

TEST_F(LogHandlersTest, CursorFromEmptyReadReturnsFirstEntry) {
  // A first read of the empty ring hands out a cursor; the entries that
  // arrive after it must all be returned to a poll carrying that cursor.
  // (With numbering from 0, the empty read's next_since of 0 was also the
  // first entry's own seq, and seq > since skipped that entry forever.)
  json empty = ParseJsonBody(Get("/v1/logs"));
  ASSERT_TRUE(empty.is_object());
  ASSERT_EQ(empty["entries"].size(), 0u);
  const uint64_t cursor = empty["next_since"].get<uint64_t>();

  // Posted without PostLogs' fixed sleep: wait on progress (the ring's
  // newest seq) instead, so a loaded runner cannot read before the drain.
  for (int i = 0; i < 2; ++i) {
    manager_->PostLog("info", "worker", "worker", "entry " + std::to_string(i));
  }
  ASSERT_TRUE(WaitForNewestSeq(2));
  json j = ParseJsonBody(Get("/v1/logs?since=" + std::to_string(cursor)));
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["entries"].size(), 2u);
  // Numbering starts at 1: seq 0 is never assigned, which is what keeps the
  // empty ring's next_since of 0 unambiguous as "from the beginning".
  EXPECT_EQ(j["entries"][0]["seq"].get<uint64_t>(), 1u);
  EXPECT_EQ(j["entries"][0]["message"].get<std::string>(), "entry 0");
  EXPECT_EQ(j["entries"][1]["seq"].get<uint64_t>(), 2u);
  EXPECT_EQ(j["next_since"].get<uint64_t>(), 2u);
  EXPECT_EQ(j["oldest_seq"].get<uint64_t>(), 1u);
  EXPECT_FALSE(j["gap"].get<bool>());
  EXPECT_FALSE(j["more"].get<bool>());
}

TEST_F(LogHandlersTest, SinceFiltersOlderEntries) {
  PostLogs(5);
  json j = ParseJsonBody(Get("/v1/logs?since=2"));
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["entries"].size(), 3u);
  EXPECT_EQ(j["entries"][0]["seq"].get<uint64_t>(), 3u);
  EXPECT_EQ(j["entries"][1]["seq"].get<uint64_t>(), 4u);
  EXPECT_EQ(j["entries"][2]["seq"].get<uint64_t>(), 5u);
  EXPECT_EQ(j["next_since"].get<uint64_t>(), 5u);
  EXPECT_EQ(j["oldest_seq"].get<uint64_t>(), 1u);
  EXPECT_FALSE(j["gap"].get<bool>());
  EXPECT_FALSE(j["more"].get<bool>());
}

TEST_F(LogHandlersTest, SinceBeyondNewestReturnsEmptyPage) {
  // Also the shape a restarted optimizer gives a reader whose cursor came
  // from the previous process: an empty page echoing the cursor -- which is
  // why every page names its stream.
  PostLogs(3);
  json j = ParseJsonBody(Get("/v1/logs?since=99"));
  ASSERT_TRUE(j.is_object());
  EXPECT_EQ(j["entries"].size(), 0u);
  EXPECT_EQ(j["next_since"].get<uint64_t>(), 99u);  // the cursor survives
  EXPECT_EQ(j["newest_seq"].get<uint64_t>(), 3u);
  EXPECT_FALSE(j["gap"].get<bool>());
  EXPECT_FALSE(j["more"].get<bool>());
  EXPECT_EQ(j["stream_id"], manager_->stream_id());
}

TEST_F(LogHandlersTest, LimitIsRespected) {
  PostLogs(10);
  // No since: the NEWEST limit entries, ascending; nothing newer is left.
  json newest = ParseJsonBody(Get("/v1/logs?limit=3"));
  ASSERT_EQ(newest["entries"].size(), 3u);
  EXPECT_EQ(newest["entries"][0]["seq"].get<uint64_t>(), 8u);
  EXPECT_EQ(newest["entries"][2]["seq"].get<uint64_t>(), 10u);
  EXPECT_EQ(newest["next_since"].get<uint64_t>(), 10u);
  EXPECT_FALSE(newest["more"].get<bool>());

  // With since: the OLDEST matching entries, so a burst pages through, and
  // `more` says there is another page.
  json paged = ParseJsonBody(Get("/v1/logs?since=0&limit=3"));
  ASSERT_EQ(paged["entries"].size(), 3u);
  EXPECT_EQ(paged["entries"][0]["seq"].get<uint64_t>(), 1u);
  EXPECT_EQ(paged["entries"][2]["seq"].get<uint64_t>(), 3u);
  EXPECT_EQ(paged["next_since"].get<uint64_t>(), 3u);
  EXPECT_TRUE(paged["more"].get<bool>());

  json last = ParseJsonBody(Get("/v1/logs?since=7&limit=3"));
  ASSERT_EQ(last["entries"].size(), 3u);
  EXPECT_EQ(last["entries"][0]["seq"].get<uint64_t>(), 8u);
  EXPECT_EQ(last["next_since"].get<uint64_t>(), 10u);
  EXPECT_FALSE(last["more"].get<bool>());
}

TEST_F(LogHandlersTest, LimitDefaultsAndClamps) {
  PostLogs(505);
  // Absent -> 500 (the newest 500 of 505: seqs 6..505).
  json def = ParseJsonBody(Get("/v1/logs"));
  ASSERT_EQ(def["entries"].size(), 500u);
  EXPECT_EQ(def["entries"][0]["seq"].get<uint64_t>(), 6u);
  EXPECT_EQ(def["next_since"].get<uint64_t>(), 505u);

  // Over the maximum -> clamped to 500, not a 400.
  json clamped = ParseJsonBody(Get("/v1/logs?limit=600"));
  ASSERT_EQ(clamped["entries"].size(), 500u);
  EXPECT_EQ(clamped["entries"][0]["seq"].get<uint64_t>(), 6u);

  // Zero -> the default, mirroring /v1/cache/urls.
  json zero = ParseJsonBody(Get("/v1/logs?limit=0"));
  ASSERT_EQ(zero["entries"].size(), 500u);

  json seven = ParseJsonBody(Get("/v1/logs?limit=7"));
  ASSERT_EQ(seven["entries"].size(), 7u);
  EXPECT_EQ(seven["entries"][0]["seq"].get<uint64_t>(), 499u);
}

TEST_F(LogHandlersTest, MalformedParamsAre400) {
  PostLogs(1);
  for (const char* target :
       {"/v1/logs?since=abc", "/v1/logs?since=-1", "/v1/logs?since=1x",
        "/v1/logs?since=1.5", "/v1/logs?since=18446744073709551616",
        "/v1/logs?limit=xyz", "/v1/logs?limit=5.5", "/v1/logs?limit=-2"}) {
    std::string resp = Get(target);
    EXPECT_EQ(StatusCode(resp), 400) << target;
    EXPECT_NE(resp.find("BAD_REQUEST"), std::string::npos) << target;
  }
  // The body names the offending parameter (the /v1/cache/urls idiom).
  EXPECT_NE(Body(Get("/v1/logs?since=abc")).find("'since'"), std::string::npos);
  EXPECT_NE(Body(Get("/v1/logs?limit=xyz")).find("'limit'"), std::string::npos);
  // Documented leniency, harmless behind the module's strict gate: an empty
  // value reads as absent.
  EXPECT_EQ(StatusCode(Get("/v1/logs?since=")), 200);
  EXPECT_EQ(StatusCode(Get("/v1/logs?limit=")), 200);
}

TEST_F(LogHandlersTest, UnknownParamsAreIgnoredLikeOtherEndpoints) {
  // The strict parameter allow-list is the module proxy's layer; the
  // daemon's own endpoints ignore unknown query parameters.
  PostLogs(1);
  std::string resp = Get("/v1/logs?bogus=1");
  EXPECT_EQ(StatusCode(resp), 200);
  EXPECT_EQ(ParseJsonBody(resp)["entries"].size(), 1u);
}

TEST_F(LogHandlersTest, GapAfterRingWrap) {
  PostLogs(2050);  // the ring retains seqs 51..2050
  json gap = ParseJsonBody(Get("/v1/logs?since=10"));
  EXPECT_TRUE(gap["gap"].get<bool>());
  EXPECT_EQ(gap["oldest_seq"].get<uint64_t>(), 51u);
  EXPECT_EQ(gap["newest_seq"].get<uint64_t>(), 2050u);
  ASSERT_EQ(gap["entries"].size(), 500u);  // default page, oldest first
  EXPECT_EQ(gap["entries"][0]["seq"].get<uint64_t>(), 51u);
  EXPECT_EQ(gap["next_since"].get<uint64_t>(), 550u);
  EXPECT_TRUE(gap["more"].get<bool>());

  // since + 1 == oldest_seq: nothing was dropped, no gap.
  json contiguous = ParseJsonBody(Get("/v1/logs?since=50&limit=1"));
  EXPECT_FALSE(contiguous["gap"].get<bool>());
  ASSERT_EQ(contiguous["entries"].size(), 1u);
  EXPECT_EQ(contiguous["entries"][0]["seq"].get<uint64_t>(), 51u);

  // Up to date: empty page, no gap, cursor preserved, nothing more.
  json current = ParseJsonBody(Get("/v1/logs?since=2050"));
  EXPECT_EQ(current["entries"].size(), 0u);
  EXPECT_FALSE(current["gap"].get<bool>());
  EXPECT_FALSE(current["more"].get<bool>());
  EXPECT_EQ(current["next_since"].get<uint64_t>(), 2050u);
}

TEST_F(LogHandlersTest, HeadSendsNoBody) {
  PostLogs(2);
  std::string resp =
      SendRequest("HEAD /v1/logs HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_EQ(StatusCode(resp), 200);
  const size_t body_start = resp.find("\r\n\r\n");
  ASSERT_NE(body_start, std::string::npos);
  EXPECT_EQ(body_start + 4, resp.size());  // headers only, no body bytes
}

TEST_F(LogHandlersTest, InvalidUtf8MessageIsServedReplaced) {
  manager_->PostLog("warning", "worker", "worker", "bad \xFF\xFE byte");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string resp = Get("/v1/logs");
  ASSERT_EQ(StatusCode(resp), 200);  // the loop survived the bytes
  json j = ParseJsonBody(resp);      // strict parse: valid UTF-8 only
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["entries"].size(), 1u);
  EXPECT_EQ(j["entries"][0]["message"].get<std::string>(),
            "bad \xEF\xBF\xBD\xEF\xBF\xBD byte");
}

TEST_F(LogHandlersTest, OversizedMessagesStayWithinThePageBudget) {
  // 500 lines carrying 8 KiB each (a notification URL can be that long).
  // Unbounded, one page would be ~4 MiB; bounded, every page fits the
  // budget, and the cursor still walks the whole ring.
  const std::string long_line(8192, 'x');
  for (int i = 0; i < 500; ++i) {
    manager_->PostLog("info", "worker", "worker", long_line);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const std::string cut = std::string(kMaxLogMessageBytes, 'x') +
                          "\xE2\x80\xA6[truncated 4096 bytes]";

  // Without since: the newest entries that fit; nothing newer is left out.
  std::string resp = Get("/v1/logs?limit=500");
  ASSERT_EQ(StatusCode(resp), 200);
  EXPECT_LE(Body(resp).size(), kMaxLogsPageBytes);
  json newest = ParseJsonBody(resp);
  ASSERT_TRUE(newest.is_object());
  ASSERT_GE(newest["entries"].size(), 1u);
  EXPECT_LT(newest["entries"].size(), 500u);
  EXPECT_EQ(newest["entries"].back()["seq"].get<uint64_t>(), 500u);
  EXPECT_FALSE(newest["more"].get<bool>());
  EXPECT_EQ(newest["entries"][0]["message"].get<std::string>(), cut);

  // With since: page after page, each within the budget, contiguous, until
  // `more` is false -- the cursor always advances.
  uint64_t since = 0;
  uint64_t expect = 1;
  bool more = true;
  for (int page = 0; more && page < 50; ++page) {
    resp = Get("/v1/logs?since=" + std::to_string(since) + "&limit=500");
    ASSERT_EQ(StatusCode(resp), 200) << page;
    EXPECT_LE(Body(resp).size(), kMaxLogsPageBytes) << page;
    json j = ParseJsonBody(resp);
    ASSERT_TRUE(j.is_object()) << page;
    ASSERT_GE(j["entries"].size(), 1u) << page;
    for (const auto& e : j["entries"]) {
      EXPECT_EQ(e["seq"].get<uint64_t>(), expect++) << page;
    }
    const uint64_t next = j["next_since"].get<uint64_t>();
    EXPECT_GT(next, since) << page;
    since = next;
    more = j["more"].get<bool>();
  }
  EXPECT_FALSE(more);
  EXPECT_EQ(since, 500u);
}

TEST_F(LogHandlersTest, ConcurrentAppendsWhileReading) {
  // A reader never sees a torn or out-of-order page while another thread
  // appends.  The appender is bounded by the reader's progress, not by the
  // clock: each read grants it one burst, so the loop thread's share of the
  // work is the same on a fast host and on a starved sanitizer build.  (A
  // free-running appender keeps the pending queue at its bound, so every
  // loop iteration drains a full queue and one request costs several of
  // those -- seconds per request under a sanitizer on a loaded host.)
  //
  // The numbers: the ring is filled first, so every page is a full one and
  // every append evicts; 50 reads x 80 appends then turn the whole ring over
  // twice underneath the reader.  One burst is far below the pending-queue
  // bound, so nothing is shed and the sequence numbers are exact.
  constexpr uint64_t kRingCapacity = 2000;  // what the ring retains
  constexpr int kReads = 50;
  constexpr int kAppendsPerRead = 80;
  constexpr uint64_t kAppends = uint64_t{kReads} * kAppendsPerRead;
  for (uint64_t i = 0; i < kRingCapacity; ++i) {
    manager_->PostLog("info", "worker", "worker", "fill " + std::to_string(i));
  }
  ASSERT_TRUE(WaitForNewestSeq(kRingCapacity));

  std::mutex mutex;
  std::condition_variable wake;
  uint64_t granted = 0;  // appends the poster may have made so far
  bool done = false;     // no further grants; guarded by `mutex` like granted
  std::thread poster([&] {
    uint64_t posted = 0;
    for (;;) {
      uint64_t target;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return done || posted < granted; });
        if (posted == granted) return;  // done, and every grant is used
        target = granted;
      }
      for (; posted < target; ++posted) {
        manager_->PostLog("info", "worker", "worker",
                          "concurrent " + std::to_string(posted));
      }
    }
  });
  test::ScopedThreadJoin poster_guard(poster, [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = true;
    }
    wake.notify_one();
  });

  uint64_t last_newest = kRingCapacity;
  int mid_burst = 0;
  for (int req = 0; req < kReads; ++req) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      granted += kAppendsPerRead;
    }
    wake.notify_one();  // the burst lands while this read is in flight
    std::string resp = Get("/v1/logs?limit=500");
    ASSERT_EQ(StatusCode(resp), 200) << req;
    json j = ParseJsonBody(resp);
    ASSERT_TRUE(j.is_object()) << req;            // never a torn document
    ASSERT_EQ(j["entries"].size(), 500u) << req;  // the ring stays full
    uint64_t prev = 0;
    bool first = true;
    for (const auto& e : j["entries"]) {
      const uint64_t seq = e.at("seq").get<uint64_t>();
      if (!first) EXPECT_EQ(seq, prev + 1) << req;  // contiguous, ascending
      // One appender and nothing shed, so a seq names its message (seqs
      // start at 1: fill i carries seq i + 1).
      EXPECT_EQ(e.at("message").get<std::string>(),
                seq <= kRingCapacity
                    ? "fill " + std::to_string(seq - 1)
                    : "concurrent " + std::to_string(seq - kRingCapacity - 1))
          << req;
      prev = seq;
      first = false;
    }
    // The page ends at the newest entry, and the ring only moves forward.
    const uint64_t newest = j["newest_seq"].get<uint64_t>();
    EXPECT_EQ(prev, newest) << req;
    EXPECT_GE(newest, last_newest) << req;
    EXPECT_EQ(j["oldest_seq"].get<uint64_t>(), newest + 1 - kRingCapacity)
        << req;
    EXPECT_EQ(j["shed_total"].get<uint64_t>(), 0u) << req;
    // Bursts are whole multiples of kAppendsPerRead, so any other count of
    // appends in the ring means this page was built partway through one.
    if ((newest - kRingCapacity) % kAppendsPerRead != 0) ++mid_burst;
    last_newest = newest;
  }
  // Reported, not asserted: where a burst lands relative to a read is the
  // scheduler's choice, and no check above depends on it.
  GTEST_LOG_(INFO) << mid_burst << " of " << kReads
                   << " pages were built partway through a burst";

  // Every granted append arrives, in order, with none shed.
  poster_guard.StopAndJoin();
  EXPECT_TRUE(WaitForNewestSeq(kRingCapacity + kAppends));
  json last = ParseJsonBody(Get("/v1/logs?limit=1"));
  ASSERT_TRUE(last.is_object());
  ASSERT_EQ(last["entries"].size(), 1u);
  EXPECT_EQ(last["entries"][0]["message"].get<std::string>(),
            "concurrent " + std::to_string(kAppends - 1));
  EXPECT_EQ(last["shed_total"].get<uint64_t>(), 0u);
}

TEST_F(LogHandlersTest, ResponseShapeIsStable) {
  // CAPTURE POINT: the page for these two entries, byte for byte as served,
  // must equal testdata/logs_page.golden.json.  Clients copy that file as
  // their fixture; if this test changes, the file changes and every copy
  // follows it.
  manager_->PostLog("info", "worker", "worker", "cache flush complete");
  manager_->PostLog("warning", "cache", "cache", "origin fetch slow");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const std::string resp = Get("/v1/logs");
  ASSERT_EQ(StatusCode(resp), 200);
  std::string body = Body(resp);
  const json parsed = json::parse(body, nullptr, false);
  ASSERT_TRUE(parsed.is_object());
  ASSERT_EQ(parsed["entries"].size(), 2u);

  // Substitute exactly the live values, textually and in place, so every
  // other byte (key order, spacing, escaping) is the daemon's own: each
  // timestamp in order, then the stream id.
  const char* const kFixedTimestamps[] = {"1759230000000", "1759230000500"};
  size_t pos = 0;
  for (size_t i = 0; i < 2; ++i) {
    const std::string live =
        "\"timestamp\":" +
        std::to_string(parsed["entries"][i]["timestamp"].get<int64_t>());
    pos = body.find(live, pos);
    ASSERT_NE(pos, std::string::npos) << live;
    body.replace(pos, live.size(),
                 std::string("\"timestamp\":") + kFixedTimestamps[i]);
    ++pos;
  }
  const std::string live_id = "\"stream_id\":\"" + manager_->stream_id() + "\"";
  const size_t id_pos = body.find(live_id);
  ASSERT_NE(id_pos, std::string::npos) << live_id;
  body.replace(id_pos, live_id.size(), "\"stream_id\":\"9f2c4e1a7b3d5c80\"");

  // Keep the actual bytes for whoever has to re-capture the golden.
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    std::ofstream(std::string(dir) + "/logs_page.json", std::ios::binary)
        << body << '\n';
  }
  std::ifstream golden_file("test/src/worker/testdata/logs_page.golden.json",
                            std::ios::binary);
  ASSERT_TRUE(golden_file.is_open()) << "golden missing (BUILD data dep?)";
  const std::string golden((std::istreambuf_iterator<char>(golden_file)),
                           std::istreambuf_iterator<char>());
  EXPECT_EQ(golden, body + "\n")
      << "---- actual /v1/logs page (live values substituted) ----\n"
      << body << "\n---- end ----";
}

// The real registration under --api-read-open with a token: the other GET
// routes are open, /v1/logs is not.
class LogHandlersReadOpenTest : public LogHandlersTest {
 protected:
  void ConfigureServer(HttpServerConfig& config) override {
    config.allow_unauthenticated = false;
    config.auth_token = "secret-token";
    config.read_open = true;
  }
};

TEST_F(LogHandlersReadOpenTest, LogsNeedTheTokenWhileStatsStayOpen) {
  PostLogs(1);
  EXPECT_EQ(StatusCode(Get("/v1/config")), 200);  // read-open is in force...
  EXPECT_EQ(StatusCode(Get("/v1/logs")), 401);    // ...but not for the log
  EXPECT_EQ(StatusCode(SendRequest(
                "HEAD /v1/logs HTTP/1.1\r\nHost: localhost\r\n\r\n")),
            401);
  EXPECT_EQ(
      StatusCode(SendRequest("GET /v1/logs HTTP/1.1\r\nHost: localhost\r\n"
                             "Authorization: Bearer wrong\r\n\r\n")),
      403);
  const std::string ok = SendRequest(
      "GET /v1/logs HTTP/1.1\r\nHost: localhost\r\n"
      "Authorization: Bearer secret-token\r\n\r\n");
  EXPECT_EQ(StatusCode(ok), 200);
  EXPECT_EQ(ParseJsonBody(ok)["entries"].size(), 1u);
}

// Direct reads of the ring: the test thread runs the loop itself, so it IS
// the loop thread and may call BuildLogsResponse.
class LogRingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();
    WsConfig config;
    config.allow_unauthenticated = true;
    manager_ = std::make_unique<WsManager>(loop_, config, handler_.get());
    manager_->Start();  // PostLog accepts entries only while running
  }

  void TearDown() override {
    manager_->Stop();
    uv_run(loop_, UV_RUN_DEFAULT);  // runs the close callbacks
    manager_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

  // Lets the loop drain everything posted so far into the ring.
  void Drain() {
    for (int i = 0; i < 10; ++i) uv_run(loop_, UV_RUN_NOWAIT);
  }

  uv_loop_t* loop_ = nullptr;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<WsManager> manager_;
};

TEST_F(LogRingTest, StreamIdIsStablePerManager) {
  const std::string id = manager_->stream_id();
  EXPECT_TRUE(IsStreamId(id)) << id;
  EXPECT_EQ(manager_->BuildLogsResponse(0, false, 500)["stream_id"], id);
  manager_->PostLog("info", "worker", "worker", "one");
  Drain();
  EXPECT_EQ(manager_->BuildLogsResponse(0, true, 500)["stream_id"], id);
  // Another manager -- another process, as far as a reader can tell --
  // draws its own identity.
  WsManager other(loop_, WsConfig{}, handler_.get());
  EXPECT_TRUE(IsStreamId(other.stream_id())) << other.stream_id();
  EXPECT_NE(other.stream_id(), id);
}

TEST_F(LogRingTest, ShedEntriesAreCountedNotSequenced) {
  // Nothing drains until Drain(): the first 10000 posts fill the pending
  // queue and the last 50 are shed.
  for (int i = 0; i < 10050; ++i) {
    manager_->PostLog("info", "worker", "worker", "entry " + std::to_string(i));
  }
  Drain();
  json page = manager_->BuildLogsResponse(0, false, 500);
  EXPECT_EQ(page["shed_total"].get<uint64_t>(), 50u);
  // Shed posts burned no seq: 10000 entries, seqs 1..10000, the ring keeps
  // the newest 2000.
  EXPECT_EQ(page["newest_seq"].get<uint64_t>(), 10000u);
  EXPECT_EQ(page["oldest_seq"].get<uint64_t>(), 8001u);
  ASSERT_EQ(page["entries"].size(), 500u);
  EXPECT_EQ(page["entries"][0]["seq"].get<uint64_t>(), 9501u);
  // A cursor inside the ring maps straight to the next entry.
  json from = manager_->BuildLogsResponse(8999, true, 2);
  ASSERT_EQ(from["entries"].size(), 2u);
  EXPECT_EQ(from["entries"][0]["seq"].get<uint64_t>(), 9000u);
  EXPECT_EQ(from["entries"][1]["seq"].get<uint64_t>(), 9001u);
  EXPECT_FALSE(from["gap"].get<bool>());
  EXPECT_TRUE(from["more"].get<bool>());
}

}  // namespace
}  // namespace pagespeed
