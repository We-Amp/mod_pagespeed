// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Browser CSS Extractor Tests
//
// Tests use mock pipe pairs to simulate Chrome CDP responses.
// No actual Chrome binary is required.

#include "src/browser/browser_css_extractor.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/worker/critical_css_extractor.h"
#include "test/test_util/cdp_pipe.h"
#include "test/test_util/cdp_setup_order.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

// Test fixture that sets up a mock pipe pair and CdpClient.
class BrowserCssExtractorTest : public ::testing::Test {
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

    // Raw FD for test to read commands sent by CdpClient.
    test_read_fd_ = client_to_chrome_.read_fd;

    // Make test's read FD non-blocking for raw polling.
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

  // Poll for commands from CdpClient via raw non-blocking read().
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

  // Respond to the next pending command with a CDP response.
  void RespondToCommand(int id, const json& result) {
    json response = {{"id", id}, {"result", result}};
    SendFromChrome(response.dump());
  }

  // Send a CDP event.
  void SendEvent(const std::string& method, const json& params,
                 const std::string& session_id = "") {
    json event = {{"method", method}, {"params", params}};
    if (!session_id.empty()) {
      event["sessionId"] = session_id;
    }
    SendFromChrome(event.dump());
  }

  // Wait for a specific number of commands to be received from
  // the client.
  void WaitForCommands(size_t count) {
    RunLoopUntil([this, count] { return received_commands_.size() >= count; });
  }

  // Auto-responder: responds to each command from the client
  // with pre-programmed responses based on the method.
  void SetupAutoResponder(const std::string& session_id) {
    session_id_ = session_id;
    client_->SetEventCallback([](const CdpEvent&) {});
  }

  // Process all pending commands and auto-respond.
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
                    {{"name", name}, {"loaderId", "blank-loader"}},
                    session_id_);
        }
        RespondToCommand(id, json::object());
        continue;
      }
      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Network.emulateNetworkConditions" ||
                 method == "Emulation.setScriptExecutionDisabled" ||
                 method == "Fetch.enable" ||
                 method == "Emulation.setDeviceMetricsOverride" ||
                 method == "Emulation.setTouchEmulationEnabled" ||
                 method == "Emulation.setUserAgentOverride" ||
                 method == "Page.navigate" || method == "DOM.enable" ||
                 method == "Page.enable" || method == "CSS.enable" ||
                 method == "Page.setLifecycleEventsEnabled" ||
                 method == "Target.closeTarget" ||
                 method ==
                     "Fetch.failRequest") {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      } else if (
          method ==
          "CSS.startRuleUsageTracking") {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Page.setDocumentContent") {
        RespondToCommand(id, json::object());
      } else if (method == "CSS.takeCoverageDelta") {
        json coverage = json::array();
        coverage.push_back({
            {"styleSheetId", "sheet-1"},
            {"startOffset", 0},
            {"endOffset", 20},
            {"used", true},
        });
        RespondToCommand(id, {{"coverage", coverage}, {"timestamp", 1.0}});
      } else if (method == "CSS.stopRuleUsageTracking") {
        json usage = json::array();
        usage.push_back({
            {"styleSheetId", "sheet-1"},
            {"startOffset", 0},
            {"endOffset", 40},
            {"used", true},
        });
        RespondToCommand(id, {{"ruleUsage", usage}});
      } else if (method == "CSS.getStyleSheetText") {
        // 50 chars of CSS text.
        RespondToCommand(
            id, {{"text", "body{margin:0;padding:0}h1{color:red}p{font:1em}"}});
      }
    }
  }

  // Replay the blank tab's lifecycle on Page.setLifecycleEventsEnabled, as
  // Chromium does.
  bool replay_blank_lifecycle_ = false;
  uv_loop_t* loop_ = nullptr;
  test::PipePair chrome_to_client_;
  test::PipePair client_to_chrome_;
  uv_pipe_t read_pipe_{};
  uv_pipe_t client_write_{};
  int test_read_fd_ = -1;
  int chrome_write_fd_ = -1;
  std::unique_ptr<CdpClient> client_;
  bool extraction_done_ = false;
  absl::StatusOr<BrowserCssResult> extraction_result_;
  std::vector<json> received_commands_;
  std::string received_data_;
  std::string session_id_ = "session-1";
  std::set<int> responded_ids_;
};

TEST_F(BrowserCssExtractorTest, SuccessfulExtraction) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract(
      "<html><head><style>body{margin:0}</style></head>"
      "<body><h1>Hello</h1></body></html>",
      1440, 900, [this](absl::StatusOr<BrowserCssResult> result) {
        extraction_result_ = std::move(result);
        extraction_done_ = true;
      });

  // Auto-respond to setup commands (steps 1-8).
  // Need to iterate because commands are chained.
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
    if (extraction_done_) break;
  }

  // Send FCP lifecycle event.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Send networkIdle lifecycle event.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  ASSERT_TRUE(extraction_result_.ok()) << extraction_result_.status().message();
  EXPECT_FALSE(extraction_result_->critical_css.empty());
  EXPECT_GT(extraction_result_->total_css_bytes, 0u);
  EXPECT_GT(extraction_result_->critical_css_bytes, 0u);
  EXPECT_GT(extraction_result_->coverage_ratio, 0.0f);
  EXPECT_LE(extraction_result_->coverage_ratio, 1.0f);
}

// Chromium replays the blank tab's networkIdle when lifecycle
// events are enabled, before the document is written. The CSS coverage render takes nothing
// from the page before it is written: its first snapshot waits for the
// page's firstContentfulPaint, which is not replayed. (Its "load" snapshot
// may still count the replayed networkIdle: measured on the pinned Chromium
// 145, waiting for the page's own networkIdle instead changes none of its
// outputs and costs 0.5 s to 2 s per render.)
TEST_F(BrowserCssExtractorTest, TakesNothingBeforeTheDocumentIsWritten) {
  replay_blank_lifecycle_ = true;
  BrowserCssExtractor extractor(client_.get());
  extractor.Extract(
      "<html><head><style>body{margin:0}</style></head>"
      "<body><h1>Hello</h1></body></html>",
      375, 667, [this](absl::StatusOr<BrowserCssResult> result) {
        extraction_result_ = std::move(result);
        extraction_done_ = true;
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
  EXPECT_EQ(index_of("CSS.takeCoverageDelta"), -1)
      << "CSS.takeCoverageDelta before the page painted";
  EXPECT_EQ(index_of("CSS.stopRuleUsageTracking"), -1)
      << "CSS.stopRuleUsageTracking before the page painted";
  EXPECT_FALSE(extraction_done_);

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 20 && !extraction_done_; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  ASSERT_TRUE(extraction_done_);
  EXPECT_TRUE(extraction_result_.ok()) << extraction_result_.status();
  EXPECT_GT(index_of("CSS.takeCoverageDelta"), content)
      << "CSS.takeCoverageDelta";
  EXPECT_GT(index_of("CSS.stopRuleUsageTracking"), content)
      << "CSS.stopRuleUsageTracking";
}

TEST_F(BrowserCssExtractorTest, CreateTargetFailure) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  RunLoop();
  // Respond with an error to createTarget.
  WaitForCommands(1);
  int id = received_commands_[0].value("id", 0);
  json error_response = {
      {"id", id}, {"error", {{"code", -32000}, {"message", "Target failed"}}}};
  SendFromChrome(error_response.dump());

  RunLoopUntil([this] { return extraction_done_; });

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

TEST_F(BrowserCssExtractorTest, NoStylesheetsReturnsEmpty) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html><body>No CSS</body></html>", 375, 667,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  // Override stopRuleUsageTracking to return empty usage.
  auto orig_respond = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "CSS.stopRuleUsageTracking") {
        RespondToCommand(id, {{"ruleUsage", json::array()}});
      } else if (method == "CSS.takeCoverageDelta") {
        RespondToCommand(id, {{"coverage", json::array()}, {"timestamp", 1.0}});
      } else {
        // Default auto-respond.
        if (method == "Target.createTarget") {
          RespondToCommand(id, {{"targetId", "target-1"}});
        } else if (method == "Target.attachToTarget") {
          RespondToCommand(id, {{"sessionId", session_id_}});
        } else if (method == "Page.getFrameTree") {
          RespondToCommand(id,
                           {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
        } else {
          RespondToCommand(id, json::object());
        }
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    orig_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    orig_respond();
    RunLoop();
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 10; ++i) {
    orig_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  ASSERT_TRUE(extraction_result_.ok());
  EXPECT_TRUE(extraction_result_->critical_css.empty());
  EXPECT_EQ(extraction_result_->total_css_bytes, 0u);
  EXPECT_EQ(extraction_result_->coverage_ratio, 0.0f);
}

TEST_F(BrowserCssExtractorTest, SessionIdCorrect) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  // Run enough to get through createTarget and attachToTarget.
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Verify that commands after attachToTarget include sessionId.
  bool found_session_command = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.contains("sessionId") && cmd["sessionId"] == session_id_) {
      found_session_command = true;
      break;
    }
  }
  EXPECT_TRUE(found_session_command);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 15; ++i) {
    AutoRespondAll();
    RunLoop();
    if (extraction_done_) break;
  }
}

TEST_F(BrowserCssExtractorTest, ViewportDimensionsSet) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 375, 667,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  // Run enough to capture the setDeviceMetricsOverride command.
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Find setDeviceMetricsOverride command.
  bool found = false;
  bool found_touch = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "Emulation.setDeviceMetricsOverride") {
      auto params = cmd.value("params", json::object());
      EXPECT_EQ(params.value("width", 0), 375);
      EXPECT_EQ(params.value("height", 0), 667);
      EXPECT_TRUE(params.value("mobile", false));
      found = true;
    }
    // A phone is a touch device, on the same session, after the
    // metrics.
    if (cmd.value("method", "") == "Emulation.setTouchEmulationEnabled") {
      EXPECT_TRUE(found) << "touch is emulated after the device metrics";
      auto params = cmd.value("params", json::object());
      EXPECT_TRUE(params.value("enabled", false));
      EXPECT_EQ(params.value("maxTouchPoints", 0), 5);
      EXPECT_EQ(cmd.value("sessionId", ""), session_id_);
      found_touch = true;
      break;
    }
  }
  EXPECT_TRUE(found);
  EXPECT_TRUE(found_touch);
  // Then the phone's user agent, and the document goes into a
  // fresh about:blank window.
  test::ExpectFreshWindowRenderSetup(received_commands_, session_id_, 375, 667);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 15; ++i) {
    AutoRespondAll();
    RunLoop();
    if (extraction_done_) break;
  }
}

// The setup order at the tablet (its own user agent) and the
// desktop (no user agent override), each ending in a fresh window.
TEST_F(BrowserCssExtractorTest, TabletAndDesktopSetupOrder) {
  for (const auto& [width, height] :
       {std::pair<uint32_t, uint32_t>{768, 1024}, {1440, 900}}) {
    received_commands_.clear();
    responded_ids_.clear();
    extraction_done_ = false;
    BrowserCssExtractor extractor(client_.get());
    extractor.Extract("<html></html>", width, height,
                      [this](absl::StatusOr<BrowserCssResult> result) {
                        extraction_result_ = std::move(result);
                        extraction_done_ = true;
                      });
    for (int i = 0; i < 20; ++i) {
      AutoRespondAll();
      RunLoop();
    }
    test::ExpectFreshWindowRenderSetup(received_commands_, session_id_, width,
                                       height);
    SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
              session_id_);
    SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
    for (int i = 0; i < 15; ++i) {
      AutoRespondAll();
      RunLoop();
      if (extraction_done_) break;
    }
    EXPECT_TRUE(extraction_done_) << width;
  }
}

TEST_F(BrowserCssExtractorTest, DeferredCssIsNotDuplicateOfCritical) {
  // Regression test: deferred CSS must be the set difference
  // (all_used - fcp_used), NOT a duplicate of all_used.
  //
  // Setup: sheet text is "AAAABBBBCCCCDDDD" (16 chars).
  // FCP covers bytes [0,8) = "AAAABBBB"
  // All covers bytes [0,16) = entire sheet
  // Expected critical = "AAAABBBB", deferred = "CCCCDDDD"
  BrowserCssExtractor extractor(client_.get());

  auto custom_respond = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "CSS.takeCoverageDelta") {
        // FCP: first 8 bytes used.
        json cov = json::array();
        cov.push_back({{"styleSheetId", "s1"},
                       {"startOffset", 0},
                       {"endOffset", 8},
                       {"used", true}});
        RespondToCommand(id, {{"coverage", cov}, {"timestamp", 1.0}});
      } else if (method == "CSS.stopRuleUsageTracking") {
        // All: entire 16 bytes used.
        json usage = json::array();
        usage.push_back({{"styleSheetId", "s1"},
                         {"startOffset", 0},
                         {"endOffset", 16},
                         {"used", true}});
        RespondToCommand(id, {{"ruleUsage", usage}});
      } else if (method == "CSS.getStyleSheetText") {
        RespondToCommand(id, {{"text", "AAAABBBBCCCCDDDD"}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  extractor.Extract("<html><head><style>AAAABBBBCCCCDDDD</style></head></html>",
                    1440, 900, [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    custom_respond();
    RunLoop();
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 15; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  ASSERT_TRUE(extraction_result_.ok()) << extraction_result_.status().message();
  EXPECT_EQ(extraction_result_->critical_css, "AAAABBBB");
  EXPECT_EQ(extraction_result_->deferred_css, "CCCCDDDD");
  EXPECT_EQ(extraction_result_->total_css_bytes, 16u);
}

TEST_F(BrowserCssExtractorTest, NetworkIdleBeforeFcp) {
  // Regression test: if networkIdle fires before the FCP
  // coverage delta response arrives, extraction must still
  // succeed (not race).
  BrowserCssExtractor extractor(client_.get());
  bool delta_requested = false;
  int delta_cmd_id = 0;

  auto custom_respond = [this, &delta_requested, &delta_cmd_id]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");

      if (method == "CSS.takeCoverageDelta") {
        // Don't respond yet -- we'll respond after networkIdle.
        delta_requested = true;
        delta_cmd_id = id;
        continue;
      }

      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "CSS.stopRuleUsageTracking") {
        json usage = json::array();
        usage.push_back({{"styleSheetId", "s1"},
                         {"startOffset", 0},
                         {"endOffset", 10},
                         {"used", true}});
        RespondToCommand(id, {{"ruleUsage", usage}});
      } else if (method == "CSS.getStyleSheetText") {
        RespondToCommand(id, {{"text", "0123456789"}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  extractor.Extract("<html><head><style>0123456789</style></head></html>", 1440,
                    900, [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  // Process setup commands.
  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
  }

  // Send FCP -- this triggers takeCoverageDelta.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    custom_respond();
    RunLoop();
  }
  ASSERT_TRUE(delta_requested);

  // Send networkIdle BEFORE responding to delta.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 3; ++i) {
    custom_respond();
    RunLoop();
  }

  // Now respond to the delta -- should trigger stopTracking.
  responded_ids_.insert(delta_cmd_id);
  json cov = json::array();
  cov.push_back({{"styleSheetId", "s1"},
                 {"startOffset", 0},
                 {"endOffset", 5},
                 {"used", true}});
  RespondToCommand(delta_cmd_id, {{"coverage", cov}, {"timestamp", 1.0}});

  for (int i = 0; i < 15; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  ASSERT_TRUE(extraction_result_.ok()) << extraction_result_.status().message();
  EXPECT_EQ(extraction_result_->critical_css, "01234");
  EXPECT_EQ(extraction_result_->deferred_css, "56789");
}

TEST_F(BrowserCssExtractorTest, NetworkOfflineFailureAbortsExtraction) {
  // Test that if a security-critical setup command fails,
  // extraction is aborted.
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  // Respond to createTarget and attachToTarget normally.
  for (int i = 0; i < 5; ++i) {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Network.emulateNetworkConditions") {
        // Fail the network offline command.
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "not supported"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return extraction_done_; }, 50);

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

TEST_F(BrowserCssExtractorTest, SessionTimeoutFiresOnStall) {
  // Test that if Chrome never sends lifecycle events, the session
  // timeout fires and the callback receives an error.
  BrowserCssExtractor extractor(client_.get());

  // Use a short timeout (200ms). Setup takes several loop iterations,
  // so the timer starts counting from StartTimeout() in Extract().
  extractor.Extract(
      "<html><head><style>body{margin:0}</style></head></html>", 1440, 900,
      [this](absl::StatusOr<BrowserCssResult> result) {
        extraction_result_ = std::move(result);
        extraction_done_ = true;
      },
      200);  // 200ms timeout

  // Use a guard timer (2s) to prevent infinite hang.
  bool guard_fired = false;
  uv_timer_t guard;
  uv_timer_init(loop_, &guard);
  guard.data = &guard_fired;
  uv_timer_start(
      &guard, [](uv_timer_t* t) { *static_cast<bool*>(t->data) = true; }, 2000,
      0);

  // Continuously auto-respond to setup commands AND run the loop.
  // The session timeout (200ms) should fire after setup completes
  // and no lifecycle events arrive. UV_RUN_ONCE blocks waiting for
  // I/O or timers, which lets the session timer fire.
  while (!extraction_done_ && !guard_fired) {
    AutoRespondAll();
    uv_run(loop_, UV_RUN_NOWAIT);
    PollCommands();
    test::SleepUs(1000);  // 1ms
  }

  uv_timer_stop(&guard);
  uv_close(reinterpret_cast<uv_handle_t*>(&guard), nullptr);
  RunLoop();

  ASSERT_TRUE(extraction_done_) << "Session timeout did not fire within 2s";
  EXPECT_FALSE(extraction_result_.ok());
  // Should contain "timeout" in the error message.
  EXPECT_TRUE(extraction_result_.status().message().find("timeout") !=
              std::string::npos)
      << "Error: " << extraction_result_.status().message();
}

TEST_F(BrowserCssExtractorTest, AttachToTargetFailure) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 10; ++i) {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "attach failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return extraction_done_; }, 50);

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

TEST_F(BrowserCssExtractorTest, JSDisableFailure) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 15; ++i) {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (
          method ==
          "Network.emulateNetworkConditions") {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      } else if (method == "Emulation.setScriptExecutionDisabled") {
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "JS disable failed"}}}};
        SendFromChrome(err.dump());
      } else {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return extraction_done_; }, 50);

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

TEST_F(BrowserCssExtractorTest, FetchEnableFailure) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 15; ++i) {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Fetch.enable") {
        json err = {{"id", id},
                    {"error", {{"code", -32000}, {"message", "Fetch failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return extraction_done_; }, 50);

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

TEST_F(BrowserCssExtractorTest, GetFrameTreeEmptyFrameId) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 40 && !extraction_done_; ++i) {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        // Return empty frame tree.
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", ""}}}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
    RunLoop();
  }

  RunLoopUntil([this] { return extraction_done_; }, 50);

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

TEST_F(BrowserCssExtractorTest, MultipleStylesheets) {
  BrowserCssExtractor extractor(client_.get());
  int text_fetch_count = 0;

  auto custom_respond = [this, &text_fetch_count]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "CSS.takeCoverageDelta") {
        json cov = json::array();
        cov.push_back({{"styleSheetId", "sheet-a"},
                       {"startOffset", 0},
                       {"endOffset", 10},
                       {"used", true}});
        cov.push_back({{"styleSheetId", "sheet-b"},
                       {"startOffset", 0},
                       {"endOffset", 5},
                       {"used", true}});
        RespondToCommand(id, {{"coverage", cov}, {"timestamp", 1.0}});
      } else if (method == "CSS.stopRuleUsageTracking") {
        json usage = json::array();
        usage.push_back({{"styleSheetId", "sheet-a"},
                         {"startOffset", 0},
                         {"endOffset", 20},
                         {"used", true}});
        usage.push_back({{"styleSheetId", "sheet-b"},
                         {"startOffset", 0},
                         {"endOffset", 15},
                         {"used", true}});
        RespondToCommand(id, {{"ruleUsage", usage}});
      } else if (method == "CSS.getStyleSheetText") {
        text_fetch_count++;
        auto params = cmd.value("params", json::object());
        std::string sheet_id = params.value("styleSheetId", "");
        if (sheet_id == "sheet-a") {
          RespondToCommand(id, {{"text", "body{margin:0}div{pad:1}"}});
        } else {
          RespondToCommand(id, {{"text", "h1{color:red}p{x:y}"}});
        }
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  extractor.Extract(
      "<html><head>"
      "<style>body{margin:0}div{pad:1}</style>"
      "<style>h1{color:red}p{x:y}</style>"
      "</head></html>",
      1440, 900, [this](absl::StatusOr<BrowserCssResult> result) {
        extraction_result_ = std::move(result);
        extraction_done_ = true;
      });

  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    custom_respond();
    RunLoop();
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 15; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  ASSERT_TRUE(extraction_result_.ok()) << extraction_result_.status().message();
  // Both stylesheet texts should have been fetched.
  EXPECT_EQ(text_fetch_count, 2);
  EXPECT_FALSE(extraction_result_->critical_css.empty());
  EXPECT_FALSE(extraction_result_->deferred_css.empty());
  // Two DISTINCT sheets (24 + 19 bytes) must BOTH be counted — this pins the
  // negative case of the identical-text dedup (see
  // IdenticalDuplicateStylesheetsCriticalNotDoubled): genuinely different
  // stylesheets must never be collapsed.
  EXPECT_EQ(extraction_result_->total_css_bytes, 43u);
}

// Regression (critical-CSS double-ship): the browser-analysis HTML inlines a
// cached <style> copy of each sheet but leaves the original external <link>
// (css_cache_inliner is append-only), so Chrome reports the SAME css as two
// stylesheet ids with byte-identical text. Aggregating critical ranges per-id
// then emits the critical block TWICE — observed on prod iispeed.com as 13.2KB,
// exactly 2x the 6.5KB critical subset. Identical stylesheet texts must
// contribute their critical/deferred/total exactly once.
TEST_F(BrowserCssExtractorTest,
       IdenticalDuplicateStylesheetsCriticalNotDoubled) {
  BrowserCssExtractor extractor(client_.get());

  auto custom_respond = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "CSS.takeCoverageDelta") {
        // FCP: first 8 bytes used in BOTH identical sheets.
        json cov = json::array();
        cov.push_back({{"styleSheetId", "dup-1"},
                       {"startOffset", 0},
                       {"endOffset", 8},
                       {"used", true}});
        cov.push_back({{"styleSheetId", "dup-2"},
                       {"startOffset", 0},
                       {"endOffset", 8},
                       {"used", true}});
        RespondToCommand(id, {{"coverage", cov}, {"timestamp", 1.0}});
      } else if (method == "CSS.stopRuleUsageTracking") {
        json usage = json::array();
        usage.push_back({{"styleSheetId", "dup-1"},
                         {"startOffset", 0},
                         {"endOffset", 16},
                         {"used", true}});
        usage.push_back({{"styleSheetId", "dup-2"},
                         {"startOffset", 0},
                         {"endOffset", 16},
                         {"used", true}});
        RespondToCommand(id, {{"ruleUsage", usage}});
      } else if (method == "CSS.getStyleSheetText") {
        // Both ids return byte-identical sheet text (external + inlined copy).
        RespondToCommand(id, {{"text", "AAAABBBBCCCCDDDD"}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  extractor.Extract(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<style data-pagespeed-inlined>AAAABBBBCCCCDDDD</style>"
      "</head></html>",
      1440, 900, [this](absl::StatusOr<BrowserCssResult> result) {
        extraction_result_ = std::move(result);
        extraction_done_ = true;
      });

  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    custom_respond();
    RunLoop();
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 15; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  ASSERT_TRUE(extraction_result_.ok()) << extraction_result_.status().message();
  // Critical must be the single 8-byte FCP prefix, NOT doubled
  // ("AAAABBBB\nAAAABBBB").
  EXPECT_EQ(extraction_result_->critical_css, "AAAABBBB");
  // The duplicate sheet's bytes must not be double-counted (16, not 32).
  EXPECT_EQ(extraction_result_->total_css_bytes, 16u);
}

TEST_F(BrowserCssExtractorTest, FetchRequestBlockedDuringExtraction) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html><head><style>body{margin:0}</style></head></html>",
                    1440, 900, [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Simulate Fetch.requestPaused.
  SendEvent(
      "Fetch.requestPaused",
      {{"requestId", "req-1"}, {"request", {{"url", "http://evil.com/steal"}}}},
      session_id_);

  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Verify Fetch.failRequest was sent.
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

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 15; ++i) {
    AutoRespondAll();
    RunLoop();
    if (extraction_done_) break;
  }
}

TEST_F(BrowserCssExtractorTest, SetDocumentContentFailure) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 40 && !extraction_done_; ++i) {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "Page.setDocumentContent") {
        json err = {{"id", id},
                    {"error", {{"code", -32000}, {"message", "failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
    RunLoop();
  }

  RunLoopUntil([this] { return extraction_done_; }, 50);

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

TEST_F(BrowserCssExtractorTest, StopRuleUsageTrackingFailure) {
  BrowserCssExtractor extractor(client_.get());

  auto custom_respond = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "CSS.takeCoverageDelta") {
        RespondToCommand(id, {{"coverage", json::array()}, {"timestamp", 1.0}});
      } else if (method == "CSS.stopRuleUsageTracking") {
        json err = {{"id", id},
                    {"error", {{"code", -32000}, {"message", "failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  extractor.Extract("<html></html>", 1440, 900,
                    [this](absl::StatusOr<BrowserCssResult> result) {
                      extraction_result_ = std::move(result);
                      extraction_done_ = true;
                    });

  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    custom_respond();
    RunLoop();
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 15; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  EXPECT_FALSE(extraction_result_.ok());
}

// Regression test for issue #194: if FCP and networkIdle fire before the
// takeCoverageDelta response arrives, only one CSS.stopRuleUsageTracking
// command should be sent (the stop_requested flag prevents duplicates).
TEST_F(BrowserCssExtractorTest, NoDuplicateStopTrackingOnRapidEvents) {
  BrowserCssExtractor extractor(client_.get());

  extractor.Extract(
      "<html><head><style>body{margin:0}</style></head>"
      "<body><h1>Hello</h1></body></html>",
      1440, 900, [this](absl::StatusOr<BrowserCssResult> result) {
        extraction_result_ = std::move(result);
        extraction_done_ = true;
      });

  // Auto-respond to setup commands (steps 1-8), but hold back
  // CSS.takeCoverageDelta so we can control its timing.
  int delta_cmd_id = 0;
  auto custom_respond = [&]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");

      if (method == "CSS.takeCoverageDelta") {
        // Don't respond yet — save the id for later.
        delta_cmd_id = id;
        responded_ids_.insert(id);
        continue;
      }

      responded_ids_.insert(id);
      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", session_id_}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "CSS.stopRuleUsageTracking") {
        json usage = json::array();
        usage.push_back({{"styleSheetId", "sheet-1"},
                         {"startOffset", 0},
                         {"endOffset", 40},
                         {"used", true}});
        RespondToCommand(id, {{"ruleUsage", usage}});
      } else if (method == "CSS.getStyleSheetText") {
        RespondToCommand(
            id, {{"text", "body{margin:0;padding:0}h1{color:red}p{font:1em}"}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  // Drive setup to completion.
  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
  }

  // Fire BOTH lifecycle events before the delta response.
  SendEvent("Page.lifecycleEvent", {{"name", "firstContentfulPaint"}},
            session_id_);
  for (int i = 0; i < 5; ++i) {
    custom_respond();
    RunLoop();
  }

  // At this point the client has sent takeCoverageDelta but we haven't
  // responded. Fire networkIdle too.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, session_id_);
  for (int i = 0; i < 3; ++i) {
    custom_respond();
    RunLoop();
  }

  // Now respond to takeCoverageDelta — this triggers
  // MaybeProceedToStopTracking with both flags true.
  ASSERT_NE(delta_cmd_id, 0) << "takeCoverageDelta was never requested";
  json coverage = json::array();
  coverage.push_back({{"styleSheetId", "sheet-1"},
                      {"startOffset", 0},
                      {"endOffset", 20},
                      {"used", true}});
  RespondToCommand(delta_cmd_id, {{"coverage", coverage}, {"timestamp", 1.0}});

  // Let the extraction complete.
  for (int i = 0; i < 15; ++i) {
    custom_respond();
    RunLoop();
    if (extraction_done_) break;
  }

  ASSERT_TRUE(extraction_done_);
  ASSERT_TRUE(extraction_result_.ok()) << extraction_result_.status().message();

  // Count how many CSS.stopRuleUsageTracking commands were sent.
  int stop_count = 0;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "CSS.stopRuleUsageTracking") {
      ++stop_count;
    }
  }
  EXPECT_EQ(stop_count, 1) << "Expected exactly one stopRuleUsageTracking "
                              "command, got "
                           << stop_count;
}

// Issue #1056: coverage identities are reconstructed from Chrome CSS-coverage
// byte slices, which can be brace-unbalanced and truncated. BuildCoverageIdentities
// must never crash and must still surface the selector tokens it can see.
TEST(BuildCoverageIdentitiesTest,
     CoverageIdentitiesParsedFromUnbalancedCritical) {
  // A slice whose @layer and trailing rule never close.
  std::string_view unbalanced =
      "@layer utilities { .flex { display: flex; } .h-16 { height: 4rem; ";
  absl::flat_hash_set<RuleIdentity> ids = BuildCoverageIdentities(unbalanced);
  EXPECT_TRUE(ids.contains(RuleIdentity{"utilities", "", ".flex"}))
      << ".flex should be recovered from the unbalanced slice";
  EXPECT_TRUE(ids.contains(RuleIdentity{"utilities", "", ".h-16"}))
      << ".h-16 should be recovered even though its block never closes";

  // Leading stray closing braces and a truncated final rule must be tolerated.
  absl::flat_hash_set<RuleIdentity> ids2 =
      BuildCoverageIdentities("}} .btn { color: red; } .card {");
  EXPECT_TRUE(ids2.contains(RuleIdentity{"", "", ".btn"}));
  EXPECT_TRUE(ids2.contains(RuleIdentity{"", "", ".card"}));

  // Fully empty / whitespace input yields an empty set, no crash.
  EXPECT_TRUE(BuildCoverageIdentities("").empty());
  EXPECT_TRUE(BuildCoverageIdentities("   \n\t").empty());
}

// Characterization, not a defect report: this pins WHY the producer-side
// force-include lookup needs a context-relaxed fallback, so a later reader does
// not "tighten" that fallback back into inertness.
//
// Chrome reports CSS coverage as byte ranges over the stylesheet text, and the
// critical blob is the plain concatenation of those ranges (ExtractRanges).
// A used range covers the rule it belongs to, never the `@layer utilities {`
// or `@media ... {` prelude that encloses it — those bytes were never
// "used" by any rule. So the reconstructed blob is a flat sequence of style
// rules and every identity derived from it carries an EMPTY layer_path and an
// EMPTY media_condition, while the producer sees the same rules under their
// real wrappers. Passes on current code by construction.
TEST(BuildCoverageIdentitiesTest,
     BuildCoverageIdentitiesYieldsEmptyLayerPathForUnwrappedSlice) {
  const std::string sheet =
      "@layer utilities {\n"
      ".flex { display: flex; }\n"
      "@media (min-width: 768px) {\n"
      ".md\\:flex { display: flex; }\n"
      "}\n"
      "}\n";

  // The two used ranges, exactly as coverage reports them: the rules alone.
  const std::string rule_a = ".flex { display: flex; }";
  const std::string rule_b = ".md\\:flex { display: flex; }";
  ASSERT_NE(sheet.find(rule_a), std::string::npos);
  ASSERT_NE(sheet.find(rule_b), std::string::npos);

  // Mirror ExtractRanges: slice at the reported offsets, join with '\n'.
  std::string sliced = sheet.substr(sheet.find(rule_a), rule_a.size()) + "\n" +
                       sheet.substr(sheet.find(rule_b), rule_b.size());

  absl::flat_hash_set<RuleIdentity> ids = BuildCoverageIdentities(sliced);
  EXPECT_TRUE(ids.contains(RuleIdentity{"", "", ".flex"}))
      << "the layer prelude is not part of any used range";
  EXPECT_TRUE(ids.contains(RuleIdentity{"", "", ".md\\:flex"}))
      << "the @media prelude is not part of any used range either";
  EXPECT_FALSE(ids.contains(RuleIdentity{"utilities", "", ".flex"}));
  EXPECT_FALSE(ids.contains(
      RuleIdentity{"utilities", "(min-width: 768px)", ".md\\:flex"}));
}

// An escaped quote in a selector is part of the ident, so it
// opens no string and every later rule is still seen.
TEST(BuildCoverageIdentitiesTest, EscapedQuoteInSelectorKeepsLaterRules) {
  absl::flat_hash_set<RuleIdentity> ids = BuildCoverageIdentities(
      "@layer utilities{.a\\'b{color:red}.c{color:blue}}");
  EXPECT_TRUE(ids.contains(RuleIdentity{"utilities", "", ".a\\'b"}));
  EXPECT_TRUE(ids.contains(RuleIdentity{"utilities", "", ".c"}));
}

// A string missing its closing quote ends at the newline (a bad string), as
// in the extractor, so the rule after it is still seen; a CRLF continuation
// does not end it.
TEST(BuildCoverageIdentitiesTest, BadStringEndsAtNewline) {
  absl::flat_hash_set<RuleIdentity> ids =
      BuildCoverageIdentities(".a{content:\"open\n}.b{color:red}");
  EXPECT_TRUE(ids.contains(RuleIdentity{"", "", ".b"}));
  absl::flat_hash_set<RuleIdentity> continued =
      BuildCoverageIdentities(".a{content:\"x\\\r\n}y\"}.c{color:red}");
  EXPECT_TRUE(continued.contains(RuleIdentity{"", "", ".c"}));
  EXPECT_FALSE(continued.contains(RuleIdentity{"", "", "y\"}.c"}));
}

}  // namespace
}  // namespace pagespeed
