// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Page Analysis Tests

#include "src/browser/page_analysis.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "lib/net/fetch_policy.h"
#include "nlohmann/json.hpp"
#include "src/browser/agent_fetcher.h"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/render_support.h"
#include "test/test_util/cdp_pipe.h"
#include "test/test_util/cdp_setup_order.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

// The settle-after-load bound in the tests (kSettleAfterLoadMs is 2 s).
constexpr uint64_t kTestSettleMs = 50;

class PageAnalysisTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    ASSERT_EQ(0, uv_loop_init(loop_));

    ASSERT_TRUE(chrome_to_client_.Create());
    ASSERT_TRUE(client_to_chrome_.Create());

    chrome_write_fd_ = chrome_to_client_.write_fd;
    test_read_fd_ = client_to_chrome_.read_fd;

    // Make test's read FD non-blocking for raw polling.
    ASSERT_TRUE(test::SetNonBlocking(test_read_fd_));

    uv_pipe_init(loop_, &read_pipe_, 0);
    uv_pipe_open(&read_pipe_, chrome_to_client_.TakeReadFd());

    uv_pipe_init(loop_, &client_write_, 0);
    uv_pipe_open(&client_write_, client_to_chrome_.TakeWriteFd());

    client_ = std::make_unique<CdpClient>(loop_);
    ASSERT_TRUE(client_->AttachPipes(&read_pipe_, &client_write_).ok());
  }

  void TearDown() override {
    client_.reset();

    uv_walk(
        loop_,
        [](uv_handle_t* h, void*) {
          if (!uv_is_closing(h)) uv_close(h, nullptr);
        },
        nullptr);
    uv_run(loop_, UV_RUN_DEFAULT);

    chrome_to_client_.CloseWrite();
    client_to_chrome_.CloseRead();

    uv_loop_close(loop_);
    delete loop_;
  }

  void SendFromChrome(const std::string& json_str) const {
    std::string msg = json_str;
    msg.push_back('\0');
    ssize_t n = test::PipeWrite(chrome_write_fd_, msg.data(), msg.size());
    (void)n;
  }

  // Write a message that may exceed one pipe-buffer write: chunk it and pump
  // the loop between chunks so the client drains the read side.  Required for
  // any payload over a few KB — PipeWrite caps a single write at 4096 bytes on
  // Windows, so SendFromChrome would silently truncate it there.
  void SendLargeFromChrome(const std::string& json_str) const {
    std::string msg = json_str;
    msg.push_back('\0');
    test::SetNonBlocking(chrome_write_fd_);
    size_t offset = 0;
    int stalls = 0;
    while (offset < msg.size()) {
      ssize_t n = test::PipeWrite(chrome_write_fd_, msg.data() + offset,
                                  msg.size() - offset);
      if (n > 0) {
        offset += static_cast<size_t>(n);
        stalls = 0;
      } else if (++stalls > 1000) {
        break;  // Safety valve.
      }
      if (offset < msg.size()) uv_run(loop_, UV_RUN_ONCE);
    }
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

  void RunLoop() {
    for (int i = 0; i < 5; i++) {
      if (uv_run(loop_, UV_RUN_NOWAIT) == 0) break;
    }
    PollCommands();
  }

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

  void SendEvent(const std::string& method, const json& params,
                 const std::string& session_id = "") {
    json event = {{"method", method}, {"params", params}};
    if (!session_id.empty()) {
      event["sessionId"] = session_id;
    }
    SendFromChrome(event.dump());
  }

  // Collected analysis result from evaluate.
  json MakeEvalResult() {
    json result;
    result["lcp"] = {
        {"selector", "img#hero"}, {"url", "https://cdn.example.com/hero.jpg"},
        {"tag", "IMG"},           {"size", 50000},
        {"time", 250.5},
    };
    result["cls"] = {
        {"total", 0.05},
        {"sources", json::array({{{"selector", "img#ad"}, {"value", 0.05}}})},
    };
    result["fcp"] = 180.0;
    result["images"] = json::array({
        {{"selector", "img#hero"},
         {"src", "https://cdn.example.com/hero.jpg"},
         {"above_fold", true},
         {"rendered_width", 800},
         {"rendered_height", 400},
         {"natural_width", 1600},
         {"natural_height", 800}},
        {{"selector", "img.thumb"},
         {"src", "https://cdn.example.com/thumb.jpg"},
         {"above_fold", false},
         {"rendered_width", 150},
         {"rendered_height", 150},
         {"natural_width", 300},
         {"natural_height", 300}},
    });
    return result;
  }

  // Respond to every outstanding command, answering Runtime.evaluate with a
  // caller-supplied payload instead of MakeEvalResult().
  void AutoRespondAllWithEval(const json& eval_payload) {
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
      } else if (method == "Runtime.evaluate") {
        json response = {
            {"id", id},
            {"result",
             {{"result",
               {{"type", "string"}, {"value", eval_payload.dump()}}}}}};
        SendLargeFromChrome(response.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  // Drive one complete analysis whose Runtime.evaluate returns `eval_payload`.
  void AnalyzeWithEval(PageAnalyzer& analyzer, const json& eval_payload) {
    auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
    analyzer.Analyze("<html><body><h1>Hi</h1></body></html>", resources, 1440,
                     900, [this](absl::StatusOr<PageAnalysisResult> result) {
                       analysis_result_ = std::move(result);
                       analysis_done_ = true;
                     });

    for (int i = 0; i < 20; ++i) {
      AutoRespondAllWithEval(eval_payload);
      RunLoop();
      if (analysis_done_) break;
    }

    SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
    for (int i = 0; i < 10; ++i) {
      AutoRespondAllWithEval(eval_payload);
      RunLoop();
      if (analysis_done_) break;
    }
  }

  // An eval payload with the perf fields present but nothing measured, so a
  // test can speak only about the fold fields it cares about.
  static json EmptyEvalResult() {
    json result;
    result["lcp"] = json::object();
    result["cls"] = {{"total", 0.0}, {"sources", json::array()}};
    result["fcp"] = 100.0;
    result["images"] = json::array();
    return result;
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
      if (method == "Page.setDocumentContent" &&
          (load_before_content_reply_ || hold_content_reply_)) {
        // On a page with nothing left to fetch, Chromium fires the page's own
        // load before it answers setDocumentContent.
        SendEvent("Page.lifecycleEvent",
                  {{"name", "load"}, {"loaderId", "blank-loader"}}, "sess-1");
        if (hold_content_reply_) {
          held_content_reply_id_ = id;
        } else {
          RespondToCommand(id, json::object());
        }
        continue;
      }
      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        json frame = {{"id", "frame-1"}};
        if (!blank_loader_id_.empty()) frame["loaderId"] = blank_loader_id_;
        RespondToCommand(id, {{"frameTree", {{"frame", frame}}}});
      } else if (method == "Page.navigate" && !blank_loader_id_.empty()) {
        RespondToCommand(
            id, {{"frameId", "frame-1"}, {"loaderId", blank_loader_id_}});
      } else if (method == "Runtime.evaluate") {
        json eval_result = MakeEvalResult();
        RespondToCommand(
            id,
            {{"result", {{"type", "string"}, {"value", eval_result.dump()}}}});
      } else if (method == "Fetch.failRequest" ||
                 method ==
                     "Fetch.fulfillRequest") {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      } else {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      }
    }
  }

  // Fire the page's load ahead of the setDocumentContent reply, as Chromium
  // does on a page with nothing left to fetch; with hold_content_reply_ the
  // reply is held (held_content_reply_id_) until the test sends it.
  bool load_before_content_reply_ = false;
  bool hold_content_reply_ = false;
  int held_content_reply_id_ = -1;
  // Index of the first received command with `method`, or -1.
  int IndexOf(const std::string& method) const {
    for (size_t i = 0; i < received_commands_.size(); ++i) {
      if (received_commands_[i].value("method", "") == method) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  // Answer and pump until `condition` holds (test::PumpUntil).
  void PumpUntil(const std::function<bool()>& condition) {
    test::PumpUntil(
        [this] {
          AutoRespondAll();
          RunLoop();
        },
        condition);
  }
  // Let the settle timer, armed by an event already sent, run out
  // (test::RunPastTimer).
  void RunOutTheSettleBound() {
    test::RunPastTimer([this] { RunLoop(); },
                       std::chrono::milliseconds(kTestSettleMs));
  }
  // Replay the blank tab's lifecycle on Page.setLifecycleEventsEnabled, as
  // Chromium does.
  bool replay_blank_lifecycle_ = false;
  // When set, Page.navigate answers with this loader and Page.getFrameTree
  // reports it, as Chromium does once the blank document has committed.
  std::string blank_loader_id_;
  uv_loop_t* loop_ = nullptr;
  test::PipePair chrome_to_client_;
  test::PipePair client_to_chrome_;
  uv_pipe_t read_pipe_{};
  uv_pipe_t client_write_{};
  int chrome_write_fd_ = -1;
  int test_read_fd_ = -1;
  std::unique_ptr<CdpClient> client_;
  bool analysis_done_ = false;
  absl::StatusOr<PageAnalysisResult> analysis_result_;
  std::vector<json> received_commands_;
  std::string received_data_;
  std::set<int> responded_ids_;
};

TEST_F(PageAnalysisTest, SuccessfulAnalysis) {
  PageAnalyzer analyzer(client_.get());

  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html><body><img id='hero' src='hero.jpg'></body></html>",
                   resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  // Process setup commands.
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }

  // Send networkIdle event.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }

  ASSERT_TRUE(analysis_done_);
  ASSERT_TRUE(analysis_result_.ok()) << analysis_result_.status().message();

  // LCP.
  EXPECT_EQ(analysis_result_->lcp.selector, "img#hero");
  EXPECT_EQ(analysis_result_->lcp.url, "https://cdn.example.com/hero.jpg");
  EXPECT_EQ(analysis_result_->lcp.element_tag, "IMG");
  EXPECT_EQ(analysis_result_->lcp.size, 50000u);
  EXPECT_FLOAT_EQ(analysis_result_->lcp_ms, 250.5f);

  // CLS.
  EXPECT_FLOAT_EQ(analysis_result_->cls_total, 0.05f);
  ASSERT_EQ(analysis_result_->cls_sources.size(), 1u);
  EXPECT_EQ(analysis_result_->cls_sources[0].selector, "img#ad");

  // FCP.
  EXPECT_FLOAT_EQ(analysis_result_->fcp_ms, 180.0f);

  // Images.
  ASSERT_EQ(analysis_result_->images.size(), 2u);
  EXPECT_EQ(analysis_result_->images[0].selector, "img#hero");
  EXPECT_TRUE(analysis_result_->images[0].above_fold);
  EXPECT_EQ(analysis_result_->images[0].rendered_width, 800u);
  EXPECT_EQ(analysis_result_->images[0].natural_width, 1600u);
  EXPECT_FALSE(analysis_result_->images[1].above_fold);

  // A render that reports no element descriptors reads back as "nothing was
  // measured" — never as "a fold of zero elements", which would be an
  // instruction to shrink the extractor's budget.
  EXPECT_TRUE(analysis_result_->elements.empty());
  EXPECT_FALSE(analysis_result_->elements_truncated);
}

// D1: the fold must be described by the elements that are actually visible, not
// by the subset of them that happen to be images.  The live repro was a page
// whose <head> exhausted the extractor's element estimate (then 25, counted
// from <html>) long before the first visible body element, so the utility
// classes that lay out the header were never seen.
TEST_F(PageAnalysisTest, CollectsAboveFoldElementDescriptorsNotJustImages) {
  json eval = EmptyEvalResult();
  eval["elements"] = json::array({
      {{"tag", "header"},
       {"id", "top"},
       {"classes", json::array({"flex", "h-16"})},
       {"index", 44},
       {"above_fold", true}},
      {{"tag", "div"},
       {"id", ""},
       {"classes", json::array({"items-center"})},
       {"index", 45},
       {"above_fold", true}},
      {{"tag", "section"},
       {"id", "comments"},
       {"classes", json::array({"mt-96"})},
       {"index", 300},
       {"above_fold", false}},
  });

  PageAnalyzer analyzer(client_.get());
  AnalyzeWithEval(analyzer, eval);

  ASSERT_TRUE(analysis_done_);
  ASSERT_TRUE(analysis_result_.ok()) << analysis_result_.status().message();

  ASSERT_EQ(analysis_result_->elements.size(), 3u)
      << "every measured element must survive, image or not";
  EXPECT_EQ(analysis_result_->elements[0].tag, "header");
  EXPECT_EQ(analysis_result_->elements[0].id, "top");
  EXPECT_EQ(analysis_result_->elements[0].classes,
            (std::vector<std::string>{"flex", "h-16"}));
  EXPECT_EQ(analysis_result_->elements[0].index, 44u);
  EXPECT_TRUE(analysis_result_->elements[0].above_fold);

  EXPECT_EQ(analysis_result_->elements[1].tag, "div");
  EXPECT_TRUE(analysis_result_->elements[1].id.empty());
  EXPECT_EQ(analysis_result_->elements[1].classes,
            (std::vector<std::string>{"items-center"}));
  EXPECT_TRUE(analysis_result_->elements[1].above_fold);

  // The below-fold element is reported too, flagged — dropping it here would
  // make "measured nothing" and "measured, not visible" indistinguishable.
  EXPECT_EQ(analysis_result_->elements[2].id, "comments");
  EXPECT_FALSE(analysis_result_->elements[2].above_fold);

  // None of these carry an image, so the pre-existing image list stays empty:
  // the descriptors are a new channel, not a re-labelling of the old one.
  EXPECT_TRUE(analysis_result_->images.empty());
}

// A short list can still be a clipped one: only the collector knows how many
// elements the document actually had, so a truncation it reports must survive
// even when the list that arrives is nowhere near the cap.
TEST_F(PageAnalysisTest, PageReportedTruncationIsHonouredOnAShortList) {
  json eval = EmptyEvalResult();
  eval["elements"] = json::array({
      {{"tag", "nav"},
       {"id", ""},
       {"classes", json::array({"flex"})},
       {"index", 44},
       {"above_fold", true}},
  });
  eval["elements_truncated"] = true;

  PageAnalyzer analyzer(client_.get());
  AnalyzeWithEval(analyzer, eval);

  ASSERT_TRUE(analysis_done_);
  ASSERT_TRUE(analysis_result_.ok()) << analysis_result_.status().message();
  EXPECT_EQ(analysis_result_->elements.size(), 1u);
  EXPECT_TRUE(analysis_result_->elements_truncated)
      << "the flag is the collector's, not an inference from the list length";
}

// The page controls this payload end to end — it can replace __PS_stringify and
// return anything that parses. A well-formed array of the wrong shape must not
// reach nlohmann's throwing accessors: the try/catch upstream only wraps the
// parse, so a type_error here would escape as an unhandled exception.
TEST_F(PageAnalysisTest, MalformedElementDescriptorsAreSkippedNotFatal) {
  json eval = EmptyEvalResult();
  eval["elements"] = json::array({
      "not-an-object",
      42,
      json::array({1, 2, 3}),
      nullptr,
      {{"tag", "nav"},
       {"id", "top"},
       {"classes", json::array({"flex", 7, nullptr})},
       {"index", 44},
       {"above_fold", true}},
      {{"tag", 99}, {"index", "nope"}, {"above_fold", "yes"}},
  });

  PageAnalyzer analyzer(client_.get());
  AnalyzeWithEval(analyzer, eval);

  ASSERT_TRUE(analysis_done_);
  ASSERT_TRUE(analysis_result_.ok()) << analysis_result_.status().message();

  // The two objects survive; the four non-objects are skipped.
  ASSERT_EQ(analysis_result_->elements.size(), 2u);
  EXPECT_EQ(analysis_result_->elements[0].tag, "nav");
  EXPECT_EQ(analysis_result_->elements[0].id, "top");
  // Non-string class entries are dropped, the string one is kept.
  EXPECT_EQ(analysis_result_->elements[0].classes,
            (std::vector<std::string>{"flex"}));
  // Wrong-typed fields fall back to their defaults rather than throwing.
  EXPECT_TRUE(analysis_result_->elements[1].tag.empty());
  EXPECT_EQ(analysis_result_->elements[1].index, 0u);
  EXPECT_FALSE(analysis_result_->elements[1].above_fold);
}

// The guard around the WHOLE result population, not just the descriptor loop.
// The older LCP/CLS/image reads still use nlohmann's throwing value(), and in
// production this callback is reached from a libuv C frame — an exception
// unwinding through C aborts the process. It must surface as a failed analysis.
TEST_F(PageAnalysisTest, WrongTypedResultFieldFailsTheAnalysisCleanly) {
  json eval = EmptyEvalResult();
  eval["lcp"] = 42;  // `data["lcp"].value("selector", "")` throws on a number

  PageAnalyzer analyzer(client_.get());
  AnalyzeWithEval(analyzer, eval);

  ASSERT_TRUE(analysis_done_)
      << "the callback must run — a swallowed throw would hang the render";
  EXPECT_FALSE(analysis_result_.ok())
      << "a malformed result is an error, not a silently empty success";
}

TEST_F(PageAnalysisTest, ElementDescriptorsAreCappedAndTruncationIsSignalled) {
  constexpr size_t kCap = PageAnalysisResult::kMaxElementDescriptors;
  const size_t over_cap = kCap + 37;

  json eval = EmptyEvalResult();
  json elements = json::array();
  for (size_t i = 0; i < over_cap; ++i) {
    elements.push_back({{"index", i}});
  }
  eval["elements"] = std::move(elements);
  // A page that lies about not being truncated must not be believed: the cap
  // is enforced here, so the flag is set from what was actually kept.
  eval["elements_truncated"] = false;

  PageAnalyzer analyzer(client_.get());
  AnalyzeWithEval(analyzer, eval);

  ASSERT_TRUE(analysis_done_);
  ASSERT_TRUE(analysis_result_.ok()) << analysis_result_.status().message();

  EXPECT_EQ(analysis_result_->elements.size(), kCap);
  EXPECT_TRUE(analysis_result_->elements_truncated)
      << "a truncated descriptor list must never read as a short page";
  // The kept slice is the document prefix, not an arbitrary sample.
  ASSERT_FALSE(analysis_result_->elements.empty());
  EXPECT_EQ(analysis_result_->elements.front().index, 0u);
  EXPECT_EQ(analysis_result_->elements.back().index,
            static_cast<uint32_t>(kCap - 1));
}

TEST_F(PageAnalysisTest, CreateTargetFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  RunLoop();
  // Wait for the createTarget command.
  RunLoopUntil([this] { return !received_commands_.empty(); });
  int id = received_commands_[0].value("id", 0);
  json error = {{"id", id},
                {"error", {{"code", -32000}, {"message", "failed"}}}};
  SendFromChrome(error.dump());

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, FetchInterceptionBlocksUnknownUrls) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html><body><img src='unknown.jpg'></body></html>",
                   resources, 375, 667,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Simulate a Fetch.requestPaused event for an unknown URL.
  SendEvent(
      "Fetch.requestPaused",
      {{"requestId", "req-1"}, {"request", {{"url", "http://evil.com/steal"}}}},
      "sess-1");

  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Look for Fetch.failRequest in commands.
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
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
}

TEST_F(PageAnalysisTest, FetchBlocksRedirectResponses) {
  // A Fetch.requestPaused event carrying a 3xx responseStatusCode (response-
  // stage interception) must be failed, not fulfilled from cache.  Following
  // the redirect would generate new requestPaused events for URLs not in our
  // cache and can leave Chrome's network stack in an inconsistent state.
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<PageAnalyzer::ResourceMap>();
  // Cache the URL so it would normally be fulfilled.
  (*resources)["https://example.com/old.css"] = "body{color:red}";
  auto const_resources =
      std::static_pointer_cast<const PageAnalyzer::ResourceMap>(resources);

  analyzer.Analyze(
      "<html><head><link rel='stylesheet' "
      "href='https://example.com/old.css'></head><body></body></html>",
      const_resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        analysis_result_ = std::move(result);
        analysis_done_ = true;
      });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Simulate a response-stage Fetch.requestPaused with a 301 redirect.
  // Even though the URL is in our cache, the redirect must be blocked.
  SendEvent(
      "Fetch.requestPaused",
      {{"requestId", "req-redirect"},
       {"request", {{"url", "https://example.com/old.css"}}},
       {"responseStatusCode", 301},
       {"responseHeaders",
        {{{"name", "Location"}, {"value", "https://example.com/new.css"}}}}},
      "sess-1");

  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Must see failRequest, not fulfillRequest.
  bool found_fail = false;
  bool found_fulfill = false;
  for (const auto& cmd : received_commands_) {
    auto params = cmd.value("params", json::object());
    if (params.value("requestId", "") != "req-redirect") continue;
    std::string method = cmd.value("method", "");
    if (method == "Fetch.failRequest") {
      EXPECT_EQ(params.value("errorReason", ""), "BlockedByClient");
      EXPECT_FALSE(params.contains("reason")) << "Chromium rejects it";
      found_fail = true;
    } else if (method == "Fetch.fulfillRequest") {
      found_fulfill = true;
    }
  }
  EXPECT_TRUE(found_fail);
  EXPECT_FALSE(found_fulfill);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
}

TEST_F(PageAnalysisTest, SessionIdPropagated) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Verify session commands use the session ID.
  bool found_session = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.contains("sessionId") && cmd["sessionId"] == "sess-1") {
      found_session = true;
      break;
    }
  }
  EXPECT_TRUE(found_session);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
}

TEST_F(PageAnalysisTest, FetchServesFromCacheWithCorrectContentType) {
  // Verify that cached resources are served via Fetch.fulfillRequest
  // with correct Content-Type (not application/octet-stream).
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<PageAnalyzer::ResourceMap>();
  (*resources)["https://example.com/style.css"] = "body{color:red}";
  auto const_resources =
      std::static_pointer_cast<const PageAnalyzer::ResourceMap>(resources);

  analyzer.Analyze(
      "<html><head><link rel='stylesheet' "
      "href='https://example.com/style.css'>"
      "</head><body></body></html>",
      const_resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        analysis_result_ = std::move(result);
        analysis_done_ = true;
      });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Simulate a Fetch.requestPaused for the cached CSS URL.
  SendEvent("Fetch.requestPaused",
            {{"requestId", "req-css"},
             {"request", {{"url", "https://example.com/style.css"}}}},
            "sess-1");

  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Find the fulfillRequest command and check Content-Type.
  bool found_fulfill = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "Fetch.fulfillRequest") {
      auto params = cmd.value("params", json::object());
      auto headers = params.value("responseHeaders", json::array());
      for (const auto& h : headers) {
        if (h.value("name", "") == "Content-Type") {
          EXPECT_EQ(h.value("value", ""), "text/css");
          found_fulfill = true;
        }
      }
      // Body should be base64-encoded "body{color:red}"
      EXPECT_FALSE(params.value("body", "").empty());
      break;
    }
  }
  EXPECT_TRUE(found_fulfill);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
}

TEST_F(PageAnalysisTest, NetworkOfflineFailureAbortsAnalysis) {
  // Test that if the network offline setup fails, analysis aborts.
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Network.emulateNetworkConditions") {
        // Fail the SSRF-critical command.
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "not supported"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, FetchEnableFailureAbortsAnalysis) {
  // Test that if Fetch.enable fails, analysis aborts.
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Fetch.enable") {
        // Fail the Fetch.enable command.
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "not supported"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, EvaluateReturnsEmptyAbortsAnalysis) {
  // Test that if Runtime.evaluate returns empty, analysis fails.
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  // Override evaluate to return empty.
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Runtime.evaluate") {
        // Return empty value.
        RespondToCommand(id, {{"result", {{"type", "string"}, {"value", ""}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
    if (analysis_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    custom_respond();
    RunLoop();
    if (analysis_done_) break;
  }

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, EvaluateReturnsMalformedJsonAbortsAnalysis) {
  // Test that if evaluate returns invalid JSON, analysis fails.
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Runtime.evaluate") {
        // Return malformed JSON string.
        RespondToCommand(
            id,
            {{"result", {{"type", "string"}, {"value", "{invalid json!!!"}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
    if (analysis_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    custom_respond();
    RunLoop();
    if (analysis_done_) break;
  }

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, ViewportSetupFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Emulation.setDeviceMetricsOverride") {
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "viewport failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, AddScriptSetupFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.addScriptToEvaluateOnNewDocument") {
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "addScript failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, PageEnableFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.enable") {
        json err = {
            {"id", id},
            {"error", {{"code", -32000}, {"message", "Page.enable failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, LifecycleEnableFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.setLifecycleEventsEnabled") {
        json err = {{"id", id},
                    {"error", {{"code", -32000}, {"message", "failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, GetFrameTreeFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  for (int i = 0; i < 40 && !analysis_done_; ++i) {
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
        json err = {{"id", id},
                    {"error", {{"code", -32000}, {"message", "failed"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
    RunLoop();
  }

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, SetDocumentContentFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  for (int i = 0; i < 40 && !analysis_done_; ++i) {
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

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, MobileViewport) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 375, 667,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Verify mobile flag in viewport setup, and touch.
  bool found_viewport = false;
  bool found_touch = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "Emulation.setDeviceMetricsOverride") {
      auto params = cmd.value("params", json::object());
      EXPECT_EQ(params.value("width", 0), 375);
      EXPECT_EQ(params.value("height", 0), 667);
      EXPECT_TRUE(params.value("mobile", false));
      found_viewport = true;
    }
    if (cmd.value("method", "") == "Emulation.setTouchEmulationEnabled") {
      EXPECT_TRUE(found_viewport) << "touch follows the device metrics";
      auto params = cmd.value("params", json::object());
      EXPECT_TRUE(params.value("enabled", false));
      EXPECT_EQ(params.value("maxTouchPoints", 0), 5);
      found_touch = true;
      break;
    }
  }
  EXPECT_TRUE(found_viewport);
  EXPECT_TRUE(found_touch);
  // Then the phone's user agent; the observers are registered
  // before the fresh window, so they run in the window the document is
  // written into.
  test::ExpectFreshWindowRenderSetup(received_commands_, "sess-1", 375, 667);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
}

// Enabling lifecycle events makes Chromium replay the blank
// tab's networkIdle before the document is written. The page analysis must not
// collect on it (it would read about:blank): it collects only on a
// networkIdle that arrives after Page.setDocumentContent answered.
TEST_F(PageAnalysisTest, IgnoresTheBlankTabsReplayedNetworkIdle) {
  replay_blank_lifecycle_ = true;
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html><body><h1>Hi</h1></body></html>", resources, 375, 667,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  int content = -1;
  int evaluate = -1;
  for (size_t i = 0; i < received_commands_.size(); ++i) {
    const std::string method = received_commands_[i].value("method", "");
    if (method == "Page.setDocumentContent" && content < 0) {
      content = static_cast<int>(i);
    }
    if (method == "Runtime.evaluate" && evaluate < 0) {
      evaluate = static_cast<int>(i);
    }
  }
  ASSERT_GE(content, 0) << "the document was never written";
  EXPECT_EQ(evaluate, -1)
      << "collected on the blank tab's replayed networkIdle";
  EXPECT_FALSE(analysis_done_);

  // The page's own networkIdle, after the write: now it collects.
  SendEvent("Page.lifecycleEvent",
            {{"name", "networkIdle"}, {"loaderId", "blank-loader"}}, "sess-1");
  for (int i = 0; i < 20 && !analysis_done_; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  ASSERT_TRUE(analysis_done_);
  EXPECT_TRUE(analysis_result_.ok()) << analysis_result_.status();
  evaluate = -1;
  for (size_t i = 0; i < received_commands_.size(); ++i) {
    if (received_commands_[i].value("method", "") == "Runtime.evaluate") {
      evaluate = static_cast<int>(i);
      break;
    }
  }
  EXPECT_GT(evaluate, content) << "the page is read after it was written";
}

// A page that keeps a request in flight at least every 500 ms
// never reaches networkIdle. The page analysis then collects settle_after_load_ms
// after the page's own load event, instead of running into its session
// timeout; the replayed blank-tab load does not start that wait. (The bound
// is shortened for the test.)
TEST_F(PageAnalysisTest, CollectsAfterLoadWhenTheNetworkNeverGoesIdle) {
  replay_blank_lifecycle_ = true;
  PageAnalyzer analyzer(client_.get());
  analyzer.set_settle_after_load_ms_for_testing(kTestSettleMs);
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html><body><h1>Hi</h1></body></html>", resources, 375, 667,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  const int content = IndexOf("Page.setDocumentContent");
  ASSERT_GE(content, 0) << "the document was never written";
  EXPECT_FALSE(analysis_done_) << "the replayed load started the settle wait";

  // The page's load, after the reply; no networkIdle follows.
  SendEvent("Page.lifecycleEvent", {{"name", "load"}}, "sess-1");
  PumpUntil([this] { return analysis_done_; });
  ASSERT_TRUE(analysis_done_) << "no collection after the settle bound";
  EXPECT_TRUE(analysis_result_.ok()) << analysis_result_.status();
  EXPECT_GT(IndexOf("Runtime.evaluate"), content);
}

// On a page with nothing left to fetch, the page's load arrives
// BEFORE the reply to setDocumentContent. It still starts the settle wait.
TEST_F(PageAnalysisTest, LoadBeforeTheContentReplyStartsTheSettle) {
  replay_blank_lifecycle_ = true;
  load_before_content_reply_ = true;
  PageAnalyzer analyzer(client_.get());
  analyzer.set_settle_after_load_ms_for_testing(kTestSettleMs);
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html><body><h1>Hi</h1></body></html>", resources, 375, 667,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });
  PumpUntil([this] { return analysis_done_; });
  ASSERT_TRUE(analysis_done_) << "the load before the reply was dropped";
  EXPECT_TRUE(analysis_result_.ok()) << analysis_result_.status();
  EXPECT_GT(IndexOf("Runtime.evaluate"), IndexOf("Page.setDocumentContent"));
}

// The settle bound can run out before setDocumentContent is answered; the
// page analysis then collects at the reply, never before the document is written.
TEST_F(PageAnalysisTest, SettleBeforeTheContentReplyCollectsAtTheReply) {
  replay_blank_lifecycle_ = true;
  hold_content_reply_ = true;
  PageAnalyzer analyzer(client_.get());
  analyzer.set_settle_after_load_ms_for_testing(kTestSettleMs);
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html><body><h1>Hi</h1></body></html>", resources, 375, 667,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });
  // Wait for the command itself, not a fixed time: the setup round trips
  // before it can take longer than the settle bound on a busy host.
  PumpUntil([this] { return held_content_reply_id_ >= 0; });
  ASSERT_GE(held_content_reply_id_, 0) << "setDocumentContent was not sent";
  // The page's load went out with the command; the bound runs out before
  // the reply.
  RunOutTheSettleBound();
  EXPECT_EQ(IndexOf("Runtime.evaluate"), -1)
      << "collected before the document was written";
  EXPECT_FALSE(analysis_done_);
  RespondToCommand(held_content_reply_id_, json::object());
  PumpUntil([this] { return analysis_done_; });
  ASSERT_TRUE(analysis_done_);
  EXPECT_TRUE(analysis_result_.ok()) << analysis_result_.status();
}

// The desktop render sends no user agent override, still gets a
// fresh window, and waits for that window's document to commit (the frame's
// loader is the navigation's) before it enables lifecycle events.
TEST_F(PageAnalysisTest, DesktopSetupWaitsForTheBlankDocument) {
  blank_loader_id_ = "loader-blank";
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();
  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  test::ExpectFreshWindowRenderSetup(received_commands_, "sess-1", 1440, 900);
  const int navigate =
      test::IndexOfCommand(received_commands_, "Page.navigate");
  const int poll = test::IndexOfCommand(received_commands_, "Page.getFrameTree",
                                        static_cast<size_t>(navigate) + 1);
  const int lifecycle = test::IndexOfCommand(received_commands_,
                                             "Page.setLifecycleEventsEnabled");
  ASSERT_GE(navigate, 0);
  ASSERT_GE(poll, 0) << "no wait for the blank document's commit";
  EXPECT_LT(poll, lifecycle);

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
  EXPECT_TRUE(analysis_done_);
}

TEST_F(PageAnalysisTest, AttachToTargetFailure) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze("<html></html>", resources, 1440, 900,
                   [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
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

  RunLoopUntil([this] { return analysis_done_; }, 50);

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

TEST_F(PageAnalysisTest, ResultsWithNoCls) {
  // Test that missing CLS data is handled gracefully.
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  // Override eval to return result without CLS.
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
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Runtime.evaluate") {
        json result;
        result["lcp"] = {{"selector", "h1"},
                         {"url", ""},
                         {"tag", "H1"},
                         {"size", 500},
                         {"time", 100.0}};
        result["fcp"] = 80.0;
        result["images"] = json::array();
        // No CLS field.
        RespondToCommand(
            id, {{"result", {{"type", "string"}, {"value", result.dump()}}}});
      } else if (method == "Fetch.failRequest" ||
                 method ==
                     "Fetch.fulfillRequest") {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      } else {  // NOLINT(bugprone-branch-clone)
        RespondToCommand(id, json::object());
      }
    }
  };

  analyzer.Analyze("<html><body><h1>Hello</h1></body></html>", resources, 1440,
                   900, [this](absl::StatusOr<PageAnalysisResult> result) {
                     analysis_result_ = std::move(result);
                     analysis_done_ = true;
                   });

  for (int i = 0; i < 20; ++i) {
    custom_respond();
    RunLoop();
    if (analysis_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    custom_respond();
    RunLoop();
    if (analysis_done_) break;
  }

  ASSERT_TRUE(analysis_done_);
  ASSERT_TRUE(analysis_result_.ok()) << analysis_result_.status().message();
  EXPECT_EQ(analysis_result_->lcp.selector, "h1");
  EXPECT_EQ(analysis_result_->lcp.element_tag, "H1");
  EXPECT_FLOAT_EQ(analysis_result_->cls_total, 0.0f);
  EXPECT_TRUE(analysis_result_->cls_sources.empty());
  EXPECT_TRUE(analysis_result_->images.empty());
}

TEST_F(PageAnalysisTest, SessionTimeoutFiresOnStall) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  analyzer.Analyze(
      "<html><body>Test</body></html>", resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        analysis_result_ = std::move(result);
        analysis_done_ = true;
      },
      200);  // 200ms timeout

  bool guard_fired = false;
  uv_timer_t guard;
  uv_timer_init(loop_, &guard);
  guard.data = &guard_fired;
  uv_timer_start(
      &guard, [](uv_timer_t* t) { *static_cast<bool*>(t->data) = true; }, 2000,
      0);

  while (!analysis_done_ && !guard_fired) {
    AutoRespondAll();
    uv_run(loop_, UV_RUN_NOWAIT);
    test::SleepUs(1000);  // 1ms
  }

  uv_timer_stop(&guard);
  uv_close(reinterpret_cast<uv_handle_t*>(&guard), nullptr);
  RunLoop();

  ASSERT_TRUE(analysis_done_) << "Session timeout did not fire within 2s";
  EXPECT_FALSE(analysis_result_.ok());
  EXPECT_TRUE(analysis_result_.status().message().find("timeout") !=
              std::string::npos)
      << "Error: " << analysis_result_.status().message();
}

// ---- agent_optimize render bridge -----------------------------------------

// An allowlisted subresource is fetched out-of-Chrome (mock spawn) and its bytes
// are served back to the page via Fetch.fulfillRequest — exercising the full async
// path (uv_queue_work -> threadpool fetch -> after-callback -> fulfill) through the
// real Session.
TEST_F(PageAnalysisTest, AgentOptimizeFetchesAndFulfills) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  auto policy = std::make_shared<FetchPolicy>();
  policy->allow_hosts = {"cdn.example.com"};
  auto opts = std::make_shared<AgentRenderOptions>();
  opts->policy = policy;
  opts->settle_ms = 0;  // read the rendered DOM immediately on networkIdle
  opts->deps.spawn = [](const std::vector<std::string>&, std::size_t) {
    CurlSpawnOutcome o;
    o.spawned = true;
    o.exit_code = 0;
    o.output =
        "HTTP/1.1 200 OK\r\nContent-Type: text/javascript\r\n\r\nAGENT_BODY";
    return o;
  };
  opts->deps.resolve = [](std::string_view) {
    return std::vector<std::string>{"93.184.216.34"};
  };

  analyzer.Analyze(
      "<html><body><script src='https://cdn.example.com/lib.js'></script>"
      "</body></html>",
      resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        analysis_result_ = std::move(result);
        analysis_done_ = true;
      },
      PageAnalyzer::kDefaultTimeoutMs, opts);

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  SendEvent("Fetch.requestPaused",
            {{"requestId", "req-js"},
             {"resourceType", "Script"},
             {"request", {{"url", "https://cdn.example.com/lib.js"}}}},
            "sess-1");

  // The fetch runs on the libuv threadpool; wait for the fulfill to come back.
  RunLoopUntil(
      [this] {
        for (const auto& cmd : received_commands_) {
          if (cmd.value("method", "") == "Fetch.fulfillRequest" &&
              cmd.value("params", json::object()).value("requestId", "") ==
                  "req-js") {
            return true;
          }
        }
        return false;
      },
      2000);

  bool found = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") != "Fetch.fulfillRequest") continue;
    auto params = cmd.value("params", json::object());
    if (params.value("requestId", "") != "req-js") continue;
    found = true;
    EXPECT_EQ(params.value("responseCode", 0), 200);
    EXPECT_FALSE(params.value("body", "").empty());  // base64 of AGENT_BODY
    bool ct = false;
    for (const auto& h : params.value("responseHeaders", json::array())) {
      if (h.value("name", "") == "Content-Type" &&
          h.value("value", "") == "text/javascript") {
        ct = true;
      }
    }
    EXPECT_TRUE(ct);
  }
  EXPECT_TRUE(found);

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
}

// An allowlisted host that DNS-rebinds to loopback must be blocked by the SSRF guard
// BEFORE any spawn: the bridge fails the request and curl is never invoked.
TEST_F(PageAnalysisTest, AgentOptimizeBlocksRebindToPrivate) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  auto spawn_count = std::make_shared<std::atomic<int>>(0);
  auto policy = std::make_shared<FetchPolicy>();
  policy->allow_hosts = {"cdn.example.com"};
  auto opts = std::make_shared<AgentRenderOptions>();
  opts->policy = policy;
  opts->settle_ms =
      0;  // adjudicate the fetch then read immediately on networkIdle
  opts->deps.spawn = [spawn_count](const std::vector<std::string>&,
                                   std::size_t) {
    spawn_count->fetch_add(1);
    CurlSpawnOutcome o;
    o.spawned = true;
    o.exit_code = 0;
    o.output = "HTTP/1.1 200 OK\r\n\r\nSHOULD_NOT_HAPPEN";
    return o;
  };
  opts->deps.resolve = [](std::string_view) {
    return std::vector<std::string>{"127.0.0.1"};  // rebind to loopback
  };

  analyzer.Analyze(
      "<html><body><script src='https://cdn.example.com/lib.js'></script>"
      "</body></html>",
      resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        analysis_result_ = std::move(result);
        analysis_done_ = true;
      },
      PageAnalyzer::kDefaultTimeoutMs, opts);

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  SendEvent("Fetch.requestPaused",
            {{"requestId", "req-js"},
             {"resourceType", "Script"},
             {"request", {{"url", "https://cdn.example.com/lib.js"}}}},
            "sess-1");

  RunLoopUntil(
      [this] {
        for (const auto& cmd : received_commands_) {
          if (cmd.value("method", "") == "Fetch.failRequest" &&
              cmd.value("params", json::object()).value("requestId", "") ==
                  "req-js") {
            return true;
          }
        }
        return false;
      },
      2000);

  bool found_fail = false;
  bool found_fulfill = false;
  for (const auto& cmd : received_commands_) {
    auto params = cmd.value("params", json::object());
    if (params.value("requestId", "") != "req-js") continue;
    std::string method = cmd.value("method", "");
    if (method == "Fetch.failRequest") {
      EXPECT_EQ(params.value("errorReason", ""), "BlockedByClient");
      EXPECT_FALSE(params.contains("reason")) << "Chromium rejects it";
      found_fail = true;
    } else if (method == "Fetch.fulfillRequest") {
      found_fulfill = true;
    }
  }
  EXPECT_TRUE(found_fail);
  EXPECT_FALSE(found_fulfill);
  EXPECT_EQ(spawn_count->load(), 0);  // SSRF block happened before any spawn

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (analysis_done_) break;
  }
}

// ---- agent_optimize rendered-DOM read -> markdown --------------------------

// The agent-content path: after networkIdle (settle 0), the Session reads the
// hydrated outerHTML via Runtime.evaluate and SAX-extracts markdown. Proves the
// end-to-end DOM-read -> ExtractAgentMarkdown wiring through the real Session.
TEST_F(PageAnalysisTest, AgentOptimizeRendersDomToMarkdown) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  auto opts = std::make_shared<AgentRenderOptions>();
  opts->policy = std::make_shared<FetchPolicy>();  // active => agent path
  opts->settle_ms = 0;                             // read immediately
  opts->deps.spawn = [](const std::vector<std::string>&, std::size_t) {
    return CurlSpawnOutcome{};  // unused: the fixture has no subresources
  };
  opts->deps.resolve = [](std::string_view) {
    return std::vector<std::string>{};
  };

  const std::string kRendered =
      "<html><body><main><h1>Hydrated</h1><p>From JS.</p></main>"
      "</body></html>";

  auto respond = [this, &kRendered]() {
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
      } else if (method == "Runtime.evaluate") {
        RespondToCommand(
            id, {{"result", {{"type", "string"}, {"value", kRendered}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  analyzer.Analyze(
      "<html><body><div id=\"root\"></div></body></html>", resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        analysis_result_ = std::move(result);
        analysis_done_ = true;
      },
      PageAnalyzer::kDefaultTimeoutMs, opts);

  for (int i = 0; i < 20; ++i) {
    respond();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    respond();
    RunLoop();
    if (analysis_done_) break;
  }

  ASSERT_TRUE(analysis_done_);
  ASSERT_TRUE(analysis_result_.ok()) << analysis_result_.status().message();
  EXPECT_EQ(analysis_result_->rendered_html, kRendered);
  EXPECT_EQ(analysis_result_->agent_markdown, "# Hydrated\n\nFrom JS.");
}

// A failed rendered-DOM evaluate degrades gracefully (FinishError, no crash).
TEST_F(PageAnalysisTest, AgentOptimizeEvaluateFailureAborts) {
  PageAnalyzer analyzer(client_.get());
  auto resources = std::make_shared<const PageAnalyzer::ResourceMap>();

  auto opts = std::make_shared<AgentRenderOptions>();
  opts->policy = std::make_shared<FetchPolicy>();
  opts->settle_ms = 0;
  opts->deps.spawn = [](const std::vector<std::string>&, std::size_t) {
    return CurlSpawnOutcome{};
  };
  opts->deps.resolve = [](std::string_view) {
    return std::vector<std::string>{};
  };

  auto respond = [this]() {
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
      } else if (method == "Runtime.evaluate") {
        json err = {{"id", id},
                    {"error", {{"code", -32000}, {"message", "boom"}}}};
        SendFromChrome(err.dump());
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  analyzer.Analyze(
      "<html><body></body></html>", resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        analysis_result_ = std::move(result);
        analysis_done_ = true;
      },
      PageAnalyzer::kDefaultTimeoutMs, opts);

  for (int i = 0; i < 20; ++i) {
    respond();
    RunLoop();
  }
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    respond();
    RunLoop();
    if (analysis_done_) break;
  }

  ASSERT_TRUE(analysis_done_);
  EXPECT_FALSE(analysis_result_.ok());
}

}  // namespace
}  // namespace pagespeed
