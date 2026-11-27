// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the pieces the browser-analysis renders share.

#include "src/browser/render_support.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "uv.h"

namespace pagespeed {
namespace {

// Chromium's parameter is `errorReason`; `reason` is refused with "Invalid
// parameters" and leaves the request paused.
TEST(RenderSupportTest, FailRequestCommandUsesErrorReason) {
  const CdpCommand cmd = FailRequestCommand("req-9", "sess-3");
  EXPECT_EQ(cmd.method, "Fetch.failRequest");
  EXPECT_EQ(cmd.session_id, "sess-3");
  EXPECT_EQ(cmd.params, (nlohmann::json{{"requestId", "req-9"},
                                        {"errorReason", "BlockedByClient"}}));
}

// A render session with the FailPausedRequest contract. Its Send() stores
// the callback; Reply() moves it out before running it, as a CdpClient
// releases a callback it answered, and the fixture clears what a test left
// unanswered, so no callback keeps the session alive past its test.
struct FakeSession {
  explicit FakeSession(CdpClient* c) : client(c) {}
  CdpClient* client;
  std::string session_id = "sess-1";
  bool completed = false;
  bool fetch_error_reported = false;
  std::vector<CdpCommand> sent;
  std::vector<CdpResponseCallback> pending;
  bool Send(const CdpCommand& cmd, CdpResponseCallback cb) {
    sent.push_back(cmd);
    pending.push_back(std::move(cb));
    return true;
  }
  void Reply(size_t i, absl::StatusOr<CdpResponse> response) {
    ASSERT_LT(i, pending.size());
    CdpResponseCallback cb = std::move(pending[i]);
    pending[i] = nullptr;
    ASSERT_TRUE(cb);
    cb(std::move(response));
  }
};

CdpResponse Refused() {
  CdpResponse r;
  r.error_code = -32602;
  r.error_message = "Invalid parameters";
  return r;
}

class FailPausedRequestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    uv_loop_init(&loop_);
    client_ = std::make_unique<CdpClient>(&loop_);
    client_->set_diagnostic_handler(
        [this](std::string_view m) { reports_.emplace_back(m); });
    s_ = std::make_shared<FakeSession>(client_.get());
  }
  void TearDown() override {
    s_->pending.clear();
    EXPECT_EQ(s_.use_count(), 1) << "the session outlived its test";
    s_.reset();
    client_.reset();
    uv_run(&loop_, UV_RUN_DEFAULT);
    uv_loop_close(&loop_);
  }
  uv_loop_t loop_{};
  std::unique_ptr<CdpClient> client_;
  std::shared_ptr<FakeSession> s_;
  std::vector<std::string> reports_;
};

TEST_F(FailPausedRequestTest, SendsTheCommandOnTheSession) {
  FailPausedRequest(s_, "req-1", "page analysis");
  ASSERT_EQ(s_->sent.size(), 1u);
  EXPECT_EQ(s_->sent[0].params, FailRequestCommand("req-1", "sess-1").params);
  EXPECT_EQ(s_->sent[0].session_id, "sess-1");
  s_->Reply(0, CdpResponse{});
  EXPECT_TRUE(reports_.empty()) << "an accepted reply is not reported";
}

TEST_F(FailPausedRequestTest, ReportsTheFirstRefusalOfARender) {
  FailPausedRequest(s_, "req-1", "page analysis");
  FailPausedRequest(s_, "req-2", "page analysis");
  s_->Reply(0, Refused());
  s_->Reply(1, Refused());
  ASSERT_EQ(reports_.size(), 1u) << "once per render";
  EXPECT_EQ(reports_[0],
            "page analysis: Fetch.failRequest was refused (Invalid "
            "parameters); the blocked request stays paused");
  EXPECT_TRUE(s_->fetch_error_reported);
}

// A transport status is not a refusal: CdpClient::CancelAll answers every
// pending command with one when the pipe closes or Chrome exits, possibly
// before the render has finished.
TEST_F(FailPausedRequestTest, TransportFailureIsNotReported) {
  FailPausedRequest(s_, "req-1", "validation render");
  FailPausedRequest(s_, "req-2", "validation render");
  s_->Reply(0, absl::UnavailableError("Chrome pipe closed (EOF)"));
  s_->Reply(1, absl::DeadlineExceededError("command timeout"));
  EXPECT_TRUE(reports_.empty());
  EXPECT_FALSE(s_->fetch_error_reported);
}

// Chromium refuses commands for a target the render already closed; such a
// late reply is not a problem worth reporting.
TEST_F(FailPausedRequestTest, ReplyAfterTheRenderFinishedIsIgnored) {
  FailPausedRequest(s_, "req-1", "font scan");
  s_->completed = true;
  s_->Reply(0, Refused());
  EXPECT_TRUE(reports_.empty());
}

TEST(OneShotTimerTest, FiresOnceAfterTheDelay) {
  uv_loop_t loop;
  uv_loop_init(&loop);
  int fired = 0;
  {
    OneShotTimer timer;
    timer.Start(&loop, 5, [&fired]() { ++fired; });
    EXPECT_TRUE(timer.armed());
    timer.Start(&loop, 1, [&fired]() { fired += 100; });  // ignored: armed
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(timer.armed());
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  EXPECT_EQ(uv_loop_close(&loop), 0) << "the timer handle was closed";
}

TEST(OneShotTimerTest, CancelledTimerNeverFires) {
  uv_loop_t loop;
  uv_loop_init(&loop);
  int fired = 0;
  {
    OneShotTimer timer;
    timer.Start(&loop, 5, [&fired]() { ++fired; });
    timer.Cancel();
    EXPECT_FALSE(timer.armed());
    timer.Cancel();  // harmless when not armed
    uv_run(&loop, UV_RUN_DEFAULT);
  }
  EXPECT_EQ(fired, 0);
  EXPECT_EQ(uv_loop_close(&loop), 0);
}

// The callback may destroy the timer's owner (a render session finishing).
TEST(OneShotTimerTest, CallbackMayDestroyTheOwner) {
  uv_loop_t loop;
  uv_loop_init(&loop);
  auto owner = std::make_unique<OneShotTimer>();
  bool fired = false;
  owner->Start(&loop, 1, [&]() {
    fired = true;
    owner.reset();
  });
  uv_run(&loop, UV_RUN_DEFAULT);
  EXPECT_TRUE(fired);
  EXPECT_EQ(owner, nullptr);
  EXPECT_EQ(uv_loop_close(&loop), 0);
}

}  // namespace
}  // namespace pagespeed
