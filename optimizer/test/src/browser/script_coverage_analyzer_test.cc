// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Script Coverage Analyzer Tests
//
// Tests the deferral classification logic and struct defaults.
// CDP integration tests use mock pipe pairs.

#include "src/browser/script_coverage_analyzer.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/page_analysis.h"
#include "test/test_util/cdp_pipe.h"
#include "test/test_util/cdp_setup_order.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

// ---- DeferralAdvice classification unit tests ----

TEST(ScriptClassifyTest, AlreadyAsync) {
  ScriptInfo info;
  info.url = "https://example.com/analytics.js";
  info.is_async = true;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kAlreadyAsync);
}

TEST(ScriptClassifyTest, AlreadyDefer) {
  ScriptInfo info;
  info.url = "https://example.com/app.js";
  info.is_defer = true;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kAlreadyAsync);
}

TEST(ScriptClassifyTest, AlreadyModule) {
  ScriptInfo info;
  info.url = "https://example.com/main.mjs";
  info.is_module = true;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kAlreadyAsync);
}

TEST(ScriptClassifyTest, HasDocumentWrite) {
  ScriptInfo info;
  info.url = "https://example.com/legacy.js";
  info.has_document_write = true;
  info.coverage_at_fcp = 0.0f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

TEST(ScriptClassifyTest, ModifiesDomBeforePaint) {
  ScriptInfo info;
  info.url = "https://example.com/dom-setup.js";
  info.modifies_dom_before_paint = true;
  info.coverage_at_fcp = 0.0f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

TEST(ScriptClassifyTest, SafeToDefer) {
  ScriptInfo info;
  info.url = "https://example.com/tracking.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.0f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kSafeToDefer);
}

// Regression: the default-coverage ScriptInfo shape (no profiler evidence)
// must NEVER classify deferrable.
TEST(ScriptClassifyTest, ZeroCoverageWithoutEvidenceIsNoCoverageData) {
  ScriptInfo info;
  info.url = "https://example.com/tracking.js";
  info.coverage_at_fcp = 0.0f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kNoCoverageData);
}

TEST(ScriptClassifyTest, CandidateForAsync) {
  ScriptInfo info;
  info.url = "https://example.com/widget.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.05f;  // 5% < 10%
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kCandidateForAsync);
}

TEST(ScriptClassifyTest, CandidateForAsyncAtBoundary) {
  // Exactly 9.9% should still be a candidate.
  ScriptInfo info;
  info.url = "https://example.com/light.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.099f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kCandidateForAsync);
}

TEST(ScriptClassifyTest, KeepSynchronousHighCoverage) {
  ScriptInfo info;
  info.url = "https://example.com/critical.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.50f;  // 50% coverage
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

TEST(ScriptClassifyTest, KeepSynchronousAt10Percent) {
  // Exactly 10% is NOT less than 10%, so keep synchronous.
  ScriptInfo info;
  info.url = "https://example.com/borderline.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.10f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

TEST(ScriptClassifyTest, AsyncTakesPrecedenceOverDocWrite) {
  // If script is already async, classify as kAlreadyAsync even
  // if it has document.write (the async attribute is the existing
  // state, not our recommendation).
  ScriptInfo info;
  info.url = "https://example.com/weird.js";
  info.is_async = true;
  info.has_document_write = true;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kAlreadyAsync);
}

TEST(ScriptClassifyTest, DocumentWriteBlocksDefer) {
  // Even with 0% coverage, document.write prevents deferral.
  ScriptInfo info;
  info.url = "https://example.com/old-ad.js";
  info.coverage_at_fcp = 0.0f;
  info.has_document_write = true;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

TEST(ScriptClassifyTest, CandidateForAsyncNoDocWrite) {
  // Low coverage and no document.write -> candidate for async.
  ScriptInfo info;
  info.url = "https://example.com/chat.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.03f;
  info.has_document_write = false;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kCandidateForAsync);
}

// ---- Struct default tests ----

TEST(ScriptInfoTest, DefaultValues) {
  ScriptInfo info;
  EXPECT_TRUE(info.url.empty());
  EXPECT_TRUE(info.selector.empty());
  EXPECT_FALSE(info.is_inline);
  EXPECT_FALSE(info.is_async);
  EXPECT_FALSE(info.is_defer);
  EXPECT_FALSE(info.is_module);
  EXPECT_FLOAT_EQ(info.coverage_at_fcp, 0.0f);
  EXPECT_FLOAT_EQ(info.coverage_at_load, 0.0f);
  EXPECT_FALSE(info.has_document_write);
  EXPECT_FALSE(info.modifies_dom_before_paint);
  // The load-bearing default: no evidence unless the profiler joined.
  EXPECT_FALSE(info.has_coverage_data);
}

TEST(ScriptCoverageResultTest, DefaultValues) {
  ScriptCoverageResult result;
  EXPECT_TRUE(result.scripts.empty());
  EXPECT_TRUE(result.recommendations.empty());
  EXPECT_EQ(result.total_script_bytes, 0u);
  EXPECT_EQ(result.fcp_used_bytes, 0u);
  EXPECT_FLOAT_EQ(result.fcp_coverage_ratio, 0.0f);
  EXPECT_EQ(result.fetches_served, 0u);
  EXPECT_EQ(result.fetches_blocked, 0u);
}

// ---- Multiple scripts classification test ----

TEST(ScriptClassifyTest, MixedScriptsList) {
  // Simulate a realistic page with multiple scripts.
  std::vector<ScriptInfo> scripts;

  ScriptInfo analytics;
  analytics.url = "analytics.js";
  analytics.is_async = true;
  scripts.push_back(analytics);

  ScriptInfo framework;
  framework.url = "react.js";
  framework.has_coverage_data = true;
  framework.coverage_at_fcp = 0.45f;
  scripts.push_back(framework);

  ScriptInfo tracker;
  tracker.url = "tracker.js";
  tracker.has_coverage_data = true;
  tracker.coverage_at_fcp = 0.0f;
  scripts.push_back(tracker);

  ScriptInfo chat;
  chat.url = "chat-widget.js";
  chat.has_coverage_data = true;
  chat.coverage_at_fcp = 0.02f;
  scripts.push_back(chat);

  ScriptInfo legacy;
  legacy.url = "legacy-ads.js";
  legacy.has_document_write = true;
  scripts.push_back(legacy);

  std::vector<DeferralAdvice> expected = {
      DeferralAdvice::kAlreadyAsync,       // analytics: async
      DeferralAdvice::kKeepSynchronous,    // react: 45% coverage
      DeferralAdvice::kSafeToDefer,        // tracker: 0% coverage
      DeferralAdvice::kCandidateForAsync,  // chat: 2% coverage
      DeferralAdvice::kKeepSynchronous,    // legacy: document.write
  };

  for (size_t i = 0; i < scripts.size(); ++i) {
    EXPECT_EQ(ScriptCoverageAnalyzer::Classify(scripts[i]), expected[i])
        << "Script " << scripts[i].url;
  }
}

// ---- Additional coverage: boundary conditions and aggregation ----

TEST(ScriptClassifyTest, ZeroCoverageNoFlagsIsSafeToDefer) {
  ScriptInfo info;
  info.url = "https://example.com/idle.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.0f;
  info.has_document_write = false;
  info.modifies_dom_before_paint = false;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kSafeToDefer);
}

// Regression: the same flag-free zero-coverage shape WITHOUT evidence is
// exactly what every fetch-blocked script looks like — never defer.
TEST(ScriptClassifyTest, ZeroCoverageNoFlagsWithoutEvidenceIsNoCoverageData) {
  ScriptInfo info;
  info.url = "https://example.com/idle.js";
  info.coverage_at_fcp = 0.0f;
  info.has_document_write = false;
  info.modifies_dom_before_paint = false;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kNoCoverageData);
}

TEST(ScriptClassifyTest, FullCoverageKeepsSynchronous) {
  ScriptInfo info;
  info.url = "https://example.com/critical-render.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 1.0f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

TEST(ScriptClassifyTest, DeferTakesPrecedenceOverDocWrite) {
  // If script is already defer, classify as kAlreadyAsync regardless
  // of document.write or DOM mutations.
  ScriptInfo info;
  info.url = "https://example.com/deferred-legacy.js";
  info.is_defer = true;
  info.has_document_write = true;
  info.modifies_dom_before_paint = true;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kAlreadyAsync);
}

TEST(ScriptClassifyTest, ModuleTakesPrecedenceOverHighCoverage) {
  ScriptInfo info;
  info.url = "https://example.com/app.mjs";
  info.is_module = true;
  info.coverage_at_fcp = 0.95f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kAlreadyAsync);
}

TEST(ScriptClassifyTest, DomMutationBlocksDefer) {
  ScriptInfo info;
  info.url = "https://example.com/setup.js";
  info.modifies_dom_before_paint = true;
  info.coverage_at_fcp = 0.0f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

TEST(ScriptClassifyTest, JustBelowAsyncBoundary) {
  ScriptInfo info;
  info.url = "https://example.com/near-boundary.js";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.0999f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kCandidateForAsync);
}

TEST(ScriptInfoTest, InlineScriptNoUrl) {
  ScriptInfo info;
  info.selector = "body > script:nth-child(3)";
  info.has_coverage_data = true;
  info.coverage_at_fcp = 0.5f;
  EXPECT_TRUE(info.url.empty());
  EXPECT_FALSE(info.selector.empty());
  // Inline scripts with high coverage should stay synchronous.
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kKeepSynchronous);
}

// Regression: an inline-shaped ScriptInfo without profiler evidence (the
// common case — V8 collapses inline coverage onto the document URL, so no
// per-element join exists) classifies kNoCoverageData, never a defer verdict.
TEST(ScriptInfoTest, InlineScriptNoUrlWithoutEvidenceIsNoCoverageData) {
  ScriptInfo info;
  info.selector = "body > script:nth-child(3)";
  info.is_inline = true;
  info.coverage_at_fcp = 0.5f;
  EXPECT_EQ(ScriptCoverageAnalyzer::Classify(info),
            DeferralAdvice::kNoCoverageData);
}

// ---- CDP-based Analyze() tests ----

class ScriptCoverageAnalyzerCdpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    ASSERT_EQ(0, uv_loop_init(loop_));

    // Chrome -> Client pipe pair.
    ASSERT_TRUE(chrome_to_client_.Create());
    // Client -> Chrome pipe pair.
    ASSERT_TRUE(client_to_chrome_.Create());

    // Raw FD for test to write simulated Chrome responses.
    chrome_write_fd_ = chrome_to_client_.write_fd;

    // Client-side pipe handles (wrapped in libuv).
    uv_pipe_init(loop_, &read_pipe_, 0);
    uv_pipe_open(&read_pipe_, chrome_to_client_.TakeReadFd());

    uv_pipe_init(loop_, &client_write_, 0);
    uv_pipe_open(&client_write_, client_to_chrome_.TakeWriteFd());

    // Raw FD for test to read client-to-chrome commands (non-blocking).
    test_read_fd_ = client_to_chrome_.read_fd;
    ASSERT_TRUE(test::SetNonBlocking(test_read_fd_));

    client_ = std::make_unique<CdpClient>(loop_);
    ASSERT_TRUE(client_->AttachPipes(&read_pipe_, &client_write_).ok());
  }

  void TearDown() override {
    client_.reset();

    // Close all libuv handles.
    uv_walk(
        loop_,
        [](uv_handle_t* h, void*) {
          if (!uv_is_closing(h)) uv_close(h, nullptr);
        },
        nullptr);
    uv_run(loop_, UV_RUN_DEFAULT);

    // Close test-owned raw FDs (not owned by libuv).
    chrome_to_client_.CloseWrite();
    client_to_chrome_.CloseRead();

    uv_loop_close(loop_);
    delete loop_;
  }

  // Send a CDP message from "Chrome" to the client via raw write().
  void SendFromChrome(const std::string& json_str) const {
    std::string msg = json_str;
    msg.push_back('\0');
    ssize_t n = test::PipeWrite(chrome_write_fd_, msg.data(), msg.size());
    (void)n;
  }

  // Poll for commands sent by the client (raw non-blocking read).
  void PollCommands() {
    char buf[4096];
    while (true) {
      ssize_t n = test::PipeRead(test_read_fd_, buf, sizeof(buf));
      if (n <= 0) break;
      received_data_.append(buf, static_cast<size_t>(n));
    }
    size_t pos;
    while ((pos = received_data_.find('\0')) != std::string::npos) {
      std::string msg = received_data_.substr(0, pos);
      received_data_.erase(0, pos + 1);
      try {
        received_commands_.push_back(json::parse(msg));
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }
    }
  }

  // Run the event loop briefly to process I/O.
  void RunLoop() {
    for (int i = 0; i < 5; i++) {
      if (uv_run(loop_, UV_RUN_NOWAIT) == 0) break;
    }
    PollCommands();
  }

  // Run the event loop until a condition is met or timeout.
  // Uses UV_RUN_NOWAIT + usleep to avoid UV_RUN_ONCE blocking
  // issues on Linux in Docker (epoll + pipe fd interaction).
  void RunLoopUntil(std::function<bool()> condition,
                    int max_iterations = 5000) {
    for (int i = 0; i < max_iterations; i++) {
      if (condition()) return;
      uv_run(loop_, UV_RUN_NOWAIT);
      PollCommands();
      test::SleepUs(1000);  // 1ms
    }
  }

  void RespondToCommand(int id, const json& result) {
    json response = {{"id", id}, {"result", result}};
    SendFromChrome(response.dump());
  }

  void RespondError(int id, int code, const std::string& msg) {
    json response = {{"id", id}, {"error", {{"code", code}, {"message", msg}}}};
    SendFromChrome(response.dump());
  }

  void SendEvent(const std::string& method, const json& params,
                 const std::string& session_id = "") {
    json event = {{"method", method}, {"params", params}};
    if (!session_id.empty()) {
      event["sessionId"] = session_id;
    }
    SendFromChrome(event.dump());
  }

  // Make Profiler coverage data for a single script.
  json MakeCoverageData(const std::string& script_id, const std::string& url,
                        size_t total, size_t used_start, size_t used_end) {
    json script;
    script["scriptId"] = script_id;
    script["url"] = url;
    json func;
    func["ranges"] = json::array({
        {{"startOffset", 0}, {"endOffset", total}, {"count", 0}},
        {{"startOffset", used_start}, {"endOffset", used_end}, {"count", 1}},
    });
    script["functions"] = json::array({func});
    return script;
  }

  // Make Runtime.evaluate response for script elements.
  json MakeScriptInfoResponse() {
    json scripts = json::array({
        {{"url", "https://example.com/app.js"},
         {"selector", "script#app"},
         {"is_async", false},
         {"is_defer", false},
         {"is_module", false},
         {"has_document_write", false}},
        {{"url", "https://example.com/tracking.js"},
         {"selector", "script#tracking"},
         {"is_async", false},
         {"is_defer", false},
         {"is_module", false},
         {"has_document_write", false}},
    });
    return scripts;
  }

  // Have the "browser" request a script, as it does for each script element
  // of the page: served when the URL is in the resource map, blocked otherwise.
  void RequestScript(const std::string& url, const std::string& request_id) {
    SendEvent("Fetch.requestPaused",
              {{"requestId", request_id}, {"request", {{"url", url}}}},
              "sess-1");
  }

  void AutoRespondAll() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Page.setLifecycleEventsEnabled" &&
          replay_blank_lifecycle_) {
        // What Chromium does: enabling lifecycle events
        // replays the current document's lifecycle, the blank tab's
        // networkIdle included, ahead of the reply.
        for (const char* name :
             {"commit", "DOMContentLoaded", "load", "networkIdle"}) {
          SendEvent("Page.lifecycleEvent",
                    {{"name", name}, {"loaderId", "blank-loader"}}, "sess-1");
        }
        RespondToCommand(id, json::object());
        continue;
      }
      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Profiler.takePreciseCoverage") {
        // Return coverage data for two scripts.
        json result = json::array({
            MakeCoverageData("s1", "https://example.com/app.js", 1000, 0, 500),
            MakeCoverageData("s2", "https://example.com/tracking.js", 500, 0,
                             0),
        });
        RespondToCommand(id, {{"result", result}});
      } else if (method == "Runtime.evaluate") {
        json scripts = MakeScriptInfoResponse();
        RespondToCommand(
            id, {{"result", {{"type", "string"}, {"value", scripts.dump()}}}});
      } else if (method == "Fetch.failRequest" && refuse_fail_request_) {
        // What Chromium answered to the `reason` parameter.
        json err = {
            {"id", id},
            {"error", {{"code", -32602}, {"message", "Invalid parameters"}}}};
        SendFromChrome(err.dump());
      } else if (method == "Fetch.failRequest" ||
                 method ==
                     "Fetch.fulfillRequest") {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      } else {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      }
    }
  }

  // Replay the blank tab's lifecycle on Page.setLifecycleEventsEnabled, as
  // Chromium does.
  bool replay_blank_lifecycle_ = false;
  // Answer Fetch.failRequest with an error, as Chromium answered `reason`.
  bool refuse_fail_request_ = false;
  uv_loop_t* loop_ = nullptr;
  test::PipePair chrome_to_client_;
  test::PipePair client_to_chrome_;
  uv_pipe_t read_pipe_{};
  uv_pipe_t client_write_{};
  int test_read_fd_ = -1;
  int chrome_write_fd_ = -1;
  std::unique_ptr<CdpClient> client_;
  bool analyze_done_ = false;
  absl::StatusOr<ScriptCoverageResult> analyze_result_;
  // Push the lifecycle events a render waits for (FCP, then networkIdle)
  // and pump until it reports, so the session releases itself.
  void FinishAnalysis() {
    SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
              "sess-1");
    for (int i = 0; i < 5; ++i) {
      AutoRespondAll();
      RunLoop();
    }
    SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
    for (int i = 0; i < 15; ++i) {
      AutoRespondAll();
      RunLoop();
      if (analyze_done_) break;
    }
  }

  std::vector<json> received_commands_;
  std::string received_data_;
  std::set<int> responded_ids_;
};

TEST_F(ScriptCoverageAnalyzerCdpTest, SuccessfulAnalysis) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto mutable_resources = std::make_shared<PageAnalyzer::ResourceMap>();
  (*mutable_resources)["https://example.com/app.js"] = "var a=1;";
  (*mutable_resources)["https://example.com/tracking.js"] = "var t=1;";
  auto resources = std::static_pointer_cast<const PageAnalyzer::ResourceMap>(
      mutable_resources);

  analyzer.Analyze("<html><body><script src='app.js'></script></body></html>",
                   resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analyze_done_) break;
  }

  RequestScript("https://example.com/app.js", "req-1");
  RequestScript("https://example.com/tracking.js", "req-2");
  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Send FCP.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Send networkIdle.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analyze_done_) break;
  }

  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  EXPECT_EQ(analyze_result_->scripts.size(), 2u);
  EXPECT_EQ(analyze_result_->recommendations.size(), 2u);

  // First script (app.js) has 50% coverage -> kKeepSynchronous.
  bool found_app = false;
  bool found_tracking = false;
  for (const auto& [url, advice] : analyze_result_->recommendations) {
    if (url == "https://example.com/app.js") {
      EXPECT_EQ(advice, DeferralAdvice::kKeepSynchronous);
      found_app = true;
    } else if (url == "https://example.com/tracking.js") {
      // tracking.js has 0% coverage -> kSafeToDefer.
      EXPECT_EQ(advice, DeferralAdvice::kSafeToDefer);
      found_tracking = true;
    }
  }
  EXPECT_TRUE(found_app);
  EXPECT_TRUE(found_tracking);
}

// Chromium replays the blank tab's networkIdle when lifecycle
// events are enabled, before the document is written. Script coverage takes nothing
// from the page before it is written: its first snapshot waits for the
// page's firstContentfulPaint, which is not replayed. (Its "load" snapshot
// may still count the replayed networkIdle: measured on the pinned Chromium
// 145, waiting for the page's own networkIdle instead changes none of its
// outputs and costs 0.5 s to 2 s per render.)
TEST_F(ScriptCoverageAnalyzerCdpTest, TakesNothingBeforeTheDocumentIsWritten) {
  replay_blank_lifecycle_ = true;
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html><body><script src='app.js'></script></body></html>",
                   resources, 375, 667,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  auto index_of = [this](const std::string& method) {
    for (size_t i = 0; i < received_commands_.size(); ++i) {
      if (received_commands_[i].value("method", "") == method) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  const int content = index_of("Page.setDocumentContent");
  ASSERT_GE(content, 0) << "the document was never written";
  EXPECT_EQ(index_of("Profiler.takePreciseCoverage"), -1)
      << "Profiler.takePreciseCoverage before the page painted";
  EXPECT_EQ(index_of("Runtime.evaluate"), -1)
      << "Runtime.evaluate before the page painted";
  EXPECT_FALSE(analyze_done_);

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 20 && !analyze_done_; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  ASSERT_TRUE(analyze_done_);
  EXPECT_TRUE(analyze_result_.ok()) << analyze_result_.status();
  EXPECT_GT(index_of("Profiler.takePreciseCoverage"), content)
      << "Profiler.takePreciseCoverage";
  EXPECT_GT(index_of("Runtime.evaluate"), content) << "Runtime.evaluate";
}

// The script-coverage render emulates the device the way every
// analysis render does (src/browser/device_emulation.h): the device metrics,
// then touch, on the render's session and before the profiler. A phone is a
// touch device.
TEST_F(ScriptCoverageAnalyzerCdpTest, EmulatesAPhoneAsATouchDevice) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html></html>", resources, 375, 667,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  int metrics = -1;
  int touch = -1;
  int profiler = -1;
  for (size_t i = 0; i < received_commands_.size(); ++i) {
    const std::string method = received_commands_[i].value("method", "");
    if (method == "Emulation.setDeviceMetricsOverride" && metrics < 0) {
      metrics = static_cast<int>(i);
    } else if (method == "Emulation.setTouchEmulationEnabled" && touch < 0) {
      touch = static_cast<int>(i);
    } else if (method == "Profiler.enable" && profiler < 0) {
      profiler = static_cast<int>(i);
    }
  }
  ASSERT_GE(metrics, 0) << "no device metrics";
  ASSERT_GE(touch, 0) << "no touch emulation: the render bypassed "
                         "EmulateDevice";
  ASSERT_GE(profiler, 0);
  EXPECT_LT(metrics, touch) << "touch follows the device metrics";
  EXPECT_LT(touch, profiler) << "the emulation precedes the profiler";

  const json metrics_params =
      received_commands_[metrics].value("params", json::object());
  EXPECT_EQ(metrics_params.value("width", 0), 375);
  EXPECT_EQ(metrics_params.value("height", 0), 667);
  EXPECT_TRUE(metrics_params.value("mobile", false));
  EXPECT_EQ(received_commands_[metrics].value("sessionId", ""), "sess-1");
  const json touch_params =
      received_commands_[touch].value("params", json::object());
  EXPECT_TRUE(touch_params.value("enabled", false));
  EXPECT_EQ(touch_params.value("maxTouchPoints", 0), 5);
  EXPECT_EQ(received_commands_[touch].value("sessionId", ""), "sess-1");
  // Then the phone's user agent, and a fresh window.
  test::ExpectFreshWindowRenderSetup(received_commands_, "sess-1", 375, 667);

  // Drive the render to completion so its session releases itself (the
  // client's callbacks hold it until then), as the other call-site tests do.
  FinishAnalysis();
  EXPECT_TRUE(analyze_done_);
}

// The desktop render is a window without touch; the call is still sent,
// disabled, so no target inherits a state.
TEST_F(ScriptCoverageAnalyzerCdpTest, EmulatesADesktopWindowWithoutTouch) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  bool found_metrics = false;
  bool found_touch = false;
  for (const auto& cmd : received_commands_) {
    const std::string method = cmd.value("method", "");
    if (method == "Emulation.setDeviceMetricsOverride") {
      EXPECT_FALSE(cmd.value("params", json::object()).value("mobile", true));
      found_metrics = true;
    } else if (method == "Emulation.setTouchEmulationEnabled") {
      EXPECT_TRUE(found_metrics) << "touch follows the device metrics";
      const json params = cmd.value("params", json::object());
      EXPECT_FALSE(params.value("enabled", true));
      EXPECT_EQ(params.value("maxTouchPoints", 0), 5);
      found_touch = true;
    }
  }
  EXPECT_TRUE(found_metrics);
  EXPECT_TRUE(found_touch);
  // No user agent override on the desktop; a fresh window still.
  test::ExpectFreshWindowRenderSetup(received_commands_, "sess-1", 1440, 900);
  FinishAnalysis();
  EXPECT_TRUE(analyze_done_);
}

TEST_F(ScriptCoverageAnalyzerCdpTest, CreateTargetFailure) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });

  RunLoop();
  RunLoopUntil([this] { return !received_commands_.empty(); });
  int id = received_commands_[0].value("id", 0);
  RespondError(id, -32000, "failed");

  RunLoopUntil([this] { return analyze_done_; }, 50);

  ASSERT_TRUE(analyze_done_);
  EXPECT_FALSE(analyze_result_.ok());
}

TEST_F(ScriptCoverageAnalyzerCdpTest, NetworkIdleBeforeFcp) {
  // Regression: if networkIdle fires before FCP, should wait for
  // FCP coverage before proceeding.
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html><body><script src='app.js'></script></body></html>",
                   resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Send networkIdle FIRST.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 3; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Then send FCP.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  for (int i = 0; i < 15; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analyze_done_) break;
  }

  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  EXPECT_EQ(analyze_result_->scripts.size(), 2u);
}

TEST_F(ScriptCoverageAnalyzerCdpTest, FetchServesCachedResource) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<PageAnalyzer::ResourceMap>();
  (*resources)["https://example.com/app.js"] = "console.log('hello');";
  auto const_resources =
      std::static_pointer_cast<const PageAnalyzer::ResourceMap>(resources);

  analyzer.Analyze(
      "<html><head><script src='https://example.com/app.js'></script></head>"
      "</html>",
      const_resources, 1440, 900,
      [this](absl::StatusOr<ScriptCoverageResult> result) {
        analyze_result_ = std::move(result);
        analyze_done_ = true;
      });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Simulate Fetch.requestPaused for the cached URL.
  SendEvent("Fetch.requestPaused",
            {{"requestId", "req-1"},
             {"request", {{"url", "https://example.com/app.js"}}}},
            "sess-1");

  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Verify Fetch.fulfillRequest was sent.
  bool found_fulfill = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "Fetch.fulfillRequest") {
      EXPECT_FALSE(
          cmd.value("params", json::object()).value("body", "").empty());
      found_fulfill = true;
      break;
    }
  }
  EXPECT_TRUE(found_fulfill);

  // Drive the session to completion; the counter assertion must not be
  // skippable by an early exit.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analyze_done_) break;
  }
  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  EXPECT_GE(analyze_result_->fetches_served, 1u);
}

TEST_F(ScriptCoverageAnalyzerCdpTest, FetchBlocksUnknownUrls) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Simulate Fetch for an unknown URL.
  SendEvent(
      "Fetch.requestPaused",
      {{"requestId", "req-1"}, {"request", {{"url", "http://evil.com/steal"}}}},
      "sess-1");

  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  bool found_fail = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "Fetch.failRequest") {
      // Fetch.failRequest's parameter is `errorReason`: Chromium rejects a
      // `reason` with "Invalid parameters" and leaves the request paused, so
      // the page never reaches load or networkIdle.
      EXPECT_EQ(cmd.value("params", json::object()).value("errorReason", ""),
                "BlockedByClient");
      EXPECT_FALSE(cmd.value("params", json::object()).contains("reason"));
      found_fail = true;
      break;
    }
  }
  EXPECT_TRUE(found_fail);

  // Drive the session to completion; the counter assertion must not be
  // skippable by an early exit.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analyze_done_) break;
  }
  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  EXPECT_GE(analyze_result_->fetches_blocked, 1u);
}

// The redirect leg of the Fetch interception (a response-stage 3xx is never
// followed) blocks through the same Fetch.failRequest, with Chromium's
// `errorReason`, even for a URL the resource map holds.
TEST_F(ScriptCoverageAnalyzerCdpTest, FetchBlocksRedirectResponses) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<PageAnalyzer::ResourceMap>();
  (*resources)["https://example.com/old.js"] = "var a = 1;";
  auto const_resources =
      std::static_pointer_cast<const PageAnalyzer::ResourceMap>(resources);
  analyzer.Analyze("<html></html>", const_resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  SendEvent("Fetch.requestPaused",
            {{"requestId", "req-redirect"},
             {"request", {{"url", "https://example.com/old.js"}}},
             {"responseStatusCode", 302},
             {"responseHeaders",
              {{{"name", "Location"}, {"value", "https://evil.test/new.js"}}}}},
            "sess-1");
  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  bool found_fail = false;
  bool found_fulfill = false;
  for (const auto& cmd : received_commands_) {
    const json params = cmd.value("params", json::object());
    if (params.value("requestId", "") != "req-redirect") continue;
    const std::string method = cmd.value("method", "");
    if (method == "Fetch.failRequest") {
      EXPECT_EQ(params.value("errorReason", ""), "BlockedByClient");
      EXPECT_FALSE(params.contains("reason"));
      EXPECT_EQ(cmd.value("sessionId", ""), "sess-1");
      found_fail = true;
    } else if (method == "Fetch.fulfillRequest") {
      found_fulfill = true;
    }
  }
  EXPECT_TRUE(found_fail);
  EXPECT_FALSE(found_fulfill) << "a redirect is never followed";

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15 && !analyze_done_; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  EXPECT_EQ(analyze_result_->fetches_blocked, 1u);
}

// A refused Fetch.failRequest (the request stays paused) is
// reported through the client's diagnostic handler, once per render, instead
// of being dropped unseen.
TEST_F(ScriptCoverageAnalyzerCdpTest, ARefusedFailRequestIsReportedOnce) {
  std::vector<std::string> reports;
  client_->set_diagnostic_handler(
      [&reports](std::string_view m) { reports.emplace_back(m); });
  refuse_fail_request_ = true;
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  for (const char* id : {"req-1", "req-2"}) {
    SendEvent("Fetch.requestPaused",
              {{"requestId", id}, {"request", {{"url", "https://x.test/a"}}}},
              "sess-1");
  }
  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  ASSERT_EQ(reports.size(), 1u) << "reported once per render";
  EXPECT_NE(reports[0].find("script coverage"), std::string::npos)
      << reports[0];
  EXPECT_NE(reports[0].find("Invalid parameters"), std::string::npos)
      << reports[0];

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15 && !analyze_done_; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  EXPECT_TRUE(analyze_done_);
  client_->set_diagnostic_handler(nullptr);
}

// Direct repro of the evidence gate: a script the DOM lists but the profiler
// never reported (its fetch was blocked — no cached bytes) must classify
// kNoCoverageData, NOT kSafeToDefer from the 0.0 coverage default.
TEST_F(ScriptCoverageAnalyzerCdpTest,
       BlockedScriptAbsentFromProfilerNotDeferred) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto mutable_resources = std::make_shared<PageAnalyzer::ResourceMap>();
  (*mutable_resources)["https://example.com/app.js"] = "var a=1;";
  auto resources = std::static_pointer_cast<const PageAnalyzer::ResourceMap>(
      mutable_resources);

  analyzer.Analyze(
      "<html><head><script src='https://example.com/app.js'></script>"
      "<script src='https://example.com/blocked.js'></script></head></html>",
      resources, 1440, 900,
      [this](absl::StatusOr<ScriptCoverageResult> result) {
        analyze_result_ = std::move(result);
        analyze_done_ = true;
      });

  auto custom = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Profiler.takePreciseCoverage") {
        // Coverage for app.js ONLY — blocked.js never compiled.
        json result = json::array({
            MakeCoverageData("s1", "https://example.com/app.js", 1000, 0, 500),
        });
        RespondToCommand(id, {{"result", result}});
      } else if (method == "Runtime.evaluate") {
        // The DOM still lists BOTH scripts.
        json scripts = json::array({
            {{"url", "https://example.com/app.js"},
             {"selector", "script"},
             {"is_inline", false},
             {"is_async", false},
             {"is_defer", false},
             {"is_module", false},
             {"has_document_write", false}},
            {{"url", "https://example.com/blocked.js"},
             {"selector", "script"},
             {"is_inline", false},
             {"is_async", false},
             {"is_defer", false},
             {"is_module", false},
             {"has_document_write", false}},
        });
        RespondToCommand(
            id, {{"result", {{"type", "string"}, {"value", scripts.dump()}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom();
    RunLoop();
  }
  RequestScript("https://example.com/app.js", "req-1");
  RequestScript("https://example.com/blocked.js", "req-2");
  for (int i = 0; i < 5; ++i) {
    custom();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  for (int i = 0; i < 5; ++i) {
    custom();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15; ++i) {
    custom();
    RunLoop();
    if (analyze_done_) break;
  }

  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  ASSERT_EQ(analyze_result_->scripts.size(), 2u);

  bool found_blocked = false;
  for (const auto& [url, advice] : analyze_result_->recommendations) {
    if (url == "https://example.com/blocked.js") {
      EXPECT_EQ(advice, DeferralAdvice::kNoCoverageData);
      found_blocked = true;
    } else if (url == "https://example.com/app.js") {
      EXPECT_EQ(advice, DeferralAdvice::kKeepSynchronous);  // 50% coverage
    }
  }
  EXPECT_TRUE(found_blocked);
  for (const auto& info : analyze_result_->scripts) {
    if (info.url == "https://example.com/blocked.js") {
      EXPECT_FALSE(info.has_coverage_data);
    } else if (info.url == "https://example.com/app.js") {
      EXPECT_TRUE(info.has_coverage_data);
    }
  }
}

// A profiler entry under a URL does not show that the script at that URL was
// loaded. A script the browser never received (here: refused by the resource
// map) gets no coverage data from an entry reported under its URL, while a
// script that was served keeps its coverage.
TEST_F(ScriptCoverageAnalyzerCdpTest,
       CoverageReportedUnderUnservedUrlIsNotJoined) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto mutable_resources = std::make_shared<PageAnalyzer::ResourceMap>();
  (*mutable_resources)["https://example.com/app.js"] = "var a=1;";
  auto resources = std::static_pointer_cast<const PageAnalyzer::ResourceMap>(
      mutable_resources);

  analyzer.Analyze(
      "<html><head><script>var x=1;</script>"
      "<script src='https://example.com/app.js'></script>"
      "<script src='https://example.com/never-loaded.js'></script>"
      "</head></html>",
      resources, 1440, 900,
      [this](absl::StatusOr<ScriptCoverageResult> result) {
        analyze_result_ = std::move(result);
        analyze_done_ = true;
      });

  auto element = [](const std::string& url) {
    return json{{"url", url},
                {"selector", "script"},
                {"is_inline", false},
                {"is_async", false},
                {"is_defer", false},
                {"is_module", false},
                {"has_document_write", false}};
  };
  auto custom = [this, &element]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Profiler.takePreciseCoverage") {
        // An entry reported under the URL of a script that was not served.
        json result = json::array({
            MakeCoverageData("s1", "https://example.com/app.js", 1000, 0, 500),
            MakeCoverageData("s2", "https://example.com/never-loaded.js", 400,
                             0, 400),
        });
        RespondToCommand(id, {{"result", result}});
      } else if (method == "Runtime.evaluate") {
        json scripts = json::array({
            element("https://example.com/app.js"),
            element("https://example.com/never-loaded.js"),
        });
        RespondToCommand(
            id, {{"result", {{"type", "string"}, {"value", scripts.dump()}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom();
    RunLoop();
  }
  RequestScript("https://example.com/app.js", "req-1");
  RequestScript("https://example.com/never-loaded.js", "req-2");
  for (int i = 0; i < 5; ++i) {
    custom();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  for (int i = 0; i < 5; ++i) {
    custom();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15; ++i) {
    custom();
    RunLoop();
    if (analyze_done_) break;
  }

  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  EXPECT_EQ(analyze_result_->fetches_served, 1u);
  EXPECT_EQ(analyze_result_->fetches_blocked, 1u);
  ASSERT_EQ(analyze_result_->scripts.size(), 2u);
  for (const auto& info : analyze_result_->scripts) {
    if (info.url == "https://example.com/app.js") {
      EXPECT_TRUE(info.has_coverage_data);
      EXPECT_FLOAT_EQ(info.coverage_at_fcp, 0.5f);
    } else {
      EXPECT_EQ(info.url, "https://example.com/never-loaded.js");
      EXPECT_FALSE(info.has_coverage_data);
      EXPECT_FLOAT_EQ(info.coverage_at_fcp, 0.0f);
    }
  }
  for (const auto& [url, advice] : analyze_result_->recommendations) {
    if (url == "https://example.com/never-loaded.js") {
      EXPECT_EQ(advice, DeferralAdvice::kNoCoverageData);
    }
  }
}

// Inline scripts are reported in `scripts` (is_inline, empty url) but never
// produce a recommendation — nothing without a real URL may reach the
// production suffix-matcher.
TEST_F(ScriptCoverageAnalyzerCdpTest, InlineScriptsNeverInRecommendations) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze(
      "<html><head><script>var a=1;</script>"
      "<script src='https://example.com/app.js'></script></head></html>",
      resources, 1440, 900,
      [this](absl::StatusOr<ScriptCoverageResult> result) {
        analyze_result_ = std::move(result);
        analyze_done_ = true;
      });

  auto custom = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Profiler.takePreciseCoverage") {
        json result = json::array({
            MakeCoverageData("s1", "https://example.com/app.js", 1000, 0, 0),
        });
        RespondToCommand(id, {{"result", result}});
      } else if (method == "Runtime.evaluate") {
        json scripts = json::array({
            {{"url", ""},
             {"selector", "script"},
             {"is_inline", true},
             {"is_async", false},
             {"is_defer", false},
             {"is_module", false},
             {"has_document_write", false}},
            {{"url", "https://example.com/app.js"},
             {"selector", "script"},
             {"is_inline", false},
             {"is_async", false},
             {"is_defer", false},
             {"is_module", false},
             {"has_document_write", false}},
        });
        RespondToCommand(
            id, {{"result", {{"type", "string"}, {"value", scripts.dump()}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  for (int i = 0; i < 5; ++i) {
    custom();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15; ++i) {
    custom();
    RunLoop();
    if (analyze_done_) break;
  }

  ASSERT_TRUE(analyze_done_);
  ASSERT_TRUE(analyze_result_.ok()) << analyze_result_.status().message();
  ASSERT_EQ(analyze_result_->scripts.size(), 2u);

  // Exactly one recommendation (the external script); every recommendation
  // URL is a real absolute URL.
  ASSERT_EQ(analyze_result_->recommendations.size(), 1u);
  for (const auto& [url, advice] : analyze_result_->recommendations) {
    EXPECT_NE(url.find("://"), std::string::npos) << url;
  }
  EXPECT_EQ(analyze_result_->recommendations[0].first,
            "https://example.com/app.js");

  bool found_inline = false;
  for (const auto& info : analyze_result_->scripts) {
    if (info.is_inline) {
      EXPECT_TRUE(info.url.empty());
      found_inline = true;
    }
  }
  EXPECT_TRUE(found_inline);
}

TEST_F(ScriptCoverageAnalyzerCdpTest, SessionTimeout) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze(
      "<html></html>", resources, 1440, 900,
      [this](absl::StatusOr<ScriptCoverageResult> result) {
        analyze_result_ = std::move(result);
        analyze_done_ = true;
      },
      200);

  bool guard_fired = false;
  uv_timer_t guard;
  uv_timer_init(loop_, &guard);
  guard.data = &guard_fired;
  uv_timer_start(
      &guard, [](uv_timer_t* t) { *static_cast<bool*>(t->data) = true; }, 2000,
      0);

  while (!analyze_done_ && !guard_fired) {
    AutoRespondAll();
    uv_run(loop_, UV_RUN_NOWAIT);
    PollCommands();
    test::SleepUs(1000);  // 1ms
  }

  uv_timer_stop(&guard);
  uv_close(reinterpret_cast<uv_handle_t*>(&guard), nullptr);
  RunLoop();

  ASSERT_TRUE(analyze_done_) << "Session timeout did not fire within 2s";
  EXPECT_FALSE(analyze_result_.ok());
  EXPECT_TRUE(analyze_result_.status().message().find("timeout") !=
              std::string::npos);
}

TEST_F(ScriptCoverageAnalyzerCdpTest, EvaluateReturnsMalformed) {
  ScriptCoverageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<ScriptCoverageResult> result) {
                     analyze_result_ = std::move(result);
                     analyze_done_ = true;
                   });

  auto custom = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "Profiler.takePreciseCoverage") {
        RespondToCommand(id, {{"result", json::array()}});
      } else if (method == "Runtime.evaluate") {
        RespondToCommand(
            id,
            {{"result", {{"type", "string"}, {"value", "{invalid json!!!"}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom();
    RunLoop();
    if (analyze_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            "sess-1");
  for (int i = 0; i < 5; ++i) {
    custom();
    RunLoop();
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 15; ++i) {
    custom();
    RunLoop();
    if (analyze_done_) break;
  }

  ASSERT_TRUE(analyze_done_);
  EXPECT_FALSE(analyze_result_.ok());
}

}  // namespace
}  // namespace pagespeed
